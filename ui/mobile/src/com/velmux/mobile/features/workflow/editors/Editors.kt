package com.velmux.mobile.features.workflow.editors

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.actionRow
import com.velmux.mobile.design.VelmuxIcons
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.engine.SessionSnapshot
import com.velmux.mobile.features.workflow.UsRange
import com.velmux.mobile.features.workflow.formatUs
import com.velmux.mobile.features.workflow.parseDecimal
import com.velmux.mobile.features.workflow.parseSecondsUs

/** Formats decimal bytes for display (KB = 1000 bytes, mandate §21). */
internal fun formatBytesLabel(bytes: Long): String = when {
    bytes < 0 -> Strings.NO_VALUE
    bytes < 1_000 -> "$bytes B"
    bytes < 1_000_000 -> "%.1f KB".format(bytes / 1_000.0)
    bytes < 1_000_000_000 -> "%.1f MB".format(bytes / 1_000_000.0)
    else -> "%.2f GB".format(bytes / 1_000_000_000.0)
}

/**
 * The compression section ("Reduce file size", mandate §20): disabled by
 * default; the enabled sheet offers the approved presets; Target size and
 * Custom expose their numeric inputs only when applicable. Every control
 * routes through the session fields — no client-side policy.
 */
@Composable
fun CompressionSection(
    snapshot: SessionSnapshot,
    onSelect: (FieldChange) -> Unit,
    modifier: Modifier = Modifier,
) {
    val enabled = snapshot.valueFor(Fields.COMPRESSION_ENABLED)?.valueId == "true"
    val preset = snapshot.valueFor(Fields.COMPRESSION_PRESET)?.valueId ?: "balanced"

    SwitchRow(
        label = Strings.COMPRESSION_TITLE,
        checked = enabled,
        supportingText = if (enabled) null else Strings.COMPRESSION_OFF_HINT,
        onCheckedChange = { checked ->
            onSelect(
                FieldChange(
                    field = Fields.COMPRESSION_ENABLED,
                    valueId = if (checked) "true" else "false",
                ),
            )
        },
        modifier = modifier,
    )

    if (enabled) {
        PresetRow(
            accepted = preset,
            onSelect = onSelect,
        )
        when (preset) {
            "target-size" -> {
                val acceptedBytes =
                    snapshot.valueFor(Fields.COMPRESSION_TARGET_SIZE)?.valueId
                LabeledInputRow(
                    label = Strings.COMPRESSION_TARGET_SIZE,
                    acceptedValue = acceptedBytes
                        ?.toLongOrNull()
                        ?.takeIf { it > 0 }
                        ?.let { formatBytesLabel(it) } ?: "",
                    placeholder = "25 MB",
                    keyboardType = KeyboardType.Ascii,
                    onApply = { text ->
                        parseDecimal(text)?.let { bytes ->
                            onSelect(
                                FieldChange(
                                    field = Fields.COMPRESSION_TARGET_SIZE,
                                    valueId = bytes.toString(),
                                ),
                            )
                        }
                    },
                    applyEnabled = { parseDecimal(it) != null },
                )
            }
            "custom" -> {
                val videoBitrate =
                    snapshot.valueFor(Fields.COMPRESSION_VIDEO_BITRATE)?.valueId
                val audioBitrate =
                    snapshot.valueFor(Fields.COMPRESSION_AUDIO_BITRATE)?.valueId
                val quality =
                    snapshot.valueFor(Fields.COMPRESSION_QUALITY)?.valueId
                LabeledInputRow(
                    label = Strings.COMPRESSION_VIDEO_BITRATE,
                    acceptedValue = videoBitrate
                        ?.toLongOrNull()
                        ?.takeIf { it > 0 }
                        ?.let { "${formatBytesLabel(it)}bps" } ?: "",
                    placeholder = "2.5 Mbps",
                    keyboardType = KeyboardType.Ascii,
                    onApply = { text ->
                        parseDecimal(text)?.let { bits ->
                            onSelect(
                                FieldChange(
                                    field = Fields.COMPRESSION_VIDEO_BITRATE,
                                    valueId = bits.toString(),
                                ),
                            )
                        }
                    },
                    applyEnabled = { parseDecimal(it) != null },
                )
                LabeledInputRow(
                    label = Strings.COMPRESSION_AUDIO_BITRATE,
                    acceptedValue = audioBitrate
                        ?.toLongOrNull()
                        ?.takeIf { it > 0 }
                        ?.let { "${formatBytesLabel(it)}bps" } ?: "",
                    placeholder = "128 kbps",
                    keyboardType = KeyboardType.Ascii,
                    onApply = { text ->
                        parseDecimal(text)?.let { bits ->
                            onSelect(
                                FieldChange(
                                    field = Fields.COMPRESSION_AUDIO_BITRATE,
                                    valueId = bits.toString(),
                                ),
                            )
                        }
                    },
                    applyEnabled = { parseDecimal(it) != null },
                )
                LabeledInputRow(
                    label = Strings.COMPRESSION_QUALITY,
                    acceptedValue = quality?.takeIf { it != "auto" && it.toLongOrNull() != null } ?: "",
                    placeholder = "60",
                    onApply = { text ->
                        text.toIntOrNull()?.let { value ->
                            if (value in 1..100) {
                                onSelect(
                                    FieldChange(
                                        field = Fields.COMPRESSION_QUALITY,
                                        valueId = value.toString(),
                                    ),
                                )
                            }
                        }
                    },
                    applyEnabled = { it.toIntOrNull() in 1..100 },
                )
            }
        }
    }
}

/** The preset selection row (the five approved presets, mandate §20.2). */
@Composable
private fun PresetRow(
    accepted: String,
    onSelect: (FieldChange) -> Unit,
) {
    var open by rememberSaveable { mutableStateOf(false) }
    val presets = listOf(
        "high-quality" to "High quality",
        "balanced" to "Balanced quality",
        "smaller-file" to "Smaller file",
        "target-size" to "Target file size",
        "custom" to "Custom",
    )
    val label = presets.firstOrNull { it.first == accepted }?.second ?: accepted
    PresetRowSurface(
        label = label,
        open = open,
        onToggle = { open = !open },
    ) {
        presets.forEach { (id, name) ->
            PresetChoiceRow(
                name = name,
                selected = id == accepted,
                onClick = {
                    open = false
                    onSelect(FieldChange(field = Fields.COMPRESSION_PRESET, valueId = id))
                },
            )
        }
    }
}

@Composable
private fun PresetRowSurface(
    label: String,
    open: Boolean,
    onToggle: () -> Unit,
    content: @Composable () -> Unit,
) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = 44.dp)
                .actionRow { onToggle() },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = Strings.COMPRESSION_PRESET,
                style = MaterialTheme.typography.bodyLarge,
                modifier = Modifier.weight(1f),
            )
            Text(
                text = label,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (open) {
            content()
        }
    }
}

@Composable
private fun PresetChoiceRow(
    name: String,
    selected: Boolean,
    onClick: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 44.dp)
            .padding(start = 16.dp)
            .actionRow { onClick() },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = name,
            style = MaterialTheme.typography.bodyMedium,
            color = if (selected) {
                MaterialTheme.colorScheme.primary
            } else {
                MaterialTheme.colorScheme.onSurface
            },
            modifier = Modifier.weight(1f),
        )
        if (selected) {
            Icon(
                imageVector = com.velmux.mobile.design.VelmuxIcons.Check,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
            )
        }
    }
}

/**
 * The trim range editor (mandate §27.1): start/end inputs in seconds
 * with the accepted range visible; end may stay open (to end of stream).
 */
@Composable
fun TimeRangeEditor(
    range: UsRange,
    onApply: (UsRange) -> Unit,
    modifier: Modifier = Modifier,
) {
    LabeledInputRow(
        label = Strings.RANGE_START,
        acceptedValue = formatUs(range.startUs),
        placeholder = "0:00.000",
        keyboardType = KeyboardType.Ascii,
        onApply = { text ->
            parseSecondsUs(text)?.let { us -> onApply(range.copy(startUs = us)) }
        },
        applyEnabled = { parseSecondsUs(it) != null },
        modifier = modifier,
    )
    LabeledInputRow(
        label = Strings.RANGE_END,
        acceptedValue = if (range.endUs < 0) {
            Strings.RANGE_OPEN_END
        } else {
            formatUs(range.endUs)
        },
        placeholder = Strings.RANGE_OPEN_END,
        keyboardType = KeyboardType.Ascii,
        onApply = { text ->
            if (text.isBlank()) {
                onApply(range.copy(endUs = -1))
            } else {
                parseSecondsUs(text)?.let { us -> onApply(range.copy(endUs = us)) }
            }
        },
        applyEnabled = { it.isBlank() || parseSecondsUs(it) != null },
        modifier = modifier,
    )
}

/**
 * The cut editor's removed-interval list (mandate §27.2): each row shows
 * the accepted interval with a remove action; the add row appends a
 * candidate interval.
 */
@Composable
fun RemovedRangesEditor(
    ranges: List<UsRange>,
    onApply: (List<UsRange>) -> Unit,
    modifier: Modifier = Modifier,
) {
    ranges.forEachIndexed { index, range ->
        Row(
            modifier = modifier
                .fillMaxWidth()
                .heightIn(min = 44.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(
                text = formatUs(range.startUs) + " – " +
                    (if (range.endUs < 0) Strings.RANGE_OPEN_END else formatUs(range.endUs)),
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.weight(1f),
            )
            IconButton(
                onClick = { onApply(ranges.filterIndexed { i, _ -> i != index }) },
            ) {
                Icon(
                    imageVector = VelmuxIcons.Delete,
                    contentDescription = Strings.RANGE_REMOVE,
                )
            }
        }
    }
    AddRangeRow(onAdd = { candidate -> onApply(ranges + candidate) })
}

/** One add-interval input row for [RemovedRangesEditor]. */
@Composable
private fun AddRangeRow(onAdd: (UsRange) -> Unit) {
    var start by rememberSaveable { mutableStateOf("") }
    var end by rememberSaveable { mutableStateOf("") }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        OutlinedTextField(
            value = start,
            onValueChange = { start = it },
            label = { Text(text = Strings.RANGE_START) },
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii),
            modifier = Modifier.weight(1f),
        )
        OutlinedTextField(
            value = end,
            onValueChange = { end = it },
            label = { Text(text = Strings.RANGE_END) },
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii),
            modifier = Modifier.weight(1f),
        )
        FilledTonalButton(
            onClick = {
                val startUs = parseSecondsUs(start)
                val endUs = if (end.isBlank()) -1L else parseSecondsUs(end)
                if (startUs != null && (endUs == null || endUs < 0 || endUs > startUs)) {
                    onAdd(UsRange(startUs, endUs ?: -1L))
                    start = ""
                    end = ""
                }
            },
            enabled = parseSecondsUs(start) != null &&
                (end.isBlank() || parseSecondsUs(end) != null),
            modifier = Modifier.heightIn(min = 44.dp),
        ) {
            Text(text = Strings.RANGE_ADD)
        }
    }
}
