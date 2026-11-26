package com.velmux.mobile.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.design.VelmuxColors
import com.velmux.mobile.design.VelmuxIcons
import com.velmux.mobile.features.jobs.JobStateUi

/**
 * Job state presentation: a colored chip + label rendered the same way on
 * Home, Jobs and detail surfaces. "Target not met" is a state of its own,
 * shown with the honest output-vs-target story in the Jobs row.
 */
@Composable
fun StateChip(state: JobStateUi, modifier: Modifier = Modifier) {
    val (background, content) = when {
        state.isFailed -> VelmuxColors.Danger to Color.White
        state.isTerminal && !state.isFailed -> VelmuxColors.Success to Color.White
        state == JobStateUi.TARGET_NOT_MET -> VelmuxColors.Warn to Color.White
        else -> VelmuxColors.Copper300 to VelmuxColors.Copper700
    }
    Surface(
        modifier = modifier,
        shape = MaterialTheme.shapes.small,
        color = background,
        contentColor = content,
    ) {
        Text(
            text = state.label,
            style = MaterialTheme.typography.labelMedium,
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 3.dp),
        )
    }
}

/**
 * Progress line for a job row: linear progress with the state label, or an
 * indeterminate bar while the engine has no progress yet (preparing,
 * queued...). Never fabricated.
 */
@Composable
fun JobProgress(
    state: JobStateUi,
    progress: Double,
    modifier: Modifier = Modifier,
) {
    Column(modifier = modifier.fillMaxWidth()) {
        ProgressRow(progress = progress)
        Spacer(Modifier.height(4.dp))
    }
}

/** Icon + label for an inline action chip row (44dp targets). */
@Composable
fun ActionChip(
    label: String,
    icon: ImageVector,
    modifier: Modifier = Modifier,
    destructive: Boolean = false,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    Surface(
        modifier = modifier.actionRow { onClick() },
        shape = MaterialTheme.shapes.small,
        color = if (destructive) {
            MaterialTheme.colorScheme.errorContainer
        } else {
            MaterialTheme.colorScheme.surfaceVariant
        },
        contentColor = if (destructive) {
            MaterialTheme.colorScheme.onErrorContainer
        } else {
            MaterialTheme.colorScheme.onSurfaceVariant
        },
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Icon(
                imageVector = icon,
                contentDescription = null,
                modifier = Modifier.size(16.dp),
            )
            Text(
                text = label,
                style = MaterialTheme.typography.labelLarge,
            )
        }
    }
}

/** Summary line for the Home active-jobs card. */
@Composable
fun JobSummaryLine(
    state: JobStateUi,
    name: String,
    progress: Double,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier.fillMaxWidth().padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        StateChip(state = state)
        Spacer(Modifier.width(10.dp))
        Text(
            text = name,
            style = MaterialTheme.typography.bodyMedium,
            maxLines = 1,
            modifier = Modifier.weight(1f),
        )
        Text(
            text = Strings.formatProgress(progress),
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}
