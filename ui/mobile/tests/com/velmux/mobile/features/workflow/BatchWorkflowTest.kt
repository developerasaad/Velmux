package com.velmux.mobile.features.workflow

import com.velmux.mobile.engine.AcceptedValue
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.EngineException
import com.velmux.mobile.engine.FakeTransport
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.engine.SessionSnapshot
import com.velmux.mobile.navigation.Destination
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * Batch workflow tests (mandate §40): per-file independence, shared
 * intent semantics, and one job per valid file. Sessions are seeded
 * through the client's own beginBatch flow (the client's session state
 * only reflects what the engine accepted).
 */
class BatchWorkflowTest {

    /** Values the fake engine accepts for a session (per-session fixtures). */
    private var acceptedValues: Map<String, List<AcceptedValue>> = emptyMap()

    private fun fakeWithValues(): FakeTransport {
        val fake = FakeTransport()
        fake.prepareSessionHandler = { sessionId ->
            FakeTransport.sessionSnapshot(
                sessionId,
                operation = fake.sessions[sessionId]?.operation ?: "video-convert",
                state = "ready",
                revision = 1,
                configurationValid = true,
                values = acceptedValues[sessionId] ?: emptyList(),
            )
        }
        return fake
    }

    private fun value(field: String, valueId: String) =
        AcceptedValue(field = field, valueId = valueId, origin = "user")

    @Test
    fun sharedValueIsTheCommonAcceptedValueAndVariesDiverges() = runTest {
        // Two files accept the same codec and format; the third diverged
        // on format (per-file override), all share the codec.
        acceptedValues = mapOf(
            "s-0" to listOf(value(Fields.FORMAT, "mp4"), value(Fields.CODEC, "h264")),
            "s-1" to listOf(value(Fields.FORMAT, "mp4"), value(Fields.CODEC, "h264")),
            "s-2" to listOf(value(Fields.FORMAT, "mkv"), value(Fields.CODEC, "h264")),
        )
        val fake = fakeWithValues()
        val client = EngineClient(transport = fake, pollIntervalMs = 10_000L)

        val picks = listOf(FakeTransport.pick("a.mp4"), FakeTransport.pick("b.mp4"), FakeTransport.pick("c.mp4"))
        val started = client.beginBatch("video-convert", picks)
        assertEquals(3, started.count { it.sessionId != null })

        val entries = started.mapNotNull { result ->
            result.sessionId?.let { Destination.BatchEntry(name = result.pick.displayName, sessionId = it) }
        }
        val controller = BatchWorkflowController(
            operation = "video-convert",
            entries = entries,
            client = client,
            navigator = com.velmux.mobile.navigation.Navigator(),
            scope = kotlinx.coroutines.CoroutineScope(kotlinx.coroutines.Dispatchers.Unconfined),
        )

        assertEquals("h264", controller.sharedValue(Fields.CODEC))
        // Diverged values are honest: null means "varies".
        assertNull(controller.sharedValue(Fields.FORMAT))
        // Fields nothing accepted anywhere are null too.
        assertNull(controller.sharedValue(Fields.RESOLUTION))
    }

    @Test
    fun broadcastSendsTheSharedChangeToEverySession() = runTest {
        acceptedValues = emptyMap()
        val fake = fakeWithValues()
        val client = EngineClient(transport = fake, pollIntervalMs = 10_000L)

        val picks = listOf(FakeTransport.pick("a.mp4"), FakeTransport.pick("b.mp4"))
        val started = client.beginBatch("video-convert", picks)
        val entries = started.mapNotNull { result ->
            result.sessionId?.let { Destination.BatchEntry(name = result.pick.displayName, sessionId = it) }
        }
        val controller = BatchWorkflowController(
            operation = "video-convert",
            entries = entries,
            client = client,
            navigator = com.velmux.mobile.navigation.Navigator(),
            scope = kotlinx.coroutines.CoroutineScope(kotlinx.coroutines.Dispatchers.Unconfined),
        )

        controller.broadcast(FieldChange(field = Fields.FORMAT, valueId = "mkv"))
        runCurrent()

        assertEquals(2, fake.updateSessionCalls)
        // Both broadcasts carried the same shared intent.
        assertEquals(2, fake.receivedUpdates.size)
        fake.receivedUpdates.forEach { (_, request) ->
            val change = request.changes.single()
            assertEquals(Fields.FORMAT, change.field)
            assertEquals("mkv", change.valueId)
        }
    }

    @Test
    fun aFailingFileNeverBlocksTheOthersDuringBatchStart() = runTest {
        val fake = FakeTransport()
        // The SECOND file's (s-1, the counter starts at 0) inspection
        // fails; the other two must still produce sessions (mandate §40:
        // a failed source must not invalidate unrelated valid items).
        fake.prepareSessionHandler = { sessionId ->
            if (sessionId == "s-1") {
                throw EngineException("source-unavailable", "unreadable file")
            }
            fake.sessions.getValue(sessionId)
        }
        val client = EngineClient(transport = fake, pollIntervalMs = 10_000L)

        val picks = listOf(
            FakeTransport.pick("a.mp4"),
            FakeTransport.pick("b.mp4"),
            FakeTransport.pick("c.mp4"),
        )
        val started = client.beginBatch("video-convert", picks)

        assertEquals(3, started.size)
        // File 2 failed honestly with the engine's error; 1 and 3 prepared.
        assertNull(started[1].sessionId)
        assertEquals("source-unavailable", started[1].failure?.code)
        assertTrue(started[0].sessionId != null)
        assertTrue(started[2].sessionId != null)
        // The failed half-created session was closed, not leaked.
        assertTrue(fake.closedSessions.contains("s-1"))
    }

    @Test
    fun submitAllCreatesOneJobPerValidFile() = runTest {
        val fake = FakeTransport()
        // s-0 and s-2 validate; s-1 does not (its source cannot satisfy
        // the accepted configuration).
        fake.prepareSessionHandler = { sessionId ->
            FakeTransport.sessionSnapshot(
                sessionId,
                operation = fake.sessions[sessionId]?.operation ?: "video-convert",
                state = "ready",
                revision = 1,
                configurationValid = sessionId != "s-1",
            )
        }
        val client = EngineClient(transport = fake, pollIntervalMs = 10_000L)

        val picks = listOf(FakeTransport.pick("a.mp4"), FakeTransport.pick("b.mp4"), FakeTransport.pick("c.mp4"))
        val started = client.beginBatch("video-convert", picks)
        val entries = started.mapNotNull { result ->
            result.sessionId?.let { Destination.BatchEntry(name = result.pick.displayName, sessionId = it) }
        }
        val controller = BatchWorkflowController(
            operation = "video-convert",
            entries = entries,
            client = client,
            navigator = com.velmux.mobile.navigation.Navigator(),
            scope = kotlinx.coroutines.CoroutineScope(kotlinx.coroutines.Dispatchers.Unconfined),
        )

        assertTrue(controller.anyValid())
        controller.submitAll()
        runCurrent()

        // One job per VALID file only; the invalid file stayed unsubmitted.
        assertEquals(2, fake.submitCalls)
        assertEquals(
            setOf("s-0", "s-2"),
            fake.submissions.map { it.first }.toSet(),
        )
    }
}
