package com.velmux.mobile.features.workflow

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.SessionSnapshot
import com.velmux.mobile.navigation.Navigator

/**
 * The shared host for every workflow configuration screen (UI-2): wires
 * the per-session controller, reconciles its options cache when the
 * revision moves, renders the fixed scaffold chrome (sources, estimate,
 * submit) and the related-change proposal; the screen itself composes
 * only its operation-specific configuration content.
 */
@Composable
fun WorkflowScreenHost(
    operation: String,
    client: EngineClient,
    navigator: Navigator,
    sessionId: String?,
    submitLabel: String,
    modifier: Modifier = Modifier,
    content: @Composable (snapshot: SessionSnapshot, controller: WorkflowController) -> Unit,
) {
    val sessions by client.sessions.collectAsState()
    val snapshot = sessionId?.let { sessions[it] }
    val scope = rememberCoroutineScope()
    val controller = remember(sessionId) {
        sessionId?.let { WorkflowController(it, client, navigator, scope) }
    }

    if (snapshot != null && controller != null) {
        LaunchedEffect(snapshot.revision) {
            controller.reconcileOptions(snapshot)
        }
    }

    WorkflowScaffold(
        operation = operation,
        client = client,
        navigator = navigator,
        snapshot = snapshot,
        submitLabel = submitLabel,
        onSubmit = { controller?.submit() },
        submitting = controller?.submitting?.collectAsState()?.value ?: false,
        modifier = modifier,
    ) {
        if (snapshot != null && controller != null) {
            RelatedChangeCard(controller = controller, snapshot = snapshot)
            content(snapshot, controller)
        }
    }
}
