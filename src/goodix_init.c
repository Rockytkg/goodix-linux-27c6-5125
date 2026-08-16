/* goodix_init.c - device activation sequence
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 设备初始化序列。仅 OS 层 API（libusb, usleep）不同，其余逻辑与
 * 参考实现一致。
 *
 * 设备初始化：
 *   device_action(15,{0,0})   BESD 关闭
 *   初始化 MCU
 *   初始化传感器
 *   device_action(15,{1,0})   BESD 开启
 *
 * 初始化 MCU：
 *   版本握手重试        （NOP + 0xA8 x N, N=retry_count）
 *   project 8 (ST32)：固件更新（无条件执行）
 *     - 固件更新逻辑：同版本跳过 / 清应用区+复位 / IAP 下载
 *     - 返回固件软复位标记 -> 初始化停止（MCU 重启中）
 *   PSK 处理        （校验主机哈希与 MCU，不一致则写新 PSK）
 *
 * 初始化传感器：
 *   复位传感器（SetIdle）
 *   读 ChipID x N（0x82）-> chipid
 *   按 chipid 选择传感器参数
 *   读 OTP (0xA6) -> OTP 解析 -> FDT 初始化 -> 配置下发 (0x90)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "goodix.h"

#define RETRY_COUNT 5

/* 定义见文件末尾：设备重枚举后 close + 轮询 reopen（供 evk 唤醒 L3 用） */
static int gx_transport_reopen(struct goodix_dev *d, int max_ms);

/* ================= 配置下发数据表 =================
 * 每类传感器一份 224 字节配置，按类型偏移组织：base + 224*type，
 * 初始化时把对应 224 字节通过 0x90 (cmd0=9 cmd1=0) 下发给 MCU。
 * 每份都是真实配置，绝不是全零——全零等于让 MCU 用错误的曝光/阈值表采集。
 * sensor type 由 chipid 决定。 */
static const uint8_t gf_config_type2[224] = {   /* MilanG, chipid 0x2208 */
    0x30, 0x11, 0x64, 0x75, 0x00, 0x75, 0x2c, 0xa1, 0x1c, 0xbd, 0x18, 0xd5, 0x00, 0xd5, 0x00, 0xd5,
    0x00, 0xba, 0x00, 0x00, 0x80, 0xca, 0x00, 0x06, 0x00, 0x84, 0x00, 0xbe, 0xb2, 0x86, 0x00, 0xc5,
    0xb9, 0x88, 0x00, 0xb5, 0xad, 0x8a, 0x00, 0x9d, 0x95, 0x8c, 0x00, 0x00, 0xbe, 0x8e, 0x00, 0x00,
    0xc5, 0x90, 0x00, 0x00, 0xb5, 0x92, 0x00, 0x00, 0x9d, 0x94, 0x00, 0x00, 0xaf, 0x96, 0x00, 0x00,
    0xbf, 0x98, 0x00, 0x00, 0xb6, 0x9a, 0x00, 0x00, 0xa7, 0xd2, 0x00, 0x00, 0x00, 0xd4, 0x00, 0x00,
    0x00, 0xd6, 0x00, 0x00, 0x00, 0xd8, 0x00, 0x00, 0x00, 0x12, 0x00, 0x03, 0x04, 0xd0, 0x00, 0x00,
    0x00, 0x70, 0x00, 0x00, 0x00, 0x72, 0x00, 0x78, 0x56, 0x74, 0x00, 0x34, 0x12, 0x20, 0x00, 0x10,
    0x40, 0x20, 0x02, 0x08, 0x10, 0x2a, 0x01, 0x82, 0x03, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x14,
    0x00, 0x80, 0x00, 0x01, 0x04, 0x5c, 0x00, 0x00, 0x01, 0x56, 0x00, 0x0c, 0x24, 0x58, 0x00, 0x05,
    0x00, 0x32, 0x00, 0x08, 0x02, 0x66, 0x00, 0x00, 0x02, 0x7c, 0x00, 0x00, 0x38, 0x82, 0x00, 0x80,
    0x15, 0x2a, 0x01, 0x08, 0x00, 0x5c, 0x00, 0x80, 0x00, 0x54, 0x00, 0x00, 0x01, 0x62, 0x00, 0x38,
    0x04, 0x64, 0x00, 0x10, 0x00, 0x66, 0x00, 0x00, 0x02, 0x7c, 0x00, 0x01, 0x38, 0x2a, 0x01, 0x08,
    0x00, 0x5c, 0x00, 0x80, 0x00, 0x52, 0x00, 0x08, 0x00, 0x54, 0x00, 0x00, 0x01, 0x66, 0x00, 0x00,
    0x02, 0x7c, 0x00, 0x01, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t gf_config_type3[224] = {   /* MilanL, chipid 0x2205 */
    0x18, 0x11, 0x70, 0x81, 0x00, 0x81, 0x24, 0xa5, 0x10, 0xb5, 0x10, 0xc5, 0x00, 0xc5, 0x00, 0xc5,
    0x00, 0x04, 0x02, 0x00, 0x00, 0x08, 0x00, 0x11, 0x11, 0xba, 0x00, 0x01, 0x80, 0xca, 0x00, 0x07,
    0x00, 0x84, 0x00, 0xbe, 0xb2, 0x86, 0x00, 0xc5, 0xb9, 0x88, 0x00, 0xb5, 0xad, 0x8a, 0x00, 0x9d,
    0x95, 0x8c, 0x00, 0x00, 0xbe, 0x8e, 0x00, 0x00, 0xc5, 0x90, 0x00, 0x00, 0xb5, 0x92, 0x00, 0x00,
    0x9d, 0x94, 0x00, 0x00, 0xaf, 0x96, 0x00, 0x00, 0xbf, 0x98, 0x00, 0x00, 0xb6, 0x9a, 0x00, 0x00,
    0xa7, 0xd2, 0x00, 0x00, 0x00, 0xd4, 0x00, 0x00, 0x00, 0xd6, 0x00, 0x00, 0x00, 0xd8, 0x00, 0x00,
    0x00, 0x50, 0x00, 0x01, 0x05, 0xd0, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x72, 0x00, 0x78,
    0x56, 0x74, 0x00, 0x34, 0x12, 0x20, 0x00, 0x10, 0x40, 0x12, 0x00, 0x03, 0x04, 0x20, 0x02, 0x08,
    0x10, 0x2a, 0x01, 0x82, 0x03, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x14, 0x00, 0x80, 0x00, 0x01,
    0x00, 0x5c, 0x00, 0x00, 0x01, 0x56, 0x00, 0x08, 0x2c, 0x58, 0x00, 0x03, 0x00, 0x32, 0x00, 0x08,
    0x04, 0x82, 0x00, 0x80, 0x15, 0x2a, 0x01, 0x08, 0x00, 0x5c, 0x00, 0x80, 0x00, 0x62, 0x00, 0x0a,
    0x04, 0x64, 0x00, 0x18, 0x00, 0x2a, 0x01, 0x08, 0x00, 0x5c, 0x00, 0x80, 0x00, 0x52, 0x00, 0x08,
    0x00, 0x54, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t gf_config_type12[224] = {  /* ChicagoHS, chipid 0x2503/0x2504 */
    0x70, 0x11, 0x74, 0x85, 0x00, 0x85, 0x2c, 0xb1, 0x18, 0xc9, 0x14, 0xdd, 0x00, 0xdd, 0x00, 0xdd,
    0x00, 0xba, 0x00, 0x01, 0x80, 0xca, 0x00, 0x04, 0x00, 0x84, 0x00, 0x15, 0xb3, 0x86, 0x00, 0x00,
    0xc4, 0x88, 0x00, 0x00, 0xba, 0x8a, 0x00, 0x00, 0xb2, 0x8c, 0x00, 0x00, 0xaa, 0x8e, 0x00, 0x00,
    0xc1, 0x90, 0x00, 0xbb, 0xbb, 0x92, 0x00, 0xb1, 0xb1, 0x94, 0x00, 0x00, 0xa8, 0x96, 0x00, 0x00,
    0xb6, 0x98, 0x00, 0x00, 0x00, 0x9a, 0x00, 0x00, 0x00, 0xd2, 0x00, 0x00, 0x00, 0xd4, 0x00, 0x00,
    0x00, 0xd6, 0x00, 0x00, 0x00, 0xd8, 0x00, 0x00, 0x00, 0x50, 0x00, 0x01, 0x05, 0xd0, 0x00, 0x00,
    0x00, 0x70, 0x00, 0x00, 0x00, 0x72, 0x00, 0x78, 0x56, 0x74, 0x00, 0x34, 0x12, 0x20, 0x00, 0x10,
    0x40, 0x5c, 0x00, 0x80, 0x01, 0x20, 0x02, 0x08, 0x08, 0x36, 0x02, 0x80, 0x00, 0x38, 0x02, 0x80,
    0x00, 0x3a, 0x02, 0x80, 0x00, 0x2a, 0x01, 0x82, 0x03, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x14,
    0x00, 0x80, 0x00, 0x01, 0x00, 0x5c, 0x00, 0x00, 0x01, 0x56, 0x00, 0x04, 0x20, 0x58, 0x00, 0x03,
    0x02, 0x32, 0x00, 0x0c, 0x02, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0x58, 0x82, 0x00, 0x80,
    0x15, 0x2a, 0x01, 0x08, 0x00, 0x54, 0x00, 0x10, 0x01, 0x62, 0x00, 0x04, 0x03, 0x64, 0x00, 0x19,
    0x00, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0x58, 0x2a, 0x01, 0x08, 0x00, 0x52, 0x00, 0x08,
    0x00, 0x54, 0x00, 0x00, 0x01, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00,
};
static const uint8_t gf_config_type14[224] = {  /* ChicagoT, chipid 0x2505-0x2508/0x2510 */
    0x90, 0x11, 0x74, 0x85, 0x00, 0x85, 0x2c, 0xb1, 0x18, 0xc9, 0x14, 0xdd, 0x00, 0xdd, 0x00, 0xdd,
    0x00, 0xba, 0x00, 0x01, 0x80, 0xca, 0x00, 0x07, 0x00, 0x84, 0x00, 0x15, 0xb3, 0x86, 0x00, 0x00,
    0xc4, 0x88, 0x00, 0x00, 0xba, 0x8a, 0x00, 0x00, 0xb2, 0x8c, 0x00, 0x00, 0xaa, 0x8e, 0x00, 0x00,
    0xc1, 0x90, 0x00, 0x00, 0xbb, 0x92, 0x00, 0x00, 0xb1, 0x94, 0x00, 0x00, 0xa8, 0x96, 0x00, 0x00,
    0xb6, 0x98, 0x00, 0x00, 0x00, 0x9a, 0x00, 0x00, 0x00, 0xd2, 0x00, 0x00, 0x00, 0xd4, 0x00, 0x00,
    0x00, 0xd6, 0x00, 0x00, 0x00, 0xd8, 0x00, 0x00, 0x00, 0x50, 0x00, 0x01, 0x05, 0xd0, 0x00, 0x00,
    0x00, 0x70, 0x00, 0x00, 0x00, 0x72, 0x00, 0x78, 0x56, 0x74, 0x00, 0x34, 0x12, 0x20, 0x00, 0x10,
    0x40, 0x5c, 0x00, 0x80, 0x01, 0x20, 0x02, 0x08, 0x08, 0x36, 0x02, 0x80, 0x00, 0x38, 0x02, 0x80,
    0x00, 0x3a, 0x02, 0x80, 0x00, 0x2a, 0x01, 0x82, 0x03, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x14,
    0x00, 0x80, 0x00, 0x01, 0x00, 0x5c, 0x00, 0x00, 0x01, 0x56, 0x00, 0x04, 0x14, 0x58, 0x00, 0x02,
    0x02, 0x32, 0x00, 0x0c, 0x05, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0xbc, 0x82, 0x00, 0x80,
    0x15, 0x2a, 0x01, 0x08, 0x00, 0x54, 0x00, 0x10, 0x01, 0x62, 0x00, 0x04, 0x03, 0x64, 0x00, 0x0a,
    0x00, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0xbc, 0x2a, 0x01, 0x08, 0x00, 0x52, 0x00, 0x08,
    0x00, 0x54, 0x00, 0x00, 0x01, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0xbc, 0x00, 0xbc, 0xff,
};

/* chipid -> sensor type/geometry/config（按 chipid 选择的 switch +
 * 几何表: [0]=col [1]=row [4]=frame bytes） */
struct gf_sensor_info {
    uint8_t type;
    uint16_t w, h;
    const uint8_t *config;
};
static const struct gf_sensor_info *gf_sensor_by_chipid(uint16_t chipid)
{
    static const struct gf_sensor_info tbl[] = {
        {  2, 176,  54, gf_config_type2  },   /* 0x2208 MilanG (GF5116M) */
        {  3, 112, 132, gf_config_type3  },   /* 0x2205 MilanL */
        { 12,  80,  64, gf_config_type12 },   /* 0x2503/0x2504 ChicagoHS */
        { 14,  36, 160, gf_config_type14 },   /* 0x2505-0x2508/0x2510 ChicagoT */
    };
    int idx;
    if (chipid == 0x2205)            idx = 1;
    else if (chipid == 0x2208)       idx = 0;
    else if (chipid == 0x2503 || chipid == 0x2504)
        idx = 2;
    else if ((chipid >= 0x2505 && chipid <= 0x2508) || chipid == 0x2510)
        idx = 3;
    else
        return NULL;
    return &tbl[idx];
}

/* ================= 命令辅助函数 ================ */

/* 版本握手重试：N 次尝试后，MCU 软复位 + 重试。
 *
 * 参考流程：
 *   for i in retry_count:
 *       读版本（NOP + 0xA8）          [ack 100ms, data 500ms, evt 9]
 *       if ok: return
 *   全部失败：
 *       MCU 硬复位命令：
 *           0xA2 {2,20}, ack_tmo=0, data_tmo=0, evt=-1（fire & forget）
 *           （{2,20}=复位 MCU，{1,20}=复位传感器）
 *       复位完成后重新读版本
 *
 * 这是设备的"唤醒"机制：冷插入 / 休眠中的 MCU 在收到 0xA2 {2,20}
 * 软复位之前不会应答 0xA8。
 */
static int evk_with_retry(struct goodix_dev *d, uint8_t out[64])
{
    int r;

    /* 层级 0：仅读版本（NOP + 0xA8），重试 retry_count 次 */
    for (int i = 0; i < RETRY_COUNT; i++) {
        if (gx_stop_requested(d))
            return -4;              /* 取消（fprintd stop）：快速退出 */
        memset(out, 0, 64);
        r = gx_dev_evk(d, out);
        if (r == 0)
            return 0;
        LOG("GetEvkVersion try %d failed (%d)", i + 1, r);
        usleep(100000);
    }

    /* 层级 1：MCU 软复位 0xA2 {2,20}。唤醒休眠 / 冷启动的 MCU。 */
    if (gx_stop_requested(d))
        return -4;
    LOG("wake L1: soft-reset MCU (0xA2 {2,20})");
    uint8_t reset_mcu[2] = { 2, 20 };
    gx_send_cmd(d, 0xA2, reset_mcu, 2, 100);
    usleep(600000);
    memset(out, 0, 64);
    r = gx_dev_evk(d, out);
    if (r == 0)
        return 0;
    LOG("GetEvkVersion after L1 wake failed (%d)", r);

    /* 层级 2：复位 MCU + 传感器 0xA2 {3,20} */
    if (gx_stop_requested(d))
        return -4;
    LOG("wake L2: soft-reset MCU+sensor (0xA2 {3,20})");
    uint8_t reset_both[2] = { 3, 20 };
    gx_send_cmd(d, 0xA2, reset_both, 2, 100);
    usleep(600000);
    memset(out, 0, 64);
    r = gx_dev_evk(d, out);
    if (r == 0)
        return 0;
    LOG("GetEvkVersion after L2 wake failed (%d)", r);

    /* 层级 3：USB 端口复位（设备加入时主机侧通常会自动执行；
     * libusb_open 不会）。重新唤醒 MCU 的 USB 外设。
     * gx_usb_reset 内部会重新做 CDC 激活（端口复位会清掉 DTR），
     * 否则复位后设备依旧静默丢包；若仍无响应，整体 close + 重开
     * （重新 detach 内核驱动 / claim / CDC 激活），覆盖复位后
     * 内核重新绑定 cdc_acm 导致句柄失效的情况。 */
    if (gx_stop_requested(d))
        return -4;
    LOG("wake L3: USB port reset + retry");
    gx_usb_reset(d);
    usleep(600000);
    memset(out, 0, 64);
    r = gx_dev_evk(d, out);
    if (r == 0)
        return 0;
    LOG("GetEvkVersion after L3 USB reset failed (%d)", r);
    if (gx_transport_reopen(d, 5000) == 0) {
        memset(out, 0, 64);
        r = gx_dev_evk(d, out);
        if (r == 0)
            return 0;
        LOG("GetEvkVersion after L3 reopen failed (%d)", r);
    }

    return -1;
}

/* 读 OTP：0xA6 {0,0}, evt 8。
 * 有效 OTP 长度按参考实现中的传感器参数表：
 * type 12/14 = 64 字节（设备实际也只回 64B 数据）。 */
static int cmd_read_otp(struct goodix_dev *d, uint8_t *out, uint16_t len)
{
    uint8_t data[2] = { 0, 0 };
    uint8_t rsp[256];
    uint16_t rl = sizeof(rsp);
    int r = gx_send_cmd_wait(d, GF_CMD_READ_OTP, data, 2, rsp, &rl, 500);
    if (r == 0 && out && rl > 0) {
        uint16_t n = rl < len ? rl : len;
        memcpy(out, rsp, n);
        d->otp_len = n;         /* 实际有效 OTP 长度（type 12 = 64） */
    }
    return r;
}

/* 配置下发：0x90 config, evt 3（224 字节） */
static int cmd_download_config(struct goodix_dev *d, const uint8_t *cfg, uint16_t len)
{
    uint8_t rsp[8]; uint16_t rl = sizeof(rsp);
    /* 先复位传感器进入 Idle，再下发配置 */
    gx_dev_reset(d);
    usleep(50000);
    return gx_send_cmd_wait(d, GF_CMD_DOWNLOAD_CONFIG, cfg, len, rsp, &rl, 500);
}

/* 设置 BESD（device_action 15）：仅本地标志，不下发 USB 命令 */
static void cmd_set_besd(struct goodix_dev *d, uint8_t en)
{
    LOG("SetBesDenable(%d)", en);
    d->besd = en;
}

/* ================= 初始化 MCU ================================== */

static int init_mcu(struct goodix_dev *d)
{
    uint8_t evk[64] = { 0 };
    int r;

    /* 版本握手（带重试） */
    r = evk_with_retry(d, evk);
    if (r < 0) {
        LOG("GetEvkVersion failed - MCU unresponsive");
        return r;
    }
    LOG("MCU version: %.32s", evk);
    memcpy(d->evk, evk, 64);

    /* 设置驱动状态：0x97 {1,2}，在版本握手之后执行 */
    gx_set_driver_state(d, 1);

    /* 读 MCU 状态：0xAF {0x55, ts16le, 0, 0}, evt6。
     * 参考流程每次初始化都会调用；返回 16 字节 MCU 状态，其 byte[1]
     * 的位标志驱动 POV/采集路径（(state[1] & 2) = pov_valid）。
     * 这里仅记录用于诊断。 */
    {
        uint8_t mcu_state[16] = { 0 };
        int sr = gx_get_mcu_state(d, mcu_state);
        if (sr == 0) {
            LOG("MCU state: %02X %02X %02X %02X %02X %02X %02X %02X "
                "(pov_valid=%d)", mcu_state[0], mcu_state[1], mcu_state[2],
                mcu_state[3], mcu_state[4], mcu_state[5], mcu_state[6],
                mcu_state[7], (mcu_state[1] & 2) ? 1 : 0);
        } else {
            LOG("GetMcuState failed (%d)", sr);
        }
    }

    /* project 8 (ST32)：无条件执行固件更新。
     * 内部逻辑：同版本跳过 / 清应用区+复位 / IAP 下载。 */
    r = gx_fw_update(d, evk);
    if (r == FW_SOFT_RESET) {
        /* MCU is rebooting into APP; stop init, wait for next run.
         *（返回固件软复位标记后停止初始化流程）*/
        LOG("firmware updated, MCU reset - init deferred");
        return FW_SOFT_RESET;
    }
    if (r < 0) {
        LOG("firmware update failed (%d)", r);
        return r;
    }
    /* r == 1 (same version) or 0: continue */

    /* PSK 处理：
     *   1. 校验 PSK 有效性（主机哈希 vs MCU 哈希 0xBB020003），2 次尝试
     *   2. 无效则写新 PSK（随机 -> WB 加密 -> TLV 0xE0），2 次尝试
     * 主机侧：PSK 持久化到 0600 权限文件（对应参考实现的缓存文件；
     * 参考实现用 DPAPI 密封，但 MCU 从不校验密封，因此任意 Linux 存储
     * 方式都可行：先用文件，后续可选接入 libsecret）。 */
    r = -1;
    /* try local store first (avoids MCU read; we wrote it last run).
     * GOODIX_RESET_PSK=1 forces a fresh random PSK + rewrite (use after
     * the white-box AES key fix; the MCU still holds a blob made with the
     * OLD key, whose decrypted PSK differs from what we have locally). */
    if (!getenv("GOODIX_RESET_PSK")) {
        uint32_t plen = 32;
        if (gx_psk_store_load(d->psk, &plen) == 0 && plen == 32) {
            d->psk_len = 32;
            d->have_psk = true;
            r = 0;
            LOG("PSK recovered from local store");
        }
    } else {
        LOG("GOODIX_RESET_PSK: ignoring local store, will rewrite");
    }
    /* 自愈（关键）：本地缓存只是缓存，必须与 MCU 持有同一 PSK，TLS 握手
     * 才可能成功。用 MCU 哈希（0xBB020003）验证本地缓存：
     *   - 一致（0）     -> 直接用；
     *   - 不一致（-6）  -> 缓存过期，丢弃后走"读 MCU / 重写"路径；
     *     典型场景：CLI（~/.config/goodix）与 fprintd（/var/lib/fprint/
     *     goodix）分属不同状态目录却共享同一 MCU，后写者覆盖 MCU 的 PSK
     *     使先写者缓存失效；或 WB 密钥更新后 MCU 仍持旧 blob。若信任过期
     *     缓存，握手必然 bad_record_mac（MBEDTLS_ERR_SSL_INVALID_MAC），
     *     MCU 进入锁定态，表现为后续 evk 全部超时且无法自愈。
     *   - 其它错误      -> 传输层暂不可答，保留本地 PSK，下次 init 再校验。 */
    if (r == 0) {
        int vr = gx_psk_verify_mcu_hash(d);
        if (vr == -6) {
            LOG("local PSK mismatch with MCU - re-sync");
            r = -1;
        } else if (vr != 0) {
            LOG("PSK verify failed (%d), keeping local PSK", vr);
        }
    }
    /* Recovery path (no local store / cache expired): try MCU WB read.
     * 0xBB010003 is the WB storage segment (production_write_key 2nd TLV).
     * 成功即 WB 解密已验证密钥一致，并把 MCU 的 PSK 固化回本地缓存。
     * GOODIX_RESET_PSK 时跳过读 MCU，强制重写新 PSK（与注释语义一致）。 */
    if (r != 0 && !getenv("GOODIX_RESET_PSK")) {
        r = gx_psk_read_from_mcu(d);
        if (r == 0)
            gx_psk_store_save(d->psk, 32);
    }
    if (r == 0) {
        LOG("PSK valid, no update needed");
    } else {
        LOG("PSK invalid (%d), writing new PSK", r);
        uint8_t new_psk[32];
        FILE *ur = fopen("/dev/urandom", "rb");
        if (ur) {
            size_t got = fread(new_psk, 1, 32, ur);
            fclose(ur);
            if (got == 32) {
                r = gx_psk_write_to_mcu(d, new_psk);
                if (r == 0) {
                    LOG("PSK written, persist + verify");
                    gx_psk_store_save(new_psk, 32);
                    /* don't re-read: local store already holds the PSK;
                     * verify via MCU hash (0xBB020003) instead. */
                    memcpy(d->psk, new_psk, 32);
                    d->psk_len = 32;
                    d->have_psk = true;
                    r = 0;
                    gx_psk_verify_mcu_hash(d);
                } else {
                    LOG("PSK write failed (%d)", r);
                }
            }
        }
    }
    d->have_psk = (r == 0);

    d->mcu_inited = true;
    return 0;
}

/* ================= 初始化传感器 =============================== */

static int init_fpsensor(struct goodix_dev *d)
{
    uint8_t otp[256] = { 0 };
    uint8_t rsp[16];
    int r;

    /* --- 复位传感器进入 Idle --- */
    LOG("SetIdle (ResetSensor)");
    r = gx_dev_reset(d);
    if (r < 0) {
        LOG("switch to idle failed");
        return r;
    }
    usleep(100000);

    /* --- 读 ChipID：寄存器 0x82 读 4 字节 x retry --- */
    uint16_t chipid = 0;
    for (int i = 0; i < RETRY_COUNT; i++) {
        if (gx_stop_requested(d))
            return -4;
        uint16_t rl = 16;
        r = gx_send_cmd_wait(d, GF_CMD_CHIPREG_READ,
                             (const uint8_t[]){ 0, 0, 0, 4, 0 }, 5,
                             rsp, &rl, 500);
        if (r == 0 && rl >= 3) {
            /* chipid = Dst[1] | Dst[2]<<8 */
            chipid = (uint16_t)(rsp[1] | (rsp[2] << 8));
            LOG("Get Chip ID: 0x%04x", chipid);
            break;
        }
        LOG("read chipid failed, try again, reset sensor");
        gx_dev_reset(d);
        usleep(100000);
    }
    d->chipid = chipid;
    if (chipid == 0) {
        LOG("Get Chip ID Failed");
        return -1;
    }

    /* --- 按 chipid 选择传感器参数 ---
     * chipid 决定 sensor type / 几何 / 224B 配置表；未知 chipid 在参考
     * 实现里走 "wrong senseor type kept" 并视为初始化失败，我们也失败。 */
    const struct gf_sensor_info *si = gf_sensor_by_chipid(chipid);
    if (!si) {
        LOG("unknown chipid 0x%04x - no sensor config, init fails",
            chipid);
        return -1;
    }
    d->sensor_type = si->type;
    d->img_w = si->w;
    d->img_h = si->h;
    d->img_size = (uint16_t)(si->w * si->h * 2);
    LOG("sensor type %d, %ux%u, frame %u bytes",
        si->type, si->w, si->h, d->img_size);

    /* --- OTP：读 OTP -> OTP 解析 -> FDT 初始化 -> 配置下发 ---
     * 顺序：读 OTP -> 解析（用 OTP 工厂校准值现场补丁 224B 配置表）
     * -> FDT 初始化 -> 下发补丁后的配置。不补丁直接发静态表 =
     * FDT 阈值/曝光全是通用默认值，手指检测不触发。 */
    r = cmd_read_otp(d, otp, sizeof(otp));
    if (r == 0) {
        LOG("Success to get OTP data (%d bytes)", (int)d->otp_len);
        memcpy(d->otp, otp, sizeof(otp));   /* goodix.dat 的绑定凭据 */
        /* 基线存在性检查：加载 FDT 基线（CRC+OTP 校验） */
        gx_base_load(d);
        /* OTP 解析：用 OTP 补丁配置表（tcode/delta/offset/DAC） */
        {
            uint8_t cfg[224];
            memcpy(cfg, si->config, sizeof(cfg));
            d->fdt_delta = 21;              /* 参考实现默认值 */
            if (gx_otp_patch_config(d, cfg, &d->fdt_delta))
                LOG("OTP parsed, config patched (fdt_delta=%u)",
                    d->fdt_delta);
            LOG("OTP data valid, download config");
            /* 配置下发 224B：发送补丁后的配置 */
            r = cmd_download_config(d, cfg, sizeof(cfg));
        }
        d->have_config = (r == 0);
        LOG("download config %s", d->have_config ? "ok" : "failed");
    } else {
        LOG("fail to get OTP data, reset sensor");
        gx_dev_reset(d);
    }

    d->fp_inited = true;
    return 0;
}

/* ================= 设备初始化（总入口） ========================== */

static int device_init(struct goodix_dev *d)
{
    int r;

    cmd_set_besd(d, 0);

    r = init_mcu(d);
    if (r < 0)
        return r;   /* includes FW_SOFT_RESET */

    r = init_fpsensor(d);
    if (r < 0) {
        LOG("fail to init FP");
        return r;
    }

    cmd_set_besd(d, 1);
    return 0;
}

/* ================= 顶层初始化流程 ============================== */

/* MCU 硬复位（0xA2 {2,20}）后 USB 设备会重新枚举：先从总线消失几百毫秒，
 * 再以（可能不同的）PID 重新出现。参考实现此时会关闭并重新打开设备。
 * 复位后我们的 libusb 句柄已失效（LIBUSB_ERROR_NO_DEVICE），
 * 因此关闭句柄并轮询直到设备重新出现，再重新打开（claim + CDC activate）。 */
static int gx_transport_reopen(struct goodix_dev *d, int max_ms)
{
    gx_transport_close(d);
    /* MCU 复位后可能以不同 PID 重新枚举（见上方注释）：扫描任一。
     * gx_transport_open 匹配成功后会把 d->pid 更新为实际 PID。 */
    d->pid = 0;
    int waited = 0;
    while (waited < max_ms) {
        if (gx_stop_requested(d))
            return -4;              /* 取消：不再等设备重新枚举 */
        usleep(200000);
        waited += 200;
        if (gx_transport_open(d) == 0) {
            LOG("device re-enumerated after %d ms, reopened", waited);
            return 0;
        }
    }
    LOG("device did not re-enumerate within %d ms", max_ms);
    return -1;
}

int gx_device_init(struct goodix_dev *d)
{
    int r;

    LOG("=== device init %04x:%04x ===", d->vid, d->pid);

    for (int try = 0; try < RETRY_COUNT; try++) {
        if (gx_stop_requested(d)) {
            LOG("device init cancelled");
            return -4;
        }
        /* evk 唤醒 L3 的 reopen 失败会把传输层关掉（usb_devh=NULL）；
         * 下次重试前先重新打开，避免带着空句柄继续跑（见 transport.c
         * 的 NULL 防护，这里是主动恢复）。 */
        if (!d->usb_devh && gx_transport_open(d) != 0) {
            LOG("device gone, cannot reopen transport");
            return -1;
        }
        r = device_init(d);
        if (r == 0)
            break;
        if (r == FW_SOFT_RESET) {
            /* 固件更新后 MCU 重启，参考实现会停止初始化。
             * USB 设备将重新枚举，必须重新打开。 */
            LOG("soft reset marker, waiting for MCU reboot + re-enum...");
            usleep(1000000);
            if (gx_transport_reopen(d, 8000) < 0) {
                LOG("Init Device Failed (no device after MCU reset)");
                return -1;
            }
            /* device state is fresh after re-open */
            memset(d->evk, 0, sizeof(d->evk));
            d->mcu_inited = false;
            d->fp_inited = false;
            d->besd = 0;
            continue;
        }
        LOG("device init try %d failed (%d)", try + 1, r);
        usleep(300000);
    }
    if (r != 0) {
        LOG("Init Device Failed");
        return r;
    }

    LOG("Init Device Success");

    /* --- TLS 握手（仅当存在 PSK 时）---
     * gx_tls_server_init: mbedtls 上下文建立
     * gx_tls_handshake:    服务端握手(0xD1) -> mbedtls 握手 ->
     *                      握手命令下发(0xD4) -> 读 MCU 状态 bit1
     * 参考实现会重试整个序列。这里只执行一次；
     * 失败时调用方可重新调用 gx_device_init。 */
    if (d->have_psk && !getenv("GOODIX_NO_TLS")) {
        r = gx_tls_server_init(d);
        if (r == 0) {
            r = gx_tls_handshake(d);
        } else {
            LOG("TLS server init failed (%d)", r);
        }
    } else if (d->have_psk) {
        LOG("TLS skipped (GOODIX_NO_TLS) - testing plaintext capture");
    }

    /* --- "Init: update all base..."（TLS 握手之后）---
     * 无基线文件时，参考实现会更新全部基线：FDT-manual (0x36)
     * 让 MCU 现场采样无手指 FDT 基线，随后直接曝光采一帧无手指
     * 图像基线，一并存 goodix.dat。没有真实基线表时 FDT down 布防
     * 不会报手指事件，没有图像基线时主机侧手指判别无法进行，所以
     * 这两步是首次采集的前提。（此时传感器上不能有手指——参考实现
     * 同样假设。） */
    if (d->fp_inited && !d->base_valid) {
        LOG("no FDT base, sampling via FDT-manual");
        if (gx_fdt_sample_base(d) == 0) {
            gx_base_save(d);
        } else {
            LOG("FDT base sampling failed - capture falls back to "
                "default-config arm");
        }
    }
    if (d->fp_inited && !d->img_base_valid) {
        LOG("no image base, capturing no-finger frame");
        if (gx_capture_base_image(d) == 0) {
            gx_base_save(d);
        } else {
            LOG("image base capture failed - finger check disabled");
        }
    }

    LOG("=== device init complete ===");
    return 0;
}
