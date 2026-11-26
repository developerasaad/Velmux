package com.velmux.mobile.features.workflow

import com.velmux.mobile.fmt
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.FieldOptions
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.engine.JobStates
import com.velmux.mobile.engine.SessionSnapshot
import com.velmux.mobile.navigation.Destination
import com.velmux.mobile.navigation.Navigator
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

/**
 * The batch workflow driver (mandate §40): a group of independently
 * resolved operations sharing one user intent.
 *
 *  - A SHARED edit broadcasts one field change to every session; the
 *    engine validates each file independently (a file that cannot accept
 *    the change keeps its accepted state and shows its own status — it
 *    never blocks the others).
 *  - The shared display shows the COMMON accepted value per field, or
 *    "varies" when the files diverged (after per-file overrides or
 *    different source-driven defaults).
 *  - Shared option lists are the INTERSECTION across the files: a value
 *    selectable in the shared editor is one every file supports.
 *  - Individual overrides go through the normal single-file workflow
 *    screen (pushed on top of the batch route).
 *  - Submission creates one job per file with its own durable submission
 *    id; a failing file never invalidates the others.
 */
class BatchWorkflowController(
    val operation: String,
    private val entries: List<Destination.BatchEntry>,
    private val client: EngineClient,
    private val navigator: Navigator,
    private val scope: CoroutineScope,
) {

    private val sessionIds: List<String> = entries.map { it.sessionId }

    /** Snapshots of the batch's sessions (core-owned, per file). */
    fun snapshots(): List<Pair<Destination.BatchEntry, SessionSnapshot>> =
        entries.mapNotNull { entry ->
            val snapshot = client.sessions.value[entry.sessionId]
            snapshot?.let { entry to it }
        }

    fun snapshotOf(sessionId: String): SessionSnapshot? =
        client.sessions.value[sessionId]

    /**
     * The common accepted value for [field]: the valueId every file
     * accepts, or null when the files diverge (or the field is unset).
     */
    fun sharedValue(field: String): String? {
        var common: String? = null
        for (id in sessionIds) {
            val snapshot = client.sessions.value[id] ?: continue
            val value = snapshot.valueFor(field)?.valueId
            common = when (common) {
                null -> value
                value -> common
                else -> return null // varies
            }
        }
        return common
    }

    /**
     * The shared option list for [field]: the intersection of every
     * file's supported values at each file's current revision (a value
     * selectable here is one every file can accept; per-file-only values
     * are reachable through the individual override screen).
     */
    fun sharedOptions(field: String): List<String> {
        var intersection: Set<String>? = null
        for (id in sessionIds) {
            val options = optionsCache[id]?.get(field) ?: continue
            val ids = options.values.map { it.id }.toSet()
            intersection = when (intersection) {
                null -> ids
                else -> intersection intersect ids
            }
        }
        return intersection?.toList() ?: emptyList()
    }

    /** sessionId -> field -> options (loaded once per revision). */
    private val optionsCache = MutableStateFlow<Map<String, Map<String, FieldOptions>>>(emptyMap())
    val options: StateFlow<Map<String, Map<String, FieldOptions>>> = optionsCache

    /** Fields whose shared options are loading. */
    private val _loadingFields = MutableStateFlow<Set<String>>(emptySet())
    val loadingFields: StateFlow<Set<String>> = _loadingFields

    /** True while a batch submission is in flight. */
    private val _submitting = MutableStateFlow(false)
    val submitting: StateFlow<Boolean> = _submitting

    /** Loads [field]'s options for every file (per-file revisions may differ). */
    fun requestOptions(field: String) {
        if (_loadingFields.value.contains(field)) return
        _loadingFields.value = _loadingFields.value + field
        scope.launch {
            val perSession = mutableMapOf<String, FieldOptions>()
            for (id in sessionIds) {
                val loaded = client.sessionOptions(id, field)
                if (loaded != null && loaded.values.isNotEmpty()) {
                    perSession[id] = loaded
                }
            }
            optionsCache.value = optionsCache.value +
                perSession.mapValues { (id, opt) ->
                    (optionsCache.value[id] ?: emptyMap()) + (field to opt)
                }
            _loadingFields.value = _loadingFields.value - field
        }
    }

    /**
     * Broadcasts one shared field change to every session (mandate §40:
     * shared configuration = common user intent; per-file validation
     * decides acceptance independently).
     */
    fun broadcast(change: FieldChange) {
        for (id in sessionIds) {
            scope.launch {
                client.updateSession(id, listOf(change))
            }
        }
    }

    /** Applies a change to ONE file's session (individual override). */
    fun override(sessionId: String, change: FieldChange) {
        scope.launch {
            client.updateSession(sessionId, listOf(change))
        }

        // The shared display follows single-file edits through the session
        // snapshots; no separate state to maintain here.
    }

    /** Opens the single-file override screen for one entry. */
    fun openOverride(sessionId: String) {
        navigator.open(Destination.Workflow(operation, sessionId))
    }

    /**
     * Submits every session whose configuration is valid; files that are
     * not valid stay listed with their reason. Navigation to Jobs when at
     * least one job was created.
     */
    fun submitAll() {
        if (_submitting.value) return
        _submitting.value = true
        scope.launch {
            var submitted = 0
            for (id in sessionIds) {
                val snapshot = client.sessions.value[id] ?: continue
                if (!snapshot.configurationValid) continue
                if (client.submitSession(id) != null) {
                    submitted++
                }
            }
            _submitting.value = false
            if (submitted > 0) {
                navigator.open(Destination.Jobs)
            }
        }
    }

    /** True when at least one file's configuration is valid. */
    fun anyValid(): Boolean = sessionIds.any { id ->
        client.sessions.value[id]?.configurationValid == true
    }

    /** Per-file estimate label (unknown when the core cannot estimate). */
    fun estimateLabel(snapshot: SessionSnapshot): String {
        val estimate = snapshot.estimate
        return if (!estimate.known) {
            ""
        } else {
            val bytes = estimate.estimatedBytes
            when {
                bytes >= 1_000_000_000 -> "%.2f GB".fmt(bytes / 1e9)
                bytes >= 1_000_000 -> "%.1f MB".fmt(bytes / 1e6)
                bytes >= 1_000 -> "%.0f KB".fmt(bytes / 1e3)
                else -> "$bytes B"
            }
        }
    }

    /** True when the batch screen should show an active jobs summary. */
    fun hasOngoingJobs(): Boolean =
        client.jobs.value.any { it.state in JobStates.ONGOING }

    /** The display name for a session's source. */
    fun nameOf(sessionId: String): String =
        entries.firstOrNull { it.sessionId == sessionId }?.name ?: ""

    companion object {
        /**
         * The shared primary selectors per batchable operation (mirroring
         * the single-file screens' primary sections; everything else —
         * adjustments, metadata, tracks, ranges — is a per-file override).
         */
        val sharedFields: Map<String, List<String>> = mapOf(
            "video-convert" to listOf(Fields.FORMAT, Fields.CODEC, Fields.RESOLUTION, Fields.FRAME_RATE),
            "audio-convert" to listOf(Fields.FORMAT, Fields.CODEC, Fields.AUDIO_SAMPLE_RATE, Fields.AUDIO_CHANNELS),
            "extract-audio" to listOf(Fields.FORMAT, Fields.AUDIO_CODEC, Fields.AUDIO_SAMPLE_RATE, Fields.AUDIO_CHANNELS),
            "extract-frames" to listOf(Fields.IMAGE_FORMAT),
            "extract-subtitles" to listOf(Fields.SUBTITLE_TARGET),
        )
    }
}
