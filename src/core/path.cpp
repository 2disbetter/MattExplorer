#include "core/path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void path_normalize(const char* in, char* out, u32 cap) {
  if (in[0] == '~' && (in[1] == 0 || in[1] == '/')) {
    const char* home = getenv("HOME");
    snprintf(out, cap, "%s%s", home ? home : "", in + 1);
  } else if (in[0] != '/') {
    char cwd[4096];
    snprintf(out, cap, "%s/%s", getcwd(cwd, sizeof cwd) ? cwd : "", in);
  } else {
    snprintf(out, cap, "%s", in);
  }
  char* w = out; const char* r = out;
  while (*r) {
    if (*r == '/') {
      while (*r == '/') r++;
      if (r[0] == '.' && (r[1] == '/' || r[1] == 0)) { r++; continue; }
      if (r[0] == '.' && r[1] == '.' && (r[2] == '/' || r[2] == 0)) {
        r += 2;
        while (w > out && *(w - 1) != '/') w--;
        if (w > out) w--;
        continue;
      }
      *w++ = '/';
      continue;
    }
    *w++ = *r++;
  }
  if (w > out + 1 && w[-1] == '/') w--;
  *w = 0;
  if (!out[0]) { out[0] = '/'; out[1] = 0; }
}

const char* path_base(const char* path) {
  const char* base = strrchr(path, '/');
  return (base && base[1]) ? base + 1 : path;
}
