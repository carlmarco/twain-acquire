/* Event loop for non-Windows builds (simulated sources only).
 *
 * As on Windows, a reader thread takes requests off stdin and the main thread
 * runs the backend, so the end of stdin (the application has gone) is seen
 * even while the main thread is stuck inside a source. */
#include "helper.h"

#ifndef _WIN32

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct request {
  struct request *next;
  uint32_t type;
  uint32_t length;
  uint8_t data[1];
} request;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static request *g_head, *g_tail;
static int g_eof, g_bad;
static int g_wake[2] = {-1, -1}; /* self-pipe: the reader wakes the main loop */

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

static void wake(void) {
  const char c = 1;
  while (write(g_wake[1], &c, 1) < 0 && errno == EINTR) {
  }
}

static void *reader(void *arg) {
  (void)arg;
  for (;;) {
    uint8_t h[TA_WIRE_HEADER_SIZE];
    ta_wire_header hdr;
    request *req;
    if (!read_exact(h, sizeof h)) break;
    if (!ta_wire_get_header(h, &hdr)) {
      pthread_mutex_lock(&g_mu);
      g_bad = 1;
      pthread_mutex_unlock(&g_mu);
      break;
    }
    req = (request *)malloc(sizeof(request) + hdr.length);
    if (!req) break;
    req->next = NULL;
    req->type = hdr.type;
    req->length = hdr.length;
    if (hdr.length && !read_exact(req->data, hdr.length)) {
      free(req);
      break;
    }
    req->data[hdr.length] = 0;
    pthread_mutex_lock(&g_mu);
    if (g_tail) g_tail->next = req;
    else g_head = req;
    g_tail = req;
    pthread_mutex_unlock(&g_mu);
    wake();
  }
  /* The application has gone: the main thread disarms and exits, and if a
     source has it stuck, the watchdog ends the process anyway. */
  ta_arm_exit_watchdog();
  pthread_mutex_lock(&g_mu);
  g_eof = 1;
  pthread_mutex_unlock(&g_mu);
  wake();
  return NULL;
}

int ta_run_loop(ta_backend *b) {
  pthread_t thread;
  if (pipe(g_wake) != 0) return 4;
  if (pthread_create(&thread, NULL, reader, NULL) != 0) return 4;
  pthread_detach(thread);

  for (;;) {
    int timeout = b->tick ? b->tick(b) : -1;
    struct pollfd pfd = {g_wake[0], POLLIN, 0};
    int r = poll(&pfd, 1, timeout);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) return 4;
    if (r > 0) {
      char drain[64];
      (void)read(g_wake[0], drain, sizeof drain);
    }
    for (;;) {
      request *req;
      int eof, bad;
      pthread_mutex_lock(&g_mu);
      req = g_head;
      if (req) {
        g_head = req->next;
        if (!g_head) g_tail = NULL;
      }
      eof = g_eof;
      bad = g_bad;
      pthread_mutex_unlock(&g_mu);
      if (!req) {
        if (bad) {
          ta_send_error("protocol", "bad message header", 1);
          return 5;
        }
        if (eof) {
          /* stdin closed: leave the source disarmed and closed. */
          ta_dispatch(b, TA_MSG_QUIT, NULL, 0);
          return 0;
        }
        break;
      }
      {
        const int keep = ta_dispatch(b, req->type, req->data, req->length);
        free(req);
        if (!keep) return 0;
      }
    }
  }
}

#endif
