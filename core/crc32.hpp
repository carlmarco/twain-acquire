/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <cstddef>
#include <cstdint>

namespace ta {

// CRC-32 (IEEE 802.3, as used by zlib and PNG). Pass the previous result as
// `crc` to continue over several buffers.
uint32_t crc32(const void* data, size_t size, uint32_t crc = 0);

}  // namespace ta
