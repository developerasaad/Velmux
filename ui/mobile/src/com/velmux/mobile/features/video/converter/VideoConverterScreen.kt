package com.velmux.mobile.features.video.converter

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.ExpandableSection
import com.velmux.mobile.components.Section
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.engine.SessionSnapshot
import com.velmux.mobile.features.workflow.OptionFieldRow
import com.velmux.mobile.features.workflow.WorkflowController
import com.velmux.mobile.features.workflow.WorkflowScreenHost
import com.velmux.mobile.features.workflow.editors.CheckRow
import com.velmux.mobile.features.workflow.editors.ChoiceRow
import com.velmux.mobile.features.workflow.editors.CompressionSection
import com.velmux.mobile.features.workflow.editors.LabeledInputRow
import com.velmux.mobile.features.workflow.editors.MetadataSection
import com.velmux.mobile.features.workflow.editors.VideoAdjustmentsSection
import com.velmux.mobile.navigation.Navigator
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/**
 * Video Converter (mandate §19 — the FIXED layout): the four primary
 * selectors visible (Format, Codec, Resolution, Frame rate), the
 * secondary sections collapsed by default (Reduce file size, Audio
 * tracks, Subtitles, Adjustments, Metadata), estimate and Convert.
 */
@Composable
fun VideoConverterScreen(
    client: EngineClient,
    navigator: Navigator,
    sessionId: String?,
    modifier: Modifier = Modifier,
) {
    WorkflowScreenHost(
        operation = "video-convert",
        client = client,
        navigator = navigator,
        sessionId = sessionId,
        submitLabel = Strings.CONVERT_ACTION,
        modifier = modifier,
    ) { snapshot, controller ->
        Section(title = Strings.VIDEO_PRIMARY_SECTION) {
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
                label = Strings.FIELD_RESOLUTION,
                field = Fields.RESOLUTION,
                snapshot = snapshot,
                controller = controller,
            )
            CustomResolutionInputs(snapshot = snapshot, controller = controller)
            OptionFieldRow(
                label = Strings.FIELD_FRAME_RATE,
                field = Fields.FRAME_RATE,
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
            ExpandableSection(title = Strings.AUDIO_TRACKS_SECTION) {
                AudioTrackRulesEditor(snapshot = snapshot, controller = controller)
            }
            ExpandableSection(title = Strings.SUBTITLES_SECTION) {
                SubtitleRulesEditor(snapshot = snapshot, controller = controller)
            }
            ExpandableSection(title = Strings.ADJUSTMENTS_SECTION) {
                VideoAdjustmentsSection(
                    snapshot = snapshot,
                    onSelect = { controller.select(snapshot, it) },
                )
            }
            ExpandableSection(title = Strings.METADATA_SECTION) {
                MetadataSection(
                    snapshot = snapshot,
                    fields = listOf("title", "date", "location", "description"),
                    onSelect = { controller.select(snapshot, it) },
                )
            }
        }
    }
}

/**
 * Width/height inputs shown when the accepted resolution is Custom; the
 * core validates even dimensions in 16..8192.
 */
@Composable
private fun CustomResolutionInputs(
    snapshot: SessionSnapshot,
    controller: WorkflowController,
) {
    val accepted = snapshot.valueFor(Fields.RESOLUTION)?.valueId
    if (accepted != "custom") {
        return
    }
    val payload = snapshot.valueFor(Fields.RESOLUTION)?.payload
    val width = (payload as? kotlinx.serialization.json.JsonObject)
        ?.get("width")?.let { (it as? JsonPrimitive)?.content?.toIntOrNull() } ?: 0
    val height = (payload as? kotlinx.serialization.json.JsonObject)
        ?.get("height")?.let { (it as? JsonPrimitive)?.content?.toIntOrNull() } ?: 0

    LabeledInputRow(
        label = Strings.RESOLUTION_WIDTH,
        acceptedValue = if (width > 0) width.toString() else "",
        placeholder = "1920",
        onApply = { text ->
            text.toIntOrNull()?.let { value ->
                controller.select(
                    snapshot,
                    resolutionChange(width = value, height = height),
                )
            }
        },
        applyEnabled = { (it.toIntOrNull() ?: 0) in 16..8192 && (it.toIntOrNull() ?: 0) % 2 == 0 },
    )
    LabeledInputRow(
        label = Strings.RESOLUTION_HEIGHT,
        acceptedValue = if (height > 0) height.toString() else "",
        placeholder = "1080",
        onApply = { text ->
            text.toIntOrNull()?.let { value ->
                controller.select(
                    snapshot,
                    resolutionChange(width = width, height = value),
                )
            }
        },
        applyEnabled = { (it.toIntOrNull() ?: 0) in 16..8192 && (it.toIntOrNull() ?: 0) % 2 == 0 },
    )
}

private fun resolutionChange(width: Int, height: Int): FieldChange =
    FieldChange(
        field = Fields.RESOLUTION,
        valueId = "custom",
        payload = buildJsonObject {
            put("width", width)
            put("height", height)
        },
    )

/**
 * The audio track rules editor (mandate §23): include/remove per stable
 * source track. All tracks are preserved by default (the core's
 * preserve-all contract); unchecking a track sends the full rules array
 * with include=false for it.
 */
@Composable
private fun AudioTrackRulesEditor(
    snapshot: SessionSnapshot,
    controller: WorkflowController,
) {
    val media = snapshot.sources.firstOrNull() ?: return
    val acceptedPayload = (snapshot.valueFor(Fields.AUDIO_TRACKS)?.payload as? JsonArray)
    val excluded = acceptedPayload
        ?.mapNotNull { item ->
            val rule = item as? kotlinx.serialization.json.JsonObject ?: return@mapNotNull null
            val index = rule["index"]?.let { (it as? JsonPrimitive)?.content?.toIntOrNull() }
            val include = rule["include"]?.let { (it as? JsonPrimitive)?.content == "true" }
            if (index != null && include == false) index else null
        }
        ?.toSet() ?: emptySet()

    if (media.audio.isEmpty()) {
        androidx.compose.material3.Text(
            text = Strings.TRACKS_NONE,
            style = androidx.compose.material3.MaterialTheme.typography.bodyMedium,
            color = androidx.compose.material3.MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(vertical = 8.dp),
        )
        return
    }
    media.audio.forEachIndexed { index, track ->
        CheckRow(
            label = Strings.TRACK_FORMAT.format(index + 1) +
                (track.language?.takeIf { it.isNotBlank() && it != "und" }
                    ?.let { " · $it" } ?: ""),
            detail = "${track.codec}" +
                (if (track.channels > 0) " ${track.channels}ch" else "") +
                (if (track.sampleRate > 0) " ${track.sampleRate / 1000}kHz" else ""),
            checked = index !in excluded,
            onCheckedChange = { checked ->
                val nextExcluded = if (checked) {
                    excluded - index
                } else {
                    excluded + index
                }
                controller.select(
                    snapshot,
                    audioTracksChange(media.audio.size, nextExcluded),
                )
            },
        )
    }
}

/** Builds the audio.tracks rules payload (include=false for excluded). */
private fun audioTracksChange(trackCount: Int, excluded: Set<Int>): FieldChange =
    FieldChange(
        field = Fields.AUDIO_TRACKS,
        valueId = "custom",
        payload = buildJsonArray {
            for (index in 0 until trackCount) {
                add(
                    buildJsonObject {
                        put("index", index)
                        put("include", index !in excluded)
                    },
                )
            }
        },
    )

/**
 * The subtitle rules editor (mandate §24): per-track policy — Preserve,
 * Burn in (video processing) or Remove.
 */
@Composable
private fun SubtitleRulesEditor(
    snapshot: SessionSnapshot,
    controller: WorkflowController,
) {
    val media = snapshot.sources.firstOrNull() ?: return
    if (media.subtitles.isEmpty()) {
        androidx.compose.material3.Text(
            text = Strings.SUBTITLE_TRACKS_NONE,
            style = androidx.compose.material3.MaterialTheme.typography.bodyMedium,
            color = androidx.compose.material3.MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(vertical = 8.dp),
        )
        return
    }
    val acceptedPayload = (snapshot.valueFor(Fields.SUBTITLES)?.payload as? JsonArray)
    val policies = acceptedPayload
        ?.mapNotNull { item ->
            val rule = item as? kotlinx.serialization.json.JsonObject ?: return@mapNotNull null
            val index = rule["index"]?.let { (it as? JsonPrimitive)?.content?.toIntOrNull() }
            val policy = rule["policy"]?.let { (it as? JsonPrimitive)?.content }
            if (index != null && policy != null) index to policy else null
        }
        ?.toMap() ?: emptyMap()

    media.subtitles.forEachIndexed { index, track ->
        val current = policies[index] ?: "preserve"
        androidx.compose.material3.Text(
            text = Strings.TRACK_FORMAT.format(index + 1) +
                (track.language?.takeIf { it.isNotBlank() && it != "und" }
                    ?.let { " · $it" } ?: "") + " · ${track.format}",
            style = androidx.compose.material3.MaterialTheme.typography.bodyLarge,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
        )
        listOf(
            "preserve" to Strings.SUBTITLE_POLICY_PRESERVE,
            "burnIn" to Strings.SUBTITLE_POLICY_BURN_IN,
            "remove" to Strings.SUBTITLE_POLICY_REMOVE,
        ).forEach { (policy, label) ->
            ChoiceRow(
                label = label,
                selected = current == policy,
                onClick = {
                    controller.select(
                        snapshot,
                        subtitleTracksChange(media.subtitles.size, index, policy),
                    )
                },
                modifier = Modifier.padding(start = 16.dp),
            )
        }
    }
}

/** Builds the subtitles rules payload with [policy] for [trackIndex]. */
private fun subtitleTracksChange(
    trackCount: Int,
    trackIndex: Int,
    policy: String,
): FieldChange =
    FieldChange(
        field = Fields.SUBTITLES,
        valueId = "custom",
        payload = buildJsonArray {
            for (index in 0 until trackCount) {
                add(
                    buildJsonObject {
                        put("index", index)
                        put("policy", if (index == trackIndex) policy else "preserve")
                    },
                )
            }
        },
    )
