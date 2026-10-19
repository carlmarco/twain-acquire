#include "staging.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <random>
#include <system_error>

#include "crc32.hpp"
#include "kv.hpp"
#include "tiff.hpp"

namespace ta {

namespace {

constexpr const char* kJournal = "session.journal";

using Fields = std::vector<std::pair<std::string, std::string>>;

bool fail(std::string* err, const std::string& what) {
  if (err) *err = what;
  return false;
}

std::string field(const Fields& f, const char* key) {
  for (const auto& kv : f)
    if (kv.first == key) return kv.second;
  return {};
}

int64_t num(const Fields& f, const char* key, int64_t def = 0) {
  const std::string v = field(f, key);
  if (v.empty()) return def;
  char* end = nullptr;
  long long n = std::strtoll(v.c_str(), &end, 10);
  return (end && *end == '\0') ? n : def;
}

double real(const Fields& f, const char* key) {
  const std::string v = field(f, key);
  return v.empty() ? 0.0 : std::strtod(v.c_str(), nullptr);
}

std::string hex8(uint32_t v) {
  char buf[9];
  std::snprintf(buf, sizeof buf, "%08x", v);
  return buf;
}

std::string realStr(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.6f", v);
  return buf;
}

// One journal line: op TAB key=value ... TAB #crc. A line whose CRC does not
// match was torn by a crash mid-write and is ignored, along with anything after it.
bool parseLine(const std::string& line, std::string& op, Fields& fields) {
  const size_t hash = line.rfind("\t#");
  if (hash == std::string::npos) return false;
  const std::string body = line.substr(0, hash);
  if (hex8(crc32(body.data(), body.size())) != line.substr(hash + 2)) return false;
  fields.clear();
  size_t start = 0;
  bool first = true;
  while (start <= body.size()) {
    size_t tab = body.find('\t', start);
    if (tab == std::string::npos) tab = body.size();
    const std::string part = body.substr(start, tab - start);
    if (first) {
      op = part;
      first = false;
    } else {
      const size_t eq = part.find('=');
      if (eq == std::string::npos) return false;
      auto v = kvUnescape(std::string_view(part).substr(eq + 1));
      if (!v) return false;
      fields.emplace_back(part.substr(0, eq), *v);
    }
    start = tab + 1;
  }
  return !op.empty();
}

StagedImage* findImage(SessionState& s, uint32_t seq) {
  for (auto& img : s.images)
    if (img.seq == seq) return &img;
  return nullptr;
}

void apply(SessionState& s, const std::string& op, const Fields& f) {
  if (op == "session") {
    s.id = field(f, "id");
    s.layout = field(f, "layout");
    s.source = field(f, "source");
    s.startedUnixMs = num(f, "started");
  } else if (op == "patient") {
    s.patient.id = field(f, "id");
    s.patient.familyName = field(f, "family");
    s.patient.givenName = field(f, "given");
    s.patient.birthDate = field(f, "birth");
    s.patient.sex = field(f, "sex");
    s.patient.accession = field(f, "accession");
  } else if (op == "image") {
    StagedImage img;
    img.seq = uint32_t(num(f, "seq"));
    img.file = field(f, "file");
    img.slot = int(num(f, "slot", -1));
    img.crc = uint32_t(std::strtoul(field(f, "crc").c_str(), nullptr, 16));
    img.width = uint32_t(num(f, "w"));
    img.height = uint32_t(num(f, "h"));
    img.bitsStored = int(num(f, "bits", 16));
    img.dpiX = real(f, "dpix");
    img.dpiY = real(f, "dpiy");
    img.acquiredUnixMs = num(f, "at");
    img.orientation.quarterTurns = int(num(f, "q"));
    img.orientation.flipH = num(f, "flip") != 0;
    if (!findImage(s, img.seq)) s.images.push_back(img);
  } else if (StagedImage* img = findImage(s, uint32_t(num(f, "seq", -1)))) {
    if (op == "place") {
      img->slot = int(num(f, "slot", -1));
    } else if (op == "orient") {
      img->orientation.quarterTurns = int(num(f, "q"));
      img->orientation.flipH = num(f, "flip") != 0;
    } else if (op == "discard") {
      img->discarded = true;
    }
  }
  if (op == "close") {
    s.closed = true;
    s.closeReason = field(f, "reason");
  }
}

// `goodBytes` (optional) receives the length of the journal up to the last
// intact record.
bool replay(const fs::Path& dir, SessionState& s, std::string* err, size_t* goodBytes = nullptr) {
  std::string text;
  if (!fs::readText(dir / kJournal, text, err)) return false;
  s = SessionState{};
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) break;  // an unterminated last line was never completed
    std::string op;
    Fields f;
    if (!parseLine(text.substr(pos, nl - pos), op, f)) break;
    apply(s, op, f);
    pos = nl + 1;
  }
  if (goodBytes) *goodBytes = pos;
  if (s.id.empty()) return fail(err, "journal has no session record");
  return true;
}

std::string newSessionId() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
  std::mt19937 rng{std::random_device{}()};
  char tail[8];
  std::snprintf(tail, sizeof tail, "-%04x", unsigned(rng() & 0xFFFF));
  return std::string(buf) + tail;
}

Fields patientFields(const PatientInfo& p) {
  return {{"id", p.id},     {"family", p.familyName}, {"given", p.givenName},
          {"birth", p.birthDate}, {"sex", p.sex},   {"accession", p.accession}};
}

}  // namespace

std::string PatientInfo::displayName() const {
  if (familyName.empty()) return givenName;
  if (givenName.empty()) return familyName;
  return familyName + ", " + givenName;
}

const StagedImage* SessionState::image(uint32_t seq) const {
  for (const auto& img : images)
    if (img.seq == seq) return &img;
  return nullptr;
}

bool StagingSession::record(const std::string& op, const Fields& fields, std::string* err) {
  std::string body = op;
  for (const auto& [k, v] : fields) body += "\t" + k + "=" + kvEscape(v);
  const std::string line = body + "\t#" + hex8(crc32(body.data(), body.size())) + "\n";
  if (!fs::appendDurable(dir_ / kJournal, line, err)) return false;
  apply(state_, op, fields);
  return true;
}

std::unique_ptr<StagingSession> StagingSession::create(const fs::Path& root, const std::string& layout,
                                                       const std::string& source,
                                                       const PatientInfo& patient, std::string* err) {
  std::unique_ptr<StagingSession> s(new StagingSession);
  const std::string id = newSessionId();
  s->dir_ = root / "sessions" / id;
  std::error_code ec;
  std::filesystem::create_directories(s->dir_, ec);
  if (ec) {
    fail(err, "cannot create " + fs::toUtf8(s->dir_) + ": " + ec.message());
    return nullptr;
  }
  if (!s->record("session", {{"id", id}, {"layout", layout}, {"source", source},
                             {"started", std::to_string(fs::nowUnixMs())}}, err))
    return nullptr;
  if (!s->record("patient", patientFields(patient), err)) return nullptr;
  return s;
}

std::unique_ptr<StagingSession> StagingSession::resume(const fs::Path& dir, std::string* err) {
  std::unique_ptr<StagingSession> s(new StagingSession);
  s->dir_ = dir;
  size_t good = 0;
  if (!replay(dir, s->state_, err, &good)) return nullptr;
  // A crash can leave a torn last record. Cut it off before appending, or the
  // next record would be joined to it and lost too. The torn bytes are kept
  // beside the journal.
  std::string text;
  if (!fs::readText(dir / kJournal, text, err)) return nullptr;
  if (good < text.size()) {
    const std::string tail = text.substr(good);
    const fs::Path keep = dir / fs::fromUtf8(std::string(kJournal) + ".torn-" + std::to_string(fs::nowUnixMs()));
    if (!fs::atomicWrite(keep, tail.data(), tail.size(), err)) return nullptr;
    if (!fs::atomicWrite(dir / kJournal, text.data(), good, err)) return nullptr;
  }
  return s;
}

std::vector<fs::Path> StagingSession::listOpen(const fs::Path& root) {
  std::vector<fs::Path> out;
  std::error_code ec;
  const fs::Path sessions = root / "sessions";
  if (!std::filesystem::is_directory(sessions, ec)) return out;
  for (const auto& entry : std::filesystem::directory_iterator(sessions, ec)) {
    SessionState s;
    if (entry.is_directory(ec) && replay(entry.path(), s, nullptr) && !s.closed) out.push_back(entry.path());
  }
  std::sort(out.begin(), out.end(), [](const fs::Path& a, const fs::Path& b) { return a.filename() > b.filename(); });
  return out;
}

bool StagingSession::readState(const fs::Path& dir, SessionState& out, std::string* err) {
  return replay(dir, out, err);
}

bool StagingSession::readImage(const fs::Path& dir, const StagedImage& img, Frame& out, std::string* err) {
  std::vector<uint8_t> bytes;
  if (!fs::readAll(dir / fs::fromUtf8(img.file), bytes, err)) return false;
  if (crc32(bytes.data(), bytes.size()) != img.crc)
    return fail(err, "staged image " + img.file + " is damaged (checksum mismatch)");
  if (!decodeTiff(bytes.data(), bytes.size(), out, nullptr, err)) return false;
  out.bitsStored = uint8_t(img.bitsStored);
  out.dpiX = img.dpiX;
  out.dpiY = img.dpiY;
  out.acquiredUnixMs = img.acquiredUnixMs;
  return true;
}

bool StagingSession::addImage(const Frame& f, int slot, uint32_t* seqOut, std::string* err) {
  if (state_.closed) return fail(err, "session is closed");
  if (!f.valid()) return fail(err, "image is empty");
  uint32_t seq = 1;
  for (const auto& img : state_.images) seq = std::max(seq, img.seq + 1);
  char name[32];
  std::snprintf(name, sizeof name, "%04u.tif", seq);
  const std::vector<uint8_t> bytes = encodeTiff(f, 16);
  if (bytes.empty()) return fail(err, "image is too large to stage (over the 4 GB TIFF limit)");
  if (!fs::atomicWrite(dir_ / name, bytes.data(), bytes.size(), err)) return false;
  const uint32_t crc = crc32(bytes.data(), bytes.size());
  if (!record("image", {{"seq", std::to_string(seq)},
                        {"file", name},
                        {"slot", std::to_string(slot)},
                        {"crc", hex8(crc)},
                        {"w", std::to_string(f.width)},
                        {"h", std::to_string(f.height)},
                        {"bits", std::to_string(f.bitsStored)},
                        {"dpix", realStr(f.dpiX)},
                        {"dpiy", realStr(f.dpiY)},
                        {"at", std::to_string(f.acquiredUnixMs)}},
              err))
    return false;
  if (seqOut) *seqOut = seq;
  return true;
}

bool StagingSession::place(uint32_t seq, int slot, std::string* err) {
  if (!state_.image(seq)) return fail(err, "no such image");
  return record("place", {{"seq", std::to_string(seq)}, {"slot", std::to_string(slot)}}, err);
}

bool StagingSession::setOrientation(uint32_t seq, const Orientation& o, std::string* err) {
  if (!state_.image(seq)) return fail(err, "no such image");
  return record("orient", {{"seq", std::to_string(seq)},
                           {"q", std::to_string(((o.quarterTurns % 4) + 4) % 4)},
                           {"flip", o.flipH ? "1" : "0"}},
                err);
}

bool StagingSession::discard(uint32_t seq, std::string* err) {
  if (!state_.image(seq)) return fail(err, "no such image");
  return record("discard", {{"seq", std::to_string(seq)}}, err);
}

bool StagingSession::setPatient(const PatientInfo& p, std::string* err) {
  return record("patient", patientFields(p), err);
}

bool StagingSession::close(const std::string& reason, std::string* err) {
  if (state_.closed) return true;
  return record("close", {{"reason", reason}}, err);
}

bool StagingSession::loadImage(uint32_t seq, Frame& out, std::string* err) const {
  const StagedImage* img = state_.image(seq);
  if (!img) return fail(err, "no such image");
  return readImage(dir_, *img, out, err);
}

}  // namespace ta
