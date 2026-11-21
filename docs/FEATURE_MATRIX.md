# Velmux — Feature Matrix

What Velmux intentionally supports in V1. This registry is product-controlled:
an OS adding a random new codec must NOT make a new format appear automatically.
Runtime discovery only selects execution paths among intentional features.

Legend per codec: **D** decode, **E** encode, **C** stream copy. Container compatibility
is explicit; invalid combinations are never exposed in the UI.

## Video codecs

| Codec | D | E | C | Notes |
|---|---|---|---|---|
| H.264 / AVC | x | x | x | Native (MediaCodec / VideoToolbox) + FFmpeg |
| H.265 / HEVC | x | x | x | HDR passthrough where compatible |
| AV1 | x | x | x | Native where device supports; FFmpeg (dav1d / SVT-AV1) |
| VP8 | x | x | x | |
| VP9 | x | x | x | Profile 0/2 |
| MPEG-4 Part 2 | x | x | x | |
| MPEG-2 | x | – | x | decode/copy only |
| MPEG-1 | x | – | x | decode/copy only |
| H.263 | x | – | x | decode/copy only |
| VC-1 | x | – | x | decode/copy only |
| WMV family | x | – | x | decode/copy only |
| ProRes | x | – | x | decode/copy only (V1) |
| DNxHD/DNxHR | x | – | x | decode/copy only (V1) |
| FFV1 | x | x | x | archival |
| MJPEG | x | x | x | |

## Audio codecs

| Codec | D | E | C | Notes |
|---|---|---|---|---|
| AAC-LC | x | x | x | |
| HE-AAC | x | x | x | |
| xHE-AAC | x* | – | – | decode where OS/FFmpeg provides; not assumed |
| MP3 | x | x | x | |
| Opus | x | x | x | |
| Vorbis | x | x | x | |
| FLAC | x | x | x | |
| ALAC | x | x | x | |
| PCM | x | x | x | bit-depth control where meaningful |
| AC-3 | x | x | x | |
| E-AC-3 | x | x* | x | encode where practical |
| DTS / DTS-HD | x | – | x | decode/copy only |
| TrueHD / MLP | x | – | x | decode/copy only |
| WMA | x | – | x | decode/copy only |
| WavPack | x | x | x | |
| APE | x | – | – | decode only |
| AMR-NB | x | x | x | |
| AMR-WB | x | x | x | |

Feature availability must reflect the actual compiled/native backends — the planner
only offers what a registered backend can really execute on this device.

## Containers

MP4 / M4V, MOV, MKV, WebM, AVI, MPEG-TS / MTS, FLV, 3GP, M4A, Ogg, MP3, WAV, FLAC,
ASF/WMA (where useful).

Valid codec/container pairs are maintained in the `FeatureRegistry`
(`core/src/feature_registry.cpp`). The UI only ever exposes valid combinations.

## Subtitle formats

SRT, WebVTT, ASS, SSA, MP4 timed text (mov_text), TTML, CEA-608, CEA-708, PGS,
DVD VobSub, DVB subtitles.

V1 operations: detect embedded tracks, preserve, select, remove, extract, add
external, convert compatible text formats, adjust timing, preserve/edit language
metadata, preserve default/forced flags, burn into video (text via libass; bitmap
PGS/VobSub/DVB burn-in supported). PGS/VobSub → SRT requires OCR and is NOT V1.
Automatic speech captioning is NOT V1.

Subtitles are never silently discarded during ordinary conversion/compression.

## Video operations (V1)

Codec conversion, bitrate control, quality-oriented compression, target file-size
compression, resolution conversion, custom resolution, frame-rate conversion, crop,
rotation, flip, trimming, remove/replace/extract audio, multiple audio tracks,
fast remux, stream copy, batch processing, metadata preservation, HDR preservation
where compatible, HDR → SDR where supported, subtitle preservation/burn-in, multiple
language tracks, output validation.

## Audio operations (V1)

Codec conversion, bitrate control, CBR/VBR where supported, sample-rate conversion,
mono/stereo conversion, channel layout handling, PCM bit-depth control, trim, gain,
loudness normalization, fade in/out, playback-speed adjustment, pitch-preserving time
stretch, silence detection/removal, audio extraction from video, metadata, cover art,
batch processing.

## Sample compression

Before a long conversion, generate a short representative sample with proposed
settings (preferring a section with motion/detail), compare original vs sample at
matched source timestamps, adjust settings, then commit to the full conversion.
