/* goodix_cmd.c - 命令层（命令字节与功能对应）
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 命令字节 = (cmd0<<4)|(cmd1<<1)：
 *   ResetSensor    0xA2  cmd0=10 cmd1=1  data={1,20}   [复位 MCU 与指纹模块]
 *   GetEvkVersion  0xA8  cmd0=10 cmd1=4  data={0,0}    [读取 EVK 版本]
 *   GetMcuState    0xAF  cmd0=10 cmd1=7  data={0x55,ts16,0,0} [读取 MCU 状态, more=1]
 *   ChipRegRead    0x82  cmd0=8  cmd1=1  data={0,reg16,len16} [读芯片寄存器]
 *   SetMode idle   0x20  cmd0=2  cmd1=0                 [设置空闲模式]
 *   SetMode capture 0x36 cmd0=3  cmd1=3                 [设置采集模式]
 *   TLS init       0xD1  cmd0=13 cmd1=0  data={0,0}    [TLS server 初始化, more=1]
 *   TLS handshake  0xD4  cmd0=13 cmd1=2  data={0,0}    [TLS 握手]
 *   MCU read       0xE4  cmd0=14 cmd1=2  data={type32,0}[MCU 生产读]
 *   MCU write      0xE0  cmd0=14 cmd1=0                 [MCU 生产写]
 *   ClearApp       0xA4  cmd0=10 cmd1=2  data={0,0}    [清除 APP 区域]
 *   ReadOtpData    0xA6  cmd0=10 cmd1=3  data={0,0}    [读 OTP 数据]
 *   DownloadConfig 0x90  cmd0=9  cmd1=0  data=cfg(224B)[下载配置]
 *   FW slice       0xF0  cmd0=15 cmd1=0  {off32,len32,data} [固件分片传输]
 *   FW finish      0xF4  cmd0=15 cmd1=2  {0,0,len32,crc32} [固件传输结束]
 */
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include "goodix.h"

/* 发送明文命令 */
int gx_send_cmd(struct goodix_dev *d, uint8_t cmd,
                const uint8_t *data, uint16_t len, int tmo)
{
    (void)tmo;
    return gx_send_cmd_frame(d, cmd, data, len);
}

/* 发送命令并等待匹配的数据响应帧。
 *
 * 协议交互过程：
 *
 *   1. 发送明文帧（命令字节，可能携带 more 位）。
 *   2. 设备回复 ACK 帧（命令字节 0xB0，data[0] = 原命令，
 *      data[1] = 状态位：0x02 配置丢失，0x04 TLS 丢失），
 *      此帧在此处消费并跳过。
 *   3. 设备随后回复 DATA 响应帧，其命令字节必须等于请求命令
 *      （more 位已清零）。该帧触发各命令对应的事件
 *      （读取 EVK 版本：cmd0=10 cmd1=4 -> event 9；
 *       读取 MCU 状态/复位传感器：cmd0=10 cmd1=1/7 -> event 6；
 *       寄存器读写：cmd0=8 -> event 2）。
 *   4. 响应 payload[1:2] 是数据长度，含末尾校验和字节。
 *
 * 返回 0 表示成功，数据复制到 rsp（长度在 *rsp_len，校验和字节已去除），
 * 超时返回 -3。
 */
int gx_send_cmd_wait(struct goodix_dev *d, uint8_t cmd,
                     const uint8_t *data, uint16_t len,
                     uint8_t *rsp, uint16_t *rsp_len, int tmo)
{
    uint8_t frame[MAX_FRAME];
    uint16_t flen = 0;
    int r;
    int deadline = tmo > 0 ? tmo : 1500;
    uint8_t want = (uint8_t)(cmd & 0xFE);   /* 期望响应命令，more 位清零 */

    r = gx_send_cmd(d, cmd, data, len, tmo);
    if (r < 0)
        return r;

    while (deadline > 0) {
        r = gx_read_frame(d, frame, &flen, 200);
        if (r < 0) {
            deadline -= 200;
            continue;
        }
        if (flen < 7)
            continue;
        uint8_t cb = frame[GF_CMD_BYTE];
        uint8_t c0 = cb >> 4;
        if ((cb & 1) != 0)       /* more 位：非最终帧 */
            continue;
        if (c0 == 0xB || c0 == 0xC)
            continue;            /* ACK (0xB0) / 通知 (0xC0)：跳过，继续等待 */
        /* 生产命令（cmd0=0xE）：响应携带相同 cmd0 但 cmd1 不同
         * （读 0xE4 -> 0xE4/0xE6，写 0xE0 -> 0xE2）。仅按 cmd0 匹配。 */
        if ((want >> 4) == 0xE) {
            if (c0 != 0xE)
                continue;
        } else if (cb != want) {
            continue;            /* 其他命令的响应 */
        }

        uint16_t dlen = (uint16_t)(frame[GF_DATA_LEN] | (frame[GF_DATA_LEN + 1] << 8));
        if (dlen < 1)
            continue;
        if (dlen > 1)            /* 去掉末尾校验和字节 */
            dlen--;
        if (dlen > flen - GF_DATA)
            dlen = (uint16_t)(flen - GF_DATA);
        if (rsp && rsp_len && *rsp_len >= dlen)
            memcpy(rsp, &frame[GF_DATA], dlen);
        if (rsp_len)
            *rsp_len = dlen;
        return 0;
    }
    return -3;
}

/* MCU 生产读：0xE4, data={data_type(4B),0}
 * 响应：rsp[0]=状态(0=ok)，数据从 rsp[1] 起。 */
int gx_mcu_read(struct goodix_dev *d, uint32_t data_type, uint8_t *out, uint16_t *len)
{
    uint8_t data[8] = { 0 };
    uint8_t rsp[1024];
    uint16_t rl = sizeof(rsp);
    int r;
    /* 生产读：发送 (dev, 14, 2, data, 8, 1, 500, 1000, 7)，
     * 失败时重试一次。回复 data[0] 是 MCU 执行码：
     * 0 = ok，1 = 无密钥，其他 = 错误。 */
    memcpy(data, &data_type, 4);
    for (int try_i = 0; try_i < 2; try_i++) {
        rl = sizeof(rsp);
        r = gx_send_cmd_wait(d, GF_CMD_MCU_READ, data, 8, rsp, &rl, 1500);
        if (r < 0) {
            LOG("gx_mcu_read try %d failed (%d)", try_i + 1, r);
            usleep(200000);
            continue;
        }
        if (rl < 1) {
            LOG("gx_mcu_read empty response");
            usleep(200000);
            continue;
        }
        if (rsp[0] != 0) {
            LOG("gx_mcu_read(0x%08X): MCU status 0x%02x (1=no data)",
                data_type, rsp[0]);
            return -5;
        }
        if (rl > 1) {
            uint16_t n = (uint16_t)(rl - 1);
            if (n > *len)
                n = *len;
            memcpy(out, &rsp[1], n);
            *len = n;
        } else {
            *len = 0;
        }
        return 0;
    }
    return r;
}

/* MCU 生产写：0xE0，响应状态必须为 0 */
int gx_mcu_write(struct goodix_dev *d, const uint8_t *data, uint16_t len)
{
    uint8_t rsp[8];
    uint16_t rl = sizeof(rsp);
    int r;
    /* 生产写：发送 (dev, 14, 0, data, len, 1, 500, 1000, 7)：
     *   cmd0=14 cmd1=0 (0xE0)，ack_tmo=500，data_tmo=1000，evt=7，
     * 首次写失败时重试一次。192 字节的 blob 可能让 MCU 消化超过 1 秒，
     * 因此使用较长的超时并保留一次重试。 */
    for (int try_i = 0; try_i < 2; try_i++) {
        r = gx_send_cmd_wait(d, GF_CMD_MCU_WRITE, data, len, rsp, &rl, 3000);
        if (r == 0) {
            if (rl < 1 || rsp[0] != 0) {
                LOG("MCU rejected PSK write (status 0x%02x)", rl >= 1 ? rsp[0] : 0);
                return -5;
            }
            return 0;
        }
        LOG("gx_mcu_write try %d failed (%d), retrying", try_i + 1, r);
        usleep(200000);
    }
    return r;
}

/* 设置驱动状态：NOP + 0x97 {state, 2}
 * cmd0=9 cmd1=3, more=1，type 2 = USB。校验和用 0x96。
 * 在读取 EVK 版本之后发送。
 * 帧：A0 06 00 A6 97 03 00 01 02 0E */
int gx_set_driver_state(struct goodix_dev *d, uint8_t state)
{
    uint8_t data[2] = { state, 2 };
    gx_send_nop(d);
    usleep(10000);
    return gx_send_cmd_wait(d, GF_CMD_SET_DRIVER_STATE, data, 2, NULL, NULL, 200);
}

/* 复位传感器：0xA2, data={1,20}（复位 MCU 与指纹模块） */
int gx_dev_reset(struct goodix_dev *d)
{
    uint8_t data[2] = { 1, 20 };
    uint8_t rsp[8];
    uint16_t rl = sizeof(rsp);
    return gx_send_cmd_wait(d, GF_CMD_RESET_SENSOR, data, 2, rsp, &rl, 1000);
}

/* 读取 EVK 版本：先发送 NOP，再 0xA8 {0,0} -> 64B */
int gx_dev_evk(struct goodix_dev *d, uint8_t out[64])
{
    uint8_t data[2] = { 0, 0 };
    uint16_t rl = 64;
    int r = gx_send_nop(d);          /* NOP 前置同步（pcap 报文 17） */
    if (r < 0)
        return r;
    usleep(10000);                    /* 延时 10ms */
    return gx_send_cmd_wait(d, GF_CMD_EVK_VERSION, data, 2, out, &rl, 500);
}

/* 读取 MCU 状态：0xAF (more=1，无 ACK) {0x55, ts16le, 0, 0} -> 16B
 * 抓包报文 23：A0 09 00 A9 AF 06 00 55 [ts16] 00 00 [cks]
 * 校验和用 0xAE。
 * ts16 = 本地时间：毫秒 + 1000*秒（低 16 位），每次调用都填充。
 * MCU 用它做 POV 计时，必须是真实本地时间，不能填零。 */
int gx_get_mcu_state(struct goodix_dev *d, uint8_t out[16])
{
    uint8_t data[5] = { 0x55, 0, 0, 0, 0 };
    uint16_t rl = 16;
    struct timeval tv;
    struct tm tmv;
    if (gettimeofday(&tv, NULL) == 0 && localtime_r(&tv.tv_sec, &tmv)) {
        uint16_t ts = (uint16_t)(tmv.tm_sec * 1000 + tv.tv_usec / 1000);
        data[1] = (uint8_t)ts;
        data[2] = (uint8_t)(ts >> 8);
    }
    return gx_send_cmd_wait(d, GF_CMD_MCU_STATE, data, 5, out, &rl, 500);
}

/* TLS server 初始化：0xD1 {0,0} - 只发送不等待 (event=-1, ack_tmo=0)。
 * 帧被接受即返回；实际 TLS 握手状态由 mbedtls + 读取 MCU 状态跟踪。 */
int gx_tls_server_cmd(struct goodix_dev *d)
{
    uint8_t z[2] = { 0, 0 };
    return gx_send_cmd(d, GF_CMD_TLS_SERVER_INIT, z, 2, 200);
}

/* TLS 握手命令：0xD4 {0,0} (event=-1)。发送后立即返回；
 * 握手完成通过 MCU 状态 bit1 确认。 */
int gx_tls_handshake_cmd(struct goodix_dev *d)
{
    uint8_t z[2] = { 0, 0 };
    return gx_send_cmd(d, GF_CMD_TLS_HANDSHAKE, z, 2, 200);
}
