/* goodix_imgproc.c - 图像预处理参数化管线（16bit 帧 -> 8bit 灰度）
 *
 * 各环节可经 struct gx_imgproc_params / GOODIX_IMGPROC_* 环境变量调整，
 * 用于真机标定。
 *
 * 管线（每帧调用，无 static 缓冲，可重入；dump 序号例外，仅调试用）：
 *   1) baseline  可选减无手指基线：v = offset + raw - base，clamp [0,4095]。
 *      2048 余量提供对称空间，避免半波整流/饱和；基线无效时直接用原帧。
 *   2) flatfield 可选去低频背景：可分离盒式均值（半径远大于脊距，只移除
 *      接触压力造成的缓慢亮度梯度），v = clamp(v - lowf + mean_all, 0, 4095)，
 *      同步累计直方图（mean_all 为平场前的均值，与原实现运算顺序一致）。
 *   3) stretch   pct_lo/pct_hi 百分位截断映射到 0..255（hi<=lo 回退全量程）。
 *   4) enhance   可选反锐化局部对比增强（SIGFM/SIFT 定向）：对 8bit 输出
 *      out = clamp(in + boost*(in - gauss(in,σ)))。SIGFM 的 sigfm_extract()
 *      用 SIFT 默认 contrastThreshold=0.04，弱脊/轻触下关键点常低于 fork 的
 *      25 个下限——本环节提升脊谷局部对比度，可提高关键点通过率。
 *      反锐化同时放大噪点，默认关闭（GX_IMGPROC_ENHANCE_NONE），
 *      靠 SIGFM 的几何一致性投票过滤假关键点。
 *
 * 调试：GOODIX_DUMP_IMGPROC=1 时把各阶段写到 gx_state_dir() 下
 *   imgproc-<seq>-<stage>.pgm（raw8/base8/flat8/final8/enh8），
 *   16bit 中间态按 v>>4 转 8bit 输出。
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "goodix.h"

#define IMGPROC_PX_MAX   4095            /* 12bit 像素有效上界 */
#define IMGPROC_CLAMP16(v) ((v) < 0 ? 0 : (v) > IMGPROC_PX_MAX ? IMGPROC_PX_MAX : (v))

static inline uint16_t imgproc_px16(const uint8_t *img, uint32_t idx)
{
    return (uint16_t)(img[2 * idx] | (img[2 * idx + 1] << 8));
}

/* ---- 可分离盒式均值（水平+垂直一维前缀和，半径 r，边界复制）----
 * 用于低频亮度场估计。原实现用 static 缓冲且按 GF_IMG_MAX/2 硬编码尺寸，
 * 这里改为每次调用按实际 w*h 动态分配，保证可重入。返回 0 成功、-1 分配失败。 */
static int boxmean(const uint16_t *in, uint16_t *out,
                   uint32_t w, uint32_t h, int r)
{
    uint32_t npix = w * h;
    uint32_t pslen = (w > h ? w : h) + 1;
    uint32_t *tmp = malloc((size_t)npix * sizeof(uint32_t));
    uint32_t *ps = malloc((size_t)pslen * sizeof(uint32_t));
    uint32_t x, y;

    if (!tmp || !ps) {
        free(tmp);
        free(ps);
        return -1;
    }
    /* 水平 pass */
    for (y = 0; y < h; y++) {
        const uint16_t *row = &in[y * w];
        ps[0] = 0;
        for (x = 0; x < w; x++)
            ps[x + 1] = ps[x] + row[x];
        for (x = 0; x < w; x++) {
            uint32_t x0 = x > (uint32_t)r ? x - r : 0;
            uint32_t x1 = x + r < w ? x + r : w - 1;
            tmp[y * w + x] = (ps[x1 + 1] - ps[x0]) / (x1 - x0 + 1);
        }
    }
    /* 垂直 pass */
    for (x = 0; x < w; x++) {
        ps[0] = 0;
        for (y = 0; y < h; y++)
            ps[y + 1] = ps[y] + tmp[y * w + x];
        for (y = 0; y < h; y++) {
            uint32_t y0 = y > (uint32_t)r ? y - r : 0;
            uint32_t y1 = y + r < h ? y + r : h - 1;
            out[y * w + x] = (uint16_t)((ps[y1 + 1] - ps[y0]) / (y1 - y0 + 1));
        }
    }
    free(tmp);
    free(ps);
    return 0;
}

/* ---- stage 1: 有符号基线相减 ----
 * use_baseline 且 d->img_base_valid 时 v = offset + raw - base（clamp）；
 * 否则直接用原帧（等价原实现的无基线分支）。 */
static void stage_baseline(const struct goodix_dev *d, const uint8_t *img16,
                           uint16_t *buf, int use_baseline, uint16_t offset)
{
    uint32_t npix = (uint32_t)d->img_w * d->img_h;
    for (uint32_t i = 0; i < npix; i++) {
        uint16_t r = imgproc_px16(img16, i);
        if (use_baseline && d->img_base_valid) {
            uint16_t b = imgproc_px16(d->img_base, i);
            int v = (int)offset + (int)r - (int)b;
            buf[i] = (uint16_t)IMGPROC_CLAMP16(v);
        } else {
            buf[i] = r;
        }
    }
}

/* ---- stage 2: 低频平场校正 + 直方图累计 ----
 * mean_all 来自平场前的 buf 均值（与原实现顺序一致）；clamp 后 buf[i]
 * 必在 [0,4095]，直方图索引无需掩码。返回 0 成功、-1 分配失败（调用方
 * 回退为"无平场"直方图路径，输出仍合法）。 */
static int stage_flatfield(uint16_t *buf, uint32_t w, uint32_t h, int r,
                           uint32_t *hist)
{
    uint32_t npix = w * h;
    uint16_t *lowf = malloc((size_t)npix * sizeof(uint16_t));
    uint64_t sum = 0;
    uint32_t mean_all;

    if (!lowf)
        return -1;
    if (boxmean(buf, lowf, w, h, r) < 0) {
        free(lowf);
        return -1;
    }
    for (uint32_t i = 0; i < npix; i++)
        sum += buf[i];
    mean_all = (uint32_t)(sum / npix);
    for (uint32_t i = 0; i < npix; i++) {
        int v = (int)buf[i] - (int)lowf[i] + (int)mean_all;
        buf[i] = (uint16_t)IMGPROC_CLAMP16(v);
        hist[buf[i]]++;
    }
    free(lowf);
    return 0;
}

/* ---- stage 3: 百分位截断 -> 0..255 ----
 * pct_lo/pct_hi 定义保留区间 [pct_lo%, pct_hi%]：底部截断 pct_lo%、
 * 顶部截断 (100-pct_hi)%。原实现向下扫描用 npix/100（顶部累计 1%），
 * 即默认 pct_hi=99 等价顶部截断 1%。 */
static void stage_stretch(const uint16_t *buf, uint8_t *out8,
                          uint32_t npix, const uint32_t *hist,
                          uint8_t pct_lo, uint8_t pct_hi)
{
    uint32_t lo = 0, hi = IMGPROC_PX_MAX;
    uint32_t n_lo = (uint32_t)((uint64_t)npix * pct_lo / 100);
    uint32_t n_hi = (uint32_t)((uint64_t)npix * (100 - pct_hi) / 100);
    uint32_t acc = 0;

    for (int v = 0; v <= IMGPROC_PX_MAX; v++) {
        acc += hist[v];
        if (acc > n_lo) {
            lo = (uint32_t)v;
            break;
        }
    }
    acc = 0;
    for (int v = IMGPROC_PX_MAX; v >= 0; v--) {
        acc += hist[v];
        if (acc > n_hi) {
            hi = (uint32_t)v;
            break;
        }
    }
    if (hi <= lo) {
        lo = 0;
        hi = IMGPROC_PX_MAX;
    }

    for (uint32_t i = 0; i < npix; i++) {
        uint32_t v = buf[i];
        if (v <= lo)
            out8[i] = 0;
        else if (v >= hi)
            out8[i] = 255;
        else
            out8[i] = (uint8_t)((v - lo) * 255 / (hi - lo));
    }
}

/* ---- stage 4: 可选反锐化（SIGFM 定向，8bit 域）----
 * out = clamp(in + boost*(in - gauss(in,σ)))，可分离高斯、边界复制。 */
static void gaussian_blur_u8(const uint8_t *in, uint8_t *out,
                             uint32_t w, uint32_t h, float sigma)
{
    int r = (int)ceilf(3.0f * sigma);
    int klen;
    float *k;
    uint8_t *tmp;
    float ksum = 0.0f;
    uint32_t x, y;

    if (r < 1)
        r = 1;
    klen = 2 * r + 1;
    k = malloc((size_t)klen * sizeof(float));
    tmp = malloc((size_t)w * h);
    if (!k || !tmp) {
        free(k);
        free(tmp);
        memcpy(out, in, (size_t)w * h);
        return;
    }
    for (int i = -r; i <= r; i++) {
        float t = (float)i;
        k[i + r] = expf(-(t * t) / (2.0f * sigma * sigma));
        ksum += k[i + r];
    }
    for (int i = 0; i < klen; i++)
        k[i] /= ksum;

    /* 水平 pass（边界复制） */
    for (y = 0; y < h; y++) {
        const uint8_t *row = &in[y * w];
        for (x = 0; x < w; x++) {
            float acc = 0.0f;
            for (int j = -r; j <= r; j++) {
                int sx = (int)x + j;
                if (sx < 0)
                    sx = 0;
                else if ((uint32_t)sx >= w)
                    sx = (int)w - 1;
                acc += k[j + r] * row[sx];
            }
            tmp[y * w + x] = (uint8_t)(acc + 0.5f);
        }
    }
    /* 垂直 pass */
    for (x = 0; x < w; x++) {
        for (y = 0; y < h; y++) {
            float acc = 0.0f;
            for (int j = -r; j <= r; j++) {
                int sy = (int)y + j;
                if (sy < 0)
                    sy = 0;
                else if ((uint32_t)sy >= h)
                    sy = (int)h - 1;
                acc += k[j + r] * tmp[sy * w + x];
            }
            out[y * w + x] = (uint8_t)(acc + 0.5f);
        }
    }
    free(k);
    free(tmp);
}

static void stage_enhance(const uint8_t *in, uint8_t *out,
                          uint32_t w, uint32_t h, float boost, float sigma)
{
    uint8_t *blur = malloc((size_t)w * h);
    if (!blur) {
        memcpy(out, in, (size_t)w * h);
        return;
    }
    gaussian_blur_u8(in, blur, w, h, sigma);
    for (uint32_t i = 0; i < w * h; i++) {
        int d = (int)in[i] - (int)blur[i];
        int v = (int)in[i] + (int)lroundf(boost * (float)d);
        out[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    free(blur);
}

/* ================= GX_IMGPROC_MODE_RIDGE =================
 * 背景：本传感器的 (raw - base) 除了脊谷信号外还带一个固定于像素网格的
 * 残影（逐像素/逐列增益 x 手指整体压低量，及基线漂移），弱按压时它在
 * 百分位拉伸后占满整幅图，SIFT 关键点全落在残影上 -> 不同手指互相匹配。
 * 本管线全程浮点（不做 4095 截断），用 DoG 带通只保留脊距频段
 * （σ1≈1 去像素噪声，σ2≈4 去残影/压力的低频分量），可选按基线形状回归
 * 去除增益残影，再拉伸 + CLAHE 均衡局部对比度。参数与离线标定
 * （tools/fp_tune）的 OpenCV/numpy 参考实现一致：高斯核长 round(8σ+1)|1、
 * 边界 reflect-101、百分位线性插值、CLAHE 同 OpenCV 算法。 */

static const uint8_t *select_base(const struct goodix_dev *d,
                                  const struct gx_imgproc_params *p)
{
    if (p->fresh_base && d->img_fresh_valid)
        return d->img_fresh;
    return d->img_base_valid ? d->img_base : NULL;
}

/* reflect-101 边界索引（OpenCV BORDER_DEFAULT），要求 |越界| < n */
static inline int reflect101(int i, int n)
{
    if (i < 0)
        return -i;
    if (i >= n)
        return 2 * n - 2 - i;
    return i;
}

/* 可分离高斯（浮点），核同 cv::getGaussianKernel(round(8σ+1)|1, σ)。
 * 返回 0 成功、-1 分配失败（out 保持不变）。 */
static int gauss_f(const float *in, float *out, int w, int h, float sigma)
{
    int ks = ((int)lroundf(sigma * 8.0f + 1.0f)) | 1;
    int r = ks / 2;
    float *k, *tmp;
    float ksum = 0.0f;

    if (r >= w || r >= h)
        r = (w < h ? w : h) - 1;
    ks = 2 * r + 1;
    k = malloc((size_t)ks * sizeof(float));
    tmp = malloc((size_t)w * h * sizeof(float));
    if (!k || !tmp) {
        free(k);
        free(tmp);
        return -1;
    }
    for (int i = 0; i < ks; i++) {
        float t = (float)(i - r);
        k[i] = expf(-(t * t) / (2.0f * sigma * sigma));
        ksum += k[i];
    }
    for (int i = 0; i < ks; i++)
        k[i] /= ksum;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc = 0.0f;
            for (int j = -r; j <= r; j++)
                acc += k[j + r] * in[y * w + reflect101(x + j, w)];
            tmp[y * w + x] = acc;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc = 0.0f;
            for (int j = -r; j <= r; j++)
                acc += k[j + r] * tmp[reflect101(y + j, h) * w + x];
            out[y * w + x] = acc;
        }
    free(k);
    free(tmp);
    return 0;
}

/* d = raw - base（浮点）；deghost 时减去 d 在 (base - mean(base)) 上的
 * 最小二乘投影：增益残影与基线逐像素形状同源（手指整体压低量 x 增益）。 */
static void ridge_diff(const uint8_t *img16, const uint8_t *base,
                       uint32_t npix, int deghost, float *dst)
{
    double bmean = 0.0, num = 0.0, den = 0.0, dmean = 0.0;

    for (uint32_t i = 0; i < npix; i++) {
        dst[i] = (float)imgproc_px16(img16, i) - (float)imgproc_px16(base, i);
        dmean += dst[i];
        bmean += imgproc_px16(base, i);
    }
    if (!deghost)
        return;
    dmean /= npix;
    bmean /= npix;
    for (uint32_t i = 0; i < npix; i++) {
        double rb = imgproc_px16(base, i) - bmean;
        num += (dst[i] - dmean) * rb;
        den += rb * rb;
    }
    if (den <= 0.0)
        return;
    for (uint32_t i = 0; i < npix; i++)
        dst[i] -= (float)(num / den * (imgproc_px16(base, i) - bmean));
}

/* DoG 带通：dst = G(σ1) - G(σ2)，原地可用（dst == src）。 */
static int ridge_dog(const float *src, float *dst, int w, int h,
                     float s1, float s2)
{
    uint32_t npix = (uint32_t)w * h;
    float *g1 = malloc(npix * sizeof(float));
    float *g2 = malloc(npix * sizeof(float));
    int r = -1;

    if (g1 && g2 && gauss_f(src, g1, w, h, s1) == 0 &&
        gauss_f(src, g2, w, h, s2) == 0) {
        for (uint32_t i = 0; i < npix; i++)
            dst[i] = g1[i] - g2[i];
        r = 0;
    }
    free(g1);
    free(g2);
    return r;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* numpy.percentile（linear）于已排序数组 */
static float pct_sorted(const float *v, uint32_t n, float q)
{
    float pos = (float)(n - 1) * q / 100.0f;
    uint32_t i = (uint32_t)pos;
    float fr = pos - (float)i;
    if (i + 1 >= n)
        return v[n - 1];
    return v[i] + fr * (v[i + 1] - v[i]);
}

/* 百分位拉伸到 0..255（截断取整，同 numpy astype(uint8)） */
static int ridge_stretch(const float *src, uint8_t *out8, uint32_t npix,
                         uint8_t pct_lo, uint8_t pct_hi)
{
    float *s = malloc(npix * sizeof(float));
    float lo, hi, sc;

    if (!s)
        return -1;
    memcpy(s, src, npix * sizeof(float));
    qsort(s, npix, sizeof(float), cmp_float);
    lo = pct_sorted(s, npix, pct_lo);
    hi = pct_sorted(s, npix, pct_hi);
    free(s);
    if (hi - lo < 1e-3f)
        hi = lo + 1e-3f;
    sc = 255.0f / (hi - lo);
    for (uint32_t i = 0; i < npix; i++) {
        float v = (src[i] - lo) * sc;
        out8[i] = (uint8_t)(v <= 0.0f ? 0 : v >= 255.0f ? 255 : (int)v);
    }
    return 0;
}

/* CLAHE（OpenCV 算法：分块直方图裁剪+均匀再分配、LUT、双线性插值）。
 * 要求 w、h 可被 tiles 整除（80x64 / 4 成立），否则不处理。原地。 */
static void ridge_clahe(uint8_t *img, int w, int h, float clip, int tiles)
{
    int tw, th, total, limit;
    float lut_scale, inv_tw, inv_th;
    uint8_t *lut;

    if (clip <= 0.0f || tiles < 1 || w % tiles || h % tiles)
        return;
    tw = w / tiles;
    th = h / tiles;
    total = tw * th;
    lut_scale = 255.0f / (float)total;
    limit = (int)(clip * total / 256);
    if (limit < 1)
        limit = 1;
    lut = malloc((size_t)tiles * tiles * 256);
    if (!lut)
        return;
    for (int ty = 0; ty < tiles; ty++)
        for (int tx = 0; tx < tiles; tx++) {
            int hist[256] = { 0 };
            int clipped = 0, batch, residual, sum = 0;
            uint8_t *l = &lut[(ty * tiles + tx) * 256];

            for (int y = ty * th; y < (ty + 1) * th; y++)
                for (int x = tx * tw; x < (tx + 1) * tw; x++)
                    hist[img[y * w + x]]++;
            for (int i = 0; i < 256; i++)
                if (hist[i] > limit) {
                    clipped += hist[i] - limit;
                    hist[i] = limit;
                }
            batch = clipped / 256;
            residual = clipped - batch * 256;
            for (int i = 0; i < 256; i++)
                hist[i] += batch;
            if (residual) {
                int step = 256 / residual > 1 ? 256 / residual : 1;
                for (int i = 0; i < 256 && residual > 0; i += step, residual--)
                    hist[i]++;
            }
            for (int i = 0; i < 256; i++) {
                int v;
                sum += hist[i];
                v = (int)lrintf((float)sum * lut_scale);
                l[i] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
    inv_tw = 1.0f / (float)tw;
    inv_th = 1.0f / (float)th;
    for (int y = 0; y < h; y++) {
        float tyf = (float)y * inv_th - 0.5f;
        int ty1 = (int)floorf(tyf), ty2 = ty1 + 1;
        float ya = tyf - (float)ty1;
        ty1 = ty1 < 0 ? 0 : ty1;
        ty2 = ty2 > tiles - 1 ? tiles - 1 : ty2;
        for (int x = 0; x < w; x++) {
            float txf = (float)x * inv_tw - 0.5f;
            int tx1 = (int)floorf(txf), tx2 = tx1 + 1;
            float xa = txf - (float)tx1;
            uint8_t s = img[y * w + x];
            float res;
            int v;
            tx1 = tx1 < 0 ? 0 : tx1;
            tx2 = tx2 > tiles - 1 ? tiles - 1 : tx2;
            res = ((float)lut[(ty1 * tiles + tx1) * 256 + s] * (1.0f - xa) +
                   (float)lut[(ty1 * tiles + tx2) * 256 + s] * xa) * (1.0f - ya) +
                  ((float)lut[(ty2 * tiles + tx1) * 256 + s] * (1.0f - xa) +
                   (float)lut[(ty2 * tiles + tx2) * 256 + s] * xa) * ya;
            v = (int)lrintf(res);
            img[y * w + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
    free(lut);
}

int gx_imgproc_signal(const struct goodix_dev *d,
                      const struct gx_imgproc_params *p,
                      const uint8_t *img16, struct gx_imgproc_signal *out)
{
    const uint8_t *base = select_base(d, p);
    uint32_t npix = (uint32_t)d->img_w * d->img_h;
    float *buf;
    double s = 0.0, s2 = 0.0;

    if (!base || !npix || !out)
        return -1;
    buf = malloc(npix * sizeof(float));
    if (!buf)
        return -1;
    ridge_diff(img16, base, npix, 0, buf);
    for (uint32_t i = 0; i < npix; i++)
        s += buf[i];
    out->dc = (float)(s / npix);
    if (ridge_dog(buf, buf, d->img_w, d->img_h, p->dog_s1, p->dog_s2) != 0) {
        free(buf);
        return -1;
    }
    s = 0.0;
    for (uint32_t i = 0; i < npix; i++) {
        s += buf[i];
        s2 += (double)buf[i] * buf[i];
    }
    s /= npix;
    out->ridge = (float)sqrt(s2 / npix - s * s);
    free(buf);
    return 0;
}

/* RIDGE 管线主体。返回 0 成功；-1 无基线/分配失败（调用方回退 legacy）。 */
static int ridge_to8bit(const struct goodix_dev *d,
                        const struct gx_imgproc_params *p,
                        const uint8_t *img16, uint8_t *out8)
{
    const uint8_t *base = select_base(d, p);
    uint32_t npix = (uint32_t)d->img_w * d->img_h;
    float *buf;
    int r = -1;

    if (!base)
        return -1;
    buf = malloc(npix * sizeof(float));
    if (!buf)
        return -1;
    ridge_diff(img16, base, npix, p->deghost, buf);
    if (ridge_dog(buf, buf, d->img_w, d->img_h, p->dog_s1, p->dog_s2) == 0 &&
        ridge_stretch(buf, out8, npix, p->pct_lo, p->pct_hi) == 0) {
        ridge_clahe(out8, d->img_w, d->img_h, p->clahe_clip, p->clahe_tiles);
        r = 0;
    }
    free(buf);
    return r;
}

/* ---- 逐级调试 dump（GOODIX_DUMP_IMGPROC=1）----
 * 序号用 static 计数器：仅调试路径使用，非线程安全可接受。 */
static uint32_t imgproc_seq;

static void dump_pgm_u8(const char *stage, uint32_t seq,
                        uint32_t w, uint32_t h, const uint8_t *px)
{
    char path[512];
    FILE *f;

    gx_state_dir_ensure();
    snprintf(path, sizeof(path), "%s/imgproc-%06u-%s.pgm",
             gx_state_dir(), seq, stage);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P5\n%u %u\n255\n", w, h);
    fwrite(px, 1, (size_t)w * h, f);
    fclose(f);
    LOG("imgproc: dumped %s -> %s", stage, path);
}

static void dump_pgm16(const char *stage, uint32_t seq,
                       uint32_t w, uint32_t h, const uint16_t *px)
{
    uint8_t *tmp = malloc((size_t)w * h);
    if (!tmp)
        return;
    for (uint32_t i = 0; i < (uint32_t)w * h; i++)
        tmp[i] = (uint8_t)(px[i] >> 4);     /* 12bit -> 高 8 位 */
    dump_pgm_u8(stage, seq, w, h, tmp);
    free(tmp);
}

static void dump_pgm16le(const char *stage, uint32_t seq,
                         uint32_t w, uint32_t h, const uint8_t *img16le)
{
    uint8_t *tmp = malloc((size_t)w * h);
    if (!tmp)
        return;
    for (uint32_t i = 0; i < (uint32_t)w * h; i++)
        tmp[i] = (uint8_t)(imgproc_px16(img16le, i) >> 4);
    dump_pgm_u8(stage, seq, w, h, tmp);
    free(tmp);
}

/* 16bit 原样 dump：PGM P5 maxval 4095，像素大端（PGM 规范） */
static void dump_pgm16raw(const char *stage, uint32_t seq,
                          uint32_t w, uint32_t h, const uint8_t *img16le)
{
    char path[512];
    FILE *f;

    gx_state_dir_ensure();
    snprintf(path, sizeof(path), "%s/imgproc-%06u-%s.pgm",
             gx_state_dir(), seq, stage);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P5\n%u %u\n%u\n", w, h, IMGPROC_PX_MAX);
    for (uint32_t i = 0; i < w * h; i++) {
        uint16_t v = imgproc_px16(img16le, i);
        fputc(v >> 8, f);
        fputc(v & 0xFF, f);
    }
    fclose(f);
}

void gx_imgproc_dump16(const struct goodix_dev *d, const char *stage,
                       const uint8_t *img16)
{
    if (!d || !img16 || !getenv("GOODIX_DUMP_IMGPROC"))
        return;
    dump_pgm16raw(stage, imgproc_seq, d->img_w, d->img_h, img16);
    LOG("imgproc: dumped %s (seq %u)", stage, imgproc_seq);
}

/* ---- 公共入口 ---- */
int gx_imgproc_to8bit(const struct goodix_dev *d,
                      const struct gx_imgproc_params *p,
                      const uint8_t *img16, uint8_t *out8)
{
    uint32_t w, h, npix;
    uint16_t *buf;
    uint32_t hist[IMGPROC_PX_MAX + 1];
    int do_dump;

    if (!d || !img16 || !out8 || !p)
        return -1;
    w = d->img_w;
    h = d->img_h;
    if (w == 0 || h == 0)
        return -1;
    if (p->flatfield && p->flatfield_r < 0)
        return -1;
    if (p->pct_lo > p->pct_hi || p->pct_hi > 100)
        return -1;
    npix = w * h;
    memset(hist, 0, sizeof(hist));

    do_dump = getenv("GOODIX_DUMP_IMGPROC") != NULL;

    /* 标定：16bit 原帧 + 两种基线原样落盘，离线可复现任意管线 */
    if (do_dump) {
        struct gx_imgproc_signal sig;
        dump_pgm16raw("raw16", imgproc_seq, w, h, img16);
        if (d->img_base_valid)
            dump_pgm16raw("base16", imgproc_seq, w, h, d->img_base);
        if (d->img_fresh_valid)
            dump_pgm16raw("fresh16", imgproc_seq, w, h, d->img_fresh);
        if (gx_imgproc_signal(d, p, img16, &sig) == 0)
            LOG("imgproc: seq %u signal dc=%.0f ridge=%.1f (%s base)",
                imgproc_seq, sig.dc, sig.ridge,
                p->fresh_base && d->img_fresh_valid ? "fresh" : "stored");
    }

    if (p->mode == GX_IMGPROC_MODE_RIDGE && ridge_to8bit(d, p, img16, out8) == 0) {
        if (do_dump)
            dump_pgm_u8("ridge8", imgproc_seq, w, h, out8);
        imgproc_seq++;
        return 0;
    }

    buf = malloc((size_t)npix * sizeof(uint16_t));
    if (!buf)
        return -1;

    stage_baseline(d, img16, buf, p->baseline, p->baseline_offset);
    if (do_dump)
        dump_pgm16le("raw8", imgproc_seq, w, h, img16);
    if (do_dump)
        dump_pgm16("base8", imgproc_seq, w, h, buf);

    if (p->flatfield) {
        if (stage_flatfield(buf, w, h, p->flatfield_r, hist) == 0) {
            if (do_dump)
                dump_pgm16("flat8", imgproc_seq, w, h, buf);
        } else {
            /* 平场分配失败：回退为无平场直方图（输出仍合法） */
            for (uint32_t i = 0; i < npix; i++)
                hist[buf[i]]++;
        }
    } else {
        /* 无平场：直接用 buf 建直方图（clamp 后值在 [0,4095]） */
        for (uint32_t i = 0; i < npix; i++)
            hist[buf[i]]++;
    }

    stage_stretch(buf, out8, npix, hist, p->pct_lo, p->pct_hi);
    free(buf);
    if (do_dump)
        dump_pgm_u8("final8", imgproc_seq, w, h, out8);

    if (p->enhance == GX_IMGPROC_ENHANCE_SIGFM && p->enhance_sigma > 0.0f) {
        stage_enhance(out8, out8, w, h, p->enhance_boost, p->enhance_sigma);
        if (do_dump)
            dump_pgm_u8("enh8", imgproc_seq, w, h, out8);
    }

    imgproc_seq++;
    return 0;
}

void gx_imgproc_apply_env(struct gx_imgproc_params *p)
{
    const char *e;
    int v;

    if ((e = getenv("GOODIX_IMGPROC_BASELINE"))) {
        v = atoi(e);
        if (v == 0 || v == 1)
            p->baseline = (uint8_t)v;
    }
    if ((e = getenv("GOODIX_IMGPROC_FLATFIELD"))) {
        v = atoi(e);
        if (v == 0 || v == 1)
            p->flatfield = (uint8_t)v;
    }
    if ((e = getenv("GOODIX_IMGPROC_FLATFIELD_R"))) {
        v = atoi(e);
        if (v > 0)
            p->flatfield_r = v;
    }
    if ((e = getenv("GOODIX_IMGPROC_PCT_LO"))) {
        v = atoi(e);
        if (v >= 0 && v <= 100)
            p->pct_lo = (uint8_t)v;
    }
    if ((e = getenv("GOODIX_IMGPROC_PCT_HI"))) {
        v = atoi(e);
        if (v >= 0 && v <= 100)
            p->pct_hi = (uint8_t)v;
    }
    if ((e = getenv("GOODIX_IMGPROC_ENHANCE"))) {
        if (!strcmp(e, "sigfm"))
            p->enhance = GX_IMGPROC_ENHANCE_SIGFM;
        else if (!strcmp(e, "none"))
            p->enhance = GX_IMGPROC_ENHANCE_NONE;
    }
    if ((e = getenv("GOODIX_IMGPROC_BOOST"))) {
        float f = (float)atof(e);
        if (f > 0.0f)
            p->enhance_boost = f;
    }
    if ((e = getenv("GOODIX_IMGPROC_SIGMA"))) {
        float f = (float)atof(e);
        if (f > 0.0f)
            p->enhance_sigma = f;
    }
    if ((e = getenv("GOODIX_IMGPROC_MODE"))) {
        if (!strcmp(e, "ridge"))
            p->mode = GX_IMGPROC_MODE_RIDGE;
        else if (!strcmp(e, "legacy"))
            p->mode = GX_IMGPROC_MODE_LEGACY;
    }
    if ((e = getenv("GOODIX_IMGPROC_FRESH_BASE"))) {
        v = atoi(e);
        if (v == 0 || v == 1)
            p->fresh_base = (uint8_t)v;
    }
    if ((e = getenv("GOODIX_IMGPROC_DEGHOST"))) {
        v = atoi(e);
        if (v == 0 || v == 1)
            p->deghost = (uint8_t)v;
    }
    if ((e = getenv("GOODIX_IMGPROC_DOG_S1"))) {
        float f = (float)atof(e);
        if (f > 0.0f)
            p->dog_s1 = f;
    }
    if ((e = getenv("GOODIX_IMGPROC_DOG_S2"))) {
        float f = (float)atof(e);
        if (f > 0.0f)
            p->dog_s2 = f;
    }
    if ((e = getenv("GOODIX_IMGPROC_CLAHE"))) {
        float f = (float)atof(e);
        if (f >= 0.0f)
            p->clahe_clip = f;
    }
    if ((e = getenv("GOODIX_IMGPROC_CLAHE_TILES"))) {
        v = atoi(e);
        if (v > 0 && v <= 16)
            p->clahe_tiles = (uint8_t)v;
    }
}
