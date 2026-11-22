// Velmux bridge — engine-side serialization for frontend bridges.
//
// The React Native TurboModule (apps/mobile) and future Kotlin/Swift/CLI
// hosts translate their native types into the engine's C++ types. This
// header provides the canonical JSON serialization of the bridge contract
// (apps/mobile/src/native/NativeVelmux.ts) so every frontend speaks the
// same wire format for events and snapshots.
//
// The bridge owns no policy: it forwards user intent in and job state out.
#pragma once

#include "velmux/core/engine.h"
#include "velmux/core/events.h"
#include "velmux/core/job.h"

#include <string>

namespace velmux::bridge {

// Serialize a JobSnapshot to the bridge JSON contract.
std::string snapshotToJson(const core::JobSnapshot &snapshot);

// Serialize a JobEvent to the bridge JSON contract (event name:
// "velmux.jobEvent").
std::string eventToJson(const core::JobEvent &event);

// Serialize a MediaInfo to the bridge JSON contract.
std::string mediaInfoToJson(const core::MediaInfo &info);

// Escape a string for JSON embedding.
std::string jsonEscape(const std::string &text);

// Parse a ConversionRequest from the bridge JSON contract. Returns nullopt
// and fills `error` when the payload is not a valid request.
std::optional<core::ConversionRequest>
requestFromJson(const std::string &json, std::string &error);

} // namespace velmux::bridge

// ---------------------------------------------------------------------------
// Stable C facade for hosts that register the engine from C/JSI without a
// C++ compiler (Kotlin JNI, Swift wrapper, CLI embedders).
//
// String-returning functions produce either the documented payload or an
// {"error": "..."} object; the caller frees with velmux_free_string.
// ---------------------------------------------------------------------------
extern "C" {

// Opaque engine handle.
typedef struct VelmuxEngineHandle VelmuxEngineHandle;

VelmuxEngineHandle *velmux_engine_create(const char *dataDir);
void velmux_engine_destroy(VelmuxEngineHandle *engine);

// Inspect a media source. Payload: MediaInfo JSON.
char *velmux_inspect(VelmuxEngineHandle *engine, const char *uri,
                     const char *displayName);

// Plan without creating a job. Payload:
// {"feasible":bool,"message":..,"estimatedOutputBytes":..,"pipelineSummary":..}
char *velmux_plan(VelmuxEngineHandle *engine, const char *requestJson);

// Start a durable job from a request. Payload: {"jobId":"..."}.
char *velmux_start_job(VelmuxEngineHandle *engine, const char *requestJson);

char *velmux_list_jobs(VelmuxEngineHandle *engine);
char *velmux_get_job(VelmuxEngineHandle *engine, const char *jobId);
char *velmux_describe_backends(VelmuxEngineHandle *engine);
int velmux_cancel_job(VelmuxEngineHandle *engine, const char *jobId);
int velmux_pause_job(VelmuxEngineHandle *engine, const char *jobId);
int velmux_resume_job(VelmuxEngineHandle *engine, const char *jobId);
// Re-queue a failed/canceled/interrupted durable job at high priority.
int velmux_retry_job(VelmuxEngineHandle *engine, const char *jobId);
// Delete a terminal job's durable record (outputs are not touched).
int velmux_delete_job(VelmuxEngineHandle *engine, const char *jobId);

// Update the resource snapshot the scheduler adapts to (thermal, power,
// memory, hardware availability). Payload:
// {"cpuCores":int,"memoryAvailableMb":int,"thermalPressure":0..1,
//  "onBattery":bool,"hardwareCodecAvailable":bool} (all fields optional).
// Returns 0 on success, -1 on a malformed payload.
int velmux_update_resources(VelmuxEngineHandle *engine, const char *json);

// Drain queued job events (oldest first) as a JSON array of event objects.
// Hosts poll this at UI cadence; the queue is bounded and drops the oldest
// entries under extreme pressure (progress spam is superseded by newer
// state anyway).
char *velmux_poll_events(VelmuxEngineHandle *engine);

void velmux_free_string(char *str);
}
