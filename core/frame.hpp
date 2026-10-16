#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ta {

// How a stored image is turned for display and export. Stored pixels are
// never rotated in place; this is applied when rendering or exporting.
struct Orientation {
  int quarterTurns = 0;  // clockwise, 0..3
  bool flipH = false;    // mirror left-right, applied after the rotation

  bool operator==(const Orientation& o) const {
    return quarterTurns == o.quarterTurns && flipH == o.flipH;
  }
  Orientation rotated(int turns) const;
  Orientation mirrored() const;
};

// The one in-memory image type. Samples are 16-bit containers holding
// `bitsStored` significant bits; 0 is black (MONOCHROME2).
struct Frame {
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t bitsStored = 16;  // 8..16
  double dpiX = 0;          // 0 = unknown
  double dpiY = 0;
  int64_t acquiredUnixMs = 0;
  std::vector<uint16_t> pixels;  // row-major, width * height

  bool valid() const {
    return width > 0 && height > 0 && bitsStored >= 8 && bitsStored <= 16 &&
           pixels.size() == size_t(width) * height;
  }
  uint16_t maxValue() const { return uint16_t((1u << bitsStored) - 1u); }
};

// Linear map from oriented (display) coordinates to an index into the stored
// pixel array: index = base + ox * colStride + oy * rowStride.
struct OrientMap {
  int64_t base = 0;
  int64_t colStride = 1;
  int64_t rowStride = 0;
  uint32_t width = 0;   // oriented width
  uint32_t height = 0;  // oriented height
};

OrientMap orientMap(uint32_t storedW, uint32_t storedH, const Orientation& o);

// A new frame with the orientation baked into the pixels (dpi swapped to match).
Frame applyOrientation(const Frame& f, const Orientation& o);

// Rescale samples to `bits` significant bits (8..16), rounding to nearest.
Frame toBitDepth(const Frame& f, int bits);

}  // namespace ta
