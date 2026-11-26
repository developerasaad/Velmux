# Velmux — Feature Matrix

The catalog is core-owned and product-controlled: an OS adding a codec
never makes a new format appear automatically. Runtime discovery only
selects execution paths among intentional features, and the UI only ever
exposes combinations the engine validated for the current source.

Legend: **D** decode, **E** encode, **C** stream copy. Status legend:
**implemented** = code paths complete; **verified** = exercised by an
automated suite (host ctest, Android API 29/34 emulator instrumented
suites, iOS simulator XCTest — see `docs/TESTING.md`);
**device-conditional** = depends on the physical device's codec support,
which emulator/simulator runs do not prove.

## Status overview (V1 workflows)

| Workflow | Status |
|---|---|
| Video Converter (format/codec/resolution/frame rate, compression, audio tracks, subtitles, adjustments, metadata) | implemented; core processing verified on host + Android emulators; hardware codec routes device-conditional |
| Crop Video | implemented; verified (core geometry/crop cases in host suite) |
| Video Cut & Merge (Trim / Cut / Split / sequential Merge / spatial Merge) | implemented; real-media cases verified in the host suite and Android instrumented suite |
| Extract Audio | implemented; verified (standalone-audio validation cases) |
| Extract Frames | implemented; frame-identity cases verified (host suite) |
| Extract Subtitles | implemented; text extraction verified; representation support per table below |
| Audio Converter (format/codec/sample rate/channels, compression, adjustments, metadata) | implemented; core processing verified on host + Android emulators |
| Audio Cut & Merge (waveform-range Trim / Cut / Split / Merge) | implemented; sample-precision cases verified in the host suite |
| Jobs (unified active + previous, filters, retry/cancel/recover, outputs, share) | implemented; job store/scheduler/recovery verified in host suite; notification surface verified on Android emulators |
| Settings (appearance, defaults, presets, storage, notifications, diagnostics) | implemented |

## Video codecs

| Codec | D | E | C | Notes |
|---|---|---|---|---|
| H.264 / AVC | x | x | x | FFmpeg (x264) always; MediaCodec/VideoToolbox routes device-conditional |
| H.265 / HEVC | x | x | x | FFmpeg (x265); HDR passthrough where compatible |
| AV1 | x | x | x | dav1d decode; SVT-AV1 encode (not on the armeabi-v7a slice); native routes device-conditional |
| VP8 | x | x | x | |
| VP9 | x | x | x | |
| MPEG-4 Part 2 | x | x | x | |
| FFV1 | x | x | x | lossless |
| MJPEG | x | x | x | |
| MPEG-2, MPEG-1, H.263, VC-1, WMV, ProRes, DNxHD/DNxHR | x | – | x | legacy decode/copy only |

## Audio codecs

| Codec | D | E | C | Notes |
|---|---|---|---|---|
| AAC-LC | x | x | x | |
| HE-AAC | x | x | x | encode where the platform encoder provides it |
| MP3 | x | x | x | |
| Opus | x | x | x | |
| Vorbis | x | x | x | |
| FLAC / ALAC / WavPack | x | x | x | lossless |
| PCM | x | x | x | |
| AC-3 / E-AC-3 | x | x | x | E-AC-3 encode where supported |
| AMR-NB / AMR-WB | x | x | x | speech modes |
| DTS, TrueHD/MLP, WMA, APE, xHE-AAC | x | – | x | decode/copy (xHE-AAC decode where the decoder exists) |

## Containers

MP4 / M4V / MOV, MKV, WebM, AVI, MPEG-TS / MTS, FLV, 3GP (video); M4A,
Ogg, Opus, MP3, WAV, FLAC, AIFF, AMR, ASF/WMA (audio). Valid
codec/container pairs live in the core catalog
(`core/src/catalog/features.cpp`); invalid combinations are absent from
the UI selectors and rejected on direct injection.

## Subtitle representations

| Source | Extraction / output |
|---|---|
| SRT, WebVTT, ASS/SSA, MP4 timed text, TTML | standalone text output with compatible conversion (verified) |
| PGS | SUP representation where the source carries it |
| VobSub | IDX/SUB pair |
| DVB | supported bitmap representation |
| CEA-608/708 | supported where the container/decoder actually exposes the caption data — detection alone never advertises extraction |

No OCR, no authoring, no manual timing adjustment (V1 scope). Subtitles
are never silently discarded during ordinary conversion.

## Adjustments

Video: rotate, flip, playback speed (0.5x–2x, audio/subtitles kept in
sync, pitch preserved by default), reverse, HDR-to-SDR (genuine color
conversion + tone mapping through zimg; not tag-stripping). Audio:
volume, loudness normalization, fades, speed/pitch, reverse, silence
removal. Verified through the host suite's real-media adjustment cases.

## Compression

Presets High Quality / Balanced / Smaller File / Target File Size /
Custom, sharing one intent model across the converters and Extract
Audio. Disabled by default. Target-size uses the shared solver with
per-stream budgets, decimal units, and an honest "target not met"
outcome. Verified: policy unit tests + real-media target-size cases in
the host suite.

## Device-conditional capabilities (never claimed without hardware)

- MediaCodec / VideoToolbox hardware encode/decode routes: verified as
  real pipelines on emulators/simulator (software encoders there);
  physical-device codec behavior and performance are only provable on
  physical devices (the device suites exist for this).
- VideoToolbox HDR/HEVC specifics, thermal behavior under sustained
  hardware encoding, and per-device bitrate ceilings.
