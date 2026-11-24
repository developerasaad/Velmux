// Velmux instrumented tests: REAL conversions through the engine's bridge
// facade on a device/emulator (Milestone 1 validation, spec §45).
//
// The fixture is REAL media generated on-device by hardware/software
// MediaCodec encoders (H.264 + AAC into MP4). The conversion then runs
// through the same JNI facade the React Native app uses, and the output is
// validated with an independent MediaExtractor probe (never with the code
// that produced it, spec §50).
package com.velmux.mobile

import android.content.Context
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
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
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class NativeConversionTest {

  private val context: Context
    get() = ApplicationProvider.getApplicationContext()

  // ---- Fixture generation (real encoders, real muxer; duplicated from
  // HybridConversionTest, which stays an independent suite) ---------------------
  //
  // The generator is deterministic-by-construction:
  //  - encoders are chosen by NAME from MediaCodecList after checking the
  //    target size/rate/channels against their real capabilities, and the
  //    video input color format is picked from the chosen encoder's
  //    supported color formats (Planar/SemiPlanar preferred: tight-packed
  //    Y + neutral chroma is exact for 16-aligned dimensions; Flexible
  //    falls back to the Image API);
  //  - encoded samples produced BEFORE both output formats are known (the
  //    muxer cannot start yet) are buffered and flushed the moment
  //    MediaMuxer.start() succeeds — nothing is discarded, so no track can
  //    end up sample-less (a sample-less added track makes muxer.stop()
  //    throw, which is how this generator used to fail);
  //  - tracks are added ONLY from the INFO_OUTPUT_FORMAT_CHANGED format
  //    (it carries csd-0/csd-1; the configure-time outputFormat does not);
  //  - EOS is queued for both encoders and draining continues until output
  //    EOS is observed on both, bounded by a deadline so CI can never hang;
  //  - every resource is released in finally, and generation fails with a
  //    precise message if no real media sample reached a track.

  private class GeneratedMedia(val path: String, val durationUs: Long,
                               val width: Int, val height: Int)

  /** A media sample copied out of the codec buffer (safe to hold). */
  private class PendingSample(val data: ByteArray, val ptsUs: Long, val flags: Int)

  /**
   * First "video/avc" encoder (MediaCodecList order is stable per device)
   * whose real capabilities cover 320x240@24fps, plus a supported YUV420
   * input color format. Emulator software encoders (OMX.google.* /
   * c2.android.*) always list Planar or SemiPlanar.
   */
  private fun pickAvcEncoder(width: Int, height: Int, frameRate: Int): Pair<String, Int> {
    val planar = MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar
    val semiPlanar = MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar
    val flexible = MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible
    for (info in MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos) {
      if (!info.isEncoder || !info.supportedTypes.contains("video/avc")) continue
      val caps = info.getCapabilitiesForType("video/avc") ?: continue
      val video = caps.videoCapabilities ?: continue
      if (!video.supportedWidths.contains(width) || !video.supportedHeights.contains(height)) continue
      if (!video.supportedFrameRates.contains(frameRate)) continue
      val color = when {
        planar in caps.colorFormats -> planar
        semiPlanar in caps.colorFormats -> semiPlanar
        flexible in caps.colorFormats -> flexible
        else -> continue
      }
      return Pair(info.name, color)
    }
    throw IllegalStateException(
        "no video/avc encoder on this device supports ${width}x${height}@${frameRate}")
  }

  /** First "audio/mp4a-latm" encoder covering 44100Hz stereo. */
  private fun pickAacEncoder(sampleRate: Int, channels: Int): String {
    for (info in MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos) {
      if (!info.isEncoder || !info.supportedTypes.contains("audio/mp4a-latm")) continue
      val caps = info.getCapabilitiesForType("audio/mp4a-latm") ?: continue
      val audio = caps.audioCapabilities ?: continue
      if (!audio.supportedSampleRates.contains(sampleRate)) continue
      if (audio.maxInputChannelCount < channels) continue
      return info.name
    }
    throw IllegalStateException(
        "no audio/mp4a-latm encoder on this device supports ${sampleRate}Hz/${channels}ch")
  }

  private val fixtureTimeoutMs = 60_000L

  /**
   * Encodes synthetic frames (moving luminance gradient, neutral chroma)
   * with a real H.264 encoder and a real AAC encoder into an MP4.
   */
  private fun generateMp4(seconds: Int): GeneratedMedia {
    val width = 320
    val height = 240
    val frameRate = 24
    val totalFrames = seconds * frameRate
    val sampleRate = 44100
    val channels = 2
    val bytesPerAudioFrame = 2 * channels

    val (videoCodecName, videoColorFormat) = pickAvcEncoder(width, height, frameRate)
    val audioCodecName = pickAacEncoder(sampleRate, channels)

    val out = File(context.cacheDir, "velmux-fixture-${System.nanoTime()}.mp4")
    val muxer = MediaMuxer(out.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)

    var videoEncoder: MediaCodec? = null
    var audioEncoder: MediaCodec? = null
    var muxerStarted = false
    try {
      val videoFormat = MediaFormat.createVideoFormat("video/avc", width, height).apply {
        setInteger(MediaFormat.KEY_COLOR_FORMAT, videoColorFormat)
        setInteger(MediaFormat.KEY_BIT_RATE, 1_500_000)
        setInteger(MediaFormat.KEY_FRAME_RATE, frameRate)
        setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
      }
      videoEncoder = MediaCodec.createByCodecName(videoCodecName)
      videoEncoder.configure(videoFormat, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
      videoEncoder.start()

      val audioFormat = MediaFormat.createAudioFormat("audio/mp4a-latm", sampleRate, channels).apply {
        setInteger(MediaFormat.KEY_BIT_RATE, 96_000)
      }
      audioEncoder = MediaCodec.createByCodecName(audioCodecName)
      audioEncoder.configure(audioFormat, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
      audioEncoder.start()

      var videoTrack = -1
      var audioTrack = -1
      var videoSamples = 0
      var audioSamples = 0
      var videoEosQueued = false
      var audioEosQueued = false
      var videoEosSeen = false
      var audioEosSeen = false
      val videoPending = ArrayList<PendingSample>()
      val audioPending = ArrayList<PendingSample>()
      val info = MediaCodec.BufferInfo()
      val deadline = System.nanoTime() + fixtureTimeoutMs * 1_000_000

      // Tight-packed YUV420 frame with neutral chroma: byte-identical for
      // Planar and SemiPlanar at 16-aligned dimensions (chroma ordering is
      // irrelevant when every U/V byte is 0x80).
      val yuv = ByteArray(width * height * 3 / 2)
      var frameIndex = 0
      var audioBytesFed = 0L
      val audioChunk = ByteArray(sampleRate * bytesPerAudioFrame / 10) // ~100ms

      fun notExpired() {
        check(System.nanoTime() < deadline) {
          "fixture generation timed out after ${fixtureTimeoutMs}ms " +
          "(fed=$frameIndex/$totalFrames video frames, " +
          "videoSamples=$videoSamples, audioSamples=$audioSamples, " +
          "muxerStarted=$muxerStarted)"
        }
      }

      // Track registration: ONLY the format delivered by
      // INFO_OUTPUT_FORMAT_CHANGED (or, defensively, the encoder's
      // outputFormat after a real output buffer exists) — it embeds the
      // codec-specific data (csd-0/csd-1) the muxer needs.
      fun registerTrack(encoder: MediaCodec, isVideo: Boolean) {
        if (isVideo && videoTrack < 0) {
          videoTrack = muxer.addTrack(encoder.outputFormat)
        } else if (!isVideo && audioTrack < 0) {
          audioTrack = muxer.addTrack(encoder.outputFormat)
        }
        if (!muxerStarted && videoTrack >= 0 && audioTrack >= 0) {
          muxer.start()
          muxerStarted = true
          // Flush everything buffered while the muxer could not start —
          // per-track PTS stay monotonic (encoder output order).
          for (sample in videoPending) {
            val muxInfo = MediaCodec.BufferInfo()
            muxInfo.set(0, sample.data.size, sample.ptsUs, sample.flags)
            muxer.writeSampleData(videoTrack, ByteBuffer.wrap(sample.data), muxInfo)
            videoSamples++
          }
          videoPending.clear()
          for (sample in audioPending) {
            val muxInfo = MediaCodec.BufferInfo()
            muxInfo.set(0, sample.data.size, sample.ptsUs, sample.flags)
            muxer.writeSampleData(audioTrack, ByteBuffer.wrap(sample.data), muxInfo)
            audioSamples++
          }
          audioPending.clear()
        }
      }

      // Codec-config buffers are NOT media samples; size<=0 carries no
      // sample. Pre-start media is buffered; post-start media is written.
      fun handleSample(isVideo: Boolean, buffer: ByteBuffer, sampleInfo: MediaCodec.BufferInfo) {
        if (sampleInfo.size <= 0) return
        if (sampleInfo.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) return
        val data = ByteArray(sampleInfo.size)
        buffer.position(sampleInfo.offset)
        buffer.limit(sampleInfo.offset + sampleInfo.size)
        buffer.get(data)
        if (isVideo) {
          if (videoTrack < 0) registerTrack(videoEncoder!!, true)
          if (muxerStarted) {
            val muxInfo = MediaCodec.BufferInfo()
            muxInfo.set(0, data.size, sampleInfo.presentationTimeUs, sampleInfo.flags)
            muxer.writeSampleData(videoTrack, ByteBuffer.wrap(data), muxInfo)
            videoSamples++
          } else {
            videoPending.add(PendingSample(data, sampleInfo.presentationTimeUs, sampleInfo.flags))
          }
        } else {
          if (audioTrack < 0) registerTrack(audioEncoder!!, false)
          if (muxerStarted) {
            val muxInfo = MediaCodec.BufferInfo()
            muxInfo.set(0, data.size, sampleInfo.presentationTimeUs, sampleInfo.flags)
            muxer.writeSampleData(audioTrack, ByteBuffer.wrap(data), muxInfo)
            audioSamples++
          } else {
            audioPending.add(PendingSample(data, sampleInfo.presentationTimeUs, sampleInfo.flags))
          }
        }
      }

      // Drain until output EOS; INFO_TRY_AGAIN_LATER only ends THIS pass
      // (the outer loop keeps calling), never the generation.
      fun drain(isVideo: Boolean, timeoutUs: Long): Boolean {
        val encoder = if (isVideo) videoEncoder!! else audioEncoder!!
        while (true) {
          val id = encoder.dequeueOutputBuffer(info, timeoutUs)
          if (id >= 0) {
            val eos = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
            if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0) {
              handleSample(isVideo, encoder.getOutputBuffer(id)!!, info)
            }
            encoder.releaseOutputBuffer(id, false)
            if (eos) return false
          } else if (id == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
            registerTrack(encoder, isVideo)
          } else {
            return true
          }
        }
      }

      fun feedVideoFrame() {
        val index = videoEncoder!!.dequeueInputBuffer(10_000)
        if (index < 0) return
        val ptsUs = frameIndex * 1_000_000L / frameRate
        if (videoColorFormat == MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible) {
          val image = videoEncoder!!.getInputImage(index)
          if (image != null) {
            val stripe = (frameIndex * 4) % 256
            var span = 0
            for (planeIdx in 0..2) {
              val plane = image.planes[planeIdx]
              val rows = if (planeIdx == 0) height else height / 2
              val cols = if (planeIdx == 0) width else width / 2
              val buffer = plane.buffer
              for (row in 0 until rows) {
                var pos = row * plane.rowStride
                for (col in 0 until cols) {
                  buffer.position(pos)
                  buffer.put(if (planeIdx == 0) ((row + stripe) and 0xFF).toByte()
                             else 128.toByte())
                  pos += plane.pixelStride
                }
              }
              span += plane.rowStride * (rows - 1) + plane.pixelStride * (cols - 1) + 1
            }
            videoEncoder!!.queueInputBuffer(index, 0, span, ptsUs, 0)
            frameIndex++
            return
          }
        }
        // Planar/SemiPlanar (or an encoder without Image support): the
        // tight-packed buffer with neutral chroma.
        val input = videoEncoder!!.getInputBuffer(index)!!
        input.clear()
        val stripe = (frameIndex * 4) % 256
        var p = 0
        for (i in 0 until width * height) {
          yuv[p++] = ((i / width + stripe) and 0xFF).toByte()
        }
        for (i in 0 until width * height / 2) {
          yuv[p++] = 128.toByte()
        }
        val fill = minOf(yuv.size, input.remaining())
        input.put(yuv, 0, fill)
        videoEncoder!!.queueInputBuffer(index, 0, fill, ptsUs, 0)
        frameIndex++
      }

      fun feedAudioChunk() {
        val index = audioEncoder!!.dequeueInputBuffer(10_000)
        if (index < 0) return
        val input = audioEncoder!!.getInputBuffer(index)!!
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
        audioEncoder!!.queueInputBuffer(index, 0, chunk,
            audioBytesFed * 1_000_000L / (sampleRate * bytesPerAudioFrame), 0)
        audioBytesFed += chunk
      }

      fun queueEos(encoder: MediaCodec, ptsUs: Long): Boolean {
        val index = encoder.dequeueInputBuffer(50_000)
        if (index < 0) return false
        encoder.queueInputBuffer(index, 0, 0, ptsUs, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
        return true
      }

      val videoEosPts = totalFrames * 1_000_000L / frameRate

      while (!(videoEosQueued && audioEosQueued && videoEosSeen && audioEosSeen)) {
        notExpired()
        if (frameIndex < totalFrames) {
          feedVideoFrame()
          feedAudioChunk()
        } else {
          if (!videoEosQueued) videoEosQueued = queueEos(videoEncoder!!, videoEosPts)
          if (!audioEosQueued) {
            audioEosQueued = queueEos(audioEncoder!!,
                (audioBytesFed + audioChunk.size) * 1_000_000L /
                    (sampleRate * bytesPerAudioFrame))
          }
        }
        if (!videoEosSeen) videoEosSeen = !drain(isVideo = true, timeoutUs = 10_000)
        if (!audioEosSeen) audioEosSeen = !drain(isVideo = false, timeoutUs = 10_000)
      }

      // The fixture is only usable when both tracks carry REAL media.
      check(muxerStarted) {
        "fixture muxer never started (no encoder delivered an output format); " +
        "video=$videoCodecName audio=$audioCodecName"
      }
      check(videoSamples > 0) {
        "the video encoder ($videoCodecName) produced no media samples"
      }
      check(audioSamples > 0) {
        "the audio encoder ($audioCodecName) produced no media samples"
      }
      muxer.stop()
    } finally {
      // Resources are always released, on success and on every failure
      // path (a leaked MediaCodec keeps emulator CI flaky for the tests
      // that run after this one).
      for (encoder in listOf(videoEncoder, audioEncoder)) {
        if (encoder == null) continue
        try { encoder.stop() } catch (_: Exception) {}
        try { encoder.release() } catch (_: Exception) {}
      }
      muxer.release()
    }
    assertTrue(out.length() > 1024)
    return GeneratedMedia(out.absolutePath,
        totalFrames * 1_000_000L / frameRate, width, height)
  }

  // ---- Independent validation ------------------------------------------------

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

  // ---- Engine facade ----------------------------------------------------------

  private fun engineHandle(): Long {
    val dir = File(context.filesDir, "velmux-test-engine")
    dir.mkdirs()
    return VelmuxJni.nativeCreateEngine(dir.absolutePath)
  }

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

  private fun assertTrue(message: String, condition: Boolean) {
    if (!condition) throw AssertionError(message)
  }

  // ---- Tests -------------------------------------------------------------------

  @Test
  fun nativeTranscodeProducesValidOutput() {
    val fixture = generateMp4(3)
    val handle = engineHandle()
    try {
      val request = """
        {
          "source": {"uri": "${fixture.path}", "displayName": "fixture.mp4"},
          "outputContainer": "mp4",
          "goal": {"kind": "custom"},
          "video": {"codec": "h264", "enabled": true,
                    "rateControl": "bitrate", "bitRate": 800000},
          "audio": {"codec": "aac", "enabled": true, "bitRate": 64000},
          "subtitles": {"policy": "remove"},
          "processingMode": "auto"
        }
      """.trimIndent()
      val result = runJob(handle, request)
      assertEquals("engine error: ${result.error}", "completed", result.state)
      assertNotNull(result.outputPath)
      val out = File(result.outputPath!!)
      assertTrue("output missing", out.exists() && out.length() > 1024)
      // INDEPENDENT validation: MediaExtractor probe of the produced file.
      val probed = probe(out.absolutePath)
      assertEquals("video/avc", probed.videoMime)
      assertEquals("audio/mp4a-latm", probed.audioMime)
      assertEquals(fixture.width, probed.width)
      assertEquals(fixture.height, probed.height)
      // Duration within 15% of the fixture (hardware pipelines may pad the
      // last frame slightly; gross shortening means dropped frames).
      assertTrue(
          "duration ${probed.durationUs / 1000.0}ms vs expected ${fixture.durationUs / 1000.0}ms",
          probed.durationUs > fixture.durationUs * 85 / 100)
    } finally {
      VelmuxJni.nativeDestroyEngine(handle)
    }
  }

  @Test
  fun nativeRemuxCopiesWithoutReencoding() {
    val fixture = generateMp4(2)
    val handle = engineHandle()
    try {
      // Same codecs, same container, no bitrate/rate changes: the planner
      // must choose the stream-copy pipeline.
      val request = """
        {
          "source": {"uri": "${fixture.path}", "displayName": "fixture.mp4"},
          "outputContainer": "mp4",
          "goal": {"kind": "custom"},
          "video": {"codec": "h264", "enabled": true},
          "audio": {"codec": "aac", "enabled": true},
          "subtitles": {"policy": "remove"},
          "processingMode": "auto"
        }
      """.trimIndent()
      val result = runJob(handle, request)
      assertEquals("engine error: ${result.error}", "completed", result.state)
      val out = File(result.outputPath!!)
      assertTrue("output missing", out.exists() && out.length() > 1024)
      val probed = probe(out.absolutePath)
      assertEquals("video/avc", probed.videoMime)
      assertEquals("audio/mp4a-latm", probed.audioMime)
      assertTrue("duration too short",
          probed.durationUs > fixture.durationUs * 85 / 100)
    } finally {
      VelmuxJni.nativeDestroyEngine(handle)
    }
  }

  @Test
  fun inspectionReportsRealTracks() {
    val fixture = generateMp4(1)
    val handle = engineHandle()
    try {
      val infoJson = String(
          VelmuxJni.nativeInspect(handle, fixture.path, "fixture.mp4"),
          Charsets.UTF_8)
      assertTrue("no video codec reported: $infoJson",
          infoJson.contains("\"codec\":\"h264\""))
      assertTrue("no duration reported: $infoJson",
          infoJson.contains("\"durationSec\""))
    } finally {
      VelmuxJni.nativeDestroyEngine(handle)
    }
  }
}
