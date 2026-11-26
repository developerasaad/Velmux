package com.velmux.mobile.features.video.crop

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.Section
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.features.workflow.WorkflowScreenHost
import com.velmux.mobile.features.workflow.outputNameOf
import com.velmux.mobile.features.workflow.payloadOf
import com.velmux.mobile.features.workflow.toCropRect
import com.velmux.mobile.features.workflow.toJson
import com.velmux.mobile.features.workflow.editors.CropRectEditor
import com.velmux.mobile.features.workflow.editors.LabeledInputRow
import com.velmux.mobile.navigation.Navigator

/**
 * Crop Video (mandate §26): the photo-style cropping workflow. The crop
 * rectangle editor works in source display coordinates with the approved
 * ratio presets; the output name is the only additional control — no
 * format/quality/technical selectors.
 */
@Composable
fun CropScreen(
    client: EngineClient,
    navigator: Navigator,
    sessionId: String?,
    modifier: Modifier = Modifier,
) {
    WorkflowScreenHost(
        operation = "video-crop",
        client = client,
        navigator = navigator,
        sessionId = sessionId,
        submitLabel = Strings.CROP_ACTION,
        modifier = modifier,
    ) { snapshot, controller ->
        val media = snapshot.sources.firstOrNull()
        val video = media?.primaryVideo
        val sourceWidth = video?.width ?: 0
        val sourceHeight = video?.height ?: 0
        val cropRect = snapshot.crop

        Section(title = Strings.CROP_SECTION) {
            CropRectEditor(
                crop = cropRect,
                sourceWidth = sourceWidth,
                sourceHeight = sourceHeight,
                onApply = { next ->
                    controller.select(
                        snapshot,
                        com.velmux.mobile.engine.FieldChange(
                            field = Fields.CROP_RECT,
                            valueId = "custom",
                            payload = next.toJson(),
                        ),
                    )
                },
            )
        }

        Section(title = Strings.OUTPUT_SECTION) {
            LabeledInputRow(
                label = Strings.OUTPUT_NAME,
                acceptedValue = snapshot.outputNameOf() ?: "",
                placeholder = Strings.CROP_DEFAULT_NAME,
                keyboardType = androidx.compose.ui.text.input.KeyboardType.Text,
                onApply = { text ->
                    controller.select(
                        snapshot,
                        com.velmux.mobile.engine.FieldChange(
                            field = Fields.OUTPUT_NAME,
                            valueId = "custom",
                            payload = com.velmux.mobile.features.workflow.outputNameJson(text),
                        ),
                    )
                },
                modifier = Modifier.padding(bottom = 8.dp),
            )
        }
    }
}

/** The accepted crop rectangle (display coordinates). */
private val com.velmux.mobile.engine.SessionSnapshot.crop:
    com.velmux.mobile.features.workflow.CropRect
    get() = payloadOf(Fields.CROP_RECT).toCropRect()
