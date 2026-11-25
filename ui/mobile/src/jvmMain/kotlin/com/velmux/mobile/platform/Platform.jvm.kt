package com.velmux.mobile.platform

import java.io.File

/** JVM actuals: the host-verification target (no engine, no pickers). */
actual val platformName: String = "JVM (development)"

actual val platformDataDir: String =
    File(System.getProperty("user.home") ?: ".", "velmux-dev-data").absolutePath

actual fun shareFile(path: String): Boolean {
    // No platform share sheet on the JVM dev target.
    return false
}

actual fun openFile(path: String): Boolean {
    // No platform viewer on the JVM dev target.
    return false
}

actual fun deleteFile(path: String): Boolean = File(path).delete()

actual class SettingsStore actual constructor() {
    private val file = File(platformDataDir, "settings.json")

    actual fun load(): AppSettings? {
        if (!file.isFile) return null
        return try {
            val text = file.readText()
            decodeSettings(text)
        } catch (error: Exception) {
            null
        }
    }

    actual fun save(settings: AppSettings) {
        file.parentFile?.mkdirs()
        file.writeText(encodeSettings(settings))
    }
}

/** Minimal hand-rolled codec (the shared engine Json config is engine-only). */
internal fun encodeSettings(settings: AppSettings): String {
    fun mode(s: ThemeMode) = when (s) {
        ThemeMode.SYSTEM -> "system"
        ThemeMode.LIGHT -> "light"
        ThemeMode.DARK -> "dark"
    }
    return """
        {"themeMode":"${mode(settings.themeMode)}",
         "defaultGoalPreset":"${settings.defaultGoalPreset}",
         "defaultOutputContainer":"${settings.defaultOutputContainer}",
         "notifyOnCompletion":${settings.notifyOnCompletion},
         "notifyOnFailure":${settings.notifyOnFailure}}
    """.trimIndent().replace("\n", "")
}

internal fun decodeSettings(text: String): AppSettings {
    val mode = Regex("\"themeMode\"\\s*:\\s*\"(\\w+)\"").find(text)?.groupValues?.get(1)
    val goal = Regex("\"defaultGoalPreset\"\\s*:\\s*\"([^\"]+)\"").find(text)?.groupValues?.get(1)
    val container = Regex("\"defaultOutputContainer\"\\s*:\\s*\"([^\"]+)\"")
        .find(text)?.groupValues?.get(1)
    val completion = Regex("\"notifyOnCompletion\"\\s*:\\s*(true|false)")
        .find(text)?.groupValues?.get(1)?.toBoolean()
    val failure = Regex("\"notifyOnFailure\"\\s*:\\s*(true|false)")
        .find(text)?.groupValues?.get(1)?.toBoolean()
    return AppSettings(
        themeMode = when (mode) {
            "light" -> ThemeMode.LIGHT
            "dark" -> ThemeMode.DARK
            else -> ThemeMode.SYSTEM
        },
        defaultGoalPreset = goal ?: "balanced",
        defaultOutputContainer = container ?: "mp4",
        notifyOnCompletion = completion ?: true,
        notifyOnFailure = failure ?: true,
    )
}
