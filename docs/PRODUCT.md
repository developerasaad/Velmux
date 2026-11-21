# Velmux — Product

Velmux is a premium cross-platform video/audio/subtitle conversion and compression
application for ordinary users, with full technical control retained for power users.

## Mission

Ordinary users convert and compress media without understanding codecs, containers,
bitrate control, GOP structures, profiles, pixel formats, subtitle codecs, or platform
media APIs. Power users keep access to the real technical controls.

The user specifies the desired result. The engine decides how to execute it.

## Real-world goals

- Make a video much smaller while preserving acceptable visual quality.
- Fit a video under an exact upload/file-size limit.
- Convert media into a compatible format.
- Remux without re-encoding when possible.
- Extract or convert audio.
- Preserve or manipulate multiple audio tracks.
- Preserve, extract, convert, add, remove, or burn subtitles.
- Batch-process multiple media files.
- Continue long jobs in the background where the OS permits.
- Recover interrupted work as aggressively as technically possible.

## Non-goals (V1)

- PGS/VobSub → text subtitle OCR (not ordinary subtitle conversion).
- Automatic speech caption generation.
- Video editing (cutting together multiple sources, effects authoring).
- Fake/unlimited background execution against OS policy.

## Product surfaces

- **Home** — media selection, primary actions (Compress, Convert, Audio tools),
  active jobs, recent results. Never overloaded.
- **Conversion workspace** — preview, source info, goal, output format,
  compression/quality controls, resolution, audio, subtitles, estimation,
  Advanced (hidden by default), Start.
- **Queue** — queued, preparing, running, checkpointing, paused, recovering,
  interrupted, failed, completed.
- **History** — original vs output size, output metadata, open/play/share/reuse/delete.
- **Settings** — appearance, defaults, processing preference, recovery, storage,
  notifications, diagnostics.

## Compression UX

Goal-oriented: Recommended / Smaller File / Fit Under Size / High Quality / Custom.
No misleading generic "compression percentage". Fit Under Size computes a realistic
bitrate budget (duration, audio allocation, container overhead), determines feasibility,
may adjust resolution, validates the final output, and tells the user clearly when the
requested size is unrealistic.

## Presets vs direct controls

Presets express user intention. Direct controls express precise technical choices.
Exact resolution, exact bitrate, sample rate, channel layout, subtitle timing offset,
and exact codec selection use direct controls. Whenever a preset hides real parameters,
an Advanced section exposes the underlying controls. Advanced is hidden by default;
closing it never discards custom changes. A modified preset becomes
"Custom — based on <preset>".

## Platform promise

Background processing: continue where the OS permits and recover safely when execution
is interrupted. A crash must not mean restarting a multi-hour job from zero.
