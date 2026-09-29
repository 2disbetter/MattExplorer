#include "core/format.h"
#include <stdio.h>
#include <time.h>

const char* fmt_size(u64 v, char* b, u32 cap) {
  if (v < 1024) { snprintf(b, cap, "%llu B", (unsigned long long)v); return b; }
  static const char* unit[] = { "KB", "MB", "GB", "TB", "PB" };
  double d = (double)v / 1024.0; u32 u = 0;
  while (d >= 1024.0 && u < 4) { d /= 1024.0; u++; }
  if (d < 10.0) snprintf(b, cap, "%.1f %s", d, unit[u]);
  else snprintf(b, cap, "%.0f %s", d, unit[u]);
  return b;
}

const char* fmt_time(i64 ns, char* b, u32 cap) {
  time_t t = (time_t)(ns / 1000000000LL);
  struct tm tm;
  if (!localtime_r(&t, &tm)) { b[0] = 0; return b; }
  strftime(b, cap, "%Y-%m-%d %H:%M", &tm);
  return b;
}

const char* fmt_count(u64 v, char* b, u32 cap) {
  char tmp[32];
  int n = snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v);
  u32 o = 0;
  for (int i = 0; i < n && o + 1 < cap; i++) {
    if (i && (n - i) % 3 == 0 && o + 1 < cap) b[o++] = ',';
    b[o++] = tmp[i];
  }
  b[o] = 0;
  return b;
}
