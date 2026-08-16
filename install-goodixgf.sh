#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# install-goodixgf.sh - 构建并安装带 goodixgf 驱动（SIGFM 匹配）的 libfprint
#                      （27c6:5125 / 27c6:5135）
#
# 用法：
#   bash install-goodixgf.sh [libfprint 源码目录]
# 默认源码目录：<goodix-linux>/libfprint——goodix-fp-linux-dev 社区的 libfprint fork
# （libfprint-sigfm 分支，跟踪 origin/0x00002a/libfprint-sigfm），
# 注册/比对使用 SIGFM 算法（OpenCV SIFT 关键点 + 几何一致性投票）。
#
# 支持 Debian/Ubuntu（apt）与 Fedora / CentOS/Rocky/RHEL（dnf/yum）。
# Fedora 40+ 直接用主仓库包（libusb1-devel / zlib-ng-compat-devel）；
# RHEL 系自动启用 EPEL。
# SIGFM 依赖 OpenCV >= 4.5（SIFT 位于主模块）与 doctest（算法自测），要求：
#   Ubuntu 22.04+ / Debian 12+（libopencv-dev >= 4.5）
#   Fedora 40+ 主仓库 / EPEL 9（opencv-devel >= 4.5）
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
echo "==> [1/5] 检查/安装依赖（含 SIGFM 所需的 OpenCV>=4.5 / doctest）"
# 先探测关键构建依赖是否已就绪：全部满足则跳过包管理器安装
# （Arch/CachyOS 等已自带完整构建链与库的场景，也符合"不引入未装依赖"的约束）。
# mbedtls 在多数发行版无 .pc 文件，根 meson.build 用 cc.find_library() 定位，
# 这里只检查 pkg-config 能见到的模块。
DEP_OK=1
for m in glib-2.0 gio-unix-2.0 gobject-2.0 gusb libusb-1.0 pixman-1 \
         openssl zlib libudev gobject-introspection-1.0 \
         girepository-2.0; do
  pkg-config --exists "$m" 2>/dev/null || { DEP_OK=0; break; }
done
# OpenCV：opencv4/opencv5 任一 .pc 存在即可；个别打包未带 .pc 时
# （meson 会改走 CMake 配置定位），再用 pacman 包版本兜底
if [ "$DEP_OK" = "1" ] && ! { pkg-config --exists opencv4 2>/dev/null || \
                              pkg-config --exists opencv5 2>/dev/null || \
                              { command -v pacman >/dev/null 2>&1 && \
                                pacman -Q opencv >/dev/null 2>&1; }; }; then
  DEP_OK=0
fi
# doctest 是 header-only 库，多数发行版不提供 .pc：Arch 查包，其它发行版查 .pc
if [ "$DEP_OK" = "1" ] && ! { pkg-config --exists doctest 2>/dev/null || \
                              { command -v pacman >/dev/null 2>&1 && \
                                pacman -Q doctest >/dev/null 2>&1; }; }; then
  DEP_OK=0
fi
# 构建工具链；dos2unix 仅用于行尾规范化（脚本有 sed 兜底），缺失不阻断
for t in meson ninja gcc g++ pkg-config python3; do
  command -v "$t" >/dev/null 2>&1 || { DEP_OK=0; break; }
done

if [ "$DEP_OK" = "1" ]; then
  echo "    构建依赖已全部就绪，跳过包管理器安装"
elif command -v pacman >/dev/null 2>&1; then
  # Arch/CachyOS。g++/pkg-config/python3 由 gcc/pkgconf/python 提供；
  # libudev 在 systemd-libs；OpenCV 包名恒为 opencv（4.x 提供 opencv4.pc，
  # 5.x 提供 opencv5.pc）。
  # mbedtls 包名随发行版而异：Arch 同时提供 mbedtls（4.x）与 mbedtls3
  # （3.x LTS），CachyOS 为 mbedtls（4.x）；优先 mbedtls，缺失时回退
  # mbedtls3（用 sync DB 探测，-Si 不需要网络）。
  # zlib 与 zlib-ng-compat（提供 zlib ABI，CachyOS 默认）互斥：已装
  # zlib-ng-compat 时不再请求 zlib，否则 pacman 弹出"删除 zlib-ng-compat
  # 吗？[y/N]"（--noconfirm 取默认 N）导致整个事务失败。
  ZLIB_PKG=""
  pacman -Q zlib-ng-compat >/dev/null 2>&1 || ZLIB_PKG="zlib"
  MBEDTLS_PKG=mbedtls
  pacman -Si mbedtls >/dev/null 2>&1 || MBEDTLS_PKG=mbedtls3
  sudo pacman -S --needed --noconfirm meson ninja gcc pkgconf python \
    dos2unix glib2 libgusb libusb pixman openssl $ZLIB_PKG systemd-libs \
    gobject-introspection doctest opencv "$MBEDTLS_PKG" fprintd || \
    echo "!! 部分依赖安装失败，请手动安装后重跑" >&2
elif command -v apt >/dev/null 2>&1; then
  PKG=apt
  sudo apt update
  # systemd 头文件包名随发行版而异：Debian 12 / Ubuntu 22.04 为
  # libsystemd-dev，Debian 13 / Ubuntu 24.04+ 改名 systemd-dev；
  # 装错名字会让整个 apt install 失败（set -e 直接中断），先探测。
  SYSTEMD_DEV=libsystemd-dev
  apt-cache show systemd-dev >/dev/null 2>&1 && SYSTEMD_DEV=systemd-dev
  # gobject-introspection 1.80+ 改名（libgirepository-1.0 -> 2.0）：
  # Debian 13+ / Ubuntu 24.04+ 为 libgirepository-2.0-dev，
  # libgirepository1.0-dev 已退化为过渡包；先探测新名。
  GIREPO_DEV=libgirepository1.0-dev
  apt-cache show libgirepository-2.0-dev >/dev/null 2>&1 && GIREPO_DEV=libgirepository-2.0-dev
  sudo apt install -y meson ninja-build gcc g++ pkg-config python3 dos2unix \
    libglib2.0-dev libgusb-dev libusb-1.0-0-dev libpixman-1-dev \
    libmbedtls-dev libssl-dev zlib1g-dev libudev-dev \
    gobject-introspection "$GIREPO_DEV" \
    libopencv-dev doctest-dev "$SYSTEMD_DEV" \
    fprintd libpam-fprintd
elif command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; then
  PKG=$(command -v dnf || command -v yum)
  # Fedora 40+ 起 libusbx/zlib 包名已变更（libusb1-devel / zlib-ng-compat-devel），
  # 且 mbedtls/opencv/doctest/fprintd 均在 Fedora 主仓库，无需启用 EPEL/CRB。
  # RHEL/Rocky/CentOS 保持原名（EPEL 提供 opencv-devel/doctest-devel/mbedtls-devel）。
  FEDORA_GE40=0
  if [ -r /etc/os-release ]; then
    . /etc/os-release
    if [ "${ID:-}" = "fedora" ]; then
      # VERSION_ID 缺失/非数字（如 Rawhide）时按新包名处理（支持的 Fedora 均已 >= 40）
      FEDORA_GE40=$(awk -v v="${VERSION_ID:-99}" 'BEGIN{print (v+0 >= 40) ? 1 : 0}')
    fi
  fi
  if [ "$FEDORA_GE40" = "1" ]; then
    sudo $PKG install -y meson ninja-build gcc gcc-c++ pkgconf-pkg-config python3 dos2unix \
      glib2-devel libgusb-devel libusb1-devel pixman-devel \
      openssl-devel zlib-ng-compat-devel systemd-devel \
      gobject-introspection-devel \
      opencv-devel doctest-devel mbedtls-devel fprintd fprintd-pam || \
      echo "!! 部分依赖安装失败，请手动安装后重跑" >&2
  else
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
  fi
else
  echo "!! 未识别的包管理器：请手动安装 meson/ninja/gcc/glib2/libgusb/libusb/pixman/openssl/zlib/mbedtls/gobject-introspection/fprintd/libopencv-dev/libdoctest-dev" >&2
  exit 1
fi

# SIGFM 的 SIFT 需要 OpenCV >= 4.5（libfprint/sigfm 编译期硬依赖）。
# pkg-config 包名随 OpenCV 版本而异：4.x 为 opencv4，5.x 为 opencv5
# （features2d 模块更名为 features，兼容头仍在），与发行版无关
# （Debian/Ubuntu/RHEL/Fedora 目前为 4.x，CachyOS/Arch 滚动为 5.x）。
# 个别打包未带 .pc 时 meson 会改走 CMake 配置定位，这里用包版本兜底。
OPENCV_VER="$(pkg-config --modversion opencv4 2>/dev/null || true)"
if [ -z "$OPENCV_VER" ]; then
  OPENCV_VER="$(pkg-config --modversion opencv5 2>/dev/null || true)"
fi
if [ -z "$OPENCV_VER" ] && command -v pacman >/dev/null 2>&1; then
  OPENCV_VER="$(pacman -Q opencv 2>/dev/null | awk '{print $2}')"
fi
if [ -z "$OPENCV_VER" ]; then
  echo "!! 找不到 OpenCV：libopencv-dev/opencv-devel 未安装或版本过旧" >&2
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
  echo "   请升级发行版（Ubuntu 22.04+ / Debian 12+ / Fedora / EPEL9+ / Arch）或手动安装新版 OpenCV 后重试。" >&2
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
   "$GOODIX_LINUX/src/goodix_crc.c" \
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
# 已存在 goodixgf 条目时（本脚本之前跑过）整体替换，保证新增/删除核心源
# 文件（如 goodix_crc.c）能幂等生效；不存在时在 goodixmoc 之后追加。
p = src + '/libfprint/meson.build'
s = open(p).read()
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
          'drivers/goodixgf/core/goodix_imgproc.c',
          'drivers/goodixgf/core/goodix_crc.c' ],
"""
if "'goodixgf'" in s:
    import re
    pat = re.compile(r"    'goodixgf' :\n        \[.*?\],\n", re.S)
    if not pat.search(s):
        sys.exit("!! libfprint/meson.build 中 goodixgf 条目格式异常，请手动检查")
    s = pat.sub(entry, s, count=1)
else:
    anchor = "    'goodixmoc' :\n        [ 'drivers/goodixmoc/goodix.c', 'drivers/goodixmoc/goodix_proto.c' ],\n"
    if anchor not in s:
        sys.exit("!! libfprint/meson.build 中找不到 goodixmoc 源文件锚点（fork 结构变化？），"
                 "请手动在 driver_sources 中加 goodixgf 条目")
    s = s.replace(anchor, anchor + entry, 1)
open(p, 'w').write(s)

# --- 3c. sigfm/meson.build：OpenCV pkg-config 名 opencv4/opencv5 双兼容 ---
# pkg-config 名随 OpenCV 版本而异：4.x 为 opencv4，5.x 为 opencv5
# （features2d 模块同时更名为 features，兼容头 features2d.hpp 仍在），
# 与发行版无关（Debian/Fedora 当前 4.x，CachyOS/Arch 滚动 5.x）。
# 两者均满足 SIFT >= 4.5，逐一探测。fork 可能因 meson submodule 重置
# 回到 dependency('opencv4')，此步幂等保证每次运行都适配。
p = src + '/libfprint/sigfm/meson.build'
s = open(p).read()
if "'opencv5'" not in s:
    old = "opencv = dependency('opencv4', required: true)"
    new = """# OpenCV 的 pkg-config 名随版本而异：4.x 为 opencv4，5.x 为 opencv5
# （features2d 模块更名为 features，兼容头仍在）。
opencv = dependency('opencv4', required: false)
if not opencv.found()
  opencv = dependency('opencv5', required: true)
endif"""
    if old not in s:
        sys.exit("!! sigfm/meson.build 中找不到 opencv4 依赖行（fork 结构变化？），"
                 "请手动在 sigfm/meson.build 中加 opencv5 回退")
    s = s.replace(old, new, 1)
    open(p, 'w').write(s)
print('patched: optional_deps + driver_sources (goodixgf) + sigfm opencv fallback')
PYEOF

# ---------------- 4. 编译安装 ----------------
echo "==> [4/5] 编译安装"
if command -v dpkg >/dev/null 2>&1; then
  LIBDIR="lib/$(gcc -dumpmachine)"     # Debian/Ubuntu 多架构目录
elif command -v pacman >/dev/null 2>&1; then
  LIBDIR="lib"                          # Arch/CachyOS：统一 /usr/lib
else
  LIBDIR="lib64"                        # Fedora/CentOS/Rocky/RHEL
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

# 让 fprintd 常驻：默认 ExecStart=/usr/lib/fprintd 无 --no-timeout，
# fprintd 在空闲约 30 秒后退出，登录界面等待期间设备会被关闭，
# 用户首次触摸往往落空、需重按，表现为"开机指纹登录很慢"
# （而 sudo 时 fprintd 是热启动/设备已打开，所以按上就好）。
# 加 drop-in 注入 --no-timeout：守护进程常驻、设备保持打开，
# 登录/锁屏/验证都能按上即响应。
FPRINTD_SVC=/usr/lib/systemd/system/fprintd.service
FPRINTD_DROPIN=/etc/systemd/system/fprintd.service.d/keepalive.conf
if [ -f "$FPRINTD_SVC" ]; then
  # 从服务文件提取实际 fprintd 可执行路径（Arch/Debian/Ubuntu 不同）
  FPRINTD_EXEC="$(sed -n 's/^ExecStart=//p' "$FPRINTD_SVC" | head -1)"
  FPRINTD_EXEC="${FPRINTD_EXEC%% *}"
  if [ -n "$FPRINTD_EXEC" ] && [ -x "$FPRINTD_EXEC" ]; then
    sudo mkdir -p "$(dirname "$FPRINTD_DROPIN")"
    if [ ! -f "$FPRINTD_DROPIN" ]; then
      printf '[Service]\nExecStart=\nExecStart=%s --no-timeout\n' \
        "$FPRINTD_EXEC" | sudo tee "$FPRINTD_DROPIN" >/dev/null
      echo "    fprintd 常驻已启用（--no-timeout，设备保持打开）"
    else
      echo "    fprintd 常驻 drop-in 已存在，跳过"
    fi
    sudo systemctl daemon-reload
  else
    echo "    !! 未找到 fprintd 可执行文件，跳过常驻配置" >&2
  fi
fi
sudo systemctl restart fprintd

cat <<DONE

安装完成。已注册模板与当前驱动/图像链绑定，升级后请先删除再重新注册：
  fprintd-delete \$USER                        # 删除已注册指纹
  journalctl -u fprintd -b | grep -i goodix   # 应能看到 goodixgf 打开设备
  fprintd-enroll -f right-index-finger \$USER  # 注册（首次 open 会供应 PSK/采基线，别放手指；动态采样 3-8 次，位置分散）
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
