package com.velmux.mobile.engine

import com.velmux.mobile.platform.ImportedSource
import com.velmux.mobile.platform.PickedFile
import com.velmux.mobile.platform.ResolvedSource
import com.velmux.mobile.platform.SourcePorter
import com.velmux.mobile.platform.deleteFile
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlin.uuid.ExperimentalUuidApi
import kotlin.uuid.Uuid

/** Lifecycle of the engine as seen by the UI. */
sealed interface EngineStatus {
    data object NotStarted : EngineStatus
    data object Ready : EngineStatus

    /** The loaded library speaks a different ABI than this client. */
    data class AbiMismatch(val found: Int, val expected: Int) : EngineStatus

    /** No usable engine behind the transport (honest dev/error surface). */
    data class Unavailable(val error: EngineError) : EngineStatus
}

/** Read-only diagnostics (Settings screen). */
data class EngineInfo(
    val abiVersion: Int,
    val engineVersion: String?,
    val backends: List<String>,
)

/**
 * Honest preparation states after the user picks files (mandate §36.1):
 * selected -> resolving -> importing (only when the platform requires it) ->
 * inspecting -> configuring -> ready / failed / canceled.
 */
sealed interface PreparationState {
    data object Idle : PreparationState

    /** Files picked; nothing sent to the engine yet. */
    data class Selected(val operation: String, val sources: List<PickedFile>) : PreparationState

    /** Resolving the picked documents into engine-readable sources. */
    data class Resolving(val operation: String, val sources: List<PickedFile>) : PreparationState

    /** Copying into engine-readable storage (only when required). */
    data class Importing(val operation: String, val sources: List<PickedFile>) : PreparationState

    /** Session created; the engine is inspecting the media. */
    data class Inspecting(val operation: String, val sessionId: String) : PreparationState

    /** Session ready for editing; the configuration is not valid yet. */
    data class Configuring(val operation: String, val sessionId: String) : PreparationState

    /** Configuration valid; submit is possible. */
    data class Ready(val operation: String, val sessionId: String) : PreparationState

    /** The engine refused a step (inspection, validation...). */
    data class Failed(val error: EngineError) : PreparationState

    /** The user backed out; any session created was closed. */
    data object Canceled : PreparationState
}

/** Registration handle returned by [EngineClient.addListener]. */
fun interface EngineListener {
    fun dispose()
}

/**
 * The ONE shared engine client (mandate §36.1): the single source of truth
 * every screen renders. No second mutable business model exists — revision
 * mismatches and event/snapshot drift surface as state REFRESHES.
 *
 * Responsibilities:
 *  - StateFlows for jobs / sessions / events / engine diagnostics and the
 *    single coherent error banner (one surface, no alert spam);
 *  - a 250 ms poll loop (coalescing per job) that runs while active work
 *    exists OR UI listeners are attached, parking otherwise; terminal
 *    events force a FULL jobs refresh (snapshot reconciliation: the UI
 *    never depends on receiving every progress event); a transport that
 *    comes back healthy is reconciled the same way;
 *  - the session high-level API: create / prepare / update / submit with
 *    stale-revision handling and durable idempotent submission ids;
 *  - job control (cancel / pause / resume / retry / delete record /
 *    delete managed output) through the transport.
 */
class EngineClient(
    private val transport: EngineTransport,
    internal val pollIntervalMs: Long = 250L,
) {

    // ---- observable state (screens render ONLY these flows) ----------------

    private val _engineStatus = MutableStateFlow<EngineStatus>(EngineStatus.NotStarted)
    val engineStatus: StateFlow<EngineStatus> = _engineStatus

    private val _engineInfo = MutableStateFlow<EngineInfo?>(null)
    val engineInfo: StateFlow<EngineInfo?> = _engineInfo

    private val _jobs = MutableStateFlow<List<JobSnapshot>>(emptyList())
    val jobs: StateFlow<List<JobSnapshot>> = _jobs

    private val _sessions = MutableStateFlow<Map<String, SessionSnapshot>>(emptyMap())
    val sessions: StateFlow<Map<String, SessionSnapshot>> = _sessions

    private val _preparation = MutableStateFlow<PreparationState>(PreparationState.Idle)
    val preparation: StateFlow<PreparationState> = _preparation

    /** The latest coalesced event batch (progress spam superseded). */
    private val _lastEvents = MutableStateFlow<List<JobEvent>>(emptyList())
    val lastEvents: StateFlow<List<JobEvent>> = _lastEvents

    /**
     * The one coherent error surface: one banner at a time. Screens render
     * it as a single snackbar; [dismissBanner] clears it.
     */
    private val _banner = MutableStateFlow<EngineError?>(null)
    val banner: StateFlow<EngineError?> = _banner

    /** Emitted whenever a stale revision was reconciled by a state refresh. */
    private val _revisionRefreshed = MutableSharedFlow<String>(extraBufferCapacity = 8)
    val revisionRefreshed: SharedFlow<String> = _revisionRefreshed

    /** Emitted when a session submitted and a job was accepted (jobId). */
    private val _jobSubmitted = MutableSharedFlow<String>(extraBufferCapacity = 8)
    val jobSubmitted: SharedFlow<String> = _jobSubmitted

    /**
     * Every coalesced event batch the poll loop drained, for the platform
     * notification/foreground surfaces (mandate §10.1: ONE engine and ONE
     * event pump per process — platform services subscribe here instead of
     * polling the engine a second time). Terminal states ride the same
     * batches; the buffer is deep enough that a momentarily slow platform
     * collector cannot lose them (the emitting side back-pressures).
     */
    private val _eventBatches =
        MutableSharedFlow<List<JobEvent>>(replay = 0, extraBufferCapacity = 64)
    val eventBatches: SharedFlow<List<JobEvent>> = _eventBatches

    // ---- internals -----------------------------------------------------------

    private val listenerCount = MutableStateFlow(0)
    private val wakeups = Channel<Unit>(Channel.CONFLATED)
    private val sessionMutex = Mutex()
    private var clientScope: CoroutineScope? = null
    private var loopJob: Job? = null

    /** sessionId -> durable submission id (idempotency, mandate §37.1). */
    private val pendingSubmissions = mutableMapOf<String, String>()

    /** True while the transport last answered; false after a transport error. */
    @Volatile
    private var transportHealthy: Boolean = true

    /** Set by [cancelPreparation]; phase boundaries abort when raised. */
    @Volatile
    private var cancelRequested: Boolean = false

    // ---- lifecycle -----------------------------------------------------------

    /**
     * Starts the client: ABI check, engine diagnostics, initial jobs
     * snapshot and the poll loop in [scope]. Starting twice is a no-op.
     */
    fun start(scope: CoroutineScope) {
        if (loopJob?.isActive == true) return
        clientScope = scope
        loopJob = scope.launch {
            bootstrap()
            pollLoop()
        }
    }

    /** Stops the poll loop; [dispose] also releases the engine handle. */
    fun stop() {
        loopJob?.cancel()
        loopJob = null
    }

    fun dispose() {
        stop()
        transport.dispose()
    }

    /**
     * Registers a UI listener: the 250 ms poll loop keeps running while at
     * least one listener exists even without active work. Dispose to
     * detach (DisposableEffect onDispose).
     */
    fun addListener(): EngineListener {
        listenerCount.value = listenerCount.value + 1
        wakeups.trySend(Unit)
        return EngineListener {
            listenerCount.value = (listenerCount.value - 1).coerceAtLeast(0)
            wakeups.trySend(Unit)
        }
    }

    fun dismissBanner() {
        _banner.value = null
    }

    private fun banner(error: EngineError) {
        _banner.value = error
    }

    private suspend fun bootstrap() {
        try {
            val abi = transport.abiVersion()
            if (abi != Abi.VERSION) {
                _engineStatus.value = EngineStatus.AbiMismatch(abi, Abi.VERSION)
                banner(
                    EngineError(
                        "abi-mismatch",
                        "Engine ABI $abi does not match the app contract ${Abi.VERSION}",
                    )
                )
                return
            }
            refreshJobs()
            val version = try {
                transport.engineVersion()
            } catch (error: EngineException) {
                null
            }
            val backends = try {
                transport.describeBackends()
            } catch (error: EngineException) {
                emptyList()
            }
            _engineInfo.value = EngineInfo(abi, version, backends)
            _engineStatus.value = EngineStatus.Ready
        } catch (error: EngineException) {
            transportHealthy = false
            _engineStatus.value = EngineStatus.Unavailable(error.error)
            banner(error.error)
        }
    }

    // ---- poll loop -----------------------------------------------------------

    private suspend fun pollLoop() {
        while (currentCoroutineContext().isActive) {
            val busy = hasOngoingJobs() ||
                listenerCount.value > 0 ||
                pendingSubmissions.isNotEmpty() ||
                preparationInProgress()
            if (busy) {
                pollTick()
                delay(pollIntervalMs)
            } else {
                // Nothing active and nobody listening: park until woken.
                wakeups.receive()
            }
        }
    }

    private fun preparationInProgress(): Boolean = when (val state = _preparation.value) {
        is PreparationState.Resolving, is PreparationState.Importing,
        is PreparationState.Inspecting, is PreparationState.Selected,
        -> true
        else -> false
    }

    private fun hasOngoingJobs(): Boolean =
        _jobs.value.any { it.state in JobStates.ONGOING }

    private suspend fun pollTick() {
        if (_engineStatus.value !is EngineStatus.Ready) return
        val events = try {
            transport.pollEvents()
        } catch (error: EngineException) {
            onTransportError(error)
            return
        }
        // A transport that came back healthy is reconciled from the durable
        // snapshot — never trust partially applied event state.
        if (!transportHealthy) {
            transportHealthy = true
            refreshJobs()
            return
        }
        if (events.isEmpty()) return
        val coalesced = coalesce(events)
        _lastEvents.value = coalesced
        _eventBatches.emit(coalesced)
        val sawTerminal = coalesced.any { it.state in JobStates.TERMINAL }
        if (sawTerminal) {
            // Terminal event: force a FULL jobs refresh.
            refreshJobs()
        } else {
            applyEvents(coalesced)
        }
    }

    /** Keeps only the LATEST event per job within one drained batch. */
    internal fun coalesce(events: List<JobEvent>): List<JobEvent> {
        val latest = LinkedHashMap<String, JobEvent>()
        for (event in events) {
            latest[event.jobId] = event
        }
        return latest.values.toList()
    }

    private fun applyEvents(events: List<JobEvent>) {
        val current = _jobs.value.toMutableList()
        var missing = false
        for (event in events) {
            val index = current.indexOfFirst { it.id == event.jobId }
            if (index < 0) {
                missing = true
                continue
            }
            val old = current[index]
            current[index] = old.copy(
                state = event.state,
                progress = event.progress,
                error = event.error ?: old.error,
                errorCode = event.errorCode ?: old.errorCode,
                outputSizeBytes = if (event.outputSizeBytes >= 0) {
                    event.outputSizeBytes
                } else {
                    old.outputSizeBytes
                },
            )
        }
        _jobs.value = current.toList()
        if (missing) {
            // An event for a job we never saw (fresh submission): reconcile
            // with a full refresh instead of fabricating a partial row.
            clientScope?.launch { refreshJobs() }
        }
    }

    /** Full snapshot reconciliation from the durable job store. */
    suspend fun refreshJobs() {
        try {
            val jobs = transport.listJobs()
            _jobs.value = jobs.sortedByDescending { it.createdAtMs }
        } catch (error: EngineException) {
            onTransportError(error)
        }
    }

    private fun onTransportError(error: EngineException) {
        transportHealthy = false
        if (_engineStatus.value is EngineStatus.Ready) {
            banner(error.error)
        }
    }

    // ---- sessions (mandate §14) ----------------------------------------------

    /**
     * Drives the honest preparation flow for [operation] over [picks]:
     * selected -> resolving -> importing (only when required) -> inspecting
     * (create + prepare) -> configuring / ready.
     *
     * @return the session id when the session reached
     *         Configuring/Ready; null when a step failed (Failed state +
     *         banner) or the user canceled mid-flight.
     */
    suspend fun beginWorkflow(operation: String, picks: List<PickedFile>): String? {
        if (picks.isEmpty()) return null
        sessionMutex.withLock {
            cancelRequested = false
            _preparation.value = PreparationState.Selected(operation, picks)

            _preparation.value = PreparationState.Resolving(operation, picks)
            val resolved: List<ResolvedSource> = picks.map { SourcePorter.resolve(it) }
            if (cancelRequested) return abortPreparation(null)

            if (resolved.any { it.requiresImport }) {
                _preparation.value = PreparationState.Importing(operation, picks)
            }
            val sources: List<ImportedSource> = resolved.map { source ->
                if (source.requiresImport) SourcePorter.import(source) else {
                    ImportedSource(source.uri, source.displayName)
                }
            }
            if (cancelRequested) return abortPreparation(null)

            val sessionId = try {
                transport.createSession(
                    operation,
                    sources.map { MediaSourceRequest(it.uri, it.displayName) },
                )
            } catch (error: EngineException) {
                failPreparation(error)
                return null
            }
            if (cancelRequested) return abortPreparation(sessionId)

            _preparation.value = PreparationState.Inspecting(operation, sessionId)
            val snapshot = try {
                transport.prepareSession(sessionId)
            } catch (error: EngineException) {
                // Inspection failed: close the half-created session.
                closeSessionQuietly(sessionId)
                failPreparation(error)
                return null
            }
            _sessions.value = _sessions.value + (sessionId to snapshot)
            publishPrepared(operation, sessionId, snapshot)
            return sessionId
        }
    }

    /**
     * Cancels the in-flight preparation (checked at phase boundaries) and
     * closes any session already created.
     */
    suspend fun cancelPreparation() {
        cancelRequested = true
        sessionMutex.withLock {
            val state = _preparation.value
            val sessionId = when (state) {
                is PreparationState.Inspecting -> state.sessionId
                is PreparationState.Configuring -> state.sessionId
                is PreparationState.Ready -> state.sessionId
                else -> null
            }
            if (sessionId != null) {
                closeSessionQuietly(sessionId)
                _sessions.value = _sessions.value - sessionId
            }
            _preparation.value = PreparationState.Canceled
            pendingSubmissions.remove(sessionId)
            cancelRequested = false
        }
    }

    private fun abortPreparation(sessionId: String?): String? {
        if (sessionId != null) {
            _sessions.value = _sessions.value - sessionId
        }
        _preparation.value = PreparationState.Canceled
        cancelRequested = false
        return null
    }

    private fun failPreparation(error: EngineException) {
        _preparation.value = PreparationState.Failed(error.error)
        banner(error.error)
    }

    private fun publishPrepared(
        operation: String,
        sessionId: String,
        snapshot: SessionSnapshot,
    ) {
        _preparation.value = if (snapshot.configurationValid) {
            PreparationState.Ready(operation, sessionId)
        } else {
            PreparationState.Configuring(operation, sessionId)
        }
    }

    /**
     * Applies [changes] to the session. A stale revision is NEVER an error
     * for the user: the client refreshes the snapshot from the engine (the
     * one source of truth) and emits [revisionRefreshed].
     *
     * @return the new snapshot, or null when the engine refused the change
     *         (banner carries the reason; related-change proposals surface
     *         through the refreshed snapshot itself).
     */
    suspend fun updateSession(
        sessionId: String,
        changes: List<FieldChange>,
        acceptRelatedChange: Boolean = false,
    ): SessionSnapshot? = sessionMutex.withLock {
        val current = _sessions.value[sessionId] ?: fetchSession(sessionId) ?: return null
        if (changes.isEmpty()) return current
        try {
            val updated = transport.updateSession(
                sessionId,
                SessionUpdateRequest(
                    revision = current.revision,
                    acceptRelatedChange = acceptRelatedChange,
                    changes = changes,
                ),
            )
            _sessions.value = _sessions.value + (sessionId to updated)
            publishPrepared(updated.operation, sessionId, updated)
            updated
        } catch (error: EngineException) {
            when {
                error.isStaleRevision() -> {
                    // The engine moved on: adopt ITS snapshot, re-render.
                    val fresh = fetchSession(sessionId)
                    if (fresh != null) {
                        _sessions.value = _sessions.value + (sessionId to fresh)
                        publishPrepared(fresh.operation, sessionId, fresh)
                        _revisionRefreshed.tryEmit(sessionId)
                    }
                    null
                }
                error.isRelatedChangeRequired() -> {
                    val fresh = fetchSession(sessionId)
                    if (fresh != null) {
                        _sessions.value = _sessions.value + (sessionId to fresh)
                    }
                    banner(error.error)
                    null
                }
                else -> {
                    banner(error.error)
                    null
                }
            }
        }
    }

    /** Fetches the session options for one field at the current revision. */
    suspend fun sessionOptions(sessionId: String, field: String): FieldOptions? {
        val snapshot = _sessions.value[sessionId] ?: fetchSession(sessionId) ?: return null
        return try {
            transport.getSessionOptions(sessionId, field, snapshot.revision)
        } catch (error: EngineException) {
            banner(error.error)
            null
        }
    }

    /** Generates a preview sample with the session's resolved settings. */
    suspend fun generateSample(sessionId: String): SampleResult? {
        val snapshot = _sessions.value[sessionId] ?: fetchSession(sessionId) ?: return null
        return try {
            transport.generateSample(sessionId, snapshot.revision)
        } catch (error: EngineException) {
            banner(error.error)
            null
        }
    }

    private suspend fun fetchSession(sessionId: String): SessionSnapshot? = try {
        transport.getSessionSnapshot(sessionId)
    } catch (error: EngineException) {
        banner(error.error)
        null
    }

    /**
     * Submits the session with a DURABLE idempotency id: the id is minted
     * once and retried verbatim, so a retried submission that already
     * reached the engine resolves through the job store
     * (duplicate-submission) instead of double-submitting.
     *
     * @return the job id, or null on failure (banner carries the reason).
     */
    suspend fun submitSession(sessionId: String): String? = sessionMutex.withLock {
        val snapshot = _sessions.value[sessionId] ?: fetchSession(sessionId) ?: return null
        val submissionId = pendingSubmissions.getOrPut(sessionId) { newSubmissionId() }
        val jobId = try {
            transport.submitSession(sessionId, snapshot.revision, submissionId)
        } catch (error: EngineException) {
            when {
                error.isDuplicateSubmission() -> {
                    pendingSubmissions.remove(sessionId)
                    resolveDuplicate(submissionId)
                }
                error.isStaleRevision() -> {
                    val fresh = fetchSession(sessionId)
                    if (fresh != null) {
                        _sessions.value = _sessions.value + (sessionId to fresh)
                        _revisionRefreshed.tryEmit(sessionId)
                    }
                    null
                }
                else -> {
                    banner(error.error)
                    null
                }
            }
        }
        if (jobId != null) {
            pendingSubmissions.remove(sessionId)
            _jobSubmitted.tryEmit(jobId)
            wakeups.trySend(Unit)
            refreshJobs()
            closeSessionQuietly(sessionId)
            _sessions.value = _sessions.value - sessionId
            clearPreparationIfSession(sessionId)
        }
        jobId
    }

    /** Resets the preparation surface when the submitted session was it. */
    private fun clearPreparationIfSession(sessionId: String) {
        val state = _preparation.value
        val belongs = when (state) {
            is PreparationState.Configuring -> state.sessionId == sessionId
            is PreparationState.Ready -> state.sessionId == sessionId
            is PreparationState.Inspecting -> state.sessionId == sessionId
            else -> false
        }
        if (belongs) {
            _preparation.value = PreparationState.Idle
        }
    }

    private suspend fun resolveDuplicate(submissionId: String): String? = try {
        transport.listJobs().firstOrNull { it.submissionId == submissionId }?.id
    } catch (error: EngineException) {
        banner(error.error)
        null
    }

    @OptIn(ExperimentalUuidApi::class)
    private fun newSubmissionId(): String = "u-" + Uuid.random().toString()

    private suspend fun closeSessionQuietly(sessionId: String) {
        try {
            transport.closeSession(sessionId)
        } catch (error: EngineException) {
            // Closing an already-closed/unknown session is not user-facing.
        }
    }

    /** Reuse a completed job's accepted intent as a NEW editable session. */
    suspend fun reuseJobSettings(jobId: String): String? = try {
        val sessionId = transport.reuseJobSettings(jobId)
        val snapshot = transport.getSessionSnapshot(sessionId)
        _sessions.value = _sessions.value + (sessionId to snapshot)
        publishPrepared(snapshot.operation, sessionId, snapshot)
        sessionId
    } catch (error: EngineException) {
        banner(error.error)
        null
    }

    // ---- jobs (mandate §37) ----------------------------------------------------

    suspend fun getJob(jobId: String): JobSnapshot? = try {
        transport.getJob(jobId)
    } catch (error: EngineException) {
        banner(error.error)
        null
    }

    /**
     * velmux_is_job_active: distinguishes "cancel recorded" from "cancel
     * completed" before the UI touches artifacts.
     */
    suspend fun isJobActive(jobId: String): Boolean = try {
        transport.isJobActive(jobId)
    } catch (error: EngineException) {
        false
    }

    suspend fun cancelJob(jobId: String): Boolean = jobControl { transport.cancelJob(jobId) }

    suspend fun pauseJob(jobId: String): Boolean = jobControl { transport.pauseJob(jobId) }

    suspend fun resumeJob(jobId: String): Boolean = jobControl { transport.resumeJob(jobId) }

    suspend fun retryJob(jobId: String): Boolean = jobControl { transport.retryJob(jobId) }

    /**
     * Deletes a terminal job's durable record; the engine does not touch
     * output files. See [deleteManagedOutput] for output cleanup.
     */
    suspend fun deleteJobRecord(jobId: String): Boolean =
        jobControl { transport.deleteJob(jobId) }

    /**
     * Deletes the managed output files of a terminal job (platform file
     * deletion) and then the durable record. Returns false — keeping the
     * record — when an output file could not be removed (honesty over
     * silent divergence between disk and the job store).
     */
    suspend fun deleteManagedOutput(job: JobSnapshot): Boolean {
        val paths = buildList {
            addAll(job.outputs.map { it.path })
            if (job.outputPath.isNotEmpty() && job.outputs.none { it.path == job.outputPath }) {
                add(job.outputPath)
            }
        }
        var allRemoved = paths.isNotEmpty()
        for (path in paths) {
            val removed = withContext(Dispatchers.IO) { deleteFile(path) }
            if (!removed) allRemoved = false
        }
        if (!allRemoved) {
            banner(
                EngineError(
                    "output-delete-failed",
                    "Some output files could not be removed",
                )
            )
            return false
        }
        return jobControl { transport.deleteJob(job.id) }
    }

    private suspend fun jobControl(block: suspend () -> Boolean): Boolean {
        val result = try {
            block()
        } catch (error: EngineException) {
            banner(error.error)
            false
        }
        if (result) {
            wakeups.trySend(Unit)
            clientScope?.launch { refreshJobs() }
        }
        return result
    }

    // ---- diagnostics + platform integration ------------------------------------

    /** Feeds the scheduler's resource snapshot (all fields optional). */
    suspend fun updateResources(snapshot: ResourceSnapshot): Boolean = try {
        transport.updateResources(snapshot)
    } catch (error: EngineException) {
        banner(error.error)
        false
    }
}
