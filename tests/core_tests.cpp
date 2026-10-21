/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <random>
#include <thread>

#include "check.hpp"
#include "crc32.hpp"
#include "dicom.hpp"
#include "export.hpp"
#include "frame.hpp"
#include "handoff.hpp"
#include "fs.hpp"
#include "kv.hpp"
#include "layout.hpp"
#include "normalize.hpp"
#include "profile.hpp"
#include "render.hpp"
#include "staging.hpp"
#include "ta_wire.h"
#include "tiff.hpp"

using namespace ta;

namespace {

fs::Path tempDir(const char* name) {
  static std::mt19937_64 rng{std::random_device{}()};
  fs::Path p = std::filesystem::temp_directory_path() /
               ("ta-test-" + std::string(name) + "-" + std::to_string(rng() % 1000000000));
  std::filesystem::create_directories(p);
  return p;
}

Frame pattern(uint32_t w, uint32_t h, int bits = 12) {
  Frame f;
  f.width = w;
  f.height = h;
  f.bitsStored = uint8_t(bits);
  f.dpiX = 1000;
  f.dpiY = 1000;
  f.pixels.resize(size_t(w) * h);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) f.pixels[size_t(y) * w + x] = uint16_t((y * 131 + x * 7) & f.maxValue());
  return f;
}

// Reference rotation, written the slow obvious way.
Frame naiveOrient(const Frame& f, int q, bool flip) {
  Frame cur = f;
  for (int i = 0; i < q; ++i) {
    Frame r;
    r.width = cur.height;
    r.height = cur.width;
    r.bitsStored = cur.bitsStored;
    r.pixels.resize(cur.pixels.size());
    for (uint32_t y = 0; y < cur.height; ++y)
      for (uint32_t x = 0; x < cur.width; ++x)
        r.pixels[size_t(x) * r.width + (r.width - 1 - y)] = cur.pixels[size_t(y) * cur.width + x];
    cur = r;
  }
  if (flip)
    for (uint32_t y = 0; y < cur.height; ++y)
      std::reverse(cur.pixels.begin() + long(y) * cur.width, cur.pixels.begin() + long(y + 1) * cur.width);
  return cur;
}

std::vector<uint8_t> rawPayload(const Frame& f, int bps, uint32_t flags) {
  ta_frame_info fi{};
  fi.container = TA_FRAME_RAW;
  fi.width = f.width;
  fi.height = f.height;
  fi.bits_per_sample = uint32_t(bps);
  fi.row_bytes = f.width * (bps / 8) + 3;  // padded rows, as DIBs have
  fi.flags = flags;
  fi.dpi_x_milli = 500000;
  fi.dpi_y_milli = 500000;
  std::vector<uint8_t> p(TA_FRAME_INFO_SIZE + size_t(fi.row_bytes) * f.height, 0xEE);
  ta_frame_info_put(p.data(), &fi);
  for (uint32_t y = 0; y < f.height; ++y) {
    const uint32_t srcRow = (flags & TA_FRAME_BOTTOM_UP) ? f.height - 1 - y : y;
    uint8_t* row = p.data() + TA_FRAME_INFO_SIZE + size_t(y) * fi.row_bytes;
    for (uint32_t x = 0; x < f.width; ++x) {
      uint16_t v = f.pixels[size_t(srcRow) * f.width + x];
      if (flags & TA_FRAME_MIN_IS_WHITE) v = uint16_t((bps == 8 ? 255 : 65535) - v);
      if (bps == 8) row[x] = uint8_t(v);
      else if (flags & TA_FRAME_BIG_ENDIAN) { row[2 * x] = uint8_t(v >> 8); row[2 * x + 1] = uint8_t(v); }
      else { row[2 * x] = uint8_t(v); row[2 * x + 1] = uint8_t(v >> 8); }
    }
  }
  return p;
}

// Minimal strict reader for the files we write: explicit VR little endian.
struct DicomElem {
  std::string vr;
  std::vector<uint8_t> value;
};
bool parseDicom(const std::vector<uint8_t>& b, std::map<uint32_t, DicomElem>& out, std::string& why) {
  if (b.size() < 132 || std::memcmp(b.data() + 128, "DICM", 4) != 0) return why = "no preamble", false;
  size_t at = 132;
  uint32_t last = 0;
  size_t metaEnd = 0;
  while (at < b.size()) {
    if (b.size() - at < 8) return why = "truncated header", false;
    const uint32_t tag = uint32_t(b[at] | b[at + 1] << 8) << 16 | uint32_t(b[at + 2] | b[at + 3] << 8);
    std::string vr(reinterpret_cast<const char*>(b.data() + at + 4), 2);
    size_t len, hdr;
    const bool longLen = vr == "OB" || vr == "OW" || vr == "SQ" || vr == "UT" || vr == "UN";
    if (longLen) {
      len = size_t(b[at + 8]) | size_t(b[at + 9]) << 8 | size_t(b[at + 10]) << 16 | size_t(b[at + 11]) << 24;
      hdr = 12;
    } else {
      len = size_t(b[at + 6]) | size_t(b[at + 7]) << 8;
      hdr = 8;
    }
    if (len % 2) return why = "odd length", false;
    if (at + hdr + len > b.size()) return why = "value overruns file", false;
    if (tag <= last && !(last >> 16 == 0x0002 && tag >> 16 != 0x0002)) return why = "tags not ascending", false;
    last = tag;
    out[tag] = {vr, std::vector<uint8_t>(b.begin() + long(at + hdr), b.begin() + long(at + hdr + len))};
    if (tag == 0x00020000) {
      const auto& v = out[tag].value;
      metaEnd = at + hdr + len + (size_t(v[0]) | size_t(v[1]) << 8 | size_t(v[2]) << 16 | size_t(v[3]) << 24);
    }
    at += hdr + len;
    if (metaEnd && at == metaEnd) last = 0x0002FFFF;
  }
  if (metaEnd == 0) return why = "no group length", false;
  return true;
}
std::string dstr(const std::map<uint32_t, DicomElem>& m, uint32_t tag) {
  auto it = m.find(tag);
  if (it == m.end()) return "<absent>";
  std::string s(it->second.value.begin(), it->second.value.end());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
  return s;
}

}  // namespace

TEST("kv: escapes round-trip, malformed lines refused") {
  Kv kv;
  kv.set("a", "plain");
  kv.set("b.c", "tab\there\nnewline\\slash\rcr");
  kv.set("empty", "");
  Kv back;
  CHECK(Kv::parse(kv.serialize(), back));
  CHECK(back.str("a") == "plain");
  CHECK(back.str("b.c") == "tab\there\nnewline\\slash\rcr");
  CHECK(back.get("empty").has_value());
  Kv bad;
  CHECK(!Kv::parse("noequals\n", bad));
  CHECK(!Kv::parse("bad key=1\n", bad));
  CHECK(!Kv::parse("k=bad\\qescape\n", bad));
  CHECK(Kv::parse("# comment\n\r\nk=v\r\n", bad) && bad.str("k") == "v");
}

TEST("wire: frame info refuses truncated and oversized frames") {
  Frame f = pattern(10, 4, 16);
  auto p = rawPayload(f, 16, 0);
  ta_frame_info fi;
  CHECK(ta_frame_info_get(p.data(), p.size(), &fi) == 1);
  CHECK(ta_frame_info_get(p.data(), p.size() - 1, &fi) == 0);
  CHECK(ta_frame_info_get(p.data(), TA_FRAME_INFO_SIZE - 1, &fi) == 0);
  p[16] = 1;  // row_bytes smaller than the width needs
  p[17] = p[18] = p[19] = 0;
  CHECK(ta_frame_info_get(p.data(), p.size(), &fi) == 0);
  uint8_t h[TA_WIRE_HEADER_SIZE];
  ta_wire_header hh;
  ta_wire_put_header(h, TA_MSG_FRAME, 100);
  CHECK(ta_wire_get_header(h, &hh) && hh.type == TA_MSG_FRAME && hh.length == 100);
  ta_wire_put_header(h, TA_MSG_FRAME, TA_WIRE_MAX_PAYLOAD + 1);
  CHECK(!ta_wire_get_header(h, &hh));
  h[0] ^= 1;
  CHECK(!ta_wire_get_header(h, &hh));
}

TEST("orientation: all eight match a naive reference") {
  const Frame f = pattern(5, 3);
  for (int q = 0; q < 4; ++q)
    for (int flip = 0; flip < 2; ++flip) {
      Orientation o{q, flip != 0};
      const Frame a = applyOrientation(f, o), b = naiveOrient(f, q, flip != 0);
      CHECK(a.width == b.width && a.height == b.height && a.pixels == b.pixels);
    }
  Orientation o;
  o = o.mirrored().rotated(1);
  const Frame viaSteps = naiveOrient(naiveOrient(f, 0, true), 1, false);
  CHECK(applyOrientation(f, o).pixels == viaSteps.pixels);
}

TEST("normalize: byte order, bottom-up rows, polarity, invert, bits, dpi") {
  const Frame f = pattern(9, 5, 16);
  for (uint32_t flags : {0u, unsigned(TA_FRAME_BIG_ENDIAN), unsigned(TA_FRAME_BOTTOM_UP),
                         unsigned(TA_FRAME_MIN_IS_WHITE | TA_FRAME_BIG_ENDIAN | TA_FRAME_BOTTOM_UP)}) {
    auto p = rawPayload(f, 16, flags);
    Frame out;
    CHECK(normalizeTransfer(p.data(), p.size(), {}, out));
    CHECK(out.pixels == f.pixels);
    CHECK(out.dpiX == 500.0);
  }
  auto p = rawPayload(f, 16, 0);
  NormalizeOptions opt;
  opt.invert = true;
  Frame inv;
  CHECK(normalizeTransfer(p.data(), p.size(), opt, inv));
  CHECK(inv.pixels[3] == 65535 - f.pixels[3]);
  NormalizeOptions bo;
  bo.byteOrder = NormalizeOptions::ByteOrder::Big;  // profile override wins over the flags
  Frame swapped;
  CHECK(normalizeTransfer(p.data(), p.size(), bo, swapped));
  CHECK(swapped.pixels[1] == uint16_t((f.pixels[1] >> 8) | (f.pixels[1] << 8)));
  NormalizeOptions bits;
  bits.bitsStored = 12;
  bits.dpiOverride = 254;
  Frame twelve;
  CHECK(normalizeTransfer(p.data(), p.size(), bits, twelve));
  CHECK(twelve.bitsStored == 12 && twelve.dpiX == 254);
  const Frame e = pattern(6, 2, 8);
  auto p8 = rawPayload(e, 8, TA_FRAME_MIN_IS_WHITE);
  Frame out8;
  CHECK(normalizeTransfer(p8.data(), p8.size(), {}, out8) && out8.bitsStored == 8 && out8.pixels == e.pixels);
}

TEST("tiff: 16- and 8-bit round trip, resolution kept, compression refused") {
  const Frame f = pattern(33, 17, 16);
  auto t16 = encodeTiff(f, 16, {"sw", "2026:01:02 03:04:05", "desc"});
  Frame back;
  TiffInfo info;
  CHECK(decodeTiff(t16.data(), t16.size(), back, &info));
  CHECK(back.pixels == f.pixels && back.width == 33 && back.height == 17);
  CHECK(std::abs(back.dpiX - 1000) < 1e-6 && info.description == "desc" && !info.minIsWhite);
  auto t8 = encodeTiff(f, 8);
  Frame b8;
  CHECK(decodeTiff(t8.data(), t8.size(), b8) && b8.bitsStored == 8);
  CHECK(b8.pixels[5] == (uint32_t(f.pixels[5]) * 255 + 32767) / 65535);
  auto bad = t16;
  bad[10 + 4 * 12 + 8] = 5;  // Compression entry value -> LZW
  Frame x;
  std::string why;
  CHECK(!decodeTiff(bad.data(), bad.size(), x, nullptr, &why));
  CHECK(!decodeTiff(t16.data(), 20, x, nullptr, &why));
  CHECK(tiffFits(4000, 3000, 16) && tiffFits(65535, 65535, 8) && !tiffFits(65535, 65535, 16));
  CHECK(tiffFits(46000, 46000, 16) && !tiffFits(47000, 47000, 16));
}

TEST("render: 1:1 is the LUT, orientation matches, shrinking averages") {
  const Frame f = pattern(8, 6, 12);
  std::vector<uint8_t> lut;
  buildLut({}, 12, 1.0, false, lut);
  CHECK(lut.size() == 4096 && lut[0] == 0 && lut[4095] == 255);
  std::vector<uint8_t> out(8 * 6);
  renderGray8(f, {}, lut, 0, 0, 8, 6, out.data(), 8, 6, 8);
  bool same = true;
  for (size_t i = 0; i < out.size(); ++i) same &= out[i] == lut[f.pixels[i]];
  CHECK(same);
  Orientation o{1, true};
  const Frame of = applyOrientation(f, o);
  std::vector<uint8_t> ro(6 * 8);
  renderGray8(f, o, lut, 0, 0, 6, 8, ro.data(), 6, 8, 6);
  same = true;
  for (size_t i = 0; i < ro.size(); ++i) same &= ro[i] == lut[of.pixels[i]];
  CHECK(same);
  std::vector<uint8_t> small(4 * 3);
  renderGray8(f, {}, lut, 0, 0, 8, 6, small.data(), 4, 3, 4);
  const uint32_t avg = (f.pixels[0] + f.pixels[1] + f.pixels[8] + f.pixels[9]) / 4;
  CHECK(small[0] == lut[avg]);
  std::vector<uint8_t> outside(4, 7);
  renderGray8(f, {}, lut, 100, 100, 2, 2, outside.data(), 2, 2, 2);
  CHECK(outside[0] == 0 && outside[3] == 0);
  const WindowLevel wl = autoWindow(f);
  CHECK(wl.width > 0 && wl.center > 0);
}

TEST("render: 8-bit window maps through the display lookup table") {
  Frame f = pattern(16, 8, 12);
  for (size_t i = 0; i < f.pixels.size(); ++i) f.pixels[i] = uint16_t(1000 + (i * 7) % 500);  // narrow band
  const WindowLevel wl = autoWindow(f);
  const Frame w = windowTo8Bit(f, wl);
  std::vector<uint8_t> lut;
  buildLut(wl, 12, 1.0, false, lut);
  bool same = w.bitsStored == 8 && w.width == 16 && w.dpiX == f.dpiX;
  uint16_t lo = 255, hi = 0;
  for (size_t i = 0; i < f.pixels.size(); ++i) {
    same &= w.pixels[i] == lut[f.pixels[i]];
    lo = std::min(lo, w.pixels[i]);
    hi = std::max(hi, w.pixels[i]);
  }
  CHECK(same);
  CHECK(hi - lo > 200);                                       // the band now spans most of 8 bits
  const Frame linear = toBitDepth(f, 8);
  uint16_t llo = 255, lhi = 0;
  for (uint16_t v : linear.pixels) llo = std::min(llo, v), lhi = std::max(lhi, v);
  CHECK(lhi - llo < 40);                                      // whereas linear scaling stays flat
}

TEST("staging: journal replays every change; torn line and damaged file are caught") {
  const fs::Path root = tempDir("staging");
  PatientInfo p;
  p.id = "P1";
  p.familyName = "Doe";
  p.givenName = "Jane\tTab";
  std::string err;
  auto s = StagingSession::create(root, "bitewings-4", "Sim", p, &err);
  CHECK(s != nullptr);
  if (!s) return;
  uint32_t a = 0, b = 0, c = 0;
  CHECK(s->addImage(pattern(20, 10), 0, &a, &err));
  CHECK(s->addImage(pattern(10, 20), 1, &b, &err));
  CHECK(s->addImage(pattern(4, 4), 2, &c, &err));
  CHECK(s->setOrientation(b, {3, true}));
  CHECK(s->discard(c));
  CHECK(s->place(a, 3));
  CHECK(!s->place(99, 1));

  auto r = StagingSession::resume(s->dir(), &err);
  CHECK(r != nullptr);
  if (!r) return;
  const SessionState& st = r->state();
  CHECK(st.layout == "bitewings-4" && st.patient.givenName == "Jane\tTab");
  CHECK(st.images.size() == 3);
  CHECK(st.image(a)->slot == 3 && (st.image(b)->orientation == Orientation{3, true}) && st.image(c)->discarded);
  Frame back;
  CHECK(r->loadImage(b, back) && back.width == 10 && back.bitsStored == 12);

  // A torn final record (crash mid-write) is ignored.
  fs::appendDurable(s->dir() / "session.journal", "place\tseq=1\tslot=0\t#0000");
  auto torn = StagingSession::resume(s->dir(), &err);
  CHECK(torn && torn->state().image(a)->slot == 3);

  // A damaged image file is reported, not displayed.
  std::vector<uint8_t> bytes;
  fs::readAll(s->dir() / "0001.tif", bytes);
  bytes[bytes.size() - 1] ^= 0xFF;
  fs::atomicWrite(s->dir() / "0001.tif", bytes.data(), bytes.size());
  CHECK(!r->loadImage(a, back, &err) && err.find("damaged") != std::string::npos);

  CHECK(StagingSession::listOpen(root).size() == 1);
  CHECK(r->close("test"));
  CHECK(StagingSession::listOpen(root).empty());
  std::filesystem::remove_all(root);
}

TEST("dicom: well-formed Part 10 with the intra-oral SOP class") {
  Frame f = pattern(30, 20, 12);
  PatientInfo p;
  p.id = "123";
  p.familyName = "Doe";
  p.givenName = "Jane";
  DicomStudy st;
  st.studyUid = makeUid();
  st.seriesUid = makeUid("1.2.3");
  st.studyDate = "20260102";
  DicomImage di;
  di.sopUid = makeUid();
  di.creationUnixMs = 1;
  di.imageComments = "Right molar bitewing";
  di.anatomicRegion = {{"X1", "99TEST", "Test region"}};
  di.windowCenter = 2000;
  di.windowWidth = 4000;
  auto bytes = encodeDicom(f, p, st, di);
  std::map<uint32_t, DicomElem> m;
  std::string why;
  CHECK(parseDicom(bytes, m, why));
  if (!why.empty()) std::fprintf(stderr, "  dicom: %s\n", why.c_str());
  CHECK(dstr(m, 0x00080016) == "1.2.840.10008.5.1.4.1.1.1.3");
  CHECK(dstr(m, 0x00020010) == "1.2.840.10008.1.2.1");
  CHECK(dstr(m, 0x00080060) == "IO");
  CHECK(dstr(m, 0x00100010) == "Doe^Jane");
  CHECK(dstr(m, 0x00181164) == "0.0254\\0.0254");
  CHECK(m[0x00280101].value[0] == 12 && m[0x00280010].value[0] == 20);
  CHECK(m[0x7FE00010].value.size() == 30 * 20 * 2);
  CHECK(m.count(0x00082218) == 1 && m.count(0x20500020) == 1);
  CHECK(st.studyUid.rfind("2.25.", 0) == 0 && st.studyUid.size() <= 64);
  CHECK(st.seriesUid.rfind("1.2.3.", 0) == 0);
  std::string longRoot(60, '1');
  CHECK(makeUid(longRoot).size() <= 64);
  // Same inputs give the same bytes, so a retried export is recognised as its own.
  CHECK(encodeDicom(f, p, st, di) == bytes);
}

TEST("export: jobs survive restart, outputs written, collisions refused") {
  const fs::Path root = tempDir("export");
  const fs::Path out = root / "out";
  std::string err;
  auto s = StagingSession::create(root, "bitewings-2", "Sim", PatientInfo{"ID7", "Roe", "Ann", "", "", ""}, &err);
  uint32_t a = 0, b = 0;
  s->addImage(pattern(40, 30), 1, &a);
  s->addImage(pattern(30, 40), 0, &b);
  s->setOrientation(a, {1, false});

  ExportTarget dcm;
  dcm.id = "dcm";
  dcm.folder = out / "dicom";
  ExportTarget tif;
  tif.id = "tif";
  tif.format = ExportTarget::Format::Tiff8;
  tif.folder = out / "tiff";
  tif.fileNamePattern = "{family}_{slot}";
  std::map<std::string, ExportTarget> targets{{"dcm", dcm}, {"tif", tif}};
  auto lookup = [&](const std::string& id, ExportTarget& t) {
    auto it = targets.find(id);
    if (it == targets.end()) return false;
    t = it->second;
    return true;
  };
  const Layout layout = builtinLayouts()[1];
  {
    ExportQueue q(root, lookup);
    CHECK(q.enqueueSession(s->dir(), s->state(), layout, {dcm, tif}, &err) == 4);
  }
  // A new queue object (as after a restart) finds the jobs on disk.
  auto qp = std::make_shared<ExportQueue>(root, lookup);
  ExportQueue& q = *qp;
  const auto waiting = q.snapshot();
  CHECK(waiting.size() == 4);
  q.start();
  for (int i = 0; i < 200 && q.counts().doneSinceStart < 4; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  q.stop();
  // Finished jobs leave memory and are only counted; their files move to done/.
  CHECK(q.counts().doneSinceStart == 4 && q.counts().pending == 0);
  CHECK(q.snapshot().empty());
  int doneFiles = 0;
  for (const auto& e : std::filesystem::directory_iterator(root / "queue" / "done")) doneFiles += e.path().extension() == ".job";
  CHECK(doneFiles == 4);
  CHECK(ExportQueue(root, lookup).snapshot().empty());  // a restart does not load them back
  CHECK(std::filesystem::exists(out / "tiff" / "Roe_Left bitewing.tif"));
  CHECK(std::filesystem::exists(out / "tiff" / "Roe_Right bitewing.tif"));
  Frame rotated;
  std::vector<uint8_t> bytes;
  fs::readAll(out / "tiff" / "Roe_Left bitewing.tif", bytes);
  CHECK(decodeTiff(bytes.data(), bytes.size(), rotated) && rotated.width == 30 && rotated.height == 40);

  // Running a finished job again is recognised as already done...
  ExportJob again = waiting.front();
  CHECK(q.perform(again, &err));
  // ...but a different file under that name is never overwritten.
  ExportJob other = again;
  other.outName = "Roe_Left bitewing.tif";
  other.targetId = again.targetId == "tif" ? "tif" : "tif";
  other.seq = b;
  CHECK(!q.perform(other, &err) && err.find("already exists") != std::string::npos);

  // A destination that disappears fails after the allowed attempts.
  targets.erase("dcm");
  std::string e2;
  auto q2p = std::make_shared<ExportQueue>(root, lookup);
  ExportQueue& q2 = *q2p;
  q2.maxAttempts = 1;
  auto s2 = StagingSession::create(root, "single", "Sim", PatientInfo{}, &e2);
  s2->addImage(pattern(8, 8), 0);
  CHECK(q2.enqueueSession(s2->dir(), s2->state(), builtinLayouts()[0], {dcm}, &e2) == 1);
  q2.start();
  for (int i = 0; i < 200 && q2.counts().failed == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  q2.stop();
  CHECK(q2.counts().failed == 1 && !q2.counts().lastError.empty());
  std::filesystem::remove_all(root);
}

TEST("export: stopping never waits on a write that hangs") {
  const fs::Path root = tempDir("hang");
  std::string err;
  auto s = StagingSession::create(root, "single", "Sim", PatientInfo{"1", "A", "", "", "", ""}, &err);
  s->addImage(pattern(8, 8), 0);
  ExportTarget t;
  t.id = "slow";
  t.folder = root / "out";
  // The lookup stands in for a write on an unreachable network folder.
  auto release = std::make_shared<std::atomic<bool>>(false);
  auto entered = std::make_shared<std::atomic<bool>>(false);
  auto q = std::make_shared<ExportQueue>(root, [release, entered, t](const std::string&, ExportTarget& out) {
    *entered = true;
    while (!*release) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    out = t;
    return true;
  });
  CHECK(q->enqueueSession(s->dir(), s->state(), builtinLayouts()[0], {t}, &err) == 1);
  q->start();
  for (int i = 0; i < 300 && !*entered; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(*entered);
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(!q->stop(200));  // returns although the worker is still inside the write
  const auto waited = std::chrono::steady_clock::now() - t0;
  CHECK(waited < std::chrono::milliseconds(1000));
  std::weak_ptr<ExportQueue> alive = q;
  q.reset();  // the owner lets go; the detached worker keeps the queue alive
  CHECK(!alive.expired());
  *release = true;
  for (int i = 0; i < 300 && !alive.expired(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(alive.expired());  // and frees it when the write returns
  std::filesystem::remove_all(root);
}

TEST("export: purge removes old exported sessions and job files, keeps open and needed ones") {
  const fs::Path root = tempDir("purge");
  std::string err;
  auto ago = [](const fs::Path& p, int days) {
    std::filesystem::last_write_time(p, std::filesystem::file_time_type::clock::now() - std::chrono::hours(24) * days);
  };
  // Exported long ago: goes.
  auto oldDone = StagingSession::create(root, "single", "Sim", PatientInfo{"1", "A", "", "", "", ""}, &err);
  oldDone->addImage(pattern(8, 8), 0);
  oldDone->close("exported");
  ago(oldDone->dir() / "session.journal", 30);
  // Open (never exported) and old: stays, the operator decides.
  auto oldOpen = StagingSession::create(root, "single", "Sim", PatientInfo{"2", "B", "", "", "", ""}, &err);
  oldOpen->addImage(pattern(8, 8), 0);
  ago(oldOpen->dir() / "session.journal", 30);
  // Exported recently: stays.
  auto recent = StagingSession::create(root, "single", "Sim", PatientInfo{"3", "C", "", "", "", ""}, &err);
  recent->close("exported");
  // Old and closed, but a failed job still needs it: stays.
  auto needed = StagingSession::create(root, "single", "Sim", PatientInfo{"4", "D", "", "", "", ""}, &err);
  needed->addImage(pattern(8, 8), 0);
  ExportTarget missing;
  missing.id = "gone";
  missing.folder = root / "out";
  {
    ExportQueue tmp(root, [](const std::string&, ExportTarget&) { return false; });
    CHECK(tmp.enqueueSession(needed->dir(), needed->state(), builtinLayouts()[0], {missing}, &err) == 1);
  }
  needed->close("exported");
  ago(needed->dir() / "session.journal", 30);
  // Finished-job files: one old, one new.
  std::filesystem::create_directories(root / "queue" / "done");
  ExportJob j;
  j.id = "old";
  j.state = ExportJob::State::Done;
  j.toKv().save(root / "queue" / "done" / "old.job");
  ago(root / "queue" / "done" / "old.job", 30);
  j.id = "new";
  j.toKv().save(root / "queue" / "done" / "new.job");

  ExportQueue q(root, [](const std::string&, ExportTarget&) { return false; });
  CHECK(q.purgeNow() == 0);  // retention 0 keeps everything
  q.setRetentionDays(7);
  CHECK(q.purgeNow() == 2);
  CHECK(!std::filesystem::exists(oldDone->dir()));
  CHECK(std::filesystem::exists(oldOpen->dir()) && std::filesystem::exists(recent->dir()));
  CHECK(std::filesystem::exists(needed->dir()));
  CHECK(!std::filesystem::exists(root / "queue" / "done" / "old.job"));
  CHECK(std::filesystem::exists(root / "queue" / "done" / "new.job"));
  std::filesystem::remove_all(root);
}

TEST("layout and profile: files round-trip; next empty slot wraps") {
  for (const Layout& l : builtinLayouts()) {
    Layout back;
    CHECK(Layout::fromKv(l.toKv(), back) && back.slotList.size() == l.slotList.size());
  }
  const auto builtins = builtinLayouts();
  CHECK(builtins.size() == 6);
  CHECK(builtins[4].id == "bw4-pa2" && builtins[4].slotList.size() == 6 && builtins[4].slotList[4].label == "Periapical 1");
  CHECK(builtins[5].id == "pa4-bw2" && builtins[5].slotList.size() == 6 && builtins[5].slotList[5].label == "Bitewing 2");
  Layout mirroredCopy = builtins[4];
  mirroredCopy.mirrored = true;
  mirroredCopy.slotList[0].orientation = {2, true};
  Layout back;
  CHECK(Layout::fromKv(mirroredCopy.toKv(), back) && back.mirrored && back.slotList[0].orientation == (Orientation{2, true}));
  CHECK(back.displayColumn(back.slotList[0]) == 3 && builtins[4].displayColumn(builtins[4].slotList[0]) == 0);
  const Layout fmx = builtinLayouts()[3];
  CHECK(fmx.slotList.size() == 18);
  std::vector<bool> filled(18, false);
  filled[1] = filled[2] = true;
  CHECK(fmx.nextEmpty(0, filled) == 3);
  CHECK(fmx.nextEmpty(17, filled) == 0);
  std::vector<bool> full(18, true);
  CHECK(fmx.nextEmpty(0, full) == -1);

  SourceProfile p;
  p.source = "Vendor X / Model:1";
  p.helperBits = 64;
  p.transfer = SourceProfile::Transfer::File;
  p.bitDepth = 8;
  p.normalize.polarity = NormalizeOptions::Polarity::MinIsWhite;
  p.normalize.bitsStored = 14;
  p.defaultOrientation = {2, true};
  const fs::Path dir = tempDir("profile");
  CHECK(p.save(dir));
  SourceProfile q = SourceProfile::load(dir, p.source);
  CHECK(q.helperBits == 64 && q.transfer == SourceProfile::Transfer::File && q.bitDepth == 8);
  CHECK(q.normalize.polarity == NormalizeOptions::Polarity::MinIsWhite && q.normalize.bitsStored == 14);
  CHECK(q.defaultOrientation == (Orientation{2, true}));
  CHECK(SourceProfile::pathFor(dir, p.source).filename() == "Vendor_X___Model_1.profile");
  std::filesystem::remove_all(dir);
}

TEST("handoff: RayMage-style flags, case, = and :, missing values, SSN dropped") {
  const auto presets = handoffPresets();
  const HandoffTemplate ray = presets[1];
  auto r = parseHandoff(ray, {"/PATID", "123456", "/NAME", "John", "/SURNAME", "Smith"});
  CHECK(r.matched && r.patient.id == "123456" && r.patient.givenName == "John" && r.patient.familyName == "Smith");
  r = parseHandoff(ray, {"/patid=77", "/Surname:VanDyke", "unrelated", "-x"});
  CHECK(r.patient.id == "77" && r.patient.familyName == "VanDyke" && r.patient.givenName.empty());
  r = parseHandoff(ray, {"/PATID", "/NAME", "Ann"});  // ID flag with no value
  CHECK(r.patient.id.empty() && r.patient.givenName == "Ann");
  r = parseHandoff(ray, {"--simulate"});
  CHECK(!r.matched);

  HandoffTemplate own = presets[0];
  r = parseHandoff(own, {"--patient-id", "9", "--birth", "19800131", "--sex", "female", "--accession", "A1"});
  CHECK(r.patient.id == "9" && r.patient.birthDate == "19800131" && r.patient.sex == "F" && r.patient.accession == "A1");

  HandoffTemplate custom{"c", "custom", "{id} {family} {given} {birth} {ssn} {sex}", "MM/DD/YYYY"};
  r = parseHandoff(custom, {"55", "Roe", "Ann", "01/31/1980", "123-45-6789", "M"});
  CHECK(r.patient.id == "55" && r.patient.familyName == "Roe" && r.patient.birthDate == "19800131" && r.patient.sex == "M");
  CHECK(r.problems.empty());
  r = parseHandoff(custom, {"55", "Roe", "Ann", "31/01/1980"});
  CHECK(r.patient.birthDate.empty() && r.problems.size() == 1);
  CHECK(convertDate("31.01.1980", "DD.MM.YYYY") == "19800131");
  CHECK(convertDate("1980-01-31", "YYYY-MM-DD") == "19800131");
  CHECK(convertDate("1980-13-31", "YYYY-MM-DD").empty());
}

TEST("handoff file: INI sections, case, comments, BOM, Windows-1252, name split, {file}") {
  HandoffFileMapping m;
  m.id = "Patient/PatNr";
  m.name = "Name";
  m.birth = "patient/geburtsdatum";
  m.sex = "Sex";
  m.dateFormat = "DD.MM.YYYY";
  // Windows-1252: 0xE9 = é, 0x80 = euro sign; a UTF-8 BOM-less ANSI file.
  std::string ansi = "; written by a practice program\r\n[Other]\r\nPatNr=999\r\n[PATIENT]\r\n  patnr = \"4711\"\r\n"
                     "NAME=Ren\xE9 M\xFCller\r\nGeburtsdatum=31.01.1980\r\nSSN=123-45-6789\r\nSex=female\r\nnote=\x80\r\n";
  const std::string text = decodeHandoffText(ansi);
  CHECK(text.find("Ren\xC3\xA9") != std::string::npos);
  CHECK(text.find("\xE2\x82\xAC") != std::string::npos);
  auto r = parseHandoffFile(text, m);
  CHECK(r.matched && r.patient.id == "4711");
  CHECK(r.patient.familyName == "M\xC3\xBCller" && r.patient.givenName == "Ren\xC3\xA9");
  CHECK(r.patient.birthDate == "19800131" && r.patient.sex == "F" && r.problems.empty());

  HandoffFileMapping caret;
  caret.name = "pn";
  r = parseHandoffFile(decodeHandoffText("\xEF\xBB\xBFpn=Roe^Ann\n"), caret);
  CHECK(r.patient.familyName == "Roe" && r.patient.givenName == "Ann");
  caret.name = "pn";
  r = parseHandoffFile("pn=Roe, Ann Marie\n", caret);
  CHECK(r.patient.familyName == "Roe" && r.patient.givenName == "Ann Marie");
  HandoffFileMapping none;
  CHECK(!none.any() && !parseHandoffFile("id=1\n", none).matched);
  CHECK(decodeHandoffText("plain") == "plain");
  CHECK(decodeHandoffText("\xC3\xA9") == "\xC3\xA9");  // already UTF-8 stays as is

  HandoffTemplate withFile{"f", "file", "/FILE {file} /PATID {id}", "YYYYMMDD"};
  r = parseHandoff(withFile, {"/file", "C:\\temp\\patient.ini", "/PATID", "5"});
  CHECK(r.file == "C:\\temp\\patient.ini" && r.patient.id == "5");
  PatientInfo a{"", "Roe", "", "", "F", ""}, b{"9", "Other", "Ann", "19800131", "M", "A1"};
  fillBlanks(a, b);
  CHECK(a.id == "9" && a.familyName == "Roe" && a.givenName == "Ann" && a.sex == "F" && a.accession == "A1");
}

TEST("mount: a burst of frames fills consecutive slots before any is saved") {
  const Layout bw2 = builtinLayouts()[1];
  std::vector<bool> reserved(2, false);
  // Two frames arrive back to back; nothing has been saved yet.
  auto [a, next1] = bw2.assignFrame(0, reserved, true);
  reserved[size_t(a)] = true;
  auto [b, next2] = bw2.assignFrame(next1, reserved, true);
  CHECK(a == 0 && b == 1 && next1 == 1 && next2 == -1);
  // A third frame finds the mount full and stays unplaced instead of replacing slot 2.
  CHECK(bw2.assignFrame(next2, {true, true}, true) == std::make_pair(-1, -1));
  // A deliberate retake into a filled slot keeps that slot, then moves on.
  CHECK(bw2.assignFrame(0, {true, false}, true) == std::make_pair(0, 1));
  // Without auto-advance the target stays.
  CHECK(bw2.assignFrame(1, {false, false}, false) == std::make_pair(1, 1));
}

TEST("crc32: known value") { CHECK(crc32("123456789", 9) == 0xCBF43926u); }

int main() { return check::runAll(); }
