#include "compression/compression.h"

#include "processing/pipeline.h"

#include <algorithm>
#include <cmath>

namespace velmux::core {

namespace {

// Bits per second per pixel that still yields acceptable quality for
// typical content at each codec family. Used for feasibility checks and
// resolution-downshift decisions. Conservative on purpose: Velmux would
// rather warn honestly than promise an impossible size.
struct CodecRatePoint {
  VideoCodec codec;
  double bitsPerPixelFloor; // below this, quality collapses
  double bitsPerPixelGood;  // comfortable quality
};

const CodecRatePoint kRatePoints[] = {
    {VideoCodec::H264, 0.045, 0.10},
    {VideoCodec::HEVC, 0.030, 0.07},
    {VideoCodec::AV1, 0.020, 0.05},
    {VideoCodec::VP9, 0.030, 0.07},
    {VideoCodec::VP8, 0.050, 0.11},
    {VideoCodec::Mpeg4Part2, 0.060, 0.14},
    {VideoCodec::Ffv1, 0.0, 0.0},   // lossless: size follows content
    {VideoCodec::Mjpeg, 0.0, 0.0},  // intra-only: different regime
};

const CodecRatePoint *ratePointFor(VideoCodec codec) {
  for (const auto &p : kRatePoints) {
    if (p.codec == codec) {
      return &p;
    }
  }
  return nullptr;
}

// Audio bitrate floors (bits per second): below these, music/speech becomes
// objectionable. Used for budget allocation and feasibility.
std::int64_t audioFloorBps(AudioCodec codec, std::int32_t channels) {
  const std::int32_t ch = std::max<std::int32_t>(channels, 1);
  const auto perChannel = [&]() -> std::int64_t {
    switch (codec) {
      case AudioCodec::Flac:
      case AudioCodec::Alac:
      case AudioCodec::Pcm:
      case AudioCodec::WavPack:
        return 480 * 1024; // ~lossless stereo-scale
      case AudioCodec::Opus:
        return 24000;
      case AudioCodec::Aac:
        return 32000;
      case AudioCodec::HeAac:
        return 16000;
      case AudioCodec::Mp3:
        return 32000;
      case AudioCodec::Vorbis:
        return 28000;
      case AudioCodec::Ac3:
        return 32000;
      case AudioCodec::Eac3:
        return 32000;
      case AudioCodec::AmrNb:
        return 4750;
      case AudioCodec::AmrWb:
        return 8850;
      default:
        return 32000;
    }
  }();
  return perChannel * ch;
}

std::int64_t clampAudioBitrate(AudioCodec codec, std::int32_t channels,
                               std::int64_t requested) {
  const std::int64_t floorBps = audioFloorBps(codec, channels);
  const std::int64_t ceilingBps = floorBps * 4;
  return std::clamp(requested, floorBps, ceilingBps);
}

} // namespace

double containerOverheadFactor(Container container) {
  switch (container) {
    case Container::Mp4:
    case Container::M4v:
    case Container::Mov:
    case Container::M4a:
      return 0.005; // moov + index overhead
    case Container::Mkv:
    case Container::WebM:
      return 0.004;
    case Container::Ogg:
      return 0.006;
    case Container::MpegTs:
      return 0.04; // 188-byte packet framing
    case Container::Flv:
      return 0.02;
    case Container::Avi:
      return 0.01;
    case Container::ThreeGp:
      return 0.006;
    case Container::Mp3:
      return 0.003;
    case Container::Wav:
    case Container::Flac:
      return 0.002;
    case Container::Asf:
      return 0.02;
  }
  return 0.01;
}

BitrateBudget computeBitrateBudget(const MediaInfo &source,
                                   const BudgetInputs &inputs,
                                   std::int64_t targetSizeBytes) {
  BitrateBudget budget;
  if (targetSizeBytes <= 0) {
    budget.feasible = false;
    budget.message = "Target size must be positive.";
    return budget;
  }
  // Timeline scaling (playbackSpeed): the output timeline is
  // duration / speed, and the budget must fit THAT timeline.
  const double speed = inputs.playbackSpeed.value_or(1.0);
  if (speed <= 0.0) {
    budget.feasible = false;
    budget.message = "Playback speed must be positive.";
    return budget;
  }
  const double effectiveDuration = source.durationSec / speed;
  if (effectiveDuration <= 0.0) {
    budget.feasible = false;
    budget.message = "Source duration is unknown; cannot compute a budget.";
    return budget;
  }

  const Container container = inputs.outputContainer;
  const double overhead = containerOverheadFactor(container);
  // Payload bytes available after container overhead.
  const double payloadBytes = double(targetSizeBytes) / (1.0 + overhead);
  const double payloadBits = payloadBytes * 8.0;
  const double duration = effectiveDuration;

  // Audio allocation.
  const bool wantsAudio = inputs.audioEnabled && source.hasAudio();
  if (wantsAudio) {
    const AudioStream &srcAudio = source.audio.front();
    const std::int32_t channels =
        inputs.audio.channels.value_or(srcAudio.channels);
    const AudioCodec audioCodec =
        inputs.audio.codec.value_or(srcAudio.codec);
    if (inputs.audio.bitRate.has_value()) {
      budget.audioBitrate =
          clampAudioBitrate(audioCodec, channels, *inputs.audio.bitRate);
    } else {
      // Default: keep the source audio bitrate when it looks sane, else the
      // codec floor.
      const std::int64_t floorBps = audioFloorBps(audioCodec, channels);
      const std::int64_t srcBps = srcAudio.bitRate > 0 ? srcAudio.bitRate : 0;
      budget.audioBitrate = std::clamp(srcBps > 0 ? srcBps : floorBps * 2,
                                       floorBps, floorBps * 4);
    }
  }

  const double audioBits = double(budget.audioBitrate) * duration;
  double videoBits = payloadBits - audioBits;
  if (!inputs.videoEnabled) {
    // Audio-only output: the whole payload budget is the audio budget.
    if (wantsAudio) {
      const std::int64_t audioBudget = std::int64_t(payloadBits / duration);
      budget.audioBitrate = std::max(budget.audioBitrate, audioBudget);
    }
    budget.videoBitrate = 0;
    budget.feasible = wantsAudio ? duration > 0.0 : false;
    if (!budget.feasible) {
      budget.message = "Audio-only target needs a source audio track.";
    }
    return budget;
  }

  // Feasibility: the video budget must stay above the codec floor.
  const VideoCodec videoCodec =
      inputs.videoCodec.value_or(source.hasVideo() ? source.video.front().codec
                                                   : VideoCodec::H264);
  const VideoStream &srcVideo =
      source.hasVideo() ? source.video.front() : VideoStream{};
  const std::int32_t srcHeight =
      inputs.resolutionHeight.value_or(
          srcVideo.height > 0 ? srcVideo.height : 1080);
  const std::int32_t srcWidth = srcVideo.width > 0 ? srcVideo.width
                                                    : srcHeight * 16 / 9;
  const double frameRate =
      inputs.frameRate.value_or(srcVideo.frameRate > 0 ? srcVideo.frameRate
                                                       : 30.0);
  const double pixelsPerSec = double(srcWidth) * double(srcHeight) * frameRate;

  if (const CodecRatePoint *point = ratePointFor(videoCodec)) {
    if (point->bitsPerPixelFloor > 0.0) {
      const double floorBits =
          point->bitsPerPixelFloor * pixelsPerSec * duration;
      if (videoBits < floorBits) {
        budget.feasible = false;
        budget.message =
            "The requested size is too small for this content: even at the "
            "lowest acceptable quality the video alone would not fit. Try a "
            "larger target, a shorter trim, or a lower resolution.";
        return budget;
      }
    }
  }

  if (videoBits <= 0) {
    budget.feasible = false;
    budget.message = "No room left for video after audio allocation.";
    return budget;
  }

  budget.videoBitrate = std::int64_t(videoBits / duration);
  budget.feasible = true;
  return budget;
}

double qualityFactorForPreset(CompressionPreset preset) {
  switch (preset) {
    case CompressionPreset::SmallerFile:
      return 0.35;
    case CompressionPreset::HighQuality:
      return 0.85;
    case CompressionPreset::TargetSize:
      return 0.5; // bitrate-driven; quality follows the budget
    default:
      return 0.6; // Balanced, Custom without explicit quality
  }
}

ResolvedRatePolicy resolveRatePolicy(const MediaInfo &source,
                                     const CompressionIntent &compression,
                                     const BudgetInputs &inputs) {
  ResolvedRatePolicy policy;
  if (!compression.enabled) {
    // High-fidelity default for forced transcodes: CRF ~18 per codec
    // family (quality 65 maps to CRF 18 on the x264/x265 0..51 scale and
    // ~23 on the AV1/VP9 0..63 scale).
    policy.mode = ResolvedRatePolicy::Mode::Quality;
    policy.quality = 65;
    return policy;
  }
  switch (compression.preset) {
    case CompressionPreset::HighQuality:
      policy.quality = 85;
      return policy;
    case CompressionPreset::Balanced:
      policy.quality = 60;
      return policy;
    case CompressionPreset::SmallerFile:
      policy.quality = 35;
      return policy;
    case CompressionPreset::Custom: {
      if (compression.videoBitrate.has_value() &&
          *compression.videoBitrate > 0) {
        policy.mode = ResolvedRatePolicy::Mode::Bitrate;
        policy.bitRate = *compression.videoBitrate;
        return policy;
      }
      policy.quality = std::clamp(compression.quality.value_or(60), 1, 100);
      return policy;
    }
    case CompressionPreset::TargetSize: {
      if (compression.targetSizeBytes <= 0) {
        policy.quality = 60; // honest fallback: no target, balanced default
        return policy;
      }
      const BitrateBudget budget =
          computeBitrateBudget(source, inputs, compression.targetSizeBytes);
      if (budget.feasible && budget.videoBitrate > 0) {
        policy.mode = ResolvedRatePolicy::Mode::Bitrate;
        policy.bitRate = budget.videoBitrate;
      } else {
        // Infeasible target: balanced quality; planning surfaces the honest
        // budget message through its advisories.
        policy.quality = 60;
      }
      return policy;
    }
  }
  policy.quality = 60;
  return policy;
}

std::optional<std::int64_t> estimateOutputBytes(
    const MediaInfo &source, const Pipeline &pipeline,
    const BudgetInputs &inputs, const ResolvedRatePolicy &rate) {
  if (source.durationSec <= 0.0) {
    return std::nullopt;
  }
  const double speed = inputs.playbackSpeed.value_or(1.0);
  const double duration = source.durationSec / std::max(speed, 0.01);
  const Container container = pipeline.container;
  const double overhead = containerOverheadFactor(container);

  double bits = 0.0;
  if (pipeline.videoStreamCopy && source.hasVideo()) {
    bits += double(source.video.front().bitRate) * duration;
  } else if (rate.mode == ResolvedRatePolicy::Mode::Bitrate &&
             rate.bitRate > 0) {
    bits += double(rate.bitRate) * duration;
  } else {
    // Quality-mode estimate: rate point * pixels * fps.
    const VideoStream &srcVideo =
        source.hasVideo() ? source.video.front() : VideoStream{};
    const std::int32_t height = inputs.resolutionHeight.value_or(
        srcVideo.height > 0 ? srcVideo.height : 1080);
    const std::int32_t width =
        srcVideo.width > 0 ? srcVideo.width : height * 16 / 9;
    const double fps =
        inputs.frameRate.value_or(
            srcVideo.frameRate > 0 ? srcVideo.frameRate : 30.0);
    const double bpp =
        [&]() {
          const double q = double(rate.quality) / 100.0;
          if (const CodecRatePoint *p = ratePointFor(pipeline.videoCodec)) {
            if (p->bitsPerPixelGood > 0.0) {
              return p->bitsPerPixelFloor +
                     (p->bitsPerPixelGood - p->bitsPerPixelFloor) * q;
            }
          }
          return 0.08;
        }();
    bits += bpp * double(width) * double(height) * fps * duration;
  }

  if (inputs.audioEnabled && source.hasAudio()) {
    if (pipeline.audioStreamCopy) {
      bits += double(source.audio.front().bitRate) * duration;
    } else {
      const AudioStream &srcAudio = source.audio.front();
      bits += double(inputs.audio.bitRate.value_or(
          srcAudio.bitRate > 0 ? srcAudio.bitRate : 128000)) *
              duration;
    }
  }

  const double bytes = bits / 8.0 * (1.0 + overhead);
  if (bytes <= 0.0 || !std::isfinite(bytes)) {
    return std::nullopt;
  }
  return std::int64_t(bytes);
}

bool isCanonicalResolutionHeight(std::int32_t height) {
  switch (height) {
    case 2160:
    case 1440:
    case 1080:
    case 720:
    case 480:
    case 360:
      return true;
    default:
      return false;
  }
}

std::int32_t resolutionHeightForBudget(
    std::int32_t sourceHeight,
    const std::optional<std::int32_t> &requestedHeight,
    std::int64_t videoBitrate) {
  const std::int32_t target = requestedHeight.value_or(
      sourceHeight > 0 ? sourceHeight : 1080);
  // Never upscale.
  std::int32_t best = std::min(target, sourceHeight > 0 ? sourceHeight : target);

  const std::int32_t ladder[] = {2160, 1440, 1080, 720, 480, 360};
  // Rough width assumption for the rate arithmetic (aspect preserved in
  // reality; this is a policy estimate).
  const auto supports = [&](std::int32_t h, double bitsPerPixel) {
    const double w = double(h) * 16.0 / 9.0;
    return bitsPerPixel * w * double(h) * 30.0 <= double(videoBitrate);
  };
  // 4 Mbps handles 1080p H.264 reasonably; scale expectations per family via
  // bitsPerPixelFloor ~ 0.03..0.05.
  for (std::int32_t h : ladder) {
    if (h > best) {
      continue;
    }
    if (videoBitrate <= 0 || supports(h, 0.05)) {
      return h;
    }
  }
  return 360;
}

} // namespace velmux::core
