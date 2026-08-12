#!/usr/bin/env bash
# uninstall-goodixgf.sh - 完全卸载 goodixgf 驱动的 libfprint 安装及其全部依赖
#
# 用法：bash uninstall-goodixgf.sh [libfprint 源码目录]
# 默认源码目录：<goodix-linux>/libfprint（goodix-fp-linux-dev fork，SIGFM 分支）
#
# 做的事：
#   1. 停止并禁用 fprintd
#   2. ninja uninstall（移除源码安装到 /usr 的 libfprint 库/udev 规则/hwdb）
#   3. 删除 udev 规则 /etc/udev/rules.d/70-goodix.rules
#   4. 移除安装脚本（install-goodixgf.sh）拉入的全部依赖：
#      - goodix 专用包直接 purge（fprintd/libfprint/mbedtls/opencv/doctest）
#      - 显式安装的构建工具标记为 auto，autoremove 仅在无其它软件依赖时移除
#        （gcc/meson/glib/openssl 等可能被其它软件共用，不强制误删）
#   5. 删除 goodix 状态文件（PSK/基线）与已注册指纹数据
#   6. ldconfig + udev 重载
#
# 注意：python3 是 Ubuntu 系统组件，不在清除列表（卸载会破坏系统）。
set -euo pipefail

GOODIX_LINUX="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$GOODIX_LINUX/libfprint}"

echo "==> [1/6] 停止并禁用 fprintd"
sudo systemctl stop fprintd 2>/dev/null || true
sudo systemctl disable fprintd 2>/dev/null || true

echo "==> [2/6] ninja uninstall（移除源码安装的文件）"
if [ -d "$SRC/build" ]; then
  sudo ninja -C "$SRC/build" uninstall || \
    echo "!! ninja uninstall 有残留" >&2
else
  echo "   未找到 $SRC/build，跳过"
fi

echo "==> [3/6] 删除 udev 规则"
sudo rm -f /etc/udev/rules.d/70-goodix.rules

echo "==> [4/6] 移除安装脚本拉入的全部依赖"
if command -v apt >/dev/null 2>&1; then
  # 安装脚本显式 `apt install` 的包（含构建工具与 -dev 包）标记为 auto，
  # autoremove 只会移除"不再被任何已装包依赖"的包——不残留也不误伤共享工具。
  sudo apt-mark auto \
    fprintd libpam-fprintd libfprint-2-2 \
    meson ninja-build gcc g++ pkg-config dos2unix \
    libglib2.0-dev libgusb-dev libusb-1.0-0-dev libpixman-1-dev \
    libmbedtls-dev libssl-dev zlib1g-dev libudev-dev \
    gobject-introspection libgirepository1.0-dev \
    libopencv-dev doctest-dev 2>/dev/null || true
  # goodix 专用包强制 purge（基本不被其它软件共用）
  sudo apt purge -y \
    fprintd libpam-fprintd libfprint-2-2 \
    libmbedtls-dev libopencv-dev doctest-dev || true
  sudo apt autoremove --purge -y
elif command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; then
  PKG=$(command -v dnf || command -v yum)
  sudo $PKG remove -y \
    fprintd fprintd-pam libfprint \
    meson ninja-build gcc gcc-c++ pkgconf-pkg-config dos2unix \
    glib2-devel libgusb-devel libusbx-devel pixman-devel \
    openssl-devel zlib-devel systemd-devel \
    gobject-introspection-devel opencv-devel doctest-devel \
    mbedtls-devel || true
  sudo $PKG autoremove -y || true
else
  echo "!! 未识别的包管理器，请手动卸载依赖包" >&2
fi

echo "==> [5/6] 删除状态文件与指纹数据"
sudo rm -rf /var/lib/fprint/goodix /root/.config/goodix "$HOME/.config/goodix"
sudo rm -rf /var/lib/fprint/* 2>/dev/null || true   # 已注册指纹数据
echo "   已删除 psk.bin / goodix.dat / 已注册指纹"

echo "==> [6/6] ldconfig + udev 重载"
sudo ldconfig
sudo udevadm control --reload-rules
sudo udevadm trigger

echo ""
echo "卸载完成：源码 libfprint、fprintd、依赖包（共享工具由 autoremove 兜底）、"
echo "udev 规则、状态文件、指纹数据已清除。"
