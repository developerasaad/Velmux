package com.velmux.mobile.engine

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * Android actual: JNI against the regenerated platform/android/interop
 * surface (VelmuxJni, RegisterNatives; JSON crosses as UTF-8 bytes; the
 * handle travels as jlong). Calls run on Dispatchers.IO — JNI is
 * synchronous under the hood.
 *
 * ONE process engine (mandate §10.1): the handle is a process-wide
 * singleton created on first use and never destroyed — UI detachment,
 * activity recreation and service start/stop all share it. The engine
 * outlives every transport; [dispose] only detaches.
 *
 * Coverage: the FULL frozen ABI v2 (core/include/velmux/abi.h) —
 * abi/engine version, the configuration-session lifecycle (create/
 * prepare/snapshot/options/update/sample/submit/close), direct submit,
 * reuse-settings, the job control surface, inspection, diagnostics and
 * the resource feed. Every payload passes the same core validation as
 * every other transport; this adapter owns no policy.
 */
actual open class EngineTransport actual constructor(
    private val dataDir: String,
) {

    private suspend fun engine(): Long = withContext(Dispatchers.IO) {
        processEngine(dataDir)
    }

    private fun bytes(payload: ByteArray): String = payload.decodeToString()

    // ---- ABI + engine version --------------------------------------------------

    actual open suspend fun abiVersion(): Int = withContext(Dispatchers.IO) {
        com.velmux.mobile.VelmuxJni.nativeAbiVersion()
    }

    actual open suspend fun engineVersion(): String =
        withContext(Dispatchers.IO) {
            bytes(com.velmux.mobile.VelmuxJni.nativeEngineVersion())
        }

    // ---- configuration sessions (mandate §14) -----------------------------------

    actual open suspend fun createSession(
        operation: String,
        sources: List<MediaSourceRequest>,
    ): String = withContext(Dispatchers.IO) {
        val handle = engine()
        val request = VelmuxJson.json.encodeToString(
            CreateSessionRequest.serializer(),
            CreateSessionRequest(operation = operation, sources = sources),
        )
        val result = decodeEnginePayload<SessionIdResult>(
            bytes(com.velmux.mobile.VelmuxJni.nativeCreateSession(handle, request)),
        )
        result.sessionId
    }

    actual open suspend fun prepareSession(sessionId: String): SessionSnapshot =
        withContext(Dispatchers.IO) {
            val handle = engine()
            decodeEnginePayload(
                bytes(
                    com.velmux.mobile.VelmuxJni.nativePrepareSession(handle, sessionId),
                ),
            )
        }

    actual open suspend fun getSessionSnapshot(sessionId: String): SessionSnapshot =
        withContext(Dispatchers.IO) {
            val handle = engine()
            decodeEnginePayload(
                bytes(
                    com.velmux.mobile.VelmuxJni.nativeGetSessionSnapshot(handle, sessionId),
                ),
            )
        }

    actual open suspend fun getSessionOptions(
        sessionId: String,
        field: String,
        revision: Long,
    ): FieldOptions = withContext(Dispatchers.IO) {
        val handle = engine()
        val query = VelmuxJson.json.encodeToString(
            SessionOptionsQuery.serializer(),
            SessionOptionsQuery(field = field, revision = revision),
        )
        decodeEnginePayload(
            bytes(
                com.velmux.mobile.VelmuxJni.nativeGetSessionOptions(handle, sessionId, query),
            ),
        )
    }

    actual open suspend fun updateSession(
        sessionId: String,
        request: SessionUpdateRequest,
    ): SessionSnapshot = withContext(Dispatchers.IO) {
        val handle = engine()
        val update = VelmuxJson.json.encodeToString(
            SessionUpdateRequest.serializer(),
            request,
        )
        decodeEnginePayload(
            bytes(
                com.velmux.mobile.VelmuxJni.nativeUpdateSession(handle, sessionId, update),
            ),
        )
    }

    actual open suspend fun generateSample(sessionId: String, revision: Long): SampleResult =
        withContext(Dispatchers.IO) {
            val handle = engine()
            val sample = VelmuxJson.json.encodeToString(
                SampleRequest.serializer(),
                SampleRequest(revision = revision),
            )
            decodeEnginePayload(
                bytes(
                    com.velmux.mobile.VelmuxJni.nativeGenerateSessionSample(
                        handle, sessionId, sample,
                    ),
                ),
            )
        }

    actual open suspend fun submitSession(
        sessionId: String,
        revision: Long,
        submissionId: String,
    ): String = withContext(Dispatchers.IO) {
        val handle = engine()
        val submit = VelmuxJson.json.encodeToString(
            SubmitSessionRequest.serializer(),
            SubmitSessionRequest(revision = revision, submissionId = submissionId),
        )
        val result = decodeEnginePayload<JobIdResult>(
            bytes(
                com.velmux.mobile.VelmuxJni.nativeSubmitSession(handle, sessionId, submit),
            ),
        )
        result.jobId
    }

    actual open suspend fun closeSession(sessionId: String): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            com.velmux.mobile.VelmuxJni.nativeCloseSession(handle, sessionId)
        }

    actual open suspend fun submitJob(requestJson: String): String =
        withContext(Dispatchers.IO) {
            val handle = engine()
            val result = decodeEnginePayload<JobIdResult>(
                bytes(com.velmux.mobile.VelmuxJni.nativeSubmitJob(handle, requestJson)),
            )
            result.jobId
        }

    // ---- jobs (mandate §37) --------------------------------------------------------

    actual open suspend fun listJobs(): List<JobSnapshot> = withContext(Dispatchers.IO) {
        val handle = engine()
        decodeEngineArray(bytes(com.velmux.mobile.VelmuxJni.nativeListJobs(handle)))
    }

    actual open suspend fun getJob(jobId: String): JobSnapshot? =
        withContext(Dispatchers.IO) {
            val handle = engine()
            val raw = bytes(com.velmux.mobile.VelmuxJni.nativeGetJob(handle, jobId))
            if (raw == "null") null else decodeEnginePayload(raw)
        }

    actual open suspend fun isJobActive(jobId: String): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            com.velmux.mobile.VelmuxJni.nativeIsJobActive(handle, jobId)
        }

    actual open suspend fun cancelJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            com.velmux.mobile.VelmuxJni.nativeCancelJob(handle, jobId)
        }

    actual open suspend fun pauseJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            com.velmux.mobile.VelmuxJni.nativePauseJob(handle, jobId)
        }

    actual open suspend fun resumeJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            com.velmux.mobile.VelmuxJni.nativeResumeJob(handle, jobId)
        }

    actual open suspend fun retryJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            com.velmux.mobile.VelmuxJni.nativeRetryJob(handle, jobId)
        }

    actual open suspend fun deleteJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            com.velmux.mobile.VelmuxJni.nativeDeleteJob(handle, jobId)
        }

    actual open suspend fun reuseJobSettings(jobId: String): String =
        withContext(Dispatchers.IO) {
            val handle = engine()
            val result = decodeEnginePayload<SessionIdResult>(
                bytes(com.velmux.mobile.VelmuxJni.nativeReuseJobSettings(handle, jobId)),
            )
            result.sessionId
        }

    // ---- inspection + diagnostics + platform integration ----------------------------

    actual open suspend fun inspect(uri: String, displayName: String): MediaInfo =
        withContext(Dispatchers.IO) {
            val handle = engine()
            decodeEnginePayload(
                bytes(
                    com.velmux.mobile.VelmuxJni.nativeInspect(handle, uri, displayName),
                ),
            )
        }

    actual open suspend fun describeBackends(): List<String> =
        withContext(Dispatchers.IO) {
            val handle = engine()
            decodeEngineArray(
                bytes(com.velmux.mobile.VelmuxJni.nativeDescribeBackends(handle)),
            )
        }

    actual open suspend fun updateResources(snapshot: ResourceSnapshot): Boolean =
        withContext(Dispatchers.IO) {
            val handle = engine()
            com.velmux.mobile.VelmuxJni.nativeUpdateResources(
                handle,
                encodeResources(snapshot),
            )
        }

    actual open suspend fun pollEvents(): List<JobEvent> = withContext(Dispatchers.IO) {
        val handle = engine()
        decodeEngineArray(bytes(com.velmux.mobile.VelmuxJni.nativePollEvents(handle)))
    }

    actual open fun dispose() {
        // The engine is process-owned (see class docs): detaching the client
        // releases nothing but this transport's reference. The C++ engine
        // and its durable store live until the process exits.
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

    companion object {
        private val processEngineLock = Any()

        /** The one process engine handle (0 until first creation). */
        @Volatile
        private var processEngineHandle: Long = 0L

        /**
         * Returns THE process engine, creating it on first use in [dataDir].
         * Every later call — from this transport, a recreated activity, or
         * a platform service — receives the same handle regardless of the
         * dataDir it passes (the first caller's directory owns the durable
         * store; the app always passes the same canonical path anyway).
         */
        internal fun processEngine(dataDir: String): Long {
            synchronized(processEngineLock) {
                if (processEngineHandle != 0L) {
                    return processEngineHandle
                }
                val handle = com.velmux.mobile.VelmuxJni.createEngine(File(dataDir))
                if (handle == 0L) {
                    throw EngineException(
                        EngineErrorCode.ENGINE_UNAVAILABLE,
                        "The Velmux engine failed to initialize " +
                            "(unusable data directory or no backend started)",
                    )
                }
                processEngineHandle = handle
                return handle
            }
        }
    }
}
