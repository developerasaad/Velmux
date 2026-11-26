package com.velmux.mobile.features.workflow.editors

import com.velmux.mobile.fmt
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.actionRow
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.features.workflow.CropRect

/**
 * The crop rectangle editor (mandate §26): numeric x/y/width/height
 * inputs in source display coordinates plus the approved ratio presets
 * that constrain the rectangle. The core validates boundaries; presets
 * apply the largest centered rectangle of that ratio that fits.
 */
@Composable
fun CropRectEditor(
    crop: CropRect,
    sourceWidth: Int,
    sourceHeight: Int,
    onApply: (CropRect) -> Unit,
    modifier: Modifier = Modifier,
) {
    LabeledInputRow(
        label = Strings.CROP_LEFT,
        acceptedValue = crop.x.toString(),
        placeholder = "0",
        onApply = { text ->
            text.toIntOrNull()?.let { value -> onApply(crop.copy(x = value)) }
        },
        applyEnabled = { it.toIntOrNull() != null },
        modifier = modifier,
    )
    LabeledInputRow(
        label = Strings.CROP_TOP,
        acceptedValue = crop.y.toString(),
        placeholder = "0",
        onApply = { text ->
            text.toIntOrNull()?.let { value -> onApply(crop.copy(y = value)) }
        },
        applyEnabled = { it.toIntOrNull() != null },
        modifier = modifier,
    )
    LabeledInputRow(
        label = Strings.CROP_WIDTH,
        acceptedValue = crop.width.toString(),
        placeholder = sourceWidth.toString(),
        onApply = { text ->
            text.toIntOrNull()?.let { value -> onApply(crop.copy(width = value)) }
        },
        applyEnabled = { (it.toIntOrNull() ?: 0) > 0 },
        modifier = modifier,
    )
    LabeledInputRow(
        label = Strings.CROP_HEIGHT,
        acceptedValue = crop.height.toString(),
        placeholder = sourceHeight.toString(),
        onApply = { text ->
            text.toIntOrNull()?.let { value -> onApply(crop.copy(height = value)) }
        },
        applyEnabled = { (it.toIntOrNull() ?: 0) > 0 },
        modifier = modifier,
    )

    Spacer(Modifier.height(8.dp))
    EditorCaption(text = Strings.CROP_RATIO)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        CropRatioChip(
            label = Strings.CROP_RATIO_ORIGINAL,
            onClick = { onApply(CropRect(0, 0, sourceWidth, sourceHeight)) },
        )
        CropRatioChip(
            label = "1:1",
            onClick = { onApply(ratioCrop(1, 1, sourceWidth, sourceHeight)) },
        )
        CropRatioChip(
            label = "16:9",
            onClick = { onApply(ratioCrop(16, 9, sourceWidth, sourceHeight)) },
        )
        CropRatioChip(
            label = "9:16",
            onClick = { onApply(ratioCrop(9, 16, sourceWidth, sourceHeight)) },
        )
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        CropRatioChip(
            label = "4:3",
            onClick = { onApply(ratioCrop(4, 3, sourceWidth, sourceHeight)) },
        )
        CropRatioChip(
            label = "3:4",
            onClick = { onApply(ratioCrop(3, 4, sourceWidth, sourceHeight)) },
        )
        CropRatioChip(
            label = "4:5",
            onClick = { onApply(ratioCrop(4, 5, sourceWidth, sourceHeight)) },
        )
        CropRatioChip(
            label = "5:4",
            onClick = { onApply(ratioCrop(5, 4, sourceWidth, sourceHeight)) },
        )
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        CropRatioChip(
            label = "3:2",
            onClick = { onApply(ratioCrop(3, 2, sourceWidth, sourceHeight)) },
        )
        CropRatioChip(
            label = "2:3",
            onClick = { onApply(ratioCrop(2, 3, sourceWidth, sourceHeight)) },
        )
        CropRatioChip(
            label = "21:9",
            onClick = { onApply(ratioCrop(21, 9, sourceWidth, sourceHeight)) },
        )
    }
    Spacer(Modifier.height(4.dp))
    Text(
        text = Strings.CROP_RESULT_FORMAT.fmt(
            crop.width.coerceAtLeast(0),
            crop.height.coerceAtLeast(0),
        ),
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(horizontal = 16.dp),
    )
}

/** One ratio preset chip (a 44dp action row, compact label). */
@Composable
private fun CropRatioChip(
    label: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    androidx.compose.material3.Surface(
        modifier = modifier
            .heightIn(min = 44.dp)
            .actionRow { onClick() },
        shape = MaterialTheme.shapes.small,
        color = MaterialTheme.colorScheme.surfaceVariant,
        contentColor = MaterialTheme.colorScheme.onSurfaceVariant,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelLarge,
            modifier = Modifier.padding(horizontal = 14.dp, vertical = 12.dp),
        )
    }
}

/**
 * The largest centered [num]:[den] rectangle that fits the source display
 * frame (even dimensions for encoder alignment).
 */
private fun ratioCrop(
    num: Int,
    den: Int,
    sourceWidth: Int,
    sourceHeight: Int,
): CropRect {
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        return CropRect(0, 0, 0, 0)
    }
    val sourceRatio = sourceWidth.toDouble() / sourceHeight.toDouble()
    val targetRatio = num.toDouble() / den.toDouble()
    var width = sourceWidth
    var height = (width * den / num).toInt()
    if (height > sourceHeight) {
        height = sourceHeight
        width = (height * num / den).toInt()
    }
    if (targetRatio > sourceRatio) {
        width = sourceWidth
        height = (width * den / num).toInt()
    } else {
        height = sourceHeight
        width = (height * num / den).toInt()
    }
    // Even dimensions.
    width = width - (width % 2)
    height = height - (height % 2)
    val x = ((sourceWidth - width) / 2).coerceAtLeast(0)
    val y = ((sourceHeight - height) / 2).coerceAtLeast(0)
    return CropRect(x, y, width.coerceAtLeast(2), height.coerceAtLeast(2))
}
