#include "helper.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ta_kv.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
static HANDLE g_out = INVALID_HANDLE_VALUE;
#else
#include <errno.h>
#include <unistd.h>
static int g_out = -1;
#endif

static ta_state g_state = TA_STATE_CLOSED;

/* ---- output -------------------------------------------------------------- */

static int write_all(const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  while (n > 0) {
#ifdef _WIN32
    DWORD wrote = 0;
    DWORD chunk = n > 0x10000000u ? 0x10000000u : (DWORD)n;
    if (!WriteFile(g_out, p, chunk, &wrote, NULL) || wrote == 0) return 0;
#else
    ssize_t wrote = write(g_out, p, n);
    if (wrote < 0 && errno == EINTR) continue;
    if (wrote <= 0) return 0;
#endif
    p += wrote;
    n -= (size_t)wrote;
  }
  return 1;
}

int ta_send2(uint32_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
  uint8_t h[TA_WIRE_HEADER_SIZE];
  if ((uint64_t)alen + blen > TA_WIRE_MAX_PAYLOAD) return 0;
  ta_wire_put_header(h, type, alen + blen);
  return write_all(h, sizeof h) && (alen == 0 || write_all(a, alen)) && (blen == 0 || write_all(b, blen));
}

int ta_send(uint32_t type, const void *payload, uint32_t length) {
  return ta_send2(type, payload, length, NULL, 0);
}

static void send_kv2(uint32_t type, const char *k1, const char *v1, const char *k2, const char *v2,
                     const char *k3, const char *v3) {
  char buf[4096];
  size_t len = 0;
  if (k1) ta_kv_append(buf, sizeof buf, &len, k1, v1);
  if (k2) ta_kv_append(buf, sizeof buf, &len, k2, v2);
  if (k3) ta_kv_append(buf, sizeof buf, &len, k3, v3);
  ta_send(type, buf, (uint32_t)len);
}

void ta_send_state(ta_state s, const char *reason) {
  static const char *names[] = {"closed", "open", "armed", "transferring"};
  g_state = s;
  send_kv2(TA_MSG_STATE, "state", names[s], reason ? "reason" : NULL, reason ? reason : "", NULL, NULL);
}

ta_state ta_current_state(void) { return g_state; }

void ta_send_error(const char *code, const char *text, int fatal) {
  send_kv2(TA_MSG_ERROR, "code", code, "text", text, "fatal", fatal ? "true" : "false");
}

void ta_log(const char *level, const char *fmt, ...) {
  char text[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  send_kv2(TA_MSG_LOG, "level", level, "text", text, NULL, NULL);
}

/* ---- options ------------------------------------------------------------- */

long ta_opt_int(const char *kv, size_t n, const char *key, long def) {
  char v[64];
  char *end = NULL;
  long r;
  if (!ta_kv_find(kv, n, key, v, sizeof v) || v[0] == '\0') return def;
  r = strtol(v, &end, 10);
  return (end && *end == '\0') ? r : def;
}

int ta_opt_bool(const char *kv, size_t n, const char *key, int def) {
  char v[16];
  if (!ta_kv_find(kv, n, key, v, sizeof v)) return def;
  if (!strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "yes")) return 1;
  if (!strcmp(v, "false") || !strcmp(v, "0") || !strcmp(v, "no")) return 0;
  return def;
}

void ta_opt_str(const char *kv, size_t n, const char *key, char *out, size_t cap, const char *def) {
  if (!ta_kv_find(kv, n, key, out, cap)) {
    strncpy(out, def, cap - 1);
    out[cap - 1] = '\0';
  }
}

/* ---- dispatch ------------------------------------------------------------ */

static void wrong_state(const char *what) {
  char text[128];
  snprintf(text, sizeof text, "%s is not allowed in the current state", what);
  ta_send_error("state", text, 0);
}

int ta_dispatch(ta_backend *b, uint32_t type, const uint8_t *payload, uint32_t length) {
  switch (type) {
    case TA_MSG_LIST:
      b->list(b);
      ta_send(TA_MSG_LIST_END, NULL, 0);
      return 1;
    case TA_MSG_OPEN:
      if (g_state != TA_STATE_CLOSED) {
        wrong_state("open");
        return 1;
      }
      b->open(b, (const char *)payload, length);
      return 1;
    case TA_MSG_ENABLE:
      if (g_state != TA_STATE_OPEN) {
        wrong_state("arm");
        return 1;
      }
      b->enable(b);
      return 1;
    case TA_MSG_DISABLE:
      if (g_state == TA_STATE_ARMED || g_state == TA_STATE_TRANSFERRING) b->disable(b);
      else ta_send_state(g_state, "already disarmed");
      return 1;
    case TA_MSG_CLOSE:
      if (g_state == TA_STATE_ARMED || g_state == TA_STATE_TRANSFERRING) b->disable(b);
      if (g_state == TA_STATE_OPEN) b->close(b);
      else ta_send_state(g_state, "already closed");
      return 1;
    case TA_MSG_QUIT:
      if (g_state == TA_STATE_ARMED || g_state == TA_STATE_TRANSFERRING) b->disable(b);
      if (g_state == TA_STATE_OPEN) b->close(b);
      return 0;
    default:
      ta_send_error("unknown-request", "unknown request type", 0);
      return 1;
  }
}

/* ---- main ---------------------------------------------------------------- */

/* The protocol gets a private copy of stdout, and fd 1 is pointed at stderr,
   so a source that prints to stdout cannot corrupt the stream. */
static int claim_stdout(void) {
#ifdef _WIN32
  HANDLE proc = GetCurrentProcess();
  if (!DuplicateHandle(proc, GetStdHandle(STD_OUTPUT_HANDLE), proc, &g_out, 0, FALSE,
                       DUPLICATE_SAME_ACCESS))
    return 0;
  SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
  _dup2(_fileno(stderr), _fileno(stdout));
  _setmode(_fileno(stdin), _O_BINARY);
#else
  g_out = dup(STDOUT_FILENO);
  if (g_out < 0) return 0;
  dup2(STDERR_FILENO, STDOUT_FILENO);
#endif
  return 1;
}

int main(int argc, char **argv) {
  const char *backend = "sim";
  const char *dsm_path = NULL;
  char hello[256];
  size_t len = 0;
  char bits[8];
  ta_backend *b = NULL;
  int i;

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--backend") && i + 1 < argc) backend = argv[++i];
    else if (!strcmp(argv[i], "--dsm") && i + 1 < argc) dsm_path = argv[++i];
  }
  if (!claim_stdout()) return 2;
#ifdef _WIN32
  /* A DLL that fails to load must return an error, not raise a system dialog
     that nobody sees in a hidden helper and that blocks it until it is killed. */
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
#endif

  if (!strcmp(backend, "sim")) b = ta_sim_backend();
#ifdef TA_HAVE_TWAIN
  else if (!strcmp(backend, "twain")) b = ta_twain_backend(dsm_path);
#endif
#ifndef TA_HAVE_TWAIN
  (void)dsm_path;
#endif
  if (!b) {
    ta_send_error("backend", "this helper was built without the requested backend", 1);
    return 3;
  }

  snprintf(bits, sizeof bits, "%d", (int)(sizeof(void *) * 8));
  ta_kv_append(hello, sizeof hello, &len, "protocol", "1");
  ta_kv_append(hello, sizeof hello, &len, "bits", bits);
  ta_kv_append(hello, sizeof hello, &len, "backend", b->name);
  ta_send(TA_MSG_HELLO, hello, (uint32_t)len);
  return ta_run_loop(b);
}
