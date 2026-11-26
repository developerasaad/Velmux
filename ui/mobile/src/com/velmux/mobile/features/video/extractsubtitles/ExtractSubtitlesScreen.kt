package com.velmux.mobile.features.video.extractsubtitles

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.Section
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.features.workflow.WorkflowScreenHost
import com.velmux.mobile.features.workflow.editors.ChoiceRow
import com.velmux.mobile.features.workflow.editors.SubtitleTrackSelector
import com.velmux.mobile.features.workflow.editors.trackIndicesChange
import com.velmux.mobile.features.workflow.selectedSubtitleTrackIndices
import com.velmux.mobile.features.workflow.editors.LabeledInputRow
import com.velmux.mobile.features.workflow.outputNameOf
import com.velmux.mobile.navigation.Navigator
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray

/**
 * Extract Subtitles (mandate §30): the compact workflow — track
 * selection over the inspected subtitle streams and the standalone
 * output representation per track (Original, SRT, WebVTT, ASS).
 */
@Composable
fun ExtractSubtitlesScreen(
    client: EngineClient,
    navigator: Navigator,
    sessionId: String?,
    modifier: Modifier = Modifier,
) {
    WorkflowScreenHost(
        operation = "extract-subtitles",
        client = client,
        navigator = navigator,
        sessionId = sessionId,
        submitLabel = Strings.EXTRACT_SUBTITLES_ACTION,
        modifier = modifier,
    ) { snapshot, controller ->
        val media = snapshot.sources.firstOrNull()

        Section(title = Strings.EXTRACT_TRACKS_SECTION) {
            SubtitleTrackSelector(
                tracks = media?.subtitles ?: emptyList(),
                selected = snapshot.selectedSubtitleTrackIndices(),
                onSelectionChange = { selection ->
                    controller.select(
                        snapshot,
                        trackIndicesChange(Fields.SUBTITLE_TRACKS, selection),
                    )
                },
            )
        }

        Section(title = Strings.SUBTITLE_TARGET_SECTION) {
            listOf(
                "original" to Strings.SUBTITLE_TARGET_ORIGINAL,
                "srt" to Strings.SUBTITLE_TARGET_SRT,
                "vtt" to Strings.SUBTITLE_TARGET_VTT,
                "ass" to Strings.SUBTITLE_TARGET_ASS,
            ).forEach { (targetId, label) ->
                ChoiceRow(
                    label = label,
                    selected = subtitleTarget(snapshot) == targetId,
                    onClick = {
                        val count = (media?.subtitles?.size ?: 0)
                            .coerceAtLeast(1)
                        controller.select(
                            snapshot,
                            FieldChange(
                                field = Fields.SUBTITLE_TARGET,
                                valueId = "custom",
                                payload = buildJsonArray {
                                    repeat(count) { add(JsonPrimitive(targetId)) }
                                },
                            ),
                        )
                    },
                )
            }
        }

        Section(title = Strings.OUTPUT_SECTION) {
            LabeledInputRow(
                label = Strings.OUTPUT_STEM,
                acceptedValue = snapshot.outputNameOf() ?: "",
                placeholder = Strings.SUBTITLES_DEFAULT_STEM,
                keyboardType = androidx.compose.ui.text.input.KeyboardType.Text,
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

/** The accepted uniform target representation (v1: one target for all). */
private fun subtitleTarget(snapshot: com.velmux.mobile.engine.SessionSnapshot): String {
    val valueId = snapshot.valueFor(Fields.SUBTITLE_TARGET)?.valueId ?: "original"
    if (valueId != "custom") {
        return "original"
    }
    val payload = snapshot.valueFor(Fields.SUBTITLE_TARGET)?.payload
        as? kotlinx.serialization.json.JsonArray
        ?: return "original"
    val first = payload.firstOrNull() as? JsonPrimitive ?: return "original"
    return first.content
}
