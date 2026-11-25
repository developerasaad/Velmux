package com.velmux.mobile.platform

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.core.content.FileProvider
import java.io.File

/**
 * Process runtime anchor for the androidMain actuals (the shared library
 * code cannot see the host module's classes; the host application
 * attaches the app context here during startup — see
 * platform/android/host/src/main/kotlin/.../VelmuxApplication.kt).
 */
object AppRuntime {

    @Volatile
    private var appContext: Context? = null

    fun attach(context: Context) {
        appContext = context.applicationContext
    }

    fun context(): Context = requireNotNull(appContext) {
        "AppRuntime.attach(context) must run during application startup"
    }
}

/** The engine's durable (non-cache) data directory: filesDir/velmux. */
val AppRuntimeDataDir: String
    get() = File(AppRuntime.context().filesDir, "velmux").absolutePath

actual val platformName: String = "Android"

actual val platformDataDir: String
    get() = AppRuntimeDataDir

/**
 * Shares a managed output through the system share sheet. Only the app's
 * OWN output tree (filesDir/velmux) is exposed — as a granted per-invocation
 * content URI through FileProvider (res/xml/file_paths.xml); the app holds
 * no storage permissions.
 */
actual fun shareFile(path: String): Boolean {
    val context = AppRuntime.context()
    return try {
        val file = File(path)
        if (!file.isFile) return false
        val authority = "${context.packageName}.fileprovider"
        val uri: Uri = FileProvider.getUriForFile(context, authority, file)
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = guessContentType(path)
            putExtra(Intent.EXTRA_STREAM, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        context.startActivity(
            Intent.createChooser(intent, null).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        )
        true
    } catch (error: Exception) {
        false
    }
}

/** Opens a managed output with the platform viewer (granted content URI). */
actual fun openFile(path: String): Boolean {
    val context = AppRuntime.context()
    return try {
        val file = File(path)
        if (!file.isFile) return false
        val authority = "${context.packageName}.fileprovider"
        val uri: Uri = FileProvider.getUriForFile(context, authority, file)
        val intent = Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(uri, guessContentType(path))
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        context.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        true
    } catch (error: Exception) {
        false
    }
}

actual fun deleteFile(path: String): Boolean = File(path).delete()

private fun guessContentType(path: String): String {
    val extension = path.substringAfterLast('.', "").lowercase()
    return when (extension) {
        "mp4", "m4v", "mov", "mkv", "webm", "avi", "3gp", "mpegts", "flv" -> "video/*"
        "m4a", "mp3", "ogg", "wav", "flac", "opus", "aac" -> "audio/*"
        "jpg", "jpeg", "png", "webp", "bmp" -> "image/*"
        "srt", "vtt", "ass", "ssa", "ttml" -> "text/*"
        else -> "application/octet-stream"
    }
}

/** SharedPreferences-backed settings store. */
actual class SettingsStore actual constructor() {

    private fun prefs() =
        AppRuntime.context().getSharedPreferences("velmux.settings", Context.MODE_PRIVATE)

    actual fun load(): AppSettings? {
        val prefs = prefs()
        if (!prefs.contains(KEY_THEME)) return null
        return AppSettings(
            themeMode = when (prefs.getString(KEY_THEME, "system")) {
                "light" -> ThemeMode.LIGHT
                "dark" -> ThemeMode.DARK
                else -> ThemeMode.SYSTEM
            },
            defaultGoalPreset = prefs.getString(KEY_GOAL, "balanced") ?: "balanced",
            defaultOutputContainer = prefs.getString(KEY_CONTAINER, "mp4") ?: "mp4",
            notifyOnCompletion = prefs.getBoolean(KEY_NOTIFY_COMPLETION, true),
            notifyOnFailure = prefs.getBoolean(KEY_NOTIFY_FAILURE, true),
        )
    }

    actual fun save(settings: AppSettings) {
        prefs().edit()
            .putString(KEY_THEME, when (settings.themeMode) {
                ThemeMode.SYSTEM -> "system"
                ThemeMode.LIGHT -> "light"
                ThemeMode.DARK -> "dark"
            })
            .putString(KEY_GOAL, settings.defaultGoalPreset)
            .putString(KEY_CONTAINER, settings.defaultOutputContainer)
            .putBoolean(KEY_NOTIFY_COMPLETION, settings.notifyOnCompletion)
            .putBoolean(KEY_NOTIFY_FAILURE, settings.notifyOnFailure)
            .apply()
    }

    private companion object {
        const val KEY_THEME = "themeMode"
        const val KEY_GOAL = "defaultGoalPreset"
        const val KEY_CONTAINER = "defaultOutputContainer"
        const val KEY_NOTIFY_COMPLETION = "notifyOnCompletion"
        const val KEY_NOTIFY_FAILURE = "notifyOnFailure"
    }
}
