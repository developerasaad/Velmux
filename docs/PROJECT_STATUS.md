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
| 4 | Hybrid pipeline proof | Stage contracts + HybridExecutor; F→F proven end-to-end in CI; native stage workers implemented (Android ImageReader/MediaCodec, Apple reader/writer) — hardware execution via emulator CI, physical devices pending |
| 5 | Complete V1 media support | Transcode/remux/trim/resolution/fps-conversion/crop/rotate/flip/subtitles (text burn-in via libass, bitmap PGS/VobSub/DVB burn-in via overlay, preserve, extract, offsets)/metadata preservation/strip/multi-track audio/replace-audio/cover art all implemented + host-tested; CEA-608/708 detection implemented (positive fixture needs broadcast sample; extraction not V1) |
| 6 | Compression intelligence | Complete: goal presets, budget math, budget DRIVES encoding, one corrective retry, sample generation (engine + Convert-screen preview UI) |
| 7 | Persistence/recovery | Complete at the engine level (segmented transcode, crash-safe checkpoints, durable resume, fault-injection tested); true SIGKILL-restart E2E pending (sandbox cannot host the fork/exec protocol; teardown analogue covered) |
| 8 | Background and notifications | Implemented (hosts + notifications + platform resource feeds into the scheduler, CI-validated builds); on-device behavior pending |
| 9 | Batch scheduler | Complete (resource caps, hardware scarcity, generations, slot accounting on every terminal path); batch multi-select UI shipped |
| 10 | Premium UX | Complete V1 surface: all five screens, trim UI, sample preview, crop/rotate/flip/DSP/PCM controls in Advanced, history actions, batch selection; full accessibility annotation pass (screen-reader behavior on device pending) |
| 11 | Performance measurement | Planning p95 gate runs in every host suite; telemetry PerfRecords accumulate; systematic startup/TTFS measurement pending |
| 12 | Hardening | Truncated/disappearing-source/unwrapped-destination/corrupt-tail/invalid-timestamps/very-large-metadata tests green; ENOSPC maps to terminal StorageFull (device full-disk E2E pending); true SIGKILL E2E pending |

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
  Also: metadata strip, multi-track audio with language tags, replace-audio
  on the video timeline, cover art (attach/preserve), CEA detection,
  H.264→HEVC (libx265). HDR PRESERVATION works (stream copy + tagged
  containers); HDR→SDR tone mapping is honestly NOT offered in the software
  path (needs libzimg — D-056; the planner refuses with a reason).
- Hybrid execution: stage contracts, FFmpeg + Android + Apple stage
  workers, core HybridExecutor; REAL F→F proof with ffprobe
  cross-validation; N→N/F→N/N→F composed through the same executor
  (emulator/device execution in CI).
- Runtime failure feedback: execution-attributable backend failures
  record a narrow config key (cooldown); the planner avoids live-failed
  routes — proven by engine test (record → reroute → narrowness).
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
  CI covers the Android paths on x86_64 system images).
- Hybrid runs are not checkpointed (restart from zero on retry).
- A real SIGKILL-restart E2E and a genuinely-full-disk device run are
  pending (unwritable-destination, teardown-recovery, and ENOSPC-mapping
  analogues are green).
- CEA-608/708 positive detection has no automated fixture (the pinned
  ffmpeg CLI cannot author A53 SEI); extraction is not V1 (OCR-class).
- HDR→SDR tone mapping is not offered: the pinned filter set has no
  PQ/HLG-capable converter (needs libzimg — D-056). The planner refuses
  with a reason; HDR preservation via copy works.
- 10-bit pixel formats are not yet wired through the feature registry
  (the backend's HDR-preserving transcode path exists but is not
  reachable from requests yet).
- The planner's cost model is deterministic scoring; measured-throughput
  learning (PerfRecords → planner) is not wired.
- Screen-reader behavior (VoiceOver/TalkBack) on device is pending; the
  annotation pass is complete.
- The Android/iOS native executors route trimmed/transformed/DSP requests
  to FFmpeg via planner gating (never silently mis-executed).

## Environment notes

- Sandbox: Linux, no Android SDK/NDK locally; CI is authoritative for
  Android/iOS builds, emulator tests, and the pinned FFmpeg.
- Local host dev loop: CI builds the pinned host FFmpeg and uploads it as
  an identity-named artifact (core.yml); local development downloads it
  into tools/ffmpeg-builder/out/host and `./scripts/test.sh` reuses it
  (the platform-level host identity validates the same artifact on any
  Linux x86_64 host). The local sandbox pins cmake<4 via pip.
- CI Linux runners are pinned to ubuntu-24.04: ubuntu-latest moved to
  26.04 in November 2026, where the Android emulator dies at boot.
