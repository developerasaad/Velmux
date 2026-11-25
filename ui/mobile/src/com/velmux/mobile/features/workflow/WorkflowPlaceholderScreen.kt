package com.velmux.mobile.features.workflow

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.Icon
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.LabeledValue
import com.velmux.mobile.components.SourceInfoCard
import com.velmux.mobile.design.VelmuxIcons
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.navigation.Navigator

/**
 * EXPLICIT FOLLOW-UP SURFACE (task UI-2): the workflow configuration
 * screens. This placeholder is a real navigation destination with stable
 * routes (Destination.Workflow(operation, sessionId)) — it renders the
 * LIVE session state from the engine client (single source of truth) and
 * is clearly marked as pending, not shipped as complete.
 */
@Composable
fun WorkflowPlaceholderScreen(
    client: EngineClient,
    navigator: Navigator,
    operation: String,
    sessionId: String?,
    modifier: Modifier = Modifier,
) {
    val sessions by client.sessions.collectAsState()
    val snapshot = sessionId?.let { sessions[it] }

    Column(
        modifier = modifier
            .fillMaxSize()
            .padding(horizontal = 16.dp),
    ) {
        Spacer(Modifier.height(24.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            IconButton(onClick = { navigator.back() }) {
                Icon(
                    imageVector = VelmuxIcons.Collapse,
                    contentDescription = Strings.WORKFLOW_BACK,
                )
            }
            Text(
                text = com.velmux.mobile.features.jobs.operationLabel(operation),
                style = MaterialTheme.typography.headlineSmall,
            )
        }

        Spacer(Modifier.height(16.dp))
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Icon(
                imageVector = VelmuxIcons.Info,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
            )
            Text(
                text = Strings.WORKFLOW_PENDING_TITLE,
                style = MaterialTheme.typography.titleMedium,
            )
        }
        Spacer(Modifier.height(8.dp))
        Text(
            text = Strings.WORKFLOW_PENDING_BODY,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        Spacer(Modifier.height(16.dp))
        LabeledValue(
            label = Strings.WORKFLOW_PENDING_SESSION,
            value = sessionId ?: Strings.WORKFLOW_PENDING_NO_SESSION,
        )
        Spacer(Modifier.height(8.dp))
        LabeledValue(
            label = Strings.WORKFLOW_PENDING_STATE,
            value = snapshot?.let { "${it.state} (revision ${it.revision})" }
                ?: Strings.WORKFLOW_PENDING_NO_SESSION,
        )

        snapshot?.sources?.forEach { media ->
            Spacer(Modifier.height(8.dp))
            SourceInfoCard(media = media)
        }
        Spacer(Modifier.height(24.dp))
    }
}
