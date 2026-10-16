#pragma once

#include <cstddef>
#include <cstdint>

namespace ta {

// CRC-32 (IEEE 802.3, as used by zlib and PNG). Pass the previous result as
// `crc` to continue over several buffers.
uint32_t crc32(const void* data, size_t size, uint32_t crc = 0);

}  // namespace ta
