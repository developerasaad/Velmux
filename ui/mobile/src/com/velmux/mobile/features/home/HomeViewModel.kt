package com.velmux.mobile.features.home

import androidx.compose.ui.graphics.vector.ImageVector
import com.velmux.mobile.Strings
import com.velmux.mobile.design.VelmuxIcons
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.JobSnapshot
import com.velmux.mobile.engine.PreparationState
import com.velmux.mobile.navigation.Destination
import com.velmux.mobile.navigation.Navigator
import com.velmux.mobile.platform.PickedFile
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

/** One primary workflow of the Home grid. */
data class WorkflowDef(
    val operation: String,
    val title: String,
    val hint: String,
    val icon: ImageVector,
    val mimeTypes: List<String>,
    val multiple: Boolean,
)

/**
 * The EIGHT primary workflows across the two domains, exactly as the
 * product scope fixes them (mandate §9): Video owns six — Converter,
 * Crop, Cut & Merge, Extract Audio, Extract Frames, Extract Subtitles;
 * Audio owns two — Converter, Cut & Merge.
 */
val VideoWorkflows: List<WorkflowDef> = listOf(
    WorkflowDef(
        operation = "video-convert",
        title = Strings.WORKFLOW_VIDEO_CONVERT,
        hint = Strings.WORKFLOW_VIDEO_CONVERT_HINT,
        icon = VelmuxIcons.VideoConvert,
        mimeTypes = listOf("video/*"),
        multiple = true,
    ),
    WorkflowDef(
        operation = "video-crop",
        title = Strings.WORKFLOW_VIDEO_CROP,
        hint = Strings.WORKFLOW_VIDEO_CROP_HINT,
        icon = VelmuxIcons.Crop,
        mimeTypes = listOf("video/*"),
        multiple = false,
    ),
    WorkflowDef(
        operation = "video-cutmerge",
        title = Strings.WORKFLOW_VIDEO_CUTMERGE,
        hint = Strings.WORKFLOW_VIDEO_CUTMERGE_HINT,
        icon = VelmuxIcons.CutMerge,
        mimeTypes = listOf("video/*"),
        multiple = true,
    ),
    WorkflowDef(
        operation = "extract-audio",
        title = Strings.WORKFLOW_EXTRACT_AUDIO,
        hint = Strings.WORKFLOW_EXTRACT_AUDIO_HINT,
        icon = VelmuxIcons.Audio,
        mimeTypes = listOf("video/*"),
        multiple = true,
    ),
    WorkflowDef(
        operation = "extract-frames",
        title = Strings.WORKFLOW_EXTRACT_FRAMES,
        hint = Strings.WORKFLOW_EXTRACT_FRAMES_HINT,
        icon = VelmuxIcons.Frames,
        mimeTypes = listOf("video/*"),
        multiple = true,
    ),
    WorkflowDef(
        operation = "extract-subtitles",
        title = Strings.WORKFLOW_EXTRACT_SUBTITLES,
        hint = Strings.WORKFLOW_EXTRACT_SUBTITLES_HINT,
        icon = VelmuxIcons.Subtitles,
        mimeTypes = listOf("video/*"),
        multiple = true,
    ),
)

val AudioWorkflows: List<WorkflowDef> = listOf(
    WorkflowDef(
        operation = "audio-convert",
        title = Strings.WORKFLOW_AUDIO_CONVERT,
        hint = Strings.WORKFLOW_AUDIO_CONVERT_HINT,
        icon = VelmuxIcons.Audio,
        mimeTypes = listOf("audio/*"),
        multiple = true,
    ),
    WorkflowDef(
        operation = "audio-cutmerge",
        title = Strings.WORKFLOW_AUDIO_CUTMERGE,
        hint = Strings.WORKFLOW_AUDIO_CUTMERGE_HINT,
        icon = VelmuxIcons.CutMerge,
        mimeTypes = listOf("audio/*"),
        multiple = true,
    ),
)

/**
 * Home view model: derives the active-jobs summary (max 6) and recent
 * results (max 5) from the engine client, and drives workflow starts
 * (file picking -> preparation flow -> workflow destination).
 */
class HomeViewModel(
    private val client: EngineClient,
    private val navigator: Navigator,
    private val scope: CoroutineScope,
) {

    /** The workflow the current pick flow belongs to (tile highlighting). */
    private val _pendingWorkflow = MutableStateFlow<WorkflowDef?>(null)
    val pendingWorkflow: StateFlow<WorkflowDef?> = _pendingWorkflow

    val activeJobs: List<JobSnapshot> get() = client.jobs.value
        .filter { it.state in com.velmux.mobile.engine.JobStates.ONGOING }
        .take(ACTIVE_JOBS_MAX)

    val recentResults: List<JobSnapshot> get() = client.jobs.value
        .filter { it.state in com.velmux.mobile.engine.JobStates.TERMINAL }
        .filter { it.outputs.isNotEmpty() || it.outputPath.isNotEmpty() }
        .take(RECENT_RESULTS_MAX)

    val preparation: PreparationState get() = client.preparation.value

    /** Called when the picker delivered [picks] for [workflow]. */
    fun onPicked(workflow: WorkflowDef, picks: List<PickedFile>) {
        if (picks.isEmpty()) {
            _pendingWorkflow.value = null
            return
        }
        _pendingWorkflow.value = workflow
        scope.launch {
            if (picks.size > 1 && workflow.operation in BATCHABLE_OPERATIONS) {
                // A multi-file pick on a single-source operation starts a
                // batch (mandate §40): one independently resolved session
                // per file sharing one user intent.
                val started = client.beginBatch(workflow.operation, picks)
                _pendingWorkflow.value = null
                val entries = started.mapNotNull { result ->
                    result.sessionId?.let { id ->
                        Destination.BatchEntry(name = result.pick.displayName, sessionId = id)
                    }
                }
                if (entries.isNotEmpty()) {
                    navigator.open(Destination.WorkflowBatch(workflow.operation, entries))
                }
            } else {
                val sessionId = client.beginWorkflow(workflow.operation, picks)
                _pendingWorkflow.value = null
                if (sessionId != null) {
                    navigator.open(Destination.Workflow(workflow.operation, sessionId))
                }
            }
        }
    }

    fun cancelPreparation() {
        scope.launch { client.cancelPreparation() }
    }

    fun seeAllJobs() {
        navigator.open(Destination.Jobs)
    }

    fun openResult(job: JobSnapshot) {
        if (job.outputPath.isNotEmpty()) {
            com.velmux.mobile.platform.openFile(job.outputPath)
        }
    }

    private companion object {
        /**
         * Operations whose multi-file picks form a batch (mandate §40:
         * batch conversion, compression and extraction). Merge workflows
         * consume multiple sources in ONE session; Crop keeps a single
         * photo-style surface.
         */
        val BATCHABLE_OPERATIONS = setOf(
            "video-convert",
            "audio-convert",
            "extract-audio",
            "extract-frames",
            "extract-subtitles",
        )
        const val ACTIVE_JOBS_MAX = 6
        const val RECENT_RESULTS_MAX = 5
    }
}
