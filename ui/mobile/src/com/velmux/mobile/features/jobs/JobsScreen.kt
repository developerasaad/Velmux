package com.velmux.mobile.features.jobs

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.EmptyState
import com.velmux.mobile.components.Section
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.JobSnapshot
import com.velmux.mobile.navigation.Destination
import com.velmux.mobile.navigation.Navigator
import com.velmux.mobile.platform.openFile
import com.velmux.mobile.platform.shareFile
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

/**
 * Jobs view model: ONE screen for active AND previous jobs. Filter and
 * search are view state; the job list itself always renders
 * [EngineClient.jobs] (the single source of truth).
 */
class JobsViewModel(
    private val client: EngineClient,
    private val navigator: Navigator,
    private val scope: kotlinx.coroutines.CoroutineScope,
) {
    private val _filter = MutableStateFlow(JobsFilter.ALL)
    val filter: StateFlow<JobsFilter> = _filter

    private val _query = MutableStateFlow("")
    val query: StateFlow<String> = _query

    fun setFilter(filter: JobsFilter) {
        _filter.value = filter
    }

    fun setQuery(query: String) {
        _query.value = query
    }

    fun visible(jobs: List<JobSnapshot>): List<JobSnapshot> {
        val filter = _filter.value
        val query = _query.value.trim()
        return jobs.filter { job ->
            filter.accepts(JobStateUi.of(job.state))
        }.filter { job ->
            query.isEmpty() ||
                job.sourceName.contains(query, ignoreCase = true) ||
                job.outputName.contains(query, ignoreCase = true) ||
                job.operation.contains(query, ignoreCase = true)
        }
    }

    fun cancel(job: JobSnapshot) = scope.launch { client.cancelJob(job.id) }
    fun pause(job: JobSnapshot) = scope.launch { client.pauseJob(job.id) }
    fun resume(job: JobSnapshot) = scope.launch { client.resumeJob(job.id) }
    fun retry(job: JobSnapshot) = scope.launch { client.retryJob(job.id) }

    fun open(job: JobSnapshot) {
        if (job.outputPath.isNotEmpty()) openFile(job.outputPath)
    }

    fun share(job: JobSnapshot) {
        if (job.outputPath.isNotEmpty()) shareFile(job.outputPath)
    }

    fun reuse(job: JobSnapshot) = scope.launch {
        val sessionId = client.reuseJobSettings(job.id)
        if (sessionId != null) {
            navigator.open(Destination.Workflow(job.operation, sessionId))
        }
    }

    fun deleteRecord(job: JobSnapshot) = scope.launch { client.deleteJobRecord(job.id) }

    fun deleteOutput(job: JobSnapshot) = scope.launch {
        client.deleteManagedOutput(job)
    }
}

/**
 * The ONE unified Jobs screen (never separate Queue/History screens):
 * active and previous jobs, filters All/Active/Completed/Failed, search,
 * per-state actions, multi-output manifests fully shown, failure details,
 * and the honest "Target not met" story.
 */
@Composable
fun JobsScreen(
    client: EngineClient,
    navigator: Navigator,
    modifier: Modifier = Modifier,
) {
    val scope = rememberCoroutineScope()
    val viewModel = remember { JobsViewModel(client, navigator, scope) }
    val jobs by client.jobs.collectAsState()
    val filter by viewModel.filter.collectAsState()
    val query by viewModel.query.collectAsState()

    val visible = viewModel.visible(jobs)
    val active = visible.filter { it.state in com.velmux.mobile.engine.JobStates.ONGOING }
    val previous = visible.filter { it.state in com.velmux.mobile.engine.JobStates.TERMINAL }

    Column(modifier = modifier.fillMaxSize()) {
        Spacer(Modifier.height(24.dp))
        OutlinedTextField(
            value = query,
            onValueChange = viewModel::setQuery,
            placeholder = { Text(Strings.JOBS_SEARCH_HINT) },
            singleLine = true,
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp),
        )
        Spacer(Modifier.height(8.dp))
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            JobsFilter.entries.forEach { entry ->
                FilterChip(
                    selected = entry == filter,
                    onClick = { viewModel.setFilter(entry) },
                    label = { Text(entry.label) },
                )
            }
        }

        if (visible.isEmpty()) {
            EmptyState(
                title = if (jobs.isEmpty()) Strings.JOBS_EMPTY else Strings.JOBS_EMPTY_FILTERED,
                message = Strings.HOME_SUBTITLE,
                icon = com.velmux.mobile.design.VelmuxIcons.Jobs,
            )
        } else {
            LazyColumn(modifier = Modifier.fillMaxSize()) {
                if (active.isNotEmpty()) {
                    item {
                        Section(title = Strings.JOBS_ACTIVE_SECTION) {
                            Text(
                                text = "${active.size}",
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                    items(active, key = { it.id }) { job ->
                        JobRowFor(job, viewModel)
                    }
                }
                if (previous.isNotEmpty()) {
                    item {
                        Section(title = Strings.JOBS_PREVIOUS_SECTION) {
                            Text(
                                text = "${previous.size}",
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                    items(previous, key = { it.id }) { job ->
                        JobRowFor(job, viewModel)
                    }
                }
                item { Spacer(Modifier.height(24.dp)) }
            }
        }
    }
}

@Composable
private fun JobRowFor(job: JobSnapshot, viewModel: JobsViewModel) {
    JobRow(
        job = job,
        onCancel = { viewModel.cancel(job) },
        onPause = { viewModel.pause(job) },
        onResume = { viewModel.resume(job) },
        onRetry = { viewModel.retry(job) },
        onOpen = { viewModel.open(job) },
        onShare = { viewModel.share(job) },
        onReuse = { viewModel.reuse(job) },
        onDeleteRecord = { viewModel.deleteRecord(job) },
        onDeleteOutput = { viewModel.deleteOutput(job) },
    )
}
