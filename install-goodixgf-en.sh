#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# install-goodixgf-en.sh - Build and install libfprint with goodixgf driver (SIGFM matching)
#                          (27c6:5125 / 27c6:5135)
#
# Usage:
#   bash install-goodixgf-en.sh [libfprint source directory]
# Default source directory: <goodix-linux>/libfprint — libfprint fork from goodix-fp-linux-dev community
# (libfprint-sigfm branch, tracking origin/0x00002a/libfprint-sigfm),
# enrollment/matching uses SIGFM algorithm (OpenCV SIFT keypoints + geometric consistency voting).
#
# Supports Debian/Ubuntu (apt) and CentOS/Rocky/RHEL (dnf/yum, EPEL auto-enabled).
# SIGFM depends on OpenCV >= 4.5 (SIFT in main module) and doctest (algorithm self-test), requires:
#   Ubuntu 22.04+ / Debian 12+ (libopencv-dev >= 4.5)
#   EPEL 9 (opencv-devel >= 4.5)
#
# Process:
#   1. Install build dependencies (including opencv4 >= 4.5, doctest) + fprintd
#   2. Copy goodixgf driver + protocol core to <source>/libfprint/drivers/goodixgf/
#   3. Patch meson.build (fork's driver_sources / optional_deps, idempotent)
#   4. Compile and install to /usr (SONAME unchanged, system fprintd reuses directly)
#   5. Reload udev, restart fprintd
#
# After installation (registered templates need deletion and re-registration after upgrade):
#   fprintd-delete $USER
#   fprintd-enroll -f right-index-finger $USER && fprintd-verify $USER
set -euo pipefail

GOODIX_LINUX="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$GOODIX_LINUX/libfprint}"

if [ ! -f "$SRC/libfprint/meson.build" ] || [ ! -f "$SRC/meson_options.txt" ]; then
  echo "!! $SRC is not a libfprint source tree root (missing libfprint/meson.build)" >&2
  echo "   Usage: bash $0 [libfprint source directory]" >&2
  exit 1
fi
if [ ! -f "$SRC/libfprint/sigfm/sigfm.h" ]; then
  echo "!! $SRC is not on the SIGFM branch (missing libfprint/sigfm/sigfm.h)." >&2
  echo "   Please switch to SIGFM branch in $SRC and retry:" >&2
  echo "     git -C \"$SRC\" checkout libfprint-sigfm" >&2
  exit 1
fi
SRC="$(cd "$SRC" && pwd)"

# Source code copied from Windows often has CRLF line endings: gcc/meson tolerate it,
# but g-ir-scanner's gtk-doc parser reports "identifier not found on the first line".
# Normalize line endings to LF for the entire tree first (git clone of fork already LF, idempotent).
echo "==> Normalizing source line endings (CRLF -> LF)"
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

# ---------------- 1. Dependencies ----------------
echo "==> [1/5] Installing dependencies (including OpenCV>=4.5 / doctest required by SIGFM)"
if command -v apt >/dev/null 2>&1; then
  PKG=apt
  sudo apt update
  sudo apt install -y meson ninja-build gcc g++ pkg-config python3 dos2unix \
    libglib2.0-dev libgusb-dev libusb-1.0-0-dev libpixman-1-dev \
    libmbedtls-dev libssl-dev zlib1g-dev libudev-dev \
    gobject-introspection libgirepository1.0-dev \
    libopencv-dev doctest-dev systemd-dev\
    fprintd libpam-fprintd
elif command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; then
  PKG=$(command -v dnf || command -v yum)
  # mbedtls / fprintd / opencv / doctest in EPEL (Rocky: crb or powertools may need enabling)
  sudo $PKG install -y epel-release 2>/dev/null || true
  sudo $PKG config-manager --set-enabled crb 2>/dev/null || \
    sudo $PKG config-manager --set-enabled powertools 2>/dev/null || true
  sudo $PKG install -y meson ninja-build gcc gcc-c++ pkgconf-pkg-config python3 dos2unix \
    glib2-devel libgusb-devel libusbx-devel pixman-devel \
    openssl-devel zlib-devel systemd-devel \
    gobject-introspection-devel \
    opencv-devel doctest-devel systemd-devel || \
    echo "!! Some dependencies failed to install (EPEL not ready?), please manually install opencv-devel/doctest-devel and retry" >&2
  sudo $PKG install -y mbedtls-devel fprintd fprintd-pam || \
    echo "!! mbedtls-devel/fprintd installation failed (EPEL not ready?), please manually install and retry" >&2
else
  echo "!! Unrecognized package manager: please manually install meson/ninja/gcc/glib2/libgusb/libusb/pixman/openssl/zlib/mbedtls/gobject-introspection/fprintd/libopencv-dev/libdoctest-dev" >&2
  exit 1
fi

# SIGFM's SIFT requires OpenCV >= 4.5 (hard dependency for libfprint/sigfm compilation).
OPENCV_VER="$(pkg-config --modversion opencv4 2>/dev/null || true)"
if [ -z "$OPENCV_VER" ]; then
  echo "!! Cannot find pkg-config package opencv4: libopencv-dev/opencv-devel not installed or version too old" >&2
  exit 1
fi
# Numeric version comparison (split by . into integers and compare segment-by-segment;
# string comparison would incorrectly treat "4.10.0" as < "4.5" because '1'<'5').
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
  echo "!! OpenCV version $OPENCV_VER < 4.5, SIGFM requires >= 4.5 (SIFT in main module)." >&2
  echo "   Please upgrade your distribution (Ubuntu 22.04+ / Debian 12+ / EPEL9+) or manually install newer OpenCV and retry." >&2
  exit 1
fi
echo "    OpenCV $OPENCV_VER meets SIGFM requirements"

# ---------------- 2. Copy driver ----------------
echo "==> [2/5] Copying goodixgf driver and protocol core -> $SRC"
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

# Copied driver files from Windows checkout may have CRLF (g-ir-scanner's gtk-doc
# parser doesn't recognize \r), normalize line endings like the source tree.
if command -v dos2unix >/dev/null 2>&1; then
  find "$D" -type f -name '*.[ch]' -print0 | xargs -0 -r dos2unix -q --
else
  find "$D" -type f -name '*.[ch]' -print0 | while IFS= read -r -d '' f; do
    if grep -Iq $'\r' "$f" 2>/dev/null; then
      sed -i 's/\r$//' "$f"
    fi
  done
fi

# ---------------- 3. Patch meson (idempotent, anchors match fork's actual structure) ----------------
echo "==> [3/5] Patching meson.build"
python3 - "$SRC" <<'PYEOF'
import sys

src = sys.argv[1]

# --- 3a. Root meson.build: append optional_deps for goodixgf ---
# Fork's build system uses default_drivers + driver_helper_mapping + optional_deps,
# no drivers_info dict; goodixgf's mbedtls/openssl dependencies are injected here
# (whitebox AES/HMAC uses OpenSSL libcrypto).
p = src + '/meson.build'
s = open(p).read()
if "'goodixgf'" not in s:
    anchor = "if udev_rules.disabled()\n"
    block = """# goodixgf: TLS-PSK channel (mbedtls) + whitebox AES/HMAC (OpenSSL libcrypto).
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
        sys.exit("!! Cannot find 'if udev_rules.disabled()' anchor in root meson.build (fork version changed?), "
                 "please manually add goodixgf dependency block after optional_deps collection")
    s = s.replace(anchor, block + anchor, 1)
    open(p, 'w').write(s)

# --- 3b. libfprint/meson.build: driver_sources registration (fork uses array format) ---
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
        sys.exit("!! Cannot find goodixmoc source file anchor in libfprint/meson.build (fork structure changed?), "
                 "please manually add goodixgf entry to driver_sources")
    s = s.replace(anchor, anchor + entry, 1)
    open(p, 'w').write(s)
print('patched: optional_deps + driver_sources (goodixgf)')
PYEOF

# ---------------- 4. Compile and install ----------------
echo "==> [4/5] Compiling and installing"
if command -v dpkg >/dev/null 2>&1; then
  LIBDIR="lib/$(gcc -dumpmachine)"     # Debian/Ubuntu multi-arch directory
else
  LIBDIR="lib64"                        # CentOS/Rocky/RHEL
fi
# Keep introspection enabled (fork's default): tests/meson.build depends on it to generate driver tests,
# disabling it may trigger issues during configuration. GIR generation issues already fixed by line ending normalization.
GIR_OPT="-Dintrospection=true"

cd "$SRC"
# Note: fork's meson_options.txt has no installed-tests option, cannot pass
# -Dinstalled-tests=false, otherwise meson setup reports unknown option.
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
echo "==> [5/5] Installing udev rules + restarting fprintd"
# Fork's udev rules don't include goodixgf's VID/PID, must install the project's own rules,
# otherwise non-root users (plugdev group) cannot access the device.
sudo install -m 0644 "$GOODIX_LINUX/70-goodix.rules" /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
sudo systemctl restart fprintd

cat <<DONE

Installation complete. Registered templates are linked to the current driver/image, please delete and re-register after upgrade:
  fprintd-delete \$USER                        # Delete registered fingerprints
  journalctl -u fprintd -b | grep -i goodix   # Should see goodixgf opening device
  fprintd-enroll -f right-index-finger \$USER  # Register (first open provisions PSK/baseline, don't place finger; dynamic sampling 3-8 times, spread positions)
  fprintd-verify \$USER                        # Verify

Tuning:
  GOODIX_DEBUG=1 to observe "sigfm score %d/%d" logs. Current threshold is
  GF_SIGFM_SCORE_THRESHOLD=20 (modify in src/goodixgf.c and re-run this script).
  Too low threshold accepts false matches, too high rejects valid matches.

Notes:
  - Package manager upgrades to libfprint will overwrite this installation, re-run this script after upgrade
  - Use goodix-cli for debugging after sudo systemctl stop fprintd (USB exclusive)
  - SIGFM extraction requires at least 25 SIFT keypoints per frame, insufficient keypoints trigger retry-scan
DONE
