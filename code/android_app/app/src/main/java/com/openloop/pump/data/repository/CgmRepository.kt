package com.openloop.pump.data.repository

import android.content.BroadcastReceiver
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
 *  extras: estimated_glucose_mgdl (double), trend (string), timestamp (long)
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
                val mgdl = intent.getDoubleExtra("estimated_glucose_mgdl", 0.0).toInt()
                val trendStr = intent.getStringExtra("trend") ?: "Flat"
                val ts = intent.getLongExtra("timestamp", System.currentTimeMillis())
                if (mgdl <= 0) return
                _glucose.value = GlucoseReading(
                    mgdl = mgdl,
                    trend = GlucoseReading.Trend.fromString(trendStr),
                    timestamp = ts
                )
            }
        }
        ContextCompat.registerReceiver(
            context, receiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED
        )
    }

    fun unregister() {
        runCatching { context.unregisterReceiver(receiver) }
        receiver = null
    }
}
