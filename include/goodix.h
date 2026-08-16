/* goodix.h - Goodix USB 指纹传感器驱动（完整的设备驱动实现）
 *
 * 在 Linux 上实现完整的设备驱动协议：
 *   - libusb 批量传输层
 *   - 帧与命令层
 *   - 初始化序列
 *   - 经 MCU 的 PSK 管理
 *   - PSK 的白盒 AES
 *   - mbedtls PSK TLS 服务器通道
 *   - 图像采集
 *
 * 设备：27c6:5125 / 27c6:5135，GF5116M (Milan)，CDC ACM 批量传输。
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GOODIX_H
#define GOODIX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

extern int gx_debug;  /* USB 十六进制转储开关（GOODIX_DEBUG=1） */

/* 共享日志宏（所有模块） */
#define LOG(fmt, ...) fprintf(stderr, "[goodix] " fmt "\n", ##__VA_ARGS__)

/* 固件更新“MCU 软复位、停止初始化”标记（0xFFDFFFF3） */
#define FW_SOFT_RESET (-2097165)

#define GOODIX_VID         0x27c6
#define GOODIX_PID_5125    0x5125
#define GOODIX_PID_5135    0x5135

/* 端点由 27c6:5125 的配置描述符验证（pkt9）：USB CDC-ACM，2 个接口：
 *   iface 0（CDC comm）：EP 0x82 INT-IN(8B)   -- 通知
 *   iface 1（CDC data）：EP 0x01 BULK-OUT(64B)
 *                       EP 0x81 BULK-IN(64B) -- 数据通道
 * 使用 iface 1 上的批量端点 0x01/0x81。 */
#define GOODIX_EP_OUT      0x01    /* bulk OUT 64B, interface 1 */
#define MAX_FRAME        20480   /* TLS 记录（16K）/ 图像帧（19K） */
#define GOODIX_EP_IN       0x81    /* bulk IN 64B, interface 1 */
#define GOODIX_IFACE_DATA  1

/* ---- 帧类型 ---- */
#define GF_TYPE_PLAIN   0xA0
#define GF_TYPE_TLS     0xB0
#define GF_NO_CHECK     0x88    /* 帧尾值：跳过校验和 */

/* ---- 帧偏移 ---- */
#define GF_HDR_TYPE     0
#define GF_HDR_LEN      1       /* 2 字节小端负载长度 */
#define GF_HDR_CKS      3       /* b0+b1+b2 */
#define GF_CMD_BYTE     4       /* (cmd0<<4)|(cmd1<<1) */
#define GF_DATA_LEN     5       /* 2 字节小端：数据长度 + 1 */
#define GF_DATA         7

/* USB 命令字节（命令字节 = (cmd0<<4)|(cmd1<<1)，|1 = 更多位） */
enum {
    GF_CMD_RESET_SENSOR    = 0xA2,  /* (10,1) data={1,20} */
    GF_CMD_CLEAR_APP       = 0xA4,  /* (10,2) data={0,0} */
    GF_CMD_READ_OTP        = 0xA6,  /* (10,3) data={0,0} */
    GF_CMD_EVK_VERSION     = 0xA8,  /* (10,4) data={0,0} */
    GF_CMD_MCU_STATE       = 0xAF,  /* (10,7) data={0x55,ts16,0,0}, more=1 */
    GF_CMD_CHIPREG_READ    = 0x82,  /* (8,1)  data={0,reg16,len16} */
    GF_CMD_SETMODE_IDLE    = 0x20,  /* (2,0) */
    GF_CMD_SETMODE_CAPTURE = 0x36,  /* (3,3) */
    GF_CMD_DOWNLOAD_CONFIG = 0x90,  /* (9,0)  data=cfg(224B) */
    GF_CMD_SET_DRIVER_STATE= 0x97,  /* (9,3)  data={state,2} */
    GF_CMD_TLS_SERVER_INIT = 0xD1,  /* (13,0) data={0,0}, more=1 */
    GF_CMD_TLS_HANDSHAKE   = 0xD4,  /* (13,2) data={0,0} */
    GF_CMD_MCU_READ        = 0xE4,  /* (14,2) data={type32,0} */
    GF_CMD_MCU_WRITE       = 0xE0,  /* (14,0) */
    GF_CMD_FW_SLICE        = 0xF0,  /* (15,0) {off32,len32,data} */
    GF_CMD_FW_FINISH       = 0xF4,  /* (15,2) {0,0,len32,crc32} */
};

/* SetMode */
enum {
    GF_MODE_IDLE = 0,
    GF_MODE_FDT_DOWN = 1,
    GF_MODE_FDT_UP = 2,
    GF_MODE_CAPTURE = 3,
};

/* 响应类型（响应字节的高半字节） */
enum {
    GF_RESP_IMAGE = 2,
    GF_RESP_FDT = 3,
    GF_RESP_NAV = 5,
    GF_RESP_REGRW = 8,
    GF_RESP_CONFIG = 9,
    GF_RESP_OTHER = 10,
    GF_RESP_NOTICE = 12,
    GF_RESP_PRODUCTION = 13,
    GF_RESP_PROD_RESP = 14,
};

/* 传感器几何参数。按 chipid 选择：
 *   type 2  MilanG   (chipid 0x2208): 176x54,  frame 19008
 *   type 3  MilanL   (chipid 0x2205): 112x132, frame 29568
 *   type 12 ChicagoHS(0x2503/0x2504): 80x64,   frame 10240
 *   type 14 ChicagoT (0x2505-0x2508/0x2510): 36x160, frame 11520
 * 目标设备（27c6:5125 GF5116M）为 MilanG。 */
#define GF_IMG_W    176     /* MilanG 默认值 */
#define GF_IMG_H    54
#define GF_IMG_SIZE (GF_IMG_W * GF_IMG_H * 2)   /* 19008 */
#define GF_IMG_MAX  29568   /* 最大帧（MilanL 112x132x2） */

/* 生产数据类型 */
/* 生产数据类型：
 *  0xBB020002 = PSK 数据块：[seal][entropy 8B][0xBB010003 wb 块]
 *  0xBB020003 = MCU 由 WB 解密出的 PSK 计算得到的 PSK 哈希（32B）
 *  写 TLV:     [0xBB010002][len][seal][entropy][0xBB010003][len][wb] */
#define GF_DT_PSK_DATA   0xBB010002u  /* 主机 PSK 数据（seal+entropy） */
#define GF_DT_PSK_HASH   0xBB020003u  /* MCU 计算出的 WB 哈希（校验 PSK 有效性） */
#define GF_DT_PSK_WB     0xBB010003u  /* WB 块存储段（写密钥的第二个 TLV） */

/* ---- device ---- */
struct goodix_dev {
    void *usb_ctx;
    void *usb_devh;
    uint16_t vid, pid;

    /* 批量端点（已验证 0x01 OUT / 0x81 IN） */
    uint8_t ep_out;
    uint8_t ep_in;

    /* 传感器信息 */
    uint8_t evk[64];
    uint16_t chipid;
    uint8_t sensor_type;
    uint16_t img_w, img_h, img_size;

    /* PSK（32B，由 MCU 经白盒 AES 提供） */
    uint8_t psk[32];
    size_t psk_len;
    bool have_psk;

    /* TLS（mbedtls） */
    void *tls;          /* TLS 上下文 */
    bool tls_inited;    /* TLS 握手完成，设备侧已确认 */

    /* 采集 */
    uint8_t image[GF_IMG_MAX];
    bool have_image;
    volatile int capture_stop;      /* gx_capture_cancel 置位，采集循环检查 */
    /* 可选采集事件回调（libfprint 对接用）：FDT down/up 推送帧到达时
     * 在采集上下文同步触发。见 gx_capture_set_event_cb。 */
    void (*event_cb)(struct goodix_dev *d, int event, void *user);
    void *event_cb_user;

    /* 初始化状态 */
    bool mcu_inited;      /* MCU 初始化完成 */
    bool fp_inited;       /* 传感器初始化完成 */
    bool have_config;     /* 配置下载成功 */
    uint8_t besd;         /* BESD 使能标志 */

    /* OTP + FDT 基线 */
    uint8_t otp[256];       /* OTP 读取（0xA6）内容 */
    uint16_t otp_len;       /* 有效长度：type 12/14 = 64 */
    uint8_t fdt_delta;      /* OTP 解析出的 FDT delta（默认 21） */
    uint16_t fdt_tcode;     /* OTP 解析出的 tcode（默认 128） */
    uint8_t fdt_down_base[12]; /* FDT-down 阈值表（变换后） */
    uint8_t fdt_up_base[12];   /* FDT-up 阈值表 */
    bool base_valid;        /* base 文件加载成功且 OTP 匹配 / 采样成功 */
    bool base_dirty;        /* 运行时从 FDT IRQ 学到了新基线，待落盘 */

    /* 无手指基线图像（对应 goodix.dat 的 imagebase 段），
     * 主机侧手指判别的比较基准。
     * init 期用 SetMode Image 直接曝光采一帧并存档；
     * 温度漂移（判别返回 0）时重采更新。 */
    uint8_t img_base[GF_IMG_MAX];
    bool img_base_valid;    /* 基线图像可用（文件加载或 init 采样成功） */

    /* TLS 解密数据聚合缓冲（goodix_capture.c 的混合通道：B0 帧解密后的
     * 字节流先攒出完整 payload 再分流）。放设备结构里而不是文件级 static，
     * 保证多设备实例/重入安全。 */
    uint8_t rx_acc[3 + 0x6C00];
    size_t rx_acc_len;

    /* 采集接收工作缓冲（goodix_capture.c）：采集路径单线程顺序使用。
     * rx_scratch 是各采集函数（wait_image / gx_fdt_sample_base /
     * gx_wait_finger_up，互不并发）的 payload 工作区；rx_frame 是
     * next_payload 的 USB 帧重组缓冲。同样挂设备上避免文件级 static。 */
    uint8_t rx_scratch[3 + 0x6C00];
    uint8_t rx_frame[MAX_FRAME];
};

/* ---- 传输层 (transport.c) ---- */
int gx_transport_open(struct goodix_dev *d);
void gx_transport_close(struct goodix_dev *d);
int gx_usb_write(struct goodix_dev *d, const uint8_t *b, size_t n);
int gx_usb_read(struct goodix_dev *d, uint8_t *b, size_t cap, int tmo);
int gx_usb_reset(struct goodix_dev *d);

/* ---- 共享 CRC-32/MPEG-2（goodix_crc.c）----
 * poly 0x04C11DB7，初值/续值 crc（通常 0xFFFFFFFFu），MSB-first，无最终
 * 异或。图像线 CRC、goodix.dat 基线 CRC、固件 CRC 三者共用。 */
uint32_t gx_crc32_mpeg2(const uint8_t *data, size_t len, uint32_t crc);

/* 取消采集（gx_capture_cancel）后长流程（初始化唤醒重试、重枚举轮询、
 * 采集循环）据此快速退出，避免 fprintd stop/deactivate 等待数秒。 */
static inline int gx_stop_requested(struct goodix_dev *d)
{
    return d->capture_stop;
}

/* ---- 帧层 (goodix_frame.c) ---- */
int  gx_send_cmd_frame(struct goodix_dev *d, uint8_t cmd,
                       const uint8_t *data, uint16_t datalen);
int  gx_send_tls_frame(struct goodix_dev *d, const uint8_t *record, uint16_t rlen);
int  gx_read_frame(struct goodix_dev *d, uint8_t *frame, uint16_t *len, int tmo);
int  gx_send_nop(struct goodix_dev *d);

/* ---- 命令层 (goodix_cmd.c) ---- */
int gx_send_cmd(struct goodix_dev *d, uint8_t cmd,
                const uint8_t *data, uint16_t len, int tmo);
int gx_send_cmd_wait(struct goodix_dev *d, uint8_t cmd,
                     const uint8_t *data, uint16_t len,
                     uint8_t *rsp, uint16_t *rsp_len, int tmo);
int gx_set_driver_state(struct goodix_dev *d, uint8_t state);
/* MCU 生产读写 */
int gx_mcu_read(struct goodix_dev *d, uint32_t data_type, uint8_t *out, uint16_t *len);
int gx_mcu_write(struct goodix_dev *d, const uint8_t *data, uint16_t len);
/* 设备命令辅助函数 */
int gx_dev_reset(struct goodix_dev *d);
int gx_dev_evk(struct goodix_dev *d, uint8_t out[64]);
int gx_get_mcu_state(struct goodix_dev *d, uint8_t out[16]);
int gx_tls_server_cmd(struct goodix_dev *d);
int gx_tls_handshake_cmd(struct goodix_dev *d);

/* ---- 设备初始化 (goodix_init.c) ---- */
int gx_device_init(struct goodix_dev *d);

/* ---- PSK (goodix_psk.c) ---- */
int gx_psk_read_from_mcu(struct goodix_dev *d);          /* 读取 + WB 解密 */
int gx_psk_write_to_mcu(struct goodix_dev *d, const uint8_t *psk); /* 生成 + WB 加密 + 写入 */
int gx_wb_encrypt(const uint8_t *plain, uint32_t len, uint8_t *out, uint32_t *out_len);
int gx_wb_decrypt(const uint8_t *wb, uint32_t wb_len, uint8_t *plain, uint32_t *plain_len);
  /* PSK 主机侧存储（对应 Goodix_Cache.bin）。
   * 参考实现用 DPAPI（CryptProtectData）封装 PSK 做本地持久化；MCU 从不
   * 接触它。Linux 上我们把明文 PSK 以 0600 权限存入 gx_state_dir() 下的
   * 文件——等价的单用户保护。也可换用 Secret-Service（libsecret）后端，
   * 无需改动 MCU 协议。 */
  int gx_psk_store_save(const uint8_t *psk, uint32_t len);
  int gx_psk_store_load(uint8_t *psk, uint32_t *len);
  /* 主机哈希 = SHA256(整个 WB 块) */
  int gx_psk_host_hash(const uint8_t *wb, uint32_t wb_len, uint8_t out[32]);
  /* 校验：读取 MCU 哈希 0xBB020003 并与 SHA256(我们的 WB) 比较，
   * 证明 MCU 正确解密了我们的 WB 块（WB 加密正确）。 */
  int gx_psk_verify_mcu_hash(struct goodix_dev *d);

/* ---- TLS (goodix_tls.c) ---- */
int gx_tls_server_init(struct goodix_dev *d);
int gx_tls_handshake(struct goodix_dev *d);
int gx_tls_reconnect(struct goodix_dev *d);   /* 重连（0xDA 请求） */
int gx_tls_recv(struct goodix_dev *d, uint8_t *data, size_t cap, int tmo);
/* 采集等待模式的混合通道支持（采集通道的行为：A0 明文帧直接进入帧处理
 * 流程，B0 帧进 mbedtls 解密后再进入同一流程）：
 *   gx_tls_capture_mode(1) 后 tls_bio_recv 不再自己读 USB（避免把明文
 *   推送帧吞掉），只 drain 注入缓冲；调用方用 gx_read_frame 收帧，
 *   B0 记录用 gx_tls_feed 注入，再用 gx_tls_recv 取解密数据。 */
void gx_tls_capture_mode(struct goodix_dev *d, int on);
int  gx_tls_feed(struct goodix_dev *d, const uint8_t *record, size_t len);

/* ---- 采集 (goodix_capture.c) ---- */
int gx_capture(struct goodix_dev *d, uint8_t *img, uint16_t *size);
/* FDT 基线采样：FDT-manual (0x36 [9,1,表]) 让 MCU 现场采样无手指 FDT
 * 基线并推送 IrqStatus=0x100 帧；学习为 down 阈值表。
 * 成功返回 0 且 d->base_valid=true。 */
int gx_fdt_sample_base(struct goodix_dev *d);
/* 图像基线采样：SetMode Image (0x20 [1,0]) 直接曝光采一帧无手指图像
 * 存入 d->img_base（不进 FDT 判别循环）。
 * 成功返回 0 且 d->img_base_valid=true。 */
int gx_capture_base_image(struct goodix_dev *d);
/* 主机侧手指判别——新帧与无手指基线图像做分块统计比较。
 * img 为解包后的 16bit LE 帧（w*h*2 字节）。
 * 返回：1=手指按下（接受），2=空触发，3=坏帧，
 *       0=温度漂移（需重建基线），-1=无基线可判。 */
int gx_fdt_check_finger(struct goodix_dev *d, const uint8_t *img);
/* 图像预处理（主机侧比对用，非设备协议路径）：gx_imgproc_to8bit /
 * gx_imgproc_params 见 goodix_imgproc.h（文件末尾 include）。 */
/* 采集事件（gx_capture_set_event_cb 的 event 参数） */
enum {
    GX_EV_FINGER_DOWN = 1,      /* FDT IrqStatus=0x2 推送帧 */
    GX_EV_FINGER_UP   = 2,      /* FDT IrqStatus=0x200 推送帧 */
};
void gx_capture_set_event_cb(struct goodix_dev *d,
                             void (*cb)(struct goodix_dev *d, int event,
                                        void *user),
                             void *user);
/* 取消进行中的采集（gx_capture/gx_capture_base_image/gx_wait_finger_up
 * 会在下一个收包周期返回 -4）；下次 gx_capture 自动清零。 */
void gx_capture_cancel(struct goodix_dev *d);
/* FDT UP 布防后等设备推送 IrqStatus=0x200（手指抬起）。
 * 返回 0=抬起；-2=超时；-4=取消。 */
int gx_wait_finger_up(struct goodix_dev *d, int timeout_ms);

/* ---- OTP 解析 + 配置补丁 (goodix_otp.c) ----
 * 用 OTP 工厂校准值补丁 224B 配置表（tcode/fdt delta/fdtOffset/曝光 DAC），
 * 必须在配置下载之前调用。
 * delta_out 回传 FDT delta（默认 21）。返回 1=OTP 有效已补丁。 */
int gx_otp_patch_config(struct goodix_dev *d, uint8_t *cfg, uint8_t *delta_out);

/* ---- FDT 基线持久化 (goodix_base.c) ----
 * Linux 端基线的落盘：文件布局与 goodix.dat 一致
 *   [OTP 256B][fdt_base 12B][navbase 3200B][imagebase 10240B][CRC32/MPEG-2]
 * 校验：CRC + OTP 逐字节匹配（不同设备/老化后自动作废）。
 * 学习：FDT IRQ 帧携带设备实测基线（down/up 阈值表）。 */
/* 状态目录（psk.bin / goodix.dat 的存放处）：
 *   euid==0 -> /var/lib/fprint/goodix
 *     （fprintd 的 systemd 沙箱 ProtectHome=true 会屏蔽 /root 与 /home，
 *      而 /var/lib/fprint 是 fprintd 自身的可写数据目录；
 *      CLI sudo 与 fprintd 因此共享同一份 PSK/基线）
 *   其它    -> $HOME/.config/goodix */
const char *gx_state_dir(void);
int  gx_state_dir_ensure(void);
int  gx_base_load(struct goodix_dev *d);
int  gx_base_save(struct goodix_dev *d);
/* 原始 12B 基线合法性检查 */
int  gx_fdt_base_check(const uint8_t raw[12]);
/* fdt-up IRQ 原始数据 -> down 阈值表 */
int  gx_fdt_learn_down_base(struct goodix_dev *d, const uint8_t raw[12]);
/* fdt-down IRQ 原始数据 -> up 阈值表 */
int  gx_fdt_learn_up_base(struct goodix_dev *d, const uint8_t raw[12],
                          uint16_t touchflag);

/* ---- 固件更新 (goodix_fwupdate.c) ---- */
int gx_fw_update(struct goodix_dev *d, uint8_t evk[64]);

#include "goodix_imgproc.h"

#endif /* GOODIX_H */

