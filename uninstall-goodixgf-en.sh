#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# uninstall-goodixgf-en.sh - Completely uninstall libfprint with goodixgf driver and all its dependencies
#
# Usage: bash uninstall-goodixgf-en.sh [libfprint source directory]
# Default source directory: <goodix-linux>/libfprint (goodix-fp-linux-dev fork, SIGFM branch)
#
# What it does:
#   1. Stop and disable fprintd
#   2. ninja uninstall (remove libfprint libraries/udev rules/hwdb installed to /usr from source)
#   3. Delete udev rules /etc/udev/rules.d/70-goodix.rules
#   4. Remove all dependencies pulled in by the install script (install-goodixgf.sh):
#      - Goodix-specific packages purged directly (fprintd/libfprint/mbedtls/opencv/doctest)
#      - Explicitly installed build tools marked as auto, autoremove only removes them if no other software depends
#        (gcc/meson/glib/openssl etc. may be used by other software, won't force-remove them)
#   5. Delete goodix state files (PSK/baseline) and registered fingerprint data
#   6. ldconfig + udev reload
#
# Note: python3 is a Ubuntu system component and not removed (uninstalling would break the system).
set -euo pipefail

GOODIX_LINUX="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$GOODIX_LINUX/libfprint}"

echo "==> [1/6] Stopping and disabling fprintd"
sudo systemctl stop fprintd 2>/dev/null || true
sudo systemctl disable fprintd 2>/dev/null || true

echo "==> [2/6] ninja uninstall (removing files installed from source)"
if [ -d "$SRC/build" ]; then
  sudo ninja -C "$SRC/build" uninstall || \
    echo "!! ninja uninstall left some files" >&2
else
  echo "   $SRC/build not found, skipping"
fi

echo "==> [3/6] Deleting udev rules"
sudo rm -f /etc/udev/rules.d/70-goodix.rules

echo "==> [4/6] Removing all dependencies pulled in by install script"
if command -v apt >/dev/null 2>&1; then
  # Packages explicitly installed by install script (including build tools and -dev packages)
  # are marked as auto; autoremove only removes packages "no longer required by any installed package"
  # — no leftover files and no accidental removal of shared tools.
  sudo apt-mark auto \
    fprintd libpam-fprintd libfprint-2-2 \
    meson ninja-build gcc g++ pkg-config dos2unix \
    libglib2.0-dev libgusb-dev libusb-1.0-0-dev libpixman-1-dev \
    libmbedtls-dev libssl-dev zlib1g-dev libudev-dev \
    gobject-introspection libgirepository1.0-dev \
    libopencv-dev doctest-dev 2>/dev/null || true
  # Goodix-specific packages force purged (rarely used by other software)
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
  echo "!! Unrecognized package manager, please manually uninstall dependency packages" >&2
fi

echo "==> [5/6] Deleting state files and fingerprint data"
sudo rm -rf /var/lib/fprint/goodix /root/.config/goodix "$HOME/.config/goodix"
sudo rm -rf /var/lib/fprint/* 2>/dev/null || true   # Registered fingerprint data
echo "   Deleted psk.bin / goodix.dat / registered fingerprints"

echo "==> [6/6] ldconfig + udev reload"
sudo ldconfig
sudo udevadm control --reload-rules
sudo udevadm trigger

echo ""
echo "Uninstall complete: source libfprint, fprintd, dependency packages (shared tools"
echo "handled by autoremove), udev rules, state files, and fingerprint data have been removed."
