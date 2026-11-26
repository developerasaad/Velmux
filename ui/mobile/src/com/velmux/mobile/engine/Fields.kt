package com.velmux.mobile.engine

import com.velmux.mobile.fmt
import kotlin.math.floor

/**
 * Canonical session-field identifiers (fieldIdName in core/src/catalog/
 * types.cpp) and the user-facing labels for their value ids. The UI maps
 * canonical ids to labels here — never the other way around (mandate
 * §13.2): identifiers are stable engine contract, labels are presentation.
 */
object Fields {
    const val FORMAT = "format"
    const val CODEC = "codec"
    const val RESOLUTION = "resolution"
    const val FRAME_RATE = "frame-rate"
    const val COMPRESSION_ENABLED = "compression.enabled"
    const val COMPRESSION_PRESET = "compression.preset"
    const val COMPRESSION_QUALITY = "compression.quality"
    const val COMPRESSION_VIDEO_BITRATE = "compression.video-bitrate"
    const val COMPRESSION_AUDIO_BITRATE = "compression.audio-bitrate"
    const val COMPRESSION_TARGET_SIZE = "compression.target-size"
    const val AUDIO_CODEC = "audio.codec"
    const val AUDIO_BITRATE = "audio.bitrate"
    const val AUDIO_SAMPLE_RATE = "audio.sample-rate"
    const val AUDIO_CHANNELS = "audio.channels"
    const val AUDIO_TRACKS = "audio.tracks"
    const val SUBTITLES = "subtitles"
    const val ADJUSTMENTS = "adjustments"
    const val METADATA = "metadata"
    const val OUTPUT_NAME = "output.name"
    const val CROP_RECT = "crop.rect"
    const val TIMELINE_MODE = "timeline.mode"
    const val TIMELINE_RANGES = "timeline.ranges"
    const val SPLIT_RULES = "split.rules"
    const val MERGE_LAYOUT = "merge.layout"
    const val MERGE_AUDIO = "merge.audio"
    const val FRAME_SELECTION = "frames.selection"
    const val IMAGE_FORMAT = "image.format"
    const val IMAGE_QUALITY = "image.quality"
    const val IMAGE_DIMENSIONS = "image.dimensions"
    const val SUBTITLE_TRACKS = "subtitle.tracks"
    const val SUBTITLE_TARGET = "subtitle.target"
    const val EXTRACT_AUDIO_TRACKS = "extract-audio.tracks"
    const val EXTRACT_AUDIO_RANGE = "extract-audio.range"
}

/**
 * User-facing labels for canonical value ids (display localization only).
 * Unknown future ids fall back to the id itself — additive engine values
 * stay visible instead of hidden.
 */
object ValueLabels {
    /** Labels for container/codec/format ids and common field values. */
    fun label(field: String, valueId: String): String = when (valueId) {
        "original" -> "Original"
        "auto" -> "Automatic"
        "true" -> "On"
        "false" -> "Off"
        "custom" -> "Custom"
        "default" -> "Default"
        "preserve-all" -> "Preserve all"
        "unchanged" -> "Unchanged"
        "all" -> "All tracks"
        "full" -> "Full duration"
        "none" -> "None"
        else -> labelForField(field, valueId)
    }

    private fun labelForField(field: String, valueId: String): String {
        return when (field) {
            Fields.COMPRESSION_PRESET -> presetLabel(valueId)
            Fields.TIMELINE_MODE -> when (valueId) {
                "trim" -> "Trim"
                "cut" -> "Cut"
                "split" -> "Split"
                "merge" -> "Merge"
                else -> valueId
            }
            Fields.MERGE_LAYOUT -> when (valueId) {
                "sequential" -> "Sequential"
                "side-by-side" -> "Side by side"
                "top-bottom" -> "Top and bottom"
                else -> valueId
            }
            Fields.MERGE_AUDIO -> when (valueId) {
                "first" -> "First video"
                "second" -> "Second video"
                "both" -> "Both"
                "none" -> "No audio"
                else -> valueId
            }
            Fields.AUDIO_CHANNELS -> when (valueId) {
                "1" -> "Mono"
                "2" -> "Stereo"
                "6" -> "5.1 surround"
                else -> valueId
            }
            Fields.AUDIO_SAMPLE_RATE, Fields.AUDIO_BITRATE,
            -> prettyNumber(valueId)
            Fields.RESOLUTION -> when (valueId) {
                "2160p" -> "2160p (4K UHD)"
                "1440p" -> "1440p (2K)"
                "1080p" -> "1080p (Full HD)"
                "720p" -> "720p (HD)"
                "480p" -> "480p"
                "360p" -> "360p"
                else -> valueId
            }
            Fields.FRAME_RATE -> frameRateLabel(valueId)
            Fields.IMAGE_FORMAT -> when (valueId) {
                "jpeg" -> "JPEG"
                "png" -> "PNG"
                "webp" -> "WebP"
                else -> valueId
            }
            Fields.SPLIT_RULES -> when (valueId) {
                "equal-parts" -> "Equal parts"
                "duration" -> "Duration"
                "max-size" -> "Maximum size"
                "custom" -> "Custom timestamps"
                "chapters" -> "Chapters"
                "scenes" -> "Scenes"
                else -> valueId
            }
            else -> valueId
        }
    }

    private fun presetLabel(valueId: String): String = when (valueId) {
        "high-quality" -> "High quality"
        "balanced" -> "Balanced quality"
        "smaller-file" -> "Smaller file"
        "target-size" -> "Target file size"
        "custom" -> "Custom"
        else -> valueId
    }

    /** 48000 -> "48 kHz"; 128000 -> "128 kbps" (caller decides unit). */
    private fun prettyNumber(valueId: String): String = valueId

    /** "24000/1001" -> "23.976"; "30/1" -> "30". */
    fun frameRateLabel(valueId: String): String {
        val slash = valueId.indexOf('/')
        if (slash <= 0) {
            return valueId
        }
        val num = valueId.substring(0, slash).toDoubleOrNull() ?: return valueId
        val den = valueId.substring(slash + 1).toDoubleOrNull() ?: return valueId
        if (den == 0.0) {
            return valueId
        }
        val value = num / den
        return if (value == floor(value) && !value.isInfinite()) {
            "${value.toInt()}"
        } else {
            "%.3f".fmt(value).trimEnd('0').trimEnd('.')
        }
    }
}
