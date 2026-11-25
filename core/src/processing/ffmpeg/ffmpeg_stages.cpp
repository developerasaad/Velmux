// FFmpeg stage workers — implementation. See ffmpeg_stages.h.
#include "processing/ffmpeg/ffmpeg_stages.h"

#include "processing/ffmpeg/codec_mapping.h"
#include "ffmpeg_util.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

namespace velmux::ffmpeg {

using velmux::backend::CpuFramePtr;
using velmux::backend::ICpuFrame;
using velmux::backend::InterchangeFormat;
using velmux::backend::InterchangePixel;
using velmux::backend::PacketView;
using velmux::backend::StageEncoderConfig;
using velmux::core::Container;
using velmux::core::Error;
using velmux::core::ErrorCode;
using velmux::core::VideoCodec;

namespace {

int pixelFor(InterchangePixel pixel) {
  switch (pixel) {
    case InterchangePixel::Yuv420P:
      return AV_PIX_FMT_YUV420P;
  }
  return AV_PIX_FMT_YUV420P;
}

// ICpuFrame over an owned AVFrame reference. Plane semantics follow the
// frame's format (Yuv420P: 3 planes).
class AvCpuFrame final : public ICpuFrame {
public:
  explicit AvCpuFrame(AVFrame *ref) : frame_(ref) {}
  ~AvCpuFrame() override {
    if (frame_ != nullptr) {
      AVFrame *f = frame_;
      av_frame_free(&f);
    }
  }

  int planeCount() const override {
    return frame_ != nullptr ? 3 : 0; // Yuv420P
  }
  const std::uint8_t *planeData(int plane) const override {
    return frame_ != nullptr && plane < 3 ? frame_->data[plane] : nullptr;
  }
  int planeLinesize(int plane) const override {
    return frame_ != nullptr && plane < 3 ? frame_->linesize[plane] : 0;
  }
  std::int64_t ptsUs() const override { return ptsUs_; }
  std::int64_t durationUs() const override { return durationUs_; }

  void setTiming(std::int64_t pts, std::int64_t duration) {
    ptsUs_ = pts;
    durationUs_ = duration;
  }

private:
  AVFrame *frame_ = nullptr;
  std::int64_t ptsUs_ = 0;
  std::int64_t durationUs_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

struct FfmpegStageDecoder::Impl {
  AVFormatContext *in = nullptr;
  AVCodecContext *dec = nullptr;
  SwsContext *sws = nullptr;
  AVFrame *decFrame = nullptr;
  AVFrame *yuvFrame = nullptr;
  AVPacket *pkt = nullptr;
  int videoStream = -1;
  bool demuxEof = false;
  bool decoderFlushed = false;
  InterchangeFormat format;
  std::int64_t duration = 0; // microseconds
};

FfmpegStageDecoder::FfmpegStageDecoder() : impl_(std::make_unique<Impl>()) {}
FfmpegStageDecoder::~FfmpegStageDecoder() {
  if (impl_ == nullptr) {
    return;
  }
  av_packet_free(&impl_->pkt);
  av_frame_free(&impl_->yuvFrame);
  av_frame_free(&impl_->decFrame);
  sws_freeContext(impl_->sws);
  avcodec_free_context(&impl_->dec);
  if (impl_->in != nullptr) {
    AVFormatContext *ctx = impl_->in;
    avformat_close_input(&ctx);
  }
}

Error FfmpegStageDecoder::open(const std::string &sourcePath,
                               InterchangePixel pixel) {
  if (pixel != InterchangePixel::Yuv420P) {
    return Error::make(ErrorCode::UnsupportedOperation,
                       "unsupported interchange pixel format", "ffmpeg");
  }
  int rc = avformat_open_input(&impl_->in, sourcePath.c_str(), nullptr,
                               nullptr);
  if (rc < 0) {
    return makeAvError(ErrorCode::SourceUnavailable, "open input", rc);
  }
  if (avformat_find_stream_info(impl_->in, nullptr) < 0) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot read stream info", "ffmpeg");
  }
  for (unsigned i = 0; i < impl_->in->nb_streams; ++i) {
    if (impl_->in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      impl_->videoStream = int(i);
      break;
    }
  }
  if (impl_->videoStream < 0) {
    return Error::make(ErrorCode::InvalidArgument,
                       "the source has no video track", "ffmpeg");
  }
  AVStream *stream = impl_->in->streams[impl_->videoStream];
  const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (codec == nullptr) {
    return Error::make(
        ErrorCode::UnsupportedOperation,
        std::string("no FFmpeg decoder for input video codec: ") +
            avcodec_get_name(stream->codecpar->codec_id),
        "ffmpeg");
  }
  impl_->dec = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(impl_->dec, stream->codecpar);
  impl_->dec->pkt_timebase = stream->time_base;
  if (impl_->dec->time_base.num <= 0 || impl_->dec->time_base.den <= 0) {
    impl_->dec->time_base = stream->time_base;
  }
  if (avcodec_open2(impl_->dec, codec, nullptr) < 0) {
    return Error::make(ErrorCode::ExecutionFailed, "cannot open decoder",
                       "ffmpeg");
  }
  if (impl_->dec->time_base.num <= 0 || impl_->dec->time_base.den <= 0) {
    impl_->dec->time_base = stream->time_base;
  }

  // Interchange conversion: everything becomes Yuv420P through swscale.
  const int target = pixelFor(pixel);
  impl_->sws = sws_getContext(
      impl_->dec->width, impl_->dec->height, impl_->dec->pix_fmt,
      impl_->dec->width, impl_->dec->height, static_cast<AVPixelFormat>(target),
      SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (impl_->sws == nullptr) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot create the format converter", "ffmpeg");
  }
  impl_->yuvFrame = av_frame_alloc();
  impl_->yuvFrame->format = target;
  impl_->yuvFrame->width = impl_->dec->width;
  impl_->yuvFrame->height = impl_->dec->height;
  if (av_frame_get_buffer(impl_->yuvFrame, 0) < 0) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot allocate the interchange frame", "ffmpeg");
  }
  impl_->decFrame = av_frame_alloc();
  impl_->pkt = av_packet_alloc();

  impl_->format.pixel = pixel;
  impl_->format.width = impl_->dec->width;
  impl_->format.height = impl_->dec->height;
  if (impl_->in->duration != AV_NOPTS_VALUE && impl_->in->duration > 0) {
    impl_->duration = av_rescale_q(impl_->in->duration,
                                   AVRational{1, AV_TIME_BASE},
                                   AVRational{1, 1000000});
  } else if (stream->duration != AV_NOPTS_VALUE && stream->time_base.den > 0) {
    impl_->duration = av_rescale_q(stream->duration, stream->time_base,
                                   AVRational{1, 1000000});
  }
  return Error::make(ErrorCode::None, "");
}

InterchangeFormat FfmpegStageDecoder::outputFormat() const {
  return impl_->format;
}

std::int64_t FfmpegStageDecoder::durationUs() const {
  return impl_->duration;
}

Error FfmpegStageDecoder::next(CpuFramePtr &frame) {
  frame = nullptr;
  if (impl_->videoStream < 0) {
    return Error::make(ErrorCode::InvalidArgument, "decoder not opened",
                       "ffmpeg");
  }
  for (;;) {
    int rc = avcodec_receive_frame(impl_->dec, impl_->decFrame);
    if (rc == 0) {
      // Convert into the interchange frame. A previously returned frame
      // may still hold a reference: make the scratch writable first.
      if (av_frame_make_writable(impl_->yuvFrame) < 0) {
        av_frame_unref(impl_->decFrame);
        return Error::make(ErrorCode::ExecutionFailed,
                           "cannot prepare the interchange frame", "ffmpeg");
      }
      sws_scale(impl_->sws, impl_->decFrame->data, impl_->decFrame->linesize,
                0, impl_->decFrame->height, impl_->yuvFrame->data,
                impl_->yuvFrame->linesize);
      // Independent reference for the consumer; timing on the us timeline.
      AVFrame *owned = av_frame_alloc();
      if (av_frame_ref(owned, impl_->yuvFrame) < 0) {
        av_frame_unref(impl_->decFrame);
        return Error::make(ErrorCode::ExecutionFailed,
                           "cannot reference the interchange frame", "ffmpeg");
      }
      auto out = std::make_unique<AvCpuFrame>(owned);
      std::int64_t ptsUs = 0;
      if (impl_->decFrame->pts != AV_NOPTS_VALUE &&
          impl_->dec->time_base.num > 0) {
        ptsUs = av_rescale_q(impl_->decFrame->pts, impl_->dec->time_base,
                             AVRational{1, 1000000});
      } else if (impl_->decFrame->best_effort_timestamp != AV_NOPTS_VALUE &&
                 impl_->dec->time_base.num > 0) {
        ptsUs = av_rescale_q(impl_->decFrame->best_effort_timestamp,
                             impl_->dec->time_base, AVRational{1, 1000000});
      }
      std::int64_t durUs = 0;
      if (impl_->decFrame->duration > 0 &&
          impl_->dec->time_base.num > 0) {
        durUs = av_rescale_q(impl_->decFrame->duration,
                             impl_->dec->time_base, AVRational{1, 1000000});
      }
      out->setTiming(ptsUs, durUs);
      av_frame_unref(impl_->decFrame);
      frame = std::move(out);
      return Error::make(ErrorCode::None, "");
    }
    if (rc == AVERROR_EOF) {
      // End of stream: null frame, no-error.
      return Error::make(ErrorCode::None, "");
    }
    if (rc != AVERROR(EAGAIN)) {
      return makeAvError(ErrorCode::ExecutionFailed, "decode frame", rc);
    }
    // Decoder needs input.
    if (impl_->demuxEof) {
      if (impl_->decoderFlushed) {
        return Error::make(ErrorCode::None, "");
      }
      avcodec_send_packet(impl_->dec, nullptr);
      impl_->decoderFlushed = true;
      continue;
    }
    rc = av_read_frame(impl_->in, impl_->pkt);
    if (rc < 0) {
      if (rc == AVERROR_EOF) {
        impl_->demuxEof = true;
        continue;
      }
      return makeAvError(ErrorCode::ExecutionFailed, "read frame", rc);
    }
    if (impl_->pkt->stream_index == impl_->videoStream) {
      rc = avcodec_send_packet(impl_->dec, impl_->pkt);
      av_packet_unref(impl_->pkt);
      if (rc < 0) {
        return makeAvError(ErrorCode::ExecutionFailed, "send packet", rc);
      }
    } else {
      av_packet_unref(impl_->pkt);
    }
  }
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

struct FfmpegStageEncoder::Impl {
  AVCodecContext *enc = nullptr;
  AVFrame *inFrame = nullptr;
  AVPacket *pkt = nullptr;
  bool flushed = false;
  bool opened = false;
};

FfmpegStageEncoder::FfmpegStageEncoder() : impl_(std::make_unique<Impl>()) {}
FfmpegStageEncoder::~FfmpegStageEncoder() {
  if (impl_ == nullptr) {
    return;
  }
  av_packet_free(&impl_->pkt);
  av_frame_free(&impl_->inFrame);
  avcodec_free_context(&impl_->enc);
}

Error FfmpegStageEncoder::open(const StageEncoderConfig &config,
                               InterchangePixel pixel) {
  if (pixel != InterchangePixel::Yuv420P) {
    return Error::make(ErrorCode::UnsupportedOperation,
                       "unsupported interchange pixel format", "ffmpeg");
  }
  const char *name = encoderNameFor(config.codec);
  const AVCodec *codec =
      name != nullptr ? avcodec_find_encoder_by_name(name) : nullptr;
  if (codec == nullptr) {
    codec = avcodec_find_encoder(avCodecFor(config.codec));
  }
  if (codec == nullptr) {
    return Error::make(ErrorCode::UnsupportedOperation,
                       std::string("no FFmpeg encoder for codec: ") +
                           velmux::core::codecName(config.codec),
                       "ffmpeg");
  }
  impl_->enc = avcodec_alloc_context3(codec);
  impl_->enc->width = std::max(config.width & ~1, 16);
  impl_->enc->height = std::max(config.height & ~1, 16);
  if (config.frameRate > 0.0) {
    const int fps = std::max(1, int(std::lround(config.frameRate)));
    impl_->enc->framerate = AVRational{fps, 1};
    impl_->enc->time_base = AVRational{1, fps};
  } else {
    impl_->enc->framerate = AVRational{30, 1};
    impl_->enc->time_base = AVRational{1, 30};
  }
  impl_->enc->pix_fmt = AV_PIX_FMT_YUV420P;
  if (config.globalHeader) {
    impl_->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  if (config.bitRate > 0) {
    impl_->enc->bit_rate = config.bitRate;
    impl_->enc->rc_max_rate = config.bitRate;
    impl_->enc->rc_buffer_size = config.bitRate * 2;
  } else {
    const int quality = config.quality.value_or(60);
    const int crf = qualityToCrf(config.codec, quality);
    av_opt_set(impl_->enc->priv_data, "crf", std::to_string(crf).c_str(), 0);
    if (config.codec == VideoCodec::AV1) {
      av_opt_set(impl_->enc->priv_data, "preset", "8", 0);
    }
  }
  if (avcodec_open2(impl_->enc, codec, nullptr) < 0) {
    return Error::make(ErrorCode::ExecutionFailed, "cannot open encoder",
                       "ffmpeg");
  }
  impl_->inFrame = av_frame_alloc();
  impl_->inFrame->format = AV_PIX_FMT_YUV420P;
  impl_->inFrame->width = impl_->enc->width;
  impl_->inFrame->height = impl_->enc->height;
  if (av_frame_get_buffer(impl_->inFrame, 0) < 0) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot allocate the encoder input frame", "ffmpeg");
  }
  impl_->pkt = av_packet_alloc();
  impl_->opened = true;
  return Error::make(ErrorCode::None, "");
}

Error FfmpegStageEncoder::push(const ICpuFrame &frame) {
  if (!impl_->opened) {
    return Error::make(ErrorCode::InvalidArgument, "encoder not opened",
                       "ffmpeg");
  }
  if (frame.planeCount() != 3 || frame.planeData(0) == nullptr) {
    return Error::make(ErrorCode::InvalidArgument,
                       "frame is not in the negotiated Yuv420P interchange",
                       "ffmpeg");
  }
  if (av_frame_make_writable(impl_->inFrame) < 0) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot prepare the encoder input frame", "ffmpeg");
  }
  // Plane copy from the producer's frame (cross-backend interchange).
  const int w = impl_->enc->width;
  const int h = impl_->enc->height;
  for (int p = 0; p < 3; ++p) {
    const int pw = p == 0 ? w : w / 2;
    const int ph = p == 0 ? h : h / 2;
    const std::uint8_t *src = frame.planeData(p);
    std::uint8_t *dst = impl_->inFrame->data[p];
    const int srcStride = frame.planeLinesize(p);
    const int dstStride = impl_->inFrame->linesize[p];
    for (int y = 0; y < ph; ++y) {
      std::memcpy(dst + std::int64_t(y) * dstStride,
                  src + std::int64_t(y) * srcStride, pw);
    }
  }
  if (frame.ptsUs() != AV_NOPTS_VALUE) {
    impl_->inFrame->pts = av_rescale_q(
        frame.ptsUs(), AVRational{1, 1000000}, impl_->enc->time_base);
  } else {
    impl_->inFrame->pts = AV_NOPTS_VALUE;
  }
  const int rc = avcodec_send_frame(impl_->enc, impl_->inFrame);
  if (rc < 0) {
    return makeAvError(ErrorCode::ExecutionFailed, "send frame", rc);
  }
  return Error::make(ErrorCode::None, "");
}

Error FfmpegStageEncoder::pull(PacketView &packet) {
  packet = PacketView{};
  if (!impl_->opened) {
    return Error::make(ErrorCode::InvalidArgument, "encoder not opened",
                       "ffmpeg");
  }
  const int rc = avcodec_receive_packet(impl_->enc, impl_->pkt);
  if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
    // Nothing ready (or drained): empty view, no error.
    return Error::make(ErrorCode::None, "");
  }
  if (rc < 0) {
    return makeAvError(ErrorCode::ExecutionFailed, "receive packet", rc);
  }
  packet.data = impl_->pkt->data;
  packet.size = std::size_t(impl_->pkt->size);
  packet.keyframe = (impl_->pkt->flags & AV_PKT_FLAG_KEY) != 0;
  packet.ptsUs =
      impl_->pkt->pts != AV_NOPTS_VALUE
          ? av_rescale_q(impl_->pkt->pts, impl_->enc->time_base,
                         AVRational{1, 1000000})
          : 0;
  packet.durationUs =
      impl_->pkt->duration > 0
          ? av_rescale_q(impl_->pkt->duration, impl_->enc->time_base,
                         AVRational{1, 1000000})
          : 0;
  packet.configData = impl_->enc->extradata;
  packet.configSize = std::size_t(impl_->enc->extradata_size);
  return Error::make(ErrorCode::None, "");
}

Error FfmpegStageEncoder::flush() {
  if (!impl_->opened || impl_->flushed) {
    return Error::make(ErrorCode::None, "");
  }
  const int rc = avcodec_send_frame(impl_->enc, nullptr);
  if (rc < 0) {
    return makeAvError(ErrorCode::ExecutionFailed, "flush encoder", rc);
  }
  impl_->flushed = true;
  return Error::make(ErrorCode::None, "");
}

// ---------------------------------------------------------------------------
// Mux stage
// ---------------------------------------------------------------------------

struct FfmpegMuxStage::Impl {
  AVFormatContext *out = nullptr;
  AVFormatContext *audioIn = nullptr; // for the audio stream copy
  int audioInStream = -1;
  AVPacket *pendingAudio = nullptr;   // lookahead packet held for the
                                      // next interleave window
  std::string outputPath;
  TrackOptions tracks;
  int outVideo = -1;
  int outAudio = -1;
  bool headerWritten = false;
  bool finalized = false;
  // Last video packet pts in OUTPUT STREAM timebase ticks (valid only
  // when haveLastVideoPts). The muxer requires strictly increasing dts
  // (see writeVideoPacket).
  bool haveLastVideoPts = false;
  std::int64_t lastVideoPts = 0;
};

FfmpegMuxStage::FfmpegMuxStage() : impl_(std::make_unique<Impl>()) {}
FfmpegMuxStage::~FfmpegMuxStage() {
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->out != nullptr && impl_->headerWritten && !impl_->finalized) {
    av_write_trailer(impl_->out);
  }
  if (impl_->out != nullptr && !(impl_->out->oformat->flags & AVFMT_NOFILE)) {
    avio_closep(&impl_->out->pb);
  }
  avformat_free_context(impl_->out);
  av_packet_free(&impl_->pendingAudio);
  if (impl_->audioIn != nullptr) {
    AVFormatContext *ctx = impl_->audioIn;
    avformat_close_input(&ctx);
  }
}

Error FfmpegMuxStage::open(const std::string &outputPath,
                           const TrackOptions &tracks) {
  impl_->outputPath = outputPath;
  impl_->tracks = tracks;
  int rc = avformat_alloc_output_context2(
      &impl_->out, nullptr, avFormatNameFor(tracks.container),
      outputPath.c_str());
  if (rc < 0 || impl_->out == nullptr) {
    return makeAvError(ErrorCode::ExecutionFailed, "alloc output context", rc);
  }
  if (tracks.copyAudio) {
    rc = avformat_open_input(&impl_->audioIn, tracks.sourcePath.c_str(),
                             nullptr, nullptr);
    if (rc < 0) {
      return makeAvError(ErrorCode::SourceUnavailable, "open audio source",
                         rc);
    }
    if (avformat_find_stream_info(impl_->audioIn, nullptr) < 0) {
      return Error::make(ErrorCode::ExecutionFailed,
                         "cannot read the audio source stream info", "ffmpeg");
    }
    for (unsigned i = 0; i < impl_->audioIn->nb_streams; ++i) {
      if (impl_->audioIn->streams[i]->codecpar->codec_type ==
          AVMEDIA_TYPE_AUDIO) {
        impl_->audioInStream = int(i);
        break;
      }
    }
    if (impl_->audioInStream < 0) {
      return Error::make(ErrorCode::InvalidArgument,
                         "audio copy requested but the source has no audio "
                         "track",
                         "ffmpeg");
    }
  }
  return Error::make(ErrorCode::None, "");
}

bool FfmpegMuxStage::wantsGlobalHeader() const {
  return impl_ != nullptr && impl_->out != nullptr &&
         (impl_->out->oformat->flags & AVFMT_GLOBALHEADER) != 0;
}

Error FfmpegMuxStage::writeVideoPacket(const PacketView &packet) {
  if (impl_->finalized) {
    return Error::make(ErrorCode::InvalidArgument, "mux stage finalized",
                       "ffmpeg");
  }
  if (!impl_->headerWritten) {
    // First packet: create the video track from the carried configuration
    // and the geometry agreed at open(), then start the file.
    AVStream *video = avformat_new_stream(impl_->out, nullptr);
    if (video == nullptr) {
      return Error::make(ErrorCode::ExecutionFailed, "new video stream",
                         "ffmpeg");
    }
    const AVCodecID codecId = avCodecFor(impl_->tracks.videoCodec);
    video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    video->codecpar->codec_id = codecId;
    video->codecpar->width = impl_->tracks.width;
    video->codecpar->height = impl_->tracks.height;
    video->codecpar->format = AV_PIX_FMT_YUV420P;
    if (packet.configData != nullptr && packet.configSize > 0) {
      // Deep-copy the configuration (extradata) into the stream.
      video->codecpar->extradata = static_cast<std::uint8_t *>(
          av_mallocz(packet.configSize + AV_INPUT_BUFFER_PADDING_SIZE));
      if (video->codecpar->extradata == nullptr) {
        return Error::make(ErrorCode::ExecutionFailed,
                           "cannot allocate stream configuration", "ffmpeg");
      }
      std::memcpy(video->codecpar->extradata, packet.configData,
                  packet.configSize);
      video->codecpar->extradata_size = int(packet.configSize);
    }
    if (impl_->tracks.frameRate > 0.0) {
      const int fps = std::max(1, int(std::lround(impl_->tracks.frameRate)));
      video->avg_frame_rate = AVRational{fps, 1};
      video->time_base = AVRational{1, fps};
    } else {
      video->avg_frame_rate = AVRational{30, 1};
      video->time_base = AVRational{1, 30};
    }
    impl_->outVideo = video->index;

    if (impl_->audioInStream >= 0) {
      AVStream *audio = avformat_new_stream(impl_->out, nullptr);
      if (audio == nullptr) {
        return Error::make(ErrorCode::ExecutionFailed, "new audio stream",
                           "ffmpeg");
      }
      const AVStream *src = impl_->audioIn->streams[impl_->audioInStream];
      avcodec_parameters_copy(audio->codecpar, src->codecpar);
      audio->codecpar->codec_tag = 0;
      audio->time_base = src->time_base;
      impl_->outAudio = audio->index;
    }

    if (!(impl_->out->oformat->flags & AVFMT_NOFILE)) {
      const int rc =
          avio_open(&impl_->out->pb, impl_->outputPath.c_str(),
                    AVIO_FLAG_WRITE);
      if (rc < 0) {
        return makeAvError(ErrorCode::ExecutionFailed, "open output", rc);
      }
    }
    const int rc = avformat_write_header(impl_->out, nullptr);
    if (rc < 0) {
      return makeAvError(ErrorCode::ExecutionFailed, "write header", rc);
    }
    impl_->headerWritten = true;
  }

  // Reference the view's data into our own packet (the view's buffer is
  // owned by the encoder and reused on its next pull).
  AVPacket *outPacket = av_packet_alloc();
  if (av_new_packet(outPacket, int(packet.size)) < 0) {
    av_packet_free(&outPacket);
    return Error::make(ErrorCode::ExecutionFailed, "cannot allocate packet",
                       "ffmpeg");
  }
  std::memcpy(outPacket->data, packet.data, packet.size);
  outPacket->flags = packet.keyframe ? AV_PKT_FLAG_KEY : 0;
  outPacket->stream_index = impl_->outVideo;
  const AVStream *video = impl_->out->streams[impl_->outVideo];
  const AVRounding rounding =
      static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
  outPacket->pts =
      av_rescale_q_rnd(packet.ptsUs, AVRational{1, 1000000},
                       video->time_base, rounding);
  // Last-resort monotonicity guard: the interleaved muxers reject
  // non-increasing dts ('Application provided invalid, non monotonically
  // increasing dts'). Stage pipelines carry platform-decoder timestamps
  // and the Android stage decoder already rebases duplicates (see
  // AndroidStageDecoder::next), but this stage must stay safe for every
  // packet source: bump non-increasing (or negative) pts/dts forward by
  // one timebase tick, preserving order and keeping dts == pts.
  if (outPacket->pts < 0) {
    outPacket->pts = 0;
  }
  if (impl_->haveLastVideoPts &&
      outPacket->pts <= impl_->lastVideoPts) {
    outPacket->pts = impl_->lastVideoPts + 1;
  }
  impl_->lastVideoPts = outPacket->pts;
  impl_->haveLastVideoPts = true;
  outPacket->dts = outPacket->pts;
  if (packet.durationUs > 0) {
    outPacket->duration =
        av_rescale_q(packet.durationUs, AVRational{1, 1000000},
                     video->time_base);
  }
  if (av_interleaved_write_frame(impl_->out, outPacket) < 0) {
    av_packet_free(&outPacket);
    return Error::make(ErrorCode::ExecutionFailed, "write video packet",
                       "ffmpeg");
  }
  av_packet_free(&outPacket);

  return drainAudioUpTo(packet.ptsUs);
}

Error FfmpegMuxStage::drainAudioUpTo(std::int64_t boundaryPtsUs) {
  if (impl_->outAudio < 0 || impl_->audioInStream < 0) {
    return Error::make(ErrorCode::None, "");
  }
  const AVStream *src = impl_->audioIn->streams[impl_->audioInStream];
  const AVStream *dst = impl_->out->streams[impl_->outAudio];
  AVPacket *pkt = av_packet_alloc();
  for (;;) {
    if (impl_->pendingAudio != nullptr) {
      av_packet_move_ref(pkt, impl_->pendingAudio);
      av_packet_free(&impl_->pendingAudio);
    } else {
      const int rc = av_read_frame(impl_->audioIn, pkt);
      if (rc == AVERROR_EOF) {
        break;
      }
      if (rc < 0) {
        av_packet_free(&pkt);
        return makeAvError(ErrorCode::ExecutionFailed, "read audio packet",
                           rc);
      }
      if (pkt->stream_index != impl_->audioInStream) {
        av_packet_unref(pkt);
        continue;
      }
    }
    // Lookahead: hold packets that belong to a later interleave window.
    if (boundaryPtsUs >= 0 && pkt->pts != AV_NOPTS_VALUE) {
      const std::int64_t ptsUs =
          av_rescale_q(pkt->pts, src->time_base, AVRational{1, 1000000});
      if (ptsUs > boundaryPtsUs) {
        impl_->pendingAudio = av_packet_alloc();
        av_packet_move_ref(impl_->pendingAudio, pkt);
        av_packet_free(&pkt);
        return Error::make(ErrorCode::None, "");
      }
    }
    // Write the packet. av_packet_move_ref copies the WHOLE struct: every
    // field set before it is clobbered. Save the source timing, move the
    // content, then rescale into the output stream's time base (the muxer
    // may have changed it during write_header) and retarget the stream.
    const std::int64_t srcPts = pkt->pts;
    const std::int64_t srcDts = pkt->dts;
    const std::int64_t srcDuration = pkt->duration;
    AVPacket *outPacket = av_packet_alloc();
    av_packet_move_ref(outPacket, pkt);
    const AVRounding rounding =
        static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    outPacket->pts = av_rescale_q_rnd(srcPts, src->time_base,
                                      dst->time_base, rounding);
    outPacket->dts = av_rescale_q_rnd(srcDts, src->time_base,
                                      dst->time_base, rounding);
    outPacket->duration =
        av_rescale_q(srcDuration, src->time_base, dst->time_base);
    outPacket->stream_index = impl_->outAudio;
    outPacket->pos = -1;
    const bool writeOk =
        av_interleaved_write_frame(impl_->out, outPacket) >= 0;
    av_packet_free(&outPacket);
    if (!writeOk) {
      av_packet_free(&pkt);
      return Error::make(ErrorCode::ExecutionFailed, "write audio packet",
                         "ffmpeg");
    }
  }
  av_packet_free(&pkt);
  return Error::make(ErrorCode::None, "");
}

Error FfmpegMuxStage::finalize() {
  if (impl_->finalized) {
    return Error::make(ErrorCode::None, "");
  }
  if (!impl_->headerWritten) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "no video packet was written; refusing to finalize an "
                       "empty hybrid output",
                       "ffmpeg");
  }
  Error err = drainAudioUpTo(-1);
  if (!err.ok()) {
    return err;
  }
  if (av_write_trailer(impl_->out) < 0) {
    return Error::make(ErrorCode::ExecutionFailed, "write trailer", "ffmpeg");
  }
  impl_->finalized = true;
  return Error::make(ErrorCode::None, "");
}

// ---------------------------------------------------------------------------
// Stage backend factory
// ---------------------------------------------------------------------------

velmux::backend::IVideoStageDecoder *FfmpegStageBackend::createVideoDecoder() {
  return new FfmpegStageDecoder();
}

velmux::backend::IVideoStageEncoder *FfmpegStageBackend::createVideoEncoder() {
  return new FfmpegStageEncoder();
}

velmux::backend::IMuxStage *FfmpegStageBackend::createMux() {
  return new FfmpegMuxStage();
}

} // namespace velmux::ffmpeg
