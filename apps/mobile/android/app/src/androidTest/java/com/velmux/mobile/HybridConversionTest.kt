// Velmux instrumented tests: REAL hybrid (stage-composed) conversions on a
// device/emulator (Milestone 4 validation, spec §10/§31).
//
// Three execution categories are proven with REAL media:
//   - N->N: Android stage decode -> Android stage encode (the core
//     HybridExecutor composing MediaCodec stage workers on both sides);
//   - F->N: FFmpeg stage decode -> Android (MediaCodec) stage encode;
//   - N->F: Android stage decode -> FFmpeg (libx264) stage encode.
//
// Fixtures are REAL media generated on-device by hardware/software
// MediaCodec encoders (H.264 + AAC into MP4) — the proven generation
// helpers from NativeConversionTest are duplicated here (that class is a
// self-contained suite; coupling the two would couple their failures).
// Outputs are validated with an INDEPENDENT MediaExtractor probe, never
// with the code that produced them (spec §50).
//
// Honesty notes:
//   - The native test scaffolding (see the TEST SCAFFOLDING section in
//     velmux_jni.cpp) never fakes media work. It only SELECTS routes:
//     the forced-hybrid tests narrow the capability reports the PLANNER
//     reads so no single-backend route exists and the hybrid pipeline is
//     the only plan. Every frame still crosses the real CPU-frame
//     interchange between the real FFmpeg and Android stage workers.
//   - The "pipelineSummary contains Hybrid" assertions prove the PLANNER
//     chose a hybrid composition — not merely that a conversion succeeded.
//   - FFmpeg-dependent tests skip honestly (Assume) when the app was
//     built without the FFmpeg backend (CI runs without the pinned
//     artifacts) or when the emulator lacks the capability under test.
package com.velmux.mobile

import android.content.Context
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaMuxer
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import java.io.File
import java.nio.ByteBuffer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class HybridConversionTest {

  private val context: Context
    get() = ApplicationProvider.getApplicationContext()

  // ---- Fixture generation (real encoders, real muxer; duplicated from
  // NativeConversionTest, which stays an independent suite) ---------------------

  private class GeneratedMedia(val path: String, val durationUs: Long,
                               val width: Int, val height: Int)

  /**
   * Encodes synthetic frames (moving color gradient) with a real H.264
   * encoder and a real AAC encoder into an MP4.
   */
  private fun generateMp4(seconds: Int): GeneratedMedia {
    val width = 320
    val height = 240
    val frameRate = 24
    val totalFrames = seconds * frameRate
    val out = File(context.cacheDir, "velmux-fixture-${System.nanoTime()}.mp4")
    val muxer = MediaMuxer(out.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)

    val videoFormat = MediaFormat.createVideoFormat("video/avc", width, height).apply {
      setInteger(MediaFormat.KEY_COLOR_FORMAT,
          MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
      setInteger(MediaFormat.KEY_BIT_RATE, 1_500_000)
      setInteger(MediaFormat.KEY_FRAME_RATE, frameRate)
      setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
    }
    val videoEncoder = MediaCodec.createEncoderByType("video/avc")
    videoEncoder.configure(videoFormat, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
    videoEncoder.start()

    val sampleRate = 44100
    val channels = 2
    val audioFormat = MediaFormat.createAudioFormat("audio/mp4a-latm", sampleRate, channels).apply {
      setInteger(MediaFormat.KEY_BIT_RATE, 96_000)
    }
    val audioEncoder = MediaCodec.createEncoderByType("audio/mp4a-latm")
    audioEncoder.configure(audioFormat, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
    audioEncoder.start()

    var muxerStarted = false
    var videoTrack = -1
    var audioTrack = -1

    val yuv = ByteArray(width * height * 3 / 2)
    var frameIndex = 0
    val bytesPerAudioFrame = 2 * channels
    var audioBytesFed = 0L
    val audioChunk = ByteArray(sampleRate * bytesPerAudioFrame / 2) // ~500ms

    fun ensureMuxerStarted() {
      if (!muxerStarted && videoTrack >= 0 && audioTrack >= 0) {
        muxer.start()
        muxerStarted = true
      }
    }

    fun writeSample(track: Int, buffer: ByteBuffer, info: MediaCodec.BufferInfo) {
      if (track < 0 || info.size <= 0) {
        return
      }
      val data = ByteArray(info.size)
      buffer.position(info.offset)
      buffer.limit(info.offset + info.size)
      buffer.get(data)
      val muxInfo = MediaCodec.BufferInfo()
      muxInfo.set(0, info.size, info.presentationTimeUs, info.flags)
      muxer.writeSampleData(track, ByteBuffer.wrap(data), muxInfo)
    }

    fun drainVideo(): Boolean {
      val info = MediaCodec.BufferInfo()
      while (true) {
        val id = videoEncoder.dequeueOutputBuffer(info, 0)
        if (id >= 0) {
          val eos = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
          if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0) {
            if (!muxerStarted) {
              if (videoTrack < 0) {
                videoTrack = muxer.addTrack(videoEncoder.outputFormat)
                ensureMuxerStarted()
              }
            }
            if (muxerStarted) {
              writeSample(videoTrack, videoEncoder.getOutputBuffer(id)!!, info)
            }
          }
          videoEncoder.releaseOutputBuffer(id, false)
          if (eos) return false
        } else if (id == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
          if (videoTrack < 0) {
            videoTrack = muxer.addTrack(videoEncoder.outputFormat)
            ensureMuxerStarted()
          }
        } else {
          return true
        }
      }
    }

    fun drainAudio(): Boolean {
      val info = MediaCodec.BufferInfo()
      while (true) {
        val id = audioEncoder.dequeueOutputBuffer(info, 0)
        if (id >= 0) {
          val eos = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
          if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0) {
            if (!muxerStarted) {
              if (audioTrack < 0) {
                audioTrack = muxer.addTrack(audioEncoder.outputFormat)
                ensureMuxerStarted()
              }
            }
            if (muxerStarted) {
              writeSample(audioTrack, audioEncoder.getOutputBuffer(id)!!, info)
            }
          }
          audioEncoder.releaseOutputBuffer(id, false)
          if (eos) return false
        } else if (id == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
          if (audioTrack < 0) {
            audioTrack = muxer.addTrack(audioEncoder.outputFormat)
            ensureMuxerStarted()
          }
        } else {
          return true
        }
      }
    }

    fun feedVideoFrame() {
      val index = videoEncoder.dequeueInputBuffer(10_000)
      if (index < 0) {
        return
      }
      val input = videoEncoder.getInputBuffer(index)!!
      input.clear()
      val stripe = (frameIndex * 4) % 256
      var p = 0
      for (i in 0 until width * height) {
        yuv[p++] = ((i / width + stripe) and 0xFF).toByte()
      }
      for (i in 0 until width * height / 4) {
        yuv[p++] = 128.toByte()
      }
      for (i in 0 until width * height / 4) {
        yuv[p++] = 128.toByte()
      }
      input.put(yuv)
      val ptsUs = frameIndex * 1_000_000L / frameRate
      videoEncoder.queueInputBuffer(index, 0, yuv.size, ptsUs, 0)
      frameIndex++
    }

    fun feedAudioChunk() {
      val index = audioEncoder.dequeueInputBuffer(10_000)
      if (index < 0) {
        return
      }
      val input = audioEncoder.getInputBuffer(index)!!
      input.clear()
      for (i in audioChunk.indices step bytesPerAudioFrame) {
        val v = (Math.sin((audioBytesFed + i) / 32.0) * 12000).toInt()
        audioChunk[i] = (v and 0xFF).toByte()
        audioChunk[i + 1] = (v shr 8).toByte()
        audioChunk[i + 2] = (v and 0xFF).toByte()
        audioChunk[i + 3] = (v shr 8).toByte()
      }
      val chunk = minOf(audioChunk.size, input.remaining())
      input.put(audioChunk, 0, chunk)
      audioEncoder.queueInputBuffer(index, 0, chunk,
          audioBytesFed * 1_000_000L / (sampleRate * bytesPerAudioFrame), 0)
      audioBytesFed += chunk
    }

    while (frameIndex < totalFrames) {
      feedVideoFrame()
      feedAudioChunk()
      drainVideo()
      drainAudio()
    }
    // End of stream for both encoders, then drain to completion.
    while (true) {
      val index = videoEncoder.dequeueInputBuffer(50_000)
      if (index >= 0) {
        videoEncoder.queueInputBuffer(index, 0, 0,
            totalFrames * 1_000_000L / frameRate,
            MediaCodec.BUFFER_FLAG_END_OF_STREAM)
        break
      }
      drainVideo()
    }
    while (true) {
      val index = audioEncoder.dequeueInputBuffer(50_000)
      if (index >= 0) {
        audioEncoder.queueInputBuffer(index, 0, 0,
            audioBytesFed * 1_000_000L / (sampleRate * bytesPerAudioFrame),
            MediaCodec.BUFFER_FLAG_END_OF_STREAM)
        break
      }
      drainAudio()
    }
    var videoAlive = true
    var audioAlive = true
    while (videoAlive || audioAlive) {
      if (videoAlive) videoAlive = drainVideo()
      if (audioAlive) audioAlive = drainAudio()
    }
    videoEncoder.stop(); videoEncoder.release()
    audioEncoder.stop(); audioEncoder.release()
    muxer.stop(); muxer.release()
    assertTrue(out.length() > 1024)
    return GeneratedMedia(out.absolutePath,
        totalFrames * 1_000_000L / frameRate, width, height)
  }

  // ---- Independent validation (MediaExtractor, never the producer) -----------

  private class Probe(val videoMime: String?, val audioMime: String?,
                      val durationUs: Long, val width: Int, val height: Int)

  private fun probe(path: String): Probe {
    val extractor = MediaExtractor()
    extractor.setDataSource(path)
    var videoMime: String? = null
    var audioMime: String? = null
    var durationUs = 0L
    var width = 0
    var height = 0
    for (i in 0 until extractor.trackCount) {
      val format = extractor.getTrackFormat(i)
      val mime = format.getString(MediaFormat.KEY_MIME) ?: continue
      if (mime.startsWith("video/") && videoMime == null) {
        videoMime = mime
        if (format.containsKey(MediaFormat.KEY_WIDTH)) {
          width = format.getInteger(MediaFormat.KEY_WIDTH)
        }
        if (format.containsKey(MediaFormat.KEY_HEIGHT)) {
          height = format.getInteger(MediaFormat.KEY_HEIGHT)
        }
      } else if (mime.startsWith("audio/") && audioMime == null) {
        audioMime = mime
      }
      if (format.containsKey(MediaFormat.KEY_DURATION)) {
        durationUs = maxOf(durationUs, format.getLong(MediaFormat.KEY_DURATION))
      }
    }
    extractor.release()
    return Probe(videoMime, audioMime, durationUs, width, height)
  }

  // ---- Small JSON helpers (the engine's payloads; no parser dependency) ------

  private fun jsonStringField(json: String, field: String): String? {
    val key = "\"$field\":"
    val at = json.indexOf(key)
    if (at < 0) return null
    var pos = at + key.length
    while (pos < json.length && json[pos] == ' ') pos++
    return if (pos < json.length && json[pos] == '"') {
      val end = json.indexOf('"', pos + 1)
      json.substring(pos + 1, end)
    } else {
      val end = json.indexOfAny(charArrayOf(',', '}'), pos)
      json.substring(pos, if (end < 0) json.length else end)
    }
  }

  private fun jsonLongField(json: String, field: String): Long? {
    val key = "\"$field\":"
    val at = json.indexOf(key)
    if (at < 0) return null
    var pos = at + key.length
    while (pos < json.length && json[pos] == ' ') pos++
    val end = json.indexOfAny(charArrayOf(',', '}'), pos)
    val text = json.substring(pos, if (end < 0) json.length else end).trim()
    return text.toLongOrNull()
  }

  // ---- Engine facade (the NORMAL path the app uses) ---------------------------

  private fun engineHandle(): Long {
    val dir = File(context.filesDir, "velmux-hybrid-test-engine")
    dir.mkdirs()
    return VelmuxJni.nativeCreateEngine(dir.absolutePath)
  }

  private class JobResult(val state: String, val outputPath: String?, val error: String?)

  private fun runJob(handle: Long, requestJson: String): JobResult {
    val started = String(VelmuxJni.nativeStartJob(handle, requestJson), Charsets.UTF_8)
    assertTrue("startJob returned: $started", started.contains("\"jobId\""))
    val jobId = jsonStringField(started, "jobId")!!
    var lastState = "queued"
    var outputPath: String? = null
    var error: String? = null
    for (i in 0 until 600) {
      val snapshotJson = String(VelmuxJni.nativeGetJob(handle, jobId), Charsets.UTF_8)
      val state = jsonStringField(snapshotJson, "state") ?: continue
      lastState = state
      val pathField = jsonStringField(snapshotJson, "outputPath")
      if (pathField != null && pathField.isNotEmpty()) {
        outputPath = pathField
      }
      error = jsonStringField(snapshotJson, "error")?.takeIf { it.isNotEmpty() && it != "null" }
      if (state == "completed" || state == "failed" || state == "canceled") {
        break
      }
      Thread.sleep(100)
    }
    return JobResult(lastState, outputPath, error)
  }

  /** True when the engine (built like the app's) registered the FFmpeg backend. */
  private fun ffmpegBackendPresent(handle: Long): Boolean {
    val described = String(VelmuxJni.nativeDescribeBackends(handle), Charsets.UTF_8)
    return described.contains("FFmpeg")
  }

  /** Real platform probe: can a MediaCodec encoder be created for this mime? */
  private fun hasEncoderFor(mime: String): Boolean {
    return try {
      MediaCodec.createEncoderByType(mime).release()
      true
    } catch (e: Exception) {
      false
    }
  }

  // ---- Masked test engine (see nativeTestStartJobMaskedEngine) ----------------

  private class MaskedOutcome(val ok: Boolean, val state: String?,
                              val outputPath: String?, val error: String?,
                              val pipelineSummary: String?)

  private fun runMaskedEngine(requestJson: String, workDir: File,
                              mask: String): MaskedOutcome {
    val json = String(VelmuxJni.nativeTestStartJobMaskedEngine(
        requestJson, workDir.absolutePath, mask), Charsets.UTF_8)
    return MaskedOutcome(
        json.contains("\"ok\":true"),
        jsonStringField(json, "state"),
        jsonStringField(json, "outputPath")?.takeIf { it.isNotEmpty() && it != "null" },
        jsonStringField(json, "error")?.takeIf { it.isNotEmpty() && it != "null" },
        jsonStringField(json, "pipelineSummary")?.takeIf { it.isNotEmpty() && it != "null" })
  }

  private fun freshWorkDir(label: String): File {
    val dir = File(context.cacheDir, "velmux-hybrid-$label-${System.nanoTime()}")
    assertTrue("cannot create engine working dir ${dir.absolutePath}",
        dir.mkdirs() || dir.isDirectory)
    return dir
  }

  /** The PLANNER must have chosen a hybrid composition — not just any run. */
  private fun assertHybridChosen(outcome: MaskedOutcome, label: String) {
    val summary = outcome.pipelineSummary
    assertNotNull("no pipeline summary reported for $label " +
        "(state=${outcome.state}, error=${outcome.error})", summary)
    assertTrue("the planner did not choose a hybrid pipeline for $label: \"$summary\"",
        summary!!.contains("hybrid", ignoreCase = true))
  }

  private fun assertHybridRunCompleted(outcome: MaskedOutcome, label: String) {
    assertEquals("$label failed: ${outcome.error}", "completed", outcome.state)
    assertHybridChosen(outcome, label)
    assertNotNull("$label reported no output path", outcome.outputPath)
    val out = File(outcome.outputPath!!)
    assertTrue("$label output missing or empty", out.exists() && out.length() > 0)
  }

  private fun assertDurationWithin(probedUs: Long, expectedUs: Long, label: String) {
    assertTrue(
        "$label duration ${probedUs / 1000.0}ms vs expected ${expectedUs / 1000.0}ms",
        Math.abs(probedUs - expectedUs) <= 500_000)
  }

  // ---- Tests --------------------------------------------------------------------

  /**
   * N->N: the real core HybridExecutor composes the Android stage workers on
   * both sides — MediaExtractor/MediaCodec stage decode into CPU frames,
   * MediaCodec stage encode, AMediaMuxer mux with stream-copied AAC audio.
   */
  @Test
  fun testNToN_stageComposed_realWorkers() {
    val fixture = generateMp4(2)
    val out = File(context.cacheDir, "hybrid-nton-${System.nanoTime()}.mp4")
    val request = """
      {
        "source": {"uri": "${fixture.path}", "displayName": "fixture.mp4"},
        "outputContainer": "mp4",
        "goal": {"kind": "custom"},
        "video": {"codec": "h264", "enabled": true,
                  "rateControl": "bitrate", "bitRate": 800000},
        "audio": {"codec": "aac", "enabled": true},
        "subtitles": {"policy": "remove"},
        "processingMode": "auto"
      }
    """.trimIndent()

    val resultJson = String(
        VelmuxJni.nativeTestRunAndroidStagePipeline(request, out.absolutePath),
        Charsets.UTF_8)
    assertTrue("android stage pipeline returned: $resultJson",
        resultJson.contains("\"ok\":true"))
    val frames = jsonLongField(resultJson, "frames")
    assertTrue("no real frames processed: $resultJson", frames != null && frames > 0)

    // INDEPENDENT validation: MediaExtractor probe of the produced file.
    assertTrue("output missing: ${out.absolutePath}", out.exists() && out.length() > 0)
    val probed = probe(out.absolutePath)
    assertEquals("video/avc", probed.videoMime)
    assertEquals("audio/mp4a-latm", probed.audioMime)
    assertEquals(fixture.width, probed.width)
    assertEquals(fixture.height, probed.height)
    assertDurationWithin(probed.durationUs, fixture.durationUs, "N->N")
  }

  /**
   * F->N: an FFV1-in-MKV source has no single-backend route to HEVC —
   * Android's MediaCodec cannot decode FFV1 and the pinned FFmpeg build has
   * no HEVC encoder — so the planner's ONLY plan is the hybrid pipeline
   * (FFmpeg stage decode -> Android MediaCodec stage encode).
   *
   * Step 1 produces the FFV1 intermediate through the NORMAL facade
   * start-conversion path the app uses (FFmpeg is the only backend that
   * encodes FFV1). Step 2 requests FFV1 -> HEVC through an UNMASKED engine
   * and asserts the planner chose the hybrid route. On emulators without
   * an HEVC encoder the same F->N composition is forced on the original
   * fixture by masking (comments below); if even that route cannot be
   * established, the test skips honestly.
   */
  @Test
  fun testFToN_ffv1SourceForcesHybrid() {
    val fixture = generateMp4(3)
    val handle = engineHandle()
    try {
      // Without the FFmpeg backend no FFV1 intermediate can be produced and
      // nothing on the device can decode FFV1. Honest skip (the CI matrix
      // runs both with and without the pinned FFmpeg artifacts).
      Assume.assumeTrue(
          "the engine build has no FFmpeg backend (F->N hybrid needs it)",
          ffmpegBackendPresent(handle))

      // Step 1: fixture -> FFV1 in MKV through the normal engine path.
      val ffv1Request = """
        {
          "source": {"uri": "${fixture.path}", "displayName": "fixture.mp4"},
          "outputContainer": "mkv",
          "goal": {"kind": "custom"},
          "video": {"codec": "ffv1", "enabled": true},
          "audio": {"codec": "aac", "enabled": true},
          "subtitles": {"policy": "remove"},
          "processingMode": "auto"
        }
      """.trimIndent()
      val ffv1Job = runJob(handle, ffv1Request)
      assertEquals("FFV1 intermediate failed: ${ffv1Job.error}",
          "completed", ffv1Job.state)
      val ffv1Path = ffv1Job.outputPath!!
      val ffv1File = File(ffv1Path)
      assertTrue("FFV1 intermediate missing or empty: $ffv1Path",
          ffv1File.exists() && ffv1File.length() > 1024)
      // MediaExtractor cannot parse FFV1 (not a framework codec), so the
      // intermediate is validated by the engine's own completed state, its
      // size, and the success of the step-2 hybrid run that decodes it.

      // Step 2: FFV1 -> HEVC. No single-backend route exists, so hybrid
      // F->N is the ONLY plan — the engine needs no mask.
      if (hasEncoderFor("video/hevc")) {
        val hevcRequest = """
          {
            "source": {"uri": "$ffv1Path", "displayName": "intermediate.mkv"},
            "outputContainer": "mp4",
            "goal": {"kind": "custom"},
            "video": {"codec": "hevc", "enabled": true,
                      "rateControl": "bitrate", "bitRate": 800000},
            "audio": {"codec": "aac", "enabled": true},
            "subtitles": {"policy": "remove"},
            "processingMode": "auto"
          }
        """.trimIndent()
        val outcome = runMaskedEngine(hevcRequest, freshWorkDir("fton-natural"), "")
        assertHybridRunCompleted(outcome, "natural F->N (FFV1 -> HEVC)")
        val probed = probe(outcome.outputPath!!)
        assertEquals("video/hevc", probed.videoMime)
        assertEquals("audio/mp4a-latm", probed.audioMime)
        assertDurationWithin(probed.durationUs, fixture.durationUs, "F->N")
        return
      }

      // No HEVC encoder on this emulator. The same F->N composition
      // (FFmpeg decode stage -> Android encode stage) is forced on the
      // ORIGINAL fixture by masking. WHY THE MASK IS HONEST SCAFFOLDING:
      // "ffmpeg-encoders" hides FFmpeg's video encoders from its capability
      // report (no FFmpeg whole-pipeline transcode, no FFmpeg encode side)
      // and "android-whole-pipeline" removes the Android DECODE side of its
      // report (no Android whole-pipeline candidate; Android remains the
      // encode/mux stage side). The planner then has no single-backend
      // route and must compose the hybrid pipeline; the frames still cross
      // the real FFmpeg-decode -> Android-encode CPU-frame boundary.
      //
      // Audio is disabled: with identical in/out codecs, container and
      // audio, STREAM COPY would be the cheapest (and correct) plan —
      // disabling audio removes that route, making the request a genuine
      // transcode only the hybrid route can serve.
      val h264Request = """
        {
          "source": {"uri": "${fixture.path}", "displayName": "fixture.mp4"},
          "outputContainer": "mp4",
          "goal": {"kind": "custom"},
          "video": {"codec": "h264", "enabled": true,
                    "rateControl": "bitrate", "bitRate": 800000},
          "audio": {"enabled": false},
          "subtitles": {"policy": "remove"},
          "processingMode": "auto"
        }
      """.trimIndent()
      val outcome = runMaskedEngine(h264Request, freshWorkDir("fton-masked"),
          "ffmpeg-encoders,android-whole-pipeline")
      if (!outcome.ok && outcome.pipelineSummary
              ?.contains("hybrid", ignoreCase = true) != true) {
        // The forced route could not even be established on this image
        // (unexpected capability combination). Honest skip; a CHOSEN hybrid
        // that then failed execution stays a hard failure below.
        Assume.assumeTrue(
            "cannot establish the forced F->N hybrid route on this device: " +
            "${outcome.error} (state=${outcome.state})",
            false)
      }
      assertHybridRunCompleted(outcome, "masked F->N fallback (H.264 -> H.264)")
      val probed = probe(outcome.outputPath!!)
      assertEquals("video/avc", probed.videoMime)
      assertDurationWithin(probed.durationUs, fixture.durationUs, "masked F->N")
    } finally {
      VelmuxJni.nativeDestroyEngine(handle)
    }
  }

  /**
   * N->F: "ffmpeg-decoders" hides FFmpeg's video decoders (no FFmpeg
   * whole-pipeline transcode, no FFmpeg decode side) and
   * "android-whole-pipeline" removes the Android ENCODE side of its report
   * (no Android whole-pipeline candidate; Android remains the decode stage
   * side). The ONLY plan left is Android stage decode -> FFmpeg (libx264)
   * stage encode. Route selection only — all media work is real.
   */
  @Test
  fun testNToF_maskedDecodersForceHybrid() {
    val fixture = generateMp4(2)
    // Audio disabled for the same stream-copy reason as the F->N fallback
    // (identical in/out codecs+container would otherwise remux, which is
    // correct behavior but not the hybrid path under test).
    val request = """
      {
        "source": {"uri": "${fixture.path}", "displayName": "fixture.mp4"},
        "outputContainer": "mp4",
        "goal": {"kind": "custom"},
        "video": {"codec": "h264", "enabled": true,
                  "rateControl": "bitrate", "bitRate": 800000},
        "audio": {"enabled": false},
        "subtitles": {"policy": "remove"},
        "processingMode": "auto"
      }
    """.trimIndent()

    val outcome = runMaskedEngine(request, freshWorkDir("ntof"),
        "ffmpeg-decoders,android-whole-pipeline")
    if (outcome.error == "ffmpeg-absent") {
      // The app was built without the FFmpeg backend (CI without the pinned
      // artifacts): the cross-backend route cannot exist. Honest skip.
      Assume.assumeTrue(
          "the app was built without the FFmpeg backend (N->F hybrid needs it)",
          false)
    }
    assertHybridRunCompleted(outcome, "masked N->F (H.264 -> H.264)")
    val probed = probe(outcome.outputPath!!)
    assertEquals("video/avc", probed.videoMime)
    assertDurationWithin(probed.durationUs, fixture.durationUs, "N->F")
  }
}
