package com.velmux.mobile.design

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import com.velmux.mobile.platform.ThemeMode

/**
 * Velmux design system (docs/DESIGN_SYSTEM.md): Material 3, warm copper
 * accent (#E8833A family — the established Velmux accent), light/dark/
 * system. No dynamic color: the brand palette is the product identity.
 */
object VelmuxColors {
    val Copper300 = Color(0xFFF5B784)
    val Copper400 = Color(0xFFF09A5A)
    val Copper = Color(0xFFE8833A)
    val Copper600 = Color(0xFFC96A28)
    val Copper700 = Color(0xFFA5531D)

    val Paper = Color(0xFFFFF8F3)
    val PaperDim = Color(0xFFF6EBE2)
    val Ink = Color(0xFF2A211B)
    val InkSoft = Color(0xFF6E6057)
    val Night = Color(0xFF17120E)
    val NightSurface = Color(0xFF211A14)
    val NightSurfaceHi = Color(0xFF2C231C)
    val NightInk = Color(0xFFF2E9E1)
    val NightInkSoft = Color(0xFFB5A79B)

    val Success = Color(0xFF3E8E55)
    val Warn = Color(0xFFB4770F)
    val Danger = Color(0xFFB3372B)
    val SuccessDark = Color(0xFF7FCB96)
    val WarnDark = Color(0xFFE5B45C)
    val DangerDark = Color(0xFFE8837A)
}

private val LightScheme = lightColorScheme(
    primary = VelmuxColors.Copper,
    onPrimary = Color.White,
    primaryContainer = VelmuxColors.Copper300,
    onPrimaryContainer = VelmuxColors.Copper700,
    secondary = VelmuxColors.Copper600,
    onSecondary = Color.White,
    secondaryContainer = VelmuxColors.PaperDim,
    onSecondaryContainer = VelmuxColors.Ink,
    tertiary = VelmuxColors.Warn,
    onTertiary = Color.White,
    background = VelmuxColors.Paper,
    onBackground = VelmuxColors.Ink,
    surface = Color.White,
    onSurface = VelmuxColors.Ink,
    surfaceVariant = VelmuxColors.PaperDim,
    onSurfaceVariant = VelmuxColors.InkSoft,
    error = VelmuxColors.Danger,
    onError = Color.White,
    outline = VelmuxColors.Copper300,
    outlineVariant = Color(0xFFEAD9CC),
)

private val DarkScheme = darkColorScheme(
    primary = VelmuxColors.Copper400,
    onPrimary = Color(0xFF241703),
    primaryContainer = VelmuxColors.Copper700,
    onPrimaryContainer = VelmuxColors.Copper300,
    secondary = VelmuxColors.Copper300,
    onSecondary = Color(0xFF241703),
    secondaryContainer = VelmuxColors.NightSurfaceHi,
    onSecondaryContainer = VelmuxColors.NightInk,
    tertiary = VelmuxColors.WarnDark,
    onTertiary = Color(0xFF221300),
    background = VelmuxColors.Night,
    onBackground = VelmuxColors.NightInk,
    surface = VelmuxColors.NightSurface,
    onSurface = VelmuxColors.NightInk,
    surfaceVariant = VelmuxColors.NightSurfaceHi,
    onSurfaceVariant = VelmuxColors.NightInkSoft,
    error = VelmuxColors.DangerDark,
    onError = Color(0xFF2B0906),
    outline = Color(0xFF514540),
    outlineVariant = Color(0xFF3A3129),
)

private val VelmuxTypography = Typography(
    headlineSmall = Typography().headlineSmall.copy(fontWeight = FontWeight.SemiBold),
    titleLarge = Typography().titleLarge.copy(fontWeight = FontWeight.SemiBold),
    titleMedium = Typography().titleMedium.copy(fontWeight = FontWeight.SemiBold),
    labelLarge = Typography().labelLarge.copy(
        fontWeight = FontWeight.Medium,
        fontSize = 14.sp,
    ),
)

/** The Velmux theme: light/dark/system via [themeMode]. */
@Composable
fun VelmuxTheme(
    themeMode: ThemeMode = ThemeMode.SYSTEM,
    content: @Composable () -> Unit,
) {
    val dark = when (themeMode) {
        ThemeMode.SYSTEM -> isSystemInDarkTheme()
        ThemeMode.LIGHT -> false
        ThemeMode.DARK -> true
    }
    MaterialTheme(
        colorScheme = if (dark) DarkScheme else LightScheme,
        typography = VelmuxTypography,
        content = content,
    )
}
