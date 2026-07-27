package com.openloop.pump.ui.bolus

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.openloop.pump.data.local.AppDatabase
import com.openloop.pump.data.local.entity.TreatmentEntity
import com.openloop.pump.data.repository.CgmRepository
import com.openloop.pump.data.repository.NightscoutRepository
import com.openloop.pump.data.repository.PreferencesRepository
import com.openloop.pump.data.repository.PumpRepository
import com.openloop.pump.domain.algorithm.ClosedLoopAlgorithm
import com.openloop.pump.domain.model.BolusRequest
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import javax.inject.Inject

@HiltViewModel
class BolusViewModel @Inject constructor(
    private val pumpRepo: PumpRepository,
    private val cgmRepo: CgmRepository,
    private val db: AppDatabase,
    private val nsRepo: NightscoutRepository,
    private val prefs: PreferencesRepository
) : ViewModel() {

    val iob: StateFlow<Double> = pumpRepo.iob
    val glucose = cgmRepo.glucose

    val units = MutableStateFlow(0.0)
    val recommendedBolus = MutableStateFlow(0.0)
    val isExtended = MutableStateFlow(false)
    val durationMin = MutableStateFlow(30)
    val note = MutableStateFlow("")
    val sending = MutableStateFlow(false)
    val lastResult = MutableStateFlow<BolusOutcome?>(null)

    /** 按 0.05U 步进设置剂量（精度上限）。 */
    fun setUnits(value: Double) {
        units.value = (value * 20).toInt().coerceAtLeast(0) / 20.0
    }

    fun stepUnits(delta: Double) = setUnits(units.value + delta)
    fun toggleExtended() { isExtended.value = !isExtended.value }
    fun setDuration(min: Int) { durationMin.value = min.coerceAtLeast(0) }
    fun setNote(s: String) { note.value = s }

    /** 基于当前血糖 + IOB 计算校正剂量推荐（需用户确认）。 */
    fun computeRecommendation() {
        val g = glucose.value ?: return
        viewModelScope.launch {
            val isf = prefs.isf.first()
            val target = prefs.targetGlucose.first()
            val algo = ClosedLoopAlgorithm(isf = isf, targetGlucose = target)
            recommendedBolus.value = algo.recommendCorrectionBolus(
                g.mgdl, iob.value, isf, target
            )
        }
    }

    fun applyRecommendation() = setUnits(recommendedBolus.value)

    fun submit() {
        if (units.value <= 0 || sending.value) return
        viewModelScope.launch {
            sending.value = true
            val req = BolusRequest(
                units = units.value,
                isExtended = isExtended.value,
                durationMin = durationMin.value,
                note = note.value
            )
            val r = pumpRepo.deliverBolus(req)
            val ts = System.currentTimeMillis()
            if (r.isSuccess) {
                db.treatmentDao().insert(
                    TreatmentEntity(
                        type = "BOLUS", units = req.units,
                        timestamp = ts, durationMin = req.durationMin, note = req.note
                    )
                )
                nsRepo.uploadBolus(req.units, req.note, ts)
                lastResult.value = BolusOutcome.Success
                units.value = 0.0
            } else {
                lastResult.value = BolusOutcome.Failure(
                    r.exceptionOrNull()?.message ?: "发送失败"
                )
            }
            sending.value = false
        }
    }

    sealed interface BolusOutcome {
        data object Success : BolusOutcome
        data class Failure(val msg: String) : BolusOutcome
    }
}
