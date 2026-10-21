/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frame.hpp"

#include <algorithm>

namespace ta {

namespace {
int wrap4(int q) { return ((q % 4) + 4) % 4; }
}  // namespace

Orientation Orientation::rotated(int turns) const {
  // The flip is applied after the rotation, so turning a mirrored image
  // clockwise is the same as turning the unmirrored one anticlockwise.
  Orientation o = *this;
  o.quarterTurns = wrap4(quarterTurns + (flipH ? -turns : turns));
  return o;
}

Orientation Orientation::mirrored() const {
  Orientation o = *this;
  o.flipH = !flipH;
  return o;
}

OrientMap orientMap(uint32_t storedW, uint32_t storedH, const Orientation& o) {
  const int64_t W = storedW, H = storedH;
  const int q = wrap4(o.quarterTurns);
  // index = a + b * x + c * y, where x is the column before mirroring.
  int64_t a = 0, b = 1, c = W;
  switch (q) {
    case 0: a = 0;               b = 1;  c = W;  break;
    case 1: a = (H - 1) * W;     b = -W; c = 1;  break;
    case 2: a = H * W - 1;       b = -1; c = -W; break;
    case 3: a = W - 1;           b = W;  c = -1; break;
  }
  OrientMap m;
  m.width = (q % 2 == 0) ? storedW : storedH;
  m.height = (q % 2 == 0) ? storedH : storedW;
  if (o.flipH) {
    a += b * (int64_t(m.width) - 1);
    b = -b;
  }
  m.base = a;
  m.colStride = b;
  m.rowStride = c;
  return m;
}

Frame applyOrientation(const Frame& f, const Orientation& o) {
  const OrientMap m = orientMap(f.width, f.height, o);
  Frame out;
  out.width = m.width;
  out.height = m.height;
  out.bitsStored = f.bitsStored;
  out.acquiredUnixMs = f.acquiredUnixMs;
  const bool swap = (((o.quarterTurns % 4) + 4) % 4) % 2 == 1;
  out.dpiX = swap ? f.dpiY : f.dpiX;
  out.dpiY = swap ? f.dpiX : f.dpiY;
  out.pixels.resize(size_t(m.width) * m.height);
  uint16_t* dst = out.pixels.data();
  for (uint32_t y = 0; y < m.height; ++y) {
    int64_t idx = m.base + int64_t(y) * m.rowStride;
    for (uint32_t x = 0; x < m.width; ++x, idx += m.colStride) *dst++ = f.pixels[size_t(idx)];
  }
  return out;
}

Frame toBitDepth(const Frame& f, int bits) {
  bits = std::clamp(bits, 8, 16);
  Frame out = f;
  if (bits == f.bitsStored) return out;
  const uint32_t inMax = f.maxValue();
  const uint32_t outMax = (1u << bits) - 1u;
  out.bitsStored = uint8_t(bits);
  for (auto& p : out.pixels) p = uint16_t((uint32_t(std::min<uint32_t>(p, inMax)) * outMax + inMax / 2) / inMax);
  return out;
}

}  // namespace ta
