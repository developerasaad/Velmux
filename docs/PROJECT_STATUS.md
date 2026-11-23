# Velmux — Project Status

Honest, current state. Code and tests are the ultimate truth; this file
tracks them. See docs/AUDIT.md for the feature-by-feature audit.

## Milestone overview

| # | Milestone | State |
|---|---|---|
| 0 | Foundation | Complete |
| 1 | First real Android conversion | Implemented; instrumented tests now RUN on KVM emulators (API 29 + 34) via android-emulated.yml (first green run pending); physical-device validation pending |
| 2 | Capability system and planner | Complete at the core level (static feature registry, cached capability snapshots, per-request gating incl. trim/transforms/DSP, hybrid planning); host-validated |
| 3 | FFmpeg toolchain | Complete: artifacts build in CI, both app builds consume them |
| 4 | Hybrid pipeline proof | F→F proven end-to-end in CI; native stage workers (N→N/F→N/N→F) pending |
| 5 | Complete V1 media support | Transcode/remux/trim/resolution/fps-conversion/crop/rotate/flip/subtitles (text burn-in via libass, bitmap PGS/VobSub/DVB burn-in via overlay, preserve, extract, offsets)/metadata preservation all implemented + tested; CEA-608/708 extraction not implemented |
| 6 | Compression intelligence | Complete: goal presets, budget math, budget DRIVES encoding, one corrective retry, sample generation (engine side; UI exposure pending) |
| 7 | Persistence/recovery | Complete at the engine level (segmented transcode, crash-safe checkpoints, durable resume, fault-injection tested); true SIGKILL-restart E2E pending (sandbox cannot host the fork/exec protocol; teardown analogue covered) |
| 8 | Background and notifications | Implemented (hosts + notifications, CI-validated builds); on-device behavior pending |
| 9 | Batch scheduler | Scheduler complete (resource caps, hardware scarcity, generations, slot accounting fixed); batch UI pending |
| 10 | Premium UX | Screens functional; new engine knobs (crop/rotate/flip/audio DSP) lack UI exposure; polish ongoing |
| 11 | Performance measurement | Partial (planning benchmark + telemetry records) |
| 12 | Hardening | Truncated/disappearing-source/unwrapped-destination/corrupt-tail tests green; true ENOSPC pending |

## What actually works today (host-validated in CI)

- C++ core: 10/10 test suites (engine, planner, scheduler, job store,
  recovery, compression, capability registry, feature registry, bridge)
  plus the FFmpeg backend suite with REAL conversions cross-validated by
  independent ffprobe/MediaExtractor probes.
- FFmpeg backend: transcode (H.264/VP9/AV1 families, bitrate + quality
  rate control), remux, frame-exact + keyframe-snapped trims, resolution
  conversion, frame-rate conversion (grid re-timing), crop / rotation /
  flips (transform-aware geometry + validation), audio DSP (gain, fades,
  loudness normalization, pitch-preserving speed, silence removal, PCM
  bit depth), subtitle preserve/remove/extract/burn-in (text via libass;
  PGS/VobSub/DVB bitmap cues via the overlay path; timing offsets;
  external files), metadata preservation, segmented recovery with durable
  resume, sample generation, fit-under-size with a corrective retry.
- Hybrid execution: stage contracts, FFmpeg stage workers, core
  HybridExecutor; REAL F→F proof with ffprobe cross-validation.
- Android: app builds with the engine + FFmpeg linked; MediaCodec
  surface transcode, remux, inspection implemented; instrumented tests
  now execute on emulators (API 29 + 34) in CI.
- iOS: Debug AND Release simulator builds with the FFmpeg xcframework.
- Release engineering: assembleRelease/bundleRelease with R8 + resource
  shrinking + app-specific keeps, CI-secret signing with an explicit
  test-signed fallback, Release iOS builds (simulator + unsigned device),
  a gated release workflow (host tests -> artifacts -> checksums ->
  manifest -> GitHub Release), honest RC notes, signing documentation.

## Known limitations (do not pretend otherwise)

- Physical-device validation is pending for both platforms (the emulated
  CI covers the Android MediaCodec path on x86_64 system images).
- Native stage workers for hybrid (Android ImageReader decode, Apple
  encode stage) are not implemented; N→N / F→N / N→F remain unproven.
- Hybrid runs are not checkpointed (restart from zero on retry).
- True storage-exhaustion (ENOSPC) and a real SIGKILL-restart E2E are
  not yet covered (sandbox constraints documented in D-047's context;
  unwritable-destination and teardown-recovery analogues are green).
- CEA-608/708 captions are not detected/extracted (ride along on video
  copies implicitly).
- Batch/multi-select UI and UI exposure of the newest engine knobs
  (crop/rotate/flip/audio DSP/PCM depth) are pending; the bridge JSON
  contract already accepts them.
- The Android/iOS native executors route trimmed/transformed/DSP requests
  to FFmpeg via planner gating (never silently mis-executed).

## Environment notes

- Sandbox: Linux, no Android SDK/NDK locally; CI is authoritative for
  Android/iOS builds, emulator tests, and the pinned FFmpeg.
- Local host dev loop: a minimal FFmpeg 7.1.1 (libx264 + the filters the
  tests need) at /tmp/ff-local; the pinned full build remains CI's.
