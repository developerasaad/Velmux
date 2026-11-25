package com.velmux.mobile.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
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
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.engine.MediaInfo
import com.velmux.mobile.engine.OutputEstimate

/**
 * Source media card: what the engine found (container, duration, size,
 * streams). Loading states are honest — unknown fields render as unknown.
 */
@Composable
fun SourceInfoCard(
    media: MediaInfo,
    modifier: Modifier = Modifier,
    title: String? = null,
) {
    CardSurface(modifier = modifier, title = title) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            LabeledValue(
                label = "Container",
                value = media.container.ifBlank { Strings.SIZE_UNKNOWN },
            )
            LabeledValue(
                label = "Duration",
                value = Strings.formatDuration(media.durationSec),
            )
            LabeledValue(
                label = "Size",
                value = Strings.formatBytes(media.sizeBytes),
            )
        }
        Spacer(Modifier.height(8.dp))
        media.primaryVideo?.let { video ->
            Text(
                text = "Video: ${video.codec} ${video.width}x${video.height}" +
                    (if (video.frameRate > 0) " @ ${video.frameRate}" else "") +
                    (if (video.hdr) " HDR" else ""),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        media.primaryAudio?.let { audio ->
            Text(
                text = "Audio: ${audio.codec}" +
                    (if (audio.channels > 0) " ${audio.channels}ch" else "") +
                    (if (audio.sampleRate > 0) " ${audio.sampleRate / 1000}kHz" else ""),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (media.video.isEmpty() && media.audio.isEmpty()) {
            Text(
                text = "No decodable streams reported",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

/** Estimated output card (bounds when not precisely known). */
@Composable
fun EstimateCard(
    estimate: OutputEstimate,
    modifier: Modifier = Modifier,
    title: String = "Estimated output",
) {
    CardSurface(modifier = modifier, title = title) {
        if (estimate.known) {
            OutputEstimateRow(label = "Estimate", bytes = estimate.estimatedBytes)
            Spacer(Modifier.height(4.dp))
            OutputEstimateRow(
                label = "Bounds",
                bytes = null,
                range = estimate.lowerBoundBytes to estimate.upperBoundBytes,
            )
        } else {
            Text(
                text = "Not enough information for an estimate yet",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

/** One estimate line (value or honest range). */
@Composable
fun OutputEstimateRow(
    label: String,
    bytes: Long?,
    modifier: Modifier = Modifier,
    range: Pair<Long, Long>? = null,
) {
    Row(
        modifier = modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        val value = when {
            bytes != null && bytes >= 0 -> Strings.formatBytes(bytes)
            range != null && range.first >= 0 ->
                "${Strings.formatBytes(range.first)} – ${Strings.formatBytes(range.second)}"
            else -> Strings.SIZE_UNKNOWN
        }
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
        )
    }
}

/** A file row (source picks and produced outputs share the shape). */
@Composable
fun FileListRow(
    name: String,
    modifier: Modifier = Modifier,
    detail: String? = null,
    leading: ImageVector? = null,
    trailing: (@Composable () -> Unit)? = null,
    onClick: (() -> Unit)? = null,
) {
    val rowModifier = if (onClick != null) {
        modifier.fillMaxWidth().actionRow { onClick() }
    } else {
        modifier.fillMaxWidth()
    }
    Row(
        modifier = rowModifier.padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (leading != null) {
            Icon(
                imageVector = leading,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
            )
        }
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = name,
                style = MaterialTheme.typography.bodyLarge,
                maxLines = 1,
            )
            if (detail != null) {
                Text(
                    text = detail,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                )
            }
        }
        trailing?.invoke()
    }
}

/** One primary workflow tile on Home (Material vector icon, 44dp target). */
@Composable
fun WorkflowTile(
    title: String,
    hint: String,
    icon: ImageVector,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    Surface(
        modifier = modifier
            .fillMaxWidth()
            .aspectRatio(1.75f)
            .actionRow { onClick() },
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceVariant,
        contentColor = MaterialTheme.colorScheme.onSurfaceVariant,
    ) {
        Row(
            modifier = Modifier.padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Icon(
                imageVector = icon,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.size(28.dp),
            )
            Column {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Spacer(Modifier.height(2.dp))
                Text(
                    text = hint,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun CardSurface(
    modifier: Modifier = Modifier,
    title: String?,
    content: @Composable () -> Unit,
) {
    Surface(
        modifier = modifier.fillMaxWidth().padding(vertical = 6.dp),
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceVariant,
        contentColor = MaterialTheme.colorScheme.onSurfaceVariant,
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            if (title != null) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Spacer(Modifier.height(6.dp))
            }
            content()
        }
    }
}
