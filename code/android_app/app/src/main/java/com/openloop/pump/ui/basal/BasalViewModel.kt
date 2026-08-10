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
import kotlinx.coroutines.flow.update
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

    /** 验证测试结果提示（null = 无提示）。 */
    val testResult = MutableStateFlow<String?>(null)

    /**
     * 基础率验证测试：让泵把「泵内当前激活方案」24 段总量一次性打出。
     *
     * 注意读的是**泵内**的档案而不是本页面上的编辑值 —— 这正是它的价值所在：
     * 若泵内档案没被真正写入，测试量会与本页显示的日总量对不上（甚至为 0），
     * 一次就能定性区分「没写进去」和「写进去了但电机不动」。
     */
    fun runBasalTest() {
        viewModelScope.launch {
            testResult.value = null
            val r = pumpRepo.runBasalTest()
            testResult.value = if (r.isSuccess)
                "已下发验证测试，请看泵屏与历史记录（事件名「基础率验证」，附实际微步数）"
            else
                "下发失败：${r.exceptionOrNull()?.message ?: "未知错误"}"
        }
    }

    fun clearTestResult() { testResult.value = null }
}
