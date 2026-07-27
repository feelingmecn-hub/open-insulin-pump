package com.openloop.pump.service

import android.content.Context
import androidx.hilt.work.HiltWorker
import androidx.work.CoroutineWorker
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import com.openloop.pump.data.repository.CgmRepository
import com.openloop.pump.data.repository.PreferencesRepository
import com.openloop.pump.data.repository.PumpRepository
import com.openloop.pump.domain.algorithm.ClosedLoopAlgorithm
import dagger.assisted.Assisted
import dagger.assisted.AssistedInject
import java.util.concurrent.TimeUnit

/**
 * 定时闭环 Worker。
 *
 * 每 15 分钟（WorkManager 最小周期）运行一次：
 *   读取 CGM + IOB → 计算临时基础率 (TBR) → 下发泵。
 * 安全：闭环仅调整基础率，绝不自动给大剂量。
 */
@HiltWorker
class LoopWorker @AssistedInject constructor(
    @Assisted context: Context,
    @Assisted params: WorkerParameters,
    private val pumpRepo: PumpRepository,
    private val cgmRepo: CgmRepository,
    private val prefs: PreferencesRepository
) : CoroutineWorker(context, params) {

    companion object {
        const val LOOP_INTERVAL_MIN = 15L   // WorkManager 最小周期
        const val UNIQUE_NAME = "openloop-loop"
    }

    override suspend fun doWork(): Result {
        if (!prefs.closedLoopEnabled.first()) return Result.success()

        val glucose = cgmRepo.glucose.value ?: return Result.retry()
        val iob = pumpRepo.iob.value
        val target = prefs.targetGlucose.first()
        val isf = prefs.isf.first()
        val maxIob = prefs.maxIob.first()
        val maxBasal = prefs.maxBasal.first()
        // TODO: 当前基础率应从激活的基础率方案按当前小时计算
        val currentBasal = 1.0

        val algo = ClosedLoopAlgorithm(
            maxBasalRate = maxBasal, maxIob = maxIob, isf = isf, targetGlucose = target
        )
        val out = algo.compute(
            ClosedLoopAlgorithm.Inputs(glucose.mgdl, iob, currentBasal, isf, target)
        )
        pumpRepo.setTemporaryBasal(out.tbrRateUh, out.tbrDurationMin)
        return Result.success()
    }
}

/** 调度（或更新）周期性闭环任务。 */
fun scheduleLoop(context: Context) {
    val req = PeriodicWorkRequestBuilder<LoopWorker>(LoopWorker.LOOP_INTERVAL_MIN, TimeUnit.MINUTES)
        .build()
    WorkManager.getInstance(context).enqueueUniquePeriodicWork(
        LoopWorker.UNIQUE_NAME,
        ExistingPeriodicWorkPolicy.UPDATE,
        req
    )
}
