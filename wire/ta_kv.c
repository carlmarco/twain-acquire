#include "ta_kv.h"

#include <string.h>

size_t ta_kv_escaped_len(const char *value, size_t n) {
  size_t i, out = 0;
  for (i = 0; i < n; ++i) {
    char c = value[i];
    out += (c == '\\' || c == '\n' || c == '\r' || c == '\t') ? 2 : 1;
  }
  return out;
}

size_t ta_kv_escape(const char *value, size_t n, char *out) {
  size_t i, o = 0;
  for (i = 0; i < n; ++i) {
    char c = value[i];
    switch (c) {
      case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
      case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
      case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
      case '\t': out[o++] = '\\'; out[o++] = 't'; break;
      default: out[o++] = c;
    }
  }
  return o;
}

size_t ta_kv_unescape(const char *in, size_t n, char *out) {
  size_t i, o = 0;
  for (i = 0; i < n; ++i) {
    char c = in[i];
    if (c != '\\') {
      out[o++] = c;
      continue;
    }
    if (++i == n) return (size_t)-1;
    switch (in[i]) {
      case '\\': out[o++] = '\\'; break;
      case 'n': out[o++] = '\n'; break;
      case 'r': out[o++] = '\r'; break;
      case 't': out[o++] = '\t'; break;
      default: return (size_t)-1;
    }
  }
  return o;
}

int ta_kv_valid_key(const char *key, size_t n) {
  size_t i;
  if (n == 0) return 0;
  for (i = 0; i < n; ++i) {
    char c = key[i];
    int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return 0;
  }
  return 1;
}

int ta_kv_append(char *buf, size_t cap, size_t *len, const char *key,
                 const char *value) {
  size_t kn = strlen(key), vn = strlen(value);
  size_t need = kn + 1 + ta_kv_escaped_len(value, vn) + 1;
  size_t o = *len;
  if (!ta_kv_valid_key(key, kn) || o > cap || need > cap - o) return 0;
  memcpy(buf + o, key, kn);
  o += kn;
  buf[o++] = '=';
  o += ta_kv_escape(value, vn, buf + o);
  buf[o++] = '\n';
  *len = o;
  return 1;
}

int ta_kv_next(const char *text, size_t len, size_t *pos, char *key,
               size_t kcap, char *value, size_t vcap) {
  while (*pos < len) {
    size_t start = *pos, end = start, eq, vlen;
    while (end < len && text[end] != '\n') ++end;
    *pos = end < len ? end + 1 : end;
    if (end > start && text[end - 1] == '\r') --end; /* tolerate CRLF files */
    if (end == start || text[start] == '#') continue;

    for (eq = start; eq < end && text[eq] != '='; ++eq) {
    }
    if (eq == end || !ta_kv_valid_key(text + start, eq - start)) return -1;
    if (eq - start + 1 > kcap) return -1;
    if (end - eq - 1 + 1 > vcap) return -1;
    memcpy(key, text + start, eq - start);
    key[eq - start] = '\0';
    vlen = ta_kv_unescape(text + eq + 1, end - eq - 1, value);
    if (vlen == (size_t)-1) return -1;
    value[vlen] = '\0';
    return 1;
  }
  return 0;
}

int ta_kv_find(const char *text, size_t len, const char *key, char *value,
               size_t vcap) {
  char k[128];
  size_t pos = 0;
  int r;
  while ((r = ta_kv_next(text, len, &pos, k, sizeof k, value, vcap)) != 0) {
    if (r == 1 && strcmp(k, key) == 0) return 1;
  }
  return 0;
}
