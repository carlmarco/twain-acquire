/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dicom.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <random>

namespace ta {

namespace {

constexpr const char* kSopIoPresentation = "1.2.840.10008.5.1.4.1.1.1.3";
constexpr const char* kSopIoProcessing = "1.2.840.10008.5.1.4.1.1.1.3.1";
constexpr const char* kExplicitVrLittleEndian = "1.2.840.10008.1.2.1";
constexpr const char* kImplementationUid = "2.25.247391737924537784261374549127907463045";
constexpr const char* kImplementationName = "TWAINACQ_01";

constexpr uint32_t tag(uint16_t group, uint16_t element) { return uint32_t(group) << 16 | element; }

bool longLength(const std::string& vr) {
  return vr == "OB" || vr == "OW" || vr == "OF" || vr == "SQ" || vr == "UT" || vr == "UN" ||
         vr == "UC" || vr == "UR" || vr == "OD" || vr == "OL";
}

void put16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(uint8_t(v));
  b.push_back(uint8_t(v >> 8));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

struct Element {
  std::string vr;
  std::vector<uint8_t> value;
};

// Elements kept sorted by tag, as the standard requires.
class Dataset {
 public:
  void str(uint32_t t, const char* vr, const std::string& v) {
    Element e{vr, std::vector<uint8_t>(v.begin(), v.end())};
    if (e.value.size() % 2) e.value.push_back(e.vr == "UI" ? 0 : ' ');
    elems_[t] = std::move(e);
  }
  void us(uint32_t t, uint16_t v) {
    Element e{"US", {}};
    put16(e.value, v);
    elems_[t] = std::move(e);
  }
  void ul(uint32_t t, uint32_t v) {
    Element e{"UL", {}};
    put32(e.value, v);
    elems_[t] = std::move(e);
  }
  void raw(uint32_t t, const char* vr, std::vector<uint8_t> v) {
    if (v.size() % 2) v.push_back(0);
    elems_[t] = Element{vr, std::move(v)};
  }
  void sequence(uint32_t t, const std::vector<Dataset>& items) {
    std::vector<uint8_t> v;
    for (const Dataset& item : items) {
      const std::vector<uint8_t> body = item.encode();
      put16(v, 0xFFFE);
      put16(v, 0xE000);
      put32(v, uint32_t(body.size()));
      v.insert(v.end(), body.begin(), body.end());
    }
    elems_[t] = Element{"SQ", std::move(v)};
  }

  std::vector<uint8_t> encode() const {
    std::vector<uint8_t> out;
    for (const auto& [t, e] : elems_) {
      put16(out, uint16_t(t >> 16));
      put16(out, uint16_t(t & 0xFFFF));
      out.push_back(uint8_t(e.vr[0]));
      out.push_back(uint8_t(e.vr[1]));
      if (longLength(e.vr)) {
        put16(out, 0);
        put32(out, uint32_t(e.value.size()));
      } else {
        put16(out, uint16_t(e.value.size()));
      }
      out.insert(out.end(), e.value.begin(), e.value.end());
    }
    return out;
  }

 private:
  std::map<uint32_t, Element> elems_;
};

std::string ds(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.6g", v);
  return buf;
}

std::string personName(const PatientInfo& p) {
  if (p.givenName.empty()) return p.familyName;
  return p.familyName + "^" + p.givenName;
}

Dataset codeItem(const DicomCode& c) {
  Dataset d;
  d.str(tag(0x0008, 0x0100), "SH", c.value);
  d.str(tag(0x0008, 0x0102), "SH", c.scheme);
  d.str(tag(0x0008, 0x0104), "LO", c.meaning);
  return d;
}

std::tm localTm(int64_t unixMs) {
  std::time_t t = std::time_t(unixMs / 1000);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  return tm;
}

// Decimal digits of a 128-bit value held as four 32-bit words, most significant first.
std::string decimal128(std::array<uint32_t, 4> w) {
  std::string digits;
  auto isZero = [&] { return w[0] == 0 && w[1] == 0 && w[2] == 0 && w[3] == 0; };
  if (isZero()) return "0";
  while (!isZero()) {
    uint64_t rem = 0;
    for (auto& word : w) {
      const uint64_t cur = (rem << 32) | word;
      word = uint32_t(cur / 10);
      rem = cur % 10;
    }
    digits.insert(digits.begin(), char('0' + rem));
  }
  return digits;
}

}  // namespace

std::string dicomDate(int64_t unixMs) {
  const std::tm tm = localTm(unixMs);
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y%m%d", &tm);
  return buf;
}

std::string dicomTime(int64_t unixMs) {
  const std::tm tm = localTm(unixMs);
  char buf[16];
  std::strftime(buf, sizeof buf, "%H%M%S", &tm);
  return buf;
}

std::string makeUid(const std::string& root) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  if (root.empty()) {
    // A version-4 UUID as one integer under 2.25.
    const uint64_t hi = rng(), lo = rng();
    std::array<uint32_t, 4> w{uint32_t(hi >> 32), uint32_t(hi), uint32_t(lo >> 32), uint32_t(lo)};
    w[1] = (w[1] & 0xFFFF0FFFu) | 0x00004000u;
    w[2] = (w[2] & 0x3FFFFFFFu) | 0x80000000u;
    return "2.25." + decimal128(w);
  }
  // A root too long to leave room for a unique suffix cannot be used.
  if (root.size() > 50) return makeUid();
  const std::string rnd = std::to_string(1 + rng() % 999999999999ull);
  std::string uid = root + "." + std::to_string(uint64_t(std::time(nullptr))) + "." + rnd;
  if (uid.size() > 64) uid = root + "." + rnd;
  // Shorten the random component (never to empty, never to a leading zero).
  while (uid.size() > 64 && uid[uid.size() - 2] != '.') uid.pop_back();
  return uid;
}

std::vector<uint8_t> encodeDicom(const Frame& f, const PatientInfo& patient, const DicomStudy& study,
                                 const DicomImage& image) {
  const std::string sopClass = image.forProcessing ? kSopIoProcessing : kSopIoPresentation;
  const int64_t now = image.creationUnixMs ? image.creationUnixMs : int64_t(std::time(nullptr)) * 1000;

  Dataset d;
  d.str(tag(0x0008, 0x0005), "CS", "ISO_IR 192");
  d.str(tag(0x0008, 0x0008), "CS", "ORIGINAL\\PRIMARY");
  d.str(tag(0x0008, 0x0012), "DA", dicomDate(now));
  d.str(tag(0x0008, 0x0013), "TM", dicomTime(now));
  d.str(tag(0x0008, 0x0016), "UI", sopClass);
  d.str(tag(0x0008, 0x0018), "UI", image.sopUid);
  d.str(tag(0x0008, 0x0020), "DA", study.studyDate);
  d.str(tag(0x0008, 0x0023), "DA", image.contentDate);
  d.str(tag(0x0008, 0x0030), "TM", study.studyTime);
  d.str(tag(0x0008, 0x0033), "TM", image.contentTime);
  d.str(tag(0x0008, 0x0050), "SH", patient.accession);
  d.str(tag(0x0008, 0x0060), "CS", "IO");
  d.str(tag(0x0008, 0x0068), "CS", image.forProcessing ? "FOR PROCESSING" : "FOR PRESENTATION");
  d.str(tag(0x0008, 0x0070), "LO", image.manufacturer);
  if (!image.institution.empty()) d.str(tag(0x0008, 0x0080), "LO", image.institution);
  d.str(tag(0x0008, 0x0090), "PN", study.referringPhysician);
  if (!image.modelName.empty()) d.str(tag(0x0008, 0x1090), "LO", image.modelName);
  if (!image.anatomicRegion.empty()) {
    std::vector<Dataset> items;
    for (const auto& c : image.anatomicRegion) items.push_back(codeItem(c));
    d.sequence(tag(0x0008, 0x2218), items);
  }
  if (!image.primaryStructures.empty()) {
    std::vector<Dataset> items;
    for (const auto& c : image.primaryStructures) items.push_back(codeItem(c));
    d.sequence(tag(0x0008, 0x2228), items);
  }

  d.str(tag(0x0010, 0x0010), "PN", personName(patient));
  d.str(tag(0x0010, 0x0020), "LO", patient.id);
  d.str(tag(0x0010, 0x0030), "DA", patient.birthDate);
  d.str(tag(0x0010, 0x0040), "CS", patient.sex);

  if (!image.softwareVersion.empty()) d.str(tag(0x0018, 0x1020), "LO", image.softwareVersion);
  const bool haveSpacing = f.dpiX > 0 && f.dpiY > 0;
  // Row spacing (vertical) first, then column spacing, in millimetres.
  const std::string spacing = haveSpacing ? ds(25.4 / f.dpiY) + "\\" + ds(25.4 / f.dpiX) : "";
  if (haveSpacing) d.str(tag(0x0018, 0x1164), "DS", spacing);

  d.str(tag(0x0020, 0x000D), "UI", study.studyUid);
  d.str(tag(0x0020, 0x000E), "UI", study.seriesUid);
  d.str(tag(0x0020, 0x0010), "SH", study.studyId);
  d.str(tag(0x0020, 0x0011), "IS", std::to_string(study.seriesNumber));
  d.str(tag(0x0020, 0x0013), "IS", std::to_string(image.instanceNumber));
  d.str(tag(0x0020, 0x0020), "CS", "");
  if (!image.imageComments.empty()) d.str(tag(0x0020, 0x4000), "LT", image.imageComments);

  d.us(tag(0x0028, 0x0002), 1);
  d.str(tag(0x0028, 0x0004), "CS", "MONOCHROME2");
  d.us(tag(0x0028, 0x0010), uint16_t(f.height));
  d.us(tag(0x0028, 0x0011), uint16_t(f.width));
  if (haveSpacing) d.str(tag(0x0028, 0x0030), "DS", spacing);
  d.us(tag(0x0028, 0x0100), 16);
  d.us(tag(0x0028, 0x0101), f.bitsStored);
  d.us(tag(0x0028, 0x0102), uint16_t(f.bitsStored - 1));
  d.us(tag(0x0028, 0x0103), 0);
  d.str(tag(0x0028, 0x0301), "CS", "NO");
  d.str(tag(0x0028, 0x1040), "CS", image.pixelIntensityRelationship);
  {
    std::vector<uint8_t> v;
    put16(v, uint16_t(int16_t(image.pixelIntensitySign < 0 ? -1 : 1)));
    d.raw(tag(0x0028, 0x1041), "SS", v);
  }
  if (image.windowWidth > 0) {
    d.str(tag(0x0028, 0x1050), "DS", ds(image.windowCenter));
    d.str(tag(0x0028, 0x1051), "DS", ds(image.windowWidth));
  }
  d.str(tag(0x0028, 0x1052), "DS", "0");
  d.str(tag(0x0028, 0x1053), "DS", "1");
  d.str(tag(0x0028, 0x1054), "LO", "US");
  d.str(tag(0x0028, 0x2110), "CS", "00");
  if (!image.forProcessing) d.str(tag(0x2050, 0x0020), "CS", "IDENTITY");

  std::vector<uint8_t> pixels;
  pixels.reserve(f.pixels.size() * 2);
  for (uint16_t v : f.pixels) put16(pixels, v);
  d.raw(tag(0x7FE0, 0x0010), "OW", std::move(pixels));

  Dataset meta;
  meta.raw(tag(0x0002, 0x0001), "OB", {0x00, 0x01});
  meta.str(tag(0x0002, 0x0002), "UI", sopClass);
  meta.str(tag(0x0002, 0x0003), "UI", image.sopUid);
  meta.str(tag(0x0002, 0x0010), "UI", kExplicitVrLittleEndian);
  meta.str(tag(0x0002, 0x0012), "UI", kImplementationUid);
  meta.str(tag(0x0002, 0x0013), "SH", kImplementationName);
  const std::vector<uint8_t> metaBody = meta.encode();

  std::vector<uint8_t> out(128, 0);
  out.insert(out.end(), {'D', 'I', 'C', 'M'});
  Dataset groupLength;
  groupLength.ul(tag(0x0002, 0x0000), uint32_t(metaBody.size()));
  const std::vector<uint8_t> gl = groupLength.encode();
  out.insert(out.end(), gl.begin(), gl.end());
  out.insert(out.end(), metaBody.begin(), metaBody.end());
  const std::vector<uint8_t> body = d.encode();
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

}  // namespace ta
