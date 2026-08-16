#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
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
#      - goodix 专用包直接移除（fprintd/libfprint/mbedtls/opencv/doctest）
#      - 显式安装的构建工具标记为 auto / asdeps，autoremove / 孤儿清理仅在
#        无其它软件依赖时移除（gcc/meson/glib/openssl 等可能被其它软件共用，
#        不强制误删）
#   5. 删除 goodix 状态文件（PSK/基线）与已注册指纹数据
#   6. ldconfig + udev 重载
#
# 支持 Debian/Ubuntu（apt）、CentOS/Rocky/RHEL（dnf/yum）与
# Arch/CachyOS（pacman）。
# 注意：python3 是系统组件（Ubuntu 与 Arch 皆然），不在清除列表。
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
if command -v pacman >/dev/null 2>&1; then
  # ---- Arch / CachyOS（pacman）----
  # goodix 专用包逐个独立移除：pacman 的 -R 是事务性的，若把多个包放进
  # 同一条命令且其中一个仍被其它软件依赖，整个事务会失败、一个都删不掉，
  # 所以必须逐个执行。fprintd 的 -Rns 会连带移除不再被依赖的 libfprint
  # （Arch 上 libfprint 仅被 fprintd 使用）。有其它依赖者则拒绝并跳过
  # （与 apt purge 的 || true 同理）。
  # mbedtls 包名随发行版而异：Arch 同时提供 mbedtls（4.x）与 mbedtls3
  # （3.x LTS），CachyOS 为 mbedtls（4.x）；按已安装者探测。
  MBEDTLS_PKG=mbedtls
  pacman -Q mbedtls3 >/dev/null 2>&1 && MBEDTLS_PKG=mbedtls3
  for p in fprintd doctest "$MBEDTLS_PKG" opencv; do
    if pacman -Q "$p" >/dev/null 2>&1; then
      sudo pacman -Rns --noconfirm "$p" 2>/dev/null || \
        echo "!! $p 仍被其它软件依赖或未安装，跳过" >&2
    fi
  done
  # 构建/运行依赖标记为"作为依赖安装"（等价 apt-mark auto）——只标记
  # 本项目安装脚本引入、且 Arch 上多半无其它使用者的包；刻意不碰
  # gcc/python/glib2/openssl/zlib/systemd-libs 等核心共享组件。
  # 随后仅清理列表中已无人依赖的孤儿（等价 apt autoremove），
  # 不碰系统上原有的其它孤儿。
  for p in meson ninja pkgconf dos2unix libgusb libusb pixman \
           gobject-introspection; do
    if pacman -Q "$p" >/dev/null 2>&1; then
      sudo pacman -D --asdeps "$p" 2>/dev/null || true
    fi
  done
  # 一次查询孤儿清单，仅清理列表中已无人依赖的包
  ORPHANS="$(pacman -Qtdq 2>/dev/null || true)"
  for p in meson ninja pkgconf dos2unix libgusb libusb pixman \
           gobject-introspection; do
    if printf '%s\n' "$ORPHANS" | grep -qx "$p"; then
      sudo pacman -Rns --noconfirm "$p" 2>/dev/null || true
    fi
  done
elif command -v apt >/dev/null 2>&1; then
  # 安装脚本显式 `apt install` 的包（含构建工具与 -dev 包）标记为 auto，
  # autoremove 只会移除"不再被任何已装包依赖"的包——不残留也不误伤共享工具。
  # gobject-introspection 1.80+ 改名：新包名为 libgirepository-2.0-dev
  GIREPO_DEV=libgirepository1.0-dev
  apt-cache show libgirepository-2.0-dev >/dev/null 2>&1 && GIREPO_DEV=libgirepository-2.0-dev
  sudo apt-mark auto \
    fprintd libpam-fprintd libfprint-2-2 \
    meson ninja-build gcc g++ pkg-config dos2unix \
    libglib2.0-dev libgusb-dev libusb-1.0-0-dev libpixman-1-dev \
    libmbedtls-dev libssl-dev zlib1g-dev libudev-dev \
    gobject-introspection "$GIREPO_DEV" \
    libopencv-dev doctest-dev 2>/dev/null || true
  # goodix 专用包强制 purge（基本不被其它软件共用）
  sudo apt purge -y \
    fprintd libpam-fprintd libfprint-2-2 \
    libmbedtls-dev libopencv-dev doctest-dev || true
  sudo apt autoremove --purge -y
elif command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; then
  PKG=$(command -v dnf || command -v yum)
  # Fedora 40+ 包名已变更（libusb1-devel / zlib-ng-compat-devel / 主仓库
  # mbedtls-devel），RHEL/Rocky/CentOS 保持原名（EPEL）；
  # 探测逻辑与 install-goodixgf.sh 保持一致。
  FEDORA_GE40=0
  if [ -r /etc/os-release ]; then
    . /etc/os-release
    if [ "${ID:-}" = "fedora" ]; then
      FEDORA_GE40=$(awk -v v="${VERSION_ID:-99}" 'BEGIN{print (v+0 >= 40) ? 1 : 0}')
    fi
  fi
  if [ "$FEDORA_GE40" = "1" ]; then
    LIBUSB_DEV=libusb1-devel
    ZLIB_DEV=zlib-ng-compat-devel
  else
    LIBUSB_DEV=libusbx-devel
    ZLIB_DEV=zlib-devel
  fi
  # dnf remove 遇到列表里未安装的包会整个事务失败（|| true 救不回来），
  # 用 skip_missing_names_on_remove 让未安装的包名自动跳过。
  sudo $PKG remove -y --setopt=skip_missing_names_on_remove=True \
    fprintd fprintd-pam libfprint \
    meson ninja-build gcc gcc-c++ pkgconf-pkg-config dos2unix \
    glib2-devel libgusb-devel "$LIBUSB_DEV" pixman-devel \
    openssl-devel "$ZLIB_DEV" systemd-devel \
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
echo "卸载完成：源码 libfprint、fprintd、依赖包（共享工具由 autoremove/孤儿清理兜底）、"
echo "udev 规则、状态文件、指纹数据已清除。"
