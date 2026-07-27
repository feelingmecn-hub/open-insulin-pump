/**
 * ina226.h — INA226 电流/电压监测驱动 (Arduino Wire 库)
 *
 * I2C 地址 0x40 (A0=A1=GND), 分流 20mΩ, 电流 LSB 125µA, 校准 2048。
 * ⚠️ 必须 Wire.begin(PIN_INA226_SDA, PIN_INA226_SCL) 指定引脚,
 *    不能用 Arduino 默认 I2C (GPIO21/22 已被 LCD 占用)。
 */
#pragma once

#include "pump_types.h"

bool ina226_init(void);
bool ina226_read(ina226_telemetry_t *tel);
uint16_t ina226_read_bus_voltage_mv(void);
int32_t  ina226_read_current_ma(void);
int32_t  ina226_read_power_mw(void);
bool ina226_self_test(void);
