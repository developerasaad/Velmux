// Velmux core — configuration sessions, typed operation models and the
// supported-option contract.
//
// The core is the single source of truth for application media behavior
// (mandate §13): it derives which options are supported in a particular
// situation, validates every request and owns the accepted configuration.
// The UI displays those options and forwards user intentions; it never
// becomes an independent source of media truth.
//
// Session semantics (mandate §14): a session holds the selected operation,
// the source references, the inspected source information, the accepted
// user intentions with provenance, the applicable fields, the supported
// options, automatically resolved values, validation results, output
// estimates and a monotonically increasing configuration revision.
// Updates are transactional: validate -> resolve automatic dependencies ->
// validate the complete candidate -> commit or reject. Rejected changes
// leave the accepted configuration unchanged; stale revisions cannot
// overwrite newer accepted state.
#pragma once

#include <velmux/media.h>
#include <velmux/types.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace velmux::core {

// ---------------------------------------------------------------------------
// Operations — the approved V1 product surface (mandate §9)
// ---------------------------------------------------------------------------

// The eight primary workflows exposed on Home. Cut & Merge workflows host
// the four focused timeline operations (Trim, Cut, Split, Merge).
enum class Operation : std::uint8_t {
  VideoConvert,      // Home > Video > Video Converter
  VideoCrop,         // Home > Video > Crop Video
  VideoCutMerge,     // Home > Video > Cut & Merge
  ExtractAudio,      // Home > Video > Extract Audio
  ExtractFrames,     // Home > Video > Extract Frames
  ExtractSubtitles,  // Home > Video > Extract Subtitles
  AudioConvert,      // Home > Audio > Audio Converter
  AudioCutMerge,     // Home > Audio > Cut & Merge
};

const char *operationName(Operation op);
std::optional<Operation> operationFromName(const std::string &name);

// The focused timeline operations inside a Cut & Merge workflow.
enum class TimelineMode : std::uint8_t { Trim, Cut, Split, Merge };
const char *timelineModeName(TimelineMode mode);
std::optional<TimelineMode> timelineModeFromName(const std::string &name);

// Spatial composition for two-source video merges (mandate §27.5).
enum class MergeLayout : std::uint8_t { Sequential, SideBySide, TopBottom };

// Audio source selection for spatial merges.
enum class MergeAudioSelection : std::uint8_t {
  First,
  Second,
  Both,
  None,
};

// ---------------------------------------------------------------------------
// Compression intent (mandate §20) — one shared model for the Video
// Converter, the Audio Converter and Extract Audio.
//
// Compression is disabled by default. Presets are user-facing; encoder
// calibration (CRF/CQ/QScale factors, bitrate policies) is internal.
// ---------------------------------------------------------------------------

enum class CompressionPreset : std::uint8_t {
  HighQuality,
  Balanced,
  SmallerFile,
  TargetSize,
  Custom,
};

const char *compressionPresetName(CompressionPreset preset);
std::optional<CompressionPreset> compressionPresetFromName(
    const std::string &name);

struct CompressionIntent {
  bool enabled = false;
  CompressionPreset preset = CompressionPreset::Balanced;
  // Decimal-unit maximum output size (mandate §21: KB=1000, MB=1e6, GB=1e9).
  std::int64_t targetSizeBytes = 0;
  // Custom user-facing requirements (only honored with preset Custom).
  std::optional<std::int32_t> quality;  // 1..100 user quality scale
  std::optional<std::int64_t> videoBitrate;  // bits per second
  std::optional<std::int64_t> audioBitrate;  // bits per second
};

// ---------------------------------------------------------------------------
// Adjustments (mandate §25 video, §32 audio)
// ---------------------------------------------------------------------------

struct VideoAdjustments {
  std::int32_t rotationDegrees = 0;  // 0/90/180/270 clockwise
  bool flipHorizontal = false;
  bool flipVertical = false;
  // Playback speed, 0.5..2.0. Effective duration = duration / speed. Video,
  // audio and subtitle timing stay consistent; pitch is preserved.
  std::optional<double> playbackSpeed;
  // Reverse presentation order (audio reversed with it by default).
  bool reverse = false;
  // Genuine HDR -> SDR conversion (tone mapping), not tag removal.
  bool hdrToSdr = false;
};

struct AudioAdjustments {
  std::optional<double> gainDb;  // volume, decibels
  bool normalize = false;        // measured-loudness normalization
  std::optional<std::int32_t> fadeInMs;
  std::optional<std::int32_t> fadeOutMs;
  std::optional<double> playbackSpeed;  // 0.5..2.0
  bool preservePitch = true;
  bool reverse = false;
  bool removeSilence = false;
};

// ---------------------------------------------------------------------------
// Metadata intentions (mandate §33) — familiar file details only
// ---------------------------------------------------------------------------

enum class MetadataFieldAction : std::uint8_t { Unchanged, Set, Clear };

struct MetadataIntention {
  MetadataFieldAction titleAction = MetadataFieldAction::Unchanged;
  std::string title;
  MetadataFieldAction artistAction = MetadataFieldAction::Unchanged;
  std::string artist;
  MetadataFieldAction albumAction = MetadataFieldAction::Unchanged;
  std::string album;
  MetadataFieldAction genreAction = MetadataFieldAction::Unchanged;
  std::string genre;
  MetadataFieldAction dateAction = MetadataFieldAction::Unchanged;
  std::string date;  // ISO-8601 with offset when known
  MetadataFieldAction descriptionAction = MetadataFieldAction::Unchanged;
  std::string description;
  MetadataFieldAction locationAction = MetadataFieldAction::Unchanged;
  std::string location;
  // Artwork: external image path, or nullopt to keep existing artwork.
  // empty string with action Set = clear artwork.
  MetadataFieldAction artworkAction = MetadataFieldAction::Unchanged;
  std::string artworkUri;
};

// ---------------------------------------------------------------------------
// Typed operation requests (mandate §15)
//
// Each request models one approved operation's user intention. Fields that
// do not belong to the operation are rejected during session updates and
// during direct request decoding.
// ---------------------------------------------------------------------------

struct OutputSpec {
  // Output filename (without directory). Sanitized by the output manager;
  // the extension is chosen from the actual output representation.
  std::optional<std::string> name;
  // Output container. nullopt = resolved automatically (the source
  // container when writable, otherwise the operation's preferred default).
  std::optional<Container> container;
};

// Stream-level video intent shared by VideoConvert (mandate §19).
struct VideoStreamIntent {
  // nullopt = Automatic: preserve the compatible source codec when the
  // container can hold it, otherwise resolve the container's preferred
  // codec.
  std::optional<VideoCodec> codec;
  // Resolution preset height (2160/1440/1080/720/480/360) or custom
  // dimensions. Original/absent = preserve geometry (never upscale).
  std::optional<std::int32_t> resolutionHeight;
  std::optional<std::int32_t> customWidth;
  std::optional<std::int32_t> customHeight;
  // nullopt = Original presentation timing (including VFR sources).
  std::optional<Rational> frameRate;
};

// Track-level audio management inside Video Converter (mandate §23).
struct AudioTrackRule {
  // Stable source track identity (index into the inspected audio track
  // list, or a negative sentinel handled by the core).
  std::int32_t sourceTrackIndex = -1;
  bool include = true;  // false removes this track
  // External audio added as a new track (aligned at video start).
  std::optional<std::string> externalUri;
  // Replace the referenced source track instead of adding.
  bool replace = false;
};
struct AudioSettings {
  std::optional<AudioCodec> codec;
  std::optional<std::int64_t> bitRate;  // bits per second
  std::optional<std::int32_t> sampleRate;
  std::optional<std::int32_t> channels;
  std::optional<std::int32_t> pcmBitDepth;  // 16/24/32 for PCM output
};

// Subtitle management inside Video Converter (mandate §24).
enum class SubtitleTrackPolicy : std::uint8_t {
  Preserve,
  Remove,
  Embed,  // compatible passthrough into the output container
  BurnIn,
};
struct SubtitleTrackRule {
  std::int32_t sourceTrackIndex = -1;
  SubtitleTrackPolicy policy = SubtitleTrackPolicy::Preserve;
  // External subtitle file added as an embed/burn-in source.
  std::optional<std::string> externalUri;
  // Burn-in appearance.
  std::optional<std::int32_t> fontSize;
  std::optional<std::string> color;  // "#RRGGBB"
  std::optional<std::int32_t> position;  // 0..100 vertical position
};

// A retained timeline interval, half-open [startUs, endUs) (mandate §27).
// endUs < 0 means open-ended (until end of stream).
struct TimeRange {
  std::int64_t startUs = 0;
  std::int64_t endUs = -1;
  bool valid() const { return startUs >= 0 && (endUs < 0 || endUs > startUs); }
};

// Split boundary strategies (mandate §27.3).
enum class SplitMode : std::uint8_t {
  EqualParts,       // equal number of parts
  Duration,         // specified part duration
  MaxSize,          // maximum file size per part (decimal bytes)
  CustomTimestamps, // explicit boundary list
  Chapters,         // existing chapter boundaries
  Scenes,           // detected scene boundaries (only when requested)
};

struct SplitRules {
  SplitMode mode = SplitMode::EqualParts;
  std::int32_t partCount = 2;       // EqualParts
  std::int64_t partDurationUs = 0;  // Duration
  std::int64_t maxSizeBytes = 0;    // MaxSize
  std::vector<std::int64_t> boundaryUs;  // CustomTimestamps (ascending)
};

struct MergeRules {
  MergeLayout layout = MergeLayout::Sequential;
  // Sequential: use first source's compatible characteristics for
  // normalization. Spatial: two sources, common height (side-by-side) or
  // common width (top-bottom).
  MergeAudioSelection audio = MergeAudioSelection::First;
  // Spatial only: duration policy. True (default) = longer source; the
  // shorter source's region turns black after it ends. False = shorter.
  bool longerDuration = true;
};

struct CropIntent {
  // Crop rectangle in source display coordinates (post-rotation display
  // space, pixels, top-left origin). Positive dimensions inside the
  // source display bounds.
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  bool valid() const { return width > 0 && height > 0; }
};

// Frame extraction modes (mandate §29.2).
enum class FrameSelectionMode : std::uint8_t {
  Current,      // the previewed frame
  Selected,     // explicitly selected frames
  Interval,     // every intervalUs
  FrameRate,    // rational timestamp sampling
  Count,        // desired count over the range (midpoint for 1)
  AllInRange,   // every actual frame in the range
  Keyframes,    // actual random-access points
  SceneChanges, // detected scene changes (real analysis)
};

struct ImageOutputRules {
  enum class Format : std::uint8_t { Jpeg, Png, WebP };
  Format format = Format::Jpeg;
  // 1..100 for lossy; nullopt = codec default (lossless for PNG).
  std::optional<std::int32_t> quality;
  std::optional<std::int32_t> maxWidth;  // preserve aspect when set
  std::optional<std::int32_t> maxHeight;
  // True = sequential names (frame_0001), false = timestamp names.
  bool sequentialNaming = true;
};

struct FrameSelection {
  FrameSelectionMode mode = FrameSelectionMode::AllInRange;
  std::int64_t intervalUs = 0;         // Interval
  Rational frameRate;                  // FrameRate sampling
  std::int32_t count = 0;              // Count
  std::vector<std::int64_t> timestampsUs;  // Selected/Current
  TimeRange range;                     // selection window
};

// Subtitle extraction output representations (mandate §30).
enum class SubtitleTextTarget : std::uint8_t {
  Original, // keep the source representation when it stands alone
  Srt,
  Vtt,
  Ass,
};

// ---------------------------------------------------------------------------
// The per-operation request payloads
// ---------------------------------------------------------------------------

struct VideoConvertRequest {
  MediaSource source;
  OutputSpec output;
  VideoStreamIntent video;
  std::vector<AudioTrackRule> audioTracks;  // default: preserve all
  AudioSettings audioSettings;
  std::vector<SubtitleTrackRule> subtitleTracks;
  VideoAdjustments adjustments;
  CompressionIntent compression;
  MetadataIntention metadata;
};

struct AudioConvertRequest {
  MediaSource source;
  OutputSpec output;
  AudioSettings audio;
  AudioAdjustments adjustments;
  CompressionIntent compression;
  MetadataIntention metadata;
};

struct CropRequest {
  MediaSource source;
  OutputSpec output;
  CropIntent crop;
};

struct TrimRequest {
  MediaSource source;
  OutputSpec output;
  TimeRange range;
};

struct CutRequest {
  MediaSource source;
  OutputSpec output;
  // Removed intervals (normalized, non-overlapping, ascending).
  std::vector<TimeRange> removed;
};

struct SplitRequest {
  MediaSource source;
  OutputSpec output;
  SplitRules rules;
};

struct MergeRequest {
  std::vector<MediaSource> sources;  // 1..N sequential, exactly 2 spatial
  OutputSpec output;
  MergeRules rules;
};

struct ExtractAudioRequest {
  MediaSource source;
  OutputSpec output;
  // Empty = default track (designated default, else first meaningful).
  std::vector<std::int32_t> trackIndices;
  AudioSettings audio;
  std::optional<TimeRange> range;
  CompressionIntent compression;
  MetadataIntention metadata;
};

struct ExtractFramesRequest {
  MediaSource source;
  OutputSpec output;  // naming stem + output directory
  FrameSelection selection;
  ImageOutputRules image;
};

struct ExtractSubtitlesRequest {
  MediaSource source;
  OutputSpec output;
  // Empty = all subtitle tracks. Each selected track exports independently.
  std::vector<std::int32_t> trackIndices;
  // Per-track output representation; empty = Original.
  std::vector<SubtitleTextTarget> targets;
};

// ---------------------------------------------------------------------------
// Sessions (mandate §14)
// ---------------------------------------------------------------------------

// Field identifiers for supported-option queries and update payloads.
// Stable canonical identifiers; the UI localizes labels but never derives
// identifiers from display strings.
enum class FieldId : std::uint8_t {
  Format,          // output container
  Codec,           // video codec (video ops) / audio codec (audio ops)
  Resolution,
  FrameRate,
  CompressionEnabled,
  CompressionPreset,
  CompressionQuality,
  CompressionVideoBitrate,
  CompressionAudioBitrate,
  CompressionTargetSize,
  AudioCodec,
  AudioBitrate,
  AudioSampleRate,
  AudioChannels,
  AudioTracks,
  Subtitles,
  Adjustments,
  Metadata,
  OutputName,
  CropRect,
  TimelineMode,
  TimelineRanges,
  SplitRules,
  MergeLayout,
  MergeAudio,
  FrameSelection,
  ImageFormat,
  ImageQuality,
  ImageDimensions,
  SubtitleTracks,
  SubtitleTarget,
  ExtractAudioTracks,
  ExtractAudioRange,
};

const char *fieldIdName(FieldId field);
std::optional<FieldId> fieldIdFromName(const std::string &name);

// Where an accepted value came from (mandate §14.1).
enum class ValueOrigin : std::uint8_t {
  Automatic,  // resolved by the core from dependencies
  Default,    // built-in default
  Preset,     // applied from a stored preset
  User,       // explicit user selection
};

const char *valueOriginName(ValueOrigin origin);

// One selectable value of a field, as the core derived it.
struct OptionValue {
  std::string id;  // canonical identifier (never a display string)
  // Additional value payload when the field carries structured data
  // (e.g. a codec needs its still-encoding eligibility; a sample rate its
  // Hz value). Kept small and JSON-friendly.
  std::map<std::string, std::string> attributes;
};

struct FieldOptions {
  FieldId field = FieldId::Format;
  // Applicable, supported values for the current configuration. Unsupported
  // combinations are omitted, not shown disabled (mandate §13.3).
  std::vector<OptionValue> values;
};

// An accepted field value with provenance.
struct AcceptedValue {
  FieldId field = FieldId::Format;
  std::string valueId;  // canonical identifier of the accepted value
  ValueOrigin origin = ValueOrigin::Automatic;
  // Raw structured payload for fields that carry more than an id
  // (ranges, track rules, adjustments, metadata intentions...).
  std::string payloadJson;
};

// Session preparation/lifecycle state (mandate §16.1).
enum class SessionState : std::uint8_t {
  Created,     // operation + sources registered, not yet prepared
  Preparing,   // inspection running
  Ready,       // configuration possible
  Failed,      // preparation failed (source unreadable/invalid)
  Closed,
};

const char *sessionStateName(SessionState state);

// Output estimate (mandate §22). Estimates use the same resolved settings
// execution uses; unknown is honest, never fabricated.
struct OutputEstimate {
  bool known = false;
  std::int64_t estimatedBytes = -1;
  std::int64_t lowerBoundBytes = -1;
  std::int64_t upperBoundBytes = -1;
};

// A change the core requires before an incompatible update can be
// committed (mandate §14.1: explicit selections are never silently
// overwritten; the user must accept the related change first).
struct RelatedChange {
  FieldId field = FieldId::Codec;
  std::string reason;
  std::string currentValueId;
  std::string proposedValueId;
};

struct SessionSnapshot {
  std::string sessionId;
  Operation operation = Operation::VideoConvert;
  SessionState state = SessionState::Created;
  std::uint64_t revision = 0;
  // Inspected sources (filled during Preparing).
  std::vector<MediaInfo> sources;
  // Accepted values (authoritative; the UI renders these).
  std::vector<AcceptedValue> values;
  // Validation verdict for the current accepted configuration.
  bool configurationValid = false;
  std::string validationMessage;
  OutputEstimate estimate;
  // Set when the last update attempt requires an explicit related change.
  std::optional<RelatedChange> relatedChange;
  // Sample generated for the current revision (invalidated on change).
  std::optional<std::string> samplePath;
};

// Session update payload: one or more field changes, applied atomically.
struct FieldChange {
  FieldId field = FieldId::Format;
  std::string valueId;      // canonical identifier of the requested value
  std::string payloadJson;  // structured payload when the field carries one
};

} // namespace velmux::core
