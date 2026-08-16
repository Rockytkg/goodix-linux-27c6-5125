# goodix-linux — Linux driver for the Goodix 27c6:5125/5135 fingerprint module

<div align="center">

**Languages**: [简体中文](README.md) | [English](README.en.md)

</div>

A userspace Linux fingerprint driver that implements the complete protocol stack of Goodix fingerprint modules against the native APIs (libusb + mbedtls + OpenSSL): device initialization, firmware update, PSK provisioning, white-box AES, TLS-PSK encrypted channel, FDT finger detection, image capture with host-side finger classification, plus an integrated libfprint driver (SIGFM matching) that plugs into the system's biometric authentication (fprintd).

> **⚠️ Disclaimer (read first)**
>
> This is an **independent, unofficial open-source project**, provided **solely for technical learning, research, and device interoperability** (making a Goodix fingerprint module work properly on Linux). It is **not affiliated with, endorsed, or supported by Goodix Technology** or any module vendor, and does not represent their views.
>
> - **Restricted use**: Do not use this project for commercial purposes, to infringe on others' privacy, to gain unauthorized access to devices that are not yours, or for any activity that violates applicable law.
> - **Firmware copyright**: `firmware/st411sec_app.bin` is copyrighted by its original vendor. It is provided only for compatibility use on devices you legally own — do not redistribute it.
> - **Trademarks**: Goodix and the vendor product names appearing in this document are trademarks or registered trademarks of their respective owners; references are for identification purposes only.
> - **Use at your own risk**: Using this project may result in device malfunction, firmware damage, data loss, or other consequences. The authors accept no responsibility for any direct or indirect damages.
> - **No warranty**: The software is provided "as is", without warranty of any kind, express or implied (including, but not limited to, merchantability and fitness for a particular purpose).
> - **Compliance**: You are solely responsible for ensuring that your use (including reverse engineering and driver-compatibility development) complies with the laws of your jurisdiction.
> - **No vendor keys**: This project contains no vendor keys or controlled security materials. The PSK is randomly generated on each device at first initialization and written only to that device's local MCU.

## Features

- **Full initialization sequence**: reset wake-up, version read, chipid detection, PSK provisioning, TLS establishment.
- **Firmware update**: version comparison + IAP chunked download + hard-reset re-enumeration.
- **Secure channel**: PSK written to the MCU with white-box encryption; TLS-PSK (AES-128-GCM) encrypted communication.
- **Finger detection**: FDT arming, finger-down/up events, host-side finger/void/bad/temperature classification.
- **Image capture**: SetMode Image exposure, TLS/plaintext image frames, CRC check, 12-bit unpack + transpose.
- **libfprint integration**: enrollment/verification via **SIGFM** (OpenCV SIFT keypoints + geometric consistency voting), supporting fprintd enroll/verify/identify/list/delete.

## Supported devices

- Target: `27c6:5125` (tested locally: chipid `0x2504`, sensor type 12 ChicagoHS, 80×64).
- The code also ships config tables and classification parameters for type 2 (176×54), type 3 (112×132), and type 14 (36×160). Another PID (`27c6:5135`) can be selected with `--pid 0x5135`.

## Directory layout

```
├── src/            Protocol core + libfprint driver (goodixgf.c) + CLI
├── include/        Shared headers (goodix.h / goodix_imgproc.h / goodix_fw.h)
├── libfprint/      git submodule: goodix-fp-linux-dev's libfprint fork (libfprint-sigfm branch)
├── docs/           Protocol spec, libfprint integration docs
├── firmware/       Firmware (st411sec_app.bin — vendor copyright; see firmware/README.md)
├── tools/          Helper tools (image-processing model validation, etc.)
├── Makefile        CLI build
└── 70-goodix.rules udev rules
```

> **About `libfprint/`**: this directory is a git submodule pointing at the `libfprint-sigfm` branch of [goodix-fp-linux-dev/libfprint](https://github.com/goodix-fp-linux-dev/libfprint). When cloning this repository, use:
>
> ```sh
> git clone --recurse-submodules <this-repo-url>
> # or, if already cloned:
> git submodule update --init --recursive
> ```

## Quick start

### Build the CLI (command-line debugging tool)

```sh
sudo apt install libusb-1.0-0-dev libmbedtls-dev libssl-dev zlib1g-dev
sudo dnf install libusb1-devel openssl-devel mbedtls-devel zlib-ng-compat-devel cmake
make clean && make
```

### Install the libfprint driver (fprintd biometric authentication)

```sh
bash install-goodixgf-en.sh
```

### Uninstall the libfprint driver

```sh
bash uninstall-goodixgf-en.sh
```

This removes all installed components, dependencies, udev rules, state files, and fingerprint data. See the script header for details.

The script installs dependencies (incl. OpenCV ≥ 4.5, doctest), copies the driver into the libfprint fork (SIGFM branch), patches the build files, and compiles/installs. See [docs/libfprint-integration.md](docs/libfprint-integration.md).

## Usage

### CLI subcommands

| Command | Description |
|---|---|
| `sudo ./goodix-cli --info` | Full initialization (PSK/TLS/baseline), no capture |
| `sudo ./goodix-cli --capture N` | Initialize + capture N frames, saved as `image-N.pgm` |
| `sudo ./goodix-cli --pid 0x5135` | Specify another PID |

`--capture` prompts you to place a finger; each frame is saved as a PGM grayscale image to check image quality, polarity, continuity, and contrast (also the input for image-preprocessing calibration).

### fprintd biometric authentication

```sh
fprintd-enroll -f right-index-finger $USER   # enroll (dynamic sampling: 3-8 presses; wider coverage converges sooner)
fprintd-verify $USER                          # verify
fprintd-delete $USER                          # delete enrolled template
fprintd-list $USER                            # list enrolled fingerprints
```

On the first open, the driver automatically performs PSK provisioning and baseline sampling (≈5–15 s, **do not place a finger on the sensor**); subsequent opens take ≈1–2 s.

### State files

The first run creates two files in the state directory (as root — `sudo` or fprintd — in `/var/lib/fprint/goodix/`; as a regular user in `~/.config/goodix/`):

| File | Contents |
|---|---|
| `psk.bin` (0600) | 32-byte plaintext PSK (used for the TLS handshake) |
| `goodix.dat` (0600) | `[OTP 64B][FDT down table 12B][navbase 3200B][imagebase 10240B][CRC32 4B]` |

Deleting these two files resets to factory state (PSK is re-provisioned and all baselines re-sampled on the next run).

## Configuration (environment variables)

| Variable | Effect |
|---|---|
| `GOODIX_DEBUG=1` | Print a hex dump of every USB frame and verbose logs |
| `GOODIX_RESET_PSK=1` | Ignore the local psk.bin, regenerate and write to the MCU (required once after a WB algorithm change) |
| `GOODIX_NO_TLS=1` | Skip the TLS handshake (debugging; images can still be pushed in plaintext) |
| `GOODIX_CAPTURE_IMAGE_MODE=1` | Skip FDT and grab one frame directly via SetMode Image exposure |
| `GOODIX_IMGPROC_BASELINE=0\|1` | Image preprocessing: subtract no-finger baseline (default 1) |
| `GOODIX_IMGPROC_FLATFIELD=0\|1` | Image preprocessing: remove low-frequency flat field (default 1) |
| `GOODIX_IMGPROC_FLATFIELD_R=<n>` | Flat-field box-mean radius (default 12) |
| `GOODIX_IMGPROC_PCT_LO=<n>`, `GOODIX_IMGPROC_PCT_HI=<n>` | Percentile retention range (default 1 / 99) |
| `GOODIX_IMGPROC_ENHANCE=none\|sigfm` | Unsharp local-contrast enhancement (driver default `sigfm`; improves SIFT keypoint yield on weak ridges / light touches) |
| `GOODIX_IMGPROC_BOOST=<f>`, `GOODIX_IMGPROC_SIGMA=<f>` | Unsharp gain and Gaussian σ (default 0.8 / 1.5) |
| `GOODIX_DUMP_IMGPROC=1` | Dump each preprocessing intermediate to the state directory as `imgproc-<seq>-<stage>.pgm` (raw8/base8/flat8/final8/enh8) |

## Architecture

```
src/
├── transport.c         libusb bulk transport + CDC activation
├── goodix_frame.c      A0/B0 frame packing, checksum, chunking
├── goodix_cmd.c        Command layer (synchronous response wait)
├── goodix_init.c       Init sequence, firmware-update scheduling, baseline sampling
├── goodix_fwupdate.c   IAP firmware download
├── goodix_psk.c        PSK lifecycle + white-box AES
├── goodix_tls.c        mbedtls PSK server, streaming BIO
├── goodix_capture.c    FDT arming, image reception, 12-bit unpack, finger classification
├── goodix_base.c       goodix.dat persistence and baseline learning
├── goodix_otp.c        OTP parsing + config-table patching
├── goodix_imgproc.c    16-bit frame → 8-bit grayscale parameterized pipeline (baseline/flatfield/percentile stretch + SIGFM unsharp enhancement)
└── main.c              CLI
```

Capture flow (FDT-driven):

```
Arm FDT down   TX 0x32 [8,1,down-table-12B,ts16]  # finger-down detection
Finger contact  RX cmd0=3 IrqStatus=0x2           # FDT event (also learns the up baseline)
Capture        TX 0x20 [1,0]                      # SetMode Image, MCU exposes
Image          RX cmd0=2 (TLS-encrypted or plaintext push)  # 7684B on the wire
                  └ wire CRC32 → 12-bit unpack → column→row transpose → 80x64x16bit
Classify       gx_fdt_check_finger                # 1=accept 2/3=re-arm 0=rebuild baseline
Finish         TX 0x34 [10,1,up-table-12B]        # arm finger-up detection
```

## Protocol reference

Frame formats, the command table, TLS, FDT, the classification algorithm, and the white-box layout are detailed in [docs/protocol.md](docs/protocol.md).

## Troubleshooting

- **No response at all**: make sure the interface is not claimed by the `cdc_acm` kernel driver (`sudo modprobe -r cdc_acm`); root privileges or the `70-goodix.rules` udev rule are required.
- **No FDT event after arming**: the baseline was not sampled. Delete `goodix.dat` from the state directory (as root in `/var/lib/fprint/goodix/`) and re-run `--info` once (init samples the baseline on the spot).
- **TLS INVALID_MAC**: the PSK blob in the MCU was written with an old WB key — run `sudo GOODIX_RESET_PSK=1 ./goodix-cli --info` once to rewrite it.
- **Classification all void/temperature**: the image baseline is stale (sensor aging / temperature drift). Delete `goodix.dat` and re-run `--info` to rebuild; during normal operation a temperature classification also triggers an automatic rebuild.
- **Occasional fprintd verify no-match**: capture a frame with the CLI first (`--capture 1`) and inspect `image-0.pgm` — it is the 80×64 grayscale image handed to SIGFM before feature extraction. Check the SIGFM score log to tell "score 0 (match pairs <5)" from "below threshold":
  ```sh
  sudo systemctl stop fprintd
  sudo env G_MESSAGES_DEBUG=all /usr/libexec/fprintd   # or /usr/lib/fprintd
  # in another terminal: fprintd-verify $USER, watch for "sigfm score N/20"
  ```
  Score 0 → press position/keypoints do not repeat; fix via dispersed enrollment positions (dynamic sampling: `GF_ENROLL_MIN_STAGES`=3 to `GF_ENROLL_MAX_STAGES`=8 presses, converging early once positions start repeating `GF_ENROLL_DUP_STOP` times; repeated positions are rejected by the driver) + unsharp enhancement. Score 10-19 → threshold is slightly high; lower `GF_SIGFM_SCORE_THRESHOLD` (src/goodixgf.c). After any algorithm/image-chain upgrade you must `fprintd-delete $USER` and re-enroll (registered templates are bound to the current engine).
- **Too few SIGFM keypoints / want better matching**: the preprocessing pipeline (`src/goodix_imgproc.c`) uses `GX_IMGPROC_SIGFM_PARAMS` by default in the driver (**unsharp enhancement on**); `GOODIX_IMGPROC_ENHANCE=none` disables it. For calibration, set `GOODIX_DUMP_IMGPROC=1` to inspect `raw8/base8/flat8/final8/enh8` stage by stage, then tune the enhancement strength with `GOODIX_IMGPROC_BOOST`/`GOODIX_IMGPROC_SIGMA`. Parameters are env-driven; after changing them you must `fprintd-delete $USER` and re-enroll.

## Acknowledgements

This project would not exist without the support of the following open-source communities and projects — thank you:

- **[goodix-fp-linux-dev/libfprint](https://github.com/goodix-fp-linux-dev/libfprint)** — a community-maintained libfprint fork providing the `libfprint-sigfm` branch and the **SIGFM** matching algorithm (OpenCV SIFT keypoints + geometric consistency voting). This project's libfprint driver is integrated on top of that branch (git submodule).
- **[libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) (freedesktop)** — the upstream libfprint project; driver-framework APIs such as `FpImageDevice` come from upstream.
- **the goodix-fp-linux-dev community** — their reverse-engineering and analysis of the Goodix fingerprint-module protocol provided an important reference for this implementation.
- Build/runtime dependencies: **libusb**, **mbedtls**, **OpenSSL/libcrypto**, **zlib**, **OpenCV** (SIFT), **fprintd**, and others.

Thanks again to all upstream maintainers and contributors.

## License & compliance

- **Project code**: the project's own code is released under **GPL-2.0-or-later** (see [LICENSE](LICENSE)); each file's license is declared by its **SPDX** header.
- **libfprint driver glue**: `src/goodixgf.c` is **LGPL-2.1-or-later** (kept consistent with the upstream libfprint driver license).
- **Third-party code**: `libfprint/` is a git submodule pointing at the `libfprint-sigfm` branch of [goodix-fp-linux-dev/libfprint](https://github.com/goodix-fp-linux-dev/libfprint); its license follows its upstream declaration.
- **Firmware**: `firmware/st411sec_app.bin` and the firmware data embedded in `include/goodix_fw.h` are copyrighted by their original vendor and are **not covered by this project's open-source license**. They are provided solely for compatibility use on devices you legally own. This project makes no claim of ownership over them and gives no warranty regarding their redistribution (see [firmware/README.md](firmware/README.md)).
- **Vendor keys**: the PSK is randomly generated on each device at first initialization and written to that device's local MCU. This project contains no vendor keys or controlled security materials; it is intended for authorized experimentation and the open-source community.
- **No affiliation**: this project is not affiliated with, endorsed by, or supported by Goodix Technology or any fingerprint-module vendor.

The full disclaimer is at the [top of this file](#⚠️-disclaimer-read-first).
