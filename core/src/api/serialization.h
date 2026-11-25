// Velmux core — full-fidelity JSON serialization for the typed request
// model, media info, job snapshots/events and session payloads.
//
// One codec, two consumers (mandate §35.1: retain useful existing
// serialization): the durable job store (schema v4 request/source JSON) and
// the C ABI v2 transport. requestFromJson is a CORE VALIDATION PATH, not a
// permissive decoder: unknown operations, unknown enum names, wrong-typed
// fields and out-of-range numbers are rejected with the same rules the
// SessionManager applies.
#pragma once

#include <velmux/configuration.h>
#include <velmux/events.h>
#include <velmux/jobs.h>
#include <velmux/media.h>
#include <velmux/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace velmux::core {

// ---------------------------------------------------------------------------
// Typed JobRequest codec (ABI v2 + job store)
// ---------------------------------------------------------------------------

// A decoded request plus its submission id (the envelope carries it).
struct DecodedRequest {
  JobRequest request;
  std::string submissionId;
};

// Serialize a typed request to the canonical JSON:
//   {"operation":"video-convert","submissionId":"u-..",
//    "video-convert":{...payload...}}
// Payload object keys are the canonical operation names; fields mirror the
// configuration.h structs; nullopt serializes as null; integers as numbers.
// video-cutmerge carries "mode" plus the mode payload object
// ("trim"/"cut"/"split"/"merge").
std::string requestToJson(const JobRequest &request,
                          const std::string &submissionId = {});

// Strict decode. On the first problem returns nullopt and fills `error`
// with a human-readable reason.
std::optional<DecodedRequest> requestFromJson(const std::string &json,
                                              std::string &error);

// ---------------------------------------------------------------------------
// MediaInfo codec (existing bridge shape)
// ---------------------------------------------------------------------------

std::string mediaInfoToJson(const MediaInfo &info);
std::optional<MediaInfo> mediaInfoFromJson(const std::string &json,
                                           std::string &error);
// Whole source-inspection vectors (job store schema v4).
std::string mediaInfoArrayToJson(const std::vector<MediaInfo> &infos);
std::optional<std::vector<MediaInfo>>
mediaInfoArrayFromJson(const std::string &json, std::string &error);

// ---------------------------------------------------------------------------
// Job snapshot / event codec (abi.h schemas)
// ---------------------------------------------------------------------------

std::string jobSnapshotToJson(const JobSnapshot &snapshot);
std::string jobEventToJson(const JobEvent &event);

// ---------------------------------------------------------------------------
// Session payload codecs (abi.h schemas)
// ---------------------------------------------------------------------------

std::string sessionSnapshotToJson(const SessionSnapshot &snapshot);
std::string fieldOptionsToJson(const FieldOptions &options);
std::string sampleResultToJson(const SampleResult &sample);

// {"revision":N,"acceptRelatedChange":bool,
//  "changes":[{"field":"format","valueId":"mp4","payload":{..}}]}
struct SessionUpdateParse {
  std::uint64_t revision = 0;
  bool acceptRelatedChange = false;
  std::vector<FieldChange> changes;
};
std::optional<SessionUpdateParse>
sessionUpdateFromJson(const std::string &json, std::string &error);

} // namespace velmux::core
