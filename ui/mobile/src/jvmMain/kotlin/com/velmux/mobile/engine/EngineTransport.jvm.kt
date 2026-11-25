package com.velmux.mobile.engine

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * JVM actual: the headless "dev" transport. There is no Velmux engine on
 * the JVM — every call returns the HONEST engine-unavailable error
 * (EngineException with the engine-unavailable code), so running the app
 * on the JVM shows a truthful setup error instead of pretending work.
 *
 * This class is also the base class the common test suite's fake
 * transport overrides (it must stay open with open members).
 */
actual open class EngineTransport actual constructor(
    private val dataDir: String,
) {

    private fun unavailable(what: String): Nothing = throw EngineException(
        EngineErrorCode.ENGINE_UNAVAILABLE,
        "The JVM development transport has no engine ($what); " +
            "run the app on Android or iOS for real conversions",
    )

    actual open suspend fun abiVersion(): Int = withContext(Dispatchers.IO) {
        unavailable("velmux_abi_version")
    }

    actual open suspend fun engineVersion(): String = withContext(Dispatchers.IO) {
        unavailable("velmux_engine_version")
    }

    actual open suspend fun createSession(
        operation: String,
        sources: List<MediaSourceRequest>,
    ): String = withContext(Dispatchers.IO) { unavailable("velmux_create_session") }

    actual open suspend fun prepareSession(sessionId: String): SessionSnapshot =
        withContext(Dispatchers.IO) { unavailable("velmux_prepare_session") }

    actual open suspend fun getSessionSnapshot(sessionId: String): SessionSnapshot =
        withContext(Dispatchers.IO) { unavailable("velmux_get_session_snapshot") }

    actual open suspend fun getSessionOptions(
        sessionId: String,
        field: String,
        revision: Long,
    ): FieldOptions = withContext(Dispatchers.IO) {
        unavailable("velmux_get_session_options")
    }

    actual open suspend fun updateSession(
        sessionId: String,
        request: SessionUpdateRequest,
    ): SessionSnapshot = withContext(Dispatchers.IO) {
        unavailable("velmux_update_session")
    }

    actual open suspend fun generateSample(
        sessionId: String,
        revision: Long,
    ): SampleResult = withContext(Dispatchers.IO) {
        unavailable("velmux_generate_session_sample")
    }

    actual open suspend fun submitSession(
        sessionId: String,
        revision: Long,
        submissionId: String,
    ): String = withContext(Dispatchers.IO) {
        unavailable("velmux_submit_session")
    }

    actual open suspend fun closeSession(sessionId: String): Boolean =
        withContext(Dispatchers.IO) { unavailable("velmux_close_session") }

    actual open suspend fun submitJob(requestJson: String): String =
        withContext(Dispatchers.IO) { unavailable("velmux_submit_job") }

    actual open suspend fun listJobs(): List<JobSnapshot> =
        withContext(Dispatchers.IO) { unavailable("velmux_list_jobs") }

    actual open suspend fun getJob(jobId: String): JobSnapshot? =
        withContext(Dispatchers.IO) { unavailable("velmux_get_job") }

    actual open suspend fun isJobActive(jobId: String): Boolean =
        withContext(Dispatchers.IO) { unavailable("velmux_is_job_active") }

    actual open suspend fun cancelJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) { unavailable("velmux_cancel_job") }

    actual open suspend fun pauseJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) { unavailable("velmux_pause_job") }

    actual open suspend fun resumeJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) { unavailable("velmux_resume_job") }

    actual open suspend fun retryJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) { unavailable("velmux_retry_job") }

    actual open suspend fun deleteJob(jobId: String): Boolean =
        withContext(Dispatchers.IO) { unavailable("velmux_delete_job") }

    actual open suspend fun reuseJobSettings(jobId: String): String =
        withContext(Dispatchers.IO) { unavailable("velmux_reuse_job_settings") }

    actual open suspend fun inspect(uri: String, displayName: String): MediaInfo =
        withContext(Dispatchers.IO) { unavailable("velmux_inspect") }

    actual open suspend fun describeBackends(): List<String> =
        withContext(Dispatchers.IO) { unavailable("velmux_describe_backends") }

    actual open suspend fun updateResources(snapshot: ResourceSnapshot): Boolean =
        withContext(Dispatchers.IO) { unavailable("velmux_update_resources") }

    actual open suspend fun pollEvents(): List<JobEvent> =
        withContext(Dispatchers.IO) { unavailable("velmux_poll_events") }

    actual open fun dispose() {
        // No engine handle exists on the JVM.
    }
}
