package com.velmux.mobile.features.audio.converter

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.ExpandableSection
import com.velmux.mobile.components.Section
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.features.workflow.OptionFieldRow
import com.velmux.mobile.features.workflow.WorkflowScreenHost
import com.velmux.mobile.features.workflow.editors.AudioAdjustmentsSection
import com.velmux.mobile.features.workflow.editors.CompressionSection
import com.velmux.mobile.features.workflow.editors.MetadataSection
import com.velmux.mobile.navigation.Navigator

/**
 * Audio Converter (mandate §31 — the FIXED layout): Format, Codec,
 * Sample rate, Channels visible; Reduce file size, Adjustments, Metadata
 * collapsed by default; estimate and Convert.
 */
@Composable
fun AudioConverterScreen(
    client: EngineClient,
    navigator: Navigator,
    sessionId: String?,
    modifier: Modifier = Modifier,
) {
    WorkflowScreenHost(
        operation = "audio-convert",
        client = client,
        navigator = navigator,
        sessionId = sessionId,
        submitLabel = Strings.CONVERT_ACTION,
        modifier = modifier,
    ) { snapshot, controller ->
        Section(title = Strings.AUDIO_PRIMARY_SECTION) {
            OptionFieldRow(
                label = Strings.FIELD_FORMAT,
                field = Fields.FORMAT,
                snapshot = snapshot,
                controller = controller,
            )
            OptionFieldRow(
                label = Strings.FIELD_CODEC,
                field = Fields.CODEC,
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
            ExpandableSection(title = Strings.ADJUSTMENTS_SECTION) {
                AudioAdjustmentsSection(
                    snapshot = snapshot,
                    onSelect = { controller.select(snapshot, it) },
                )
            }
            ExpandableSection(title = Strings.METADATA_SECTION) {
                MetadataSection(
                    snapshot = snapshot,
                    fields = listOf(
                        "title", "artist", "album", "genre", "date",
                    ),
                    onSelect = { controller.select(snapshot, it) },
                )
            }
        }
    }
}
