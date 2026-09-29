#include "gfx/thumb.h"
#include "gfx/jpeg.h"
#include "gfx/gif.h"
#include "base/hash.h"
#include "base/str.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const u32 kMaxSourceBytes = 96u << 20; // a source file larger than this is not decoded
static const u32 kMaxSourcePixels = 80u << 20; // nor one this large once decoded

u32 thumb_uri(const char* path, char* out, u32 cap) {
  static const char hex[] = "0123456789ABCDEF";
  u32 o = 0;
  auto put = [&](char c) { if (o + 1 < cap) out[o++] = c; };
  for (const char* p = "file://"; *p; p++) put(*p);
  for (const u8* p = (const u8*)path; *p; p++) {
    u8 c = *p;

    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' ||
              c == '/' || c == '!' || c == '$' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '*' || c == '+' || c == ',' || c == ':' || c == ';' || c == '=' || c == '@';
    if (ok) put((char)c); else { put('%'); put(hex[c >> 4]); put(hex[c & 15]); }
  }
  if (cap) out[o] = 0;
  return o;
}

bool thumb_cache_dir(char* out, u32 cap) {
  const char* x = getenv("XDG_CACHE_HOME");
  if (x && x[0]) { snprintf(out, cap, "%s/thumbnails", x); return true; }
  const char* home = getenv("HOME");
  if (!home || !home[0]) return false;
  snprintf(out, cap, "%s/.cache/thumbnails", home);
  return true;
}

bool thumb_candidate_name(const char* name) {
  Str ext = str_extension(name);
  return str_ieq(ext, "png") || str_ieq(ext, "jpg") || str_ieq(ext, "jpeg") || str_ieq(ext, "jpe") || str_ieq(ext, "jfif") || str_ieq(ext, "gif");
}

static bool read_all(const char* path, Array<u8>& out, u32 max, char* err, u32 ecap) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (fd < 0) { snprintf(err, ecap, "%s", strerror(errno)); return false; }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); snprintf(err, ecap, "not a regular file"); return false; }
  if ((u64)st.st_size > max) { close(fd); snprintf(err, ecap, "file too large"); return false; }
  out.clear(); out.reserve((u32)st.st_size + 1);
  for (;;) {
    u8 b[65536];
    ssize_t n = read(fd, b, sizeof b);
    if (n < 0) { if (errno == EINTR) continue; close(fd); snprintf(err, ecap, "%s", strerror(errno)); return false; }
    if (n == 0) break;
    if (out.len + (u32)n > max) { close(fd); snprintf(err, ecap, "file too large"); return false; }
    memcpy(out.push_n((u32)n), b, (u32)n);
  }
  close(fd);
  return true;
}

static bool mkdir_p(const char* dir) {
  char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", dir);
  for (char* p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0700); *p = '/'; }
  return mkdir(tmp, 0700) == 0 || errno == EEXIST;
}

static bool write_atomic(const char* dir, const char* name, const u8* data, u32 n) {
  if (!mkdir_p(dir)) return false;
  char tmp[1200], final[1200];
  snprintf(tmp, sizeof tmp, "%s/.mattexplorer-XXXXXX", dir);
  snprintf(final, sizeof final, "%s/%s", dir, name);
  int fd = mkstemp(tmp);
  if (fd < 0) return false;
  fchmod(fd, 0600);
  bool ok = true;
  for (u32 o = 0; o < n && ok;) { ssize_t k = write(fd, data + o, n - o); if (k < 0) { if (errno == EINTR) continue; ok = false; } else o += (u32)k; }
  if (close(fd) != 0) ok = false;
  if (ok && rename(tmp, final) != 0) ok = false;
  if (!ok) unlink(tmp);
  return ok;
}

static bool cache_read(const char* file, i64 mtime, Image& out) {
  Array<u8> data; char err[128];
  if (!read_all(file, data, 16u << 20, err, sizeof err)) return false;
  char mt[64];
  if (!png_text(data.data, data.len, "Thumb::MTime", mt, sizeof mt) || strtoll(mt, nullptr, 10) != mtime) return false;
  return png_decode(data.data, data.len, out, 4u << 20, err, sizeof err);
}

static bool fail_marked(const char* file, i64 mtime) {
  Array<u8> data; char err[128], mt[64];
  if (!read_all(file, data, 1u << 20, err, sizeof err)) return false;
  return png_text(data.data, data.len, "Thumb::MTime", mt, sizeof mt) && strtoll(mt, nullptr, 10) == mtime;
}

bool thumb_get(const char* path, i64 mtime, u64 size, u32 want, const char* app_dir, Image& out, char* err, u32 ecap, bool* from_cache) {
  out.px = nullptr; out.w = out.h = 0;
  if (from_cache) *from_cache = true;
  char uri[4096 * 3], key[33], cache[1024], file[1200];
  thumb_uri(path, uri, sizeof uri);
  md5_hex(uri, strlen(uri), key);
  bool have_cache = thumb_cache_dir(cache, sizeof cache);
  if (have_cache) {

    snprintf(file, sizeof file, "%s/large/%s.png", cache, key);
    if (cache_read(file, mtime, out)) goto done;
    if (want <= THUMB_NORMAL) { snprintf(file, sizeof file, "%s/normal/%s.png", cache, key); if (cache_read(file, mtime, out)) goto done; }
    snprintf(file, sizeof file, "%s/fail/%s/%s.png", cache, app_dir, key);
    if (fail_marked(file, mtime)) { snprintf(err, ecap, "marked as failed before"); return false; }
  }
  {
    if (from_cache) *from_cache = false;
    Array<u8> data;
    if (!read_all(path, data, kMaxSourceBytes, err, ecap)) return false;
    Image full;
    bool ok = false;
    i32 w = 0, h = 0;
    if (png_info(data.data, data.len, &w, &h)) ok = png_decode(data.data, data.len, full, kMaxSourcePixels, err, ecap);
    else {
      bool prog; u32 orient;
      if (jpeg_info(data.data, data.len, &w, &h, &prog, &orient)) {
        u32 denom = (w >= 8 * THUMB_LARGE && h >= 8 * THUMB_LARGE) ? 8 : 1;
        ok = jpeg_decode(data.data, data.len, full, denom, kMaxSourcePixels, err, ecap);
      } else if (gif_info(data.data, data.len, &w, &h)) ok = gif_decode(data.data, data.len, full, kMaxSourcePixels, err, ecap);
      else snprintf(err, ecap, "not a PNG, JPEG or GIF");
    }
    data.release();
    if (!ok) {
      if (have_cache && app_dir) { // a marker so nobody decodes it again until it changes
        Image dot; dot.w = dot.h = 1; u32 px = 0; dot.px = &px;
        char mt[32]; snprintf(mt, sizeof mt, "%lld", (long long)mtime);
        const char* kv[4] = { "Thumb::URI", uri, "Thumb::MTime", mt };
        Array<u8> png; png_encode(dot, kv, 2, png);
        char dir[1200]; snprintf(dir, sizeof dir, "%s/fail/%s", cache, app_dir);
        char name[64]; snprintf(name, sizeof name, "%s.png", key);
        write_atomic(dir, name, png.data, png.len);
      }
      return false;
    }
    image_scale_down(full, THUMB_LARGE, THUMB_LARGE, out);
    image_free(full);
    if (!out.px) { snprintf(err, ecap, "out of memory"); return false; }
    if (have_cache) {
      char mt[32], sz[32], iw[16], ih[16];
      snprintf(mt, sizeof mt, "%lld", (long long)mtime); snprintf(sz, sizeof sz, "%llu", (unsigned long long)size);
      snprintf(iw, sizeof iw, "%d", w); snprintf(ih, sizeof ih, "%d", h);
      const char* kv[12] = { "Thumb::URI", uri, "Thumb::MTime", mt, "Thumb::Size", sz, "Thumb::Image::Width", iw, "Thumb::Image::Height", ih, "Software", "MattExplorer" };
      Array<u8> png; png_encode(out, kv, 6, png);
      char dir[1200]; snprintf(dir, sizeof dir, "%s/large", cache);
      char name[64]; snprintf(name, sizeof name, "%s.png", key);
      write_atomic(dir, name, png.data, png.len);
    }
  }
done:
  if ((u32)out.w > want || (u32)out.h > want) { Image small; image_scale_down(out, (i32)want, (i32)want, small); image_free(out); out = small; }
  return out.px != nullptr;
}
