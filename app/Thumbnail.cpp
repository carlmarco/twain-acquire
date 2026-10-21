/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "Thumbnail.hpp"

#include <algorithm>
#include <vector>

#include "render.hpp"

QImage renderThumbnail(const ta::Frame& f, const ta::Orientation& o, int size) {
  const ta::OrientMap m = ta::orientMap(f.width, f.height, o);
  const double scale = std::min(double(size) / m.width, double(size) / m.height);
  const int w = std::max(1, int(m.width * scale)), h = std::max(1, int(m.height * scale));
  std::vector<uint8_t> lut;
  ta::buildLut(ta::autoWindow(f), f.bitsStored, 1.0, false, lut);
  QImage img(w, h, QImage::Format_Grayscale8);
  ta::renderGray8(f, o, lut, 0, 0, m.width, m.height, img.bits(), w, h, int(img.bytesPerLine()));
  return img;
}
