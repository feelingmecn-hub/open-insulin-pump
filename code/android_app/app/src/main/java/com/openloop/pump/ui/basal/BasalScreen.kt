package com.openloop.pump.ui.basal

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
fun BasalScreen(viewModel: BasalViewModel = hiltViewModel()) {
    val slots by viewModel.slots.collectAsStateWithLifecycle()
    val daily by viewModel.dailyTotal.collectAsStateWithLifecycle()
    val saving by viewModel.saving.collectAsStateWithLifecycle()
    val testResult by viewModel.testResult.collectAsStateWithLifecycle()
    val saveResult by viewModel.saveResult.collectAsStateWithLifecycle()
    var showTestConfirm by remember { mutableStateOf(false) }

    androidx.compose.foundation.layout.Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
    ) {
        Text("基础率方案", style = MaterialTheme.typography.titleLarge)
        Text("日总量：%.1f U".format(daily), style = MaterialTheme.typography.bodyMedium)

        LazyColumn(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            items(slots) { slot ->
                Card(modifier = Modifier.fillMaxWidth()) {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.SpaceBetween
                    ) {
                        Text("%02d:00 – %02d:00".format(slot.hour, slot.hour + 1))
                        OutlinedTextField(
                            value = "%.1f".format(slot.rateUh),
                            onValueChange = { txt ->
                                txt.toDoubleOrNull()?.let { viewModel.updateSlot(slot.hour, it) }
                            },
                            label = { Text("U/h") },
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                            modifier = Modifier.fillMaxWidth(0.4f)
                        )
                    }
                }
            }
        }

        Button(
            onClick = viewModel::applyAllToPump,
            enabled = !saving,
            modifier = Modifier.fillMaxWidth()
        ) {
            Text(if (saving) "写入泵中…" else "应用全部到泵")
        }

        saveResult?.let { msg ->
            Text(msg, style = MaterialTheme.typography.bodyMedium)
        }

        Spacer(modifier = Modifier.height(8.dp))

        OutlinedButton(
            onClick = { showTestConfirm = true },
            enabled = !saving,
            modifier = Modifier.fillMaxWidth()
        ) {
            Text("验证测试：把泵内全天总量打一次")
        }
        Text(
            "测试量取自泵内当前档案的 24 段之和（不是本页未写入的编辑值），" +
                "记为「基础率验证」事件，不计入大剂量与 IOB。可对照电机行程核对是否真的走了量。",
            style = MaterialTheme.typography.bodySmall
        )

        testResult?.let { msg ->
            Text(msg, style = MaterialTheme.typography.bodyMedium)
        }
    }

    if (showTestConfirm) {
        AlertDialog(
            onDismissRequest = { showTestConfirm = false },
            title = { Text("确认执行基础率验证测试？") },
            text = {
                Text(
                    "泵会一次性推注「泵内档案 24 段之和」，本页显示的日总量为 %.2f U。\n\n".format(daily) +
                        "· 实际量以泵内档案为准，若刚改过请先点「应用全部到泵」\n" +
                        "· 会被单次大剂量上限与余量钳制\n" +
                        "· 记录为「基础率验证」，不计入 IOB\n\n" +
                        "⚠️ 本装置为实验原型，严禁用于任何人体。请确认针头朝向废液杯/空推。"
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    showTestConfirm = false
                    viewModel.runBasalTest()
                }) { Text("确认执行") }
            },
            dismissButton = {
                TextButton(onClick = { showTestConfirm = false }) { Text("取消") }
            }
        )
    }
}
