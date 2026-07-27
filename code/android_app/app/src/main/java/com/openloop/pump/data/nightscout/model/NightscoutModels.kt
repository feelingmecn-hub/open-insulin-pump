package com.openloop.pump.data.nightscout.model

import com.google.gson.annotations.SerializedName

/** Nightscout treatment（大剂量 / 临时基础率 / 备注）。 */
data class NightscoutTreatment(
    @SerializedName("eventType") val eventType: String,
    @SerializedName("created_at") val createdAt: String,
    @SerializedName("insulin") val insulin: Double? = null,
    @SerializedName("duration") val duration: Int? = null,      // 分钟（TBR）
    @SerializedName("absolute") val absolute: Double? = null,   // U/h（TBR）
    @SerializedName("notes") val notes: String? = null,
    @SerializedName("app") val app: String = "OpenLoopPump"
)

/** Nightscout 血糖条目 (SGV)。 */
data class NightscoutEntry(
    @SerializedName("type") val type: String = "sgv",
    @SerializedName("sgv") val sgv: Int,
    @SerializedName("date") val date: Long,                      // epoch ms
    @SerializedName("direction") val direction: String? = null
)

/** Nightscout 状态响应。 */
data class NightscoutStatus(
    @SerializedName("status") val status: String? = null,
    @SerializedName("version") val version: String? = null
)
