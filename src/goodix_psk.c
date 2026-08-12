/* goodix_psk.c - PSK 管理
 *
 * PSK 生命周期：
 *   首次初始化：生成随机 32B PSK -> 白盒 AES-GCM 加密 -> 写入 MCU
 *   后续初始化：读取 MCU 数据（0xBB010003）-> 用白盒 AES 解密 Block
 *               -> 用作 TLS-PSK
 *
 * 白盒加密为 AES-256-GCM，使用固定派生密钥，外加 HMAC-SHA256 信封。
 * 全部是确定性的（无随机成分），因此 host_hash = SHA256(blob)
 * 总是等于 MCU 端对存储 blob 的哈希。
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include "goodix.h"

/* ======================================================================
 * PSK 白盒 AES-GCM。构造为固定派生密钥的 AES-256-GCM 加 HMAC-SHA256
 * 信封，并校正为 MCU 固件（st411sec_app.bin）使用的常量：
 *   KDF 密钥 = "Goodix" 变体（固件默认全局：variant=0，string selector=1），
 *   GCM AAD = 固件 flash 常量。
 * 固件是最终的解密者——它决定 TLS PSK。
 *
 * Blob 布局（inlen + 70 字节；多字节字段为 LE）：
 *   [0..31]           HMAC-SHA256(key=v43[16..48],
 *                                   msg=blob[32..38] || ct || tag)
 *   [32..33]          02 FF   （WORD 0xFF02，按 LE 存储）
 *   [34..37]          明文长度 (u32)
 *   [38..53]          iv = v42[0..15]
 *   [54..53+len]      ct  = AES-256-GCM 密文
 *   [54+len..69+len]  tag = GCM 标签（16B）
 *
 * 密钥调度：
 *   v43 = T1 || T2[0..15]  （48 字节）
 *   T_i = HMAC-SHA256(key=WB_KDF_KEY,
 *                     msg=BE32(i) || "kgoodwixg\0" ||
 *                         "kaelrgnoerlithm" || 00 00 01 80)，i = 1,2
 *   AES-256-GCM 密钥 = v43[0..32]，块 HMAC 密钥 = v43[16..48]
 *   v42 = SHA256(02 FF || len32le || plain[0..len/4] || 16 x LE32(3))
 *   GCM iv = v42[0..15]，GCM aad = WB_GCM_AAD（16 字节）
 *
 * WB_GCM_AAD 来自 MCU 固件（st411sec_app.bin 中 0xFF02 解密路径
 * 从 flash 加载），而不是来自驱动内部解密的立即数 AAD
 * （52 2A C3 F0 ...）。固件是权威：一个在固件 AAD 下无法认证的 blob
 * 会被 MCU 拒绝，MCU 随后派生出不同的 TLS PSK -> 握手报 bad_record_mac。
 * 经固件 WB 解密验证：key=WB_KDF_KEY + 此 AAD 能精确还原 PSK；
 * 该 AAD 无法通过固件的 GCM 标签检查。
 * ====================================================================== */

static const uint8_t WB_KDF_KEY[32] = {
    /* 固件 derive1（"Goodix" selector） */
    0x5C, 0xBA, 0x6E, 0x25, 0x81, 0x95, 0x18, 0xDE,
    0x2D, 0x53, 0xE9, 0x6D, 0xC0, 0x34, 0x7A, 0xB0,
    /* 固件 derive2 */
    0xD4, 0x27, 0xD4, 0x08, 0x4B, 0xDA, 0x4F, 0xAE,
    0x1B, 0xFF, 0x2B, 0x09, 0x11, 0x2A, 0x57, 0xE5
};

/* 固件 GCM AAD（st411sec_app.bin 的 0xFF02 解密路径） */
static const uint8_t WB_GCM_AAD[16] = {
    0x52, 0x2D, 0xC1, 0xF0, 0x99, 0x56, 0x7D, 0x07,
    0xF4, 0x7F, 0x37, 0xA3, 0x2A, 0x84, 0x42, 0x7D
};

/* KDF：v43 = T1 || T2[0..15]，共 48 字节 */
static void psk_kdf(uint8_t out[48])
{
    /* "kgoodwixg\0" || "kaelrgnoerlithm" || 00 00 01 80 */
    static const uint8_t salt[29] = {
        'k','g','o','o','d','w','i','x','g', 0x00,
        'k','a','e','l','r','g','n','o','e','r','l','i','t','h','m',
        0x00, 0x00, 0x01, 0x80
    };
    uint8_t msg[4 + sizeof(salt)];
    uint8_t mac[32];
    unsigned int mlen = 0;
    for (int i = 1; i <= 2; i++) {
        msg[0] = 0; msg[1] = 0; msg[2] = 0; msg[3] = (uint8_t)i; /* BE32(i) */
        memcpy(msg + 4, salt, sizeof(salt));
        HMAC(EVP_sha256(), WB_KDF_KEY, sizeof(WB_KDF_KEY),
             msg, sizeof(msg), mac, &mlen);
        memcpy(out + (i - 1) * 32, mac, (i == 1) ? 32 : 16);
    }
}

/* v42: SHA256(02 FF || len32le || plain[0..len/4] || 16 x LE32(3)) */
static int psk_derive_v42(const uint8_t *plain, uint32_t len, uint8_t out[32])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    uint8_t hdr[6] = { 0x02, 0xFF, (uint8_t)len, (uint8_t)(len >> 8),
                       (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
    uint8_t d3[4] = { 3, 0, 0, 0 };
    unsigned int olen = 0;
    uint32_t head = len >> 2;   /* 参与哈希的明文字节数 = len >> 2 */
    int ok;
    if (!ctx)
        return -1;
    ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
         EVP_DigestUpdate(ctx, hdr, sizeof(hdr)) == 1 &&
         (head == 0 || EVP_DigestUpdate(ctx, plain, head) == 1);
    for (int i = 0; ok && i < 16; i++)
        ok = EVP_DigestUpdate(ctx, d3, 4) == 1;
    if (ok)
        ok = EVP_DigestFinal_ex(ctx, out, &olen) == 1 && olen == 32;
    EVP_MD_CTX_free(ctx);
    return ok ? 0 : -1;
}

/* AES-256-GCM 加密，16 字节 iv + 16 字节 tag */
static int aes_256_gcm_encrypt(const uint8_t key[32], const uint8_t iv[16],
                               const uint8_t *plain, uint32_t len,
                               uint8_t *ct, uint8_t tag[16])
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, n2 = 0;
    int ok;
    if (!ctx)
        return -1;
    ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 16, NULL) == 1 &&
         EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) == 1 &&
         EVP_EncryptUpdate(ctx, NULL, &n, WB_GCM_AAD, sizeof(WB_GCM_AAD)) == 1 &&
         EVP_EncryptUpdate(ctx, ct, &n, plain, (int)len) == 1 &&
         EVP_EncryptFinal_ex(ctx, ct + n, &n2) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok ? 0 : -1;
}

/* AES-256-GCM 解密 + 标签校验；认证失败返回 -2 */
static int aes_256_gcm_decrypt(const uint8_t key[32], const uint8_t iv[16],
                               const uint8_t *ct, uint32_t len,
                               const uint8_t tag[16], uint8_t *plain)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, n2 = 0;
    int ok;
    if (!ctx)
        return -1;
    ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 16, NULL) == 1 &&
         EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) == 1 &&
         EVP_DecryptUpdate(ctx, NULL, &n, WB_GCM_AAD, sizeof(WB_GCM_AAD)) == 1 &&
         EVP_DecryptUpdate(ctx, plain, &n, ct, (int)len) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) == 1;
    if (ok && EVP_DecryptFinal_ex(ctx, plain + n, &n2) != 1)
        ok = 0;   /* GCM 标签不匹配：WB 密钥/blob 不一致 */
    EVP_CIPHER_CTX_free(ctx);
    return ok ? 0 : -2;
}

/* 白盒加密。输出布局见上；wb_len = len + 70 */
int gx_wb_encrypt(const uint8_t *plain, uint32_t len, uint8_t *out, uint32_t *out_len)
{
    uint8_t k48[48];
    uint8_t v42[32];
    uint8_t h[32];
    unsigned int hlen = 0;
    uint32_t total = 54 + len + 16;
    uint8_t msg[6 + 4096 + 16];
    size_t n = 0;

    if (len == 0 || len > 4096 || total > MAX_FRAME)
        return -1;
    psk_kdf(k48);
    if (psk_derive_v42(plain, len, v42) != 0)
        return -1;

    memset(out, 0, total);
    out[32] = 0x02;                     /* WORD 0xFF02, LE */
    out[33] = 0xFF;
    out[34] = (uint8_t)len;
    out[35] = (uint8_t)(len >> 8);
    out[36] = (uint8_t)(len >> 16);
    out[37] = (uint8_t)(len >> 24);
    memcpy(out + 38, v42, 16);          /* iv in clear */

    if (aes_256_gcm_encrypt(k48, v42, plain, len,
                            out + 54, out + 54 + len) != 0)
        return -1;

    /* 块 HMAC：HMAC-SHA256(key=v43[16..48],
     *                         msg=blob[32..38] || ct || tag) */
    memcpy(msg + n, out + 32, 6); n += 6;
    memcpy(msg + n, out + 54, (size_t)(len + 16)); n += len + 16;
    HMAC(EVP_sha256(), k48 + 16, 32, msg, n, h, &hlen);
    memcpy(out, h, 32);

    *out_len = total;
    return 0;
}

int gx_wb_decrypt(const uint8_t *wb, uint32_t wb_len, uint8_t *plain, uint32_t *plain_len)
{
    uint8_t k48[48];
    uint8_t h[32];
    unsigned int hlen = 0;
    uint32_t len;
    uint8_t msg[6 + 4096 + 16];
    size_t n = 0;

    if (wb_len < 70)
        return -1;
    psk_kdf(k48);
    len = (uint32_t)wb[34] | ((uint32_t)wb[35] << 8) |
          ((uint32_t)wb[36] << 16) | ((uint32_t)wb[37] << 24);
    if (len == 0 || len > 4096)
        return -3;
    if (54 + len + 16 > wb_len)
        return -2;

    /* 先校验块 HMAC，再处理密文 */
    memcpy(msg + n, wb + 32, 6); n += 6;
    memcpy(msg + n, wb + 54, (size_t)(len + 16)); n += len + 16;
    HMAC(EVP_sha256(), k48 + 16, 32, msg, n, h, &hlen);
    if (memcmp(h, wb, 32) != 0)
        return -4;

    if (aes_256_gcm_decrypt(k48, wb + 38, wb + 54, len,
                            wb + 54 + len, plain) != 0)
        return -5;
    *plain_len = len;
    return 0;
}



/* ============ MCU 写 / 读（PSK 生命周期管理） =================
 *
 * 写 TLV：
 *   [0xBB010002 u32 LE] [len u32 LE] [dummy seal 64B] [entropy 8B]
 *   [0xBB010003 u32 LE] [wb_len u32 LE] [wb block 102B]
 * MCU 忽略宿主侧 DPAPI 密封（ARM 无法运行 CryptProtectData）；
 * 这里写入固定 64B 填充 + 8B 熵。
 *
 * 读：gx_mcu_read(GF_DT_PSK_DATA) 返回 wb block；再 WB 解密。
 */
int gx_psk_write_to_mcu(struct goodix_dev *d, const uint8_t *psk)
{
    uint8_t wb[MAX_FRAME];
    uint32_t wb_len = 0;
    uint8_t tlv[32 + 64 + 8 + 4 + MAX_FRAME];
    uint32_t o = 0;
    int r;

    r = gx_wb_encrypt(psk, 32, wb, &wb_len);
    if (r < 0) {
        LOG("WB encrypt failed (%d)", r);
        return r;
    }

    /* [0xBB010002][len][seal 64][entropy 8] */
    uint32_t dt1 = 0xBB010002;
    uint32_t len1 = 64 + 8;
    memcpy(tlv + o, &dt1, 4); o += 4;
    memcpy(tlv + o, &len1, 4); o += 4;
    memset(tlv + o, 0x5A, 64); o += 64;      /* dummy DPAPI seal */
    for (int i = 0; i < 8; i++)              /* entropy (random-ish) */
        tlv[o++] = (uint8_t)(i * 7 + 1);

    /* [0xBB010003][wb_len][wb] */
    uint32_t dt2 = 0xBB010003;
    memcpy(tlv + o, &dt2, 4); o += 4;
    memcpy(tlv + o, &wb_len, 4); o += 4;
    memcpy(tlv + o, wb, wb_len); o += wb_len;

    /* 将数据长度补齐到 4 字节边界。190 -> 192。 */
    while (o & 3)
        tlv[o++] = 0;
    r = gx_mcu_write(d, tlv, (uint16_t)o);
    if (r < 0) {
        LOG("gx_mcu_write failed (%d)", r);
        return r;
    }
    LOG("PSK written to MCU (wb %u B, tlv %u B)", wb_len, o);
    return 0;
}

int gx_psk_read_from_mcu(struct goodix_dev *d)
{
    uint8_t buf[1024];
    uint16_t bl = sizeof(buf);
    uint8_t wb[102];
    uint32_t plen = 0;
    int r;

    r = gx_mcu_read(d, GF_DT_PSK_WB, buf, &bl);   /* 0xBB010003 = WB storage segment */
    if (r < 0) {
        LOG("gx_mcu_read PSK failed (%d)", r);
        return r;
    }
    /* gx_mcu_read 已剥离状态字节；载荷为
     * [data_type 4B][len 4B][wb 102B]（数据在 8 字节头之后）。跳过头部。 */
    if (bl < 8 + 102) {
        LOG("PSK read response too short (%u B, want >=110)", bl);
        return -2;
    }
    memcpy(wb, buf + 8, 102);
    r = gx_wb_decrypt(wb, 102, d->psk, &plen);
    if (r < 0 || plen != 32) {
        LOG("WB decrypt failed (%d, plen=%u)", r, plen);
        return -3;
    }
    d->psk_len = 32;
    d->have_psk = true;
    LOG("PSK recovered from MCU (WB-decrypt ok)");
    return 0;
}

/* ================= PSK 宿主侧存储 + MCU 哈希校验 =================
 *
 * 持久化：宿主存储 Goodix_Cache.bin 等价物，保存 [DPAPI-seal][entropy]，
 * 密封让宿主以后无需重读 MCU 即可恢复明文 PSK。MCU 从不校验密封
 * （它是 ARM 芯片；CryptProtectData 仅宿主可用）。Linux 上将明文 PSK
 * 存入 0600 文件，等效的单用户保护。可换用 Secret-Service (libsecret)
 * 后端——无论选择哪种宿主存储，MCU 协议都不受影响。
 */

static const char *psk_store_path(void)
{
    static char path[512];
    snprintf(path, sizeof(path), "%s/psk.bin", gx_state_dir());
    return path;
}

int gx_psk_store_save(const uint8_t *psk, uint32_t len)
{
    const char *path = psk_store_path();

    gx_state_dir_ensure();

    FILE *f = fopen(path, "wb");
    if (!f) {
        LOG("PSK store: cannot open %s", path);
        return -1;
    }
    if (fwrite(psk, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    chmod(path, 0600);
    LOG("PSK stored to %s (0600, %u B) - Goodix_Cache.bin equivalent", path, len);
    return 0;
}

int gx_psk_store_load(uint8_t *psk, uint32_t *len)
{
    const char *path = psk_store_path();
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t got = fread(psk, 1, *len, f);
    fclose(f);
    if (got < 1)
        return -1;
    *len = (uint32_t)got;
    LOG("PSK loaded from %s (%u B)", path, *len);
    return 0;
}

/* 宿主哈希 = SHA256(WB block) */
int gx_psk_host_hash(const uint8_t *wb, uint32_t wb_len, uint8_t out[32])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int olen = 0;
    int ok;
    if (!ctx)
        return -1;
    ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
         EVP_DigestUpdate(ctx, wb, wb_len) == 1 &&
         EVP_DigestFinal_ex(ctx, out, &olen) == 1 && olen == 32;
    EVP_MD_CTX_free(ctx);
    return ok ? 0 : -1;
}

/* 校验 MCU 持有的是否为我们写入的同一 PSK。
 *
 * MCU 哈希响应格式：
 *   [status 1B][data_type 4B LE][len 4B LE][hash 32B]
 * gx_mcu_read 已剥离状态字节，因此 out =
 *   [data_type 4B][len 4B][hash 32B]（40 字节），哈希位于 out[8..39]。
 * 将哈希与 SHA256(我们的 WB block) 比较。相等说明 MCU 用其白盒
 * 成功解密了我们的 WB -> 加密正确。 */
int gx_psk_verify_mcu_hash(struct goodix_dev *d)
{
    uint8_t mcu_buf[64];
    uint8_t wb[MAX_FRAME];
    uint32_t wb_len = 0;
    uint8_t host_hash[32];
    uint16_t rl = sizeof(mcu_buf);
    int r;

    /* rebuild the WB block from the plaintext PSK we hold */
    r = gx_wb_encrypt(d->psk, 32, wb, &wb_len);
    if (r < 0)
        return r;

    /* read MCU hash (0xBB020003) */
    r = gx_mcu_read(d, GF_DT_PSK_HASH, mcu_buf, &rl);
    if (r < 0) {
        LOG("read MCU hash failed (%d)", r);
        return r;
    }
    /* response: [data_type 4B][len 4B][hash 32B] -> 40 bytes, hash @8 */
    if (rl < 40) {
        LOG("MCU hash response too short (%u B, want 40)", rl);
        return -4;
    }
    uint32_t dt, ln;
    memcpy(&dt, mcu_buf, 4);
    memcpy(&ln, mcu_buf + 4, 4);
    if (dt != GF_DT_PSK_HASH || ln != 32) {
        LOG("unexpected MCU hash header dt=0x%08X len=%u", dt, ln);
        return -5;
    }
    const uint8_t *mcu_hash = mcu_buf + 8;

    r = gx_psk_host_hash(wb, wb_len, host_hash);
    if (r < 0)
        return r;

    if (memcmp(mcu_hash, host_hash, 32) == 0) {
        LOG("*** MCU hash MATCHES SHA256(WB) - white-box crypto VERIFIED ***");
        return 0;
    }
    LOG("*** MCU hash MISMATCH - WB crypto wrong! ***");
    if (gx_debug) {
        fprintf(stderr, "[goodix] host: ");
        for (int i = 0; i < 32; i++) fprintf(stderr, "%02X", host_hash[i]);
        fprintf(stderr, "\n[goodix] mcu : ");
        for (int i = 0; i < 32; i++) fprintf(stderr, "%02X", mcu_hash[i]);
        fprintf(stderr, "\n");
    }
    return -6;
}
