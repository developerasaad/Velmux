// Velmux core — configuration session manager (mandate §14).
//
// Owns the session lifecycle: creation, source preparation (inspection),
// supported-option derivation (mandate §13), transactional field updates
// with revision control and provenance, sample generation, idempotent
// submission and session closure.
//
// The session manager owns no execution: it resolves user intent into
// typed JobRequest payloads and delegates feasibility/estimates to the
// planner hook and durable execution to the submit hook (both wired by
// the engine).
#pragma once

#include <velmux/configuration.h>
#include <velmux/jobs.h>
#include <velmux/media.h>
#include <velmux/types.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace velmux::core {

class FeatureRegistry;
class CapabilityRegistry;

// Feasibility + estimate summary the planner hook returns for a candidate
// configuration (the engine wires this to the real Planner).
struct ConfigurationAssessment {
  bool feasible = false;
  std::string message;
  OutputEstimate estimate;
  std::string pipelineSummary;
};

// Engine wiring points for the session manager.
struct SessionHooks {
  std::function<Result<MediaInfo>(const MediaSource &)> inspect;
  std::function<Result<ConfigurationAssessment>(const JobRequest &,
                                                const std::vector<MediaInfo> &)>
      assess;
  std::function<Result<std::string>(const JobRequest &,
                                    const std::vector<MediaInfo> &,
                                    const std::string &submissionId)>
      submit;
  std::function<Result<SampleResult>(const JobRequest &)> generateSample;
};

class SessionManager {
public:
  SessionManager(const FeatureRegistry &features,
                 const CapabilityRegistry &capabilities, SessionHooks hooks);
  ~SessionManager();

  SessionManager(const SessionManager &) = delete;
  SessionManager &operator=(const SessionManager &) = delete;

  // Creates a session for one operation over one or more sources
  // (Merge uses several; every other operation uses exactly one).
  // The session starts in Created state; call prepareSession.
  Result<std::string> createSession(Operation operation,
                                    std::vector<MediaSource> sources);

  // Inspects the registered sources and derives the initial accepted
  // configuration (built-in defaults resolved from the actual media:
  // Original format when writable, Automatic codec, Original geometry and
  // timing, compression disabled). Synchronous; hosts call off the UI
  // thread. Idempotent for already-prepared sessions (re-inspects only
  // when forced).
  Result<SessionSnapshot> prepareSession(const std::string &sessionId,
                                         bool force = false);

  Result<SessionSnapshot> getSnapshot(const std::string &sessionId);

  // Supported values for one field under the current accepted
  // configuration (mandate §13.3: unsupported choices are omitted, not
  // shown disabled).
  Result<FieldOptions> getOptions(const std::string &sessionId, FieldId field,
                                  std::uint64_t revision);

  // Transactional update (mandate §14.2): validate identity + revision ->
  // validate field/value -> construct candidate -> resolve automatic
  // dependencies -> validate the complete candidate -> commit or reject.
  // A required related change returns the proposal without applying it;
  // the UI re-sends the update with acceptRelatedChange to commit.
  Result<SessionSnapshot> updateSession(const std::string &sessionId,
                                        std::uint64_t expectedRevision,
                                        const std::vector<FieldChange> &changes,
                                        bool acceptRelatedChange);

  Result<SampleResult> generateSample(const std::string &sessionId,
                                      std::uint64_t expectedRevision);

  // Durable idempotent submission: the same submissionId returns the same
  // jobId without creating a duplicate job (mandate §14.2).
  Result<std::string> submitSession(const std::string &sessionId,
                                    std::uint64_t expectedRevision,
                                    const std::string &submissionId);

  Result<void> closeSession(const std::string &sessionId);

  // Reuse settings (mandate §37): replaces the session's ACCEPTED request
  // with a stored job's request verbatim (a new editable configuration
  // seeded from the completed intent), re-assesses it and bumps the
  // revision. Only for prepared (Ready) sessions.
  Result<SessionSnapshot> seedAccepted(const std::string &sessionId,
                                       const JobRequest &request);

  // Drop all sessions (engine shutdown).
  void closeAll();

private:
  struct Session;
  using SessionPtr = std::shared_ptr<Session>;

  SessionSnapshot snapshotOf(const Session &session) const;

  // Derives the accepted-value entries (valueId + payload, round-trip
  // stable with applyChange) from the session's accepted request.
  static void deriveAcceptedValues(const Session &session,
                                   SessionSnapshot &snapshot);

  Result<SessionPtr> lockedSession(const std::string &sessionId,
                                   std::unique_lock<std::mutex> &lock);

  // Per-operation initial accepted configuration derived from the
  // inspected sources (defaults with Automatic/Default provenance).
  JobRequest initialRequest(Operation operation,
                            const std::vector<MediaSource> &sources,
                            const std::vector<MediaInfo> &infos) const;

  // Field validation + application on a candidate payload. Returns an
  // error (rejected) or applies the change; may set a related change on
  // the session when an explicit accepted value conflicts.
  Error applyChange(Session &session, const FieldChange &change,
                    JobRequest &candidate, bool acceptRelatedChange,
                    std::optional<RelatedChange> &outRelated) const;

  // Automatic dependency resolution on a candidate (mandate §14.3): only
  // fields with Automatic/Default provenance are re-resolved; explicit
  // user selections produce related changes instead.
  void resolveAutomatic(Session &session, JobRequest &candidate,
                        std::optional<RelatedChange> &outRelated) const;

  // Supported-option derivation (mandate §13).
  FieldOptions deriveOptions(const Session &session, FieldId field) const;

  // Helpers used by derivation and validation.
  bool containerWritable(Container container) const;
  bool containerReadable(Container container) const;
  bool videoEncodeSupported(VideoCodec codec, Container container) const;
  bool audioEncodeSupported(AudioCodec codec, Container container) const;
  bool audioDecodeSupported(AudioCodec codec) const;
  bool videoDecodeSupported(VideoCodec codec) const;
  std::vector<Container> writableContainersFor(Operation op) const;
  std::vector<VideoCodec> encodableVideoCodecsFor(Container container) const;
  std::vector<AudioCodec> encodableAudioCodecsFor(Container container) const;
  std::vector<AudioCodec> extractableAudioCodecsFor(Container container) const;

  static std::string generateSessionId();

  const FeatureRegistry &features_;
  const CapabilityRegistry &capabilities_;
  SessionHooks hooks_;

  std::mutex mutex_;
  std::map<std::string, SessionPtr> sessions_;
};

} // namespace velmux::core
