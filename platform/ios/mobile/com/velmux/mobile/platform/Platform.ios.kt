package com.velmux.mobile.platform

import kotlinx.cinterop.ExperimentalForeignApi
import platform.Foundation.NSApplicationSupportDirectory
import platform.Foundation.NSFileManager
import platform.Foundation.NSSearchPathForDirectoriesInDomains
import platform.Foundation.NSURL
import platform.Foundation.NSTemporaryDirectory
import platform.Foundation.NSUserDomainMask
import platform.UIKit.UIActivityViewController
import platform.UIKit.UIDocumentInteractionController
import platform.UIKit.UIApplication
import platform.UIKit.UIViewController

@OptIn(ExperimentalForeignApi::class)
actual val platformName: String = "iOS"

/**
 * The engine's durable data directory: Application Support/VelmuxEngine —
 * the SAME canonical location the ObjC++ host's velmux_ios_shared_engine
 * uses (and the one the previous app version created, so existing durable
 * state survives the migration, mandate §39).
 */
actual val platformDataDir: String = run {
    val directories = NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory,
        NSUserDomainMask,
        true,
    )
    val base = (directories.firstOrNull() as? String) ?: NSTemporaryDirectory()
    val dir = "$base/VelmuxEngine"
    NSFileManager.defaultManager.createDirectoryAtPath(
        dir,
        withIntermediateDirectories = true,
        attributes = null,
        error = null,
    )
    dir
}

/**
 * Shares a managed output through the platform share sheet, presented
 * from the current top view controller.
 */
actual fun shareFile(path: String): Boolean = runCatchingUIKit {
    val url = NSURL.fileURLWithPath(path)
    val controller = UIActivityViewController(
        activityItems = listOf(url),
        applicationActivities = null,
    )
    topViewController()?.presentViewController(
        controller,
        animated = true,
        completion = null,
    )
    true
}

/**
 * Opens a managed output with the platform document interaction
 * controller (preview / "Open in...").
 */
actual fun openFile(path: String): Boolean = runCatchingUIKit {
    val url = NSURL.fileURLWithPath(path)
    val controller = UIDocumentInteractionController.interactionControllerWithURL(url)
    OpenFileHolder.controller = controller
    val anchor = topViewController()
    if (anchor != null) {
        controller.presentOptionsMenuFromRect(
            anchor.view.bounds,
            inView = anchor.view,
            animated = true,
        )
        true
    } else {
        false
    }
}

actual fun deleteFile(path: String): Boolean = runCatchingUIKit {
    NSFileManager.defaultManager.removeItemAtPath(path, error = null)
}

/** Retains the interaction controller (its delegate is weakly held). */
private object OpenFileHolder {
    var controller: UIDocumentInteractionController? = null
}

private fun topViewController(): UIViewController? {
    val application = UIApplication.sharedApplication
    val window = application.keyWindow
        ?: application.windows.firstOrNull() as? platform.UIKit.UIWindow
    return window?.rootViewController?.let(::topMost)
}

private fun topMost(controller: UIViewController): UIViewController {
    val presented = controller.presentedViewController ?: return controller
    return topMost(presented)
}

private inline fun <T> runCatchingUIKit(block: () -> T): Boolean = try {
    block()
} catch (error: Throwable) {
    false
}

/** NSUserDefaults-backed settings store. */
actual class SettingsStore actual constructor() {

    private fun defaults() = platform.Foundation.NSUserDefaults.standardUserDefaults

    actual fun load(): AppSettings? {
        val defaults = defaults()
        if (!defaults.boolForKey(KEY_PRESENT)) return null
        val mode = defaults.stringForKey(KEY_THEME)
        return AppSettings(
            themeMode = when (mode) {
                "light" -> ThemeMode.LIGHT
                "dark" -> ThemeMode.DARK
                else -> ThemeMode.SYSTEM
            },
            defaultGoalPreset = defaults.stringForKey(KEY_GOAL) ?: "balanced",
            defaultOutputContainer = defaults.stringForKey(KEY_CONTAINER) ?: "mp4",
            notifyOnCompletion = defaults.boolForKey(KEY_NOTIFY_COMPLETION),
            notifyOnFailure = defaults.boolForKey(KEY_NOTIFY_FAILURE),
        )
    }

    actual fun save(settings: AppSettings) {
        val defaults = defaults()
        defaults.setBool(true, KEY_PRESENT)
        defaults.setObject(
            when (settings.themeMode) {
                ThemeMode.SYSTEM -> "system"
                ThemeMode.LIGHT -> "light"
                ThemeMode.DARK -> "dark"
            },
            forKey = KEY_THEME,
        )
        defaults.setObject(settings.defaultGoalPreset, forKey = KEY_GOAL)
        defaults.setObject(settings.defaultOutputContainer, forKey = KEY_CONTAINER)
        defaults.setBool(settings.notifyOnCompletion, KEY_NOTIFY_COMPLETION)
        defaults.setBool(settings.notifyOnFailure, KEY_NOTIFY_FAILURE)
    }

    private companion object {
        const val KEY_PRESENT = "velmux.settings.present"
        const val KEY_THEME = "velmux.settings.themeMode"
        const val KEY_GOAL = "velmux.settings.defaultGoalPreset"
        const val KEY_CONTAINER = "velmux.settings.defaultOutputContainer"
        const val KEY_NOTIFY_COMPLETION = "velmux.settings.notifyOnCompletion"
        const val KEY_NOTIFY_FAILURE = "velmux.settings.notifyOnFailure"
    }
}
