package com.velmux.mobile.engine

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonElement

/**
 * kotlinx.serialization DTOs for EVERY ABI v2 payload (spec UI-1).
 *
 * Field names MUST match `core/src/api/serialization.cpp` EXACTLY (the
 * emitter is authoritative — [com.velmux.mobile.engine.VelmuxJson] decodes
 * with `ignoreUnknownKeys` so additive engine fields never break the UI).
 *
 * Wire-format notes captured while reading the C++ (deviations between
 * abi.h comments and serialization.cpp are reported in the task report,
 * never "fixed" on the C++ side):
 *  - the session snapshot ends with `samplePath` (nullable) and carries NO
 *    `pipelineSummary` (abi.h documents the opposite);
 *  - accepted session values carry a `payload` field with the RAW payload
 *    JSON value (object/array/string), not a stringified document;
 *  - job events additionally carry `outputSizeBytes`.
 */
object Abi {
    /** VELMUX_ABI_VERSION of the contract this client speaks. */
    const val VERSION: Int = 2
}

// ---------------------------------------------------------------------------
// Media inspection (velmux_inspect / session snapshot sources)
// ---------------------------------------------------------------------------

@Serializable
data class MediaInfo(
    @SerialName("durationSec") val durationSec: Double = 0.0,
    @SerialName("container") val container: String = "",
    @SerialName("sizeBytes") val sizeBytes: Long = 0,
    @SerialName("attachedPictures") val attachedPictures: Int = 0,
    @SerialName("video") val video: List<VideoStream> = emptyList(),
    @SerialName("audio") val audio: List<AudioStream> = emptyList(),
    @SerialName("subtitles") val subtitles: List<SubtitleStream> = emptyList(),
) {
    val primaryVideo: VideoStream? get() = video.firstOrNull()
    val primaryAudio: AudioStream? get() = audio.firstOrNull()
}

@Serializable
data class VideoStream(
    @SerialName("codec") val codec: String = "",
    @SerialName("width") val width: Int = 0,
    @SerialName("height") val height: Int = 0,
    @SerialName("frameRate") val frameRate: Double = 0.0,
    @SerialName("bitRate") val bitRate: Long = 0,
    @SerialName("pixelFormat") val pixelFormat: String = "",
    @SerialName("hdr") val hdr: Boolean = false,
    @SerialName("hasClosedCaptions") val hasClosedCaptions: Boolean = false,
    @SerialName("language") val language: String? = null,
)

@Serializable
data class AudioStream(
    @SerialName("codec") val codec: String = "",
    @SerialName("channels") val channels: Int = 0,
    @SerialName("sampleRate") val sampleRate: Int = 0,
    @SerialName("bitRate") val bitRate: Long = 0,
    @SerialName("language") val language: String? = null,
)

@Serializable
data class SubtitleStream(
    @SerialName("format") val format: String = "",
    @SerialName("language") val language: String? = null,
    @SerialName("isDefault") val isDefault: Boolean = false,
    @SerialName("isForced") val isForced: Boolean = false,
)

// ---------------------------------------------------------------------------
// Configuration sessions
// ---------------------------------------------------------------------------

/** One source handed to `velmux_create_session`: {"uri":..,"name":..}. */
@Serializable
data class MediaSourceRequest(
    @SerialName("uri") val uri: String,
    @SerialName("name") val name: String,
)

/** {"operation":..,"sources":[{"uri":..,"name":..}]} — create-session request. */
@Serializable
data class CreateSessionRequest(
    @SerialName("operation") val operation: String,
    @SerialName("sources") val sources: List<MediaSourceRequest> = emptyList(),
)

/** {"sessionId":"s-..."} — response of create-session / reuse-settings. */
@Serializable
data class SessionIdResult(
    @SerialName("sessionId") val sessionId: String,
)

/** A value the engine accepted for a configuration field. */
@Serializable
data class AcceptedValue(
    @SerialName("field") val field: String,
    @SerialName("valueId") val valueId: String,
    @SerialName("origin") val origin: String = "default",
    /** Raw payload JSON (object/array/string) as emitted by the engine. */
    @SerialName("payload") val payload: JsonElement? = null,
)

@Serializable
data class OutputEstimate(
    @SerialName("known") val known: Boolean = false,
    @SerialName("estimatedBytes") val estimatedBytes: Long = 0,
    @SerialName("lowerBoundBytes") val lowerBoundBytes: Long = 0,
    @SerialName("upperBoundBytes") val upperBoundBytes: Long = 0,
)

@Serializable
data class RelatedChange(
    @SerialName("field") val field: String,
    @SerialName("reason") val reason: String,
    @SerialName("currentValueId") val currentValueId: String,
    @SerialName("proposedValueId") val proposedValueId: String,
)

/**
 * Session snapshot exactly as `sessionSnapshotToJson` emits it:
 * `samplePath` is real, `pipelineSummary` is NOT emitted for sessions.
 */
@Serializable
data class SessionSnapshot(
    @SerialName("sessionId") val sessionId: String,
    @SerialName("operation") val operation: String,
    @SerialName("state") val state: String,
    @SerialName("revision") val revision: Long = 0,
    @SerialName("sources") val sources: List<MediaInfo> = emptyList(),
    @SerialName("values") val values: List<AcceptedValue> = emptyList(),
    @SerialName("configurationValid") val configurationValid: Boolean = false,
    @SerialName("validationMessage") val validationMessage: String? = null,
    @SerialName("estimate") val estimate: OutputEstimate = OutputEstimate(),
    @SerialName("relatedChange") val relatedChange: RelatedChange? = null,
    @SerialName("samplePath") val samplePath: String? = null,
) {
    fun valueFor(field: String): AcceptedValue? = values.firstOrNull { it.field == field }
}

/** {"field":..,"revision":N} — options query request. */
@Serializable
data class SessionOptionsQuery(
    @SerialName("field") val field: String,
    @SerialName("revision") val revision: Long,
)

/** One selectable option: {"id":..,"attributes":{..}}. */
@Serializable
data class OptionValue(
    @SerialName("id") val id: String,
    @SerialName("attributes") val attributes: Map<String, String> = emptyMap(),
)

/** {"field":..,"values":[OptionValue...]} — options response. */
@Serializable
data class FieldOptions(
    @SerialName("field") val field: String,
    @SerialName("values") val values: List<OptionValue> = emptyList(),
)

/** One change in an update request: {"field":..,"valueId":..,"payload":..}. */
@Serializable
data class FieldChange(
    @SerialName("field") val field: String,
    @SerialName("valueId") val valueId: String,
    @SerialName("payload") val payload: JsonElement? = null,
)

/**
 * Update request: {"revision":N,"acceptRelatedChange":bool,"changes":[...]}.
 * The engine rejects an empty change set; callers never send one.
 */
@Serializable
data class SessionUpdateRequest(
    @SerialName("revision") val revision: Long,
    @SerialName("acceptRelatedChange") val acceptRelatedChange: Boolean = false,
    @SerialName("changes") val changes: List<FieldChange>,
)

/** {"revision":N} — sample-generation request. */
@Serializable
data class SampleRequest(
    @SerialName("revision") val revision: Long,
)

/** {"path":..,"startUs":..,"endUs":..,"sizeBytes":..} — sample result. */
@Serializable
data class SampleResult(
    @SerialName("path") val path: String,
    @SerialName("startUs") val startUs: Long,
    @SerialName("endUs") val endUs: Long,
    @SerialName("sizeBytes") val sizeBytes: Long,
)

/** {"revision":N,"submissionId":"u-..."} — session submit request. */
@Serializable
data class SubmitSessionRequest(
    @SerialName("revision") val revision: Long,
    @SerialName("submissionId") val submissionId: String,
)

// ---------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------

/** One produced output file: {"path":..,"sizeBytes":N,"kind":..}. */
@Serializable
data class JobOutput(
    @SerialName("path") val path: String,
    @SerialName("sizeBytes") val sizeBytes: Long,
    @SerialName("kind") val kind: String,
)

/** Job snapshot exactly as `jobSnapshotToJson` emits it. */
@Serializable
data class JobSnapshot(
    @SerialName("id") val id: String,
    @SerialName("submissionId") val submissionId: String? = null,
    @SerialName("operation") val operation: String,
    @SerialName("state") val state: String,
    /** 0..1, negative when unknown — never fabricated by the UI. */
    @SerialName("progress") val progress: Double = -1.0,
    @SerialName("sourceName") val sourceName: String = "",
    @SerialName("outputName") val outputName: String = "",
    @SerialName("outputPath") val outputPath: String = "",
    @SerialName("outputs") val outputs: List<JobOutput> = emptyList(),
    @SerialName("error") val error: String? = null,
    @SerialName("errorCode") val errorCode: String? = null,
    @SerialName("outputSizeBytes") val outputSizeBytes: Long = -1,
    @SerialName("inputSizeBytes") val inputSizeBytes: Long = -1,
    @SerialName("pipelineSummary") val pipelineSummary: String? = null,
    @SerialName("createdAtMs") val createdAtMs: Long = 0,
    @SerialName("finishedAtMs") val finishedAtMs: Long = 0,
)

/** Job event drained from `velmux_poll_events`. */
@Serializable
data class JobEvent(
    @SerialName("jobId") val jobId: String,
    @SerialName("state") val state: String,
    @SerialName("progress") val progress: Double = -1.0,
    @SerialName("outputSizeBytes") val outputSizeBytes: Long = -1,
    @SerialName("error") val error: String? = null,
    @SerialName("errorCode") val errorCode: String? = null,
)

/** {"jobId":"j-..."} — response of submit-session / submit-job. */
@Serializable
data class JobIdResult(
    @SerialName("jobId") val jobId: String,
)

// ---------------------------------------------------------------------------
// Diagnostics + platform integration
// ---------------------------------------------------------------------------

/**
 * Resource snapshot for `velmux_update_resources` — all fields optional
 * (absent entries are simply not reported to the scheduler).
 */
@Serializable
data class ResourceSnapshot(
    @SerialName("cpuCores") val cpuCores: Int? = null,
    @SerialName("memoryAvailableMb") val memoryAvailableMb: Int? = null,
    @SerialName("thermalPressure") val thermalPressure: Double? = null,
    @SerialName("onBattery") val onBattery: Boolean? = null,
    @SerialName("hardwareCodecAvailable") val hardwareCodecAvailable: Boolean? = null,
)

/** {"error":{"code":..,"message":..}} — the ABI error envelope. */
@Serializable
data class EngineError(
    @SerialName("code") val code: String,
    @SerialName("message") val message: String,
) {
    override fun toString(): String = "$code: $message"
}

/** Stable canonical error identifiers the client reacts to (types.h). */
object EngineErrorCode {
    const val STALE_REVISION = "stale-revision"
    const val RELATED_CHANGE_REQUIRED = "related-change-required"
    const val DUPLICATE_SUBMISSION = "duplicate-submission"
    const val INVALID_ARGUMENT = "invalid-argument"
    const val UNSUPPORTED_OPERATION = "unsupported-operation"

    /** Client-side code for a transport with no engine behind it. */
    const val ENGINE_UNAVAILABLE = "engine-unavailable"
}
