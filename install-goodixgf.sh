#!/usr/bin/env bash
# install-goodixgf.sh - 构建并安装带 goodixgf 驱动（SIGFM 匹配）的 libfprint
#                      （27c6:5125 / 27c6:5135）
#
# 用法：
#   bash install-goodixgf.sh [libfprint 源码目录]
# 默认源码目录：<goodix-linux>/libfprint——goodix-fp-linux-dev 社区的 libfprint fork
# （libfprint-sigfm 分支，跟踪 origin/0x00002a/libfprint-sigfm），
# 注册/比对使用 SIGFM 算法（OpenCV SIFT 关键点 + 几何一致性投票）。
#
# 支持 Debian/Ubuntu（apt）与 CentOS/Rocky/RHEL（dnf/yum，自动启用 EPEL）。
# SIGFM 依赖 OpenCV >= 4.5（SIFT 位于主模块）与 doctest（算法自测），要求：
#   Ubuntu 22.04+ / Debian 12+（libopencv-dev >= 4.5）
#   EPEL 9（opencv-devel >= 4.5）
#
# 流程：
#   1. 安装构建依赖（含 opencv4 >= 4.5、doctest）+ fprintd
#   2. 把 goodixgf 驱动 + 协议核心复制进 <源码>/libfprint/drivers/goodixgf/
#   3. patch meson.build（fork 的 driver_sources / optional_deps，幂等）
#   4. 编译安装到 /usr（SONAME 不变，系统 fprintd 直接复用）
#   5. 重载 udev、重启 fprintd
#
# 之后（升级后已注册模板需删除再重新注册）：
#   fprintd-delete $USER
#   fprintd-enroll -f right-index-finger $USER && fprintd-verify $USER
set -euo pipefail

GOODIX_LINUX="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$GOODIX_LINUX/libfprint}"

if [ ! -f "$SRC/libfprint/meson.build" ] || [ ! -f "$SRC/meson_options.txt" ]; then
  echo "!! $SRC 不是 libfprint 源码树根目录（缺 libfprint/meson.build）" >&2
  echo "   用法：bash $0 [libfprint 源码目录]" >&2
  exit 1
fi
if [ ! -f "$SRC/libfprint/sigfm/sigfm.h" ]; then
  echo "!! $SRC 不在 SIGFM 分支（缺 libfprint/sigfm/sigfm.h）。" >&2
  echo "   请在 $SRC 内切换到 SIGFM 分支后重跑：" >&2
  echo "     git -C \"$SRC\" checkout libfprint-sigfm" >&2
  exit 1
fi
SRC="$(cd "$SRC" && pwd)"

# Windows 拷来的源码常带 CRLF 行尾：gcc/meson 能容忍，但 g-ir-scanner
# 的 gtk-doc 解析器会直接报 "identifier not found on the first line"。
# 先把全树文本文件行尾规范为 LF（git clone 的 fork 已是 LF，此步幂等）。
echo "==> 规范化源码行尾（CRLF -> LF）"
if command -v dos2unix >/dev/null 2>&1; then
  find "$SRC" -type f \( -name '*.[ch]' -o -name '*.cpp' -o -name '*.hpp' -o \
    -name '*.build' -o -name '*.txt' -o -name '*.py' -o -name '*.md' -o \
    -name '*.ver' -o -name '*.xml' -o -name '*.policy' \) -print0 | \
    xargs -0 -r dos2unix -q --
else
  find "$SRC" -type f -print0 | while IFS= read -r -d '' f; do
    if grep -Iq $'\r' "$f" 2>/dev/null; then
      sed -i 's/\r$//' "$f"
    fi
  done
fi

# ---------------- 1. 依赖 ----------------
echo "==> [1/5] 安装依赖（含 SIGFM 所需的 OpenCV>=4.5 / doctest）"
if command -v apt >/dev/null 2>&1; then
  PKG=apt
  sudo apt update
  sudo apt install -y meson ninja-build gcc g++ pkg-config python3 dos2unix \
    libglib2.0-dev libgusb-dev libusb-1.0-0-dev libpixman-1-dev \
    libmbedtls-dev libssl-dev zlib1g-dev libudev-dev \
    gobject-introspection libgirepository1.0-dev \
    libopencv-dev doctest-dev \
    fprintd libpam-fprintd
elif command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; then
  PKG=$(command -v dnf || command -v yum)
  # mbedtls / fprintd / opencv / doctest 在 EPEL（Rocky: crb 或 powertools 可能也需启用）
  sudo $PKG install -y epel-release 2>/dev/null || true
  sudo $PKG config-manager --set-enabled crb 2>/dev/null || \
    sudo $PKG config-manager --set-enabled powertools 2>/dev/null || true
  sudo $PKG install -y meson ninja-build gcc gcc-c++ pkgconf-pkg-config python3 dos2unix \
    glib2-devel libgusb-devel libusbx-devel pixman-devel \
    openssl-devel zlib-devel systemd-devel \
    gobject-introspection-devel \
    opencv-devel doctest-devel || \
    echo "!! 部分依赖安装失败（EPEL 未就绪？），请手动安装 opencv-devel/doctest-devel 后重跑" >&2
  sudo $PKG install -y mbedtls-devel fprintd fprintd-pam || \
    echo "!! mbedtls-devel/fprintd 安装失败（EPEL 未就绪？），请手动安装后重跑" >&2
else
  echo "!! 未识别的包管理器：请手动安装 meson/ninja/gcc/glib2/libgusb/libusb/pixman/openssl/zlib/mbedtls/gobject-introspection/fprintd/libopencv-dev/libdoctest-dev" >&2
  exit 1
fi

# SIGFM 的 SIFT 需要 OpenCV >= 4.5（libfprint/sigfm 编译期硬依赖）。
OPENCV_VER="$(pkg-config --modversion opencv4 2>/dev/null || true)"
if [ -z "$OPENCV_VER" ]; then
  echo "!! 找不到 pkg-config 包 opencv4：libopencv-dev/opencv-devel 未安装或版本过旧" >&2
  exit 1
fi
# 数值比较版本（按 . 拆分成整数逐段比较；字符串比较会把 "4.10.0" 误判为
# < "4.5"，因为 '1'<'5'）。
if ! awk -v v="$OPENCV_VER" '
    function cmp(a, b, as_, bs_, n, m, i) {
      n = split(a, as_, "."); m = split(b, bs_, ".");
      for (i = 1; i <= (n > m ? n : m); i++) {
        ai = (i <= n) ? as_[i] + 0 : 0;
        bi = (i <= m) ? bs_[i] + 0 : 0;
        if (ai < bi) return -1;
        if (ai > bi) return 1;
      }
      return 0;
    }
    BEGIN { exit !(cmp(v, "4.5") >= 0) }'; then
  echo "!! OpenCV 版本 $OPENCV_VER < 4.5，SIGFM 需要 >= 4.5（SIFT 位于主模块）。" >&2
  echo "   请升级发行版（Ubuntu 22.04+ / Debian 12+ / EPEL9+）或手动安装新版 OpenCV 后重试。" >&2
  exit 1
fi
echo "    OpenCV $OPENCV_VER 满足 SIGFM 要求"

# ---------------- 2. 复制驱动 ----------------
echo "==> [2/5] 复制 goodixgf 驱动与协议核心 -> $SRC"
D="$SRC/libfprint/drivers/goodixgf"
mkdir -p "$D/core"
cp "$GOODIX_LINUX/src/goodixgf.c" "$D/"
cp "$GOODIX_LINUX/src/transport.c" \
   "$GOODIX_LINUX/src/goodix_frame.c" \
   "$GOODIX_LINUX/src/goodix_cmd.c" \
   "$GOODIX_LINUX/src/goodix_psk.c" \
   "$GOODIX_LINUX/src/goodix_tls.c" \
   "$GOODIX_LINUX/src/goodix_fwupdate.c" \
   "$GOODIX_LINUX/src/goodix_init.c" \
   "$GOODIX_LINUX/src/goodix_capture.c" \
   "$GOODIX_LINUX/src/goodix_base.c" \
   "$GOODIX_LINUX/src/goodix_otp.c" \
   "$GOODIX_LINUX/src/goodix_imgproc.c" \
   "$D/core/"
cp "$GOODIX_LINUX/include/goodix.h" \
   "$GOODIX_LINUX/include/goodix_imgproc.h" \
   "$GOODIX_LINUX/include/goodix_fw.h" "$D/core/"

# 复制的驱动文件若来自 Windows checkout 可能带 CRLF（g-ir-scanner 的 gtk-doc
# 解析器不认 \r），与源码树同样做行尾规范化。
if command -v dos2unix >/dev/null 2>&1; then
  find "$D" -type f -name '*.[ch]' -print0 | xargs -0 -r dos2unix -q --
else
  find "$D" -type f -name '*.[ch]' -print0 | while IFS= read -r -d '' f; do
    if grep -Iq $'\r' "$f" 2>/dev/null; then
      sed -i 's/\r$//' "$f"
    fi
  done
fi

# ---------------- 3. patch meson（幂等，锚点对应该 fork 的实际结构） ----------------
echo "==> [3/5] patch meson.build"
python3 - "$SRC" <<'PYEOF'
import sys

src = sys.argv[1]

# --- 3a. 根 meson.build：为 goodixgf 追加 optional_deps ---
# fork 构建系统用 default_drivers + driver_helper_mapping + optional_deps，
# 无 drivers_info dict；goodixgf 的 mbedtls/openssl
# 依赖在此注入（白盒 AES/HMAC 用 OpenSSL libcrypto）。
p = src + '/meson.build'
s = open(p).read()
if "'goodixgf'" not in s:
    anchor = "if udev_rules.disabled()\n"
    block = """# goodixgf: TLS-PSK 通道（mbedtls）+ 白盒 AES/HMAC（OpenSSL libcrypto）。
if 'goodixgf' in drivers
  optional_deps += [
    cc.find_library('mbedtls'),
    cc.find_library('mbedx509'),
    cc.find_library('mbedcrypto'),
    cc.find_library('crypto'),
  ]
endif

"""
    if anchor not in s:
        sys.exit("!! 根 meson.build 中找不到 'if udev_rules.disabled()' 锚点（fork 版本变化？），"
                 "请手动在 optional_deps 收集后加 goodixgf 依赖块")
    s = s.replace(anchor, block + anchor, 1)
    open(p, 'w').write(s)

# --- 3b. libfprint/meson.build：driver_sources 注册（fork 用数组格式） ---
p = src + '/libfprint/meson.build'
s = open(p).read()
if "'goodixgf'" not in s:
    anchor = "    'goodixmoc' :\n        [ 'drivers/goodixmoc/goodix.c', 'drivers/goodixmoc/goodix_proto.c' ],\n"
    entry = """    'goodixgf' :
        [ 'drivers/goodixgf/goodixgf.c',
          'drivers/goodixgf/core/transport.c',
          'drivers/goodixgf/core/goodix_frame.c',
          'drivers/goodixgf/core/goodix_cmd.c',
          'drivers/goodixgf/core/goodix_psk.c',
          'drivers/goodixgf/core/goodix_tls.c',
          'drivers/goodixgf/core/goodix_fwupdate.c',
          'drivers/goodixgf/core/goodix_init.c',
          'drivers/goodixgf/core/goodix_capture.c',
          'drivers/goodixgf/core/goodix_base.c',
          'drivers/goodixgf/core/goodix_otp.c',
          'drivers/goodixgf/core/goodix_imgproc.c' ],
"""
    if anchor not in s:
        sys.exit("!! libfprint/meson.build 中找不到 goodixmoc 源文件锚点（fork 结构变化？），"
                 "请手动在 driver_sources 中加 goodixgf 条目")
    s = s.replace(anchor, anchor + entry, 1)
    open(p, 'w').write(s)
print('patched: optional_deps + driver_sources (goodixgf)')
PYEOF

# ---------------- 4. 编译安装 ----------------
echo "==> [4/5] 编译安装"
if command -v dpkg >/dev/null 2>&1; then
  LIBDIR="lib/$(gcc -dumpmachine)"     # Debian/Ubuntu 多架构目录
else
  LIBDIR="lib64"                        # CentOS/Rocky/RHEL
fi
# 保持 introspection 开启（fork 默认值）：tests/meson.build 依赖它生成驱动测试，
# 关闭可能在配置阶段触发问题。GIR 生成问题已由行尾规范化根治。
GIR_OPT="-Dintrospection=true"

cd "$SRC"
# 注意：fork 的 meson_options.txt 没有 installed-tests 选项，不能传
# -Dinstalled-tests=false，否则 meson setup 报未知选项。
if [ -d build ]; then
  meson setup build --reconfigure \
    --prefix=/usr --libdir="$LIBDIR" \
    -Ddrivers=goodixgf -Ddoc=false \
    -Dudev_rules=enabled -Dudev_hwdb=enabled "$GIR_OPT"
else
  meson setup build \
    --prefix=/usr --libdir="$LIBDIR" \
    -Ddrivers=goodixgf -Ddoc=false \
    -Dudev_rules=enabled -Dudev_hwdb=enabled "$GIR_OPT"
fi
ninja -C build
sudo ninja -C build install
sudo ldconfig

# ---------------- 5. udev + fprintd ----------------
echo "==> [5/5] 安装 udev 规则 + 重启 fprintd"
# fork 的 udev 规则不含 goodixgf 的 VID/PID，必须安装项目自带的规则，
# 否则非 root 用户（plugdev 组）无法访问设备。
sudo install -m 0644 "$GOODIX_LINUX/70-goodix.rules" /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
sudo systemctl restart fprintd

cat <<DONE

安装完成。已注册模板与当前驱动/图像链绑定，升级后请先删除再重新注册：
  fprintd-delete \$USER                        # 删除已注册指纹
  journalctl -u fprintd -b | grep -i goodix   # 应能看到 goodixgf 打开设备
  fprintd-enroll -f right-index-finger \$USER  # 注册（首次 open 会供应 PSK/采基线，别放手指；5 次按压位置须分散）
  fprintd-verify \$USER                        # 验证

调优：
  GOODIX_DEBUG=1 观察 "sigfm score %d/%d" 日志。当前阈值
  GF_SIGFM_SCORE_THRESHOLD=20（src/goodixgf.c 中修改后重跑本脚本）。
  阈值过低易误接受、过高易误拒绝。

注意：
  - 包管理器升级 libfprint 会覆盖本安装，升级后重跑本脚本即可
  - 用 goodix-cli 调试前先 sudo systemctl stop fprintd（USB 独占）
  - SIGFM 提取要求每帧至少 25 个 SIFT 关键点，关键点不足会触发 retry-scan
DONE
