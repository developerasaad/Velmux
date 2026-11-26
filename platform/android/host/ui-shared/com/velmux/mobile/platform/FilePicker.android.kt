package com.velmux.mobile.platform

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Android actual: the system document picker (SAF) through the registered
 * activity-result plumbing hosted by the activity —
 * rememberLauncherForActivityResult IS that registered helper (a stable
 * launcher key on the activity's registry; the activity hosts the
 * lifecycle and survives configuration changes). No storage permission is
 * involved: granted content URIs come back from the picker and the
 * SourcePorter import copies them into app storage before the engine
 * reads them.
 */
actual class FilePickerHandle {

    private val _picks = MutableStateFlow<List<PickedFile>>(emptyList())
    actual val picks: StateFlow<List<PickedFile>> = _picks

    private var launchImpl: (() -> Unit)? = null

    actual val available: Boolean get() = launchImpl != null

    actual fun launch() {
        val impl = launchImpl
        if (impl != null) {
            impl()
        } else {
            _picks.value = emptyList()
        }
    }

    actual fun reset() {
        _picks.value = emptyList()
    }

    internal fun attach(launch: () -> Unit) {
        launchImpl = launch
    }

    internal fun deliver(uris: List<Uri>) {
        _picks.value = uris.map { uri ->
            PickedFile(
                uri = uri.toString(),
                displayName = uri.lastPathSegment
                    ?.substringAfterLast('/')
                    ?.ifBlank { null }
                    ?: "Untitled media",
            )
        }
    }
}

@Composable
actual fun rememberFilePicker(mimeTypes: List<String>, multiple: Boolean): FilePickerHandle {
    val handle = remember(mimeTypes, multiple) { FilePickerHandle() }
    val contract = if (multiple) {
        ActivityResultContracts.OpenMultipleDocuments()
    } else {
        ActivityResultContracts.OpenDocument()
    }
    val launcher = rememberLauncherForActivityResult(contract) { result ->
        when (result) {
            null -> Unit
            is Uri -> handle.deliver(listOf(result))
            is List<*> -> handle.deliver(result.filterIsInstance<Uri>())
        }
    }
    remember(launcher, handle) {
        handle.attach { launcher.launch(mimeTypes.toTypedArray()) }
        true
    }
    return handle
}
