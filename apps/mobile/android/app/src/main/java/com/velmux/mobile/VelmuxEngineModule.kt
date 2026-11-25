package com.velmux.mobile

import android.content.Intent
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReadableMap
import com.facebook.react.bridge.WritableArray
import com.facebook.react.bridge.WritableMap
import com.facebook.react.modules.core.DeviceEventManagerModule
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
 * values to/from the engine's JSON contract and forwards job events to JS.
 *
 * The engine itself is process-owned (EngineHolder, spec §22): it survives
 * React instance teardown, and the foreground ConversionService keeps the
 * process alive while jobs run without the UI.
 */
class VelmuxEngineModule(reactContext: ReactApplicationContext) :
  NativeVelmuxSpec(reactContext) {

  private val appContext = reactContext.applicationContext

  // Sample generation is a synchronous engine call: it runs on its own
  // worker so neither the JS thread nor the event poller ever blocks.
  private val sampleExecutor =
    Executors.newSingleThreadExecutor { runnable ->
      Thread(runnable, "velmux-samples").apply { isDaemon = true }
    }
  private val jsListener = JsEventForwarder()
  @Volatile private var forwarding = false

  init {
    EngineHolder.initialize(appContext)
  }

  // -- helpers -------------------------------------------------------------------

  private fun callJson(block: (handle: Long) -> ByteArray): String {
    val bytes = block(EngineHolder.engine())
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
      EngineHolder.wake()
      promise.resolve(jobId)
    } catch (error: JSONException) {
      promise.reject("E_ENGINE", "malformed engine response", error)
    }
  }

  override fun cancelJob(jobId: String, promise: Promise) {
    promise.resolve(VelmuxJni.nativeCancelJob(EngineHolder.engine(), jobId))
  }

  override fun pauseJob(jobId: String, promise: Promise) {
    promise.resolve(VelmuxJni.nativePauseJob(EngineHolder.engine(), jobId))
  }

  override fun resumeJob(jobId: String, promise: Promise) {
    val result = VelmuxJni.nativeResumeJob(EngineHolder.engine(), jobId)
    if (result) {
      EngineHolder.wake()
    }
    promise.resolve(result)
  }

  override fun retryJob(jobId: String, promise: Promise) {
    val result = VelmuxJni.nativeRetryJob(EngineHolder.engine(), jobId)
    if (result) {
      EngineHolder.wake()
    }
    promise.resolve(result)
  }

  override fun deleteJob(jobId: String, promise: Promise) {
    promise.resolve(VelmuxJni.nativeDeleteJob(EngineHolder.engine(), jobId))
  }

  override fun updateResources(resources: ReadableMap, promise: Promise) {
    promise.resolve(
        VelmuxJni.nativeUpdateResources(EngineHolder.engine(), JsonBridge.toJsonString(resources)))
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
    promise.resolve(VelmuxJni.nativeIsJobActive(EngineHolder.engine(), jobId))
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

  /**
   * Durable, non-destructive import of a picked file (spec: file access).
   * The logic lives in [FileImport] (shared with the instrumented tests);
   * this wrapper only maps outcomes to the promise contract.
   */
  override fun importFile(uri: String, fileName: String, promise: Promise) {
    try {
      promise.resolve(FileImport.importFileInto(appContext, uri, fileName))
    } catch (error: Exception) {
      promise.reject("E_IMPORT", error.message ?: "the import failed", error)
    }
  }

  /**
   * Shares one engine output through the system sheet. The engine's
   * outputs live in app-private storage (filesDir/velmux/outputs), which
   * other apps cannot read; the FileProvider exposes exactly that tree as
   * granted content URIs (see the provider declaration in the manifest —
   * the only storage-related surface the app has, and it needs no
   * permissions). No storage permissions are involved.
   */
  override fun shareOutput(outputPath: String, mimeType: String, title: String, promise: Promise) {
    try {
      val file = java.io.File(outputPath)
      if (!file.isFile) {
        promise.resolve(false)
        return
      }
      val authority = "${appContext.packageName}.fileprovider"
      val uri = androidx.core.content.FileProvider.getUriForFile(appContext, authority, file)
      val send = Intent(Intent.ACTION_SEND).apply {
        type = if (mimeType.isBlank()) "application/octet-stream" else mimeType
        putExtra(Intent.EXTRA_STREAM, uri)
        putExtra(Intent.EXTRA_TITLE, title)
        // Grant the receiving app read access to exactly this file.
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
      }
      val chooser = Intent.createChooser(send, title).apply {
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
      }
      val activity = reactApplicationContext.currentActivity
      if (activity == null) {
        promise.resolve(false)
        return
      }
      activity.startActivity(chooser)
      promise.resolve(true)
    } catch (error: Exception) {
      promise.resolve(false)
    }
  }

  // -- events -------------------------------------------------------------------------

  /** Forwards engine events to JS while the JS listener count is > 0. */
  private inner class JsEventForwarder : EngineHolder.EventListener {
    override fun onJobEvent(event: JSONObject) {
      if (!forwarding) {
        return
      }
      val emitter = reactApplicationContext
        .getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter::class.java)
      emitter.emit(JOB_EVENT_NAME, JsonBridge.toWritableMap(event))
    }
  }

  private val jsListenerCount = AtomicInteger(0)

  override fun addListener(eventName: String, promise: Promise) {
    if (jsListenerCount.incrementAndGet() == 1) {
      forwarding = true
      EngineHolder.addListener(jsListener)
    }
    promise.resolve(null)
  }

  override fun removeListeners(count: Double, promise: Promise) {
    if (jsListenerCount.updateAndGet { old ->
        (old - count.toInt()).coerceAtLeast(0)
      } == 0 && forwarding
    ) {
      forwarding = false
      EngineHolder.removeListener(jsListener)
    }
    promise.resolve(null)
  }

  override fun invalidate() {
    EngineHolder.removeListener(jsListener)
    forwarding = false
    sampleExecutor.shutdown()
    super.invalidate()
  }

  private companion object {
    const val JOB_EVENT_NAME = "velmux.jobEvent"
  }
}
