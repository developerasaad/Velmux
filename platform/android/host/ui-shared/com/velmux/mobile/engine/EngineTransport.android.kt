package com.velmux.mobile.engine

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File

/**
 * Android actual: JNI against the EXISTING platform/android/interop
 * surface (VelmuxJni, RegisterNatives; JSON crosses as UTF-8 bytes; the
 * handle travels as jlong). Calls run on Dispatchers.IO — JNI is
 * synchronous under the hood.
 *
 * COVERAGE — honest report (task UI-1, reported not fixed):
 *  - bound today: engine create/destroy, inspect, job list/get/is-active/
 *    cancel/pause/resume/retry/delete, describe backends, poll events,
 *    update resources;
 *  - NOT bound: the ABI v2 SESSION functions (velmux_create_session,
 *    prepare/snapshot/options/update/sample/submit/close_session),
 *    velmux_submit_job, velmux_reuse_job_settings, velmux_abi_version and
 *    velmux_engine_version. The committed JNI adapter
 *    (platform/android/interop/velmux_jni.cpp) predates ABI v2: it still
 *    references velmux_plan/velmux_generate_sample/velmux_start_job —
 *    symbols that no longer exist in core/include/velmux/abi.h — and its
 *    Kotlin surface has no session externals. Regenerating that adapter
 *    for the frozen ABI is an Android follow-up (C++ side); until then
 *    these calls return an explicit unsupported-operation error so the
 *    shared client surfaces an honest message instead of pretending.
 */
actual open class EngineTransport actual constructor(
    private val dataDir: String,
) {

    private val engineMutex = Mutex()

    @Volatile
    private var engineHandle: Long = 0L

    private suspend fun engine(): Long = engineMutex.withLock {
        if (engineHandle != 0L) return engineHandle
        withContext(Dispatchers.IO) {
            val handle = VelmuxJni.createEngine(File(dataDir))
            if (handle == 0L) {
                throw EngineException(
                    EngineErrorCode.ENGINE_UNAVAILABLE,
                    "The Velmux engine failed to initialize " +
                        "(unusable data directory or no backend started)",
                )
            }
            engineHandle = handle
            handle
        }
    }

    private fun missing(what: String): Nothing = throw EngineException(
        EngineErrorCode.UNSUPPORTED_OPERATION,
        "$what is not exposed by the current Android JNI adapter " +
            "(pre-ABI-v2 surface; the adapter regeneration is the Android follow-up)",
    )

    // ---- ABI version ----------------------------------------------------------

    actual open suspend fun abiVersion(): Int = withContext(Dispatchers.IO) {
        missing("velmux_abi_version")
    }

    actual open suspend fun engineVersion(): String = withContext(Dispatchers.IO) {
        missing("velmux_engine_version")
    }

    // ---- sessions (JNI surface gap — honest unsupported-operation) --------------

    actual open suspend fun createSession(
        operation: String,
        sources: List<MediaSourceRequest>,
    ): String = withContext(Dispatchers.IO) {
        missing("velmux_create_session")
    }

    actual open suspend fun prepareSession(sessionId: String): SessionSnapshot =
        withContext(Dispatchers.IO) {
            missing("velmux_prepare_session")
        }

    actual open suspend fun getSessionSnapshot(sessionId: String): SessionSnapshot =
        withContext(Dispatchers.IO) {
            missing("velmux_get_session_snapshot")
        }

    actual open suspend fun getSessionOptions(
        sessionId: String,
        field: String,
        revision: Long,
    ): FieldOptions = withContext(Dispatchers.IO) {
        missing("velmux_get_session_options")
    }

    actual open suspend fun updateSession(
        sessionId: String,
        request: SessionUpdateRequest,
    ): SessionSnapshot = withContext(Dispatchers.IO) {
        missing("velmux_update_session")
    }

    actual open suspend fun generateSample(sessionId: String, revision: Long): SampleResult =
        withContext(Dispatchers.IO) {
            missing("velmux_generate_session_sample")
        }

    actual open suspend fun submitSession(
        sessionId: String,
        revision: Long,
        submissionId: String,
    ): String = withContext(Dispatchers.IO) {
        missing("velmux_submit_session")
    }

    actual open suspend fun closeSession(sessionId: String): Boolean =
        withContext(Dispatchers.IO) {
            missing("velmux_close_session")
        }

    actual open suspend fun submitJob(requestJson: String): String =
        withContext(Dispatchers.IO) {
            missing("velmux_submit_job")
        }

    // ---- jobs (bound) ------------------------------------------------------------

    actual open suspend fun listJobs(): List<JobSnapshot> = withContext(Dispatchers.IO) {
        val handle = engine()
        decodeEngineArray(VelmuxJni.nativeListJobs(handle).decodeToString())
    }

    actual open suspend fun getJob(jobId: String): JobSnapshot? =
        withContext(Dispatchers.IO) {
            val handle = engine()
            val raw = VelmuxJni.nativeGetJob(handle, jobId).decodeToString()
            if (raw == "null") null else decodeEnginePayload(raw)
        }

    actual open suspend fun isJobActive(jobId: String): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            VelmuxJni.nativeIsJobActive(handle, jobId)
        }

    actual open suspend fun cancelJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            VelmuxJni.nativeCancelJob(handle, jobId)
        }

    actual open suspend fun pauseJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            VelmuxJni.nativePauseJob(handle, jobId)
        }

    actual open suspend fun resumeJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            VelmuxJni.nativeResumeJob(handle, jobId)
        }

    actual open suspend fun retryJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            VelmuxJni.nativeRetryJob(handle, jobId)
        }

    actual open suspend fun deleteJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            VelmuxJni.nativeDeleteJob(handle, jobId)
        }

    actual open suspend fun reuseJobSettings(jobId: String): String =
        withContext(Dispatchers.IO) {
            missing("velmux_reuse_job_settings")
        }

    // ---- inspection + diagnostics + platform integration (bound) -----------------

    actual open suspend fun inspect(uri: String, displayName: String): MediaInfo =
        withContext(Dispatchers.IO) {
            val handle = engine()
            decodeEnginePayload(
                VelmuxJni.nativeInspect(handle, uri, displayName).decodeToString()
            )
        }

    actual open suspend fun describeBackends(): List<String> =
        withContext(Dispatchers.IO) {
            val handle = engine()
            decodeEngineArray(VelmuxJni.nativeDescribeBackends(handle).decodeToString())
        }

    actual open suspend fun updateResources(snapshot: ResourceSnapshot): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            VelmuxJni.nativeUpdateResources(handle, encodeResources(snapshot))
        }

    actual open suspend fun pollEvents(): List<JobEvent> = withContext(Dispatchers.IO) {
        val handle = engine()
        decodeEngineArray(VelmuxJni.nativePollEvents(handle).decodeToString())
    }

    actual open fun dispose() {
        val handle = engineHandle
        engineHandle = 0L
        if (handle != 0L) {
            VelmuxJni.nativeDestroyEngine(handle)
        }
    }

    private fun encodeResources(snapshot: ResourceSnapshot): String {
        val fields = buildList {
            snapshot.cpuCores?.let { add("\"cpuCores\":$it") }
            snapshot.memoryAvailableMb?.let { add("\"memoryAvailableMb\":$it") }
            snapshot.thermalPressure?.let { add("\"thermalPressure\":$it") }
            snapshot.onBattery?.let { add("\"onBattery\":$it") }
            snapshot.hardwareCodecAvailable?.let { add("\"hardwareCodecAvailable\":$it") }
        }
        return "{" + fields.joinToString(",") + "}"
    }
}
