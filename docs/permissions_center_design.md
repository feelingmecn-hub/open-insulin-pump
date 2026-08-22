# 伴生 App 权限中心设计

> 目标：在设置里提供一个「所有权限申请」的统一入口，解决「很多手机找不到我们 App 需要的权限」的问题。
> 适用范围：code/android_app（Jetpack Compose + Hilt）。

## 1. 现状问题诊断

`MainActivity.kt` 当前只在启动时调一次 `requestBlePermissions()`，且**只申请蓝牙 + 低版本位置**：

```kotlin
private fun requestBlePermissions() {
    val needed = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        listOf(BLUETOOTH_SCAN, BLUETOOTH_CONNECT)
    } else {
        listOf(ACCESS_FINE_LOCATION)
    }.filter { ... 未授权 }
    if (needed.isNotEmpty()) permLauncher.launch(needed.toTypedArray())
}
```

暴露出的三个硬伤：

1. **入口缺失**：通知权限（`POST_NOTIFICATIONS`，Android 13+）、电池优化白名单、自启动/后台保活，这些**根本没有申请入口**。用户一拒绝或在系统设置里关掉，App 里找不到回去的路。
2. **拒绝即死**：用户点「不再询问」后，系统 `requestPermissions` 直接失效，而 App 没有引导跳「应用详情页」手动开的兜底，于是 BLE 静默失败、连不上泵，用户只会觉得 App 坏了。
3. **无状态可见**：没有任何地方显示「当前还缺哪些权限」，用户不知道为什么连不上。

这正是大叔反馈「现在很多手机找不到我们伴生 App 需要的权限」的根因。

## 2. 方案总览

新增一个**权限中心页面**，把「需要什么权限 / 当前状态 / 怎么补」一次性讲清楚，并支持：

- **运行时权限**：点击即弹系统授权框（蓝牙、通知）。
- **非弹窗权限**：电池优化豁免、自启动，点击跳对应系统设置页（Android 不提供弹窗 API，只能引导）。
- **一键申请全部**：把所有可弹的、未授权的必需权限一次性拉起。
- **缺失告警条**：设置页/首页顶部，当还有必需权限缺失时显示红条，点一下直达权限中心。

### 改动文件清单

| 文件 | 动作 | 说明 |
|---|---|---|
| `ui/settings/PermissionsScreen.kt` | 新增 | 权限中心 Compose 页面 |
| `util/PermissionStatus.kt` | 新增 | 权限清单定义 + 状态检测 + 跳设置 Intent 构造（纯函数 object） |
| `ui/navigation/NavGraph.kt` | 改 | 加 `composable("permissions")` 路由 |
| `ui/settings/SettingsScreen.kt` | 改 | 加「权限中心」入口卡片 + 顶部缺失告警条 |
| `MainActivity.kt` | 改 | 首启引导扩为所有 RUNTIME 类权限（保留启动体验） |
| `AndroidManifest.xml` | 改 | 加 `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` 声明 |

## 3. 权限清单（按必需性分组）

### A. 必需 · 运行时弹窗（Android 标准 API）

| 权限 | 条件 | 用途 |
|---|---|---|
| `BLUETOOTH_CONNECT` | Android 12+ (S) | 连泵必需 |
| `BLUETOOTH_SCAN` | Android 12+，已声明 `neverForLocation` | 扫泵必需 |
| `POST_NOTIFICATIONS` | Android 13+ (T) | 报警 / 状态变更通知 |

### B. 必需 · 跳系统设置（无弹窗 API）

| 项 | 用途 | 跳转方式 |
|---|---|---|
| 电池优化豁免 | 后台 BLE 长连保活，否则被系统杀 → 闭环断连 | `Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` + `Uri.parse("package:$pkg")`（直达本 App 豁免页）；不支持的 ROM 退到 `ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS` 总开关 |
| 自启动 / 后台管理 | 开机自启闭环（厂商相关，无标准 API） | 兜底 `Settings.ACTION_APPLICATION_DETAILS_SETTINGS`（让用户手动找「自启动 / 省电白名单」）；可选进阶：识别华为/小米/OPPO/vivo 跳对应 deep link |

### C. 可选 · 非必需

| 权限 | 条件 | 说明 |
|---|---|---|
| `ACCESS_FINE_LOCATION` | Android 11 及以下 | 因 `BLUETOOTH_SCAN` 已声明 `neverForLocation`，Android 12+ 扫描免位置；但 Android 11 及更低没有 `BLUETOOTH_SCAN`，仍需位置才能扫。标注「Android 11 及以下用于 BLE 扫描，可授权以兼容旧系统；新系统不授权不影响功能」，避免误导用户以为必须给定位。 |

> 红线：只申请真正需要的权限，不碰通讯录/存储/相机/麦克风，避免被商店拒审或引发用户不信任。

## 4. 关键技术卡点

### 4.1 电池优化豁免（保活关键）
后台 BLE 长连要活，必须关电池优化。Manifest 需补声明：
```xml
<uses-permission android:name="android.permission.REQUEST_IGNORE_BATTERY_OPTIMIZATIONS" />
```
跳转（直达本 App 页，体验最好）：
```kotlin
Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
    data = Uri.parse("package:$pkg")
}
```
状态检测：`(context.getSystemService(POWER_SERVICE) as PowerManager).isIgnoringBatteryOptimizations(pkg)`。

### 4.2 自启动 / 后台管理
无标准 API。最稳兜底跳应用详情页：
```kotlin
Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
    data = Uri.parse("package:$pkg")
}
```
可进阶识别厂商跳对应自启动设置（华为 `com.huawei.systemmanager` 等），但 deep link 易随 ROM 版本失效，先做兜底，进阶作为后续可选优化。

### 4.3 永久拒绝（「不再询问」）
`Activity.shouldShowRequestPermissionRationale(perm)` 返回 `false` 且 `checkSelfPermission != GRANTED` → 系统弹窗已不可触发，**只能跳应用详情页**让用户手动开。权限卡片需据此把按钮文案从「去授权」切换为「去设置」。

### 4.4 申请必须在 Activity 上下文
Compose 中用 `rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions())` 注册 launcher，**不需要动 `MainActivity` 的 launcher**，权限中心页面自己持有一个 launcher 即可。

## 5. 页面交互设计（PermissionsScreen）

- **顶部告警条**：当 `missingRequiredCount > 0` 显示红色「X 项必需权限未授予，点击去设置」，点一下逐个引导。
- **分组卡片**：按 A/B/C 三组列出。
- **每权限一行卡片**：
  - 显示名 + 用途说明（一句话）
  - 状态徽章：✅ 已授权 / ⚠ 未授权（可弹）/ 🔧 需手动设置
  - 操作按钮：
    - 可弹 → 「去授权」启动 launcher
    - 需手动 → 「去设置」启动对应 Intent
    - 已授权 → 灰色「已就绪」
- **底部「一键申请全部必需权限」按钮**：收集所有未授权的 RUNTIME 类权限一次性 launch；B 类（电池/自启动）单独提示跳设置。

## 6. SettingsScreen 改动

- 新增 `CardSection("权限中心")`，内放按钮 `nav.navigate("permissions")`，并简短说明「统一管理蓝牙/通知/电池/自启动权限」。
- 顶部（在 `Column` 开头）根据 `PermissionStatus.missingRequiredCount(LocalContext.current)` 显示红条告警（直接读 context，不改 ViewModel，最简）。
- 复用已有的 `LaunchedEffect` + Toast 机制做轻提示。

## 7. MainActivity 改动

保留启动即引导的首启体验，但把 `requestBlePermissions()` 扩为申请所有 RUNTIME 类权限（蓝牙 + 通知），避免首启时用户漏给通知权限。电池优化/自启动不在启动时弹（会骚扰），引导去权限中心处理。注意：`shouldShowRequestPermissionRationale` 判定，避免对已拒绝权限反复狂弹。

## 8. 验证与提交

1. `:app:compileDebugKotlin` 编译过（目录 `code/android_app` + JAVA_HOME 双路兜底）。
2. 华为 P30（Android 10）实测：进设置 → 权限中心，逐项验证弹窗/跳设置正常；模拟拒绝后「不再询问」，确认跳应用详情可用；确认电池优化豁免页可达。
3. 分组提交：`feat(app): 新增权限中心（状态展示+一键申请+电池/自启动跳设置兜底）`，push `origin main` 并 `ls-remote` 校验。

---

*安全红线提醒：本项目为 DIY 开源、非医疗器械、严禁人体使用。权限中心 UI 文案需保留「非医疗器械，使用风险自负」声明。*
