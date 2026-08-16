/* goodix_frame.c - 帧层（明文命令帧 / TLS 帧 / 响应帧的封装与解析）
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * == 明文命令帧 ==
 *   [0]    = 0xA0（类型；低半字节保留上一次的值，首次为 0）
 *   [1:2]  = 载荷长度 LE = datalen + 4
 *            （载荷 = cmd[1] + len[2] + data[datalen] + cks[1]）
 *   [3]    = 帧头校验和：(b0 + b1 + b2) & 0xFF
 *   [4]    = 命令字节：(cmd0<<4)|(cmd1<<1)  [仅不等待 ACK 时 |1]
 *   [5:6]  = datalen + 1 (LE)
 *   [7..]  = 数据
 *   [末]   = 数据校验和：0xAA - (sum(data) + (dlen+1) + ((dlen+1)>>8) + cmd)
 *   总长   = datalen + 8
 *
 * == TLS 帧（类型 0xB）==
 *   [0]    = 0xB0
 *   [1:2]  = TLS 记录长度 (LE)   <- 载荷 = 原始 TLS 记录
 *   [3]    = 帧头校验和
 *   [4..]  = TLS 记录（帧级无末尾校验和：载荷直接交给 TLS 层）
 *
 * == 响应帧 ==
 *   [0]    = 0xA0|0xB0|0xC0（类型，高半字节 0xA/0xB/0xC）
 *   [1:2]  = 载荷长度 LE；总长 = 载荷 + 4
 *   [3]    = 帧头校验和
 *   [4]    = 响应命令字节（type<<4|sub<<1|more）
 *   [5:6]  = 数据长度 LE
 *   [7..]  = 数据（+ 末尾校验和或 0x88）
 *
 * 传输：按 64 字节块发送，每块补齐到 64 字节。
 */
#include <stdlib.h>
#include <string.h>
#include "goodix.h"

static inline uint8_t hdr_cks(uint8_t a, uint8_t b, uint8_t c)
{ return (uint8_t)(a + b + c); }

/* 数据校验和：0xAA - (sum(data) + (len+1) + ((len+1)>>8) + cmd)
 * 校验和用不含 more 位的命令字节，故 cks_cmd = cmd & 0xFE。 */
static inline uint8_t data_cks(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    uint8_t s = (uint8_t)((cmd & 0xFE) + (len + 1) + ((len + 1) >> 8));
    for (uint16_t i = 0; i < len; i++)
        s += data[i];
    return (uint8_t)(0xAA - s);
}

/* 将缓冲区按 64B 零填充块发送 */
static int usb_send_chunks(struct goodix_dev *d, const uint8_t *frame, uint16_t total)
{
    uint16_t off = 0;
    while (off < total) {
        uint8_t chunk[64];
        uint16_t n = total - off;
        if (n > 64)
            n = 64;
        memset(chunk, 0, sizeof(chunk));
        memcpy(chunk, &frame[off], n);
        if (gx_usb_write(d, chunk, 64) != 64)
            return -2;
        off = (uint16_t)(off + n);
    }
    return 0;
}

/* 发送明文命令帧 */
int gx_send_cmd_frame(struct goodix_dev *d, uint8_t cmd,
                      const uint8_t *data, uint16_t datalen)
{
    uint8_t *frame;
    uint16_t total = datalen + 8;        /* hdr4 + payload(datalen+4) */
    uint16_t paylen = datalen + 4;       /* cmd+len+data+cks */
    int r;

    if (total > MAX_FRAME)
        return -1;
    frame = malloc(total);
    if (!frame)
        return -1;

    frame[0] = GF_TYPE_PLAIN;
    frame[1] = (uint8_t)paylen;
    frame[2] = (uint8_t)(paylen >> 8);
    frame[3] = hdr_cks(frame[0], frame[1], frame[2]);
    frame[4] = cmd;
    frame[5] = (uint8_t)(datalen + 1);
    frame[6] = (uint8_t)((datalen + 1) >> 8);
    if (datalen)
        memcpy(&frame[7], data, datalen);
    frame[7 + datalen] = data_cks(cmd, data, datalen);

    r = usb_send_chunks(d, frame, total);
    free(frame);
    return r;
}

/* 发送 TLS 帧：[0xB0][rlen][cks][TLS 记录]（无末尾校验和） */
int gx_send_tls_frame(struct goodix_dev *d, const uint8_t *record, uint16_t rlen)
{
    uint8_t *frame;
    uint16_t total = 4 + rlen;
    int r;

    if (total > MAX_FRAME)
        return -1;
    frame = malloc(total);
    if (!frame)
        return -1;

    frame[0] = GF_TYPE_TLS;
    frame[1] = (uint8_t)rlen;
    frame[2] = (uint8_t)(rlen >> 8);
    frame[3] = hdr_cks(frame[0], frame[1], frame[2]);
    memcpy(&frame[4], record, rlen);

    r = usb_send_chunks(d, frame, total);
    free(frame);
    return r;
}

/* 读取一帧（A0/B0/C0），重组 64B 块。
 * 帧头 [1:2] = 载荷长度；总长 = 载荷 + 4。
 * 调用方缓冲区必须 >= MAX_FRAME。 */
int gx_read_frame(struct goodix_dev *d, uint8_t *frame, uint16_t *len, int tmo)
{
    uint8_t buf[64];
    uint16_t total = 0, paylen = 0;
    int r;
    int deadline = tmo > 0 ? tmo : 2000;

    while (deadline > 0) {
        r = gx_usb_read(d, buf, 64, 100);
        if (r < 0) {
            deadline -= 100;
            continue;
        }
        /* 非进展路径同样消耗预算：每次读最多阻塞 100ms。若只对错误路径
         * 扣减，噪声帧/半包会让本函数实际超时无上界（可远大于 tmo）。 */
        if (r < 4) {
            deadline -= 100;
            continue;
        }
        if ((buf[0] >> 4) != 0xA && (buf[0] >> 4) != 0xB && (buf[0] >> 4) != 0xC) {
            deadline -= 100;
            continue;
        }
        if (buf[3] != hdr_cks(buf[0], buf[1], buf[2])) {
            deadline -= 100;
            continue;
        }
        paylen = (uint16_t)(buf[1] | (buf[2] << 8));
        if (paylen < 1 || paylen > MAX_FRAME - 4) {
            deadline -= 100;
            continue;
        }
        total = (uint16_t)(paylen + 4);
        memcpy(frame, buf, r < 64 ? (size_t)r : 64);
        if (r >= total) {
            *len = total;
            return 0;
        }
        /* read remaining chunks */
        uint16_t want = (uint16_t)(total - r);
        while (want > 0) {
            r = gx_usb_read(d, buf, 64, 100);
            if (r < 0) {
                deadline -= 100;
                if (deadline <= 0)
                    return -1;
                continue;
            }
            if (r > want)
                r = want;
            memcpy(&frame[total - want], buf, (size_t)r);
            want = (uint16_t)(want - r);
        }
        *len = total;
        return 0;
    }
    return -1;
}

/* 发送 NOP 命令：发送 (dev, 0, 0, {0,0,0,0}, 4, 0, 0)
 *   cmd0=0 cmd1=0, a7=0 -> more=1, a6=0 -> 无校验和 (0x88)。
 *   帧：A0 08 00 A8 01 05 00 00 00 00 00 88  （pcap 报文 17 验证）
 * 在读取 EVK 版本 / 设置驱动状态 / 读取 MCU 状态之前发送。
 */
int gx_send_nop(struct goodix_dev *d)
{
    uint8_t frame[12];
    frame[0]  = 0xA0;
    frame[1]  = 8;            /* 载荷长度 datalen+4 */
    frame[2]  = 0;
    frame[3]  = (uint8_t)(0xA0 + 8);   /* 帧头校验和 */
    frame[4]  = 0x01;         /* 命令：more=1 */
    frame[5]  = 5;            /* 数据长度 datalen+1 */
    frame[6]  = 0;
    frame[7]  = 0;            /* 4 个零数据字节 */
    frame[8]  = 0;
    frame[9]  = 0;
    frame[10] = 0;
    frame[11] = 0x88;         /* 无校验和标志：0x88 尾部 */
    return usb_send_chunks(d, frame, sizeof(frame));
}
