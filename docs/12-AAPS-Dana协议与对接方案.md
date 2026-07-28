# AAPS 对接方案 — Dana 协议分析与传输层决策

> ⚠️ **实验项目，禁止用于人体。** 本文档仅为理论验证 / 教学原型用途，记录如何让本 DIY 设备的蓝牙层"伪装"成一款 AAPS 原生支持的胰岛素泵（**Dana-i over BLE**），从而被 AndroidAPS 直接识别与驱动。任何实现**严禁接入真实人体**的胰岛素输注。
>
> 所有换算仍走 `dosing.h` 单一真源、所有电机控制走 `execute_command()` 唯一入口，不破坏既有架构。

---

## 0. 核心结论（先说重点）

1. **AAPS 不会"学习"我们的私有协议**——它只认自己支持的几款商用泵。要让 AAPS 驱动我们的设备，设备必须**在蓝牙层 impersonate（伪装成）一款 AAPS 支持的泵**。
2. 已选定路线 **方案 B：impersonate Dana-i，保持 BLE 传输栈**（用户已在 AskUserQuestion 中确认）。理由：匹配我们现有 NimBLE BLE 固件，无需换传输栈。
3. **权威实现来源 = AAPS 自己的 `pump/danars` 模块**（`danars` = DanaRS + Dana-i 共用 BLE 客户端）。本文 §6/§7 的 GATT、信封、加密、握手全部逐行取自该模块（`BLEComm.kt` / `BleEncryption.kt` / 各 `DanaRSPacket*.kt`），是确保"被 AAPS 认出"的唯一可靠依据。
4. **关键更正（相对旧版 §3）**：Dana-i BLE 使用的**不是**经典 DanaRS SPP 的 2 字节 opcode（如 `0x0102`/`0x0401`/`0x0407`），而是 **Dana-i / DanaRS-v2 的新单字节 opcode 体系**（`0x4A` 步进大剂量、`0xC1` APS 临时基础率、`0x47` 方波等）。旧 §3 的 2 字节 opcode 仅适用于"方案 A（DanaRS SPP）"，对 Dana-i BLE 无效。

---

## 1. AndroidAPS 支持的泵（传输方式）

| 泵 | 传输 | 备注 |
|---|---|---|
| DanaR / DanaRS | **经典蓝牙 SPP（RFCOMM）** | 协议简单，但需换传输栈 |
| **Dana-i** | **BLE GATT（UART 风格隧道）** | ✅ 我们选定 impersonate 的对象；协议见 §6/§7 |
| Omnipod DASH | BLE | 协议复杂 |
| Accu-Chek Insight | BLE | 协议复杂 |
| Diaconn G8 / EOPatch / Medtrum | BLE | 专有、复杂 |

> 结论：**Dana-i 是 DIY impersonation 的最优目标**——AAPS 原生支持、协议公开、被 LoopKit 的 DanaKit 与 AAPS 的 `danars` 模块双重实现，且匹配我们现有 BLE 固件。

---

## 2. DanaRS 线协议（方案 A 参考；Dana-i BLE 不使用此帧）

> 仅作"方案 A（换 SPP 栈）"的备选参考。Dana-i BLE（方案 B）用的是 §6 的 `A5 A5…5A 5A` 信封，**不是**下面的 `7E 7E…2E 2E` 帧。

### 2.1 帧格式（经典 SPP，双向一致）
```
7E 7E <len> F1 <CMD_H> <CMD_L> [data...] <CRC_H> <CRC_L> 2E 2E
```
- `<len>` = `3 + (F1 + CMD + data 的字节数)`；整帧总长 = `len + 7`。
- CRC-16/CCITT-FALSE：多项式 `0x1021`，初值 `0x0000`，查表法、大端；计算区间 = `bytes[3 .. 3+len)`。
- 经典 SPP 的 opcode 是 **2 字节**（如 `0x0102` 大剂量、`0x0401` 临时基础率、`0x0407` 方波），与 Dana-i BLE 的单字节 opcode 体系**不同**。

---

## 3. 命令集（Dana-i BLE 单字节 opcode，AAPS `danars` 实测）

> ⚠️ 下表是 **Dana-i / DanaRS-v2 BLE** 的 opcode（单字节，信封 TYPE=`0xA1`/`0xB2`/`0xC3`）。若误用经典 2 字节 opcode（§2），AAPS 会直接丢弃或断连。

| AAPS 常量 | opcode | 方向 | 请求参数 | 响应 | 本固件映射 |
|---|---|---|---|---|---|
| `BOLUS__SET_STEP_BOLUS_START` | `0x4A` | 命令 | `[amt_lo, amt_hi, speed]`：amt=U×100(int16 LE)，speed 0/1/2=12/30/60 s·U⁻¹ | 1B 错误码(0=OK) | → 常规大剂量（连续输注，`speed` 映射注射速度） |
| `BOLUS__SET_STEP_BOLUS_STOP` | `0x44` | 命令 | 无 | 1B | → 中止大剂量 |
| `BASAL__APS_SET_TEMPORARY_BASAL` | `0xC1` | 命令 | `[pct_lo, pct_hi, durCode]`：pct=0–500(相对基础率%)，durCode=150(15min,pct≥100) 或 160(30min,pct<100) | 1B(0=OK) | → **闭环临时基础率**（AAPS 闭环专用） |
| `BASAL__SET_TEMPORARY_BASAL` | `0x60` | 命令 | 同 `0xC1` | 1B(0=OK) | → 手动临时基础率（可选支持） |
| `BASAL__CANCEL_TEMPORARY_BASAL` | `0x62` | 命令 | 无 | 1B | → 停临时基础率 |
| `BOLUS__SET_EXTENDED_BOLUS` | `0x47` | 命令 | `[amt_lo, amt_hi, halfHours]`：amt=U×100(int16 LE)，halfHours=1–16(0.5–8h) | 1B(0=OK) | → **方波大剂量**（复用连续慢滴 `5b9baf2`） |
| `BOLUS__SET_EXTENDED_BOLUS_CANCEL` | `0x49` | 命令 | 无 | 1B | → 停方波 |
| `REVIEW__INITIAL_SCREEN_INFORMATION` | `0x02` | 命令(查询) | 无 | 17–18B 状态块（见 §3.1） | → 状态合成值 |
| `REVIEW__GET_TODAY_DELIVERY_TOTAL` | `0x26` | 命令(查询) | 无 | 日总量 | 合成值 |
| `BOLUS__GET_STEP_BOLUS_INFORMATION` | `0x40` | 命令(查询) | 无 | 大剂量进度 | 大额量剩余量 |
| `BOLUS__SET_DUAL_BOLUS` | `0x48` | 命令 | `[amt_lo, amt_hi]` U×100(int16 LE) + `[immediate%]` + `[duration]`（双波参数） | 1B(0=OK) | 双波大剂量（⚠️ **非 CGM 血糖**！AAPS 当前不发此命令；教学原型未实现→收到返回 OK）。CGM 屏显请走自定义 BLE `g_ch_cgm` |
| `OPTION__GET_PUMP_TIME` / `SET_PUMP_TIME` | `0x70` / `0x71` | 命令 | 日期时间字节(年-2000,月,日,时,分,秒) | 0x70 回 6B / 0x71 收 6B | ✅ **已落地**：0x70 回当前时间、0x71 设设备时钟（见 §9.3） |
| `NOTIFY__DELIVERY_COMPLETE` | `0x01` | **通知**(泵→手机) | 无（TYPE=`0xC3`） | — | 大剂量结束时主动上报 |
| `NOTIFY__DELIVERY_RATE_DISPLAY` | `0x02` | **通知**(泵→手机) | `[dlv_lo, dlv_hi]`：已输注=×100(int16 LE) | — | **大剂量进度**（每次回传剩余/已输注量） |
| `NOTIFY__ALARM` | `0x03` | **通知**(泵→手机) | 报警码 | — | 报警/错误 |

### 3.1 `REVIEW__INITIAL_SCREEN_INFORMATION` (0x02) 响应布局（17–18 字节，TYPE=`0xB2`）
| 偏移 | 含义 |
|---|---|
| 0 | status：bit0=暂停，bit4=临时基础率进行中，bit0x04=方波进行中，bit0x08=双波进行中 |
| 1–2 | 日总量(int16)/100 |
| 3–4 | 最大日总量(int16)/100 |
| 5–6 | 储药器余量(int16)/100 |
| 7–8 | 当前基础率(int16)/100 |
| 9 | 临时基础率百分比(1B) |
| 10 | 电量(1B) |
| 11–12 | 方波绝对速率(int16)/100 |
| 13–14 | IOB(int16)/100 |
| 15 | (protocol≥10) 错误状态(1B) |

---

## 4. 本固件改造映射（方案 B / Dana-i BLE）

| AAPS 命令 | 现有固件能力 | 改造点 |
|---|---|---|
| `0x4A` 步进大剂量 | `motor_deliver_bolus`（0.05U/段 + 段间1s 连续） | 直接驱动；speed→注射速度 |
| `0xC1` APS 临时基础率 | `basal_scheduler` 已有 TBR | percent→U/h 换算后下发 |
| `0x47` 方波 | `extended_bolus_tick`（15s 细拍连续慢滴，`5b9baf2`） | 复用；时长 = halfHours×0.5h |
| `0x44`/`0x49`/`0x62` 停止 | 现有中止逻辑 | 直接映射 |
| `NOTIFY__DELIVERY_RATE_DISPLAY`(0x02) | `BolusProgressData` | 输注中按节奏主动回传已输注量 |
| `NOTIFY__DELIVERY_COMPLETE`(0x01) | — | 大剂量结束主动上报 |
| `0x02` 状态 | `g_pump_state` | 填合成但自洽的值（储药器/电量/基础率/日总量/IOB） |

**所有换算仍走 `dosing.h` 单一真源、`execute_command()` 唯一电机入口**。

---

## 5. 传输层决策（已定：方案 B）

### ✅ 方案 B — impersonate **Dana-i**，保持 BLE（本次实施）
- 匹配现有 NimBLE BLE 固件，无需换传输栈。
- Dana-i ≈ DanaRS 协议，只是把 `A5 A5…5A 5A` 信封塞进 BLE 的"写特征 FFF2 + 通知特征 FFF1"隧道。
- 全部 GATT / 加密 / 握手细节已从 AAPS `danars` 模块逐行确认（§6/§7）。

### （备选）方案 A — impersonate DanaRS over 经典 SPP
- 协议在经典 SPP 快照完整验证、风险最低，但需放弃现有 BLE 层（重写传输栈）。仅作为"若 BLE  impersonation 受阻"的退路。

---

## 6. Dana-i BLE 传输细节（权威，取自 AAPS `danars`）

### 6.1 GATT 服务 / 特征
| 项 | UUID | 说明 |
|---|---|---|
| 服务 | `0000fff0-0000-1000-8000-00805f9b34fb` | 主服务 |
| 读 / 通知特征 | `0000fff1-0000-1000-8000-00805f9b34fb` | 泵→手机（开 notify） |
| 写特征 | `0000fff2-0000-1000-8000-00805f9b34fb` | 手机→泵（`WRITE_TYPE_NO_RESPONSE`） |
| CCCD | `00002902-0000-1000-8000-00805f9b34fb` | 开启 FFF1 通知 |

- 写使用 **无响应写**（write-without-response），分包每片 ≤ 20 字节。
- 设备名（`device.name`）**必须正好 10 个字符**，且匹配正则 `^[a-zA-Z]{3}[0-9]{5}[a-zA-Z]{2}$`（如 `DAN12345AB`）——AAPS 在 `getEncryptedPacket` 中 `assert(deviceName.length == 10)`，DanaKit 用同样正则校验。

### 6.2 信封格式（所有 Dana-i BLE 包）
```
[0]=A5  [1]=A5  [2]=LEN  [3]=TYPE  [4]=OPCODE  [5..]=PARAMS  [LEN+3]=CRC_H  [LEN+4]=CRC_L  [LEN+5]=5A  [LEN+6]=5A
```
- 总长 = `LEN + 7`；`LEN = 2 + nParams`（TYPE+OPCODE 占 2，再加参数）。
- `TYPE` 取值：`0x01`=加密请求、`0x02`=加密响应、`0xA1`=命令、`0xB2`=命令响应、`0xC3`=通知。
- `OPCODE` = §3 的单字节 opcode（加密层命令另有 `0x00`=PUMP_CHECK、`0x01`=TIME_INFORMATION、`0xD0`=CHECK_PASSKEY、`0xD1`=PASSKEY_REQUEST、`0xD2`=PASSKEY_RETURN）。

### 6.3 CRC（**自定义 Dana CRC，非标准 CCITT**）
计算区间 = 信封 `bytes[3 .. 3+LEN)`（即 TYPE+OPCODE+PARAMS）。初值 `0`，逐字节：
```
crc = 0
for b in range:
    result = ((crc << 8) | (crc >> 8)) & 0xFFFF      // 16-bit 字节交换/旋转
    result ^= b
    result ^= (result & 0xFF) >> 4
    result ^= (result << 12) & 0xFFFF
    if (BLE5 且 握手已完成/connectionState==2):
        result ^= ((result & 0xFF) << 4) | (((result & 0xFF) >> 3) << 2)
    else:   // DEFAULT / RSv3(conn0,1) / BLE5(conn0,1 握手期)
        result ^= ((result & 0xFF) << 3) | (((result & 0xFF) >> 2) << 5)
    crc = result & 0xFFFF
return crc
```
> ⚠️ **这是与标准 CCITT 不同的变体**，分阶段（握手期 vs 握手后）公式不同。固件必须逐字移植 AAPS `BleEncryption.generateCrc`，并用抓包向量自测，否则 AAPS 直接以 CRC 失败断开。

### 6.4 一级信封混淆（设备名 XOR）
`encodeArrayBySn`：用设备名派生 3 字节 `codingBytes`（前3字符和、中5字符和、后2字符和），对信封 `bytes[3 .. size-3]`（即 `buf[3]` 到 `buf[size-3]`，**含 TYPE / OPCODE / PARAMS / CRC_H / CRC_L**，不含开头 3 字节 `A5 A5 LEN` 与结尾 2 字节 `5A 5A`）按 `bytes[i+3] ^= codingBytes[i%3]` 混淆（循环 `i ∈ [0, size-6]`）。收发两端均施加一次（XOR 自逆）。

> ⚠️ **区间修正（逐字核对 AAPS 源码后）**：混淆区间**包含 CRC_L**（旧版含糊的"不含首尾 4 字节"是错误的）。确切循环是 `i: 0 .. size-6`，`buf[i+3]`，故末位异或字节为 `buf[size-3]` = CRC_L。固件必须 XOR 到 CRC_L 为止，否则 AAPS 解析错位。

### 6.5 二级加密 BLE_5（`enhancedEncryption == 2`，确定性混淆，**非 AES**）
- 由 6 位 ASCII 数字 `ble5Key`（如 `"123456"`）经 `secondLvlEncryptionLookupShort`（即 AAPS `bleEncryptionMatrix`，100 字节，见 §7.3）推导 3 字节密钥：
  ```
  k0 = matrix[(key[0]-'0')*10 + (key[1]-'0')]
  k1 = matrix[(key[2]-'0')*10 + (key[3]-'0')]
  k2 = matrix[(key[4]-'0')*10 + (key[5]-'0')]
  ```
- **加密**（每个字节，先整体替换首尾标记）：
  ```
  if (buf[0]==A5 && buf[1]==A5) { buf[0]=AA; buf[1]=AA }
  if (尾两字节==5A 5A)         { 尾=EE; 尾=EE }
  for each byte b:
      b = (b + k0) & 0xFF
      b = swap_nibbles(b)          // (b>>4)|(b<<4)
      b = (b - k1) & 0xFF
      b = b ^ k2
  ```
- **解密**（逆序，AAPS 在解密后**不**把 AA/EE 还原成 A5/5A——解析器同时接受 `A5 A5 / AA AA` 起、`5A 5A / EE EE` 止）：
  ```
  for each byte b:
      b = b ^ k2
      b = (b + k1) & 0xFF
      b = swap_nibbles(b)
      b = (b - k0) & 0xFF
  ```

### 6.6 握手状态机（BLE_5）
1. 手机连接 → 发现 FFF0 → 发现 FFF1(notify)+FFF2(write) → 写 CCCD `2902` 开通知 → 触发首包。
2. **手机发 `PUMP_CHECK`**（opcode `0x00`）：信封 `A5 A5 0C 01 00 <设备名10B> CRC CRC 5A 5A`（再经 6.4/6.5）。
3. **设备必须回 `PUMP_CHECK` 响应**（解密后 14 字节）：
   ```
   [0]=TYPE_ENCRYPTION_RESPONSE(0x02)
   [1]=OPCODE_PUMP_CHECK(0x00)
   [2]='O'(0x4F)  [3]='K'(0x4B)
   [4]=未用  [5]=HW_MODEL  [6]=未用  [7]=PROTOCOL
   [8..13]=BLE5_KEY(6 字节 ASCII 数字，byte[8] 必须非 0)
   ```
   - **HW_MODEL 必须是 `0x09` 或 `0x0A`**（Dana-i），否则 AAPS 断连。
   - `BLE5_KEY` 为本设备预设的固定 6 位数字串（如 `"123456"`）；AAPS 首次配对后将其存入偏好，后续连接用缓存值——故固件必须**每次都发同一把 key**。
4. 手机 `setBle5Key(缓存key)` → 发 `TIME_INFORMATION`（opcode `0x01`，参数 4 字节全 0）。
5. **设备回 `TIME_INFORMATION` 响应**：解密后 `[0]=0x02 [1]=0x01 ['O']['K']`（任意合法信封即可，BLE_5 下 AAPS `processEncryptionResponse` 仅置 `isConnected=true`，不校验内容）。→ 握手完成。

> 配对（bonding）走标准 Android BLE 绑定（6 位 PIN，实际由系统处理）。我们的 key/设备名均为**设备端固定常量**，无需用户交互即可被 AAPS 绑定。

### 6.7 ⚠️ 关键更正：握手包**不**做二级加密（逐字核对 AAPS 源码）

> 旧版 §6.6 / §7.2 隐含"所有包都走二级加密 + 同一 CRC 分支"，**这是错误的**。逐字核对 AAPS `BleEncryption.kt` / `BLEComm.kt` 得到以下决定性事实（2026-07-27 更正）：

1. **AAPS `readDataParsing` 只在 `isConnected == true` 才对收包 `decryptSecondLevelPacket`**；`sendConnect()` / `sendBLE5PairingInformation()` 发送握手包时**不**调 `encryptSecondLevelPacket`。即：**仅握手完成后的命令包才做二级加密**，握手阶段的 `PUMP_CHECK` / `TIME_INFORMATION`（双向）只做设备名 XOR，**不做二级加密**。
2. **CRC 末级分支随连接阶段切换**（§6.3 已体现）：AAPS 发握手包前强制 `connectionState = 0` 或 `1`（走"默认" CRC 末级分支）；握手完成后 `connectionState = 2`（握手完成 / 命令阶段）才走"BLE5"末级分支。**设备端必须镜像**：收完 `TIME_INFORMATION` 并回 OK 后，把本地 conn 由 `INIT`(0/1) 切到 `HANDSHAKE_DONE`(2)，此后命令包用 BLE5 CRC 分支 + 启用二级加密；握手响应包本身仍用默认分支 + 无二级加密。
3. **设备名 XOR 始终施加**（握手与命令包都做），不受连接阶段影响。

**设备端落地（`aaps_dana.cpp` 已实现）**：`dana_build_pump_check_response()` / `dana_build_time_info_response()` 调用 `dana_build_packet(..., apply_ble5=false)`（默认 CRC 分支、无二级加密）；命令响应/通知调用 `dana_build_packet(..., apply_ble5=true)`（BLE5 CRC 分支 + 二级加密）。`dana_parse_packet()` 在 `conn==HANDSHAKE_DONE` 时先二级解密，否则跳过。

---

## 7. 固件实现规格（aaps_dana BLE 模块）

### 7.1 设备固定常量（建议值，写入 `config.h`）
```c
#define DANAI_DEVICE_NAME      "DAN12345AB"   // 10 字符, 匹配 ^[a-zA-Z]{3}[0-9]{5}[a-zA-Z]{2}$
#define DANAI_BLE5_KEY         "123456"       // 6 位 ASCII 数字
#define DANAI_HW_MODEL         0x09           // Dana-i (0x0A 亦可)
#define DANAI_PROTOCOL         0x0A           // 协议号, 随便填个合理值
```

### 7.2 信封收发流水线（设备端）
**发送（响应/通知）：**
```
build: [A5 A5][LEN=2+n][TYPE][OPCODE][PARAMS...]
crc  = generateCrc(buf[3 .. 3+LEN])        // 握手期(conn=INIT)用公式A, 握手后(conn=HANDSHAKE_DONE)用公式B
buf[total-4]=crc>>8; buf[total-3]=crc&0xFF; buf[total-2]=5A; buf[total-1]=5A
encodeArrayBySn(buf)                        // 设备名 XOR (bytes[3..total-3], 含CRC_L, 见§6.4)
// ⚠️ 仅当 conn==HANDSHAKE_DONE(命令响应/通知) 才调:
secondLevelEncryptBLE5(buf)                 // AA/EE 替换 + 逐字节混淆
// 握手响应包(conn=INIT) 到此为止, 不调二级加密; 经 FFF2 无响应写, ≤20B 分包
```
**接收（命令）：**
```
secondLevelDecryptBLE5(buf)                 // 逐字节逆混淆 (AA/EE 不还原)
encodeArrayBySn(buf)                        // 撤销设备名 XOR
if buf[2] != total-7 -> 丢弃
crc = generateCrc(buf[3 .. 3+buf[2]]); if 不匹配 -> 丢弃
plain = buf[3 .. total-5]                   // [TYPE][OPCODE][PARAMS...]
```

### 7.3 `secondLvlEncryptionLookupShort` / AAPS `bleEncryptionMatrix`（100 字节, 用于 setBle5Key）
```c
static const uint8_t BLE5_MATRIX[100] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,
    0x67,0x2b,0xfe,0xd7,0xab,0x76,0x6c,0x70,0x48,0x50,
    0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,
    0x9d,0x84,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,
    0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x47,0xf1,
    0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,
    0xaa,0x18,0xbe,0x1b,0x09,0x83,0x2c,0x1a,0x1b,0x6e,
    0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0xa0,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,
    0xbe,0x39,0x4a,0x4c,0x58,0xcf,0xb0,0x54,0xbb,0x16
};
// k0 = BLE5_MATRIX[(key[0]-'0')*10+(key[1]-'0')]; 同理 k1,k2
```

### 7.4 需新建/修改的固件文件
- `code/esp32_firmware/src/aaps_dana.h` / `aaps_dana.cpp`：GATT 服务定义（FFF0/FFF1/FFF2）、NimBLE 回调、信封编解码、CRC、两级加密、握手状态机、命令分发。
- `code/esp32_firmware/src/dosing.h`：已存在，换算单一真源（不动）。
- `code/esp32_firmware/src/motor_controller.cpp`：命令经 `motor_enqueue()` / `motor_cancel_bolus()` 与 `basal_scheduler_*` 入口（内部静态 `execute_command()` 为分发函数；不动既有逻辑，仅在 `aaps_dana.cpp` 的命令分发处调用这些入口）。
- 与现有自定义 BLE 协议关系：**双模并存**——编译期宏 `USE_AAPS_DANA` 切换（或运行期按设备名/服务判定）。建议先以独立 GATT 服务并存，避免破坏本地伴生 APP 调试通道。
- `simulator/`：在 `ui_hal_sim` 增加 Dana 协议 mock（离线联调 AAPS 握手/命令）。

### 7.5 验证路径
- 宿主 C++ 单测：CRC 向量（取自 AAPS `BleEncryptionTest`/抓包）、两级加密往返、信封编解码、握手响应字节布局。
- 模拟器：用真实 `aaps_dana.cpp` + 注入的"假 AAPS 客户端"跑通 PUMP_CHECK→TIME_INFORMATION→命令→通知全流程。
- 真机：用户用 Arduino IDE 编译烧录（本机无 ESP 工具链），用真实 AndroidAPS（手机）配对 `DAN12345AB` 验证被识别。

---

## 8. 待办 / 下一步
- [x] 确定 impersonation 目标 = Dana-i（方案 B，用户已选）。
- [x] 从 AAPS `danars` 模块逐行确认 GATT / 信封 / 自定义 CRC / 设备名规则 / 两级加密 / 握手 / 命令 opcode（§3、§6、§7）。
- [x] **实现固件 `aaps_dana` BLE 模块**（§7.4）：GATT 服务(FFF0/FFF1/FFF2) + 信封 + 自定义 Dana CRC + 两级加密(含握手不二级加密, §6.7) + 握手状态机 + 命令分发到 `motor_enqueue()`/`motor_cancel_bolus()`/`basal_scheduler_*`(经 `dosing.h` 单一换算)。✅ 2026-07-27 已完成：`aaps_dana.h`/`.cpp` + `config.h` 宏 `USE_AAPS_DANA`(默认关闭) + `ble_comm.cpp` 双模挂载。
- [x] **宿主 C++ 单测 + Python AAPS 预言机字节级验证**：✅ 2026-07-27 运行通过（205 个场景全部匹配：握手包 PUMP_CHECK/TIME_INFO + 命令响应 + 通知 + 200 随机命令包；见 `code/esp32_firmware/test/` 下 `run_tests.sh` / `aaps_dana_test.cpp` / `oracle_aaps.py`）。
- [x] 与现有自定义 BLE 协议做双模并存（编译期宏 `USE_AAPS_DANA` 切换，默认关闭，避免破坏本地伴生 APP 调试通道）。
- [x] **⚠️ 2026-07-28 纠错**：早期"0x48 下发 CGM"结论**错误**——git 克隆 AAPS master 核对，`0x48`=`SET_DUAL_BOLUS`（双波大剂量），**AAPS 不经 DanaRS 下发实时 CGM**。此前 `dana_apply_glucose` 误解析已**移除**（§9.1/§9.2）；CGM 屏显仅走自定义 BLE `g_ch_cgm`。
- [x] **P0 IOB 重写**：删除单纯累加逻辑，改 Walsh 三角时间衰减（`iob_model.cpp`，删双指数宏/`math.h`），防止重复低血糖。✅ 2026-07-27 落地。
- [x] **P1 时间同步**：`0x70` GET_TIME / `0x71` SET_TIME 实现（设 ESP32 硬件 RTC）。✅ 2026-07-27 落地。
- [x] **P1b 趋势/离线 UI**：5 档趋势 + `ui_hal_glucose_valid()` 10 分钟过期/无数据判定（首页/闭环页"CGM 离线"/"--"）。✅ 2026-07-27 落地。
- [x] **P2 ESP32 硬件 RTC**：`rtc_clock.cpp` 改用 `time()`/`settimeofday()`（RTC 域 64 位，无 49 天回绕），取代 millis 累加；开机从 `rtc_base_unix` 持久化恢复。✅ 2026-07-27 落地。
- [ ] 模拟器 `ui_hal_sim` 增加 Dana 协议 mock（离线联调 AAPS 握手/命令，原 #99 待办）。
- [ ] 真机实测：用真实 AndroidAPS 配对验证被识别（本机无 ESP 工具链、无 remote，需用户以 `-DUSE_AAPS_DANA` 在 Arduino IDE 编译烧录 + 真实手机验证，原 #100 待办）。
- [x] **字节布局核对**：`0x70`/`0x71` 时间布局已与 AAPS `DanaRSPacketOptionGet/SetPumpTime.kt` **逐字节核对一致**；`0x48` 已确认 = 双波大剂量（非 CGM），无需核对（§9.7）。

---

---

## 9. CGM 屏显（自定义 BLE 通道）/ 时间同步 / IOB 模型（P0/P1/P2 落地，2026-07-28 纠错）

> ### ⚠️ 关键更正（两次迭代，最终以 AAPS 源码为准）
> 1. 早期一度误判"**AAPS 不下发血糖，CGM 搁置**"。
> 2. 后续一度反转为"**0x48 下发 CGM**"——**这同样是错误的**。
> 3. **最终结论（2026-07-28，git 克隆 AAPS master `pump/danars` 逐字节核对）**：
>    `0x48` 在 AAPS 中真实语义是 **`SET_DUAL_BOLUS`（双波大剂量）**（见 `BleEncryption.kt`：`DANAR_PACKET__OPCODE_BOLUS__SET_DUAL_BOLUS = 0x48`）；
>    **AAPS 不通过 DanaRS/BLE 协议向泵下发实时 CGM 血糖**（APS 系列仅 `0xC1` TBR、`0xC2/C3` 历史事件，无任何 glucose 下行命令）。
>    → 固件此前 `dana_apply_glucose()`（把 0x48 当血糖）**已移除**，避免真机联调时把双波剂量参数误当血糖 → 假低血糖风控误判。
>    → **CGM 屏显改走自定义 BLE 通道 `g_ch_cgm`（`ble_comm.cpp` 直写 `last_glucose_mgdl`/趋势/接收时刻）**，与 UI 解耦。

### 9.1 `0x48` 实际语义 + CGM 正确通路（2026-07-28 纠错）
- **`0x48` = `SET_DUAL_BOLUS`（双波大剂量）**，绝非 CGM 血糖。AAPS `pump/danars` 当前版本既不发送 0x48、也无任何"手机→泵下发实时血糖"的命令（全模块 grep 无 `APS_Glucose` 下行 packet）。
- 固件此前 `dana_apply_glucose()` 把 0x48 当血糖解析 —— **已移除**（aaps_dana.cpp 2026-07-28）。0x48 现正确识别为双波大剂量，教学原型未实现 → 收到返回 OK（落入 default 分支）。
- **CGM 屏显的唯一正确来源 = 自定义 BLE `g_ch_cgm` 通道**（`ble_comm.cpp`）：手机 App 经该特征直写 `g_pump_state.last_glucose_mgdl` / `glucose_trend` / `last_glucose_time_unix`。UI 仅读全局变量，不感知来源。
- ⚠️ 若今后要让 AAPS 真正把血糖推到泵屏，需：① 在 AAPS 侧新增一条非标准 APS 命令；或 ② 由手机 App 在自定义 BLE 通道转发的 CGM 值。不能"借用" Dana 0x48。

### 9.2 血糖通路（仅自定义 BLE 通道有效）
| 通路 | 入口 | 写入 |
|---|---|---|
| 自定义 BLE（`g_ch_cgm` 特征，op 直写） | `ble_comm.cpp` | `last_glucose_mgdl` + `glucose_trend` + `last_glucose_time_unix = rtc_unix_now()` |
| Dana `0x48`（AAPS） | ❌ **已撤销**（0x48 实为双波大剂量，非 CGM） | — |

- UI 仅读 `g_pump_state` 全局变量；`ui_hal_glucose_valid()` 统一做有效性/过期判定（见 §9.5）。

### 9.3 `0x70` / `0x71` 时间同步（P1）
- `0x70` GET_TIME：泵回 6 字节（年-2000, 月, 日, 时, 分, 秒）。
- `0x71` SET_TIME：泵收 6 字节 → `rtc_ymdhms_to_unix()` → `rtc_set_unix()`（持久化 + 同时设置 ESP32 硬件 RTC，见 §9.6）。
- AAPS 连上后会用 0x71 把手机时间推给泵，使泵屏/日志/基础率/方波时序全部基于真实日历时间。

### 9.4 IOB 衰减模型：Walsh 三角衰减（P0，给药安全底线）
- **错误旧逻辑**：`iob_x10000` 仅在大剂量/方波投递时**累加**，无衰减 → 首页"IOB"实为"累计输注量"，单调增长、永不回落，闭环重复给药会叠加 → **重复低血糖风险**。
- **修正（`iob_model.cpp`，Walsh 模型）**：活性胰岛素按三角时间衰减，峰值在 `DIA/2`，`t≥DIA` 归零；与 AAPS IOB 数值一致。
  - `iob_fraction(t_min)`：`t≤0→1`；`t≥DIA→0`；`t≤DIA/2 → 2·(t/DIA)²`；`DIA/2≤t≤DIA → 1-2·((DIA-t)/DIA)²`。
  - `DIA` 由 `IOB_DIA_MIN`（默认 240）推导；方波延展量仍按 1 分钟步长线性数值积分。
- 删除了原双指数衰减宏 `IOB_TAU1/IOB_TAU2` 与 `<math.h>` 依赖（Walsh 闭合解无需 exp）。

### 9.5 5 档趋势 + 离线/过期 UI 判定（P1b）
- 趋势显示码由旧 3 档(-1/0/1) 升级为 **5 档**：`-2 速降 / -1 缓降 / 0 平稳 / +1 缓升 / +2 速升`（`ui_screen.cpp` trend_str/trend_color 同步改色：速升↑↑红、缓升↗黄、缓降↘绿、速降↓↓绿、平稳→灰）。
- `ui_hal_glucose_valid()`：
  - `last_glucose_mgdl == 0` → 无效（UI 显"CGM 离线"）。
  - 时钟未设置（`!rtc_is_set()`）→ 不判过期（视为有效，避免无时钟时误判）。
  - 否则距 `last_glucose_time_unix` **> 600s（10 分钟）** → 离线（防过期血糖误显示）。
- 首页/闭环页：无效时血糖行显"CGM 离线"、趋势行显"--"（DIM 灰）；有效时显 mmol 大字 + 动态趋势。

### 9.6 ESP32 硬件 RTC（P2，取代 millis 累加）
- **旧方案缺陷**：`rtc_clock.cpp` 用 `millis()` 累加推进时间——32 位毫秒计数器约 **49.7 天回绕归零**、**deep/light sleep 停摆**、本质是"开机相对计时"而非真实日历。
- **新方案**（`rtc_clock.cpp` rewritten）：改用 ESP32 内置 **RTC 域 64 位微秒基准**，经系统 `time()`/`settimeofday()` 直接映射硬件 RTC，返回真实 Unix 秒，**无 49 天回绕**、sleep 不停摆。
- 仍无备份电池：整机完全掉电 RTC 域清零，故开机 `rtc_clock_init()` 从持久化 `g_pump_config.rtc_base_unix` 恢复（settimeofday），运行中每次 `rtc_set_unix`（本地设置 / App / Dana 0x71）持续持久化同一份基准。
- 本地设置页"日期时间"、独立手机 App 自定义 BLE 设置通道、Dana 0x71 SET_TIME **共用同一入口** `rtc_set_unix` → 写入同一份 `g_pump_config`，互不冲突。
- 模拟器端 `rtc_clock.cpp` 不编译（其 `ui_hal_sim.cpp` 用 mock 时间 + `rtc_clock.h` 内联日历互转），头接口不变。

### 9.7 已知不确定项（已大幅收敛）
- ✅ `0x48`：经 AAPS master 源码确认 = `SET_DUAL_BOLUS`，**非 CGM**。误解析已移除，无需真机核对字节布局。
- ✅ `0x70`/`0x71`：GET/SET 时间布局已与 `DanaRSPacketOptionGetPumpTime.kt` / `DanaRSPacketOptionSetPumpTime.kt` **逐字节核对一致**（6B：年-2000 / 月 / 日 / 时 / 分 / 秒）。
- ⚠️ 唯一剩余不确定：AAPS 实际同步泵时间可能用 `0x71`（本地时间）或 `0x79`（UTC+时区）。本固件实现 0x70/0x71；若 AAPS 走 0x79 同步则泵时间不更新（不影响其他功能）。真机联调用 `adb logcat | grep -i dana` 确认 AAPS 实际发送哪个 opcode，必要时补 0x79 解析即可。

---

*分析依据（权威）：`nightscout/AndroidAPS` master 分支 `pump/danars/` 模块——`services/BLEComm.kt`（GATT/握手/收发）、`encryption/BleEncryption.kt`（信封/CRC/两级加密/opcode 常量）、`comm/DanaRSPacket*.kt`（命令参数与响应布局）。参考：`bastiaanv/DanaKit` dev 分支（同一 Dana-i BLE 协议的 Swift 实现，交叉印证 GATT/加密）。*
