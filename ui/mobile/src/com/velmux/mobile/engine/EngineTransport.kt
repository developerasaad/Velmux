package com.velmux.mobile.engine

/**
 * The platform transport over the frozen C ABI v2 (core/include/velmux/abi.h).
 *
 * Mirrors the ABI function list one-to-one: JSON in, JSON out; every string
 * returning function produces either the documented payload or the error
 * envelope {"error":{"code":..,"message":..}}, which each actual decodes to
 * [EngineException] via [VelmuxJson].
 *
 * Expect/actual strategy (spec UI-1):
 *  - Android: JNI against the existing platform/android/interop surface
 *    (see the actual for the exact, honestly-reported coverage).
 *  - iOS: kotlinx.cinterop extern declarations generated from
 *    platform/ios/interop/velmux.def (core/include/velmux/abi.h).
 *  - JVM: a headless "dev" transport that returns honest
 *    engine-unavailable errors (host verification has no engine).
 *
 * All suspend members execute engine work OFF the UI thread
 * (Dispatchers.IO inside the actuals; JNI is synchronous under the hood).
 * The class is open so the common test suite can drive the
 * [EngineClient] with a fake transport.
 */
expect open class EngineTransport(dataDir: String) {

    /** VELMUX_ABI_VERSION of the loaded library (checked before use). */
    open suspend fun abiVersion(): Int

    /** Engine version label (informational, e.g. "1.0.0"). */
    open suspend fun engineVersion(): String

    // -- configuration sessions (mandate §14) -------------------------------

    /**
     * velmux_create_session: {"operation":..,"sources":[{"uri":..,"name":..}]}
     * -> {"sessionId":"s-..."}. Returns the session id.
     */
    open suspend fun createSession(
        operation: String,
        sources: List<MediaSourceRequest>,
    ): String

    /** velmux_prepare_session: synchronous inspection -> session snapshot. */
    open suspend fun prepareSession(sessionId: String): SessionSnapshot

    /** velmux_get_session_snapshot -> session snapshot. */
    open suspend fun getSessionSnapshot(sessionId: String): SessionSnapshot

    /**
     * velmux_get_session_options: {"field":..,"revision":N}
     * -> {"field":..,"values":[..]}.
     */
    open suspend fun getSessionOptions(
        sessionId: String,
        field: String,
        revision: Long,
    ): FieldOptions

    /**
     * velmux_update_session: {"revision":N,"acceptRelatedChange":bool,
     * "changes":[..]} -> session snapshot (or a structured error such as
     * stale-revision / related-change-required).
     */
    open suspend fun updateSession(
        sessionId: String,
        request: SessionUpdateRequest,
    ): SessionSnapshot

    /** velmux_generate_session_sample: {"revision":N} -> sample result. */
    open suspend fun generateSample(sessionId: String, revision: Long): SampleResult

    /**
     * velmux_submit_session: {"revision":N,"submissionId":"u-..."}
     * -> {"jobId":"j-..."}. Returns the job id.
     */
    open suspend fun submitSession(
        sessionId: String,
        revision: Long,
        submissionId: String,
    ): String

    /** velmux_close_session (int result). */
    open suspend fun closeSession(sessionId: String): Boolean

    /**
     * velmux_submit_job: the typed JobRequest JSON
     * {"operation":..,"submissionId":..,"<operation>":{..}} -> job id.
     */
    open suspend fun submitJob(requestJson: String): String

    // -- jobs (mandate §37) ---------------------------------------------------

    /** velmux_list_jobs -> [JobSnapshot...]. */
    open suspend fun listJobs(): List<JobSnapshot>

    /** velmux_get_job -> JobSnapshot (null when the id is unknown). */
    open suspend fun getJob(jobId: String): JobSnapshot?

    /**
     * velmux_is_job_active: 1 while a worker is executing/finalizing,
     * 0 when idle — distinguishes "cancel recorded" from "cancel completed".
     */
    open suspend fun isJobActive(jobId: String): Boolean

    /** velmux_cancel_job / pause / resume / retry / delete (int results). */
    open suspend fun cancelJob(jobId: String): Boolean
    open suspend fun pauseJob(jobId: String): Boolean
    open suspend fun resumeJob(jobId: String): Boolean
    open suspend fun retryJob(jobId: String): Boolean
    open suspend fun deleteJob(jobId: String): Boolean

    /**
     * velmux_reuse_job_settings: builds a NEW editable session pre-filled
     * with a completed job's accepted intent -> {"sessionId":..}.
     */
    open suspend fun reuseJobSettings(jobId: String): String

    // -- inspection + diagnostics + platform integration ----------------------

    /** velmux_inspect(uri, displayName) -> MediaInfo. */
    open suspend fun inspect(uri: String, displayName: String): MediaInfo

    /** velmux_describe_backends -> [string...]. */
    open suspend fun describeBackends(): List<String>

    /** velmux_update_resources(snapshot json) — all fields optional. */
    open suspend fun updateResources(snapshot: ResourceSnapshot): Boolean

    /** velmux_poll_events -> drained batch (oldest first, possibly empty). */
    open suspend fun pollEvents(): List<JobEvent>

    /** Releases the platform engine handle (velmux_engine_destroy). */
    open fun dispose()
}
