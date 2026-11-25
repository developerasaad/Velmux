package com.velmux.mobile.platform

import androidx.compose.runtime.Composable
import androidx.compose.runtime.State
import kotlinx.coroutines.flow.StateFlow

/**
 * A document the user picked through the platform file picker.
 * [uri] is the platform reference (content URI / file URL); [displayName]
 * is the human-readable name; [sizeBytes] when known.
 */
data class PickedFile(
    val uri: String,
    val displayName: String,
    val sizeBytes: Long? = null,
)

/**
 * Native file picking through expect/actual (spec UI-1). The handle is a
 * stable object remembered per workflow tile; [launch] opens the system
 * document picker, results arrive in [picks] (an empty list resets).
 */
expect class FilePickerHandle {

    /** The latest successful picks (replaced on every launcher run). */
    val picks: StateFlow<List<PickedFile>>

    /** False when the platform cannot pick files in this context. */
    val available: Boolean

    /** Opens the system document picker for the configured mime types. */
    fun launch()

    /** Clears [picks] (after the flow was consumed). */
    fun reset()
}

/**
 * Remembers a file picker for [mimeTypes] ("video/*", "audio/*"...),
 * allowing multiple documents when [multiple] (batch and merge workflows).
 */
@Composable
expect fun rememberFilePicker(mimeTypes: List<String>, multiple: Boolean): FilePickerHandle
