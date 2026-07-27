package com.openloop.pump.ui.basal

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
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
                            value = "%.2f".format(slot.rateUh),
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
    }
}
