package com.velmux.mobile

import android.app.Application
import android.util.Log
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
 *
 * onCreate also probes the engine facade (ABI + engine version through
 * the JNI library that System.loadLibrary just mapped) and logs the
 * result: the automated launch verification greps that marker to prove
 * the app process reached a working engine, not merely a live UI.
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
        logEngineInitMarker()
    }

    override fun onTerminate() {
        client.dispose()
        super.onTerminate()
    }

    /**
     * Loads libvelmux_jni (its JNI_OnLoad registers the native surface)
     * and reads the frozen ABI + engine versions through it. Failure to
     * log the marker fails the automated launch check — by design: a
     * process alive with a dead engine surface is a launch failure.
     */
    private fun logEngineInitMarker() {
        try {
            val abi = VelmuxJni.nativeAbiVersion()
            val engine = VelmuxJni.nativeEngineVersion().decodeToString()
            Log.i(TAG, "app init: engine ready (abi $abi, engine $engine)")
        } catch (t: Throwable) {
            Log.e(TAG, "app init: engine surface unavailable", t)
        }
    }

    private companion object {
        const val TAG = "Velmux"
    }
}
