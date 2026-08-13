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
                // 容错：xDrip 发 double，但部分发送方/测试用 float；getDoubleExtra 对 float 额外值会落默认，
                // 故先取 double，<=0 再回退 float，保证两种类型都能解析出血糖。
                val d = intent.getDoubleExtra("estimated_glucose_mgdl", -1.0)
                val mgdl = if (d > 0) d.toInt()
                else intent.getFloatExtra("estimated_glucose_mgdl", 0f).toDouble().toInt()
                val trendStr = intent.getStringExtra("trend") ?: "Flat"
                val ts = intent.getLongExtra("timestamp", System.currentTimeMillis())
                Log.i("CgmRepository", "recv xDrip BgEstimate mgdl=$mgdl trend=$trendStr ts=$ts")
                if (mgdl <= 0) return
                _glucose.value = GlucoseReading(
                    mgdl = mgdl,
                    trend = GlucoseReading.Trend.fromString(trendStr),
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
