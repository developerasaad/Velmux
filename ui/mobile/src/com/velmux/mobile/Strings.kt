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

    // -- workflow placeholder (follow-up surface) ------------------------------------
    const val WORKFLOW_PENDING_TITLE = "Configuration UI pending"
    const val WORKFLOW_PENDING_BODY =
        "This workflow's configuration screen is the follow-up task (UI-2). " +
            "The session is live in the engine client and this route is its " +
            "stable destination."
    const val WORKFLOW_PENDING_SESSION = "Session"
    const val WORKFLOW_PENDING_NO_SESSION = "No session attached"
    const val WORKFLOW_PENDING_STATE = "Session state"
    const val WORKFLOW_BACK = "Back"

    // -- shared -----------------------------------------------------------------------
    const val SIZE_UNKNOWN = "Unknown size"
    const val DURATION_UNKNOWN = "Unknown duration"
    const val NO_VALUE = "Not set"

    /** Formats a byte count the honest way (never fabricated). */
    fun formatBytes(bytes: Long): String = when {
        bytes < 0 -> SIZE_UNKNOWN
        bytes < 1024 -> "$bytes B"
        bytes < 1024 * 1024 -> "%.1f KB".format(bytes / 1024.0)
        bytes < 1024L * 1024 * 1024 -> "%.1f MB".format(bytes / (1024.0 * 1024))
        else -> "%.2f GB".format(bytes / (1024.0 * 1024 * 1024))
    }

    /** Formats seconds as m:ss / h:mm:ss (unknown when negative). */
    fun formatDuration(seconds: Double): String = when {
        seconds < 0 -> DURATION_UNKNOWN
        seconds < 60 -> "0:%02d".format(seconds.toInt())
        seconds < 3600 -> "%d:%02d".format(
            (seconds / 60).toInt(),
            (seconds % 60).toInt(),
        )
        else -> "%d:%02d:%02d".format(
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
