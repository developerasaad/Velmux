package com.velmux.mobile.engine

import com.velmux.mobile.platform.PickedFile

/**
 * The fake transport for the engine-client tests: a programmable,
 * recording in-memory implementation of the full ABI v2 surface. It
 * overrides EVERY member of the (open) JVM dev actual, so nothing from
 * the dev no-engine path leaks into test runs.
 */
class FakeTransport : EngineTransport("fake-data-dir") {

    var abiVersionResult: Int = Abi.VERSION
    var engineVersionResult: String = "1.0.0-test"
    var backends: List<String> = listOf("ffmpeg")

    /** sessionId -> snapshot the engine currently holds. */
    val sessions = mutableMapOf<String, SessionSnapshot>()

    /** Queued responses for createSession (defaults to auto ids). */
    var createSessionHandler: (operation: String, sources: List<MediaSourceRequest>) -> String =
        { operation, _ ->
            val id = "s-${sessionCounter++}"
            sessions[id] = sessionSnapshot(id, operation, state = "created")
            id
        }

    var prepareSessionHandler: (sessionId: String) -> SessionSnapshot = { sessionId ->
        sessionSnapshot(
            sessionId,
            operation = sessions[sessionId]?.operation ?: "video-convert",
            state = "ready",
            revision = 1,
            configurationValid = true,
        )
    }

    var getSnapshotHandler: (sessionId: String) -> SessionSnapshot = { sessionId ->
        sessions.getValue(sessionId)
    }

    var optionsHandler: (sessionId: String, field: String, revision: Long) -> FieldOptions =
        { _, field, _ ->
            FieldOptions(
                field = field,
                values = listOf(
                    OptionValue(id = "mp4", attributes = mapOf("container" to "mp4")),
                    OptionValue(id = "mkv", attributes = mapOf("container" to "mkv")),
                ),
            )
        }

    /** Throws (stale-revision, related-change-required...) or returns a snapshot. */
    var updateHandler: (sessionId: String, request: SessionUpdateRequest) -> SessionSnapshot =
        { sessionId, request ->
            sessions.getValue(sessionId).copy(revision = request.revision + 1)
        }

    var submitHandler: (sessionId: String, revision: Long, submissionId: String) -> String =
        { _, _, _ -> "j-${jobCounter++}" }

    var sampleHandler: (sessionId: String, revision: Long) -> SampleResult = { _, revision ->
        SampleResult(path = "/tmp/sample.mp4", startUs = 0, endUs = 1_000_000, sizeBytes = 12)
    }

    var listJobsResult: List<JobSnapshot> = emptyList()

    /** Batches drained by pollEvents, oldest first. */
    val pollBatches = ArrayDeque<List<JobEvent>>()

    val jobsById = mutableMapOf<String, JobSnapshot>()

    // ---- recording ---------------------------------------------------------------

    var createSessionCalls: Int = 0
        private set
    var prepareSessionCalls: Int = 0
        private set
    var updateSessionCalls: Int = 0
        private set
    var submitCalls: Int = 0
        private set
    var listJobsCalls: Int = 0
        private set
    var pollEventsCalls: Int = 0
        private set
    var closeSessionCalls: Int = 0
        private set

    /** (sessionId, revision, submissionId) per submit call. */
    val submissions = mutableListOf<Triple<String, Long, String>>()

    // ---- ABI surface ---------------------------------------------------------------

    override suspend fun abiVersion(): Int = abiVersionResult

    override suspend fun engineVersion(): String = engineVersionResult

    override suspend fun createSession(
        operation: String,
        sources: List<MediaSourceRequest>,
    ): String {
        createSessionCalls++
        return createSessionHandler(operation, sources)
    }

    override suspend fun prepareSession(sessionId: String): SessionSnapshot {
        prepareSessionCalls++
        val snapshot = prepareSessionHandler(sessionId)
        sessions[sessionId] = snapshot
        return snapshot
    }

    override suspend fun getSessionSnapshot(sessionId: String): SessionSnapshot =
        getSnapshotHandler(sessionId)

    override suspend fun getSessionOptions(
        sessionId: String,
        field: String,
        revision: Long,
    ): FieldOptions = optionsHandler(sessionId, field, revision)

    override suspend fun updateSession(
        sessionId: String,
        request: SessionUpdateRequest,
    ): SessionSnapshot {
        updateSessionCalls++
        val result = updateHandler(sessionId, request)
        sessions[sessionId] = result
        return result
    }

    override suspend fun generateSample(sessionId: String, revision: Long): SampleResult =
        sampleHandler(sessionId, revision)

    override suspend fun submitSession(
        sessionId: String,
        revision: Long,
        submissionId: String,
    ): String {
        submitCalls++
        submissions.add(Triple(sessionId, revision, submissionId))
        return submitHandler(sessionId, revision, submissionId)
    }

    override suspend fun closeSession(sessionId: String): Boolean {
        closeSessionCalls++
        sessions.remove(sessionId)
        return true
    }

    override suspend fun submitJob(requestJson: String): String = "j-direct"

    override suspend fun listJobs(): List<JobSnapshot> {
        listJobsCalls++
        return (listJobsResult + jobsById.values).distinctBy { it.id }
            .sortedByDescending { it.createdAtMs }
    }

    override suspend fun getJob(jobId: String): JobSnapshot? = jobsById[jobId]

    override suspend fun isJobActive(jobId: String): Boolean {
        val state = jobsById[jobId]?.state ?: return false
        return state in JobStates.ACTIVE
    }

    override suspend fun cancelJob(jobId: String): Boolean {
        jobsById[jobId] = jobsById[jobId]?.copy(state = JobStates.CANCELED) ?: return false
        return true
    }

    override suspend fun pauseJob(jobId: String): Boolean {
        jobsById[jobId] = jobsById[jobId]?.copy(state = JobStates.PAUSED) ?: return false
        return true
    }

    override suspend fun resumeJob(jobId: String): Boolean {
        jobsById[jobId] = jobsById[jobId]?.copy(state = JobStates.RUNNING) ?: return false
        return true
    }

    override suspend fun retryJob(jobId: String): Boolean {
        jobsById[jobId] = jobsById[jobId]?.copy(state = JobStates.QUEUED) ?: return false
        return true
    }

    override suspend fun deleteJob(jobId: String): Boolean = jobsById.remove(jobId) != null

    override suspend fun reuseJobSettings(jobId: String): String {
        val id = "s-reuse-${sessionCounter++}"
        sessions[id] = sessionSnapshot(id, "video-convert", state = "ready", revision = 5)
        return id
    }

    override suspend fun inspect(uri: String, displayName: String): MediaInfo =
        mediaInfoFixture()

    override suspend fun describeBackends(): List<String> = backends

    override suspend fun updateResources(snapshot: ResourceSnapshot): Boolean = true

    override suspend fun pollEvents(): List<JobEvent> {
        pollEventsCalls++
        return pollBatches.removeFirstOrNull() ?: emptyList()
    }

    override fun dispose() {
        disposeCalls++
    }

    var disposeCalls: Int = 0
        private set

    private var sessionCounter: Int = 0
    private var jobCounter: Int = 1

    companion object {
        fun sessionSnapshot(
            sessionId: String,
            operation: String,
            state: String,
            revision: Long = 0,
            configurationValid: Boolean = false,
            values: List<AcceptedValue> = emptyList(),
            relatedChange: RelatedChange? = null,
        ): SessionSnapshot = SessionSnapshot(
            sessionId = sessionId,
            operation = operation,
            state = state,
            revision = revision,
            sources = listOf(mediaInfoFixture()),
            values = values,
            configurationValid = configurationValid,
            validationMessage = null,
            estimate = OutputEstimate(
                known = true,
                estimatedBytes = 900_000,
                lowerBoundBytes = 800_000,
                upperBoundBytes = 1_000_000,
            ),
            relatedChange = relatedChange,
            samplePath = null,
        )

        fun jobSnapshot(
            id: String,
            state: String,
            progress: Double = -1.0,
            submissionId: String? = null,
            createdAtMs: Long = 1_000,
        ): JobSnapshot = JobSnapshot(
            id = id,
            submissionId = submissionId,
            operation = "video-convert",
            state = state,
            progress = progress,
            sourceName = "clip.mp4",
            outputName = "clip.mkv",
            outputPath = "/outputs/clip.mkv",
            outputs = listOf(JobOutput("/outputs/clip.mkv", 900_000, "media")),
            error = null,
            errorCode = null,
            outputSizeBytes = 900_000,
            inputSizeBytes = 1_000_000,
            pipelineSummary = "decode h264 -> encode h264",
            createdAtMs = createdAtMs,
            finishedAtMs = 2_000,
        )

        fun mediaInfoFixture(): MediaInfo = MediaInfo(
            durationSec = 63.5,
            container = "mp4",
            sizeBytes = 1_048_576,
            attachedPictures = 0,
            video = listOf(
                VideoStream(
                    codec = "h264",
                    width = 1920,
                    height = 1080,
                    frameRate = 29.97,
                    bitRate = 5_000_000,
                    pixelFormat = "yuv420p",
                    hdr = false,
                    hasClosedCaptions = true,
                    language = "und",
                )
            ),
            audio = listOf(
                AudioStream(
                    codec = "aac",
                    channels = 2,
                    sampleRate = 48_000,
                    bitRate = 128_000,
                    language = null,
                )
            ),
            subtitles = listOf(
                SubtitleStream(
                    format = "mov_text",
                    language = "eng",
                    isDefault = true,
                    isForced = false,
                )
            ),
        )

        fun pick(name: String = "clip.mp4"): PickedFile =
            PickedFile(uri = "file:///inputs/$name", displayName = name)
    }
}
