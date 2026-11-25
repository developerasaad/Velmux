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
      val outside = File(context.filesDir, "outside-${System.nanoTime()}.txt")
      outside.writeBytes(byteArrayOf(1, 2, 3))
      try {
        val outsideUri = FileProvider.getUriForFile(context, authority, outside)
        val readable = try {
          context.contentResolver.openInputStream(outsideUri)?.close()
          true
        } catch (expected: SecurityException) {
          false
        }
        assertTrue("provider must not serve files outside the velmux tree", !readable)
      } finally {
        outside.delete()
      }
    } finally {
      output.delete()
    }
  }

  /**
   * The durable-import contract: a content URI (what the document picker
   * returns on Android and what the engine's backends cannot open) must be
   * copyable into app-durable storage through the ContentResolver with the
   * exact bytes — the primitive keepLocalCopy performs at pick time. This
   * exercises the same resolver path with a REAL content URI from the app's
   * own provider.
   */
  @Test
  fun contentUriImportsByteIdenticallyIntoDurableStorage() {
    val sourceDir = File(File(context.filesDir, "velmux"), "outputs")
    sourceDir.mkdirs()
    val original = File(sourceDir, "import-source-${System.nanoTime()}.mp4")
    val payload = ByteArray(128 * 1024) { i -> (i % 241).toByte() }
    original.writeBytes(payload)
    val authority = "${context.packageName}.fileprovider"
    val uri = FileProvider.getUriForFile(context, authority, original)
    try {
      // The durable import destination (same policy as keepLocalCopy's
      // documentDirectory destination: filesDir, never the OS-purgeable
      // cache).
      val imports = File(context.filesDir, "imports")
      imports.mkdirs()
      val imported = File(imports, "imported-${System.nanoTime()}.mp4")

      context.contentResolver.openInputStream(uri)!!.use { input ->
        imported.outputStream().use { output -> input.copyTo(output) }
      }

      assertTrue("imported copy must exist", imported.isFile)
      assertEquals("imported copy must be the same size", payload.size.toLong(), imported.length())
      val digest = MessageDigest.getInstance("SHA-256")
      val originalHash = digest.digest(original.readBytes())
      val importedHash = digest.digest(imported.readBytes())
      assertArrayEquals("imported copy must be byte-identical (hash)", originalHash, importedHash)
      imported.delete()
    } finally {
      original.delete()
    }
  }
}
