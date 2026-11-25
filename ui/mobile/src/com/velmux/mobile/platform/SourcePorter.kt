package com.velmux.mobile.platform

/** A picked document resolved to an engine-readable reference. */
data class ResolvedSource(
    /** The URI the engine can open (after platform resolution). */
    val uri: String,
    val displayName: String,
    /**
     * True when this platform must IMPORT (copy) the document before the
     * engine can read it (Android SAF content URIs, iOS security-scoped
     * URLs). False when the reference is directly readable.
     */
    val requiresImport: Boolean,
)

/** A source the engine can read directly. */
data class ImportedSource(
    val uri: String,
    val displayName: String,
)

/**
 * Platform source porting: turning picked documents into engine-readable
 * sources, importing ONLY when the platform requires it (the honest
 * "importing" phase of the preparation flow).
 */
expect object SourcePorter {

    /** Resolves a pick (fast metadata resolution; no heavy IO). */
    suspend fun resolve(pick: PickedFile): ResolvedSource

    /**
     * Imports a resolved source that [ResolvedSource.requiresImport] into
     * engine-readable storage; returns the readable reference.
     */
    suspend fun import(source: ResolvedSource): ImportedSource
}
