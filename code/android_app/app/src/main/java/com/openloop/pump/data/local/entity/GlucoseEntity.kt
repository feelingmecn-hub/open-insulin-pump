package com.openloop.pump.data.local.entity

import androidx.room.Entity
import androidx.room.PrimaryKey

/**
 * 血糖记录（来自 CGM，本地缓存）。
 */
@Entity(tableName = "glucose")
data class GlucoseEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val mgdl: Int,
    val timestamp: Long,
    val trend: String,
    val synced: Boolean = false
)
