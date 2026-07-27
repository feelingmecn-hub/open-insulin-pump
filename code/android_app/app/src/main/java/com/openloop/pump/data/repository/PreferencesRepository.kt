package com.openloop.pump.data.repository

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.*
import androidx.datastore.preferences.preferencesDataStore
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import javax.inject.Inject
import javax.inject.Singleton

private val Context.dataStore by preferencesDataStore(name = "openloop_preferences")

/**
 * 用户配置（DataStore）—— 闭环参数、Nightscout、BLE 配对密钥等。
 *
 * 注意：BLE passkey 与医疗参数不计入自动云备份（见 AndroidManifest backup_rules）。
 */
@Singleton
class PreferencesRepository @Inject constructor(
    @ApplicationContext private val context: Context
) {
    private val ds: DataStore<Preferences> = context.dataStore

    companion object {
        val NIGHTSCOUT_URL = stringPreferencesKey("ns_url")
        val NIGHTSCOUT_SECRET = stringPreferencesKey("ns_secret")
        val BLE_PASSKEY = stringPreferencesKey("ble_passkey")
        val CLOSED_LOOP_ENABLED = booleanPreferencesKey("closed_loop")
        val ISF = doublePreferencesKey("isf")                 // mg/dL per U
        val TARGET_GLUCOSE = intPreferencesKey("target")       // mg/dL
        val MAX_IOB = doublePreferencesKey("max_iob")          // U
        val MAX_BASAL = doublePreferencesKey("max_basal")      // U/h
        val CARB_RATIO = doublePreferencesKey("carb_ratio")    // g per U
        val PAIRED_ADDRESS = stringPreferencesKey("paired_address")
    }

    // ---- Nightscout ----
    val nightscoutUrl: Flow<String?> = ds.data.map { it[NIGHTSCOUT_URL] }
    val nightscoutSecret: Flow<String?> = ds.data.map { it[NIGHTSCOUT_SECRET] }
    suspend fun setNightscout(url: String, secret: String) {
        ds.edit {
            it[NIGHTSCOUT_URL] = url
            it[NIGHTSCOUT_SECRET] = secret
        }
    }

    // ---- BLE ----
    val blePasskey: Flow<String?> = ds.data.map { it[BLE_PASSKEY] }
    val pairedAddress: Flow<String?> = ds.data.map { it[PAIRED_ADDRESS] }
    suspend fun setPaired(address: String, passkey: String = "") {
        ds.edit {
            it[PAIRED_ADDRESS] = address
            if (passkey.isNotBlank()) it[BLE_PASSKEY] = passkey
        }
    }
    suspend fun setBlePasskey(passkey: String) {
        ds.edit { it[BLE_PASSKEY] = passkey }
    }

    // ---- 闭环参数 ----
    val closedLoopEnabled: Flow<Boolean> = ds.data.map { it[CLOSED_LOOP_ENABLED] ?: false }
    val isf: Flow<Double> = ds.data.map { it[ISF] ?: 50.0 }
    val targetGlucose: Flow<Int> = ds.data.map { it[TARGET_GLUCOSE] ?: 110 }
    val maxIob: Flow<Double> = ds.data.map { it[MAX_IOB] ?: 4.0 }
    val maxBasal: Flow<Double> = ds.data.map { it[MAX_BASAL] ?: 5.0 }
    val carbRatio: Flow<Double> = ds.data.map { it[CARB_RATIO] ?: 10.0 }

    suspend fun setLoopParam(
        enabled: Boolean? = null, isf: Double? = null, target: Int? = null,
        maxIob: Double? = null, maxBasal: Double? = null, carbRatio: Double? = null
    ) {
        ds.edit { p ->
            enabled?.let { p[CLOSED_LOOP_ENABLED] = it }
            isf?.let { p[ISF] = it }
            target?.let { p[TARGET_GLUCOSE] = it }
            maxIob?.let { p[MAX_IOB] = it }
            maxBasal?.let { p[MAX_BASAL] = it }
            carbRatio?.let { p[CARB_RATIO] = it }
        }
    }
}
