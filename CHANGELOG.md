# 变更记录 / CHANGELOG

> 本项目为理论验证 / 教学原型，**所有变更均不代表可用于人体**。

---

## 2026-07-27（更正）— 储药器内径重大修正：4.5mm → 9.65mm（精度/安全）

> ⚠️ 此前「2026-07-27（续）」条目基于**错误的 4.5mm 笔芯内径**假设（早期误记为 1mL 笔芯），以本条为准。

- **根因**：项目实际采用**标准 3mL 胰岛素笔芯（卡式瓶）**，经网络核实其内腔直径 ≈ **9.65mm**
  （外径 11.6mm，ISO 11608-3；bogartglass 实测 9.65mm、Ypsomed 9.7mm 一致），**并非 4.5mm**。
- **安全影响（严重）**：旧系数 `STEPS_PER_UNIT≈8048` 按 4.5mm 算；若用它驱动真实 9.65mm 笔芯，
  命令 1U 实际打出 **≈4.6U**（过量近 4.6 倍，可致严重低血糖）。现已修正。
- **修正后数值**（0.5mm/转导程 · 1/32 细分 · 9.65mm 内腔）：
  - 截面积 ≈ 73.1 mm²；每转排开 ≈ 36.57 µL = 3.657 U
  - **STEPS_PER_UNIT ≈ 1750**；**0.05U = STEPS_PER_005U ≈ 88 微步**（理论 87.5，取整 88，误差 +0.56%）
  - 每微步 ≈ 0.000571 U；300U 满笔芯 ≈ 525033 微步（< uint32 上限）
- **改动**：固件 + 模拟器 `config.h`（`SYRINGE_DIAMETER_MM` 9.65f、面积、`STEPS_PER_005U` 注释、
  `BOLUS_SPEED_HZ` 500）、`pump_state.h/.cpp`（换算推导与单步误差注释）、`docs/03`、`docs/05`。
- **仍需实测**：`SYRINGE_DIAMETER_MM` 为标称值；请用游标卡尺量实际储药罐**内腔直径**（活塞药液柱直径），
  偏差仅调 `DOSE_CALIBRATION` 或该宏即可，无需改多处。

## 2026-07-27（续）— 电机控制精度 + 大剂量分段打入

> 本项目为理论验证 / 教学原型，**所有变更均不代表可用于人体**。

### 精度核对（务必精准）
- 用真实几何推导 + 程序验证：**0.5mm/转导程 · 1/32 细分（6400 微步/转）· 4.5mm 笔芯内径
  → 0.05U = 402 微步**（理论 402.4，取整），实际剂量 0.04995U（误差 −0.10%）。
- 机械分辨率 0.000124U/微步，比 0.05U 最小网格精细约 400 倍；绝对精度由实测标定决定。

### 程序（代码）变化
- **唯一换算入口 `units_to_microsteps()`** 新增 `DOSE_CALIBRATION` 标定系数（默认 1.0），
  全系统剂量随实测整体缩放；固件/模拟器 `pump_state.c` 与 `config.h` 同步。
- **大剂量分段打入（Segmented Bolus）**：重写 `motor_controller.cpp` 的 `MOTOR_CMD_BOLUS`
  处理——改为 `motor_deliver_bolus()` 循环，每批 **0.05U（402 微步）**、段间停顿 **1s**
  （≈3U/min，对标 Wellion/Medtronic 真实泵），段间复检阻塞/报警/储药器空，支持中途取消。
- **按段记账**：储药器/今日/累计/IOB 改为「实际打入量」逐段扣减（取消时只扣已打部分），
  完成后写 `EVENT_TYPE_BOLUS` 历史并 `storage_save_config()` 持久化。
- **取消机制**：新增 `MOTOR_CMD_CANCEL_BOLUS` + `motor_cancel_bolus()`；UI 在任意页面按
  ESC 可取消正在打入的大剂量，首页底部显示「大剂量注射中… (按 ESC 取消)」；`ui_hal` 抽象层
  新增 `ui_hal_bolus_active()` / `ui_hal_cancel_bolus()`（模拟器/固件双后端实现）。
- 整个大剂量期间保持电机使能，防止段间输液回压导致活塞回灌。
- 修复：固件 HAL `ui_hal_deliver_bolus` 不再双重记账（记账移交电机分段完成时统一处理）。

### 文档
- `docs/05-firmware-design.md` 新增 §6.1 精度换算与标定、§6.2 大剂量分段打入原理与实现。

---

## 2026-07-27 — 固件完善（阶段 1 收尾）+ UI 重构 + 模拟器对齐

本次为「按 UI 上所有功能把真机固件完善」的集中收尾，模拟器与固件共用同一份
`ui_screen.cpp`，并通过模拟器 ninja 编译 + 离屏渲染 9 页全量验证。

### 程序（代码）变化
- **UI-HAL 抽象层**：新增 `ui_hal.h` + `ui_hal_sim.cpp`（模拟器后端）+ `ui_hal_fw.cpp`
  （固件后端），把界面与硬件彻底解耦，解决「模拟器 UI 与固件脱节」根因。
- **基础率周期调度器**（新增 `basal_scheduler.{h,cpp}`）：每 3 分钟计算当前速率并入队
  电机微步；支持本地档案 / 闭环下发 / TBR / 暂停四种模式；维护今日总量、累计、
  储药器余量。
- **存储落盘与历史持久化**：`history_log` 改为 Preferences(`olp_hist`) 持久化 +
  60s 节流落盘 + 开机加载；`pump_state` 新增 `consume_units()` 亚单位累加器（防
  0.05U 丢精度）+ 默认基础率档案；`storage_load_config` 加载时若方案全 0 自动套默认。
- **BLE 闭环协议补全**（`ble_comm.cpp`）：全部写命令加 **CRC-8/CCITT（poly 0x07）**
  校验；新增 TBR（百分比+速率+时长+到期）、CGM 回传特征值、控制通道
  （loop_mode / 远程排气 / 清报警）；status notify 扩为 JSON 含 glu/tr/loop/tbr。
  新增 `BLE_CHAR_CGM_UUID` / `BLE_CHAR_CONTROL_UUID`（128-bit）。
- **主接线**：`esp32_firmware.ino` 接 `ui_hal_init` / `history_log_init` /
  `basal_scheduler_init` + 创建 `basal_scheduler_task`。
- **大剂量动作**：常规 / 方波 / 双波 / 向导 / 三餐均经 `ui_hal_deliver_bolus` 入队并
  写历史 + 落盘；储药器扣减接入命令剂量。

### 界面（UI）变化
- 模拟器与固件统一为**白底 + 医疗蓝（#006bb7）**迈世通风格，全中文，横屏 320×172。
- 中文字体生成器改为 **bpp=4 打包**（LVGL 标准格式，无需 RLE），442 字：
  16px 568KB→308KB，12px 350KB→198KB（体积约 -46%），适配 ESP32 4MB Flash。
- 9 页界面（主界面 / 菜单 / 基础率 / 大剂量菜单 / 常规大剂量 / 向导 / 报警 /
  闭环 / 设置）截图更新至 `simulator/lvgl_sdl/preview/`。
- `lcd_display.cpp` 英文调试屏替换为共用中文 `ui_screen_init`；
  `keypad.cpp` 4 键改为经 `ui_screen_key()` 导航（长按 SET=原点 / ESC=关机保留）。

### 硬件配置变化
- 硬件仍为 Rev.2 设计阶段，**本次无原理图/PCB 实物变更**；仅同步文档：README 硬件
  表与 `docs/05-firmware-design.md`、`docs/06-aaps-integration.md` 已对齐 Rev.2 与
  新 BLE 协议（旧 16-bit UUID + opcode 写法已废弃）。
- 运行时状态结构新增 `tbr_expiry_ms` 字段（模拟器与固件两份 `pump_types.h` /
  `pump_state.cpp` 同步）。

### 新增：硬件接线指南（docs/12-wiring.md）
- 新增**简明接线文档**，逐外设给出「ESP32 GPIO ↔ 模块引脚」对照表、Mermaid 连接图、
  电源树、电机相位判定、INA226 分流电阻放置、按键/限位接法、上电检查清单与常见错误。
- README 新增「§5.1 硬件接线（快速参考）」小节（核心引脚表 + 链接），并把该文档加入
  推荐阅读顺序第 7 条（动手前必看）。
- ⚠️ 特别标注：INA226 的 VCC **必须 3.3V**（ESP32-C6 不耐 5V），纠正了电源树注释里
  INA226 挂在 5V 分支的隐患，避免 I²C 上拉到 5V 烧毁 MCU。

### 电机控制精度统一（0.5mm/转 · 1/32 微步 · 0.05U 最小精度）
- **统一换算入口**：新增 `units_to_microsteps()` / `microsteps_to_units()` / `quantize_units_005()`
  （`pump_state.c/.h`，模拟器与固件两份同步）。推导：1 转=0.5mm 导程、1 转=200 步×1/32=6400 微步、
  储药器内径 4.5mm → STEPS_PER_UNIT≈8048 微步/U，**0.05U = STEPS_PER_005U ≈ 402 微步**，
  每微步≈0.000124U（远细于 0.05U）。全系统（大剂量/基础率/排气）禁止各模块自行用
  `STEPS_PER_UNIT` 现算，必须经此函数，取整误差 < 0.00006U。
- **修复基础率不动电机的 BUG**：原 `MOTOR_CMD_BASAL_TICK` 直接用 `cmd->steps`（调度器只填
  `units_x100`，steps 恒为 0）→ 基础率每 3 分钟推注 0 微步、实际从不打药。改为用
  `units_to_microsteps(units_x100/100)` 换算，基础率恢复真实输注（0.5U/h → 每 tick≈201 微步）。
- **修复大剂量储药器重复扣减**：原 `motor_controller` 大剂量分支自行扣 `reservoir_units_left`
  （且算法有损），而 HAL 已通过 `pump_state_consume_units()` 扣减 → 双重扣减且精度错。现电机只负责
  运动，储药器扣减统一交给 `consume_units()`（含亚单位累加器，避免 0.05U 小数丢失）。
- **0.05U 最小精度落地**：固件 HAL `ui_hal_deliver_bolus` 在入队前对立即量/延展量调用
  `quantize_units_005()` 吸附到 0.05U 网格（UI/BLE/向导/三餐所有大剂量入口统一生效）。
- **数值验证**（host 编译 `config.h` 实测）：6400 微步/转、0.05U=402 微步、100U 满笔芯≈804813 微步
  < uint32 上限；剂量回算误差均 < 6e-5 U。模拟器 ninja 编译通过（共享 pump_state.cpp 验证）。

---

## 2026-07-25 — 项目初始化与文档
- 系统架构、电源、电机驱动、PCB、固件设计、AAPS 集成、Android APP、机械、安全、
  测试、BOM 共 11 篇文档。
- 硬件 Rev.2 规划（ESP32-C6 + 3S 11.1V + INA226 + 4 键）。
- PC 模拟器（SDL2 + LVGL 9.5.0）搭建，中文界面原型。
- Android APP（Kotlin + Compose）框架。
