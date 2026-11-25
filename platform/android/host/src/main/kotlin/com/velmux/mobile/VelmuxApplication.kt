package com.velmux.mobile

import android.app.Application
import com.velmux.mobile.platform.AppRuntime
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob

/**
 * Process-level host of the shared engine client (mandate §10.1: ONE
 * engine instance per application process; UI screens attach and detach
 * without ever creating another).
 *
 * The client lives here — surviving activity recreation and config
 * changes — and starts its poll loop in a process scope. MainActivity
 * only renders the shared VelmuxApp() composable against this client.
 */
class VelmuxApplication : Application() {

    lateinit var client: EngineClient
        private set

    override fun onCreate() {
        super.onCreate()
        AppRuntime.attach(this)
        client = EngineClient(
            transport = EngineTransport(com.velmux.mobile.platform.platformDataDir),
        )
        client.start(CoroutineScope(SupervisorJob() + Dispatchers.Default))
    }

    override fun onTerminate() {
        client.dispose()
        super.onTerminate()
    }
}
