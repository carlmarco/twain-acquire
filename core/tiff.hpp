#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "frame.hpp"

namespace ta {

struct TiffMeta {
  std::string software;
  std::string dateTime;  // "YYYY:MM:DD HH:MM:SS"; empty = omitted
  std::string description;
};

// Uncompressed single-strip greyscale TIFF, little-endian, 8 or 16 bits.
std::vector<uint8_t> encodeTiff(const Frame& f, int outBits, const TiffMeta& meta = {});

struct TiffInfo {
  bool minIsWhite = false;
  std::string description;
};

// Decode an uncompressed greyscale TIFF (8 or 16 bits, strips, either byte
// order). Anything else is refused with a reason rather than guessed at.
bool decodeTiff(const uint8_t* data, size_t size, Frame& out, TiffInfo* info = nullptr,
                std::string* err = nullptr);

}  // namespace ta
