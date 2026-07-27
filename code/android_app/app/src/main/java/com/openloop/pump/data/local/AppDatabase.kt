package com.openloop.pump.data.local

import androidx.room.Database
import androidx.room.RoomDatabase
import com.openloop.pump.data.local.dao.GlucoseDao
import com.openloop.pump.data.local.dao.TreatmentDao
import com.openloop.pump.data.local.entity.GlucoseEntity
import com.openloop.pump.data.local.entity.TreatmentEntity

/**
 * 本地 Room 数据库 —— 存储治疗记录与血糖历史（用于历史页与 Nightscout 离线缓存）。
 */
@Database(
    entities = [TreatmentEntity::class, GlucoseEntity::class],
    version = 1,
    exportSchema = false
)
abstract class AppDatabase : RoomDatabase() {
    abstract fun treatmentDao(): TreatmentDao
    abstract fun glucoseDao(): GlucoseDao

    companion object {
        const val NAME = "openloop_pump.db"
    }
}
