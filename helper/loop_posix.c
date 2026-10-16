/* Event loop for non-Windows builds (simulated sources only). */
#include "helper.h"

#ifndef _WIN32

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

static int read_exact(uint8_t *p, size_t n) {
  while (n > 0) {
    ssize_t r = read(STDIN_FILENO, p, n);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return 0;
    p += r;
    n -= (size_t)r;
  }
  return 1;
}

int ta_run_loop(ta_backend *b) {
  for (;;) {
    int timeout = b->tick ? b->tick(b) : -1;
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    int r = poll(&pfd, 1, timeout);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) return 4;
    if (r == 0) continue;

    uint8_t h[TA_WIRE_HEADER_SIZE];
    ta_wire_header hdr;
    uint8_t *payload = NULL;
    int keep;
    if (!read_exact(h, sizeof h)) break; /* application went away */
    if (!ta_wire_get_header(h, &hdr)) {
      ta_send_error("protocol", "bad message header", 1);
      return 5;
    }
    if (hdr.length) {
      payload = (uint8_t *)malloc(hdr.length + 1);
      if (!payload || !read_exact(payload, hdr.length)) {
        free(payload);
        break;
      }
      payload[hdr.length] = 0;
    }
    keep = ta_dispatch(b, hdr.type, payload, hdr.length);
    free(payload);
    if (!keep) return 0;
  }
  /* stdin closed: leave the source disarmed and closed. */
  ta_dispatch(b, TA_MSG_QUIT, NULL, 0);
  return 0;
}

#endif
