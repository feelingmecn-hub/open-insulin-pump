package com.openloop.pump.data.repository

import com.google.gson.Gson
import com.openloop.pump.data.nightscout.NightscoutApi
import com.openloop.pump.data.nightscout.model.NightscoutEntry
import com.openloop.pump.data.nightscout.model.NightscoutTreatment
import com.openloop.pump.domain.model.GlucoseReading
import kotlinx.coroutines.flow.first
import okhttp3.Interceptor
import okhttp3.OkHttpClient
import okhttp3.logging.HttpLoggingInterceptor
import retrofit2.Response
import retrofit2.Retrofit
import retrofit2.converter.gson.GsonConverterFactory
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Nightscout 同步仓库 —— 将治疗 / 血糖记录上传到云端，并拉取最近血糖用于无 xDrip 场景。
 *
 * Retrofit 实例在运行时根据 DataStore 中的 URL / API-Secret 构建并缓存。
 */
@Singleton
class NightscoutRepository @Inject constructor(
    private val prefs: PreferencesRepository
) {
    private var cachedApi: NightscoutApi? = null
    private var cachedBase: String? = null

    private suspend fun api(): NightscoutApi? {
        val url = prefs.nightscoutUrl.first() ?: return null
        val secret = prefs.nightscoutSecret.first() ?: ""
        if (url != cachedBase || cachedApi == null) {
            cachedBase = url
            cachedApi = buildRetrofit(url, secret)
        }
        return cachedApi
    }

    private fun buildRetrofit(baseUrl: String, secret: String): NightscoutApi {
        val client = OkHttpClient.Builder()
            .addInterceptor { chain: Interceptor.Chain ->
                val req = chain.request().newBuilder()
                    .addHeader("API-SECRET", secret)
                    .build()
                chain.proceed(req)
            }
            .addInterceptor(
                HttpLoggingInterceptor().apply {
                    level = HttpLoggingInterceptor.Level.BASIC
                }
            )
            .build()
        return Retrofit.Builder()
            .baseUrl(if (baseUrl.endsWith("/")) baseUrl else "$baseUrl/")
            .client(client)
            .addConverterFactory(GsonConverterFactory.create(Gson()))
            .build()
            .create(NightscoutApi::class.java)
    }

    /** 上传大剂量。 */
    suspend fun uploadBolus(units: Double, note: String?, timestamp: Long): Result<Unit> {
        val a = api() ?: return noConfig()
        val body = listOf(
            NightscoutTreatment(
                eventType = "Bolus",
                createdAt = iso(timestamp),
                insulin = units,
                notes = note
            )
        )
        return runCatching { a.postTreatments(body).requireSuccess() }
    }

    /** 上传临时基础率。 */
    suspend fun uploadTempBasal(
        rateUh: Double, durationMin: Int, timestamp: Long
    ): Result<Unit> {
        val a = api() ?: return noConfig()
        val body = listOf(
            NightscoutTreatment(
                eventType = "Temp Basal",
                createdAt = iso(timestamp),
                absolute = rateUh,
                duration = durationMin
            )
        )
        return runCatching { a.postTreatments(body).requireSuccess() }
    }

    /** 上传血糖读数。 */
    suspend fun uploadGlucose(reading: GlucoseReading): Result<Unit> {
        val a = api() ?: return noConfig()
        val body = listOf(
            NightscoutEntry(
                sgv = reading.mgdl,
                date = reading.timestamp,
                direction = reading.trend.name
            )
        )
        return runCatching { a.postEntries(body).requireSuccess() }
    }

    /**
     * 拉取最近一条血糖（无本地 CGM / AAPS 广播收不到时的备用源）。
     * 闭环中 AAPS 必把血糖上传 Nightscout，故伴生 App 可直接轮询 NS 取最新 SGV，
     * 不受 xDrip 缺失 / AAPS 状态广播权限保护的限制。direction 解析为趋势箭头。
     */
    suspend fun fetchLatestGlucose(): GlucoseReading? {
        val a = api() ?: return null
        return runCatching {
            a.getEntries(1).firstOrNull()?.let {
                GlucoseReading(
                    mgdl = it.sgv,
                    trend = GlucoseReading.Trend.fromString(it.direction),
                    timestamp = it.date
                )
            }
        }.getOrNull()
    }

    private fun noConfig(): Result<Unit> =
        Result.failure(IllegalStateException("Nightscout 未配置"))

    private fun <T> Response<T>.requireSuccess() {
        if (!isSuccessful) throw IllegalStateException("Nightscout HTTP ${code()}")
    }

    private fun iso(ts: Long): String = Instant.ofEpochMilli(ts).toString()
}
