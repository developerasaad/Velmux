package com.velmux.mobile.platform

import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * JVM actual: no system document picker exists on the dev target — the
 * handle is honest about it ([available] == false, launch is a no-op).
 */
actual class FilePickerHandle actual constructor() {

    private val _picks = MutableStateFlow<List<PickedFile>>(emptyList())
    actual val picks: StateFlow<List<PickedFile>> = _picks

    actual val available: Boolean = false

    actual fun launch() {
        // No picker on the JVM dev target; nothing is picked.
    }

    actual fun reset() {
        _picks.value = emptyList()
    }
}

@Composable
actual fun rememberFilePicker(mimeTypes: List<String>, multiple: Boolean): FilePickerHandle =
    remember { FilePickerHandle() }
