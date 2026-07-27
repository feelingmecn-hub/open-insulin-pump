package com.openloop.pump

import android.app.Application
import androidx.work.Configuration
import androidx.work.WorkManager
import dagger.hilt.android.HiltAndroidApp
import androidx.hilt.work.HiltWorkerFactory
import javax.inject.Inject

/**
 * 应用入口。Hilt 在此注入依赖图根。
 *
 * 同时作为 WorkManager 的 [Configuration.Provider]，向 HiltWorkerFactory
 * 提供注入能力（闭环 LoopWorker 需要注入仓库）。
 *
 * 设备名对应固件 BLE_DEVICE_NAME = "OpenLoop-Pump"
 */
@HiltAndroidApp
class OpenLoopApplication : Application(), Configuration.Provider {

    @Inject
    lateinit var workerFactory: HiltWorkerFactory

    override val workManagerConfiguration: Configuration
        get() = Configuration.Builder()
            .setWorkerFactory(workerFactory)
            .build()
}
