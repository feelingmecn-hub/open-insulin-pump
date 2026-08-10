package com.openloop.pump.ui.dashboard

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.openloop.pump.ble.ConnectionState
import com.openloop.pump.domain.model.AlarmCode
import com.openloop.pump.domain.model.GlucoseReading

@Composable
fun DashboardScreen(viewModel: DashboardViewModel = hiltViewModel()) {
    val conn by viewModel.connectionState.collectAsStateWithLifecycle()
    val state by viewModel.pumpState.collectAsStateWithLifecycle()
    val glucose by viewModel.glucose.collectAsStateWithLifecycle()
    val iob by viewModel.iob.collectAsStateWithLifecycle()
    val reservoir by viewModel.reservoir.collectAsStateWithLifecycle()
    val loop by viewModel.closedLoop.collectAsStateWithLifecycle()
    val predicted by viewModel.predictedGlucose.collectAsStateWithLifecycle()

    LaunchedEffect(Unit) {
        if (conn is ConnectionState.Disconnected) viewModel.ensureConnected()
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        // 连接状态条
        val connText = when (conn) {
            ConnectionState.Connected, ConnectionState.Bonded -> "已连接"
            ConnectionState.Scanning -> "扫描中…"
            ConnectionState.Connecting -> "连接中…"
            is ConnectionState.Error -> "错误：${(conn as ConnectionState.Error).reason}"
            else -> "未连接"
        }
        Text(
            text = "泵状态：$connText",
            style = MaterialTheme.typography.labelMedium,
            color = if (conn is ConnectionState.Error) MaterialTheme.colorScheme.error
            else MaterialTheme.colorScheme.primary
        )

        // 报警
        state?.alarm?.let { alarm: AlarmCode ->
            if (alarm != AlarmCode.NONE) {
                Card(colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.errorContainer
                )) {
                    Text(
                        "⚠ ${alarm.label}",
                        modifier = Modifier.padding(12.dp),
                        color = MaterialTheme.colorScheme.onErrorContainer,
                        fontWeight = FontWeight.Bold
                    )
                }
            }
        }

        // 血糖大卡片
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.padding(20.dp)) {
                Text("当前血糖", style = MaterialTheme.typography.labelMedium)
                glucose?.let { g: GlucoseReading ->
                    Text(
                        "${g.mgdl} mg/dL",
                        fontSize = 40.sp,
                        fontWeight = FontWeight.Bold
                    )
                    Text(
                        "%.1f mmol/L · %s".format(g.mmolL, g.trend.name),
                        style = MaterialTheme.typography.bodyMedium
                    )
                } ?: Text("— 等待 CGM", style = MaterialTheme.typography.bodyLarge)

                if (predicted > 0) {
                    Text(
                        "30 分钟预测：$predicted mg/dL",
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.padding(top = 8.dp)
                    )
                }
            }
        }

        // IOB / 闭环
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            MetricCard(
                modifier = Modifier.weight(1f),
                label = "IOB",
                value = "%.1f U".format(iob)
            )
            MetricCard(
                modifier = Modifier.weight(1f),
                label = "闭环",
                value = if (loop) "开启" else "关闭"
            )
        }

        // 药量 / 电池
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            MetricCard(
                modifier = Modifier.weight(1f),
                label = "剩余药量",
                value = "$reservoir U"
            )
            MetricCard(
                modifier = Modifier.weight(1f),
                label = "电池",
                value = "${state?.batteryPct ?: 0}%"
            )
        }

        if (conn is ConnectionState.Disconnected) {
            Button(
                onClick = viewModel::ensureConnected,
                modifier = Modifier.fillMaxWidth()
            ) { Text("连接 / 扫描泵") }
        }
    }
}

@Composable
private fun MetricCard(modifier: Modifier, label: String, value: String) {
    Card(modifier = modifier) {
        Column(
            modifier = Modifier.padding(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(label, style = MaterialTheme.typography.labelSmall)
            Text(value, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
        }
    }
}
