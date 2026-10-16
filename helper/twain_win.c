/*
 * TWAIN backend (Windows). Built only when twain.h is available
 * (-DTWAIN_INCLUDE_DIR=...). Loads TWAINDSM.dll, which must match this
 * helper's bitness, and drives one source through TWAIN states 3 to 7.
 */
#include "helper.h"

#if defined(_WIN32) && defined(TA_HAVE_TWAIN)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "ta_kv.h"
#include "twain.h"

#define WM_TA_TWAIN (WM_APP + 10)

typedef TW_UINT16(FAR PASCAL *dsm_entry_fn)(pTW_IDENTITY, pTW_IDENTITY, TW_UINT32, TW_UINT16, TW_UINT16,
                                            TW_MEMREF);

typedef struct {
  HMODULE dll;
  dsm_entry_fn entry;
  TW_ENTRYPOINT mem; /* valid when have_mem */
  int have_mem;
  TW_IDENTITY app;
  TW_IDENTITY src;
  HWND parent;
  int dsm_open;
  int ds_open;
  int enabled;
  int use_callback;
  DWORD thread;

  /* negotiated */
  TW_UINT16 mech;
  int show_ui, modal_ui;
  char temp_file[MAX_PATH];
  wchar_t dsm_override[MAX_PATH]; /* from --dsm; empty = system DSM */
  uint32_t index;
} twain;

static twain g;

/* ---- small helpers -------------------------------------------------------- */

static TW_UINT16 dsm(pTW_IDENTITY dest, TW_UINT32 dg, TW_UINT16 dat, TW_UINT16 msg, TW_MEMREF data) {
  return g.entry(&g.app, dest, dg, dat, msg, data);
}

static TW_UINT16 condition(pTW_IDENTITY dest) {
  TW_STATUS st;
  memset(&st, 0, sizeof st);
  if (dsm(dest, DG_CONTROL, DAT_STATUS, MSG_GET, (TW_MEMREF)&st) != TWRC_SUCCESS) return 0xFFFF;
  return st.ConditionCode;
}

static void twain_error(const char *what, pTW_IDENTITY dest, int fatal) {
  char text[256];
  snprintf(text, sizeof text, "%s failed (TWAIN condition %u)", what, (unsigned)condition(dest));
  ta_send_error("twain", text, fatal);
}

static TW_HANDLE mem_alloc(TW_UINT32 size) {
  if (g.have_mem && g.mem.DSM_MemAllocate) return g.mem.DSM_MemAllocate(size);
  return (TW_HANDLE)GlobalAlloc(GHND, size);
}
static void mem_free(TW_HANDLE h) {
  if (!h) return;
  if (g.have_mem && g.mem.DSM_MemFree) g.mem.DSM_MemFree(h);
  else GlobalFree((HGLOBAL)h);
}
static TW_MEMREF mem_lock(TW_HANDLE h) {
  if (g.have_mem && g.mem.DSM_MemLock) return g.mem.DSM_MemLock(h);
  return (TW_MEMREF)GlobalLock((HGLOBAL)h);
}
static void mem_unlock(TW_HANDLE h) {
  if (g.have_mem && g.mem.DSM_MemUnlock) g.mem.DSM_MemUnlock(h);
  else GlobalUnlock((HGLOBAL)h);
}

static void copy_str32(TW_STR32 dst, const char *src) {
  strncpy((char *)dst, src, sizeof(TW_STR32) - 1);
  ((char *)dst)[sizeof(TW_STR32) - 1] = '\0';
}

/* Set a one-value capability. Returns 1 on success. */
static int cap_set(TW_UINT16 cap, TW_UINT16 type, TW_UINT32 value) {
  TW_CAPABILITY c;
  pTW_ONEVALUE one;
  TW_UINT16 rc;
  c.Cap = cap;
  c.ConType = TWON_ONEVALUE;
  c.hContainer = mem_alloc(sizeof(TW_ONEVALUE));
  if (!c.hContainer) return 0;
  one = (pTW_ONEVALUE)mem_lock(c.hContainer);
  one->ItemType = type;
  one->Item = value;
  mem_unlock(c.hContainer);
  rc = dsm(&g.src, DG_CONTROL, DAT_CAPABILITY, MSG_SET, (TW_MEMREF)&c);
  mem_free(c.hContainer);
  return rc == TWRC_SUCCESS || rc == TWRC_CHECKSTATUS;
}

/* Current value of a capability holding a 16-bit item. Returns 1 on success. */
static int cap_get16(TW_UINT16 cap, TW_UINT32 *out) {
  TW_CAPABILITY c;
  int ok = 0;
  memset(&c, 0, sizeof c);
  c.Cap = cap;
  c.ConType = TWON_DONTCARE16;
  if (dsm(&g.src, DG_CONTROL, DAT_CAPABILITY, MSG_GETCURRENT, (TW_MEMREF)&c) != TWRC_SUCCESS) return 0;
  if (!c.hContainer) return 0;
  if (c.ConType == TWON_ONEVALUE) {
    pTW_ONEVALUE one = (pTW_ONEVALUE)mem_lock(c.hContainer);
    *out = one->Item & 0xFFFFu;
    ok = 1;
    mem_unlock(c.hContainer);
  } else if (c.ConType == TWON_ENUMERATION) {
    pTW_ENUMERATION e = (pTW_ENUMERATION)mem_lock(c.hContainer);
    if (e->CurrentIndex < e->NumItems && (e->ItemType == TWTY_UINT16 || e->ItemType == TWTY_INT16 ||
                                          e->ItemType == TWTY_BOOL)) {
      *out = ((TW_UINT16 *)e->ItemList)[e->CurrentIndex];
      ok = 1;
    }
    mem_unlock(c.hContainer);
  }
  mem_free(c.hContainer);
  return ok;
}

static double fix32(TW_FIX32 f) { return (double)f.Whole + (double)f.Frac / 65536.0; }

static LRESULT CALLBACK parent_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static TW_UINT16 FAR PASCAL ds_callback(pTW_IDENTITY origin, pTW_IDENTITY dest, TW_UINT32 dg, TW_UINT16 dat,
                                        TW_UINT16 msg, TW_MEMREF data) {
  (void)origin; (void)dest; (void)dg; (void)dat; (void)data;
  /* The callback can arrive on any thread; hand it to the main loop. */
  PostThreadMessageW(g.thread, WM_TA_TWAIN, msg, 0);
  return TWRC_SUCCESS;
}

/* ---- DSM ------------------------------------------------------------------ */

/* What a candidate DSM file is, for the error report: size and PE machine. */
static void describe_dll(const wchar_t *path, char *out, size_t cap) {
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  unsigned char hdr[512];
  DWORD got = 0;
  LARGE_INTEGER size;
  const char *machine = "not a Windows DLL";
  char hex[16];
  if (f == INVALID_HANDLE_VALUE) {
    snprintf(out, cap, "cannot be opened");
    return;
  }
  size.QuadPart = 0;
  GetFileSizeEx(f, &size);
  ReadFile(f, hdr, sizeof hdr, &got, NULL);
  CloseHandle(f);
  if (got >= 0x40 && hdr[0] == 'M' && hdr[1] == 'Z') {
    DWORD pe = (DWORD)hdr[0x3C] | (DWORD)hdr[0x3D] << 8 | (DWORD)hdr[0x3E] << 16 | (DWORD)hdr[0x3F] << 24;
    if (pe + 6 <= got && hdr[pe] == 'P' && hdr[pe + 1] == 'E' && hdr[pe + 2] == 0 && hdr[pe + 3] == 0) {
      unsigned m = (unsigned)hdr[pe + 4] | (unsigned)hdr[pe + 5] << 8;
      switch (m) {
        case 0x014C: machine = "32-bit x86"; break;
        case 0x8664: machine = "64-bit x64"; break;
        case 0xAA64: machine = "ARM64"; break;
        case 0x01C4: machine = "ARM32"; break;
        default:
          snprintf(hex, sizeof hex, "machine 0x%04X", m);
          machine = hex;
      }
    }
  }
  snprintf(out, cap, "%lld bytes, %s", (long long)size.QuadPart, machine);
}

static void append_utf8(char *buf, size_t cap, const wchar_t *w) {
  size_t len = strlen(buf);
  if (len + 1 < cap) WideCharToMultiByte(CP_UTF8, 0, w, -1, buf + len, (int)(cap - len), NULL, NULL);
}

/*
 * Load the DSM by full path only: an explicit path from the application, then
 * the system directory (WOW64 sends a 32-bit helper to SysWOW64), then the
 * helper's own folder. The bare-name search order is never used, so a stray
 * or planted TWAINDSM.dll in the current directory or on PATH is not picked up.
 */
static int load_dsm(void) {
  wchar_t candidates[3][MAX_PATH];
  int n = 0, i, tried = 0;
  char report[2048];
  const int helper_bits = (int)(sizeof(void *) * 8);
  snprintf(report, sizeof report, "TWAINDSM.dll could not be loaded by this %d-bit helper.", helper_bits);

  if (g.dsm_override[0]) wcscpy_s(candidates[n++], MAX_PATH, g.dsm_override);
  if (GetSystemDirectoryW(candidates[n], MAX_PATH) && wcscat_s(candidates[n], MAX_PATH, L"\\TWAINDSM.dll") == 0) ++n;
  if (GetModuleFileNameW(NULL, candidates[n], MAX_PATH)) {
    wchar_t *slash = wcsrchr(candidates[n], L'\\');
    if (slash && wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - candidates[n]), L"TWAINDSM.dll") == 0) ++n;
  }

  for (i = 0; i < n; ++i) {
    char what[160], line[224];
    DWORD err;
    if (GetFileAttributesW(candidates[i]) == INVALID_FILE_ATTRIBUTES) continue;
    ++tried;
    g.dll = LoadLibraryExW(candidates[i], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (g.dll) {
      char ok[512] = "";
      append_utf8(ok, sizeof ok, candidates[i]);
      ta_log("info", "TWAIN DSM: %s", ok);
      return 1;
    }
    err = GetLastError();
    describe_dll(candidates[i], what, sizeof what);
    strncat(report, " ", sizeof report - strlen(report) - 1);
    append_utf8(report, sizeof report, candidates[i]);
    snprintf(line, sizeof line, ": Windows error %lu (%s).", (unsigned long)err, what);
    strncat(report, line, sizeof report - strlen(report) - 1);
  }
  if (tried == 0) {
    strncat(report, " No TWAINDSM.dll was found in the system directory or beside the helper; install the TWAIN Data Source Manager.",
            sizeof report - strlen(report) - 1);
  }
  ta_send_error("no-dsm", report, 0);
  return 0;
}

static int open_dsm(void) {
  if (g.dsm_open) return 1;
  if (!g.dll) {
    if (!load_dsm()) return 0;
    g.entry = (dsm_entry_fn)(void *)GetProcAddress(g.dll, "DSM_Entry");
    if (!g.entry) {
      ta_send_error("no-dsm", "TWAINDSM.dll has no DSM_Entry", 0);
      FreeLibrary(g.dll);
      g.dll = NULL;
      return 0;
    }
  }
  if (!g.parent) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = parent_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"twain-acquire-helper";
    RegisterClassW(&wc);
    g.parent = CreateWindowExW(0, wc.lpszClassName, L"twain-acquire", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                               CW_USEDEFAULT, 400, 300, NULL, NULL, wc.hInstance, NULL);
  }

  memset(&g.app, 0, sizeof g.app);
  g.app.Version.MajorNum = 1;
  g.app.Version.MinorNum = 0;
  g.app.Version.Language = TWLG_ENGLISH;
  g.app.Version.Country = TWCY_USA;
  copy_str32(g.app.Version.Info, "1.0");
  g.app.ProtocolMajor = TWON_PROTOCOLMAJOR;
  g.app.ProtocolMinor = TWON_PROTOCOLMINOR;
  g.app.SupportedGroups = DF_APP2 | DG_IMAGE | DG_CONTROL;
  copy_str32(g.app.Manufacturer, "twain-acquire");
  copy_str32(g.app.ProductFamily, "twain-acquire");
  copy_str32(g.app.ProductName, "twain-acquire");

  if (g.entry(&g.app, NULL, DG_CONTROL, DAT_PARENT, MSG_OPENDSM, (TW_MEMREF)&g.parent) != TWRC_SUCCESS) {
    ta_send_error("dsm-open", "the TWAIN Data Source Manager refused to open", 0);
    return 0;
  }
  g.dsm_open = 1;
  g.have_mem = 0;
  if (g.app.SupportedGroups & DF_DSM2) {
    memset(&g.mem, 0, sizeof g.mem);
    g.mem.Size = sizeof g.mem;
    g.have_mem = dsm(NULL, DG_CONTROL, DAT_ENTRYPOINT, MSG_GET, (TW_MEMREF)&g.mem) == TWRC_SUCCESS;
  }
  return 1;
}

static void close_dsm(void) {
  if (!g.dsm_open) return;
  g.entry(&g.app, NULL, DG_CONTROL, DAT_PARENT, MSG_CLOSEDSM, (TW_MEMREF)&g.parent);
  g.dsm_open = 0;
}

/* ---- backend -------------------------------------------------------------- */

static void tw_list(ta_backend *b) {
  TW_IDENTITY id;
  TW_UINT16 rc;
  int opened_here = !g.dsm_open;
  (void)b;
  if (!open_dsm()) return;
  memset(&id, 0, sizeof id);
  for (rc = dsm(NULL, DG_CONTROL, DAT_IDENTITY, MSG_GETFIRST, (TW_MEMREF)&id); rc == TWRC_SUCCESS;
       rc = dsm(NULL, DG_CONTROL, DAT_IDENTITY, MSG_GETNEXT, (TW_MEMREF)&id)) {
    char buf[1024];
    size_t len = 0;
    ta_kv_append(buf, sizeof buf, &len, "name", (const char *)id.ProductName);
    ta_kv_append(buf, sizeof buf, &len, "manufacturer", (const char *)id.Manufacturer);
    ta_kv_append(buf, sizeof buf, &len, "family", (const char *)id.ProductFamily);
    ta_kv_append(buf, sizeof buf, &len, "version", (const char *)id.Version.Info);
    ta_kv_append(buf, sizeof buf, &len, "twain2", (id.SupportedGroups & DF_DS2) ? "true" : "false");
    ta_send(TA_MSG_SOURCE, buf, (uint32_t)len);
    memset(&id, 0, sizeof id);
  }
  if (opened_here) close_dsm();
}

static int tw_open(ta_backend *b, const char *kv, size_t n) {
  char name[64], transfer[16], reply[2048], num[32];
  size_t len = 0;
  long bit_depth, count;
  TW_IDENTITY id;
  TW_UINT16 rc;
  int found = 0;
  TW_UINT32 v;
  (void)b;

  ta_opt_str(kv, n, "source", name, sizeof name, "");
  ta_opt_str(kv, n, "transfer", transfer, sizeof transfer, "auto");
  bit_depth = ta_opt_int(kv, n, "bit-depth", 0);
  count = ta_opt_int(kv, n, "transfer-count", -1);
  g.show_ui = ta_opt_bool(kv, n, "show-ui", 0);
  g.modal_ui = ta_opt_bool(kv, n, "modal-ui", 0);
  g.thread = GetCurrentThreadId();
  g.index = 0;

  if (!open_dsm()) return 0;
  memset(&id, 0, sizeof id);
  for (rc = dsm(NULL, DG_CONTROL, DAT_IDENTITY, MSG_GETFIRST, (TW_MEMREF)&id); rc == TWRC_SUCCESS;
       rc = dsm(NULL, DG_CONTROL, DAT_IDENTITY, MSG_GETNEXT, (TW_MEMREF)&id)) {
    if (!strcmp((const char *)id.ProductName, name)) {
      found = 1;
      break;
    }
  }
  if (!found) {
    ta_send_error("no-source", "no TWAIN source with that name", 0);
    close_dsm();
    return 0;
  }
  g.src = id;
  if (dsm(NULL, DG_CONTROL, DAT_IDENTITY, MSG_OPENDS, (TW_MEMREF)&g.src) != TWRC_SUCCESS) {
    twain_error("opening the source", NULL, 0);
    close_dsm();
    return 0;
  }
  g.ds_open = 1;

  g.use_callback = 0;
  if (g.src.SupportedGroups & DF_DS2) {
    TW_CALLBACK cb;
    memset(&cb, 0, sizeof cb);
    cb.CallBackProc = (TW_MEMREF)ds_callback;
    g.use_callback = dsm(&g.src, DG_CONTROL, DAT_CALLBACK, MSG_REGISTER_CALLBACK, (TW_MEMREF)&cb) == TWRC_SUCCESS;
  }

  /* Negotiate. A refusal is reported, not fatal: the source keeps its default. */
  ta_kv_append(reply, sizeof reply, &len, "source", name);
  if (!cap_set(ICAP_PIXELTYPE, TWTY_UINT16, TWPT_GRAY))
    ta_kv_append(reply, sizeof reply, &len, "note.pixel-type", "source refused greyscale");
  if (bit_depth == 8 || bit_depth == 16) {
    if (!cap_set(ICAP_BITDEPTH, TWTY_UINT16, (TW_UINT32)bit_depth))
      ta_kv_append(reply, sizeof reply, &len, "note.bit-depth", "source refused the requested bit depth");
  }
  if (!cap_set(CAP_XFERCOUNT, TWTY_INT16, (TW_UINT32)(TW_UINT16)(TW_INT16)count))
    ta_kv_append(reply, sizeof reply, &len, "note.transfer-count", "source refused the transfer count");

  {
    const TW_UINT16 order_auto[] = {TWSX_MEMORY, TWSX_FILE, TWSX_NATIVE};
    TW_UINT16 want[3];
    int nwant = 0, i;
    if (!strcmp(transfer, "memory")) want[nwant++] = TWSX_MEMORY;
    else if (!strcmp(transfer, "file")) want[nwant++] = TWSX_FILE;
    else if (!strcmp(transfer, "native")) want[nwant++] = TWSX_NATIVE;
    else for (i = 0; i < 3; ++i) want[nwant++] = order_auto[i];
    g.mech = TWSX_NATIVE; /* every source must support native */
    for (i = 0; i < nwant; ++i) {
      if (cap_set(ICAP_XFERMECH, TWTY_UINT16, want[i])) {
        g.mech = want[i];
        break;
      }
    }
    if (g.mech == TWSX_FILE && !cap_set(ICAP_IMAGEFILEFORMAT, TWTY_UINT16, TWFF_TIFF))
      ta_kv_append(reply, sizeof reply, &len, "note.file-format", "source refused TIFF file format");
  }

  ta_kv_append(reply, sizeof reply, &len, "transfer",
               g.mech == TWSX_MEMORY ? "memory" : g.mech == TWSX_FILE ? "file" : "native");
  if (cap_get16(ICAP_BITDEPTH, &v)) {
    snprintf(num, sizeof num, "%u", (unsigned)v);
    ta_kv_append(reply, sizeof reply, &len, "bit-depth", num);
  }
  if (cap_get16(ICAP_PIXELFLAVOR, &v))
    ta_kv_append(reply, sizeof reply, &len, "pixel-flavor", v == TWPF_VANILLA ? "min-is-white" : "min-is-black");
  ta_kv_append(reply, sizeof reply, &len, "callback", g.use_callback ? "true" : "false");
  ta_kv_append(reply, sizeof reply, &len, "twain2", (g.src.SupportedGroups & DF_DS2) ? "true" : "false");
  ta_send(TA_MSG_OPENED, reply, (uint32_t)len);
  ta_send_state(TA_STATE_OPEN, NULL);
  return 1;
}

static int tw_enable(ta_backend *b) {
  TW_USERINTERFACE ui;
  TW_UINT16 rc;
  (void)b;
  memset(&ui, 0, sizeof ui);
  ui.ShowUI = (TW_BOOL)g.show_ui;
  ui.ModalUI = (TW_BOOL)g.modal_ui;
  ui.hParent = (TW_HANDLE)g.parent;
  rc = dsm(&g.src, DG_CONTROL, DAT_USERINTERFACE, MSG_ENABLEDS, (TW_MEMREF)&ui);
  if (rc == TWRC_CHECKSTATUS) {
    ta_log("warn", "the source showed its own window although it was asked not to");
  } else if (rc != TWRC_SUCCESS) {
    twain_error("arming the source", &g.src, 0);
    return 0;
  }
  g.enabled = 1;
  ta_send_state(TA_STATE_ARMED, NULL);
  return 1;
}

static void disable_ds(const char *reason) {
  TW_USERINTERFACE ui;
  if (!g.enabled) return;
  memset(&ui, 0, sizeof ui);
  ui.hParent = (TW_HANDLE)g.parent;
  dsm(&g.src, DG_CONTROL, DAT_USERINTERFACE, MSG_DISABLEDS, (TW_MEMREF)&ui);
  g.enabled = 0;
  ta_send_state(TA_STATE_OPEN, reason);
}

static int tw_disable(ta_backend *b) {
  (void)b;
  disable_ds("disarmed");
  return 1;
}

static int tw_close(ta_backend *b) {
  (void)b;
  disable_ds("closing");
  if (g.ds_open) {
    dsm(NULL, DG_CONTROL, DAT_IDENTITY, MSG_CLOSEDS, (TW_MEMREF)&g.src);
    g.ds_open = 0;
  }
  close_dsm();
  ta_send_state(TA_STATE_CLOSED, NULL);
  return 1;
}

/* ---- transfers ------------------------------------------------------------ */

static void send_raw(uint32_t w, uint32_t h, uint32_t bps, uint32_t row_bytes, uint32_t flags, double dpix,
                     double dpiy, const uint8_t *data) {
  ta_frame_info fi;
  uint8_t info[TA_FRAME_INFO_SIZE];
  memset(&fi, 0, sizeof fi);
  fi.container = TA_FRAME_RAW;
  fi.width = w;
  fi.height = h;
  fi.bits_per_sample = bps;
  fi.row_bytes = row_bytes;
  fi.flags = flags;
  fi.dpi_x_milli = (uint32_t)(dpix * 1000.0 + 0.5);
  fi.dpi_y_milli = (uint32_t)(dpiy * 1000.0 + 0.5);
  fi.index = g.index++;
  ta_frame_info_put(info, &fi);
  ta_send2(TA_MSG_FRAME, info, sizeof info, data, row_bytes * h);
}

/* Reduce 24-bit RGB rows to one 8-bit channel in place (green). */
static void rgb_to_gray(uint8_t *data, uint32_t w, uint32_t h, uint32_t row_bytes) {
  uint32_t x, y;
  for (y = 0; y < h; ++y) {
    uint8_t *row = data + (size_t)y * row_bytes;
    for (x = 0; x < w; ++x) row[x] = row[3 * x + 1];
  }
}

static int xfer_memory(const TW_IMAGEINFO *info) {
  TW_SETUPMEMXFER setup;
  TW_IMAGEMEMXFER mx;
  TW_UINT32 buf_size;
  uint8_t *strip, *image = NULL;
  size_t image_rows = 0, row_bytes;
  uint32_t bpp = (uint32_t)info->BitsPerPixel, w = (uint32_t)info->ImageWidth;
  TW_UINT16 rc;
  int ok = 0;

  if (bpp != 8 && bpp != 16 && bpp != 24) {
    ta_send_error("pixel-format", "memory transfer: unsupported bits per pixel", 0);
    return 0;
  }
  memset(&setup, 0, sizeof setup);
  if (dsm(&g.src, DG_CONTROL, DAT_SETUPMEMXFER, MSG_GET, (TW_MEMREF)&setup) != TWRC_SUCCESS) {
    twain_error("memory transfer setup", &g.src, 0);
    return 0;
  }
  buf_size = setup.Preferred != TWON_DONTCARE32 ? setup.Preferred : setup.MinBufSize;
  if (buf_size < 4096) buf_size = 4096;
  strip = (uint8_t *)malloc(buf_size);
  row_bytes = (size_t)w * (bpp / 8);
  if (info->ImageLength > 0) {
    image_rows = (size_t)info->ImageLength;
    image = (uint8_t *)calloc(image_rows, row_bytes);
  }
  if (!strip || (info->ImageLength > 0 && !image)) {
    free(strip);
    free(image);
    ta_send_error("memory", "out of memory for the transfer", 0);
    return 0;
  }

  for (;;) {
    TW_UINT32 r;
    memset(&mx, 0, sizeof mx);
    mx.Compression = TWON_DONTCARE16;
    mx.BytesPerRow = TWON_DONTCARE32;
    mx.Columns = TWON_DONTCARE32;
    mx.Rows = TWON_DONTCARE32;
    mx.XOffset = TWON_DONTCARE32;
    mx.YOffset = TWON_DONTCARE32;
    mx.BytesWritten = TWON_DONTCARE32;
    mx.Memory.Flags = TWMF_APPOWNS | TWMF_POINTER;
    mx.Memory.Length = buf_size;
    mx.Memory.TheMem = (TW_MEMREF)strip;
    rc = dsm(&g.src, DG_IMAGE, DAT_IMAGEMEMXFER, MSG_GET, (TW_MEMREF)&mx);
    if (rc != TWRC_SUCCESS && rc != TWRC_XFERDONE) break;
    if (mx.Compression != TWCP_NONE) {
      ta_send_error("pixel-format", "the source sent compressed strips", 0);
      rc = TWRC_FAILURE;
      break;
    }
    if ((size_t)mx.YOffset + mx.Rows > image_rows) { /* unknown or wrong length: grow */
      size_t need = (size_t)mx.YOffset + mx.Rows;
      uint8_t *grown = (uint8_t *)realloc(image, need * row_bytes);
      if (!grown) {
        rc = TWRC_FAILURE;
        break;
      }
      memset(grown + image_rows * row_bytes, 0, (need - image_rows) * row_bytes);
      image = grown;
      image_rows = need;
    }
    for (r = 0; r < mx.Rows; ++r) {
      size_t off = (size_t)(mx.YOffset + r) * row_bytes + (size_t)mx.XOffset * (bpp / 8);
      size_t n = (size_t)mx.Columns * (bpp / 8);
      if (off + n > image_rows * row_bytes || (size_t)r * mx.BytesPerRow + n > buf_size) break;
      memcpy(image + off, strip + (size_t)r * mx.BytesPerRow, n);
    }
    if (rc == TWRC_XFERDONE) {
      ok = 1;
      break;
    }
  }
  if (ok) {
    if (bpp == 24) {
      rgb_to_gray(image, w, (uint32_t)image_rows, (uint32_t)row_bytes);
      ta_log("warn", "the source sent colour; using the green channel");
      bpp = 8;
    }
    send_raw(w, (uint32_t)image_rows, bpp, (uint32_t)row_bytes, 0, fix32(info->XResolution),
             fix32(info->YResolution), image);
  } else if (rc == TWRC_CANCEL) {
    ta_log("info", "the transfer was cancelled by the source");
  } else {
    twain_error("memory transfer", &g.src, 0);
  }
  free(strip);
  free(image);
  return ok;
}

static int xfer_native(const TW_IMAGEINFO *info) {
  TW_HANDLE h = NULL;
  TW_UINT16 rc = dsm(&g.src, DG_IMAGE, DAT_IMAGENATIVEXFER, MSG_GET, (TW_MEMREF)&h);
  BITMAPINFOHEADER *bi;
  const uint8_t *bits;
  uint32_t w, height, row_bytes, colours, flags = 0;
  int ok = 0;
  if (rc != TWRC_XFERDONE) {
    if (rc == TWRC_CANCEL) ta_log("info", "the transfer was cancelled by the source");
    else twain_error("native transfer", &g.src, 0);
    mem_free(h);
    return 0;
  }
  bi = (BITMAPINFOHEADER *)mem_lock(h);
  if (!bi) {
    mem_free(h);
    ta_send_error("memory", "could not lock the native image", 0);
    return 0;
  }
  w = (uint32_t)bi->biWidth;
  height = (uint32_t)(bi->biHeight < 0 ? -bi->biHeight : bi->biHeight);
  if (bi->biHeight > 0) flags |= TA_FRAME_BOTTOM_UP;
  row_bytes = ((w * bi->biBitCount + 31u) / 32u) * 4u;
  colours = bi->biClrUsed ? bi->biClrUsed : (bi->biBitCount <= 8 ? (1u << bi->biBitCount) : 0);
  bits = (const uint8_t *)bi + bi->biSize + colours * sizeof(RGBQUAD) +
         (bi->biCompression == BI_BITFIELDS ? 12 : 0);
  if (bi->biBitCount == 8 && colours >= 2) {
    const RGBQUAD *pal = (const RGBQUAD *)((const uint8_t *)bi + bi->biSize);
    if (pal[0].rgbGreen > pal[colours - 1].rgbGreen) flags |= TA_FRAME_MIN_IS_WHITE;
  }
  if (bi->biBitCount == 8 || bi->biBitCount == 16) {
    if (bi->biBitCount == 16) ta_log("info", "native 16-bit image treated as greyscale");
    send_raw(w, height, bi->biBitCount, row_bytes, flags, fix32(info->XResolution), fix32(info->YResolution), bits);
    ok = 1;
  } else if (bi->biBitCount == 24) {
    uint8_t *copy = (uint8_t *)malloc((size_t)row_bytes * height);
    if (copy) {
      memcpy(copy, bits, (size_t)row_bytes * height);
      rgb_to_gray(copy, w, height, row_bytes);
      ta_log("warn", "the source sent colour; using the green channel");
      send_raw(w, height, 8, row_bytes, flags, fix32(info->XResolution), fix32(info->YResolution), copy);
      free(copy);
      ok = 1;
    }
  } else {
    ta_send_error("pixel-format", "native transfer: unsupported bits per pixel", 0);
  }
  mem_unlock(h);
  mem_free(h);
  return ok;
}

static int xfer_file(void) {
  TW_SETUPFILEXFER fx;
  wchar_t dir[MAX_PATH], wname[MAX_PATH];
  TW_UINT16 rc;
  HANDLE f;
  LARGE_INTEGER size;
  uint8_t *data;
  DWORD got = 0;
  int ok = 0;
  if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"tqa", 0, wname)) {
    ta_send_error("file", "no temporary file for the transfer", 0);
    return 0;
  }
  /* TWAIN takes an ANSI path; the short form avoids characters it cannot hold. */
  GetShortPathNameW(wname, wname, MAX_PATH);
  memset(&fx, 0, sizeof fx);
  WideCharToMultiByte(CP_ACP, 0, wname, -1, (char *)fx.FileName, sizeof fx.FileName, NULL, NULL);
  fx.Format = TWFF_TIFF;
  if (dsm(&g.src, DG_CONTROL, DAT_SETUPFILEXFER, MSG_SET, (TW_MEMREF)&fx) != TWRC_SUCCESS) {
    twain_error("file transfer setup", &g.src, 0);
    DeleteFileW(wname);
    return 0;
  }
  rc = dsm(&g.src, DG_IMAGE, DAT_IMAGEFILEXFER, MSG_GET, NULL);
  if (rc != TWRC_XFERDONE) {
    if (rc == TWRC_CANCEL) ta_log("info", "the transfer was cancelled by the source");
    else twain_error("file transfer", &g.src, 0);
    DeleteFileW(wname);
    return 0;
  }
  f = CreateFileW(wname, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (f != INVALID_HANDLE_VALUE && GetFileSizeEx(f, &size) && size.QuadPart > 0 &&
      size.QuadPart < (LONGLONG)(TA_WIRE_MAX_PAYLOAD - TA_FRAME_INFO_SIZE)) {
    data = (uint8_t *)malloc((size_t)size.QuadPart);
    if (data && ReadFile(f, data, (DWORD)size.QuadPart, &got, NULL) && got == (DWORD)size.QuadPart) {
      ta_frame_info fi;
      uint8_t info[TA_FRAME_INFO_SIZE];
      memset(&fi, 0, sizeof fi);
      fi.container = TA_FRAME_TIFF;
      fi.index = g.index++;
      ta_frame_info_put(info, &fi);
      ta_send2(TA_MSG_FRAME, info, sizeof info, data, got);
      ok = 1;
    }
    free(data);
  }
  if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
  DeleteFileW(wname);
  if (!ok) ta_send_error("file", "could not read the transferred file", 0);
  return ok;
}

static void do_transfers(void) {
  TW_PENDINGXFERS pending;
  do {
    TW_IMAGEINFO info;
    memset(&info, 0, sizeof info);
    ta_send_state(TA_STATE_TRANSFERRING, NULL);
    if (dsm(&g.src, DG_IMAGE, DAT_IMAGEINFO, MSG_GET, (TW_MEMREF)&info) != TWRC_SUCCESS) {
      twain_error("reading the image description", &g.src, 0);
    } else if (g.mech == TWSX_MEMORY) {
      xfer_memory(&info);
    } else if (g.mech == TWSX_FILE) {
      xfer_file();
    } else {
      xfer_native(&info);
    }
    memset(&pending, 0, sizeof pending);
    if (dsm(&g.src, DG_CONTROL, DAT_PENDINGXFERS, MSG_ENDXFER, (TW_MEMREF)&pending) != TWRC_SUCCESS) {
      memset(&pending, 0, sizeof pending);
      dsm(&g.src, DG_CONTROL, DAT_PENDINGXFERS, MSG_RESET, (TW_MEMREF)&pending);
      break;
    }
  } while (pending.Count != 0);
  /* Still enabled (state 5): the source may raise another exposure. */
  if (g.enabled) ta_send_state(TA_STATE_ARMED, NULL);
}

static void on_ds_message(TW_UINT16 msg) {
  switch (msg) {
    case MSG_XFERREADY:
      if (g.enabled) do_transfers();
      break;
    case MSG_CLOSEDSREQ:
      disable_ds("the source asked to be closed");
      break;
    case MSG_CLOSEDSOK:
      disable_ds("the source finished");
      break;
    default:
      break;
  }
}

static int tw_event(ta_backend *b, void *p) {
  MSG *msg = (MSG *)p;
  TW_EVENT ev;
  TW_UINT16 rc;
  (void)b;
  if (msg->hwnd == NULL && msg->message == WM_TA_TWAIN) {
    on_ds_message((TW_UINT16)msg->wParam);
    return 1;
  }
  if (!g.ds_open || !g.enabled) return 0;
  /* Forward every message while enabled, callback or not: some sources still
     depend on it for their own windows and for MSG_XFERREADY. */
  ev.pEvent = (TW_MEMREF)msg;
  ev.TWMessage = MSG_NULL;
  rc = dsm(&g.src, DG_CONTROL, DAT_EVENT, MSG_PROCESSEVENT, (TW_MEMREF)&ev);
  if (ev.TWMessage != MSG_NULL) on_ds_message(ev.TWMessage);
  return rc == TWRC_DSEVENT;
}

static ta_backend g_backend = {"twain", NULL, tw_list, tw_open, tw_enable, tw_disable, tw_close, NULL, tw_event};

ta_backend *ta_twain_backend(const char *dsm_path) {
  g.dsm_override[0] = 0;
  if (dsm_path && dsm_path[0]) MultiByteToWideChar(CP_UTF8, 0, dsm_path, -1, g.dsm_override, MAX_PATH);
  return &g_backend;
}

#endif
