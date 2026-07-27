package com.openloop.pump.ui.history

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.openloop.pump.data.local.AppDatabase
import com.openloop.pump.data.local.entity.TreatmentEntity
import com.openloop.pump.data.repository.NightscoutRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import javax.inject.Inject

@HiltViewModel
class HistoryViewModel @Inject constructor(
    private val db: AppDatabase,
    private val nsRepo: NightscoutRepository
) : ViewModel() {

    val treatments = flow { emit(db.treatmentDao().recent(200)) }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    val glucose = flow { emit(db.glucoseDao().recent(200)) }
        .stateIn(viewModelScope, WhileSubscribed(5000), emptyList())

    val syncing = MutableStateFlow(false)

    /** 将本地未同步记录上传到 Nightscout。 */
    fun syncToNightscout() {
        viewModelScope.launch {
            syncing.value = true
            db.treatmentDao().unsynced().forEach { t: TreatmentEntity ->
                val r = if (t.type == "BOLUS") {
                    nsRepo.uploadBolus(t.units, t.note, t.timestamp)
                } else {
                    nsRepo.uploadTempBasal(t.units, t.durationMin, t.timestamp)
                }
                if (r.isSuccess) db.treatmentDao().update(t.copy(synced = true))
            }
            syncing.value = false
        }
    }
}
