package com.velmux.mobile

import android.content.Context
import android.net.Uri
import java.io.File
import java.util.UUID

/**
 * Durable, non-destructive import of picked files (the platform half of
 * the engine's local-file contract; see VelmuxEngineModule.importFile).
 *
 * The document picker returns content:// URIs, which no engine backend can
 * open. This object copies such a source through the ContentResolver into
 * the app's durable storage (filesDir/imports/<uuid>/<name> — never the
 * OS-purgeable cache), so inspection, jobs, recovery, retry and
 * re-use-settings read the same local file across process deaths. The
 * ORIGINAL is never modified.
 *
 * No storage permission is involved: the temporary read grant the picker
 * handed the process covers exactly this copy, which runs immediately
 * after the pick.
 */
object FileImport {

  /**
   * Copies [uri] (content:// or file://) into durable app storage and
   * returns the imported file's absolute path. Throws with the platform's
   * reason when the source is unreadable, revoked, empty, or the disk is
   * full — the module surfaces that message to JS verbatim.
   */
  fun importFileInto(context: Context, uri: String, fileName: String): String {
    // Sanitize the destination name: keep the last path component only
    // (a malicious/odd provider could return names with separators).
    val safeName = fileName.substringAfterLast('/').takeIf { it.isNotBlank() }
      ?: "imported-${System.currentTimeMillis()}"
    val destDir = File(File(context.filesDir, "imports"), UUID.randomUUID().toString())
    if (!destDir.mkdirs() && !destDir.isDirectory) {
      throw java.io.IOException("cannot create the import directory")
    }
    val dest = File(destDir, safeName)
    val input = context.contentResolver.openInputStream(Uri.parse(uri))
      ?: run {
        throw java.io.IOException("the source is not readable (revoked or unavailable)")
      }
    input.use { stream ->
      dest.outputStream().use { output -> stream.copyTo(output) }
    }
    if (dest.length() == 0L) {
      dest.delete()
      throw java.io.IOException("the source is empty")
    }
    return dest.absolutePath
  }
}
