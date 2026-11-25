// Android MediaCodec pipeline implementation. See media_transcoder.h.
#include "media_transcoder.h"

#ifdef __ANDROID__

#include "media_util.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>

#include <android/native_window.h>
#include <jni.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace velmux::android::media {

using namespace velmux::core;

// ---------------------------------------------------------------------------
// JavaVM hook
// ---------------------------------------------------------------------------

namespace {
JavaVM *g_vm = nullptr;
std::mutex g_vmMutex;
} // namespace

void setJavaVm(void *vm) {
  std::lock_guard<std::mutex> lock(g_vmMutex);
  g_vm = reinterpret_cast<JavaVM *>(vm);
}

// ---------------------------------------------------------------------------
// Mime <-> Velmux codec mapping
// ---------------------------------------------------------------------------

namespace {

const char *videoMimeFor(VideoCodec codec) {
  switch (codec) {
  case VideoCodec::H264:
    return "video/avc";
  case VideoCodec::HEVC:
    return "video/hevc";
  case VideoCodec::VP8:
    return "video/x-vnd.on2.vp8";
  case VideoCodec::VP9:
    return "video/x-vnd.on2.vp9";
  case VideoCodec::AV1:
    return "video/av01";
  case VideoCodec::Mpeg4Part2:
    return "video/mp4v-es";
  case VideoCodec::H263:
    return "video/3gpp";
  case VideoCodec::Mpeg2:
    return "video/mpeg2";
  case VideoCodec::Mpeg1:
    return "video/mpeg";
  default:
    return nullptr; // no Android platform mapping for this Velmux codec
  }
}

const char *audioMimeFor(AudioCodec codec) {
  switch (codec) {
  case AudioCodec::Aac:
  case AudioCodec::HeAac:
    return "audio/mp4a-latm";
  case AudioCodec::Mp3:
    return "audio/mpeg";
  case AudioCodec::Opus:
    return "audio/opus";
  case AudioCodec::Vorbis:
    return "audio/vorbis";
  case AudioCodec::Flac:
    return "audio/flac";
  case AudioCodec::AmrNb:
    return "audio/3gpp";
  case AudioCodec::AmrWb:
    return "audio/amr-wb";
  case AudioCodec::Ac3:
    return "audio/ac3";
  case AudioCodec::Eac3:
    return "audio/eac3";
  case AudioCodec::Pcm:
    return "audio/raw";
  default:
    return nullptr;
  }
}

std::optional<VideoCodec> videoCodecFromMime(const std::string &mime) {
  if (mime == "video/avc") {
    return VideoCodec::H264;
  }
  if (mime == "video/hevc") {
    return VideoCodec::HEVC;
  }
  if (mime == "video/x-vnd.on2.vp8") {
    return VideoCodec::VP8;
  }
  if (mime == "video/x-vnd.on2.vp9") {
    return VideoCodec::VP9;
  }
  if (mime == "video/av01") {
    return VideoCodec::AV1;
  }
  if (mime == "video/mp4v-es") {
    return VideoCodec::Mpeg4Part2;
  }
  if (mime == "video/3gpp") {
    return VideoCodec::H263;
  }
  if (mime == "video/mpeg2") {
    return VideoCodec::Mpeg2;
  }
  if (mime == "video/mpeg") {
    return VideoCodec::Mpeg1;
  }
  return std::nullopt;
}

std::optional<AudioCodec> audioCodecFromMime(const std::string &mime) {
  if (mime == "audio/mp4a-latm") {
    return AudioCodec::Aac;
  }
  if (mime == "audio/mpeg") {
    return AudioCodec::Mp3;
  }
  if (mime == "audio/opus") {
    return AudioCodec::Opus;
  }
  if (mime == "audio/vorbis") {
    return AudioCodec::Vorbis;
  }
  if (mime == "audio/flac") {
    return AudioCodec::Flac;
  }
  if (mime == "audio/3gpp") {
    return AudioCodec::AmrNb;
  }
  if (mime == "audio/amr-wb") {
    return AudioCodec::AmrWb;
  }
  if (mime == "audio/ac3") {
    return AudioCodec::Ac3;
  }
  if (mime == "audio/eac3") {
    return AudioCodec::Eac3;
  }
  if (mime == "audio/raw") {
    return AudioCodec::Pcm;
  }
  return std::nullopt;
}

// Framework software codecs (c2.android.* / c2.soft.* / OMX.google.*);
// vendor codecs are reported as hardware-backed.
bool looksLikeSoftwareCodec(const std::string &name) {
  return name.rfind("c2.android.", 0) == 0 ||
         name.rfind("c2.soft.", 0) == 0 ||
         name.rfind("OMX.google.", 0) == 0;
}

Container containerForExtension(const std::string &path) {
  const auto dot = path.rfind('.');
  const std::string ext =
      dot == std::string::npos ? "" : path.substr(dot + 1);
  std::string e = ext;
  for (char &c : e) {
    c = char(::tolower(static_cast<unsigned char>(c)));
  }
  if (e == "mp4") {
    return Container::Mp4;
  }
  if (e == "m4v") {
    return Container::M4v;
  }
  if (e == "m4a") {
    return Container::M4a;
  }
  if (e == "mov") {
    return Container::Mov;
  }
  if (e == "mkv") {
    return Container::Mkv;
  }
  if (e == "webm") {
    return Container::WebM;
  }
  if (e == "ts" || e == "mts" || e == "m2ts") {
    return Container::MpegTs;
  }
  if (e == "ogg" || e == "oga" || e == "opus") {
    return Container::Ogg;
  }
  if (e == "mp3") {
    return Container::Mp3;
  }
  if (e == "flac") {
    return Container::Flac;
  }
  if (e == "wav") {
    return Container::Wav;
  }
  if (e == "3gp" || e == "3g2") {
    return Container::ThreeGp;
  }
  return Container::Mp4;
}

} // namespace

// ---------------------------------------------------------------------------
// JNI: MediaCodecList enumeration
// ---------------------------------------------------------------------------

namespace {

class ScopedJniEnv final {
public:
  ~ScopedJniEnv() {
    if (attached_) {
      g_vm->DetachCurrentThread();
    }
  }
  JNIEnv *get() {
    if (g_vm == nullptr) {
      return nullptr;
    }
    JNIEnv *env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) ==
        JNI_OK) {
      return env; // already attached (e.g. called from a Java thread)
    }
    if (g_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached_ = true;
      return env;
    }
    return nullptr;
  }

private:
  bool attached_ = false;
};

struct JniString {
  jstring ref_ = nullptr;
  JNIEnv *env_ = nullptr;
  std::string value;
  ~JniString() {
    if (ref_ != nullptr && env_ != nullptr) {
      env_->DeleteLocalRef(ref_);
    }
  }
  bool load(JNIEnv *env, jstring s) {
    if (env == nullptr || s == nullptr) {
      return false;
    }
    env_ = env;
    ref_ = s;
    const char *chars = env->GetStringUTFChars(s, nullptr);
    if (chars == nullptr) {
      return false;
    }
    value = chars;
    env->ReleaseStringUTFChars(s, chars);
    return true;
  }
};

} // namespace

bool enumerateSupport(NativeSupport &out) {
  std::lock_guard<std::mutex> lock(g_vmMutex);
  if (g_vm == nullptr) {
    return false;
  }
  ScopedJniEnv jni;
  JNIEnv *env = jni.get();
  if (env == nullptr) {
    return false;
  }

  struct Aggregate {
    CodecEntry entry;
    bool known = false;
  };
  std::unordered_map<std::string, Aggregate> videoByMime;
  std::unordered_map<std::string, Aggregate> audioByMime;

  const jclass listClass = env->FindClass("android/media/MediaCodecList");
  if (listClass == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }
  const jmethodID ctor = env->GetMethodID(listClass, "<init>", "(I)V");
  const jmethodID getCodecInfos = env->GetMethodID(
      listClass, "getCodecInfos", "()[Landroid/media/MediaCodecInfo;");
  if (ctor == nullptr || getCodecInfos == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(listClass);
    return false;
  }
  // MediaCodecList.REGULAR_CODECS == 0
  const jobject list = env->NewObject(listClass, ctor, 0);
  if (list == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(listClass);
    return false;
  }
  const jobjectArray infos =
      static_cast<jobjectArray>(env->CallObjectMethod(list, getCodecInfos));
  env->DeleteLocalRef(list);
  env->DeleteLocalRef(listClass);
  if (infos == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }

  const jclass infoClass = env->FindClass("android/media/MediaCodecInfo");
  const jmethodID isEncoder = env->GetMethodID(infoClass, "isEncoder", "()Z");
  const jmethodID isHardware =
      env->GetMethodID(infoClass, "isHardwareAccelerated", "()Z");
  const jmethodID getCanonicalName = env->GetMethodID(
      infoClass, "getCanonicalName", "()Ljava/lang/String;");
  const jmethodID getSupportedTypes = env->GetMethodID(
      infoClass, "getSupportedTypes", "()[Ljava/lang/String;");
  if (infoClass == nullptr || isEncoder == nullptr || isHardware == nullptr ||
      getCanonicalName == nullptr || getSupportedTypes == nullptr ||
      env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(infos);
    return false;
  }

  const jsize count = env->GetArrayLength(infos);
  int hardwareEncoders = 0;
  int totalCodecs = 0;
  for (jsize i = 0; i < count; ++i) {
    const jobject info = env->GetObjectArrayElement(infos, i);
    if (info == nullptr) {
      continue;
    }
    ++totalCodecs;
    const jboolean encoder = env->CallBooleanMethod(info, isEncoder);
    jboolean hardware = env->CallBooleanMethod(info, isHardware);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      hardware = JNI_FALSE;
    }
    JniString canonical;
    canonical.load(env, static_cast<jstring>(
                            env->CallObjectMethod(info, getCanonicalName)));
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    const bool software = looksLikeSoftwareCodec(canonical.value);
    if (encoder && hardware) {
      ++hardwareEncoders;
    }
    const jobjectArray types = static_cast<jobjectArray>(
        env->CallObjectMethod(info, getSupportedTypes));
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      env->DeleteLocalRef(info);
      continue;
    }
    if (types != nullptr) {
      const jsize typeCount = env->GetArrayLength(types);
      for (jsize t = 0; t < typeCount; ++t) {
        JniString mime;
        if (!mime.load(env, static_cast<jstring>(
                                env->GetObjectArrayElement(types, t)))) {
          env->ExceptionClear();
          continue;
        }
        const bool isVideo = mime.value.rfind("video/", 0) == 0;
        const bool isAudio = mime.value.rfind("audio/", 0) == 0;
        if (!isVideo && !isAudio) {
          continue;
        }
        Aggregate &agg = isVideo ? videoByMime[mime.value]
                                 : audioByMime[mime.value];
        agg.known = true;
        agg.entry.decode = agg.entry.decode || !encoder;
        agg.entry.encode = agg.entry.encode || bool(encoder);
        // Hardware is only a meaningful claim for video; audio codecs run
        // fine in software and the planner's cost model only prefers
        // hardware for video.
        if (isVideo) {
          agg.entry.hardware = agg.entry.hardware ||
                               (hardware || !software);
        }
      }
      env->DeleteLocalRef(types);
    }
    env->DeleteLocalRef(info);
  }
  env->DeleteLocalRef(infos);
  env->DeleteLocalRef(infoClass);

  for (const auto &[mime, agg] : videoByMime) {
    const auto codec = videoCodecFromMime(mime);
    if (codec.has_value() && agg.known) {
      out.video.push_back({*codec, agg.entry});
    }
  }
  for (const auto &[mime, agg] : audioByMime) {
    const auto codec = audioCodecFromMime(mime);
    if (codec.has_value() && agg.known) {
      out.audio.push_back({*codec, agg.entry});
    }
  }
  out.summary = "MediaCodec (" + std::to_string(totalCodecs) + " codecs, " +
                std::to_string(hardwareEncoders) + " hw encoders)";
  return true;
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

bool inspectFile(const std::string &path, MediaInfo &out,
                 std::string &error) {
  AMediaExtractor *extractor = AMediaExtractor_new();
  // Local sources go through the fd-based entry point: the path-based
  // one fails on API 29 with AMEDIA_ERROR_UNSUPPORTED ('can't create
  // http service' — see media_util.h).
  int extractorFd = -1;
  if (!setExtractorDataSourceFd(extractor, path, extractorFd)) {
    AMediaExtractor_delete(extractor);
    if (extractorFd >= 0) {
      ::close(extractorFd);
    }
    error = "cannot open the source";
    return false;
  }
  MediaInfo info;
  info.container = containerForExtension(path);
  struct stat st {};
  if (::stat(path.c_str(), &st) == 0) {
    info.sizeBytes = std::int64_t(st.st_size);
  }
  const size_t trackCount = AMediaExtractor_getTrackCount(extractor);
  for (size_t i = 0; i < trackCount; ++i) {
    AMediaFormat *format = AMediaExtractor_getTrackFormat(extractor, i);
    if (format == nullptr) {
      continue;
    }
    const char *mimeChars = nullptr;
    if (!AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mimeChars) ||
        mimeChars == nullptr) {
      AMediaFormat_delete(format);
      continue;
    }
    const std::string mime = mimeChars;
    int64_t durationUs = 0;
    AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &durationUs);
    info.durationSec =
        std::max(info.durationSec, double(durationUs) / 1000000.0);
    if (mime.rfind("video/", 0) == 0) {
      VideoStream stream;
      const auto codec = videoCodecFromMime(mime);
      if (!codec.has_value()) {
        AMediaFormat_delete(format);
        continue; // not a codec Velmux models; do not fabricate a stream
      }
      stream.codec = *codec;
      AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &stream.width);
      AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &stream.height);
      float frameRate = 0.0f;
      if (AMediaFormat_getFloat(format, "frame-rate", &frameRate)) {
        stream.frameRate = double(frameRate);
      } else {
        int32_t rate = 0;
        if (AMediaFormat_getInt32(format, "frame-rate", &rate)) {
          stream.frameRate = double(rate);
        }
      }
      int32_t bitrate = 0;
      if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_BIT_RATE,
                                &bitrate)) {
        stream.bitRate = bitrate;
      }
      int32_t rotation = 0;
      if (AMediaFormat_getInt32(format, "rotation-degrees", &rotation) &&
          (rotation == 90 || rotation == 270)) {
        std::swap(stream.width, stream.height);
      }
      info.video.push_back(stream);
    } else if (mime.rfind("audio/", 0) == 0) {
      AudioStream stream;
      const auto codec = audioCodecFromMime(mime);
      if (!codec.has_value()) {
        AMediaFormat_delete(format);
        continue;
      }
      stream.codec = *codec;
      AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE,
                            &stream.sampleRate);
      AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT,
                            &stream.channels);
      int32_t bitrate = 0;
      if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_BIT_RATE,
                                &bitrate)) {
        stream.bitRate = bitrate;
      }
      info.audio.push_back(stream);
    }
    AMediaFormat_delete(format);
  }
  AMediaExtractor_delete(extractor);
  if (extractorFd >= 0) {
    ::close(extractorFd);
  }
  if (!info.hasVideo() && !info.hasAudio()) {
    error = "no recognizable media track";
    return false;
  }
  out = info;
  return true;
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

namespace {

struct TrackSelection {
  int video = -1;
  int audio = -1;
  std::string videoMime;
  std::string audioMime;
  int64_t durationUs = 0;
  int32_t rotationDegrees = 0;
};

TrackSelection selectTracks(AMediaExtractor *extractor, bool wantVideo,
                            bool wantAudio) {
  TrackSelection selection;
  const size_t trackCount = AMediaExtractor_getTrackCount(extractor);
  for (size_t i = 0; i < trackCount; ++i) {
    AMediaFormat *format = AMediaExtractor_getTrackFormat(extractor, i);
    if (format == nullptr) {
      continue;
    }
    const char *mimeChars = nullptr;
    if (!AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mimeChars) ||
        mimeChars == nullptr) {
      AMediaFormat_delete(format);
      continue;
    }
    const std::string mime = mimeChars;
    if (wantVideo && selection.video < 0 && mime.rfind("video/", 0) == 0) {
      selection.video = int(i);
      selection.videoMime = mime;
      int64_t durationUs = 0;
      AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &durationUs);
      selection.durationUs = std::max(selection.durationUs, durationUs);
      AMediaFormat_getInt32(format, "rotation-degrees",
                            &selection.rotationDegrees);
      AMediaExtractor_selectTrack(extractor, i);
    } else if (wantAudio && selection.audio < 0 &&
               mime.rfind("audio/", 0) == 0) {
      selection.audio = int(i);
      selection.audioMime = mime;
      int64_t durationUs = 0;
      AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &durationUs);
      selection.durationUs = std::max(selection.durationUs, durationUs);
      AMediaExtractor_selectTrack(extractor, i);
    }
    AMediaFormat_delete(format);
  }
  return selection;
}

// Extractor sample buffer that grows to the largest sample seen.
class SampleBuffer final {
public:
  uint8_t *data() { return buffer_.data(); }
  size_t capacity() const { return buffer_.size(); }
  void ensure(size_t size) {
    if (size > buffer_.size()) {
      buffer_.resize(std::max(size, buffer_.size() * 2));
    }
  }

private:
  std::vector<uint8_t> buffer_;
};

bool cancelled(const RunContext &ctx) {
  return ctx.shouldContinue && !ctx.shouldContinue();
}

void reportProgress(const RunContext &ctx, double fraction) {
  if (ctx.onProgress) {
    ctx.onProgress(fraction);
  }
}

// Opens a writable file descriptor for AMediaMuxer (the NDK muxer is
// fd-based; the path-based constructor is Java-only). Returns -1 on
// failure.
static int openMuxerFd(const std::string &path) {
  return ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
}

} // namespace

// ---------------------------------------------------------------------------
// Stream copy (remux)
// ---------------------------------------------------------------------------

RunResult remuxFile(const std::string &inPath, const std::string &outPath,
                    bool wantVideo, bool wantAudio, const RunContext &ctx) {
  RunResult result;
  AMediaExtractor *extractor = AMediaExtractor_new();
  AMediaMuxer *muxer = nullptr;
  int extractorFd = -1;
  int muxerFd = -1;
  auto cleanup = [&] {
    if (extractor != nullptr) {
      AMediaExtractor_delete(extractor);
    }
    if (extractorFd >= 0) {
      ::close(extractorFd);
      extractorFd = -1;
    }
    if (muxer != nullptr) {
      AMediaMuxer_delete(muxer);
    }
    // The muxer keeps the fd for its lifetime; the caller closes it.
    if (muxerFd >= 0) {
      ::close(muxerFd);
      muxerFd = -1;
    }
  };
  // fd-based source open: the path-based call fails on API 29
  // ('can't create http service' — see media_util.h).
  if (!setExtractorDataSourceFd(extractor, inPath, extractorFd)) {
    result.error = "cannot open the source";
    cleanup();
    return result;
  }
  const TrackSelection selection =
      selectTracks(extractor, wantVideo, wantAudio);
  if (selection.video < 0 && selection.audio < 0) {
    result.error = "no copyable media track";
    cleanup();
    return result;
  }
  muxerFd = openMuxerFd(outPath);
  if (muxerFd < 0) {
    result.error = "cannot create the output file";
    cleanup();
    return result;
  }
  muxer = AMediaMuxer_new(muxerFd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
  if (muxer == nullptr) {
    result.error = "cannot create the output muxer";
    cleanup();
    return result;
  }
  AMediaMuxer_setOrientationHint(muxer, selection.rotationDegrees);

  struct MuxTrack {
    int source = -1;
    ssize_t mux = -1;
  };
  std::vector<MuxTrack> tracks;
  const auto addMuxTrack = [&](int sourceIdx) {
    AMediaFormat *format =
        AMediaExtractor_getTrackFormat(extractor, size_t(sourceIdx));
    if (format == nullptr) {
      return;
    }
    const ssize_t muxIdx = AMediaMuxer_addTrack(muxer, format);
    AMediaFormat_delete(format);
    if (muxIdx >= 0) {
      tracks.push_back({sourceIdx, muxIdx});
    }
  };
  if (selection.video >= 0) {
    addMuxTrack(selection.video);
  }
  if (selection.audio >= 0) {
    addMuxTrack(selection.audio);
  }
  if (tracks.empty()) {
    result.error = "no track could be added to the muxer";
    cleanup();
    return result;
  }
  if (AMediaMuxer_start(muxer) != AMEDIA_OK) {
    result.error = "cannot start the muxer";
    cleanup();
    return result;
  }

  SampleBuffer sample;
  sample.ensure(1 << 16);
  std::int64_t samplesWritten = 0;
  std::int64_t lastReportedUs = -1;
  for (;;) {
    if (cancelled(ctx)) {
      result.error = "canceled";
      cleanup();
      return result;
    }
    const int currentTrack = AMediaExtractor_getSampleTrackIndex(extractor);
    if (currentTrack < 0) {
      break; // end of stream
    }
    const ssize_t sampleSize = AMediaExtractor_getSampleSize(extractor);
    if (sampleSize <= 0) {
      AMediaExtractor_advance(extractor);
      continue;
    }
    sample.ensure(size_t(sampleSize));
    const ssize_t copied = AMediaExtractor_readSampleData(
        extractor, sample.data(), sample.capacity());
    if (copied != sampleSize) {
      continue; // capacity changed; re-read the same sample
    }
    const int64_t ptsUs = AMediaExtractor_getSampleTime(extractor);
    const uint32_t flags = AMediaExtractor_getSampleFlags(extractor);
    for (const MuxTrack &track : tracks) {
      if (track.source != currentTrack) {
        continue;
      }
      AMediaCodecBufferInfo info;
      info.offset = 0;
      info.size = sampleSize;
      info.presentationTimeUs = ptsUs;
      info.flags = (flags & AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC)
                       ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME
                       : 0;
      if (AMediaMuxer_writeSampleData(muxer, size_t(track.mux),
                                      sample.data(), &info) == AMEDIA_OK) {
        ++samplesWritten;
        result.bytesWritten += sampleSize;
      }
    }
    if (lastReportedUs != ptsUs && selection.durationUs > 0) {
      lastReportedUs = ptsUs;
      reportProgress(
          ctx, std::clamp(double(ptsUs) / double(selection.durationUs), 0.0,
                          1.0));
    }
    AMediaExtractor_advance(extractor);
  }
  AMediaMuxer_stop(muxer);
  cleanup();
  if (samplesWritten == 0) {
    result.error = "no samples were copied";
    return result;
  }
  result.ok = true;
  result.framesProcessed = samplesWritten;
  return result;
}

// ---------------------------------------------------------------------------
// Transcode
// ---------------------------------------------------------------------------

namespace {

// Encoded sample parked between a codec and the not-yet-started muxer.
struct QueuedSample {
  std::vector<uint8_t> data;
  AMediaCodecBufferInfo info{};
};

struct MuxTrackState {
  ssize_t muxIndex = -1;
  bool formatKnown = false;
  std::vector<QueuedSample> backlog;
};

} // namespace

RunResult transcodeFile(const std::string &inPath, const std::string &outPath,
                        const Pipeline &pipeline,
                        const ConversionRequest &request,
                        const RunContext &ctx) {
  RunResult result;
  const bool wantVideo = request.video.enabled;
  AMediaExtractor *extractor = AMediaExtractor_new();
  AMediaCodec *videoDecoder = nullptr;
  AMediaCodec *videoEncoder = nullptr;
  ANativeWindow *encoderSurface = nullptr;
  AMediaCodec *audioDecoder = nullptr;
  AMediaCodec *audioEncoder = nullptr;
  AMediaMuxer *muxer = nullptr;
  int extractorFd = -1;
  int muxerFd = -1;
  auto cleanup = [&] {
    if (extractor != nullptr) {
      AMediaExtractor_delete(extractor);
    }
    if (extractorFd >= 0) {
      ::close(extractorFd);
      extractorFd = -1;
    }
    if (videoDecoder != nullptr) {
      AMediaCodec_stop(videoDecoder);
      AMediaCodec_delete(videoDecoder);
    }
    if (videoEncoder != nullptr) {
      AMediaCodec_stop(videoEncoder);
      AMediaCodec_delete(videoEncoder);
    }
    if (encoderSurface != nullptr) {
      ANativeWindow_release(encoderSurface);
    }
    if (audioDecoder != nullptr) {
      AMediaCodec_stop(audioDecoder);
      AMediaCodec_delete(audioDecoder);
    }
    if (audioEncoder != nullptr) {
      AMediaCodec_stop(audioEncoder);
      AMediaCodec_delete(audioEncoder);
    }
    if (muxer != nullptr) {
      AMediaMuxer_delete(muxer);
    }
    // The muxer keeps the fd for its lifetime; the caller closes it.
    if (muxerFd >= 0) {
      ::close(muxerFd);
      muxerFd = -1;
    }
  };
  // fd-based source open: the path-based call fails on API 29
  // ('can't create http service' — see media_util.h).
  if (!setExtractorDataSourceFd(extractor, inPath, extractorFd)) {
    result.error = "cannot open the source";
    cleanup();
    return result;
  }
  const TrackSelection selection =
      selectTracks(extractor, wantVideo, request.audio.enabled);
  if (wantVideo && selection.video < 0) {
    result.error = "the source has no video track";
    cleanup();
    return result;
  }
  if (!wantVideo && request.audio.enabled && selection.audio < 0) {
    result.error = "the source has no audio track";
    cleanup();
    return result;
  }
  const bool videoActive = wantVideo && selection.video >= 0;
  const bool audioActive = request.audio.enabled && selection.audio >= 0;
  if (!videoActive && !audioActive) {
    result.error = "no track to process";
    cleanup();
    return result;
  }

  const OutputFormat muxerFormat =
      (pipeline.container == Container::WebM)
          ? AMEDIAMUXER_OUTPUT_FORMAT_WEBM
          : AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4;
  muxerFd = openMuxerFd(outPath);
  if (muxerFd < 0) {
    result.error = "cannot create the output file";
    cleanup();
    return result;
  }
  muxer = AMediaMuxer_new(muxerFd, muxerFormat);
  if (muxer == nullptr) {
    result.error = "cannot create the output muxer";
    cleanup();
    return result;
  }
  AMediaMuxer_setOrientationHint(muxer, selection.rotationDegrees);

  MuxTrackState videoTrack;
  MuxTrackState audioTrack;

  // ---- Video: decoder renders into the encoder input surface ---------------
  int32_t videoWidth = 0;
  int32_t videoHeight = 0;
  double videoFrameRate = 0.0;
  if (videoActive) {
    AMediaFormat *sourceVideoFormat =
        AMediaExtractor_getTrackFormat(extractor, size_t(selection.video));
    AMediaFormat_getInt32(sourceVideoFormat, AMEDIAFORMAT_KEY_WIDTH,
                          &videoWidth);
    AMediaFormat_getInt32(sourceVideoFormat, AMEDIAFORMAT_KEY_HEIGHT,
                          &videoHeight);
    float frameRate = 0.0f;
    if (AMediaFormat_getFloat(sourceVideoFormat, "frame-rate", &frameRate)) {
      videoFrameRate = double(frameRate);
    }
    if (videoFrameRate <= 0.0) {
      videoFrameRate = 30.0;
    }
    const char *outMime = videoMimeFor(pipeline.videoCodec);
    if (outMime == nullptr) {
      AMediaFormat_delete(sourceVideoFormat);
      result.error = "no Android encoder mime for the output video codec";
      cleanup();
      return result;
    }
    // Encoder first: its input surface is the decoder's output.
    AMediaFormat *encoderFormat = AMediaFormat_new();
    AMediaFormat_setString(encoderFormat, AMEDIAFORMAT_KEY_MIME, outMime);
    AMediaFormat_setInt32(encoderFormat, AMEDIAFORMAT_KEY_WIDTH, videoWidth);
    AMediaFormat_setInt32(encoderFormat, AMEDIAFORMAT_KEY_HEIGHT,
                          videoHeight);
    AMediaFormat_setInt32(encoderFormat, AMEDIAFORMAT_KEY_FRAME_RATE,
                          int32_t(videoFrameRate));
    std::int64_t bitRate = 0;
    if (request.video.rateControl == RateControl::Bitrate &&
        request.video.bitRate.has_value()) {
      bitRate = *request.video.bitRate;
    } else {
      // Quality-driven: bits-per-pixel heuristic in a sane AVC range.
      const double quality = request.video.quality.has_value()
                                 ? double(*request.video.quality) / 100.0
                                 : 0.6;
      bitRate = std::int64_t(double(videoWidth) * double(videoHeight) *
                             videoFrameRate * (0.04 + 0.12 * quality));
      bitRate = std::clamp<std::int64_t>(bitRate, 300000, 40000000);
    }
    AMediaFormat_setInt32(encoderFormat, AMEDIAFORMAT_KEY_BIT_RATE,
                          int32_t(std::min<std::int64_t>(
                              bitRate, std::numeric_limits<int32_t>::max())));
    AMediaFormat_setInt32(encoderFormat, "i-frame-interval", 1);
    videoEncoder = AMediaCodec_createEncoderByType(outMime);
    if (videoEncoder == nullptr ||
        AMediaCodec_configure(videoEncoder, encoderFormat, nullptr, nullptr,
                              AMEDIACODEC_CONFIGURE_FLAG_ENCODE) !=
            AMEDIA_OK ||
        AMediaCodec_createInputSurface(videoEncoder, &encoderSurface) !=
            AMEDIA_OK ||
        AMediaCodec_start(videoEncoder) != AMEDIA_OK) {
      AMediaFormat_delete(encoderFormat);
      AMediaFormat_delete(sourceVideoFormat);
      result.error = "cannot start the video encoder";
      cleanup();
      return result;
    }
    AMediaFormat_delete(encoderFormat);

    videoDecoder = AMediaCodec_createDecoderByType(
        selection.videoMime.c_str());
    if (videoDecoder == nullptr ||
        AMediaCodec_configure(videoDecoder, sourceVideoFormat,
                              encoderSurface, nullptr, 0) != AMEDIA_OK ||
        AMediaCodec_start(videoDecoder) != AMEDIA_OK) {
      AMediaFormat_delete(sourceVideoFormat);
      result.error = "cannot start the video decoder";
      cleanup();
      return result;
    }
    AMediaFormat_delete(sourceVideoFormat);
  }

  // ---- Audio: ByteBuffer decode -> encode ------------------------------------
  int32_t audioSampleRate = 0;
  int32_t audioChannels = 0;
  if (audioActive) {
    AMediaFormat *sourceAudioFormat =
        AMediaExtractor_getTrackFormat(extractor, size_t(selection.audio));
    AMediaFormat_getInt32(sourceAudioFormat, AMEDIAFORMAT_KEY_SAMPLE_RATE,
                          &audioSampleRate);
    AMediaFormat_getInt32(sourceAudioFormat, AMEDIAFORMAT_KEY_CHANNEL_COUNT,
                          &audioChannels);
    audioDecoder =
        AMediaCodec_createDecoderByType(selection.audioMime.c_str());
    if (audioDecoder == nullptr ||
        AMediaCodec_configure(audioDecoder, sourceAudioFormat, nullptr,
                              nullptr, 0) != AMEDIA_OK ||
        AMediaCodec_start(audioDecoder) != AMEDIA_OK) {
      AMediaFormat_delete(sourceAudioFormat);
      result.error = "cannot start the audio decoder";
      cleanup();
      return result;
    }
    AMediaFormat_delete(sourceAudioFormat);

    const char *outMime = audioMimeFor(pipeline.audioCodec);
    if (outMime == nullptr) {
      result.error = "no Android encoder mime for the output audio codec";
      cleanup();
      return result;
    }
    AMediaFormat *encoderFormat = AMediaFormat_new();
    AMediaFormat_setString(encoderFormat, AMEDIAFORMAT_KEY_MIME, outMime);
    AMediaFormat_setInt32(encoderFormat, AMEDIAFORMAT_KEY_SAMPLE_RATE,
                          audioSampleRate);
    AMediaFormat_setInt32(encoderFormat, AMEDIAFORMAT_KEY_CHANNEL_COUNT,
                          audioChannels);
    const std::int64_t audioBitRate =
        request.audio.bitRate.value_or(std::int64_t(audioChannels) * 64000);
    AMediaFormat_setInt32(encoderFormat, AMEDIAFORMAT_KEY_BIT_RATE,
                          int32_t(std::min<std::int64_t>(
                              audioBitRate,
                              std::numeric_limits<int32_t>::max())));
    audioEncoder = AMediaCodec_createEncoderByType(outMime);
    if (audioEncoder == nullptr ||
        AMediaCodec_configure(audioEncoder, encoderFormat, nullptr, nullptr,
                              AMEDIACODEC_CONFIGURE_FLAG_ENCODE) !=
            AMEDIA_OK ||
        AMediaCodec_start(audioEncoder) != AMEDIA_OK) {
      AMediaFormat_delete(encoderFormat);
      result.error = "cannot start the audio encoder";
      cleanup();
      return result;
    }
    AMediaFormat_delete(encoderFormat);
  }

  // ---- Drive everything until both codecs hit EOS ----------------------------
  SampleBuffer sample;
  sample.ensure(1 << 16);
  bool muxerStarted = false;
  bool videoInputEos = false;
  bool videoDone = !videoActive;
  bool audioInputEos = false;
  bool audioDone = !audioActive;
  std::int64_t audioSamplesFed = 0;
  const int bytesPerAudioFrame =
      2 * std::max(audioChannels, 1); // decoder PCM is 16-bit
  std::int64_t lastActivityCount = 0;
  auto lastActivityTime = std::chrono::steady_clock::now();

  const auto noteActivity = [&]() {
    const std::int64_t total =
        result.bytesWritten + result.framesProcessed + audioSamplesFed;
    if (total != lastActivityCount) {
      lastActivityCount = total;
      lastActivityTime = std::chrono::steady_clock::now();
    }
  };

  const auto drainVideoEncoder = [&]() -> bool {
    for (;;) {
      AMediaCodecBufferInfo info{};
      const ssize_t idx =
          AMediaCodec_dequeueOutputBuffer(videoEncoder, &info, 0);
      if (idx >= 0) {
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
          // Codec config rides in the track format, not the sample stream.
          AMediaCodec_releaseOutputBuffer(videoEncoder, size_t(idx), false);
          continue;
        }
        if (videoTrack.muxIndex < 0) {
          AMediaCodec_releaseOutputBuffer(videoEncoder, size_t(idx), false);
          continue;
        }
        size_t size = 0;
        uint8_t *data =
            AMediaCodec_getOutputBuffer(videoEncoder, size_t(idx), &size);
        if (data != nullptr && info.size > 0) {
          if (muxerStarted) {
            if (AMediaMuxer_writeSampleData(
                    muxer, size_t(videoTrack.muxIndex),
                    data + info.offset, &info) == AMEDIA_OK) {
              result.bytesWritten += info.size;
              ++result.framesProcessed;
            }
          } else {
            QueuedSample queued;
            queued.data.assign(data + info.offset,
                               data + info.offset + info.size);
            queued.info = info;
            queued.info.offset = 0;
            videoTrack.backlog.push_back(std::move(queued));
          }
        }
        const bool eos =
            (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        AMediaCodec_releaseOutputBuffer(videoEncoder, size_t(idx), false);
        if (eos) {
          videoDone = true;
          return true;
        }
        continue;
      }
      if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat *format = AMediaCodec_getOutputFormat(videoEncoder);
        if (format != nullptr && videoTrack.muxIndex < 0) {
          videoTrack.muxIndex = AMediaMuxer_addTrack(muxer, format);
          videoTrack.formatKnown = videoTrack.muxIndex >= 0;
        }
        continue;
      }
      return true; // try-again later / benign event
    }
  };

  const auto drainAudioEncoder = [&]() -> bool {
    for (;;) {
      AMediaCodecBufferInfo info{};
      const ssize_t idx =
          AMediaCodec_dequeueOutputBuffer(audioEncoder, &info, 0);
      if (idx >= 0) {
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
          AMediaCodec_releaseOutputBuffer(audioEncoder, size_t(idx), false);
          continue;
        }
        if (audioTrack.muxIndex < 0) {
          AMediaCodec_releaseOutputBuffer(audioEncoder, size_t(idx), false);
          continue;
        }
        size_t size = 0;
        uint8_t *data =
            AMediaCodec_getOutputBuffer(audioEncoder, size_t(idx), &size);
        if (data != nullptr && info.size > 0) {
          if (muxerStarted) {
            if (AMediaMuxer_writeSampleData(
                    muxer, size_t(audioTrack.muxIndex),
                    data + info.offset, &info) == AMEDIA_OK) {
              result.bytesWritten += info.size;
            }
          } else {
            QueuedSample queued;
            queued.data.assign(data + info.offset,
                               data + info.offset + info.size);
            queued.info = info;
            queued.info.offset = 0;
            audioTrack.backlog.push_back(std::move(queued));
          }
        }
        const bool eos =
            (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        AMediaCodec_releaseOutputBuffer(audioEncoder, size_t(idx), false);
        if (eos) {
          audioDone = true;
          return true;
        }
        continue;
      }
      if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat *format = AMediaCodec_getOutputFormat(audioEncoder);
        if (format != nullptr && audioTrack.muxIndex < 0) {
          audioTrack.muxIndex = AMediaMuxer_addTrack(muxer, format);
          audioTrack.formatKnown = audioTrack.muxIndex >= 0;
        }
        continue;
      }
      return true;
    }
  };

  const auto tryStartMuxer = [&]() -> bool {
    if (muxerStarted) {
      return true;
    }
    const bool videoReady = !videoActive || videoTrack.formatKnown;
    const bool audioReady = !audioActive || audioTrack.formatKnown;
    if (!videoReady || !audioReady) {
      return true;
    }
    if (videoTrack.muxIndex < 0 && audioTrack.muxIndex < 0) {
      return true; // keep driving; formats arrive with the first output
    }
    if (AMediaMuxer_start(muxer) != AMEDIA_OK) {
      result.error = "cannot start the muxer";
      return false;
    }
    muxerStarted = true;
    for (QueuedSample &queued : videoTrack.backlog) {
      if (AMediaMuxer_writeSampleData(muxer, size_t(videoTrack.muxIndex),
                                      queued.data.data(),
                                      &queued.info) == AMEDIA_OK) {
        result.bytesWritten += queued.info.size;
        ++result.framesProcessed;
      }
    }
    videoTrack.backlog.clear();
    videoTrack.backlog.shrink_to_fit();
    for (QueuedSample &queued : audioTrack.backlog) {
      if (AMediaMuxer_writeSampleData(muxer, size_t(audioTrack.muxIndex),
                                      queued.data.data(),
                                      &queued.info) == AMEDIA_OK) {
        result.bytesWritten += queued.info.size;
      }
    }
    audioTrack.backlog.clear();
    audioTrack.backlog.shrink_to_fit();
    return true;
  };

  // Feeds the current extractor sample to whichever codec owns its track.
  // One extractor, two consumers: the sample stays put until advance().
  const auto feedCurrentSample = [&]() -> bool {
    const int currentTrack = AMediaExtractor_getSampleTrackIndex(extractor);
    if (currentTrack < 0) {
      // End of stream for everything: signal both inputs.
      if (videoActive && !videoInputEos) {
        const ssize_t idx =
            AMediaCodec_dequeueInputBuffer(videoDecoder, 10000);
        if (idx >= 0) {
          AMediaCodec_queueInputBuffer(
              videoDecoder, size_t(idx), 0, 0, 0,
              AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
          videoInputEos = true;
        }
      }
      if (audioActive && !audioInputEos) {
        const ssize_t idx =
            AMediaCodec_dequeueInputBuffer(audioDecoder, 10000);
        if (idx >= 0) {
          AMediaCodec_queueInputBuffer(
              audioDecoder, size_t(idx), 0, 0, 0,
              AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
          audioInputEos = true;
        }
      }
      return true;
    }
    AMediaCodec *codec = nullptr;
    bool *inputEos = nullptr;
    int trackIdx = -1;
    if (videoActive && currentTrack == selection.video) {
      codec = videoDecoder;
      inputEos = &videoInputEos;
      trackIdx = selection.video;
    } else if (audioActive && currentTrack == selection.audio) {
      codec = audioDecoder;
      inputEos = &audioInputEos;
      trackIdx = selection.audio;
    } else {
      AMediaExtractor_advance(extractor); // unselected track: skip it
      return true;
    }
    (void)inputEos;
    const ssize_t idx = AMediaCodec_dequeueInputBuffer(codec, 0);
    if (idx < 0) {
      return true; // no input buffer available right now
    }
    size_t capacity = 0;
    uint8_t *dst = AMediaCodec_getInputBuffer(codec, size_t(idx), &capacity);
    if (dst == nullptr) {
      AMediaCodec_queueInputBuffer(codec, size_t(idx), 0, 0, 0, 0);
      return true;
    }
    const ssize_t sampleSize = AMediaExtractor_getSampleSize(extractor);
    if (sampleSize < 0 || size_t(sampleSize) > capacity) {
      // Defensive: oversized samples for this codec are skipped, never
      // silently truncated.
      AMediaCodec_queueInputBuffer(codec, size_t(idx), 0, 0, 0, 0);
      AMediaExtractor_advance(extractor);
      return true;
    }
    sample.ensure(size_t(sampleSize));
    const ssize_t copied = AMediaExtractor_readSampleData(
        extractor, sample.data(), sample.capacity());
    if (copied != sampleSize) {
      AMediaCodec_queueInputBuffer(codec, size_t(idx), 0, 0, 0, 0);
      return true; // retry after capacity growth
    }
    const int64_t ptsUs = AMediaExtractor_getSampleTime(extractor);
    ::memcpy(dst, sample.data(), size_t(sampleSize));
    AMediaCodec_queueInputBuffer(codec, size_t(idx), 0, size_t(sampleSize),
                                 uint64_t(ptsUs < 0 ? 0 : ptsUs), 0);
    if (selection.durationUs > 0 &&
        (videoActive || trackIdx == selection.audio)) {
      reportProgress(
          ctx,
          std::clamp(double(ptsUs < 0 ? 0 : ptsUs) /
                         double(selection.durationUs),
                     0.0, 1.0));
    }
    AMediaExtractor_advance(extractor);
    return true;
  };

  const auto drainVideoDecoder = [&]() -> bool {
    for (;;) {
      AMediaCodecBufferInfo info{};
      const ssize_t idx =
          AMediaCodec_dequeueOutputBuffer(videoDecoder, &info, 0);
      if (idx >= 0) {
        // Surface mode: render hands the frame to the encoder surface.
        AMediaCodec_releaseOutputBuffer(videoDecoder, size_t(idx), true);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
          if (AMediaCodec_signalEndOfInputStream(videoEncoder) != AMEDIA_OK) {
            result.error = "cannot signal the encoder end of stream";
            return false;
          }
          return true;
        }
        continue;
      }
      if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        continue;
      }
      return true;
    }
  };

  // Pushes decoded PCM into the encoder, blocking (bounded) until
  // accepted. PTS tracks CUMULATIVE samples across decoder buffers: the
  // encoder consumes one continuous PCM stream.
  const auto feedPcmToEncoder = [&](const uint8_t *data, size_t size) -> bool {
    size_t done = 0;
    while (done < size) {
      if (cancelled(ctx)) {
        result.error = "canceled";
        return false;
      }
      const ssize_t idx = AMediaCodec_dequeueInputBuffer(audioEncoder, 50000);
      if (idx < 0) {
        continue; // 50ms; the loop re-checks cancel
      }
      size_t capacity = 0;
      uint8_t *dst =
          AMediaCodec_getInputBuffer(audioEncoder, size_t(idx), &capacity);
      if (dst == nullptr) {
        AMediaCodec_queueInputBuffer(audioEncoder, size_t(idx), 0, 0, 0, 0);
        continue;
      }
      const size_t chunk = std::min(capacity, size - done);
      ::memcpy(dst, data + done, chunk);
      const std::int64_t totalSamples =
          audioSamplesFed +
          std::int64_t(done) / std::max(bytesPerAudioFrame, 1);
      const uint64_t pts = uint64_t(
          totalSamples * 1000000 / std::max<std::int64_t>(audioSampleRate, 1));
      AMediaCodec_queueInputBuffer(audioEncoder, size_t(idx), 0, chunk, pts,
                                   0);
      done += chunk;
    }
    audioSamplesFed += std::int64_t(size) / std::max(bytesPerAudioFrame, 1);
    return true;
  };

  const auto drainAudioDecoder = [&]() -> bool {
    for (;;) {
      AMediaCodecBufferInfo info{};
      const ssize_t idx =
          AMediaCodec_dequeueOutputBuffer(audioDecoder, &info, 0);
      if (idx >= 0) {
        size_t size = 0;
        uint8_t *data =
            AMediaCodec_getOutputBuffer(audioDecoder, size_t(idx), &size);
        if (data != nullptr && info.size > 0) {
          if (!feedPcmToEncoder(data + info.offset, size_t(info.size))) {
            AMediaCodec_releaseOutputBuffer(audioDecoder, size_t(idx),
                                            false);
            return false;
          }
        }
        const bool eos =
            (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        AMediaCodec_releaseOutputBuffer(audioDecoder, size_t(idx), false);
        if (eos) {
          // Flush the encoder with an empty end-of-stream input.
          for (;;) {
            if (cancelled(ctx)) {
              result.error = "canceled";
              return false;
            }
            const ssize_t inIdx =
                AMediaCodec_dequeueInputBuffer(audioEncoder, 50000);
            if (inIdx >= 0) {
              AMediaCodec_queueInputBuffer(
                  audioEncoder, size_t(inIdx), 0, 0, 0,
                  AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
              break;
            }
          }
          return true;
        }
        continue;
      }
      if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        continue;
      }
      return true;
    }
  };

  for (;;) {
    if (cancelled(ctx)) {
      result.error = "canceled";
      cleanup();
      return result;
    }
    if (!feedCurrentSample()) {
      cleanup();
      return result;
    }
    if (videoActive) {
      if (!drainVideoDecoder() || !drainVideoEncoder()) {
        cleanup();
        return result;
      }
    }
    if (audioActive) {
      if (!drainAudioDecoder() || !drainAudioEncoder()) {
        cleanup();
        return result;
      }
    }
    if (!tryStartMuxer()) {
      cleanup();
      return result;
    }
    if (videoDone && audioDone && muxerStarted) {
      break;
    }
    noteActivity();
    if (std::chrono::steady_clock::now() - lastActivityTime >
        std::chrono::seconds(120)) {
      result.error = "the native pipeline stalled";
      cleanup();
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (!muxerStarted) {
    result.error = "no encoded output was produced";
    cleanup();
    return result;
  }
  AMediaMuxer_stop(muxer);
  cleanup();
  result.ok = true;
  return result;
}

} // namespace velmux::android::media

#else // !__ANDROID__

// Host builds: no media pipeline; the backend reports itself unavailable.
namespace velmux::android::media {

void setJavaVm(void *) {}

bool enumerateSupport(NativeSupport &) { return false; }

bool inspectFile(const std::string &, core::MediaInfo &, std::string &error) {
  error = "Android media APIs are not available in this process";
  return false;
}

RunResult remuxFile(const std::string &, const std::string &, bool, bool,
                    const RunContext &) {
  RunResult result;
  result.error = "Android media APIs are not available in this process";
  return result;
}

RunResult transcodeFile(const std::string &, const std::string &,
                        const core::Pipeline &,
                        const core::ConversionRequest &,
                        const RunContext &) {
  RunResult result;
  result.error = "Android media APIs are not available in this process";
  return result;
}

} // namespace velmux::android::media

#endif // __ANDROID__
