package com.openloop.pump.ui.mirror

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.hilt.navigation.compose.hiltViewModel
import com.openloop.pump.ble.ConnectionState
import com.openloop.pump.ble.PumpProtocol

/**
 * 泵屏（可交互虚拟屏）—— 与泵实体屏一致的菜单，可浏览/选择/操作。
 *
 * 设计：固件每次屏幕变化/1Hz 推送导航状态（PumpNav：当前屏+选中+编辑值），
 * 本屏据此在 App 本地复刻与泵一致的菜单（标题/选项/选中高亮/数值）。
 * 4 个虚拟按键通过 KEY 通道发往泵，泵端 ui_screen_key 按当前屏幕处理一切业务逻辑
 * （导航/进子菜单/调值/注射/切换环模式），操作后泵推送新导航 → 本屏自动同步。
 */
@Composable
fun PumpMirrorScreen(viewModel: PumpMirrorViewModel = hiltViewModel()) {
    val nav by viewModel.pumpNav.collectAsState()
    val live by viewModel.pumpLiveState.collectAsState()
    val conn by viewModel.connectionState.collectAsState()

    Column(Modifier.fillMaxSize().padding(12.dp)) {
        StatusBar(conn, live, nav)
        Spacer(Modifier.height(8.dp))
        Box(Modifier.weight(1f).fillMaxWidth()) {
            val n = nav
            if (n == null) {
                Text("等待泵屏同步…", color = Color.Gray, fontSize = 16.sp)
            } else {
                ScreenContent(PumpMenu.viewFor(n, live), n, live)
            }
        }
        Spacer(Modifier.height(8.dp))
        Keypad(viewModel)
    }
}

@Composable
private fun StatusBar(conn: ConnectionState, live: PumpProtocol.PumpLiveState?, nav: PumpProtocol.PumpNav?) {
    val connected = conn is ConnectionState.Connected || conn is ConnectionState.Bonded
    val connText = when {
        connected -> "已连接"
        conn is ConnectionState.Connecting || conn is ConnectionState.Scanning -> "连接中…"
        else -> "未连接"
    }
    val clock = live?.clockText ?: "--:--"
    val batt = live?.batteryPct ?: 0
    val alarm = when {
        nav?.primeActive == true -> "排气中"
        live?.alarmActive == true -> "⚠ 报警"
        else -> ""
    }
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(connText, fontSize = 14.sp,
            color = if (connected) Color(0xFF2e9e4f) else Color.Gray)
        Text(clock, fontSize = 16.sp, fontWeight = FontWeight.Bold)
        Text("🔋$batt%", fontSize = 14.sp)
        if (alarm.isNotEmpty())
            Text(alarm, fontSize = 14.sp, color = Color(0xFFd83a3a))
    }
}

@Composable
private fun ScreenContent(
    view: PumpMenu.ScreenView,
    nav: PumpProtocol.PumpNav,
    live: PumpProtocol.PumpLiveState?
) {
    Column(Modifier.fillMaxWidth()) {
        Text(view.title, fontSize = 20.sp, fontWeight = FontWeight.Bold, color = Color(0xFF006bb7))
        Spacer(Modifier.height(8.dp))
        if (view.isHome) {
            HomeCards(nav, live)
        } else if (view.items.isEmpty()) {
            Text(view.note ?: "（空）", fontSize = 15.sp, color = Color.Gray)
        } else {
            LazyColumn {
                itemsIndexed(view.items) { i, item ->
                    val selected = i == nav.sel
                    Row(
                        Modifier.fillMaxWidth()
                            .clip(RoundedCornerShape(6.dp))
                            .background(if (selected) Color(0xFF006bb7) else Color(0xFFF2F5F8))
                            .padding(12.dp),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(item.label, fontSize = 16.sp,
                            color = if (selected) Color.White else Color(0xFF1f2733),
                            fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal)
                        if (item.value.isNotEmpty())
                            Text(item.value, fontSize = 16.sp,
                                color = if (selected) Color.White else Color(0xFF006bb7))
                    }
                    Spacer(Modifier.height(4.dp))
                }
            }
        }
    }
}

@Composable
private fun HomeCards(nav: PumpProtocol.PumpNav, live: PumpProtocol.PumpLiveState?) {
    val loopStr = when (live?.loopMode) {
        0 -> "闭环中"; 1 -> "开环"; else -> "已暂停"
    }
    val glu = if ((live?.glucoseMgdl ?: 0) > 0)
        "%.1f mmol/L".format(live?.glucoseMmol ?: 0.0) else "—"
    val rows = listOf(
        "剩余药量" to "%.1f U".format(live?.reservoirUnits ?: 0.0),
        "IOB" to "%.1f U".format(live?.iobUnits ?: 0.0),
        "基础率" to "%.1f U/h".format(live?.basalRateUh ?: 0.0),
        "血糖" to glu,
        "闭环模式" to loopStr,
        "今日累计" to "%.1f U".format(live?.todayUnits ?: 0.0)
    )
    rows.forEach { (k, v) ->
        Row(Modifier.fillMaxWidth().background(Color(0xFFF2F5F8)).padding(10.dp),
            horizontalArrangement = Arrangement.SpaceBetween) {
            Text(k, fontSize = 15.sp, color = Color(0xFF1f2733))
            Text(v, fontSize = 15.sp, color = Color(0xFF006bb7), fontWeight = FontWeight.Bold)
        }
        Spacer(Modifier.height(4.dp))
    }
    Text("按 ✓ 进入主菜单操作", fontSize = 13.sp, color = Color.Gray)
}

@Composable
private fun Keypad(vm: PumpMirrorViewModel) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
        KeyBtn("▲", vm.KEY_UP, vm)
        KeyBtn("✓", vm.KEY_SET, vm)
        KeyBtn("▼", vm.KEY_DOWN, vm)
        KeyBtn("←", vm.KEY_ESC, vm)
    }
}

@Composable
private fun KeyBtn(label: String, event: Int, vm: PumpMirrorViewModel) {
    Box(
        Modifier.size(72.dp, 54.dp)
            .clip(RoundedCornerShape(8.dp))
            .background(Color(0xFF006bb7))
            .pointerInput(event) {
                detectTapGestures(
                    onPress = {
                        vm.pressKey(event)   // 按下：suspend 直到送达固件
                        tryAwaitRelease()    // 等待手指抬起（按住期间固件自动重复=连续滚动）
                        vm.releaseKey()      // 抬起：停止自动重复
                    }
                )
            },
        contentAlignment = Alignment.Center
    ) {
        Text(label, color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.Bold)
    }
}

/**
 * 泵菜单树（本地复刻）—— 仅含渲染所需：标题 + 选项列表 + 数值。
 * 业务逻辑全在固件 ui_screen_key，App 只按 nav.screen 选渲染、按 nav.sel 高亮。
 */
object PumpMenu {
    data class MenuItem(val label: String, val value: String = "")
    data class ScreenView(
        val title: String,
        val items: List<MenuItem> = emptyList(),
        val isHome: Boolean = false,
        val note: String? = null
    )

    private const val HOME = 0
    private const val MENU = 1
    private const val BASAL = 2
    private const val BOLUS_MENU = 3
    private const val BOLUS_NORMAL = 4
    private const val BOLUS_SQUARE = 5
    private const val BOLUS_DUAL = 6
    private const val BOLUS_WIZARD = 7
    private const val BOLUS_MEALS = 8
    private const val PRIME = 9
    private const val ALARM_LIST = 10
    private const val ALARM_DETAIL = 11
    private const val LOOP = 12
    private const val SETTINGS = 13
    private const val CLOCK_SET = 14
    private const val ABOUT = 15
    private const val HISTORY = 16
    private const val TBR = 17
    private const val PROFILE = 18
    private const val MISSED_BOLUS = 19
    private const val REWIND_CAL = 20
    private const val PROFILE_DETAIL = 21
    private const val PROFILE_RENAME = 22
    private const val BASAL_CHART = 23
    private const val BASAL_HISTORY = 24

    private val MENU_ITEMS = listOf(
        "基础率", "大剂量", "排气装药", "报警", "闭环",
        "临时基础率", "历史记录", "系统设置", "回退/标定"
    )
    private val LOOP_ITEMS = listOf("闭环中 (AAPS接管)", "开环", "已暂停")
    private val BOLUS_MENU_ITEMS = listOf(
        "常规大剂量", "方波大剂量", "双波大剂量", "大剂量向导", "餐时大剂量"
    )
    private val MEAL_ITEMS = listOf("早餐", "午餐", "晚餐")
    private val ALARM_ITEMS = listOf("无", "电量低", "药量低", "堵管", "输注超时", "按键锁", "过温")
    private val SETTINGS_ITEMS = listOf("亮度", "按键音")
    private val HISTORY_ITEMS = listOf("本方案", "全部")
    private val CLOCK_ITEMS = listOf("年", "月", "日", "时", "分", "保存")
    private val TBR_ITEMS = listOf("百分比", "时长", "启动临时基础率", "取消临时基础率")

    fun viewFor(nav: PumpProtocol.PumpNav, live: PumpProtocol.PumpLiveState?): ScreenView {
        val v0 = nav.v0
        val v1 = nav.v1
        val v2 = nav.v2
        return when (nav.screen) {
            HOME -> ScreenView("闭环胰岛素泵", isHome = true)
            MENU -> ScreenView("主菜单", MENU_ITEMS.map { MenuItem(it) })
            BASAL -> ScreenView("基础率", note = "基础率编辑 / 方案管理（请在泵实体键操作）")
            BOLUS_MENU -> ScreenView("大剂量", BOLUS_MENU_ITEMS.map { MenuItem(it) })
            BOLUS_NORMAL -> ScreenView("常规大剂量",
                listOf(MenuItem("剂量", "%.1f U".format(v0 / 100.0))))
            BOLUS_SQUARE -> ScreenView("方波大剂量", listOf(
                MenuItem("剂量", "%.1f U".format(v0 / 100.0)),
                MenuItem("时长", "%d h".format(v1))
            ))
            BOLUS_DUAL -> ScreenView("双波大剂量", listOf(
                MenuItem("立即量", "%.1f U".format(v0 / 100.0)),
                MenuItem("方波量", "%.1f U".format(v1 / 100.0)),
                MenuItem("时长", "%d h".format(v2))
            ))
            BOLUS_WIZARD -> ScreenView("大剂量向导", listOf(
                MenuItem("血糖", "%.1f mmol/L".format(v0 / 10.0)),
                MenuItem("碳水", "%.0f g".format(v1.toDouble()))
            ))
            BOLUS_MEALS -> ScreenView("餐时大剂量", MEAL_ITEMS.map { MenuItem(it) })
            PRIME -> ScreenView("排气装药",
                listOf(MenuItem("排气量", "%.1f U".format(v0 / 10.0))))
            ALARM_LIST -> ScreenView("报警", ALARM_ITEMS.map { MenuItem(it) })
            LOOP -> ScreenView("闭环模式", LOOP_ITEMS.map { MenuItem(it) })
            SETTINGS -> ScreenView("系统设置", SETTINGS_ITEMS.map { MenuItem(it) })
            HISTORY -> ScreenView("历史记录", HISTORY_ITEMS.map { MenuItem(it) })
            CLOCK_SET -> ScreenView("时钟设置", CLOCK_ITEMS.map { MenuItem(it) })
            TBR -> ScreenView("临时基础率", listOf(
                MenuItem("百分比", "%d%%".format(v0 / 10)),
                MenuItem("时长", "%d min".format(v1 * 30)),
                MenuItem("启动临时基础率"),
                MenuItem("取消临时基础率")
            ))
            PROFILE -> ScreenView("基础率方案", note = "方案切换（请在泵实体键操作）")
            else -> ScreenView("屏幕 ${nav.screen}", note = "此屏暂未镜像，请用泵实体键操作")
        }
    }
}
