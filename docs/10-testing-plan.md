# 测试方案

## 1. 测试策略总览

```
┌──────────────────────────────────────────────────────┐
│ 测试金字塔                                           │
│                                                      │
│       ┌──────────┐                                  │
│       │ 系统测试  │ ← 完整泵 + CGM + AAPS 联调      │
│      ┌┴──────────┴┐                                  │
│      │  集成测试   │ ← BLE + 电机 + 安全 联合测试    │
│     ┌┴────────────┴┐                                 │
│     │  单元测试     │ ← 每个模块独立测试             │
│    ┌┴──────────────┴┐                                │
│    │  硬件验证       │ ← PCB + 电源 + 信号完整性     │
│   └─────────────────┘                                │
└──────────────────────────────────────────────────────┘
```

| 阶段 | 目标 | 时间 |
|------|------|------|
| 硬件验证 | 所有电压/信号/通信正确 | PCB 打样后 1 天 |
| 单元测试 | 每个软件模块独立正确 | 2-3 天 |
| 集成测试 | 模块间协同工作正确 | 2-3 天 |
| 精度测试 | 0.05U 精度达标 | 1 天 |
| 安全测试 | 安全机制全部触发正确 | 1-2 天 |
| 续航测试 | 满电≥3 天连续工作 | 3 天 |
| 系统测试 | 完整闭环场景 | 2-3 天 |
| 压力测试 | 7×24h 连续运行 | 7 天 |

---

## 2. 硬件验证

### 2.1 电源测试

| 测试项 | 方法 | 合格标准 |
|--------|------|----------|
| USB 5V 输入 | 万用表测 VBUS | 5.0V ± 0.25V |
| 3S 电池电压 | 万用表测 BAT+/BAT- | 9.0-12.6V |
| 电池充电 | 插入 USB, 监测充电电流 | ≤ 1A, 充满 12.6V |
| 5V 降压输出 | 使能 DC-DC, 测 OUT | 5.0V ± 0.1V |
| 5V 纹波 | 示波器 AC 耦合 | < 50mVpp |
| 3.3V 输出 | 测 ESP32 3.3V 引脚 | 3.30V ± 0.15V |
| 3.3V 纹波 | 示波器 AC 耦合 | < 50mVpp |
| 空载功耗 | USB 供电, 测电流 | < 100mA |
| 满载功耗 | 电机推注 + BLE 连接 | < 500mA |

### 2.2 信号完整性

| 测试项 | 方法 | 合格标准 |
|--------|------|----------|
| STEP 脉冲 | 示波器测 DRV8825 STEP | 50µs 脉宽, 频率按预期 |
| DIR 建立时间 | STEP 前 DIR 改变 | DIR 比 STEP 早 ≥ 200ns |
| I2C 时序 | 逻辑分析仪测 SDA/SCL | 400kHz, 无异常 |
| BLE 射频 | nRF Connect 测 RSSI | ≥5m 距离 RSSI > -70dBm |
| ADC 精度 | 输入已知电压, 读 ADC | 偏差 < 5% |

### 2.3 GPIO 验证

```bash
# ESP32 简单 GPIO 测试脚本 (Arduino 平台可用类似方法)
# 切换所有 GPIO 高低电平, 万用表验证
```

| 引脚 | 功能 | 验证方法 |
|------|------|----------|
| GPIO0 | STEP | 输出方波, 示波器观察 |
| GPIO1 | DIR | 输出切换, 测电平 |
| GPIO2 | ADC/限位 | 输入上拉, 读状态 |
| GPIO3 | 电机 ADC | 读 ADC 原始值 |
| GPIO4-6 | M0-M2 | 输出不同组合, 测电平 |
| GPIO7 | nFAULT | 接 GND/3.3V, 读状态 |
| GPIO10 | ENABLE | 输出切换, 测 DRV8825 ENABLE |
| GPIO12-13 | 限位 | 短接 GND, 读 LOW |
| GPIO18-19 | I2C | 接 INA226（SDA/SCL），验证电流/电压读取 |

---

## 3. 单元测试

### 3.1 电机控制器测试

```cpp
// 测试文件: test/test_motor.cpp

TEST_CASE("motor_init", "[motor]") {
    motor_init();
    // 验证所有引脚初始状态
    TEST_ASSERT_EQUAL(1, gpio_get_level(PIN_MOTOR_ENABLE));   // 禁用
    TEST_ASSERT_EQUAL(1, gpio_get_level(PIN_MOTOR_nSLEEP));   // 唤醒
    TEST_ASSERT_EQUAL(1, gpio_get_level(PIN_MOTOR_nRESET));   // 未复位
}

TEST_CASE("motor_step_pulse", "[motor]") {
    uint32_t pos_before = motor_get_position();
    motor_enable();
    motor_step_pulse(100, 1000);
    motor_disable();
    uint32_t pos_after = motor_get_position();
    TEST_ASSERT_EQUAL(pos_before + 100, pos_after);
}

TEST_CASE("motor_direction", "[motor]") {
    motor_set_direction(MOTOR_DIR_FORWARD);
    uint32_t pos = motor_get_position();
    motor_step_pulse(50, 1000);
    TEST_ASSERT_EQUAL(pos + 50, motor_get_position());

    motor_set_direction(MOTOR_DIR_REVERSE);
    motor_step_pulse(50, 1000);
    TEST_ASSERT_EQUAL(pos, motor_get_position());  // 回到原位
}

TEST_CASE("motor_microstep_set", "[motor]") {
    motor_set_microstep(1,1,1);  // 1/32
    // 验证 M0=M1=M2=HIGH
    TEST_ASSERT_EQUAL(1, gpio_get_level(PIN_MOTOR_M0));
    TEST_ASSERT_EQUAL(1, gpio_get_level(PIN_MOTOR_M1));
    TEST_ASSERT_EQUAL(1, gpio_get_level(PIN_MOTOR_M2));
}

TEST_CASE("motor_fault_detect", "[motor]") {
    // 模拟 nFAULT LOW
    gpio_set_level(PIN_MOTOR_nFAULT, 0);
    TEST_ASSERT_TRUE(motor_is_fault());
    gpio_set_level(PIN_MOTOR_nFAULT, 1);
    TEST_ASSERT_FALSE(motor_is_fault());
}
```

### 3.2 BLE GATT 测试

```python
# 测试脚本: test/test_ble.py
# 使用 bleak 库在电脑上测试

import asyncio
from bleak import BleakClient, BleakScanner

DEVICE_NAME = "OpenLoop-Pump"
BOLUS_CHAR_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

async def test_scan():
    devices = await BleakScanner.discover(timeout=5)
    pump = [d for d in devices if d.name == DEVICE_NAME]
    assert len(pump) > 0, "Pump not found"

async def test_connect():
    pump = await find_pump()
    async with BleakClient(pump.address) as client:
        assert client.is_connected

async def test_bolus_command():
    pump = await find_pump()
    async with BleakClient(pump.address) as client:
        # 构造 bolus 命令: 1.00U, immediate, CRC
        import struct
        units_x100 = 100  # 1.00U
        payload = struct.pack('<BIBBB', 0x01, units_x100, 0, 0, 0)
        # 计算并追加 CRC
        crc = crc8_ccitt(payload)
        payload += bytes([crc])
        await client.write_gatt_char(BOLUS_CHAR_UUID, payload)
        # 验证状态变化
        # ...

async def test_status_read():
    pump = await find_pump()
    async with BleakClient(pump.address) as client:
        data = await client.read_gatt_char(STATUS_CHAR_UUID)
        assert len(data) == 16
        assert data[15] == crc8_ccitt(data[:15])
```

### 3.3 安全监控测试

```cpp
TEST_CASE("safety_battery_alarm", "[safety]") {
    // 模拟低电压
    pump_state_update_battery(6200, 5);  // 6.2V = 5%
    // 手动触发安全检查
    // ... (需要模拟 safety_task 检查周期)
    TEST_ASSERT_EQUAL(ALARM_BATTERY_CRITICAL, safety_get_alarm());
}

TEST_CASE("safety_limit_switch", "[safety]") {
    // 模拟限位触发
    gpio_set_level(PIN_LIMIT_FWD, 0);
    motor_enable();
    // 等待安全检查周期
    vTaskDelay(pdMS_TO_TICKS(1100));
    TEST_ASSERT_TRUE(!safety_is_ok());
    motor_disable();
    gpio_set_level(PIN_LIMIT_FWD, 1);
}

TEST_CASE("safety_command_range", "[safety]") {
    // 测试超范围 bolus 被拒绝
    // (在 motor_deliver_bolus 中有检查)
}
```

### 3.4 存储测试

```cpp
TEST_CASE("storage_config_save_load", "[storage]") {
    pump_config_t config_out, config_in;
    memset(&config_out, 0xAA, sizeof(config_out));
    config_out.insulin_concentration = 100;
    config_out.max_bolus_single = 15.0f;

    TEST_ASSERT_TRUE(storage_save_config(&config_out));

    memset(&config_in, 0, sizeof(config_in));
    TEST_ASSERT_TRUE(storage_load_config(&config_in));

    TEST_ASSERT_EQUAL_FLOAT(100, config_in.insulin_concentration);
    TEST_ASSERT_EQUAL_FLOAT(15.0, config_in.max_bolus_single);
}

TEST_CASE("storage_history_append", "[storage]") {
    storage_clear_events();

    history_event_t event = {
        .timestamp = 1234567890,
        .type = EVENT_TYPE_BOLUS,
        .alarm_code = 0,
        .param1 = 500,  // 5.00U
        .param2 = 0
    };

    TEST_ASSERT_TRUE(storage_append_event(&event));
    TEST_ASSERT_EQUAL(1, storage_get_event_count());

    history_event_t buf[10];
    int count = storage_get_events(0, buf, 10);
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL(500, buf[0].param1);
}
```

---

## 4. 精度测试 ⭐

### 4.1 0.05U 精度验证

**方法: 称重法**

```
设备:
  - 分析天平 (精度 0.001g = 1mg)
  - 蒸馏水 (密度 1.000 g/mL)
  - 1mL 胰岛素笔芯 (空)
  - 测试用推注管 (代替笔芯)

步骤:
  1. 安装空笔芯, 执行 prime 排气
  2. 将笔芯充满蒸馏水
  3. 将天平清零, 将笔芯尖端对准称量杯
  4. 命令推注 0.05U × 10 次 (= 0.5U = 5mg 水)
  5. 记录天平读数
  6. 重复 3 次, 统计偏差

合格标准:
  目标: 5.0 mg (0.5U 蒸馏水)
  偏差: < ±1.0 mg (±0.1U)
  标准差: < 0.5 mg (3 次测量)
```

| 测试 | 目标 (mg) | 实测 (mg) | 偏差 (mg) | 偏差 (U) | 合格 |
|------|-----------|-----------|-----------|----------|------|
| 0.05U × 10 | 5.0 | ____ | ____ | ____ | ____ |
| 0.05U × 20 | 10.0 | ____ | ____ | ____ | ____ |
| 0.10U × 10 | 10.0 | ____ | ____ | ____ | ____ |
| 0.50U × 5 | 25.0 | ____ | ____ | ____ | ____ |
| 1.00U × 3 | 30.0 | ____ | ____ | ____ | ____ |
| 5.00U × 1 | 50.0 | ____ | ____ | ____ | ____ |

### 4.2 线性度测试

推注 0.05U ~ 5.00U 不同量, 验证线性关系:

```
plot: 目标量 vs 实测量
斜率: 应 ≈ 1.0
R²: 应 > 0.99
```

### 4.3 回差测试

```
步骤:
  1. 正向推注 1.00U (前进)
  2. 记录位置
  3. 反向推注 1.00U (回退)
  4. 记录位置
  5. 正向推注 1.00U (再次前进)
  6. 比较 3 次实测体积

合格标准: 回差 < 0.05U (齿轮/丝杠间隙)
```

---

## 5. 安全测试

### 5.1 故障注入

| 测试 | 方法 | 预期 |
|------|------|------|
| 物理堵转 | 用钳子夹住推杆 | OCCLUSION 报警, 电机停止 < 100ms |
| 电机断线 | 拔掉电机 JST 连接器 | MOTOR_FAULT 报警 (nFAULT 触发?) |
| 电源跌落 | 可调电源缓慢降至 5.5V | BATTERY_CRITICAL → 停机 |
| 电源骤降 | 瞬间短接电池 | BMS 保护 + 外部 WDT 复位 |
| BLE 超时 | 手机飞行模式 | 5 分钟后 COMM_LOST 报警 |
| BLE 恶意数据 | 发送超大/负数/随机 bolus | CRC/范围检查拒绝 |
| 看门狗超时 | 在 safety_task 中 while(1) | 外部 WDT 在 1.6s 内复位 |
| 过热 | 热风枪 80°C 吹 1 分钟 | OVER_TEMP → 暂停 5 分钟 |

### 5.2 冗余验证

| 测试 | 方法 |
|------|------|
| 双重步数计数 | RMT 计数 vs GPIO ISR 计数, 引入 1 步误差, 验证检测 |
| 限位双路径 | 拉低 GPIO 同时检查 nENABLE 是否上升 |
| 双看门狗 | 分别触发 IWDT 和外部 WDT, 验证独立工作 |

---

## 6. 续航测试

### 6.1 测试设置

```
电池: 2× Samsung 30Q 18650 (3000mAh)
模式: 基础率 1.0 U/h (典型)
环境温度: 25°C (室温)

测量:
  - 第 1 天: 每 1 小时记录电压
  - 第 2 天: 每 2 小时记录电压
  - 第 3 天: 每 1 小时记录电压 (低电阶段)
```

### 6.2 预期续航计算

```
基础率模式功耗预算 (Rev.2 — 3S / 5V):
  ESP32-C6 BLE 连接:  35mA (5V)
  安全监测:            3mA
  LCD (间歇):         8mA (平均)
  电机 (3 分钟/次):   50mA × 0.5s × 20次/h = 约 0.5mA 平均 (11.1V)
  待机 (sleep 间):    1mA (5V)

  总 5V 电流: 约 47mA
  折算 11.1V 电流: 47mA × 5/11.1 / 0.85 ≈ 25mA

  3000mAh / 25mA ≈ 120 小时 ≈ 5 天

大剂量模式下:
  额外电机电流: 500mA × 0.5s / 60s ≈ 4mA 平均 (11.1V)
  折算 11.1V: 4mA (直供, 无需折算)
  总: 25 + 4 = 29mA
  3000mAh / 29mA ≈ 103 小时 ≈ 4.3 天

结论: 基础率模式下 ≥5 天完全可行 (3S 容量瓶颈)
```

### 6.3 实测记录表

| 时间 | 电压 (mV) | 电量% | 模式 | 备注 |
|------|-----------|-------|------|------|
| 0h | ____ | 100% | 基础率 1.0U/h | 满电开始 |
| 24h | ____ | ____ | | |
| 48h | ____ | ____ | | |
| 72h | ____ | ____ | | 目标达成? |
| 直到 8.4V | ____ | 0% | BMS 保护 | 自动关机 |

---

## 7. 系统集成测试

### 7.1 完整闭环模拟

```
测试环境:
  - 泵: ESP32-C6 + DRV8825 + SM2012
  - 手机: Android + AAPS (或测试 APP)
  - CGM 模拟: 手动输入或模拟数据源

测试场景:
  1. 基础率递送: 24 小时基础率方案, 验证每个时段的推注量
  2. 大剂量递送: 1U / 3U / 5U / 10U
  3. 方波大剂量: 5U 在 1 小时内递送
  4. TBR 临时基础率: 200% / 50%
  5. 换药流程: prime + rewind + 重新校准
  6. 低电场景: 基础率持续到低电报警
  7. 通信中断 + 恢复
  8. CGM 数据通过 → IOB 计算 → 闭环建议
```

### 7.2 AAPS 集成测试

```
1. 将泵注册为 AAPS 自定义驱动
2. 在 AAPS 中设置基础率方案
3. AAPS 读取 CGM 数据 → 计算微调 → 发送 TBR
4. 验证 TBR 命令正确执行
5. 进食 → AAPS 计算 bolus → 发送命令
6. 验证 bolus = AAPS 建议值
```

---

## 8. 压力测试 (7×24h)

```
设置:
  - 泵持续运行 7×24h
  - 模拟基础率 1.0 U/h
  - 每 4 小时一次 3U 大剂量
  - 定时记录: 电压、温度、步数偏差

合格标准:
  - 7 天内无意外报警
  - 7 天内无固件崩溃
  - 注射总量偏差 < 5%
  - 电机位置偏差 < 100 微步
```

---

## 9. 测试工具

| 工具 | 用途 |
|------|------|
| ESP-IDF Unit Test | C++ 单元测试框架 |
| pytest + bleak | Python BLE 集成测试 |
| 分析天平 (0.001g) | 精度称重 |
| 示波器 (≥100MHz) | 信号完整性 |
| 逻辑分析仪 | GPIO 时序 |
| 可调电源 (0-15V) | 电源测试 |
| 电子负载 | 续航测试 |
| nRF Connect APP | BLE 调试 |
| 热风枪 | 过热故障注入 |

---

## 10. 测试报告模板

```
=== OpenLoop Pump 测试报告 ===
日期: ____
固件版本: ____
硬件版本: ____
测试人员: ____

1. 硬件验证: PASS / FAIL
   电源: ____
   信号: ____
   GPIO: ____

2. 单元测试: ____/____ PASS (____ failures)
   电机: ____
   BLE: ____
   安全: ____
   存储: ____

3. 精度测试: PASS / FAIL
   0.05U 偏差: ____ mg (____ U)
   线性度 R²: ____
   回差: ____ U

4. 安全测试: PASS / FAIL
   故障检测: ____/____
   冗余: ____/____

5. 续航测试: ____ 小时 (目标 >72h)

6. 系统集成: PASS / FAIL
   闭环: ____
   AAPS: ____

7. 压力测试: PASS / FAIL
   崩溃次数: ____
   偏差: ____%

总体评价: PASS / FAIL
签字: ____
```
