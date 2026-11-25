package com.velmux.mobile.engine

import com.velmux.mobile.platform.PickedFile
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * EngineClient tests with the fake transport (spec UI-1): session
 * lifecycle happy path, stale-revision error path, event coalescing,
 * terminal event -> full jobs refresh, and submission idempotency with
 * durable submission ids.
 *
 * Virtual time: runCurrent() settles the bootstrap; advanceTimeBy(250)
 * drives exactly one poll tick; advanceUntilIdle() is only used while the
 * loop is parked (no ongoing jobs, no listeners) so it always converges.
 */
class EngineClientTest {

    private fun transport(): FakeTransport = FakeTransport()

    private fun client(transport: FakeTransport, pollMs: Long = 250L): EngineClient =
        EngineClient(transport = transport, pollIntervalMs = pollMs)

    // ---- lifecycle ---------------------------------------------------------------

    @Test
    fun startsReadyAndLoadsDiagnosticsAndJobs() = runTest {
        val fake = transport()
        fake.jobsById["j-0"] = FakeTransport.jobSnapshot("j-0", "completed")
        val engine = client(fake)

        engine.start(backgroundScope)
        advanceUntilIdle()

        assertEquals(EngineStatus.Ready, engine.engineStatus.value)
        val info = assertNotNull(engine.engineInfo.value)
        assertEquals(Abi.VERSION, info.abiVersion)
        assertEquals("1.0.0-test", info.engineVersion)
        assertEquals(listOf("ffmpeg"), info.backends)
        assertEquals(1, engine.jobs.value.size)
        assertEquals("j-0", engine.jobs.value.single().id)
    }

    @Test
    fun abiMismatchFailsLoudlyWithoutEngineCalls() = runTest {
        val fake = transport()
        fake.abiVersionResult = 3
        val engine = client(fake)

        engine.start(backgroundScope)
        advanceUntilIdle()

        val status = engine.engineStatus.value
        assertTrue(status is EngineStatus.AbiMismatch)
        assertEquals(3, status.found)
        assertEquals(Abi.VERSION, status.expected)
        assertEquals(0, fake.listJobsCalls)
    }

    // ---- session lifecycle happy path -----------------------------------------------

    @Test
    fun sessionLifecycleHappyPath() = runTest {
        val fake = transport()
        val engine = client(fake)
        engine.start(backgroundScope)
        advanceUntilIdle()

        val sessionId = engine.beginWorkflow("video-convert", listOf(FakeTransport.pick()))

        assertEquals("s-0", sessionId)
        val state = engine.preparation.value
        assertTrue(
            state is PreparationState.Ready || state is PreparationState.Configuring,
            "expected a prepared session, got $state",
        )
        val snapshot = assertNotNull(engine.sessions.value["s-0"])
        assertEquals("video-convert", snapshot.operation)
        assertEquals(1, fake.createSessionCalls)
        assertEquals(1, fake.prepareSessionCalls)

        // An accepted update refreshes the single source of truth.
        val updated = engine.updateSession(
            "s-0",
            listOf(FieldChange(field = "format", valueId = "mkv")),
        )
        assertNotNull(updated)
        assertEquals(2, updated.revision)
        assertEquals(2, engine.sessions.value.getValue("s-0").revision)
        assertEquals(1, fake.updateSessionCalls)

        // Submission returns the job id, closes the session and refreshes
        // the jobs list (the loop would spin on the queued job — no
        // advanceUntilIdle here, the refresh is synchronous).
        fake.jobsById["j-1"] = FakeTransport.jobSnapshot("j-1", "queued")
        val jobId = engine.submitSession("s-0")

        assertEquals("j-1", jobId)
        assertTrue(fake.submissions.single().third.startsWith("u-"))
        assertEquals(1, fake.closeSessionCalls)
        assertNull(engine.sessions.value["s-0"])
        assertTrue(engine.jobs.value.any { it.id == "j-1" })
    }

    @Test
    fun inspectionFailureFailsThePreparationHonestly() = runTest {
        val fake = transport()
        fake.prepareSessionHandler = { throw EngineException("inspection-failed", "no demuxer") }
        val engine = client(fake)
        engine.start(backgroundScope)
        advanceUntilIdle()

        val sessionId = engine.beginWorkflow("video-convert", listOf(FakeTransport.pick()))

        assertNull(sessionId)
        val state = engine.preparation.value
        assertTrue(state is PreparationState.Failed)
        assertEquals("inspection-failed", state.error.code)
        assertNotNull(engine.banner.value)
        // The half-created session was closed, not leaked.
        assertEquals(1, fake.closeSessionCalls)
    }

    // ---- stale revision error path ---------------------------------------------------

    @Test
    fun staleRevisionSurfacesAsAStateRefreshNeverAModelReset() = runTest {
        val fake = transport()
        val stale = EngineException(EngineErrorCode.STALE_REVISION, "configuration changed")
        fake.updateHandler = { _, _ -> throw stale }
        var refreshes = 0
        fake.getSnapshotHandler = { sessionId ->
            refreshes++
            FakeTransport.sessionSnapshot(
                sessionId,
                operation = "video-convert",
                state = "ready",
                revision = 9,
                configurationValid = true,
            )
        }
        val engine = client(fake)
        engine.start(backgroundScope)
        advanceUntilIdle()

        engine.beginWorkflow("video-convert", listOf(FakeTransport.pick()))

        val result = engine.updateSession(
            "s-0",
            listOf(FieldChange(field = "format", valueId = "mkv")),
        )

        // The update was refused, but the client ADOPTED the engine's fresh
        // snapshot (single source of truth) instead of keeping a stale one.
        assertNull(result)
        assertEquals(9, engine.sessions.value.getValue("s-0").revision)
        assertTrue(refreshes > 0)
        // A stale revision is not surfaced as a user-facing error banner.
        assertNull(engine.banner.value)
    }

    // ---- event coalescing ----------------------------------------------------------------

    @Test
    fun pollLoopCoalescesProgressSpamPerJob() = runTest {
        val fake = transport()
        fake.jobsById["j-1"] = FakeTransport.jobSnapshot(
            "j-1", "running", progress = 0.0,
        )
        val engine = client(fake)
        engine.start(backgroundScope)
        runCurrent()
        assertEquals(0.0, engine.jobs.value.single().progress)

        // One drained batch with three progress events for the same job:
        // only the LATEST may be applied (progress spam is superseded).
        fake.pollBatches.add(
            listOf(
                JobEvent("j-1", "running", progress = 0.1),
                JobEvent("j-1", "running", progress = 0.5),
                JobEvent("j-1", "running", progress = 0.9, outputSizeBytes = 42),
            )
        )
        advanceTimeBy(250)
        runCurrent()

        val job = engine.jobs.value.single()
        assertEquals(0.9, job.progress)
        assertEquals(42, job.outputSizeBytes)
        // Coalesced within one tick: no full refresh was forced.
        assertEquals(1, fake.listJobsCalls)
    }

    @Test
    fun twoJobsInOneBatchKeepIndependentLatestStates() = runTest {
        val fake = transport()
        fake.jobsById["j-1"] = FakeTransport.jobSnapshot("j-1", "running", progress = 0.0)
        fake.jobsById["j-2"] = FakeTransport.jobSnapshot("j-2", "running", progress = 0.0)
        val engine = client(fake)
        engine.start(backgroundScope)
        runCurrent()

        fake.pollBatches.add(
            listOf(
                JobEvent("j-1", "running", progress = 0.3),
                JobEvent("j-2", "running", progress = 0.7),
                JobEvent("j-1", "running", progress = 0.4),
            )
        )
        advanceTimeBy(250)
        runCurrent()

        val byId = engine.jobs.value.associateBy { it.id }
        assertEquals(0.4, byId.getValue("j-1").progress)
        assertEquals(0.7, byId.getValue("j-2").progress)
    }

    // ---- terminal event -> full jobs refresh ------------------------------------------------

    @Test
    fun terminalEventForcesAFullJobsRefresh() = runTest {
        val fake = transport()
        fake.jobsById["j-1"] = FakeTransport.jobSnapshot("j-1", "running", progress = 0.5)
        val engine = client(fake)
        engine.start(backgroundScope)
        runCurrent()
        val listsAfterBootstrap = fake.listJobsCalls
        assertTrue(listsAfterBootstrap > 0)

        // The engine already recorded the terminal state durably.
        fake.jobsById["j-1"] = fake.jobsById.getValue("j-1")
            .copy(state = "completed", progress = 1.0, finishedAtMs = 9_999)
        fake.pollBatches.add(
            listOf(JobEvent("j-1", "completed", progress = 1.0, outputSizeBytes = 900_000))
        )
        advanceTimeBy(250)
        runCurrent()

        // The terminal event forced a full reconciliation with the store.
        assertTrue(fake.listJobsCalls > listsAfterBootstrap)
        val job = engine.jobs.value.single()
        assertEquals("completed", job.state)
        assertEquals(9_999, job.finishedAtMs)
    }

    @Test
    fun unknownJobEventTriggersReconciliation() = runTest {
        val fake = transport()
        val engine = client(fake)
        engine.start(backgroundScope)
        runCurrent()
        // A listener keeps the poll loop alive (no active work otherwise).
        val listener = engine.addListener()
        runCurrent()
        val listsAfterBootstrap = fake.listJobsCalls

        // An event for a job the client has never seen (fresh submission
        // from another surface): reconcile instead of fabricating a row.
        fake.jobsById["j-9"] = FakeTransport.jobSnapshot("j-9", "queued")
        fake.pollBatches.add(listOf(JobEvent("j-9", "queued", progress = -1.0)))
        advanceTimeBy(250)
        runCurrent()

        assertTrue(fake.listJobsCalls > listsAfterBootstrap)
        assertEquals(1, engine.jobs.value.size)
        listener.dispose()
    }

    // ---- submission idempotency (durable submission ids) ---------------------------------------

    @Test
    fun submissionIdIsDurableAndDuplicateResolvesThroughTheJobStore() = runTest {
        val fake = transport()
        var attempts = 0
        fake.submitHandler = { _, _, _ ->
            attempts++
            if (attempts == 1) {
                // The first attempt never reached the durable store.
                throw EngineException("backend-failure", "worker died")
            }
            // The retry with the SAME id is recognized as a duplicate.
            throw EngineException(
                EngineErrorCode.DUPLICATE_SUBMISSION,
                "already accepted",
            )
        }
        val engine = client(fake)
        engine.start(backgroundScope)
        advanceUntilIdle()

        engine.beginWorkflow("video-convert", listOf(FakeTransport.pick()))

        // The first submit fails honestly.
        assertNull(engine.submitSession("s-0"))
        // The engine DID accept the retried id durably (that is what
        // duplicate-submission means).
        fake.jobsById["j-dup"] = FakeTransport.jobSnapshot(
            "j-dup", "queued", submissionId = fake.submissions.single().third,
        )

        // The retry reuses the SAME submission id and resolves through the
        // job store instead of submitting a third time.
        val jobId = engine.submitSession("s-0")

        assertEquals("j-dup", jobId)
        assertEquals(2, attempts)
        val ids = fake.submissions.map { it.third }.distinct()
        assertEquals(1, ids.size)
        assertTrue(ids.single().startsWith("u-"))
    }

    @Test
    fun successfulSubmissionClearsThePendingId() = runTest {
        val fake = transport()
        val engine = client(fake)
        engine.start(backgroundScope)
        advanceUntilIdle()

        engine.beginWorkflow("video-convert", listOf(FakeTransport.pick()))
        assertEquals("j-1", engine.submitSession("s-0"))

        // A second session gets a FRESH submission id (the previous one
        // was consumed by success).
        engine.beginWorkflow("video-convert", listOf(FakeTransport.pick()))
        assertEquals("j-2", engine.submitSession("s-1"))

        val ids = fake.submissions.map { it.third }
        assertEquals(2, ids.distinct().size)
    }

    // ---- listeners keep the poll loop alive ---------------------------------------------------

    @Test
    fun listenersKeepThePollLoopAliveWithoutActiveWork() = runTest {
        val fake = transport()
        val engine = client(fake)
        engine.start(backgroundScope)
        advanceUntilIdle()
        val pollsWhileIdle = fake.pollEventsCalls

        // No jobs, no listeners: the loop parks (no further polls).
        advanceTimeBy(2_500)
        runCurrent()
        assertEquals(pollsWhileIdle, fake.pollEventsCalls)

        // A registered listener wakes the loop.
        val listener = engine.addListener()
        runCurrent()
        advanceTimeBy(250)
        runCurrent()
        assertTrue(fake.pollEventsCalls > pollsWhileIdle)
        listener.dispose()
    }

    // ---- job control ----------------------------------------------------------------------------

    @Test
    fun jobControlRefreshesAfterEachAction() = runTest {
        val fake = transport()
        fake.jobsById["j-1"] = FakeTransport.jobSnapshot("j-1", "running", progress = 0.5)
        val engine = client(fake)
        engine.start(backgroundScope)
        runCurrent()

        assertTrue(engine.pauseJob("j-1"))
        assertEquals("paused", fake.jobsById.getValue("j-1").state)
        assertTrue(engine.resumeJob("j-1"))
        assertEquals("running", fake.jobsById.getValue("j-1").state)
        assertTrue(engine.cancelJob("j-1"))
        assertEquals("canceled", fake.jobsById.getValue("j-1").state)
        assertTrue(engine.retryJob("j-1"))
        assertEquals("queued", fake.jobsById.getValue("j-1").state)
        assertTrue(engine.deleteJobRecord("j-1"))
        assertNull(fake.jobsById["j-1"])
        // Let the scheduled refresh run before asserting the flow state.
        runCurrent()
        assertFalse(engine.jobs.value.any { it.id == "j-1" })
    }
}
