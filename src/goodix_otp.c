/* goodix_otp.c - OTP 解析 + 224B 配置表补丁。
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 背景：
 *   初始化顺序是 读OTP -> OTP解析 -> FDT 初始化 -> 配置下发。
 *   OTP 解析会用 OTP 里的工厂校准值现场修改 224B 配置表里的
 *   若干寄存器项，然后配置下发把改过的表发给 MCU。
 *   不补丁直接发静态表 = FDT 阈值/曝光参数全是通用默认值，
 *   手指检测不触发（这是 --capture 布防后无 FDT 事件的根因之一）。
 *
 *   补丁项（type 12 ChicagoHS, chipid 0x2503/0x2504）：
 *     reg 0x220 <- (16*dac0)|8     section 0, 全写   (OTP DAC)
 *     reg 0x236 <- dac1            section 0, 全写
 *     reg 0x238 <- dac2            section 0, 全写
 *     reg 0x23A <- dac3            section 0, 全写
 *     reg 0x5C  <- tcode           section 0, 全写   (OTP tcode)
 *     reg 0x82  <- delta<<8        section 2, 只写高字节 (FDT delta)
 *     reg 0x56  <- fdtOffset+4     section 2, 只写低字节 (仅 offset!=0)
 *
 * 配置表结构：
 *   header: section s 的表项区偏移/字节数 = cfg[2s+1], cfg[2s+2]
 *   表项: [reg16 LE][val16 LE]，步进 4；命中即改并重写 16 位校验和
 *   校验和: u16 cks = -(0xA5A5 + sum(u16[0..110]))，
 *   存在 word[111]（byte 222..223，LE）。
 */
#include <string.h>
#include "goodix.h"

/* ---------------- CRC-8 ----------------
 * CRC-8/SMBUS（poly 0x07，初值 0，MSB-first），
 * 返回时再按位取反（return ~i）。直接按位算，不搬表。 */
static uint8_t gf_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                               : (uint8_t)(crc << 1);
    }
    return (uint8_t)~crc;
}

/* ---------------- 配置表 16 位校验和 ---------------- */
static void gf_config_fix_checksum(uint8_t *cfg)
{
    int16_t v = (int16_t)0xA5A5;            /* -23131 */
    for (int i = 0; i < 111; i++)
        v = (int16_t)(v + (int16_t)(cfg[2 * i] | (cfg[2 * i + 1] << 8)));
    uint16_t cks = (uint16_t)(-v);
    cfg[222] = (uint8_t)cks;
    cfg[223] = (uint8_t)(cks >> 8);
}

/* ---------------- 修改配置表 ----------------
 * cfg: 224B 配置表；reg: 目标寄存器；val: 新值；
 * section: 0..7（选择 header 里的表项区）；mode: 0=全写 1=只写低字节 2=只写高字节。
 * 返回 1 = 命中并修改。 */
static int gf_modify_config(uint8_t *cfg, uint16_t reg, uint16_t val,
                            int section, int mode)
{
    if (section < 0 || section > 7)
        return 0;
    uint8_t off = cfg[2 * section + 1];
    uint8_t cnt = cfg[2 * section + 2];
    /* 防御：表项区必须落在 224B 配置表内且按 4B 步进。当前 4 张静态表
     * 均已验证合法，此检查防止未来新增表写错时越界读。 */
    if (cnt % 4 != 0 || (uint16_t)off + cnt > 224)
        return 0;
    for (int i = 0; i < cnt; i += 4) {
        uint16_t r = (uint16_t)(cfg[off + i] | (cfg[off + i + 1] << 8));
        if (r != reg)
            continue;
        if (mode == 1) {
            cfg[off + i + 2] = (uint8_t)val;
        } else if (mode == 2) {
            cfg[off + i + 3] = (uint8_t)(val >> 8);
        } else {
            cfg[off + i + 2] = (uint8_t)val;
            cfg[off + i + 3] = (uint8_t)(val >> 8);
        }
        LOG("config patch: reg 0x%04x -> 0x%04x (section %d mode %d)",
            reg, val, section, mode);
        gf_config_fix_checksum(cfg);
        return 1;
    }
    LOG("config patch: reg 0x%04x not found in section %d", reg, section);
    return 0;
}

/* ---------------- OTP CRC 分组校验 ----------------
 * cp: OTP[0..10] + OTP[36..39]        -> OTP[60]
 * ft: OTP[11..19] + OTP[28] + OTP[50..53] + OTP[56..59] + OTP[62] -> OTP[61]
 * mt: OTP[20..27] + OTP[29..35] + OTP[40..49] + OTP[54..55]       -> OTP[63]
 * 参考实现里 CRC 失败只记日志，不阻断（DAC 检查才是解析门禁）。 */
static void gf_otp_crc_groups(const uint8_t *otp, int *cp_ok, int *ft_ok,
                              int *mt_ok)
{
    uint8_t buf[32];

    memset(buf, 0, sizeof(buf));
    memcpy(buf, otp, 11);
    memcpy(buf + 11, otp + 36, 4);
    *cp_ok = (gf_crc8(buf, 15) == otp[60]);

    memset(buf, 0, sizeof(buf));
    memcpy(buf, otp + 11, 9);
    buf[9] = otp[28];
    memcpy(buf + 10, otp + 50, 4);
    memcpy(buf + 14, otp + 56, 4);
    buf[18] = otp[62];
    *ft_ok = (gf_crc8(buf, 19) == otp[61]);

    memset(buf, 0, sizeof(buf));
    memcpy(buf, otp + 20, 8);
    memcpy(buf + 8, otp + 29, 7);
    memcpy(buf + 15, otp + 40, 10);
    memcpy(buf + 25, otp + 54, 2);
    *mt_ok = (gf_crc8(buf, 27) == otp[63]);

    LOG("OTP CRC: cp=%d ft=%d mt=%d", *cp_ok, *ft_ok, *mt_ok);
}

/* ---------------- OTP DAC 提取 ----------------
 * ft dac = OTP[50..53]（自带 CRC8 -> OTP[62]），mt dac = OTP[46..49]
 *（CRC8 -> OTP[22]）。选择顺序与参考实现一致：
 *   ft 组有效（组 CRC 或自 CRC）-> 用 ft；
 *   mt 有效 -> 用 mt；
 *   否则逐位多数表决（>=3 位一致才行；3 位一致时不一致位取其余三位均值）。
 * 返回 1 = dac[4] 有效（这是 OTP 解析的总门禁）。 */
static int gf_otp_dac_extract(const uint8_t *otp, int ft_grp, int mt_grp,
                              uint8_t dac[4])
{
    int ft_ok = 0, mt_ok = 0;

    if (otp[50] && otp[51] && otp[52] && otp[53])
        ft_ok = (gf_crc8(otp + 50, 4) == otp[62]);
    if (ft_grp || ft_ok) {
        memcpy(dac, otp + 50, 4);
        return 1;
    }

    if (otp[46] && otp[47] && otp[48] && otp[49])
        mt_ok = (gf_crc8(otp + 46, 4) == otp[22]);
    if (mt_grp || mt_ok) {
        memcpy(dac, otp + 46, 4);
        return 1;
    }

    int same = 0;
    for (int i = 0; i < 4; i++)
        if (otp[46 + i] == otp[50 + i] && otp[50 + i])
            same++;
    if (same < 3) {
        LOG("OTP ft/mt dac match num check failed (%d)", same);
        return 0;
    }
    if (same == 4) {
        memcpy(dac, otp + 50, 4);
        return 1;
    }
    for (int i = 0; i < 4; i++) {
        if (otp[46 + i] == otp[50 + i]) {
            dac[i] = otp[50 + i];
        } else {
            dac[i] = (uint8_t)((otp[46 + (i + 2) % 4] +
                                otp[46 + (i + 3) % 4] +
                                otp[46 + (i + 1) % 4]) / 3);
        }
    }
    return 1;
}

/* ---------------- OTP 解析主入口 ----------------
 * 合并自 OTP 检查与 OTP 解析两段流程：解析 OTP，现场补丁 224B 配置表。
 * 同时输出 FDT delta（fdt-up 基线学习要用；默认 21）。
 * 返回 1 = OTP 有效且补丁完成；0 = OTP 无效（参考实现此时不下任何补丁，
 * 保留静态表，delta/tcode 用默认 21/128）。 */
int gx_otp_patch_config(struct goodix_dev *d, uint8_t *cfg, uint8_t *delta_out)
{
    const uint8_t *otp = d->otp;
    uint8_t dac[4];
    int cp_ok, ft_ok, mt_ok;
    uint8_t tcode_diff = 0;
    uint16_t tcode = 128;
    uint8_t delta = 21;

    *delta_out = 21;                 /* 默认 fdt delta */
    d->fdt_tcode = 128;              /* 默认 tcode 初值 */

    if (d->otp_len < 64) {
        LOG("OTP too short (%u), skip config patch", d->otp_len);
        return 0;
    }

    gf_otp_crc_groups(otp, &cp_ok, &ft_ok, &mt_ok);
    (void)cp_ok;

    if (!gf_otp_dac_extract(otp, ft_ok, mt_ok, dac)) {
        LOG("OTP DAC check failed - keep static config");
        return 0;
    }
    LOG("OTP dac 0x%02x 0x%02x 0x%02x 0x%02x", dac[0], dac[1], dac[2], dac[3]);

    /* DAC -> 曝光寄存器（section 0 全写） */
    gf_modify_config(cfg, 0x220, (uint16_t)((16 * dac[0]) | 8), 0, 0);
    gf_modify_config(cfg, 0x236, dac[1], 0, 0);
    gf_modify_config(cfg, 0x238, dac[2], 0, 0);
    gf_modify_config(cfg, 0x23A, dac[3], 0, 0);

    /* tcode_diff：OTP[42]/[43]/[45] 三取一 */
    if (otp[42] && otp[42] == (uint8_t)~otp[43])
        tcode_diff = otp[42];
    else if (otp[45] && otp[45] == (uint8_t)~otp[43])
        tcode_diff = otp[45];
    else if (otp[42] && otp[45] == otp[42])
        tcode_diff = otp[42];
    else
        LOG("wrong OTP tcode_diff - keep static tcode/delta");

    if (tcode_diff) {
        tcode = (uint16_t)(16 * (((tcode_diff >> 4) & 0xF) + 1) + 64);
        gf_modify_config(cfg, 0x5C, tcode, 0, 0);
        /* delta = ((100*((diff&0xF)+2) << 8) / tcode / 3) >> 4 */
        delta = (uint8_t)((((100u * ((tcode_diff & 0xF) + 2)) << 8)
                           / tcode / 3) >> 4);
        *delta_out = delta;
        gf_modify_config(cfg, 0x82, (uint16_t)(delta << 8), 2, 2);
        LOG("OTP tcode %u, fdt delta %u", tcode, delta);
    }
    d->fdt_tcode = tcode;   /* 判别阈值 T=tcode*delta/8 要用 */

    /* fdtOffset：OTP[27]，仅 chipid 0x2503/0x2504（9475/9476） */
    if (otp[27] && (d->chipid == 0x2504 || d->chipid == 0x2503)) {
        uint8_t b = otp[27], off;
        if ((b & 3) == ((b >> 4) & 3))
            off = b & 3;
        else if ((b & 3) == ((~b >> 2) & 3))
            off = b & 3;
        else if (((b >> 4) & 3) == ((~b >> 2) & 3))
            off = (b >> 4) & 3;
        else
            off = 0;
        LOG("fdtOffset: 0x%x", off);
        if (off)
            gf_modify_config(cfg, 0x56, (uint16_t)(off + 4), 2, 1);
    }
    return 1;
}
