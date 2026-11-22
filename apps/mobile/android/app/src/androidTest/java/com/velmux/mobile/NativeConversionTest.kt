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

  // ---- Fixture generation (real encoders, real muxer) -----------------------

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
