package com.velmux.mobile.engine

/**
 * Job state vocabulary exactly as the engine emits it (jobStateName in
 * core/src/catalog/types.cpp). DTOs keep the wire string; this is the
 * closed vocabulary shared by the engine client and the Jobs UI.
 */
object JobStates {
    const val QUEUED = "queued"
    const val PREPARING = "preparing"
    const val RUNNING = "running"
    const val PAUSING = "pausing"
    const val PAUSED = "paused"
    const val CANCELING = "canceling"
    const val INTERRUPTED = "interrupted"
    const val RECOVERING = "recovering"
    const val FINALIZING = "finalizing"
    const val VALIDATING = "validating"
    const val COMPLETED = "completed"
    const val TARGET_NOT_MET = "target-not-met"
    const val FAILED = "failed"
    const val CANCELED = "canceled"

    /** All fourteen canonical states, in enum order. */
    val ALL: List<String> = listOf(
        QUEUED, PREPARING, RUNNING, PAUSING, PAUSED, CANCELING, INTERRUPTED,
        RECOVERING, FINALIZING, VALIDATING, COMPLETED, TARGET_NOT_MET, FAILED,
        CANCELED,
    )

    /**
     * Terminal states (isTerminalJobState, core jobs.h): completed,
     * target-not-met, failed, canceled. Terminal states carry output info.
     */
    val TERMINAL: Set<String> = setOf(COMPLETED, TARGET_NOT_MET, FAILED, CANCELED)

    /**
     * States with a worker executing/finalizing (isActiveJobState, core
     * jobs.h). Paused/interrupted durable jobs are NOT active work.
     */
    val ACTIVE: Set<String> = setOf(
        QUEUED, PREPARING, RUNNING, PAUSING, CANCELING, RECOVERING, FINALIZING,
        VALIDATING,
    )

    /**
     * Everything non-terminal — the honest "Active" bucket of the ONE Jobs
     * screen (paused/interrupted jobs still belong there: they are not
     * results, and the user can resume/retry them).
     */
    val ONGOING: Set<String> = ALL.filter { it !in TERMINAL }.toSet()
}
