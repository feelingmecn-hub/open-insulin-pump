package com.openloop.pump.ui.basal

import android.util.Log
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
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import javax.inject.Inject

private const val TAG = "BasalViewModel"

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

    /** 写入结果提示（null = 无提示）。 */
    val saveResult = MutableStateFlow<String?>(null)

    /**
     * 将全部 24 槽基础率真正写入泵内 NVS 档案（SET 0x17，按激活方案逐槽下发）。
     * 区别于旧实现（只设实时运行速率、忽略槽位），本实现持久化 24 段方案，
     * 使「验证测试」能核对泵内档案与电机动作是否一致。
     * 全程串行（BLE Mutex）+ 每写带超时，finally 保证 saving 复位，杜绝按钮卡「写入泵中」。
     */
    fun applyAllToPump() {
        viewModelScope.launch {
            saving.value = true
            saveResult.value = null
            try {
                val profRes = pumpRepo.getActiveBasalProfile()
                val profile = if (profRes.isSuccess) profRes.getOrDefault(0) else 0
                if (profRes.isFailure) {
                    Log.w(TAG, "读取激活方案失败，退回方案#0: ${profRes.exceptionOrNull()?.message}")
                }
                var ok = 0
                slots.value.forEach { slot ->
                    val r = pumpRepo.setBasalProfileSlot(profile, slot.hour, slot.rateUh)
                    if (r.isSuccess) ok++ else Log.w(TAG, "写槽 hour=${slot.hour} 失败: ${r.exceptionOrNull()?.message}")
                    delay(60) // 让步，避免刷爆 GATT 队列 / 与 CGM 推送冲突
                }
                val total = slots.value.size
                saveResult.value = if (ok == total)
                    "已写入泵内档案(激活方案 #$profile)全部 $total 段。点「验证测试」核对电机会不会动。"
                else
                    "部分写入失败：$ok/$total 段成功（其余失败，见日志）。"
            } catch (e: Exception) {
                saveResult.value = "写入异常：${e.message ?: "未知错误"}"
            } finally {
                saving.value = false
            }
        }
    }

    fun clearSaveResult() { saveResult.value = null }

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
