package com.velmux.mobile.engine

import com.velmux.cinterop.VelmuxEngineHandle
import com.velmux.cinterop.velmux_abi_version
import com.velmux.cinterop.velmux_cancel_job
import com.velmux.cinterop.velmux_close_session
import com.velmux.cinterop.velmux_create_session
import com.velmux.cinterop.velmux_delete_job
import com.velmux.cinterop.velmux_describe_backends
import com.velmux.cinterop.velmux_engine_create
import com.velmux.cinterop.velmux_engine_destroy
import com.velmux.cinterop.velmux_engine_version
import com.velmux.cinterop.velmux_free_string
import com.velmux.cinterop.velmux_generate_session_sample
import com.velmux.cinterop.velmux_get_job
import com.velmux.cinterop.velmux_get_session_options
import com.velmux.cinterop.velmux_get_session_snapshot
import com.velmux.cinterop.velmux_inspect
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
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.toKString
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.encodeToString

/**
 * iOS actual: the frozen C ABI v2 through kotlinx.cinterop — extern
 * declarations generated from platform/ios/interop/velmux.def, which
 * points at core/include/velmux/abi.h. Every JSON payload crosses as a
 * UTF-8 C string; malloc'd results are released with velmux_free_string
 * (velmux_engine_version returns a static label and is never freed).
 * Calls run on Dispatchers.IO (the C functions are synchronous).
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

    /** Lazily creates the process engine (velmux_engine_create). */
    private suspend fun engine(): CPointer<VelmuxEngineHandle> = engineMutex.withLock {
        val existing = handle
        if (handleValid && existing != null) return existing
        val created = withContext(Dispatchers.IO) {
            memScoped { velmux_engine_create(dataDir.cstr.ptr) }
        }
        if (created == null) {
            throw EngineException(
                EngineErrorCode.ENGINE_UNAVAILABLE,
                "The Velmux engine failed to initialize " +
                    "(unusable data directory or no backend started)",
            )
        }
        handle = created
        handleValid = true
        created
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
    private inline fun <reified T> decodeFacade(call: () -> CPointer<ByteVar>?): T =
        decodeEnginePayload(readFacadeString(call))

    /** Reads a facade string and decodes it as a typed array payload. */
    private inline fun <reified T> decodeFacadeArray(call: () -> CPointer<ByteVar>?): List<T> =
        decodeEngineArray(readFacadeString(call))

    // ---- ABI version -----------------------------------------------------------

    actual open suspend fun abiVersion(): Int = withContext(Dispatchers.IO) {
        velmux_abi_version()
    }

    actual open suspend fun engineVersion(): String = withContext(Dispatchers.IO) {
        // Static label: NOT malloc'd, never freed.
        velmux_engine_version()?.toKString() ?: "unknown"
    }

    // ---- sessions -----------------------------------------------------------------

    actual open suspend fun createSession(
        operation: String,
        sources: List<MediaSourceRequest>,
    ): String = withContext(Dispatchers.IO) {
        val engine = engine()
        val json = VelmuxJson.json.encodeToString(CreateSessionRequest(operation, sources))
        val raw = memScoped { readFacadeString { velmux_create_session(engine, json.cstr.ptr) } }
        VelmuxJson.json.decodeFromString(SessionIdResult.serializer(), raw).sessionId
    }

    actual open suspend fun prepareSession(sessionId: String): SessionSnapshot =
        withContext(Dispatchers.IO) {
            val engine = engine()
            memScoped {
                decodeFacade { velmux_prepare_session(engine, sessionId.cstr.ptr) }
            }
        }

    actual open suspend fun getSessionSnapshot(sessionId: String): SessionSnapshot =
        withContext(Dispatchers.IO) {
            val engine = engine()
            memScoped {
                decodeFacade { velmux_get_session_snapshot(engine, sessionId.cstr.ptr) }
            }
        }

    actual open suspend fun getSessionOptions(
        sessionId: String,
        field: String,
        revision: Long,
    ): FieldOptions = withContext(Dispatchers.IO) {
        val engine = engine()
        val json = VelmuxJson.json.encodeToString(SessionOptionsQuery(field, revision))
        memScoped {
            decodeFacade {
                velmux_get_session_options(engine, sessionId.cstr.ptr, json.cstr.ptr)
            }
        }
    }

    actual open suspend fun updateSession(
        sessionId: String,
        request: SessionUpdateRequest,
    ): SessionSnapshot = withContext(Dispatchers.IO) {
        val engine = engine()
        val json = VelmuxJson.json.encodeToString(request)
        memScoped {
            decodeFacade { velmux_update_session(engine, sessionId.cstr.ptr, json.cstr.ptr) }
        }
    }

    actual open suspend fun generateSample(sessionId: String, revision: Long): SampleResult =
        withContext(Dispatchers.IO) {
            val engine = engine()
            val json = VelmuxJson.json.encodeToString(SampleRequest(revision))
            memScoped {
                decodeFacade {
                    velmux_generate_session_sample(engine, sessionId.cstr.ptr, json.cstr.ptr)
                }
            }
        }

    actual open suspend fun submitSession(
        sessionId: String,
        revision: Long,
        submissionId: String,
    ): String = withContext(Dispatchers.IO) {
        val engine = engine()
        val json = VelmuxJson.json.encodeToString(
            SubmitSessionRequest(revision, submissionId)
        )
        val raw = memScoped {
            readFacadeString { velmux_submit_session(engine, sessionId.cstr.ptr, json.cstr.ptr) }
        }
        VelmuxJson.json.decodeFromString(JobIdResult.serializer(), raw).jobId
    }

    actual open suspend fun closeSession(sessionId: String): Boolean =
        withContext(Dispatchers.IO) {
            val engine = engine()
            memScoped { velmux_close_session(engine, sessionId.cstr.ptr) == 0 }
        }

    actual open suspend fun submitJob(requestJson: String): String =
        withContext(Dispatchers.IO) {
            val engine = engine()
            val raw = memScoped {
                readFacadeString { velmux_submit_job(engine, requestJson.cstr.ptr) }
            }
            VelmuxJson.json.decodeFromString(JobIdResult.serializer(), raw).jobId
        }

    // ---- jobs ----------------------------------------------------------------------

    actual open suspend fun listJobs(): List<JobSnapshot> = withContext(Dispatchers.IO) {
        val engine = engine()
        memScoped { decodeFacadeArray { velmux_list_jobs(engine) } }
    }

    actual open suspend fun getJob(jobId: String): JobSnapshot? =
        withContext(Dispatchers.IO) {
            val engine = engine()
            val raw = memScoped { readFacadeString { velmux_get_job(engine, jobId.cstr.ptr) } }
            if (raw == "null") null else decodeEnginePayload(raw)
        }

    actual open suspend fun isJobActive(jobId: String): Boolean =
        withContext(Dispatchers.IO) {
            val engine = engine()
            memScoped { velmux_is_job_active(engine, jobId.cstr.ptr) > 0 }
        }

    actual open suspend fun cancelJob(jobId: String): Boolean = withContext(Dispatchers.IO) {
        val engine = engine()
        memScoped { velmux_cancel_job(engine, jobId.cstr.ptr) > 0 }
    }

    actual open suspend fun pauseJob(jobId: String): Boolean = withContext(Dispatchers.IO) {
        val engine = engine()
        memScoped { velmux_pause_job(engine, jobId.cstr.ptr) > 0 }
    }

    actual open suspend fun resumeJob(jobId: String): Boolean = withContext(Dispatchers.IO) {
        val engine = engine()
        memScoped { velmux_resume_job(engine, jobId.cstr.ptr) > 0 }
    }

    actual open suspend fun retryJob(jobId: String): Boolean = withContext(Dispatchers.IO) {
        val engine = engine()
        memScoped { velmux_retry_job(engine, jobId.cstr.ptr) > 0 }
    }

    actual open suspend fun deleteJob(jobId: String): Boolean = withContext(Dispatchers.IO) {
        val engine = engine()
        memScoped { velmux_delete_job(engine, jobId.cstr.ptr) > 0 }
    }

    actual open suspend fun reuseJobSettings(jobId: String): String =
        withContext(Dispatchers.IO) {
            val engine = engine()
            val raw = memScoped {
                readFacadeString { velmux_reuse_job_settings(engine, jobId.cstr.ptr) }
            }
            VelmuxJson.json.decodeFromString(SessionIdResult.serializer(), raw).sessionId
        }

    // ---- inspection + diagnostics + platform integration -------------------------------

    actual open suspend fun inspect(uri: String, displayName: String): MediaInfo =
        withContext(Dispatchers.IO) {
            val engine = engine()
            memScoped {
                decodeFacade {
                    velmux_inspect(engine, uri.cstr.ptr, displayName.cstr.ptr)
                }
            }
        }

    actual open suspend fun describeBackends(): List<String> =
        withContext(Dispatchers.IO) {
            val engine = engine()
            memScoped { decodeFacadeArray<String> { velmux_describe_backends(engine) } }
        }

    actual open suspend fun updateResources(snapshot: ResourceSnapshot): Boolean =
        withContext(Dispatchers.IO) {
            val engine = engine()
            val json = VelmuxJson.json.encodeToString(snapshot)
            memScoped { velmux_update_resources(engine, json.cstr.ptr) == 0 }
        }

    actual open suspend fun pollEvents(): List<JobEvent> = withContext(Dispatchers.IO) {
        val engine = engine()
        memScoped { decodeFacadeArray { velmux_poll_events(engine) } }
    }

    actual open fun dispose() {
        val current = handle
        handle = null
        handleValid = false
        if (current != null) {
            velmux_engine_destroy(current)
        }
    }
}
