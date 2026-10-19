/*
 * Event loop for Windows. TWAIN sources deliver their events through the
 * thread's message queue, so the main thread runs a message loop; a reader
 * thread turns stdin messages into thread messages for it.
 */
#include "helper.h"

#ifdef _WIN32

#include <stdlib.h>
#include <windows.h>

#define WM_TA_REQUEST (WM_APP + 1)
#define WM_TA_EOF (WM_APP + 2)

typedef struct {
  uint32_t type;
  uint32_t length;
  uint8_t data[1];
} request;

static DWORD g_main_thread;

static int read_exact(HANDLE in, uint8_t *p, size_t n) {
  while (n > 0) {
    DWORD got = 0;
    DWORD chunk = n > 0x10000000u ? 0x10000000u : (DWORD)n;
    if (!ReadFile(in, p, chunk, &got, NULL) || got == 0) return 0;
    p += got;
    n -= got;
  }
  return 1;
}

static DWORD WINAPI reader(LPVOID arg) {
  HANDLE in = (HANDLE)arg;
  for (;;) {
    uint8_t h[TA_WIRE_HEADER_SIZE];
    ta_wire_header hdr;
    request *req;
    if (!read_exact(in, h, sizeof h) || !ta_wire_get_header(h, &hdr)) break;
    req = (request *)malloc(sizeof(request) + hdr.length);
    if (!req) break;
    req->type = hdr.type;
    req->length = hdr.length;
    if (hdr.length && !read_exact(in, req->data, hdr.length)) {
      free(req);
      break;
    }
    req->data[hdr.length] = 0;
    while (!PostThreadMessageW(g_main_thread, WM_TA_REQUEST, 0, (LPARAM)req)) Sleep(1);
  }
  /* The application has gone: the main thread disarms and exits, and if a
     source has it stuck, the watchdog ends the process anyway. */
  ta_arm_exit_watchdog();
  PostThreadMessageW(g_main_thread, WM_TA_EOF, 0, 0);
  return 0;
}

int ta_run_loop(ta_backend *b) {
  MSG msg;
  HANDLE thread;
  /* Make sure this thread has a message queue before the reader posts to it. */
  PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
  g_main_thread = GetCurrentThreadId();
  thread = CreateThread(NULL, 0, reader, GetStdHandle(STD_INPUT_HANDLE), 0, NULL);
  if (!thread) return 4;
  CloseHandle(thread);

  for (;;) {
    int timeout = b->tick ? b->tick(b) : -1;
    DWORD wait = MsgWaitForMultipleObjectsEx(0, NULL, timeout < 0 ? INFINITE : (DWORD)timeout,
                                             QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (wait == WAIT_TIMEOUT) continue;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.hwnd == NULL && msg.message == WM_TA_REQUEST) {
        request *req = (request *)msg.lParam;
        int keep = ta_dispatch(b, req->type, req->data, req->length);
        free(req);
        if (!keep) return 0;
        continue;
      }
      if (msg.hwnd == NULL && msg.message == WM_TA_EOF) {
        ta_dispatch(b, TA_MSG_QUIT, NULL, 0);
        return 0;
      }
      if (msg.message == WM_QUIT) {
        ta_dispatch(b, TA_MSG_QUIT, NULL, 0);
        return 0;
      }
      if (b->event && b->event(b, &msg)) continue;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
}

#endif
