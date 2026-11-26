package com.velmux.mobile.platform

import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import platform.Foundation.NSFileManager
import platform.Foundation.NSURL
import platform.Foundation.NSTemporaryDirectory
import platform.Foundation.NSUUID

/**
 * iOS actual: picked documents arrive as security-scoped URLs — the
 * import step starts scope access and copies the document into the app's
 * own container (tmp/velmux-imports) where the engine can read it. The
 * copy (not the scoped original) is the engine-readable source.
 */
@OptIn(ExperimentalForeignApi::class)
actual object SourcePorter {

    actual suspend fun resolve(pick: PickedFile): ResolvedSource =
        withContext(Dispatchers.Default) {
            ResolvedSource(
                uri = pick.uri,
                displayName = pick.displayName,
                requiresImport = true,
            )
        }

    actual suspend fun import(source: ResolvedSource): ImportedSource =
        withContext(Dispatchers.Default) {
            val url = NSURL.URLWithString(source.uri)
                ?: NSURL.fileURLWithPath(source.uri)
            val scoped = url.startAccessingSecurityScopedResource()
            try {
                val importDir = "${NSTemporaryDirectory()}velmux-imports"
                NSFileManager.defaultManager.createDirectoryAtPath(
                    importDir,
                    withIntermediateDirectories = true,
                    attributes = null,
                    error = null,
                )
                val extension = source.displayName.substringAfterLast('.', "")
                val suffix = if (extension.isNotBlank()) ".$extension" else ""
                val target = "$importDir/${NSUUID().UUIDString}$suffix"
                val moved = NSFileManager.defaultManager.copyItemAtPath(
                    url.path ?: "",
                    toPath = target,
                    error = null,
                )
                if (!moved) {
                    throw com.velmux.mobile.engine.EngineException(
                        com.velmux.mobile.engine.EngineErrorCode.SOURCE_UNAVAILABLE,
                        "The picked document is no longer readable",
                    )
                }
                ImportedSource(uri = target, displayName = source.displayName)
            } finally {
                if (scoped) {
                    url.stopAccessingSecurityScopedResource()
                }
            }
        }
}
