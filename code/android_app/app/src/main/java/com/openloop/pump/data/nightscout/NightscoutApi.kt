package com.openloop.pump.data.nightscout

import com.openloop.pump.data.nightscout.model.NightscoutEntry
import com.openloop.pump.data.nightscout.model.NightscoutTreatment
import retrofit2.Response
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.Query

/**
 * Nightscout REST API（v1/v2 兼容）。
 *
 * 所有请求需在 Header 携带 API-SECRET（由 [com.openloop.pump.data.repository.NightscoutRepository] 注入）。
 */
interface NightscoutApi {

    @POST("api/v1/treatments")
    suspend fun postTreatments(@Body treatments: List<NightscoutTreatment>): Response<Unit>

    @GET("api/v1/treatments")
    suspend fun getTreatments(@Query("count") count: Int = 10): List<NightscoutTreatment>

    @POST("api/v2/entries")
    suspend fun postEntries(@Body entries: List<NightscoutEntry>): Response<Unit>

    @GET("api/v2/entries")
    suspend fun getEntries(@Query("count") count: Int = 10): List<NightscoutEntry>

    @GET("api/v1/status")
    suspend fun getStatus(): NightscoutStatus
}
