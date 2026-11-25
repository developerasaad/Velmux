package com.velmux.mobile.features.home

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.ErrorState
import com.velmux.mobile.components.FileListRow
import com.velmux.mobile.components.JobSummaryLine
import com.velmux.mobile.components.PendingIndicator
import com.velmux.mobile.components.Section
import com.velmux.mobile.components.WorkflowTile
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.JobStates
import com.velmux.mobile.engine.PreparationState
import com.velmux.mobile.features.jobs.JobStateUi
import com.velmux.mobile.features.jobs.operationLabel
import com.velmux.mobile.navigation.Navigator
import com.velmux.mobile.platform.openFile
import com.velmux.mobile.platform.rememberFilePicker

private const val ACTIVE_JOBS_SHOWN = 6
private const val RECENT_RESULTS_SHOWN = 5

/**
 * Home: two domains (Video & Audio) with the EIGHT primary workflows as
 * rows, the active-jobs summary (max 6 + "See all in Jobs") and recent
 * results (max 5). Picking files drives the honest preparation flow
 * (selected -> resolving -> importing (only when required) -> inspecting ->
 * configuring -> ready/failed/canceled).
 */
@Composable
fun HomeScreen(
    client: EngineClient,
    navigator: Navigator,
    modifier: Modifier = Modifier,
) {
    val scope = rememberCoroutineScope()
    val viewModel = remember { HomeViewModel(client, navigator, scope) }
    val preparation by client.preparation.collectAsState()
    val jobs by client.jobs.collectAsState()

    // One shared picker per screen; each tile configures it on demand.
    var pickerConfig by remember { mutableStateOf<WorkflowDef?>(null) }
    val picker = rememberFilePicker(
        mimeTypes = pickerConfig?.mimeTypes ?: listOf("*/*"),
        multiple = pickerConfig?.multiple ?: false,
    )
    LaunchedEffect(pickerConfig, picker) {
        picker.picks.collect { picks ->
            val workflow = pickerConfig
            if (picks.isNotEmpty() && workflow != null) {
                viewModel.onPicked(workflow, picks)
                picker.reset()
            }
        }
    }

    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
    ) {
        Column(modifier = Modifier.padding(horizontal = 16.dp)) {
            Spacer(Modifier.height(24.dp))
            Text(
                text = Strings.HOME_TITLE,
                style = MaterialTheme.typography.headlineSmall,
                color = MaterialTheme.colorScheme.onBackground,
            )
            Spacer(Modifier.height(2.dp))
            Text(
                text = Strings.HOME_SUBTITLE,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        PreparationCard(
            preparation = preparation,
            onCancel = { viewModel.cancelPreparation() },
        )

        Section(title = Strings.HOME_DOMAIN_VIDEO) {
            WorkflowGrid(workflows = VideoWorkflows) { workflow ->
                pickerConfig = workflow
                picker.launch()
            }
        }

        Section(title = Strings.HOME_DOMAIN_AUDIO) {
            WorkflowGrid(workflows = AudioWorkflows) { workflow ->
                pickerConfig = workflow
                picker.launch()
            }
        }

        Section(
            title = Strings.HOME_ACTIVE_JOBS,
            trailing = {
                TextButton(onClick = { viewModel.seeAllJobs() }) {
                    Text(Strings.HOME_SEE_ALL_JOBS)
                }
            },
        ) {
            val active = jobs
                .filter { it.state in JobStates.ONGOING }
                .take(ACTIVE_JOBS_SHOWN)
            if (active.isEmpty()) {
                Text(
                    text = Strings.HOME_NO_JOBS,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else {
                active.forEach { job ->
                    JobSummaryLine(
                        state = JobStateUi.of(job.state),
                        name = job.sourceName.ifBlank { job.id },
                        progress = job.progress,
                    )
                }
            }
        }

        Section(title = Strings.HOME_RECENT_RESULTS) {
            val results = jobs
                .filter { it.state in JobStates.TERMINAL }
                .filter { it.outputs.isNotEmpty() || it.outputPath.isNotEmpty() }
                .take(RECENT_RESULTS_SHOWN)
            if (results.isEmpty()) {
                Text(
                    text = Strings.HOME_NO_RESULTS,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else {
                results.forEach { job ->
                    FileListRow(
                        name = job.outputName.ifBlank { job.sourceName },
                        detail = operationLabel(job.operation) +
                            " · " + Strings.formatBytes(job.outputSizeBytes),
                        onClick = { openFile(job.outputPath) },
                    )
                }
            }
        }
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun WorkflowGrid(
    workflows: List<WorkflowDef>,
    onPick: (WorkflowDef) -> Unit,
) {
    val rows = (workflows.size + 1) / 2
    LazyVerticalGrid(
        columns = GridCells.Fixed(2),
        modifier = Modifier
            .fillMaxWidth()
            .height((rows * 132).dp),
        userScrollEnabled = false,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        items(workflows, key = { it.operation }) { workflow ->
            WorkflowTile(
                title = workflow.title,
                hint = workflow.hint,
                icon = workflow.icon,
                onClick = { onPick(workflow) },
            )
        }
    }
}

/** The honest preparation card (selected -> ... -> ready/failed/canceled). */
@Composable
private fun PreparationCard(
    preparation: PreparationState,
    onCancel: () -> Unit,
) {
    when (preparation) {
        PreparationState.Idle, PreparationState.Canceled -> Unit
        is PreparationState.Selected,
        is PreparationState.Resolving,
        is PreparationState.Importing,
        is PreparationState.Inspecting,
        -> {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 56.dp)
                    .padding(horizontal = 16.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                PendingIndicator()
                Spacer(Modifier.width(12.dp))
                Text(
                    text = when (preparation) {
                        is PreparationState.Selected -> Strings.PREP_SELECTED
                        is PreparationState.Resolving -> Strings.PREP_RESOLVING
                        is PreparationState.Importing -> Strings.PREP_IMPORTING
                        is PreparationState.Inspecting -> Strings.PREP_INSPECTING
                        else -> Strings.PREP_RESOLVING
                    },
                    style = MaterialTheme.typography.bodyLarge,
                    modifier = Modifier.weight(1f),
                )
                TextButton(onClick = onCancel) {
                    Text(Strings.PREP_CANCEL)
                }
            }
        }
        is PreparationState.Failed -> ErrorState(
            message = "${Strings.PREP_FAILED}: ${preparation.error.message}",
            onRetry = onCancel,
            retryLabel = Strings.BANNER_DISMISS,
        )
        is PreparationState.Ready, is PreparationState.Configuring -> Unit
    }
}
