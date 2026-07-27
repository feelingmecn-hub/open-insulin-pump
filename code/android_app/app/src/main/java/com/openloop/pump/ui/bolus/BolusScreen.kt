package com.openloop.pump.ui.bolus

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
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.compose.ui.text.input.KeyboardOptions

@Composable
fun BolusScreen(viewModel: BolusViewModel = hiltViewModel()) {
    val units by viewModel.units.collectAsStateWithLifecycle()
    val recommended by viewModel.recommendedBolus.collectAsStateWithLifecycle()
    val isExtended by viewModel.isExtended.collectAsStateWithLifecycle()
    val duration by viewModel.durationMin.collectAsStateWithLifecycle()
    val sending by viewModel.sending.collectAsStateWithLifecycle()
    val result by viewModel.lastResult.collectAsStateWithLifecycle()
    var noteText by remember { mutableStateOf("") }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Text("大剂量推注", style = MaterialTheme.typography.titleLarge)

        // 剂量显示 + 步进
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(
                modifier = Modifier.padding(20.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Text("%.2f".format(units), fontSize = 48.sp, fontWeight = FontWeight.Bold)
                Text("U (步进 0.05U)", style = MaterialTheme.typography.labelMedium)
                Row(
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    modifier = Modifier.padding(top = 12.dp)
                ) {
                    Button(onClick = { viewModel.stepUnits(-0.05) }) { Text("−0.05") }
                    Button(onClick = { viewModel.stepUnits(0.05) }) { Text("+0.05") }
                }
                Row(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    modifier = Modifier.padding(top = 8.dp)
                ) {
                    listOf(0.5, 1.0, 2.0, 5.0).forEach { q ->
                        Button(onClick = { viewModel.setUnits(q) }) { Text("$q") }
                    }
                }
            }
        }

        // 推荐剂量
        if (recommended > 0) {
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text("推荐校正剂量：%.2f U".format(recommended))
                    Button(onClick = viewModel::applyRecommendation) { Text("采用推荐") }
                }
            }
        }
        Button(onClick = viewModel::computeRecommendation) { Text("根据血糖计算推荐") }

        // 方波大剂量
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Text("方波大剂量（延长推注）")
            Switch(checked = isExtended, onCheckedChange = { viewModel.toggleExtended() })
        }
        if (isExtended) {
            Text("延长时间：$duration 分钟")
            androidx.compose.material3.Slider(
                value = duration.toFloat(),
                onValueChange = { viewModel.setDuration(it.toInt()) },
                valueRange = 0f..600f,
                steps = 119
            )
        }

        // 备注
        OutlinedTextField(
            value = noteText,
            onValueChange = { noteText = it; viewModel.setNote(it) },
            label = { Text("备注（餐前 / 校正）") },
            modifier = Modifier.fillMaxWidth(),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Text)
        )

        // 确认
        Button(
            onClick = viewModel::submit,
            enabled = units > 0 && !sending,
            modifier = Modifier.fillMaxWidth()
        ) {
            Text(if (sending) "发送中…" else "确认推注 %.2f U".format(units))
        }

        result?.let {
            val msg = when (it) {
                BolusViewModel.BolusOutcome.Success -> "✓ 推注已发送"
                is BolusViewModel.BolusOutcome.Failure -> "✗ ${it.msg}"
            }
            Text(
                msg,
                color = if (it is BolusViewModel.BolusOutcome.Success)
                    MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error
            )
        }
    }
}
