package com.velmux.mobile.features.workflow

import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.EngineException
import com.velmux.mobile.engine.FakeTransport
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.navigation.Destination
import com.velmux.mobile.navigation.Navigator
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * Batch workflow tests (mandate §40): per-file independence, shared
 * intent semantics, and one job per valid file.
 */
class BatchWorkflowTest {

    private fun batchController(
        fake: FakeTransport,
        entries: List<Destination.BatchEntry>,
    ): Pair<EngineClient, BatchWorkflowController> {
        val client = EngineClient(transport = fake, pollIntervalMs = 10_000L)
        return client to BatchWorkflowController(
            operation = "video-convert",
            entries = entries,
            client = client,
            navigator = Navigator(),
            scope = kotlinx.coroutines.CoroutineScope(
                kotlinx.coroutines.Dispatchers.Unconfined,
            ),
        )
    }

    @Test
    fun sharedValueIsTheCommonAcceptedValueAndVariesDiverges() = runTest {
        val fake = FakeTransport()
        // Two sessions with the same accepted codec, differing formats.
        fake.sessions["s-1"] = FakeTransport.sessionSnapshot(
            "s-1", "video-convert", state = "ready",
        ).withValue(Fields.FORMAT, "mp4").withValue(Fields.CODEC, "h264")
        fake.sessions["s-2"] = FakeTransport.sessionSnapshot(
            "s-2", "video-convert", state = "ready",
        ).withValue(Fields.FORMAT, "mp4").withValue(Fields.CODEC, "h264")
        fake.sessions["s-3"] = FakeTransport.sessionSnapshot(
            "s-3", "video-convert", state = "ready",
        ).withValue(Fields.FORMAT, "mkv").withValue(Fields.CODEC, "h264")

        val entries = listOf("s-1", "s-2", "s-3").map {
            Destination.BatchEntry(name = "file-$it", sessionId = it)
        }
        val (client, controller) = batchController(fake, entries)
        client.start(backgroundScope)
        runCurrent()

        assertEquals("h264", controller.sharedValue(Fields.CODEC))
        // Diverged values are honest: null means "varies".
        assertNull(controller.sharedValue(Fields.FORMAT))
        // Unknown fields (nothing accepted anywhere) are null too.
        assertNull(controller.sharedValue(Fields.RESOLUTION))
    }

    @Test
    fun broadcastSendsTheSharedChangeToEverySession() = runTest {
        val fake = FakeTransport()
        fake.sessions["s-1"] = FakeTransport.sessionSnapshot("s-1", "video-convert", state = "ready")
        fake.sessions["s-2"] = FakeTransport.sessionSnapshot("s-2", "video-convert", state = "ready")
        val entries = listOf("s-1", "s-2").map {
            Destination.BatchEntry(name = "f", sessionId = it)
        }
        val (client, controller) = batchController(fake, entries)
        client.start(backgroundScope)
        runCurrent()

        controller.broadcast(FieldChange(field = Fields.FORMAT, valueId = "mkv"))
        runCurrent()

        assertEquals(2, fake.updateSessionCalls)
        // Both broadcasts carried the same shared intent.
        assertEquals(
            listOf("s-1", "s-2"),
            fake.receivedUpdates.map { it.first },
        )
        fake.receivedUpdates.forEach { (_, request) ->
            val change = request.changes.single()
            assertEquals(Fields.FORMAT, change.field)
            assertEquals("mkv", change.valueId)
        }
    }

    @Test
    fun aFailingFileNeverBlocksTheOthersDuringBatchStart() = runTest {
        val fake = FakeTransport()
        // The second file's inspection fails; the other two must still
        // produce sessions (mandate §40: a failed source must not
        // invalidate unrelated valid items).
        fake.prepareSessionHandler = { sessionId ->
            if (sessionId == "s-2") {
                throw EngineException("source-unavailable", "unreadable file")
            }
            fake.sessions.getValue(sessionId)
        }
        val engine = EngineClient(transport = fake, pollIntervalMs = 10_000L)

        val picks = listOf(
            FakeTransport.pick("a.mp4"),
            FakeTransport.pick("b.mp4"),
            FakeTransport.pick("c.mp4"),
        )
        val started = engine.beginBatch("video-convert", picks)

        assertEquals(3, started.size)
        // File 2 failed honestly with the engine's error; 1 and 3 prepared.
        assertNull(started[1].sessionId)
        assertEquals("source-unavailable", started[1].failure?.code)
        assertTrue(started[0].sessionId != null)
        assertTrue(started[2].sessionId != null)
        // The failed half-created session was closed, not leaked.
        assertTrue(fake.closedSessions.contains("s-2"))
    }

    @Test
    fun submitAllCreatesOneJobPerValidFile() = runTest {
        val fake = FakeTransport()
        fake.sessions["s-1"] = FakeTransport.sessionSnapshot(
            "s-1", "video-convert", state = "ready", configurationValid = true,
        )
        fake.sessions["s-2"] = FakeTransport.sessionSnapshot(
            "s-2", "video-convert", state = "ready",
            configurationValid = false,
        )
        fake.sessions["s-3"] = FakeTransport.sessionSnapshot(
            "s-3", "video-convert", state = "ready", configurationValid = true,
        )
        val entries = listOf("s-1", "s-2", "s-3").map {
            Destination.BatchEntry(name = "f", sessionId = it)
        }
        val (client, controller) = batchController(fake, entries)
        client.start(backgroundScope)
        runCurrent()

        assertTrue(controller.anyValid())
        controller.submitAll()
        runCurrent()

        // One job per VALID file only; the invalid file stayed unsubmitted.
        assertEquals(2, fake.submitCalls)
        assertEquals(
            setOf("s-1", "s-3"),
            fake.submissions.map { it.first }.toSet(),
        )
    }
}

/** Copies a snapshot with one accepted value added (test helper). */
private fun com.velmux.mobile.engine.SessionSnapshot.withValue(
    field: String,
    valueId: String,
): com.velmux.mobile.engine.SessionSnapshot = copy(
    values = values + com.velmux.mobile.engine.AcceptedValue(
        field = field,
        valueId = valueId,
        origin = "default",
        payload = null,
    ),
)
