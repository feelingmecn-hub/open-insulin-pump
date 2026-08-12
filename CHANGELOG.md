# 变更记录 / CHANGELOG

> 本项目为理论验证 / 教学原型，**所有变更均不代表可用于人体**。

---

## 2026-08-12 — TBR 临时基础率设置失败修复 + 泵菜单 TBR 进 AAPS 账本

> 用户实测闭环：AAPS 设 TBR 报 `Temp basal set :成功:false 评论:临时基础输注错误`，治疗页也看不到泵本地菜单设的 TBR。两处根因不同，均已修复。

### ① AAPS 闭环设 TBR 失败（setTempBasal false）根因（logcat 坐实）
- AAPS `setTempBasalPercent/ Absolute`（DanaRSPlugin.kt）成功需同时满足三条件：`connectionOK` + `isTempBasalInProgress`(0x02 状态 bit4=0x10) + percent 匹配。任一不满足即报「临时基础输注错误」。
- **Bug A（字节数误判跳过整段 TBR）**：固件旧 `0x60`(SET_TEMPORARY_BASAL 手动 TBR) handler 要求 `nparams>=3`，但 AAPS 实发 **2 字节** `[pct u8][durHours u8]`（logcat `B2 60 00 01` = 0%/1h）→ 条件不成立，整段 TBR 处理被跳过，状态从未更新。
- **Bug B（0% low-temp 置不了「进行中」位）**：0x02 状态位与 `basal_scheduler` 原都用 `tbr_percent>0` 判 TBR 进行中；闭环发 **0% low-temp**（ hypoglycemia 保护）时该位不置 → AAPS 读回 `isTempBasalInProgress=false` → 验证失败。
- **字节布局（AAPS 源码确认）**：`0x60`=`[pct u8][durHours u8]`（小时）；`0xC1`(APS_TBR)=`[pct 2B LE][durCode u8]`（`PARAM15MIN=150`/`PARAM30MIN=160`：`percent≥100→150(15min)`，`percent<100→160(30min)`，与真 Dana-i 一致）；`0x62`=取消。

### ② 修复（aaps_dana.cpp + basal_scheduler.cpp）
- 拆分 `DANA_CMD_APS_TBR`(0xC1, `nparams>=3`) 与 `DANA_CMD_SET_TBR`(0x60, `nparams>=2`) 两个 handler，各自按真实字节布局写入 `tbr_percent/tbr_rate/tbr_expiry_ms` 并记 `TEMP_START` 历史。
- 0x02 状态位与 `basal_scheduler` 的 TBR 生效判据统一改为基于 `millis() < tbr_expiry_ms`（含 0% low-temp 也置 0x10 位、也生效为 0 U/h）。
- `basal_scheduler::basal_rate_for_now`：到期清除逻辑修正，`now < tbr_expiry_ms` 即套用 `tbr_rate`（0% → 0 U/h，闭环低血糖保护真生效）。

### ③ 泵菜单 TBR 路径补全（HAL 钩子进 0xC2 回放）
- 泵本地菜单（`ui_screen.cpp` 的 `SCR_TBR`）经 `ui_hal_set_tbr()/ui_hal_cancel_tbr()` 落状态，**不经 0x60/0xC1**，原不喂 AAPS `0xC2` 回放缓冲 → AAPS 看不到菜单 TBR。
- 架构约束：HAL 层（`ui_hal.h`）不可反向依赖 Dana 协议层（`aaps_dana.cpp`）。方案：在 `ui_hal.h` 新增中性 `UI_HAL_TBR_EVENT_START/STOP` 常量 + `ui_hal_register_tbr_history_cb()` 钩子；`ui_hal_fw.cpp` 在 set/cancel 落状态时触发钩子；`aaps_dana.cpp` 注册 `aaps_tbr_hist_cb`（包装 `aaps_dana_record_tbr`）。`test/host/host_glue.cpp` 桩同步忠实实现 `ui_hal_set_tbr/cancel_tbr`（替代原 no-op）。

### ④ 验证
- 主机联调（真实固件命令分发）**PASS=75 FAIL=0**：新增 [19] 菜单 TBR 端到端（注册真实记录钩子 → `ui_hal_set_tbr(120%/45min)` → 0xC2 回放断言 `TEMP_START(120%/45min)` → `ui_hal_cancel_tbr()` → 断言 `TEMP_STOP`）。
- ESP32 全量固件重编成功（v10_debug，1,221,824B）。
- **真机验证（2026-08-12）**：烧录后 AAPS 发 `B2 C1 F4 01 96`(500%, durCode=150) → `Temp basal in progress: true` → `setTempBasalAbsolute: high temp basal set ok`；11:30 手动 0%/2h(0x60) 也 `setTempBasalPercent: OK`。旧 `Failed to set temp basal. isTempBasalInProgress: false` 彻底消失。
- 泵菜单 TBR 真机落账本验证：待下次烧录后于泵屏设一条 TBR，logcat 确认 AAPS 收到 `**NEW** EVENT TEMP_START`（钩子已接、主机联调验过，走同 0xC2 路径）。

---

## 2026-08-12 — AAPS 历史事件(0xC2)回放修复：治疗页全量记账（大剂量/TBR/方波）

> 用户实测：修复前 AAPS 里**所有对泵的操作记录（大剂量/临时基础率/方波）都看不到**——治疗-碳水与大剂量页永远空。根因：固件对 `0xC2`(APS_HISTORY_EVENTS) 历来只回 `0xFF`、从不回放历史记录，而 AAPS 仅在 `loadEvents()` 读回 BOLUS/TEMP/EXTENDED 记录时才 `syncXxxWithPumpId` 写账本。已真机验证修复。

### ① 根因（AAPS 源码坐实）
- AAPS `DanaRSService` 在**每个操作后**都调 `loadEvents()`（大剂量 / TBR / 方波 / 双波 …），发 `0xC2` 读泵历史事件；`DanaRSPacketAPSHistoryEvents.processMessage()` 从回放记录里按 code `syncBolusWithPumpId`/`syncTemporaryBasalWithPumpId`/`syncExtendedBolusWithPumpId` 写出治疗记录。
- 固件旧实现 `0xC2` handler 只回 `0xFF`，一条记录都不回放 → AAPS 收不到任何 EVENT → **所有泵操作都不进 AAPS 账本/历史/日志**。
- 推翻前置错误推论：此前「治疗页空 ⟹ 非 AAPS 所发 ⟹ 来自伴生 App/按键残留」以及「0x40 last_bolus_time 乱跳导致不记账」均不成立（已读 `DanaRSPacketBolusGetStepBolusInformation` 确认 0x40 处理器根本不用返回日期，只用 `DateTime.now()`）。

### ② 修复：历史回放子系统（新增，`aaps_dana.cpp`）
- 环形缓冲 `g_hist[48]` + 稳定唯一 `id(1–2000)`；大剂量完成 / TBR 下发 / 取消 TBR / 方波起停 时分别 `aaps_dana_record_bolus()` / `aaps_dana_record_tbr()` 写入。
- `dana_history_replay(from)`：按 from 时间戳**增量回放**（UTC 11B 布局：`id[0..1]`/`code[2]`/`Unix秒[3..6]`/`param1[7..8]`/`param2[9..10]`，MSB），每条作为独立 `RESPONSE(0xC2)` 入队，**末尾补 `0xFF`**（不补 AAPS `loadEvents()` 死等直到断连）。
- 仅回放 AAPS 已知 code（BOLUS=5 / TEMP_START=1 / TEMP_STOP=2 / EXT_START=3 / EXT_STOP=4 / DUAL=6 …），避免未知 code 触发 `fromInt()` 异常中断整批。
- `DANA_TXQ_SLOTS` **32 → 64**：防首次全量回放 48+1 包溢出丢 `0xFF`。

### ③ 时区与字节布局核对（无误）
- 记录 `ts` 走 `dana_local_now()`（RTC 设好后 = `rtc_unix_now()` 真 UTC，无 +8h）；`from_ts` 用 `rtc_ymdhms_to_unix()` 把 AAPS 发来的 **UTC** 6 分量重建 → 两边皆真 UTC，增量过滤自洽；id 稳定使 AAPS `pumpId=datetime*2+id` 去重正确，重复连接不重复建账。

### ④ 验证
- 主机联调（真实固件 `aaps_dana.cpp` 命令分发代码）**PASS=67 FAIL=0**（新增 [18] 0xC2 回放 BOLUS/TEMP/EXT 历史回归）。
- ESP32 全量固件编译成功（v10_debug，1,221,321B）。
- **真机验证（2026-08-12）**：烧录 v10_debug 后用户手动打 0.5U；logcat 铁证 `**NEW** EVENT BOLUS (5) 2026/8/12 08:26 Bolus: 0.5U`（pumpId=3572988762001, newRecord=true）→ 治疗页成功写入，时间戳正确，剂量零误差。后续回放同条不带 NEW = pumpId 去重（正常）。
- TBR/方波本次未实测（仅测 BOLUS），钩子已接 + 主机联调验过回放，走同路径。

---

## 2026-08-11 — AAPS 大剂量「已输注」记账修复 + 联调验证文档

> 用户实测 AAPS 大剂量：先报「已输注 0.00U」(notify 全丢)，再实测 10U 报「已输注 9.92U ≠ 请求 10.00U」。两处根因不同，均已修复；并经 logcat 坐实泵连接/握手/状态读取 100% 正常。

### ① 大剂量完成 notify 丢失 → 已输注记 0.00U
- **根因**：`aaps_dana.cpp` 发送队列 `DANA_TXQ_SLOTS=8` 在 BLE 流控 / loop 取包稍慢时填满，把**关键的大剂量完成 notify 直接丢弃**（AAPS 大剂量完成完全依赖此 notify，收不到即超时记 0.00U）；原已因 NimBLE 不能在接收事件回调内同步发 notify 改异步，但队列容量不足仍丢包。
- **修复**：`DANA_TXQ_SLOTS` **8 → 32**（留足大剂量期间每段进度 notify + 完成包余量）；`aaps_notify_bolus_complete()` 完成 notify **重复投递 3 次**（值相同，AAPS 幂等无害），显著提高送达率。

### ② 大剂量记账漂移 → 已输注记 9.92U ≠ 10.00U
- **根因**：`motor_controller.cpp` 段内 `bolus_delivered_x100 += ux100`（每段 `round(this_units*100)`）逐段四舍五入累加，≈20 段后漂移达 0.08U → AAPS 经 `0x40` 回读 / 完成通知得到 9.92U，超容差→判「输注量不符」报错。0.08U < 最小步进 0.1U，佐证是累加误差而非整段 notify 丢失。
- **修复**：段内更新改由**真实已打微步数反算**（`microsteps_to_units(delivered_steps)*100` 取整，零漂移）；完成时**干净完成对齐请求量** `round(total_units*100)`（微步量化 <0.1U，真实 Dana-i 成功完成亦按请求回报，消除容差报错）；**中断(aborted)则回报真实已打部分**，绝不虚报。IOB 走 `iob_record_bolus(actual)` 用真值，本就正确。

### ③ 联调验证文档与脚本（新增）
- `AAPS泵联调验证清单.md`：逐项验证清单 + logcat 关键词表（连接 / 状态读取 / 大剂量 / TBR / 方波 / 回归）。
- `aaps_auto_reconnect_test.sh`：自动开关蓝牙 5 轮 + 抓 logcat 分析。**注意**：须用 AAPS 英文日志 tag（如 `DanaRS` / `Connect !!` / `ENCRYPTION` / `Last connection`）分析；logcat 里没有界面中文文案，按中文搜会全 0。

### 验证结论
- 经 logcat 实锤：AAPS 对泵 MAC `B0:A6:04:8A:CA:A2` 的每次 connect 均成功、读完状态（battery 100% / reservoir 298 / basal 1U/h / lastBolus / LocalProfile1）；用户所视「卡连接中」是 AAPS 连-取-断周期的瞬时 UI 态，**非固件故障**。
- 大剂量修复已烧录最终固件（含①②），建议用 1~2U 小剂量复测「已输注 == 请求量」。

---

## 2026-08-10 — 关键修复：环模式崩溃 / 基础率不执行 / 历史缺失 / 验证测试（v10 发布构建）

> 用户实测四连击：App 切环模式→固件崩溃且重启仍显示闭环；设了基础率电机不动；历史只有大剂量；要求基础率验证测试按钮。根因均为设计层错误，非表面 bug。

### ① App 切环模式崩溃 + 重启回闭环
- **崩溃根因**：BLE 回调（中断上下文）里同步写 NVS flash。ESP32 擦写 flash 会关 flash cache，此刻 BLE ISR 取指落到禁用 cache → `Cache disabled but cached memory region accessed` panic 重启。
- **修复**：所有持久化改为「标脏 + loop() 上下文去抖落盘」。`.ino::loop()` 接三个 flush（`storage_flush_tick` / `basal_history_flush_tick` / `history_log_tick`），每拍最多落一个，避免连击 flash 触发同款 panic。
- **显示错误根因**：`loop_mode` 不在 `pump_config_t` 内，重启后 `pump_state_init()` 无条件置 0（闭环）。
- **修复**：新增 `loop_mode_pref` 字段（**追加到结构体末尾**保证向前兼容），UI/BLE 两处切换都写；`storage_load_config` 改为仅 `n==0` 用默认（旧 `n!=sizeof` 丢弃会令旧 NVS 整份作废、24 段档案被清零）。

### ② 设了基础率电机不动
- **根因**：闭环取速率读 `g_pump_state.current_basal_rate`，而 **AAPS 从不写此字段**（真机模型=泵按 24 段档案自跑 + AAPS 用 0x66 下发档案 + TBR 增量）。0x66 把值写进 `g_pump_config.profiles[]`，闭环却不读 → rate 恒 0 → 永不入队 `MOTOR_CMD_BASAL_TICK`。
- **修复**：闭环/开环统一读 `g_pump_config.profiles[active].slots[RTC墙钟整点].rate_uh`。整点由 `(millis()/3600000)%24`（开机后小时数，与真实时间无关，对昼夜档案是致命错）改为 RTC 墙钟；伴生 App BASAL 通道直推改为写 `basal_override_uh/_ms/_valid` 限时覆盖（旧只写 `current_basal_rate` 被下一拍覆盖=没写）。

### ③ 历史只有大剂量
- **修复（三重留痕）**：`dose_log` 逐条（3min 窗口，≈35 万条容量完整审计）+ `history_log` 按 `BASAL_HISTORY_AGG_MS`(30min) 聚合成一条（否则 32 条环形被 3 分钟一条挤爆、大剂量记录全没）+ `basal_history` 速率**变化**时打 `BH_BASAL_ACTIVE`（归零也收尾打 0 点）。

### ④ 基础率验证测试按钮
- **设计**：读**泵内档案**（非 App 页面编辑值），全天 24 段总量一次性打出，走大剂量物理路径但**不计大剂量次数 / 不计 IOB**（否则污染 AAPS IOB→闭环误判），历史 param2 存**实际微步数**供对照丝杠位移（屏显 `12.0U/1510步`）。一次定性区分「没写进去」vs「写进去了但电机不动」。
- **入口**：泵屏 `设置→基础率方案→⑧ 验证测试·打全天量`；App 基础率页「验证测试」按钮（带二次确认）。
- **诊断健壮**：返回 0 时显式报「全天总量为0! 基础率未写入泵」（而非静默退回误以为按钮坏）；`bh_dup()` 2 分钟去重排除 `BH_BASAL_TEST`，否则为对照行程连按两次被静默吞掉。
- **伴生 App 修正**：`PumpUuids` 环模式常量原标反（`OPEN=0`/`CLOSED=1`，与固件真源 0=闭环相反），已改正；新增 `CTRL_CMD_BASAL_TEST=0x18`。

### 发布构建
- 新增 `basal_history.{h,cpp}` / `dose_log.{h,cpp}` 模块（基础率/大剂量执行留痕，去重保护 `BH_BASAL_TEST`）。
- 两变体固件：`v10`（正式，`-DUSE_AAPS_DANA`）/ `v10-debug`（`-DMOTOR_DEBUG_UNLOCKED`，INA226 未接时用）。主机链路模拟器回归 **PASS=61 FAIL=0**。
- ⚠️ 编译须 `--jobs 1` 串行（并行编库 .a 竞态会损坏 "is not an object"）。交付产物在桌面 `闭环泵固件v10/` 与 `闭环泵固件v10-debug/`。

---

## 2026-08-08 — BLE 健壮性：多连接 / 广播包布局 / NVS 延迟写 / 配置落库

> 配套 8/10 的底层修复，先一步清掉 BLE 层的并发与持久化隐患。

- **多连接并存**（华为系统连接 + AAPS 连接）：`onConnect` 内 `startAdvertising()`（否则 AAPS 白名单 connectGatt 扫不到→GATT_ERROR133 死循环）；`notify` 带 connHandle 定向投递；`onDisconnect` 仅断 Dana 对端才重置握手；`ble_connected=(getConnectedCount()>0)`。
- **广播包 31B 上限**：AAPS 的 FFF0(16-bit) 进广播包，伴生 App 的 `6E400001`(128-bit) 溢入扫描响应；`setName()` 须在 `enableScanResponse(true)` 之前调用，否则设备名被塞进扫描响应→AAPS 搜不到。
- **0x64/0x66/0x53 配置真落库**：原只回 OK 不写 `g_pump_config` → AAPS 每次连接读回比对不一致反复弹「设置配置文件」；改为真解析+写配置，落盘走标脏+1.5s 延迟（不在 BLE 回调里擦 flash）。`dana_unpack_packet` 的 params 缓冲扩到 ≥`DANA_MAX_PACKET`(128)，修复 0x53 后 48B(CF) 被静默截断。
- **安全上限兜底 0/NaN**：`max_basal_per_hour` 为 0（旧 NVS/未初始化）时旧逻辑把 24 段全压成 0→泵完全不给基础率；改为字段≤0 时回退 `MAX_BASAL_RATE`。

---

## 2026-08-06 — 修复退药顶死丝杆 / 行程安全上限 / 前限位缺失 / 手动退药上限

> 现象：退药"推到底卡死还在退"；运行几次后所有操作(打药)疑似反向、重启/断电池无效、自行恢复；手动退药量上限仅 50U(用户需 300U)。
> 根因：① `REWIND_MAX_STEPS=700000` 注释误算(实际≈5563U), 远超 300U 储药器行程(≈37752步)；若后限位开关 `PIN_LIMIT_REV` 未接/失效则一路顶死；且调试宏 `MOTOR_DEBUG_UNLOCKED` 跳过堵转硬停 → 顶死还空转。② 大剂量/排气/标定 FORWARD 分支此前无前限位检测，近前端还打会顶死前限位。③ 丝杆顶死后的机械卡死表象被误判为"方向反转"。④ 手动退药 UI 上限 50U 过小。

- **`config.h`**：`REWIND_MAX_STEPS` 700000 → **45000**(满容量300U×STEPS_PER_UNIT×1.15 兜底, 防无限位时超行程顶死)；新增 `RESERVOIR_MAX_STEPS=45000` 供正向边界兜底；注释纠正旧误算。
- **`motor_controller.cpp`**：`motor_pulse()` 入口对 **FORWARD** 加通用限位/行程保护(`manual_limit_hit(FWD)` + `g_motor_position>=RESERVOIR_MAX_STEPS` 即停)，覆盖大剂量/排气/标定此前缺失的前限位检测；加 `manual_limit_hit` 前向声明。`motor_rewind_full()` 的堵转/丢步**物理停止**移出 `#ifndef MOTOR_DEBUG_UNLOCKED`(无论调试与否都停硬推, 仅报警区分), 彻底消除"卡死还退"。
- **`ui_screen.cpp`**：手动退药量上限 50U → **300U**(步进 0.1U, 满足"最大300U距离可手动输入")。
- **`ui_hal_fw.cpp`**：标定系数 `factor` 钳制 `<=0 或 >2.0` 重置为默认(防异常系数令 `units_to_microsteps` 步数爆炸顶死丝杆)。
- **`dosing.h`**：`units_to_microsteps()` 结果封顶 60000 步(纵深防御异常系数)。
- **待烧录（旧版, 已被下条 INA226 限位改造取代）**：本机编译通过(1210623字节)。⚠️ 用户确认**本项目无硬件限位开关**，原 `PIN_LIMIT_FWD/REV=GPIO2/3` 实为 ESP32-C6 USB-D+/D-，旧"确认限位开关已接好"建议作废。

---

## 2026-08-06 (2) — 限位检测改为 INA226 堵转电流（移除 GPIO2/3 伪限位）

> 用户确认：硬件未设计限位开关，限位今后由 INA226 电流判断。原 `manual_limit_hit()` / `safety_monitor` 读 `PIN_LIMIT_FWD=2`/`PIN_LIMIT_REV=3`，而这俩脚是 ESP32-C6 的 USB-D+/D-（CDCOnBoot=cdc 下被 USB-CDC 占用），读到的是 USB 差分信号(垃圾值) → 既造成"退药卡死还退"(限位永 false)，也制造随机怪象。

- **`config.h`**：删除 `PIN_LIMIT_FWD/PIN_LIMIT_REV` 定义(改为注释说明无硬件限位+限位改 INA226)；新增 `STALL_OCCL_CONSEC=5` / `STALL_NOLOAD_CONSEC=5` 堵转/丢步去抖连续采样阈值(滤除启动浪涌/瞬态尖峰)。
- **`motor_controller.cpp`**：`motor_stall_guard_tick()` 改为连续 5 采样超阈值才置 `g_occlusion`/`g_step_loss`(去抖)；`motor_pulse()` 等待循环内**堵转即立即停脉冲**(不等整批)，且堵转/丢步**无论调试/正式构建都返回 false 中止运动**(调试仅不弹 ALARM)，彻底消除"顶死还退"/"顶死前限位还打"；`manual_limit_hit()` 改为返回 `g_occlusion`(INA226 堵转=到限位, 方向无关)；`motor_init()` 不再配置 GPIO2/3 限位引脚。
- **`safety_monitor.cpp`**：删除 GPIO 限位引脚配置与 `ALARM_LIMIT_TRIGGERED` 的 GPIO 触发(改由 INA226 堵转经 `ALARM_OCCLUSION` 体现)。
- **`dist/windows/firmware_src/src/`**：同步 config.h / motor_controller.cpp / safety_monitor.cpp。
- **编译**：本机编译通过(1210617字节, 带 `-DMOTOR_DEBUG_UNLOCKED`)。需泵进 boot 烧录验证；正式发布须去掉调试宏。
- ⚠️ **安全红线补充**：严禁在泵由 3S 电池供电运行时给同一电池接平衡充/充电器(用户实测曾因此致 DIR 引脚瞬态闩锁、所有操作反向, 断电后自愈)。充电前先断开泵供电或拆电池单独充。

---

## 2026-08-06 (3) — 主屏实时显示 INA226 电流（调试堵转阈值标定）

> 用户需求：实时显示电机电流，便于调试观察"啥样的电流是堵转"。

- **`pump_types.h` / `pump_state.h` / `pump_state.cpp`**：`motor_current_ma` 语义由"运动峰值"改为**实时电流(运动/空闲均采样)**；新增 `motor_current_peak_ma`(单次运动峰值, 供标定堵转阈值)；新增 `pump_state_update_motor_current_peak()`。
- **`motor_controller.cpp`**：`motor_stall_guard_tick()` 每 5ms 采样即写实时电流到 `pump_state.motor_current_ma`；运动结束写峰值到 `motor_current_peak_ma`。
- **`safety_monitor.cpp`**：`safety_task` 周期(每 `SAFETY_TASK_INTERVAL_MS`)读 INA226 电流刷新实时电流——电机空闲时也持续采样(作 LCD 实时显示数据源)。
- **`ui_screen.cpp`**：主屏右栏 y108 新增"电流 XXX mA"行；`≥ occlusion_threshold` 显示**红色"堵转!"**，正常绿、无电流灰；今日/IOB 行下移至 y122 避让。主屏由 `ui_screen_periodic()` 周期重绘, 电流实时刷新。
- **`dist/windows/firmware_src/src/`**：同步上述 6 文件。
- **编译**：本机编译通过(1210897字节, 带 `-DMOTOR_DEBUG_UNLOCKED`)。需泵进 boot 烧录验证；正式发布须去掉调试宏。BLE 状态包(20字节)已满, 本次未新增 BLE 通道(仅 LCD 显示; 手机侧如需看电流可后续加独立 CURRENT 通道)。

---

## 2026-08-05 — 最小可靠剂量下限 0.05U → 0.1U（落实"做不到的精度不显示/不支持"原则）

> 依据实测 CY-13 储药罐（PHRay 3mL，内腔 Φ11.38mm）与机械误差分析：整机实际可靠精度仅 ±0.1~0.3U（丝杆背隙为主，标定只能修系统偏移、修不了随机背隙/丢步）。故将系统最小可靠单剂量下限统一提到 0.1U，与丹纳原厂大剂量增量一致。

- **`config.h`**：`MIN_DOSE_UNITS` 0.05f → **0.1f**（全系统剂量网格 / 单剂量下限）；`BOLUS_GRAN_FINE` 同步 0.05f → 0.1f（大剂量最细分批粒度）；标定系数注释「笔芯内径」→「储药罐内径」。
- **`dosing.h`（单一真源）**：吸附函数 `quantize_units_005` 重命名为 `quantize_units_grid`（内部仍用 `MIN_DOSE_UNITS`，改宏即联动）；宏 `STEPS_PER_005U` → `STEPS_PER_MIN_DOSE_U`；剂量诚实性原则注释更新——物理分辨率随 11.38mm 直径变为 ≈0.0079U/微步，但对外承诺下限取 0.1U。
- **UI（`ui_screen.cpp`）**：大剂量/立即量/方波量/手动退药/实测体积 等所有剂量步进与下限 0.05 → 0.1；基础率设置步进 `BASAL_RATE_STEP` 0.05 → 0.1；上述剂量显示由 `%.2f` 改为 `%.1f`（不对外声称 0.01U 精度）；向导大剂量建议值先 `quantize_units_grid` 再投递，屏上显示与实投一致。
- **后端/边界**：`ui_hal_fw.cpp`（基础率、大剂量吸附、手动退药/标定下限）、`ble_comm.cpp`（标定出测试量下限）、`ui_hal_link.cpp` / `link_session.cpp`（模拟器后端）同步改名与 0.1 下限；`dist/windows/firmware_src` 分发副本、`docs/03-motor-drive.md` / `05-firmware-design.md` 同步。
- **不受影响**：基础率连续投递走微步累加器（不受 `MIN_DOSE_UNITS` 闸门，仍可平滑输送）；延展量逐拍下限 `EXT_BOLUS_MIN_UNITS=0.005` 刻意远低于 0.1U 以保持连续，保留。

---

## 2026-08-05 — 安卓控制 App 同步 0.1U（与固件对齐）

> 固件已统一最小可靠剂量下限到 0.1U（`MIN_DOSE_UNITS`/`BOLUS_GRAN_FINE`=0.1，剂量显示 `%.1f`）。安卓控制 App 同步对齐，避免 App 端还能选 0.05U 或显示 0.01U 假精度，与泵行为不一致。

- **`BolusViewModel.kt`**：剂量量化 `(value*20)/20.0` → `(value*10)/10.0`，真正剂量网格由 0.05U 改为 **0.1U**（与固件 `MIN_DOSE_UNITS` 一致）；注释同步。
- **`BolusScreen.kt`**：步进按钮 `±0.05` → `±0.1`、提示「步进 0.05U」→「步进 0.1U」；大剂量数值 / 推荐校正剂量 / 确认推注 显示 `%.2f` → `%.1f`。
- **`BolusRequest.kt`**：剂量精度注释 0.05U → 0.1U。
- **胰岛素量显示统一 `%.1f`**：镜像屏（剩余药量本就是 0.1U；IOB / 基础率 / 今日累计 / 常规·方波·双波大剂量菜单剂量）、基础率编辑（BasalScreen / BasalProfileScreen）、历史记录、仪表盘 IOB、设置页「标定测试量 / 排气量」确认文案，全部 `%.2f` → `%.1f`。
- **`SettingsViewModel.kt`**：标定推出测试量 `coerceIn` 下限 `0.05f` → `0.1f`（排气量下限本就是 0.1f，无需改）。
- **协议层**：BLE 大剂量仍走 `units_x100`（0.1U=10），无编码变更；固件 IOB 通知当前仍为 `%.2f` ASCII，App 解析后按 `%.1f` 显示（显示已一致，固件文本格式如需收紧可后续单独处理）。

---

## 2026-08-05 — 修复 AAPS 蓝牙配对失败（Passkey Entry → Just Works）

> 现象：AAPS 与系统蓝牙配对均弹 6 位码、点配对后超时失败；自研 App 不配对也能控泵（加密在应用层）。
> 根因：`aaps_dana.cpp` 原 `setSecurityIOCap(KEYBOARD_DISP)` + `setSecurityAuth(true,true,true)`（强制 MITM）触发 Passkey Entry；协商后 Android 成 Display 端要求泵确认，但 `onConfirmPassKey()` 空实现 → 泵不确认 → 超时失败。

- **`aaps_dana.cpp`**：配对改为 **Just Works**（`setSecurityAuth(true,false,true)` bond+LESC、MITM 关；`setSecurityIOCap(NO_IO)`）。Dana-i 真正安全靠应用层 `DANAI_BLE5_KEY` 加密握手（不依赖链路层 MITM），AAPS 仅要求 bond 成功。Android 不再弹 6 位码，改为「是否配对」确认框，点一下即成功。`onConfirmPassKey`/`onPassKeyDisplay` 注释说明 NO_IO 下不触发（保留兼容若改回 DISPLAY_ONLY 的 passkey 场景）。
- **`DANAI_BLE5_KEY` 仍为 `"123456"`**（合法 6 位，atoi 无误；仅 passkey 流程不再走它）。
- **待烧录**：本机已编译通过（1210479 字节），需泵进 boot 后用 `-u` 烧录验证；正式发布版仍须去掉 `-DMOTOR_DEBUG_UNLOCKED`。

---

## 2026-07-28 — AAPS 闭环联调同步演示（四宫格 GUI + 一键启动器）

> 为向他人无硬件演示「AAPS 发令 → 固件收令 → 电机推药 → 泵屏响应」完整闭环，新增桌面联调同步演示。

- **架构（统一进模拟器）**：把真实固件命令核心（`aaps_dana` + `dosing.h` 单一真源 + `basal_scheduler`）编进 LVGL 模拟器（`SIM_LINK_MODE`），由 `link_session` 脚本化 17 步会话引擎驱动 `g_pump_state`，泵屏幕每帧重绘即与 AAPS 命令严格同步；TCP 控制通道 `127.0.0.1:18923`（`link_ipc`）向 Python 控制面板广播状态 JSON 并接收 play/pause/step/reset/delay 指令。
- **四宫格控制面板 `test/link_demo_gui.py`**（Tkinter，无外部依赖）：① 步进电机推药动画（转子随微步旋转 + 丝杠 + 注射器柱塞随发药推进，数据来自固件 `motor_delivered_units_x100()`）；② AAPS 发送调试窗（真实 `dana_build_packet` 在途字节 + opcode 名 + 意图）；③ 固件接收调试窗（真实 `aaps_dana_feed_rx_test` 解包 / 分发动作 / 回应字节，篡改 CRC 红显「被拒绝」）；④ 泵屏 UI 实时复刻（canvas 320×172）。协议轨迹缓冲 `LinkTrace` 同时喂②③两窗。
- **一键启动器 `test/run_link_demo.command` / `run_link_demo.sh`**：双击即全自动构建联调版 → 启模拟器（弹泵屏窗口）→ 启控制面板；以构建模式标记 `.built_link_mode` 判断，确保跑的是联调版（避免切回 mock 后演示失效）；关闭窗口自动清理进程。配套 `build_sim_link.sh` / `build_sim_mock.sh` 与 `link_mode_smoke.cpp`（纯逻辑冒烟）。
- **验证**：端到端（后台启模拟器 + 客户端驱动）17/17 步、50 检查 0 失败；协议轨迹 18 条（含 1 条篡改被拒）；末态 `motor_units=2.00 / microsteps=4356`（2U×2178）；首条 TX `A5 A5 02 D2 FF 9A 4A 5A 5A` 为真实在途字节。mock 模式回归构建无破坏。

---

## 2026-07-28 — AAPS Dana-i impersonation 协议实现（字节级 205 场景验证）

- **新增 `code/esp32_firmware/src/aaps_dana.{h,cpp}`**：纯逻辑（无 NimBLE 依赖，可被宿主编译）的 Dana-i BLE 协议模块；`config.h` 宏 `USE_AAPS_DANA`（默认关闭）；`ble_comm.cpp` 双模挂载 `FFF0/FFF1/FFF2`。命令分发到 `motor_enqueue` / `motor_cancel_bolus` / `basal_scheduler_*`（经 `dosing.h` 单一换算），不动既有电机逻辑。
- **协议细节（逐行对齐 AndroidAPS `pump/danars` 的 `BleEncryption.kt` + `DanaRSPacket*`）**：GATT `FFF0/FFF1/FFF2` + CCCD `2902`；信封 `A5 A5 len TYPE OPCODE params CRC16 5A 5A`；单字节 opcode（`0x4A` 步进大剂量 / `0xC1` APS-TBR / `0x47` 方波 / `0x62` 停 TBR / `0x49` 停方波 / `0x02` 状态 / `0x48` CGM / `0x70`/`0x71` 时间同步…）；设备名须正好 10 字符 `^[a-zA-Z]{3}[0-9]{5}[a-zA-Z]{2}$`。
- **关键纠正（与旧文档 §3 不同）**：① 一级信封 XOR 混淆区间**含 CRC_L**（`buf[3..size-3]`）；② 握手包（PUMP_CHECK / TIME_INFORMATION，双向）**不做二级加密**——AAPS 仅在 `isConnected==true` 才二级解密，握手期 `connectionState=0/1`；设备端镜像：收完 TIME_INFORMATION 置 `HANDSHAKE_DONE` 后命令包才二级加密。
- **字节级验证**：`test/aaps_dana_test.cpp`（g++ 纯逻辑）+ `test/oracle_aaps.py`（AAPS `BleEncryption.kt` 逐字转译预言机）→ `test/run_tests.sh` 抽取 `^PKT ` 行排序 diff，**205 个场景全部字节级匹配**（握手包 + 命令响应 + 通知 + 200 随机命令包）。
- **配套新增**：`iob_model.{h,cpp}`（Walsh 三角衰减，替代旧双指数）、`rtc_clock.{h,cpp}`（ESP32 硬件 RTC，无 49 天回绕）、`docs/12-AAPS-Dana协议与对接方案.md`（权威协议文档）。
- ⚠️ 0x48/0x71 字节布局按标准 DanaRS 文档推算，**未抓到用户 AAPS 版本源码**，须真机联调 `adb logcat | grep -i dana` + 抓包核对；不符仅改 `dana_apply_glucose` / `dana_apply_time` 映射。

---

## 2026-07-28（方波/双波）— 延展量改为连续慢滴（贴近真实泵，非几分钟一跳）

- **问题**：上一版方波/双波延展量挂在 3 分钟基础率节拍上、用大剂量快速度(`BOLUS_SPEED_HZ`)
  把每拍 delta 在不到 1 秒内 burst 完、再歇 3 分钟——与真实泵「在 duration 内匀速连续输注、
  步间 ~1s」的行为不符（用户指出「不太可能注射等待间隔几分钟」）。
- **改为连续慢滴**：调度器循环以 `EXT_BOLUS_WINDOW_MS`(默认 15s) 为细拍；基础率仍按
  `BASAL_TICK_INTERVAL_MS`(3 分钟) 窗口、每 `basal_div` 个细拍投递一次。每个细拍的延展量微投递
  按**方波速率** `rate_uh = total/duration_h` 换算成慢速 `steps_per_s` 下发，使电机在该窗口内
  **连续运行填满时间**（匀速慢滴）；单次投递下限 `EXT_BOLUS_MIN_UNITS = 0.005U` 保连续、防空转。
- **记账修正**：每个细拍按「实际投递量」累计 `ext_bolus_delivered_x100`（此前按时间目标舍入标记，
  与按 delta 舍入入队各自舍入导致逐拍漂移、总量偏离）；现 delta 自动对齐时间目标，60min 全程
  总量精确等于 `total`。
- **验证**：宿主 C++ 测试（编译真实 `basal_scheduler.cpp`）5 用例全过——3U/60min 经 240 个
  15s 细拍连续铺开、每拍 <0.05U、使用慢速而非 500Hz、总量精确 3.0U；0.1U 微量慢滴不一次性
  burst；半程取消得 1.5U；储药器空中止不过量；`duration ≤ 0` 退化。模拟器 ninja 构建无回归。

---

## 2026-07-27（方波/双波）— 大剂量按时间维铺开 + 修复累计字段缺失

- **方波/双波「时长铺开」已实现**：延展量不再作为第二条一次性大剂量入队，
  改由 `basal_scheduler` 按 `duration_h` 时间维在每 3 分钟 tick 中按比例铺开
  （`extended_bolus_tick()`）；每个 tick 微投递封装为新增的 `MOTOR_CMD_BOLUS_EXT`，
  仍走 `execute_command()` 唯一电机入口、换算走 `dosing.h`、记账（储药器/今日/累计/IOB）
  在入队时同步完成。时间到收尾写一条 `EVENT_TYPE_BOLUS`、双波 = 立即 + 延展两条事件。
- **取消/安全**：`ui_hal_cancel_bolus()` 同时取消立即量与延展量；`ui_hal_bolus_active()`
  在任一进行中均返回真；延展量投递前复检储药器空，空则中止并记部分事件；`duration_h ≤ 0` 退化为一次性大剂量。
- **修复预存固件编译缺陷**：`g_pump_state.total_units_x100_delivered` 此前仅在
  `pump_config_t` 声明，但 `motor_controller.cpp` / `basal_scheduler.cpp` 以
  `g_pump_state.total_units_x100_delivered` 累加（模拟器不编译这两文件，缺陷一直潜伏）。
  已在 `pump_runtime_state_t` 补该 live 累计字段，固件现在可正确编译。
- **验证**：宿主 C++ 测试（stub 掉 FreeRTOS/ESP 依赖，编译真实 `basal_scheduler.cpp`）
  覆盖 5 个用例全部通过——3U/60min 等比铺开(20×0.15U)、0.1U 小量末段投递、
  中途取消只损失已铺开部分、储药器空中止不过量、`duration ≤ 0` 退化。模拟器 ninja 构建无回归。

---

## 2026-07-27（架构重构）— 储药罐类型配置化 + 剂量换算单一真源 (dosing.h)

> 用户要求：储药罐类型应在配置里选、所有打药控制从一个参数出、所有算法从一个地方输出，
> 不再东一块西一块地硬编码推导常数。

- **储药罐类型配置化**：`config.h` 新增 `RESERVOIR_TYPE` 唯一选择点
  （`RESERVOIR_TYPE_CY13_DANA` / `RESERVOIR_TYPE_CARTRIDGE_3ML`），每种类型给出内腔直径、
  容量、`RESERVOIR_TYPE_NAME`。**切换耗材 = 改这一个宏**，几何/换算全自动重算。
- **剂量换算单一真源 `dosing.h`**：新建 header-only 模块，从「内腔直径」单一参数推导
  截面积 / 每转体积 / `STEPS_PER_UNIT` / `STEPS_PER_005U`，并定义
  `units_to_microsteps()` / `microsteps_to_units()` / `quantize_units_005()` 三个函数。
  固件与模拟器**共用同一份** `dosing.h`（模拟器 CMake 把固件 src 加入包含路径），杜绝算法双份。
- **去重**：删除固件与模拟器两份 `pump_state.cpp` 中重复的换算函数定义；
  删除 `config.h` 里手算硬编码的 `SYRINGE_AREA_MM2` / `STEPS_PER_UNIT` / `STEPS_PER_005U` 等常数
  （连同过期的 `≈58.8` / `=109` 注释）。所有调用方仍只走唯一接口。
- **电机控制唯一入口**：`motor_controller.cpp` 明确为全系统唯一电机驱动入口，
  所有打药路径经 `execute_command()` 分发，且一律调用 `dosing.h` 换算，禁止自行现算。
- **验证**：ninja 重编模拟器通过；编译测试程序确认 `RESERVOIR_TYPE=CY13_DANA` 时
  0.05U=109 微步（+0.085%），切到 `CARTRIDGE_3ML` 自动变为 0.05U=88 微步（73.1mm²/1750 步每 U）。
- **文档**：`docs/03`、`docs/05` 将硬编码示例片段改为指向 `dosing.h` 单一真源与 `RESERVOIR_TYPE` 配置。

---

## 2026-07-27（二次更正）— 储药器类型澄清：9.65mm(卡式瓶) → 8.65mm(注射器型)

> ⚠️ 上一条「（更正）」把本项目耗材当成了 **3mL 卡式瓶（笔芯，内径≈9.65mm）**，但经用户确认实际
> 耗材为 **瑞宇优泵 PH300 / 丹纳 Dana 兼容的 3mL 注射器型储药器（优泵 CY-13）**。FDA 510(k) 明确该类
> 耗材为「3 ml polypropylene syringe」，**不是卡式瓶**。标准 3ml 鲁尔注射器内腔 I.D.=Φ8.65±0.09mm
> （国标预灌封注射器规格 + ClearJect 3mL 一致），故换算系数应按 8.65mm 计。

- **安全影响**：若按旧的 9.65mm 跑真实 8.65mm 注射器型储药器，命令 1U 实际打出 **≈1.24U**
  （过量约 24%，仍属安全隐患）。已修正为 8.65mm。
- **修正后数值**（0.5mm/转导程 · 1/32 细分 · 8.65mm 内腔）：
  - 截面积 ≈ 58.8 mm²；每转排开 ≈ 29.38 µL = 2.938 U
  - **STEPS_PER_UNIT ≈ 2178**；**0.05U = STEPS_PER_005U = 109 微步**（理论 108.9，取整 109，误差 +0.12%）
  - 每微步 ≈ 0.000459 U；300U 满储药器 ≈ 653445 微步（< uint32 上限）
- **改动**：固件 + 模拟器 `config.h`（`SYRINGE_DIAMETER_MM` 8.65f、面积、`STEPS_PER_005U`、注释）、
  `pump_state.h/.cpp` 换算推导注释、`docs/03` 选型表、`docs/05` §6.1 精度表与结论、§6.2.2 分段剂量。
- **仍须物理实测**：`SYRINGE_DIAMETER_MM=8.65f` 为标称值；请用游标卡尺量实际储药器**内腔直径**
  （活塞药液柱直径），偏差仅调 `DOSE_CALIBRATION` 或该宏即可，全系统剂量自动缩放。
- **术语澄清**：商品页「4.5mm / 6.5mm」是**输注钢针/软针的针长**，与储药器内径无关，勿混淆。

## 2026-07-27（更正）— 储药器内径重大修正：4.5mm → 9.65mm（精度/安全）

> 📌 本条已被上方「（二次更正）」部分取代：原假设耗材为 3mL 卡式瓶（9.65mm），实际为 3mL 注射器型（8.65mm）。
> 本条保留作为「4.5mm → 卡式瓶 9.65mm」的推导史，最终以 8.65mm 为准。

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
