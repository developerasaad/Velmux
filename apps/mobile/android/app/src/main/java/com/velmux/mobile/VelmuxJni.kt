package com.velmux.mobile

import java.io.File

/**
 * Thin Kotlin binding to the Velmux C++ engine through the stable C facade
 * (bridge/cpp/velmux_bridge.h), reached via JNI (RegisterNatives).
 *
 * All JSON payloads cross as UTF-8 byte arrays for full Unicode fidelity.
 * This object is intentionally dumb: parsing and policy live in
 * [VelmuxEngineModule]; the engine and its decisions live in C++.
 */
object VelmuxJni {
  init {
    System.loadLibrary("velmux_jni")
  }

  external fun nativeCreateEngine(dataDir: String): Long
  external fun nativeDestroyEngine(handle: Long)
  external fun nativeInspect(handle: Long, uri: String, displayName: String): ByteArray
  external fun nativePlan(handle: Long, requestJson: String): ByteArray
  external fun nativeGenerateSample(
    handle: Long,
    requestJson: String,
    sampleSeconds: Int
  ): ByteArray
  external fun nativeStartJob(handle: Long, requestJson: String): ByteArray
  external fun nativeGetJob(handle: Long, jobId: String): ByteArray
  external fun nativeIsJobActive(handle: Long, jobId: String): Boolean
  external fun nativeListJobs(handle: Long): ByteArray
  external fun nativeDescribeBackends(handle: Long): ByteArray
  external fun nativePollEvents(handle: Long): ByteArray
  external fun nativeCancelJob(handle: Long, jobId: String): Boolean
  external fun nativePauseJob(handle: Long, jobId: String): Boolean
  external fun nativeResumeJob(handle: Long, jobId: String): Boolean
  external fun nativeRetryJob(handle: Long, jobId: String): Boolean
  external fun nativeDeleteJob(handle: Long, jobId: String): Boolean
  external fun nativeUpdateResources(handle: Long, resourcesJson: String): Boolean

  /** Creates the engine in the app's durable (non-cache) storage. */
  fun createEngine(dataDir: File): Long = nativeCreateEngine(dataDir.absolutePath)

  // ---- TEST SCAFFOLDING (hybrid instrumented tests) -------------------------
  // Never called by the app: HybridConversionTest drives these to prove the
  // three hybrid (stage-composed) execution categories with real media on a
  // device/emulator. All media work is real — the entry points only SELECT
  // routes (see the TEST SCAFFOLDING section in velmux_jni.cpp).

  /**
   * Runs the real core HybridExecutor over an all-Android stage assignment
   * (MediaCodec stage decode -> MediaCodec stage encode -> AMediaMuxer, with
   * the source's audio stream-copied when the request keeps audio).
   *
   * [outputPath] is the output file the mux stage writes. Returns
   * `{"ok":true,"frames":N}` or `{"error":"..."}` as UTF-8 bytes.
   */
  external fun nativeTestRunAndroidStagePipeline(
    requestJson: String,
    outputPath: String
  ): ByteArray

  /**
   * Runs a conversion through the real engine stack (planner + JobManager +
   * scheduler) built exactly like the app facade, but with the two real
   * backends wrapped per [maskMode]: a comma-separated list of tokens
   * "ffmpeg-decoders", "ffmpeg-encoders", "android-whole-pipeline". The
   * masks only narrow the capability ROUTES the planner may choose; every
   * demux/decode/encode/mux still runs the real backend code.
   *
   * [outputPath] is the working directory for the test engine (durable
   * store; the job's output lands under `<dir>/outputs` and must be created
   * by the caller). The ACTUAL output file path is reported in the returned
   * JSON as "outputPath", together with "ok", "state", "error",
   * "pipelineSummary" and "outputSizeBytes".
   *
   * Returns `{"error":"ffmpeg-absent"}` when the app was built without the
   * FFmpeg backend — callers treat that as an honest skip, not a failure.
   */
  external fun nativeTestStartJobMaskedEngine(
    requestJson: String,
    outputPath: String,
    maskMode: String
  ): ByteArray
}
