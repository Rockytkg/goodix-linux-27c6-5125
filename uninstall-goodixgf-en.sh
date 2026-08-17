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
#   4. Remove all dependencies pulled in by the install script (install-goodixgf-en.sh):
#      - Goodix-specific packages removed directly (fprintd/libfprint/mbedtls/opencv/doctest)
#      - Explicitly installed build tools marked as auto / asdeps; autoremove / orphan
#        cleanup removes them only if nothing else depends on them (gcc/meson/glib/openssl
#        etc. may be shared by other software, never force-removed)
#   5. Delete goodix state files (PSK/baseline) and registered fingerprint data
#   6. ldconfig + udev reload
#
# Supports Debian/Ubuntu (apt), CentOS/Rocky/RHEL (dnf/yum) and Arch/CachyOS (pacman).
# Note: python3 is a system component (on both Ubuntu and Arch) and not removed.
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
  echo "    $SRC/build not found, skipping"
fi

echo "==> [3/6] Deleting udev rules"
sudo rm -f /etc/udev/rules.d/70-goodix.rules

echo "==> [4/6] Removing all dependencies pulled in by install script"
if command -v pacman >/dev/null 2>&1; then
  # ---- Arch / CachyOS (pacman) ----
  # Goodix-specific packages are removed one by one: pacman -R is transactional, if several
  # packages are put in one command and one of them is still required by another package,
  # the whole transaction fails and nothing is removed, so each must run separately.
  # fprintd's -Rns also removes libfprint that is no longer depended on (on Arch libfprint
  # is only used by fprintd). Packages with other dependents are refused and skipped
  # (same rationale as apt purge || true).
  # mbedtls package name varies by distro: Arch ships both mbedtls (4.x) and mbedtls3
  # (3.x LTS), CachyOS ships mbedtls (4.x); probe which one is installed.
  MBEDTLS_PKG=mbedtls
  pacman -Q mbedtls3 >/dev/null 2>&1 && MBEDTLS_PKG=mbedtls3
  for p in fprintd doctest "$MBEDTLS_PKG" opencv; do
    if pacman -Q "$p" >/dev/null 2>&1; then
      sudo pacman -Rns --noconfirm "$p" 2>/dev/null || \
        echo "!! $p is still required by other packages or not installed, skipping" >&2
    fi
  done
  # Mark build/run dependencies as "installed as dependency" (equivalent to apt-mark auto) —
  # only packages introduced by this project's install script that likely have no other
  # users on Arch; deliberately leave core shared components (gcc/python/glib2/openssl/
  # zlib/systemd-libs) untouched. Afterwards only remove orphans nobody depends on
  # (equivalent to apt autoremove), without touching other pre-existing orphans.
  for p in meson ninja pkgconf dos2unix libgusb libusb pixman \
           gobject-introspection; do
    if pacman -Q "$p" >/dev/null 2>&1; then
      sudo pacman -D --asdeps "$p" 2>/dev/null || true
    fi
  done
  # Query the orphan list once, only remove packages from the list nobody depends on
  ORPHANS="$(pacman -Qtdq 2>/dev/null || true)"
  for p in meson ninja pkgconf dos2unix libgusb libusb pixman \
           gobject-introspection; do
    if printf '%s\n' "$ORPHANS" | grep -qx "$p"; then
      sudo pacman -Rns --noconfirm "$p" 2>/dev/null || true
    fi
  done
elif command -v apt >/dev/null 2>&1; then
  # Packages explicitly `apt install`ed by the install script (including build tools and
  # -dev packages) are marked as auto; autoremove only removes packages "no longer required
  # by any installed package" — no leftover files and no accidental removal of shared tools.
  # gobject-introspection 1.80+ renamed: new package name is libgirepository-2.0-dev
  GIREPO_DEV=libgirepository1.0-dev
  apt-cache show libgirepository-2.0-dev >/dev/null 2>&1 && GIREPO_DEV=libgirepository-2.0-dev
  sudo apt-mark auto \
    fprintd libpam-fprintd libfprint-2-2 \
    meson ninja-build gcc g++ pkg-config dos2unix \
    libglib2.0-dev libgusb-dev libusb-1.0-0-dev libpixman-1-dev \
    libmbedtls-dev libssl-dev zlib1g-dev libudev-dev \
    gobject-introspection "$GIREPO_DEV" \
    libopencv-dev doctest-dev 2>/dev/null || true
  # Goodix-specific packages force purged (rarely used by other software)
  sudo apt purge -y \
    fprintd libpam-fprintd libfprint-2-2 \
    libmbedtls-dev libopencv-dev doctest-dev || true
  sudo apt autoremove --purge -y
elif command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; then
  PKG=$(command -v dnf || command -v yum)
  # Fedora 40+ renamed packages (libusb1-devel / zlib-ng-compat-devel / mbedtls-devel in
  # main repo), RHEL/Rocky/CentOS keep the old names (EPEL); probe logic matches
  # install-goodixgf-en.sh.
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
  # dnf remove fails the whole transaction if any listed package is not installed
  # (|| true cannot save it), so use skip_missing_names_on_remove to skip absent names.
  sudo $PKG remove -y --setopt=skip_missing_names_on_remove=True \
    fprintd fprintd-pam libfprint \
    meson ninja-build gcc gcc-c++ pkgconf-pkg-config dos2unix \
    glib2-devel libgusb-devel "$LIBUSB_DEV" pixman-devel \
    openssl-devel "$ZLIB_DEV" systemd-devel \
    gobject-introspection-devel opencv-devel doctest-devel \
    mbedtls-devel || true
  sudo $PKG autoremove -y || true
else
  echo "!! Unrecognized package manager, please manually uninstall dependency packages" >&2
fi

echo "==> [5/6] Deleting state files and fingerprint data"
sudo rm -rf /var/lib/fprint/goodix /root/.config/goodix "$HOME/.config/goodix"
sudo rm -rf /var/lib/fprint/* 2>/dev/null || true   # Registered fingerprint data
echo "    Deleted psk.bin / goodix.dat / registered fingerprints"

echo "==> [6/6] ldconfig + udev reload"
sudo ldconfig
sudo udevadm control --reload-rules
sudo udevadm trigger

echo ""
echo "Uninstall complete: source libfprint, fprintd, dependency packages (shared tools"
echo "handled by autoremove/orphan cleanup), udev rules, state files, and fingerprint data have been removed."
