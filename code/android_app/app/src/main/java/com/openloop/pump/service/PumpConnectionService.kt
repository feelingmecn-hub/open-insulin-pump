package com.openloop.pump.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import androidx.work.WorkManager
import com.openloop.pump.R
import com.openloop.pump.data.repository.CgmRepository
import com.openloop.pump.data.repository.PreferencesRepository
import com.openloop.pump.data.repository.PumpRepository
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import javax.inject.Inject

/**
 * 前台服务：保持 BLE 连接、注册 CGM 接收器、调度闭环 Worker。
 *
 * 在前台运行以避免系统回收，同时向用户展示常驻通知。
 */
@AndroidEntryPoint
class PumpConnectionService : Service() {

    @Inject lateinit var pumpRepo: PumpRepository
    @Inject lateinit var cgmRepo: CgmRepository
    @Inject lateinit var prefs: PreferencesRepository

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    override fun onCreate() {
        super.onCreate()
        cgmRepo.register()
        startForeground(NOTIF_ID, buildNotification())
        scheduleLoop(this)
        scope.launch {
            val addr = prefs.pairedAddress.first()
            if (addr != null) pumpRepo.connect(addr) else pumpRepo.startScan()
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_STICKY

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        cgmRepo.unregister()
        scope.cancel()
        super.onDestroy()
    }

    private fun buildNotification(): Notification {
        val channelId = "pump_connection"
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(
                channelId,
                getString(R.string.channel_pump_connection),
                NotificationManager.IMPORTANCE_LOW
            )
            getSystemService(NotificationManager::class.java).createNotificationChannel(ch)
        }
        return NotificationCompat.Builder(this, channelId)
            .setContentTitle("OpenLoop Pump")
            .setContentText("泵连接与闭环运行中")
            .setSmallIcon(android.R.mipmap.sym_def_app_icon)
            .setOngoing(true)
            .build()
    }

    companion object {
        const val NOTIF_ID = 1001
    }
}
