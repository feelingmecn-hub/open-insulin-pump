package com.openloop.pump.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val BrandBlue = Color(0xFF0E6BA8)

private val LightColors = lightColorScheme(
    primary = BrandBlue,
    secondary = Color(0xFF4CAF50),
    tertiary = Color(0xFFFF9800),
    error = Color(0xFFD32F2F)
)

private val DarkColors = darkColorScheme(
    primary = Color(0xFF4FA3E3),
    secondary = Color(0xFF66BB6A),
    tertiary = Color(0xFFFFB74D),
    error = Color(0xFFEF5350)
)

@Composable
fun OpenLoopTheme(content: @Composable () -> Unit) {
    val colors = if (isSystemInDarkTheme()) DarkColors else LightColors
    MaterialTheme(
        colorScheme = colors,
        content = content
    )
}
