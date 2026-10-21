/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <cstdint>
#include <vector>

#include "frame.hpp"

namespace ta {

// Display rendering runs on the CPU through a lookup table. One table lookup
// per screen pixel is a few milliseconds for a full screen on a low-end office
// PC, and it needs no GPU or graphics driver.

struct WindowLevel {
  double center = 0;
  double width = 0;  // <= 0 means "full range"
};

// Window covering the [lowPct, highPct] percentiles of a sparse sample.
WindowLevel autoWindow(const Frame& f, double lowPct = 0.5, double highPct = 99.5);

// 2^bitsStored entries mapping a sample to an 8-bit display value.
void buildLut(const WindowLevel& wl, int bitsStored, double gamma, bool invert,
              std::vector<uint8_t>& lut);

// An 8-bit copy of `f` with the window applied through the same lookup table
// the screen uses: values below the window go black, above it white.
Frame windowTo8Bit(const Frame& f, const WindowLevel& wl);

// Render the oriented region (sx, sy, sw, sh) of `f` into an 8-bit buffer of
// dw x dh pixels. Shrinking averages up to 4x4 source pixels per output pixel;
// enlarging uses the nearest pixel so individual pixels stay visible.
// Outside the image the output is black.
void renderGray8(const Frame& f, const Orientation& o, const std::vector<uint8_t>& lut,
                 double sx, double sy, double sw, double sh, uint8_t* dst, int dw, int dh,
                 int dstStride);

}  // namespace ta
