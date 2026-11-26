package com.velmux.mobile

/**
 * All user-facing text (v1: single English Strings file — explicitly
 * allowed by spec UI-1; migrating to compose string resources happens with
 * the workflow screens). No text is scattered through screens.
 */
object Strings {
    const val APP_NAME = "Velmux"

    // -- navigation ------------------------------------------------------------
    const val NAV_HOME = "Home"
    const val NAV_JOBS = "Jobs"
    const val NAV_SETTINGS = "Settings"

    // -- home ------------------------------------------------------------------
    const val HOME_TITLE = "Velmux"
    const val HOME_SUBTITLE = "Video and audio, converted honestly"
    const val HOME_DOMAIN_VIDEO = "Video"
    const val HOME_DOMAIN_AUDIO = "Audio"
    const val HOME_ACTIVE_JOBS = "Active jobs"
    const val HOME_ACTIVE_JOBS_MAX_HINT = "Up to six jobs are shown here"
    const val HOME_SEE_ALL_JOBS = "See all in Jobs"
    const val HOME_RECENT_RESULTS = "Recent results"
    const val HOME_NO_JOBS = "No active jobs"
    const val HOME_NO_RESULTS = "No finished conversions yet"

    // -- workflow tiles ----------------------------------------------------------
    const val WORKFLOW_VIDEO_CONVERT = "Convert video"
    const val WORKFLOW_VIDEO_CONVERT_HINT = "Container, codec, quality"
    const val WORKFLOW_VIDEO_CROP = "Crop video"
    const val WORKFLOW_VIDEO_CROP_HINT = "Remove borders, reframe"
    const val WORKFLOW_VIDEO_CUTMERGE = "Cut & merge video"
    const val WORKFLOW_VIDEO_CUTMERGE_HINT = "Trim ranges, join files"
    const val WORKFLOW_EXTRACT_FRAMES = "Extract frames"
    const val WORKFLOW_EXTRACT_FRAMES_HINT = "Still images from video"
    const val WORKFLOW_EXTRACT_SUBTITLES = "Extract subtitles"
    const val WORKFLOW_EXTRACT_SUBTITLES_HINT = "Text tracks to files"
    const val WORKFLOW_EXTRACT_AUDIO = "Extract audio"
    const val WORKFLOW_EXTRACT_AUDIO_HINT = "Sound track as a file"
    const val WORKFLOW_AUDIO_CONVERT = "Convert audio"
    const val WORKFLOW_AUDIO_CONVERT_HINT = "Codec, bitrate, sampling"
    const val WORKFLOW_AUDIO_CUTMERGE = "Cut & merge audio"
    const val WORKFLOW_AUDIO_CUTMERGE_HINT = "Trim ranges, join files"

    const val PICK_VIDEO = "Pick video"
    const val PICK_VIDEOS = "Pick videos"
    const val PICK_AUDIO = "Pick audio"
    const val PICK_AUDIOS = "Pick audio files"
    const val PICKER_UNAVAILABLE = "The system file picker is not available here"

    // -- preparation states --------------------------------------------------------
    const val PREP_SELECTED = "Files selected"
    const val PREP_RESOLVING = "Resolving files…"
    const val PREP_IMPORTING = "Importing files…"
    const val PREP_INSPECTING = "Inspecting media…"
    const val PREP_CONFIGURING = "Ready to configure"
    const val PREP_READY = "Ready to convert"
    const val PREP_CANCELED = "Canceled"
    const val PREP_FAILED = "Could not prepare the conversion"
    const val PREP_CANCEL = "Cancel preparation"
    const val PREP_OPEN_CONFIG = "Open configuration"

    // -- jobs --------------------------------------------------------------------
    const val JOBS_TITLE = "Jobs"
    const val JOBS_SEARCH_HINT = "Search jobs"
    const val JOBS_FILTER_ALL = "All"
    const val JOBS_FILTER_ACTIVE = "Active"
    const val JOBS_FILTER_COMPLETED = "Completed"
    const val JOBS_FILTER_FAILED = "Failed"
    const val JOBS_EMPTY = "No jobs yet"
    const val JOBS_EMPTY_FILTERED = "No jobs match this filter"
    const val JOBS_ACTIVE_SECTION = "Active"
    const val JOBS_PREVIOUS_SECTION = "Previous"
    const val JOBS_OPEN = "Open"
    const val JOBS_SHARE = "Share"
    const val JOBS_CANCEL = "Cancel"
    const val JOBS_PAUSE = "Pause"
    const val JOBS_RESUME = "Resume"
    const val JOBS_RETRY = "Retry"
    const val JOBS_REUSE = "Reuse settings"
    const val JOBS_DELETE_RECORD = "Delete record"
    const val JOBS_DELETE_OUTPUT = "Delete output"
    const val JOBS_DETAILS = "Details"
    const val JOBS_HIDE_DETAILS = "Hide details"
    const val JOBS_OUTPUTS = "Outputs"
    const val JOBS_OUTPUT_KIND_MEDIA = "media"
    const val JOBS_OUTPUT_KIND_IMAGE = "image"
    const val JOBS_OUTPUT_KIND_SUBTITLE = "subtitle"
    const val JOBS_OUTPUT_KIND_MANIFEST = "manifest"
    const val JOBS_TARGET_NOT_MET = "Target not met"
    const val JOBS_ERROR_SECTION = "Failure"
    const val JOBS_PIPELINE = "Pipeline"
    const val JOBS_CANCELED = "Canceled"

    // -- settings -------------------------------------------------------------------
    const val SETTINGS_TITLE = "Settings"
    const val SETTINGS_APPEARANCE = "Appearance"
    const val SETTINGS_THEME = "Theme"
    const val SETTINGS_THEME_SYSTEM = "System"
    const val SETTINGS_THEME_LIGHT = "Light"
    const val SETTINGS_THEME_DARK = "Dark"
    const val SETTINGS_CONVERSION = "Conversion defaults"
    const val SETTINGS_GOAL_PRESET = "Default goal"
    const val SETTINGS_OUTPUT_CONTAINER = "Default output container"
    const val SETTINGS_STORAGE = "Storage"
    const val SETTINGS_MANAGED_OUTPUTS = "Managed outputs"
    const val SETTINGS_MANAGED_OUTPUTS_EMPTY = "No managed outputs"
    const val SETTINGS_MANAGED_OUTPUTS_COUNT = "%1\$d job records"
    const val SETTINGS_CLEANUP = "Clean up"
    const val SETTINGS_NOTIFICATIONS = "Notifications"
    const val SETTINGS_NOTIFY_COMPLETION = "Notify when conversions finish"
    const val SETTINGS_NOTIFY_FAILURE = "Notify when conversions fail"
    const val SETTINGS_NOTIFICATIONS_NOTE =
        "Platform notifications follow the system notification settings"
    const val SETTINGS_DIAGNOSTICS = "Diagnostics"
    const val SETTINGS_ENGINE_VERSION = "Engine version"
    const val SETTINGS_ABI_VERSION = "ABI version"
    const val SETTINGS_ABI_OK = "Matches the app contract (v2)"
    const val SETTINGS_ABI_MISMATCH = "Does not match the app contract (v2)"
    const val SETTINGS_BACKENDS = "Backends"
    const val SETTINGS_BACKENDS_NONE = "No backends reported"
    const val SETTINGS_ABOUT = "About"
    const val SETTINGS_APP_VERSION = "App version"
    const val SETTINGS_REPOSITORY = "Repository"
    const val SETTINGS_REPOSITORY_URL = "velmux.local (private repository)"
    const val SETTINGS_LICENSES = "Licenses"
    const val SETTINGS_LICENSES_TEXT =
        "Velmux uses FFmpeg (LGPL/GPL components per build configuration), " +
            "Kotlin and Compose Multiplatform (Apache 2.0), and SQLite (public domain)."

    // -- errors / banners -------------------------------------------------------------
    const val BANNER_ENGINE_UNAVAILABLE =
        "No engine is available in this environment"
    const val BANNER_DISMISS = "Dismiss"
    const val ERROR_RETRY = "Retry"

    // -- workflow screens (UI-2: the eight primary workflows) --------------------
    const val WORKFLOW_BACK = "Back"
    const val WORKFLOW_NO_SESSION = "No open configuration"
    const val WORKFLOW_NO_SESSION_BODY =
        "This workflow has no session attached. Pick media from Home to " +
            "start it."
    const val WORKFLOW_UNKNOWN_TITLE = "Unknown workflow"
    const val WORKFLOW_UNKNOWN_BODY =
        "The engine reported an operation this app version does not " +
            "provide a screen for (%s)."

    const val INPUT_APPLY = "Apply"

    // -- related changes (mandate §14.1) -------------------------------------------
    const val RELATED_CHANGE_TITLE = "A related change is required"
    const val RELATED_CHANGE_KEEP = "Keep current"
    const val RELATED_CHANGE_APPLY = "Apply change"

    // -- fixed converter layout (mandate §19 / §31) --------------------------------
    const val VIDEO_PRIMARY_SECTION = "Video output"
    const val AUDIO_PRIMARY_SECTION = "Audio output"
    const val SECONDARY_SECTION = "More options"
    const val FIELD_FORMAT = "Format"
    const val FIELD_CODEC = "Codec"
    const val FIELD_RESOLUTION = "Resolution"
    const val FIELD_FRAME_RATE = "Frame rate"
    const val FIELD_SAMPLE_RATE = "Sample rate"
    const val FIELD_CHANNELS = "Channels"
    const val RESOLUTION_WIDTH = "Width"
    const val RESOLUTION_HEIGHT = "Height"
    const val CONVERT_ACTION = "Convert"
    const val COMPRESSION_SECTION_TITLE = "Reduce file size"

    // -- batch (mandate §40) ---------------------------------------------------------
    const val BATCH_FILES_SECTION = "Files"
    const val BATCH_SHARED_SECTION = "Shared settings"
    const val BATCH_VALUE_VARIES = "Varies"
    const val BATCH_NO_COMMON_OPTIONS = "No common value — adjust files individually"
    const val BATCH_ADJUST = "Adjust"
    const val BATCH_SUBMIT_ALL = "Convert all"
    const val BATCH_FILE_READY = "Ready"
    const val BATCH_FILE_INVALID = "Needs attention"
    const val BATCH_READY_COUNT = "%d of %d ready"

    // -- compression (mandate §20) ---------------------------------------------------
    const val COMPRESSION_TITLE = "Reduce file size"
    const val COMPRESSION_OFF_HINT =
        "Off by default — compatible streams are preserved without " +
            "re-encoding"
    const val COMPRESSION_PRESET = "Goal"
    const val COMPRESSION_TARGET_SIZE = "Target file size"
    const val COMPRESSION_VIDEO_BITRATE = "Video bitrate"
    const val COMPRESSION_AUDIO_BITRATE = "Audio bitrate"
    const val COMPRESSION_QUALITY = "Quality"

    // -- track sections (mandate §23 / §24 / §28 / §30) --------------------------------
    const val AUDIO_TRACKS_SECTION = "Audio tracks"
    const val SUBTITLES_SECTION = "Subtitles"
    const val EXTRACT_TRACKS_SECTION = "Tracks"
    const val TRACKS_NONE = "No tracks of this kind"
    const val TRACK_FORMAT = "Track %d"

    // -- subtitle policies (mandate §24) ------------------------------------------------
    const val SUBTITLE_POLICY_PRESERVE = "Preserve"
    const val SUBTITLE_POLICY_BURN_IN = "Burn into video"
    const val SUBTITLE_POLICY_REMOVE = "Remove"
    const val SUBTITLE_TRACKS_NONE = "No subtitle tracks"
    const val SUBTITLE_DEFAULT = "Default"
    const val SUBTITLE_FORCED = "Forced"
    const val SUBTITLE_TARGET_SECTION = "Output format"
    const val SUBTITLE_TARGET_ORIGINAL = "Original representation"
    const val SUBTITLE_TARGET_SRT = "SRT"
    const val SUBTITLE_TARGET_VTT = "WebVTT"
    const val SUBTITLE_TARGET_ASS = "ASS"
    const val SUBTITLES_DEFAULT_STEM = "subtitles"

    // -- adjustments (mandate §25 / §32) ---------------------------------------------------
    const val ADJUSTMENTS_SECTION = "Adjustments"
    const val ADJUST_ROTATION = "Rotation (degrees)"
    const val ADJUST_FLIP_H = "Flip horizontally"
    const val ADJUST_FLIP_V = "Flip vertically"
    const val ADJUST_SPEED = "Playback speed"
    const val ADJUST_REVERSE = "Reverse"
    const val ADJUST_HDR_TO_SDR = "Convert HDR to SDR"
    const val ADJUST_HDR_TO_SDR_HINT =
        "Tone-maps HDR (PQ/HLG) to standard dynamic range"
    const val ADJUST_VOLUME = "Volume (dB)"
    const val ADJUST_NORMALIZE = "Normalize loudness"
    const val ADJUST_NORMALIZE_HINT = "Measured loudness to -16 LUFS"
    const val ADJUST_FADE_IN = "Fade in (ms)"
    const val ADJUST_FADE_OUT = "Fade out (ms)"
    const val ADJUST_PRESERVE_PITCH = "Preserve pitch"
    const val ADJUST_REMOVE_SILENCE = "Remove silence"
    const val ADJUST_REMOVE_SILENCE_HINT =
        "Removes intervals quieter than -45 dBFS (600 ms or longer)"

    // -- metadata (mandate §33) ------------------------------------------------------------
    const val METADATA_SECTION = "Metadata"
    const val META_TITLE = "Title"
    const val META_ARTIST = "Artist"
    const val META_ALBUM = "Album"
    const val META_GENRE = "Genre"
    const val META_DATE = "Date"
    const val META_DESCRIPTION = "Description"
    const val META_LOCATION = "Location"
    const val META_PLACEHOLDER = "Leave unchanged"

    // -- timeline / cut & merge (mandate §27) ----------------------------------------------
    const val TIMELINE_MODE_SECTION = "Operation"
    const val TIMELINE_MODE_TRIM = "Trim"
    const val TIMELINE_MODE_CUT = "Cut"
    const val TIMELINE_MODE_SPLIT = "Split"
    const val TIMELINE_MODE_MERGE = "Merge"
    const val RANGE_SECTION = "Range"
    const val REMOVED_SECTION = "Removed intervals"
    const val SPLIT_SECTION = "Split rules"
    const val MERGE_SECTION = "Merge layout"
    const val RANGE_START = "Start"
    const val RANGE_END = "End"
    const val RANGE_OPEN_END = "To end"
    const val RANGE_ADD = "Add"
    const val RANGE_REMOVE = "Remove interval"
    const val SPLIT_PART_COUNT = "Number of parts"
    const val SPLIT_PART_DURATION = "Part duration"
    const val SPLIT_MAX_SIZE = "Maximum part size"
    const val SPLIT_BOUNDARY = "Boundary"
    const val MERGE_LAYOUT = "Layout"
    const val MERGE_LAYOUT_SEQUENTIAL = "Sequential"
    const val MERGE_LAYOUT_SIDE_BY_SIDE = "Side by side"
    const val MERGE_LAYOUT_TOP_BOTTOM = "Top and bottom"
    const val MERGE_AUDIO = "Audio"
    const val MERGE_AUDIO_FIRST = "From the first video"
    const val MERGE_AUDIO_SECOND = "From the second video"
    const val MERGE_AUDIO_BOTH = "From both videos"
    const val MERGE_AUDIO_NONE = "No audio"
    const val MERGE_LONGER_DURATION = "Run to the longer duration"
    const val MERGE_LONGER_DURATION_HINT =
        "The finished side turns black after the shorter video ends"
    const val CUTMERGE_ACTION = "Run"

    // -- crop (mandate §26) --------------------------------------------------------------------
    const val CROP_SECTION = "Crop rectangle"
    const val CROP_LEFT = "Left"
    const val CROP_TOP = "Top"
    const val CROP_WIDTH = "Width"
    const val CROP_HEIGHT = "Height"
    const val CROP_RATIO = "Aspect ratio"
    const val CROP_RATIO_ORIGINAL = "Original"
    const val CROP_RESULT_FORMAT = "Result: %d × %d"
    const val CROP_ACTION = "Crop video"
    const val CROP_DEFAULT_NAME = "cropped"

    // -- extract frames (mandate §29) -----------------------------------------------------------
    const val FRAMES_SELECTION_SECTION = "Frames"
    const val FRAMES_MODE = "Mode"
    const val FRAMES_INTERVAL = "Interval"
    const val FRAMES_COUNT = "Number of frames"
    const val FRAMES_RANGE = "Range"
    const val IMAGE_OUTPUT_SECTION = "Image output"
    const val IMAGE_FORMAT = "Format"
    const val IMAGE_QUALITY = "Quality"
    const val IMAGE_MAX_WIDTH = "Maximum width"
    const val IMAGE_MAX_HEIGHT = "Maximum height"
    const val IMAGE_DIMENSIONS_ORIGINAL = "Original"
    const val EXTRACT_FRAMES_ACTION = "Extract frames"
    const val FRAMES_DEFAULT_STEM = "frame"

    // -- extract audio / subtitles ---------------------------------------------------------------
    const val EXTRACT_AUDIO_ACTION = "Extract audio"
    const val EXTRACT_SUBTITLES_ACTION = "Extract subtitles"
    const val EXTRACTED_AUDIO_DEFAULT_NAME = "audio"

    // -- output naming ------------------------------------------------------------------------------
    const val OUTPUT_SECTION = "Output"
    const val OUTPUT_NAME = "File name"
    const val OUTPUT_STEM = "Name stem"
    const val TRIMMED_DEFAULT_NAME = "trimmed"

    // -- shared -----------------------------------------------------------------------
    const val SIZE_UNKNOWN = "Unknown size"
    const val DURATION_UNKNOWN = "Unknown duration"
    const val NO_VALUE = "Not set"

    /** Formats a byte count the honest way (never fabricated). */
    fun formatBytes(bytes: Long): String = when {
        bytes < 0 -> SIZE_UNKNOWN
        bytes < 1024 -> "$bytes B"
        bytes < 1024 * 1024 -> "%.1f KB".fmt(bytes / 1024.0)
        bytes < 1024L * 1024 * 1024 -> "%.1f MB".fmt(bytes / (1024.0 * 1024))
        else -> "%.2f GB".fmt(bytes / (1024.0 * 1024 * 1024))
    }

    /** Formats seconds as m:ss / h:mm:ss (unknown when negative). */
    fun formatDuration(seconds: Double): String = when {
        seconds < 0 -> DURATION_UNKNOWN
        seconds < 60 -> "0:%02d".fmt(seconds.toInt())
        seconds < 3600 -> "%d:%02d".fmt(
            (seconds / 60).toInt(),
            (seconds % 60).toInt(),
        )
        else -> "%d:%02d:%02d".fmt(
            (seconds / 3600).toInt(),
            ((seconds % 3600) / 60).toInt(),
            (seconds % 60).toInt(),
        )
    }

    /** Formats a 0..1 progress fraction; negative shows nothing fabricated. */
    fun formatProgress(progress: Double): String = when {
        progress < 0 -> ""
        else -> "${(progress * 100).toInt()}%"
    }
}
