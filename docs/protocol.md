# Goodix 指纹模组通信协议规格

目标设备：`USB\VID_27C6&PID_5125 / 5135`（CDC-ACM）
固件：`GF_ST411SEC_APP_12508`（IAP loader：`MILAN_ST411SEC_IAP_12501`）
本机实测：chipid `0x2504`，sensor type 12（ChicagoHS），80×64，帧 10240B

> 本文所述行为均已在真实设备上验证。实现代码见 `src/`，模块说明见根目录 README。

## 目录

- [0. 总览](#0-总览)
- [1. USB 传输](#1-usb-传输)
- [2. 命令层（A0 明文帧）](#2-命令层a0-明文帧)
- [3. 初始化序列](#3-初始化序列)
- [4. PSK 生命周期与白盒 AES](#4-psk-生命周期与白盒-aes)
- [5. TLS 加密层](#5-tls-加密层)
- [6. FDT 手指检测与基线](#6-fdt-手指检测与基线)
- [7. OTP 与配置补丁](#7-otp-与配置补丁)
- [8. 图像格式（type 12 ChicagoHS）](#8-图像格式type-12-chicagohs)
- [9. WBF IOCTL 层（历史存档）](#9-wbf-ioctl-层历史存档)

## 0. 总览

设备 = SPI 指纹传感器 + STM32 MCU（USB CDC 桥）。通信分两层：

1. **明文命令层**：A0 帧，主机↔MCU 命令/响应/事件推送。
2. **TLS 加密层**：B0 帧承载 TLS record。主机是 **server**（mbedtls），设备是 client，套件 `TLS-PSK-WITH-AES-128-GCM-SHA256`（0x00A8），PSK identity `Client_identity`，锁定 TLS 1.2。

TLS 激活后两层**并存**（USB 数据帧 state≥16）：A0 明文帧照常直接分流，B0 帧解密后按同一 payload 格式分流。FDT 事件帧与图像帧在两条通道上都可能出现——接收端必须混合处理。此外 TLS 激活后 MCU 对每条明文命令额外推一帧 `cmd=0xB0 data=[原cmd,0x03]` 的通知帧（忽略即可）。

## 1. USB 传输

### 1.1 端点（描述符实测）

- CDC-ACM 设备，2 个接口：
  - iface 0（CDC comm）：EP 0x82 INT-IN 8B
  - iface 1（CDC data）：EP 0x01 **BULK-OUT** 64B / EP 0x81 **BULK-IN** 64B
- 使用 iface 1 的 bulk 0x01/0x81；Linux 需 detach `cdc_acm`。

### 1.2 CDC 激活（冷启动关键）

打开设备后必须发 **SET_LINE_CODING（115200 8N1）+SET_CONTROL_LINE_STATE（DTR+RTS）**。不发这两个控制请求，设备会静默丢弃所有命令（早期"设备零响应"的根因）。

### 1.3 帧头（4 字节）

```
byte[0]   : type  0xA0=明文  0xB0=TLS record
byte[1..2]: payload 长度（LE，不含 4 字节头）
byte[3]   : 头校验 = (b0+b1+b2) & 0xFF
```

## 2. 命令层（A0 明文帧）

### 2.1 命令字节与帧布局

```
命令字节 v22 = (cmd0<<4) | (cmd1<<1) | more(bit0)
```

more 位：在 ack_tmo==0（fire-and-forget）时置位。

```
[0..3]  4 字节帧头（0xA0）
[4]     v22
[5..6]  len16 = 数据长度 + 1（LE）
[7..]   数据（len 字节）
[7+len] 数据校验 = 0xAA - (sum(data) + (len+1)&0xFF + (len+1)>>8 + v22) & 0xFF
```

帧按 64B USB 包分片发送。

### 2.2 命令表

| cmd 字节 | (cmd0,cmd1) | 功能 | data | 说明 |
|---|---|---|---|---|
| 0x01 | (0,0) | NOP | {0,0,0,0} | 版本读取前置 |
| 0x20 | (2,0) | **SetMode Image** | {1,0} | 直接曝光推一帧图像 |
| 0x32 | (3,1) | **FDT down 布防** | {8,1,down表12B,ts16} 或 {8,0,t0,t1} | 手指按下检测 |
| 0x34 | (3,2) | **FDT up 布防** | {10,1,up表12B} | 手指抬起检测 |
| 0x36 | (3,3) | **FDT manual 采样** | {9,1,down表12B} | MCU 现场采无手指基线，回推 IrqStatus=0x100 |
| 0x82 | (8,1) | ChipRegRead | {0,reg16,len16} | → 寄存器数据 |
| 0x90 | (9,0) | DownloadConfig | 224B 配置表 | OTP 补丁后下发 |
| 0x97 | (9,3) | SetDriverState | {state,2} | |
| 0xA2 | (10,1) | ResetSensor/MCU | {flags,20} | 1=reset sensor, 2=reset MCU, 3=两者 |
| 0xA4 | (10,2) | ClearApp | {0,0} | 固件更新前擦除 |
| 0xA6 | (10,3) | ReadOtpData | {0,0} | → OTP（type 12/14 有效 64B） |
| 0xA8 | (10,4) | GetEvkVersion | {0,0} | → 版本串（"GF_ST411SEC_APP_12508"） |
| 0xAF | (10,7) | GetMcuState | {0x55,ts16,0,0} | → 16B MCU 状态 |
| 0xD1 | (13,0) | TLSServerInit | {0,0} | 让 MCU 开始发 ClientHello |
| 0xD2 | (13,1) | POV WakeupMCU | {0,0} | 省电唤醒取缓存图像 |
| 0xD4 | (13,2) | TLS 握手确认 | {0,0} | 主机握手完成后发送 |
| 0xDA | (13,5) | TLS reconnect | 设备推送 | 设备请求重协商 |
| 0xE0 | (14,0) | MCU write | TLV blob | 生产数据写入（PSK 等） |
| 0xE4 | (14,2) | MCU read | {type32,0} | 生产数据读取 |
| 0xF0 | (15,0) | FW slice | {off32,len32,data} | 1008B 分片 |
| 0xF4 | (15,2) | FW finish | {0,0,len32,crc32} | 校验后硬复位 |

ACK：每条命令后设备回 `cmd=0xB0 data=[原cmd,0x01]`（TLS 激活后是 `[原cmd,0x03]`）。命令响应数据的 event 号：2=image 3=fdt 5=nav 8=regrw 9=config 10=other 12=notice 13=production 14=prod-resp 15=fw。

### 2.3 GetMcuState 状态字节

响应 16B，`state[1]` 关键位：
- bit0 = isPOVImageValid（有省电缓存图像 → capture 走 0xD2 POV 路径）
- bit1 = isTlsConnected（TLS 握手确认依据）
- bit3 = isLocked（设备锁定，等 TLS 解锁）

## 3. 初始化序列

```
设备初始化:
  SetBesDenable(0)                 # 本地标志，无 USB 流量
  MCU 初始化:
    GetEvkVersionWithRetry         # NOP+0xA8 x N；全失败 → 0xA2{2,20}
                                   # 软复位唤醒 → 再试 → 0xA2{3,20} → USB 复位
    SetDriverState (0x97 {1,2})
    GetMcuState (0xAF)
    固件更新（无条件进入，内部判版本）:
      版本相同(非 IAP)  → 跳过
      版本不同(非 IAP)  → ClearApp(0xA4) + 软复位
      IAP              → 0xF0 分片下载 + 0xF4 完成 + 0xA2{2,20} 硬复位
                         （USB 重枚举，需重新 open；init 本轮终止）
    PSK 生命周期             # 见 §4
  传感器初始化:
    ResetSensor (0xA2 {1,20})
    ChipRegRead (0x82) → chipid → sensor type/几何/配置表选择
    ReadOtpData (0xA6) → OTP 解析（OTP 补丁 224B 配置表，见 §7）
      → FDT 初始化（判别参数装填，见 §6.4）
      → DownloadConfig (0x90 下发补丁后配置)
    基线存在性检查   # 加载 goodix.dat（CRC + OTP 绑定）
  SetBesDenable(1)
TLS 握手:
  TLSServerInit (0xD1) → 收 ClientHello → mbedtls 握手 →
  SendTlsHandshakeCmd (0xD4) → GetMcuState bit1=1 确认
基线采样（无基线文件时）:
  0x36 FDT-manual 采样 ×2（delta 稳定性比对）→ 导航基线采样 →
  0x20 SetMode Image 采无手指图像基线 → 0x36 再采样 → 存 goodix.dat
```

固件块（`firmware/st411sec_app.bin`）：`[21B 头]["GF_ST411SEC_APP_12508"][128404B 数据][4B 尾]`；运行时校验用**非反射 CRC32**（poly 0x04C11DB7，初值 0xFFFFFFFF，无最终异或）= 0x6FBC0F40；文件自校验 zlib CRC32 = 0x98BA66C2。

## 4. PSK 生命周期与白盒 AES

### 4.1 生命周期

**PSK 不是出厂固定的**：首次初始化时主机生成随机 32B PSK，白盒加密后写入 MCU；同时本地持久化明文（root 为 `/var/lib/fprint/goodix/psk.bin`，普通用户为 `~/.config/goodix/psk.bin`，0600 权限）——MCU 从不校验本地存储格式，任何主机侧存储都行。

```
写（MCU write, cmd 0xE0）TLV:
  [0xBB010002][len][seal 72B][entropy 8B][0xBB010003][len][wb 102B]
  （190B → 4 字节对齐补到 192B；MCU 只解 WB 段，seal 可为占位值）
读（cmd 0xE4, data={type32,0}）:
  0xBB010003 → WB blob（恢复 PSK：WB 解密）
  0xBB020003 → MCU 对 WB 算的 SHA256（32B，验证用：
               host 算 SHA256(wb) 与之比对，一致即证明 WB 加解密对称）
响应数据布局: [status 1B][data_type 4B][len 4B][payload]；status=1 表示无数据
```

### 4.2 白盒 AES（加密 / 固件解密，逐字节对齐）

```
v40 = derive16("Goodix") || derive16({0A,0E,0D,06,16,04})   # 32B HMAC key
KDF = HMAC-SHA256(v40, bswap32(i) || "kgoodwixg\0" ||
      "kaelrgnoerlithm" || bswap32(0x180))，i=1,2 → 48B
AES-256 key = KDF[0..31]；块 HMAC key = KDF[16..47]

derive16：6 输入字节各 ror 1/3/5/7 → v24[24]；
  out[2i+2..2i+3] = AES_dec/enc(v24[3i+3..3i+5] || 0xCC×13, 零 key)（无链）；
  out[0..1]=SHA256(v24[0..2]) 截断；out[10..11]=HMAC("123456"+10×00)；
  out[12..13]=CRC32-MSB；out[14..15]=SHA256(v24[21..23])

WB 块（102B）：
  v42 = SHA256(0xFF02 || len32le || plain[0..7] || 16×dword(3))
  wb[0..31]  = HMAC-SHA256(KDF[16..47], FF02 || len || wb[54..101])
  wb[32..33] = 0xFF02；wb[34..37] = len LE；wb[38..53] = v42[0..15]（明文）
  J0  = AES_dec(AES_dec(v42[0..15]) XOR [0×15,0x80])
  cum = AES_dec(IV)，IV = 52 2D C1 F0 99 56 7D 07 F4 7F 37 A3 2A 84 42 7D
  wb[54..85] = CTR：counter=J0 先递增（字节15→12）再 AES_enc
  每块: cum = AES_dec(cum ^ ct)
  lenblock = [0×7, 0x80, HIBYTE(8*len), 0×6, LOBYTE(8*len), 0]
  tag = AES_enc(J0) ^ AES_dec(cum ^ lenblock)  → wb[86..101]
```

固件侧先验证 wb[0..31] 的 HMAC，失败则拒绝解密（设备退回旧/默认 PSK → TLS Finished 必炸）。MCU hash 一致只证明 blob 存储相同，**不证明能解密**。

## 5. TLS 加密层

- 角色：主机 = server，设备 = client（设备在 0xD1 后主动发 ClientHello）
- 套件：0x00A8（TLS-PSK-WITH-AES-128-GCM-SHA256，mbedtls 编号；ClientHello 只带 0x00A8 + 0x00FF）
- identity：`Client_identity`（15B）；固定 PSK 模式（conf_psk，非回调）
- 版本：锁定 TLS 1.2（min=max=3,3；mbedtls 3.x 默认会协商 1.3 → 必失败）
- 帧：B0 [len16][cks][TLS record]
- **流式 BIO**：mbedtls 按 record 头/体分段 recv，必须把每个 B0 帧解出的字节追加进聚合缓冲区供其流式读取；每帧发送后 Sleep(10ms)
- 握手完成后发 0xD4 确认，GetMcuState bit1=1
- 设备可推 0xDA 请求重协商
- 采集等待期：tls 的 bio_recv 不能私吞 A0 明文帧——A0 直接分流，B0 注入 ssl_read 后按同一 payload 格式分流（本项目 `gx_tls_capture_mode` / `gx_tls_feed` 机制）

## 6. FDT 手指检测与基线

### 6.1 基线文件 goodix.dat

```
[OTP 64B][FDT down 表 12B][navbase 3200B][imagebase 10240B][CRC32 4B LE]
CRC = CRC-32/MPEG-2（poly 0x04C11DB7，初值 FFFFFFFF，无最终异或）
加载校验：CRC + OTP 逐字节绑定（换设备/老化自动作废重采）
```

### 6.2 基线来源与学习公式

- 无文件时 init 跑基线采样：0x36 FDT-manual 让 MCU 现场采样，设备回推 cmd0=3 `IrqStatus=0x100` 帧（data=[irq 2B][touchflag 2B][raw 12B]）
- 合法性：每个 u16 的 `(v>>1)&0xFF ∉ {0,0xFF}`
- down 表：`w = ((raw>>1)<<8) | 0x80`
- up 表（finger-down IRQ IrqStatus=0x2 顺带学习）：`w = ((delta + (raw>>1))<<8) | 0x80`；touchflag 第 i 位为 0 的槽位强制 `((delta-2)<<8) | 0x80`
- delta 来自 OTP（本机 29），fallback 21

### 6.3 采集链

```
host TX 0x32 [8,1,down表12B,ts16]   # FDT down 布防
dev  RX cmd0=3 IrqStatus=0x2        # 手指按下（顺手学 up 表）
host TX 0x20 [1,0]                  # SetMode Image → MCU 曝光
dev  RX cmd0=2 图像帧               # 可能走明文也可能走 TLS
host 主机侧手指判别（§6.4）:
       1 finger      → 交付图像
       2 void / 3 bad → 重新布防 0x32 继续等
       0 temperature → 重建全部基线 → 再布防
host TX 0x34 [10,1,up表12B]         # 收尾：布防手指抬起
```

IrqStatus 值：0x2=finger down，0x200=finger up，0x100=manual 采样回报，0x80/0x82=reverse，0x800=fp reset。

### 6.4 主机侧手指判别

新帧与无手指图像基线（goodix.dat imagebase）做 8x8 分块统计比较，mode-0 路径（8x8 块网格）。参数按 sensor type 装填：

| type | 几何 | 网格 | 块起点（行/列） | need | 阈值 T |
|---|---|---|---|---|---|
| 2 MilanG | 176x54 | 2x5 | {8,38} / {12,48,84,120,156} | 6 | tcode·16·delta/256 |
| 3 MilanL | 112x132 | 3x4 | {8,52,96} / {8,44,80,116} | 7 | tcode·16·delta/256 |
| 12 ChicagoHS | 80x64 | 2x3 | {12,44} / {4,36,68} | 4 | tcode·16·delta/128 |
| 14 ChicagoT | 36x160 | 5x2 | {12,44,76,108,140} / {4,24} | 7 | tcode·16·delta/256 |

每块（64 像素）计算：mean_base、mean_new、mean|diff|、diff 方差（Σ(mean_d−dᵢ)²/63）。hi=(int)(1.4T)，lo=(int)(0.6T)。

```
v38 = #{块: 方差 > hi}
v39 = 任意块 mean|diff| > hi
v40 = #{块: lo < mean|diff| < hi}
v41 = 任意块 mean_new > mean_base + hi          （新帧异常变亮）
v51 = #{内部像素(去边框): new > base+32} ≥ (w·h − 2(w+h) + 4)·0.1
v52 = #{内部像素: base > new+32} ≥ 同上

if (v38 ≥ need)   return (v51 && !v52) ? 3(bad) : 1(finger)
elif (v39)        return v41 ? 3(bad) : 1(finger)
elif (v40 < need) return 2(void)
else              return 0(temperature)
```

## 7. OTP 与配置补丁

ReadOtpData（0xA6）→ OTP 解析与配置补丁 → 补丁 224B 配置表 → DownloadConfig（0x90）。不补丁 = FDT 阈值/曝光全是通用默认值，手指检测不触发。

- OTP CRC 分组（CRC-8 poly07 取反）：cp→OTP[60]，ft→OTP[61]，mt→OTP[63]
- DAC（曝光）：ft 组 OTP[50..53] 优先（CRC 自验 OTP[62]），mt 组 OTP[46..49] 次之，最后多数表决；→ reg 0x220=`(16·dac0)|8`、0x236/0x238/0x23A = dac1..3（section 0 全写）
- tcode_diff：OTP[42]/[43]/[45] 三取一（互为取反校验）；tcode=`16·((diff>>4)+1)+64` → reg 0x5C（s0）；delta=`((100·((diff&0xF)+2)<<8)/tcode/3)>>4` → reg 0x82 高字节（s2）
- fdtOffset：OTP[27] 2bit 字段三取一 → 非零时 reg 0x56 低字节写 off+4（s2）
- 配置表项 = [reg16][val16]，header 第 s 段偏移/长度在 cfg[2s+1]/cfg[2s+2]；改完重算校验和 `−(0xA5A5 + Σu16[0..110])` 存 byte 222..223

tcode/delta 同时写入判别阈值供 §6.4 使用。

## 8. 图像格式（type 12 ChicagoHS）

线上帧（cmd0=2）：`data[0]=标记`（0xAA=POV 帧，跳过），像素在 `data[5..]` 共 7684B = 7680B 12bit 打包像素（列优先）+ 4B CRC-32/MPEG-2（计算范围不含自身；存储为半字大端：`crc = b1|b0<<8|b3<<16|b2<<24`）。

解包（12bit 列优先像素解包）：每 6B 出 4 像素

```
p0 = b1 + ((b0 & 0xF) << 8)     p1 = (b0 >> 4) + 16·b3
p2 = b2 + ((b5 & 0xF) << 8)     p3 = (b5 >> 4) + 16·b4
```

像素流列优先（每列 64px），解包时转置：`dst = k/64 + 80·(k%64)`，输出 80x64x16bit LE（有效值 12bit）= 10240B。

其它 sensor type 几何（按 chipid 选择）：

| type | chipid | 几何 | 帧字节 | 配置表 |
|---|---|---|---|---|
| 2 MilanG | 0x2208 | 176x54 | 19008 | 静态配置表 type2 |
| 3 MilanL | 0x2205 | 112x132 | 29568 | 静态配置表 type3 |
| 12 ChicagoHS | 0x2503/0x2504 | 80x64 | 10240 | 静态配置表 type12 |
| 14 ChicagoT | 0x2505-0x2508/0x2510 | 36x160 | 11520 | 静态配置表 type14 |

224B 静态配置表（每 type 一份）见 `src/goodix_init.c`。

## 9. WBF IOCTL 层（历史存档）

| IOCTL | 处理 |
|---|---|
| 0x440004 | OnGetAttributes |
| 0x440008 | OnReset |
| 0x44000C | OnCalibrate |
| 0x440010 | OnGetStatus |
| 0x440014 | OnCaptureData（→ FDT down 布防） |
| 0x442008 | OnGetSensorInfo |
| 0x442020 | OnActivate |
| 0x442030 | OnPBAOperation |

设备接口 GUID：`{E2B5183A-99EA-4CC3-AD6B-80CA8D715B80}`
