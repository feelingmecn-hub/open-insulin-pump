package com.openloop.pump.util

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import android.provider.Settings
import androidx.core.content.ContextCompat

/** 权限分类。 */
enum class PermGroup { RUNTIME, SETTINGS, OPTIONAL }

/**
 * 单项权限状态。
 * @param androidPermission 实际 Android 权限字符串；SETTINGS 类为 null（非标准权限，靠跳转设置）。
 * @param granted 是否已满足（运行时权限看是否授权；电池看是否豁免；自启动无法检测恒为 false）。
 * @param needsSettings 是否需要跳系统设置（SETTINGS 类恒为 true，无弹窗 API）。
 * @param applicable 当前系统版本是否适用（如 S+ 才需 BLUETOOTH_SCAN）。
 * @param detectable 程序能否检测到满足状态；自启动等厂商相关项无法检测，恒 false（不计入缺失红条）。
 */
data class PermissionItem(
    val key: String,
    val label: String,
    val desc: String,
    val group: PermGroup,
    val androidPermission: String?,
    val granted: Boolean,
    val needsSettings: Boolean,
    val applicable: Boolean,
    val detectable: Boolean
)

/** 权限状态检测与跳设置 Intent 构造（纯函数 object，无依赖）。 */
object PermissionStatus {

    fun isBatteryOptimizationIgnored(context: Context): Boolean {
        val pm = context.getSystemService(Context.POWER_SERVICE) as PowerManager
        return pm.isIgnoringBatteryOptimizations(context.packageName)
    }

    /** 电池优化豁免：直达本 App 豁免页；ROM 不支持则退回总开关页。 */
    fun batteryExemptionIntent(context: Context): Intent {
        val direct = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
            data = Uri.parse("package:${context.packageName}")
        }
        return if (direct.resolveActivity(context.packageManager) != null) direct
        else Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)
    }

    /** 应用详情页：用于永久拒绝后手动补权限，以及自启动/后台管理兜底入口。 */
    fun appDetailsIntent(context: Context): Intent =
        Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
            data = Uri.parse("package:${context.packageName}")
        }

    /** 自启动 / 后台管理：无标准 API，统一兜底到应用详情页引导手动开启。 */
    fun autostartIntent(context: Context): Intent = appDetailsIntent(context)

    /** 当前系统版本下该权限是否适用。 */
    private fun applicableFor(perm: String): Boolean = when (perm) {
        Manifest.permission.BLUETOOTH_SCAN,
        Manifest.permission.BLUETOOTH_CONNECT -> Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
        Manifest.permission.POST_NOTIFICATIONS -> Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
        Manifest.permission.ACCESS_FINE_LOCATION -> Build.VERSION.SDK_INT < Build.VERSION_CODES.S
        else -> true
    }

    private fun granted(context: Context, perm: String?): Boolean =
        perm?.let {
            ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
        } ?: false

    /** 收集全部权限项（含不适用版本，UI 会标注「本机无需」）。 */
    fun collect(context: Context): List<PermissionItem> {
        val items = mutableListOf<PermissionItem>()

        // A 组：运行时权限（必需，可弹窗）
        val runtime = listOf(
            Manifest.permission.BLUETOOTH_CONNECT to "蓝牙连接",
            Manifest.permission.BLUETOOTH_SCAN to "蓝牙扫描",
            Manifest.permission.POST_NOTIFICATIONS to "通知"
        )
        runtime.forEach { (perm, label) ->
            val app = applicableFor(perm)
            items.add(
                PermissionItem(
                    key = perm,
                    label = label,
                    desc = when (perm) {
                        Manifest.permission.BLUETOOTH_CONNECT -> "连接胰岛素泵必需"
                        Manifest.permission.BLUETOOTH_SCAN -> "扫描发现泵必需"
                        else -> "报警与状态变更通知"
                    },
                    group = PermGroup.RUNTIME,
                    androidPermission = perm,
                    granted = granted(context, perm),
                    needsSettings = false,
                    applicable = app,
                    detectable = true
                )
            )
        }

        // B 组：系统设置类（必需，无弹窗 API）
        items.add(
            PermissionItem(
                key = "battery",
                label = "电池优化豁免",
                desc = "允许后台 BLE 长连，否则系统杀进程导致闭环断连",
                group = PermGroup.SETTINGS,
                androidPermission = null,
                granted = isBatteryOptimizationIgnored(context),
                needsSettings = true,
                applicable = true,
                detectable = true
            )
        )
        items.add(
            PermissionItem(
                key = "autostart",
                label = "自启动 / 后台管理",
                desc = "开机与后台保活闭环（厂商相关，无系统弹窗，需手动开）",
                group = PermGroup.SETTINGS,
                androidPermission = null,
                granted = false,
                needsSettings = true,
                applicable = true,
                detectable = false
            )
        )

        // C 组：可选（旧系统位置）
        items.add(
            PermissionItem(
                key = Manifest.permission.ACCESS_FINE_LOCATION,
                label = "位置（旧系统 BLE 扫描）",
                desc = "Android 11 及以下用于 BLE 扫描；Android 12+ 已免位置，可不授权",
                group = PermGroup.OPTIONAL,
                androidPermission = Manifest.permission.ACCESS_FINE_LOCATION,
                granted = granted(context, Manifest.permission.ACCESS_FINE_LOCATION),
                needsSettings = false,
                applicable = applicableFor(Manifest.permission.ACCESS_FINE_LOCATION),
                detectable = true
            )
        )
        return items
    }

    /** 当前缺失的必需权限数量（仅计可检测且适用的必需的：蓝牙/通知/电池优化）。 */
    fun missingRequiredCount(context: Context): Int =
        collect(context).count { it.applicable && it.detectable && !it.granted }
}
