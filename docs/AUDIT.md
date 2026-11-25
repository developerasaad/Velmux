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
| Native (MediaCodec/VideoToolbox) encode/decode where device supports | IMPLEMENTED — HARDWARE VALIDATION PENDING | Android `backends/android/cpp/media_transcoder.cpp` (real surface-path MediaCodec); Apple `backends/apple/objcpp/apple_pipeline.mm` (real VideoToolbox/AVFoundation). Instrumented tests are GREEN on KVM emulators in CI (`android-emulated.yml`, API 29 + 34: cold launch + RN init marker + 6/6 tests + reinstall + relaunch, verified on commits 97f6799–8768d7f); physical-device runs remain pending |
| HEVC software encode via FFmpeg | IMPLEMENTED + VERIFIED | libx265 3.6 pinned in the builder (`tools/ffmpeg-builder/versions.json`; CMake-4 compat patch in `lib.sh`); real H.264→HEVC transcode test with engine validation + ffprobe cross-check in the host suite |

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
| Bitmap burn-in (PGS/VobSub/DVB) | IMPLEMENTED + VERIFIED | overlay-filter compositing of pre-decoded cues; hand-crafted PGS bitstream fixtures; pixel cross-checks; DVB/VobSub share the identical decode->composite path |
| CEA-608/708 | DETECTION IMPLEMENTED + VERIFIED (negative path); POSITIVE DETECTION PENDING REAL SAMPLE | Inspection probes A53 CC side data over a bounded 64-packet window and reports `hasClosedCaptions` (serialized through the bridge); a clean source honestly reports none (host test). Captions ride along on video stream copy. A positive-detection fixture requires media with real broadcast CC data (the pinned ffmpeg CLI cannot author A53 SEI); text extraction is NOT V1 (OCR-class problem, documented non-goal) |

## 5. Video operations (spec §9)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Codec conversion, bitrate control, quality compression | IMPLEMENTED + VERIFIED | FFmpeg executor CRF/bitrate paths (tested); native bitrate/quality paths hardware-pending |
| Resolution conversion / custom resolution | IMPLEMENTED + VERIFIED | FFmpeg downscale path (tested 144p); planner gating for native (capability `videoScaling=false`) |
| Frame-rate conversion | IMPLEMENTED + VERIFIED | FFmpeg transcode re-times onto a constant output grid (duplication/dropping), composes with burn-in + segmented recovery |
| Crop / rotation / flip | IMPLEMENTED + VERIFIED | CropRect/rotationDegrees/flip fields through the full stack; FFmpeg unified transform graph; honest out-of-frame refusal; pixel-verified flip test |
| Trim | IMPLEMENTED + VERIFIED | FFmpeg frame-exact + segmented + keyframe-snap copy paths tested; Android/Apple/hybrid executors refuse trimmed requests via canExecute + planner timeTrim gating (tested: native-only registries refuse, hybrid shapes refuse) |
| Remove/replace audio, audio extraction | IMPLEMENTED + VERIFIED (2026-11-24 correction: replace-audio was a dead control through the job-store round trip until D-059 — the request field was not persisted; verified for real after the round-trip fix) | remove+extract = tested; replace-audio: external lane paced onto the video timeline (host test: differently-long replacement, output follows video duration) |
| Multiple audio tracks / language tracks | IMPLEMENTED + VERIFIED (2026-11-24 correction: allTracks did not survive the job-store round trip until D-059; the eng+fra test passed only because the -1 default also keeps every lane — the field itself was lost) | allTracks carries every lane with language metadata (host test: eng+fra MKV) |
| Fast remux / stream copy | IMPLEMENTED + VERIFIED | remux tests; planner prefers copy |
| Batch processing | IMPLEMENTED + VERIFIED | engine+scheduler per-job isolation; Home multi-select batch conversion wired to real job submission (`jobsStore`, host tests for scheduler) |
| Metadata preservation | IMPLEMENTED + VERIFIED (2026-11-24 correction: stripMetadata was a dead control until D-059 — the flag was not persisted, strip requests silently preserved tags) | container+stream tags through transcode and remux (D-047); stripMetadata now verified end-to-end; hardening test with ~600 KB of container tags (the test itself SIGSEGV'd in CI from a dangling event subscriber — fixed with RAII subscriptions, D-059 notes) |
| HDR preservation where compatible | IMPLEMENTED + VERIFIED (copy path) | stream copy preserves bitstream+signaling (host test asserts bt2020/PQ tags survive remux) |
| HDR → SDR where supported | NOT OFFERED in this build (honest gating) | the pinned filter set has no PQ/HLG-capable converter: colorspace handles SDR gamma curves only, and tonemap needs float pixels that only zscale produces (libzimg is not wired — D-056). The planner refuses HDR→SDR routes with a reason instead of producing a broken or washed-out result; HDR PRESERVATION works (stream copy + 10-bit tagging, verified) |
| Output validation | IMPLEMENTED + VERIFIED | `core/src/validation.cpp` + independent ffprobe cross-checks in tests |

## 6. Audio operations (spec §10)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Codec conversion, bitrate, sample-rate, channels | IMPLEMENTED + VERIFIED (FFmpeg) | tested |
| CBR/VBR where meaningful | IMPLEMENTED + VERIFIED | bitrate CBR via rc_max_rate; quality modes VBR (CRF) |
| PCM bit depth | IMPLEMENTED + VERIFIED | 16/24/32-bit selects pcm_s16le/s24le/s32le (probed) |
| Gain / fade / speed (pitch-preserving) / loudness normalization / silence removal | IMPLEMENTED + VERIFIED | abuffer graph chain; gain via volumedetect delta, speed via duration ratio + honest combined-with-video refusal |
| Trim (audio) | IMPLEMENTED + VERIFIED | trim applies to audio in FFmpeg path |
| Metadata | IMPLEMENTED + VERIFIED | container + stream tags through transcode and remux |
| Cover art | IMPLEMENTED + VERIFIED (2026-11-24 correction: coverArtUri was lost in the job-store round trip until D-059, so the "host test" never actually exercised embedding — it ran for the first time after the SIGSEGV fix and exposed the loss; attached pictures also no longer count as content video in inspection/validation) | external image attaches with the attached_pic disposition; source pictures preserve on request (host test, now genuinely running) |
| Batch | IMPLEMENTED + VERIFIED (shared batch UI) | Home multi-select drives per-job scheduling |

## 7. Compression intelligence (spec §17)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Goal presets (Recommended/Smaller File/High Quality/Custom) | IMPLEMENTED + VERIFIED | `qualityFactorForGoal`; UI chips |
| Budget math + feasibility + unrealistic-target detection | IMPLEMENTED + VERIFIED | `computeBitrateBudget` + planner advisories (tested) |
| Fit Under Size: budget DRIVES encoding | IMPLEMENTED + VERIFIED | budget-derived bitrate parameterizes the real encode (deterministic on retry; samples preview it) |
| Second attempt when first exceeds target | IMPLEMENTED + VERIFIED | one corrective re-encode at 0.9x(target/actual); honest summary note on a still-missed target |
| Sample compression preview | IMPLEMENTED + VERIFIED | engine op + tests + TurboModule method + Convert-screen SamplePreviewCard (generates the exact request's 8 s densest-window preview, invalidates on draft change, share on iOS) |

## 8. Capability system & planner (spec §15/§16)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Feature registry (product-controlled) | IMPLEMENTED + VERIFIED | static catalog + tests |
| Capability registry, normalized snapshots | IMPLEMENTED + VERIFIED | registered once at engine creation; invalidate on re-registration |
| Runtime failure feedback into planning | IMPLEMENTED + VERIFIED | job manager records execution-attributable failures (narrow config key, cooldown, thread-safe registry); planner avoids live-failed routes single-backend and hybrid; engine test proves record → reroute → narrowness |
| Whole-pipeline cost model | IMPLEMENTED (deterministic scoring) — measured-throughput learning MISSING / INCOMPLETE | telemetry records PerfRecords; planner does not consult them |
| Hybrid planning | IMPLEMENTED + VERIFIED | planner hybrid candidates only when no single-backend route (tested) |

## 9. Hybrid pipeline (spec §12)

| Category | Status | Evidence |
|---|---|---|
| F→F | IMPLEMENTED + VERIFIED | real composition test + ffprobe cross-check in CI |
| Native stage workers (Android ImageReader decode, MediaCodec encode; Apple reader/writer stages) | IMPLEMENTED — HARDWARE VALIDATION PENDING | real workers: `backends/android/cpp/android_stages.cpp`, `backends/apple/objcpp/apple_stages.mm`; Android backend exposes `stages()`; JNI masked-engine test scaffolding executes stage pipelines on-device (emulator CI) |
| N→N / F→N / N→F | IMPLEMENTED — HARDWARE VALIDATION PENDING | stage contracts + HybridExecutor compose the routes; capability-masked engine tests select real workers; execution on emulator/device hardware in CI, physical devices pending |

## 10. Background & notifications (spec §22/§23)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Android foreground service + notifications + Cancel action | IMPLEMENTED + VERIFIED (build) — HARDWARE VALIDATION PENDING | `ConversionService.kt`; behavior on device untested (emulator CI being added) |
| iOS BGProcessingTask + notifications | IMPLEMENTED + VERIFIED (build) — HARDWARE VALIDATION PENDING | `VelmuxEngineHost.mm` |
| Resource updates from platform (thermal/battery/memory) | IMPLEMENTED — HARDWARE VALIDATION PENDING | ConversionService (Android thermal/battery listeners + memory) and VelmuxEngineHost (iOS thermal state + os_proc_available_memory) push debounced resource updates to the scheduler while work runs; behavior on real hardware pending |

## 11. Scheduler (spec §21)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Resource-aware concurrency | IMPLEMENTED + VERIFIED | slots free on every terminal path (`Scheduler::finish` → `eraseRunningLocked`, hardware count included); scheduler tests |
| Batch failure isolation | IMPLEMENTED + VERIFIED (per-job) | jobs run on independent worker threads |
| Batch UI | IMPLEMENTED + VERIFIED | Home multi-select + queue screen actions (pause/resume/cancel/retry per job) |

## 12. Recovery (spec §22)

| Requirement | Status | Evidence |
|---|---|---|
| Segmented transcode + checkpoints + durable resume | IMPLEMENTED + VERIFIED | fault-injection test: cancel ≥50%, ffprobe-verified segments, resume, full-length output |
| Process-kill + restart E2E (real SIGKILL) | MISSING / INCOMPLETE | teardown-based analogue only |
| Storage exhaustion (true ENOSPC) | IMPLEMENTED + VERIFIED (mapping) — device E2E pending | output-write failures carry the AVERROR code; ENOSPC surfaces as terminal `ErrorCode::StorageFull` (subtitle sidecar writes included); a genuinely-full-disk run requires a device/emulator with a small writable partition |
| Hybrid checkpoints | OUT OF SCOPE (documented D-028) | hybrid jobs restart from zero; non-hybrid long jobs keep segmented recovery |

## 13. Hardening (spec §23)

| Scenario | Status |
|---|---|
| Truncated source / disappearing source / unwritable destination / malformed media / bad sidecar / damaged checkpoint | IMPLEMENTED + VERIFIED (host CI) |
| Invalid timestamps fixture | IMPLEMENTED + VERIFIED | MPEG-TS with backward PTS jump mid-stream (fixture verified non-monotonic by packet-level ffprobe): converts honestly or fails with a real error; never fake success, hang, or crash |
| Very large metadata | IMPLEMENTED + VERIFIED (2026-11-24: this test SEGFAULTED in CI — doctest's never-entered tests count as skipped, so the earlier VERIFIED claim was wrong; now green under ASan/UBSan with the full 48-test suite actually running) | ~600 KB of container tags: remux preserves them, stripMetadata drops them, output duration/size cross-checked |
| Process termination | ANALOGUE VERIFIED (teardown-based); real SIGKILL-restart E2E pending a suitable environment (see §12) |
| Storage exhaustion | IMPLEMENTED + VERIFIED (mapping; see §12) |
| App upgrade / schema migration | IMPLEMENTED + VERIFIED (v1→v2 ALTER TABLE migration is exercised by job-store tests on old-schema DBs) |

## 14. Premium UX (spec §18/§19/§20)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Screens (Home/Convert/Queue/History/Settings) | IMPLEMENTED + VERIFIED | all five screens functional against the real engine (inspection, planning, jobs, history, settings); design system tokens + shared components |
| Advanced controls hidden by default, full control set | IMPLEMENTED + VERIFIED | full engine contract wired through computeRequest: codec, rate control, bitrate/quality, fps, profile, pixel format, bit depth, GOP, B-frames, backend preference, metadata strip, HDR policy; Advanced section collapsed by default |
| "Custom — based on [preset]" | IMPLEMENTED + VERIFIED | `computeGoalLabel()` |
| Trim UI | IMPLEMENTED + VERIFIED | Convert-screen TrimSection: toggle + start/end steppers (mm:ss), window summary, honest disabled states; plan re-estimates on trim change; engine trim paths host-verified |
| Sample preview flow | IMPLEMENTED + VERIFIED | SamplePreviewCard on the Convert screen (see §7) |
| History actions (Open/Play/Share) | IMPLEMENTED + VERIFIED | wired to real output locations (share/play on iOS, delete with file removal, re-use settings) |
| Accessibility pass | IMPLEMENTED — screen-reader validation pending | full annotation pass: one accessible element per row/card, control+value labels, job-scoped action labels, selected/disabled states announced; VoiceOver/TalkBack behavior on device pending |

## 15. Performance (spec §24/§25)

| Requirement | Status | Evidence / gap |
|---|---|---|
| Planning latency benchmark | IMPLEMENTED + VERIFIED | warm-path p95 assertion runs in every host-suite execution (`planner_tests.cpp`, CI-visible) |
| Startup / TTFS / bridge overhead / throughput / recovery measurements | PARTIAL | telemetry records backend throughput (PerfRecord) and planning timings; systematic startup/TTFS measurement + gates pending |
| UI responsiveness under load (event coalescing) | IMPLEMENTED (250 ms poll, bounded queue) — not validated under load |

## 16. Release (spec §26–29)

| Requirement | Status |
|---|---|
| Release build variants (Android APK+AAB, iOS Release) | IMPLEMENTED + VERIFIED | R8 + resource shrinking with app-specific keeps; CI-secret signing with explicit test-signed fallback; Release iOS builds (simulator + unsigned device) |
| Release workflow (gated, checksums, manifest) | IMPLEMENTED + VERIFIED | gated release workflow: host tests → artifacts → checksums → manifest → GitHub Release |
| GitHub Release with real artifacts | IMPLEMENTED + VERIFIED | RC v0.1.0-rc.1 shipped with real APK/AAB/xcframework + checksums; rc.2 pending the current CI cycle |

## 17. Platform validation

| Area | Status |
|---|---|
| Android build (APK with FFmpeg) | IMPLEMENTED + VERIFIED (CI) |
| Android instrumented tests executed | IMPLEMENTED + VERIFIED (CI, 2026-11-24) | `android-emulated.yml` boots KVM emulators on API 29 + 34 (google_apis x86_64), installs the app, verifies the cold launch (process alive + RN init marker + clean log), runs 6/6 instrumented engine tests (NativeConversionTest/HybridConversionTest with on-device encoder fixtures + independent MediaExtractor validation), reinstalls the app AGP uninstalls and re-verifies the relaunch — green on both API levels (runs 45483513108 / 45494973509 / 45504299023) |
| iOS simulator build with FFmpeg | IMPLEMENTED + VERIFIED (CI) |
| iOS simulator tests (engine execution) | IMPLEMENTED (VelmuxEngineTests XCTest target runs via `./scripts/test.sh --ios-sim`; first green run pending the bash-3.2 fix) |
| Physical-device validation (both platforms) | HARDWARE VALIDATION PENDING (documented; RC notes must state it) |
