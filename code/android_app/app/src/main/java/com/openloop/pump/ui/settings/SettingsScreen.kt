package com.openloop.pump.ui.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
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
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle

@Composable
fun SettingsScreen(viewModel: SettingsViewModel = hiltViewModel()) {
    val loop by viewModel.closedLoop.collectAsStateWithLifecycle()
    val isf by viewModel.isf.collectAsStateWithLifecycle()
    val target by viewModel.target.collectAsStateWithLifecycle()
    val maxIob by viewModel.maxIob.collectAsStateWithLifecycle()
    val maxBasal by viewModel.maxBasal.collectAsStateWithLifecycle()
    val carbRatio by viewModel.carbRatio.collectAsStateWithLifecycle()
    val nsUrl by viewModel.nsUrl.collectAsStateWithLifecycle()
    val passkey by viewModel.blePasskey.collectAsStateWithLifecycle()

    var urlDraft by remember { mutableStateOf(nsUrl) }
    var secretDraft by remember { mutableStateOf("") }
    var passkeyDraft by remember { mutableStateOf(passkey) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        // ---- 闭环参数 ----
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text("闭环参数", style = MaterialTheme.typography.titleMedium)
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text("自动闭环（调整基础率）")
                    Switch(checked = loop, onCheckedChange = viewModel::setLoop)
                }
                NumberField("胰岛素敏感系数 ISF (mg/dL/U)", isf) { viewModel.setIsp2(it) }
                NumberField("目标血糖 (mg/dL)", target.toDouble()) { viewModel.setTarget(it.toInt()) }
                NumberField("最大 IOB (U)", maxIob) { viewModel.setMaxIob(it) }
                NumberField("最大基础率 (U/h)", maxBasal) { viewModel.setMaxBasal(it) }
                NumberField("碳水比 (g/U)", carbRatio) { viewModel.setCarbRatio(it) }
            }
        }

        // ---- Nightscout ----
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text("Nightscout 云端", style = MaterialTheme.typography.titleMedium)
                OutlinedTextField(
                    value = urlDraft,
                    onValueChange = { urlDraft = it },
                    label = { Text("URL") },
                    modifier = Modifier.fillMaxWidth()
                )
                OutlinedTextField(
                    value = secretDraft,
                    onValueChange = { secretDraft = it },
                    label = { Text("API Secret") },
                    modifier = Modifier.fillMaxWidth()
                )
                Button(
                    onClick = { viewModel.setNightscout(urlDraft, secretDraft) },
                    modifier = Modifier.fillMaxWidth()
                ) { Text("保存 Nightscout") }
            }
        }

        // ---- BLE 配对 ----
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text("BLE 配对", style = MaterialTheme.typography.titleMedium)
                Text(
                    "若泵固件配置了非零 passkey，请在此填入（否则留空 = Just Works）。",
                    style = MaterialTheme.typography.bodySmall
                )
                OutlinedTextField(
                    value = passkeyDraft,
                    onValueChange = { passkeyDraft = it },
                    label = { Text("Passkey (6 位数字)") },
                    modifier = Modifier.fillMaxWidth(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
                )
                Button(
                    onClick = { viewModel.setPasskey(passkeyDraft) },
                    modifier = Modifier.fillMaxWidth()
                ) { Text("保存 Passkey") }
            }
        }

        HorizontalDivider()
        Text(
            "⚠ 本应用为 DIY 开源项目，非医疗器械。使用风险自负。",
            style = MaterialTheme.typography.bodySmall
        )
    }
}

@Composable
private fun NumberField(label: String, value: Double, onSet: (Double) -> Unit) {
    var text by remember { mutableStateOf(value.toString()) }
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
