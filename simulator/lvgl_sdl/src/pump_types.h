/**
 * pump_types.h — 共享类型定义与数据结构 (Arduino 框架)
 *
 * OpenLoop Insulin Pump Firmware
 */

#ifndef PUMP_TYPES_H
#define PUMP_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <cstddef>      // size_t (模拟器独立编译时需要; 固件由 Arduino.h 间接提供)

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
    MOTOR_CMD_PRIME,
    MOTOR_CMD_STOP,
    MOTOR_CMD_REWIND,
    MOTOR_CMD_CALIBRATE,
    MOTOR_CMD_CANCEL_BOLUS,   // 取消正在进行的大剂量 (置 abort 标志)
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
    uint32_t         steps;        // 步数
    uint16_t         speed_hz;     // 速度 Hz (0 = 默认)
    uint16_t         accel_hz;     // 加速度 (0 = 默认)
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

    // --- 安全参数 ---
    uint16_t occlusion_threshold;         // 阻塞检测阈值
    uint8_t  watchdog_timeout_sec;        // 看门狗超时
    float    over_temp_threshold_c;       // 过温阈值

    // --- 统计 (累积) ---
    uint32_t total_units_x100_delivered;  // 累计注射单位 × 100
    uint32_t total_runtime_seconds;       // 累计运行秒数
    uint32_t total_bolus_count;           // 大剂量次数
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
    uint8_t      current_state;           // pump_state_t
    uint8_t      alarm_code;              // alarm_code_t
    uint8_t      alarm_active;
    uint16_t     reservoir_units_left;     // 剩余药量 (U)
    uint16_t     battery_mv;              // 电池电压 (mV)
    uint8_t      battery_pct;             // 电池百分比
    uint16_t     last_glucose_mgdl;       // 最近血糖
    int8_t       glucose_trend;           // CGM 趋势 (箭头方向)
    uint32_t     iob_x10000;             // IOB × 10000
    uint32_t     last_bolus_time;         // 上次大剂量 Unix 时间
    uint32_t     last_basal_time;         // 上次基础率 Unix 时间
    float        current_basal_rate;      // 当前基础率 U/h
    uint32_t     motor_position;          // 当前电机位置 (微步)
    uint32_t     motor_max_position;      // 最大位置 (对应空储药器)
    bool         ble_connected;           // BLE 连接状态
    bool         is_primed;               // 是否已排气
    int8_t       board_temp_c;            // 板载温度
    // --- 闭环 / 临时基础率 / 今日统计 ---
    uint8_t      loop_mode;               // 0 闭环(AAPS接管) / 1 开环(本地档案) / 2 暂停
    uint32_t     today_units_x100;        // 今日累计注射 (U × 100)
    float        tbr_percent;             // 临时基础率百分比 (0 = 无)
    float        tbr_rate;                // 临时基础率 U/h (绝对)
    uint32_t     tbr_expiry_ms;           // 临时基础率到期时间 (millis())
    // --- INA226 实时遥测 ---
    uint16_t     battery_current_ma;      // 电池电流 mA
    uint16_t     motor_current_ma;        // 电机电流 mA (运动期间)
    uint16_t     bus_power_mw;            // 母线功率 mW
    bool         step_loss_detected;      // 丢步/异常标志
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
