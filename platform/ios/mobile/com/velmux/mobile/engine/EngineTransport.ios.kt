package com.velmux.mobile.engine

import cnames.structs.VelmuxEngineHandle
import com.velmux.cinterop.velmux_abi_version
import com.velmux.cinterop.velmux_cancel_job
import com.velmux.cinterop.velmux_close_session
import com.velmux.cinterop.velmux_create_session
import com.velmux.cinterop.velmux_delete_job
import com.velmux.cinterop.velmux_describe_backends
import com.velmux.cinterop.velmux_engine_version
import com.velmux.cinterop.velmux_free_string
import com.velmux.cinterop.velmux_generate_session_sample
import com.velmux.cinterop.velmux_get_job
import com.velmux.cinterop.velmux_get_session_options
import com.velmux.cinterop.velmux_get_session_snapshot
import com.velmux.cinterop.velmux_inspect
import com.velmux.cinterop.velmux_ios_shared_engine
import com.velmux.cinterop.velmux_is_job_active
import com.velmux.cinterop.velmux_list_jobs
import com.velmux.cinterop.velmux_pause_job
import com.velmux.cinterop.velmux_poll_events
import com.velmux.cinterop.velmux_prepare_session
import com.velmux.cinterop.velmux_resume_job
import com.velmux.cinterop.velmux_retry_job
import com.velmux.cinterop.velmux_reuse_job_settings
import com.velmux.cinterop.velmux_submit_job
import com.velmux.cinterop.velmux_submit_session
import com.velmux.cinterop.velmux_update_resources
import com.velmux.cinterop.velmux_update_session
import kotlinx.cinterop.ByteVar
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.toKString
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.encodeToString

/**
 * iOS actual: the frozen C ABI v2 through kotlinx.cinterop — extern
 * declarations generated from platform/ios/interop/velmux.def, which
 * binds core/include/velmux/abi.h plus the host-provided
 * velmux_ios_shared_engine (VelmuxEngineHost.mm).
 *
 * Binding notes (verified against the generated interop):
 *  - `const char*` parameters map to Kotlin String? — payloads cross as
 *    plain strings;
 *  - `char*` returns map to CPointer<ByteVar>? — malloc'd results are
 *    read with toKString() and released with velmux_free_string
 *    (velmux_engine_version returns a static label and is never freed);
 *  - the opaque engine handle maps to cnames.structs.VelmuxEngineHandle
 *    (the forward-declared struct pattern; the def-package typealias
 *    does not exist for the self-referential typedef).
 *
 * Calls run on Dispatchers.Default (Kotlin/Native has no
 * Dispatchers.IO; Default is the background pool and the C functions are
 * synchronous).
 *
 * ONE process engine (mandate §10.1): velmux_ios_shared_engine returns
 * the same handle the ObjC++ host services use, created once in the
 * canonical Application Support/VelmuxEngine directory; dispose only
 * detaches (the engine is process-owned and never destroyed).
 *
 * On Linux this compiles to a klib (the verification this task performs);
 * LINKING the framework is the Xcode product build (scripts/build.sh --ios)
 * and is never attempted here.
 */
@OptIn(ExperimentalForeignApi::class)
actual open class EngineTransport actual constructor(
    private val dataDir: String,
) {

    private val engineMutex = Mutex()

    private var handle: CPointer<VelmuxEngineHandle>? = null
    private var handleValid: Boolean = false

    /** Resolves THE process engine (velmux_ios_shared_engine). */
    private suspend fun engine(): CPointer<VelmuxEngineHandle> = engineMutex.withLock {
        val existing = handle
        if (handleValid && existing != null) return existing
        val resolved = withContext(Dispatchers.Default) {
            velmux_ios_shared_engine(dataDir)
        }
        if (resolved == null) {
            throw EngineException(
                EngineErrorCode.ENGINE_UNAVAILABLE,
                "The Velmux engine failed to initialize " +
                    "(unusable data directory or no backend started)",
            )
        }
        handle = resolved
        handleValid = true
        resolved
    }

    /** Reads (and frees) a malloc'd facade string. */
    private fun readFacadeString(call: () -> CPointer<ByteVar>?): String {
        val raw = call() ?: throw EngineException(
            EngineErrorCode.ENGINE_UNAVAILABLE,
            "The engine returned no payload",
        )
        return try {
            raw.toKString()
        } finally {
            velmux_free_string(raw)
        }
    }

    /** Reads a facade string and decodes it as the (reified) payload. */
    private inline fun <reified T> decodeFacade(
        noinline call: () -> CPointer<ByteVar>?,
    ): T = decodeEnginePayload(readFacadeString(call))

    /** Reads a facade string and decodes it as a typed array payload. */
    private inline fun <reified T> decodeFacadeArray(
        noinline call: () -> CPointer<ByteVar>?,
    ): List<T> = decodeEngineArray(readFacadeString(call))

    // ---- ABI version -----------------------------------------------------------

    actual open suspend fun abiVersion(): Int = withContext(Dispatchers.Default) {
        velmux_abi_version()
    }

    actual open suspend fun engineVersion(): String = withContext(Dispatchers.Default) {
        // Static label: NOT malloc'd, never freed.
        velmux_engine_version()?.toKString() ?: "unknown"
    }

    // ---- sessions -----------------------------------------------------------------

    actual open suspend fun createSession(
        operation: String,
        sources: List<MediaSourceRequest>,
    ): String = withContext(Dispatchers.Default) {
        val engine = engine()
        val json = VelmuxJson.json.encodeToString(CreateSessionRequest(operation, sources))
        val raw = readFacadeString { velmux_create_session(engine, json) }
        VelmuxJson.json.decodeFromString(SessionIdResult.serializer(), raw).sessionId
    }

    actual open suspend fun prepareSession(sessionId: String): SessionSnapshot =
        withContext(Dispatchers.Default) {
            val engine = engine()
            decodeFacade { velmux_prepare_session(engine, sessionId) }
        }

    actual open suspend fun getSessionSnapshot(sessionId: String): SessionSnapshot =
        withContext(Dispatchers.Default) {
            val engine = engine()
            decodeFacade { velmux_get_session_snapshot(engine, sessionId) }
        }

    actual open suspend fun getSessionOptions(
        sessionId: String,
        field: String,
        revision: Long,
    ): FieldOptions = withContext(Dispatchers.Default) {
        val engine = engine()
        val json = VelmuxJson.json.encodeToString(SessionOptionsQuery(field, revision))
        decodeFacade { velmux_get_session_options(engine, sessionId, json) }
    }

    actual open suspend fun updateSession(
        sessionId: String,
        request: SessionUpdateRequest,
    ): SessionSnapshot = withContext(Dispatchers.Default) {
        val engine = engine()
        val json = VelmuxJson.json.encodeToString(request)
        decodeFacade { velmux_update_session(engine, sessionId, json) }
    }

    actual open suspend fun generateSample(sessionId: String, revision: Long): SampleResult =
        withContext(Dispatchers.Default) {
            val engine = engine()
            val json = VelmuxJson.json.encodeToString(SampleRequest(revision))
            decodeFacade { velmux_generate_session_sample(engine, sessionId, json) }
        }

    actual open suspend fun submitSession(
        sessionId: String,
        revision: Long,
        submissionId: String,
    ): String = withContext(Dispatchers.Default) {
        val engine = engine()
        val json = VelmuxJson.json.encodeToString(
            SubmitSessionRequest(revision, submissionId)
        )
        val raw = readFacadeString { velmux_submit_session(engine, sessionId, json) }
        VelmuxJson.json.decodeFromString(JobIdResult.serializer(), raw).jobId
    }

    actual open suspend fun closeSession(sessionId: String): Boolean =
        withContext(Dispatchers.Default) {
            val engine = engine()
            velmux_close_session(engine, sessionId) == 0
        }

    actual open suspend fun submitJob(requestJson: String): String =
        withContext(Dispatchers.Default) {
            val engine = engine()
            val raw = readFacadeString { velmux_submit_job(engine, requestJson) }
            VelmuxJson.json.decodeFromString(JobIdResult.serializer(), raw).jobId
        }

    // ---- jobs ----------------------------------------------------------------------

    actual open suspend fun listJobs(): List<JobSnapshot> = withContext(Dispatchers.Default) {
        val engine = engine()
        decodeFacadeArray { velmux_list_jobs(engine) }
    }

    actual open suspend fun getJob(jobId: String): JobSnapshot? =
        withContext(Dispatchers.Default) {
            val engine = engine()
            val raw = readFacadeString { velmux_get_job(engine, jobId) }
            if (raw == "null") null else decodeEnginePayload(raw)
        }

    actual open suspend fun isJobActive(jobId: String): Boolean =
        withContext(Dispatchers.Default) {
            val engine = engine()
            velmux_is_job_active(engine, jobId) > 0
        }

    actual open suspend fun cancelJob(jobId: String): Boolean = withContext(Dispatchers.Default) {
        val engine = engine()
        velmux_cancel_job(engine, jobId) > 0
    }

    actual open suspend fun pauseJob(jobId: String): Boolean = withContext(Dispatchers.Default) {
        val engine = engine()
        velmux_pause_job(engine, jobId) > 0
    }

    actual open suspend fun resumeJob(jobId: String): Boolean = withContext(Dispatchers.Default) {
        val engine = engine()
        velmux_resume_job(engine, jobId) > 0
    }

    actual open suspend fun retryJob(jobId: String): Boolean = withContext(Dispatchers.Default) {
        val engine = engine()
        velmux_retry_job(engine, jobId) > 0
    }

    actual open suspend fun deleteJob(jobId: String): Boolean = withContext(Dispatchers.Default) {
        val engine = engine()
        velmux_delete_job(engine, jobId) > 0
    }

    actual open suspend fun reuseJobSettings(jobId: String): String =
        withContext(Dispatchers.Default) {
            val engine = engine()
            val raw = readFacadeString { velmux_reuse_job_settings(engine, jobId) }
            VelmuxJson.json.decodeFromString(SessionIdResult.serializer(), raw).sessionId
        }

    // ---- inspection + diagnostics + platform integration -------------------------------

    actual open suspend fun inspect(uri: String, displayName: String): MediaInfo =
        withContext(Dispatchers.Default) {
            val engine = engine()
            decodeFacade {
                velmux_inspect(engine, uri, displayName)
            }
        }

    actual open suspend fun describeBackends(): List<String> =
        withContext(Dispatchers.Default) {
            val engine = engine()
            decodeFacadeArray<String> { velmux_describe_backends(engine) }
        }

    actual open suspend fun updateResources(snapshot: ResourceSnapshot): Boolean =
        withContext(Dispatchers.Default) {
            val engine = engine()
            val json = VelmuxJson.json.encodeToString(snapshot)
            velmux_update_resources(engine, json) == 0
        }

    actual open suspend fun pollEvents(): List<JobEvent> = withContext(Dispatchers.Default) {
        val engine = engine()
        decodeFacadeArray { velmux_poll_events(engine) }
    }

    actual open fun dispose() {
        // The engine is process-owned (shared with the ObjC++ host services):
        // detaching releases only this transport's reference. The C++ engine
        // and its durable store live until the process exits.
        handle = null
        handleValid = false
    }
}
