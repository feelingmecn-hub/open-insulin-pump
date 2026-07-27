package com.openloop.pump.ui.navigation

import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.History
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.WaterDrop
import androidx.compose.material.icons.filled.Schedule
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.NavHostController
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import com.openloop.pump.ui.bolus.BolusScreen
import com.openloop.pump.ui.basal.BasalScreen
import com.openloop.pump.ui.dashboard.DashboardScreen
import com.openloop.pump.ui.history.HistoryScreen
import com.openloop.pump.ui.settings.SettingsScreen

/** 底部导航目标。 */
sealed class Screen(val route: String, val label: String, val icon: ImageVector) {
    data object Dashboard : Screen("dashboard", "首页", Icons.Filled.Home)
    data object Bolus : Screen("bolus", "大剂量", Icons.Filled.WaterDrop)
    data object Basal : Screen("basal", "基础率", Icons.Filled.Schedule)
    data object History : Screen("history", "历史", Icons.Filled.History)
    data object Settings : Screen("settings", "设置", Icons.Filled.Settings)
}

val bottomItems = listOf(
    Screen.Dashboard, Screen.Bolus, Screen.Basal, Screen.History, Screen.Settings
)

@Composable
fun AppNavHost(
    navController: NavHostController = rememberNavController()
) {
    Scaffold(
        bottomBar = { BottomBar(navController) }
    ) { padding ->
        NavHost(
            navController = navController,
            startDestination = Screen.Dashboard.route,
            modifier = Modifier.padding(padding)
        ) {
            composable(Screen.Dashboard.route) { DashboardScreen() }
            composable(Screen.Bolus.route) { BolusScreen() }
            composable(Screen.Basal.route) { BasalScreen() }
            composable(Screen.History.route) { HistoryScreen() }
            composable(Screen.Settings.route) { SettingsScreen() }
        }
    }
}

@Composable
private fun BottomBar(navController: NavHostController) {
    val backStackEntry by navController.currentBackStackEntryAsState()
    val current = backStackEntry?.destination
    NavigationBar {
        bottomItems.forEach { screen ->
            val selected = current?.hierarchy?.any { it.route == screen.route } == true
            NavigationBarItem(
                icon = { Icon(screen.icon, contentDescription = screen.label) },
                label = { Text(screen.label) },
                selected = selected,
                onClick = {
                    navController.navigate(screen.route) {
                        popUpTo(navController.graph.findStartDestination().id) {
                            saveState = true
                        }
                        launchSingleTop = true
                        restoreState = true
                    }
                }
            )
        }
    }
}
