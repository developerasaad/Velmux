package com.velmux.mobile.features.jobs

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.ActionChip
import com.velmux.mobile.components.FileListRow
import com.velmux.mobile.components.LabeledValue
import com.velmux.mobile.components.ProgressRow
import com.velmux.mobile.components.StateChip
import com.velmux.mobile.components.actionRow
import com.velmux.mobile.design.VelmuxIcons
import com.velmux.mobile.engine.JobSnapshot

/**
 * One job row of the unified Jobs screen: state chip, source/output names,
 * honest progress, per-state actions, and (expanded) the multi-output
 * manifest, failure details, pipeline summary and the target-not-met
 * story. Stable row height while collapsed.
 */
@Composable
fun JobRow(
    job: JobSnapshot,
    modifier: Modifier = Modifier,
    onCancel: () -> Unit,
    onPause: () -> Unit,
    onResume: () -> Unit,
    onRetry: () -> Unit,
    onOpen: () -> Unit,
    onShare: () -> Unit,
    onReuse: () -> Unit,
    onDeleteRecord: () -> Unit,
    onDeleteOutput: () -> Unit,
) {
    val state = JobStateUi.of(job.state)
    val actions = remember(job.state) { actionsFor(state) }
    var expanded by remember { mutableStateOf(false) }

    Surface(
        modifier = modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp),
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceVariant,
        contentColor = MaterialTheme.colorScheme.onSurfaceVariant,
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .actionRow { expanded = !expanded },
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = job.sourceName.ifBlank { job.id },
                        style = MaterialTheme.typography.titleMedium,
                        color = MaterialTheme.colorScheme.onSurface,
                        maxLines = 1,
                    )
                    Text(
                        text = operationLabel(job.operation),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
                Spacer(Modifier.width(8.dp))
                StateChip(state = state)
            }

            Spacer(Modifier.height(8.dp))
            ProgressRow(progress = job.progress)

            if (state.hasOutput) {
                Spacer(Modifier.height(8.dp))
                Text(
                    text = when (state) {
                        JobStateUi.TARGET_NOT_MET ->
                            "${Strings.JOBS_TARGET_NOT_MET} · ${Strings.formatBytes(job.outputSizeBytes)}"
                        else -> job.outputName.ifBlank { job.outputPath }
                    },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurface,
                    maxLines = 1,
                )
            }

            if (expanded) {
                Details(job = job, state = state)
            }

            if (actions.isNotEmpty()) {
                Spacer(Modifier.height(8.dp))
                Row(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    actions.forEach { action ->
                        ActionFor(
                            action = action,
                            expanded = expanded,
                            onToggleDetails = { expanded = !expanded },
                            onCancel = onCancel,
                            onPause = onPause,
                            onResume = onResume,
                            onRetry = onRetry,
                            onOpen = onOpen,
                            onShare = onShare,
                            onReuse = onReuse,
                            onDeleteRecord = onDeleteRecord,
                            onDeleteOutput = onDeleteOutput,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun Details(job: JobSnapshot, state: JobStateUi) {
    Spacer(Modifier.height(8.dp))
    if (state == JobStateUi.FAILED || job.error != null) {
        Text(
            text = Strings.JOBS_ERROR_SECTION,
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.error,
        )
        Text(
            text = job.error ?: Strings.SIZE_UNKNOWN,
            style = MaterialTheme.typography.bodyMedium,
        )
        job.errorCode?.let { code ->
            Text(
                text = code,
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.height(8.dp))
    }
    if (job.outputs.isNotEmpty()) {
        Text(
            text = Strings.JOBS_OUTPUTS,
            style = MaterialTheme.typography.labelMedium,
        )
        job.outputs.forEach { output ->
            FileListRow(
                name = output.path.substringAfterLast('/'),
                detail = "${Strings.formatBytes(output.sizeBytes)} · ${output.kind}",
                leading = when (output.kind) {
                    "image" -> VelmuxIcons.Frames
                    "subtitle" -> VelmuxIcons.Subtitles
                    "media" -> VelmuxIcons.VideoConvert
                    else -> VelmuxIcons.Info
                },
            )
        }
        Spacer(Modifier.height(4.dp))
    }
    Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
        LabeledValue(
            label = "Input",
            value = Strings.formatBytes(job.inputSizeBytes),
        )
        LabeledValue(
            label = "Output",
            value = Strings.formatBytes(job.outputSizeBytes),
        )
    }
    job.pipelineSummary?.let { summary ->
        Spacer(Modifier.height(4.dp))
        LabeledValue(label = Strings.JOBS_PIPELINE, value = summary)
    }
}

@Composable
private fun ActionFor(
    action: JobAction,
    expanded: Boolean,
    onToggleDetails: () -> Unit,
    onCancel: () -> Unit,
    onPause: () -> Unit,
    onResume: () -> Unit,
    onRetry: () -> Unit,
    onOpen: () -> Unit,
    onShare: () -> Unit,
    onReuse: () -> Unit,
    onDeleteRecord: () -> Unit,
    onDeleteOutput: () -> Unit,
) {
    when (action) {
        JobAction.CANCEL -> ActionChip(
            label = Strings.JOBS_CANCEL,
            icon = VelmuxIcons.Stop,
            destructive = true,
            onClick = onCancel,
        )
        JobAction.PAUSE -> ActionChip(
            label = Strings.JOBS_PAUSE,
            icon = VelmuxIcons.Pause,
            onClick = onPause,
        )
        JobAction.RESUME -> ActionChip(
            label = Strings.JOBS_RESUME,
            icon = VelmuxIcons.Play,
            onClick = onResume,
        )
        JobAction.RETRY -> ActionChip(
            label = Strings.JOBS_RETRY,
            icon = VelmuxIcons.Refresh,
            onClick = onRetry,
        )
        JobAction.OPEN -> ActionChip(
            label = Strings.JOBS_OPEN,
            icon = VelmuxIcons.Open,
            onClick = onOpen,
        )
        JobAction.SHARE -> ActionChip(
            label = Strings.JOBS_SHARE,
            icon = VelmuxIcons.Share,
            onClick = onShare,
        )
        JobAction.REUSE -> ActionChip(
            label = Strings.JOBS_REUSE,
            icon = VelmuxIcons.CutMerge,
            onClick = onReuse,
        )
        JobAction.DELETE_RECORD -> ActionChip(
            label = Strings.JOBS_DELETE_RECORD,
            icon = VelmuxIcons.Delete,
            destructive = true,
            onClick = onDeleteRecord,
        )
        JobAction.DELETE_OUTPUT -> ActionChip(
            label = Strings.JOBS_DELETE_OUTPUT,
            icon = VelmuxIcons.Delete,
            destructive = true,
            onClick = onDeleteOutput,
        )
        JobAction.DETAILS -> ActionChip(
            label = if (expanded) {
                Strings.JOBS_HIDE_DETAILS
            } else {
                Strings.JOBS_DETAILS
            },
            icon = if (expanded) VelmuxIcons.Collapse else VelmuxIcons.Info,
            onClick = onToggleDetails,
        )
    }
}
