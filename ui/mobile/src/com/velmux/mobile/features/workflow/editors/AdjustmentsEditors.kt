package com.velmux.mobile.features.workflow.editors

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.features.workflow.payloadOf
import com.velmux.mobile.features.workflow.toJson
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.engine.SessionSnapshot
import com.velmux.mobile.features.workflow.AudioAdjustmentsUi
import com.velmux.mobile.features.workflow.VideoAdjustmentsUi
import com.velmux.mobile.features.workflow.toAudioAdjustments
import com.velmux.mobile.features.workflow.toVideoAdjustments

/**
 * The Video Converter Adjustments section (mandate §25): rotate, flips,
 * playback speed, reverse, HDR-to-SDR.
 */
@Composable
fun VideoAdjustmentsSection(
    snapshot: SessionSnapshot,
    onSelect: (FieldChange) -> Unit,
    modifier: Modifier = Modifier,
) {
    val current = snapshot.payloadOf(Fields.ADJUSTMENTS).toVideoAdjustments()
    val apply = { next: VideoAdjustmentsUi ->
        onSelect(
            FieldChange(
                field = Fields.ADJUSTMENTS,
                valueId = "custom",
                payload = next.toJson(),
            ),
        )
    }

    LabeledInputRow(
        label = Strings.ADJUST_ROTATION,
        acceptedValue = current.rotation.toString(),
        placeholder = "0",
        onApply = { text ->
            text.toIntOrNull()?.let { degrees ->
                apply(current.copy(rotation = degrees))
            }
        },
        applyEnabled = { it.toIntOrNull() in setOf(0, 90, 180, 270) },
        modifier = modifier,
    )
    SwitchRow(
        label = Strings.ADJUST_FLIP_H,
        checked = current.flipH,
        onCheckedChange = { checked -> apply(current.copy(flipH = checked)) },
        modifier = modifier,
    )
    SwitchRow(
        label = Strings.ADJUST_FLIP_V,
        checked = current.flipV,
        onCheckedChange = { checked -> apply(current.copy(flipV = checked)) },
        modifier = modifier,
    )
    LabeledInputRow(
        label = Strings.ADJUST_SPEED,
        acceptedValue = current.speed?.toString() ?: "",
        placeholder = "1.0 (0.5 – 2.0)",
        keyboardType = KeyboardType.Decimal,
        onApply = { text ->
            val value = text.toDoubleOrNull()
            if (value == null) {
                apply(current.copy(speed = null))
            } else {
                apply(current.copy(speed = value))
            }
        },
        applyEnabled = { it.isBlank() || (it.toDoubleOrNull() ?: 0.0) in 0.5..2.0 },
        modifier = modifier,
    )
    SwitchRow(
        label = Strings.ADJUST_REVERSE,
        checked = current.reverse,
        onCheckedChange = { checked -> apply(current.copy(reverse = checked)) },
        modifier = modifier,
    )
    SwitchRow(
        label = Strings.ADJUST_HDR_TO_SDR,
        checked = current.hdrToSdr,
        supportingText = Strings.ADJUST_HDR_TO_SDR_HINT,
        onCheckedChange = { checked -> apply(current.copy(hdrToSdr = checked)) },
        modifier = modifier,
    )
}

/**
 * The Audio Converter Adjustments section (mandate §32): volume,
 * normalize, fades, speed, pitch, reverse, silence removal.
 */
@Composable
fun AudioAdjustmentsSection(
    snapshot: SessionSnapshot,
    onSelect: (FieldChange) -> Unit,
    modifier: Modifier = Modifier,
) {
    val current = snapshot.payloadOf(Fields.ADJUSTMENTS).toAudioAdjustments()
    val apply = { next: AudioAdjustmentsUi ->
        onSelect(
            FieldChange(
                field = Fields.ADJUSTMENTS,
                valueId = "custom",
                payload = next.toJson(),
            ),
        )
    }

    LabeledInputRow(
        label = Strings.ADJUST_VOLUME,
        acceptedValue = current.volumeDb?.toString() ?: "",
        placeholder = "0 (dB, -60 – 60)",
        keyboardType = KeyboardType.Decimal,
        onApply = { text ->
            val value = text.toDoubleOrNull()
            if (value == null) {
                apply(current.copy(volumeDb = null))
            } else {
                apply(current.copy(volumeDb = value))
            }
        },
        applyEnabled = { it.isBlank() || (it.toDoubleOrNull() ?: 0.0) in -60.0..60.0 },
        modifier = modifier,
    )
    SwitchRow(
        label = Strings.ADJUST_NORMALIZE,
        checked = current.normalize,
        supportingText = Strings.ADJUST_NORMALIZE_HINT,
        onCheckedChange = { checked -> apply(current.copy(normalize = checked)) },
        modifier = modifier,
    )
    LabeledInputRow(
        label = Strings.ADJUST_FADE_IN,
        acceptedValue = current.fadeInMs?.toString() ?: "",
        placeholder = "1000 (ms)",
        onApply = { text ->
            val value = text.toIntOrNull()
            apply(current.copy(fadeInMs = value))
        },
        applyEnabled = { it.isBlank() || (it.toIntOrNull() ?: -1) in 0..60000 },
        modifier = modifier,
    )
    LabeledInputRow(
        label = Strings.ADJUST_FADE_OUT,
        acceptedValue = current.fadeOutMs?.toString() ?: "",
        placeholder = "1000 (ms)",
        onApply = { text ->
            val value = text.toIntOrNull()
            apply(current.copy(fadeOutMs = value))
        },
        applyEnabled = { it.isBlank() || (it.toIntOrNull() ?: -1) in 0..60000 },
        modifier = modifier,
    )
    LabeledInputRow(
        label = Strings.ADJUST_SPEED,
        acceptedValue = current.speed?.toString() ?: "",
        placeholder = "1.0 (0.5 – 2.0)",
        keyboardType = KeyboardType.Decimal,
        onApply = { text ->
            val value = text.toDoubleOrNull()
            if (value == null) {
                apply(current.copy(speed = null))
            } else {
                apply(current.copy(speed = value))
            }
        },
        applyEnabled = { it.isBlank() || (it.toDoubleOrNull() ?: 0.0) in 0.5..2.0 },
        modifier = modifier,
    )
    SwitchRow(
        label = Strings.ADJUST_PRESERVE_PITCH,
        checked = current.preservePitch,
        onCheckedChange = { checked -> apply(current.copy(preservePitch = checked)) },
        modifier = modifier,
    )
    SwitchRow(
        label = Strings.ADJUST_REVERSE,
        checked = current.reverse,
        onCheckedChange = { checked -> apply(current.copy(reverse = checked)) },
        modifier = modifier,
    )
    SwitchRow(
        label = Strings.ADJUST_REMOVE_SILENCE,
        checked = current.removeSilence,
        supportingText = Strings.ADJUST_REMOVE_SILENCE_HINT,
        onCheckedChange = { checked -> apply(current.copy(removeSilence = checked)) },
        modifier = modifier,
    )
}

/**
 * The Metadata section (mandate §33): the familiar file-detail fields.
 * Blank input clears a Set field; the "unchanged" state is preserved
 * until the user edits the field (present-key contract).
 */
@Composable
fun MetadataSection(
    snapshot: SessionSnapshot,
    fields: List<String>,
    onSelect: (FieldChange) -> Unit,
    modifier: Modifier = Modifier,
) {
    val payload = snapshot.payloadOf(Fields.METADATA)
    val labels = mapOf(
        "title" to Strings.META_TITLE,
        "artist" to Strings.META_ARTIST,
        "album" to Strings.META_ALBUM,
        "genre" to Strings.META_GENRE,
        "date" to Strings.META_DATE,
        "description" to Strings.META_DESCRIPTION,
        "location" to Strings.META_LOCATION,
    )
    fields.forEach { key ->
        val label = labels[key] ?: key
        val accepted = when {
            payload == null -> ""
            !payload.containsKey(key) -> ""
            payload.getValue(key) is JsonNull -> ""
            else -> (payload.getValue(key) as? JsonPrimitive)
                ?.content ?: ""
        }
        LabeledInputRow(
            label = label,
            acceptedValue = accepted,
            placeholder = Strings.META_PLACEHOLDER,
            keyboardType = androidx.compose.ui.text.input.KeyboardType.Text,
            onApply = { text ->
                onSelect(
                    FieldChange(
                        field = Fields.METADATA,
                        valueId = "custom",
                        payload = buildMetadataPayload(fields, payload, key, text),
                    ),
                )
            },
            modifier = modifier,
        )
    }
}

/**
 * Builds the metadata update payload preserving every other field's
 * current intention (Set stays Set, Clear stays Clear, Unchanged stays
 * Unchanged — only [editedKey] changes).
 */
private fun buildMetadataPayload(
    fields: List<String>,
    current: JsonObject?,
    editedKey: String,
    text: String,
): JsonObject {
    return buildJsonObject {
        fields.forEach { key ->
            if (key == editedKey) {
                if (text.isBlank()) {
                    put(key, JsonNull)
                } else {
                    put(key, text)
                }
            } else if (current != null && current.containsKey(key)) {
                val value = current.getValue(key)
                if (value is JsonNull) {
                    put(key, JsonNull)
                } else {
                    put(
                        key,
                        (value as? JsonPrimitive)?.content
                            ?: "",
                    )
                }
            }
        }
    }
}
