package com.openloop.pump

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import androidx.core.content.ContextCompat
import com.openloop.pump.service.PumpConnectionService
import com.openloop.pump.ui.navigation.AppNavHost
import com.openloop.pump.ui.theme.OpenLoopTheme
import dagger.hilt.android.AndroidEntryPoint

@AndroidEntryPoint
class MainActivity : ComponentActivity() {
    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { /* 权限缺失时 BLE 扫描会静默失败；用户可在系统设置中补授权后重开 App */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestRuntimePermissions()
        startPumpConnectionServiceIfNeeded()
        setContent {
            OpenLoopTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    AppNavHost()
                }
            }
        }
    }

    /** 启动即引导所有运行时必需权限（蓝牙 + 通知；Android 11 及以下兼容位置）。
     *  电池优化/自启动不在启动弹窗（避免骚扰），引导去权限中心手动处理。 */
    private fun requestRuntimePermissions() {
        val needed = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            needed += Manifest.permission.BLUETOOTH_SCAN
            needed += Manifest.permission.BLUETOOTH_CONNECT
        } else {
            needed += Manifest.permission.ACCESS_FINE_LOCATION
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            needed += Manifest.permission.POST_NOTIFICATIONS
        }
        val missing = needed.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) permLauncher.launch(missing.toTypedArray())
    }

    /**
     * 打开 App 即确保前台连接服务在跑（连泵 + CGM 回传）。
     * 服务已由 BootCompleteReceiver 在开机/重装时拉起；此处补覆盖"用户手动打开 App"的场景，
     * 且对已在运行的服务幂等（系统复用同一 Service 实例，onStartCommand 再次调用无副作用）。
     */
    private fun startPumpConnectionServiceIfNeeded() {
        val intent = Intent(this, PumpConnectionService::class.java)
        ContextCompat.startForegroundService(this, intent)
    }
}
