/**
 * ina226.cpp — INA226 驱动 (Arduino Wire)
 */
#include "ina226.h"
#include "config.h"
#include <Wire.h>

// 寄存器
#define INA226_REG_CONFIG   0x00
#define INA226_REG_SHUNTV   0x01
#define INA226_REG_BUSV     0x02
#define INA226_REG_POWER    0x03
#define INA226_REG_CURRENT  0x04
#define INA226_REG_CALIB    0x05
#define INA226_REG_MFR_ID   0xFE
#define INA226_REG_MFR_MODEL 0xFF

static uint16_t read_reg(uint8_t reg)
{
    Wire.beginTransmission(INA226_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0xFFFF;
    if (Wire.requestFrom(INA226_I2C_ADDR, (uint8_t)2) != 2) return 0xFFFF;
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    return (uint16_t)((hi << 8) | lo);
}

static bool write_reg(uint8_t reg, uint16_t val)
{
    Wire.beginTransmission(INA226_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val >> 8);
    Wire.write(val & 0xFF);
    return (Wire.endTransmission() == 0);
}

bool ina226_init(void)
{
    Wire.begin(PIN_INA226_SDA, PIN_INA226_SCL);
    Wire.setClock(I2C_FREQ_HZ);

    // CONFIG: 复位后默认值。显式设为连续测量, AVG=1, VBUS/CT=1.1ms, Shunt=2.116us
    // 0x4127 = RESET; 正常配置 0x0247 (与传感器默认一致)
    write_reg(INA226_REG_CONFIG, 0x0247);
    // 校准寄存器: 0.00512 / (CurrentLSB * Rshunt) = 2048
    write_reg(INA226_REG_CALIB, INA226_CAL_VALUE);

    return ina226_self_test();
}

bool ina226_self_test(void)
{
    uint16_t mfr = read_reg(INA226_REG_MFR_ID);
    return (mfr == 0x5449);  // INA226 Manufacturer ID
}

uint16_t ina226_read_bus_voltage_mv(void)
{
    uint16_t raw = read_reg(INA226_REG_BUSV);
    if (raw == 0xFFFF) return 0;
    // LSB = 1.25 mV
    return (uint16_t)((uint32_t)raw * 125 / 100);
}

int32_t ina226_read_current_ma(void)
{
    uint16_t raw = read_reg(INA226_REG_CURRENT);
    if (raw == 0xFFFF) return 0;
    // LSB = 125 µA = 0.125 mA
    int16_t s = (int16_t)raw;
    return (int32_t)s * 125 / 1000;
}

int32_t ina226_read_power_mw(void)
{
    uint16_t raw = read_reg(INA226_REG_POWER);
    if (raw == 0xFFFF) return 0;
    // Power LSB = 25 * CurrentLSB = 25 * 0.125mA = 3.125 mW
    return (int32_t)raw * 25 / 8;
}

bool ina226_read(ina226_telemetry_t *tel)
{
    if (!tel) return false;
    uint16_t bus = read_reg(INA226_REG_BUSV);
    uint16_t shunt = read_reg(INA226_REG_SHUNTV);
    uint16_t cur = read_reg(INA226_REG_CURRENT);
    uint16_t pwr = read_reg(INA226_REG_POWER);

    tel->valid = !(bus == 0xFFFF || cur == 0xFFFF);
    tel->bus_voltage_mv  = tel->valid ? (uint16_t)((uint32_t)bus * 125 / 100) : 0;
    tel->shunt_voltage_uv = (int16_t)shunt * 5 / 2;   // LSB 2.5µV
    tel->current_ma = tel->valid ? (int32_t)(int16_t)cur * 125 / 1000 : 0;
    tel->power_mw   = tel->valid ? (int32_t)pwr * 25 / 8 : 0;
    tel->motor_current_ma = tel->current_ma;
    return tel->valid;
}
