package com.velmux.mobile.features.workflow.editors

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
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
import com.velmux.mobile.features.workflow.SplitModeUi
import com.velmux.mobile.features.workflow.SplitRulesUi
import com.velmux.mobile.features.workflow.formatUs
import com.velmux.mobile.features.workflow.parseSecondsUs
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/**
 * The split rules editor (mandate §27.3): the mode selector plus the
 * mode-specific parameter inputs. Bound to the accepted split.rules
 * payload; the core validates ranges (part count 2..100, positive
 * durations...).
 */
@Composable
fun SplitRulesEditor(
    rules: SplitRulesUi,
    onApply: (SplitRulesUi) -> Unit,
    modifier: Modifier = Modifier,
) {
    SplitModeUi.entries.forEach { mode ->
        Row(
            modifier = modifier
                .fillMaxWidth()
                .heightIn(min = 44.dp)
                .actionRow { onApply(rules.copy(mode = mode)) },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            RadioButton(
                selected = rules.mode == mode,
                onClick = null,
            )
            Spacer(Modifier.width(8.dp))
            Text(
                text = mode.label,
                style = MaterialTheme.typography.bodyLarge,
            )
        }
    }
    when (rules.mode) {
        SplitModeUi.EQUAL_PARTS -> {
            LabeledInputRow(
                label = Strings.SPLIT_PART_COUNT,
                acceptedValue = rules.partCount.toString(),
                placeholder = "2",
                onApply = { text ->
                    text.toIntOrNull()?.let { count ->
                        onApply(rules.copy(partCount = count))
                    }
                },
                applyEnabled = { (it.toIntOrNull() ?: 0) in 2..100 },
            )
        }
        SplitModeUi.DURATION -> {
            LabeledInputRow(
                label = Strings.SPLIT_PART_DURATION,
                acceptedValue = if (rules.partDurationUs > 0) {
                    formatUs(rules.partDurationUs)
                } else {
                    ""
                },
                placeholder = "0:30.000",
                keyboardType = KeyboardType.Ascii,
                onApply = { text ->
                    parseSecondsUs(text)?.let { us ->
                        onApply(rules.copy(partDurationUs = us))
                    }
                },
                applyEnabled = { (parseSecondsUs(it) ?: 0) > 0 },
            )
        }
        SplitModeUi.MAX_SIZE -> {
            LabeledInputRow(
                label = Strings.SPLIT_MAX_SIZE,
                acceptedValue = if (rules.maxSizeBytes > 0) {
                    formatBytesLabel(rules.maxSizeBytes)
                } else {
                    ""
                },
                placeholder = "100 MB",
                keyboardType = KeyboardType.Ascii,
                onApply = { text ->
                    com.velmux.mobile.features.workflow.parseDecimal(text)?.let { bytes ->
                        onApply(rules.copy(maxSizeBytes = bytes))
                    }
                },
                applyEnabled = {
                    (com.velmux.mobile.features.workflow.parseDecimal(it) ?: 0) >= 1024
                },
            )
        }
        SplitModeUi.CUSTOM -> {
            rules.boundariesUs.forEachIndexed { index, boundary ->
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = 44.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Text(
                        text = formatUs(boundary),
                        style = MaterialTheme.typography.bodyMedium,
                        modifier = Modifier.weight(1f),
                    )
                    IconButton(
                        onClick = {
                            onApply(
                                rules.copy(
                                    boundariesUs = rules.boundariesUs
                                        .filterIndexed { i, _ -> i != index },
                                ),
                            )
                        },
                    ) {
                        Icon(
                            imageVector = VelmuxIcons.Delete,
                            contentDescription = Strings.RANGE_REMOVE,
                        )
                    }
                }
            }
            AddBoundaryRow { boundary ->
                onApply(rules.copy(boundariesUs = rules.boundariesUs + boundary))
            }
        }
        SplitModeUi.CHAPTERS, SplitModeUi.SCENES -> {
            // Boundary-driven modes resolved by the core from actual media
            // analysis (chapters) or real scene detection when requested.
        }
    }
}

@Composable
private fun AddBoundaryRow(onAdd: (Long) -> Unit) {
    var text by rememberSaveable { mutableStateOf("") }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        OutlinedTextField(
            value = text,
            onValueChange = { text = it },
            label = { Text(text = Strings.SPLIT_BOUNDARY) },
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii),
            modifier = Modifier.weight(1f),
        )
        FilledTonalButton(
            onClick = {
                parseSecondsUs(text)?.let { us ->
                    onAdd(us)
                    text = ""
                }
            },
            enabled = (parseSecondsUs(text) ?: 0) > 0,
            modifier = Modifier.heightIn(min = 44.dp),
        ) {
            Text(text = Strings.RANGE_ADD)
        }
    }
}

/**
 * The merge rules (mandate §27.4/§27.5): layout (sequential vs the two
 * spatial modes), audio source and the spatial duration policy.
 */
@Composable
fun MergeRulesEditor(
    layout: String,
    audioSelection: String,
    longerDuration: Boolean,
    isVideo: Boolean,
    onSelect: (FieldChange) -> Unit,
    onLongerDuration: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
) {
    if (isVideo) {
        EditorCaption(text = Strings.MERGE_LAYOUT)
        listOf(
            "sequential" to Strings.MERGE_LAYOUT_SEQUENTIAL,
            "side-by-side" to Strings.MERGE_LAYOUT_SIDE_BY_SIDE,
            "top-bottom" to Strings.MERGE_LAYOUT_TOP_BOTTOM,
        ).forEach { (id, label) ->
            ChoiceRow(
                label = label,
                selected = layout == id,
                onClick = {
                    onSelect(FieldChange(field = Fields.MERGE_LAYOUT, valueId = id))
                },
                modifier = modifier,
            )
        }
    }
    EditorCaption(text = Strings.MERGE_AUDIO)
    listOf(
        "first" to Strings.MERGE_AUDIO_FIRST,
        "second" to Strings.MERGE_AUDIO_SECOND,
        "both" to Strings.MERGE_AUDIO_BOTH,
        "none" to Strings.MERGE_AUDIO_NONE,
    ).forEach { (id, label) ->
        ChoiceRow(
            label = label,
            selected = audioSelection == id,
            onClick = {
                onSelect(FieldChange(field = Fields.MERGE_AUDIO, valueId = id))
            },
            modifier = modifier,
        )
    }
    if (isVideo && layout != "sequential") {
        SwitchRow(
            label = Strings.MERGE_LONGER_DURATION,
            checked = longerDuration,
            supportingText = Strings.MERGE_LONGER_DURATION_HINT,
            onCheckedChange = onLongerDuration,
            modifier = modifier,
        )
    }
}

/** A plain radio choice row bound to a field value. */
@Composable
fun ChoiceRow(
    label: String,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .heightIn(min = 44.dp)
            .actionRow { onClick() },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = null)
        Spacer(Modifier.width(8.dp))
        Text(
            text = label,
            style = MaterialTheme.typography.bodyLarge,
            color = if (selected) {
                MaterialTheme.colorScheme.primary
            } else {
                MaterialTheme.colorScheme.onSurface
            },
        )
    }
}

/** Sends a merge.audio update carrying the duration policy payload. */
fun mergeAudioChange(valueId: String, longerDuration: Boolean): FieldChange =
    FieldChange(
        field = Fields.MERGE_AUDIO,
        valueId = valueId,
        payload = buildJsonObject {
            put("longerDuration", longerDuration)
        },
    )
