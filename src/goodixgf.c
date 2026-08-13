/* goodixgf.c - Goodix 27c6:5125 / 27c6:5135 的 libfprint 驱动
 *
 * 基于 FpImageDevice 的驱动，面向 Goodix 指纹模块（荣耀/华为笔记本，
 * GF_ST411SEC_APP 固件家族）。完整的线上协议（初始化、PSK 配发、白盒
 * AES、TLS-PSK 服务器通道、FDT 手指检测、12-bit 图像解包、主机侧手指
 * 判别）位于配套核心库（goodix.h / goodix_*.c），实现完整的设备驱动
 * 协议。本文件是 libfprint 胶水层：在工作线程上运行阻塞式核心逻辑，
 * 并通过主上下文上的 FpImageDevice API 上报结果。
 *
 * 采集流程：
 *   activate  -> 工作线程：布防 FDT down (0x32)
 *   手指落下  -> FDT 事件 IrqStatus=0x2  -> 上报 FP_FINGER_PRESENT
 *             -> 主机发送 SetMode Image (0x20)，MCU 曝光
 *             -> 图像帧（TLS 或明文推送）
 *             -> 解包 + CRC + 主机侧手指判别 (gx_fdt_check_finger)
 *             -> fpi_image_device_image_captured()
 *   手指抬起  -> FDT up (0x34，由核心布防) -> IrqStatus=0x200
 *             -> 上报 FP_FINGER_NONE -> 为下一阶段重新布防
 *
 * 匹配（enroll/verify/identify）由 FpImageDevice 通过 SIGFM（OpenCV SIFT
 * 关键点 + 几何一致性投票）在主机侧完成，经 goodix-fp-linux-dev fork
 * 上的 FPI_DEVICE_ALGO_SIGFM 选择。
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#define FP_COMPONENT "goodixgf"

#include <glib.h>
#include <string.h>

#include "drivers_api.h"

#include "core/goodix.h"

/* 协议核心支持其他传感器几何参数，但本 libfprint 图像配置仅为 80x64
 * ChicagoHS 校准与测试。 */
#define GOODIXGF_IMG_W  80
#define GOODIXGF_IMG_H  64

/* SIGFM 匹配阈值。SIGFM 得分是几何一致的匹配关键点对的数量：真实匹配
 * 得分在数百以上，误匹配得分为 0（低于 min_match）。新 enrollment 后用
 * GOODIX_DEBUG=1 日志（"sigfm score %d/%d"）在设备上校准。 */
#define GF_SIGFM_SCORE_THRESHOLD 20

/* SIGFM 直接对 image->data 原样提取 SIFT 关键点并忽略颜色标志；
 * FPI_IMAGE_COLORS_INVERTED 仅供上层按实际极性显示。 */
#define GF_IMAGE_FLAGS FPI_IMAGE_COLORS_INVERTED

/* 动态 enrollment —— 空间覆盖收敛 + 上下限封顶：
 *   80x64 单帧只覆盖手指一小块区域，SIGFM 模板由多帧 SIFT 关键点集合
 *   组成。位置分散的按压（见 gf_enroll_frame_dup 的多样性检查）逐步覆盖
 *   主要区域；连续 GF_ENROLL_DUP_STOP 次位置重复按压说明用户已无新位置
 *   可采，采满 GF_ENROLL_MIN_STAGES 后即以当前帧为收尾帧提前完成。
 *   下限保证基本覆盖，上限封顶避免无休止采集（基类 nr_enroll_stages 的
 *   初值设为 GF_ENROLL_MAX_STAGES，收敛后运行期下调）。 */
#define GF_ENROLL_MIN_STAGES  3
#define GF_ENROLL_MAX_STAGES  8
#define GF_ENROLL_DUP_STOP    2
#define GF_ENROLL_NPIX   (GOODIXGF_IMG_W * GOODIXGF_IMG_H)
/* enroll 多样性阈值：两帧 8bit 图像的平均绝对差低于该值即判为"同一位置
 * 重复按压"（SIFT 关键点几乎一致）。可经 GOODIX_ENROLL_MIN_DIFF 覆盖。 */
#define GF_ENROLL_MIN_DIFF  8.0

struct _FpiDeviceGoodixGF
{
  FpImageDevice   parent;

  struct goodix_dev *gx;          /* 核心设备状态（工作线程持有） */
  GThread          *worker;       /* 阻塞式核心工作线程 */
  gint              worker_stop;  /* 跨线程原子访问 */

  /* enroll 多样性控制：字段在 main context（gf_activate）初始化，
   * 之后仅在采集 worker 线程访问。 */
  gboolean          enroll;            /* 当前会话是否为 enroll */
  int               accepted_count;    /* 已接受的 enroll 帧数 */
  uint8_t           accepted[GF_ENROLL_MAX_STAGES][GF_ENROLL_NPIX]; /* 已接受帧 */
  gboolean          enroll_converged;  /* 空间覆盖已收敛，交付收尾帧直到完成 */
  int               dup_streak;        /* 连续位置重复按压次数 */
};

G_DECLARE_FINAL_TYPE (FpiDeviceGoodixGF, fpi_device_goodixgf,
                      FPI, DEVICE_GOODIXGF, FpImageDevice)
G_DEFINE_TYPE (FpiDeviceGoodixGF, fpi_device_goodixgf, FP_TYPE_IMAGE_DEVICE)

static const FpIdEntry id_table[] = {
  { .vid = 0x27c6, .pid = 0x5125 },
  { .vid = 0x27c6, .pid = 0x5135 },
  { .vid = 0,      .pid = 0      },
};

/* 判断新帧是否与某张已接受帧"同一位置重复"。80x64 单帧只覆盖手指一小块
 * 区域：位置相同则图像几乎一致（SIFT 关键点几乎一致），位置不同则差异
 * 显著。判据为图像平均绝对差，低于 GF_ENROLL_MIN_DIFF 判为重复。返回
 * TRUE = 与某帧重复，应拒绝交付并提示换位置重按。 */
static gboolean
gf_enroll_frame_dup (const uint8_t *new8,
                     const uint8_t (*accepted)[GF_ENROLL_NPIX], int n)
{
  const gchar *e = g_getenv ("GOODIX_ENROLL_MIN_DIFF");
  gdouble min_diff = e && *e ? g_ascii_strtod (e, NULL) : GF_ENROLL_MIN_DIFF;

  if (min_diff <= 0.0)
    return FALSE;               /* 显式禁用多样性检查 */
  for (int i = 0; i < n; i++)
    {
      gint64 sum = 0;
      for (int k = 0; k < GF_ENROLL_NPIX; k++)
        {
          gint d = (gint)new8[k] - (gint)accepted[i][k];
          sum += d < 0 ? -d : d;
        }
      if ((gdouble)sum / GF_ENROLL_NPIX < min_diff)
        return TRUE;
    }
  return FALSE;
}

/* ------------------------------------------------------------------ */
/* 主上下文转发辅助函数                                      */
/* ------------------------------------------------------------------ */

typedef struct
{
  FpiDeviceGoodixGF *self;
  FpImage           *image;          /* 用于 image_captured */
  GError            *error;          /* 用于 open/session 错误 */
  gboolean           present;        /* 用于手指状态 */
  gint               enroll_stages;  /* 0=不改；>0 在交付前下调 nr_enroll_stages */
} GfMainMsg;

static void
gf_msg_free (GfMainMsg *msg)
{
  g_clear_object (&msg->self);
  g_clear_object (&msg->image);
  g_clear_error (&msg->error);
  g_free (msg);
}

static gboolean
gf_on_finger_status (gpointer user_data)
{
  GfMainMsg *msg = user_data;
  fpi_image_device_report_finger_status (
      FP_IMAGE_DEVICE (msg->self),
      msg->present ? FP_FINGER_STATUS_PRESENT : FP_FINGER_STATUS_NONE);
  gf_msg_free (msg);
  return G_SOURCE_REMOVE;
}

static gboolean
gf_on_image_captured (gpointer user_data)
{
  GfMainMsg *msg = user_data;
  /* enroll 空间覆盖收敛：先下调基类 nr_enroll_stages 再交付收尾帧，保证
   * 基类在新帧成功提取后命中 enroll_stage == nr_enroll_stages 提前完成。
   * fpi_device_set_nr_enroll_stages 内部 g_object_notify，必须在主上下文
   * 调用（本回调即主上下文）。 */
  if (msg->enroll_stages > 0)
    fpi_device_set_nr_enroll_stages (FP_DEVICE (msg->self),
                                     msg->enroll_stages);
  fpi_image_device_image_captured (FP_IMAGE_DEVICE (msg->self),
                                   g_steal_pointer (&msg->image));
  gf_msg_free (msg);
  return G_SOURCE_REMOVE;
}

static gboolean
gf_on_open_complete (gpointer user_data)
{
  GfMainMsg *msg = user_data;
  fpi_image_device_open_complete (FP_IMAGE_DEVICE (msg->self),
                                  g_steal_pointer (&msg->error));
  gf_msg_free (msg);
  return G_SOURCE_REMOVE;
}

static gboolean
gf_on_retry_scan (gpointer user_data)
{
  GfMainMsg *msg = user_data;
  fpi_image_device_retry_scan (FP_IMAGE_DEVICE (msg->self),
                               FP_DEVICE_RETRY_GENERAL);
  gf_msg_free (msg);
  return G_SOURCE_REMOVE;
}

static void
gf_post (FpiDeviceGoodixGF *self, GSourceFunc func,
         FpImage *image, GError *error, gboolean present)
{
  GfMainMsg *msg = g_new0 (GfMainMsg, 1);
  /* worker 线程可能在 idle 回调执行前退出/设备被释放，持引用防 UAF */
  msg->self = g_object_ref (self);
  msg->image = image;
  msg->error = error;
  msg->present = present;
  g_idle_add (func, msg);
}

/* 交付一个收尾帧并在主上下文下调基类 nr_enroll_stages 到 target_stages。
 * 用于 enroll 收敛：把当前（重复）帧作为模板的最后一帧交给基类，同时把
 * nr_enroll_stages 改为 accepted_count + 1，使基类在下一次 minutiae_detected
 * 后提前完成。时序：仅在主上下文（gf_on_image_captured）中触发 setter，
 * 保证先改值后交付帧。 */
static void
gf_post_enroll_close (FpiDeviceGoodixGF *self, FpImage *image, gint target_stages)
{
  GfMainMsg *msg = g_new0 (GfMainMsg, 1);
  msg->self = g_object_ref (self);
  msg->image = image;
  msg->enroll_stages = target_stages;
  g_idle_add (gf_on_image_captured, msg);
}

/* ------------------------------------------------------------------ */
/* 核心 -> libfprint 事件回调（工作线程上下文）         */
/* ------------------------------------------------------------------ */

static void
gf_core_event_cb (struct goodix_dev *gxdev, int event, void *user)
{
  FpiDeviceGoodixGF *self = user;
  (void) gxdev;

  switch (event)
    {
    case GX_EV_FINGER_DOWN:
      gf_post (self, gf_on_finger_status, NULL, NULL, TRUE);
      break;
    case GX_EV_FINGER_UP:
      gf_post (self, gf_on_finger_status, NULL, NULL, FALSE);
      break;
    }
}

/* ------------------------------------------------------------------ */
/* 采集工作线程（阻塞式核心在此运行，绝不在主上下文） */
/* ------------------------------------------------------------------ */

static gpointer
gf_capture_worker (gpointer user_data)
{
  FpiDeviceGoodixGF *self = user_data;
  struct goodix_dev *gx = self->gx;
  uint8_t *img = g_malloc (GF_IMG_MAX);
  int empty_rounds = 0;

  fp_dbg ("capture worker started");

  while (!g_atomic_int_get (&self->worker_stop))
    {
      uint16_t sz = 0;
      int r = gx_capture (gx, img, &sz);

      if (g_atomic_int_get (&self->worker_stop) || r == -4)
        break;

      if (r == 0 && sz == (uint16_t) (gx->img_w * gx->img_h * 2))
        {
          FpImage *fimg = fp_image_new (gx->img_w, gx->img_h);
          struct gx_imgproc_params p = GX_IMGPROC_SIGFM_PARAMS;

          empty_rounds = 0;
          /* 16bit 原帧 -> 8bit：参数化管线（baseline 减差 / 低频平场 /
           * 百分位拉伸 + 反锐化增强）。驱动默认走 GX_IMGPROC_SIGFM_PARAMS
           * （开启增强）：80x64 小图弱按压/轻触下 SIFT 关键点少且不稳定，
           * 反锐化提升脊谷局部对比度与关键点可复现性，缓解验证间歇性
           * no-match（匹配对 <5 时 sigfm_match_score 直接归 0）。
           * 可用 GOODIX_IMGPROC_ENHANCE=none 关掉、GOODIX_IMGPROC_* 调参；
           * 修改任何图像参数后必须 fprintd-delete 重新 enroll。 */
          gx_imgproc_apply_env (&p);
          gx_imgproc_to8bit (gx, &p, img, fimg->data);
          /* 这是一个完整的按压帧。SIGFM 使用对尺度/旋转不敏感的 SIFT
           * 关键点；保持 FPI_IMAGE_PARTIAL 不设置，因为设置后会被裁剪
           * 边缘特征。 */
          fimg->flags = GF_IMAGE_FLAGS;

          /* 原始图纵向自相关的脊距约 9-10px，符合 500 DPI 指纹的典型尺度。
           * SIGFM 的 SIFT 特征对分辨率不敏感，无需放大；ppmm 仅供上层显示。 */
          fimg->ppmm = 500.0 / 25.4;

          /* enroll 动态采样 —— 空间覆盖收敛 + 上下限封顶：
           *   与某张已接受帧位置重复（关键点几乎一致）的按压对模板无
           *   增益，通常拒绝并提示换位置；但连续 GF_ENROLL_DUP_STOP 次
           *   重复且已采满 GF_ENROLL_MIN_STAGES 说明用户已无新位置可采，
           *   接受该帧作为收尾帧，并在主上下文把基类 nr_enroll_stages 下调
           *   为 accepted_count + 1，使基类提前完成。verify/identify 每次
           *   会话只交付一帧，不受影响。 */
          if (self->enroll && !self->enroll_converged)
            {
              if (self->accepted_count > 0 &&
                  gf_enroll_frame_dup (fimg->data, self->accepted,
                                       self->accepted_count))
                {
                  self->dup_streak++;
                  if (self->accepted_count >= GF_ENROLL_MIN_STAGES &&
                      self->accepted_count < GF_ENROLL_MAX_STAGES &&
                      self->dup_streak >= GF_ENROLL_DUP_STOP)
                    {
                      self->enroll_converged = TRUE;
                      fp_info ("enroll coverage converged at %d stages",
                               self->accepted_count + 1);
                      gf_post_enroll_close (self, fimg,
                                            self->accepted_count + 1);
                      gx_wait_finger_up (gx, 15000);
                      continue;
                    }
                  fp_warn ("enroll press too similar to a previous one - "
                           "move finger and try again");
                  gf_post (self, gf_on_retry_scan, NULL, NULL, FALSE);
                  g_object_unref (fimg);
                  /* 等手指抬起，基类 retry-scan 后回 await-finger-on，
                   * 等待下一次位置不同的按压。 */
                  gx_wait_finger_up (gx, 15000);
                  continue;
                }
              self->dup_streak = 0;
              if (self->accepted_count < GF_ENROLL_MAX_STAGES)
                memcpy (self->accepted[self->accepted_count++],
                        fimg->data, GF_ENROLL_NPIX);
            }

          fp_dbg ("captured %ux%u image, delivering at 500 DPI",
                  gx->img_w, gx->img_h);
          gf_post (self, gf_on_image_captured, fimg, NULL, FALSE);

          /* 等手指抬起（FDT up 由 gx_capture 收尾的 0x34 布防）；
           * FP_FINGER_STATUS_NONE 由事件回调在 IrqStatus=0x200 到达时
           * 上报。超时（-2/-3，MCU 未推抬起事件）时兜底上报一次 NONE，
           * 避免基类停在 AWAIT_FINGER_OFF；-4 为取消，基类已 deactivating，
           * 不再上报。 */
          {
            int up = gx_wait_finger_up (gx, 15000);
            if (up != 0 && up != -4)
              gf_post (self, gf_on_finger_status, NULL, NULL, FALSE);
          }
          continue;
        }

      /* 无帧/被判别拒绝：继续等（enroll 超时由 fprintd 侧控制，
       * deactivate 经 gx_capture_cancel 随时可中断）。TLS 通道错误
       * （-3）短暂退避后重试，避免热循环。 */
      if (r == -3)
        g_usleep (200 * 1000);
      if (++empty_rounds >= 50)
        {
          fp_warn ("no fingerprint frame after %d rounds, keep waiting",
                   empty_rounds);
          empty_rounds = 0;
        }
    }

  g_free (img);
  fp_dbg ("capture worker exiting");
  return NULL;
}

/* ------------------------------------------------------------------ */
/* open 工作线程：完整设备初始化（PSK + TLS + 基线）   */
/* ------------------------------------------------------------------ */

static gpointer
gf_open_worker (gpointer user_data)
{
  FpiDeviceGoodixGF *self = user_data;
  struct goodix_dev *gx = g_malloc0 (sizeof (*gx));
  int r;

  gx->vid = GOODIX_VID;
  gx->pid = 0;                      /* 0x5125/0x5135 任一（传输层扫描） */

  r = gx_transport_open (gx);
  if (r == 0)
    r = gx_device_init (gx);

  if (r == 0 &&
      (gx->img_w != GOODIXGF_IMG_W || gx->img_h != GOODIXGF_IMG_H))
    {
      fp_err ("unsupported sensor geometry %ux%u (expected %ux%u)",
              gx->img_w, gx->img_h, GOODIXGF_IMG_W, GOODIXGF_IMG_H);
      r = -5;
    }

  if (r == 0)
    {
      self->gx = gx;
      gx_capture_set_event_cb (gx, gf_core_event_cb, self);
      fp_info ("device ready: chipid 0x%04x, %ux%u, TLS %s",
               gx->chipid, gx->img_w, gx->img_h,
               gx->tls_inited ? "inited" : "plaintext");
      gf_post (self, gf_on_open_complete, NULL, NULL, FALSE);
    }
  else
    {
      gx_transport_close (gx);
      g_free (gx);
      gf_post (self, gf_on_open_complete, NULL,
               g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                            "goodix device init failed (%d)", r),
               FALSE);
    }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* FpImageDevice 虚拟函数                                          */
/* ------------------------------------------------------------------ */

static void
gf_img_open (FpImageDevice *dev)
{
  FpiDeviceGoodixGF *self = FPI_DEVICE_GOODIXGF (dev);

  g_atomic_int_set (&self->worker_stop, FALSE);
  self->worker = g_thread_new ("goodixgf-open", gf_open_worker, self);
}

static void
gf_img_close (FpImageDevice *dev)
{
  FpiDeviceGoodixGF *self = FPI_DEVICE_GOODIXGF (dev);

  g_atomic_int_set (&self->worker_stop, TRUE);
  if (self->gx)
    gx_capture_cancel (self->gx);
  g_clear_pointer (&self->worker, g_thread_join);

  if (self->gx)
    {
      gx_capture_set_event_cb (self->gx, NULL, NULL);
      gx_transport_close (self->gx);
      g_clear_pointer (&self->gx, g_free);
    }
  fpi_image_device_close_complete (dev, NULL);
}

static void
gf_activate (FpImageDevice *dev)
{
  FpiDeviceGoodixGF *self = FPI_DEVICE_GOODIXGF (dev);

  if (!self->gx)
    {
      fpi_image_device_activate_complete (
          dev, g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_OPEN,
                            "device not initialised"));
      return;
    }
  /* 已完成的 open 线程仍持有一个可 join 的 GThread 句柄。 */
  g_clear_pointer (&self->worker, g_thread_join);
  /* enroll 时启用多样性检查（按压位置须分散，动态采样见 gf_capture_worker）；
   * verify/identify/capture 每次会话只交付一帧，无需检查。 */
  self->enroll = fpi_device_get_current_action (FP_DEVICE (dev)) ==
                 FPI_DEVICE_ACTION_ENROLL;
  self->accepted_count = 0;
  self->dup_streak = 0;
  self->enroll_converged = FALSE;
  /* nr_enroll_stages 是实例字段，可能在上次 enroll 收敛时被运行期下调，
   * 每次会话开始复位到上限，保证动态采样有完整的收敛空间。gf_activate
   * 运行于主上下文，此处调用 setter 安全。 */
  fpi_device_set_nr_enroll_stages (FP_DEVICE (dev), GF_ENROLL_MAX_STAGES);
  g_atomic_int_set (&self->worker_stop, FALSE);
  self->worker = g_thread_new ("goodixgf-capture", gf_capture_worker, self);
  fpi_image_device_activate_complete (dev, NULL);
}

static void
gf_deactivate (FpImageDevice *dev)
{
  FpiDeviceGoodixGF *self = FPI_DEVICE_GOODIXGF (dev);

  g_atomic_int_set (&self->worker_stop, TRUE);
  if (self->gx)
    gx_capture_cancel (self->gx);
  g_clear_pointer (&self->worker, g_thread_join);
  fpi_image_device_deactivate_complete (dev, NULL);
}

/* ------------------------------------------------------------------ */
/* 类型注册                                                        */
/* ------------------------------------------------------------------ */

static void
fpi_device_goodixgf_init (FpiDeviceGoodixGF *self)
{
}

static void
fpi_device_goodixgf_class_init (FpiDeviceGoodixGFClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "Goodix fingerprint (27c6:5125/5135)";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  /* 80x64 传感器单帧只覆盖手指一小块区域，采用动态采样（空间覆盖收敛
   * + 上下限封顶）：初值设为 GF_ENROLL_MAX_STAGES，运行期在位置连续重复、
   * 且已采满 GF_ENROLL_MIN_STAGES 时下调到实际帧数提前完成。上限封顶避免
   * 无休止采集，下限保证基本覆盖（SIGFM 匹配对 <5 即得分 0）。 */
  dev_class->nr_enroll_stages = GF_ENROLL_MAX_STAGES;
  dev_class->temp_hot_seconds = -1;

  img_class->img_open = gf_img_open;
  img_class->img_close = gf_img_close;
  img_class->activate = gf_activate;
  img_class->deactivate = gf_deactivate;
  img_class->score_threshold = GF_SIGFM_SCORE_THRESHOLD;
  img_class->algorithm = FPI_DEVICE_ALGO_SIGFM;
  img_class->img_width = GOODIXGF_IMG_W;
  img_class->img_height = GOODIXGF_IMG_H;
}
