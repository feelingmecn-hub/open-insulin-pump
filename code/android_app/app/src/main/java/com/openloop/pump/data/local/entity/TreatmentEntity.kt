package com.openloop.pump.data.local.entity

import androidx.room.Entity
import androidx.room.PrimaryKey

/**
 * 治疗记录（大剂量 / 临时基础率 / 排气）。
 */
@Entity(tableName = "treatments")
data class TreatmentEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    /** BOLUS / TEMP_BASAL / PRIME / REWIND */
    val type: String,
    val units: Double,
    val timestamp: Long,
    val durationMin: Int = 0,
    val note: String? = null,
    /** 是否已同步到 Nightscout */
    val synced: Boolean = false
)
