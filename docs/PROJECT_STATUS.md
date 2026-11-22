# Velmux — Project Status

Honest, current state. Update at the end of each work session. Code and tests are
the ultimate truth.

## Milestone overview

| # | Milestone | State |
|---|---|---|
| 0 | Foundation (monorepo, RN app, C++ engine, bridge, backend skeletons, FFmpeg builder foundation, docs, CI) | Mostly complete; FFmpeg mobile artifacts pending (M3) |
| 1 | First real Android conversion (MP4/H.264/AAC via native APIs) | Not started (Android backend is an honest skeleton) |
| 2 | Capability system and planner | In progress (core-level, host-validated) |
| 3 | FFmpeg toolchain (Android/iOS artifacts + manifest) | In progress: build-android.sh + build-ios.sh written; artifacts building in CI (manual dispatch) |
| 4 | Hybrid pipeline proof (N→N, F→N, N→F, F→F) | Not started |
| 5 | Complete V1 media support | Partial (subtitle preserve/burn-in/offset done; extraction rejected honestly per D-013; bitmap burn-in pending) |
| 6 | Compression intelligence | Partial (budget math implemented + unit tested) |
| 7 | Persistence/recovery | Complete at the engine level (segmented transcode + crash-safe checkpoint commit + durable resume; REAL cancel/resume fault-injection test passes on CI and locally with ffprobe-verified full-length output) |
| 8 | Background and notifications | Not started |
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
  in-run), mobile TS checks, Android APK, iOS simulator build, FFmpeg mobile
  artifacts (manual dispatch, building now).

## Known limitations (do not pretend otherwise)

- Android/iOS backends are skeletons: real interfaces and capability
  reporting, but no working device conversion yet (M1).
- The Android app links the engine WITHOUT the FFmpeg backend until the
  mobile artifacts are consumed (M3 wiring: `-PvelmuxFFmpegPrefix=...`);
  conversions on-device report honest "no compatible pipeline" errors until
  then.
- Hybrid stage-composed execution interfaces exist; only whole-pipeline
  FFmpeg execution is implemented (M4 pending).
- No background hosts/notifications yet (M8).
- FFmpeg mobile artifacts (out/android, out/ios) are being validated in CI
  for the first time; armeabi-v7a honestly omits SVT-AV1 (32-bit unsupported
  upstream), as does the x86_64 iOS simulator slice.
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
