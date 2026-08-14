package com.openloop.pump.data.repository

import android.content.BroadcastReceiver
import android.util.Log
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import androidx.core.content.ContextCompat
import com.openloop.pump.domain.model.GlucoseReading
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import javax.inject.Inject
import javax.inject.Singleton

/**
 * CGM 数据源 —— 三路取任一最新读数：
 *  1) xDrip+ 广播  Action: "com.eveningoutpost.dexdrip.BgEstimate"
 *     extras(真实命名空间 key，已用真机 dump 确认):
 *       com.eveningoutpost.dexdrip.Extras.BgEstimate (Double/Float, mg/dL)
 *       com.eveningoutpost.dexdrip.Extras.Time       (Long, ms)
 *       com.eveningoutpost.dexdrip.Extras.BgSlopeName(String: DoubleUp/SingleUp/Flat/SingleDown/DoubleDown)
 *     （同时兼容旧 key estimated_glucose_mgdl/trend/timestamp 兜底，防其它发送方）
 *  2) AAPS 状态广播 Action: "info.nightscout.androidaps.status"（Tizen/Smartwatch 同步插件发出）
 *     extras: glucoseMgdl(Double, 单位见 units) / units("mg/dl"|"mmol") / slopeArrow(String) / glucoseTimeStamp(Long ms)
 *     闭环中 AAPS 必然持有最新血糖，但其广播带权限保护，第三方 App（含本 App）常收不到。
 *  3) Nightscout 轮询：闭环中 AAPS 必把血糖上传 NS，本 App 定时拉取最近 SGV，
 *     作为无 xDrip / AAPS 广播收不到时的稳定备用源（权限自由、HTTP 可达）。
 * 两路广播用 RECEIVER_EXPORTED 注册（第三方 App 广播，公开且无安全/计费影响）。
 */
@Singleton
class CgmRepository @Inject constructor(
    @ApplicationContext private val context: Context,
    private val nsRepo: NightscoutRepository
) {
    private val _glucose = MutableStateFlow<GlucoseReading?>(null)
    val glucose: StateFlow<GlucoseReading?> = _glucose.asStateFlow()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    /** Nightscout 轮询间隔：CGM 约 5min 一跳，60s 轮询可在新值到达后 ~1min 内推给泵。 */
    private val NS_POLL_MS = 60_000L

    private var receiver: BroadcastReceiver? = null

    /** Nightscout 轮询 Job（register 启动 / unregister 取消）。 */
    private var nsPollJob: Job? = null

    /** AAPS 状态广播（Tizen/Smartwatch 同步插件发出）：含 glucoseMgdl / slopeArrow / glucoseTimeStamp。 */
    private val AAPS_STATUS_ACTION = "info.nightscout.androidaps.status"

    private val filter = IntentFilter("com.eveningoutpost.dexdrip.BgEstimate").apply {
        addAction("com.eveningoutpost.dexdrip.BgEstimate")
        addAction(AAPS_STATUS_ACTION)
    }

    fun register() {
        if (receiver != null) return
        receiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context?, intent: Intent?) {
                when (intent?.action) {
                    "com.eveningoutpost.dexdrip.BgEstimate" -> handleXDrip(intent)
                    AAPS_STATUS_ACTION -> handleAapsStatus(intent)
                    else -> return
                }
            }
        }
        // xDrip+ 是第三方 App（不同 UID），其 BgEstimate 广播必须用能收外部广播的
        // RECEIVER_EXPORTED 注册，否则收不到（NOT_EXPORTED 仅收同 UID/系统广播）。
        // 该广播本就公开（AAPS 也据此收血糖），且 CGM 仅用于泵屏显示，无安全/计费影响。
        ContextCompat.registerReceiver(
            context, receiver, filter, ContextCompat.RECEIVER_EXPORTED
        )
        startNsPolling()
    }

    /** 启动 Nightscout 轮询：定时拉取最近 SGV，作为广播收不到时的备用 CGM 源。 */
    private fun startNsPolling() {
        if (nsPollJob?.isActive == true) return
        nsPollJob = scope.launch {
            while (isActive) {
                delay(NS_POLL_MS)
                runCatching { nsRepo.fetchLatestGlucose() }
                    .getOrNull()
                    ?.let { g ->
                        val cur = _glucose.value
                        // 仅当 NS 读数更新（时间戳更新）才刷新，避免重复推送同一帧
                        if (cur == null || g.timestamp > cur.timestamp) {
                            Log.i("CgmRepository", "NS poll -> mgdl=${g.mgdl} trend=${g.trend} ts=${g.timestamp}")
                            _glucose.value = g
                        } else {
                            Log.d("CgmRepository", "NS poll: 无更新 (ns ts=${g.timestamp} <= cur ${cur?.timestamp})")
                        }
                    }
            }
        }
    }

    /**
     * xDrip 本地广播（com.eveningoutpost.dexdrip.BgEstimate）。
     * 真实 xDrip 广播使用带命名空间前缀的 extra key（已真机 dump 确认）；
     * 旧文档/部分发送方的 estimated_glucose_mgdl/trend/timestamp 作为兜底保留。
     */
    private fun handleXDrip(intent: Intent) {
        val X = "com.eveningoutpost.dexdrip.Extras"
        val bgD = intent.getDoubleExtra("$X.BgEstimate", 0.0)
            .let { if (it > 0) it else intent.getFloatExtra("$X.BgEstimate", 0f).toDouble() }
        val mgdl = if (bgD > 0) bgD.toInt()
        else intent.getDoubleExtra("estimated_glucose_mgdl", 0.0).toInt()
        val ts = intent.getLongExtra("$X.Time", 0L).let { t ->
            if (t > 0) t else intent.getLongExtra("timestamp", System.currentTimeMillis())
        }
        val slope = intent.getStringExtra("$X.BgSlopeName")
            ?: intent.getStringExtra("trend") ?: "Flat"
        Log.i("CgmRepository", "recv xDrip mgdl=$mgdl trend=$slope ts=$ts")
        if (mgdl <= 0) return
        post(mgdl, slope, ts)
    }

    /**
     * AAPS 状态广播（info.nightscout.androidaps.status，由 AAPS 的 Tizen/Smartwatch 同步插件发出）。
     * 闭环中 AAPS 必然持有最新血糖，故以此作为第二数据源，免去对 xDrip 的硬性依赖。
     * extras: glucoseMgdl(Double) / units("mg/dl"|"mmol") / slopeArrow(String) / glucoseTimeStamp(Long ms)
     * 注意：AAPS 把 recalculated 值塞在 glucoseMgdl 下，单位由 units 决定——mmol 需换算回 mg/dL。
     */
    private fun handleAapsStatus(intent: Intent) {
        // AAPS 用 putDouble 写入；个别发送方/测试可能用 float，故 double 优先、float 兜底。
        val raw = intent.getDoubleExtra("glucoseMgdl", 0.0)
            .let { if (it > 0) it else intent.getFloatExtra("glucoseMgdl", 0f).toDouble() }
        if (raw <= 0) return
        val units = intent.getStringExtra("units")
        val mgdl = if (units == "mmol") (raw * 18.0182).toInt() else raw.toInt()
        val slope = intent.getStringExtra("slopeArrow") ?: "Flat"
        val ts = intent.getLongExtra("glucoseTimeStamp", 0L).let { t ->
            if (t > 0) t else System.currentTimeMillis()
        }
        Log.i("CgmRepository", "recv AAPS status mgdl=$mgdl trend=$slope ts=$ts units=$units")
        if (mgdl <= 0) return
        post(mgdl, slope, ts)
    }

    private fun post(mgdl: Int, slope: String, ts: Long) {
        _glucose.value = GlucoseReading(
            mgdl = mgdl,
            trend = GlucoseReading.Trend.fromString(slope),
            timestamp = ts
        )
    }

    fun unregister() {
        runCatching { context.unregisterReceiver(receiver) }
        receiver = null
        nsPollJob?.cancel()
        nsPollJob = null
    }
}
