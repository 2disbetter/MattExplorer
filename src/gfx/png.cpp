#include "gfx/png.h"
#include "gfx/inflate.h"
#include "base/hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void image_free(Image& img) { free(img.px); img.px = nullptr; img.w = img.h = 0; }

static inline u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static const u8 kSig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

static inline u32 premul(u32 r, u32 g, u32 b, u32 a) {
  if (a == 255) return 0xFF000000u | (r << 16) | (g << 8) | b;
  if (a == 0) return 0;
  return (a << 24) | (((r * a + 127) / 255) << 16) | (((g * a + 127) / 255) << 8) | ((b * a + 127) / 255);
}

struct Chunk { u32 len; const u8* type; const u8* data; };

static bool next_chunk(const u8* d, u32 n, u32* pos, Chunk& c, bool check_crc) {
  if (*pos + 12 > n) return false;
  c.len = be32(d + *pos);
  if (c.len > n - *pos - 12) return false;
  c.type = d + *pos + 4;
  c.data = d + *pos + 8;
  if (check_crc) { u32 crc = crc32_update(0, c.type, c.len + 4); if (crc != be32(c.data + c.len)) return false; }
  *pos += 12 + c.len;
  return true;
}

bool png_info(const u8* data, u32 n, i32* w, i32* h) {
  if (n < 33 || memcmp(data, kSig, 8) || memcmp(data + 12, "IHDR", 4)) return false;
  *w = (i32)be32(data + 16); *h = (i32)be32(data + 20);
  return *w > 0 && *h > 0;
}

bool png_text(const u8* data, u32 n, const char* key, char* out, u32 cap) {
  if (n < 8 || memcmp(data, kSig, 8)) return false;
  u32 pos = 8; Chunk c;
  u32 kl = (u32)strlen(key);
  while (next_chunk(data, n, &pos, c, false)) {
    if (!memcmp(c.type, "IEND", 4)) break;
    if (memcmp(c.type, "tEXt", 4)) continue;
    if (c.len > kl && !memcmp(c.data, key, kl) && c.data[kl] == 0) {
      u32 vl = c.len - kl - 1;
      snprintf(out, cap, "%.*s", (int)vl, (const char*)c.data + kl + 1);
      return true;
    }
  }
  return false;
}

static inline u8 paeth(i32 a, i32 b, i32 c) {
  i32 p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
  return (u8)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

static bool unfilter(u8 type, u8* row, const u8* prev, u32 len, u32 bpp) {
  switch (type) {
    case 0: return true;
    case 1: for (u32 i = bpp; i < len; i++) row[i] = (u8)(row[i] + row[i - bpp]); return true;
    case 2: if (prev) for (u32 i = 0; i < len; i++) row[i] = (u8)(row[i] + prev[i]); return true;
    case 3: for (u32 i = 0; i < len; i++) { u32 a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0; row[i] = (u8)(row[i] + ((a + b) >> 1)); } return true;
    case 4: for (u32 i = 0; i < len; i++) { u32 a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= bpp) ? prev[i - bpp] : 0; row[i] = (u8)(row[i] + paeth((i32)a, (i32)b, (i32)c)); } return true;
    default: return false;
  }
}

struct PngHdr { u32 w, h; u8 depth, ctype, interlace; u32 channels, bpp_bits, bpp; };

static void expand_row(const PngHdr& H, const u8* row, u32 npx, u32* out, u32 x0, u32 dx, const u8* plte, u32 nplte, const u8* trns, u32 ntrns) {
  u32 d = H.depth;
  auto sample = [&](u32 i, u32 ch) -> u32 { // channel value scaled to 8 bits (16-bit: high byte)
    u32 idx = i * H.channels + ch;
    if (d == 8) return row[idx];
    if (d == 16) return row[2 * idx];
    u32 bit = idx * d, byte = bit >> 3, shift = 8 - d - (bit & 7);
    return (row[byte] >> shift) & ((1u << d) - 1);
  };
  auto sample16 = [&](u32 i, u32 ch) -> u32 { u32 idx = i * H.channels + ch; return d == 16 ? ((u32)row[2 * idx] << 8) | row[2 * idx + 1] : sample(i, ch); };
  u32 maxv = (1u << d) - 1;
  for (u32 i = 0; i < npx; i++) {
    u32 r, g, b, a = 255;
    switch (H.ctype) {
      case 0: { // grey
        u32 v = sample(i, 0);
        if (ntrns >= 2 && sample16(i, 0) == (((u32)trns[0] << 8) | trns[1])) a = 0;
        r = g = b = d < 8 ? v * 255 / maxv : v;
        break;
      }
      case 2: // RGB
        r = sample(i, 0); g = sample(i, 1); b = sample(i, 2);
        if (ntrns >= 6 && sample16(i, 0) == (((u32)trns[0] << 8) | trns[1]) && sample16(i, 1) == (((u32)trns[2] << 8) | trns[3]) && sample16(i, 2) == (((u32)trns[4] << 8) | trns[5])) a = 0;
        break;
      case 3: { // palette
        u32 v = sample(i, 0);
        if (v < nplte) { r = plte[3 * v]; g = plte[3 * v + 1]; b = plte[3 * v + 2]; } else r = g = b = 0;
        if (v < ntrns) a = trns[v];
        break;
      }
      case 4: { // grey + alpha
        u32 v = sample(i, 0); r = g = b = v; a = sample(i, 1);
        break;
      }
      default: // RGBA
        r = sample(i, 0); g = sample(i, 1); b = sample(i, 2); a = sample(i, 3);
        break;
    }
    out[x0 + i * dx] = premul(r, g, b, a);
  }
}

bool png_decode(const u8* data, u32 n, Image& out, u32 max_pixels, char* err, u32 ecap) {
  out.px = nullptr; out.w = out.h = 0;
  if (n < 8 || memcmp(data, kSig, 8)) { snprintf(err, ecap, "not a PNG"); return false; }
  u32 pos = 8; Chunk c;
  PngHdr H = {};
  bool have_hdr = false;
  u8 plte[768]; u32 nplte = 0;
  u8 trns[256]; u32 ntrns = 0;
  Array<u8> idat;
  while (next_chunk(data, n, &pos, c, true)) {
    if (!memcmp(c.type, "IHDR", 4)) {
      if (c.len != 13) { snprintf(err, ecap, "bad IHDR"); return false; }
      H.w = be32(c.data); H.h = be32(c.data + 4); H.depth = c.data[8]; H.ctype = c.data[9]; H.interlace = c.data[12];
      if (c.data[10] != 0 || c.data[11] != 0 || (H.interlace != 0 && H.interlace != 1)) { snprintf(err, ecap, "unsupported PNG method"); return false; }
      H.channels = H.ctype == 0 ? 1 : H.ctype == 2 ? 3 : H.ctype == 3 ? 1 : H.ctype == 4 ? 2 : H.ctype == 6 ? 4 : 0;
      bool depth_ok = H.depth == 1 || H.depth == 2 || H.depth == 4 || H.depth == 8 || H.depth == 16;
      if (!H.channels || !depth_ok || ((H.ctype == 2 || H.ctype == 4 || H.ctype == 6) && H.depth < 8) || (H.ctype == 3 && H.depth == 16)) { snprintf(err, ecap, "bad colour type / depth"); return false; }
      H.bpp_bits = H.channels * H.depth; H.bpp = (H.bpp_bits + 7) / 8;
      if (!H.w || !H.h || H.w > 65535 || H.h > 65535 || (u64)H.w * H.h > max_pixels) { snprintf(err, ecap, "image too large (%ux%u)", H.w, H.h); return false; }
      have_hdr = true;
    } else if (!memcmp(c.type, "PLTE", 4)) { nplte = c.len / 3; if (nplte > 256) nplte = 256; memcpy(plte, c.data, nplte * 3); }
    else if (!memcmp(c.type, "tRNS", 4)) { ntrns = c.len > 256 ? 256 : c.len; memcpy(trns, c.data, ntrns); }
    else if (!memcmp(c.type, "IDAT", 4)) memcpy(idat.push_n(c.len), c.data, c.len);
    else if (!memcmp(c.type, "IEND", 4)) break;
  }
  if (!have_hdr) { snprintf(err, ecap, "no IHDR"); return false; }
  if (!idat.len) { snprintf(err, ecap, "no image data"); return false; }

  struct Pass { u32 x0, y0, dx, dy; };
  static const Pass adam7[7] = { { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 } };
  const Pass single = { 0, 0, 1, 1 };
  u32 npass = H.interlace ? 7 : 1;
  u64 raw_size = 0;
  for (u32 p = 0; p < npass; p++) {
    const Pass& P = H.interlace ? adam7[p] : single;
    u32 pw = P.x0 < H.w ? (H.w - P.x0 + P.dx - 1) / P.dx : 0, ph = P.y0 < H.h ? (H.h - P.y0 + P.dy - 1) / P.dy : 0;
    if (pw && ph) raw_size += (u64)ph * (1 + ((u64)pw * H.bpp_bits + 7) / 8);
  }
  if (raw_size > (1u << 30)) { snprintf(err, ecap, "image too large"); return false; }
  u8* raw = (u8*)malloc((usize)raw_size);
  if (!raw) { snprintf(err, ecap, "out of memory"); return false; }
  u32 got = inflate(idat.data, idat.len, raw, (u32)raw_size, true, err, ecap);
  if (got != raw_size) { if (got) snprintf(err, ecap, "image data is short (%u of %llu bytes)", got, (unsigned long long)raw_size); free(raw); return false; }
  out.px = (u32*)malloc((usize)H.w * H.h * 4);
  if (!out.px) { free(raw); snprintf(err, ecap, "out of memory"); return false; }
  out.w = (i32)H.w; out.h = (i32)H.h;
  u8* p = raw;
  for (u32 pi = 0; pi < npass; pi++) {
    const Pass& P = H.interlace ? adam7[pi] : single;
    u32 pw = P.x0 < H.w ? (H.w - P.x0 + P.dx - 1) / P.dx : 0, ph = P.y0 < H.h ? (H.h - P.y0 + P.dy - 1) / P.dy : 0;
    if (!pw || !ph) continue;
    u32 rowbytes = (pw * H.bpp_bits + 7) / 8;
    u8* prev = nullptr;
    for (u32 y = 0; y < ph; y++) {
      u8 ftype = p[0]; u8* row = p + 1;
      if (!unfilter(ftype, row, prev, rowbytes, H.bpp)) { snprintf(err, ecap, "bad filter type %u", ftype); free(raw); image_free(out); return false; }
      expand_row(H, row, pw, out.px + (usize)(P.y0 + y * P.dy) * H.w, P.x0, P.dx, plte, nplte, trns, ntrns);
      prev = row;
      p += 1 + rowbytes;
    }
  }
  free(raw);
  return true;
}

static void put32(Array<u8>& o, u32 v) { o.push((u8)(v >> 24)); o.push((u8)(v >> 16)); o.push((u8)(v >> 8)); o.push((u8)v); }
static void chunk(Array<u8>& o, const char* type, const u8* data, u32 len) {
  put32(o, len);
  u32 start = o.len;
  memcpy(o.push_n(4), type, 4);
  if (len) memcpy(o.push_n(len), data, len);
  put32(o, crc32_update(0, o.data + start, len + 4));
}

void png_encode(const Image& img, const char* const* kv, u32 npairs, Array<u8>& out) {
  out.clear();
  memcpy(out.push_n(8), kSig, 8);
  u8 ihdr[13];
  ihdr[0] = (u8)(img.w >> 24); ihdr[1] = (u8)(img.w >> 16); ihdr[2] = (u8)(img.w >> 8); ihdr[3] = (u8)img.w;
  ihdr[4] = (u8)(img.h >> 24); ihdr[5] = (u8)(img.h >> 16); ihdr[6] = (u8)(img.h >> 8); ihdr[7] = (u8)img.h;
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  chunk(out, "IHDR", ihdr, 13);
  for (u32 i = 0; i < npairs; i++) {
    Array<u8> t;
    u32 kl = (u32)strlen(kv[2 * i]), vl = (u32)strlen(kv[2 * i + 1]);
    memcpy(t.push_n(kl), kv[2 * i], kl); t.push(0); memcpy(t.push_n(vl), kv[2 * i + 1], vl);
    chunk(out, "tEXt", t.data, t.len);
  }

  u32 rowbytes = 1 + (u32)img.w * 4;
  Array<u8> raw; raw.resize(rowbytes * (u32)img.h);
  for (i32 y = 0; y < img.h; y++) {
    u8* r = raw.data + (usize)y * rowbytes;
    r[0] = 0;
    for (i32 x = 0; x < img.w; x++) {
      u32 px = img.px[(usize)y * img.w + x];
      u32 a = px >> 24, rr = (px >> 16) & 255, gg = (px >> 8) & 255, bb = px & 255;
      if (a && a != 255) { rr = mx_min(rr * 255 / a, 255u); gg = mx_min(gg * 255 / a, 255u); bb = mx_min(bb * 255 / a, 255u); }
      r[1 + 4 * x] = (u8)rr; r[2 + 4 * x] = (u8)gg; r[3 + 4 * x] = (u8)bb; r[4 + 4 * x] = (u8)a;
    }
  }

  Array<u8> z;
  z.push(0x78); z.push(0x01);
  for (u32 off = 0; off < raw.len || off == 0;) {
    u32 len = mx_min(raw.len - off, 65535u);
    bool last = off + len >= raw.len;
    z.push(last ? 1 : 0);
    z.push((u8)len); z.push((u8)(len >> 8)); z.push((u8)~len); z.push((u8)(~len >> 8));
    if (len) memcpy(z.push_n(len), raw.data + off, len);
    off += len;
    if (last) break;
  }
  put32(z, adler32_update(1, raw.data, raw.len));
  chunk(out, "IDAT", z.data, z.len);
  chunk(out, "IEND", nullptr, 0);
}

void image_scale_down(const Image& src, i32 max_w, i32 max_h, Image& dst) {
  dst.px = nullptr; dst.w = dst.h = 0;
  if (src.w <= 0 || src.h <= 0 || max_w <= 0 || max_h <= 0) return;
  i32 w = src.w, h = src.h;
  if (w > max_w || h > max_h) {

    u64 nw = (u64)src.w * (u64)max_h, nh = (u64)src.h * (u64)max_w;
    if (nw > nh) { w = max_w; h = (i32)mx_max((u64)1, ((u64)src.h * (u64)max_w + (u64)src.w / 2) / (u64)src.w); }
    else { h = max_h; w = (i32)mx_max((u64)1, ((u64)src.w * (u64)max_h + (u64)src.h / 2) / (u64)src.h); }
    w = mx_min(w, max_w); h = mx_min(h, max_h);
  }
  dst.px = (u32*)malloc((usize)w * h * 4);
  if (!dst.px) return;
  dst.w = w; dst.h = h;
  if (w == src.w && h == src.h) { memcpy(dst.px, src.px, (usize)w * h * 4); return; }

  for (i32 y = 0; y < h; y++) {
    i32 sy0 = (i32)((i64)y * src.h / h), sy1 = (i32)((i64)(y + 1) * src.h / h); if (sy1 <= sy0) sy1 = sy0 + 1;
    for (i32 x = 0; x < w; x++) {
      i32 sx0 = (i32)((i64)x * src.w / w), sx1 = (i32)((i64)(x + 1) * src.w / w); if (sx1 <= sx0) sx1 = sx0 + 1;
      u64 a = 0, r = 0, g = 0, b = 0, cnt = 0;
      for (i32 sy = sy0; sy < sy1 && sy < src.h; sy++) {
        const u32* row = src.px + (usize)sy * src.w;
        for (i32 sx = sx0; sx < sx1 && sx < src.w; sx++) { u32 p = row[sx]; a += p >> 24; r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; cnt++; }
      }
      if (!cnt) cnt = 1;
      dst.px[(usize)y * w + x] = ((u32)(a / cnt) << 24) | ((u32)(r / cnt) << 16) | ((u32)(g / cnt) << 8) | (u32)(b / cnt);
    }
  }
}

void image_resample(const Image& src, i32 w, i32 h, Image& dst) {
  dst.px = nullptr; dst.w = dst.h = 0;
  if (src.w <= 0 || src.h <= 0 || w <= 0 || h <= 0) return;
  if (w < src.w || h < src.h) { image_scale_down(src, w, h, dst); if (dst.w == w && dst.h == h) return; image_free(dst); }
  dst.px = (u32*)malloc((usize)w * h * 4);
  if (!dst.px) return;
  dst.w = w; dst.h = h;
  for (i32 y = 0; y < h; y++) {
    float fy = ((float)y + 0.5f) * (float)src.h / (float)h - 0.5f;
    i32 y0 = (i32)fy; if (fy < 0) { fy = 0; y0 = 0; }
    i32 y1 = mx_min(y0 + 1, src.h - 1); float ty = fy - (float)y0;
    for (i32 x = 0; x < w; x++) {
      float fx = ((float)x + 0.5f) * (float)src.w / (float)w - 0.5f;
      i32 x0 = (i32)fx; if (fx < 0) { fx = 0; x0 = 0; }
      i32 x1 = mx_min(x0 + 1, src.w - 1); float tx = fx - (float)x0;
      u32 p00 = src.px[(usize)y0 * src.w + x0], p01 = src.px[(usize)y0 * src.w + x1], p10 = src.px[(usize)y1 * src.w + x0], p11 = src.px[(usize)y1 * src.w + x1];
      u32 out = 0;
      for (u32 sh = 0; sh < 32; sh += 8) {
        float v = ((float)((p00 >> sh) & 255) * (1 - tx) + (float)((p01 >> sh) & 255) * tx) * (1 - ty)
                + ((float)((p10 >> sh) & 255) * (1 - tx) + (float)((p11 >> sh) & 255) * tx) * ty;
        out |= (u32)mx_clamp((i32)(v + 0.5f), 0, 255) << sh;
      }
      dst.px[(usize)y * w + x] = out;
    }
  }
}

void image_orient(const Image& src, u32 o, Image& dst) {
  dst.px = nullptr; dst.w = dst.h = 0;
  if (src.w <= 0 || src.h <= 0) return;
  bool swap = o >= 5;
  dst.w = swap ? src.h : src.w; dst.h = swap ? src.w : src.h;
  dst.px = (u32*)malloc((usize)dst.w * dst.h * 4);
  if (!dst.px) { dst.w = dst.h = 0; return; }
  for (i32 y = 0; y < src.h; y++) for (i32 x = 0; x < src.w; x++) {
    i32 dx, dy;
    switch (o) {
      case 2: dx = src.w - 1 - x; dy = y; break; // mirrored
      case 3: dx = src.w - 1 - x; dy = src.h - 1 - y; break; // 180
      case 4: dx = x; dy = src.h - 1 - y; break; // flipped
      case 5: dx = y; dy = x; break; // transposed
      case 6: dx = src.h - 1 - y; dy = x; break; // 90 clockwise
      case 7: dx = src.h - 1 - y; dy = src.w - 1 - x; break; // transversed
      case 8: dx = y; dy = src.w - 1 - x; break; // 90 counter-clockwise
      default: dx = x; dy = y; break;
    }
    dst.px[(usize)dy * dst.w + dx] = src.px[(usize)y * src.w + x];
  }
}
