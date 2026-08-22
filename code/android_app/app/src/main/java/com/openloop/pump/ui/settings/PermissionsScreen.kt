package com.openloop.pump.ui.settings

import android.app.Activity
import android.content.Context
import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.openloop.pump.util.PermGroup
import com.openloop.pump.util.PermissionItem
import com.openloop.pump.util.PermissionStatus

@Composable
fun PermissionsScreen() {
    val context = LocalContext.current
    val activity = context as? Activity

    var items by remember { mutableStateOf(PermissionStatus.collect(context)) }

    val launcher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { items = PermissionStatus.collect(context) }

    // 从系统设置页返回后刷新状态（电池豁免/手动授权点完 back 即刷新）。
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        val obs = LifecycleEventObserver { _, e ->
            if (e == Lifecycle.Event.ON_RESUME) items = PermissionStatus.collect(context)
        }
        lifecycleOwner.lifecycle.addObserver(obs)
        onDispose { lifecycleOwner.lifecycle.removeObserver(obs) }
    }

    val missing = PermissionStatus.missingRequiredCount(context)

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text("权限中心", style = MaterialTheme.typography.titleLarge)
        Text(
            "统一管理蓝牙 / 通知 / 电池优化 / 自启动权限。闭环联调前请确保必需项全部就绪。",
            style = MaterialTheme.typography.bodySmall
        )

        // 顶部状态条
        Card(
            modifier = Modifier.fillMaxWidth(),
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                if (missing > 0) {
                    Text(
                        "⚠ 有 $missing 项必需权限未授予（蓝牙 / 通知 / 电池优化），闭环可能断连。",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.error
                    )
                } else {
                    Text(
                        "✓ 必需权限已全部就绪",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.primary
                    )
                }
            }
        }

        // A 组：运行时权限
        PermissionGroup(
            title = "运行时权限（必需）",
            list = items.filter { it.group == PermGroup.RUNTIME && it.applicable },
            context = context,
            activity = activity,
            onRequest = { p -> launcher.launch(arrayOf(p)) },
            onSettings = { i -> context.startActivity(i) }
        )

        // B 组：系统设置类
        PermissionGroup(
            title = "系统设置（必需，需手动开启）",
            list = items.filter { it.group == PermGroup.SETTINGS && it.applicable },
            context = context,
            activity = activity,
            onRequest = { p -> launcher.launch(arrayOf(p)) },
            onSettings = { i -> context.startActivity(i) }
        )

        // C 组：可选
        PermissionGroup(
            title = "可选",
            list = items.filter { it.group == PermGroup.OPTIONAL && it.applicable },
            context = context,
            activity = activity,
            onRequest = { p -> launcher.launch(arrayOf(p)) },
            onSettings = { i -> context.startActivity(i) }
        )

        // 一键操作
        Button(
            onClick = {
                val toRequest = items
                    .filter { it.applicable && it.group != PermGroup.OPTIONAL && it.androidPermission != null && !it.granted }
                    .mapNotNull { it.androidPermission }
                    .toSet()
                if (toRequest.isNotEmpty()) launcher.launch(toRequest.toTypedArray())
            },
            modifier = Modifier.fillMaxWidth()
        ) { Text("一键申请全部必需权限（蓝牙 / 通知）") }

        Button(
            onClick = { context.startActivity(PermissionStatus.batteryExemptionIntent(context)) },
            modifier = Modifier.fillMaxWidth()
        ) { Text("去豁免电池优化（保活后台连接）") }

        Button(
            onClick = { context.startActivity(PermissionStatus.autostartIntent(context)) },
            modifier = Modifier.fillMaxWidth()
        ) { Text("去设置自启动 / 后台管理") }

        HorizontalDivider()
        Text(
            "⚠ 本应用为 DIY 开源项目，非医疗器械。使用风险自负。",
            style = MaterialTheme.typography.bodySmall
        )
    }
}

@Composable
private fun PermissionGroup(
    title: String,
    list: List<PermissionItem>,
    context: Context,
    activity: Activity?,
    onRequest: (String) -> Unit,
    onSettings: (Intent) -> Unit
) {
    if (list.isEmpty()) return
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            list.forEach { item ->
                PermissionRow(item, context, activity, onRequest, onSettings)
            }
        }
    }
}

@Composable
private fun PermissionRow(
    item: PermissionItem,
    context: Context,
    activity: Activity?,
    onRequest: (String) -> Unit,
    onSettings: (Intent) -> Unit
) {
    val rationale = remember(item, activity) {
        item.androidPermission?.let { p ->
            activity?.shouldShowRequestPermissionRationale(p) ?: false
        } ?: false
    }

    Card(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(item.label, style = MaterialTheme.typography.titleSmall)
                val badge = when {
                    !item.applicable -> "本机无需"
                    item.granted -> "✓ 已就绪"
                    item.needsSettings -> "🔧 需手动设置"
                    rationale -> "⚠ 已拒绝"
                    else -> "⚠ 未授权"
                }
                Text(badge, style = MaterialTheme.typography.bodySmall)
            }
            Text(item.desc, style = MaterialTheme.typography.bodySmall)

            when {
                item.granted ->
                    Text("已就绪", style = MaterialTheme.typography.labelSmall)
                item.needsSettings -> {
                    val intent = if (item.key == "battery")
                        PermissionStatus.batteryExemptionIntent(context)
                    else
                        PermissionStatus.autostartIntent(context)
                    Button(onClick = { onSettings(intent) }, modifier = Modifier.fillMaxWidth()) {
                        Text("去设置")
                    }
                }
                rationale ->
                    Button(
                        onClick = { onSettings(PermissionStatus.appDetailsIntent(context)) },
                        modifier = Modifier.fillMaxWidth()
                    ) { Text("去设置（手动授权）") }
                else ->
                    Button(
                        onClick = { item.androidPermission?.let(onRequest) },
                        modifier = Modifier.fillMaxWidth()
                    ) { Text("去授权") }
            }
        }
    }
}
