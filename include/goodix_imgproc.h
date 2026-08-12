/* goodix_imgproc.h - 图像预处理（主机侧比对输入）参数化管线
 *
 * 独立于协议/采集代码的 16bit 帧 -> 8bit 灰度管线，供 libfprint 的
 * SIGFM 匹配与 CLI --capture 共用（两入口输出一致）；各环节可经
 * 环境变量（GOODIX_IMGPROC_*）或 struct gx_imgproc_params 调整，用于真机标定。
 *
 * 管线（每帧调用，全部无 static 缓冲，可重入）：
 *   1. baseline    可选减无手指基线（有符号差 + 2048 余量，clamp [0,4095]）
 *   2. flatfield   可选去低频背景（可分离盒式均值，窗口远大于脊距）
 *   3. stretch     1%/99% 百分位截断映射到 0..255
 *   4. enhance     可选反锐化局部对比增强（针对 SIGFM/SIFT 的
 *                  contrastThreshold=0.04，提升弱脊关键点通过率）
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GOODIX_IMGPROC_H
#define GOODIX_IMGPROC_H

#include <stdint.h>

struct goodix_dev;          /* 定义见 goodix.h，此处仅前向声明 */

/* 增强模式 */
enum gx_imgproc_enhance {
    GX_IMGPROC_ENHANCE_NONE = 0,     /* 不做增强（默认） */
    GX_IMGPROC_ENHANCE_SIGFM,        /* 反锐化：out = in + boost*(in - gauss(in,σ)) */
};

/* 处理参数。默认值见 GX_IMGPROC_DEFAULT_PARAMS。 */
struct gx_imgproc_params {
    uint8_t  baseline;          /* 1: 减无手指基线（d->img_base）；0: 用原帧 */
    uint16_t baseline_offset;   /* 有符号差余量（默认 2048，对称容纳双向差） */
    uint8_t  flatfield;         /* 1: 低频平场校正 */
    int      flatfield_r;       /* 盒式均值半径（默认 12，窗口 2r+1 ≈ 25px） */
    uint8_t  pct_lo, pct_hi;    /* 保留区间 [pct_lo%, pct_hi%]（默认 1/99 = 各截断
                                  底部/顶部 1%），clamp 后映射 0..255 */
    uint8_t  enhance;           /* GX_IMGPROC_ENHANCE_* */
    float    enhance_boost;     /* 反锐化增益（默认 0.8） */
    float    enhance_sigma;     /* 反锐化高斯 σ（默认 1.5，像素） */
};

#define GX_IMGPROC_DEFAULT_PARAMS \
    { 1, 2048, 1, 12, 1, 99, GX_IMGPROC_ENHANCE_NONE, 0.8f, 1.5f }
/* SIGFM 驱动路径默认：开启反锐化局部对比增强。小图（80x64）弱按压下
 * SIFT 关键点少且不可复现，增强提升脊谷对比度与关键点密度/可复现性，
 * 缓解注册/验证间歇性 no-match；几何一致性投票可过滤增强引入的噪点。
 * 环境变量（GOODIX_IMGPROC_*）可覆盖，改参数后须重新 enroll。 */
#define GX_IMGPROC_SIGFM_PARAMS \
    { 1, 2048, 1, 12, 1, 99, GX_IMGPROC_ENHANCE_SIGFM, 0.8f, 1.5f }

/* 主入口：按参数处理。img16 为 w*h*2 字节 16bit LE 帧，out8 为 w*h 字节。
 * 返回 0 成功；-1 参数非法（w/h 越界、半径<0、百分位非法）。
 * 设置 GOODIX_DUMP_IMGPROC=1 时把各阶段写到 gx_state_dir()/imgproc-<seq>-<stage>.pgm
 * （raw8/base8/flat8/final8/enh8），用于真机标定。 */
int gx_imgproc_to8bit(const struct goodix_dev *d,
                      const struct gx_imgproc_params *p,
                      const uint8_t *img16, uint8_t *out8);

/* 用环境变量覆盖参数（各字段可单独覆盖，未设置的环境变量保持原值）：
 *   GOODIX_IMGPROC_BASELINE=0|1
 *   GOODIX_IMGPROC_FLATFIELD=0|1
 *   GOODIX_IMGPROC_FLATFIELD_R=<int>
 *   GOODIX_IMGPROC_PCT_LO=<int>  GOODIX_IMGPROC_PCT_HI=<int>
 *   GOODIX_IMGPROC_ENHANCE=none|sigfm
 *   GOODIX_IMGPROC_BOOST=<float>  GOODIX_IMGPROC_SIGMA=<float>
 * 非法取值静默忽略。 */
void gx_imgproc_apply_env(struct gx_imgproc_params *p);

#endif /* GOODIX_IMGPROC_H */
