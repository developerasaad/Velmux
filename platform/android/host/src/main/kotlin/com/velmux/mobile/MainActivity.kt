package com.velmux.mobile

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import com.velmux.mobile.app.VelmuxApp

/**
 * The Compose entry point: renders the shared VelmuxApp() against the
 * process-owned engine client (singleTask + configChanges keep one
 * activity across orientation and UI-mode changes, so the engine client
 * and its poll loop are never torn down mid-conversion).
 */
class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val client = (application as VelmuxApplication).client
        setContent {
            VelmuxApp(client = client)
        }
    }
}
