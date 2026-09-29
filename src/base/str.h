#pragma once

#include "base/types.h"
#include <string.h>

struct Str {
  const char* p = nullptr;
  u32         n = 0;

  Str() = default;
  Str(const char* s) : p(s), n(s ? (u32)strlen(s) : 0) {}
  Str(const char* s, u32 len) : p(s), n(len) {}

  char operator[](u32 i) const { MX_ASSERT(i < n); return p[i]; }
  bool empty() const { return n == 0; }
  Str  sub(u32 from, u32 count) const { MX_ASSERT(from + count <= n); return Str(p + from, count); }
};

static inline bool is_digit(char c)     { return c >= '0' && c <= '9'; }
static inline bool is_upper(char c)     { return c >= 'A' && c <= 'Z'; }
static inline bool is_lower(char c)     { return c >= 'a' && c <= 'z'; }
static inline char ascii_lower(char c)  { return is_upper(c) ? (char)(c + 32) : c; }

static inline bool str_eq(Str a, Str b) { return a.n == b.n && memcmp(a.p, b.p, a.n) == 0; }

static inline int str_cmp(Str a, Str b) {
  u32 m = a.n < b.n ? a.n : b.n;
  int r = memcmp(a.p, b.p, m);
  if (r) return r;
  return (a.n > b.n) - (a.n < b.n);
}

static inline Str str_extension(Str name) {
  for (u32 i = name.n; i > 1; i--) {
    if (name.p[i - 1] == '.') return Str(name.p + i, name.n - i);
  }
  return Str();
}

static inline u32 utf8_next(Str s, u32& i) {
  u8 c = (u8)s.p[i++];
  if (c < 0x80) return c;
  u32 n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  if (!n || c > 0xF4 || i + n > s.n) return 0xFFFD;
  u32 cp = c & (0x3F >> n);
  for (u32 k = 0; k < n; k++) {
    u8 t = (u8)s.p[i + k];
    if ((t & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (t & 0x3F);
  }
  static const u32 min_cp[4] = { 0, 0x80, 0x800, 0x10000 };
  if (cp < min_cp[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
  i += n;
  return cp;
}
static inline u32 utf8_put(u32 cp, char out[4]) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
  if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
  if (cp > 0x10FFFF) return 0;
  out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

static inline bool str_ieq(Str a, Str b) {
  if (a.n != b.n) return false;
  for (u32 i = 0; i < a.n; i++) if (ascii_lower(a.p[i]) != ascii_lower(b.p[i])) return false;
  return true;
}
