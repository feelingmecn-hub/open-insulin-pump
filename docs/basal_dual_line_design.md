# 基础率双线隔离架构设计（AAPS 闭环安全）

> 实验项目，禁止用于人体。本文只解决「泵本体基础率」与「AAPS 下发基础率」共用同一 NVS 真源导致的互相覆盖 / 残留过量风险。

---

## 1. 问题根因

当前固件：

- AAPS 通过 Dana 协议 `0x66 SET_PROFILE_BASAL` **直接改写** `g_pump_config.profiles[active].slots[hour].rate_uh` —— 这是泵本体的 24 段 NVS 档案（线A）。
- 泵菜单、伴生 App NUS 通道 `0x17 SET_PROFILE_SLOT` 也写同一个 `g_pump_config.profiles`（线A）。
- `basal_scheduler.cpp::basal_rate_for_now()` 无论闭环/开环都读线A 作为基础率真源，AAPS 仅用 TBR 做增量。

**后果**：AAPS 写进去的 profile 永久污染泵本体档案。AAPS 断连后，泵仍在跑 AAPS 设定的那个 profile（可能早已过期或错误）；且本体干净基线被覆盖，无法判断「当前到底跑的是谁的」。这正是「残留基础率 → AAPS TBR 百分比算偏 → 过量」的根因。

---

## 2. 目标

把基础率拆成**两条物理隔离的线**，机制性保证「本体 profile 永不被 AAPS 污染」：

- **线A（本体基线）**：仅泵菜单 / 伴生 App 管理，存 NVS `olp_pump`（现有 `g_pump_config.profiles`）。离线 / 独立模式走这条。
- **线B（AAPS 闭环）**：AAPS 下发的 profile + TBR，存**独立** NVS 命名空间 `olp_aaps`（`g_aaps_basal[24]`），绝不碰线A。
- 运行时由 `basal_source`（LOCAL / AAPS）决定基准线；**AAPS 失联超时自动回退线A** 本地安全基线。

---

## 3. 两条线的入口（天然隔离）

| | 线A 本体 | 线B AAPS |
|---|---|---|
| 存储 | `g_pump_config.profiles` (NVS `olp_pump`) | `g_aaps_basal[24]` (NVS `olp_aaps`) |
| 写入入口 | 泵菜单 UI、伴生App NUS `0x17`、伴生App BASAL 限时覆盖 `basal_override` | Dana 通道 `0x66` / `0x65`(读) / `0x67`(读) / `0xC1` / `0x60` |
| 协议通道 | NUS（自定义 `6E400001-...`） | `FFF0/FFF1/FFF2`（AAPS Dana-i 服务） |
| 有效期 | 持久 | 持久，但受 `basal_source` 时间窗约束 |

> 通道本身就是天然分流：AAPS 来的全走线B，伴生/菜单来的走线A，互不串门。

---

## 4. 新增数据结构

`pump_types.h` / `pump_state.h`：

```c
// 线B：AAPS 下发的 24 段基础率缓存（独立于线A 的 g_pump_config.profiles）
extern float g_aaps_basal[BASAL_SLOTS_PER_DAY];   // U/h，索引=小时 0..23
extern uint8_t g_aaps_profile_valid;              // 1=线B 已写入有效 profile

typedef enum { BASAL_SRC_LOCAL = 0, BASAL_SRC_AAPS = 1 } basal_source_t;
extern basal_source_t g_basal_source;             // 当前生效基准线
extern uint32_t g_aaps_last_cmd_ms;               // 最近一次收到 AAPS 命令的 millis() 时间戳

#define AAPS_SOURCE_TIMEOUT_MS (35UL * 60UL * 1000UL)  // 35min 无 AAPS 命令 → 回退线A
```

`storage.cpp` 增加独立命名空间：

```c
// 线B 持久化，与 olp_pump 完全隔离
void aaps_basal_load(void);   // g_prefs.begin("olp_aaps") → getBytes("basal", ...)
void aaps_basal_save(void);   // putBytes("basal", g_aaps_basal, ...)
```

---

## 5. 协议映射（改 `aaps_dana.cpp`）

| opcode | 现状 | 改造后 |
|---|---|---|
| `0x66 SET_PROFILE_BASAL` | 写 `g_pump_config.profiles` | **写 `g_aaps_basal`**；置 `g_aaps_profile_valid=1`、`g_basal_source=AAPS`、`g_aaps_last_cmd_ms=millis()`；`aaps_basal_save()` |
| `0x65 GET_PROFILE_BASAL` | 读 `g_pump_config.profiles` | **读 `g_aaps_basal`**（AAPS 读回自己写的，自洽） |
| `0x67 GET_BASAL_RATE` | 读 `g_pump_config.profiles` | **读 `g_aaps_basal`**（读写口径一致） |
| `0xC1 APS_TBR` | 刷 TBR + 置 source | 同上 + 刷 `g_aaps_last_cmd_ms`、置 `g_basal_source=AAPS` |
| `0x60 SET_TBR` | 刷 TBR | 同上 + 刷 `g_aaps_last_cmd_ms` |
| 任意 AAPS 命令 | — | 统一刷 `g_aaps_last_cmd_ms`（保活 `0xFF` 也算） |

> 线A `g_pump_config.profiles` 对 AAPS **完全不可见** —— AAPS 再也读不到、改不到泵本体的基线。

**TBR 的 `ref` 不用改**：`0xC1`/`0x60` 里 `ref = g_pump_state.current_basal_rate`（上一 tick 的活跃线速率），`tbr_rate = pct/100 * ref`。AAPS 接管时 `current_basal_rate` 已是线B 速率 → TBR 百分比天然基于线B 算；LOCAL 时基于线A。这条天然跟线，无需改动。

---

## 6. 调度逻辑（改 `basal_scheduler.cpp`）

`basal_rate_for_now()` 选型顺序：

```
1) 暂停(loop_mode==2) → 0
2) 选基准线 base：
     if (g_basal_source==AAPS && g_aaps_profile_valid
         && (millis()-g_aaps_last_cmd_ms) < AAPS_SOURCE_TIMEOUT_MS)
         base = g_aaps_basal[hour];          // 线B
     else
         base = g_pump_config.profiles[active].slots[hour].rate_uh;  // 线A
3) 伴生App BASAL 限时覆盖 basal_override（仍叠加，不变）
4) TBR 优先（有效期内，基于 current_basal_rate，不变）
5) 钳制 [0, MAX_BASAL_RATE]
```

`basal_scheduler_daily_total()` / `basal_scheduler_run_daily_test()` 的「验证测试」仍读**线A** `g_pump_config.profiles` —— 它本就用来验证「本体档案是否落库」，与线B 无关，保持原样。

---

## 7. 失联回退

- `g_basal_source` 实际由时间窗动态决定：`(millis() - g_aaps_last_cmd_ms) < 35min` 才走线B，否则走线A。
- AAPS 每 ~5min 循环连泵刷新时间戳 → 始终走线B；一旦失联（>35min 无命令），**自动回退线A 本地安全基线**，不会卡在被污染的旧 profile 上。
- TBR 到期后（`now >= tbr_expiry_ms`）同样回落到当前基准线，符合现有逻辑。

---

## 8. 状态展示（改 `ui_screen.cpp` / 初始屏）

- 状态页 / 初始屏 `0x02` 增加 1 字节标识当前 `g_basal_source`（0=LOCAL, 1=AAPS），便于现场一眼看出现在跑的是哪条线。
- `g_pump_state.current_basal_rate` 仍反映实际速率（已天然跟线）。

---

## 9. 联调自测（改 `test/run_link_sim.sh` + host 桩）

- `host_glue.cpp` 补 `g_aaps_basal` / `g_aaps_profile_valid` / `g_basal_source` / `g_aaps_last_cmd_ms` 桩与 `aaps_basal_load/save` 空实现。
- 新增字节级断言：
  1. `0x66` 写 24 段后，`0x65` 读回完全一致 **且** `g_pump_config.profiles` 内容不变（线A 未被污染）。
  2. `0xC1` 设 TBR 后，`0x02` 初始屏 TBR 标志 + 速率正确。
  3. 模拟 `g_aaps_last_cmd_ms` 超过 35min → `basal_rate_for_now()` 自动回退线A（用 host 直接调用断言，或加 1 条 0xC1 后快进时间的用例）。

需先 `bash test/run_link_sim.sh` 确认现有 75 条断言不退化，再新增上述 3 条，目标 PASS=全绿。

---

## 10. 安全措施 / 风险

- **线A 回退基线不能是污染或全 0**：接入 AAPS 前，应在泵菜单确认本体基础率档案为合理本地基线（默认 0.5 U/h 全天亦可）。README / 联调清单需加一句提醒。
- **伴生 App 在闭环时建议不推长时基础率**：App 的 `basal_override` 是限时覆盖，闭环时会被 AAPS TBR 压住（TBR 优先级最高），短时（排气/应急）可用；但不要在 AAPS 闭环会话中通过 App 长期改写基础率，避免混淆。不强改代码，仅文档约定。
- 线B `g_aaps_basal` 初始化为 0 时 `g_aaps_profile_valid=0`，强制回退线A，避免「空线B 当 0 基础率跑」。

---

## 11. 改动文件清单 & 提交分组

| 文件 | 改动 |
|---|---|
| `src/pump_types.h` / `src/pump_state.h` | 新增线B 数据结构声明 |
| `src/storage.cpp` | `olp_aaps` 命名空间 load/save |
| `src/aaps_dana.cpp` | 0x65/0x66/0x67/TBR 重定向线B + 时间戳 |
| `src/basal_scheduler.cpp` | `basal_rate_for_now` 按 source 选线 + 回退 |
| `src/ui_screen.cpp` | 初始屏/状态页加 source 标识 |
| `test/host_glue.cpp` + `test/run_link_sim.sh` | 双线桩 + 断言 |

提交分组：
- `feat(basal): 基础率双线隔离（线A本体NVS / 线B-AAPS独立NVS + 来源标志 + 失联回退）`
- `fix(aaps): 0x65/0x66/0x67/TBR 重定向至线B缓存，本体profile对AAPS不可见`
