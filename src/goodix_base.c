/* goodix_base.c - FDT 基线持久化与学习（goodix.dat 基线机制）
 *
 * 基线相关逻辑：
 *   - 基线文件加载（init 末尾）：加载 goodix.dat，CRC 校验 +
 *     OTP 逐字节绑定，匹配才采用。
 *   - 基线落盘：基线更新后写回文件。
 *   - 文件基线 -> FDT-down 阈值表。
 *   - 设备回传基线的合法性检查。
 *   - 从 FDT IRQ 推送的原始数据计算 down/up 阈值表。
 *
 * 文件布局（type 12 ChicagoHS）：
 *   [OTP 64B][fdt_base 12B][navbase 3200B][imagebase 10240B][CRC32 4B LE]
 *   （OTP 段长度 = 64，不是读出缓冲的 256）
 * CRC = CRC-32/MPEG-2（poly 0x04C11DB7，初值 0xFFFFFFFF，无最终异或，
 * 查表实现，种子初值 = -1）。
 *
 * 说明：navbase(3200B) 来自初始化时基线采样流程的
 * 导航基线采样，仅用于导航事件（本驱动不做导航，保留字段写 0）；
 * imagebase(10240B) 是同一流程采的无手指图像基线，加载后作为主机侧
 * 手指判别（FDT 判别）的比较基准，本驱动如实持久化。
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "goodix.h"

#define BASE_FDT_LEN   12                   /* FDT 表长度（type 12） */
#define BASE_NAV_LEN   3200                 /* 导航基线长度（type 12） */
#define BASE_IMG_LEN   10240                /* 图像基线长度（type 12） */
#define BASE_TAIL_LEN  (BASE_FDT_LEN + BASE_NAV_LEN + BASE_IMG_LEN + 4)

/* CRC-32/MPEG-2 表（MSB-first，poly 0x04C11DB7） */
static uint32_t crc_tab[256];
static int crc_tab_ready = 0;
static void crc32_init_table(void)
{
    if (crc_tab_ready)
        return;
    for (int i = 0; i < 256; i++) {
        uint32_t crc = (uint32_t)i << 24;
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u)
                                      : (crc << 1);
        crc_tab[i] = crc;
    }
    crc_tab_ready = 1;
}

/* CRC-32 计算：初值 0xFFFFFFFF，无最终异或 */
static uint32_t base_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    crc32_init_table();
    for (size_t i = 0; i < len; i++)
        crc = (crc << 8) ^ crc_tab[(uint8_t)(data[i] ^ (crc >> 24))];
    return crc;
}

const char *gx_state_dir(void)
{
    static char dir[512];
    const char *home;
    if (geteuid() == 0)
        return "/var/lib/fprint/goodix";
    home = getenv("HOME");
    if (!home || !*home)
        home = "/tmp";
    snprintf(dir, sizeof(dir), "%s/.config/goodix", home);
    return dir;
}

int gx_state_dir_ensure(void)
{
    const char *d;
    if (geteuid() == 0) {
        mkdir("/var/lib/fprint", 0755);
    } else {
        const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
        char parent[512];
        snprintf(parent, sizeof(parent), "%s/.config", home);
        mkdir(parent, 0700);
    }
    d = gx_state_dir();
    if (mkdir(d, 0700) == 0 || errno == EEXIST) {
        chmod(d, 0700);
        return 0;
    }
    return -1;
}

static const char *base_store_path(void)
{
    static char path[512];
    snprintf(path, sizeof(path), "%s/goodix.dat", gx_state_dir());
    return path;
}

/* 基线合法性检查：每个 u16 的 (v>>1)&0xFF 不能为 0 或 0xFF */
int gx_fdt_base_check(const uint8_t raw[12])
{
    for (int i = 0; i < 12; i += 2) {
        uint16_t v = (uint16_t)(raw[i] | (raw[i + 1] << 8));
        uint8_t b = (uint8_t)(v >> 1);
        if (b == 0 || b == 0xFF) {
            LOG("fdt base invalid (word%d = 0x%04x)", i / 2, v);
            return 0;
        }
    }
    return 1;
}

/* down 阈值表学习：fdt-up IRQ 的原始基线 -> down 阈值表
 * table[i] = ((raw16[i] >> 1) << 8) | 0x80 */
int gx_fdt_learn_down_base(struct goodix_dev *d, const uint8_t raw[12])
{
    if (!gx_fdt_base_check(raw))
        return -1;
    for (int i = 0; i < 12; i += 2) {
        uint16_t v = (uint16_t)(raw[i] | (raw[i + 1] << 8));
        uint16_t w = (uint16_t)(((v >> 1) << 8) | 0x80);
        d->fdt_down_base[i] = (uint8_t)w;
        d->fdt_down_base[i + 1] = (uint8_t)(w >> 8);
    }
    d->base_dirty = true;
    LOG("learned fdt-down base from device IRQ");
    return 0;
}

/* up 阈值表学习：
 * table[i] = ((raw16[i]>>1) + delta) << 8 | 0x80，
 * touchflag 第 i 位为 0 的槽位强制 ((delta-2)<<8)|0x80（delta=21 时即 4992）。
 * delta 来自 OTP 解析（d->fdt_delta，默认 21）。 */
int gx_fdt_learn_up_base(struct goodix_dev *d, const uint8_t raw[12],
                         uint16_t touchflag)
{
    uint8_t delta = d->fdt_delta ? d->fdt_delta : 21;
    if (!gx_fdt_base_check(raw))
        return -1;
    for (int i = 0; i < 6; i++) {
        uint16_t w;
        if (!((touchflag >> i) & 1)) {
            w = (uint16_t)(((delta - 2) << 8) | 0x80);
        } else {
            uint16_t v = (uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
            w = (uint16_t)(((delta + (v >> 1)) << 8) | 0x80);
        }
        d->fdt_up_base[2 * i] = (uint8_t)w;
        d->fdt_up_base[2 * i + 1] = (uint8_t)(w >> 8);
    }
    d->base_dirty = true;
    LOG("learned fdt-up base from device IRQ (touchflag 0x%x, delta %u)",
        touchflag, delta);
    return 0;
}

/* 基线文件加载路径：
 * 读文件 -> CRC 校验 -> OTP 绑定 -> 取出 12B FDT 阈值表。 */
int gx_base_load(struct goodix_dev *d)
{
    uint8_t buf[256 + BASE_TAIL_LEN];
    const char *path = base_store_path();
    size_t want;
    FILE *f;
    size_t got;

    if (!d->otp_len)
        return -1;
    want = d->otp_len + BASE_TAIL_LEN;
    f = fopen(path, "rb");
    if (!f)
        return -1;                        /* 无基线文件：走采样 */
    got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (got != want) {
        LOG("base file %s size mismatch (%u, want %u), ignored",
            path, (unsigned)got, (unsigned)want);
        return -2;
    }
    uint32_t crc_file = (uint32_t)buf[got - 4] | ((uint32_t)buf[got - 3] << 8) |
                        ((uint32_t)buf[got - 2] << 16) | ((uint32_t)buf[got - 1] << 24);
    uint32_t crc_calc = base_crc32(buf, got - 4);
    if (crc_file != crc_calc) {
        LOG("base file CRC mismatch (file 0x%08x calc 0x%08x), ignored",
            crc_file, crc_calc);
        return -3;
    }
    if (memcmp(buf, d->otp, d->otp_len) != 0) {
        LOG("base file OTP mismatch (different device?), ignored");
        return -4;
    }
    memcpy(d->fdt_down_base, buf + d->otp_len, BASE_FDT_LEN);
    /* imagebase 段（offset = otp + 12 + 3200）：主机侧手指判别的
     * 无手指基线图像（装入全局基线图供判别使用）。
     * type 12 固定 10240B。
     * 全 0 段视为无效（旧版本存档只写 FDT 表，imagebase 段是占位 0，
     * 采用它会让判别把一切帧当成 finger）。 */
    if (d->img_size == BASE_IMG_LEN) {
        const uint8_t *ib = buf + d->otp_len + BASE_FDT_LEN + BASE_NAV_LEN;
        int nonzero = 0;
        for (int i = 0; i < BASE_IMG_LEN; i++)
            if (ib[i]) {
                nonzero = 1;
                break;
            }
        if (nonzero) {
            memcpy(d->img_base, ib, BASE_IMG_LEN);
            d->img_base_valid = true;
        }
    }
    /* 文件里的表必须是合法阈值（使用文件前同样做合法性检查） */
    d->base_valid = true;
    LOG("FDT base loaded from %s (CRC+OTP ok%s)", path,
        d->img_base_valid ? ", imagebase ok" : "");
    return 0;
}

/* 基线落盘：布局固定；
 * navbase 段写 0（不做导航采样），imagebase 段写真实无手指基线图像。 */
int gx_base_save(struct goodix_dev *d)
{
    uint8_t buf[256 + BASE_TAIL_LEN];
    const char *path = base_store_path();
    size_t total;
    FILE *f;

    if (!d->otp_len)
        return -1;                        /* 没有 OTP 不建档 */
    total = d->otp_len + BASE_TAIL_LEN;
    memset(buf, 0, sizeof(buf));
    memcpy(buf, d->otp, d->otp_len);
    memcpy(buf + d->otp_len, d->fdt_down_base, BASE_FDT_LEN);
    /* imagebase 段：有基线图像写真实数据，否则保持 0（未采到基线时
     * 参考实现也不会建档——由调用方保证 img_base_valid 时再调）。 */
    if (d->img_base_valid && d->img_size == BASE_IMG_LEN)
        memcpy(buf + d->otp_len + BASE_FDT_LEN + BASE_NAV_LEN,
               d->img_base, BASE_IMG_LEN);
    uint32_t crc = base_crc32(buf, total - 4);
    buf[total - 4] = (uint8_t)crc;
    buf[total - 3] = (uint8_t)(crc >> 8);
    buf[total - 2] = (uint8_t)(crc >> 16);
    buf[total - 1] = (uint8_t)(crc >> 24);

    gx_state_dir_ensure();

    f = fopen(path, "wb");
    if (!f) {
        LOG("base store: cannot open %s", path);
        return -2;
    }
    if (fwrite(buf, 1, total, f) != total) {
        fclose(f);
        return -3;
    }
    fclose(f);
    chmod(path, 0600);
    d->base_dirty = false;
    LOG("FDT base stored to %s (%u B) - goodix.dat equivalent",
        path, (unsigned)total);
    return 0;
}
