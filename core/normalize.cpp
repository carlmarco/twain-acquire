#include "normalize.hpp"

#include <algorithm>
#include <vector>

#include "ta_wire.h"
#include "tiff.hpp"

namespace ta {

namespace {

void setErr(std::string* err, const char* what) {
  if (err) *err = what;
}

}  // namespace

bool normalizeTransfer(const uint8_t* payload, size_t size, const NormalizeOptions& opt,
                       Frame& out, std::string* err) {
  ta_frame_info fi;
  if (!ta_frame_info_get(payload, size, &fi)) {
    setErr(err, "frame payload is truncated or malformed");
    return false;
  }
  const uint8_t* bytes = payload + TA_FRAME_INFO_SIZE;
  const size_t nbytes = size - TA_FRAME_INFO_SIZE;

  bool minIsWhite = false;
  if (fi.container == TA_FRAME_TIFF) {
    TiffInfo info;
    if (!decodeTiff(bytes, nbytes, out, &info, err)) return false;
    minIsWhite = info.minIsWhite;
  } else {
    bool big = (fi.flags & TA_FRAME_BIG_ENDIAN) != 0;
    if (opt.byteOrder == NormalizeOptions::ByteOrder::Little) big = false;
    if (opt.byteOrder == NormalizeOptions::ByteOrder::Big) big = true;
    minIsWhite = (fi.flags & TA_FRAME_MIN_IS_WHITE) != 0;
    const bool bottomUp = (fi.flags & TA_FRAME_BOTTOM_UP) != 0;

    out = Frame{};
    out.width = fi.width;
    out.height = fi.height;
    out.pixels.resize(size_t(fi.width) * fi.height);
    for (uint32_t y = 0; y < fi.height; ++y) {
      const uint8_t* row = bytes + size_t(bottomUp ? fi.height - 1 - y : y) * fi.row_bytes;
      uint16_t* dst = out.pixels.data() + size_t(y) * fi.width;
      if (fi.bits_per_sample == 8) {
        for (uint32_t x = 0; x < fi.width; ++x) dst[x] = row[x];
      } else if (big) {
        for (uint32_t x = 0; x < fi.width; ++x) dst[x] = uint16_t(row[2 * x] << 8 | row[2 * x + 1]);
      } else {
        for (uint32_t x = 0; x < fi.width; ++x) dst[x] = uint16_t(row[2 * x + 1] << 8 | row[2 * x]);
      }
    }
    // A 12- or 14-bit sensor in 16-bit containers needs the profile's bitsStored;
    // guessing from the data would make each image's scale depend on its exposure.
    out.bitsStored = uint8_t(fi.bits_per_sample);
    out.dpiX = fi.dpi_x_milli / 1000.0;
    out.dpiY = fi.dpi_y_milli / 1000.0;
  }

  if (opt.bitsStored >= 8 && opt.bitsStored < out.bitsStored) {
    const uint16_t cap = uint16_t((1u << opt.bitsStored) - 1u);
    for (auto& p : out.pixels) p = std::min(p, cap);
    out.bitsStored = uint8_t(opt.bitsStored);
  }
  if (opt.polarity == NormalizeOptions::Polarity::MinIsBlack) minIsWhite = false;
  if (opt.polarity == NormalizeOptions::Polarity::MinIsWhite) minIsWhite = true;
  if (minIsWhite != opt.invert) {
    const uint16_t maxv = out.maxValue();
    for (auto& p : out.pixels) p = uint16_t(maxv - std::min(p, maxv));
  }
  if (opt.dpiOverride > 0) out.dpiX = out.dpiY = opt.dpiOverride;
  return out.valid() || (setErr(err, "frame has no pixels"), false);
}

}  // namespace ta
