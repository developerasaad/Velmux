package com.velmux.mobile.features.workflow

import com.velmux.mobile.fmt
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Button
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.OptionRow
import com.velmux.mobile.components.PendingIndicator
import com.velmux.mobile.components.Section
import com.velmux.mobile.components.SelectionOption
import com.velmux.mobile.components.SelectionSheet
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.engine.SessionSnapshot
import com.velmux.mobile.engine.ValueLabels
import com.velmux.mobile.features.jobs.operationLabel
import com.velmux.mobile.navigation.Destination
import com.velmux.mobile.navigation.Navigator

/**
 * The batch configuration surface (mandate §40): a group of
 * independently resolved operations sharing one user intent.
 *
 * The file list shows per-file states and estimates (a file that cannot
 * accept a shared setting keeps its accepted state and is honest about
 * it — it never invalidates the others). The shared settings broadcast
 * one change to every session; option lists are the intersection across
 * the files, so a shared selection is one every file supports.
 * Individual overrides push the normal single-file workflow screen on
 * top of this route. Submission creates one job per valid file.
 */
@Composable
fun BatchScreen(
    batch: Destination.WorkflowBatch,
    client: EngineClient,
    navigator: Navigator,
    modifier: Modifier = Modifier,
) {
    val scope = rememberCoroutineScope()
    val controller = remember(batch) {
        BatchWorkflowController(batch.operation, batch.entries, client, navigator, scope)
    }
    val sessions by client.sessions.collectAsState()
    val submitting by controller.submitting.collectAsState()
    val loadingFields by controller.loadingFields.collectAsState()

    // Snapshots recompute as the core updates each session (broadcast,
    // per-file validation, overrides).
    val rows: List<Pair<Destination.BatchEntry, SessionSnapshot>> =
        batch.entries.mapNotNull { entry ->
            sessions[entry.sessionId]?.let { entry to it }
        }
    val readyCount = rows.count { it.second.configurationValid }

    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
    ) {
        Spacer(Modifier.height(8.dp))
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconButton(onClick = { navigator.back() }) {
                Icon(
                    imageVector = com.velmux.mobile.design.VelmuxIcons.Collapse,
                    contentDescription = Strings.WORKFLOW_BACK,
                )
            }
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = operationLabel(batch.operation),
                    style = MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    text = Strings.BATCH_READY_COUNT.fmt(readyCount, rows.size),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }

        Section(title = Strings.BATCH_FILES_SECTION) {
            Column(
                modifier = Modifier.fillMaxWidth(),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                rows.forEach { (entry, snapshot) ->
                    BatchFileRow(
                        name = entry.name,
                        snapshot = snapshot,
                        estimate = controller.estimateLabel(snapshot),
                        onAdjust = { controller.openOverride(entry.sessionId) },
                    )
                }
            }
        }

        Section(title = Strings.BATCH_SHARED_SECTION) {
            Column(modifier = Modifier.fillMaxWidth()) {
                BatchWorkflowController.sharedFields[batch.operation]
                    .orEmpty()
                    .forEach { field ->
                        SharedFieldRow(
                            field = field,
                            controller = controller,
                            loading = field in loadingFields,
                        )
                    }
                SharedCompressionRow(controller = controller)
            }
        }

        // Stable action area (mandate §19: the action stays accessible).
        Surface(
            tonalElevation = 2.dp,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 72.dp)
                    .padding(horizontal = 16.dp, vertical = 12.dp),
            ) {
                Button(
                    onClick = { controller.submitAll() },
                    enabled = !submitting && readyCount > 0,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    if (submitting) {
                        PendingIndicator()
                        Spacer(Modifier.width(8.dp))
                    }
                    Text(batchSubmitLabel(batch.operation))
                }
                if (readyCount < rows.size) {
                    Spacer(Modifier.height(4.dp))
                    Text(
                        text = Strings.BATCH_FILE_INVALID,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

/** One file row: name, per-file state, estimate, override entry point. */
@Composable
private fun BatchFileRow(
    name: String,
    snapshot: SessionSnapshot,
    estimate: String,
    onAdjust: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(
        shape = MaterialTheme.shapes.medium,
        tonalElevation = 1.dp,
        modifier = modifier.fillMaxWidth(),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = name,
                    style = MaterialTheme.typography.bodyMedium,
                    maxLines = 1,
                )
                val stateText = when {
                    snapshot.configurationValid -> Strings.BATCH_FILE_READY
                    else -> snapshot.validationMessage
                        ?: Strings.BATCH_FILE_INVALID
                }
                Text(
                    text = if (estimate.isEmpty()) stateText else "$stateText · $estimate",
                    style = MaterialTheme.typography.bodySmall,
                    color = if (snapshot.configurationValid) {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    } else {
                        MaterialTheme.colorScheme.error
                    },
                    maxLines = 2,
                )
            }
            OutlinedButton(onClick = onAdjust) {
                Text(Strings.BATCH_ADJUST)
            }
        }
    }
}

/**
 * A shared selector row: shows the COMMON accepted value (or "Varies"),
 * opens the intersection option sheet, broadcasts the selection to every
 * session (mandate §40 — per-file engines validate independently).
 */
@Composable
private fun SharedFieldRow(
    field: String,
    controller: BatchWorkflowController,
    loading: Boolean,
    modifier: Modifier = Modifier,
) {
    var sheetOpen by remember { mutableStateOf(false) }
    val shared = controller.sharedValue(field)
    val sharedOptions = controller.sharedOptions(field)

    val valueText = when (shared) {
        null -> Strings.BATCH_VALUE_VARIES
        else -> ValueLabels.label(field, shared)
    }

    OptionRow(
        label = fieldLabel(field),
        value = valueText,
        modifier = modifier,
        trailing = if (loading) {
            { PendingIndicator() }
        } else {
            null
        },
        onClick = {
            controller.requestOptions(field)
            sheetOpen = true
        },
    )

    if (sheetOpen) {
        if (sharedOptions.isEmpty()) {
            // The intersection is empty: no value every file supports.
            // Honest message; per-file overrides remain available.
            SelectionSheet(
                title = fieldLabel(field),
                options = emptyList(),
                selectedId = shared,
                emptyMessage = Strings.BATCH_NO_COMMON_OPTIONS,
                onDismiss = { sheetOpen = false },
                onSelect = { },
            )
        } else {
            SelectionSheet(
                title = fieldLabel(field),
                options = sharedOptions.map { id ->
                    SelectionOption(
                        id = id,
                        label = ValueLabels.label(field, id),
                        detail = null,
                    )
                },
                selectedId = shared,
                onDismiss = { sheetOpen = false },
                onSelect = { selected ->
                    sheetOpen = false
                    controller.broadcast(FieldChange(field = field, valueId = selected.id))
                },
            )
        }
    }
}

/**
 * The shared compression preset row: the common goal preset broadcast to
 * every file (target size and custom bitrates are per-file concerns —
 * per-source budgeting is inherent when each session resolves its own
 * plan; the mandate applies target size independently per file).
 */
@Composable
private fun SharedCompressionRow(
    controller: BatchWorkflowController,
    modifier: Modifier = Modifier,
) {
    var sheetOpen by remember { mutableStateOf(false) }
    val shared = controller.sharedValue(Fields.COMPRESSION_PRESET)
    val presets = listOf("high-quality", "balanced", "smaller-file", "target-size", "custom")

    OptionRow(
        label = Strings.COMPRESSION_SECTION_TITLE,
        value = when (shared) {
            null -> Strings.BATCH_VALUE_VARIES
            else -> ValueLabels.label(Fields.COMPRESSION_PRESET, shared)
        },
        modifier = modifier,
        onClick = { sheetOpen = true },
    )

    if (sheetOpen) {
        SelectionSheet(
            title = Strings.COMPRESSION_SECTION_TITLE,
            options = presets.map { id ->
                SelectionOption(id = id, label = ValueLabels.label(Fields.COMPRESSION_PRESET, id), detail = null)
            },
            selectedId = shared,
            onDismiss = { sheetOpen = false },
            onSelect = { selected ->
                sheetOpen = false
                controller.broadcast(
                    FieldChange(
                        field = Fields.COMPRESSION_ENABLED,
                        valueId = "true",
                    )
                )
                controller.broadcast(
                    FieldChange(field = Fields.COMPRESSION_PRESET, valueId = selected.id)
                )
            },
        )
    }
}

/** Canonical field id -> display label (presentation only). */
private fun fieldLabel(field: String): String = when (field) {
    Fields.FORMAT -> Strings.FIELD_FORMAT
    Fields.CODEC -> Strings.FIELD_CODEC
    Fields.RESOLUTION -> Strings.FIELD_RESOLUTION
    Fields.FRAME_RATE -> Strings.FIELD_FRAME_RATE
    Fields.AUDIO_CODEC -> Strings.FIELD_CODEC
    Fields.AUDIO_SAMPLE_RATE -> Strings.FIELD_SAMPLE_RATE
    Fields.AUDIO_CHANNELS -> Strings.FIELD_CHANNELS
    Fields.IMAGE_FORMAT -> "Image format"
    Fields.SUBTITLE_TARGET -> "Output format"
    else -> field
}

/** The batch submit label mirrors the single-file action per operation. */
private fun batchSubmitLabel(operation: String): String = when (operation) {
    "extract-audio" -> Strings.EXTRACT_AUDIO_ACTION
    "extract-frames" -> Strings.EXTRACT_FRAMES_ACTION
    "extract-subtitles" -> Strings.EXTRACT_SUBTITLES_ACTION
    else -> Strings.BATCH_SUBMIT_ALL
}
