package com.openloop.pump.ui.motortest

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
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.openloop.pump.ble.ConnectionState
import com.openloop.pump.ble.PumpProtocolSpec

@Composable
fun MotorTestScreen(
    viewModel: MotorTestViewModel = hiltViewModel()
) {
    val conn by viewModel.connectionState.collectAsStateWithLifecycle()
    val position by viewModel.motorPosition.collectAsStateWithLifecycle()
    val busy by viewModel.busy.collectAsStateWithLifecycle()
    val err by viewModel.lastError.collectAsStateWithLifecycle()

    val connected = conn is ConnectionState.Connected || conn is ConnectionState.Bonded

    var dir by remember { mutableStateOf(PumpProtocolSpec.MANUAL_DIR_FWD) }
    var stepsText by remember { mutableStateOf("200") }
    var speedText by remember { mutableStateOf("1000") }

    var confirmStop by remember { mutableStateOf(false) }

    LaunchedEffect(connected) { if (connected) viewModel.refreshPosition() }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text(
                    when {
                        connected -> "已连接 ✓"
                        conn is ConnectionState.Error -> "错误：${(conn as ConnectionState.Error).reason}"
                        else -> "未连接（请在首页连接泵）"
                    },
                    style = MaterialTheme.typography.bodyMedium
                )
                if (!connected) Text(
                    "未连接时按钮无效。请先在首页连接泵后再测试电机。",
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }

        // ---- 方向 + 参数 + 动作 ----
        CardSection("电机手动控制") {
            Text("方向", style = MaterialTheme.typography.bodyMedium)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = { dir = PumpProtocolSpec.MANUAL_DIR_FWD },
                    modifier = Modifier.weight(1f),
                    colors = if (dir == PumpProtocolSpec.MANUAL_DIR_FWD)
                        ButtonDefaults.buttonColors() else ButtonDefaults.outlinedButtonColors()
                ) { Text("前进 ▶ (推注)") }
                Button(
                    onClick = { dir = PumpProtocolSpec.MANUAL_DIR_REV },
                    modifier = Modifier.weight(1f),
                    colors = if (dir == PumpProtocolSpec.MANUAL_DIR_REV)
                        ButtonDefaults.buttonColors() else ButtonDefaults.outlinedButtonColors()
                ) { Text("后退 ◀ (回退)") }
            }

            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                OutlinedTextField(
                    value = stepsText, onValueChange = { stepsText = it.filter { c -> c.isDigit() } },
                    label = { Text("步数 (0=连续)") }, modifier = Modifier.weight(1f),
                    singleLine = true, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
                )
                OutlinedTextField(
                    value = speedText, onValueChange = { speedText = it.filter { c -> c.isDigit() } },
                    label = { Text("速度 Hz") }, modifier = Modifier.weight(1f),
                    singleLine = true, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
                )
            }
            Text(
                "步数填 0 = 连续点动（按住跑、点停止停）；填具体数值 = 定量走那么多微步。速度建议 500~3000Hz。",
                style = MaterialTheme.typography.bodySmall
            )

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = {
                        val steps = stepsText.toLongOrNull() ?: 0L
                        val speed = speedText.toIntOrNull() ?: 0
                        viewModel.move(dir, steps, speed)
                    },
                    modifier = Modifier.weight(1f),
                    enabled = connected && !busy
                ) { Text(if ((stepsText.toLongOrNull() ?: 0L) == 0L) "连续点动" else "运行(定量)") }
                Button(
                    onClick = { confirmStop = true },
                    modifier = Modifier.weight(1f),
                    enabled = connected,
                    colors = ButtonDefaults.buttonColors(containerColor = Color(0xFFD83A3A))
                ) { Text("停止") }
            }
        }

        // ---- 位置读取 ----
        CardSection("电机位置") {
            Text("当前电机位置：$position 微步", style = MaterialTheme.typography.bodyMedium)
            Button(
                onClick = { viewModel.refreshPosition() },
                modifier = Modifier.fillMaxWidth(),
                enabled = connected
            ) { Text("读取位置") }
        }

        err?.let {
            Text("⚠ $it", style = MaterialTheme.typography.bodySmall, color = Color(0xFFD83A3A))
        }

        Text(
            "⚠ 本页为电机/丝杠调试专用，直接驱动步进电机，不计入药量/IOB。" +
                "请仅在空载（空注射器+水）下测试，勿对带药/人体使用。",
            style = MaterialTheme.typography.bodySmall
        )
    }

    if (confirmStop) {
        AlertDialog(
            onDismissRequest = { confirmStop = false },
            title = { Text("停止电机") },
            text = { Text("将发送停止指令，终止正在进行的连续点动。") },
            confirmButton = {
                TextButton(onClick = { viewModel.stop(); confirmStop = false }) { Text("停止") }
            },
            dismissButton = {
                TextButton(onClick = { confirmStop = false }) { Text("取消") }
            }
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
