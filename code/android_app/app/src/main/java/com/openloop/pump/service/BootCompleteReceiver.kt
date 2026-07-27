package com.openloop.pump.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import androidx.core.content.ContextCompat

/**
 * 开机 / 应用更新后自启前台连接服务，恢复泵连接与闭环。
 */
class BootCompleteReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_BOOT_COMPLETED,
            Intent.ACTION_MY_PACKAGE_REPLACED -> {
                val i = Intent(context, PumpConnectionService::class.java)
                ContextCompat.startForegroundService(context, i)
            }
        }
    }
}
