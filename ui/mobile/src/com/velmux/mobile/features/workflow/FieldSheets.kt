package com.velmux.mobile.features.workflow

import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import com.velmux.mobile.Strings
import com.velmux.mobile.components.OptionRow
import com.velmux.mobile.components.PendingIndicator
import com.velmux.mobile.components.SelectionOption
import com.velmux.mobile.components.SelectionSheet
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.SessionSnapshot
import com.velmux.mobile.engine.ValueLabels
import kotlinx.serialization.json.JsonElement

/**
 * An option row bound to one core-owned field: shows the accepted value,
 * loads the field's supported options when tapped and commits the
 * selection through the controller (single-value choices commit after
 * core acceptance and close the sheet — mandate §36.3).
 */
@Composable
fun OptionFieldRow(
    label: String,
    field: String,
    snapshot: SessionSnapshot,
    controller: WorkflowController,
    modifier: Modifier = Modifier,
    payloadFor: (valueId: String) -> JsonElement? = { null },
) {
    var sheetOpen by remember { mutableStateOf(false) }
    val accepted = snapshot.valueFor(field)
    val options = controller.options.value[field]
    val loading = field in controller.loadingFields.value

    val valueText = when {
        accepted == null -> Strings.NO_VALUE
        else -> ValueLabels.label(field, accepted.valueId)
    }

    OptionRow(
        label = label,
        value = valueText,
        modifier = modifier,
        trailing = if (loading) {
            { PendingIndicator() }
        } else {
            null
        },
        onClick = {
            controller.requestOptions(snapshot, field)
            sheetOpen = true
        },
    )

    if (sheetOpen) {
        val selections = (options?.values ?: emptyList()).map { option ->
            SelectionOption(
                id = option.id,
                label = ValueLabels.label(field, option.id),
                detail = optionDetail(option.attributes),
            )
        }
        SelectionSheet(
            title = label,
            options = selections,
            selectedId = accepted?.valueId,
            onDismiss = { sheetOpen = false },
            onSelect = { selected ->
                sheetOpen = false
                controller.select(
                    snapshot,
                    FieldChange(
                        field = field,
                        valueId = selected.id,
                        payload = payloadFor(selected.id),
                    ),
                )
            },
        )
    }
}

/** Renders option attributes as a compact detail line (honest, no noise). */
private fun optionDetail(attributes: Map<String, String>): String? {
    if (attributes.isEmpty()) {
        return null
    }
    // Resolution presets carry width/height; channel layouts carry layout.
    val width = attributes["width"]
    val height = attributes["height"]
    if (width != null && height != null) {
        return "$width × $height"
    }
    val layout = attributes["layout"]
    if (layout != null) {
        return layout
    }
    return null
}
