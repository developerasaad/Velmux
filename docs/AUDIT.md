# Velmux — V1 Implementation Audit

Formal mapping of the original specification to the actual implementation. Statuses
are exactly one of:

- **IMPLEMENTED + VERIFIED** — real production code, real tests, validated behavior.
- **IMPLEMENTED — HARDWARE VALIDATION PENDING** — real code; the missing validation
  requires physical hardware this environment cannot provide (documented per item).
- **MISSING / INCOMPLETE** — must be implemented before V1 completion.

This file is updated as work lands; `git log` on this file is the audit history.
Evidence cites files/functions. Last full revision: see Git history.

## 1. Video codecs (matrix §8)

| Requirement | Status | Evidence / gap |
|---|---|---|
| H.264/HEVC/AV1/VP8/VP9/MPEG-4/MPEG-2/MPEG-1/H.263/VC-1/WMV/ProRes/DNxHD/FFV1/MJPEG decode+encode asymmetry | IMPLEMENTED + VERIFIED (FFmpeg path, host CI) | `core/src/feature_registry.cpp` catalog; real transcode tests `backends/ffmpeg/tests/ffmpeg_backend_tests.cpp`; remux/copy tests; planner rejects unsupported combos (`core/tests/planner_tests.cpp`) |
| Native (MediaCodec/VideoToolbox) encode/decode where device supports | IMPLEMENTED — HARDWARE VALIDATION PENDING | Android `backends/android/cpp/media_transcoder.cpp` (real surface-path MediaCodec); Apple `backends/apple/objcpp/apple_pipeline.mm` (real VideoToolbox/AVFoundation). Instrumented tests written (`NativeConversionTest.kt`) but never executed — emulator CI being added this session; physical-device runs remain pending |
| HEVC software encode via FFmpeg | MISSING / INCOMPLETE | `encoderNameFor()` pins no HEVC software encoder; HEVC encode is native-only today. Add libx265 or keep honest native-only routing |

## 2. Audio codecs (matrix §8)

| Requirement | Status | Evidence / gap |
|---|---|---|
| AAC/MP3/Opus/Vorbis/FLAC/ALAC/PCM/WavPack/AMR encode + full decode set | IMPLEMENTED + VERIFIED (FFmpeg path) | feature registry + pinned builder config (`tools/ffmpeg-builder/versions.json`), real encode tests (Opus/AV1 WebM path) |
| xHE-AAC decode where available | IMPLEMENTED + VERIFIED (honest) | treated as opportunistic per matrix; FFmpeg build lacks it → capability-gated, never offered |

## 3. Containers (matrix §8)

| Requirement | Status | Evidence / gap |
|---|---|---|
| MP4/M4V/MOV/MKV/WebM/AVI/TS/FLV/3GP/M4A/Ogg/MP3/WAV/FLAC (+ASF where meaningful) | IMPLEMENTED + VERIFIED | feature registry container compatibility; planner pair validation; remux tests |

## 4. Subtitles (matrix §8, §11)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Detect/preserve/remove/select, language metadata, dispositions | IMPLEMENTED + VERIFIED | inspection summary carries all subtitle tracks (D-040); PRESERVE copies tracks with metadata/dispositions (tested) |
| External import, timing offset, text conversion (SRT/VTT/ASS/SSA/mov_text/TTML) | IMPLEMENTED + VERIFIED | `subtitle_tools.cpp`; extraction tests incl. mov_text→SRT decode |
| Text burn-in via libass | IMPLEMENTED + VERIFIED | filter-graph burn-in, pixel-tested (embedded + external + offset) |
| Bitmap burn-in (PGS/VobSub/DVB) | MISSING / INCOMPLETE | currently refused honestly; being implemented this session |
| CEA-608/708 | MISSING / INCOMPLETE (detect only) | SEI CCs are not detected at inspection; ride along on video copy implicitly. Detection + honest routing being added |

## 5. Video operations (spec §9)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Codec conversion, bitrate control, quality compression | IMPLEMENTED + VERIFIED | FFmpeg executor CRF/bitrate paths (tested); native bitrate/quality paths hardware-pending |
| Resolution conversion / custom resolution | IMPLEMENTED + VERIFIED | FFmpeg downscale path (tested 144p); planner gating for native (capability `videoScaling=false`) |
| Frame-rate conversion | MISSING / INCOMPLETE | `video.frameRate` parsed/planner-tracked/UI-exposed but NO executor implements it |
| Crop / rotation / flip | MISSING / INCOMPLETE | request fields do not exist |
| Trim | IMPLEMENTED + VERIFIED (FFmpeg only) — MISSING guards elsewhere | FFmpeg frame-exact + segmented + keyframe-snap copy paths tested; **Android/Apple/Hybrid silently ignore trim** (no canExecute guard) — being fixed |
| Remove/replace audio, audio extraction | Partial: remove+extract IMPLEMENTED + VERIFIED; replace audio MISSING / INCOMPLETE | extraction = audio-only transcode (tested) |
| Multiple audio tracks / language tracks | MISSING / INCOMPLETE | only first audio stream processed |
| Fast remux / stream copy | IMPLEMENTED + VERIFIED | remux tests; planner prefers copy |
| Batch processing | MISSING / INCOMPLETE (UI) | engine+scheduler support jobs; no multi-select UI |
| Metadata preservation | MISSING / INCOMPLETE | only subtitle-stream metadata copied today |
| HDR preservation where compatible | IMPLEMENTED + VERIFIED (copy path) | stream copy preserves bitstream+ signaling |
| HDR → SDR where supported | MISSING / INCOMPLETE | transcode forces 8-bit YUV420P with no tone mapping |
| Output validation | IMPLEMENTED + VERIFIED | `core/src/validation.cpp` + independent ffprobe cross-checks in tests |

## 6. Audio operations (spec §10)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Codec conversion, bitrate, sample-rate, channels | IMPLEMENTED + VERIFIED (FFmpeg) | tested |
| CBR/VBR where meaningful | IMPLEMENTED + VERIFIED | bitrate CBR via rc_max_rate; quality modes VBR (CRF) |
| PCM bit depth | MISSING / INCOMPLETE | fixed pcm_s16le |
| Gain / fade / speed (pitch-preserving) / loudness normalization / silence removal | MISSING / INCOMPLETE | no request fields, no DSP |
| Trim (audio) | IMPLEMENTED + VERIFIED | trim applies to audio in FFmpeg path |
| Metadata / cover art | MISSING / INCOMPLETE | |
| Batch | MISSING / INCOMPLETE (UI) | |

## 7. Compression intelligence (spec §17)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Goal presets (Recommended/Smaller File/High Quality/Custom) | IMPLEMENTED + VERIFIED | `qualityFactorForGoal`; UI chips |
| Budget math + feasibility + unrealistic-target detection | IMPLEMENTED + VERIFIED | `computeBitrateBudget` + planner advisories (tested) |
| Fit Under Size: budget DRIVES encoding | MISSING / INCOMPLETE | computed budget is advisory only, never injected |
| Second attempt when first exceeds target | MISSING / INCOMPLETE | no re-plan/re-encode loop |
| Sample compression preview | IMPLEMENTED + VERIFIED (engine+bridge) — MISSING UI | engine op + tests + TurboModule method exist; no screen calls it |

## 8. Capability system & planner (spec §15/§16)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Feature registry (product-controlled) | IMPLEMENTED + VERIFIED | static catalog + tests |
| Capability registry, normalized snapshots | IMPLEMENTED + VERIFIED | registered once at engine creation; invalidate on re-registration |
| Runtime failure feedback into planning | MISSING / INCOMPLETE | `recordFailure` exists, never called from production paths |
| Whole-pipeline cost model | IMPLEMENTED (deterministic scoring) — measured-throughput learning MISSING / INCOMPLETE | telemetry records PerfRecords; planner does not consult them |
| Hybrid planning | IMPLEMENTED + VERIFIED | planner hybrid candidates only when no single-backend route (tested) |

## 9. Hybrid pipeline (spec §12)

| Category | Status | Evidence |
|---|---|---|
| F→F | IMPLEMENTED + VERIFIED | real composition test + ffprobe cross-check in CI |
| Native stage workers (Android ImageReader decode, MediaCodec encode; Apple reader/writer stages) | MISSING / INCOMPLETE | only FFmpeg exposes stage workers today |
| N→N / F→N / N→F | MISSING / INCOMPLETE | blocked on native stage workers + device/emulator execution |

## 10. Background & notifications (spec §22/§23)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Android foreground service + notifications + Cancel action | IMPLEMENTED + VERIFIED (build) — HARDWARE VALIDATION PENDING | `ConversionService.kt`; behavior on device untested (emulator CI being added) |
| iOS BGProcessingTask + notifications | IMPLEMENTED + VERIFIED (build) — HARDWARE VALIDATION PENDING | `VelmuxEngineHost.mm` |
| Resource updates from platform (thermal/battery/memory) | MISSING / INCOMPLETE | `updateResources` plumbed end-to-end, never called by hosts |

## 11. Scheduler (spec §21)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Resource-aware concurrency | PARTIALLY IMPLEMENTED — **slot leak bug**: natural completion never frees slots; hardware-job cap never enforced | `core/src/scheduler.cpp`, `engine.cpp` (finish only on cancel) |
| Batch failure isolation | IMPLEMENTED + VERIFIED (per-job) | jobs run on independent worker threads |
| Batch UI | MISSING / INCOMPLETE | |

## 12. Recovery (spec §22)

| Requirement | Status | Evidence |
|---|---|---|
| Segmented transcode + checkpoints + durable resume | IMPLEMENTED + VERIFIED | fault-injection test: cancel ≥50%, ffprobe-verified segments, resume, full-length output |
| Process-kill + restart E2E (real SIGKILL) | MISSING / INCOMPLETE | teardown-based analogue only |
| Storage exhaustion (true ENOSPC) | MISSING / INCOMPLETE | `StorageFull` error code never produced; unwritable-dir covers only open-for-write |
| Hybrid checkpoints | OUT OF SCOPE (documented D-028) | hybrid jobs restart from zero; non-hybrid long jobs keep segmented recovery |

## 13. Hardening (spec §23)

| Scenario | Status |
|---|---|
| Truncated source / disappearing source / unwritable destination / malformed media / bad sidecar / damaged checkpoint | IMPLEMENTED + VERIFIED (host CI) |
| Invalid timestamps fixture | MISSING / INCOMPLETE |
| Very large metadata | MISSING / INCOMPLETE |
| Process termination | MISSING / INCOMPLETE (see §12) |
| Storage exhaustion | MISSING / INCOMPLETE (see §12) |
| App upgrade / schema migration | IMPLEMENTED + VERIFIED (v1→v2 ALTER TABLE migration is exercised by job-store tests on old-schema DBs) |

## 14. Premium UX (spec §18/§19/§20)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Screens (Home/Convert/Queue/History/Settings) | IMPLEMENTED (scaffold level) — polish INCOMPLETE | design system tokens + components exist |
| Advanced controls hidden by default, full control set | PARTIAL: codec/rate-control/bitrate/quality/fps/backend preference work; profile/level/pixfmt/bitdepth/GOP/B-frames/metadata are UI-only "pending contract" (not sent to engine) |
| "Custom — based on [preset]" | IMPLEMENTED + VERIFIED | `computeGoalLabel()` |
| Trim UI | MISSING / INCOMPLETE | `trim` always null in requests |
| Sample preview flow | MISSING UI (engine ready) |
| History actions (Open/Play/Share) | MISSING / INCOMPLETE | rendered but disabled |
| Accessibility pass | INCOMPLETE |

## 15. Performance (spec §24/§25)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Planning latency benchmark | IMPLEMENTED (micro-benchmark exists) | CI-visible gates MISSING |
| Startup / TTFS / bridge overhead / throughput / recovery measurements | MISSING / INCOMPLETE | telemetry records backend throughput (PerfRecord); no systematic measurement/gates |
| UI responsiveness under load (event coalescing) | IMPLEMENTED (250 ms poll, bounded queue) — not validated under load |

## 16. Release (spec §26–29)

| Requirement | Status |
|---|---|
| Release build variants (Android APK+AAB, iOS Release) | MISSING / INCOMPLETE (debug-only builds; release buildType signs with debug key, R8 off) |
| Release workflow (gated, checksums, manifest) | MISSING / INCOMPLETE |
| GitHub Release with real artifacts | MISSING / INCOMPLETE |

## 17. Platform validation

| Area | Status |
|---|---|
| Android build (APK with FFmpeg) | IMPLEMENTED + VERIFIED (CI) |
| Android instrumented tests executed | MISSING / INCOMPLETE (written, never run; emulator CI being added) |
| iOS simulator build with FFmpeg | IMPLEMENTED + VERIFIED (CI) |
| iOS simulator tests (engine execution) | MISSING / INCOMPLETE (no test target) |
| Physical-device validation (both platforms) | HARDWARE VALIDATION PENDING (documented; RC notes must state it) |
