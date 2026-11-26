// JNI adapter for the Velmux bridge C facade.
//
// Contract mirrors apps/mobile/android/.../VelmuxJni.kt (RegisterNatives):
//  - engine handles travel as jlong
//  - all JSON payloads cross the boundary as UTF-8 byte arrays (correct for
//    every Unicode range; String would need modified-UTF-8 care)
//  - errors are JSON objects, never null pointers
#include <jni.h>

#include <cstring>
#include <string>

// TEST SCAFFOLDING (hybrid instrumented tests): the test entry points at
// the bottom of this file reuse the bridge's C++ JSON helpers
// (requestFromJson) to parse the SAME request JSON the app facade parses.
// The macro must be set before the include (see abi.h); the C
// facade the app uses is unaffected.
#define VELMUX_BRIDGE_CXX_HELPERS 1
#include <velmux/abi.h>

// TEST SCAFFOLDING (hybrid instrumented tests): backend/engine headers for
// the test entry points at the bottom of this file. The FFmpeg header only
// reaches this translation unit when the backend target was actually built
// (velmux_abi links it PUBLIC), so __has_include is an exact
// build-presence probe — mirroring the bridge facade's
// VELMUX_WITH_FFMPEG_BACKEND gate without touching its PRIVATE definitions.
#include "android_backend.h"
#include "processing/contracts/contracts.h"
#include <velmux/engine.h>
#include "api/serialization.h"
#include "serialization/json.h"
#include "processing/hybrid_executor.h"

#if __has_include("processing/ffmpeg/ffmpeg_backend.h")
#include "processing/ffmpeg/ffmpeg_backend.h"
#define VELMUX_JNI_WITH_FFMPEG_BACKEND 1
#endif

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

// Android backend: the JavaVM registered here lets the backend query
// MediaCodecList at engine startup (capability reporting, spec §28).
extern "C" void velmux_android_set_java_vm(void *vm);

namespace {

VelmuxEngineHandle *asEngine(jlong handle) {
  return reinterpret_cast<VelmuxEngineHandle *>(static_cast<intptr_t>(handle));
}

void appendUtf8(std::string &out, unsigned cp) {
  if (cp < 0x80) {
    out.push_back(char(cp));
  } else if (cp < 0x800) {
    out.push_back(char(0xC0 | (cp >> 6)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(char(0xE0 | (cp >> 12)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(char(0xF0 | (cp >> 18)));
    out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  }
}

// UTF-16 (JNI jchar) -> UTF-8, with surrogate pair decoding. Kotlin/JS
// strings are UTF-16 under the hood; this keeps every code point intact.
std::string utf16ToUtf8(JNIEnv *env, jstring value) {
  std::string out;
  if (env == nullptr || value == nullptr) {
    return out;
  }
  const jsize length = env->GetStringLength(value);
  if (length <= 0) {
    return out;
  }
  const jchar *chars = env->GetStringChars(value, nullptr);
  if (chars == nullptr) {
    return out;
  }
  out.reserve(size_t(length));
  for (jsize i = 0; i < length; ++i) {
    unsigned cp = chars[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < length &&
        chars[i + 1] >= 0xDC00 && chars[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (chars[i + 1] - 0xDC00);
      ++i;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD; // lone surrogate
    }
    appendUtf8(out, cp);
  }
  env->ReleaseStringChars(value, chars);
  return out;
}

// Copies a facade string (malloc'd) into a Java byte array and frees it.
jbyteArray toJsonBytes(JNIEnv *env, char *facadeString) {
  if (facadeString == nullptr) {
    jbyteArray empty = env->NewByteArray(0);
    return empty != nullptr ? empty : env->NewByteArray(0);
  }
  const jsize length = static_cast<jsize>(std::strlen(facadeString));
  jbyteArray array = env->NewByteArray(length);
  if (array != nullptr && length > 0) {
    env->SetByteArrayRegion(array, 0, length,
                            reinterpret_cast<const jbyte *>(facadeString));
  }
  velmux_free_string(facadeString);
  return array;
}

jlong nativeCreateEngine(JNIEnv *env, jclass, jstring dataDir) {
  if (dataDir == nullptr) {
    return 0;
  }
  const std::string dir = utf16ToUtf8(env, dataDir);
  VelmuxEngineHandle *engine = velmux_engine_create(dir.c_str());
  return reinterpret_cast<intptr_t>(engine);
}

void nativeDestroyEngine(JNIEnv *, jclass, jlong handle) {
  velmux_engine_destroy(asEngine(handle));
}

jbyteArray nativeInspect(JNIEnv *env, jclass, jlong handle, jstring uri,
                         jstring displayName) {
  const std::string uriText = utf16ToUtf8(env, uri);
  const std::string nameText = utf16ToUtf8(env, displayName);
  return toJsonBytes(env,
                     velmux_inspect(asEngine(handle), uriText.c_str(),
                                    nameText.c_str()));
}

jint nativeAbiVersion(JNIEnv *, jclass) {
  return velmux_abi_version();
}

jbyteArray nativeEngineVersion(JNIEnv *env, jclass) {
  const jsize length = static_cast<jsize>(
      std::strlen(velmux_engine_version()));
  jbyteArray array = env->NewByteArray(length);
  if (array != nullptr && length > 0) {
    env->SetByteArrayRegion(
        array, 0, length,
        reinterpret_cast<const jbyte *>(velmux_engine_version()));
  }
  return array;
}

jbyteArray nativeCreateSession(JNIEnv *env, jclass, jlong handle,
                               jstring operationJson) {
  const std::string payload = utf16ToUtf8(env, operationJson);
  return toJsonBytes(
      env, velmux_create_session(asEngine(handle), payload.c_str()));
}

jbyteArray nativePrepareSession(JNIEnv *env, jclass, jlong handle,
                                jstring sessionId) {
  const std::string id = utf16ToUtf8(env, sessionId);
  return toJsonBytes(env, velmux_prepare_session(asEngine(handle),
                                                 id.c_str()));
}

jbyteArray nativeGetSessionSnapshot(JNIEnv *env, jclass, jlong handle,
                                    jstring sessionId) {
  const std::string id = utf16ToUtf8(env, sessionId);
  return toJsonBytes(env, velmux_get_session_snapshot(asEngine(handle),
                                                      id.c_str()));
}

jbyteArray nativeGetSessionOptions(JNIEnv *env, jclass, jlong handle,
                                   jstring sessionId, jstring fieldJson) {
  const std::string id = utf16ToUtf8(env, sessionId);
  const std::string field = utf16ToUtf8(env, fieldJson);
  return toJsonBytes(env, velmux_get_session_options(asEngine(handle),
                                                     id.c_str(),
                                                     field.c_str()));
}

jbyteArray nativeUpdateSession(JNIEnv *env, jclass, jlong handle,
                               jstring sessionId, jstring updateJson) {
  const std::string id = utf16ToUtf8(env, sessionId);
  const std::string update = utf16ToUtf8(env, updateJson);
  return toJsonBytes(env, velmux_update_session(asEngine(handle),
                                                id.c_str(),
                                                update.c_str()));
}

jbyteArray nativeGenerateSessionSample(JNIEnv *env, jclass, jlong handle,
                                       jstring sessionId,
                                       jstring sampleJson) {
  const std::string id = utf16ToUtf8(env, sessionId);
  const std::string sample = utf16ToUtf8(env, sampleJson);
  return toJsonBytes(env, velmux_generate_session_sample(asEngine(handle),
                                                         id.c_str(),
                                                         sample.c_str()));
}

jbyteArray nativeSubmitSession(JNIEnv *env, jclass, jlong handle,
                               jstring sessionId, jstring submitJson) {
  const std::string id = utf16ToUtf8(env, sessionId);
  const std::string submit = utf16ToUtf8(env, submitJson);
  return toJsonBytes(env, velmux_submit_session(asEngine(handle),
                                                id.c_str(),
                                                submit.c_str()));
}

jboolean nativeCloseSession(JNIEnv *env, jclass, jlong handle,
                            jstring sessionId) {
  const std::string id = utf16ToUtf8(env, sessionId);
  // 0 = closed, -1 = error (abi.h convention; unlike the job controls,
  // close has no "recorded but not completed" middle state).
  return velmux_close_session(asEngine(handle), id.c_str()) == 0 ? JNI_TRUE
                                                                : JNI_FALSE;
}

jbyteArray nativeSubmitJob(JNIEnv *env, jclass, jlong handle,
                           jstring requestJson) {
  const std::string request = utf16ToUtf8(env, requestJson);
  return toJsonBytes(env,
                     velmux_submit_job(asEngine(handle), request.c_str()));
}

jbyteArray nativeReuseJobSettings(JNIEnv *env, jclass, jlong handle,
                                  jstring jobId) {
  const std::string id = utf16ToUtf8(env, jobId);
  return toJsonBytes(env,
                     velmux_reuse_job_settings(asEngine(handle), id.c_str()));
}

jbyteArray nativeGetJob(JNIEnv *env, jclass, jlong handle, jstring jobId) {
  const std::string id = utf16ToUtf8(env, jobId);
  return toJsonBytes(env, velmux_get_job(asEngine(handle), id.c_str()));
}

jboolean nativeIsJobActive(JNIEnv *env, jclass, jlong handle, jstring jobId) {
  const std::string id = utf16ToUtf8(env, jobId);
  return velmux_is_job_active(asEngine(handle), id.c_str()) > 0 ? JNI_TRUE
                                                               : JNI_FALSE;
}

jbyteArray nativeListJobs(JNIEnv *env, jclass, jlong handle) {
  return toJsonBytes(env, velmux_list_jobs(asEngine(handle)));
}

jbyteArray nativeDescribeBackends(JNIEnv *env, jclass, jlong handle) {
  return toJsonBytes(env, velmux_describe_backends(asEngine(handle)));
}

jbyteArray nativePollEvents(JNIEnv *env, jclass, jlong handle) {
  return toJsonBytes(env, velmux_poll_events(asEngine(handle)));
}

jboolean nativeCancelJob(JNIEnv *env, jclass, jlong handle, jstring jobId) {
  const std::string id = utf16ToUtf8(env, jobId);
  return velmux_cancel_job(asEngine(handle), id.c_str()) > 0 ? JNI_TRUE
                                                             : JNI_FALSE;
}

jboolean nativePauseJob(JNIEnv *env, jclass, jlong handle, jstring jobId) {
  const std::string id = utf16ToUtf8(env, jobId);
  return velmux_pause_job(asEngine(handle), id.c_str()) > 0 ? JNI_TRUE
                                                            : JNI_FALSE;
}

jboolean nativeResumeJob(JNIEnv *env, jclass, jlong handle, jstring jobId) {
  const std::string id = utf16ToUtf8(env, jobId);
  return velmux_resume_job(asEngine(handle), id.c_str()) > 0 ? JNI_TRUE
                                                             : JNI_FALSE;
}

jboolean nativeRetryJob(JNIEnv *env, jclass, jlong handle, jstring jobId) {
  const std::string id = utf16ToUtf8(env, jobId);
  return velmux_retry_job(asEngine(handle), id.c_str()) > 0 ? JNI_TRUE
                                                            : JNI_FALSE;
}

jboolean nativeDeleteJob(JNIEnv *env, jclass, jlong handle, jstring jobId) {
  const std::string id = utf16ToUtf8(env, jobId);
  return velmux_delete_job(asEngine(handle), id.c_str()) > 0 ? JNI_TRUE
                                                             : JNI_FALSE;
}

jboolean nativeUpdateResources(JNIEnv *env, jclass, jlong handle,
                               jstring resourcesJson) {
  const std::string payload = utf16ToUtf8(env, resourcesJson);
  return velmux_update_resources(asEngine(handle), payload.c_str()) == 0
             ? JNI_TRUE
             : JNI_FALSE;
}

// ===========================================================================
// TEST SCAFFOLDING — hybrid (stage-composed) pipeline instrumented tests
// ===========================================================================
//
// The entry points below exist ONLY for the instrumented test suite
// (HybridConversionTest.kt); the app never calls them. They never fake,
// simulate or shortcut media work: every demux, decode, encode and mux
// still runs through the REAL backend implementations (backends/android,
// backends/ffmpeg) and the REAL core machinery (planner, JobManager,
// scheduler, HybridExecutor). What they do is SELECT routes:
//
//  - nativeTestRunAndroidStagePipeline builds the all-Android stage
//    assignment directly (the exact stage shape the planner's hybrid
//    candidate uses) and runs the real HybridExecutor over it, so the
//    Android stage workers execute even when whole-pipeline routes
//    would normally win.
//
//  - nativeTestStartJobMaskedEngine builds the engine stack exactly like
//    the app facade (bridge velmux_engine_create: backends ->
//    VelmuxEngine::create) but wraps the two real backends in
//    capability-narrowing wrappers (see the wrapper classes below). The
//    planner then has no single-backend route for the masked operation
//    and must compose the hybrid pipeline — which the returned pipeline
//    summary proves ("Hybrid: <decode> decode -> <encode> encode").

// std::string -> Java byte array (UTF-8, same convention as toJsonBytes).
jbyteArray jbyteArrayFromString(JNIEnv *env, const std::string &text) {
  const jsize length = static_cast<jsize>(text.size());
  jbyteArray array = env->NewByteArray(length);
  if (array != nullptr && length > 0) {
    env->SetByteArrayRegion(array, 0, length,
                            reinterpret_cast<const jbyte *>(text.data()));
  }
  return array;
}

std::string testQuoted(const std::string &s) {
  return "\"" + velmux::core::json::escape(s) + "\"";
}

std::string testErrorJson(const std::string &message) {
  return "{\"error\":" + testQuoted(message) + "}";
}

// Mask tokens understood by nativeTestStartJobMaskedEngine (comma-
// separated in the maskMode parameter).
struct TestMaskMode {
  bool ffmpegDecoders = false;     // hide FFmpeg's video decoders
  bool ffmpegEncoders = false;     // hide FFmpeg's video encoders
  bool androidWholePipeline = false; // Android opts out of whole pipelines
};

bool parseTestMask(const std::string &text, TestMaskMode &mask,
                   std::string &error) {
  error.clear();
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t comma = text.find(',', pos);
    std::string token = text.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos);
    const std::size_t first = token.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) {
      const std::size_t last = token.find_last_not_of(" \t\r\n");
      token = token.substr(first, last - first + 1);
      if (token == "ffmpeg-decoders") {
        mask.ffmpegDecoders = true;
      } else if (token == "ffmpeg-encoders") {
        mask.ffmpegEncoders = true;
      } else if (token == "android-whole-pipeline") {
        mask.androidWholePipeline = true;
      } else {
        error = "unknown mask token: " + token;
        return false;
      }
    }
    if (comma == std::string::npos) {
      break;
    }
    pos = comma + 1;
  }
  return true;
}

#if defined(VELMUX_JNI_WITH_FFMPEG_BACKEND)
// Wraps the REAL FFmpeg backend. "ffmpeg-decoders"/"ffmpeg-encoders" hide
// the video decoder/encoder entries of its capability report — the only
// inputs the planner routes on. Inspector, whole-pipeline executor and
// stage factories forward untouched: the real libav* code does all work.
class MaskedFfmpegBackend final : public velmux::backend::IBackend {
public:
  MaskedFfmpegBackend(bool hideVideoDecoders, bool hideVideoEncoders)
      : hideVideoDecoders_(hideVideoDecoders),
        hideVideoEncoders_(hideVideoEncoders) {}

  velmux::core::BackendId id() const override { return real_.id(); }
  std::string name() const override { return real_.name(); }
  velmux::core::Error initialize() override { return real_.initialize(); }

  velmux::core::BackendCapabilities capabilities() const override {
    velmux::core::BackendCapabilities caps = real_.capabilities();
    if (hideVideoDecoders_ || hideVideoEncoders_) {
      for (auto &entry : caps.video) {
        if (hideVideoDecoders_) {
          entry.support.decode = false;
        }
        if (hideVideoEncoders_) {
          entry.support.encode = false;
        }
      }
    }
    return caps;
  }

  velmux::backend::IInspector *inspector() override {
    return real_.inspector();
  }
  velmux::backend::IPipelineExecutor *executor() override {
    return real_.executor();
  }
  velmux::backend::IStageBackend *stages() override { return real_.stages(); }

private:
  velmux::ffmpeg::FfmpegBackend real_;
  bool hideVideoDecoders_;
  bool hideVideoEncoders_;
};
#endif // VELMUX_JNI_WITH_FFMPEG_BACKEND

// Whole-pipeline opt-out forwarder for the Android backend: answers
// canExecute() with false (the documented opt-out) but still forwards
// execute() to the REAL executor — a run that somehow reaches it executes
// the real MediaCodec path, never a fabricated result.
class WholePipelineOptOutExecutor final
    : public velmux::backend::IPipelineExecutor {
public:
  explicit WholePipelineOptOutExecutor(
      velmux::backend::IPipelineExecutor &real)
      : real_(real) {}

  bool canExecute(const velmux::core::Pipeline &,
                  const velmux::core::JobRequest &) const override {
    return false;
  }

  velmux::backend::ExecuteResult
  execute(const velmux::backend::ExecuteParams &params) override {
    return real_.execute(params);
  }

private:
  velmux::backend::IPipelineExecutor &real_;
};

// Wraps the REAL Android backend for the "android-whole-pipeline" mask.
// Two things are narrowed — both route selection, never media work:
//   1. executor() hands out the canExecute()=false forwarder above.
//   2. capabilities() removes the video codec side that would otherwise
//      keep a whole-pipeline route alive NEXT TO the ffmpeg mask: with
//      "ffmpeg-decoders" the ENCODE side goes (Android stays a decode
//      stage: android-decode -> ffmpeg-encode), with "ffmpeg-encoders"
//      the DECODE side goes (Android stays the encode/mux stage:
//      ffmpeg-decode -> android-encode), alone both go (full opt-out).
//      (2) is required because the planner routes on capability reports,
//      not on executor()->canExecute(): without it the Android
//      whole-pipeline candidate would keep winning and the hybrid path
//      would never run. Stage participation flags, readable containers,
//      audio capabilities, inspector and stage factories are untouched.
class MaskedAndroidBackend final : public velmux::backend::IBackend {
public:
  MaskedAndroidBackend(bool hideVideoEncoders, bool hideVideoDecoders,
                       bool optOutOfWholePipeline)
      : hideVideoEncoders_(hideVideoEncoders),
        hideVideoDecoders_(hideVideoDecoders) {
    if (optOutOfWholePipeline) {
      optOut_ = std::make_unique<WholePipelineOptOutExecutor>(
          *real_.executor());
    }
  }

  velmux::core::BackendId id() const override { return real_.id(); }
  std::string name() const override { return real_.name(); }
  velmux::core::Error initialize() override { return real_.initialize(); }

  velmux::core::BackendCapabilities capabilities() const override {
    velmux::core::BackendCapabilities caps = real_.capabilities();
    if (hideVideoEncoders_ || hideVideoDecoders_) {
      for (auto &entry : caps.video) {
        if (hideVideoEncoders_) {
          entry.support.encode = false;
        }
        if (hideVideoDecoders_) {
          entry.support.decode = false;
        }
      }
    }
    return caps;
  }

  velmux::backend::IInspector *inspector() override {
    return real_.inspector();
  }
  velmux::backend::IPipelineExecutor *executor() override {
    return optOut_ != nullptr ? optOut_.get() : real_.executor();
  }
  velmux::backend::IStageBackend *stages() override { return real_.stages(); }

private:
  velmux::android::AndroidBackend real_;
  bool hideVideoEncoders_;
  bool hideVideoDecoders_;
  std::unique_ptr<WholePipelineOptOutExecutor> optOut_;
};

// N->N: real Android stage decode -> real Android stage encode, composed
// by the REAL core HybridExecutor. The stage assignment mirrors the
// planner's hybrid candidate shape (decode side owns demux + decode +
// video processing; mux side owns encode + mux + bitstream; audio
// processing rides the mux side when the request keeps audio).
jbyteArray nativeTestRunAndroidStagePipeline(JNIEnv *env, jclass,
                                             jstring requestJson,
                                             jstring outputPath) {
  const std::string requestText = utf16ToUtf8(env, requestJson);
  const std::string outPath = utf16ToUtf8(env, outputPath);
  if (requestText.empty() || outPath.empty()) {
    return jbyteArrayFromString(
        env, testErrorJson("requestJson and outputPath must be non-empty"));
  }
  std::string parseError;
  const auto decoded =
      velmux::core::requestFromJson(requestText, parseError);
  if (!decoded.has_value()) {
    return jbyteArrayFromString(env, testErrorJson(parseError));
  }
  const velmux::core::JobRequest &request = decoded->request;
  const auto *convert = std::get_if<velmux::core::job::VideoConvert>(
      &request.payload);
  if (convert == nullptr) {
    return jbyteArrayFromString(
        env, testErrorJson("the hybrid test needs a video-convert request"));
  }

  velmux::android::AndroidBackend backend;
  const velmux::core::Error initError = backend.initialize();
  if (!initError.ok() || backend.stages() == nullptr) {
    // Honest unavailability (host processes without a JavaVM): no fake run.
    return jbyteArrayFromString(
        env, testErrorJson("the Android backend is unavailable in this "
                           "process: " +
                           initError.message));
  }

  // Source facts through the REAL inspector (never fabricated).
  velmux::core::MediaInfo info;
  const velmux::core::Error inspectError =
      backend.inspector()->inspect(convert->source, info);
  if (!inspectError.ok()) {
    return jbyteArrayFromString(env, testErrorJson(inspectError.message));
  }
  if (!info.hasVideo()) {
    return jbyteArrayFromString(
        env, testErrorJson("the source has no video track"));
  }

  // Audio stays when the request keeps it: empty rules preserve every
  // track (the core's preserve-all default), any include=true rule keeps
  // at least one.
  const bool requestKeepsAudio = convert->audioTracks.empty() ||
      std::any_of(convert->audioTracks.begin(), convert->audioTracks.end(),
                  [](const velmux::core::AudioTrackRule &rule) {
                    return rule.include;
                  });
  const bool wantsAudio = requestKeepsAudio && info.hasAudio();
  velmux::core::Pipeline pipeline;
  const velmux::core::BackendId android = velmux::core::BackendId::Android;
  pipeline.stages.push_back({velmux::core::StageId::Demux, android});
  pipeline.stages.push_back({velmux::core::StageId::Decode, android});
  pipeline.stages.push_back(
      {velmux::core::StageId::VideoProcessing, android});
  if (wantsAudio) {
    pipeline.stages.push_back(
        {velmux::core::StageId::AudioProcessing, android});
  }
  pipeline.stages.push_back({velmux::core::StageId::Encode, android});
  pipeline.stages.push_back({velmux::core::StageId::Mux, android});
  pipeline.stages.push_back({velmux::core::StageId::Bitstream, android});
  pipeline.videoCodec =
      convert->video.codec.value_or(info.video.front().codec);
  pipeline.audioCodec =
      wantsAudio
          ? convert->audioSettings.codec.value_or(info.audio.front().codec)
          : velmux::core::AudioCodec::Aac;
  pipeline.container = convert->output.container.value_or(info.container);
  pipeline.sourceFrameRate = info.video.front().frameRate;
  pipeline.videoStreamCopy = false;
  pipeline.audioStreamCopy = wantsAudio;
  pipeline.subtitleStreamCopy = false;
  pipeline.summary = std::string("Hybrid: android decode -> android encode "
                                 "-> ") +
                     velmux::core::containerName(pipeline.container);

  velmux::core::HybridExecutor hybrid({&backend});
  if (!hybrid.canExecute(pipeline, request)) {
    return jbyteArrayFromString(
        env,
        testErrorJson("the hybrid executor cannot serve this pipeline shape "
                      "(v1 hybrid: video transcode with source geometry and "
                      "rate, no trim/transform/audio DSP, stream-copied "
                      "audio)"));
  }
  velmux::backend::ExecuteParams params;
  params.pipeline = pipeline;
  params.request = request;
  params.outputPath = outPath;
  params.jobId = "android-stage-pipeline-test";
  params.cancel = std::make_shared<velmux::backend::CancelToken>();
  const velmux::backend::ExecuteResult result = hybrid.execute(params);
  if (!result.success) {
    return jbyteArrayFromString(
        env, testErrorJson(result.error.message.empty()
                               ? "the hybrid executor failed"
                               : result.error.message));
  }
  return jbyteArrayFromString(
      env, "{\"ok\":true,\"frames\":" +
               std::to_string(result.framesProcessed) + "}");
}

// Forced-hybrid job through the real engine stack (planner + JobManager +
// scheduler), with the two real backends wrapped per maskMode. outputPath
// is the engine's working directory (durable store; the job's output lands
// under <dir>/outputs). The actual output file is reported in the returned
// JSON together with state/error/pipeline summary.
jbyteArray nativeTestStartJobMaskedEngine(JNIEnv *env, jclass,
                                          jstring requestJson,
                                          jstring outputPath,
                                          jstring maskMode) {
  const std::string requestText = utf16ToUtf8(env, requestJson);
  const std::string dataDir = utf16ToUtf8(env, outputPath);
  const std::string maskText = utf16ToUtf8(env, maskMode);
  if (requestText.empty() || dataDir.empty()) {
    return jbyteArrayFromString(
        env, testErrorJson("requestJson and outputPath must be non-empty"));
  }
  TestMaskMode mask;
  std::string maskError;
  if (!parseTestMask(maskText, mask, maskError)) {
    return jbyteArrayFromString(env, testErrorJson(maskError));
  }
#if !defined(VELMUX_JNI_WITH_FFMPEG_BACKEND)
  // The app was built without the FFmpeg backend: every cross-backend
  // hybrid route this entry point exists to prove is impossible here.
  // Honest skip signal — the Kotlin tests treat it as Assume.skip.
  (void)mask;
  return jbyteArrayFromString(env, testErrorJson("ffmpeg-absent"));
#else
  std::string parseError;
  const auto decoded =
      velmux::core::requestFromJson(requestText, parseError);
  if (!decoded.has_value()) {
    return jbyteArrayFromString(env, testErrorJson(parseError));
  }
  const velmux::core::JobRequest request = decoded->request;

  // Engine stack exactly like the app facade (bridge velmux_engine_create):
  // FFmpeg first, Android second, VelmuxEngine::create wires the registries,
  // JobManager, scheduler, validator and recovery.
  const bool androidOptOut = mask.androidWholePipeline;
  std::vector<std::unique_ptr<velmux::backend::IBackend>> backends;
  backends.push_back(std::make_unique<MaskedFfmpegBackend>(
      mask.ffmpegDecoders, mask.ffmpegEncoders));
  backends.push_back(std::make_unique<MaskedAndroidBackend>(
      androidOptOut && mask.ffmpegDecoders, // Android keeps DECODE only
      androidOptOut && mask.ffmpegEncoders, // Android keeps ENCODE only
      androidOptOut));
  velmux::core::EngineConfig config;
  config.dataDir = dataDir;
  auto engine =
      velmux::core::VelmuxEngine::create(config, std::move(backends));
  if (engine == nullptr) {
    return jbyteArrayFromString(
        env, testErrorJson("cannot create the engine (is the working "
                           "directory writable?)"));
  }

  const auto jobId = engine->submitJob(request);
  if (!jobId.ok()) {
    return jbyteArrayFromString(env, testErrorJson(jobId.error().message));
  }

  // Wait like the app facade: poll the snapshot to a terminal state, then
  // let the worker unwind (isJobActive false) before reading the record.
  velmux::core::JobSnapshot snapshot;
  bool terminal = false;
  for (int i = 0; i < 1200; ++i) { // ~120s ceiling
    const auto current = engine->getJob(jobId.value());
    if (current.has_value()) {
      snapshot = *current;
      terminal = snapshot.state == velmux::core::JobState::Completed ||
                 snapshot.state == velmux::core::JobState::Failed ||
                 snapshot.state == velmux::core::JobState::Canceled;
      if (terminal) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!terminal) {
    return jbyteArrayFromString(
        env, testErrorJson("the job did not reach a terminal state in time"));
  }
  while (engine->isJobActive(jobId.value())) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (const auto settled = engine->getJob(jobId.value());
      settled.has_value()) {
    snapshot = *settled;
  }

  std::string out = "{\"ok\":";
  out += snapshot.state == velmux::core::JobState::Completed ? "true" : "false";
  out += ",\"state\":\"";
  out += velmux::core::jobStateName(snapshot.state);
  out += "\",\"outputPath\":";
  out += testQuoted(snapshot.outputPath);
  out += ",\"error\":";
  out += snapshot.error.empty() ? "null" : testQuoted(snapshot.error);
  out += ",\"pipelineSummary\":";
  out += snapshot.pipelineSummary.empty()
            ? "null"
            : testQuoted(snapshot.pipelineSummary);
  out += ",\"outputSizeBytes\":";
  out += std::to_string(snapshot.outputSizeBytes);
  out += "}";
  return jbyteArrayFromString(env, out);
#endif // VELMUX_JNI_WITH_FFMPEG_BACKEND
}

const JNINativeMethod kVelmuxJniMethods[] = {
    {"nativeCreateEngine", "(Ljava/lang/String;)J",
     reinterpret_cast<void *>(nativeCreateEngine)},
    {"nativeDestroyEngine", "(J)V",
     reinterpret_cast<void *>(nativeDestroyEngine)},
    {"nativeInspect", "(JLjava/lang/String;Ljava/lang/String;)[B",
     reinterpret_cast<void *>(nativeInspect)},
    {"nativeAbiVersion", "()I", reinterpret_cast<void *>(nativeAbiVersion)},
    {"nativeEngineVersion", "()[B",
     reinterpret_cast<void *>(nativeEngineVersion)},
    {"nativeCreateSession", "(JLjava/lang/String;)[B",
     reinterpret_cast<void *>(nativeCreateSession)},
    {"nativePrepareSession", "(JLjava/lang/String;)[B",
     reinterpret_cast<void *>(nativePrepareSession)},
    {"nativeGetSessionSnapshot", "(JLjava/lang/String;)[B",
     reinterpret_cast<void *>(nativeGetSessionSnapshot)},
    {"nativeGetSessionOptions", "(JLjava/lang/String;Ljava/lang/String;)[B",
     reinterpret_cast<void *>(nativeGetSessionOptions)},
    {"nativeUpdateSession", "(JLjava/lang/String;Ljava/lang/String;)[B",
     reinterpret_cast<void *>(nativeUpdateSession)},
    {"nativeGenerateSessionSample", "(JLjava/lang/String;Ljava/lang/String;)[B",
     reinterpret_cast<void *>(nativeGenerateSessionSample)},
    {"nativeSubmitSession", "(JLjava/lang/String;Ljava/lang/String;)[B",
     reinterpret_cast<void *>(nativeSubmitSession)},
    {"nativeCloseSession", "(JLjava/lang/String;)Z",
     reinterpret_cast<void *>(nativeCloseSession)},
    {"nativeSubmitJob", "(JLjava/lang/String;)[B",
     reinterpret_cast<void *>(nativeSubmitJob)},
    {"nativeReuseJobSettings", "(JLjava/lang/String;)[B",
     reinterpret_cast<void *>(nativeReuseJobSettings)},
    {"nativeGetJob", "(JLjava/lang/String;)[B",
     reinterpret_cast<void *>(nativeGetJob)},
    {"nativeIsJobActive", "(JLjava/lang/String;)Z",
     reinterpret_cast<void *>(nativeIsJobActive)},
    {"nativeListJobs", "(J)[B", reinterpret_cast<void *>(nativeListJobs)},
    {"nativeDescribeBackends", "(J)[B",
     reinterpret_cast<void *>(nativeDescribeBackends)},
    {"nativePollEvents", "(J)[B",
     reinterpret_cast<void *>(nativePollEvents)},
    {"nativeCancelJob", "(JLjava/lang/String;)Z",
     reinterpret_cast<void *>(nativeCancelJob)},
    {"nativePauseJob", "(JLjava/lang/String;)Z",
     reinterpret_cast<void *>(nativePauseJob)},
    {"nativeResumeJob", "(JLjava/lang/String;)Z",
     reinterpret_cast<void *>(nativeResumeJob)},
    {"nativeRetryJob", "(JLjava/lang/String;)Z",
     reinterpret_cast<void *>(nativeRetryJob)},
    {"nativeDeleteJob", "(JLjava/lang/String;)Z",
     reinterpret_cast<void *>(nativeDeleteJob)},
    {"nativeUpdateResources", "(JLjava/lang/String;)Z",
     reinterpret_cast<void *>(nativeUpdateResources)},
    // TEST SCAFFOLDING (hybrid instrumented tests; see the section above).
    {"nativeTestRunAndroidStagePipeline", "(Ljava/lang/String;Ljava/lang/String;)[B",
     reinterpret_cast<void *>(nativeTestRunAndroidStagePipeline)},
    {"nativeTestStartJobMaskedEngine",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[B",
     reinterpret_cast<void *>(nativeTestStartJobMaskedEngine)},
};

} // namespace

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
  JNIEnv *env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }
  // MediaCodecList enumeration (Android backend capabilities) attaches to
  // this VM at engine startup.
  velmux_android_set_java_vm(vm);
  jclass clazz = env->FindClass("com/velmux/mobile/VelmuxJni");
  if (clazz == nullptr) {
    return JNI_ERR;
  }
  if (env->RegisterNatives(clazz, kVelmuxJniMethods,
                           sizeof(kVelmuxJniMethods) /
                               sizeof(kVelmuxJniMethods[0])) != JNI_OK) {
    return JNI_ERR;
  }
  return JNI_VERSION_1_6;
}
