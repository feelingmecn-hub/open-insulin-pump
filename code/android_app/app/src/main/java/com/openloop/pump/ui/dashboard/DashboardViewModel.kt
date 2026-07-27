package com.openloop.pump.ui.dashboard

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.openloop.pump.data.repository.CgmRepository
import com.openloop.pump.data.repository.PreferencesRepository
import com.openloop.pump.data.repository.PumpRepository
import com.openloop.pump.domain.algorithm.GlucoseForecast
import com.openloop.pump.domain.model.toDomain
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import javax.inject.Inject

@HiltViewModel
class DashboardViewModel @Inject constructor(
    private val pumpRepo: PumpRepository,
    private val cgmRepo: CgmRepository,
    private val prefs: PreferencesRepository
) : ViewModel() {

    val connectionState = pumpRepo.connectionState
    val pumpState = pumpRepo.pumpStatusFlow.map { it?.toDomain() }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), null)
    val glucose = cgmRepo.glucose
    val iob = pumpRepo.iob
    val reservoir = pumpRepo.reservoir
    val closedLoop = prefs.closedLoopEnabled
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), false)

    /** 基于 IOB 与 ISF 预测 30 分钟后血糖 (mg/dL)。 */
    val predictedGlucose = combine(glucose, iob, prefs.isf) { g, i, isf ->
        g?.let { GlucoseForecast.predict(it.mgdl, i, isf, 30) } ?: 0
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

    /** 未连接时尝试连接已配对设备，或开始扫描。 */
    fun ensureConnected() {
        viewModelScope.launch {
            val addr = prefs.pairedAddress.first()
            if (addr != null) pumpRepo.connect(addr)
            else pumpRepo.startScan()
        }
    }

    fun disconnect() = pumpRepo.disconnect()
}
