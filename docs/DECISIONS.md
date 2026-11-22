# Velmux — Decisions

Append-only decision log. Escalate conflicts; never silently change settled
decisions without evidence.

| ID | Date | Decision | Rationale |
|---|---|---|---|
| D-001 | 2026-11-21 | Monorepo with `core`, `backends/*`, `bridge`, `apps/mobile`, `tools/ffmpeg-builder` | Single canonical repo; C++ core reusable independent of RN |
| D-002 | 2026-11-21 | doctest for C++ unit tests | Header-only, fast compile, readable assertions; sufficient for core unit testing |
| D-003 | 2026-11-21 | Android min API 29 (Android 10) | Per spec; MediaCodec/AMediaExtractor surface stable at 29 |
| D-004 | 2026-11-21 | iOS deployment target 15 | VideoToolbox/AVFoundation APIs used by Velmux are stable on 15; Swift concurrency available; lowest sensible target for the architecture. Revisit if a required API demands higher |
| D-005 | 2026-11-21 | FFmpeg pinned at 7.1.x release line (see tools/ffmpeg-builder/versions.json) | Current stable LTS-line; contains all required codecs/filters; pinned by tarball + SHA-256 |
| D-006 | 2026-11-21 | FFmpeg consumed as libraries only; CLI never used for production conversion | Spec §29; capability manifest generated from actual build |
| D-007 | 2026-11-21 | Whole-pipeline executors allowed when one backend serves every stage of the best candidate; stage-composed hybrid execution targeted for M4 | Pragmatic vertical slice while keeping stage interfaces real |
| D-008 | 2026-11-21 | SQLite for durable job state; schema versioned, forward migrations | Spec §24; system lib on host, bundled on mobile |
| D-009 | 2026-11-21 | Host (Linux) builds of core + FFmpeg backend in CI as fast signal; Android emulator/instrumented tests and iOS simulator tests on their respective runners | Linux cannot build Apple projects; CI is authoritative cross-platform validation |
| D-010 | 2026-11-21 | Independent output validation: core validator + external ffprobe cross-check in integration tests | Spec §50: never validate output only with the code that produced it |
| D-011 | 2026-11-21 | External dependency pins recorded with SHA-256 checksums at first fetch (TOFU), documented for maintainer cross-check against upstream signatures | Reproducibility without pretending offline trust anchors exist |
| D-012 | 2026-11-21 | React Native app initialized from the community CLI template (New Architecture: Fabric/TurboModules on) | Spec §6 requires modern architecture |
| D-013 | 2026-11-22 | Subtitle EXTRACT policy is rejected by the planner (honest infeasibility) until sidecar extraction is implemented; Preserve/Remove/BurnIn are fully implemented and tested | Never silently drop streams (spec §14); fail loudly instead |
| D-014 | 2026-11-22 | Burn-in styling: SRT-like sources get force_style redirected to the discovered local font family (hermetic static builds have no system font provider); ASS/SSA keep authored styling | libass can only match fontsdir fonts by family without a provider; authored ASS styles (with embedded fonts) must not be overridden |
| D-015 | 2026-11-22 | Burn-in never stream-copies video (planner-enforced, executor-defended): subtitle rendering rewrites pixels | Correctness over speed; remux cannot burn |
| D-016 | 2026-11-22 | iOS harfbuzz builds with CoreText disabled: upstream 10.2.0's CMake port never defines HB_IOS, so the CoreText branch is dead and the macOS fallback records no frameworks | harfbuzz's internal shaper (what libass drives) is unaffected; revisit if harfbuzz moves to Meson |
