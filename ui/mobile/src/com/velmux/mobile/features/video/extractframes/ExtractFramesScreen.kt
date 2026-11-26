package com.velmux.mobile.features.video.extractframes

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.Section
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.features.workflow.OptionFieldRow
import com.velmux.mobile.features.workflow.WorkflowScreenHost
import com.velmux.mobile.features.workflow.editors.FrameSelectionEditor
import com.velmux.mobile.features.workflow.editors.LabeledInputRow
import com.velmux.mobile.features.workflow.outputNameOf
import com.velmux.mobile.features.workflow.payloadOf
import com.velmux.mobile.features.workflow.toImageDimensions
import com.velmux.mobile.navigation.Navigator
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/**
 * Extract Frames (mandate §29): the selection modes, the range, the
 * image output settings (JPEG/PNG/WebP, quality, dimensions) and the
 * naming stem.
 */
@Composable
fun ExtractFramesScreen(
    client: EngineClient,
    navigator: Navigator,
    sessionId: String?,
    modifier: Modifier = Modifier,
) {
    WorkflowScreenHost(
        operation = "extract-frames",
        client = client,
        navigator = navigator,
        sessionId = sessionId,
        submitLabel = Strings.EXTRACT_FRAMES_ACTION,
        modifier = modifier,
    ) { snapshot, controller ->
        Section(title = Strings.FRAMES_SELECTION_SECTION) {
            FrameSelectionEditor(
                snapshot = snapshot,
                onSelect = { controller.select(snapshot, it) },
            )
        }

        Section(title = Strings.IMAGE_OUTPUT_SECTION) {
            OptionFieldRow(
                label = Strings.IMAGE_FORMAT,
                field = Fields.IMAGE_FORMAT,
                snapshot = snapshot,
                controller = controller,
            )
            val quality = snapshot.valueFor(Fields.IMAGE_QUALITY)?.valueId
            if (snapshot.valueFor(Fields.IMAGE_FORMAT)?.valueId != "png") {
                LabeledInputRow(
                    label = Strings.IMAGE_QUALITY,
                    acceptedValue = quality?.takeIf { it != "default" } ?: "",
                    placeholder = "85 (1 – 100)",
                    onApply = { text ->
                        text.toIntOrNull()?.let { value ->
                            if (value in 1..100) {
                                controller.select(
                                    snapshot,
                                    FieldChange(
                                        field = Fields.IMAGE_QUALITY,
                                        valueId = value.toString(),
                                    ),
                                )
                            }
                        }
                    },
                    applyEnabled = { it.toIntOrNull() in 1..100 },
                )
            }
            ImageDimensionsEditor(snapshot = snapshot, controller = controller)
        }

        Column(modifier = Modifier.padding(horizontal = 16.dp)) {
            Section(title = Strings.OUTPUT_SECTION) {
                LabeledInputRow(
                    label = Strings.OUTPUT_STEM,
                    acceptedValue = snapshot.outputNameOf() ?: "",
                    placeholder = Strings.FRAMES_DEFAULT_STEM,
                    keyboardType = KeyboardType.Text,
                    onApply = { text ->
                        controller.select(
                            snapshot,
                            FieldChange(
                                field = Fields.OUTPUT_NAME,
                                valueId = "custom",
                                payload = com.velmux.mobile.features.workflow.outputNameJson(text),
                            ),
                        )
                    },
                )
            }
        }
    }
}

/** Custom image dimensions with aspect preservation (mandate §29.3). */
@Composable
private fun ImageDimensionsEditor(
    snapshot: com.velmux.mobile.engine.SessionSnapshot,
    controller: com.velmux.mobile.features.workflow.WorkflowController,
) {
    val dimensions = snapshot.payloadOf(Fields.IMAGE_DIMENSIONS).toImageDimensions()
    LabeledInputRow(
        label = Strings.IMAGE_MAX_WIDTH,
        acceptedValue = dimensions.first?.toString() ?: "",
        placeholder = Strings.IMAGE_DIMENSIONS_ORIGINAL,
        onApply = { text ->
            val width = text.toIntOrNull() ?: 0
            controller.select(
                snapshot,
                FieldChange(
                    field = Fields.IMAGE_DIMENSIONS,
                    valueId = "custom",
                    payload = buildJsonObject {
                        put("maxWidth", width)
                        put("maxHeight", dimensions.second ?: 0)
                    },
                ),
            )
        },
        applyEnabled = { it.isBlank() || (it.toIntOrNull() ?: 0) in 16..8192 },
    )
    LabeledInputRow(
        label = Strings.IMAGE_MAX_HEIGHT,
        acceptedValue = dimensions.second?.toString() ?: "",
        placeholder = Strings.IMAGE_DIMENSIONS_ORIGINAL,
        onApply = { text ->
            val height = text.toIntOrNull() ?: 0
            controller.select(
                snapshot,
                FieldChange(
                    field = Fields.IMAGE_DIMENSIONS,
                    valueId = "custom",
                    payload = buildJsonObject {
                        put("maxWidth", dimensions.first ?: 0)
                        put("maxHeight", height)
                    },
                ),
            )
        },
        applyEnabled = { it.isBlank() || (it.toIntOrNull() ?: 0) in 16..8192 },
    )
}
