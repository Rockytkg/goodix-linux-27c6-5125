# goodix-linux — Goodix 27c6:5125/5135 指纹模组 Linux 驱动

<div align="center">

**语言 / Languages**: [简体中文](README.md) | [English](README.en.md)

</div>

Linux 用户态指纹驱动，基于原生 API（libusb + mbedtls + OpenSSL）实现 Goodix 指纹模组的完整协议栈：设备初始化、固件更新、PSK 供应、白盒 AES、TLS-PSK 加密通道、FDT 手指检测、图像采集与主机侧手指判别，并集成 libfprint 驱动（SIGFM 匹配）接入系统生物认证。

> **⚠️ 免责声明（请先阅读）**
>
> 本项目为**独立开发的非官方开源项目**，仅供**技术学习、研究与设备互操作**（使 Goodix 指纹模组在 Linux 系统上正常工作）之用。本项目与**汇顶科技（Goodix Technology）**及任何模组厂商**无任何关联**，未获其授权、认可或支持，亦不代表其立场。
>
> - **用途限定**：请勿将本项目用于商业用途、侵犯他人隐私、未经授权访问他人设备，或任何违反适用法律的活动。
> - **固件版权**：`firmware/st411sec_app.bin` 版权归原厂商所有；其仅用于在你的合法设备上进行兼容性使用，请勿擅自再分发。
> - **商标声明**：Goodix 及文中出现的厂商产品名称为其各自所有者的商标或注册商标，本文引用仅为指称目的。
> - **风险自负**：使用本项目可能导致设备异常、固件损坏、数据丢失或其他后果，作者不承担任何直接或间接责任。
> - **无担保**：软件按“现状”提供，不提供任何明示或默示担保（包括但不限于适销性与特定用途适用性）。
> - **合规提示**：请确保你在所在司法辖区内的使用行为（含逆向工程、驱动兼容开发等）符合当地法律法规，并自行承担全部责任。
> - **不含厂商密钥**：本项目不包含任何厂商密钥或受控安全材料；PSK 由每台设备首次初始化时随机生成并写入其本地 MCU，仅存于该设备。

## 功能特性

- **完整初始化序列**：复位唤醒、版本读取、chipid 识别、PSK 供应、TLS 建立。
- **固件更新**：版本比较 + IAP 分片下载 + 硬复位重枚举。
- **安全通道**：PSK 白盒加密写入 MCU，TLS-PSK（AES-128-GCM）加密通信。
- **手指检测**：FDT 布防、手指按下/抬起事件、主机侧手指/空/坏/温度判别。
- **图像采集**：SetMode Image 曝光、TLS/明文图像帧、CRC 校验、12bit 解包转置。
- **libfprint 集成**：注册/比对走 **SIGFM**（OpenCV SIFT 关键点 + 几何一致性投票），支持 fprintd 的 enroll/verify/identify/list/delete。

## 支持设备

- 目标：`27c6:5125`（本机实测 chipid `0x2504`，sensor type 12 ChicagoHS，80×64）。
- 代码同时带 type 2（176×54）、type 3（112×132）、type 14（36×160）的配置表与判别参数，可通过 `--pid 0x5135` 指定另一 PID（`27c6:5135`）。

## 目录结构

```
├── src/            协议核心 + libfprint 驱动（goodixgf.c）与 CLI
├── include/        公共头文件（goodix.h / goodix_imgproc.h / goodix_fw.h）
├── libfprint/      git 子模块：goodix-fp-linux-dev 的 libfprint fork（libfprint-sigfm 分支）
├── docs/           协议规格、libfprint 集成文档
├── firmware/       固件（st411sec_app.bin，版权归原厂商，见 firmware/README.md）
├── tools/          辅助工具（图像处理模型校验等）
├── Makefile        CLI 构建
└── 70-goodix.rules udev 规则
```

> **关于 `libfprint/`**：该目录为 git 子模块，指向 [goodix-fp-linux-dev/libfprint](https://github.com/goodix-fp-linux-dev/libfprint) 的 `libfprint-sigfm` 分支。克隆本项目时请使用：
>
> ```sh
> git clone --recurse-submodules <本仓库地址>
> # 或已克隆后：
> git submodule update --init --recursive
> ```

## 快速开始

### 构建 CLI（命令行调试工具）

```sh
sudo apt install libusb-1.0-0-dev libmbedtls-dev libssl-dev zlib1g-dev
make clean && make
```

### 安装 libfprint 驱动（fprintd 生物认证）

```sh
bash install-goodixgf.sh
```

脚本会自动安装依赖（含 OpenCV ≥ 4.5、doctest）、复制驱动进 libfprint fork（SIGFM 分支）、patch 构建文件并编译安装。详见 [docs/libfprint-integration.md](docs/libfprint-integration.md)。

## 使用指南

### CLI 子命令

| 命令 | 说明 |
|---|---|
| `sudo ./goodix-cli --info` | 完整初始化（PSK/TLS/基线），不采集 |
| `sudo ./goodix-cli --capture N` | 初始化 + 采集 N 帧，存 `image-N.pgm` |
| `sudo ./goodix-cli --pid 0x5135` | 指定另一 PID |

`--capture` 采集时提示放上手指；每帧保存为 PGM 灰度图，用于核对图像质量、极性、连续性与对比度（也是图像预处理标定的输入）。

### fprintd 生物认证

```sh
fprintd-enroll -f right-index-finger $USER   # 注册（动态采样：3-8 次按压，位置分散、覆盖越全越早收敛）
fprintd-verify $USER                          # 验证
fprintd-delete $USER                          # 删除已注册模板
fprintd-list $USER                            # 查看已注册
```

首次打开设备时驱动自动完成 PSK 供应与基线采样（约 5–15 秒，**不要在传感器上放手指**）；之后每次打开约 1–2 秒。

### 状态文件

首次运行在状态目录生成两个文件（root 身份——`sudo` 或 fprintd——为 `/var/lib/fprint/goodix/`；普通用户为 `~/.config/goodix/`）：

| 文件 | 内容 |
|---|---|
| `psk.bin` (0600) | 32B 明文 PSK（供 TLS 握手） |
| `goodix.dat` (0600) | `[OTP 64B][FDT down 表 12B][navbase 3200B][imagebase 10240B][CRC32 4B]` |

删除这两个文件 = 恢复出厂（下次运行重新供应 PSK、重采全部基线）。

## 配置（环境变量）

| 变量 | 作用 |
|---|---|
| `GOODIX_DEBUG=1` | 打印每个 USB 帧的 hex dump 与详细日志 |
| `GOODIX_RESET_PSK=1` | 忽略本地 psk.bin，重新生成并写入 MCU（WB 算法变更后必须用一次） |
| `GOODIX_NO_TLS=1` | 跳过 TLS 握手（调试用；图像仍可走明文推送） |
| `GOODIX_CAPTURE_IMAGE_MODE=1` | 采集时不走 FDT，直接 SetMode Image 曝光取一帧 |
| `GOODIX_IMGPROC_BASELINE=0\|1` | 图像预处理：是否减无手指基线（默认 1） |
| `GOODIX_IMGPROC_FLATFIELD=0\|1` | 图像预处理：是否去低频平场（默认 1） |
| `GOODIX_IMGPROC_FLATFIELD_R=<n>` | 平场盒式均值半径（默认 12） |
| `GOODIX_IMGPROC_PCT_LO=<n>`、`GOODIX_IMGPROC_PCT_HI=<n>` | 百分位保留区间（默认 1 / 99） |
| `GOODIX_IMGPROC_ENHANCE=none\|sigfm` | 反锐化局部对比增强（驱动默认 `sigfm`；提升弱脊/轻触下 SIFT 关键点通过率） |
| `GOODIX_IMGPROC_BOOST=<f>`、`GOODIX_IMGPROC_SIGMA=<f>` | 反锐化增益与高斯 σ（默认 0.8 / 1.5） |
| `GOODIX_DUMP_IMGPROC=1` | 逐级 dump 预处理中间态到状态目录 `imgproc-<seq>-<stage>.pgm`（raw8/base8/flat8/final8/enh8） |

## 架构

```
src/
├── transport.c         libusb bulk 传输 + CDC 激活
├── goodix_frame.c      A0/B0 帧封装、校验、分片
├── goodix_cmd.c        命令层（同步等待响应）
├── goodix_init.c       初始化序列、固件更新调度、基线采样
├── goodix_fwupdate.c   IAP 固件下载
├── goodix_psk.c        PSK 生命周期 + 白盒 AES
├── goodix_tls.c        mbedtls PSK server、流式 BIO
├── goodix_capture.c    FDT 布防、图像接收、12bit 解包、手指判别
├── goodix_base.c       goodix.dat 持久化与基线学习
├── goodix_otp.c        OTP 解析 + 配置表补丁
├── goodix_imgproc.c    16bit 帧 → 8bit 灰度参数化管线（基线/平场/百分位拉伸 + SIGFM 反锐化增强）
└── main.c              CLI
```

采集时序（FDT 驱动）：

```
布防 FDT down   TX 0x32 [8,1,down表12B,ts16]     # 手指按下检测
手指接触         RX cmd0=3 IrqStatus=0x2          # FDT 事件（顺带学 up 基线）
取图             TX 0x20 [1,0]                    # SetMode Image，MCU 曝光
图像             RX cmd0=2（TLS 加密或明文推送）   # 7684B 线上数据
                  └ wire CRC32 校验 → 12bit 解包 → 列→行转置 → 80x64x16bit
判别             gx_fdt_check_finger              # 1=接受 2/3=重布防 0=重建基线
收尾             TX 0x34 [10,1,up表12B]           # 布防手指抬起检测
```

## 协议参考

帧格式、命令表、TLS、FDT、判别算法、白盒结构等协议细节见 [docs/protocol.md](docs/protocol.md)。

## 故障排查

- **完全不响应**：确认接口未被 `cdc_acm` 内核驱动占用（`sudo modprobe -r cdc_acm`），需要 root 或安装 `70-goodix.rules`。
- **FDT 布防后无事件**：基线未采样。删除状态目录下的 `goodix.dat`（root 在 `/var/lib/fprint/goodix/`）重跑一次 `--info`（init 会自动现场采样）。
- **TLS INVALID_MAC**：MCU 里的 PSK blob 是用旧 WB 密钥写的，`sudo GOODIX_RESET_PSK=1 ./goodix-cli --info` 重写一次。
- **判别全是 void/temperature**：图像基线过期（传感器老化/温度漂移），删除 `goodix.dat` 重跑 `--info` 重建；正常运行中判别返回 temperature 也会自动重建。
- **fprintd verify 偶发 no-match**：先用 CLI 采一帧（`--capture 1`）查看 `image-0.pgm`，它是交给 SIGFM 提取特征前的 80x64 灰度图。查看 SIGFM 得分日志判断是"得分 0（匹配对 <5）"还是"低于阈值"：
  ```sh
  sudo systemctl stop fprintd
  sudo env G_MESSAGES_DEBUG=all /usr/libexec/fprintd   # 或 /usr/lib/fprintd
  # 另开终端 fprintd-verify $USER，观察 "sigfm score N/20"
  ```
  得分 0 → 按压位置/关键点不重复，靠注册时位置分散（动态采样：至少 3 次、至多 8 次，位置连续重复即收敛、提前完成，重复按压由驱动拒绝重按）+ 反锐化增强解决；得分 10-19 → 阈值略高，调低 `GF_SIGFM_SCORE_THRESHOLD`（src/goodixgf.c）。升级算法/图像链后必须执行 `fprintd-delete $USER` 并重新 enroll（已注册模板与当前引擎绑定）。
- **SIGFM 关键点不足 / 想提升匹配**：预处理管线（`src/goodix_imgproc.c`）驱动默认走 `GX_IMGPROC_SIGFM_PARAMS`（**反锐化增强开启**）；`GOODIX_IMGPROC_ENHANCE=none` 可关。标定时 `GOODIX_DUMP_IMGPROC=1` 逐级查看 `raw8/base8/flat8/final8/enh8`，再用 `GOODIX_IMGPROC_BOOST`/`GOODIX_IMGPROC_SIGMA` 调增强强度。参数由 env 驱动，修改后必须 `fprintd-delete $USER` 重新 enroll。

## 致谢

本项目能落地离不开以下开源社区与项目的支持，特此致谢：

- **[goodix-fp-linux-dev/libfprint](https://github.com/goodix-fp-linux-dev/libfprint)** — 社区维护的 libfprint fork，提供 `libfprint-sigfm` 分支与 **SIGFM**（OpenCV SIFT 关键点 + 几何一致性投票）匹配算法。本项目的 libfprint 驱动基于该分支集成（git 子模块）。
- **[libfprint](https://gitlab.freedesktop.org/libfprint/libfprint)（freedesktop）** — libfprint 上游项目，`FpImageDevice` 等驱动框架 API 来自上游。
- **goodix-fp-linux-dev 社区** — 对 Goodix 指纹模组协议的逆向与分析工作，为协议实现提供了重要参考。
- 构建与运行依赖的开源项目：**libusb**、**mbedtls**、**OpenSSL/libcrypto**、**zlib**、**OpenCV**（SIFT）、**fprintd** 等。

再次感谢所有上游维护者与贡献者。

## 许可与合规

- **项目代码**：本项目自有代码基于 **GPL-2.0-or-later** 许可发布（见 [LICENSE](LICENSE)），各文件许可以其文件头 **SPDX** 声明为准。
- **libfprint 驱动胶水层**：`src/goodixgf.c` 为 **LGPL-2.1-or-later**（与上游 libfprint 驱动许可保持一致）。
- **第三方代码**：`libfprint/` 为 git 子模块，指向 [goodix-fp-linux-dev/libfprint](https://github.com/goodix-fp-linux-dev/libfprint) 的 `libfprint-sigfm` 分支，其许可以其上游声明为准。
- **固件**：`firmware/st411sec_app.bin` 及 `include/goodix_fw.h` 内嵌的固件数据版权归原厂商所有，**不属于本项目开源许可范围**，仅限在你合法拥有的设备上用于兼容性使用；本项目不主张其版权，亦不对其再分发权利作任何保证（详见 [firmware/README.md](firmware/README.md)）。
- **厂商密钥**：PSK 为每台设备首次初始化时随机生成并写入其本地 MCU；本项目不含任何厂商密钥或受控安全材料，仅用于授权实验与开源社区。
- **无关联声明**：本项目与汇顶科技（Goodix Technology）及任何指纹模组厂商无任何关联，未获其授权、认可或支持。

完整免责声明见上文[顶部声明](#免责声明请先阅读)。
