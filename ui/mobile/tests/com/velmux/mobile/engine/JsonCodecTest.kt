package com.velmux.mobile.engine

import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * JSON codec tests: decode sample payloads EXACTLY as
 * core/src/api/serialization.cpp emits them (handwritten fixtures from the
 * documented schema; field mapping verified) and verify request encoding
 * is byte-exact for the payloads the client sends.
 */
class JsonCodecTest {

    // ---- response decoding -----------------------------------------------------

    @Test
    fun decodesMediaInfoExactlyAsEmitted() {
        val raw = """
            {"durationSec":63.5,"container":"mp4","sizeBytes":1048576,
             "attachedPictures":0,
             "video":[{"codec":"h264","width":1920,"height":1080,
                       "frameRate":29.97,"bitRate":5000000,
                       "pixelFormat":"yuv420p","hdr":false,
                       "hasClosedCaptions":true,"language":"und"}],
             "audio":[{"codec":"aac","channels":2,"sampleRate":48000,
                       "bitRate":128000,"language":null}],
             "subtitles":[{"format":"mov_text","language":"eng",
                           "isDefault":true,"isForced":false}]}
        """.trimIndent().replace("\n", "").replace("  ", "")

        val media: MediaInfo = decodeEnginePayload(raw)

        assertEquals(63.5, media.durationSec)
        assertEquals("mp4", media.container)
        assertEquals(1_048_576, media.sizeBytes)
        assertEquals(0, media.attachedPictures)
        val video = media.video.single()
        assertEquals("h264", video.codec)
        assertEquals(1920, video.width)
        assertEquals(1080, video.height)
        assertEquals(29.97, video.frameRate)
        assertEquals(5_000_000, video.bitRate)
        assertEquals("yuv420p", video.pixelFormat)
        assertEquals(false, video.hdr)
        assertEquals(true, video.hasClosedCaptions)
        assertEquals("und", video.language)
        val audio = media.audio.single()
        assertEquals("aac", audio.codec)
        assertEquals(2, audio.channels)
        assertEquals(48_000, audio.sampleRate)
        assertEquals(128_000, audio.bitRate)
        assertNull(audio.language)
        val subtitle = media.subtitles.single()
        assertEquals("mov_text", subtitle.format)
        assertEquals("eng", subtitle.language)
        assertEquals(true, subtitle.isDefault)
        assertEquals(false, subtitle.isForced)
    }

    @Test
    fun decodesSessionSnapshotExactlyAsEmitted() {
        val raw = """
            {"sessionId":"s-1","operation":"video-convert","state":"ready",
             "revision":3,
             "sources":[{"durationSec":63.5,"container":"mp4","sizeBytes":1048576,
                         "attachedPictures":0,"video":[],"audio":[],"subtitles":[]}],
             "values":[
               {"field":"format","valueId":"mp4","origin":"default","payload":null},
               {"field":"codec","valueId":"h264","origin":"preset",
                "payload":{"hardware":false}}],
             "configurationValid":true,"validationMessage":null,
             "estimate":{"known":true,"estimatedBytes":900000,
                         "lowerBoundBytes":800000,"upperBoundBytes":1000000},
             "relatedChange":{"field":"codec",
                              "reason":"container does not support the codec",
                              "currentValueId":"hevc","proposedValueId":"h264"},
             "samplePath":null}
        """.trimIndent().replace("\n", "").replace("  ", "")

        val snapshot: SessionSnapshot = decodeEnginePayload(raw)

        assertEquals("s-1", snapshot.sessionId)
        assertEquals("video-convert", snapshot.operation)
        assertEquals("ready", snapshot.state)
        assertEquals(3, snapshot.revision)
        assertEquals("mp4", snapshot.sources.single().container)
        val formatValue = snapshot.values[0]
        assertEquals("format", formatValue.field)
        assertEquals("mp4", formatValue.valueId)
        assertEquals("default", formatValue.origin)
        assertNull(formatValue.payload)
        val codecValue = snapshot.values[1]
        assertEquals("preset", codecValue.origin)
        // The payload is the RAW JSON value, not a stringified document.
        assertEquals(
            buildJsonObject { put("hardware", false) },
            codecValue.payload,
        )
        assertEquals(true, snapshot.configurationValid)
        assertNull(snapshot.validationMessage)
        assertEquals(true, snapshot.estimate.known)
        assertEquals(900_000, snapshot.estimate.estimatedBytes)
        assertEquals(800_000, snapshot.estimate.lowerBoundBytes)
        assertEquals(1_000_000, snapshot.estimate.upperBoundBytes)
        val change = assertNotNull(snapshot.relatedChange)
        assertEquals("codec", change.field)
        assertEquals("hevc", change.currentValueId)
        assertEquals("h264", change.proposedValueId)
        assertNull(snapshot.samplePath)
    }

    @Test
    fun decodesSessionSnapshotWithoutRelatedChange() {
        val raw = """
            {"sessionId":"s-2","operation":"audio-convert","state":"created",
             "revision":0,"sources":[],"values":[],
             "configurationValid":false,"validationMessage":"select a codec",
             "estimate":{"known":false,"estimatedBytes":0,"lowerBoundBytes":0,
                         "upperBoundBytes":0},
             "relatedChange":null,
             "samplePath":"/tmp/sample.mp4"}
        """.trimIndent().replace("\n", "").replace("  ", "")

        val snapshot: SessionSnapshot = decodeEnginePayload(raw)

        assertNull(snapshot.relatedChange)
        assertEquals("select a codec", snapshot.validationMessage)
        assertEquals("/tmp/sample.mp4", snapshot.samplePath)
        assertEquals(false, snapshot.estimate.known)
    }

    @Test
    fun decodesFieldOptionsExactlyAsEmitted() {
        val raw = """
            {"field":"format","values":[
              {"id":"mp4","attributes":{"codecSupport":"h264,hevc",
                                        "estimatedBytes":"1200000"}},
              {"id":"mkv","attributes":{}}]}
        """.trimIndent().replace("\n", "").replace("  ", "")

        val options: FieldOptions = decodeEnginePayload(raw)

        assertEquals("format", options.field)
        val mp4 = options.values[0]
        assertEquals("mp4", mp4.id)
        assertEquals("h264,hevc", mp4.attributes["codecSupport"])
        assertEquals("1200000", mp4.attributes["estimatedBytes"])
        assertEquals(emptyMap(), options.values[1].attributes)
    }

    @Test
    fun decodesJobSnapshotExactlyAsEmitted() {
        val raw = """
            {"id":"j-1","submissionId":"u-abc","operation":"video-convert",
             "state":"completed","progress":1.0,
             "sourceName":"clip.mp4","outputName":"clip.mkv",
             "outputPath":"/outputs/clip.mkv",
             "outputs":[{"path":"/outputs/clip.mkv","sizeBytes":900000,
                         "kind":"media"}],
             "error":null,"errorCode":null,"outputSizeBytes":900000,
             "inputSizeBytes":1000000,
             "pipelineSummary":"decode h264 -> encode h264",
             "createdAtMs":1000,"finishedAtMs":2000}
        """.trimIndent().replace("\n", "").replace("  ", "")

        val job: JobSnapshot = decodeEnginePayload(raw)

        assertEquals("j-1", job.id)
        assertEquals("u-abc", job.submissionId)
        assertEquals("video-convert", job.operation)
        assertEquals("completed", job.state)
        assertEquals(1.0, job.progress)
        assertEquals("clip.mp4", job.sourceName)
        assertEquals("/outputs/clip.mkv", job.outputPath)
        val output = job.outputs.single()
        assertEquals(900_000, output.sizeBytes)
        assertEquals("media", output.kind)
        assertNull(job.error)
        assertNull(job.errorCode)
        assertEquals(900_000, job.outputSizeBytes)
        assertEquals(1_000_000, job.inputSizeBytes)
        assertEquals("decode h264 -> encode h264", job.pipelineSummary)
        assertEquals(1_000, job.createdAtMs)
        assertEquals(2_000, job.finishedAtMs)
    }

    @Test
    fun decodesJobEventExactlyAsEmitted() {
        // serialization.cpp emits outputSizeBytes on events (a superset of
        // the abi.h comment — reported, not a client-side problem).
        val raw = """
            {"jobId":"j-1","state":"running","progress":0.5,
             "outputSizeBytes":450000,"error":null,"errorCode":null}
        """.trimIndent().replace("\n", "").replace("  ", "")

        val event: JobEvent = decodeEnginePayload(raw)

        assertEquals("j-1", event.jobId)
        assertEquals("running", event.state)
        assertEquals(0.5, event.progress)
        assertEquals(450_000, event.outputSizeBytes)
        assertNull(event.error)
        assertNull(event.errorCode)
    }

    @Test
    fun decodesJobListAndEventArray() {
        val raw = """
            [{"jobId":"j-1","state":"queued","progress":-1.0,
              "outputSizeBytes":-1,"error":null,"errorCode":null},
             {"jobId":"j-2","state":"failed","progress":-1.0,
              "outputSizeBytes":-1,"error":"no encoder","errorCode":"encoder-unavailable"}]
        """.trimIndent().replace("\n", "").replace("  ", "")

        val events: List<JobEvent> = decodeEngineArray(raw)

        assertEquals(2, events.size)
        assertEquals("queued", events[0].state)
        assertEquals("encoder-unavailable", events[1].errorCode)
        assertEquals("no encoder", events[1].error)
    }

    @Test
    fun decodesSampleResult() {
        val raw = """{"path":"/tmp/sample.mp4","startUs":0,"endUs":1000000,"sizeBytes":12}"""
        val sample: SampleResult = decodeEnginePayload(raw)
        assertEquals("/tmp/sample.mp4", sample.path)
        assertEquals(0, sample.startUs)
        assertEquals(1_000_000, sample.endUs)
        assertEquals(12, sample.sizeBytes)
    }

    @Test
    fun decodesErrorEnvelopeToEngineException() {
        val raw = """{"error":{"code":"stale-revision","message":"configuration changed"}}"""
        val error = assertFailsWith<EngineException> { decodeEnginePayload<SessionSnapshot>(raw) }
        assertEquals("stale-revision", error.code)
        assertEquals("configuration changed", error.message)
        assertTrue(error.isStaleRevision())
    }

    @Test
    fun guardPathsThrowEngineExceptions() {
        // A non-object payload where an object is required:
        val objectGuard = assertFailsWith<EngineException> {
            VelmuxJson.parseObject("""[1]""")
        }
        assertEquals(EngineErrorCode.INVALID_ARGUMENT, objectGuard.code)
        // A non-array payload where an array is required:
        val arrayGuard = assertFailsWith<EngineException> {
            decodeEngineArray<JobEvent>("""{"jobId":"j-1"}""")
        }
        assertEquals(EngineErrorCode.INVALID_ARGUMENT, arrayGuard.code)
    }

    // ---- request encoding ------------------------------------------------------

    @Test
    fun encodesCreateSessionRequest() {
        val request = CreateSessionRequest(
            operation = "video-convert",
            sources = listOf(MediaSourceRequest("content://media/1", "clip.mp4")),
        )
        val json = VelmuxJson.json.encodeToString(request)
        assertEquals(
            """{"operation":"video-convert","sources":[{"uri":"content://media/1","name":"clip.mp4"}]}""",
            json,
        )
    }

    @Test
    fun encodesSessionUpdateRequest() {
        val request = SessionUpdateRequest(
            revision = 7,
            acceptRelatedChange = true,
            changes = listOf(
                FieldChange(field = "format", valueId = "mkv"),
                FieldChange(
                    field = "resolution",
                    valueId = "1920x1080",
                    payload = buildJsonObject { put("width", 1920) },
                ),
            ),
        )
        val json = VelmuxJson.json.encodeToString(request)
        assertEquals(
            """{"revision":7,"acceptRelatedChange":true,"changes":[""" +
                """{"field":"format","valueId":"mkv","payload":null},""" +
                """{"field":"resolution","valueId":"1920x1080","payload":{"width":1920}}]}""",
            json,
        )
    }

    @Test
    fun encodesOptionsQueryAndSubmitRequest() {
        assertEquals(
            """{"field":"format","revision":4}""",
            VelmuxJson.json.encodeToString(SessionOptionsQuery("format", 4)),
        )
        assertEquals(
            """{"revision":4,"submissionId":"u-1"}""",
            VelmuxJson.json.encodeToString(SubmitSessionRequest(4, "u-1")),
        )
    }

    @Test
    fun encodesResourceSnapshotWithOnlyPresentFields() {
        val snapshot = ResourceSnapshot(cpuCores = 8, onBattery = true)
        val json = VelmuxJson.json.encodeToString(snapshot)
        assertEquals(
            """{"cpuCores":8,"memoryAvailableMb":null,"thermalPressure":null,""" +
                """"onBattery":true,"hardwareCodecAvailable":null}""",
            json,
        )
        // Nulls are accepted by the ABI's optional-field readers.
        val decoded: ResourceSnapshot = decodeEnginePayload(json)
        assertEquals(8, decoded.cpuCores)
        assertEquals(true, decoded.onBattery)
    }

    @Test
    fun decodesSessionAndJobIdResults() {
        assertEquals(
            "s-9",
            decodeEnginePayload<SessionIdResult>("""{"sessionId":"s-9"}""").sessionId,
        )
        assertEquals(
            "j-9",
            decodeEnginePayload<JobIdResult>("""{"jobId":"j-9"}""").jobId,
        )
    }

    @Test
    fun decodeIsForwardCompatibleWithAdditiveEngineFields() {
        val raw = """
            {"jobId":"j-1","state":"running","progress":0.5,
             "outputSizeBytes":1,"error":null,"errorCode":null,
             "futureField":{"anything":JsonPrimitive(1)}}
        """.trimIndent().replace("\n", "").replace("  ", "")
        val event: JobEvent = decodeEnginePayload(raw)
        assertEquals("running", event.state)
    }
}
