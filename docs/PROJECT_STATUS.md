# Velmux — Project Status

Honest, current state. Update at the end of each work session. Code and tests are
the ultimate truth.

## Milestone overview

| # | Milestone | State |
|---|---|---|
| 0 | Foundation (monorepo, RN app, C++ engine, bridge, backend skeletons, FFmpeg builder foundation, docs, CI) | Complete (M3 artifact consumption closes the last gap) |
| 1 | First real Android conversion (MP4/H.264/AAC via native APIs) | Implemented (MediaCodec surface-path transcode, MediaExtractor inspection, MediaMuxer remux, MediaCodecList capability reporting via JNI); device/emulator validation pending (instrumented tests written; emulator CI infra pending) |
| 2 | Capability system and planner | In progress (core-level, host-validated) |
| 3 | FFmpeg toolchain (Android/iOS artifacts + manifest) | Implemented: artifacts build in CI (manual dispatch); both app builds consume them (push events) so shipped APKs and the iOS app carry the real FFmpeg backend. First fully-PIC rebuild in flight after the linker-fix round (D-030) |
| 4 | Hybrid pipeline proof (N→N, F→N, N→F, F→F) | Implemented at the composition level: stage contracts, FFmpeg stage workers, core HybridExecutor, planner hybrid candidates, real F→F proof with ffprobe validation in CI. Native-side participation (Android/Apple stage workers) and device proof of N-involving categories pending |
| 5 | Complete V1 media support | Partial (subtitle preserve/burn-in/offset done; TRIM implemented and tested; EXTRACTION implemented and CI-validated: SRT/WebVTT/ASS copy + mov_text/ttml decode to SRT sidecars, bitmap/CEA refused honestly; bitmap burn-in pending) |
| 6 | Compression intelligence | Partial (budget math implemented + unit tested; SAMPLE GENERATION implemented end-to-end: representative-window picker, engine op, bridge facade, TurboModule surface, real-file tests) |
| 7 | Persistence/recovery | Complete at the engine level (segmented transcode + crash-safe checkpoint commit + durable resume; REAL cancel/resume fault-injection test passes on CI and locally with ffprobe-verified full-length output) |
| 8 | Background and notifications | Implemented (hosts + notifications, CI-validated builds; on-device behavior pending): Android foreground service (dataSync) with aggregate progress + Cancel action + terminal notifications, process-owned engine surviving React teardown; iOS BGProcessingTask resume for INTERRUPTED jobs, beginBackgroundTask window, UNUserNotifications. Simulator/device behavior validation pending |
| 9 | Batch scheduler | Partial (resource-aware scheduler implemented + unit tested) |
| 10 | Premium UX polish | In progress (design system + screens scaffolded) |
| 11 | Performance measurement | Partial (planning micro-benchmark + telemetry) |
| 12 | Hardening | Not started |

## What actually works today

- C++ core builds on Linux with CMake/Ninja; 10/10 test suites pass (feature
  registry, capability registry, planner, compression budgets, scheduler, job
  state machine, pause/resume, shutdown-to-recovery handoff, job store,
  recovery). Full suite passes under ASan+UBSan with leak detection.
- FFmpeg backend performs REAL conversions through libav* libraries directly
  (demux → decode → process → encode → mux) on host, with output validation via
  the core validator and an independent ffprobe cross-check.
- Subtitles are first-class in the FFmpeg backend: PRESERVE copies every
  subtitle track through transcodes (language metadata + dispositions kept,
  planner-guaranteed container compatibility); BURN-IN renders embedded or
  external text subtitles into the video through a libavfilter graph with the
  pinned libass (fonts discovered per platform, VELMUX_FONTS_DIR override);
  timing offsets are applied to preserved packets or via SRT/ASS/VTT
  timestamp shifting for burn-in (subtitle_tools). Pixel-level tests verify
  rendered text; malformed inputs are refused, never mangled.
- The bridge C facade is complete and tested: JSON request parsing, plan,
  startJob, cancel/pause/resume, getJob/listJobs, describeBackends and
  event polling; bridge integration tests run a REAL conversion end-to-end
  through JSON exactly as mobile hosts will, with ffprobe cross-validation.
- Android app builds the C++ engine natively (gradle externalNativeBuild →
  monorepo CMake, embedded public-domain SQLite for the NDK) with a JNI
  adapter and a TurboModule (`VelmuxEngine`) implementing the codegen'd spec;
  durable engine storage under filesDir; event polling at UI cadence.
- The Android backend is a REAL MediaCodec engine (M1 implemented):
  MediaCodecList enumerated through JNI (the app's JNI_OnLoad registers
  the JavaVM), AMediaExtractor inspection, extractor→AMediaMuxer stream
  copy, and the zero-copy surface transcode (decoder renders into the
  encoder's input surface; audio through ByteBuffer PCM with cumulative
  PTS; cancel + stall guards; the muxer starts when both encoder formats
  are known). The planner routes to it only for pipelines it can truly
  serve (no scaling/rate/audio-format changes, no burn-in, no subtitle
  passthrough — capability flags enforced at both plan and execute time).
  Instrumented tests (real on-device transcode + remux + inspection with
  independent MediaExtractor validation) are written; they await device /
  emulator execution.
- iOS app builds in CI (CocoaPods + simulator build); engine wiring for iOS
  is the next integration step.
- Pinned host FFmpeg builds through `tools/ffmpeg-builder/build-host.sh`
  (all 16 pinned sources fetch+checksum-verified from automation-friendly
  mirrors; SourceForge/code.videolan.org are avoided because they block or
  serve byte-unstable content to CI).
- Recovery is REAL end-to-end (M7 engine-level): long transcodes run in
  independently finalized, validated, checkpoint-committed segments; a
  mid-run cancel leaves the committed prefix durable on disk and in
  SQLite; retry (or post-crash recovery) resumes from the still-valid
  prefix and re-encodes only the remainder; final assembly stream-copies
  the validated pieces through the concat demuxer (safe=0, core-generated
  paths); the whole flow is covered by a fault-injection test that cancels
  a real VP9 transcode at >=50%, verifies every committed segment with
  ffprobe, retries, and independently verifies the assembled output
  covers the FULL source duration. Checkpoint rows survive every state
  persist, never collide across runs (monotonic sequences), and are
  cleaned on completion and job deletion.
- React Native app: Home/Convert/Queue/History/Settings; tsc + eslint + jest
  green; durable SAF document picking (requestLongTermAccess).
- CI on GitHub Actions: core+tests (Linux, with the pinned FFmpeg built
  in-run), mobile TS checks, Android APK (with the FFmpeg artifacts), iOS
  simulator build (with the packaged FFmpeg xcframework), FFmpeg mobile
  artifacts (manual dispatch).
- Hybrid stage-composed execution (M4, composition level): the FFmpeg
  backend exposes demux/decode/encode/mux stage workers; the core-owned
  HybridExecutor composes them across backends through the Yuv420P
  CPU-frame interchange; the planner generates hybrid candidates only
  when no single-backend route exists. A REAL F→F composition runs in CI
  with ffprobe cross-validation.
- Trimming (spec §15/§16): TimeRange on the request, honored by the
  transcode path (single-range and segmented recovery tiling rebased
  onto the window) and the stream-copy path (keyframe-snapped start);
  persisted through job-store schema v2 (a submit→reload round-trip
  initially dropped it — caught by a local end-to-end run).
- Sample generation (spec §20): the engine plans against the FULL source
  (the preview uses exactly the settings the real job would), then
  transcodes only the densest window (demux-level per-second byte scan,
  single pass, no decode; mid-file fallback). Independent validation
  before reporting success. Exposed through the bridge facade and both
  mobile TurboModules on dedicated workers.
- The Android APK builds with the real FFmpeg backend linked in (M3
  complete): the workflow consumes the PIC-fixed artifacts from the
  latest successful ffmpeg-mobile run on main.

## Known limitations (do not pretend otherwise)

- M1 Android MediaCodec is implemented but awaits device/emulator
  execution (instrumented tests are written; emulator CI infra pending).
  The Apple backend (VideoToolbox/AVFoundation execution) compiles in CI;
  simulator/device execution tests are the next integration step.
- Hybrid (M4) composition is proven F→F with real files in CI; the
  native-side stage workers (Android ImageReader decode, Apple encoder
  stages) and the N→N / F→N / N→F device proofs are pending.
- Hybrid runs are not checkpointed (v1: restart from zero on retry);
  whole-pipeline executors keep the segmented-recovery duty.
- No background hosts/notifications yet (M8).
- The first FFmpeg-wired Android link exposed non-PIC artifacts; the
  builders are fixed (D-030) and the artifact rebuild is in flight.
  Until it lands, the Android APK build consumes the older non-PIC
  artifacts and fails at link — expected and tracked.
- Recovery fault-injection is covered at the executor level (mid-run
  cancel, durable resume, ffprobe-verified assembly); a full process-kill
  + restart end-to-end test is still pending (the reconcile/requeue logic
  itself is unit tested).

## Environment notes

- Sandbox: Linux, no Android SDK/NDK installed locally; CI is authoritative
  for Android/iOS builds (and now for the FFmpeg mobile artifacts too).
- Host FFmpeg dev packages unavailable; backend builds against the pinned
  FFmpeg produced by `tools/ffmpeg-builder` (correct per spec §28).
- The sandbox image happens to carry system fribidi/libass headers, which
  once masked a pkg-config search-path bug; CI's clean runners are the
  authoritative validator for the builder.
