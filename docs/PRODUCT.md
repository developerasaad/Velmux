# Velmux — Product

Velmux is a fully offline, cross-platform video and audio converter,
compressor and focused media-processing application for ordinary users.

## Mission

Ordinary users convert, compress, cut, merge, crop and extract media
without understanding codecs, containers, bitrate control, profiles,
pixel formats, subtitle representations, or platform media APIs. The
application stays simple on the surface while implementing
comprehensive, correct media processing internally.

The user specifies the desired result. The core determines which
configurations are supported and how the accepted operation executes.
There are no manual processing/implementation/backend selectors, no raw
FFmpeg arguments, no technical encoder tuning exposed anywhere — the
approved compression presets (High Quality / Balanced / Smaller File /
Target File Size / Custom) are the full control surface.

## Scope (fixed)

Two media domains on the Home screen, eight primary workflows:

**Video** — Video Converter, Crop Video, Cut & Merge (Trim / Cut /
Split / Merge), Extract Audio, Extract Frames, Extract Subtitles.

**Audio** — Audio Converter, Cut & Merge.

Main navigation: Home, Jobs (one unified screen for active and previous
work — no separate Queue/History), Settings.

Explicitly outside V1: a general-purpose editor, creative effects,
subtitle authoring or timing tools, OCR, standalone image
conversion/editing, animated GIF/WebP output, cloud processing, user
accounts, AI features. These are not "coming soon" — they are not in the
product.

## Real-world goals

- Make a video much smaller while preserving acceptable visual quality.
- Fit a video under an exact file-size limit (target size).
- Convert media into a compatible format.
- Remux without re-encoding when the streams already fit.
- Extract audio tracks without touching the video stream.
- Pull specific frames out of a video — real frame identity, not
  keyframe approximations.
- Cut, split and join media with exact boundaries and correct timing.
- Crop video like cropping a photo.
- Extract subtitle tracks in valid standalone representations.

## Behavior principles

- Everything works offline; no media leaves the device.
- Originals are never modified; outputs are new files.
- Supported options come from the core — the UI never offers a
  combination the engine cannot execute, and direct injected requests
  are rejected the same way.
- Compression is off by default (preserve media; re-encode only when a
  requested change requires it).
- Completion means validated output, not a successful library call.
- A target-size result that misses the target is reported as "target
  not met", never as success.
