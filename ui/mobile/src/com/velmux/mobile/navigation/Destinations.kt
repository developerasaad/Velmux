package com.velmux.mobile.navigation

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Typed routes (spec UI-1): Home, Jobs, Settings are top-level tabs;
 * workflow routes carry the operation and, once a session exists, the
 * sessionId. The workflow CONFIGURATION screens are the explicit follow-up
 * surface (UI-2) — the routes are stable from day one so the follow-up
 * plugs in without navigation changes.
 */
sealed interface Destination {
    data object Home : Destination
    data object Jobs : Destination
    data object Settings : Destination

    /** A workflow configuration screen for [operation] (follow-up: UI-2). */
    data class Workflow(
        val operation: String,
        val sessionId: String? = null,
    ) : Destination

    /**
     * One batch entry: the file's display name and the session the engine
     * prepared for it (mandate §40: every source is an independently
     * resolved operation; the name rides the route because session
     * snapshots do not echo source display names).
     */
    data class BatchEntry(
        val name: String,
        val sessionId: String,
    )

    /**
     * The batch configuration surface for [operation] (mandate §40):
     * shared user intent broadcast to every session, per-file states and
     * estimates, individual overrides through the single-file workflow
     * screen pushed on top of this route.
     */
    data class WorkflowBatch(
        val operation: String,
        val entries: List<BatchEntry>,
    ) : Destination

    companion object {
        val topLevel: List<Destination> = listOf(Home, Jobs, Settings)
    }
}

/**
 * A small, real, typed back-stack navigator (no external navigation
 * dependency). Top-level destinations reset the stack (tab semantics);
 * workflow destinations push and pop.
 */
class Navigator(initial: Destination = Destination.Home) {

    private val backStack = MutableStateFlow(listOf(initial))
    val stack: StateFlow<List<Destination>> = backStack

    val current: Destination get() = backStack.value.last()

    /** Opens a top-level tab (resets the stack) or pushes a detail route. */
    fun open(destination: Destination) {
        if (destination in Destination.topLevel) {
            backStack.value = listOf(destination)
        } else if (backStack.value.last() != destination) {
            backStack.value = backStack.value + destination
        }
    }

    /** Replaces the top of the stack (e.g. session id became known). */
    fun replaceTop(destination: Destination) {
        backStack.value = backStack.value.dropLast(1) + destination
    }

    /** Pops one detail route; returns false when already at the root. */
    fun back(): Boolean {
        if (backStack.value.size <= 1) return false
        backStack.value = backStack.value.dropLast(1)
        return true
    }
}
