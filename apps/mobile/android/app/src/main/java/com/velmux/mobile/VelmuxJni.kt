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
  external fun nativeStartJob(handle: Long, requestJson: String): ByteArray
  external fun nativeGetJob(handle: Long, jobId: String): ByteArray
  external fun nativeListJobs(handle: Long): ByteArray
  external fun nativeDescribeBackends(handle: Long): ByteArray
  external fun nativePollEvents(handle: Long): ByteArray
  external fun nativeCancelJob(handle: Long, jobId: String): Boolean
  external fun nativePauseJob(handle: Long, jobId: String): Boolean
  external fun nativeResumeJob(handle: Long, jobId: String): Boolean

  /** Creates the engine in the app's durable (non-cache) storage. */
  fun createEngine(dataDir: File): Long = nativeCreateEngine(dataDir.absolutePath)
}
