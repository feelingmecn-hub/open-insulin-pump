package com.openloop.pump.ui.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavController
import com.openloop.pump.ble.ConnectionState
import com.openloop.pump.ble.PumpProtocolSpec
import kotlin.math.roundToInt

@Composable
fun SettingsScreen(
    nav: NavController,
    viewModel: SettingsViewModel = hiltViewModel()
) {
    val loop by viewModel.closedLoop.collectAsStateWithLifecycle()
    val isf by viewModel.isf.collectAsStateWithLifecycle()
    val target by viewModel.target.collectAsStateWithLifecycle()
    val maxIob by viewModel.maxIob.collectAsStateWithLifecycle()
    val maxBasal by viewModel.maxBasal.collectAsStateWithLifecycle()
    val carbRatio by viewModel.carbRatio.collectAsStateWithLifecycle()
    val nsUrl by viewModel.nsUrl.collectAsStateWithLifecycle()
    val appPasskey by viewModel.appBlePasskey.collectAsStateWithLifecycle()

    val conn by viewModel.connectionState.collectAsStateWithLifecycle()
    val brightness by viewModel.brightness.collectAsStateWithLifecycle()
    val activeProfile by viewModel.activeProfile.collectAsStateWithLifecycle()
    val keypadSound by viewModel.keypadSound.collectAsStateWithLifecycle()
    val vibrate by viewModel.vibrate.collectAsStateWithLifecycle()
    val pumpPasskey by viewModel.pumpPasskey.collectAsStateWithLifecycle()
    val loopMode by viewModel.loopMode.collectAsStateWithLifecycle()
    val maxBolusSingle by viewModel.maxBolusSingle.collectAsStateWithLifecycle()
    val maxBolusPerHour by viewModel.maxBolusPerHour.collectAsStateWithLifecycle()
    val maxBasalPerHour by viewModel.maxBasalPerHour.collectAsStateWithLifecycle()
    val occlusion by viewModel.occlusion.collectAsStateWithLifecycle()
    val watchdog by viewModel.watchdog.collectAsStateWithLifecycle()
    val overTemp by viewModel.overTemp.collectAsStateWithLifecycle()
    val calibration by viewModel.calibration.collectAsStateWithLifecycle()
    val autoDim by viewModel.autoDim.collectAsStateWithLifecycle()
    val autoDimTimeout by viewModel.autoDimTimeout.collectAsStateWithLifecycle()

    val connected = conn is ConnectionState.Connected || conn is ConnectionState.Bonded

    LaunchedEffect(conn) {
        if (connected) viewModel.loadPumpConfig()
    }

    var urlDraft by remember { mutableStateOf(nsUrl ?: "") }
    var secretDraft by remember { mutableStateOf("") }
    var passkeyDraft by remember { mutableStateOf(appPasskey ?: "") }
    var pumpPasskeyDraft by remember { mutableStateOf(pumpPasskey) }
    var primeMl by remember { mutableStateOf("1.0") }
    var calUnits by remember { mutableStateOf("1.0") }
    var calFactor by remember { mutableStateOf("1.000") }

    var confirm by remember { mutableStateOf<ConfirmReq?>(null) }
    confirm?.let { req ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            title = { Text(req.title) },
            text = { Text(req.text) },
            confirmButton = {
                TextButton(onClick = { req.onConfirm(); confirm = null }) { Text("确认执行") }
            },
            dismissButton = {
                TextButton(onClick = { confirm = null }) { Text("取消") }
            }
        )
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        // ---- 泵连接 ----
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text("泵连接", style = MaterialTheme.typography.titleMedium)
                Text(
                    when (conn) {
                        is ConnectionState.Connected, is ConnectionState.Bonded -> "已连接 ✓"
                        is ConnectionState.Connecting -> "连接中…"
                        is ConnectionState.Scanning -> "扫描中…"
                        is ConnectionState.Error -> "错误：${(conn as ConnectionState.Error).reason}"
                        else -> "未连接"
                    },
                    style = MaterialTheme.typography.bodyMedium
                )
            }
        }

        if (!connected) {
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(
                        "未连接泵：以下「泵参数」分类显示上次读取值或默认，需连接后才会真正下发。",
                        style = MaterialTheme.typography.bodySmall
                    )
                }
            }
        }

        // ---- 系统与时间 ----
        CardSection("系统与时间") {
            Text("屏幕亮度：$brightness%", style = MaterialTheme.typography.bodyMedium)
            Slider(
                value = brightness / 100f,
                onValueChange = { viewModel.setBrightness((it * 100).roundToInt()) },
                modifier = Modifier.fillMaxWidth()
            )
            SwitchRow("按键音", keypadSound, viewModel::setKeypadSound)
            SwitchRow("振动反馈", vibrate, viewModel::setVibrate)
            Button(
                onClick = { viewModel.syncTimeFromPhone() },
                modifier = Modifier.fillMaxWidth()
            ) { Text("用手机时间同步泵时钟") }
            Text("泵 BLE 配对码 (6 位数字, 0=Just Works)", style = MaterialTheme.typography.bodyMedium)
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                OutlinedTextField(
                    value = pumpPasskeyDraft,
                    onValueChange = { pumpPasskeyDraft = it.filter { c -> c.isDigit() }.take(6) },
                    modifier = Modifier.weight(1f),
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
                )
                Button(onClick = { viewModel.setPumpPasskey(pumpPasskeyDraft) }) { Text("保存") }
            }
        }

        // ---- 省电 ----
        CardSection("省电") {
            Text(
                "默认屏幕亮度已设为 10%。空闲超时后自动关闭背光并令屏幕休眠；按任意键或手机指令即唤醒，大剂量/报警时保持亮屏。",
                style = MaterialTheme.typography.bodySmall
            )
            SwitchRow("空闲自动熄屏", autoDim) { viewModel.setAutoDim(it, autoDimTimeout) }
            Text("熄屏超时：${autoDimTimeout} 秒", style = MaterialTheme.typography.bodyMedium)
            Slider(
                value = autoDimTimeout / 600f,
                onValueChange = { viewModel.setAutoDim(autoDim, (it * 600).roundToInt()) },
                modifier = Modifier.fillMaxWidth()
            )
            Text(
                "固件另已启用：CPU 降频至 80MHz、BLE 发射功率下调，进一步省电。",
                style = MaterialTheme.typography.bodySmall
            )
        }

        // ---- 环模式 ----
        CardSection("环模式") {
            Text("闭环(AAPS接管) / 开环(本地档案) / 暂停", style = MaterialTheme.typography.bodySmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                val modes = listOf(0 to "闭环", 1 to "开环", 2 to "暂停")
                modes.forEach { (m, label) ->
                    Button(
                        onClick = { viewModel.setLoopMode(m) },
                        modifier = Modifier.weight(1f),
                        enabled = m != loopMode
                    ) { Text(label) }
                }
            }
        }

        // ---- 大剂量与安全限制 ----
        CardSection("大剂量与安全限制") {
            NumberField("单次最大大剂量 (U)", maxBolusSingle.toDouble()) {
                viewModel.setLimit(PumpProtocolSpec.LIMIT_SINGLE, it.toFloat())
            }
            NumberField("每小时最大大剂量 (U)", maxBolusPerHour.toDouble()) {
                viewModel.setLimit(PumpProtocolSpec.LIMIT_PER_HOUR, it.toFloat())
            }
            NumberField("每小时最大基础率 (U/h)", maxBasalPerHour.toDouble()) {
                viewModel.setLimit(PumpProtocolSpec.LIMIT_MAX_BASAL, it.toFloat())
            }
            IntField("阻塞检测阈值 (mA)", occlusion) {
                viewModel.setSafety(PumpProtocolSpec.SAFE_OCCLUSION, it.toFloat())
            }
            IntField("看门狗超时 (秒)", watchdog) {
                viewModel.setSafety(PumpProtocolSpec.SAFE_WATCHDOG, it.toFloat())
            }
            NumberField("过温阈值 (°C)", overTemp.toDouble()) {
                viewModel.setSafety(PumpProtocolSpec.SAFE_OVER_TEMP, it.toFloat())
            }
        }

        // ---- 剂量标定 ----
        CardSection("剂量标定") {
            Text("当前标定系数：${"%.3f".format(calibration)} (指令/实测)", style = MaterialTheme.typography.bodyMedium)
            Text(
                "① 先「推出测试量」并实测体积；② 把 实测体积/指令体积 填入系数并「应用」。",
                style = MaterialTheme.typography.bodySmall
            )
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                OutlinedTextField(
                    value = calUnits, onValueChange = { calUnits = it },
                    label = { Text("测试量 (U)") }, modifier = Modifier.weight(1f),
                    singleLine = true, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal)
                )
                Button(onClick = {
                    val u = calUnits.toFloatOrNull() ?: 1.0f
                    confirm = ConfirmReq("推出标定测试量", "将向管路推注约 ${"%.1f".format(u)}U 测试量，确认？",
                        onConfirm = { viewModel.calibrateDispense(u) })
                }) { Text("推出测试量") }
            }
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                OutlinedTextField(
                    value = calFactor, onValueChange = { calFactor = it },
                    label = { Text("标定系数") }, modifier = Modifier.weight(1f),
                    singleLine = true, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal)
                )
                Button(onClick = {
                    val f = calFactor.toFloatOrNull() ?: 1.0f
                    confirm = ConfirmReq("应用标定系数", "将把剂量标定系数设为 ${"%.3f".format(f)} 并持久化，确认？",
                        onConfirm = { viewModel.applyCalibration(f) })
                }) { Text("应用系数") }
            }
        }

        // ---- 基础率与闭环参数（进入编辑器）----
        CardSection("基础率与闭环参数") {
            Text("在泵内存储 4 套基础率方案与逐时闭环参数，可在此编辑并写回。", style = MaterialTheme.typography.bodySmall)
            Button(
                onClick = { nav.navigate("basal_profile/$activeProfile") },
                modifier = Modifier.fillMaxWidth()
            ) { Text("编辑基础率方案 (当前激活：方案${activeProfile + 1})") }
            Button(
                onClick = { nav.navigate("cl_params") },
                modifier = Modifier.fillMaxWidth()
            ) { Text("编辑泵闭环参数 (ISF / 碳水比 / 目标血糖)") }
        }

        // ---- 维护操作 ----
        CardSection("维护操作") {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                OutlinedTextField(
                    value = primeMl, onValueChange = { primeMl = it },
                    label = { Text("排气量 (U)") }, modifier = Modifier.weight(1f),
                    singleLine = true, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal)
                )
                Button(onClick = {
                    val ml = primeMl.toFloatOrNull() ?: 1.0f
                    confirm = ConfirmReq("开始排气装药", "将向管路推注约 ${"%.1f".format(ml)}U 排气，确认？",
                        onConfirm = { viewModel.primePump(ml) })
                }) { Text("排气装药") }
            }
            Button(
                onClick = {
                    confirm = ConfirmReq("退回装药", "将回退活塞到原点以便装入新的储药器，确认？",
                        onConfirm = { viewModel.rewindPump() })
                },
                modifier = Modifier.fillMaxWidth()
            ) { Text("退回装药（装新储药器）") }
            Button(
                onClick = { viewModel.clearAlarm() },
                modifier = Modifier.fillMaxWidth()
            ) { Text("清除报警") }
            Button(
                onClick = { nav.navigate("motor_test") },
                modifier = Modifier.fillMaxWidth()
            ) { Text("电机手动测试（前进/后退/连续点动）") }
        }

        // ---- App 本地闭环偏好 ----
        CardSection("App 本地偏好 (仅供本机编排)") {
            SwitchRow("自动闭环（调整基础率）", loop, viewModel::setLoop)
            NumberField("胰岛素敏感系数 ISF (mg/dL/U)", isf) { viewModel.setIsp2(it) }
            NumberField("目标血糖 (mg/dL)", target.toDouble()) { viewModel.setTarget(it.toInt()) }
            NumberField("最大 IOB (U)", maxIob) { viewModel.setMaxIob(it) }
            NumberField("最大基础率 (U/h)", maxBasal) { viewModel.setMaxBasal(it) }
            NumberField("碳水比 (g/U)", carbRatio) { viewModel.setCarbRatio(it) }
        }

        // ---- Nightscout ----
        CardSection("Nightscout 云端") {
            OutlinedTextField(value = urlDraft, onValueChange = { urlDraft = it },
                label = { Text("URL") }, modifier = Modifier.fillMaxWidth())
            OutlinedTextField(value = secretDraft, onValueChange = { secretDraft = it },
                label = { Text("API Secret") }, modifier = Modifier.fillMaxWidth())
            Button(onClick = { viewModel.setNightscout(urlDraft, secretDraft) },
                modifier = Modifier.fillMaxWidth()) { Text("保存 Nightscout") }
        }

        // ---- App BLE 配对 (本地) ----
        CardSection("App BLE 配对 (本机)") {
            Text("若泵固件配置了非零 passkey，请在此填入以完成绑定（否则留空 = Just Works）。",
                style = MaterialTheme.typography.bodySmall)
            OutlinedTextField(
                value = passkeyDraft, onValueChange = { passkeyDraft = it },
                label = { Text("Passkey (6 位数字)") }, modifier = Modifier.fillMaxWidth(),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
            )
            Button(onClick = { viewModel.setPasskey(passkeyDraft) },
                modifier = Modifier.fillMaxWidth()) { Text("保存 Passkey") }
        }

        HorizontalDivider()
        Text(
            "⚠ 本应用为 DIY 开源项目，非医疗器械。使用风险自负。",
            style = MaterialTheme.typography.bodySmall
        )
    }
}

@Composable
private fun CardSection(title: String, content: @Composable ColumnScope.() -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
            content = {
                Text(title, style = MaterialTheme.typography.titleMedium)
                content()
            }
        )
    }
}

@Composable
private fun SwitchRow(label: String, checked: Boolean, onChecked: (Boolean) -> Unit) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
        modifier = Modifier.fillMaxWidth()
    ) {
        Text(label)
        Switch(checked = checked, onCheckedChange = onChecked)
    }
}

private data class ConfirmReq(
    val title: String,
    val text: String,
    val onConfirm: () -> Unit
)

@Composable
private fun NumberField(label: String, value: Double, onSet: (Double) -> Unit) {
    var text by remember(value) { mutableStateOf(value.toString()) }
    OutlinedTextField(
        value = text,
        onValueChange = {
            text = it
            it.toDoubleOrNull()?.let(onSet)
        },
        label = { Text(label) },
        modifier = Modifier.fillMaxWidth(),
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal)
    )
}

@Composable
private fun IntField(label: String, value: Int, onSet: (Int) -> Unit) {
    var text by remember(value) { mutableStateOf(value.toString()) }
    OutlinedTextField(
        value = text,
        onValueChange = {
            text = it
            it.toIntOrNull()?.let(onSet)
        },
        label = { Text(label) },
        modifier = Modifier.fillMaxWidth(),
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
    )
}
