package com.velmux.mobile.features.workflow

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.EstimateCard
import com.velmux.mobile.components.SourceInfoCard
import com.velmux.mobile.design.VelmuxIcons
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.SessionSnapshot
import com.velmux.mobile.features.jobs.operationLabel
import com.velmux.mobile.navigation.Navigator

/**
 * The shared workflow screen chrome (mandate §19/§31 layouts): header with
 * back navigation, honest source cards, the related-change proposal card,
 * the validation message, the estimate and the stable action area. The
 * per-operation configuration content composes between the sources and
 * the estimate.
 */
@Composable
fun WorkflowScaffold(
    operation: String,
    client: EngineClient,
    navigator: Navigator,
    snapshot: SessionSnapshot?,
    submitLabel: String,
    onSubmit: () -> Unit,
    submitting: Boolean,
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
    ) {
        Spacer(Modifier.height(8.dp))
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconButton(onClick = { navigator.back() }) {
                Icon(
                    imageVector = VelmuxIcons.Collapse,
                    contentDescription = Strings.WORKFLOW_BACK,
                )
            }
            Text(
                text = operationLabel(operation),
                style = MaterialTheme.typography.headlineSmall,
            )
        }

        Spacer(Modifier.height(8.dp))
        if (snapshot == null) {
            Spacer(Modifier.height(48.dp))
            Column(
                modifier = Modifier.fillMaxWidth(),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(
                    text = Strings.WORKFLOW_NO_SESSION,
                    style = MaterialTheme.typography.titleMedium,
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    text = Strings.WORKFLOW_NO_SESSION_BODY,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 32.dp),
                )
            }
        } else {
            // Source preview and information (the top of every fixed layout).
            snapshot.sources.forEach { media ->
                SourceInfoCard(
                    media = media,
                    modifier = Modifier.padding(horizontal = 16.dp),
                )
            }

            Spacer(Modifier.height(4.dp))
            content()

            // Estimated output + the stable action area.
            Spacer(Modifier.height(8.dp))
            EstimateCard(
                estimate = snapshot.estimate,
                modifier = Modifier.padding(horizontal = 16.dp),
            )

            snapshot.validationMessage?.let { message ->
                if (message.isNotBlank() && !snapshot.configurationValid) {
                    Spacer(Modifier.height(8.dp))
                    Text(
                        text = message,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.error,
                        modifier = Modifier.padding(horizontal = 24.dp),
                    )
                }
            }

            Spacer(Modifier.height(16.dp))
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 72.dp)
                    .padding(horizontal = 16.dp),
                contentAlignment = Alignment.Center,
            ) {
                Button(
                    onClick = onSubmit,
                    enabled = snapshot.configurationValid && !submitting,
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = 48.dp),
                ) {
                    if (submitting) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(20.dp),
                            strokeWidth = 2.dp,
                            color = MaterialTheme.colorScheme.onPrimary,
                        )
                        Spacer(Modifier.size(12.dp))
                    }
                    Text(text = submitLabel)
                }
            }
            Spacer(Modifier.height(24.dp))
        }
    }
}

/**
 * The related-change proposal card (mandate §14.1): the core refuses to
 * silently overwrite an explicit selection; the user decides. Accept
 * re-sends the attempted change; decline keeps the accepted value.
 */
@Composable
fun RelatedChangeCard(
    controller: WorkflowController,
    snapshot: SessionSnapshot,
    modifier: Modifier = Modifier,
) {
    val proposal = snapshot.relatedChange ?: return
    if (!controller.hasOpenProposal(snapshot)) {
        return
    }
    Surface(
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 8.dp),
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.secondaryContainer,
        contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = Strings.RELATED_CHANGE_TITLE,
                style = MaterialTheme.typography.titleSmall,
            )
            Spacer(Modifier.height(4.dp))
            Text(
                text = proposal.reason,
                style = MaterialTheme.typography.bodyMedium,
            )
            Spacer(Modifier.height(4.dp))
            Text(
                text = proposal.currentValueId + " → " + proposal.proposedValueId,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(12.dp))
            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                OutlinedButton(
                    onClick = { controller.declineProposal(snapshot) },
                    modifier = Modifier.heightIn(min = 44.dp),
                ) {
                    Text(text = Strings.RELATED_CHANGE_KEEP)
                }
                Button(
                    onClick = { controller.acceptProposal() },
                    modifier = Modifier.heightIn(min = 44.dp),
                ) {
                    Text(text = Strings.RELATED_CHANGE_APPLY)
                }
            }
        }
    }
}
