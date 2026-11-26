package com.velmux.mobile.app

import androidx.compose.ui.window.ComposeUIViewController
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.EngineTransport
import com.velmux.mobile.platform.platformDataDir
import platform.UIKit.UIViewController

/**
 * The Compose Multiplatform entry point for the iOS host (mandate §35):
 * the Swift AppDelegate/SceneDelegate call this through the shared
 * framework (import Velmux; MainViewControllerKt.MainViewController()).
 *
 * The engine client is created exactly once per view controller root and
 * owned by the composition (VelmuxApp starts/stops it with its lifecycle)
 * — the same single-engine-per-process contract Android follows through
 * VelmuxApplication.
 */
fun MainViewController(): UIViewController {
    val client = EngineClient(transport = EngineTransport(platformDataDir))
    return ComposeUIViewController { VelmuxApp(client = client) }
}
