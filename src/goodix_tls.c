/* goodix_tls.c - mbedtls PSK TLS server
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * mbedtls server 配置：
 *   - PSK identity："Client_identity"（设备以客户端身份连接）
 *   - 密码套件：TLS-PSK-WITH-AES-128-GCM-SHA256（0x00A8，仅 TLS1.2）
 *   - 认证模式：无（仅 PSK）
 *   - send/recv 回调桥接到 USB TLS 帧层（0xB0 帧）
 *
 * 握手流程：
 *   1. mbedtls server 初始化
 *   2. TLS server 初始化命令（0xD1 {0,0}）-> 设备以客户端启动
 *   3. mbedtls_ssl_handshake() 响应设备 ClientHello
 *   4. TLS 握手命令（0xD4 {0,0}）
 *   5. 读 MCU 状态（0xAF）-> bit1=1 表示 TLS 已连接；bit3=1 锁定 -> 重发命令
 *   6. 标记 tls_inited
 *
 * 构建：apt install libmbedtls-dev
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/version.h>
#include "goodix.h"

/* ---- TLS 帧桥接 ----
 * TLS 帧：[0]=0xB0 [1:2]=总长 [3]=校验和 [4..]=TLS 记录 [末]=校验和
 */
static int tls_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct goodix_dev *d = ctx;
    if (len > 0x3FFF)    /* 保持在 MAX_FRAME 内 */
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (gx_debug) {
        fprintf(stderr, "[goodix] TLS send %uB: %02X %02X %02X %02X %02X",
                (unsigned)len, buf[0], buf[1], buf[2], buf[3], buf[4]);
        if (buf[0] == 0x16 && len > 10) {
            /* ServerHello / ServerKeyExchange：打印一段以查看
             * 协商出的密码套件（偏移 11+32+1 = 44）。 */
            fprintf(stderr, " [dump]");
            for (size_t i = 0; i < len && i < 96; i++)
                fprintf(stderr, " %02X", buf[i]);
        }
        fprintf(stderr, "\n");
    }
    if (gx_send_tls_frame(d, buf, (uint16_t)len) < 0)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    /* 每帧发送后延时 10ms，给设备时间处理 TLS 记录。 */
    usleep(10000);
    return (int)len;
}

/* ---- 流式接收缓冲 ----
 * mbedtls BIO 是流式的：先 recv(5) 读 TLS 记录头，再 recv(n) 读记录体。
 * 接收数据来自聚合缓冲，多余字节会在调用间保留。这里用环形缓冲实现
 * 同样行为：帧中多出的数据保留给下一次 recv()。
 */
static uint8_t tls_rx_buf[8192];
static size_t tls_rx_head = 0, tls_rx_tail = 0;
/* 采集模式：bio_recv 不再自己读 USB（否则会把设备推送的明文 A0 帧
 * ——FDT 事件/图像——静默吞掉），只 drain gx_tls_feed 注入的缓冲。
 * TLS 激活后明文 A0 帧直接进入采集分流路径，B0 帧才进 ssl_read。 */
static int tls_rx_no_usb = 0;

void gx_tls_capture_mode(struct goodix_dev *d, int on)
{
    (void)d;
    tls_rx_no_usb = on;
}

/* 把一条设备发来的 TLS record（B0 帧 payload）注入接收缓冲 */
int gx_tls_feed(struct goodix_dev *d, const uint8_t *record, size_t len)
{
    (void)d;
    if (tls_rx_tail + len > sizeof(tls_rx_buf)) {
        /* 压缩/复位缓冲，实在放不下就报错 */
        if (tls_rx_head) {
            memmove(tls_rx_buf, tls_rx_buf + tls_rx_head,
                    tls_rx_tail - tls_rx_head);
            tls_rx_tail -= tls_rx_head;
            tls_rx_head = 0;
        }
        if (tls_rx_tail + len > sizeof(tls_rx_buf))
            return -1;
    }
    memcpy(tls_rx_buf + tls_rx_tail, record, len);
    tls_rx_tail += len;
    return 0;
}

static int tls_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    struct goodix_dev *d = ctx;
    static uint8_t frame[MAX_FRAME];

    /* 采集模式：USB 读取由调用方驱动（明文帧走采集分流路径），
     * 这里只 drain 注入缓冲，空了就 WANT_READ。 */
    if (tls_rx_no_usb)
        goto drain;

    /* 持续拉取 USB TLS 帧，直到积累 >= len 字节 */
    while (tls_rx_tail - tls_rx_head < len) {
        uint16_t flen = 0;
        int r = gx_read_frame(d, frame, &flen, 500);
        if (r < 0)
            break;
        if (flen < 5)
            continue;
        if (frame[0] != GF_TYPE_TLS)
            continue;
        size_t n = flen - 4;
        if (tls_rx_tail + n > sizeof(tls_rx_buf)) {
            /* 溢出保护：丢弃已缓冲数据并重新填充 */
            tls_rx_head = tls_rx_tail = 0;
            if (n > sizeof(tls_rx_buf)) {
                LOG("TLS frame too big (%u)", (unsigned)n);
                return MBEDTLS_ERR_SSL_WANT_READ;
            }
        }
        memcpy(tls_rx_buf + tls_rx_tail, &frame[4], n);
        tls_rx_tail += n;
        if (gx_debug) {
            fprintf(stderr, "[goodix] TLS recv +%uB (frame %uB): %02X %02X %02X %02X %02X",
                    (unsigned)n, flen, frame[4], frame[5], frame[6], frame[7], frame[8]);
            /* ClientHello：打印整帧以查看密码套件 */
            if (frame[4] == 0x16 && frame[9] == 0x01) {
                fprintf(stderr, " [ClientHello]");
                for (uint16_t i = 9; i < flen && i < 4 + 64; i++)
                    fprintf(stderr, " %02X", frame[i]);
            } else if (frame[4] == 0x16 && frame[9] == 0x10) {
                fprintf(stderr, " [ClientKeyExchange]");
                for (uint16_t i = 9; i < flen && i < 4 + 48; i++)
                    fprintf(stderr, " %02X", frame[i]);
            } else if (frame[4] == 0x16) {
                /* CCS 后的加密握手（Finished）：完整转储 */
                fprintf(stderr, " [handshake/encrypted]");
                for (uint16_t i = 4; i < flen && i < 4 + 64; i++)
                    fprintf(stderr, " %02X", frame[i]);
            }
            fprintf(stderr, "\n");
        }
    }
drain:;
    size_t avail = tls_rx_tail - tls_rx_head;
    if (avail == 0)
        return MBEDTLS_ERR_SSL_WANT_READ;
    size_t n = avail < len ? avail : len;
    memcpy(buf, tls_rx_buf + tls_rx_head, n);
    tls_rx_head += n;
    if (tls_rx_head == tls_rx_tail)
        tls_rx_head = tls_rx_tail = 0;
    return (int)n;
}

/* ---- mbedtls 上下文 ---- */
struct gx_tls_ctx {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
};

/* 初始化 mbedtls PSK server */
int gx_tls_server_init(struct goodix_dev *d)
{
    struct gx_tls_ctx *t;
    const char *pers = "ssl_server2";
    int r;

    if (!d->have_psk) {
        LOG("no PSK, cannot init TLS server");
        return -1;
    }
    t = calloc(1, sizeof(*t));
    if (!t)
        return -1;
    d->tls = t;

    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->ctr_drbg);
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);

    r = mbedtls_ctr_drbg_seed(&t->ctr_drbg, mbedtls_entropy_func,
                              &t->entropy, (const unsigned char *)pers, strlen(pers));
    if (r) { LOG("ctr_drbg_seed: -0x%x", -r); goto fail; }

    r = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (r) { LOG("config_defaults: -0x%x", -r); goto fail; }

    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->ctr_drbg);
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);

    /* 将 TLS 版本锁定为 min=max=TLS1.2
     * (mbedtls_ssl_conf_min/max_version(conf, 3, 3))。若不锁定，
     * mbedtls 3.x 可能协商到 TLS1.3（该版本无 PSK 密码套件），
     * 在 TLS1.2 ClientHello 上停滞。必须固定为 TLS1.2。 */
#if MBEDTLS_VERSION_NUMBER >= 0x03010000
    /* mbedtls 3.1+：mbedtls_ssl_conf_min/max_version() 已废弃，
     * 改用接受 MBEDTLS_SSL_VERSION_* 的 *_tls_version() 变体。 */
    mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
#else
    mbedtls_ssl_conf_min_version(&t->conf,
                                 MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&t->conf,
                                 MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
#endif

    /* TLS-PSK-WITH-AES-128-GCM-SHA256 (0x00A8) 仅支持 TLS1.2，
     * 因此仅凭密码套件列表即可固定协议版本。
     * 注意：0xC0A8 是 TLS-ECDHE-PSK-WITH-AES-128-GCM-SHA256 —— 一个不同的
     * 套件；设备的 ClientHello 提供的是 0x00A8。 */
#ifndef MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256
#define MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256 0x00A8
#endif
    static const int ciphersuites[] = {
        MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256,
        0
    };
    /* 检查 0x00A8 是否实际编译进 mbedtls（Ubuntu 通常已包含） */
    {
        const int *list = mbedtls_ssl_list_ciphersuites();
        int found = 0;
        for (int i = 0; list && list[i]; i++) {
            if (list[i] == MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256) {
                found = 1;
                break;
            }
        }
        LOG("PSK ciphersuite 0x%04X %s in mbedtls",
            MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256,
            found ? "AVAILABLE" : "NOT COMPILED (!!)");
    }
    mbedtls_ssl_conf_ciphersuites(&t->conf, ciphersuites);

    r = mbedtls_ssl_conf_psk(&t->conf, d->psk, d->psk_len,
                             (const unsigned char *)"Client_identity", 15);
    if (r) { LOG("conf_psk: -0x%x", -r); goto fail; }

    r = mbedtls_ssl_setup(&t->ssl, &t->conf);
    if (r) { LOG("ssl_setup: -0x%x", -r); goto fail; }

    /* setup 后调用 mbedtls_ssl_session_reset */
    r = mbedtls_ssl_session_reset(&t->ssl);
    if (r) { LOG("session_reset: -0x%x", -r); goto fail; }

    mbedtls_ssl_set_bio(&t->ssl, d, tls_bio_send, tls_bio_recv, NULL);
    LOG("TLS server configured (PSK %u bytes, TLS1.2 pinned)", (unsigned)d->psk_len);
    return 0;

fail:
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->ctr_drbg);
    mbedtls_entropy_free(&t->entropy);
    free(t);
    d->tls = NULL;
    return r;
}

/* 执行 TLS 握手（设备为客户端，本端作为服务端响应） */
int gx_tls_handshake(struct goodix_dev *d)
{
    struct gx_tls_ctx *t = d->tls;
    uint8_t state[16] = { 0 };
    int r, tried = 0;

    if (!t)
        return -1;
    if (d->tls_inited)
        return 0;

    /* 步骤 2：TLS server 初始化命令 */
    r = gx_tls_server_cmd(d);
    LOG("TLSServerInit cmd ret=%d", r);

    /* 运行握手循环；设备在命令后发送 ClientHello。
     * 记录每个不同的 mbedtls 返回码，便于观察卡住的状态。
     * 限制在约 30 秒墙钟时间内，避免永远挂起。 */
    {
        int last_r = 0;
        uint32_t t0 = (uint32_t)time(NULL);
        while ((uint32_t)time(NULL) - t0 < 30) {
            r = mbedtls_ssl_handshake(&t->ssl);
            if (r == 0)
                break;
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
                LOG("handshake err -0x%x", -r);
                return r;
            }
            if (r != last_r || tried < 3) {
                LOG("handshake step: mbedtls ret %s (try %d)",
                    r == MBEDTLS_ERR_SSL_WANT_READ ? "WANT_READ" : "WANT_WRITE", tried);
            }
            last_r = r;
            tried++;
        }
    }

    if (r != 0) {
        LOG("handshake not complete after %d tries (30s)", tried);
        /* 失败收尾：延时 10ms + 重发 TLS 握手命令 */
        usleep(10000);
        gx_tls_handshake_cmd(d);
        return -2;
    }
    LOG("TLS handshake done (server side), cipher %s",
        mbedtls_ssl_get_ciphersuite(&t->ssl));

    /* 步骤 4：发送 TLS 握手命令 */
    r = gx_tls_handshake_cmd(d);
    LOG("SendTlsHandshakeCmd ret=%d", r);

    /* 步骤 5：通过 MCU 状态 bit1 确认；bit3 时解锁 */
    for (int i = 0; i < 3; i++) {
        usleep(20000);
        if (gx_get_mcu_state(d, state) == 0) {
            if (state[1] & 0x02) {
                LOG("TLS connected confirmed (mcu state 0x%02x)", state[1]);
                d->tls_inited = true;
                return 0;
            }
            if (state[1] & 0x08) {
                LOG("MCU still locked, unlocking...");
                gx_tls_handshake_cmd(d);
            }
        }
    }
    LOG("TLS connect not confirmed in MCU (state 0x%02x)", state[1]);
    /* 失败收尾（确认失败分支）：即使确认失败也
     * 再延时 10ms + 重发 TLS 握手命令一次，给 MCU 最后一次解锁机会。 */
    usleep(10000);
    gx_tls_handshake_cmd(d);
    return -3;
}

/* TLS 重连：设备通过 cmd0=13 cmd1=5 (0xDA) 明文帧主动要求重建 TLS，
 * 或读取 MCU 状态发现 bit1(isTlsConnected)=0 时由 host 发起。
 * 这里重置 mbedtls session + 清空接收缓冲后重跑完整握手序列。 */
int gx_tls_reconnect(struct goodix_dev *d)
{
    struct gx_tls_ctx *t = d->tls;
    if (!t)
        return -1;
    LOG("TLS reconnect requested, resetting session");
    mbedtls_ssl_session_reset(&t->ssl);
    tls_rx_head = tls_rx_tail = 0;
    d->tls_inited = false;
    return gx_tls_handshake(d);
}

/* TLS recv：读取解密后的应用数据。
 * 返回 mbedtls_ssl_read 的原值：>0 = 读到的字节数（透传给调用方），
 * <0 = WANT_READ/错误。TLS 激活后解密数据进入采集数据流，靠此返回值
 * 拿到图像帧字节。 */
int gx_tls_recv(struct goodix_dev *d, uint8_t *data, size_t cap, int tmo)
{
    struct gx_tls_ctx *t = d->tls;
    (void)tmo;
    if (!t || !d->tls_inited)
        return -1;
    return mbedtls_ssl_read(&t->ssl, data, cap);
}
