# libfprint 对接指南（goodixgf 驱动 + SIGFM 匹配）

> **快速安装**：`bash install-goodixgf.sh` 一键完成（默认源码目录就是本仓库随附的 `libfprint`——goodix-fp-linux-dev 社区的 libfprint fork，`libfprint-sigfm` 分支，无需联网）。支持 Debian/Ubuntu 与 CentOS/Rocky/RHEL（自动启用 EPEL）。下文是手动流程与原理说明，锚点均对应附带的 fork 源码树实测。

本目录材料：

- `src/goodixgf.c` — FpImageDevice 驱动（异步 glue：协议核心跑在 worker 线程，事件经 main context 上报；注册/比对由 FpImageDevice + **SIGFM** 完成）
- `install-goodixgf.sh` — 一键安装脚本（依赖 + 复制 + patch + 编译安装）
- `uninstall-goodixgf.sh` — 彻底卸载脚本
- `libfprint/` — goodix-fp-linux-dev fork 完整源码（已在 `libfprint-sigfm` 分支，跟踪 `origin/0x00002a/libfprint-sigfm`）

## SIGFM 匹配算法

**SIGFM**（`libfprint/sigfm/`，OpenCV **SIFT** 关键点 + BFMatcher 比率匹配 + 角度/长度几何一致性投票）从 `FpImage` 的 8bit `data` 直接提取 SIFT 关键点/描述子，模板序列化为 `SigfmImgInfo`。驱动只需声明 `img_class->algorithm = FPI_DEVICE_ALGO_SIGFM`，基类自动完成特征提取、模板存储与比对分发，无需改算法代码。

## 设计要点（遵循上游最佳实践）

| 方面 | 做法 |
|---|---|
| 基类 | `FpImageDevice`，`algorithm = FPI_DEVICE_ALGO_SIGFM`，主机侧 SIGFM 比对，enroll/verify/identify/list/delete 由基类实现 |
| 异步模型 | 协议核心是阻塞式同步 I/O，全部放到 `GThread` worker；`fpi_image_device_*` 只在 main context 调用（`g_idle_add` 回投 + GObject 引用防 UAF） |
| 手指状态 | 核心 FDT 事件回调：按下 → `FP_FINGER_STATUS_PRESENT`；0x34 布防后等 IrqStatus=0x200 → `FP_FINGER_STATUS_NONE` |
| 图像 | 16bit 原帧经 `gx_imgproc_to8bit()`（`src/goodix_imgproc.c` 参数化管线：有符号基线相减 → 低频平场校正 → 百分位拉伸 → 可选 SIGFM 反锐化增强）转 80x64 灰度图并直接交给 SIGFM 做 SIFT 提取；`ppmm=500/25.4`，`score_threshold=20` |
| 匹配 | SIGFM 从 `image->data` 直接提取特征（SIFT 对极性不敏感），`FPI_IMAGE_COLORS_INVERTED` 仅影响调试 dump |
| 取消 | `gx_capture_cancel()` 让 worker 在 200ms 内退出，deactivate/close 立即返回 |

## SIGFM 的硬性依赖

`libfprint/sigfm/meson.build` 无条件构建 `libsigfm` 与 `sigfm-tests`：

- **OpenCV ≥ 4.5**：`cv::SIFT::create()` 自 OpenCV 4.5 起位于主模块。Ubuntu 20.04（OpenCV 4.2）不满足；要求 **Ubuntu 22.04+ / Debian 12+**，RPM 系需 **EPEL 9**。安装脚本在构建前用 `pkg-config --modversion opencv4` 做了版本门禁。
- **doctest**（≥2.0，<3.0）：`sigfm-tests` 自测用。Debian/Ubuntu 包名 `doctest-dev`，RPM 系 `doctest-devel`。
- fork 的 `meson_options.txt` **没有** `installed-tests` 选项，meson setup 时不能传 `-Dinstalled-tests=false`（否则报未知选项）。

## 集成步骤（脚本自动化内容的逐条说明）

1. 依赖：

   ```sh
   # Debian/Ubuntu（22.04+）
   sudo apt install meson ninja-build gcc g++ pkg-config python3 dos2unix \
     libglib2.0-dev libgusb-dev libusb-1.0-0-dev libpixman-1-dev \
     libmbedtls-dev libssl-dev zlib1g-dev libudev-dev \
     gobject-introspection libgirepository1.0-dev \
     libopencv-dev doctest-dev \
     fprintd libpam-fprintd

   # CentOS/Rocky/RHEL（mbedtls/fprintd/opencv/doctest 在 EPEL）
   sudo dnf install -y epel-release
   sudo dnf config-manager --set-enabled crb
   sudo dnf install -y meson ninja-build gcc gcc-c++ pkgconf-pkg-config python3 \
     glib2-devel libgusb-devel libusbx-devel pixman-devel \
     openssl-devel zlib-devel systemd-devel gobject-introspection-devel \
     opencv-devel doctest-devel mbedtls-devel fprintd fprintd-pam
   ```

2. 复制驱动到 fork 源码树 `libfprint/drivers/goodixgf/`：`goodixgf.c` + `core/`（goodix-linux 的 `src/*.c` 去掉 `main.c`，以及 `include/goodix.h`、`include/goodix_imgproc.h`、`include/goodix_fw.h`）。

3. patch meson（该 fork 构建系统结构，锚点已对齐）：

   - **根 `meson.build`**：fork 用 `default_drivers` 列表 + `driver_helper_mapping` + `optional_deps` 数组（无 `drivers_info` dict）。在 `optional_deps` 收集完毕处注入 goodixgf 的链接依赖（白盒 AES/HMAC 用 OpenSSL libcrypto，TLS 用 mbedtls）：

     ```meson
     if 'goodixgf' in drivers
       optional_deps += [
         cc.find_library('mbedtls'),
         cc.find_library('mbedx509'),
         cc.find_library('mbedcrypto'),
         cc.find_library('crypto'),
       ]
     endif
     ```

   - **`libfprint/meson.build`**：`driver_sources` 用数组格式，追加 `'goodixgf'` 条目（`goodixgf.c` + `core/*.c`）。驱动类型函数 `fpi_device_goodixgf_get_type` 由构建系统按 `-Ddrivers=goodixgf` 自动收集（`drivers_type_func`），与 `goodixgf.c` 的 `G_DEFINE_TYPE` 对应。

4. 编译安装：

   ```sh
   meson setup build --prefix=/usr \
     --libdir=lib/x86_64-linux-gnu   `# Debian/Ubuntu；RPM 系用 lib64` \
     -Ddrivers=goodixgf -Ddoc=false \
     -Dudev_rules=enabled -Dudev_hwdb=enabled -Dintrospection=true
   ninja -C build && sudo ninja -C build install && sudo ldconfig
   sudo udevadm control --reload-rules && sudo udevadm trigger
   sudo systemctl restart fprintd
   ```

5. 使用（升级驱动或调整图像预处理后，已注册模板需删除并重新注册）：

   ```sh
   fprintd-delete $USER
   fprintd-enroll -f right-index-finger $USER
   fprintd-verify $USER
   ```

   首次打开设备时驱动自动完成 PSK 供应与基线采样（等效 CLI 第一次 `--info`，约 5–15 秒，**不要在传感器上放手指**）；之后每次 open 约 1–2 秒。状态文件在 `/var/lib/fprint/goodix/`（fprintd 沙箱内可写）。

## 注意事项 / 已知限制

- **分支要求**：`libfprint` 必须处于 SIGFM 分支（`libfprint-sigfm`，跟踪 `origin/0x00002a/libfprint-sigfm`），脚本以 `libfprint/sigfm/sigfm.h` 存在性校验，不在该分支会给出明确提示。
- **模板兼容**：注册模板是序列化的 SIFT 关键点/描述子，与驱动/图像链版本强绑定；升级驱动或调整图像预处理后必须 `fprintd-delete $USER` 再重新 enroll。
- **score_threshold 标定**：SIGFM 分数是几何一致匹配对的计数，当前默认 `GF_SIGFM_SCORE_THRESHOLD=20`。真机标定：`GOODIX_DEBUG=1` 观察 `sigfm score %d/%d` 日志，收集同指/异指分数分布后再调，阈值过低易误接受、过高易误拒绝。
- **关键点下限**：`fp_image.c` 要求每帧 SIFT 关键点 ≥ 25，不足则报 "No enough keypoints found" 并触发 retry-scan。80x64 小图通常有足够纹理，但过糊/过暗样本可能触底，属正常重试。
- **OpenCV 版本**：Ubuntu 20.04 的 OpenCV 4.2 不含主模块 SIFT，必须 22.04+ / Debian 12+（或手动升级 OpenCV）。
- **图像预处理标定**：管线（`core/goodix_imgproc.c`，`gx_imgproc_to8bit`）各环节参数可用 `GOODIX_IMGPROC_*` 环境变量单独开关/调整（baseline 减差、低频平场、百分位区间、反锐化增强）。驱动默认走 `GX_IMGPROC_SIGFM_PARAMS`（**反锐化增强默认开启**，`GOODIX_IMGPROC_ENHANCE=none` 可关）：80x64 小图弱按压下 SIFT 关键点少且不可复现，增强提升脊谷对比度与关键点密度，缓解验证间歇性 no-match（匹配对 <5 时 `sigfm_match_score` 直接归 0）。标定流程：`GOODIX_DUMP_IMGPROC=1` 逐级查看 `raw8/base8/flat8/final8/enh8`，再用 `GOODIX_IMGPROC_BOOST`/`SIGMA` 调增强强度。**修改任何图像参数后必须 `fprintd-delete $USER` 重新 enroll。**
- **验证不稳定（偶发 verify-no-match）调优**：先确认是"得分 0"还是"低于阈值"——停 fprintd 后前台跑，观察 SIGFM 得分日志：

  ```sh
  sudo systemctl stop fprintd
  sudo env G_MESSAGES_DEBUG=all /usr/libexec/fprintd   # 或 /usr/lib/fprintd
  # 另开终端：fprintd-verify $USER，观察 "sigfm score N/20"
  ```

  得分 0 = 匹配对 <5（位置/关键点不重复）→ 靠注册时位置分散（默认 5 次，位置重复的按压由驱动拒绝重按）+ 增强解决；得分在 10-19 = 阈值略高 → 调低 `GF_SIGFM_SCORE_THRESHOLD`（goodixgf.c）后重装。验证时保持同一按压位置。
- **极性**：CLI `--capture` 保存的是 `gx_imgproc_to8bit()` 的 80x64 输出，与 SIGFM 提取输入一致。ChicagoHS 连续结构是亮脊线；SIGFM 从 `image->data` 直接提取（SIFT 对极性不敏感），`FPI_IMAGE_COLORS_INVERTED` 供上层按实际极性显示。不要按局部黑色串珠判断极性，那些是谷线内部的接触/电容变化。
- **suspend/resume**：恢复后 MCU 可能处于休眠态，核心的 GetEvkVersionWithRetry 恢复路径（0xA2 软复位唤醒）会自动处理。
- **fprintd 与 CLI 互斥**：两者都要独占 USB 接口；用 CLI 调试前先 `systemctl stop fprintd`。
- **包管理器升级 libfprint 会覆盖本安装**：升级后重跑脚本即可（幂等）。
