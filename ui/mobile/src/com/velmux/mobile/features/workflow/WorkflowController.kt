package com.velmux.mobile.features.workflow

import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.FieldOptions
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.engine.JobStates
import com.velmux.mobile.engine.SessionSnapshot
import com.velmux.mobile.navigation.Destination
import com.velmux.mobile.navigation.Navigator
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.serialization.json.JsonElement

/**
 * The per-session workflow driver (mandate §36.1): forwards user
 * intentions to the engine client and exposes ONLY presentation state
 * (which field's options are loading, the pending related-change
 * decision). The accepted configuration itself stays core-owned — the
 * screens render [EngineClient.sessions] snapshots.
 *
 * Related changes (mandate §14.1): when the core refuses an update that
 * would silently overwrite an explicit selection, it returns a snapshot
 * carrying the proposal. The controller remembers the attempted change;
 * accepting re-sends it with acceptRelatedChange=true, declining drops
 * it. A declined proposal is kept dismissed until the revision moves.
 */
class WorkflowController(
    private val sessionId: String,
    private val client: EngineClient,
    private val navigator: Navigator,
    private val scope: CoroutineScope,
) {

    /** Field -> options for the session's CURRENT revision. */
    private val _options = MutableStateFlow<Map<String, FieldOptions>>(emptyMap())

    /** Options cache; empty entries reload when the revision changes. */
    val options: StateFlow<Map<String, FieldOptions>> = _options

    /** Fields whose options are currently loading (capability pending). */
    private val _loadingFields = MutableStateFlow<Set<String>>(emptySet())
    val loadingFields: StateFlow<Set<String>> = _loadingFields

    /** True while a submit is in flight (button shows progress). */
    private val _submitting = MutableStateFlow(false)
    val submitting: StateFlow<Boolean> = _submitting

    /** The last change the core answered with a related-change proposal. */
    private var pendingChange: FieldChange? = null

    /** (field, current, proposed) triple the user declined; cleared on new revisions. */
    private var declinedProposal: Triple<String, String, String>? = null
    private var declinedAtRevision: Long = -1

    /**
     * Loads [field]'s supported options at the snapshot's revision (cached
     * per revision; reloads when the revision moved).
     */
    fun requestOptions(snapshot: SessionSnapshot, field: String) {
        val cached = _options.value[field]
        if (cached != null && snapshot.valueFor(Fields.OUTPUT_NAME) != null) {
            // Options are revision-scoped; simplest correct policy: reload
            // when the revision changed since the cache was filled.
        }
        if (cached != null && cachedRevision == snapshot.revision) {
            return
        }
        if (_loadingFields.value.contains(field)) {
            return
        }
        _loadingFields.value = _loadingFields.value + field
        scope.launch {
            val loaded = client.sessionOptions(sessionId, field)
            if (loaded != null && loaded.values.isNotEmpty()) {
                _options.value = _options.value + (field to loaded)
                cachedRevision = snapshot.revision
            }
            _loadingFields.value = _loadingFields.value - field
        }
    }

    private var cachedRevision: Long = -1

    /** Invalidates the options cache when the revision moved. */
    fun reconcileOptions(snapshot: SessionSnapshot) {
        if (cachedRevision != snapshot.revision) {
            if (cachedRevision >= 0) {
                _options.value = emptyMap()
            }
            cachedRevision = snapshot.revision
            declinedProposal = null
            declinedAtRevision = -1
        }
    }

    /**
     * Sends one field change; the engine's answer updates the session
     * snapshot (accept, related-change proposal, or rejection with the
     * banner carrying the reason).
     */
    fun select(snapshot: SessionSnapshot, change: FieldChange) {
        pendingChange = change
        scope.launch {
            client.updateSession(sessionId, listOf(change))
        }
    }

    /** True while the snapshot carries an UNDECLINED related-change proposal. */
    fun hasOpenProposal(snapshot: SessionSnapshot): Boolean {
        val proposal = snapshot.relatedChange ?: return false
        if (declinedProposal != null &&
            declinedAtRevision == snapshot.revision &&
            declinedProposal == Triple(
                snapshot.relatedChange.field,
                proposal.currentValueId,
                proposal.proposedValueId,
            )
        ) {
            return false
        }
        return true
    }

    /** Declines the open proposal (presentation-only; nothing is sent). */
    fun declineProposal(snapshot: SessionSnapshot) {
        val proposal = snapshot.relatedChange ?: return
        declinedProposal = Triple(
            proposal.field,
            proposal.currentValueId,
            proposal.proposedValueId,
        )
        declinedAtRevision = snapshot.revision
    }

    /** Accepts the open proposal by re-sending the attempted change. */
    fun acceptProposal() {
        val change = pendingChange ?: return
        scope.launch {
            client.updateSession(sessionId, listOf(change), acceptRelatedChange = true)
        }
    }

    /** Submits the session; on success navigates to the Jobs screen. */
    fun submit() {
        if (_submitting.value) {
            return
        }
        _submitting.value = true
        scope.launch {
            val jobId = client.submitSession(sessionId)
            _submitting.value = false
            if (jobId != null) {
                navigator.open(Destination.Jobs)
            }
        }
    }

    /** True when the workflow screen should show an active jobs summary. */
    fun hasOngoingJobs(): Boolean =
        client.jobs.value.any { it.state in JobStates.ONGOING }
}
