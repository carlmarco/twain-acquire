/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "render.hpp"

#include <algorithm>
#include <cmath>

namespace ta {

WindowLevel autoWindow(const Frame& f, double lowPct, double highPct) {
  WindowLevel wl;
  if (!f.valid()) return wl;
  const size_t bins = size_t(f.maxValue()) + 1;
  std::vector<uint32_t> hist(bins, 0);
  // Every 4th pixel of every 4th row is plenty for percentiles.
  uint64_t total = 0;
  for (uint32_t y = 0; y < f.height; y += 4) {
    const uint16_t* row = f.pixels.data() + size_t(y) * f.width;
    for (uint32_t x = 0; x < f.width; x += 4) {
      ++hist[std::min<size_t>(row[x], bins - 1)];
      ++total;
    }
  }
  const uint64_t lowCount = uint64_t(total * lowPct / 100.0);
  const uint64_t highCount = uint64_t(total * highPct / 100.0);
  uint64_t acc = 0;
  size_t lo = 0, hi = bins - 1;
  bool haveLo = false;
  for (size_t v = 0; v < bins; ++v) {
    acc += hist[v];
    if (!haveLo && acc > lowCount) {
      lo = v;
      haveLo = true;
    }
    if (acc >= highCount) {
      hi = v;
      break;
    }
  }
  if (hi <= lo) hi = std::min(bins - 1, lo + 1);
  wl.center = (double(lo) + double(hi)) / 2.0;
  wl.width = double(hi - lo) + 1.0;
  return wl;
}

void buildLut(const WindowLevel& wl, int bitsStored, double gamma, bool invert,
              std::vector<uint8_t>& lut) {
  bitsStored = std::clamp(bitsStored, 8, 16);
  const size_t n = size_t(1) << bitsStored;
  lut.resize(n);
  double lo = 0, width = double(n);
  if (wl.width > 0) {
    width = std::max(1.0, wl.width);
    lo = wl.center - width / 2.0;
  }
  const double g = gamma > 0 ? 1.0 / gamma : 1.0;
  for (size_t v = 0; v < n; ++v) {
    double t = std::clamp((double(v) - lo) / width, 0.0, 1.0);
    if (g != 1.0) t = std::pow(t, g);
    uint8_t out = uint8_t(std::lround(t * 255.0));
    lut[v] = invert ? uint8_t(255 - out) : out;
  }
}

Frame windowTo8Bit(const Frame& f, const WindowLevel& wl) {
  std::vector<uint8_t> lut;
  buildLut(wl, f.bitsStored, 1.0, false, lut);
  Frame out;
  out.width = f.width;
  out.height = f.height;
  out.bitsStored = 8;
  out.dpiX = f.dpiX;
  out.dpiY = f.dpiY;
  out.acquiredUnixMs = f.acquiredUnixMs;
  out.pixels.resize(f.pixels.size());
  const uint16_t maxv = f.maxValue();
  for (size_t i = 0; i < f.pixels.size(); ++i) out.pixels[i] = lut[std::min(f.pixels[i], maxv)];
  return out;
}

void renderGray8(const Frame& f, const Orientation& o, const std::vector<uint8_t>& lut,
                 double sx, double sy, double sw, double sh, uint8_t* dst, int dw, int dh,
                 int dstStride) {
  if (dw <= 0 || dh <= 0) return;
  if (!f.valid() || lut.size() < size_t(f.maxValue()) + 1 || sw <= 0 || sh <= 0) {
    for (int y = 0; y < dh; ++y) std::fill_n(dst + size_t(y) * dstStride, dw, uint8_t(0));
    return;
  }
  const OrientMap m = orientMap(f.width, f.height, o);
  const double scaleX = sw / dw, scaleY = sh / dh;
  const int k = std::clamp(int(std::floor(std::min(scaleX, scaleY))), 1, 4);
  const uint16_t maxv = f.maxValue();

  // Integer source start of each output column / row; -1 = outside the image.
  auto starts = [k](int n, double s0, double scale, uint32_t limit, std::vector<int64_t>& out) {
    out.resize(size_t(n));
    for (int i = 0; i < n; ++i) {
      const double center = s0 + (i + 0.5) * scale;
      if (center < 0 || center >= double(limit)) {
        out[size_t(i)] = -1;
        continue;
      }
      int64_t start = int64_t(std::floor(center - k / 2.0 + 0.5));
      start = std::clamp<int64_t>(start, 0, int64_t(limit) - k < 0 ? 0 : int64_t(limit) - k);
      out[size_t(i)] = start;
    }
  };
  std::vector<int64_t> cols, rows;
  starts(dw, sx, scaleX, m.width, cols);
  starts(dh, sy, scaleY, m.height, rows);
  const int kk = (int64_t(m.width) >= k && int64_t(m.height) >= k) ? k : 1;

  std::vector<int64_t> colOff(static_cast<size_t>(dw));
  std::vector<int64_t> sub(static_cast<size_t>(kk * kk));
  for (int x = 0; x < dw; ++x) colOff[size_t(x)] = cols[size_t(x)] < 0 ? 0 : cols[size_t(x)] * m.colStride;
  for (int dy = 0; dy < kk; ++dy)
    for (int dx = 0; dx < kk; ++dx) sub[size_t(dy) * kk + dx] = dx * m.colStride + dy * m.rowStride;

  const uint16_t* px = f.pixels.data();
  const uint32_t area = uint32_t(kk * kk);
  for (int y = 0; y < dh; ++y) {
    uint8_t* out = dst + size_t(y) * dstStride;
    if (rows[size_t(y)] < 0) {
      std::fill_n(out, dw, uint8_t(0));
      continue;
    }
    const int64_t rowBase = m.base + rows[size_t(y)] * m.rowStride;
    for (int x = 0; x < dw; ++x) {
      if (cols[size_t(x)] < 0) {
        out[x] = 0;
        continue;
      }
      const uint16_t* p = px + (rowBase + colOff[size_t(x)]);
      uint32_t v;
      if (area == 1) {
        v = *p;
      } else {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < area; ++i) sum += p[sub[i]];
        v = sum / area;
      }
      out[x] = lut[std::min<uint32_t>(v, maxv)];
    }
  }
}

}  // namespace ta
