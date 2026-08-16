/* goodix_fwupdate.c - STM32 MCU 固件下载
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * （项目 8 = ST32 3626, 27c6:5125, ST411SEC）。
 *
 * 固件数据（include/goodix_fw.h 内嵌的 gf_fw = firmware/st411sec_app.bin
 * 原始镜像，128404 字节，以 ARM 向量表开头）：
 *   gf_fw[]        = 固件镜像（128404 字节，直接传输，不含版本前缀）
 *   GF_FW_VERSION  = "GF_ST411SEC_APP_12508"（仅用于版本比较）
 *   GF_FW_CRC      = 镜像的 CRC-32/MPEG-2（0x6FBC0F40，运行时校验）
 *
 * 固件更新流程：
 *   1. MCU 不在 IAP 且版本相同           -> 无需更新（返回 1）
 *   2. MCU 不在 IAP 且版本不同           -> 清除 APP（0xA4）
 *      + 硬复位 + 重读 EVK 版本 -> 返回 -2097165（软复位）
 *   3. MCU 在 IAP -> 传输固件（gf_fw, 128404）：
 *      - 1008 字节分片，命令 0xF0 {off32,len32,data}，evt5
 *      - 结束命令 0xF4 {0,0,len32,crc32}，evt5（MCU 校验 CRC）
 *      - CRC32 为非反射（init 0xFFFFFFFF，poly 0x04C11DB7，
 *        MSB 优先，无最终异或）= 本固件的 0x6FBC0F40
 *   4. 传输成功 -> 硬复位 -> 返回 -2097165
 *
 * init 阶段将 -2097165 作为"软复位，停止初始化"标记向上传播
 * （4292870131 = 0xFFDFFFF3）；后续流程停止并等待下一次运行。
 */
#include <string.h>
#include <unistd.h>
#include "goodix.h"
#include "goodix_fw.h"

#define FW_SLICE 1008

/* ---- 传输一个分片：0xF0 {off32le, len32le, data} ---- */
static int fw_send_slice(struct goodix_dev *d, const uint8_t *fw,
                         uint32_t offset, uint32_t len)
{
    uint8_t payload[8 + FW_SLICE];
    uint8_t rsp[16];
    uint16_t rl = sizeof(rsp);
    int r;

    memcpy(payload, &offset, 4);
    memcpy(payload + 4, &len, 4);
    memcpy(payload + 8, fw + offset, len);
    r = gx_send_cmd_wait(d, GF_CMD_FW_SLICE, payload, (uint16_t)(8 + len), rsp, &rl, 2000);
    if (r < 0)
        LOG("slice @0x%x (%u B) failed: %d", offset, len, r);
    return r;
}

/* ---- 结束：0xF4 {0, 0, len32le, crc32le} -> MCU 校验 ----
 * 然后检查响应第一个数据字节：非零 = CRC+len 校验通过。
 * 状态字节通过 *status 返回（0 = MCU 拒绝）。 */
static int fw_finish(struct goodix_dev *d, uint32_t len, uint32_t crc,
                     uint8_t *status)
{
    uint8_t payload[12] = { 0 };
    uint8_t rsp[16];
    uint16_t rl = sizeof(rsp);
    int r;

    memcpy(payload + 4, &len, 4);
    memcpy(payload + 8, &crc, 4);
    r = gx_send_cmd_wait(d, GF_CMD_FW_FINISH, payload, 12, rsp, &rl, 2000);
    if (r < 0) {
        LOG("firmware finish cmd failed: %d", r);
        return r;
    }
    if (status)
        *status = (rl > 0) ? rsp[0] : 0;
    LOG("firmware finish response: data[0]=0x%02x", status ? *status : 0);
    return r;
}

/* 固件传输核心：传输分片 + CRC 结束。
 * 成功后 MCU 硬复位；返回 FW_SOFT_RESET 标记。 */
static int fw_update_core(struct goodix_dev *d, const uint8_t *fw, uint32_t total)
{
    uint32_t frames = (total + FW_SLICE - 1) / FW_SLICE;
    uint32_t crc;
    int r;

    LOG("firmware download: %u bytes, %u frames", total, frames);

    /* 内嵌固件完整性预检：GF_FW_CRC 与运行时计算的 CRC-32/MPEG-2 不符
     * （镜像损坏 / 生成脚本出错）时直接拒绝，避免把坏固件刷进 MCU。 */
    crc = gx_crc32_mpeg2(fw, total, 0xFFFFFFFFu);
    if (crc != GF_FW_CRC) {
        LOG("embedded firmware CRC mismatch (calc 0x%08X, expected 0x%08X) - "
            "refusing to flash", crc, GF_FW_CRC);
        return -6;
    }

    for (uint32_t i = 0; i < frames; i++) {
        uint32_t off = i * FW_SLICE;
        uint32_t len = (total - off > FW_SLICE) ? FW_SLICE : (total - off);
        r = fw_send_slice(d, fw, off, len);
        if (r < 0)
            return r;
    }

    crc = gx_crc32_mpeg2(fw, total, 0xFFFFFFFFu);
    LOG("firmware CRC32: 0x%08X (non-reflected)", crc);

    uint8_t status = 0;
    r = fw_finish(d, total, crc, &status);
    if (r < 0)
        return r;

    /* MCU 必须已校验 CRC+len（响应 data[0] != 0）。
     * 若未校验通过 -> "check firmware failed"，返回 0。 */
    if (status == 0) {
        LOG("MCU rejected firmware (CRC/LEN check failed, data[0]=0x00)");
        return 0;
    }
    LOG("firmware verified by MCU (data[0]=0x%02x)", status);

    /* 硬复位 MCU 以运行 APP：0xA2 {2,20}，只发不等。
     * 否则 MCU 会一直停留在 IAP 并持续上报 IAP。 */
    LOG("firmware OK, hard-reset MCU to APP (0xA2 {2,20})");
    uint8_t reset[2] = { 2, 20 };
    gx_send_cmd(d, 0xA2, reset, 2, 100);
    usleep(1000000);   /* MCU flash-verify + reboot into APP */
    LOG("firmware download complete, MCU reset to APP");
    return FW_SOFT_RESET;
}

/* 清除 APP（cmd0=10 cmd1=2 {0,0}, ackt=1, tmo=500, evt=-1）
 * 擦除 APP 区域；随后 MCU 重启进入 IAP。 */
static int fw_clear_app(struct goodix_dev *d)
{
    uint8_t data[2] = { 0, 0 };
    uint8_t rsp[4]; uint16_t rl = sizeof(rsp);
    return gx_send_cmd_wait(d, GF_CMD_CLEAR_APP, data, 2, rsp, &rl, 500);
}

/* 固件更新主流程。
 * now_version：64B 的 EVK 版本字符串。
 * 返回 1 = 无需更新；FW_SOFT_RESET = 已更新并复位；
 * 0 = 失败。 */
static int fw_update_master(struct goodix_dev *d, uint8_t now_version[64])
{
    char ver[65];
    memcpy(ver, now_version, 64);
    ver[64] = 0;

    /* 1. MCU 不在 IAP 时比较版本 */
    if (!strstr(ver, "IAP")) {
        LOG("MCU not in IAP (%.32s), compare version", ver);
        if (strncmp(ver, GF_FW_VERSION, strlen(GF_FW_VERSION)) == 0) {
            LOG("same version, no firmware update needed");
            return 1;
        }
        LOG("version differs, Clear App first");
        /* 0xA4 清除 APP 区；gx_send_cmd_wait 成功返回 0（此前这里
         * "r > 0" 分支永远不可达，属于死代码——清除后 MCU 重启进入
         * IAP，下面统一走"重读版本 -> 在 IAP 则下载"路径）。 */
        fw_clear_app(d);
        usleep(200000);
        memset(now_version, 0, 64);
        gx_dev_evk(d, now_version);
        memcpy(ver, now_version, 64);
        ver[64] = 0;
    }

    /* 2. 现在必须处于 IAP */
    if (strstr(ver, "IAP")) {
        LOG("MCU in IAP, downloading firmware");
        return fw_update_core(d, gf_fw, GF_FW_LEN);
    }

    LOG("Fail to clear APP, MCU still not in IAP");
    return 0;
}

/* 公开入口：运行完整固件更新流程。
 * 返回 0 表示成功/无需更新，FW_SOFT_RESET 标记，或 <0 错误。 */
int gx_fw_update(struct goodix_dev *d, uint8_t evk[64])
{
    return fw_update_master(d, evk);
}
