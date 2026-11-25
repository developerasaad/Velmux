package com.velmux.mobile.platform

import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import platform.Foundation.NSURL
import platform.UIKit.UIApplication
import platform.UIKit.UIDocumentPickerDelegateProtocol
import platform.UIKit.UIDocumentPickerMode
import platform.UIKit.UIDocumentPickerViewController
import platform.UIKit.UIViewController
import platform.darwin.NSObject

/**
 * iOS actual: the system document picker (UIDocumentPickerViewController
 * in import mode) presented from the current top view controller; the
 * delegate is retained by the handle (the picker holds it weakly).
 *
 * Picked URLs are security-scoped; [SourcePorter] imports them into app
 * storage before the engine reads them.
 */
actual class FilePickerHandle actual constructor() {

    private val _picks = MutableStateFlow<List<PickedFile>>(emptyList())
    actual val picks: StateFlow<List<PickedFile>> = _picks

    private var documentTypes: List<String> = emptyList()
    private var multiple: Boolean = false

    /** Delegate kept alive for as long as the handle lives. */
    private val delegate = PickerDelegate(this)

    actual val available: Boolean get() = topViewController() != null

    actual fun launch() {
        val anchor = topViewController() ?: run {
            _picks.value = emptyList()
            return
        }
        val picker = UIDocumentPickerViewController(
            documentTypes = documentTypes,
            inMode = UIDocumentPickerMode.Import,
        )
        picker.delegate = delegate
        anchor.presentViewController(picker, animated = true, completion = null)
    }

    actual fun reset() {
        _picks.value = emptyList()
    }

    internal fun configure(documentTypes: List<String>, multiple: Boolean) {
        this.documentTypes = documentTypes
        this.multiple = multiple
    }

    internal fun deliver(urls: List<NSURL>) {
        _picks.value = urls.map { url ->
            val name = url.lastPathComponent ?: "Untitled media"
            PickedFile(
                uri = url.absoluteString ?: url.path ?: "",
                displayName = name,
            )
        }
    }

    private class PickerDelegate(
        private val handle: FilePickerHandle,
    ) : NSObject(), UIDocumentPickerDelegateProtocol {

        override fun documentPicker(
            controller: UIDocumentPickerViewController,
            didPickDocumentsAtURLs: List<*>,
        ) {
            val urls = didPickDocumentsAtURLs.filterIsInstance<NSURL>()
            handle.deliver(urls)
        }

        override fun documentPickerWasCancelled(
            controller: UIDocumentPickerViewController,
        ) {
            handle.reset()
        }
    }
}

@Composable
actual fun rememberFilePicker(mimeTypes: List<String>, multiple: Boolean): FilePickerHandle {
    val handle = remember(mimeTypes, multiple) { FilePickerHandle() }
    remember(handle, mimeTypes, multiple) {
        handle.configure(mimeTypes, multiple)
        true
    }
    return handle
}

private fun topViewController(): UIViewController? {
    val application = UIApplication.sharedApplication
    val window = application.keyWindow
        ?: application.windows.firstOrNull() as? platform.UIKit.UIWindow
    return window?.rootViewController?.let(::topMost)
}

private fun topMost(controller: UIViewController): UIViewController {
    val presented = controller.presentedViewController ?: return controller
    return topMost(presented)
}
