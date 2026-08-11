# AAPS ↔ 闭环泵 联调验证清单

> 实验原型，仅用于协议/功能验证，**严禁用于人体**。本清单用于确认 AAPS 与本泵固件能否正确配合工作。

## 当前固件状态（2026-08-11 已烧录）
- 已含修复：① 大剂量 notify 队列 8→32 + 完成 notify 重发 3 次；② 大剂量「已输注」记账对齐请求量（修复 9.92U 漂移）。
- 烧录方式：分区烧（`flash.sh --input-dir`），**保留 NVS / 蓝牙配对记录**。
- 广播名 `DAN12345AB`，AAPS 选 Dana-i 驱动。

## 测试前置
1. 泵 LCD 亮（正常模式，非 bootloader）；手机 AAPS 已装并配对 `DAN12345AB`。
2. 手机用 USB 连 Mac，本机可 `adb logcat` 抓日志。
3. 抓日志基线命令（按需替换关键词）：
   ```bash
   export PATH="$HOME/Library/Android/sdk/platform-tools:$PATH"
   adb logcat -c
   adb logcat -v time | grep -iE "PUMPBTCOMM|ENCRYPTION__PUMP_CHECK|GET_BASAL_RATE|GET_PUMP_UTC|APS_HISTORY|SetStepBolus|NotifyDelivery|onServicesDiscovered|Device was disconnected"
   ```

## 已验证项（无需重测，记录备查）
- [x] **连接会话正常**：AAPS 连泵 → `ENCRYPTION__PUMP_CHECK BLE5 OK` → 读设置/历史 → 空闲自动断开。每次连接 `status=0` 成功，无失败/超时。
- [x] **「获取泵设置→历史→等待连接→断开」是 AAPS 正常流程**，非故障。
- [x] **「接受新的临时基础率」提示 = 开放循环正常行为**（预测低血糖→建议 0% 短 TBR），非 bug。

---

## 待测项

### ① 连接与会话稳定性
- [ ] 连续多次点 AAPS 蓝牙图标「重连」，每次都能在数秒内连上、不卡「连接中」。
- [ ] 长时间（>10 分钟）空闲后重连正常。
- [ ] 断蓝牙再开，AAPS 能自动/手动重连。
- 判定：logcat 中每次 `onServicesDiscovered` 出现且无 `连接卡在 connecting 超时`。

### ② 泵状态读取正确性（AAPS 显示 vs 泵屏）
- [ ] 储药量（reservoir）一致。
- [ ] 电量（battery）一致。
- [ ] 24 段基础率档案一致。
- [ ] IOB 一致。
- [ ] 泵屏时间为本地时间（时区修复）；AAPS 不再报「大时间差」。
- 判定：逐项肉眼比对，误差为 0（电量/储药量允许 ±1 显示步进）。

### ③ 大剂量（最关键，安全）
> 仅在确认注射器无人体连接、用替代液/空注或可接受的安全剂量下验证。
- [ ] **小剂量 0.1U / 1U**：AAPS 显示「已输注 == 请求量」（修复后应精确相等，不再 9.92U）。
- [ ] 大剂量进度通知实时刷新，完成 notify 收到。
- [ ] **中断大剂量**（手动停止）：AAPS 回报「真实已打部分」，不虚报为整数请求量。
- [ ] 大剂量后 IOB 正确累加、并在后续按时衰减。
- 判定：请求量、泵实际打出的微步数换算量、AAPS「已输注」三者一致（误差 < 0.1U）。

### ④ 临时基础率 TBR
- [ ] 开放循环下点「接受」→ 泵收到对应 % 的短 TBR（15/30 分钟），AAPS 显示当前 TBR 与泵一致。
- [ ] 取消 TBR → 泵恢复基础率；AAPS 显示归零。
- 判定：AAPS 当前基础率数值 == 泵屏当前基础率。

### ⑤ 方波/扩展大剂量（若固件已实现）
- [ ] 发起方波大剂量，AAPS 与泵记录一致、按时段推进。

### ⑥ 回归 / 耐久
- [ ] 连续多次大剂量 + TBR 混合操作无卡死。
- [ ] 断开重连后状态（IOB/基础率/TBR）保持正确。

---

## 如何判定「可以配合正常工作」
全部待测项（①~④）通过，且 ③ 的「已输注==请求量」与「中断回报真实」两项无偏差，即可认为 AAPS 与本泵在协议层面已能正常配合。

> 注意：「协议正常」≠「医疗安全可用」。闭环自动给药前的长期稳定性、边界（低电量/堵转/断连恢复）仍需更多验证。

## 日志关键词速查
| 现象 | 抓日志关键词 |
|---|---|
| 连接/握手 | `PUMPBTCOMM`, `ENCRYPTION__PUMP_CHECK`, `onServicesDiscovered`, `Connect !!` |
| 设置读取 | `GET_BASAL_RATE`, `GET_CIR_CF_ARRAY`, `GET_USER_OPTION`, `GET_PUMP_UTC_AND_TIMEZONE` |
| 大剂量 | `SetStepBolus`, `NotifyDelivery`, `DELIVERY_RATE`, `DELIVERY_COMPLETE` |
| 断开 | `Device was disconnected`, `Queue empty` |
| 异常 | `连接卡在 connecting 超时`, `status=133`, `status=8` |
