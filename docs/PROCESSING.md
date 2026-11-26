# Velmux — Media Processing

How the authoritative C++ core executes media work. One architecture:
FFmpeg (direct libav* usage) plus native platform stages, composed by
one deterministic planner — never separate product engines, never a CLI
subprocess, never user-supplied FFmpeg arguments.

## Planning (deterministic routing)

1. Validate the accepted operation (the same validation the UI path
   uses; injected direct requests are rejected identically).
2. Determine which streams actually require changes.
3. Prefer compatible stream copying — per stream, not all-or-nothing
   (container conversion can copy video and audio; changing one audio
   track can preserve video; Extract Audio never decodes video).
4. Resolve required processing stages; select native/hardware routes
   when beneficial and supported, software otherwise (a single job may
   mix all three).
5. Execute with cancellation propagated into I/O, demuxing, decoding,
   filtering, encoding and muxing.
6. Fallback preserves accepted output semantics — a failing hardware
   HEVC encoder falls back to software HEVC, never silently to H.264 or
   a reduced resolution; if nothing compatible exists, the structured
   error says so.
7. Validate the output independently (below) before reporting success.

The user never selects an implementation. The planner uses cached
capabilities and known implementation characteristics — no continuous
benchmarking, no trial encodes during configuration.

## Configuration sessions

Sessions own the selected operation, source references, inspected
source information, accepted values, supported options, resolved
automatic values, validation results, output estimates and the
configuration revision. Updates are transactional: validate identity +
revision, construct a candidate, resolve affected automatic values,
validate the complete candidate, commit or reject — rejected changes
leave accepted state untouched; stale revisions cannot overwrite newer
state. Automatic values follow dependencies (format change resolves a
compatible codec; an explicit codec choice conflicting with a new
format surfaces a related-change proposal instead of being silently
replaced). Submissions carry durable idempotency identifiers.

## Processing semantics per workflow

**Converters (video/audio).** The fixed selector hierarchies
(Format/Codec/Resolution/Frame rate; Format/Codec/Sample rate/Channels)
render only core-supported options for the current source. Compression
reuses one intent model (disabled by default; High Quality / Balanced /
Smaller File / Target File Size / Custom) with codec-specific calibrated
policies (CRF/CQ/QScale per encoder for software, deterministic
dimension-based bitrates for hardware). Target File Size uses the shared
solver: decimal units, per-stream budgets (every retained audio track,
subtitle payloads, artwork, copied streams, container overhead),
a 0.05 safety margin, one bounded corrective attempt measured against
the ACTUAL output, and an honest "target not met" result when the
target is exceeded.

**Crop.** Photo-style cropping in stable source display coordinates:
free rectangles, ratio presets, rule-of-thirds guidance. The crop is
physical (never metadata hiding unwanted content), preserves format/
codec/audio/subtitles/timing, and a full-frame unchanged crop does not
trigger encoding.

**Cut & Merge (video and audio).** Retained intervals use the
half-open `[start, end)` convention with integer timestamps. Trim keeps
one interval; Cut removes intervals (complementary retained ranges
computed, overlapping/adjacent normalized); Split produces independently
valid parts (equal count, duration, max size, custom timestamps,
chapter/scene boundaries); sequential Merge concatenates in user order,
verifying real codec/bitstream compatibility (matching codec names are
insufficient) and normalizing when necessary with the first source's
characteristics, scale/pad (never stretch), silence fill and subtitle
offsets. Spatial merge composes exactly two sources side-by-side or
top-bottom with preserved aspect ratios, black fill after the shorter
source ends, and first/second/both/no audio. Boundaries respect actual
presentation order, decode dependencies, variable frame rates, nonzero
start times, time bases and audio encoder priming/padding; smart
boundary encoding is used only when codec configuration, random access,
timestamps and stitching can all be validated — otherwise the boundary
transcodes.

**Extract Audio.** All embedded audio tracks are inspected; the
designated default (or first meaningful) track is preselected; single or
multiple tracks export to independent files (never mixed). Compatible
original streams are copied into valid standalone representations
(AAC → M4A without re-encoding); conversion reuses the Audio Converter
machinery.

**Extract Frames.** Real frame identity: seeking goes to an earlier
decoding boundary and decodes forward to the exact presentation frame —
never the nearest keyframe or a cached thumbnail. Modes: current frame,
selected frames, by interval, by frame rate (rational sampling),
by desired count (distinct actual frames over the range — never
fabricated duplicates), all frames in range, actual keyframes, and
scene-change frames (real analysis only when requested). Output: JPEG /
PNG / still WebP with quality options, original or aspect-preserving
custom dimensions, orientation and pixel-aspect resolved, genuine SDR
conversion for HDR frames. Incremental bounded-memory decoding,
sequential and timestamp-based filenames, a per-frame manifest, and
recovery from already-validated images.

**Extract Subtitles.** Actual track inspection (language, title,
representation, dispositions); one or multiple tracks export
independently. Only valid standalone representations are exposed
(see `docs/FEATURE_MATRIX.md`); UTF-8 text output preserves Unicode,
timing and compatible styling; unavoidable representation losses are
disclosed. Video is never decoded when subtitles are container streams.

**Adjustments.** Video rotate/flip resolve orientation exactly once
(metadata-only when that reliably fulfills the request; pixel
transformation otherwise). Playback speed (0.5x–2x) keeps video, audio
and subtitle timing consistent with pitch preserved by default. Reverse
reverses actual presentation order with bounded memory (temporary
storage, never whole-video RAM). HDR-to-SDR performs genuine color
conversion and tone mapping. Audio follows the fixed processing order:
decode → retained timeline → reverse/speed → silence removal →
sample-rate/channels → volume → loudness normalization → fades → peak
protection → encode, with stages combined into one graph and no
intermediate re-encodes.

## Source access and inspection

Native file selection (SAF on Android, UIDocumentPicker on iOS);
direct access preferred (document URIs/file descriptors, security-
scoped references) with managed staging copies only when the source
requires it. Inspection reports actual container, streams, geometry,
rotation, timing, language, HDR and artwork facts — file extensions are
hints; unknown properties stay unknown. Results are cached per source
identity; ordinary configuration changes never re-inspect.

## Output management and validation

One output manager owns safe naming, extensions, collision avoidance,
temporary outputs, atomic publication, manifests, and native export/
share. Originals are never overwritten (identity checks, not string
comparison). Every completed operation requires independent validation
— container/codec/geometry/timing for conversions, actual size for
compression, geometry for crop, retained timeline for cuts, part set
for splits, ordering for merges, valid standalone audio, real frame
manifests for extraction, and actual embedded values for metadata.
