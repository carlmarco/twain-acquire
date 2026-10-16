/*
 * The acquisition helper: a small process that owns one TWAIN session (or a
 * simulated one) and talks to the application over stdin/stdout.
 *
 * The helper enforces the session states itself: a request in the wrong state
 * is answered with an ERROR and changes nothing.
 */
#ifndef TA_HELPER_H
#define TA_HELPER_H

#include <stddef.h>
#include <stdint.h>

#include "ta_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TA_STATE_CLOSED = 0,
  TA_STATE_OPEN = 1,
  TA_STATE_ARMED = 2,
  TA_STATE_TRANSFERRING = 3
} ta_state;

typedef struct ta_backend ta_backend;

struct ta_backend {
  const char *name;
  void *impl;
  /* Emit one SOURCE message per source. */
  void (*list)(ta_backend *b);
  /* Open the source named in the kv text. On success the backend sends OPENED. */
  int (*open)(ta_backend *b, const char *kv, size_t n);
  int (*enable)(ta_backend *b);
  int (*disable)(ta_backend *b);
  int (*close)(ta_backend *b);
  /* Called whenever the loop wakes. Returns the milliseconds until the backend
     next needs a call, or -1 for "only when something arrives". */
  int (*tick)(ta_backend *b);
  /* Windows only: offer a window message to the source. Returns 1 when the
     source consumed it. May be NULL. */
  int (*event)(ta_backend *b, void *msg);
};

/* Output, all on the helper's main thread. */
int ta_send(uint32_t type, const void *payload, uint32_t length);
int ta_send2(uint32_t type, const void *a, uint32_t alen, const void *b, uint32_t blen);
void ta_send_state(ta_state s, const char *reason);
void ta_send_error(const char *code, const char *text, int fatal);
void ta_log(const char *level, const char *fmt, ...);

/* The current state as last announced. */
ta_state ta_current_state(void);

/* Read an integer / boolean / string option from kv text. */
long ta_opt_int(const char *kv, size_t n, const char *key, long def);
int ta_opt_bool(const char *kv, size_t n, const char *key, int def);
void ta_opt_str(const char *kv, size_t n, const char *key, char *out, size_t cap, const char *def);

ta_backend *ta_sim_backend(void);
#ifdef TA_HAVE_TWAIN
/* dsm_path: UTF-8 full path of TWAINDSM.dll to use, or NULL/empty for the system one. */
ta_backend *ta_twain_backend(const char *dsm_path);
#endif

/* Request dispatch, shared by the platform loops. Returns 0 to stop. */
int ta_dispatch(ta_backend *b, uint32_t type, const uint8_t *payload, uint32_t length);

/* Platform loop. Returns the process exit code. */
int ta_run_loop(ta_backend *b);

#ifdef __cplusplus
}
#endif

#endif
