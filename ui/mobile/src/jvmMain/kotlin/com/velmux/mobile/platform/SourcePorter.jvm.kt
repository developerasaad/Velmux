package com.velmux.mobile.platform

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * JVM actual: picked references are plain file paths — resolution is the
 * identity and no import is ever required.
 */
actual object SourcePorter {

    actual suspend fun resolve(pick: PickedFile): ResolvedSource =
        withContext(Dispatchers.IO) {
            ResolvedSource(
                uri = pick.uri,
                displayName = pick.displayName,
                requiresImport = false,
            )
        }

    actual suspend fun import(source: ResolvedSource): ImportedSource =
        withContext(Dispatchers.IO) {
            // Never called on the JVM (requiresImport is always false).
            ImportedSource(source.uri, source.displayName)
        }
}
