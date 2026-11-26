package com.velmux.mobile.features.video.extractaudio

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.ExpandableSection
import com.velmux.mobile.components.Section
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.features.workflow.OptionFieldRow
import com.velmux.mobile.features.workflow.WorkflowScreenHost
import com.velmux.mobile.features.workflow.editors.AudioTrackSelector
import com.velmux.mobile.features.workflow.editors.CompressionSection
import com.velmux.mobile.features.workflow.editors.MetadataSection
import com.velmux.mobile.features.workflow.editors.TimeRangeEditor
import com.velmux.mobile.features.workflow.editors.trackIndicesChange
import com.velmux.mobile.features.workflow.outputNameOf
import com.velmux.mobile.features.workflow.payloadOf
import com.velmux.mobile.features.workflow.selectedAudioTrackIndices
import com.velmux.mobile.features.workflow.toJson
import com.velmux.mobile.features.workflow.toUsRange
import com.velmux.mobile.navigation.Navigator

/**
 * Extract Audio (mandate §28): track selection over the inspected audio
 * streams, the shared Audio Converter configuration (format, codec,
 * sample rate, channels), optional compression, optional range and
 * metadata.
 */
@Composable
fun ExtractAudioScreen(
    client: EngineClient,
    navigator: Navigator,
    sessionId: String?,
    modifier: Modifier = Modifier,
) {
    WorkflowScreenHost(
        operation = "extract-audio",
        client = client,
        navigator = navigator,
        sessionId = sessionId,
        submitLabel = Strings.EXTRACT_AUDIO_ACTION,
        modifier = modifier,
    ) { snapshot, controller ->
        val media = snapshot.sources.firstOrNull()

        Section(title = Strings.EXTRACT_TRACKS_SECTION) {
            AudioTrackSelector(
                tracks = media?.audio ?: emptyList(),
                selected = snapshot.selectedAudioTrackIndices(),
                onSelectionChange = { selection ->
                    controller.select(
                        snapshot,
                        trackIndicesChange(
                            Fields.EXTRACT_AUDIO_TRACKS,
                            selection,
                        ),
                    )
                },
            )
        }

        Section(title = Strings.AUDIO_PRIMARY_SECTION) {
            OptionFieldRow(
                label = Strings.FIELD_FORMAT,
                field = Fields.FORMAT,
                snapshot = snapshot,
                controller = controller,
            )
            OptionFieldRow(
                label = Strings.FIELD_CODEC,
                field = Fields.AUDIO_CODEC,
                snapshot = snapshot,
                controller = controller,
            )
            OptionFieldRow(
                label = Strings.FIELD_SAMPLE_RATE,
                field = Fields.AUDIO_SAMPLE_RATE,
                snapshot = snapshot,
                controller = controller,
            )
            OptionFieldRow(
                label = Strings.FIELD_CHANNELS,
                field = Fields.AUDIO_CHANNELS,
                snapshot = snapshot,
                controller = controller,
            )
        }

        Column(modifier = Modifier.padding(horizontal = 16.dp)) {
            ExpandableSection(title = Strings.COMPRESSION_SECTION_TITLE) {
                CompressionSection(
                    snapshot = snapshot,
                    onSelect = { controller.select(snapshot, it) },
                )
            }
            ExpandableSection(title = Strings.RANGE_SECTION) {
                TimeRangeEditor(
                    range = snapshot.payloadOf(Fields.EXTRACT_AUDIO_RANGE).toUsRange(),
                    onApply = { range ->
                        controller.select(
                            snapshot,
                            FieldChange(
                                field = Fields.EXTRACT_AUDIO_RANGE,
                                valueId = "custom",
                                payload = range.toJson(),
                            ),
                        )
                    },
                )
            }
            ExpandableSection(title = Strings.METADATA_SECTION) {
                MetadataSection(
                    snapshot = snapshot,
                    fields = listOf("title", "artist", "album", "genre", "date"),
                    onSelect = { controller.select(snapshot, it) },
                )
            }
        }

        Section(title = Strings.OUTPUT_SECTION) {
            com.velmux.mobile.features.workflow.editors.LabeledInputRow(
                label = Strings.OUTPUT_NAME,
                acceptedValue = snapshot.outputNameOf() ?: "",
                placeholder = Strings.EXTRACTED_AUDIO_DEFAULT_NAME,
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
