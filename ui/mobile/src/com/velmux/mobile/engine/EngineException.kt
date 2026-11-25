package com.velmux.mobile.engine

/**
 * An engine-side failure decoded from the ABI error envelope
 * {"error":{"code":"...","message":"..."}} (or an honest transport-level
 * failure, e.g. no engine behind a dev transport).
 *
 * The [code] is the stable canonical identifier from core types.h
 * (`errorCodeName`); the UI reacts to a small closed set of them
 * (stale-revision, related-change-required, duplicate-submission).
 */
class EngineException(
    val code: String,
    message: String,
) : Exception(message) {
    val error: EngineError = EngineError(code, message)

    fun isStaleRevision(): Boolean = code == EngineErrorCode.STALE_REVISION

    fun isDuplicateSubmission(): Boolean = code == EngineErrorCode.DUPLICATE_SUBMISSION

    fun isRelatedChangeRequired(): Boolean =
        code == EngineErrorCode.RELATED_CHANGE_REQUIRED
}
