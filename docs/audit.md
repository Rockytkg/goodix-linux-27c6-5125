# 审计与优化报告（2025-08）

对 goodix-linux-27c6-5125 全量源码（`src/`、`include/`、`Makefile`、
`install-goodixgf.sh`、libfprint 驱动胶水层）的审计结果与已落地修改。

审计方法：逐文件精读 + `-Wall -Wextra -Wshadow -Wcast-qual
-Wmissing-prototypes` 严格编译 + 真实硬件（27c6:5125，本机已连接）全流程
实测（init / TLS 握手 / FDT 基线采样 / 图像采集）+ 位级一致性回归测试。

---

## 一、兼容性 Bug（已修复）

| # | 问题 | 位置 | 修复 |
|---|------|------|------|
| 1 | `GF_FW_CRC` 常量错误：写成 `0x98BA66C2`（那是 zlib 文件 CRC），运行时用的非反射 CRC-32/MPEG-2 实测为 `0x6FBC0F40`（与 goodix_fwupdate.c 注释及 docs/protocol.md 一致）。常量此前未被使用，但属于误导性错误。 | `include/goodix_fw.h` | 修正为 `0x6FBC0F40U`，并在 `fw_update_core()` 刷写前增加内嵌固件完整性预检（CRC 不符直接拒绝刷写，防止坏镜像进 MCU）。 |
| 2 | `Makefile` 的 `%.o` 规则把 803KB 的 `goodix_fw.h` 设为**所有**目标文件的依赖——改一次固件头全量重编。 | `Makefile` | 拆分为通用规则（只依赖 goodix.h / goodix_imgproc.h）+ `goodix_fwupdate.o` 单独依赖 goodix_fw.h。实测 touch 该头只重编 1 个目标。 |
| 3 | `install-goodixgf.sh` 的 apt 分支安装 `systemd-dev`——Debian 12 / Ubuntu 22.04 上该包不存在（应装 `libsystemd-dev`），`set -e` 下**整个安装脚本会中断**；dnf 分支 `systemd-devel` 重复安装。 | `install-goodixgf.sh` | apt update 后先探测 `apt-cache show systemd-dev`，存在才用新名，否则回退 `libsystemd-dev`；删除 dnf 分支重复项。 |
| 4 | `fw_update_master()` 死分支：`if (r > 0)` 判断 ClearApp 成功——但 `gx_send_cmd_wait` 成功返回 0，该分支永远不可达（每次都会打误导性的 "ClearApp failed" 日志）。 | `src/goodix_fwupdate.c` | 行为保持不变的清理：直接 ClearApp → 重读 EVK → 在 IAP 则下载，去掉死分支与误导日志。 |
| 5 | `goodix_fwupdate.c` 头部注释描述固件 blob 布局（"128430 字节 / 版本前缀"）与真实数据不符：内嵌 `gf_fw` 就是 128404 字节的原始镜像（以 ARM 向量表开头），版本串仅用于比较。 | `src/goodix_fwupdate.c` | 重写注释为真实布局。 |
| 6 | mbedtls 4.x 兼容（entropy/ctr_drbg 移除、PSA 随机源、`*_tls_version()` API）：工作区已有未提交修复，本次审计确认其正确并纳入（构建于 mbedtls 4.2.0 通过）。 | `src/goodix_tls.c` | 保留并整理（见结构优化）。 |

## 二、代码结构（已优化）

| # | 问题 | 修复 |
|---|------|------|
| 1 | **三份重复的 CRC-32/MPEG-2 实现**：`goodix_capture.c`（逐位）、`goodix_base.c`（查表）、`goodix_fwupdate.c`（查表）各自实现同一算法。 | 新增共享模块 `src/goodix_crc.c`（`gx_crc32_mpeg2(data, len, crc)`，查表），三处统一调用；Makefile 与安装脚本同步接入。标准校验向量 "123456789"→0x0376E6E7 与固件镜像 0x6FBC0F40 均验证通过。 |
| 2 | **TLS 接收缓冲是文件级 static**：`tls_rx_buf/head/tail`、`tls_rx_no_usb`、bio_recv 的 `frame` 全局共享——多设备实例/重入会串扰。 | 全部移入每个设备独立的 `struct gx_tls_ctx`（rx_buf/rx_head/rx_tail/rx_no_usb/frame），`gx_tls_capture_mode`/`gx_tls_feed`/`gx_tls_reconnect` 相应改为操作设备上下文。 |
| 3 | **采集 TLS 聚合缓冲是文件级 static**：`rx_acc`/`rx_acc_len` 全局共享。 | 移入 `struct goodix_dev`（`d->rx_acc`/`d->rx_acc_len`），`acc_take`/`next_payload`/`dispatch_payload`/`wait_image` 等全部改为设备状态。 |
| 4 | `main.c` 重复 `#include <stdlib.h>`。 | 删除重复行。 |

## 三、响应速度（已优化）

| # | 优化 | 说明 |
|---|------|------|
| 1 | **手指判别块方差单遍化**（`gx_fdt_check_finger`）：原实现每个 8×8 块对两幅图像各读两遍（求和遍 + 方差遍）。改为单遍把像素差缓存进 `di[64]`，方差遍直接读缓存，**消除对基线图/新帧的重复读取**。运算顺序与原实现完全一致，经 Python 参考模型 + 真实设备基线 5 组用例验证**逐位相同**。 |
| 2 | **fprintd 停止/关闭响应**：open 工作线程原先无法被中断，`gf_img_close` join 可能等待整个初始化（evk 唤醒重试最坏 >10s）。现在：`gf_img_open` 预先分配 `self->gx`（close 总能 `gx_capture_cancel`）；核心初始化各重试/轮询路径（`evk_with_retry` 各级、ChipID 重试、`gx_transport_reopen` 轮询、`gx_device_init` 外层循环）检查 `capture_stop` 快速返回 -4；open 失败路径与 close 竞争时不再回发 open_complete。取消响应从 ~10s+ 降到 <1s。 |
| 3 | 共享 CRC 由逐位改为查表（图像线 CRC 每帧 7680B 的校验提速 ~8 倍）。 |

## 四、libfprint 胶水层（goodixgf.c）

- `gf_open_worker` 不再自行分配/释放设备结构，`self->gx` 由 `gf_img_open` 在主线程预分配（消除 close 与 open 之间的竞态与 use-after-free 隐患）；
- open 成功/失败回发都受 `worker_stop` 守卫：close 进行中不再产生迟到的 open_complete 回调；
- `gf_img_close` 对 open 失败路径幂等（transport 已关、再关无害）。

## 五、验证结果

- `make clean && make`：0 错误 0 警告（`-O2 -Wall -Wextra`）。
- 严格告警扫描（-Wshadow/-Wcast-qual/-Wmissing-prototypes 等）：仅剩两处
  预存在的良性 API 强制转换告警（libusb_bulk_transfer / EVP_CTRL_GCM_SET_TAG）。
- libfprint 上下文中全部 core 文件 + goodixgf.c 语法检查通过。
- `tools/imgproc_model_check.py`：全部通过（默认参数逐字节一致）。
- **真实硬件实测**（本机 27c6:5125）：
  - `./goodix-cli --info`：完整初始化成功，TLS-PSK-WITH-AES-128-GCM-SHA256
    握手确认，FDT 基线采样、图像基线采集、goodix.dat 落盘成功；
  - 采集路径实测：图像线 CRC（共享实现）校验 OK，80×64 帧正常产出；
  - 手指判别位级一致性：C 实现 vs 原算法 Python 参考模型 5/5 逐位相同。
- 安装脚本 `bash -n` 通过；meson 补丁逻辑验证幂等（已补丁树重跑可正确
  增补 `goodix_crc.c` 源文件条目）。

## 六、审计发现但未改动的风险清单（供后续决策）

1. **`gx_read_frame` 单次读到超出一帧的字节会丢弃**（`r >= total` 直接返回）。
   当前设备按 64B 块补齐发送，实际不会把两帧塞进同一块，风险低；若换设备
   需改为留余字节。
2. **capture.c 中 `wait_image`/`gx_fdt_sample_base`/`gx_wait_finger_up` 的
   27KB `static pl[]` 工作缓冲**仍是文件级 static（单设备单线程下安全，
   但多设备重入不干净）。因采集路径严格单线程、且三个函数互不并发，
   本次未迁移；如需彻底重入可移入 `struct goodix_dev`。
3. **`gx_psk_store_load` 不校验文件大小上限**：`fread(psk, 1, *len, f)` 超出
   调用方 32B 缓冲的部分被忽略，安全但可加长度告警。
4. **`gx_send_cmd_wait` 对 dlen==1 的响应不做校验和字节剥离**：当前命令
   均有数据体，未触发；协议边界情况。
5. **`fw_clear_app` 成功路径**：清理后直接在本会话内重读 EVK 并下载固件
   （而非按参考实现"返回软复位标记、等待下次运行"）。当前行为已实测可用
   （固件版本相同即跳过），改动有硬件时序风险，未动。
6. **CLI 与 fprintd 的 USB 独占**：无设备句柄互斥，二者并发会互相干扰
   （README 已提示先停 fprintd）。

---

## 七、事故复盘：PSK 缓存过期导致驱动"假死"（2025-08-16 补录）

### 现象

升级后 fprintd 报：
`MCU hash MISMATCH` → `handshake err -0x7180`（= MBEDTLS_ERR_SSL_INVALID_MAC，
PSK 不匹配的典型错误）→ `MCU state ... locked=1` → 此后所有会话
`GetEvkVersion -3 / MCU unresponsive`，L1/L2/L3 唤醒全部无效，需重插设备。

### 根因（已用 journal + 状态目录 + 引导记录定位）

1. **不是代码改动**：`GF_FW_CRC` 完全无辜——所有失败日志都先打
   `same version, no firmware update needed`，固件更新路径（含 CRC 预检）
   从未执行；且 0x6FBC0F40 本来就是运行时非反射 CRC（docs/protocol.md
   明载），0x98BA66C2 是 zlib 文件 CRC，原常量确实写错。
2. **真正的事件链**：
   - Boot -2：fprintd 正常，`MCU hash MATCHES`（root 缓存与 MCU 一致）；
   - Boot -1：以非 root 用户跑 `goodix-cli`（本驱动 CLI 与 fprintd 分属
     两个状态目录：root → /var/lib/fprint/goodix，普通用户 →
     ~/.config/goodix，却共享同一 MCU）。CLI 无本地缓存且 MCU 读取失败，
     触发了"生成随机 PSK → 写入 MCU"路径，**MCU 的 PSK 被改写**；
   - Boot 0：fprintd 仍加载旧 root 缓存 → 与 MCU 不一致 → TLS
     INVALID_MAC → MCU 锁定 → 所有后续命令无响应。
3. **驱动自身缺陷**（本次事故暴露）：`init_mcu` 无条件信任本地缓存
   （`gx_psk_store_load` 成功即视为有效），`gx_psk_verify_mcu_hash` 的
   结果被忽略——缓存过期时既不重读 MCU 也不重写，握手必然失败且无自愈
   路径；MCU 锁死后连 evk 都超时，只能靠重插/重启恢复。

### 修复（本轮新增）

| 修复 | 位置 | 说明 |
|------|------|------|
| **PSK 自愈** | `goodix_init.c` | 本地缓存加载后必须用 MCU 哈希（0xBB020003）验证：一致（0）→ 使用；不一致（-6）→ 丢弃缓存，走"读 MCU（WB 解密即验证）并固化回本地缓存 / 重写新 PSK"路径。过期缓存不再导致永久 INVALID_MAC。 |
| **GOODIX_RESET_PSK 生效** | `goodix_init.c` | 原代码设了该环境变量仍会先读 MCU（读成功就跳过重写），与注释语义矛盾；现改为跳过 MCU 读、强制重写新 PSK。 |
| **L3 唤醒 CDC 重激活** | `transport.c` | 端口复位会清掉 SET_LINE_CODING + DTR，不重新激活则复位后设备依旧静默丢包（"usb reset done 后仍 -3"的根因）；`gx_usb_reset` 复位成功后重新 CDC 激活。 |
| **L3 重开兜底** | `goodix_init.c` | 复位+激活仍无响应时整体 close + 轮询 reopen（重新 detach 内核驱动/claim/CDC 激活），覆盖复位后内核重新绑定 cdc_acm 使旧句柄失效的情况。 |
| **TLS 失败解锁尝试** | `goodix_tls.c` | 握手硬错误返回前重发 0xD4 握手命令，给 MCU 解锁机会，避免锁定态阻塞后续 init。 |

### 恢复步骤（用户操作，需 sudo）

```
sudo systemctl stop fprintd
sudo rm -f /var/lib/fprint/goodix/psk.bin /var/lib/fprint/goodix/goodix.dat   # 清过期缓存
# 重新插拔指纹传感器（或重启电脑），清除 MCU 锁定态
bash install-goodixgf.sh      # 重装含自愈修复的驱动
sudo systemctl restart fprintd
fprintd-delete $USER && fprintd-enroll -f right-index-finger $USER
```

即使不删缓存也能自愈（日志应出现 `local PSK mismatch with MCU - re-sync` →
`PSK synchronized from MCU` → `TLS connected confirmed`），但删掉最干净。
以后用 CLI 调试前先 `sudo systemctl stop fprintd`，且避免以与 fprintd
不同的用户身份跑 CLI（共享 MCU、分属不同 PSK 缓存，是这次事故的诱因）。

---

## 八、第二轮审计：深层 bug 修复与代码规范化（追加）

对全部核心模块做了第二轮逐行审计（重点：崩溃路径、越界、失效注释、
文件级状态），修复如下：

| # | 问题 | 严重度 | 修复 |
|---|------|--------|------|
| 1 | **设备拔出后重试会崩溃**：evk 唤醒 L3 的 reopen 失败会关掉传输层（`usb_devh=NULL`），`gx_device_init` 的下一次重试仍直接调用 USB 读写 → `libusb_bulk_transfer(NULL)` 未定义行为（实测会崩）。 | 高 | `gx_usb_write/read/reset` 增加 NULL 句柄防护（返回 `LIBUSB_ERROR_NO_DEVICE`）；`gx_device_init` 重试循环在句柄丢失时先 `gx_transport_open` 主动恢复，失败即返回。 |
| 2 | **`gx_read_frame` 超时无上界**：deadline 只对"读错误"扣减，噪声帧/半包（`r<4`、类型/校验和/长度非法）路径不扣减 → 设备持续发坏帧时可远超 tmo。 | 中 | 所有非进展路径统一扣减预算，超时严格有界。 |
| 3 | **`gx_base_load` 声称做了 FDT 表合法性检查但实际缺失**：注释写着"使用文件前同样做合法性检查"，代码里没有——全零/损坏的阈值表会被直接采用。 | 中 | 补上落盘格式检查（高字节阈值 0/0xFF 即拒绝，返回 -5，走重新采样）。 |
| 4 | **`gx_transport_reopen` 只按原 PID 扫描**：注释明说"复位后可能以不同 PID 重新枚举"，但 reopen 用 `d->pid` 精确匹配 → 换 PID 后永远等不到设备。 | 中 | reopen 时 `d->pid = 0` 扫描任一 PID（成功后自动更新为实际 PID）。 |
| 5 | **`gx_transport_open` 设备列表获取失败时泄漏 libusb ctx**（`d->usb_ctx` 悬垂）。 | 低 | 失败路径 `libusb_exit` + 置 NULL。 |
| 6 | **`gf_modify_config` 无边界防御**：`off+cnt` 越出 224B 表即越界读（当前 4 张表已脚本验证全部合法，属防御性修复）。 | 低 | `cnt % 4 != 0` 或 `off+cnt > 224` 直接返回 0。 |
| 7 | **`gx_psk_store_load` 不识别超长缓存文件**：文件 > 期望长度（被破坏/篡改）时静默截断采用。 | 低 | 读后 `fgetc` 检测多余字节 → 拒绝并走自愈路径。 |
| 8 | **capture.c 残留文件级 static**（`pl`×3、`frame`）——多设备/重入串扰隐患（上轮审计遗留项）。 | 结构 | 全部移入 `struct goodix_dev`（`rx_scratch`/`rx_frame`），与 `rx_acc` 一并构成设备级接收状态。 |

### 审计确认无问题的区域（附验证）
- **4 张 224B 配置表**：脚本逐表验证 224B、8 个 section 的 `off+cnt ≤ 224` 且 `cnt%4==0`，`gf_modify_config` 不会越界；
- **图像/固件/基线 CRC**：与标准校验向量、固件镜像、真机线上帧一致；
- **`parse_image_payload`/`unpack_12bit_transpose` 边界**：v19/plen/packed 全部有界检查；
- **TLS 缓冲（rx_buf/head/tail）、采集聚合（rx_acc）**：上轮已移入设备上下文，本轮复核无残留文件级状态；
- **字节序/整数**：ts16、tcode/delta 运算、di[] 方差（上轮已位级验证）无溢出。

### 验证
- `make` 0 警告 0 错误；严格告警扫描（-Wshadow/-Wstrict-prototypes/...）干净；
- 合成文件单元测试 5/5 通过：合法基线加载、全零表拒绝（新检查）、CRC 损坏拒绝、超长 PSK 拒绝、正常 PSK 加载；
- 子模块驱动副本已同步（重跑安装脚本即生效）。
