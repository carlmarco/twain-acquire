/*
 * key=value text, one pair per line. Used for wire payloads and for every
 * settings, profile and queue file, so there is exactly one parser.
 *
 * Keys are [A-Za-z0-9._-]+. Values are UTF-8 with '\\', '\n', '\r' and '\t'
 * escaped as "\\\\", "\\n", "\\r" and "\\t". Blank lines and lines starting
 * with '#' are ignored.
 */
#ifndef TA_KV_H
#define TA_KV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes needed to escape `n` bytes of `value` (excluding any terminator). */
size_t ta_kv_escaped_len(const char *value, size_t n);

/* Escape into out (no terminator written). Returns bytes written. `out` must
   hold ta_kv_escaped_len(value, n) bytes. */
size_t ta_kv_escape(const char *value, size_t n, char *out);

/* Unescape in place semantics: writes at most n bytes to out. Returns the
   length written, or (size_t)-1 on a malformed escape. */
size_t ta_kv_unescape(const char *in, size_t n, char *out);

/* 1 when the key is non-empty and uses only the allowed characters. */
int ta_kv_valid_key(const char *key, size_t n);

/* Append "key=value\n". Returns 0 (and leaves *len unchanged) when it does
   not fit in cap or the key is invalid. */
int ta_kv_append(char *buf, size_t cap, size_t *len, const char *key,
                 const char *value);

/*
 * Read the next pair starting at *pos. On success returns 1, writes the key
 * and unescaped value as NUL-terminated strings, and advances *pos past the
 * line. Returns 0 at the end of text and -1 on a malformed or oversize line
 * (*pos still advances, so a caller may skip it).
 */
int ta_kv_next(const char *text, size_t len, size_t *pos, char *key,
               size_t kcap, char *value, size_t vcap);

/* Find `key` and copy its value. Returns 1 when found. */
int ta_kv_find(const char *text, size_t len, const char *key, char *value,
               size_t vcap);

#ifdef __cplusplus
}
#endif

#endif
