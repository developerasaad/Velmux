// Velmux — public C ABI, the versioned application integration contract
// (mandate §35).
//
// Consumers: the shared Kotlin engine client (JNI on Android, C interop on
// iOS) and integration tests. The ABI is the ONLY boundary between the
// shared UI and the authoritative C++ core: opaque handles, explicit
// versions, explicit string ownership, structured errors, sequenced events
// and configuration revisions.
//
// Transport: JSON payloads (UTF-8). Every string-returning function
// produces either the documented payload or {"error":{"code":"...","message":"..."}}
// and the caller releases it with velmux_free_string. All payloads pass the
// same authoritative core validation as the session path; there is no
// permissive back door.
//
// ABI versioning: VELMUX_ABI_VERSION is the contract revision. Hosts verify
// it at load time; mismatched versions fail loudly (velmux_engine_create
// returns null) instead of silently misinterpreting payloads.
#pragma once

#define VELMUX_ABI_VERSION 2

#ifdef __cplusplus
extern "C" {
#endif

// Opaque engine handle. One engine instance per application process
// (mandate §10.1); a UI screen attaching/detaching never creates another.
typedef struct VelmuxEngineHandle VelmuxEngineHandle;

// Runtime ABI version check: always returns VELMUX_ABI_VERSION of the
// loaded library. Hosts compare before creating an engine.
int velmux_abi_version(void);

// Engine version label (e.g. "1.0.0"); informational.
const char *velmux_engine_version(void);

// Creates the engine (opens the durable job store under dataDir,
// initializes backends, runs crash recovery). Returns null on failure
// (unusable data directory or no backend compiled in).
VelmuxEngineHandle *velmux_engine_create(const char *dataDir);
void velmux_engine_destroy(VelmuxEngineHandle *engine);

// ---------------------------------------------------------------------------
// Configuration sessions (mandate §14). Payloads:
//   create:  {"operation":"video-convert","sources":[{"uri":..,"name":..}]}
//            -> {"sessionId":"s-..."}
//   prepare: synchronous inspection; -> session snapshot
//   snapshot: -> session snapshot
//   options: {"field":"format","revision":N} -> {"field":"format",
//            "values":[{"id":..,"attributes":{..}}]}
//   update:  {"revision":N,"acceptRelatedChange":bool,
//            "changes":[{"field":"format","valueId":"mp4","payload":{..}}]}
//            -> session snapshot (or error / related-change proposal)
//   sample:  {"revision":N} -> {"path":..,"startUs":..,"endUs":..,"sizeBytes":..}
//   submit:  {"revision":N,"submissionId":"u-..."} -> {"jobId":"j-..."}
//
// Session snapshot schema:
//   {"sessionId":..,"operation":..,"state":"created|preparing|ready|failed",
//    "revision":N,"sources":[MediaInfo...],
//    "values":[{"field":..,"origin":"automatic|default|preset|user"}],
//    "configurationValid":bool,"validationMessage":..,
//    "estimate":{"known":bool,"estimatedBytes":N,"lowerBoundBytes":N,
//                "upperBoundBytes":N},
//    "relatedChange":{"field":..,"reason":..,"currentValueId":..,
//                     "proposedValueId":..},
//    "pipelineSummary":..}
// ---------------------------------------------------------------------------
char *velmux_create_session(VelmuxEngineHandle *engine,
                            const char *operationJson);
char *velmux_prepare_session(VelmuxEngineHandle *engine,
                             const char *sessionId);
char *velmux_get_session_snapshot(VelmuxEngineHandle *engine,
                                  const char *sessionId);
char *velmux_get_session_options(VelmuxEngineHandle *engine,
                                 const char *sessionId, const char *fieldJson);
char *velmux_update_session(VelmuxEngineHandle *engine,
                            const char *sessionId, const char *updateJson);
char *velmux_generate_session_sample(VelmuxEngineHandle *engine,
                                     const char *sessionId,
                                     const char *sampleJson);
char *velmux_submit_session(VelmuxEngineHandle *engine,
                            const char *sessionId, const char *submitJson);
int velmux_close_session(VelmuxEngineHandle *engine, const char *sessionId);

// ---------------------------------------------------------------------------
// Direct request submission (bypasses sessions but passes the SAME
// validation; used by tests, batch importers and migrated queued jobs).
// Payload: the typed JobRequest JSON:
//   {"operation":"video-convert","submissionId":"u-...",
//    "video-convert":{...typed payload...}}  -> {"jobId":"j-..."}
// ---------------------------------------------------------------------------
char *velmux_submit_job(VelmuxEngineHandle *engine, const char *requestJson);

// ---------------------------------------------------------------------------
// Jobs (mandate §37). Snapshot schema:
//   {"id":..,"submissionId":..,"operation":..,"state":..,"progress":0..1|-1,
//    "sourceName":..,"outputName":..,"outputPath":..,
//    "outputs":[{"path":..,"sizeBytes":N,"kind":..}],
//    "error":..,"errorCode":..,"outputSizeBytes":N,"inputSizeBytes":N,
//    "pipelineSummary":..,"createdAtMs":N,"finishedAtMs":N}
// ---------------------------------------------------------------------------
char *velmux_list_jobs(VelmuxEngineHandle *engine);
char *velmux_get_job(VelmuxEngineHandle *engine, const char *jobId);
// 1 while a worker is executing/finalizing the job, 0 when idle, -1 on a
// bad handle/id. Durable state may already read canceled/paused while a
// worker is still unwinding; hosts use this to distinguish "cancel
// recorded" from "cancel completed" before touching artifacts.
int velmux_is_job_active(VelmuxEngineHandle *engine, const char *jobId);
int velmux_cancel_job(VelmuxEngineHandle *engine, const char *jobId);
int velmux_pause_job(VelmuxEngineHandle *engine, const char *jobId);
int velmux_resume_job(VelmuxEngineHandle *engine, const char *jobId);
// Re-queue a failed/canceled/interrupted durable job at high priority
// (preserves the accepted intent).
int velmux_retry_job(VelmuxEngineHandle *engine, const char *jobId);
// Delete a terminal job's durable record (outputs are not touched).
int velmux_delete_job(VelmuxEngineHandle *engine, const char *jobId);

// Reuse settings: build a session pre-filled with a completed job's
// accepted intent (a NEW editable configuration; the original job is
// immutable). -> {"sessionId":..}
char *velmux_reuse_job_settings(VelmuxEngineHandle *engine,
                                const char *jobId);

// ---------------------------------------------------------------------------
// Source inspection outside sessions (read-only media information).
// Payload: MediaInfo JSON.
// ---------------------------------------------------------------------------
char *velmux_inspect(VelmuxEngineHandle *engine, const char *uri,
                     const char *displayName);

// ---------------------------------------------------------------------------
// Diagnostics + platform integration
// ---------------------------------------------------------------------------

// Backend registry description (diagnostics panel).
char *velmux_describe_backends(VelmuxEngineHandle *engine);

// Update the resource snapshot the scheduler adapts to (thermal, power,
// memory, hardware availability). Payload:
// {"cpuCores":int,"memoryAvailableMb":int,"thermalPressure":0..1,
//  "onBattery":bool,"hardwareCodecAvailable":bool} (all fields optional).
// Returns 0 on success, -1 on a malformed payload.
int velmux_update_resources(VelmuxEngineHandle *engine, const char *json);

// Drain queued job events (oldest first) as a JSON array of event
// objects: {"jobId":..,"state":..,"progress":..,"error":..,"errorCode":..}.
// Hosts poll this at UI cadence; the queue is bounded and drops the oldest
// entries under extreme pressure (progress spam is superseded by newer
// state anyway).
char *velmux_poll_events(VelmuxEngineHandle *engine);

void velmux_free_string(char *str);

#ifdef __cplusplus
} // extern "C"
#endif
