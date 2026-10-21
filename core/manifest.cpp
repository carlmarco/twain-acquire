/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "manifest.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

#include "dicom.hpp"
#include "json.hpp"
#include "tiff.hpp"

namespace ta {

namespace {

constexpr const char* kFormat = "twain-acquire-mount";
constexpr uintmax_t kMaxManifestBytes = 4u << 20;
constexpr uintmax_t kMaxImageBytes = 512u << 20;

bool fail(std::string* err, const std::string& why) {
  if (err) *err = why;
  return false;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

// A plain file name: no folder part, so a mount file can only point beside itself.
bool plainName(const std::string& name) {
  return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string::npos;
}

}  // namespace

std::string localIsoTime(int64_t unixMs) {
  const std::string d = dicomDate(unixMs), t = dicomTime(unixMs);
  if (d.size() != 8 || t.size() < 6) return {};
  return d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2) + "T" + t.substr(0, 2) + ":" +
         t.substr(2, 2) + ":" + t.substr(4, 2);
}

std::string encodeManifest(const MountManifest& m) {
  JsonWriter w;
  w.beginObject();
  w.str("format", kFormat);
  w.integer("version", MountManifest::kVersion);
  w.str("software", m.software);
  w.str("session", m.session);
  w.str("source", m.source);
  w.str("exported", m.exported);

  w.beginObject("patient");
  w.str("id", m.patient.id);
  w.str("family", m.patient.familyName);
  w.str("given", m.patient.givenName);
  w.str("birth", m.patient.birthDate);
  w.str("sex", m.patient.sex);
  w.str("accession", m.patient.accession);
  w.endObject();

  w.beginObject("layout");
  w.str("id", m.layout.id);
  w.str("name", m.layout.name);
  w.integer("columns", m.layout.columns);
  w.integer("rows", m.layout.rows);
  w.flag("mirrored", m.layout.mirrored);
  w.beginArray("slots");
  for (size_t i = 0; i < m.layout.slotList.size(); ++i) {
    const LayoutSlot& s = m.layout.slotList[i];
    w.beginObject();
    w.integer("slot", int64_t(i) + 1);
    w.str("key", m.layout.slotKey(int(i)));
    w.str("label", s.label);
    w.integer("column", s.column);
    w.integer("row", s.row);
    w.endObject();
  }
  w.endArray();
  w.endObject();

  w.beginArray("images");
  for (const ManifestImage& img : m.images) {
    w.beginObject();
    w.str("file", img.file);
    w.integer("seq", img.seq);
    if (img.slot >= 0) {
      w.integer("slot", int64_t(img.slot) + 1);
      w.str("key", m.layout.slotKey(img.slot));
      w.str("label", img.slot < int(m.layout.slotList.size()) ? m.layout.slotList[size_t(img.slot)].label : "");
    } else {
      w.null("slot");
    }
    w.integer("quarter-turns", img.orientation.quarterTurns);
    w.flag("mirrored", img.orientation.flipH);
    w.flag("orientation-applied", img.orientationApplied);
    w.str("acquired", img.acquired);
    w.endObject();
  }
  w.endArray();
  w.endObject();
  return w.text();
}

bool decodeManifest(const std::string& text, MountManifest& out, std::string* err) {
  Json j;
  std::string why;
  if (!Json::parse(text, j, &why)) return fail(err, "not a readable mount file: " + why);
  if (j.type != Json::Type::Object || j.str("format") != kFormat) return fail(err, "not a twain-acquire mount file");
  out = MountManifest{};
  out.version = int(j.integer("version", 0));
  if (out.version < 1 || out.version > MountManifest::kVersion)
    return fail(err, "mount file version " + std::to_string(out.version) + " is newer than this program reads");
  out.software = j.str("software");
  out.session = j.str("session");
  out.source = j.str("source");
  out.exported = j.str("exported");
  if (const Json* p = j.find("patient")) {
    out.patient.id = p->str("id");
    out.patient.familyName = p->str("family");
    out.patient.givenName = p->str("given");
    out.patient.birthDate = p->str("birth");
    out.patient.sex = p->str("sex");
    out.patient.accession = p->str("accession");
  }

  const Json* l = j.find("layout");
  if (!l || l->type != Json::Type::Object) return fail(err, "the mount file has no layout");
  out.layout.id = l->str("id", "exported");
  out.layout.name = l->str("name", out.layout.id);
  out.layout.columns = int(std::clamp<int64_t>(l->integer("columns", 1), 1, 32));
  out.layout.rows = int(std::clamp<int64_t>(l->integer("rows", 1), 1, 32));
  out.layout.mirrored = l->flag("mirrored");
  if (const Json* slots = l->find("slots"); slots && slots->type == Json::Type::Array) {
    for (const Json& s : slots->array) {
      if (out.layout.slotList.size() >= 256) return fail(err, "the mount file has too many slots");
      LayoutSlot slot;
      slot.label = s.str("label");
      slot.key = s.str("key");
      slot.column = int(std::clamp<int64_t>(s.integer("column"), 0, out.layout.columns - 1));
      slot.row = int(std::clamp<int64_t>(s.integer("row"), 0, out.layout.rows - 1));
      out.layout.slotList.push_back(std::move(slot));
    }
  }
  if (out.layout.slotList.empty()) return fail(err, "the mount file's layout has no slots");

  const Json* images = j.find("images");
  if (!images || images->type != Json::Type::Array) return fail(err, "the mount file lists no images");
  std::vector<bool> taken(out.layout.slotList.size(), false);
  for (const Json& i : images->array) {
    if (out.images.size() >= 1024) return fail(err, "the mount file lists too many images");
    ManifestImage img;
    img.file = i.str("file");
    if (!plainName(img.file)) return fail(err, "the mount file names an image outside its folder: " + img.file);
    img.seq = uint32_t(std::clamp<int64_t>(i.integer("seq", int64_t(out.images.size()) + 1), 0, 1 << 30));
    const int64_t slot = i.integer("slot", 0);  // 1-based; null or absent = not placed
    if (slot >= 1 && slot <= int64_t(out.layout.slotList.size()) && !taken[size_t(slot - 1)]) {
      img.slot = int(slot - 1);
      taken[size_t(img.slot)] = true;
    }
    img.orientation.quarterTurns = int(((i.integer("quarter-turns") % 4) + 4) % 4);
    img.orientation.flipH = i.flag("mirrored");
    img.orientationApplied = i.flag("orientation-applied");
    img.acquired = i.str("acquired");
    out.images.push_back(std::move(img));
  }
  return true;
}

bool readManifest(const fs::Path& file, MountManifest& out, std::string* err) {
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(file, ec);
  if (ec) return fail(err, "cannot read " + fs::toUtf8(file.filename()));
  if (size > kMaxManifestBytes) return fail(err, fs::toUtf8(file.filename()) + " is too large to be a mount file");
  std::vector<uint8_t> bytes;
  if (!fs::readAll(file, bytes, err)) return false;
  return decodeManifest(std::string(bytes.begin(), bytes.end()), out, err);
}

std::vector<fs::Path> listManifests(const fs::Path& folder) {
  std::vector<std::pair<std::filesystem::file_time_type, fs::Path>> found;
  std::error_code ec;
  const std::string suffix = kManifestSuffix;
  for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
    const std::string name = lower(fs::toUtf8(e.path().filename()));
    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
        e.is_regular_file(ec))
      found.emplace_back(e.last_write_time(ec), e.path());
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<fs::Path> out;
  for (auto& f : found) out.push_back(std::move(f.second));
  return out;
}

std::vector<fs::Path> listImageFiles(const fs::Path& folder) {
  std::vector<fs::Path> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
    const std::string ext = lower(fs::toUtf8(e.path().extension()));
    if ((ext == ".dcm" || ext == ".tif" || ext == ".tiff") && e.is_regular_file(ec)) out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool readImageFile(const fs::Path& file, Frame& out, std::string* err) {
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(file, ec);
  if (ec) return fail(err, "cannot read " + fs::toUtf8(file.filename()));
  if (size > kMaxImageBytes) return fail(err, fs::toUtf8(file.filename()) + " is too large");
  std::vector<uint8_t> bytes;
  if (!fs::readAll(file, bytes, err)) return false;
  std::string why;
  const std::string ext = lower(fs::toUtf8(file.extension()));
  TiffInfo info;
  const bool ok = ext == ".dcm" ? decodeDicom(bytes.data(), bytes.size(), out, &why)
                                : decodeTiff(bytes.data(), bytes.size(), out, &info, &why);
  if (!ok) return fail(err, fs::toUtf8(file.filename()) + ": " + why);
  if (info.minIsWhite)
    for (auto& p : out.pixels) p = uint16_t(out.maxValue() - p);
  return true;
}

}  // namespace ta
