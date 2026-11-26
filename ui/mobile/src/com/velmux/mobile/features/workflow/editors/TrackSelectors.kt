package com.velmux.mobile.features.workflow.editors

import com.velmux.mobile.fmt
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Checkbox
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.actionRow
import com.velmux.mobile.components.toggleRow
import com.velmux.mobile.engine.AudioStream
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.engine.SubtitleStream
import com.velmux.mobile.features.workflow.payloadArrayOf

/**
 * The multi-select audio track list (Extract Audio, mandate §28): stable
 * source track indices, not UI rows. Empty selection = default track;
 * the core validates every index.
 */
@Composable
fun AudioTrackSelector(
    tracks: List<AudioStream>,
    selected: Set<Int>,
    onSelectionChange: (Set<Int>) -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(modifier = modifier.fillMaxWidth()) {
        if (tracks.isEmpty()) {
            Text(
                text = Strings.TRACKS_NONE,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            )
        }
        tracks.forEachIndexed { index, track ->
            val label = trackLabel(
                index,
                track.language,
                "${track.codec}" + if (track.channels > 0) {
                    " ${track.channels}ch"
                } else {
                    ""
                } + if (track.sampleRate > 0) {
                    " ${track.sampleRate / 1000}kHz"
                } else {
                    ""
                },
            )
            CheckRow(
                label = label.first,
                detail = label.second,
                checked = index in selected,
                onCheckedChange = { checked ->
                    val next = selected.toMutableSet()
                    if (checked) {
                        next.add(index)
                    } else {
                        next.remove(index)
                    }
                    onSelectionChange(next)
                },
            )
        }
    }
}

/**
 * The subtitle track multi-select (Extract Subtitles, mandate §30) with
 * the representation of each track shown.
 */
@Composable
fun SubtitleTrackSelector(
    tracks: List<SubtitleStream>,
    selected: Set<Int>,
    onSelectionChange: (Set<Int>) -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(modifier = modifier.fillMaxWidth()) {
        if (tracks.isEmpty()) {
            Text(
                text = Strings.SUBTITLE_TRACKS_NONE,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            )
        }
        tracks.forEachIndexed { index, track ->
            val flags = buildString {
                if (track.isDefault) {
                    append(Strings.SUBTITLE_DEFAULT)
                }
                if (track.isForced) {
                    if (isNotEmpty()) {
                        append(" · ")
                    }
                    append(Strings.SUBTITLE_FORCED)
                }
            }
            val label = trackLabel(index, track.language, track.format)
            CheckRow(
                label = label.first,
                detail = label.second +
                    (if (flags.isNotEmpty()) " · $flags" else ""),
                checked = index in selected,
                onCheckedChange = { checked ->
                    val next = selected.toMutableSet()
                    if (checked) {
                        next.add(index)
                    } else {
                        next.remove(index)
                    }
                    onSelectionChange(next)
                },
            )
        }
    }
}

/** One labeled checkbox row (44dp target, checkbox semantics). */
@Composable
fun CheckRow(
    label: String,
    detail: String?,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .heightIn(min = 44.dp)
            .padding(horizontal = 16.dp)
            .toggleRow(checked) { onCheckedChange(!checked) },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Checkbox(checked = checked, onCheckedChange = null)
        Spacer(Modifier.width(8.dp))
        Column(modifier = Modifier.padding(vertical = 6.dp)) {
            Text(text = label, style = MaterialTheme.typography.bodyLarge)
            if (detail != null && detail.isNotBlank()) {
                Text(
                    text = detail,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

/** "Track 2 · English · aac 2ch 48kHz". */
private fun trackLabel(index: Int, language: String?, detail: String): Pair<String, String> {
    val title = Strings.TRACK_FORMAT.fmt(index + 1) +
        (language?.takeIf { it.isNotBlank() && it != "und" }
            ?.let { " · ${languageLabel(it)}" } ?: "")
    return title to detail
}

/** Common ISO-639 language codes the engine reports, as display names. */
private fun languageLabel(code: String): String = when (code.lowercase()) {
    "eng" -> "English"
    "deu", "ger" -> "German"
    "fra", "fre" -> "French"
    "spa" -> "Spanish"
    "ita" -> "Italian"
    "por" -> "Portuguese"
    "rus" -> "Russian"
    "jpn" -> "Japanese"
    "kor" -> "Korean"
    "zho", "chi" -> "Chinese"
    "ara" -> "Arabic"
    "hin" -> "Hindi"
    "nld", "dut" -> "Dutch"
    "pol" -> "Polish"
    "tur" -> "Turkish"
    "swe" -> "Swedish"
    "nor" -> "Norwegian"
    "dan" -> "Danish"
    "fin" -> "Finnish"
    "ces", "cze" -> "Czech"
    else -> code
}

/** Reads the accepted extract-audio track selection from the snapshot. */
fun com.velmux.mobile.engine.SessionSnapshot.selectedAudioTrackIndices(): Set<Int> {
    val element = payloadArrayOf(com.velmux.mobile.engine.Fields.EXTRACT_AUDIO_TRACKS)
        ?: return emptySet()
    return element.mapNotNull { (it as? kotlinx.serialization.json.JsonPrimitive)?.content?.toIntOrNull() }.toSet()
}

/** Reads the accepted subtitle track selection from the snapshot. */
fun com.velmux.mobile.engine.SessionSnapshot.selectedSubtitleTrackIndices(): Set<Int> {
    val element = payloadArrayOf(com.velmux.mobile.engine.Fields.SUBTITLE_TRACKS)
        ?: return emptySet()
    return element.mapNotNull { (it as? kotlinx.serialization.json.JsonPrimitive)?.content?.toIntOrNull() }.toSet()
}

/** Builds the track-selection change (indices payload). */
fun trackIndicesChange(field: String, indices: Set<Int>): FieldChange =
    FieldChange(
        field = field,
        valueId = "custom",
        payload = kotlinx.serialization.json.buildJsonArray {
            indices.sorted().forEach { index ->
                add(kotlinx.serialization.json.JsonPrimitive(index))
            }
        },
    )
