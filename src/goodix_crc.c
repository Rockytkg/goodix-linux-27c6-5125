/* goodix_crc.c - CRC-32/MPEG-2 共享实现
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 项目中三处（图像线 CRC、goodix.dat 基线 CRC、固件 CRC）使用的是同一个
 * 算法：CRC-32/MPEG-2（poly 0x04C11DB7，初值 0xFFFFFFFF，MSB-first，
 * 无最终异或）。此前三处各自实现了一份（逐位 / 查表 ×2），这里合并为
 * 单一查表实现，供三个模块共用。
 *
 * 注意：这是 MPEG-2 变体，不是 zlib/ISO 的 CRC-32（后者位序相反且有
 * 最终异或），不要用 zlib 的 crc32() 替代。
 */
#include <stdint.h>
#include <stddef.h>
#include "goodix.h"     /* gx_crc32_mpeg2 原型 */

static uint32_t crc_tab[256];
static int crc_tab_ready = 0;

static void crc32_init_table(void)
{
    if (crc_tab_ready)
        return;
    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i << 24;
        for (int k = 0; k < 8; k++)
            c = (c & 0x80000000u) ? ((c << 1) ^ 0x04C11DB7u) : (c << 1);
        crc_tab[i] = c;
    }
    crc_tab_ready = 1;
}

/* 以 crc 为初值（通常 0xFFFFFFFFu）对 data 计算 CRC-32/MPEG-2，返回当前值。
 * 无最终异或：结果可直接与存储值比较。 */
uint32_t gx_crc32_mpeg2(const uint8_t *data, size_t len, uint32_t crc)
{
    crc32_init_table();
    for (size_t i = 0; i < len; i++)
        crc = (crc << 8) ^ crc_tab[(uint8_t)(data[i] ^ (crc >> 24))];
    return crc;
}
