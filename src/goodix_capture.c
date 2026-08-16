/* goodix_capture.c - 图像采集（POV 唤醒路径 + 手指判别图像路径 + 基线采样）。
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 架构说明（对照参考实现行为 + ST411SEC 固件交互确认）：
 *   - 本机 chipid 0x2504 -> sensor type 12 (ChicagoH)，SetMode 是真实
 *     命令（早期"ST32 ops 全 dummy"的结论只适用于 ST32/MilanG 分支）。
 *   - 采集触发：发送 FDT-down 设置命令
 *     (3,1,1) "Fdt down" -> 发 cmd0=3 cmd1=1 (0x32)，
 *     恒 a3=1：data=[8,1,fdt_down_base(12B),ts16] 共 16 字节。
 *     （a3=0 的 4 字节形态 [8,0,tbl0,tbl1] 只在基线采样失败、
 *     无任何基线时由基线存在性检查兜底使用。）
 *   - 基线来源：init 期基线采样发
 *     FDT-manual (cmd0=3 cmd1=3 = 0x36, data=[9,1,12B 表])，MCU 现场
 *     采样无手指基线并推 cmd0=3 IrqStatus=0x100 帧，host 学为 down 表
 *     （down 表：((raw>>1)<<8)|0x80）。之后手指触碰时 MCU 先推
 *     cmd0=3 FDT 事件帧（IrqStatus=2 finger-down，顺带学 up 表），
 *     再推 cmd0=2 的图像帧。
 *   - POV 路径：MCU state bit0(isPOVImageValid)=1 时发 0xD2 {0,0}
 *     （POV 唤醒命令）取省电唤醒时缓存的图像。
 *   - 通道（USB 数据帧 state>=16）：TLS 激活后 A0 明文帧照常直接
 *     分流，B0 帧进 mbedtls 解密后再分流同一 payload 格式——两边都
 *     可能出现 FDT/图像帧，必须混合接收。
 *
 * 设备数据 payload（== USB 帧去掉 4 字节头）：
 *   payload[0]   = cmd 字节 (cmd0<<4|cmd1<<1|more)
 *   payload[1:2] = 数据长度 v19 (LE), 数据从 [3] 起
 *   payload[3..] = data[0..] : data[0]=0xAA(POV)/非0xAA(图像)
 *                  图像像素 = data[5 ..], 长度 v19-6 (== img_size)
 *   末字节       = 校验和 (0x88 => 跳过校验, 否则累加和==0xAA)
 *
 * 判定图像帧只查 cmd0==2 (设备数据)，不查 cmd1/more。
 */
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <mbedtls/ssl.h>
#include "goodix.h"

/* CRC 校验：数据字节累加和必须等于 0xAA */
static int verify_crc(const uint8_t *data, uint16_t len)
{
    uint8_t s = 0;
    for (uint16_t i = 0; i < len; i++)
        s += data[i];
    return s == 0xAA ? 0 : -1;
}

/* ---- 线上图像 CRC-32（图像数据 CRC-32 计算）----
 * 与 goodix.dat 同一个 CRC-32/MPEG-2（poly 0x04C11DB7，初值 FFFFFFFF，
 * 无最终异或），查表实现见 goodix_crc.c。图像数据末尾 4 字节是 CRC，
 * 存储序是每 16 位半字大端、半字间小端：crc = b1|b0<<8 | b3<<16|b2<<24。 */

/* ---- 12bit 解包 + 转置（列优先像素解包）----
 * 线上 6 字节打包 4 个 12bit 像素；像素流是按列优先（每列 64px）发送的，
 * 解包时转置成行优先 80x64、每像素 16bit（值左对齐在低 12bit）。
 * in: 7680B packed；out: 10240B (80*64*2)。 */
static void unpack_12bit_transpose(const uint8_t *in, uint8_t *out,
                                   unsigned int packed_len)
{
    uint32_t k = 0;                         /* 线序像素号 */
    for (unsigned int j = 0; j < packed_len; j += 6) {
        uint16_t p0 = (uint16_t)(in[j + 1] + ((in[j] & 0xF) << 8));
        uint16_t p1 = (uint16_t)((in[j] >> 4) + 16 * in[j + 3]);
        uint16_t p2 = (uint16_t)(in[j + 2] + ((in[j + 5] & 0xF) << 8));
        uint16_t p3 = (uint16_t)((in[j + 5] >> 4) + 16 * in[j + 4]);
        uint16_t px[4] = { p0, p1, p2, p3 };
        for (int t = 0; t < 4; t++, k++) {
            uint32_t dst = k / 64 + 80 * (k % 64);   /* 转置: 列->行 */
            out[2 * dst] = (uint8_t)px[t];
            out[2 * dst + 1] = (uint8_t)(px[t] >> 8);
        }
    }
}

/* ---------------- 主机侧手指判别 ----------------
 * 新帧与无手指基线图像（d->img_base，全局基线图）做 8x8 分块
 * 统计比较；参数按 sensor type 装填（解析 OTP 后初始化）：
 *   - 网格: brows x bcols 个 8x8 块，块起点查 row_off/col_off 表
 *   - 阈值 T = tcode*16*delta/tdiv（tcode/delta 来自 OTP，tdiv 按 type）
 *   - need = 判别成立所需的高变块数
 * 判定树（a2=基线、a3=新帧）：
 *   v38(高变块数) >= need          -> 新帧显著变亮且基线不变亮 ?
 *                                     3(bad) : 1(finger)
 *   任意块 mean|diff| > 1.4T       -> 新帧块均值比基线亮 1.4T ? 3 : 1
 *   中差异块数(0.6T<d<1.4T) < need -> 2(void 空触发)
 *   否则                           -> 0(temperature 温度漂移, 需重建基线) */
struct fdt_disc_params {
    uint8_t need, brows, bcols;
    uint8_t row_off[5], col_off[5];
    uint8_t h, w;
    uint16_t tdiv;
};
static const struct fdt_disc_params *fdt_disc_params(uint8_t type)
{
    static const struct fdt_disc_params tbl[] = {
        /* type 2 MilanG 176x54 (a1=6, tdiv=256) */
        { 6, 2, 5, { 8, 38 }, { 12, 48, 84, 120, 156 }, 54, 176, 256 },
        /* type 3 MilanL 112x132 (a1=7, tdiv=256) */
        { 7, 3, 4, { 8, 52, 96 }, { 8, 44, 80, 116 }, 112, 132, 256 },
        /* type 12 ChicagoHS 80x64 (a1=4, tdiv=128) */
        { 4, 2, 3, { 12, 44 }, { 4, 36, 68 }, 64, 80, 128 },
        /* type 14 ChicagoT 36x160 (a1=7, tdiv=256) */
        { 7, 5, 2, { 12, 44, 76, 108, 140 }, { 4, 24 }, 160, 36, 256 },
    };
    switch (type) {
    case 2:  return &tbl[0];
    case 3:  return &tbl[1];
    case 12: return &tbl[2];
    case 14: return &tbl[3];
    default: return NULL;
    }
}

static inline uint16_t px16(const uint8_t *img, uint32_t idx)
{
    return (uint16_t)(img[2 * idx] | (img[2 * idx + 1] << 8));
}

/* 手指判别的 mode-0 路径（a1+4==0，8x8 块网格）。
 * 参数对应：a2=基线(base)、a3=新帧(new)；本函数 img=new，
 * d->img_base=base。
 * 返回 1=finger / 2=void / 3=bad / 0=temperature / -1=无法判别。 */
int gx_fdt_check_finger(struct goodix_dev *d, const uint8_t *img)
{
    const struct fdt_disc_params *p = fdt_disc_params(d->sensor_type);
    if (!p || !d->img_base_valid)
        return -1;
    if (d->img_w != p->w || d->img_h != p->h)
        return -1;

    /* 阈值（tcode*16*delta/tdiv，整数运算） */
    uint32_t t = (uint32_t)d->fdt_tcode * 16u * d->fdt_delta / p->tdiv;
    int hi = (int)(t * 1.4);            /* (int)(double(T)*1.4) */
    int lo = (int)(t * 0.6);

    uint8_t v38 = 0, v40 = 0;
    int v39 = 0, v41 = 0;
    for (uint8_t br = 0; br < p->brows; br++) {
        for (uint8_t bc = 0; bc < p->bcols; bc++) {
            uint32_t sum_new = 0, sum_base = 0, sum_diff = 0;
            /* 单遍扫描：像素差异缓存到 di[]（64 像素，|a-b| <= 4095 有符号
             * 16bit 可容纳），第二遍方差直接读缓存，避免把两幅图像各重读
             * 一遍。运算顺序与原两遍实现完全一致，结果逐位相同。 */
            int16_t di[64];
            uint32_t di_n = 0;
            for (uint8_t m = 0; m < 8; m++) {
                for (uint8_t n = 0; n < 8; n++) {
                    uint32_t idx = (uint32_t)p->w * (m + p->row_off[br])
                                   + n + p->col_off[bc];
                    uint16_t a = px16(d->img_base, idx);/* 基线 (a2) */
                    uint16_t b = px16(img, idx);        /* 新帧 (a3) */
                    int16_t diff = (int16_t)((a > b) ? (a - b) : (b - a));
                    sum_base += a;
                    sum_new += b;
                    sum_diff += (uint32_t)diff;
                    di[di_n++] = diff;
                }
            }
            uint32_t mean_base = sum_base / 64;         /* 块均值（偏移 +56） */
            uint32_t mean_new = sum_new / 64;           /* 块均值（偏移 +64） */
            uint32_t mean_diff = sum_diff / 64;         /* 块均值（偏移 +72） */
            /* 块内差异方差（+80）：Σ(mean_d - d_i)^2 / 63 */
            uint64_t var = 0;
            for (uint32_t k = 0; k < 64; k++) {
                int dd = (int)mean_diff - (int)di[k];
                var += (uint64_t)(dd * dd);
            }
            var /= 63;
            if (var > (uint32_t)hi)
                v38++;
            if ((int)mean_diff > hi)
                v39 = 1;
            if ((int)mean_diff < hi && (int)mean_diff > lo)
                v40++;
            /* v41：新帧块均值比基线亮 hi 以上（+64 > +56 + 1.4T） */
            if (mean_new > mean_base + (uint32_t)hi)
                v41 = 1;
        }
    }

    /* 内部像素方向统计（去边框，内部像素扫描）：
     * v53 = 新帧比基线亮 32+ 的像素数（a3 > a2+32）
     * v54 = 基线比新帧亮 32+ 的像素数（a2 > a3+32） */
    uint32_t v53 = 0, v54 = 0;
    for (uint16_t r = 1; r < p->h - 1; r++) {
        for (uint16_t c = 1; c < p->w - 1; c++) {
            uint32_t idx = (uint32_t)c + p->w * r;
            uint16_t a = px16(d->img_base, idx);        /* 基线 */
            uint16_t b = px16(img, idx);                /* 新帧 */
            if (b > a + 32)
                v53++;
            if (a > b + 32)
                v54++;
        }
    }
    uint32_t plim = (uint32_t)((p->w * p->h - 2 * (p->w + p->h) + 4) * 0.1);
    int v51 = v53 >= plim;      /* 新帧整体显著变亮 */
    int v52 = v54 >= plim;      /* 基线整体显著更亮 */

    /* 判定树：
     *   v38>=need: (!v51 || v52) ? 1 : 3
     *     —— 只有"新帧显著变亮且基线不变亮"才是 bad，其余结构化差异=finger
     *   v39:       v41 ? 3 : 1  —— 新帧异常变亮=bad
     *   v40<need:  2(void)；否则 0(temperature) */
    int ret;
    if (v38 >= p->need)
        ret = (v51 && !v52) ? 3 : 1;
    else if (v39)
        ret = v41 ? 3 : 1;
    else if (v40 < p->need)
        ret = 2;
    else
        ret = 0;
    LOG("capture: finger check ret=%d (%s) [v38=%u v39=%d v40=%u v41=%d "
        "v51=%d v52=%d T=%u]", ret,
        ret == 1 ? "finger down" : ret == 2 ? "void" :
        ret == 3 ? "bad" : "temperature",
        v38, v39, v40, v41, v51, v52, t);
    return ret;
}

/* 解析一条设备数据 payload，提取图像帧。
 * payload[0]=cmd, [1:2]=v19, [3..]=data.
 * （设备数据）: 图像 == cmd0==2, cmd1/more 不检查;
 * data[0]==0xAA 是 POV 帧直接跳过; 图像数据在 data[5..], 长度 v19-6。
 * type 12 线上格式（图像数据）：
 *   data[5..5+7680) = 12bit packed 像素（列优先），随后 4B CRC-32/MPEG-2；
 *   校验 CRC 后解包转置成 80x64x16bit 帧（10240B）。
 * 返回 0 + 解包后的 16bit 帧（大小 *size）。 */
static int parse_image_payload(const uint8_t *p, uint16_t plen,
                               uint8_t *img, uint16_t cap, uint16_t *size)
{
    if (plen < 4)
        return -1;
    uint8_t cmd0 = p[0] >> 4;
    uint16_t v19 = (uint16_t)(p[1] | (p[2] << 8));
    if (v19 < 4 || 3 + v19 > plen)
        return -1;
    const uint8_t *data = &p[3];   /* data[0..v19-1] */
    if (cmd0 != 2)
        return -1;                 /* 不是图像帧（仅 cmd0==2） */
    if (data[0] == 0xAA)
        return -1;                 /* POV 帧 - 非图像像素 */
    uint16_t ilen = (uint16_t)(v19 - 6);
    if (ilen < 2)
        return -1;
    /* 尾部校验和字节：data[v19-1]；0x88 => 跳过，否则累加和==0xAA */
    if (data[v19 - 1] != GF_NO_CHECK) {
        if (verify_crc(&data[5], ilen) != 0)
            return -1;
    }
    const uint8_t *w = &data[5];   /* 线上图像：打包像素 + crc32 */
    /* 线上格式：末 4 字节 CRC-32（crc32(payload[0..ilen-4])），
     * 半字大端存储。校验失败只告警不拒收（参考实现是拒收重等）。 */
    if (ilen >= 10 && ((ilen - 4) % 6) == 0) {
        uint32_t cw = (uint32_t)w[ilen - 3] | ((uint32_t)w[ilen - 4] << 8) |
                      ((uint32_t)w[ilen - 1] << 16) | ((uint32_t)w[ilen - 2] << 24);
        uint32_t cc = gx_crc32_mpeg2(w, ilen - 4, 0xFFFFFFFFu);
        LOG("capture: image wire CRC32 %s (wire 0x%08x calc 0x%08x)",
            cw == cc ? "OK" : "MISMATCH", cw, cc);
        unsigned int packed = ilen - 4;             /* 7680 */
        unsigned int npix = packed / 6 * 4;         /* 5120 */
        if (npix * 2 <= cap) {
            unpack_12bit_transpose(w, img, packed);
            if (size)
                *size = (uint16_t)(npix * 2);       /* 10240 */
            return 0;
        }
    }
    /* 非打包格式（其它 sensor）：原样交付 */
    if (ilen > cap)
        return -1;
    if (img)
        memcpy(img, w, ilen);
    if (size)
        *size = ilen;
    return 0;
}

/* 图像预处理（16bit -> 8bit）在 goodix_imgproc.c 实现：参数化管线
 * gx_imgproc_to8bit + SIGFM 反锐化增强，声明见 goodix_imgproc.h（goodix.h
 * 末尾 include）。 */

/* POV（手指存在）通知帧：cmd0=13 cmd1=1, data[0]=0xAA
 * （POV 等待过程中用于上报手指事件）。 */
static int is_pov_frame(const uint8_t *p, uint16_t plen)
{
    if (plen < 4)
        return 0;
    return (p[0] >> 4) == 13 && ((p[0] >> 1) & 0x7) == 1 && p[3] == 0xAA;
}

/* 设备主动发起的 TLS 重连请求：cmd0=13 cmd1=5 (0xDA).
 * 设备数据帧中据此触发 TLS 服务端重连。 */
static int is_tls_reconnect_req(const uint8_t *p, uint16_t plen)
{
    if (plen < 1)
        return 0;
    return (p[0] >> 4) == 13 && ((p[0] >> 1) & 0x7) == 5;
}

/* FDT/IRQ 推送帧：cmd0=3（设备数据 "--- cmd: fdt"）。
 * data[0:1] = IrqStatus (2=fdt-down 手指触碰,
 * 0x100=fdt-manual 采样回报, 0x200=fdt-up, 0x80/0x82=reverse, 0x800=fp reset)。
 * 返回 IrqStatus；非 FDT 帧返回 -1。 */
static int fdt_irq_status(const uint8_t *p, uint16_t plen)
{
    if (plen < 5)
        return -1;
    if ((p[0] >> 4) != 3)
        return -1;
    return p[3] | (p[4] << 8);
}

/* 处理 FDT 事件帧并顺手完成基线学习：
 * data = payload+3：[IrqStatus 2B][touchflag 2B][raw base 12B]。
 * IrqStatus=2     (手指按下) -> 学习 up 表
 * IrqStatus=0x200 (手指抬起) -> 学习 down 表
 * IrqStatus=0x100 (manual)   -> 由 gx_fdt_sample_base 自己处理 */
static void handle_fdt_frame(struct goodix_dev *d, const uint8_t *p,
                             uint16_t plen, int irq)
{
    const uint8_t *data = &p[3];
    uint16_t touchflag;
    LOG("capture: FDT event IrqStatus=0x%x%s", irq,
        irq == 2 ? " (finger down)" :
        irq == 0x200 ? " (finger up)" :
        irq == 0x100 ? " (manual sample)" : "");
    if (plen < 3 + 16)
        return;
    touchflag = (uint16_t)(data[2] | (data[3] << 8));
    if (irq == 2)
        gx_fdt_learn_up_base(d, data + 4, touchflag);
    else if (irq == 0x200)
        gx_fdt_learn_down_base(d, data + 4);
    /* 事件回调（libfprint 对接：手指按下/抬起状态上报） */
    if (d->event_cb && (irq == 2 || irq == 0x200))
        d->event_cb(d, irq == 2 ? GX_EV_FINGER_DOWN : GX_EV_FINGER_UP,
                    d->event_cb_user);
}

void gx_capture_set_event_cb(struct goodix_dev *d,
                             void (*cb)(struct goodix_dev *d, int event,
                                        void *user),
                             void *user)
{
    d->event_cb = cb;
    d->event_cb_user = user;
}

void gx_capture_cancel(struct goodix_dev *d)
{
    d->capture_stop = 1;
}

/* ---------------- 混合通道 payload 读取 ----------------
 * TLS 激活后 A0 明文帧直接分流到设备数据处理，B0 帧 ssl_read 解密后
 * 同样分流。明文帧整帧即一条 payload；TLS 解密数据是字节流，需要先
 * 攒出完整 payload（cmd+v19+data）。
 * 聚合缓冲（d->rx_acc / d->rx_acc_len）是设备状态的一部分（见 goodix.h），
 * 采集路径单线程访问，但挂在设备上保证多设备/重入安全。 */

/* 从聚合缓冲取一条完整 payload；取到返回 1 */
static int acc_take(struct goodix_dev *d, uint8_t *out, uint16_t *outlen,
                    uint16_t cap)
{
    if (d->rx_acc_len < 3)
        return 0;
    uint16_t v19 = (uint16_t)(d->rx_acc[1] | (d->rx_acc[2] << 8));
    size_t total = 3 + v19;
    if (v19 < 4 || total > sizeof(d->rx_acc)) {
        LOG("capture: TLS payload desync (v19=%u), resync", v19);
        d->rx_acc_len = 0;
        return 0;
    }
    if (d->rx_acc_len < total)
        return 0;
    if (total <= cap) {
        memcpy(out, d->rx_acc, total);
        *outlen = (uint16_t)total;
    }
    memmove(d->rx_acc, d->rx_acc + total, d->rx_acc_len - total);
    d->rx_acc_len -= total;
    return total <= cap;
}

/* 读下一条设备数据 payload。
 * 明文 A0/C0 帧：payload = frame[4..flen) 直接返回；
 * B0 帧：注入 TLS（gx_tls_feed），取出解密字节流进聚合缓冲。
 * 返回 1 = 拿到 payload；0 = 超时；-1 = TLS 通道错误。 */
static int next_payload(struct goodix_dev *d, uint8_t *out, uint16_t *outlen,
                        uint16_t cap, int tmo_ms)
{
    uint8_t *frame = d->rx_frame;   /* 设备级帧重组缓冲（见 goodix.h） */
    int tls = d->tls_inited && d->tls;
    int deadline = tmo_ms > 0 ? tmo_ms : 200;

    while (deadline > 0) {
        if (acc_take(d, out, outlen, cap))
            return 1;
        uint16_t flen = 0;
        int r = gx_read_frame(d, frame, &flen, 200);
        if (r < 0) {
            deadline -= 200;
            continue;
        }
        if (flen < 5)
            continue;
        if (tls && frame[0] == GF_TYPE_TLS) {
            if (gx_tls_feed(d, &frame[4], (size_t)(flen - 4)) < 0)
                return -1;
            for (;;) {
                uint8_t tmp[2048];
                int n = gx_tls_recv(d, tmp, sizeof(tmp), 0);
                if (n <= 0)
                    break;          /* WANT_READ / 暂无更多 */
                if (d->rx_acc_len + (size_t)n > sizeof(d->rx_acc)) {
                    d->rx_acc_len = 0; /* 溢出：失步重置 */
                    break;
                }
                memcpy(d->rx_acc + d->rx_acc_len, tmp, (size_t)n);
                d->rx_acc_len += (size_t)n;
            }
            continue;
        }
        /* 明文帧 */
        if (gx_debug) {
            fprintf(stderr, "[goodix] capture: frame %uB cmd=0x%02x dlen=%u [",
                    flen, frame[4], frame[5] | (frame[6] << 8));
            for (uint16_t i = 7; i < flen && i < 7 + 16; i++)
                fprintf(stderr, "%02X ", frame[i]);
            fprintf(stderr, "]\n");
        }
        if (flen - 4 > cap)
            continue;
        memcpy(out, &frame[4], (size_t)(flen - 4));
        *outlen = (uint16_t)(flen - 4);
        return 1;
    }
    return 0;
}

/* 分流一条 payload（POV/FDT/TLS-reconnect/图像）。
 * FDT 手指按下事件后，按参考实现行为立即发
 * SetMode Image (0x20 [1,0]) 让 MCU 曝光推图（img_req 保证只发一次）。
 * 返回 1 = 拿到图像。 */
static int dispatch_payload(struct goodix_dev *d, const uint8_t *pl,
                            uint16_t plen, uint8_t *img, uint16_t *size,
                            int *img_req)
{
    if (is_tls_reconnect_req(pl, plen)) {
        /* 设备数据 cmd0==13 cmd1==5 -> 触发 TLS 服务端重连 */
        LOG("capture: device requested TLS reconnect (0xDA)");
        if (d->tls) {
            gx_tls_capture_mode(d, 0);   /* 握手期 bio 要自己读 USB */
            int r = gx_tls_reconnect(d);
            gx_tls_capture_mode(d, 1);
            d->rx_acc_len = 0;              /* 旧会话聚合数据作废 */
            if (r < 0)
                return -1;
        }
        return 0;
    }
    if (is_pov_frame(pl, plen)) {
        LOG("capture: POV frame - finger present");
        return 0;
    }
    {
        int irq = fdt_irq_status(pl, plen);
        if (irq >= 0) {
            /* FDT 事件：0x2=手指按下，0x200=抬起；图像帧随后推送 */
            handle_fdt_frame(d, pl, plen, irq);
            /* FDT down 事件 -> 请求取图 ->
             * SetMode Image (cmd0=2 cmd1=0 = 0x20, data=[1,0])，
             * MCU 随即曝光并推 cmd0=2 图像帧 */
            if (irq == 2 && img_req && !*img_req) {
                uint8_t im[2] = { 1, 0 };
                *img_req = 1;
                LOG("capture: finger down, request image "
                    "(SetMode Image 0x20 [1,0])");
                gx_send_cmd(d, 0x20, im, 2, 200);
            }
            return 0;
        }
    }
    if (parse_image_payload(pl, plen, img, GF_IMG_MAX, size) == 0)
        return 1;
    /* 其它帧（命令/ACK）忽略 */
    return 0;
}

/* 等设备推送 cmd0=2 图像帧（混合通道） */
static int wait_image(struct goodix_dev *d, uint8_t *img, uint16_t *size,
                      int timeout_ms)
{
    uint8_t *pl = d->rx_scratch;    /* 设备级 payload 工作区 */
    int deadline = timeout_ms > 0 ? timeout_ms : 10000;
    int tls = d->tls_inited && d->tls;
    int img_req = 0;

    d->rx_acc_len = 0;
    if (tls)
        gx_tls_capture_mode(d, 1);
    while (deadline > 0) {
        if (d->capture_stop) {
            if (tls)
                gx_tls_capture_mode(d, 0);
            return -4;
        }
        uint16_t plen = sizeof(d->rx_scratch);
        int r = next_payload(d, pl, &plen, sizeof(d->rx_scratch), 200);
        if (r < 0) {
            if (tls)
                gx_tls_capture_mode(d, 0);
            return -3;
        }
        if (r == 0) {
            deadline -= 200;
            continue;
        }
        int dr = dispatch_payload(d, pl, plen, img, size, &img_req);
        if (dr < 0) {
            if (tls)
                gx_tls_capture_mode(d, 0);
            return -3;
        }
        if (dr == 1) {
            if (tls)
                gx_tls_capture_mode(d, 0);
            return 0;
        }
    }
    if (tls)
        gx_tls_capture_mode(d, 0);
    return -2;
}

/* 基线采样（FDT-manual）：SetMode(3,3,1) FDT-manual ->
 * cmd0=3 cmd1=3 (0x36)，data=[9, 1, 当前 down 表 12B]（首跑全 0，同
 * 参考实现初值）。MCU 现场采样无手指基线并推 cmd0=3
 * IrqStatus=0x100 帧：data=[irq 2B][touchflag 2B][raw 12B]。
 * 学习为 down 阈值表，成功后 base_valid=true。
 * 参考实现 init 期（基线采样，TLS 握手之后）做这件事；
 * 采不到/校验失败重试 3 次。 */
int gx_fdt_sample_base(struct goodix_dev *d)
{
    uint8_t *pl = d->rx_scratch;    /* 设备级 payload 工作区 */
    int tls = d->tls_inited && d->tls;

    if (tls)
        gx_tls_capture_mode(d, 1);
    d->rx_acc_len = 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t req[14];
        req[0] = 9;                     /* FDT 手动采样 */
        req[1] = 1;                     /* a3=1：携带当前 down 表 */
        memcpy(&req[2], d->fdt_down_base, 12);
        LOG("FDT manual sample try %d (0x36 [9,1,table12])", attempt + 1);
        if (gx_send_cmd(d, 0x36, req, sizeof(req), 200) < 0) {
            LOG("FDT manual cmd send failed");
            continue;
        }
        int deadline = 1500;
        while (deadline > 0) {
            uint16_t plen = sizeof(d->rx_scratch);
            int r = next_payload(d, pl, &plen, sizeof(d->rx_scratch), 200);
            if (r <= 0) {
                deadline -= 200;
                continue;
            }
            int irq = fdt_irq_status(pl, plen);
            if (irq < 0)
                continue;               /* ACK/其它命令响应帧 */
            if (irq == 0x100 && plen >= 3 + 17) {
                /* [cmd][v19][irq 2B][touchflag 2B][raw 12B][cks] */
                const uint8_t *raw = &pl[3 + 4];
                if (gx_fdt_learn_down_base(d, raw) == 0) {
                    d->base_valid = true;
                    d->base_dirty = true;
                    LOG("FDT base sampled ok (IrqStatus=0x100)");
                    if (tls)
                        gx_tls_capture_mode(d, 0);
                    return 0;
                }
                LOG("FDT sample invalid, retry");
                break;
            }
            /* 采样期间检测到手指（down/up 事件）：本轮作废重采
             *（参考实现用 delta 比对两次采样判断是否沾手指） */
            if (irq == 2 || irq == 0x200) {
                LOG("FDT sample disturbed by finger (irq 0x%x), retry", irq);
                break;
            }
        }
    }
    if (tls)
        gx_tls_capture_mode(d, 0);
    return -1;
}

/* FDT down 布防（采集触发链路 = cmd0=3 cmd1=1 = 0x32）。
 * 参考实现恒 a3=1：data=[8,1,12B down表,ts16] 共 16B
 * （基线采样已在 init 期采好表）。
 * 仅在连采样基线都没有时退回 a3=0 的 4B 形态 [8,0,tbl0,tbl1]
 * （基线存在性检查的兜底分支；
 *  tbl0/tbl1 是当前表的前两字节——无表时即全 0）。 */
static void arm_fdt_down(struct goodix_dev *d)
{
    if (d->base_valid) {
        uint8_t fdt[16] = { 0 };
        struct timeval tv;
        fdt[0] = 8;
        fdt[1] = 1;                 /* a3=1：携带阈值表 */
        memcpy(&fdt[2], d->fdt_down_base, 12);
        gettimeofday(&tv, NULL);
        uint16_t ts = (uint16_t)(tv.tv_sec % 60 * 1000 + tv.tv_usec / 1000);
        fdt[14] = (uint8_t)ts;
        fdt[15] = (uint8_t)(ts >> 8);
        LOG("capture: arming finger detect (FDT down with base, 0x32)");
        gx_send_cmd(d, 0x32, fdt, sizeof(fdt), 200);
    } else {
        uint8_t fdt[4] = { 8, 0, d->fdt_down_base[0],
                           d->fdt_down_base[1] };
        LOG("capture: arming finger detect "
            "(FDT down default-config, 0x32)");
        gx_send_cmd(d, 0x32, fdt, sizeof(fdt), 200);
    }
}

/* 基线采样的图像基线段：SetMode Image（cmd0=2 cmd1=0 = 0x20,
 * data=[1,0]）直接曝光取一帧无手指图像（init 期 / 温度漂移重建时用，
 * 传感器上必须没有手指）。 */
int gx_capture_base_image(struct goodix_dev *d)
{
    uint8_t im[2] = { 1, 0 };
    uint16_t sz = 0;
    LOG("capture base image (SetMode Image 0x20 [1,0], no-finger frame)");
    gx_send_cmd(d, 0x20, im, 2, 200);
    if (wait_image(d, d->img_base, &sz, 4000) != 0 ||
        sz != d->img_size) {
        LOG("base image capture failed (sz=%u)", sz);
        return -1;
    }
    d->img_base_valid = true;
    d->base_dirty = true;
    LOG("base image captured (%u B) - goodix.dat imagebase", sz);
    return 0;
}

/* 等设备推送 FDT up（IrqStatus=0x200）帧：gx_capture 收尾的 0x34
 * 布防之后，手指离开传感器时 MCU 会推该事件（设备数据的
 * cmd0=3 分流）。libfprint 驱动用它上报 FP_FINGER_NONE。 */
int gx_wait_finger_up(struct goodix_dev *d, int timeout_ms)
{
    uint8_t *pl = d->rx_scratch;    /* 设备级 payload 工作区 */
    int deadline = timeout_ms > 0 ? timeout_ms : 10000;
    int tls = d->tls_inited && d->tls;
    int ret = -2;

    d->rx_acc_len = 0;
    if (tls)
        gx_tls_capture_mode(d, 1);
    while (deadline > 0) {
        if (d->capture_stop) {
            ret = -4;
            break;
        }
        uint16_t plen = sizeof(d->rx_scratch);
        int r = next_payload(d, pl, &plen, sizeof(d->rx_scratch), 200);
        if (r < 0) {
            ret = -3;
            break;
        }
        if (r == 0) {
            deadline -= 200;
            continue;
        }
        int irq = fdt_irq_status(pl, plen);
        if (irq < 0)
            continue;               /* ACK/其它帧 */
        handle_fdt_frame(d, pl, plen, irq);
        if (irq == 0x200 || irq == 2) {
            /* 0x200=抬起；2=抬起后又按下（边缘抖动）：都视为本轮结束 */
            LOG("capture: finger up (irq 0x%x)", irq);
            ret = 0;
            break;
        }
    }
    if (tls)
        gx_tls_capture_mode(d, 0);
    return ret;
}

/* gx_capture：取图 + POV 唤醒路径的实现。
 *
 * 采集流程（取图/POV/普通路径）：
 *   1. 取图开头：若 TLS 项目标志置位且 (MCU state bit1
 *      isTlsConnected==0 或 host TLS 未就绪) -> 触发 TLS 服务端重连。
 *   2. POV 路径只在 MCU state bit0 (isPOVImageValid)==1 时走：
 *      发 0xD2 {0,0}（POV 唤醒命令），等 POV 帧
 *      (cmd0=13 cmd1=1 data[0]=0xAA)；bit0==0 则不发任何命令。
 *   3. 普通路径：触发采集命令 = FDT down（0x32，16B 形态
 *      [8,1,12B down表,ts16]）。之后设备在手指触碰时先推 cmd0=3 FDT
 *      事件帧（顺带学 up 基线），host 发 SetMode Image 取图。
 *   4. 收到图像后主机侧判别：1=finger 交付；2=void/3=bad 重新布防 FDT DOWN；
 *      0=temperature 则重建基线（重建 FDT/图像基线后再布防）。
 *   5. 调试用 GOODIX_CAPTURE_IMAGE_MODE=1：跳过 FDT，直接
 *      SetMode Image（0x20 [1,0]），首帧即收。
 * 必须用手指触摸传感器才能产生 FDT 驱动的图像帧。
 */
int gx_capture(struct goodix_dev *d, uint8_t *img, uint16_t *size)
{
    int r;
    int pov_valid = 0;

    d->capture_stop = 0;            /* 每次采集开始清除取消标志 */

    /* step 1: 获取 MCU 状态（取图前置检查）。
     * state[1] bit0 = isPOVImageValid（POV 条件）,
     *          bit1 = isTlsConnected（TLS 连接状态）,
     *          bit3 = isLocked. */
    {
        uint8_t st[16] = { 0 };
        int sr = gx_get_mcu_state(d, st);
        if (sr == 0) {
            LOG("MCU state: %02X %02X %02X %02X %02X %02X %02X %02X "
                "(POV_valid=%d tls_conn=%d locked=%d)",
                st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7],
                (st[1] & 1) ? 1 : 0, (st[1] & 2) ? 1 : 0,
                (st[1] & 8) ? 1 : 0);
            pov_valid = st[1] & 1;
            /* 取图前置的 TLS 活性检查：host 认为 TLS 已建立但 MCU
             * 报 isTlsConnected==0 -> 触发 TLS 服务端重连 */
            if (d->tls_inited && !(st[1] & 0x02)) {
                LOG("MCU reports TLS not connected, reconnecting");
                gx_tls_reconnect(d);
            }
        } else {
            LOG("GetMcuState failed (%d)", sr);
        }
    }

    /* step 2: POV 唤醒 - 仅当 MCU 报 isPOVImageValid（POV 条件
     * state[1]&1）；否则走普通路径发 FDT down 布防手指检测。 */
    if (pov_valid) {
        LOG("capture: POV image valid, WakeupMCU (0xD2 {0,0})");
        uint8_t wake[2] = { 0, 0 };
        gx_send_cmd(d, 0xD2, wake, 2, 200);   /* evt=-1, 等 ACK 由读循环跳过 */
        usleep(300000);                        /* 让 MCU 进入检测状态 */
    } else if (getenv("GOODIX_CAPTURE_IMAGE_MODE")) {
        /* 调试旁路：直接 SetMode Image（cmd0=2 cmd1=0 = 0x20,
         * data=[1,0]；设置模式 "setmode: Image"）。
         * MCU 立即曝光推一帧图像，不需要 FDT 触发。 */
        uint8_t im[2] = { 1, 0 };
        LOG("capture: SetMode Image (0x20 [1,0]) - direct exposure");
        gx_send_cmd(d, 0x20, im, 2, 200);
    } else {
        arm_fdt_down(d);
    }

    LOG("capture: waiting for device-pushed image frame (tls=%d)",
        d->tls_inited ? 1 : 0);
    LOG("capture: PLACE YOUR FINGER ON THE SENSOR NOW");

    /* step 3: 等设备推送 cmd0=2 图像帧，拿到后做主机侧手指判别：
     *  1 finger      -> 接受交付
     *  2 void/3 bad  -> "not finger nor temp: to FDT DOWN" 重新布防
     *  0 temperature -> 温度漂移：重建 FDT+图像基线再布防
     * 直曝调试模式 / 无基线图像：保持旧行为，首帧即收。 */
    r = -2;
    {
        int direct = getenv("GOODIX_CAPTURE_IMAGE_MODE") != NULL;
        int rejects = 0;
        for (;;) {
            r = wait_image(d, img, size, 10000);
            if (r != 0)
                break;
            if (direct || !d->img_base_valid)
                break;
            int v = gx_fdt_check_finger(d, img);
            if (v == 1 || v == -1)
                break;
            if (++rejects > 4) {
                LOG("capture: too many rejected frames, giving up");
                r = -2;
                break;
            }
            if (v == 0) {
                /* 温度漂移处理：重建基线 -> FDT DOWN */
                LOG("capture: temperature drift, rebuild all base");
                gx_fdt_sample_base(d);
                if (gx_capture_base_image(d) == 0)
                    gx_base_save(d);
            } else {
                LOG("capture: %s frame rejected, re-arm FDT down",
                    v == 2 ? "void" : "bad");
            }
            arm_fdt_down(d);
        }
    }

    if (r == 0) {
        d->have_image = true;
        LOG("captured %u bytes", size ? *size : 0);
        /* 取图收尾："to get upbase and to set FDT UP" ->
         * SetMode(3,2,1) = cmd (3<<4)|(2<<1) = 0x34，
         * data=[10, 1, fdt_up_base 12B]，布防手指抬起检测（为下次采集
         * 循环做准备；下次 capture 会重新布防 down）。 */
        {
            uint8_t fup[14];
            fup[0] = 10;
            fup[1] = 1;
            memcpy(&fup[2], d->fdt_up_base, 12);
            gx_send_cmd(d, 0x34, fup, sizeof(fup), 200);
        }
        /* 基线落盘：采到图且运行中学到了新基线 -> 落盘 */
        if (d->base_dirty)
            gx_base_save(d);
    } else {
        LOG("no image frame received (finger not detected?)");
    }
    return r;
}
