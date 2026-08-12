/* main.c - 命令行调试工具
 *
 * 用法：
 *   sudo ./goodix-cli            # 完整初始化（复位→版本→chipid→PSK→TLS）+ 采集
 *   sudo ./goodix-cli --info     # 仅初始化
 *   sudo ./goodix-cli --capture N  # 采集 N 帧
 *   sudo ./goodix-cli --pid 0x5135 # 指定 PID
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "goodix.h"
#include <stdlib.h>

/* 图像缓冲 = 解包后的 16bit LE 像素（有效值 12bit，0..4095）。
 * PGM 是 gx_imgproc_to8bit() 的输出，与 libfprint 送入 SIGFM（SIFT 提取）
 * 的图像一致；可经 GOODIX_IMGPROC_* 环境变量调参、GOODIX_DUMP_IMGPROC=1
 * 逐级 dump 标定。用于核对脊线极性、连续性和对比度。 */
static int save_pgm(struct goodix_dev *d, const char *path,
                    const uint8_t *img, uint16_t sz)
{
    uint8_t out8[GF_IMG_MAX / 2];
    /* 与 libfprint 驱动一致走 SIGFM 参数（反锐化增强开启），使 CLI 标定的
     * 就是实际送入 SIGFM（SIFT 提取）的图像；GOODIX_IMGPROC_* 可覆盖。 */
    struct gx_imgproc_params p = GX_IMGPROC_SIGFM_PARAMS;
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    if (sz == (uint16_t)(d->img_w * d->img_h * 2)) {
        fprintf(f, "P5\n%u %u\n255\n", d->img_w, d->img_h);
        gx_imgproc_apply_env(&p);   /* CLI 兼作标定工具，可感知环境变量 */
        gx_imgproc_to8bit(d, &p, img, out8);
        fwrite(out8, 1, (size_t)d->img_w * d->img_h, f);
    } else {
        /* 非预期长度：原样写出便于调试 */
        fprintf(f, "P5\n%u %u\n255\n", (unsigned)sz, 1u);
        fwrite(img, 1, sz, f);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    struct goodix_dev dev;
    uint16_t pid = GOODIX_PID_5125;
    int captures = 1;
    bool only_info = false;
    int r;

    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && !strcmp(argv[i], "--pid"))
            pid = (uint16_t)strtoul(argv[++i], NULL, 0);
        else if (i + 1 < argc && !strcmp(argv[i], "--capture"))
            captures = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--info"))
            only_info = true;
    }

    if (getenv("GOODIX_DEBUG"))
        gx_debug = 1;
    printf("goodix: probing %04x:%04x\n", GOODIX_VID, pid);
    memset(&dev, 0, sizeof(dev));
    dev.vid = GOODIX_VID;
    dev.pid = pid;

    r = gx_transport_open(&dev);
    if (r < 0) {
        fprintf(stderr, "open failed: %d (need root? detach cdc_acm?)\n", r);
        return 1;
    }

    r = gx_device_init(&dev);
    if (r < 0) {
        fprintf(stderr, "device init failed: %d\n", r);
        gx_transport_close(&dev);
        return 1;
    }
    printf("sensor: chipid 0x%04x, %ux%u, PSK %s, TLS %s\n",
           dev.chipid, dev.img_w, dev.img_h,
           dev.have_psk ? "yes" : "no",
           dev.tls_inited ? "inited" : "plaintext");

    if (only_info) {
        gx_transport_close(&dev);
        return 0;
    }

    for (int i = 0; i < captures; i++) {
        uint8_t img[GF_IMG_MAX];
        uint16_t sz = 0;
        printf("capture %d/%d...\n", i + 1, captures);
        printf(">>> PLACE YOUR FINGER ON THE SENSOR NOW <<<\n");
        fflush(stdout);
        r = gx_capture(&dev, img, &sz);
        if (r == 0) {
            char path[64];
            snprintf(path, sizeof(path), "image-%d.pgm", i);
            if (save_pgm(&dev, path, img, sz) == 0)
                printf("saved %s (%u bytes)\n", path, sz);
        } else {
            fprintf(stderr, "capture %d failed: %d\n", i + 1, r);
        }
    }

    gx_transport_close(&dev);
    return 0;
}
