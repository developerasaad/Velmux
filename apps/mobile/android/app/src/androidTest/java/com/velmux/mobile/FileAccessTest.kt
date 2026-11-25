// Velmux instrumented tests: the app's file-access platform layer.
//
// The app deliberately declares NO storage permissions: import goes through
// the system document picker and export/share through the Storage Access
// Framework plus the app's own FileProvider. This suite validates the
// provider surface that Share/Save-to rely on, with the REAL provider (same
// authority, same paths XML, same resolver) — no RN runtime involved, the
// manifest wiring is what is under test.
package com.velmux.mobile

import android.content.Context
import android.net.Uri
import androidx.core.content.FileProvider
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import java.io.File
import java.security.MessageDigest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class FileAccessTest {

  private val context: Context
    get() = ApplicationProvider.getApplicationContext()

  /**
   * The provider must serve engine output files as content URIs with the
   * exact bytes on disk (openInputStream AND openFileDescriptor — share
   * targets use both), a non-null MIME type, and no access to anything
   * outside the velmux tree.
   */
  @Test
  fun fileProviderServesEngineOutputs() {
    // The REAL engine tree (EngineHolder uses filesDir/velmux; outputs land
    // in filesDir/velmux/outputs — the provider's paths XML covers exactly
    // that subtree).
    val outputs = File(File(context.filesDir, "velmux"), "outputs")
    outputs.mkdirs()
    val output = File(outputs, "file-access-test-${System.nanoTime()}.mp4")
    val payload = ByteArray(64 * 1024) { i -> (i % 251).toByte() }
    output.writeBytes(payload)
    try {
      val authority = "${context.packageName}.fileprovider"
      val uri: Uri = FileProvider.getUriForFile(context, authority, output)

      // Stream read: byte-identical content through the provider.
      val streamed = context.contentResolver.openInputStream(uri)!!.use { stream ->
        stream.readBytes()
      }
      assertArrayEquals("provider stream must return the exact file bytes", payload, streamed)

      // Descriptor read: share targets that use fds (media players) must
      // see the same content.
      val descriptor = context.contentResolver.openFileDescriptor(uri, "r")
      assertNotNull("provider must expose a readable fd", descriptor)
      descriptor!!.use { fd ->
        val sized = ByteArray(payload.size)
        java.io.FileInputStream(fd.fileDescriptor).use { it.read(sized) }
        assertArrayEquals("provider fd must return the exact file bytes", payload, sized)
      }

      // The system save/share UIs key off the MIME type.
      val type = context.contentResolver.getType(uri)
      assertNotNull("provider must report a MIME type for .mp4", type)

      // Scope: a file OUTSIDE the velmux tree must not be servable — the
      // provider is the app's only storage surface and must stay minimal.
      // FileProvider enforces this AT URI-CREATION TIME:
      // getUriForFile throws IllegalArgumentException ("Failed to find
      // configured root") for any file outside the configured roots, so a
      // URI for foreign storage can never even be minted (observed on the
      // API 34 emulator, run 45553834138).
      val outside = File(context.filesDir, "outside-${System.nanoTime()}.txt")
      outside.writeBytes(byteArrayOf(1, 2, 3))
      try {
        val refusedAtUriCreation = try {
          FileProvider.getUriForFile(context, authority, outside)
          // A URI was minted for a foreign file: the scope is too broad.
          false
        } catch (expected: IllegalArgumentException) {
          true
        }
        assertTrue(
            "provider must refuse to mint URIs for files outside the velmux tree",
            refusedAtUriCreation)
      } finally {
        outside.delete()
      }
    } finally {
      output.delete()
    }
  }

  /**
   * The PRODUCTION import path (FileImport.importFileInto — what
   * VelmuxEngineModule.importFile runs for every pick): a content URI
   * (what the document picker returns on Android and what the engine's
   * backends cannot open) must land in DURABLE app storage with the exact
   * bytes, and the ORIGINAL must be preserved — a copy, never a move.
   */
  @Test
  fun productionImportCopiesByteIdenticallyAndPreservesTheOriginal() {
    val sourceDir = File(File(context.filesDir, "velmux"), "outputs")
    sourceDir.mkdirs()
    val original = File(sourceDir, "import-source-${System.nanoTime()}.mp4")
    val payload = ByteArray(128 * 1024) { i -> (i % 241).toByte() }
    original.writeBytes(payload)
    val authority = "${context.packageName}.fileprovider"
    val uri = FileProvider.getUriForFile(context, authority, original)
    var importedDir: File? = null
    try {
      val importedPath = FileImport.importFileInto(context, uri.toString(), original.name)
      val imported = File(importedPath)
      importedDir = imported.parentFile

      // Durable destination: filesDir/imports/**, never the cache.
      val importsRoot = File(context.filesDir, "imports").absolutePath
      assertTrue("import must land in the durable imports tree (got $importedPath)",
          importedPath.startsWith(importsRoot))
      assertTrue("imported copy must exist", imported.isFile)
      assertEquals("imported copy must be the same size", payload.size.toLong(), imported.length())
      val digest = MessageDigest.getInstance("SHA-256")
      val originalHash = digest.digest(original.readBytes())
      val importedHash = digest.digest(imported.readBytes())
      assertArrayEquals("imported copy must be byte-identical (hash)", originalHash, importedHash)

      // ORIGINALS ARE PRESERVED (copy, not move): the source must be
      // untouched after the import.
      assertTrue("the original must still exist after the import", original.isFile)
      assertEquals("the original must be unmodified", payload.size.toLong(), original.length())
    } finally {
      importedDir?.deleteRecursively()
      original.delete()
    }
  }
}
