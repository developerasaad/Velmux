package com.velmux.mobile.platform

import android.net.Uri
import android.provider.OpenableColumns
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.util.UUID

/**
 * Android actual: SAF content URIs cannot be read by the native engine
 * (no file descriptor path through C stdio), so resolving a pick marks
 * [ResolvedSource.requiresImport] and the import copies the granted
 * document into the engine's input directory (filesDir/velmux/inputs).
 * Querying the display name is the resolve step (cheap, metadata only).
 */
actual object SourcePorter {

    actual suspend fun resolve(pick: PickedFile): ResolvedSource =
        withContext(Dispatchers.IO) {
            val uri = Uri.parse(pick.uri)
            val name = queryDisplayName(uri) ?: pick.displayName
            ResolvedSource(
                uri = pick.uri,
                displayName = name,
                requiresImport = uri.scheme == "content",
            )
        }

    actual suspend fun import(source: ResolvedSource): ImportedSource =
        withContext(Dispatchers.IO) {
            val context = AppRuntime.context()
            val uri = Uri.parse(source.uri)
            val inputs = File(platformDataDir, "inputs").apply { mkdirs() }
            val extension = guessExtension(uri, source.displayName)
            val target = File(inputs, "${UUID.randomUUID()}$extension")
            val opened = context.contentResolver.openInputStream(uri)
            if (opened == null) {
                throw com.velmux.mobile.engine.EngineException(
                    com.velmux.mobile.engine.EngineErrorCode.SOURCE_UNAVAILABLE,
                    "The picked document is no longer readable",
                )
            }
            opened.use { input ->
                target.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
            ImportedSource(
                uri = target.absolutePath,
                displayName = source.displayName,
            )
        }

    private fun queryDisplayName(uri: Uri): String? = try {
        val context = AppRuntime.context()
        context.contentResolver.query(
            uri,
            arrayOf(OpenableColumns.DISPLAY_NAME),
            null,
            null,
            null,
        )?.use { cursor ->
            if (cursor.moveToFirst()) {
                val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (index >= 0) cursor.getString(index) else null
            } else {
                null
            }
        }
    } catch (error: Exception) {
        null
    }

    private fun guessExtension(uri: Uri, displayName: String): String {
        val fromName = displayName.substringAfterLast('.', "").lowercase()
        if (fromName.isNotBlank()) return ".$fromName"
        val fromUri = uri.toString().substringAfterLast('.', "").lowercase()
        if (fromUri.isNotBlank() && fromUri.length <= 5) return ".$fromUri"
        return ""
    }
}
