package com.openloop.pump.data.local.dao

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.Query
import androidx.room.Update
import com.openloop.pump.data.local.entity.GlucoseEntity

@Dao
interface GlucoseDao {

    @Insert
    suspend fun insert(entity: GlucoseEntity): Long

    @Query("SELECT * FROM glucose ORDER BY timestamp DESC LIMIT :limit")
    suspend fun recent(limit: Int): List<GlucoseEntity>

    @Query("SELECT * FROM glucose WHERE synced = 0")
    suspend fun unsynced(): List<GlucoseEntity>

    @Update
    suspend fun update(entity: GlucoseEntity)

    @Query("DELETE FROM glucose WHERE timestamp < :before")
    suspend fun prune(before: Long)
}
