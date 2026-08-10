/**
 * pump_types.h — 共享类型定义与数据结构 (Arduino 框架)
 *
 * OpenLoop Insulin Pump Firmware
 */

#ifndef PUMP_TYPES_H
#define PUMP_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <cstddef>   // size_t (本头用到, 固件靠 Arduino.h 间接提供, 模拟器需显式引入以保证双端一致)

// ============================================================
// 状态机
// ============================================================

typedef enum {
    PUMP_STATE_BOOTING = 0,
    PUMP_STATE_IDLE,
    PUMP_STATE_PRIMING,
    PUMP_STATE_DELIVERING,
    PUMP_STATE_BASAL,
    PUMP_STATE_BOLUS,
    PUMP_STATE_STOPPING,
    PUMP_STATE_ALARM,
    PUMP_STATE_SLEEP,
    PUMP_STATE_ERROR
} pump_state_t;

// ============================================================
// 报警码
// ============================================================

typedef enum {
    ALARM_NONE              = 0x00,
    ALARM_BATTERY_LOW       = 0x01,  // 电池 < 20%
    ALARM_BATTERY_CRITICAL  = 0x02,  // 电池 < 10%
    ALARM_RESERVOIR_EMPTY   = 0x03,  // 储药器空
    ALARM_OCCLUSION         = 0x04,  // 阻塞
    ALARM_MOTOR_FAULT       = 0x05,  // 电机故障
    ALARM_COMM_LOST         = 0x06,  // 通信丢失
    ALARM_OVERFLOW          = 0x07,  // 推注超时
    ALARM_OVER_TEMP         = 0x08,  // 过温
    ALARM_OVER_CURRENT      = 0x09,  // 过流
    ALARM_LIMIT_TRIGGERED   = 0x0A,  // 限位触发
    ALARM_WATCHDOG          = 0x0B,  // 看门狗
    ALARM_NVS_ERROR         = 0x0C,  // 存储错误
    ALARM_OTA_FAILED        = 0x0D,  // OTA 失败
    ALARM_PUMP_STALLED      = 0x0E,  // 泵卡住
    ALARM_STEP_LOSS         = 0x0F,  // 电机丢步 (INA226 电流异常)
} alarm_code_t;

// ============================================================
// 电机命令类型
// ============================================================

typedef enum {
    MOTOR_CMD_NONE = 0,
    MOTOR_CMD_BOLUS,
    MOTOR_CMD_BASAL_TICK,
    MOTOR_CMD_BOLUS_EXT,   // 方波/双波延展量的一次性微投递(由 basal_scheduler 按 duration 时间维铺开)
    MOTOR_CMD_PRIME,
    MOTOR_CMD_STOP,
    MOTOR_CMD_REWIND,
    MOTOR_CMD_CALIBRATE,
    MOTOR_CMD_MANUAL,         // 完全手动电机控制 (电机测试): 定量前进/后退 或 连续点动(steps=0), 经 BLE CONTROL 0x15 下发
    MOTOR_CMD_CANCEL_BOLUS,   // 取消正在进行的大剂量 (置 abort 标志)
    MOTOR_CMD_BASAL_TEST,     // 基础率验证测试: 把当前方案 24 段总量一次性打出(走大剂量的分段+安全复检路径,
                              //   但历史记为 EVENT_TYPE_BASAL_TEST, 不计入大剂量次数/IOB, 便于对照电机行程核验)
} motor_cmd_type_t;

typedef enum {
    MOTOR_DIR_FORWARD = 0,
    MOTOR_DIR_REVERSE = 1
} motor_dir_t;

// ============================================================
// 按键板事件 (4 键: 上/下/确认/取消)
// ============================================================

typedef enum {
    KEY_NONE = 0,
    KEY_UP,
    KEY_DOWN,
    KEY_SET,
    KEY_ESC,
    KEY_LONG_SET,    // 长按确认 → 进入菜单/保存原点
    KEY_LONG_ESC     // 长按取消 → 关机
} key_event_t;

typedef struct {
    motor_cmd_type_t type;
    uint32_t         steps;        // 步数 (MOTOR_CMD_MANUAL 中 steps=0 表示连续点动直到停止)
    uint16_t         speed_hz;     // 速度 Hz (0 = 默认)
    uint16_t         accel_hz;     // 加速度 (0 = 默认)
    uint8_t          dir;          // 方向 MOTOR_DIR_FORWARD(0)/REVERSE(1) — 手动控制/通用命令使用
    uint32_t         delay_ms;     // 延迟执行 ms
    // 大剂量专用
    uint32_t         units_x100;   // 单位 × 100 (e.g. 500 = 5.00U)
    uint8_t          duration_min; // 方波大剂量持续时间 min
    bool             is_extended;  // 是否为方波大剂量
    // 基础率专用
    float            rate_uh;      // 基础率速率 U/h
    uint8_t          kind;         // 大剂量类型 (bolus_kind_t, 见 ui_hal.h)
} motor_command_t;

// ============================================================
// 泵配置 (存储于 Preferences)
// ============================================================

#define MAX_BASAL_PROFILES  4
#define BASAL_SLOTS_PER_DAY 24

typedef struct {
    uint8_t  hour;
    float    rate_uh;       // 基础率 U/h
} basal_slot_t;

typedef struct {
    char     name[32];
    basal_slot_t slots[BASAL_SLOTS_PER_DAY];
} basal_profile_t;

typedef struct {
    // --- 基础参数 ---
    float    insulin_concentration;       // U-100 = 100
    uint32_t ble_passkey;                 // BLE 配对密钥 (0 = Just Works)
    uint8_t  active_profile;              // 当前基础率方案 (0-3)

    // --- 基础率方案 ---
    basal_profile_t profiles[MAX_BASAL_PROFILES];

    // --- 大剂量限制 ---
    float    max_bolus_per_hour;          // 最大大剂量/小时 (默认 25U)
    float    max_basal_per_hour;          // 最大基础率/小时 (默认 5U/h)
    float    max_bolus_single;            // 单次最大大剂量 (默认 25U)

    // --- 闭环参数 ---
    float    carb_ratio[24];              // 碳水比例 (g/U)
    float    isf[24];                     // 胰岛素敏感系数 (mg/dL per U)
    uint16_t target_glucose[24];          // 目标血糖 (mg/dL)

    // --- 校准参数 ---
    float    motor_steps_per_unit;        // 电机步数/U
    float    syringe_area_mm2;            // 储药器截面积
    float    lead_screw_pitch_mm;         // 丝杠导程
    uint16_t motor_microstep;             // 细分系数
    float    units_per_ml;                // 胰岛素浓度
    float    dose_calibration;            // P3-14: 剂量标定系数 (实测/指令, 默认 1.0)

    // --- 安全参数 ---
    uint16_t occlusion_threshold;         // 阻塞检测阈值
    uint8_t  watchdog_timeout_sec;        // 看门狗超时
    float    over_temp_threshold_c;       // 过温阈值

    // --- 显示 / 用户设置 (持久化, 供本地 UI 与独立手机 App 共用) ---
    uint8_t  display_brightness;          // 背光亮度 0-100 (%)
    uint8_t  keypad_sound;                // 0=关 1=开
    uint8_t  vibrate_enabled;             // P3-15: 振动反馈开关 0=关 1=开 (默认关)
    uint32_t rtc_base_unix;               // 时钟基准 Unix 秒 (0 = 未设置)

    // --- 省电 ---
    uint8_t  auto_dim_enabled;            // 空闲自动熄屏 0=关 1=开 (默认开)
    uint16_t auto_dim_timeout_s;          // 空闲超时(秒), 超时后关背光+关屏 (默认 30)

    // --- 统计 (累积) ---
    uint32_t total_units_x100_delivered;  // 累计注射单位 × 100
    uint32_t total_runtime_seconds;       // 累计运行秒数
    uint32_t total_bolus_count;           // 大剂量次数

    // ⚠️ 新增字段一律追加在结构体末尾 ——
    //    storage_load_config 依赖"旧存档字节数 < sizeof 时前缀仍可用"做向前兼容,
    //    在中间插字段会让老存档整体错位(基础率方案被垃圾数据覆盖), 严禁。
    // --- 环模式偏好 (2026-08-08 修复) ---
    //    原先 loop_mode 只存在于运行时状态, 重启后 pump_state_init 必置 0=闭环,
    //    表现为"App 里切了开环/暂停, 重启又显示闭环中"。现持久化并开机恢复。
    uint8_t  loop_mode_pref;              // 0 闭环 / 1 开环 / 2 暂停
} pump_config_t;

// ============================================================
// INA226 遥测 (电池/电机电流电压)
// ============================================================

typedef struct {
    uint16_t bus_voltage_mv;     // 总线电压 (电池电压) mV
    int16_t  shunt_voltage_uv;   // 分流电压 µV
    int32_t  current_ma;         // 总电流 mA (电池母线)
    int32_t  power_mw;           // 功率 mW
    int32_t  motor_current_ma;   // 运动期间电机电流估计 mA
    bool     valid;              // 读数是否有效
} ina226_telemetry_t;

// ============================================================
// 泵运行状态 (实时)
// ============================================================

typedef struct {
    uint8_t      current_state;           // pump_state_t (enum)
    uint8_t      alarm_code;              // alarm_code_t
    uint8_t      alarm_active;
    uint16_t     reservoir_units_left;     // 剩余药量 (U)
    uint16_t     battery_mv;              // 电池电压 (mV)
    uint8_t      battery_pct;             // 电池百分比
    uint16_t     last_glucose_mgdl;       // 最近血糖 (mg/dL; 自定义 BLE / Dana 0x48 双通路均写入此处)
    int8_t       glucose_trend;           // CGM 趋势 5档显示码: -2 速降 / -1 缓降 / 0 平稳 / +1 缓升 / +2 速升
    uint32_t     last_glucose_time_unix;  // 最近血糖接收时间 (Unix 秒; 离线/过期判定)
    uint32_t     iob_x10000;              // IOB × 10000
    uint32_t     last_bolus_time;         // 上次大剂量 Unix 时间
    uint32_t     last_basal_time;         // 上次基础率 Unix 时间
    float        current_basal_rate;      // 当前基础率 U/h
    uint32_t     motor_position;          // 当前电机位置 (微步)
    uint32_t     motor_max_position;      // 最大位置 (对应空储药器)
    bool         ble_connected;           // BLE 连接状态
    bool         is_primed;               // 是否已排气
    float        board_temp_c;            // 板载温度(°C, P3-13: 过温检测源, 来自 ui_hal_get_board_temp_c)
    // --- 闭环 / 临时基础率 / 今日统计 ---
    uint8_t      loop_mode;               // 0 闭环(AAPS接管) / 1 开环(本地档案) / 2 暂停
    uint32_t     today_units_x100;        // 今日累计注射 (U × 100)
    uint32_t     total_units_x100_delivered; // 累计注射单位 × 100 (全生涯 live 计数, 由 motor/basal 实时累加)
    float        tbr_percent;             // 临时基础率百分比 (0 = 无)
    float        tbr_rate;                // 临时基础率 U/h (绝对)
    uint32_t     tbr_expiry_ms;           // 临时基础率到期时间 (millis())
    // --- INA226 实时遥测 ---
    uint16_t     battery_current_ma;      // 电池电流 mA
    uint16_t     motor_current_ma;        // 电机实时电流 mA (运动/空闲均采样, 见 motor_stall_guard_tick / safety_task)
    uint16_t     motor_current_peak_ma;   // 电机峰值电流 mA (单次运动期间峰值, 用于标定堵转阈值)
    uint16_t     bus_power_mw;            // 母线功率 mW
    bool         step_loss_detected;      // 丢步/异常标志
    // --- 方波/双波延展量(按时间铺开, 由 basal_scheduler 驱动) ---
    bool     ext_bolus_active;          // 延展量铺开中
    uint8_t  ext_bolus_kind;            // 大剂量类型 (bolus_kind_t)
    uint32_t ext_bolus_total_x100;      // 延展总量 × 100
    uint32_t ext_bolus_delivered_x100;  // 已铺开量 × 100
    uint32_t ext_bolus_duration_ms;     // 总时长 ms
    uint32_t ext_bolus_start_ms;        // 起始 millis()
    // --- Dana / AAPS 接管状态 ---
    bool     dana_paired;               // AAPS 已完成 Dana 握手接管 (闭环页区分显示)

    // --- P0~P2 新增非阻塞状态/进度(单一真源, 模拟器共用同一份) ---
    uint8_t  reservoir_low_warn;        // 低药量预警(非阻塞, 1=剩余≤阈值, 提示换笔芯)
    uint8_t  keypad_locked;             // 按键锁 (1=锁定, 需长按"确认"解锁, 防误触)
    uint8_t  bolus_progress_pct;        // 大剂量进度 (0-100, 首页/上报显示)
    uint32_t bolus_delivered_x100;      // 当前大剂量已输注量 (U×100, 供 AAPS 0x40 进度查询/通知)
    uint8_t  missed_bolus;              // 错过大剂量提醒标志 (1=有待处理提醒, 见 P2-12)
    uint8_t  over_temp_warn;            // 过温预警(非阻塞, 1=接近阈值)

    // --- 基础率速率覆盖 (伴生 App BASAL 通道直推; 2026-08-08) ---
    //   闭环/开环模式下, 基础率的**真源是 g_pump_config.profiles[active].slots[hour]**
    //   (AAPS 用 Dana 0x66 写这里, 真实 Dana-i 也是泵自己跑档案 + AAPS 用 TBR 调节)。
    //   伴生 App 若通过 BASAL 通道直推一个瞬时速率, 记在这里做**限时覆盖**,
    //   超时自动回落到档案, 避免 App 断连后泵被永久钉死在某个速率上。
    float    basal_override_uh;         // 覆盖速率 U/h
    uint32_t basal_override_ms;         // 覆盖写入时刻 millis()
    uint8_t  basal_override_valid;      // 1 = 覆盖有效

    // --- 基础率验证测试 (一次性打出全天总量, 供用户对照电机行程核验) ---
    uint8_t  basal_test_running;        // 1 = 测试注射进行中
    uint32_t basal_test_units_x100;     // 本次测试目标总量 ×100
} pump_runtime_state_t;

// ============================================================
// 历史事件 (环形缓冲区)
// ============================================================

typedef enum {
    EVENT_TYPE_BOLUS        = 0x01,
    EVENT_TYPE_BASAL_RATE   = 0x02,
    EVENT_TYPE_TBR          = 0x03,
    EVENT_TYPE_PRIME        = 0x04,
    EVENT_TYPE_REWIND       = 0x05,
    EVENT_TYPE_ALARM        = 0x06,
    EVENT_TYPE_ALARM_CLEAR  = 0x07,
    EVENT_TYPE_RESERVOIR    = 0x08,
    EVENT_TYPE_BATTERY      = 0x09,
    EVENT_TYPE_POWER_ON     = 0x0A,
    EVENT_TYPE_POWER_OFF    = 0x0B,
    EVENT_TYPE_CALIBRATE    = 0x0C,
    EVENT_TYPE_BASAL_TEST   = 0x0D,   // 基础率验证测试 (一次性打出全天 24 段总量)
} event_type_t;

#pragma pack(push, 1)
typedef struct {
    uint32_t  timestamp;     // Unix 时间戳
    uint8_t   type;          // event_type_t
    uint8_t   alarm_code;    // 报警码 (仅 EVENT_TYPE_ALARM)
    uint32_t  param1;        // 参数 (单位 × 100)
    uint16_t  param2;        // 辅助参数
} history_event_t;
#pragma pack(pop)

// ============================================================
// CRC-8 校验 (用于 BLE 包)
// ============================================================

uint8_t crc8_ccitt(const uint8_t *data, size_t len);

#endif // PUMP_TYPES_H
