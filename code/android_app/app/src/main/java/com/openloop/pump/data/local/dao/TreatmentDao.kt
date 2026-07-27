package com.openloop.pump.data.local.dao

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.Query
import androidx.room.Update
import com.openloop.pump.data.local.entity.TreatmentEntity

@Dao
interface TreatmentDao {

    @Insert
    suspend fun insert(entity: TreatmentEntity): Long

    @Query("SELECT * FROM treatments ORDER BY timestamp DESC LIMIT :limit")
    suspend fun recent(limit: Int): List<TreatmentEntity>

    @Query("SELECT * FROM treatments WHERE synced = 0")
    suspend fun unsynced(): List<TreatmentEntity>

    @Update
    suspend fun update(entity: TreatmentEntity)

    @Query("DELETE FROM treatments WHERE timestamp < :before")
    suspend fun prune(before: Long)
}
