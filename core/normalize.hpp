#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "frame.hpp"

namespace ta {

// Per-source corrections applied when a transfer becomes a Frame. Each one
// exists because some source reports it wrongly or not at all.
struct NormalizeOptions {
  enum class ByteOrder { Auto, Little, Big } byteOrder = ByteOrder::Auto;
  enum class Polarity { Auto, MinIsBlack, MinIsWhite } polarity = Polarity::Auto;
  int bitsStored = 0;   // 0 = as transferred; else the significant bits in a 16-bit sample
  bool invert = false;  // flip the grey scale after everything else
  double dpiOverride = 0;
};

// Turn one FRAME payload (ta_frame_info + bytes) into a canonical Frame.
bool normalizeTransfer(const uint8_t* payload, size_t size, const NormalizeOptions& opt,
                       Frame& out, std::string* err = nullptr);

}  // namespace ta
