package com.openloop.pump.ui.basal

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.openloop.pump.data.repository.PumpRepository
import com.openloop.pump.domain.model.BasalSlot
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import javax.inject.Inject

@HiltViewModel
class BasalViewModel @Inject constructor(
    private val pumpRepo: PumpRepository
) : ViewModel() {

    val slots = MutableStateFlow(List(24) { BasalSlot(it, 1.0) })
    val saving = MutableStateFlow(false)

    val dailyTotal = slots.map { it.sumOf { s -> s.rateUh } }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 24.0)

    fun updateSlot(hour: Int, rate: Double) {
        slots.update { list ->
            list.map { if (it.hour == hour) it.copy(rateUh = rate.coerceIn(0.0, 5.0)) else it }
        }
    }

    /** 将全部 24 槽基础率写入泵（分槽下发 Ble 命令）。 */
    fun applyAllToPump() {
        viewModelScope.launch {
            saving.value = true
            slots.value.forEach { slot ->
                pumpRepo.updateBasalSlot(slot.hour, slot.rateUh)
            }
            saving.value = false
        }
    }
}
