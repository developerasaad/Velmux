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

} // namespace velmux::bridge

// ---------------------------------------------------------------------------
// Stable C facade for hosts that register the engine from C/JSI without a
// C++ compiler (Kotlin JNI, Swift wrapper, CLI embedders).
// ---------------------------------------------------------------------------
extern "C" {

// Opaque engine handle.
typedef struct VelmuxEngineHandle VelmuxEngineHandle;

VelmuxEngineHandle *velmux_engine_create(const char *dataDir);
void velmux_engine_destroy(VelmuxEngineHandle *engine);

// Returns a JSON string (caller frees with velmux_free_string) or nullptr.
char *velmux_inspect(VelmuxEngineHandle *engine, const char *uri,
                     const char *displayName);
char *velmux_list_jobs(VelmuxEngineHandle *engine);
char *velmux_get_job(VelmuxEngineHandle *engine, const char *jobId);
char *velmux_describe_backends(VelmuxEngineHandle *engine);
// Starts a job from a JSON request; returns the jobId JSON or an error JSON.
char *velmux_start_job(VelmuxEngineHandle *engine, const char *requestJson);
int velmux_cancel_job(VelmuxEngineHandle *engine, const char *jobId);
int velmux_pause_job(VelmuxEngineHandle *engine, const char *jobId);
int velmux_resume_job(VelmuxEngineHandle *engine, const char *jobId);

void velmux_free_string(char *str);
}
