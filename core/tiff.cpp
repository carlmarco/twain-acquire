/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "tiff.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace ta {

namespace {

enum : uint16_t { kAscii = 2, kShort = 3, kLong = 4, kRational = 5 };

struct Entry {
  uint16_t tag;
  uint16_t type;
  uint32_t count;
  std::vector<uint8_t> data;  // raw little-endian value bytes
};

void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
  b[at] = uint8_t(v);
  b[at + 1] = uint8_t(v >> 8);
}
void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i));
}

Entry shortEntry(uint16_t tag, uint16_t v) {
  return {tag, kShort, 1, {uint8_t(v), uint8_t(v >> 8)}};
}
Entry longEntry(uint16_t tag, uint32_t v) {
  return {tag, kLong, 1, {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}};
}
Entry asciiEntry(uint16_t tag, const std::string& s) {
  Entry e{tag, kAscii, uint32_t(s.size() + 1), std::vector<uint8_t>(s.begin(), s.end())};
  e.data.push_back(0);
  return e;
}
Entry rationalEntry(uint16_t tag, double v) {
  const uint32_t den = 1000;
  const uint32_t num = uint32_t(std::lround(v * den));
  Entry e{tag, kRational, 1, std::vector<uint8_t>(8)};
  put32(e.data, 0, num);
  put32(e.data, 4, den);
  return e;
}

}  // namespace

bool tiffFits(uint32_t width, uint32_t height, int outBits) {
  // Pixel data plus room for the header and tags, below the 32-bit offset limit.
  const uint64_t pixels = uint64_t(width) * height * (outBits == 8 ? 1u : 2u);
  return pixels <= 0xFFFFFFFFull - 65536;
}

std::vector<uint8_t> encodeTiff(const Frame& src, int outBits, const TiffMeta& meta) {
  if (!tiffFits(src.width, src.height, outBits)) return {};
  const Frame f = (outBits == 8) ? toBitDepth(src, 8) : src;
  const uint32_t bps = outBits == 8 ? 8 : 16;
  const uint32_t stripBytes = f.width * f.height * (bps / 8);

  std::vector<Entry> entries;
  entries.push_back(longEntry(254, 0));
  entries.push_back(longEntry(256, f.width));
  entries.push_back(longEntry(257, f.height));
  entries.push_back(shortEntry(258, uint16_t(bps)));
  entries.push_back(shortEntry(259, 1));
  entries.push_back(shortEntry(262, 1));  // BlackIsZero
  if (!meta.description.empty()) entries.push_back(asciiEntry(270, meta.description));
  entries.push_back(longEntry(273, 0));  // patched below
  entries.push_back(shortEntry(274, 1));
  entries.push_back(shortEntry(277, 1));
  entries.push_back(longEntry(278, f.height));
  entries.push_back(longEntry(279, stripBytes));
  if (f.dpiX > 0 && f.dpiY > 0) {
    entries.push_back(rationalEntry(282, f.dpiX));
    entries.push_back(rationalEntry(283, f.dpiY));
    entries.push_back(shortEntry(296, 2));  // inch
  }
  if (!meta.software.empty()) entries.push_back(asciiEntry(305, meta.software));
  if (!meta.dateTime.empty()) entries.push_back(asciiEntry(306, meta.dateTime));

  const size_t ifdSize = 2 + entries.size() * 12 + 4;
  size_t extra = 8 + ifdSize;
  std::map<size_t, size_t> extraAt;  // entry index -> offset of its out-of-line data
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].data.size() > 4) {
      extraAt[i] = extra;
      extra += (entries[i].data.size() + 1) & ~size_t(1);
    }
  }
  const size_t pixelAt = extra;

  std::vector<uint8_t> out(pixelAt + stripBytes, 0);
  out[0] = 'I';
  out[1] = 'I';
  put16(out, 2, 42);
  put32(out, 4, 8);
  put16(out, 8, uint16_t(entries.size()));
  for (size_t i = 0; i < entries.size(); ++i) {
    Entry& e = entries[i];
    if (e.tag == 273) put32(e.data, 0, uint32_t(pixelAt));
    const size_t at = 10 + i * 12;
    put16(out, at, e.tag);
    put16(out, at + 2, e.type);
    put32(out, at + 4, e.count);
    if (e.data.size() <= 4) {
      for (size_t k = 0; k < e.data.size(); ++k) out[at + 8 + k] = e.data[k];
    } else {
      put32(out, at + 8, uint32_t(extraAt[i]));
      for (size_t k = 0; k < e.data.size(); ++k) out[extraAt[i] + k] = e.data[k];
    }
  }
  put32(out, 10 + entries.size() * 12, 0);  // no next IFD

  uint8_t* p = out.data() + pixelAt;
  if (bps == 8) {
    for (uint16_t v : f.pixels) *p++ = uint8_t(v);
  } else {
    for (uint16_t v : f.pixels) {
      *p++ = uint8_t(v);
      *p++ = uint8_t(v >> 8);
    }
  }
  return out;
}

namespace {

class Reader {
 public:
  Reader(const uint8_t* d, size_t n, bool big) : d_(d), n_(n), big_(big) {}
  bool ok(size_t at, size_t len) const { return at <= n_ && len <= n_ - at; }
  uint16_t u16(size_t at) const {
    return big_ ? uint16_t(d_[at] << 8 | d_[at + 1]) : uint16_t(d_[at + 1] << 8 | d_[at]);
  }
  uint32_t u32(size_t at) const {
    return big_ ? (uint32_t(d_[at]) << 24 | uint32_t(d_[at + 1]) << 16 | uint32_t(d_[at + 2]) << 8 | d_[at + 3])
                : (uint32_t(d_[at + 3]) << 24 | uint32_t(d_[at + 2]) << 16 | uint32_t(d_[at + 1]) << 8 | d_[at]);
  }
  bool big() const { return big_; }
  const uint8_t* data() const { return d_; }

 private:
  const uint8_t* d_;
  size_t n_;
  bool big_;
};

struct Field {
  uint16_t type = 0;
  uint32_t count = 0;
  size_t valueAt = 0;  // where the value bytes start
};

bool fail(std::string* err, const char* what) {
  if (err) *err = what;
  return false;
}

}  // namespace

bool decodeTiff(const uint8_t* data, size_t size, Frame& out, TiffInfo* info, std::string* err) {
  if (size < 8) return fail(err, "TIFF: file too short");
  bool big;
  if (data[0] == 'I' && data[1] == 'I') big = false;
  else if (data[0] == 'M' && data[1] == 'M') big = true;
  else return fail(err, "TIFF: bad byte-order mark");
  Reader r(data, size, big);
  if (r.u16(2) != 42) return fail(err, "TIFF: not a classic TIFF");
  const size_t ifd = r.u32(4);
  if (!r.ok(ifd, 2)) return fail(err, "TIFF: IFD out of range");
  const uint16_t n = r.u16(ifd);
  if (!r.ok(ifd + 2, size_t(n) * 12)) return fail(err, "TIFF: IFD truncated");

  std::map<uint16_t, Field> fields;
  for (uint16_t i = 0; i < n; ++i) {
    const size_t at = ifd + 2 + size_t(i) * 12;
    Field f;
    f.type = r.u16(at + 2);
    f.count = r.u32(at + 4);
    const size_t unit = f.type == kShort ? 2 : f.type == kLong ? 4 : f.type == kRational ? 8 : 1;
    const uint64_t bytes = uint64_t(unit) * f.count;
    if (bytes > size) return fail(err, "TIFF: field larger than file");
    f.valueAt = bytes <= 4 ? at + 8 : r.u32(at + 8);
    if (!r.ok(f.valueAt, size_t(bytes))) return fail(err, "TIFF: field value out of range");
    fields[r.u16(at)] = f;
  }

  auto value = [&](uint16_t tag, size_t idx, uint32_t def) -> uint32_t {
    auto it = fields.find(tag);
    if (it == fields.end() || idx >= it->second.count) return def;
    const Field& f = it->second;
    if (f.type == kShort) return r.u16(f.valueAt + idx * 2);
    if (f.type == kLong) return r.u32(f.valueAt + idx * 4);
    return def;
  };
  auto rational = [&](uint16_t tag) -> double {
    auto it = fields.find(tag);
    if (it == fields.end() || it->second.type != kRational) return 0;
    const uint32_t num = r.u32(it->second.valueAt), den = r.u32(it->second.valueAt + 4);
    return den ? double(num) / den : 0;
  };

  const uint32_t width = value(256, 0, 0), height = value(257, 0, 0);
  const uint32_t bps = value(258, 0, 1);
  if (width == 0 || height == 0 || width > 65535 || height > 65535)
    return fail(err, "TIFF: bad dimensions");
  if (value(259, 0, 1) != 1) return fail(err, "TIFF: compressed images are not supported; use memory transfer");
  if (value(277, 0, 1) != 1) return fail(err, "TIFF: only single-channel greyscale is supported");
  if (bps != 8 && bps != 16) return fail(err, "TIFF: only 8- and 16-bit samples are supported");
  if (fields.count(322)) return fail(err, "TIFF: tiled images are not supported");
  const uint32_t photometric = value(262, 0, 1);
  if (photometric > 1) return fail(err, "TIFF: not a greyscale image");
  auto offsets = fields.find(273);
  auto counts = fields.find(279);
  if (offsets == fields.end() || counts == fields.end() || offsets->second.count != counts->second.count)
    return fail(err, "TIFF: strip tables missing");

  const size_t rowBytes = size_t(width) * (bps / 8);
  const uint32_t rowsPerStrip = std::min<uint32_t>(value(278, 0, height), height);
  if (rowsPerStrip == 0) return fail(err, "TIFF: zero rows per strip");

  out = Frame{};
  out.width = width;
  out.height = height;
  out.bitsStored = uint8_t(bps);
  out.pixels.resize(size_t(width) * height);
  uint32_t row = 0;
  for (uint32_t s = 0; s < offsets->second.count && row < height; ++s) {
    const size_t off = value(273, s, 0);
    const uint32_t rows = std::min(rowsPerStrip, height - row);
    if (!r.ok(off, rowBytes * rows)) return fail(err, "TIFF: strip out of range");
    for (uint32_t y = 0; y < rows; ++y, ++row) {
      const size_t at = off + size_t(y) * rowBytes;
      uint16_t* dst = out.pixels.data() + size_t(row) * width;
      if (bps == 8) {
        for (uint32_t x = 0; x < width; ++x) dst[x] = data[at + x];
      } else {
        for (uint32_t x = 0; x < width; ++x) dst[x] = r.u16(at + 2 * x);
      }
    }
  }
  if (row != height) return fail(err, "TIFF: strips do not cover the image");

  double rx = rational(282), ry = rational(283);
  const uint32_t unit = value(296, 0, 2);
  if (unit == 3) {  // centimetre
    rx *= 2.54;
    ry *= 2.54;
  } else if (unit != 2) {
    rx = ry = 0;
  }
  out.dpiX = rx;
  out.dpiY = ry;

  if (info) {
    info->minIsWhite = photometric == 0;
    info->description.clear();
    auto d = fields.find(270);
    if (d != fields.end() && d->second.type == kAscii && d->second.count > 0)
      info->description.assign(reinterpret_cast<const char*>(data + d->second.valueAt),
                               strnlen(reinterpret_cast<const char*>(data + d->second.valueAt), d->second.count));
  }
  return true;
}

}  // namespace ta
