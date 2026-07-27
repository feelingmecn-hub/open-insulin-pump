package com.openloop.pump.ui.history

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

private val fmt = DateTimeFormatter.ofPattern("MM-dd HH:mm")
private fun ts(ms: Long): String =
    Instant.ofEpochMilli(ms).atZone(ZoneId.systemDefault()).format(fmt)

@Composable
fun HistoryScreen(viewModel: HistoryViewModel = hiltViewModel()) {
    val treatments by viewModel.treatments.collectAsStateWithLifecycle()
    val glucose by viewModel.glucose.collectAsStateWithLifecycle()
    val syncing by viewModel.syncing.collectAsStateWithLifecycle()
    var tab by remember { mutableIntStateOf(0) }

    Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
        Text("历史记录", style = MaterialTheme.typography.titleLarge)

        TabRow(selectedTabIndex = tab) {
            Tab(selected = tab == 0, onClick = { tab = 0 }) { Text("治疗") }
            Tab(selected = tab == 1, onClick = { tab = 1 }) { Text("血糖") }
        }

        if (tab == 0) {
            LazyColumn(modifier = Modifier.weight(1f), verticalArrangement = Arrangement(8.dp)) {
                items(treatments) { t ->
                    Card(modifier = Modifier.fillMaxWidth()) {
                        Column(modifier = Modifier.padding(12.dp)) {
                            Text("${t.type}  %.2f U".format(t.units),
                                style = MaterialTheme.typography.bodyLarge)
                            Text(ts(t.timestamp) + (t.note?.let { " · $it" } ?: ""),
                                style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            }
        } else {
            LazyColumn(modifier = Modifier.weight(1f), verticalArrangement = Arrangement(8.dp)) {
                items(glucose) { g ->
                    Card(modifier = Modifier.fillMaxWidth()) {
                        Text("${g.mgdl} mg/dL  ·  ${ts(g.timestamp)}",
                            modifier = Modifier.padding(12.dp))
                    }
                }
            }
        }

        Button(
            onClick = viewModel::syncToNightscout,
            enabled = !syncing,
            modifier = Modifier.fillMaxWidth()
        ) { Text(if (syncing) "同步中…" else "同步到 Nightscout") }
    }
}
