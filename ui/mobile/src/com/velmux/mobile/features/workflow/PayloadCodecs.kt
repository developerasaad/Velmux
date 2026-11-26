package com.velmux.mobile.features.workflow

import com.velmux.mobile.engine.Fields
import com.velmux.mobile.engine.SessionSnapshot
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put

/**
 * Payload codecs for the composite session fields: parse the accepted
 * payload the core emitted and build update payloads the core's
 * applyChange accepts (round-trip stable by construction — the shapes
 * mirror core/src/configuration/sessions.cpp).
 */

/** The accepted payload object for [field], or null when absent/not one. */
fun SessionSnapshot.payloadOf(field: String): JsonObject? {
    val element = valueFor(field)?.payload ?: return null
    return element as? JsonObject
}

/** The accepted payload array for [field], or null when absent/not one. */
fun SessionSnapshot.payloadArrayOf(field: String): JsonArray? {
    val element = valueFor(field)?.payload ?: return null
    return element as? JsonArray
}

// -- time ranges -------------------------------------------------------------

/** [startUs, endUs); endUs < 0 means open-ended (to end of stream). */
data class UsRange(val startUs: Long, val endUs: Long)

fun JsonObject?.toUsRange(): UsRange = UsRange(
    startUs = this.longOr("startUs", 0L),
    endUs = this.longOr("endUs", -1L),
)

fun UsRange.toJson(): JsonObject = buildJsonObject {
    put("startUs", startUs)
    put("endUs", endUs)
}

fun JsonObject?.toRemovedRanges(): List<UsRange> {
    val removed = (this?.get("removed") as? JsonArray) ?: return emptyList()
    return removed.mapNotNull { item ->
        (item as? JsonObject)?.toUsRange()
    }
}

fun removedRangesJson(ranges: List<UsRange>): JsonObject = buildJsonObject {
    put(
        "removed",
        buildJsonArray {
            ranges.forEach { range -> add(range.toJson()) }
        },
    )
}

// -- crop --------------------------------------------------------------------

data class CropRect(val x: Int, val y: Int, val width: Int, val height: Int)

fun JsonObject?.toCropRect(): CropRect = CropRect(
    x = this.intOr("x", 0),
    y = this.intOr("y", 0),
    width = this.intOr("width", 0),
    height = this.intOr("height", 0),
)

fun CropRect.toJson(): JsonObject = buildJsonObject {
    put("x", x)
    put("y", y)
    put("width", width)
    put("height", height)
}

// -- split rules ---------------------------------------------------------------

enum class SplitModeUi(val wire: String, val label: String) {
    EQUAL_PARTS("equal-parts", "Equal parts"),
    DURATION("duration", "Duration"),
    MAX_SIZE("max-size", "Maximum size"),
    CUSTOM("custom", "Custom timestamps"),
    CHAPTERS("chapters", "Chapters"),
    SCENES("scenes", "Scenes"),
    ;

    companion object {
        fun of(wire: String): SplitModeUi =
            entries.firstOrNull { it.wire == wire } ?: EQUAL_PARTS
    }
}

data class SplitRulesUi(
    val mode: SplitModeUi = SplitModeUi.EQUAL_PARTS,
    val partCount: Int = 2,
    val partDurationUs: Long = 0,
    val maxSizeBytes: Long = 0,
    val boundariesUs: List<Long> = emptyList(),
)

fun JsonObject?.toSplitRules(): SplitRulesUi {
    val mode = SplitModeUi.of(this?.get("mode")?.let { (it as? JsonPrimitive)?.content } ?: "equal-parts")
    return SplitRulesUi(
        mode = mode,
        partCount = this.intOr("partCount", 2),
        partDurationUs = this.longOr("partDurationUs", 0L),
        maxSizeBytes = this.longOr("maxSizeBytes", 0L),
        boundariesUs = (this?.get("boundaries") as? JsonArray)
            ?.mapNotNull { (it as? JsonPrimitive)?.longOrNull } ?: emptyList(),
    )
}

fun SplitRulesUi.toJson(): JsonObject = buildJsonObject {
    put("mode", mode.wire)
    put("partCount", partCount)
    if (partDurationUs > 0) {
        put("partDurationUs", partDurationUs)
    }
    if (maxSizeBytes > 0) {
        put("maxSizeBytes", maxSizeBytes)
    }
    if (boundariesUs.isNotEmpty()) {
        put(
            "boundaries",
            buildJsonArray { boundariesUs.forEach { add(JsonPrimitive(it)) } },
        )
    }
}

// -- merge ----------------------------------------------------------------------

fun JsonObject?.mergeLongerDuration(): Boolean =
    (this?.get("longerDuration") as? JsonPrimitive)?.booleanOrNull ?: true

fun mergeAudioJson(longerDuration: Boolean): JsonObject = buildJsonObject {
    put("longerDuration", longerDuration)
}

// -- frame selection ---------------------------------------------------------------

enum class FrameModeUi(val wire: String, val label: String) {
    CURRENT("current", "Current frame"),
    SELECTED("selected", "Selected frames"),
    INTERVAL("interval", "By interval"),
    FRAME_RATE("frame-rate", "By frame rate"),
    COUNT("count", "Desired count"),
    ALL("all", "All frames in range"),
    KEYFRAMES("keyframes", "Keyframes"),
    SCENES("scenes", "Scene changes"),
    ;

    companion object {
        fun of(wire: String): FrameModeUi =
            entries.firstOrNull { it.wire == wire } ?: ALL
    }
}

data class FrameSelectionUi(
    val mode: FrameModeUi = FrameModeUi.ALL,
    val intervalUs: Long = 0,
    val frameRateWire: String? = null,
    val count: Int = 0,
    val range: UsRange = UsRange(0, -1),
)

fun JsonObject?.toFrameSelection(): FrameSelectionUi {
    val modeWire = this?.get("mode")?.let { (it as? JsonPrimitive)?.content } ?: "all"
    return FrameSelectionUi(
        mode = FrameModeUi.of(modeWire),
        intervalUs = this.longOr("intervalUs", 0L),
        frameRateWire = (this?.get("frameRate") as? JsonPrimitive)?.content,
        count = this.intOr("count", 0),
        range = (this?.get("range") as? JsonObject).toUsRange(),
    )
}

fun FrameSelectionUi.toJson(): JsonObject = buildJsonObject {
    put("mode", mode.wire)
    if (intervalUs > 0) {
        put("intervalUs", intervalUs)
    }
    if (frameRateWire != null) {
        put("frameRate", frameRateWire)
    }
    if (count > 0) {
        put("count", count)
    }
    put("range", range.toJson())
}

// -- image output ---------------------------------------------------------------

data class ImageOutputUi(
    val formatWire: String = "jpeg",
    val quality: Int? = null,
    val maxWidth: Int? = null,
    val maxHeight: Int? = null,
)

fun JsonObject?.toImageDimensions(): Pair<Int?, Int?> {
    val width = (this?.get("maxWidth") as? JsonPrimitive)?.intOrNull
    val height = (this?.get("maxHeight") as? JsonPrimitive)?.intOrNull
    return width to height
}

fun imageDimensionsJson(width: Int?, height: Int?): JsonObject = buildJsonObject {
    put("maxWidth", width ?: 0)
    put("maxHeight", height ?: 0)
}


/** Primitive accessors over a nullable payload object (defensive). */
private fun JsonObject?.longOr(key: String, fallback: Long): Long =
    (this?.get(key) as? JsonPrimitive)?.longOrNull ?: fallback

private fun JsonObject?.intOr(key: String, fallback: Int): Int =
    (this?.get(key) as? JsonPrimitive)?.intOrNull ?: fallback

private fun JsonObject?.intOrNullable(key: String): Int? =
    if (this == null || !containsKey(key) || get(key) is JsonNull) {
        null
    } else {
        (get(key) as? JsonPrimitive)?.intOrNull
    }

private fun JsonObject?.doubleOr(key: String): Double? =
    if (this == null || !containsKey(key) || get(key) is JsonNull) {
        null
    } else {
        (get(key) as? JsonPrimitive)?.doubleOrNull
    }

private fun JsonObject?.boolOr(key: String, fallback: Boolean = false): Boolean =
    (this?.get(key) as? JsonPrimitive)?.booleanOrNull ?: fallback

// -- track selections (accepted payload arrays of track indices) -------------

fun JsonArray?.toIntList(): List<Int> =
    this?.mapNotNull { (it as? JsonPrimitive)?.intOrNull } ?: emptyList()

fun intListJson(values: List<Int>): JsonArray = buildJsonArray {
    values.forEach { add(JsonPrimitive(it)) }
}


/** Reads the accepted extract-audio track selection from the snapshot. */
fun SessionSnapshot.selectedAudioTrackIndices(): Set<Int> {
    val element = payloadArrayOf(Fields.EXTRACT_AUDIO_TRACKS) ?: return emptySet()
    return element.mapNotNull { (it as? JsonPrimitive)?.content?.toIntOrNull() }.toSet()
}

/** Reads the accepted subtitle track selection from the snapshot. */
fun SessionSnapshot.selectedSubtitleTrackIndices(): Set<Int> {
    val element = payloadArrayOf(Fields.SUBTITLE_TRACKS) ?: return emptySet()
    return element.mapNotNull { (it as? JsonPrimitive)?.content?.toIntOrNull() }.toSet()
}

// -- video adjustments ---------------------------------------------------------------

data class VideoAdjustmentsUi(
    val rotation: Int = 0,
    val flipH: Boolean = false,
    val flipV: Boolean = false,
    val speed: Double? = null,
    val reverse: Boolean = false,
    val hdrToSdr: Boolean = false,
)

fun JsonObject?.toVideoAdjustments(): VideoAdjustmentsUi = VideoAdjustmentsUi(
    rotation = this.intOr("rotation", 0),
    flipH = this.boolOr("flipH"),
    flipV = this.boolOr("flipV"),
    speed = this.doubleOr("speed"),
    reverse = this.boolOr("reverse"),
    hdrToSdr = this.boolOr("hdrToSdr"),
)

fun VideoAdjustmentsUi.toJson(): JsonObject = buildJsonObject {
    put("rotation", rotation)
    put("flipH", flipH)
    put("flipV", flipV)
    put("speed", speed?.let { JsonPrimitive(it) } ?: JsonNull)
    put("reverse", reverse)
    put("hdrToSdr", hdrToSdr)
}

// -- audio adjustments -------------------------------------------------------------

data class AudioAdjustmentsUi(
    val volumeDb: Double? = null,
    val normalize: Boolean = false,
    val fadeInMs: Int? = null,
    val fadeOutMs: Int? = null,
    val speed: Double? = null,
    val preservePitch: Boolean = true,
    val reverse: Boolean = false,
    val removeSilence: Boolean = false,
)

fun JsonObject?.toAudioAdjustments(): AudioAdjustmentsUi = AudioAdjustmentsUi(
    volumeDb = this.doubleOr("volume"),
    normalize = this.boolOr("normalize"),
    fadeInMs = this.intOrNullable("fadeIn"),
    fadeOutMs = this.intOrNullable("fadeOut"),
    speed = this.doubleOr("speed"),
    preservePitch = this.boolOr("preservePitch", true),
    reverse = this.boolOr("reverse"),
    removeSilence = this.boolOr("removeSilence"),
)

fun AudioAdjustmentsUi.toJson(): JsonObject = buildJsonObject {
    put("volume", volumeDb?.let { JsonPrimitive(it) } ?: JsonNull)
    put("normalize", normalize)
    put("fadeIn", fadeInMs?.let { JsonPrimitive(it) } ?: JsonNull)
    put("fadeOut", fadeOutMs?.let { JsonPrimitive(it) } ?: JsonNull)
    put("speed", speed?.let { JsonPrimitive(it) } ?: JsonNull)
    put("preservePitch", preservePitch)
    put("reverse", reverse)
    put("removeSilence", removeSilence)
}

// -- metadata intentions -------------------------------------------------------------

data class MetadataUi(
    val title: String? = null,
    val artist: String? = null,
    val album: String? = null,
    val genre: String? = null,
    val date: String? = null,
    val description: String? = null,
    val location: String? = null,
)

/**
 * Reads the accepted metadata payload: present string = Set, null = Clear,
 * absent = Unchanged. [value] null keeps Unchanged semantics only when the
 * key is absent, so this codec keeps a THIRD state per field via the
 * presence of the key.
 */
fun JsonObject?.toMetadata(): MetadataUi = MetadataUi(
    title = metadataValue("title"),
    artist = metadataValue("artist"),
    album = metadataValue("album"),
    genre = metadataValue("genre"),
    date = metadataValue("date"),
    description = metadataValue("description"),
    location = metadataValue("location"),
)

private fun JsonObject?.metadataValue(key: String): String? {
    if (this == null || !containsKey(key)) {
        return null
    }
    val element = getValue(key)
    return if (element is JsonNull) {
        "" // explicit Clear — distinct from Unchanged (null)
    } else {
        (element as? JsonPrimitive)?.content ?: ""
    }
}

/** [value]: null = Unchanged, "" = Clear, other = Set. */
fun metadataJson(
    title: String? = null,
    artist: String? = null,
    album: String? = null,
    genre: String? = null,
    date: String? = null,
    description: String? = null,
    location: String? = null,
): JsonObject = buildJsonObject {
    fun entry(key: String, value: String?) {
        when (value) {
            null -> {}
            "" -> put(key, JsonNull)
            else -> put(key, value)
        }
    }
    entry("title", title)
    entry("artist", artist)
    entry("album", album)
    entry("genre", genre)
    entry("date", date)
    entry("description", description)
    entry("location", location)
}

// -- output name -----------------------------------------------------------------

/** The accepted timeline mode ("trim"/"cut"/"split"/"merge"). */
val SessionSnapshot.timelineMode: String
    get() = valueFor(Fields.TIMELINE_MODE)?.valueId ?: "trim"

/** The accepted output name payload (a JSON string), or null when auto. */
fun SessionSnapshot.outputNameOf(): String? {
    val element = valueFor(Fields.OUTPUT_NAME)?.payload
        ?: return null
    return (element as? JsonPrimitive)?.takeIf { it !is JsonNull }?.content
}

fun outputNameJson(name: String): JsonElement = JsonPrimitive(name)

// -- numeric helpers ----------------------------------------------------------------

/** Parses seconds text ("12", "12.5", "1:02.5") into microseconds. */
fun parseSecondsUs(text: String): Long? {
    val trimmed = text.trim()
    if (trimmed.isEmpty()) {
        return null
    }
    val colonParts = trimmed.split(':')
    val seconds: Double = when (colonParts.size) {
        1 -> colonParts[0].toDoubleOrNull() ?: return null
        2 -> {
            val minutes = colonParts[0].toDoubleOrNull() ?: return null
            val secs = colonParts[1].toDoubleOrNull() ?: return null
            minutes * 60.0 + secs
        }
        3 -> {
            val hours = colonParts[0].toDoubleOrNull() ?: return null
            val minutes = colonParts[1].toDoubleOrNull() ?: return null
            val secs = colonParts[2].toDoubleOrNull() ?: return null
            hours * 3600.0 + minutes * 60.0 + secs
        }
        else -> return null
    }
    if (seconds < 0.0 || seconds.isInfinite() || seconds.isNaN()) {
        return null
    }
    return (seconds * 1_000_000.0).toLong()
}

/** Formats microseconds as seconds text with 3 decimals ("1:02.500"). */
fun formatUs(us: Long): String {
    val totalSeconds = us / 1_000_000.0
    val hours = (totalSeconds / 3600).toInt()
    val minutes = ((totalSeconds % 3600) / 60).toInt()
    val seconds = totalSeconds % 60
    return if (hours > 0) {
        "%d:%02d:%06.3f".format(hours, minutes, seconds)
    } else {
        "%d:%06.3f".format(minutes, seconds)
    }
}

/** Parses a bitrate/size text into bits/bytes ("2.5M" -> 2_500_000). */
fun parseDecimal(text: String): Long? {
    val trimmed = text.trim().uppercase()
    if (trimmed.isEmpty()) {
        return null
    }
    val multiplier = when (trimmed.last()) {
        'K' -> 1_000L
        'M' -> 1_000_000L
        'G' -> 1_000_000_000L
        else -> 1L
    }
    val numberText = if (multiplier == 1L) {
        trimmed
    } else {
        trimmed.dropLast(1)
    }
    val number = numberText.toDoubleOrNull() ?: return null
    if (number < 0 || number.isInfinite() || number.isNaN()) {
        return null
    }
    return (number * multiplier).toLong()
}
