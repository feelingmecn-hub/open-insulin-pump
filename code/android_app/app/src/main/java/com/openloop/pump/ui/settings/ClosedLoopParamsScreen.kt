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
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
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
import com.openloop.pump.ble.PumpProtocolSpec
import java.util.Locale

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ClosedLoopParamsScreen(
    nav: NavController,
    viewModel: SettingsViewModel = hiltViewModel()
) {
    val kinds = listOf(
        Triple(PumpProtocolSpec.CL_KIND_ISF, "胰岛素敏感系数 ISF", "mg/dL/U"),
        Triple(PumpProtocolSpec.CL_KIND_CARB_RATIO, "碳水比", "g/U"),
        Triple(PumpProtocolSpec.CL_KIND_TARGET_GLU, "目标血糖", "mg/dL")
    )
    var kind by remember { mutableStateOf(PumpProtocolSpec.CL_KIND_ISF) }
    var values by remember { mutableStateOf(List(24) { 0f }) }
    var loading by remember { mutableStateOf(true) }

    val meta = kinds.first { it.first == kind }

    LaunchedEffect(kind) {
        loading = true
        viewModel.loadClKind(kind)?.let { values = it.toList() }
        loading = false
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("泵闭环参数 (逐时)") },
                navigationIcon = {
                    IconButton(onClick = { nav.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "返回")
                    }
                }
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(padding)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            // 分段切换
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                kinds.forEach { (k, label, _) ->
                    Button(
                        onClick = { kind = k },
                        modifier = Modifier.weight(1f),
                        enabled = k != kind
                    ) { Text(label.take(4), style = MaterialTheme.typography.labelSmall) }
                }
            }

            Card(modifier = Modifier.fillMaxWidth()) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(meta.second, style = MaterialTheme.typography.titleMedium)
                    Text(
                        "24 小时逐段设置，修改即时写回泵（${meta.third}）。",
                        style = MaterialTheme.typography.bodySmall
                    )
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        var fillAll by remember { mutableStateOf("") }
                        OutlinedTextField(
                            value = fillAll,
                            onValueChange = { fillAll = it },
                            label = { Text("统一值 (${meta.third})") },
                            modifier = Modifier.weight(1f),
                            singleLine = true,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal)
                        )
                        Button(onClick = {
                            val v = fillAll.toFloatOrNull() ?: return@Button
                            for (h in 0..23) {
                                values = values.toMutableList().also { it[h] = v }
                                viewModel.setClParam(kind, h, v)
                            }
                        }) { Text("全部应用") }
                    }
                    if (loading) Text("加载中…", style = MaterialTheme.typography.bodyMedium)
                    for (h in 0..23) {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text(
                                "%02d:00".format(h),
                                style = MaterialTheme.typography.bodyMedium,
                                modifier = Modifier.weight(0.4f)
                            )
                            OutlinedTextField(
                                value = if (kind == PumpProtocolSpec.CL_KIND_TARGET_GLU)
                                    "%.0f".format(values[h]) else "%.1f".format(values[h]),
                                onValueChange = { txt ->
                                    val v = txt.toFloatOrNull()
                                    if (v != null) {
                                        values = values.toMutableList().also { it[h] = v }
                                        viewModel.setClParam(kind, h, v)
                                    }
                                },
                                modifier = Modifier.weight(1f),
                                singleLine = true,
                                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal)
                            )
                        }
                    }
                }
            }
        }
    }
}
