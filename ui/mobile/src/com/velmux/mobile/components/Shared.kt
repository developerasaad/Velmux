package com.velmux.mobile.components

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.selection.toggleable
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp

/**
 * Shared component helpers: every interactive row keeps a 44dp minimum
 * touch target (design rule) with a coherent click semantics.
 */

/** A full-width clickable row with a guaranteed 44dp touch target. */
fun Modifier.actionRow(onClick: () -> Unit): Modifier = this
    .heightIn(min = 44.dp)
    .clickable(onClickLabel = null, role = Role.Button, onClick = onClick)

/** A 44dp toggleable row (expand/collapse, checkboxes...). */
fun Modifier.toggleRow(value: Boolean, onToggle: () -> Unit): Modifier = this
    .heightIn(min = 44.dp)
    .toggleable(
        value = value,
        role = Role.Button,
        onValueChange = { onToggle() },
    )
