package com.openloop.pump.data.repository

import android.content.BroadcastReceiver
import android.util.Log
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import androidx.core.content.ContextCompat
import com.openloop.pump.domain.model.GlucoseReading
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import javax.inject.Inject
import javax.inject.Singleton

/**
 * CGM 数据源 —— 通过 xDrip+ 本地广播接收实时血糖。
 *
 * xDrip 广播 Action: "com.eveningoutpost.dexdrip.BgEstimate"
 *  extras(真实命名空间 key，已用真机 dump 确认):
 *    com.eveningoutpost.dexdrip.Extras.BgEstimate (Double, mg/dL)
 *    com.eveningoutpost.dexdrip.Extras.Time       (Long, ms)
 *    com.eveningoutpost.dexdrip.Extras.BgSlopeName(String: DoubleUp/SingleUp/Flat/SingleDown/DoubleDown)
 *  （同时兼容旧 key estimated_glucose_mgdl/trend/timestamp 兜底，防其它发送方）
 */
@Singleton
class CgmRepository @Inject constructor(
    @ApplicationContext private val context: Context
) {
    private val _glucose = MutableStateFlow<GlucoseReading?>(null)
    val glucose: StateFlow<GlucoseReading?> = _glucose.asStateFlow()

    private var receiver: BroadcastReceiver? = null

    private val filter = IntentFilter("com.eveningoutpost.dexdrip.BgEstimate").apply {
        addAction("com.eveningoutpost.dexdrip.BgEstimate")
    }

    fun register() {
        if (receiver != null) return
        receiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context?, intent: Intent?) {
                if (intent?.action != "com.eveningoutpost.dexdrip.BgEstimate") return
                // 真实 xDrip 广播使用带命名空间前缀的 extra key（已真机 dump 确认）；
                // 旧文档/部分发送方的 estimated_glucose_mgdl/trend/timestamp 作为兜底保留。
                val X = "com.eveningoutpost.dexdrip.Extras"
                val bgD = intent.getDoubleExtra("$X.BgEstimate", -1.0)
                val mgdl = if (bgD > 0) bgD.toInt()
                else intent.getDoubleExtra("estimated_glucose_mgdl", 0.0).toInt()
                val ts = intent.getLongExtra("$X.Time", 0L).let { t ->
                    if (t > 0) t else intent.getLongExtra("timestamp", System.currentTimeMillis())
                }
                val slope = intent.getStringExtra("$X.BgSlopeName")
                    ?: intent.getStringExtra("trend") ?: "Flat"
                Log.i("CgmRepository", "recv xDrip mgdl=$mgdl trend=$slope ts=$ts")
                if (mgdl <= 0) return
                _glucose.value = GlucoseReading(
                    mgdl = mgdl,
                    trend = GlucoseReading.Trend.fromString(slope),
                    timestamp = ts
                )
            }
        }
        // xDrip+ 是第三方 App（不同 UID），其 BgEstimate 广播必须用能收外部广播的
        // RECEIVER_EXPORTED 注册，否则收不到（NOT_EXPORTED 仅收同 UID/系统广播）。
        // 该广播本就公开（AAPS 也据此收血糖），且 CGM 仅用于泵屏显示，无安全/计费影响。
        ContextCompat.registerReceiver(
            context, receiver, filter, ContextCompat.RECEIVER_EXPORTED
        )
    }

    fun unregister() {
        runCatching { context.unregisterReceiver(receiver) }
        receiver = null
    }
}
