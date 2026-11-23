package com.velmux.mobile

import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReadableMap
import com.facebook.react.bridge.WritableArray
import com.facebook.react.bridge.WritableMap
import com.facebook.react.modules.core.DeviceEventManagerModule
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject

/**
 * TurboModule `VelmuxEngine` implementation.
 *
 * Owns no media policy: every decision is made by the C++ core reached
 * through the stable bridge facade. This class translates typed bridge
 * values to/from the engine's JSON contract and forwards job events to JS
 * by polling the engine's bounded event queue (coalesced to UI cadence).
 *
 * The engine lives in the app's durable storage so committed work survives
 * process death; the C++ core owns recovery.
 */
class VelmuxEngineModule(reactContext: ReactApplicationContext) :
  NativeVelmuxSpec(reactContext) {

  private val appContext = reactContext.applicationContext
  private val engineLock = Any()
  private var engineHandle: Long = 0L

  private val eventExecutor =
    Executors.newSingleThreadExecutor { runnable ->
      Thread(runnable, "velmux-events").apply { isDaemon = true }
    }

  // Sample generation is a synchronous engine call: it runs on its own
  // worker so neither the JS thread nor the event poller ever blocks.
  private val sampleExecutor =
    Executors.newSingleThreadExecutor { runnable ->
      Thread(runnable, "velmux-samples").apply { isDaemon = true }
    }
  private val listenerCount = AtomicInteger(0)
  @Volatile private var polling = false

  // -- engine lifecycle --------------------------------------------------------

  private fun engine(): Long {
    synchronized(engineLock) {
      if (engineHandle == 0L) {
        val dataDir = File(appContext.filesDir, "velmux").apply { mkdirs() }
        engineHandle = VelmuxJni.createEngine(dataDir)
        require(engineHandle != 0L) {
          "Velmux engine failed to initialize (no backend could start)"
        }
      }
      return engineHandle
    }
  }

  private fun destroyEngine() {
    synchronized(engineLock) {
      if (engineHandle != 0L) {
        VelmuxJni.nativeDestroyEngine(engineHandle)
        engineHandle = 0L
      }
    }
  }

  // -- helpers -------------------------------------------------------------------

  private fun callJson(block: (handle: Long) -> ByteArray): String {
    val bytes = block(engine())
    return String(bytes, Charsets.UTF_8)
  }

  /** Resolves or rejects a promise from an engine JSON response. */
  private fun complete(promise: Promise, json: String) {
    try {
      val parsed = JSONObject(json)
      if (parsed.has("error")) {
        promise.reject("E_ENGINE", parsed.optString("error", "engine error"))
        return
      }
      promise.resolve(JsonBridge.toWritableMap(parsed))
    } catch (error: JSONException) {
      promise.reject("E_ENGINE", "malformed engine response", error)
    }
  }

  private fun requestJson(request: ReadableMap): String =
    JsonBridge.toJsonString(request)

  // -- TurboModule contract --------------------------------------------------------

  override fun inspect(source: ReadableMap, promise: Promise) {
    val uri = source.getString("uri")
    if (uri.isNullOrBlank()) {
      promise.reject("E_ARGUMENTS", "source.uri is required")
      return
    }
    val displayName = source.getString("displayName") ?: "Untitled media"
    complete(promise, callJson { handle ->
      VelmuxJni.nativeInspect(handle, uri, displayName)
    })
  }

  override fun plan(request: ReadableMap, promise: Promise) {
    complete(promise, callJson { handle ->
      VelmuxJni.nativePlan(handle, requestJson(request))
    })
  }

  override fun generateSample(
    request: ReadableMap,
    sampleSeconds: Double,
    promise: Promise
  ) {
    // Sample generation is a synchronous engine call: run it on the
    // module's background executor so the JS thread never blocks.
    sampleExecutor.execute {
      try {
        complete(promise, callJson { handle ->
          VelmuxJni.nativeGenerateSample(
            handle,
            requestJson(request),
            sampleSeconds.toInt().coerceAtLeast(1)
          )
        })
      } catch (error: RuntimeException) {
        promise.reject("E_ENGINE", "sample generation failed", error)
      }
    }
  }

  override fun startJob(request: ReadableMap, promise: Promise) {
    val json = callJson { handle ->
      VelmuxJni.nativeStartJob(handle, requestJson(request))
    }
    try {
      val parsed = JSONObject(json)
      if (parsed.has("error")) {
        promise.reject("E_ENGINE", parsed.optString("error", "engine error"))
        return
      }
      val jobId = parsed.optString("jobId")
      if (jobId.isEmpty()) {
        promise.reject("E_ENGINE", "engine returned no job id")
        return
      }
      ensurePolling()
      promise.resolve(jobId)
    } catch (error: JSONException) {
      promise.reject("E_ENGINE", "malformed engine response", error)
    }
  }

  override fun cancelJob(jobId: String, promise: Promise) {
    promise.resolve(VelmuxJni.nativeCancelJob(engine(), jobId))
  }

  override fun pauseJob(jobId: String, promise: Promise) {
    promise.resolve(VelmuxJni.nativePauseJob(engine(), jobId))
  }

  override fun resumeJob(jobId: String, promise: Promise) {
    val result = VelmuxJni.nativeResumeJob(engine(), jobId)
    if (result) {
      ensurePolling()
    }
    promise.resolve(result)
  }

  override fun retryJob(jobId: String, promise: Promise) {
    val result = VelmuxJni.nativeRetryJob(engine(), jobId)
    if (result) {
      ensurePolling()
    }
    promise.resolve(result)
  }

  override fun deleteJob(jobId: String, promise: Promise) {
    promise.resolve(VelmuxJni.nativeDeleteJob(engine(), jobId))
  }

  override fun updateResources(resources: ReadableMap, promise: Promise) {
    promise.resolve(
        VelmuxJni.nativeUpdateResources(engine(), JsonBridge.toJsonString(resources)))
  }

  override fun getJob(jobId: String, promise: Promise) {
    val json = callJson { handle -> VelmuxJni.nativeGetJob(handle, jobId) }
    if (json.trim() == "null") {
      promise.resolve(null)
      return
    }
    complete(promise, json)
  }

  override fun isJobActive(jobId: String, promise: Promise) {
    promise.resolve(VelmuxJni.nativeIsJobActive(engine(), jobId))
  }

  override fun listJobs(promise: Promise) {
    val json = callJson { handle -> VelmuxJni.nativeListJobs(handle) }
    try {
      val array = JSONArray(json)
      val result: WritableArray = Arguments.createArray()
      for (i in 0 until array.length()) {
        val snapshot: WritableMap =
          JsonBridge.toWritableMap(array.getJSONObject(i))
        result.pushMap(snapshot)
      }
      promise.resolve(result)
    } catch (error: JSONException) {
      promise.reject("E_ENGINE", "malformed engine response", error)
    }
  }

  override fun describeBackends(promise: Promise) {
    val json = callJson { handle -> VelmuxJni.nativeDescribeBackends(handle) }
    try {
      val array = JSONArray(json)
      val result: WritableArray = Arguments.createArray()
      for (i in 0 until array.length()) {
        result.pushString(array.optString(i))
      }
      promise.resolve(result)
    } catch (error: JSONException) {
      promise.reject("E_ENGINE", "malformed engine response", error)
    }
  }

  // -- events -------------------------------------------------------------------------

  override fun addListener(eventName: String, promise: Promise) {
    listenerCount.incrementAndGet()
    ensurePolling()
    promise.resolve(null)
  }

  override fun removeListeners(count: Double, promise: Promise) {
    val remaining = listenerCount.updateAndGet { old ->
      (old - count.toInt()).coerceAtLeast(0)
    }
    if (remaining == 0) {
      polling = false
    }
    promise.resolve(null)
  }

  private fun ensurePolling() {
    if (polling) {
      return
    }
    polling = true
    eventExecutor.execute(::pollLoop)
  }

  private fun pollLoop() {
    // Coalesced cadence: fast enough for progress, quiet enough for the
    // event pipe and notifications (the engine may drop superseded events
    // under pressure; state transitions are never lost).
    while (polling && listenerCount.get() > 0) {
      try {
        val handle = engine()
        val bytes = VelmuxJni.nativePollEvents(handle)
        val text = String(bytes, Charsets.UTF_8)
        val events = JSONArray(text)
        if (events.length() > 0) {
          emitEvents(events)
        }
      } catch (error: IllegalStateException) {
        // Engine unavailable mid-poll (teardown); stop quietly.
        break
      } catch (error: JSONException) {
        // A malformed poll response must not kill the loop.
      }
      try {
        Thread.sleep(POLL_INTERVAL_MS)
      } catch (interrupted: InterruptedException) {
        Thread.currentThread().interrupt()
        break
      }
    }
    polling = false
  }

  private fun emitEvents(events: JSONArray) {
    val emitter = reactApplicationContext
      .getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter::class.java)
    for (i in 0 until events.length()) {
      val payload = events.getJSONObject(i)
      emitter.emit(JOB_EVENT_NAME, JsonBridge.toWritableMap(payload))
    }
  }

  override fun invalidate() {
    polling = false
    listenerCount.set(0)
    eventExecutor.shutdown()
    destroyEngine()
    super.invalidate()
  }

  private companion object {
    const val JOB_EVENT_NAME = "velmux.jobEvent"
    const val POLL_INTERVAL_MS = 250L
  }
}
