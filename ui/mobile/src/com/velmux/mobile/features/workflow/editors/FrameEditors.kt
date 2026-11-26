package com.velmux.mobile.features.workflow.editors

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.engine.SessionSnapshot
import com.velmux.mobile.features.workflow.FrameSelectionUi
import com.velmux.mobile.features.workflow.payloadOf
import com.velmux.mobile.features.workflow.payloadOf
import com.velmux.mobile.features.workflow.toJson
import com.velmux.mobile.features.workflow.toFrameSelection

/**
 * The frame-selection editor (mandate §29.2): the eight approved modes
 * with their mode-specific inputs, all routed through frames.selection.
 */
@Composable
fun FrameSelectionEditor(
    snapshot: SessionSnapshot,
    onSelect: (FieldChange) -> Unit,
    modifier: Modifier = Modifier,
) {
    val current = snapshot.payloadOf(Fields.FRAME_SELECTION).toFrameSelection()
    val apply = { next: FrameSelectionUi ->
        onSelect(
            FieldChange(
                field = Fields.FRAME_SELECTION,
                valueId = next.mode.wire,
                payload = next.toJson(),
            ),
        )
    }

    Column(modifier = modifier.fillMaxWidth()) {
        Text(
            text = Strings.FRAMES_MODE,
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
        )
    }
    com.velmux.mobile.features.workflow.FrameModeUi.entries.forEach { mode ->
        ChoiceRow(
            label = mode.label,
            selected = current.mode == mode,
            onClick = { apply(current.copy(mode = mode)) },
            modifier = modifier,
        )
    }

    when (current.mode) {
        com.velmux.mobile.features.workflow.FrameModeUi.INTERVAL -> {
            LabeledInputRow(
                label = Strings.FRAMES_INTERVAL,
                acceptedValue = if (current.intervalUs > 0) {
                    com.velmux.mobile.features.workflow.formatUs(current.intervalUs)
                } else {
                    ""
                },
                placeholder = "0:01.000",
                keyboardType = KeyboardType.Ascii,
                onApply = { text ->
                    com.velmux.mobile.features.workflow.parseSecondsUs(text)
                        ?.let { us -> apply(current.copy(intervalUs = us)) }
                },
                applyEnabled = {
                    (com.velmux.mobile.features.workflow.parseSecondsUs(it) ?: 0) > 0
                },
                modifier = modifier,
            )
        }
        com.velmux.mobile.features.workflow.FrameModeUi.FRAME_RATE -> {
            FrameRateChoice(
                acceptedWire = current.frameRateWire,
                onPick = { wire -> apply(current.copy(frameRateWire = wire)) },
                modifier = modifier,
            )
        }
        com.velmux.mobile.features.workflow.FrameModeUi.COUNT -> {
            LabeledInputRow(
                label = Strings.FRAMES_COUNT,
                acceptedValue = if (current.count > 0) {
                    current.count.toString()
                } else {
                    ""
                },
                placeholder = "12",
                onApply = { text ->
                    text.toIntOrNull()?.let { count ->
                        apply(current.copy(count = count))
                    }
                },
                applyEnabled = { (it.toIntOrNull() ?: 0) in 1..100_000 },
                modifier = modifier,
            )
        }
        else -> {}
    }

    EditorCaption(text = Strings.FRAMES_RANGE)
    TimeRangeEditor(
        range = current.range,
        onApply = { range -> apply(current.copy(range = range)) },
        modifier = modifier,
    )
}

/** Common sampling frame rates for frame-rate mode ("24/1" wires). */
@Composable
private fun FrameRateChoice(
    acceptedWire: String?,
    onPick: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val rates = listOf("1/1", "5/1", "10/1", "12/1", "24/1", "25/1", "30/1", "60/1")
    rates.forEach { wire ->
        val label = if (wire == "1/1") {
            "1 fps"
        } else {
            "${wire.substringBefore('/')} fps"
        }
        ChoiceRow(
            label = label,
            selected = acceptedWire == wire,
            onClick = { onPick(wire) },
            modifier = modifier,
        )
    }
}
