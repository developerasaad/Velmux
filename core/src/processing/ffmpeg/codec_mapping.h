// FFmpeg backend — codec identifier mapping between Velmux's product
// vocabulary and libavcodec's AVCodecID. Only intentional Velmux codecs are
// mapped; everything else the FFmpeg build may contain stays invisible to
// the product (feature registry is product-controlled).
#pragma once

#include <velmux/types.h>

extern "C" {
#include <libavcodec/codec_id.h>
}

namespace velmux::ffmpeg {

// AVCodecID -> Velmux enum (nullopt when the codec is not in the Velmux
// catalog). Used by the inspector when normalizing media.
std::optional<velmux::core::VideoCodec> videoCodecFromAv(AVCodecID id);
std::optional<velmux::core::AudioCodec> audioCodecFromAv(AVCodecID id);
std::optional<velmux::core::SubtitleFormat> subtitleFormatFromAv(AVCodecID id);

// Velmux enum -> AVCodecID (AV_CODEC_ID_NONE when unmapped).
AVCodecID avCodecFor(velmux::core::VideoCodec codec);
AVCodecID avCodecFor(velmux::core::AudioCodec codec);

// Container <-> AVOutputFormat/AVInputFormat short names.
const char *avFormatNameFor(velmux::core::Container container);
std::optional<velmux::core::Container> containerFromAvFormatName(const char *name);

} // namespace velmux::ffmpeg
