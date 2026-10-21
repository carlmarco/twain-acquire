/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ta_wire.h"

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

void ta_wire_put_header(uint8_t out[TA_WIRE_HEADER_SIZE], uint32_t type,
                        uint32_t length) {
  put32(out, TA_WIRE_MAGIC);
  put32(out + 4, type);
  put32(out + 8, length);
}

int ta_wire_get_header(const uint8_t in[TA_WIRE_HEADER_SIZE],
                       ta_wire_header *h) {
  if (get32(in) != TA_WIRE_MAGIC) return 0;
  h->type = get32(in + 4);
  h->length = get32(in + 8);
  return h->length <= TA_WIRE_MAX_PAYLOAD;
}

void ta_frame_info_put(uint8_t out[TA_FRAME_INFO_SIZE], const ta_frame_info *fi) {
  put32(out + 0, fi->container);
  put32(out + 4, fi->width);
  put32(out + 8, fi->height);
  put32(out + 12, fi->bits_per_sample);
  put32(out + 16, fi->row_bytes);
  put32(out + 20, fi->flags);
  put32(out + 24, fi->dpi_x_milli);
  put32(out + 28, fi->dpi_y_milli);
  put32(out + 32, fi->index);
  put32(out + 36, fi->reserved);
}

int ta_frame_info_get(const uint8_t *payload, size_t len, ta_frame_info *fi) {
  uint64_t need;
  if (len < TA_FRAME_INFO_SIZE) return 0;
  fi->container = get32(payload + 0);
  fi->width = get32(payload + 4);
  fi->height = get32(payload + 8);
  fi->bits_per_sample = get32(payload + 12);
  fi->row_bytes = get32(payload + 16);
  fi->flags = get32(payload + 20);
  fi->dpi_x_milli = get32(payload + 24);
  fi->dpi_y_milli = get32(payload + 28);
  fi->index = get32(payload + 32);
  fi->reserved = get32(payload + 36);

  if (fi->container == TA_FRAME_TIFF) return len > TA_FRAME_INFO_SIZE;
  if (fi->container != TA_FRAME_RAW) return 0;
  if (fi->width == 0 || fi->height == 0) return 0;
  if (fi->width > 65535 || fi->height > 65535) return 0;
  if (fi->bits_per_sample != 8 && fi->bits_per_sample != 16) return 0;
  if ((uint64_t)fi->row_bytes * 8 < (uint64_t)fi->width * fi->bits_per_sample)
    return 0;
  need = (uint64_t)fi->row_bytes * fi->height;
  return (uint64_t)(len - TA_FRAME_INFO_SIZE) >= need;
}
