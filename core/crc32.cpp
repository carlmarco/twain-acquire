#include "crc32.hpp"

#include <array>

namespace ta {

namespace {

std::array<uint32_t, 256> makeTable() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    t[i] = c;
  }
  return t;
}

}  // namespace

uint32_t crc32(const void* data, size_t size, uint32_t crc) {
  static const std::array<uint32_t, 256> table = makeTable();
  const auto* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}

}  // namespace ta
