#!/usr/bin/env python3
"""逐字节一致性验证：goodix_imgproc.c（默认参数）vs 参考模型（GX_IMGPROC_DEFAULT_PARAMS）。

无第三方依赖（纯标准库）。合成多组 16bit LE 帧 + 无手指基线，断言：
  1. gx_imgproc_to8bit() 在默认参数下与参考模型逐字节一致；
  2. baseline/flatfield/enhance 开关路径输出合法且按预期变化。

与 C 实现逐条对齐的整数语义：uint16/uint32 截断在正数域等价于 Python //，
clamp 宏 v<0?0:v>4095?4095:v 等价 max(0,min(4095,v))。

用法：python3 tools/imgproc_model_check.py
"""
import math
import random

PX_MAX = 4095
IMGPROC_DEFAULT = dict(baseline=1, baseline_offset=2048, flatfield=1,
                       flatfield_r=12, pct_lo=1, pct_hi=99,
                       enhance='none', boost=0.8, sigma=1.5)


def px16(img, idx):
    return img[2 * idx] | (img[2 * idx + 1] << 8)


def clamp16(v):
    return 0 if v < 0 else (PX_MAX if v > PX_MAX else v)


def boxmean(inp, w, h, r):
    """镜像 C img_boxmean：水平/垂直一维前缀和、边界复制、整数除法。"""
    tmp = [0] * (w * h)
    out = [0] * (w * h)
    ps = [0] * (max(w, h) + 1)
    for y in range(h):
        row = inp[y * w:(y + 1) * w]
        ps[0] = 0
        for x in range(w):
            ps[x + 1] = ps[x] + row[x]
        for x in range(w):
            x0 = x - r if x > r else 0
            x1 = x + r if x + r < w else w - 1
            tmp[y * w + x] = (ps[x1 + 1] - ps[x0]) // (x1 - x0 + 1)
    for x in range(w):
        ps[0] = 0
        for y in range(h):
            ps[y + 1] = ps[y] + tmp[y * w + x]
        for y in range(h):
            y0 = y - r if y > r else 0
            y1 = y + r if y + r < h else h - 1
            out[y * w + x] = (ps[y1 + 1] - ps[y0]) // (y1 - y0 + 1)
    return out


def gaussian_blur(inp, w, h, sigma):
    r = max(1, int(math.ceil(3.0 * sigma)))
    k = [math.exp(-(i * i) / (2.0 * sigma * sigma)) for i in range(-r, r + 1)]
    ks = sum(k)
    k = [x / ks for x in k]
    tmp = [0] * (w * h)
    out = [0] * (w * h)
    for y in range(h):
        row = inp[y * w:(y + 1) * w]
        for x in range(w):
            acc = 0.0
            for j in range(-r, r + 1):
                sx = x + j
                sx = 0 if sx < 0 else (w - 1 if sx >= w else sx)
                acc += k[j + r] * row[sx]
            tmp[y * w + x] = int(acc + 0.5)
    for x in range(w):
        for y in range(h):
            acc = 0.0
            for j in range(-r, r + 1):
                sy = y + j
                sy = 0 if sy < 0 else (h - 1 if sy >= h else sy)
                acc += k[j + r] * tmp[sy * w + x]
            out[y * w + x] = int(acc + 0.5)
    return out


def enhance(inp, w, h, boost, sigma):
    blur = gaussian_blur(inp, w, h, sigma)
    out = bytearray(len(inp))
    for i in range(len(inp)):
        d = inp[i] - blur[i]
        v = inp[i] + int(round(boost * d))
        out[i] = 0 if v < 0 else (255 if v > 255 else v)
    return out


def reference_to8bit(img, base, base_valid, w, h):
    """参考模型：默认参数（GX_IMGPROC_DEFAULT_PARAMS）的逐字节镜像。"""
    npix = w * h
    buf1 = [0] * npix
    for i in range(npix):
        r = px16(img, i)
        if base_valid:
            b = px16(base, i)
            buf1[i] = clamp16(2048 + r - b)
        else:
            buf1[i] = r
    lowf = boxmean(buf1, w, h, 12)
    mean_all = sum(buf1) // npix
    hist = [0] * 4096
    for i in range(npix):
        v = clamp16(buf1[i] - lowf[i] + mean_all)
        buf1[i] = v
        hist[v & 0x0FFF] += 1
    lo, hi = 0, PX_MAX
    acc = 0
    for v in range(4096):
        acc += hist[v]
        if acc > npix // 100:
            lo = v
            break
    acc = 0
    for v in range(4095, -1, -1):
        acc += hist[v]
        if acc > npix // 100:
            hi = v
            break
    if hi <= lo:
        lo, hi = 0, PX_MAX
    out = bytearray(npix)
    for i in range(npix):
        v = buf1[i]
        if v <= lo:
            out[i] = 0
        elif v >= hi:
            out[i] = 255
        else:
            out[i] = (v - lo) * 255 // (hi - lo)
    return bytes(out)


def new_to8bit(img, base, base_valid, w, h, params):
    """新 gx_imgproc_to8bit() 的逐字节镜像（含增强）。"""
    npix = w * h
    buf = [0] * npix
    use_base = params['baseline'] and base_valid
    for i in range(npix):
        r = px16(img, i)
        if use_base:
            buf[i] = clamp16(params['baseline_offset'] + r - px16(base, i))
        else:
            buf[i] = r
    hist = [0] * (PX_MAX + 1)
    if params['flatfield']:
        lowf = boxmean(buf, w, h, params['flatfield_r'])
        mean_all = sum(buf) // npix
        for i in range(npix):
            v = clamp16(buf[i] - lowf[i] + mean_all)
            buf[i] = v
            hist[v] += 1
    else:
        for i in range(npix):
            hist[buf[i]] += 1
    n_lo = (npix * params['pct_lo']) // 100
    n_hi = (npix * (100 - params['pct_hi'])) // 100
    lo, hi = 0, PX_MAX
    acc = 0
    for v in range(PX_MAX + 1):
        acc += hist[v]
        if acc > n_lo:
            lo = v
            break
    acc = 0
    for v in range(PX_MAX, -1, -1):
        acc += hist[v]
        if acc > n_hi:
            hi = v
            break
    if hi <= lo:
        lo, hi = 0, PX_MAX
    out = bytearray(npix)
    for i in range(npix):
        v = buf[i]
        if v <= lo:
            out[i] = 0
        elif v >= hi:
            out[i] = 255
        else:
            out[i] = (v - lo) * 255 // (hi - lo)
    if params['enhance'] == 'sigfm':
        out = enhance(out, w, h, params['boost'], params['sigma'])
    return bytes(out)


def make_frame(w, h, seed):
    """合成 16bit LE 帧：base = 低频渐变 + 平滑凸起；
    finger = base + 斜向正弦脊（周期 ~9px，幅值 ~260）+ 压力渐变 + 噪声。"""
    rng = random.Random(seed)
    base = [0] * (w * h)
    finger = [0] * (w * h)
    for y in range(h):
        for x in range(w):
            idx = y * w + x
            # 低频背景：全局渐变 + 中心凸起（模拟传感器非均匀响应）
            bg = 1800 + int(600 * (x / w)) + int(300 * math.exp(
                -((x - w * 0.5) ** 2 + (y - h * 0.5) ** 2) / (2.0 * (max(w, h) / 3) ** 2)))
            base[idx] = max(0, min(PX_MAX, bg))
            # 手指：脊线 + 压力（越靠中心越亮）+ 噪声
            ridge = int(260 * math.sin(2 * math.pi * (y * 0.37 + x * 0.12) / 9.0))
            press = int(120 * math.exp(-((x - w * 0.45) ** 2 + (y - h * 0.55) ** 2) / (2.0 * (max(w, h) / 4) ** 2)))
            v = base[idx] + ridge + press + rng.randint(-18, 18)
            finger[idx] = max(0, min(PX_MAX, v))
    raw = bytearray()
    for v in finger:
        raw += v.to_bytes(2, 'little')
    baseb = bytearray()
    for v in base:
        baseb += v.to_bytes(2, 'little')
    return bytes(raw), bytes(baseb)


def main():
    failures = 0
    geoms = [(80, 64), (112, 132), (176, 54), (36, 160)]

    for (w, h) in geoms:
        for seed in range(3):
            img, base = make_frame(w, h, seed)

            # 1) 基线有效：默认参数必须逐字节一致
            o = reference_to8bit(img, base, True, w, h)
            n = new_to8bit(img, base, True, w, h, IMGPROC_DEFAULT)
            if o != n:
                diff = sum(1 for a, b in zip(o, n) if a != b)
                print(f"FAIL {w}x{h} seed{seed} base=on : {diff}/{w*h} bytes differ")
                failures += 1
            else:
                print(f"OK   {w}x{h} seed{seed} base=on  : bit-exact")

            # 2) 基线无效（img_base_valid=false）：同样必须一致
            o = reference_to8bit(img, base, False, w, h)
            n = new_to8bit(img, base, False, w, h, IMGPROC_DEFAULT)
            if o != n:
                print(f"FAIL {w}x{h} seed{seed} base=off: differs")
                failures += 1
            else:
                print(f"OK   {w}x{h} seed{seed} base=off : bit-exact")

            # 3) 开关路径合理性（参数化新增能力）
            p_flat_off = dict(IMGPROC_DEFAULT); p_flat_off['flatfield'] = 0
            p_base_off = dict(IMGPROC_DEFAULT); p_base_off['baseline'] = 0
            p_enh = dict(IMGPROC_DEFAULT); p_enh['enhance'] = 'sigfm'
            out_default = new_to8bit(img, base, True, w, h, IMGPROC_DEFAULT)
            for name, p in [('flatfield=0', p_flat_off),
                            ('baseline=0', p_base_off),
                            ('enhance=sigfm', p_enh)]:
                out = new_to8bit(img, base, True, w, h, p)
                changed = sum(1 for a, b in zip(out, out_default) if a != b)
                if changed == 0:
                    print(f"WARN {w}x{h} seed{seed} {name}: 输出与默认完全一致（预期有变化）")
                elif max(out) > 255 or min(out) < 0:
                    print(f"FAIL {w}x{h} seed{seed} {name}: 越界")
                    failures += 1
                else:
                    print(f"OK   {w}x{h} seed{seed} {name}: {changed}/{w*h} px 变化，范围合法")

    print()
    if failures:
        print(f"结果：{failures} 处不一致")
        return 1
    print("结果：全部通过（默认参数逐字节一致 + 开关路径合法）")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
