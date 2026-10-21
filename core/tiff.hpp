/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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
// Returns an empty vector when the image would not fit a classic TIFF
// (offsets are 32-bit, so about 4 GB): callers must treat that as a failure.
std::vector<uint8_t> encodeTiff(const Frame& f, int outBits, const TiffMeta& meta = {});
bool tiffFits(uint32_t width, uint32_t height, int outBits);

struct TiffInfo {
  bool minIsWhite = false;
  std::string description;
};

// Decode an uncompressed greyscale TIFF (8 or 16 bits, strips, either byte
// order). Anything else is refused with a reason rather than guessed at.
bool decodeTiff(const uint8_t* data, size_t size, Frame& out, TiffInfo* info = nullptr,
                std::string* err = nullptr);

}  // namespace ta
