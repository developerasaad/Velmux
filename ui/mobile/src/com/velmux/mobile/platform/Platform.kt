package com.velmux.mobile.platform

import kotlinx.coroutines.flow.StateFlow

/** Application-wide settings persisted per platform (Settings screen). */
expect class SettingsStore() {
    /** Loads the persisted settings (null when none stored yet). */
    fun load(): AppSettings?

    /** Persists the settings. */
    fun save(settings: AppSettings)
}

/** How the app follows the system dark/light setting. */
enum class ThemeMode { SYSTEM, LIGHT, DARK }

/** Persisted user preferences (v1 scope of the Settings screen). */
data class AppSettings(
    val themeMode: ThemeMode = ThemeMode.SYSTEM,
    /** Default compression goal preset (balanced / high-quality / smaller-file / target-size). */
    val defaultGoalPreset: String = "balanced",
    /** Default output container id (mp4 / mkv / ...). */
    val defaultOutputContainer: String = "mp4",
    val notifyOnCompletion: Boolean = true,
    val notifyOnFailure: Boolean = true,
)

/** Diagnostics + platform integration surface. */
expect val platformName: String

/** The engine's durable data directory on this platform. */
expect val platformDataDir: String

/** Shares a managed output file through the platform share sheet. */
expect fun shareFile(path: String): Boolean

/** Opens a managed output file with the platform viewer. */
expect fun openFile(path: String): Boolean

/** Deletes a managed output file (record cleanup keeps engine + disk honest). */
expect fun deleteFile(path: String): Boolean
