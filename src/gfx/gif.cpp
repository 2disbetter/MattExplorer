#include "gfx/gif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline u32 le16(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8); }

bool gif_info(const u8* data, u32 n, i32* w, i32* h) {
  if (n < 13 || (memcmp(data, "GIF87a", 6) && memcmp(data, "GIF89a", 6))) return false;
  *w = (i32)le16(data + 6); *h = (i32)le16(data + 8);
  return *w > 0 && *h > 0;
}

namespace {

bool lzw_decode(const u8* d, u32 n, u32* pos, u32 min_code, u8* out, u32 count) {
  if (min_code < 2 || min_code > 11) return false;
  enum { MAX_CODES = 4096 };
  static_assert(MAX_CODES == 4096, "");
  u16* prefix = (u16*)malloc(MAX_CODES * sizeof(u16));
  u8*  suffix = (u8*)malloc(MAX_CODES);
  u8*  stack  = (u8*)malloc(MAX_CODES + 1);
  if (!prefix || !suffix || !stack) { free(prefix); free(suffix); free(stack); return false; }
  u32 clear = 1u << min_code, eoi = clear + 1;
  u32 code_size = min_code + 1, next = clear + 2, limit = 1u << code_size;
  for (u32 i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = (u8)i; }
  u32 acc = 0, nbits = 0, o = 0, prev = 0xFFFF; u8 first = 0;
  bool ok = true, done = false;

  u32 sub_left = 0;
  auto next_byte = [&](u32* b) -> bool {
    while (sub_left == 0) { if (*pos >= n) return false; sub_left = d[(*pos)++]; if (sub_left == 0) return false; } // a zero block ends the data
    if (*pos >= n) return false;
    *b = d[(*pos)++]; sub_left--;
    return true;
  };
  while (!done && o < count) {
    while (nbits < code_size) { u32 b; if (!next_byte(&b)) { done = true; break; } acc |= b << nbits; nbits += 8; }
    if (nbits < code_size) break;
    u32 code = acc & (limit - 1); acc >>= code_size; nbits -= code_size;
    if (code == clear) { code_size = min_code + 1; limit = 1u << code_size; next = clear + 2; prev = 0xFFFF; continue; }
    if (code == eoi) { done = true; break; }
    u32 c = code; u32 sp = 0;
    if (code >= next) { // KwKwK: the code being defined
      if (code != next || prev == 0xFFFF) { ok = false; break; }
      stack[sp++] = first; c = prev;
    }
    while (c != 0xFFFF && c >= clear) { if (sp > MAX_CODES) { ok = false; break; } stack[sp++] = suffix[c]; c = prefix[c]; }
    if (!ok) break;
    if (c == 0xFFFF) { ok = false; break; }
    stack[sp++] = suffix[c]; first = suffix[c];
    while (sp && o < count) out[o++] = stack[--sp];
    if (prev != 0xFFFF && next < MAX_CODES) {
      prefix[next] = (u16)prev; suffix[next] = first; next++;
      if (next == limit && code_size < 12) { code_size++; limit = 1u << code_size; }
    }
    prev = code;
  }

  { u32 b; while (sub_left && next_byte(&b)) {} while (*pos < n && d[*pos] != 0) { u32 l = d[*pos]; *pos += 1 + l; } if (*pos < n) (*pos)++; }
  free(prefix); free(suffix); free(stack);
  return ok || o > 0;
}

} // namespace

bool gif_decode(const u8* data, u32 n, Image& out, u32 max_pixels, char* err, u32 ecap) {
  out.px = nullptr; out.w = out.h = 0;
  i32 sw, sh;
  if (!gif_info(data, n, &sw, &sh)) { snprintf(err, ecap, "not a GIF"); return false; }
  if ((u64)sw * sh > max_pixels) { snprintf(err, ecap, "image too large"); return false; }
  u32 packed = data[10];
  u32 pos = 13;
  const u8* gct = nullptr; u32 gct_n = 0;
  if (packed & 0x80) { gct_n = 2u << (packed & 7); if (pos + 3 * gct_n > n) { snprintf(err, ecap, "truncated"); return false; } gct = data + pos; pos += 3 * gct_n; }
  i32 transparent = -1;
  while (pos < n) {
    u8 b = data[pos++];
    if (b == 0x3B) break; // trailer
    if (b == 0x21) { // extension
      if (pos >= n) break;
      u8 label = data[pos++];
      if (label == 0xF9 && pos + 5 <= n && data[pos] == 4) { if (data[pos + 1] & 1) transparent = data[pos + 4]; }
      while (pos < n && data[pos] != 0) { u32 l = data[pos]; pos += 1 + l; } // sub-blocks
      pos++;
      continue;
    }
    if (b != 0x2C) { snprintf(err, ecap, "unexpected block 0x%02x", b); return false; }
    if (pos + 9 > n) { snprintf(err, ecap, "truncated"); return false; }
    u32 left = le16(data + pos), top = le16(data + pos + 2), fw = le16(data + pos + 4), fh = le16(data + pos + 6);
    u32 fp = data[pos + 8]; pos += 9;
    const u8* ct = gct; u32 ct_n = gct_n;
    if (fp & 0x80) { ct_n = 2u << (fp & 7); if (pos + 3 * ct_n > n) { snprintf(err, ecap, "truncated"); return false; } ct = data + pos; pos += 3 * ct_n; }
    bool interlaced = (fp & 0x40) != 0;
    if (!fw || !fh || !ct) { snprintf(err, ecap, "bad frame"); return false; }
    if (pos >= n) { snprintf(err, ecap, "truncated"); return false; }
    u32 min_code = data[pos++];
    u8* idx = (u8*)malloc((usize)fw * fh);
    if (!idx) { snprintf(err, ecap, "out of memory"); return false; }
    memset(idx, (u8)(transparent >= 0 ? transparent : 0), (usize)fw * fh);
    if (!lzw_decode(data, n, &pos, min_code, idx, fw * fh)) { free(idx); snprintf(err, ecap, "bad LZW data"); return false; }
    out.w = sw; out.h = sh;
    out.px = (u32*)calloc((usize)sw * sh, 4);
    if (!out.px) { free(idx); snprintf(err, ecap, "out of memory"); return false; }

    u32 src_row = 0;
    static const u32 starts[4] = { 0, 4, 2, 1 }, steps[4] = { 8, 8, 4, 2 };
    for (u32 pass = 0; pass < (interlaced ? 4u : 1u); pass++) {
      for (u32 y = interlaced ? starts[pass] : 0; y < fh; y += interlaced ? steps[pass] : 1, src_row++) {
        u32 oy = top + y; if (oy >= (u32)sh) continue;
        const u8* srow = idx + (usize)src_row * fw;
        u32* orow = out.px + (usize)oy * sw;
        for (u32 x = 0; x < fw; x++) {
          u32 ox = left + x; if (ox >= (u32)sw) break;
          u32 i = srow[x];
          if ((i32)i == transparent || i >= ct_n) continue;
          orow[ox] = 0xFF000000u | ((u32)ct[3 * i] << 16) | ((u32)ct[3 * i + 1] << 8) | ct[3 * i + 2];
        }
      }
    }
    free(idx);
    return true; // the first frame is the thumbnail
  }
  snprintf(err, ecap, "no image in the GIF");
  return false;
}
