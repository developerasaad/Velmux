package com.velmux.mobile.features.jobs

import com.velmux.mobile.engine.JobStates
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * Jobs state mapping tests (spec UI-1): ALL fourteen canonical states map
 * to labels, terminal/ongoing classification and per-state action sets.
 */
class JobsStateMappingTest {

    @Test
    fun allFourteenStatesMapToUiEnums() {
        assertEquals(14, JobStates.ALL.size)
        assertEquals(14, JobStateUi.entries.size)
        JobStates.ALL.forEachIndexed { index, wire ->
            val ui = JobStateUi.of(wire)
            assertEquals(wire, ui.wire, "wire name roundtrip failed for $wire")
            assertEquals(JobStateUi.entries[index], ui)
            assertTrue(ui.label.isNotBlank(), "label missing for $wire")
        }
    }

    @Test
    fun unknownFutureStateFallsBackToOngoing() {
        val ui = JobStateUi.of("some-future-state")
        assertFalse(ui.isTerminal)
        assertTrue(ui.isOngoing)
    }

    @Test
    fun terminalStatesMatchTheEngineDefinition() {
        // isTerminalJobState (core jobs.h): completed, target-not-met,
        // failed, canceled — and nothing else.
        val terminal = JobStateUi.entries.filter { it.isTerminal }.map { it.wire }.toSet()
        assertEquals(JobStates.TERMINAL, terminal)
        assertTrue(JobStateUi.COMPLETED.isTerminal)
        assertTrue(JobStateUi.TARGET_NOT_MET.isTerminal)
        assertTrue(JobStateUi.FAILED.isTerminal)
        assertTrue(JobStateUi.CANCELED.isTerminal)
        assertTrue(!JobStateUi.PAUSED.isTerminal)
        assertTrue(!JobStateUi.INTERRUPTED.isTerminal)
    }

    @Test
    fun activeStatesMatchTheEngineDefinition() {
        val active = JobStates.ACTIVE
        assertTrue("running" in active)
        assertTrue("queued" in active)
        assertTrue("finalizing" in active)
        assertTrue("validating" in active)
        assertFalse("paused" in active)
        assertFalse("interrupted" in active)
        assertFalse("completed" in active)
    }

    @Test
    fun everyStateHasAOngoingOrTerminalClassification() {
        JobStateUi.entries.forEach { state ->
            assertTrue(state.isTerminal || state.isOngoing, "unclassified: $state")
        }
    }

    @Test
    fun queuedPreparingAndRecoveringOfferCancel() {
        for (wire in listOf(JobStates.QUEUED, JobStates.PREPARING, JobStates.RECOVERING)) {
            val actions = actionsFor(JobStateUi.of(wire))
            assertEquals(listOf(JobAction.CANCEL), actions, "actions for $wire")
        }
    }

    @Test
    fun runningOffersPauseAndCancel() {
        assertEquals(
            listOf(JobAction.PAUSE, JobAction.CANCEL),
            actionsFor(JobStateUi.RUNNING),
        )
    }

    @Test
    fun pausedOffersResumeOnly() {
        assertEquals(listOf(JobAction.RESUME), actionsFor(JobStateUi.PAUSED))
    }

    @Test
    fun inFlightStatesOfferNoActions() {
        for (wire in listOf(
                JobStates.PAUSING, JobStates.CANCELING, JobStates.FINALIZING,
                JobStates.VALIDATING,
            )
        ) {
            assertEquals(emptyList(), actionsFor(JobStateUi.of(wire)), "actions for $wire")
        }
    }

    @Test
    fun interruptedOffersRetry() {
        assertEquals(listOf(JobAction.RETRY), actionsFor(JobStateUi.INTERRUPTED))
    }

    @Test
    fun completedStatesOfferOutputActions() {
        for (state in listOf(JobStateUi.COMPLETED, JobStateUi.TARGET_NOT_MET)) {
            val actions = actionsFor(state)
            assertTrue(JobAction.OPEN in actions, "open missing for $state")
            assertTrue(JobAction.SHARE in actions, "share missing for $state")
            assertTrue(JobAction.REUSE in actions, "reuse missing for $state")
            assertTrue(JobAction.DELETE_RECORD in actions, "delete record missing for $state")
            assertTrue(JobAction.DELETE_OUTPUT in actions, "delete output missing for $state")
        }
    }

    @Test
    fun failedOffersRetryAndDelete() {
        val actions = actionsFor(JobStateUi.FAILED)
        assertTrue(JobAction.RETRY in actions)
        assertTrue(JobAction.DELETE_RECORD in actions)
        assertFalse(JobAction.OPEN in actions)
        assertFalse(JobAction.SHARE in actions)
    }

    @Test
    fun canceledOffersRetryAndDelete() {
        val actions = actionsFor(JobStateUi.CANCELED)
        assertEquals(listOf(JobAction.RETRY, JobAction.DELETE_RECORD), actions)
    }

    @Test
    fun filtersClassifyTheSameWay() {
        // Active = everything non-terminal (paused jobs are not results).
        JobsFilter.ACTIVE.let { filter ->
            JobStateUi.entries.forEach { state ->
                assertEquals(!state.isTerminal, filter.accepts(state), "filter $state")
            }
        }
        assertTrue(JobsFilter.COMPLETED.accepts(JobStateUi.COMPLETED))
        assertTrue(JobsFilter.COMPLETED.accepts(JobStateUi.TARGET_NOT_MET))
        assertFalse(JobsFilter.COMPLETED.accepts(JobStateUi.RUNNING))
        assertTrue(JobsFilter.FAILED.accepts(JobStateUi.FAILED))
        assertFalse(JobsFilter.FAILED.accepts(JobStateUi.COMPLETED))
        JobStateUi.entries.forEach { state ->
            assertTrue(JobsFilter.ALL.accepts(state))
        }
    }

    @Test
    fun operationsMapToWorkflows() {
        assertEquals("Convert video", operationLabel("video-convert"))
        assertEquals("Cut & merge video", operationLabel("video-cutmerge"))
        assertEquals("Extract audio", operationLabel("extract-audio"))
        // Unknown operations render their id (never a lie).
        assertEquals("future-op", operationLabel("future-op"))
    }
}
