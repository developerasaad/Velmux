package com.velmux.mobile

import android.content.Context
import android.content.Intent
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.JobEvent
import com.velmux.mobile.engine.JobStates
import java.util.Collections
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import org.json.JSONObject

/**
 * Process-level platform-surface hub (mandate §10.1: ONE engine, ONE event
 * pump per process).
 *
 * The engine handle is owned by the shared [EngineClient]'s transport
 * (a process-wide singleton) and the client's poll loop is the ONE event
 * pump. This holder binds to that client (VelmuxApplication binds it at
 * process start) and:
 *
 *  - fans the client's coalesced event batches out to registered platform
 *    listeners (the foreground ConversionService renders notifications);
 *  - starts the ConversionService when work becomes active and lets it
 *    stop itself when the engine idles;
 *  - routes job control (cancel intents) back through the client.
 *
 * It owns no engine handle and never polls the engine itself — a second
 * pump would race the client's for the engine's bounded event queue.
 */
object EngineHolder {

  /** Bounded listener contract; see [dispatch]. */
  interface EventListener {
    fun onJobEvent(event: JSONObject)
  }

  private val listenerLock = Any()
  private val listeners: MutableSet<EventListener> =
    Collections.newSetFromMap(ConcurrentHashMap<EventListener, Boolean>())

  // Latest state per job, updated from the client's event batches. Read by
  // the service to decide foreground lifetime; the engine remains the
  // source of truth.
  private val jobStates = ConcurrentHashMap<String, JSONObject>()

  private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

  @Volatile private var boundClient: EngineClient? = null
  private lateinit var appContext: Context

  /** Tracks whether the foreground service was requested; avoids duplicate
   * start calls when many events arrive. */
  @Volatile var serviceRunning = false

  /**
   * Binds the process client once (VelmuxApplication.onCreate). Collects
   * the client's event batches and job states for as long as the process
   * lives; idempotent.
   */
  fun bind(client: EngineClient, context: Context) {
    if (boundClient != null) return
    synchronized(this) {
      if (boundClient != null) return
      boundClient = client
      appContext = context.applicationContext
    }
    scope.launch {
      client.eventBatches.collect { batch ->
        for (event in batch) {
          dispatch(event)
        }
      }
    }
  }

  /** The bound process client (null before VelmuxApplication.onCreate). */
  fun clientOrNull(): EngineClient? = boundClient

  // -- events --------------------------------------------------------------------

  fun addListener(listener: EventListener) {
    synchronized(listenerLock) {
      listeners.add(listener)
    }
    replayStatesFor(listener)
  }

  fun removeListener(listener: EventListener) {
    synchronized(listenerLock) {
      listeners.remove(listener)
    }
  }

  /**
   * A newly attached listener first sees the LAST known state of every
   * active job, so a service restarted mid-work renders correctly without
   * waiting for the next event.
   */
  private fun replayStatesFor(listener: EventListener) {
    for (event in jobStates.values) {
      deliverTo(listener, event)
    }
  }

  private fun dispatch(event: JobEvent) {
    val payload = JSONObject().apply {
      put("jobId", event.jobId)
      put("state", event.state)
      put("progress", event.progress)
      put("outputSizeBytes", event.outputSizeBytes)
      event.error?.let { put("error", it) }
      event.errorCode?.let { put("errorCode", it) }
    }
    if (event.jobId.isNotEmpty()) {
      jobStates[event.jobId] = payload
    }
    synchronized(listenerLock) {
      for (listener in listeners) {
        deliverTo(listener, payload)
      }
    }
    maybeStartService()
  }

  private fun deliverTo(listener: EventListener, event: JSONObject) {
    try {
      listener.onJobEvent(event)
    } catch (error: Exception) {
      // A broken listener must not break the fan-out.
    }
  }

  // -- job control (used by the service and cancel intents) ----------------------

  fun cancel(jobId: String) {
    scope.launch {
      boundClient?.cancelJob(jobId)
    }
  }

  /**
   * True while at least one durable job is in an execution state that
   * justifies a foreground service (JobStates.ACTIVE minus QUEUED — merely
   * queued work does not yet hold a worker). Paused/interrupted jobs do
   * not: the engine idles, and resume re-starts the service.
   */
  fun hasActiveWork(): Boolean {
    for (event in jobStates.values) {
      val state = event.optString("state")
      if (state in FOREGROUND_STATES) return true
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

  /** States that justify holding the foreground service. */
  private val FOREGROUND_STATES: Set<String> =
    JobStates.ACTIVE.filter { it != JobStates.QUEUED }.toSet()
}
