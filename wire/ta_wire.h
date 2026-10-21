/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * Wire format between the application and the acquisition helper.
 *
 * The helper runs as a separate process (so a 32-bit TWAIN source can serve a
 * 64-bit application, and so a crashing source cannot take the application
 * down). The two talk over the helper's stdin/stdout.
 *
 * Every message is a 12-byte header followed by `length` payload bytes:
 *
 *   uint32 magic   TA_WIRE_MAGIC
 *   uint32 type    enum ta_msg
 *   uint32 length  payload bytes
 *
 * All integers are little-endian. Text payloads are key=value lines (ta_kv.h).
 * A FRAME payload is a ta_frame_info block followed by the image bytes.
 */
#ifndef TA_WIRE_H
#define TA_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TA_WIRE_MAGIC 0x31514154u /* "TAQ1" */
#define TA_WIRE_PROTOCOL 1
#define TA_WIRE_HEADER_SIZE 12u
#define TA_WIRE_MAX_PAYLOAD (256u * 1024u * 1024u)

enum ta_msg {
  /* application -> helper */
  TA_MSG_LIST = 1,    /* enumerate sources; answered by SOURCE* then LIST_END */
  TA_MSG_OPEN = 2,    /* kv: source + profile; answered by OPENED or ERROR    */
  TA_MSG_ENABLE = 3,  /* arm; answered by STATE armed or ERROR               */
  TA_MSG_DISABLE = 4, /* disarm; answered by STATE open                      */
  TA_MSG_CLOSE = 5,   /* close the source; answered by STATE closed          */
  TA_MSG_QUIT = 6,    /* close everything and exit                           */

  /* helper -> application */
  TA_MSG_HELLO = 64,    /* kv: protocol, bits, backend                       */
  TA_MSG_SOURCE = 65,   /* kv: one source                                    */
  TA_MSG_LIST_END = 66, /* end of a LIST answer                              */
  TA_MSG_OPENED = 67,   /* kv: what was negotiated                           */
  TA_MSG_STATE = 68,    /* kv: state=closed|open|armed|transferring          */
  TA_MSG_FRAME = 69,    /* ta_frame_info + bytes                             */
  TA_MSG_ERROR = 70,    /* kv: code, text, fatal                             */
  TA_MSG_LOG = 71       /* kv: level, text                                   */
};

typedef struct {
  uint32_t type;
  uint32_t length;
} ta_wire_header;

void ta_wire_put_header(uint8_t out[TA_WIRE_HEADER_SIZE], uint32_t type,
                        uint32_t length);

/* Returns 1 when the magic matches and the length is within bounds. */
int ta_wire_get_header(const uint8_t in[TA_WIRE_HEADER_SIZE],
                       ta_wire_header *h);

/* Frame containers. */
enum { TA_FRAME_RAW = 0, TA_FRAME_TIFF = 1 };

/* Frame flags (RAW only). */
enum {
  TA_FRAME_BIG_ENDIAN = 1u,   /* 16-bit samples are big-endian        */
  TA_FRAME_MIN_IS_WHITE = 2u, /* sample 0 is white                    */
  TA_FRAME_BOTTOM_UP = 4u     /* first row in the buffer is the bottom */
};

#define TA_FRAME_INFO_SIZE 40u

typedef struct {
  uint32_t container;       /* TA_FRAME_RAW or TA_FRAME_TIFF          */
  uint32_t width;           /* RAW: pixels                            */
  uint32_t height;          /* RAW: rows                              */
  uint32_t bits_per_sample; /* RAW: 8 or 16                           */
  uint32_t row_bytes;       /* RAW: bytes per row in the buffer       */
  uint32_t flags;           /* RAW: TA_FRAME_* flags                  */
  uint32_t dpi_x_milli;     /* resolution x 1000, 0 = unknown          */
  uint32_t dpi_y_milli;
  uint32_t index;           /* transfer number within one arming       */
  uint32_t reserved;
} ta_frame_info;

void ta_frame_info_put(uint8_t out[TA_FRAME_INFO_SIZE], const ta_frame_info *fi);

/*
 * Parse and validate a FRAME payload of `len` bytes. Returns 1 only when the
 * info block is complete and, for RAW, the buffer holds every row it claims.
 */
int ta_frame_info_get(const uint8_t *payload, size_t len, ta_frame_info *fi);

#ifdef __cplusplus
}
#endif

#endif
