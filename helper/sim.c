/*
 * Simulated sources. They exist to exercise the application without hardware,
 * including the ways real sources misbehave: hanging, crashing, delivering
 * only 8 bits, or sending big-endian min-is-white data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "helper.h"
#include "ta_kv.h"

#ifdef _WIN32
#include <windows.h>
static uint64_t now_ms(void) { return GetTickCount64(); }
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
#include <unistd.h>
static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static void sleep_ms(unsigned ms) { usleep(ms * 1000u); }
#endif

enum { SIM_NORMAL, SIM_HANG_ON_ARM, SIM_CRASH_ON_TRANSFER, SIM_EIGHT_BIT, SIM_INVERTED_BE, SIM_SERIES };

static const char *const k_names[] = {
    "Simulated sensor",
    "Simulated sensor (hangs when armed)",
    "Simulated sensor (crashes during transfer)",
    "Simulated sensor (8-bit only)",
    "Simulated sensor (big-endian, min-is-white)",
    "Simulated sensor (series of 2 in one batch)",
};

typedef struct {
  int kind;
  int width, height, bits;
  int interval_ms;
  int count;      /* -1 = until disarmed */
  int delivered;  /* this arming */
  uint64_t due;   /* next exposure, 0 = none */
  uint32_t seed;
} sim;

static sim g_sim;

static void sim_list(ta_backend *b) {
  size_t i;
  (void)b;
  for (i = 0; i < sizeof k_names / sizeof k_names[0]; ++i) {
    char buf[512];
    size_t len = 0;
    ta_kv_append(buf, sizeof buf, &len, "name", k_names[i]);
    ta_kv_append(buf, sizeof buf, &len, "manufacturer", "twain-acquire");
    ta_kv_append(buf, sizeof buf, &len, "family", "Simulator");
    ta_kv_append(buf, sizeof buf, &len, "version", "1.0");
    ta_kv_append(buf, sizeof buf, &len, "twain2", "true");
    ta_send(TA_MSG_SOURCE, buf, (uint32_t)len);
  }
}

static int sim_open(ta_backend *b, const char *kv, size_t n) {
  char name[256];
  char reply[1024];
  char num[32];
  size_t len = 0, i;
  int requested;
  (void)b;
  ta_opt_str(kv, n, "source", name, sizeof name, "");
  memset(&g_sim, 0, sizeof g_sim);
  g_sim.kind = -1;
  for (i = 0; i < sizeof k_names / sizeof k_names[0]; ++i)
    if (!strcmp(name, k_names[i])) g_sim.kind = (int)i;
  if (g_sim.kind < 0) {
    ta_send_error("no-source", "no such source", 0);
    return 0;
  }
  g_sim.width = (int)ta_opt_int(kv, n, "sim.width", 1000);
  g_sim.height = (int)ta_opt_int(kv, n, "sim.height", 1300);
  g_sim.interval_ms = (int)ta_opt_int(kv, n, "sim.interval-ms", 1500);
  g_sim.count = (int)ta_opt_int(kv, n, "transfer-count", -1);
  requested = (int)ta_opt_int(kv, n, "bit-depth", 0);
  g_sim.bits = (g_sim.kind == SIM_EIGHT_BIT || requested == 8) ? 8 : 16;
  g_sim.seed = 12345u;
  if (g_sim.width < 16 || g_sim.width > 8000 || g_sim.height < 16 || g_sim.height > 8000) {
    ta_send_error("bad-option", "simulated size out of range", 0);
    return 0;
  }

  ta_kv_append(reply, sizeof reply, &len, "source", name);
  ta_kv_append(reply, sizeof reply, &len, "transfer", "memory");
  snprintf(num, sizeof num, "%d", g_sim.bits);
  ta_kv_append(reply, sizeof reply, &len, "bit-depth", num);
  if (requested && requested != g_sim.bits)
    ta_kv_append(reply, sizeof reply, &len, "note", "the source refused the requested bit depth");
  ta_send(TA_MSG_OPENED, reply, (uint32_t)len);
  ta_send_state(TA_STATE_OPEN, NULL);
  return 1;
}

static int sim_enable(ta_backend *b) {
  (void)b;
  if (g_sim.kind == SIM_HANG_ON_ARM) {
    for (;;) sleep_ms(1000); /* never answers: the application must time out */
  }
  g_sim.delivered = 0;
  g_sim.due = now_ms() + (uint64_t)g_sim.interval_ms;
  ta_send_state(TA_STATE_ARMED, NULL);
  return 1;
}

static int sim_disable(ta_backend *b) {
  (void)b;
  g_sim.due = 0;
  ta_send_state(TA_STATE_OPEN, "disarmed");
  return 1;
}

static int sim_close(ta_backend *b) {
  (void)b;
  g_sim.due = 0;
  ta_send_state(TA_STATE_CLOSED, NULL);
  return 1;
}

static uint32_t rnd(void) {
  g_sim.seed = g_sim.seed * 1664525u + 1013904223u;
  return g_sim.seed >> 8;
}

/* A plausible-looking 12-bit image: dark background, bright rounded blocks
   where teeth would be, and some noise. */
static void synth(uint8_t *rows, int row_bytes) {
  const int w = g_sim.width, h = g_sim.height;
  const int maxv = g_sim.bits == 8 ? 255 : 4095;
  int x, y, t;
  for (y = 0; y < h; ++y) {
    uint8_t *row = rows + (size_t)y * (size_t)row_bytes;
    for (x = 0; x < w; ++x) {
      double v = 0.12 + 0.10 * (double)y / h;
      for (t = 0; t < 3; ++t) {
        double cx = w * (0.25 + 0.25 * t), cy = h * 0.55;
        double dx = (x - cx) / (w * 0.10), dy = (y - cy) / (h * 0.32);
        double d = dx * dx + dy * dy;
        if (d < 1.0) v += 0.55 * (1.0 - d * d);
      }
      v += ((double)(rnd() % 1000) / 1000.0 - 0.5) * 0.03;
      if (v < 0) v = 0;
      if (v > 1) v = 1;
      {
        int s = (int)(v * maxv);
        if (g_sim.bits == 8) {
          row[x] = (uint8_t)s;
        } else if (g_sim.kind == SIM_INVERTED_BE) {
          s = 65535 - s;
          row[2 * x] = (uint8_t)(s >> 8);
          row[2 * x + 1] = (uint8_t)s;
        } else {
          row[2 * x] = (uint8_t)s;
          row[2 * x + 1] = (uint8_t)(s >> 8);
        }
      }
    }
  }
}

static void deliver(void) {
  ta_frame_info fi;
  uint8_t info[TA_FRAME_INFO_SIZE];
  uint8_t *pixels;
  size_t size;
  memset(&fi, 0, sizeof fi);
  fi.container = TA_FRAME_RAW;
  fi.width = (uint32_t)g_sim.width;
  fi.height = (uint32_t)g_sim.height;
  fi.bits_per_sample = (uint32_t)g_sim.bits;
  fi.row_bytes = fi.width * (fi.bits_per_sample / 8);
  fi.flags = g_sim.kind == SIM_INVERTED_BE ? (TA_FRAME_BIG_ENDIAN | TA_FRAME_MIN_IS_WHITE) : 0;
  fi.dpi_x_milli = fi.dpi_y_milli = 1270000; /* 20 um pixels */
  fi.index = (uint32_t)g_sim.delivered;
  size = (size_t)fi.row_bytes * fi.height;
  pixels = (uint8_t *)malloc(size);
  if (!pixels) {
    ta_send_error("memory", "out of memory building the image", 0);
    return;
  }
  ta_send_state(TA_STATE_TRANSFERRING, NULL);
  if (g_sim.kind == SIM_CRASH_ON_TRANSFER) abort();
  synth(pixels, (int)fi.row_bytes);
  ta_frame_info_put(info, &fi);
  ta_send_frame(info, pixels, size);
  free(pixels);
  ++g_sim.delivered;
}

static int sim_tick(ta_backend *b) {
  uint64_t now;
  (void)b;
  if (!g_sim.due) return -1;
  now = now_ms();
  if (now < g_sim.due) return (int)(g_sim.due - now);
  if (g_sim.kind == SIM_SERIES) {
    /* Like a source that collects a series itself: every image in one burst,
       then it waits, still enabled, for the application to disable it. */
    deliver();
    deliver();
    g_sim.due = 0;
    ta_send_state(TA_STATE_ARMED, "batch");
    return -1;
  }
  deliver();
  if (g_sim.count > 0 && g_sim.delivered >= g_sim.count) {
    /* Like a TWAIN source with no pending transfers left. */
    g_sim.due = 0;
    ta_send_state(TA_STATE_OPEN, "all images delivered");
    return -1;
  }
  g_sim.due = now_ms() + (uint64_t)g_sim.interval_ms;
  ta_send_state(TA_STATE_ARMED, "batch"); /* each simulated exposure is a batch of one */
  return g_sim.interval_ms;
}

static ta_backend g_backend = {"sim", NULL, sim_list, sim_open, sim_enable, sim_disable,
                               sim_close, sim_tick, NULL};

ta_backend *ta_sim_backend(void) { return &g_backend; }
