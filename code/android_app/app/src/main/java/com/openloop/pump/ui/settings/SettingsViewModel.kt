package com.openloop.pump.ui.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.openloop.pump.data.repository.PreferencesRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import javax.inject.Inject

@HiltViewModel
class SettingsViewModel @Inject constructor(
    private val prefs: PreferencesRepository
) : ViewModel() {

    val closedLoop = prefs.closedLoopEnabled
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), false)
    val isf = prefs.isf.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 50.0)
    val target = prefs.targetGlucose
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 110)
    val maxIob = prefs.maxIob.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 4.0)
    val maxBasal = prefs.maxBasal.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 5.0)
    val carbRatio = prefs.carbRatio
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 10.0)
    val nsUrl = prefs.nightscoutUrl
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), "")
    val blePasskey = prefs.blePasskey
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), "")

    fun setLoop(enabled: Boolean) = io { prefs.setLoopParam(enabled = enabled) }
    fun setIsp2(v: Double) = io { prefs.setLoopParam(isf = v) }
    fun setTarget(v: Int) = io { prefs.setLoopParam(target = v) }
    fun setMaxIob(v: Double) = io { prefs.setLoopParam(maxIob = v) }
    fun setMaxBasal(v: Double) = io { prefs.setLoopParam(maxBasal = v) }
    fun setCarbRatio(v: Double) = io { prefs.setLoopParam(carbRatio = v) }
    fun setNightscout(url: String, secret: String) = io { prefs.setNightscout(url, secret) }
    fun setPasskey(pin: String) = io { prefs.setBlePasskey(pin) }

    private fun io(block: suspend () -> Unit) = viewModelScope.launch { block() }
}
