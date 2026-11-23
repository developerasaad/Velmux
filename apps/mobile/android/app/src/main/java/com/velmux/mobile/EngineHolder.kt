package com.velmux.mobile

import android.content.Context
import android.content.Intent
import java.io.File
import java.util.Collections
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import org.json.JSONArray
import org.json.JSONObject

/**
 * Process-level owner of the Velmux engine (spec §22: the job engine must
 * be independent of React Native).
 *
 * The engine is a C++ library inside this process; the durable job state
 * lives in its SQLite store. This holder keeps the engine alive across
 * React instance teardowns (background → activity recreation → reload),
 * pumps the engine's bounded event queue at a coalesced cadence, and
 * distributes events to registered listeners:
 *
 *  - the React Native module (UI updates), and
 *  - the foreground ConversionService (notifications).
 *
 * Pump lifetime is state-driven: it runs while listeners are attached OR
 * the engine has active work, and exits after a bounded idle period — no
 * manual request/release pairing to leak. [wake] restarts it on demand.
 */
object EngineHolder {

  /** Bounded listener contract; see [dispatch]. */
  interface EventListener {
    fun onJobEvent(event: JSONObject)
  }

  private val engineLock = Any()
  private var engineHandle = 0L

  private val pumpExecutor =
    Executors.newSingleThreadExecutor { runnable ->
      Thread(runnable, "velmux-events").apply { isDaemon = true }
    }

  private val listeners: MutableSet<EventListener> =
    Collections.newSetFromMap(ConcurrentHashMap<EventListener, Boolean>())
  private val pumping = AtomicBoolean(false)

  // Latest state per job, updated from engine events. Read by the service
  // to decide foreground lifetime; the engine remains the source of truth.
  private val jobStates = ConcurrentHashMap<String, JSONObject>()

  private lateinit var appContext: Context

  /** Tracks whether the foreground service was requested; avoids duplicate
   * start calls when many events arrive. */
  @Volatile var serviceRunning = false

  fun initialize(context: Context) {
    if (::appContext.isInitialized) {
      return
    }
    appContext = context.applicationContext
  }

  // -- engine lifecycle --------------------------------------------------------

  /** The engine handle, created lazily in durable (non-cache) storage. */
  fun engine(): Long {
    synchronized(engineLock) {
      if (engineHandle == 0L) {
        val dataDir = File(appContext.filesDir, "velmux").apply { mkdirs() }
        engineHandle = VelmuxJni.nativeCreateEngine(dataDir.absolutePath)
        require(engineHandle != 0L) {
          "Velmux engine failed to initialize (no backend could start)"
        }
      }
      return engineHandle
    }
  }

  /** The engine is process-owned and never destroyed; null when absent. */
  fun engineHandleOrNull(): Long? = engineHandle.takeIf { it != 0L }

  // -- events --------------------------------------------------------------------

  fun addListener(listener: EventListener) {
    listeners.add(listener)
    wake()
  }

  fun removeListener(listener: EventListener) {
    listeners.remove(listener)
  }

  /** Restarts the event pump if it is not running (cheap when it is). */
  fun wake() {
    if (listeners.isNotEmpty() && pumping.compareAndSet(false, true)) {
      pumpExecutor.execute(::pumpLoop)
    }
  }

  private fun pumpLoop() {
    var idleRounds = 0
    while (true) {
      var sawEvent = false
      try {
        val handle = engine()
        val bytes = VelmuxJni.nativePollEvents(handle)
        val events = JSONArray(String(bytes, Charsets.UTF_8))
        for (i in 0 until events.length()) {
          sawEvent = true
          dispatch(events.getJSONObject(i))
        }
      } catch (error: IllegalStateException) {
        break // Engine unavailable (process teardown); stop quietly.
      } catch (error: Exception) {
        // A malformed poll response must not kill the loop.
      }
      // Keep pumping while anyone listens or work is active; exit after a
      // bounded idle period so nothing runs forever unnoticed. A terminal
      // event has already been dispatched by the time hasActiveWork()
      // turns false, so no transitions are lost by exiting.
      if (sawEvent || listeners.isNotEmpty() || hasActiveWork()) {
        idleRounds = 0
      } else {
        idleRounds++
        if (idleRounds >= IDLE_ROUNDS_BEFORE_EXIT) {
          break
        }
      }
      // Coalesced cadence: fast enough for progress, quiet enough for
      // the event pipe and notifications (superseded progress events may
      // be dropped by the engine under pressure; transitions never are).
      try {
        Thread.sleep(POLL_INTERVAL_MS)
      } catch (interrupted: InterruptedException) {
        Thread.currentThread().interrupt()
        break
      }
    }
    pumping.set(false)
    // Re-check in case a listener registered while we were exiting.
    wake()
  }

  private fun dispatch(event: JSONObject) {
    val jobId = event.optString("jobId")
    if (jobId.isNotEmpty()) {
      jobStates[jobId] = event
    }
    for (listener in listeners) {
      try {
        listener.onJobEvent(event)
      } catch (error: Exception) {
        // A broken listener must not break the pump.
      }
    }
    maybeStartService()
  }

  // -- job control (used by the service and the RN module) ------------------------

  fun cancel(jobId: String): Boolean {
    val handle = engineHandleOrNull() ?: return false
    return VelmuxJni.nativeCancelJob(handle, jobId)
  }

  /**
   * True while at least one durable job is in an execution state that
   * justifies a foreground service. Paused/interrupted jobs do not: the
   * engine idles, and resume re-starts the service.
   */
  fun hasActiveWork(): Boolean {
    for (event in jobStates.values) {
      when (event.optString("state")) {
        "preparing", "running", "checkpointing", "recovering" -> return true
      }
    }
    return false
  }

  /** Latest known event for a job (null when never seen). */
  fun lastEvent(jobId: String): JSONObject? = jobStates[jobId]

  private fun maybeStartService() {
    if (!::appContext.isInitialized || serviceRunning || !hasActiveWork()) {
      return
    }
    serviceRunning = true
    try {
      val intent = Intent(appContext, ConversionService::class.java)
      appContext.startForegroundService(intent)
    } catch (error: Exception) {
      // Android 12+ forbids foreground-service starts from the background.
      // Work always originates from user interaction in the app, but a
      // resumed job could race the app going away; the engine keeps
      // running in the process regardless and recovery covers the rest.
      serviceRunning = false
    }
  }

  private const val POLL_INTERVAL_MS = 250L
  // ~12s of fully idle polling before the pump thread exits.
  private const val IDLE_ROUNDS_BEFORE_EXIT = 48
}
