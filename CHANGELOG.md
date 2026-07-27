# 变更记录 / CHANGELOG

> 本项目为理论验证 / 教学原型，**所有变更均不代表可用于人体**。

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

---

## 2026-07-25 — 项目初始化与文档
- 系统架构、电源、电机驱动、PCB、固件设计、AAPS 集成、Android APP、机械、安全、
  测试、BOM 共 11 篇文档。
- 硬件 Rev.2 规划（ESP32-C6 + 3S 11.1V + INA226 + 4 键）。
- PC 模拟器（SDL2 + LVGL 9.5.0）搭建，中文界面原型。
- Android APP（Kotlin + Compose）框架。
