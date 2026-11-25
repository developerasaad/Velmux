// Velmux core — SQLite job persistence.
//
// Durable job state (spec §24/§37): stable job ID, submission id, schema
// version, the full typed request (JSON), inspected source info, selected
// pipeline, output manifest, progress, timestamps, errors, error codes.
//
// Schema v4: the request is stored as full-fidelity typed JSON; the legacy
// k=v pair strings and the goal/processing-mode columns are migrated away
// in one transaction (every row preserved).
//
// Thin RAII wrapper over the sqlite3 C API. On Android/iOS the system
// SQLite is used; on host the system library.
#pragma once

#include <velmux/jobs.h>
#include "processing/pipeline.h"
#include <velmux/types.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <functional>

struct sqlite3;
struct sqlite3_stmt;

namespace velmux::core {

class SqliteStore {
public:
  SqliteStore();
  ~SqliteStore();

  SqliteStore(const SqliteStore &) = delete;
  SqliteStore &operator=(const SqliteStore &) = delete;

  // Opens (creating when absent) the database and applies schema
  // migrations. Returns an Error on failure.
  Error open(const std::string &path);
  void close();
  bool isOpen() const { return db_ != nullptr; }

  // Escapes a string for safe embedding in SQL literals. Prepared
  // statements are preferred; this exists for the rare dynamic case.
  std::string quote(const std::string &text) const;

  // Minimal statement API used by the stores.
  bool exec(const std::string &sql, Error &error);
  // Prepares a statement; caller steps it and finalizes via the handle.
  sqlite3_stmt *prepare(const std::string &sql, Error &error);
  sqlite3 *handle() const { return db_; }

  // Convenience: run a statement with a callback over rows.
  using RowCallback = std::function<bool(sqlite3_stmt &)>;
  bool query(const std::string &sql, const RowCallback &row, Error &error);

  // Last insert rowid.
  std::int64_t lastInsertRowId() const;

private:
  Error migrate();
  sqlite3 *db_ = nullptr;
};

// ---------------------------------------------------------------------------
// Job store
// ---------------------------------------------------------------------------

class JobStore {
public:
  explicit JobStore(SqliteStore &db) : db_(db) {}

  Error insertJob(const JobRecord &record);
  // State/progress persistence. NEVER touches the checkpoint table: a
  // state change (cancel, pause, retry, running, validating...) must not
  // destroy the committed checkpoints those very transitions rely on for
  // recovery (spec §25/§26). Checkpoint rows are removed only through
  // deleteJob (job removal) or explicit completion cleanup.
  Error updateJob(const JobRecord &record);
  std::optional<JobRecord> loadJob(const std::string &id, Error &error);
  std::vector<JobRecord> loadAllJobs(Error &error);
  // Jobs that were mid-flight when the process disappeared.
  std::vector<JobRecord> loadResumableJobs(Error &error);
  // Durable idempotency (mandate §14.2): the job created for a submission
  // id, when one exists.
  std::optional<JobRecord> findJobBySubmissionId(const std::string &submissionId,
                                                 Error &error);
  // Removes the job AND its checkpoint rows (cascade).
  Error deleteJob(const std::string &id);

  static constexpr std::int32_t kSchemaVersion = 4;

private:
  static JobRecord rowToRecord(sqlite3_stmt &stmt);
  // Deletes only the jobs-table row (no checkpoint cascade).
  Error deleteJobRow(const std::string &id);
  SqliteStore &db_;
};

} // namespace velmux::core
