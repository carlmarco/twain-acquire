#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fs.hpp"
#include "kv.hpp"
#include "layout.hpp"
#include "staging.hpp"

namespace ta {

// Where and how images leave the application.
struct ExportTarget {
  enum class Format { DicomPresentation, DicomProcessing, Tiff16, Tiff8 };

  std::string id;
  std::string name;
  bool enabled = true;
  Format format = Format::DicomPresentation;
  fs::Path folder;
  std::string fileNamePattern = "{patient-id}_{date}_{time}_{seq}";
  bool applyOrientation = true;  // bake rotation/mirroring into the pixels
  bool scaleToFullRange = false; // 16-bit TIFF: stretch bitsStored to the full 16 bits
  bool windowFor8Bit = false;    // 8-bit TIFF: apply the automatic display window, as on screen

  // DICOM
  std::string uidRoot;           // empty = UUID-derived 2.25 UIDs
  std::string institution;
  std::string manufacturer;
  std::string modelName;
  std::string intensityRelationship = "LIN";
  int intensitySign = 1;
  bool writeWindow = true;

  Kv toKv() const;
  static ExportTarget fromKv(const Kv& kv);
  std::string extension() const;
};

const char* formatName(ExportTarget::Format f);

struct ExportJob {
  enum class State { Pending, Done, Failed };

  std::string id;
  State state = State::Pending;
  std::string targetId;
  fs::Path sessionDir;
  uint32_t seq = 0;
  std::string outName;
  int attempts = 0;
  int64_t nextTryUnixMs = 0;
  std::string lastError;
  Kv snapshot;  // patient, study, slot and orientation as confirmed by the operator

  Kv toKv() const;
  static ExportJob fromKv(const Kv& kv);
};

// A durable queue of export jobs, processed by one background thread. Jobs are
// files under <root>/queue, so a crash or restart loses none; a job is only
// marked done after its output file has been completely written.
class ExportQueue {
 public:
  using TargetLookup = std::function<bool(const std::string& id, ExportTarget& out)>;

  ExportQueue(fs::Path root, TargetLookup targets);
  ~ExportQueue();

  // One job per kept image and enabled target. Returns the number of jobs.
  int enqueueSession(const fs::Path& sessionDir, const SessionState& session, const Layout& layout,
                     const std::vector<ExportTarget>& targets, std::string* err = nullptr);

  void start();
  void stop();
  void retryFailed();

  // Jobs still waiting or failed. Finished jobs leave memory: their files
  // move to queue/done and are only counted, so the queue's cost does not
  // grow with the practice's history.
  std::vector<ExportJob> snapshot() const;

  struct Counts {
    int pending = 0;
    int failed = 0;
    uint64_t doneSinceStart = 0;
    std::string lastError;  // of the most recent failed job
  };
  Counts counts() const;

  // Retention for patient data at rest. A closed session whose journal has
  // not changed for `days` and that no waiting or failed job still needs is
  // deleted with its images, and so are finished-job files older than that.
  // Open (unexported) sessions are never deleted. 0 = keep everything.
  void setRetentionDays(int days);
  // Runs the purge now (the worker also runs it hourly). Returns the number
  // of sessions and job files removed.
  int purgeNow();

  // Called on the worker thread after any job changes state.
  std::function<void()> onChanged;

  int maxAttempts = 8;

  // Exposed for tests: run one job now, synchronously.
  bool perform(ExportJob& job, std::string* err);

 private:
  void run();
  bool persist(const ExportJob& job, std::string* err);
  void loadAll();

  fs::Path root_;
  TargetLookup targets_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::map<std::string, ExportJob> jobs_;
  std::thread worker_;
  bool stopping_ = false;
  uint64_t doneSinceStart_ = 0;
  int retentionDays_ = 0;
  int64_t lastPurgeMs_ = 0;
};

}  // namespace ta
