#include "export.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <system_error>

#include "dicom.hpp"
#include "render.hpp"
#include "tiff.hpp"

namespace ta {

namespace {

bool fail(std::string* err, const std::string& what) {
  if (err) *err = what;
  return false;
}

ExportTarget::Format parseFormat(const std::string& s) {
  if (s == "dicom-processing") return ExportTarget::Format::DicomProcessing;
  if (s == "tiff16") return ExportTarget::Format::Tiff16;
  if (s == "tiff8") return ExportTarget::Format::Tiff8;
  return ExportTarget::Format::DicomPresentation;
}

const char* stateName(ExportJob::State s) {
  switch (s) {
    case ExportJob::State::Done: return "done";
    case ExportJob::State::Failed: return "failed";
    default: return "pending";
  }
}

std::string newJobId() {
  static std::mt19937_64 rng{std::random_device{}()};
  char buf[40];
  std::snprintf(buf, sizeof buf, "%013lld-%08x", static_cast<long long>(fs::nowUnixMs()),
                unsigned(rng() & 0xFFFFFFFFu));
  return buf;
}

// Keep file names portable and free of path separators.
std::string safeFilePart(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
        c == '>' || c == '|')
      out += '_';
    else
      out += char(c);
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  return out;
}

std::string expandPattern(const std::string& pattern, const Kv& snap, uint32_t seq) {
  const std::pair<const char*, std::string> tokens[] = {
      {"{patient-id}", snap.str("patient.id")},
      {"{family}", snap.str("patient.family")},
      {"{given}", snap.str("patient.given")},
      {"{date}", snap.str("content.date")},
      {"{time}", snap.str("content.time")},
      {"{seq}", [&] {
         char b[16];
         std::snprintf(b, sizeof b, "%04u", seq);
         return std::string(b);
       }()},
      {"{slot}", snap.str("slot.label")},
      {"{session}", snap.str("session.id")},
      {"{uid}", snap.str("sop.uid")},
  };
  std::string out = pattern;
  for (const auto& [tok, value] : tokens) {
    for (size_t at = out.find(tok); at != std::string::npos; at = out.find(tok, at + value.size()))
      out.replace(at, std::char_traits<char>::length(tok), value);
  }
  out = safeFilePart(out);
  return out.empty() ? snap.str("sop.uid") : out;
}

}  // namespace

const char* formatName(ExportTarget::Format f) {
  switch (f) {
    case ExportTarget::Format::DicomProcessing: return "dicom-processing";
    case ExportTarget::Format::Tiff16: return "tiff16";
    case ExportTarget::Format::Tiff8: return "tiff8";
    default: return "dicom";
  }
}

std::string ExportTarget::extension() const {
  return (format == Format::Tiff16 || format == Format::Tiff8) ? ".tif" : ".dcm";
}

Kv ExportTarget::toKv() const {
  Kv kv;
  kv.set("id", id);
  kv.set("name", name);
  kv.setBool("enabled", enabled);
  kv.set("format", formatName(format));
  kv.set("folder", fs::toUtf8(folder));
  kv.set("file-name", fileNamePattern);
  kv.setBool("apply-orientation", applyOrientation);
  kv.setBool("scale-to-full-range", scaleToFullRange);
  kv.setBool("window-8bit", windowFor8Bit);
  kv.set("dicom.uid-root", uidRoot);
  kv.set("dicom.institution", institution);
  kv.set("dicom.manufacturer", manufacturer);
  kv.set("dicom.model", modelName);
  kv.set("dicom.intensity-relationship", intensityRelationship);
  kv.setInt("dicom.intensity-sign", intensitySign);
  kv.setBool("dicom.window", writeWindow);
  return kv;
}

ExportTarget ExportTarget::fromKv(const Kv& kv) {
  ExportTarget t;
  t.id = kv.str("id");
  t.name = kv.str("name", t.id);
  t.enabled = kv.boolean("enabled", true);
  t.format = parseFormat(kv.str("format", "dicom"));
  t.folder = fs::fromUtf8(kv.str("folder"));
  t.fileNamePattern = kv.str("file-name", t.fileNamePattern);
  t.applyOrientation = kv.boolean("apply-orientation", true);
  t.scaleToFullRange = kv.boolean("scale-to-full-range", false);
  t.windowFor8Bit = kv.boolean("window-8bit", false);
  t.uidRoot = kv.str("dicom.uid-root");
  t.institution = kv.str("dicom.institution");
  t.manufacturer = kv.str("dicom.manufacturer");
  t.modelName = kv.str("dicom.model");
  t.intensityRelationship = kv.str("dicom.intensity-relationship", "LIN");
  t.intensitySign = kv.integer("dicom.intensity-sign", 1) < 0 ? -1 : 1;
  t.writeWindow = kv.boolean("dicom.window", true);
  return t;
}

Kv ExportJob::toKv() const {
  Kv kv;
  kv.set("id", id);
  kv.set("state", stateName(state));
  kv.set("target", targetId);
  kv.set("session.dir", fs::toUtf8(sessionDir));
  kv.setInt("seq", seq);
  kv.set("out", outName);
  kv.setInt("attempts", attempts);
  kv.setInt("next-try", nextTryUnixMs);
  kv.set("error", lastError);
  for (const auto& [k, v] : snapshot.items()) kv.set("snap." + k, v);
  return kv;
}

ExportJob ExportJob::fromKv(const Kv& kv) {
  ExportJob j;
  j.id = kv.str("id");
  const std::string st = kv.str("state");
  j.state = st == "done" ? State::Done : st == "failed" ? State::Failed : State::Pending;
  j.targetId = kv.str("target");
  j.sessionDir = fs::fromUtf8(kv.str("session.dir"));
  j.seq = uint32_t(kv.integer("seq"));
  j.outName = kv.str("out");
  j.attempts = int(kv.integer("attempts"));
  j.nextTryUnixMs = kv.integer("next-try");
  j.lastError = kv.str("error");
  for (const auto& [k, v] : kv.items())
    if (k.rfind("snap.", 0) == 0) j.snapshot.set(k.substr(5), v);
  return j;
}

ExportQueue::ExportQueue(fs::Path root, TargetLookup targets)
    : root_(std::move(root)), targets_(std::move(targets)) {
  std::error_code ec;
  std::filesystem::create_directories(root_ / "queue" / "done", ec);
  loadAll();
}

ExportQueue::~ExportQueue() { stop(); }

void ExportQueue::loadAll() {
  std::error_code ec;
  // Only unfinished jobs; finished ones stay on disk in queue/done.
  for (const auto& e : std::filesystem::directory_iterator(root_ / "queue", ec)) {
    if (e.path().extension() != ".job") continue;
    Kv kv;
    if (!Kv::load(e.path(), kv)) continue;
    ExportJob j = ExportJob::fromKv(kv);
    if (!j.id.empty() && j.state != ExportJob::State::Done) jobs_[j.id] = j;
  }
}

bool ExportQueue::persist(const ExportJob& job, std::string* err) {
  const fs::Path pending = root_ / "queue" / fs::fromUtf8(job.id + ".job");
  const fs::Path done = root_ / "queue" / "done" / fs::fromUtf8(job.id + ".job");
  if (job.state == ExportJob::State::Done) {
    if (!job.toKv().save(done, err)) return false;
    std::error_code ec;
    std::filesystem::remove(pending, ec);
    return true;
  }
  return job.toKv().save(pending, err);
}

int ExportQueue::enqueueSession(const fs::Path& sessionDir, const SessionState& session, const Layout& layout,
                                const std::vector<ExportTarget>& targets, std::string* err) {
  std::vector<const StagedImage*> keep;
  for (const auto& img : session.images)
    if (!img.discarded) keep.push_back(&img);
  // Placed images in slot order, then unplaced ones in the order they were taken.
  std::stable_sort(keep.begin(), keep.end(), [](const StagedImage* a, const StagedImage* b) {
    const int sa = a->slot < 0 ? 1 << 20 : a->slot, sb = b->slot < 0 ? 1 << 20 : b->slot;
    return sa != sb ? sa < sb : a->seq < b->seq;
  });
  if (keep.empty()) {
    fail(err, "there are no images to export");
    return 0;
  }

  const std::string studyUid = makeUid(targets.empty() ? std::string() : targets.front().uidRoot);
  const int64_t created = fs::nowUnixMs();
  std::vector<ExportJob> made;
  int series = 0;
  for (const auto& t : targets) {
    if (!t.enabled) continue;
    ++series;
    const std::string seriesUid = makeUid(t.uidRoot);
    int instance = 0;
    for (const StagedImage* img : keep) {
      ExportJob j;
      j.id = newJobId();
      j.targetId = t.id;
      j.sessionDir = sessionDir;
      j.seq = img->seq;
      Kv& s = j.snapshot;
      s.set("session.id", session.id);
      s.set("patient.id", session.patient.id);
      s.set("patient.family", session.patient.familyName);
      s.set("patient.given", session.patient.givenName);
      s.set("patient.birth", session.patient.birthDate);
      s.set("patient.sex", session.patient.sex);
      s.set("patient.accession", session.patient.accession);
      s.set("study.uid", studyUid);
      s.set("study.date", dicomDate(session.startedUnixMs));
      s.set("study.time", dicomTime(session.startedUnixMs));
      s.set("series.uid", seriesUid);
      s.setInt("series.number", series);
      s.set("sop.uid", makeUid(t.uidRoot));
      s.setInt("instance", ++instance);
      s.setInt("created", created);
      const int64_t at = img->acquiredUnixMs ? img->acquiredUnixMs : created;
      s.set("content.date", dicomDate(at));
      s.set("content.time", dicomTime(at));
      s.setInt("orient.q", img->orientation.quarterTurns);
      s.setBool("orient.flip", img->orientation.flipH);
      if (img->slot >= 0 && img->slot < int(layout.slotList.size())) {
        const LayoutSlot& slot = layout.slotList[size_t(img->slot)];
        s.set("slot.label", slot.label);
        s.set("slot.region", formatCodes(slot.region));
        s.set("slot.structures", formatCodes(slot.structures));
      }
      j.outName = expandPattern(t.fileNamePattern, s, img->seq) + t.extension();
      made.push_back(std::move(j));
    }
  }
  if (made.empty()) {
    fail(err, "no export destination is enabled");
    return 0;
  }
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& j : made) {
    if (!persist(j, err)) return 0;
    jobs_[j.id] = j;
  }
  cv_.notify_all();
  return int(made.size());
}

bool ExportQueue::perform(ExportJob& job, std::string* err) {
  ExportTarget target;
  if (!targets_ || !targets_(job.targetId, target))
    return fail(err, "export destination '" + job.targetId + "' no longer exists");

  SessionState session;
  if (!StagingSession::readState(job.sessionDir, session, err)) return false;
  const StagedImage* img = session.image(job.seq);
  if (!img) return fail(err, "image is missing from its session");
  Frame frame;
  if (!StagingSession::readImage(job.sessionDir, *img, frame, err)) return false;

  const Kv& s = job.snapshot;
  if (target.applyOrientation) {
    Orientation o;
    o.quarterTurns = int(s.integer("orient.q"));
    o.flipH = s.boolean("orient.flip");
    frame = applyOrientation(frame, o);
  }

  std::vector<uint8_t> bytes;
  switch (target.format) {
    case ExportTarget::Format::Tiff8:
      // Either the full stored range scaled linearly, or the display window:
      // a 12- or 14-bit image usually fills only part of its range.
      bytes = encodeTiff(target.windowFor8Bit ? windowTo8Bit(frame, autoWindow(frame)) : frame, 8,
                         {"twain-acquire", "", s.str("slot.label")});
      break;
    case ExportTarget::Format::Tiff16:
      bytes = encodeTiff(target.scaleToFullRange ? toBitDepth(frame, 16) : frame, 16,
                         {"twain-acquire", "", s.str("slot.label")});
      break;
    default: {
      PatientInfo p;
      p.id = s.str("patient.id");
      p.familyName = s.str("patient.family");
      p.givenName = s.str("patient.given");
      p.birthDate = s.str("patient.birth");
      p.sex = s.str("patient.sex");
      p.accession = s.str("patient.accession");
      DicomStudy st;
      st.studyUid = s.str("study.uid");
      st.seriesUid = s.str("series.uid");
      st.studyDate = s.str("study.date");
      st.studyTime = s.str("study.time");
      st.seriesNumber = int(s.integer("series.number", 1));
      DicomImage di;
      di.sopUid = s.str("sop.uid");
      di.instanceNumber = int(s.integer("instance", 1));
      di.creationUnixMs = s.integer("created");
      di.contentDate = s.str("content.date");
      di.contentTime = s.str("content.time");
      di.forProcessing = target.format == ExportTarget::Format::DicomProcessing;
      di.imageComments = s.str("slot.label");
      di.anatomicRegion = parseCodes(s.str("slot.region"));
      di.primaryStructures = parseCodes(s.str("slot.structures"));
      di.manufacturer = target.manufacturer;
      di.modelName = target.modelName;
      di.institution = target.institution;
      di.softwareVersion = "twain-acquire";
      di.pixelIntensityRelationship = target.intensityRelationship;
      di.pixelIntensitySign = target.intensitySign;
      if (target.writeWindow) {
        const WindowLevel wl = autoWindow(frame);
        di.windowCenter = wl.center;
        di.windowWidth = wl.width;
      }
      bytes = encodeDicom(frame, p, st, di);
    }
  }

  std::error_code ec;
  std::filesystem::create_directories(target.folder, ec);
  const fs::Path dest = target.folder / fs::fromUtf8(job.outName);
  if (std::filesystem::exists(dest, ec)) {
    // A retry after a crash finds its own earlier output; anything else is
    // someone else's file and is never overwritten.
    std::vector<uint8_t> existing;
    if (fs::readAll(dest, existing) && existing == bytes) return true;
    return fail(err, "a different file named " + job.outName + " already exists in the destination");
  }
  return fs::atomicWrite(dest, bytes.data(), bytes.size(), err);
}

void ExportQueue::start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (worker_.joinable()) return;
  stopping_ = false;
  worker_ = std::thread([this] { run(); });
}

void ExportQueue::stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void ExportQueue::retryFailed() {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& [id, j] : jobs_) {
    if (j.state != ExportJob::State::Failed) continue;
    j.state = ExportJob::State::Pending;
    j.attempts = 0;
    j.nextTryUnixMs = 0;
    persist(j, nullptr);
  }
  cv_.notify_all();
}

std::vector<ExportJob> ExportQueue::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<ExportJob> out;
  out.reserve(jobs_.size());
  for (const auto& [id, j] : jobs_) out.push_back(j);
  return out;
}

ExportQueue::Counts ExportQueue::counts() const {
  std::lock_guard<std::mutex> lock(mu_);
  Counts c;
  c.doneSinceStart = doneSinceStart_;
  for (const auto& [id, j] : jobs_) {
    if (j.state == ExportJob::State::Pending) ++c.pending;
    if (j.state == ExportJob::State::Failed) {
      ++c.failed;
      c.lastError = j.lastError;
    }
  }
  return c;
}

void ExportQueue::setRetentionDays(int days) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    retentionDays_ = std::max(0, days);
    lastPurgeMs_ = 0;  // apply a new setting at the worker's next pass
  }
  cv_.notify_all();
}

int ExportQueue::purgeNow() {
  int days;
  std::vector<std::string> needed;
  {
    std::lock_guard<std::mutex> lock(mu_);
    days = retentionDays_;
    for (const auto& [id, j] : jobs_) needed.push_back(fs::toUtf8(j.sessionDir.lexically_normal()));
  }
  if (days <= 0) return 0;
  using Clock = std::filesystem::file_time_type::clock;
  const auto limit = std::chrono::hours(24) * days;
  const auto now = Clock::now();
  std::error_code ec;
  int removed = 0;

  for (const auto& e : std::filesystem::directory_iterator(root_ / "sessions", ec)) {
    if (!e.is_directory(ec)) continue;
    const fs::Path journal = e.path() / "session.journal";
    const auto written = std::filesystem::last_write_time(journal, ec);
    if (ec || now - written < limit) {
      ec.clear();
      continue;
    }
    SessionState st;
    if (!StagingSession::readState(e.path(), st) || !st.closed) continue;  // open: the operator decides
    const std::string dir = fs::toUtf8(e.path().lexically_normal());
    if (std::find(needed.begin(), needed.end(), dir) != needed.end()) continue;
    if (std::filesystem::remove_all(e.path(), ec) > 0 && !ec) ++removed;
    ec.clear();
  }
  for (const auto& e : std::filesystem::directory_iterator(root_ / "queue" / "done", ec)) {
    if (e.path().extension() != ".job") continue;
    const auto written = std::filesystem::last_write_time(e.path(), ec);
    if (!ec && now - written >= limit && std::filesystem::remove(e.path(), ec)) ++removed;
    ec.clear();
  }
  return removed;
}

void ExportQueue::run() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!stopping_) {
    const int64_t now = fs::nowUnixMs();
    if (retentionDays_ > 0 && now - lastPurgeMs_ >= 3600 * 1000) {
      lastPurgeMs_ = now;
      lock.unlock();
      purgeNow();
      lock.lock();
      continue;
    }
    ExportJob* due = nullptr;
    int64_t wake = now + 60000;
    for (auto& [id, j] : jobs_) {
      if (j.state != ExportJob::State::Pending) continue;
      if (j.nextTryUnixMs <= now) {
        due = &j;
        break;
      }
      wake = std::min(wake, j.nextTryUnixMs);
    }
    if (!due) {
      cv_.wait_for(lock, std::chrono::milliseconds(std::max<int64_t>(50, wake - now)));
      continue;
    }

    ExportJob job = *due;
    lock.unlock();
    std::string err;
    const bool ok = perform(job, &err);
    lock.lock();

    ExportJob& j = jobs_[job.id];
    if (ok) {
      j.state = ExportJob::State::Done;
      j.lastError.clear();
      persist(j, nullptr);
      jobs_.erase(job.id);
      ++doneSinceStart_;
      auto notify = onChanged;
      lock.unlock();
      if (notify) notify();
      lock.lock();
      continue;
    } else {
      ++j.attempts;
      j.lastError = err;
      if (j.attempts >= maxAttempts) {
        j.state = ExportJob::State::Failed;
      } else {
        const int64_t backoff = std::min<int64_t>(300000, 2000LL << std::min(j.attempts, 8));
        j.nextTryUnixMs = fs::nowUnixMs() + backoff;
      }
    }
    persist(j, nullptr);
    auto notify = onChanged;
    lock.unlock();
    if (notify) notify();
    lock.lock();
  }
}

}  // namespace ta
