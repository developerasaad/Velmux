package com.velmux.mobile.features.jobs

import com.velmux.mobile.Strings
import com.velmux.mobile.engine.JobStates

/**
 * The pure jobs-state mapping (wire state -> UI presentation): labels,
 * terminal/ongoing classification, filters and per-state actions.
 * Exhaustive over the fourteen canonical states — covered by the jobs
 * state mapping tests.
 */
enum class JobStateUi(
    val wire: String,
    val label: String,
) {
    QUEUED(JobStates.QUEUED, "Queued"),
    PREPARING(JobStates.PREPARING, "Preparing"),
    RUNNING(JobStates.RUNNING, "Running"),
    PAUSING(JobStates.PAUSING, "Pausing"),
    PAUSED(JobStates.PAUSED, "Paused"),
    CANCELING(JobStates.CANCELING, "Canceling"),
    INTERRUPTED(JobStates.INTERRUPTED, "Interrupted"),
    RECOVERING(JobStates.RECOVERING, "Recovering"),
    FINALIZING(JobStates.FINALIZING, "Finalizing"),
    VALIDATING(JobStates.VALIDATING, "Validating"),
    COMPLETED(JobStates.COMPLETED, "Completed"),
    TARGET_NOT_MET(JobStates.TARGET_NOT_MET, Strings.JOBS_TARGET_NOT_MET),
    FAILED(JobStates.FAILED, "Failed"),
    CANCELED(JobStates.CANCELED, Strings.JOBS_CANCELED),
    ;

    val isTerminal: Boolean get() = wire in JobStates.TERMINAL
    val isFailed: Boolean get() = this == FAILED
    val isOngoing: Boolean get() = wire in JobStates.ONGOING
    val hasOutput: Boolean get() = this == COMPLETED || this == TARGET_NOT_MET

    companion object {
        private val byWire = entries.associateBy { it.wire }

        /** Maps a wire state; unknown future states render as ongoing. */
        fun of(wire: String): JobStateUi = byWire[wire] ?: PREPARING
    }
}

/** User-facing job actions, derived from state only (never from progress). */
enum class JobAction {
    CANCEL,
    PAUSE,
    RESUME,
    RETRY,
    OPEN,
    SHARE,
    REUSE,
    DELETE_RECORD,
    DELETE_OUTPUT,
    DETAILS,
}

/** Filters of the ONE Jobs screen. */
enum class JobsFilter(val label: String) {
    ALL(Strings.JOBS_FILTER_ALL),
    ACTIVE(Strings.JOBS_FILTER_ACTIVE),
    COMPLETED(Strings.JOBS_FILTER_COMPLETED),
    FAILED(Strings.JOBS_FILTER_FAILED),
    ;

    fun accepts(state: JobStateUi): Boolean = when (this) {
        ALL -> true
        ACTIVE -> state.isOngoing
        COMPLETED -> state == JobStateUi.COMPLETED ||
            state == JobStateUi.TARGET_NOT_MET
        FAILED -> state.isFailed
    }
}

/**
 * Per-state actions (the honest set — the engine rejects impossible
 * transitions and the client surfaces that; the UI never offers an action
 * the state cannot carry):
 *
 *  - queued/preparing/recovering: cancel;
 *  - running: pause + cancel;
 *  - paused: resume;
 *  - canceling/pausing/finalizing/validating: no action (in flight);
 *  - interrupted: retry (the durable intent is preserved);
 *  - completed/target-not-met: open, share, reuse, delete record, delete
 *    managed output, details;
 *  - failed: retry, details, delete record;
 *  - canceled: retry, delete record.
 */
fun actionsFor(state: JobStateUi): List<JobAction> = when (state) {
    JobStateUi.QUEUED, JobStateUi.PREPARING, JobStateUi.RECOVERING ->
        listOf(JobAction.CANCEL)
    JobStateUi.RUNNING -> listOf(JobAction.PAUSE, JobAction.CANCEL)
    JobStateUi.PAUSED -> listOf(JobAction.RESUME)
    JobStateUi.PAUSING, JobStateUi.CANCELING, JobStateUi.FINALIZING,
    JobStateUi.VALIDATING,
    -> emptyList()
    JobStateUi.INTERRUPTED -> listOf(JobAction.RETRY)
    JobStateUi.COMPLETED, JobStateUi.TARGET_NOT_MET -> listOf(
        JobAction.OPEN,
        JobAction.SHARE,
        JobAction.REUSE,
        JobAction.DELETE_RECORD,
        JobAction.DELETE_OUTPUT,
        JobAction.DETAILS,
    )
    JobStateUi.FAILED -> listOf(
        JobAction.RETRY,
        JobAction.DETAILS,
        JobAction.DELETE_RECORD,
    )
    JobStateUi.CANCELED -> listOf(
        JobAction.RETRY,
        JobAction.DELETE_RECORD,
    )
}

/** The label for a job row (operation name for v1; workflow names come with UI-2). */
fun operationLabel(operation: String): String = when (operation) {
    "video-convert" -> Strings.WORKFLOW_VIDEO_CONVERT
    "video-crop" -> Strings.WORKFLOW_VIDEO_CROP
    "video-cutmerge" -> Strings.WORKFLOW_VIDEO_CUTMERGE
    "extract-audio" -> Strings.WORKFLOW_EXTRACT_AUDIO
    "extract-frames" -> Strings.WORKFLOW_EXTRACT_FRAMES
    "extract-subtitles" -> Strings.WORKFLOW_EXTRACT_SUBTITLES
    "audio-convert" -> Strings.WORKFLOW_AUDIO_CONVERT
    "audio-cutmerge" -> Strings.WORKFLOW_AUDIO_CUTMERGE
    else -> operation
}
