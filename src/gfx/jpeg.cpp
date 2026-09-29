#include "gfx/jpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const u8 kZigzag[64] = {
  0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

struct HuffTab {
  u16 fast16[512];
  u32 maxcode[18], valptr[17];
  i32 mincode[17];
  u8  vals[256];
  bool present;
};

struct Component { u32 id, h, v, tq, td, ta; u32 bw, bh; u8* plane; u32 pw, ph; i32 dc_pred; i16* coef; u32 cbw, cbh; };

struct ScanInfo { u32 ncomp; u32 comp[4]; u32 ss, se, ah, al; };

struct Reader {
  const u8* p; const u8* end;
  u32 acc; u32 n; // bit buffer
  u32 marker; // a marker hit inside entropy data (0 none)
  u32 pad_bits;
};

inline u32 be16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }

inline void fill(Reader& r) {
  while (r.n <= 24) {
    u32 byte = 0;
    bool pad = true;
    if (!r.marker && r.p < r.end) {
      byte = *r.p++; pad = false;
      if (byte == 0xFF) {
        u32 nx = r.p < r.end ? *r.p : 0xD9;
        if (nx == 0) r.p++; // stuffed 0xFF
        else { r.marker = nx; byte = 0; pad = true; } // a marker (RSTn, EOI, ...): only zeros from here
      }
    }
    if (pad) r.pad_bits += 8;
    r.acc |= byte << (24 - r.n);
    r.n += 8;
  }
}
inline u32 getbits(Reader& r, u32 k) { if (!k) return 0; if (r.n < k) fill(r); u32 v = r.acc >> (32 - k); r.acc <<= k; r.n -= k; return v; }
inline u32 getbit(Reader& r) { return getbits(r, 1); }

bool huff_build(HuffTab& t, const u8* counts, const u8* vals, u32 nvals) {
  memset(t.fast16, 0, sizeof t.fast16);
  memcpy(t.vals, vals, nvals);
  u32 code = 0, k = 0;
  for (u32 len = 1; len <= 16; len++) {
    t.valptr[len] = k; t.mincode[len] = (i32)code;
    for (u32 i = 0; i < counts[len - 1]; i++, k++) {
      if (len <= 9) { u32 base = code << (9 - len); for (u32 f = 0; f < (1u << (9 - len)); f++) t.fast16[base | f] = (u16)((len << 8) | vals[k]); }
      code++;
    }
    t.maxcode[len] = counts[len - 1] ? code - 1 : 0xFFFFFFFFu; // no codes of this length
    if (!counts[len - 1]) t.maxcode[len] = 0xFFFFFFFFu;
    code <<= 1;
    if (code > (2u << len)) return false;
  }
  t.maxcode[17] = 0xFFFFFFFFu;
  t.present = true;
  return true;
}

inline i32 huff_decode(Reader& r, const HuffTab& t) {
  if (r.n < 16) fill(r);
  u32 look = r.acc >> 23;
  u16 e = t.fast16[look];
  if (e) { u32 len = e >> 8; r.acc <<= len; r.n -= len; return e & 255; }
  u32 code = r.acc >> 22; r.acc <<= 10; r.n -= 10; // 10 bits consumed
  for (u32 len = 10; len <= 16; len++) {
    if (t.maxcode[len] != 0xFFFFFFFFu && code <= t.maxcode[len]) return t.vals[t.valptr[len] + (code - (u32)t.mincode[len])];
    code = (code << 1) | getbit(r);
  }
  return -1;
}

inline i32 extend(u32 v, u32 t) { return t == 0 ? 0 : (v < (1u << (t - 1)) ? (i32)v - (i32)(1u << t) + 1 : (i32)v); }

#define F2F(x) ((i32)((x) * 4096 + 0.5))
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                                   \
  i32 t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;                       \
  p2 = s2; p3 = s6; p1 = (p2 + p3) * F2F(0.5411961f);                           \
  t2 = p1 + p3 * F2F(-1.847759065f); t3 = p1 + p2 * F2F(0.765366865f);          \
  p2 = s0; p3 = s4; t0 = (p2 + p3) * 4096; t1 = (p2 - p3) * 4096;               \
  x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2;                       \
  t0 = s7; t1 = s5; t2 = s3; t3 = s1;                                            \
  p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2;                        \
  p5 = (p3 + p4) * F2F(1.175875602f);                                            \
  t0 = t0 * F2F(0.298631336f); t1 = t1 * F2F(2.053119869f);                      \
  t2 = t2 * F2F(3.072711026f); t3 = t3 * F2F(1.501321110f);                      \
  p1 = p5 + p1 * F2F(-0.899976223f); p2 = p5 + p2 * F2F(-2.562915447f);          \
  p3 = p3 * F2F(-1.961570560f); p4 = p4 * F2F(-0.390180644f);                    \
  t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

inline u8 clamp8(i32 v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

void idct_block(u8* out, u32 stride, const i32 in[64]) {
  i32 val[64];
  for (u32 i = 0; i < 8; i++) {
    const i32* d = in + i; i32* v = val + i;
    if (!d[8] && !d[16] && !d[24] && !d[32] && !d[40] && !d[48] && !d[56]) { i32 dc = d[0] * 4; v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc; continue; }
    IDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
    x0 += 512; x1 += 512; x2 += 512; x3 += 512;
    v[0] = (x0 + t3) >> 10; v[56] = (x0 - t3) >> 10; v[8] = (x1 + t2) >> 10; v[48] = (x1 - t2) >> 10;
    v[16] = (x2 + t1) >> 10; v[40] = (x2 - t1) >> 10; v[24] = (x3 + t0) >> 10; v[32] = (x3 - t0) >> 10;
  }
  for (u32 i = 0; i < 8; i++) {
    const i32* v = val + 8 * i; u8* o = out + i * stride;
    IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
    x0 += 65536 + (128 << 17); x1 += 65536 + (128 << 17); x2 += 65536 + (128 << 17); x3 += 65536 + (128 << 17);
    o[0] = clamp8((x0 + t3) >> 17); o[7] = clamp8((x0 - t3) >> 17); o[1] = clamp8((x1 + t2) >> 17); o[6] = clamp8((x1 - t2) >> 17);
    o[2] = clamp8((x2 + t1) >> 17); o[5] = clamp8((x2 - t1) >> 17); o[3] = clamp8((x3 + t0) >> 17); o[4] = clamp8((x3 - t0) >> 17);
  }
}

struct Jpeg {
  u32 w = 0, h = 0, ncomp = 0;
  Component comp[4];
  u16 qt[4][64]; bool qt_present[4];
  HuffTab dc[4], ac[4];
  u32 restart = 0;
  bool progressive = false, arithmetic = false, have_sof = false;
  u32 orientation = 1;
  bool adobe_rgb = false;
  bool eoi = false;
  u32 hmax = 1, vmax = 1, mcus_x = 0, mcus_y = 0;
};

u32 exif_orientation(const u8* p, u32 n) {
  if (n < 14 || memcmp(p, "Exif\0\0", 6)) return 0;
  const u8* t = p + 6; u32 tn = n - 6;
  bool le;
  if (!memcmp(t, "II", 2)) le = true; else if (!memcmp(t, "MM", 2)) le = false; else return 0;
  auto r16 = [&](u32 off) -> u32 { if (off + 2 > tn) return 0; return le ? (u32)t[off] | ((u32)t[off + 1] << 8) : ((u32)t[off] << 8) | t[off + 1]; };
  auto r32 = [&](u32 off) -> u32 { if (off + 4 > tn) return 0; return le ? (u32)t[off] | ((u32)t[off + 1] << 8) | ((u32)t[off + 2] << 16) | ((u32)t[off + 3] << 24) : ((u32)t[off] << 24) | ((u32)t[off + 1] << 16) | ((u32)t[off + 2] << 8) | t[off + 3]; };
  if (r16(2) != 42) return 0;
  u32 ifd = r32(4);
  if (ifd + 2 > tn) return 0;
  u32 count = r16(ifd);
  for (u32 i = 0; i < count && i < 512; i++) {
    u32 e = ifd + 2 + 12 * i;
    if (e + 12 > tn) break;
    if (r16(e) == 0x0112 && r16(e + 2) == 3) { u32 v = r16(e + 8); return v >= 1 && v <= 8 ? v : 0; }
  }
  return 0;
}

u32 parse_segments(Jpeg& j, const u8* d, u32 n, u32 pos, ScanInfo& scan, char* err, u32 ecap) {
  if (pos == 0) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) { snprintf(err, ecap, "not a JPEG"); return 0; }
    memset(j.qt_present, 0, sizeof j.qt_present);
    for (u32 i = 0; i < 4; i++) { j.dc[i].present = j.ac[i].present = false; }
    pos = 2;
  }
  for (;;) {
    while (pos < n && d[pos] != 0xFF) pos++;
    while (pos < n && d[pos] == 0xFF) pos++;
    if (pos >= n) { j.eoi = true; return 0; }
    u32 m = d[pos++];
    if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue; // no payload
    if (m == 0xD9) { j.eoi = true; return 0; }
    if (pos + 2 > n) { j.eoi = true; return 0; }
    u32 len = be16(d + pos);
    if (len < 2 || pos + len > n) { snprintf(err, ecap, "bad segment length"); return 0; }
    const u8* s = d + pos + 2; u32 sl = len - 2;
    switch (m) {
      case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF: {
        if (j.have_sof) break; // a second frame: ignored
        j.progressive = m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE;
        j.arithmetic = m >= 0xC9;
        if (sl < 6) { snprintf(err, ecap, "bad SOF"); return 0; }
        u32 prec = s[0]; j.h = be16(s + 1); j.w = be16(s + 3); j.ncomp = s[5];
        if (prec != 8) { snprintf(err, ecap, "%u-bit JPEG not supported", prec); return 0; }
        if (j.ncomp != 1 && j.ncomp != 3) { snprintf(err, ecap, "%u-component JPEG not supported", j.ncomp); return 0; }
        if (sl < 6 + 3 * j.ncomp) { snprintf(err, ecap, "bad SOF"); return 0; }
        for (u32 c = 0; c < j.ncomp; c++) {
          Component& k = j.comp[c];
          k.id = s[6 + 3 * c]; k.h = s[7 + 3 * c] >> 4; k.v = s[7 + 3 * c] & 15; k.tq = s[8 + 3 * c] & 3;
          if (!k.h || !k.v || k.h > 4 || k.v > 4) { snprintf(err, ecap, "bad sampling factors"); return 0; }
        }
        j.have_sof = true;
        break;
      }
      case 0xC4: { // DHT
        u32 o = 0;
        while (o + 17 <= sl) {
          u32 tc = s[o] >> 4, th = s[o] & 15;
          if (th > 3 || tc > 1) { snprintf(err, ecap, "bad Huffman table id"); return 0; }
          u32 total = 0; for (u32 i = 0; i < 16; i++) total += s[o + 1 + i];
          if (total > 256 || o + 17 + total > sl) { snprintf(err, ecap, "bad Huffman table"); return 0; }
          if (!huff_build(tc ? j.ac[th] : j.dc[th], s + o + 1, s + o + 17, total)) { snprintf(err, ecap, "bad Huffman table"); return 0; }
          o += 17 + total;
        }
        break;
      }
      case 0xDB: { // DQT
        u32 o = 0;
        while (o < sl) {
          u32 pq = s[o] >> 4, tq = s[o] & 15;
          if (tq > 3 || o + 1 + 64 * (pq ? 2 : 1) > sl) { snprintf(err, ecap, "bad quantisation table"); return 0; }
          for (u32 i = 0; i < 64; i++) j.qt[tq][kZigzag[i]] = (u16)(pq ? be16(s + o + 1 + 2 * i) : s[o + 1 + i]);
          j.qt_present[tq] = true;
          o += 1 + 64 * (pq ? 2 : 1);
        }
        break;
      }
      case 0xDD: if (sl >= 2) j.restart = be16(s); break;
      case 0xE1: { u32 o = exif_orientation(s, sl); if (o) j.orientation = o; break; }
      case 0xEE: if (sl >= 12 && !memcmp(s, "Adobe", 5)) j.adobe_rgb = s[11] == 0; break;
      case 0xDA: { // SOS
        if (!j.have_sof) { snprintf(err, ecap, "scan before frame"); return 0; }
        u32 ns = s[0];
        if (ns < 1 || ns > 4 || sl < 1 + 2 * ns + 3) { snprintf(err, ecap, "bad SOS"); return 0; }
        scan.ncomp = ns;
        for (u32 i = 0; i < ns; i++) {
          u32 cid = s[1 + 2 * i], tables = s[2 + 2 * i];
          u32 ci = 0xFFFFFFFFu;
          for (u32 c = 0; c < j.ncomp; c++) if (j.comp[c].id == cid) ci = c;
          if (ci == 0xFFFFFFFFu) { snprintf(err, ecap, "scan names an unknown component"); return 0; }
          j.comp[ci].td = tables >> 4; j.comp[ci].ta = tables & 15;
          scan.comp[i] = ci;
        }
        const u8* t = s + 1 + 2 * ns;
        scan.ss = t[0]; scan.se = t[1]; scan.ah = t[2] >> 4; scan.al = t[2] & 15;
        return pos + len;
      }
      default: break; // APPn, COM, DNL, ...: skipped
    }
    pos += len;
  }
}

u32 after_scan(const Reader& r, const u8* d, u32 n) {
  if (r.marker) return (u32)(r.p - 1 - d); // p is on the marker code byte; pos wants the 0xFF
  const u8* q = r.p;
  while (q + 1 < d + n && !(q[0] == 0xFF && q[1] != 0 && !(q[1] >= 0xD0 && q[1] <= 0xD7))) q++;
  return (u32)(q - d);
}

inline bool prog_dc_first(Reader& r, Jpeg& j, Component& k, i16* coef, u32 al, char* err, u32 ecap) {
  i32 t = huff_decode(r, j.dc[k.td]);
  if (t < 0 || t > 11) { snprintf(err, ecap, "bad DC code"); return false; }
  i32 diff = extend(getbits(r, (u32)t), (u32)t);
  k.dc_pred += diff;
  coef[0] = (i16)(k.dc_pred * (1 << al));
  return true;
}
inline void prog_dc_refine(Reader& r, i16* coef, u32 al) { if (getbit(r)) coef[0] = (i16)(coef[0] | (1 << al)); }

inline bool prog_ac_first(Reader& r, Jpeg& j, Component& k, i16* coef, const ScanInfo& sc, u32* eobrun, char* err, u32 ecap) {
  if (*eobrun) { (*eobrun)--; return true; }
  u32 z = sc.ss;
  do {
    i32 rs = huff_decode(r, j.ac[k.ta]);
    if (rs < 0) { snprintf(err, ecap, "bad AC code"); return false; }
    u32 run = (u32)rs >> 4, size = (u32)rs & 15;
    if (size == 0) {
      if (run < 15) { *eobrun = (1u << run) - 1; if (run) *eobrun += getbits(r, run); break; }
      z += 16;
    } else {
      z += run;
      if (z > 63) { snprintf(err, ecap, "AC run past the block"); return false; }
      coef[kZigzag[z]] = (i16)(extend(getbits(r, size), size) * (1 << sc.al));
      z++;
    }
  } while (z <= sc.se);
  return true;
}

inline bool prog_ac_refine(Reader& r, Jpeg& j, Component& k, i16* coef, const ScanInfo& sc, u32* eobrun, char* err, u32 ecap) {
  i16 bit = (i16)(1 << sc.al);
  auto refine = [&](i16* p) { if (getbit(r) && !(*p & bit)) *p = (i16)(*p > 0 ? *p + bit : *p - bit); };
  if (*eobrun) {
    (*eobrun)--;
    for (u32 z = sc.ss; z <= sc.se; z++) if (coef[kZigzag[z]]) refine(&coef[kZigzag[z]]);
    return true;
  }
  u32 z = sc.ss;
  do {
    i32 rs = huff_decode(r, j.ac[k.ta]);
    if (rs < 0) { snprintf(err, ecap, "bad AC code"); return false; }
    u32 run = (u32)rs >> 4, size = (u32)rs & 15;
    i16 val = 0;
    if (size == 0) {
      if (run < 15) { *eobrun = (1u << run) - 1; if (run) *eobrun += getbits(r, run); run = 64; } // EOB: only refinements follow
    } else {
      if (size != 1) { snprintf(err, ecap, "bad refinement code"); return false; }
      val = getbit(r) ? bit : (i16)-bit;
    }
    while (z <= sc.se) {
      i16* p = &coef[kZigzag[z++]];
      if (*p) refine(p);
      else { if (run == 0) { *p = val; break; } run--; }
    }
  } while (z <= sc.se);
  return true;
}

bool progressive_scan(Jpeg& j, const ScanInfo& sc, Reader& r, bool dc_only, char* err, u32 ecap) {
  bool dc = sc.ss == 0;
  if (dc && sc.se != 0) { snprintf(err, ecap, "bad progressive scan"); return false; }
  if (!dc && (sc.ncomp != 1 || sc.se > 63 || sc.ss > sc.se)) { snprintf(err, ecap, "bad progressive scan"); return false; }
  if (!dc && dc_only) return true; // skipped: the caller finds the next marker
  u32 stride = dc_only ? 1 : 64;
  for (u32 i = 0; i < sc.ncomp; i++) {
    Component& k = j.comp[sc.comp[i]];
    if (dc && sc.ah == 0 && !j.dc[k.td].present) { snprintf(err, ecap, "missing DC table"); return false; }
    if (!dc && !j.ac[k.ta].present) { snprintf(err, ecap, "missing AC table"); return false; }
    k.dc_pred = 0;
  }
  u32 eobrun = 0, units = 0;
  auto restart_check = [&]() -> bool {
    if (!j.restart || !units || units % j.restart) return true;
    if (!r.marker) {
      const u8* q = r.p;
      while (q + 1 < r.end && !(q[0] == 0xFF && q[1] >= 0xD0 && q[1] <= 0xD7)) q++;
      if (q + 1 < r.end) { r.p = q + 1; r.marker = q[1]; }
    }
    if (!(r.marker >= 0xD0 && r.marker <= 0xD7)) { snprintf(err, ecap, "missing restart marker"); return false; }
    r.marker = 0; r.acc = 0; r.n = 0; r.pad_bits = 0;
    if (r.p < r.end) r.p++;
    for (u32 i = 0; i < sc.ncomp; i++) j.comp[sc.comp[i]].dc_pred = 0;
    eobrun = 0;
    return true;
  };
  if (dc && sc.ncomp > 1) { // interleaved, MCU order
    for (u32 my = 0; my < j.mcus_y; my++) for (u32 mx = 0; mx < j.mcus_x; mx++) {
      if (!restart_check()) return false;
      for (u32 i = 0; i < sc.ncomp; i++) {
        Component& k = j.comp[sc.comp[i]];
        for (u32 by = 0; by < k.v; by++) for (u32 bx = 0; bx < k.h; bx++) {
          i16* coef = k.coef + ((usize)(my * k.v + by) * k.bw + (mx * k.h + bx)) * stride;
          if (sc.ah == 0) { if (!prog_dc_first(r, j, k, coef, sc.al, err, ecap)) return false; }
          else prog_dc_refine(r, coef, sc.al);
        }
      }
      units++;
    }
    return true;
  }
  Component& k = j.comp[sc.comp[0]];
  for (u32 by = 0; by < k.cbh; by++) for (u32 bx = 0; bx < k.cbw; bx++) {
    if (!restart_check()) return false;
    i16* coef = k.coef + ((usize)by * k.bw + bx) * stride;
    if (dc) { if (sc.ah == 0) { if (!prog_dc_first(r, j, k, coef, sc.al, err, ecap)) return false; } else prog_dc_refine(r, coef, sc.al); }
    else if (sc.ah == 0) { if (!prog_ac_first(r, j, k, coef, sc, &eobrun, err, ecap)) return false; }
    else { if (!prog_ac_refine(r, j, k, coef, sc, &eobrun, err, ecap)) return false; }
    units++;
  }
  return true;
}

} // namespace

bool jpeg_info(const u8* data, u32 n, i32* w, i32* h, bool* progressive, u32* orientation) {
  Jpeg j; char err[64]; ScanInfo sc;
  parse_segments(j, data, n, 0, sc, err, sizeof err);
  if (!j.have_sof) return false;
  *w = (i32)j.w; *h = (i32)j.h; *progressive = j.progressive || j.arithmetic; *orientation = j.orientation;
  return true;
}

bool jpeg_decode(const u8* data, u32 n, Image& out, u32 denom, u32 max_pixels, char* err, u32 ecap) {
  out.px = nullptr; out.w = out.h = 0;
  Jpeg j; ScanInfo sc = {};
  u32 sos = parse_segments(j, data, n, 0, sc, err, ecap);
  if (!sos) { if (j.eoi) snprintf(err, ecap, "no scan"); return false; }
  if (j.arithmetic) { snprintf(err, ecap, "arithmetic-coded JPEG not supported"); return false; }
  if (!j.w || !j.h) { snprintf(err, ecap, "bad dimensions"); return false; }
  if ((u64)j.w * j.h > max_pixels) { snprintf(err, ecap, "image too large (%ux%u)", j.w, j.h); return false; }
  if (denom != 1 && denom != 8) denom = 1;
  for (u32 c = 0; c < j.ncomp; c++) { j.hmax = mx_max(j.hmax, j.comp[c].h); j.vmax = mx_max(j.vmax, j.comp[c].v); }
  for (u32 c = 0; c < j.ncomp; c++) {
    Component& k = j.comp[c];
    if (j.hmax % k.h || j.vmax % k.v) { snprintf(err, ecap, "irregular sampling factors"); return false; }
  }
  u32 mcu_w = 8 * j.hmax, mcu_h = 8 * j.vmax;
  j.mcus_x = (j.w + mcu_w - 1) / mcu_w; j.mcus_y = (j.h + mcu_h - 1) / mcu_h;
  u32 bs = denom == 8 ? 1 : 8; // pixels per block side in the planes
  bool prog = j.progressive;
  if (prog && denom == 1 && (u64)j.w * j.h > (40u << 20)) { snprintf(err, ecap, "progressive JPEG too large to decode in full (%ux%u)", j.w, j.h); return false; }
  for (u32 c = 0; c < j.ncomp; c++) {
    Component& k = j.comp[c];
    k.bw = j.mcus_x * k.h; k.bh = j.mcus_y * k.v;
    k.pw = k.bw * bs; k.ph = k.bh * bs;
    k.plane = (u8*)malloc((usize)k.pw * k.ph);
    k.dc_pred = 0; k.coef = nullptr;
    u32 cw = (j.w * k.h + j.hmax - 1) / j.hmax, ch = (j.h * k.v + j.vmax - 1) / j.vmax;
    k.cbw = (cw + 7) / 8; k.cbh = (ch + 7) / 8;
    if (prog) { usize per = denom == 8 ? 1 : 64; k.coef = (i16*)calloc((usize)k.bw * k.bh * per, sizeof(i16)); }
    if (!k.plane || (prog && !k.coef)) { for (u32 i = 0; i <= c; i++) { free(j.comp[i].plane); free(j.comp[i].coef); } snprintf(err, ecap, "out of memory"); return false; }
  }
  auto cleanup = [&]() { for (u32 c = 0; c < j.ncomp; c++) { free(j.comp[c].plane); free(j.comp[c].coef); } };

  if (prog) {

    for (;;) {
      Reader r = { data + sos, data + n, 0, 0, 0, 0 };
      if (!progressive_scan(j, sc, r, denom == 8, err, ecap)) { cleanup(); return false; }
      u32 pos = after_scan(r, data, n);
      sos = parse_segments(j, data, n, pos, sc, err, ecap);
      if (!sos) { if (!j.eoi) { cleanup(); return false; } break; }
    }
    for (u32 c = 0; c < j.ncomp; c++) {
      Component& k = j.comp[c];
      if (!j.qt_present[k.tq]) { cleanup(); snprintf(err, ecap, "missing quantisation table"); return false; }
      const u16* q = j.qt[k.tq];
      for (u32 by = 0; by < k.bh; by++) for (u32 bx = 0; bx < k.bw; bx++) {
        if (denom == 8) { i32 dc = k.coef[(usize)by * k.bw + bx] * q[0]; k.plane[(usize)by * k.pw + bx] = clamp8(128 + ((dc + 4) >> 3)); continue; }
        const i16* cf = k.coef + ((usize)by * k.bw + bx) * 64;
        i32 blk[64];
        for (u32 i = 0; i < 64; i++) blk[i] = cf[i] * q[i];
        idct_block(k.plane + (usize)by * 8 * k.pw + bx * 8, k.pw, blk);
      }
    }
  } else {
    if (sc.ncomp != j.ncomp) { cleanup(); snprintf(err, ecap, "non-interleaved scan not supported"); return false; }
    for (u32 c = 0; c < j.ncomp; c++) {
      Component& k = j.comp[c];
      if (!j.qt_present[k.tq] || !j.dc[k.td].present || !j.ac[k.ta].present) { cleanup(); snprintf(err, ecap, "missing tables"); return false; }
    }
    Reader r = { data + sos, data + n, 0, 0, 0, 0 };
    i32 coef[64];
    u32 mcu_count = 0;
    bool ok = true;
    for (u32 my = 0; my < j.mcus_y && ok; my++) {
      for (u32 mx = 0; mx < j.mcus_x && ok; mx++) {
        if (j.restart && mcu_count && mcu_count % j.restart == 0) {

          if (!r.marker) {
            const u8* q = r.p;
            while (q + 1 < r.end && !(q[0] == 0xFF && q[1] >= 0xD0 && q[1] <= 0xD7)) q++;
            if (q + 1 < r.end) { r.p = q + 1; r.marker = q[1]; }
          }
          if (r.marker >= 0xD0 && r.marker <= 0xD7) {
            r.marker = 0; r.acc = 0; r.n = 0; r.pad_bits = 0;
            if (r.p < r.end) r.p++; // past the marker byte
          } else { snprintf(err, ecap, "missing restart marker"); ok = false; break; }
          for (u32 c = 0; c < j.ncomp; c++) j.comp[c].dc_pred = 0;
        }
        for (u32 c = 0; c < j.ncomp && ok; c++) {
          Component& k = j.comp[c];
          for (u32 by = 0; by < k.v && ok; by++) for (u32 bx = 0; bx < k.h && ok; bx++) {
            memset(coef, 0, sizeof coef);
            i32 t = huff_decode(r, j.dc[k.td]);
            if (t < 0 || t > 11) { snprintf(err, ecap, "bad DC code"); ok = false; break; }
            i32 diff = extend(getbits(r, (u32)t), (u32)t);
            k.dc_pred += diff;
            coef[0] = k.dc_pred * j.qt[k.tq][0];
            for (u32 z = 1; z < 64;) {
              i32 rs = huff_decode(r, j.ac[k.ta]);
              if (rs < 0) { snprintf(err, ecap, "bad AC code"); ok = false; break; }
              u32 run = (u32)rs >> 4, size = (u32)rs & 15;
              if (size == 0) { if (run == 15) { z += 16; continue; } break; }
              z += run;
              if (z > 63) { snprintf(err, ecap, "AC run past the block"); ok = false; break; }
              coef[kZigzag[z]] = extend(getbits(r, size), size) * j.qt[k.tq][kZigzag[z]];
              z++;
            }
            if (!ok) break;
            u32 gx = mx * k.h + bx, gy = my * k.v + by; // block position in the plane
            if (denom == 8) k.plane[(usize)gy * k.pw + gx] = clamp8(128 + ((coef[0] + 4) >> 3));
            else idct_block(k.plane + (usize)gy * 8 * k.pw + gx * 8, k.pw, coef);
          }
        }
        mcu_count++;
        if (r.marker && !(r.marker >= 0xD0 && r.marker <= 0xD7) && r.n < r.pad_bits && (my + 1 < j.mcus_y || mx + 1 < j.mcus_x)) {

          for (u32 c = 0; c < j.ncomp; c++) if (my + 1 < j.mcus_y) memset(j.comp[c].plane + (usize)(my + 1) * j.comp[c].v * bs * j.comp[c].pw, 128, (usize)(j.mcus_y - my - 1) * j.comp[c].v * bs * j.comp[c].pw);
          my = j.mcus_y; break;
        }
      }
    }
    if (!ok) { cleanup(); return false; }
  }

  u32 ow = (j.w + denom - 1) / denom, oh = (j.h + denom - 1) / denom;
  Image img; img.w = (i32)ow; img.h = (i32)oh;
  img.px = (u32*)malloc((usize)ow * oh * 4);
  if (!img.px) { cleanup(); snprintf(err, ecap, "out of memory"); return false; }
  for (u32 y = 0; y < oh; y++) {
    u32* row = img.px + (usize)y * ow;
    for (u32 x = 0; x < ow; x++) {
      i32 Y, Cb = 128, Cr = 128;
      const Component& c0 = j.comp[0];
      Y = c0.plane[(usize)mx_min(y * c0.v / j.vmax, c0.ph - 1) * c0.pw + mx_min(x * c0.h / j.hmax, c0.pw - 1)];
      if (j.ncomp == 3) {
        const Component& c1 = j.comp[1]; const Component& c2 = j.comp[2];
        Cb = c1.plane[(usize)mx_min(y * c1.v / j.vmax, c1.ph - 1) * c1.pw + mx_min(x * c1.h / j.hmax, c1.pw - 1)];
        Cr = c2.plane[(usize)mx_min(y * c2.v / j.vmax, c2.ph - 1) * c2.pw + mx_min(x * c2.h / j.hmax, c2.pw - 1)];
      }
      i32 rr, gg, bb;
      if (j.ncomp == 3 && !j.adobe_rgb) {
        i32 cb = Cb - 128, cr = Cr - 128;
        rr = Y + ((91881 * cr + 32768) >> 16);
        gg = Y - ((22554 * cb + 46802 * cr + 32768) >> 16);
        bb = Y + ((116130 * cb + 32768) >> 16);
      } else if (j.ncomp == 3) { rr = Y; gg = Cb; bb = Cr; }
      else rr = gg = bb = Y;
      row[x] = 0xFF000000u | ((u32)clamp8(rr) << 16) | ((u32)clamp8(gg) << 8) | clamp8(bb);
    }
  }
  cleanup();
  if (j.orientation > 1) { image_orient(img, j.orientation, out); image_free(img); if (!out.px) { snprintf(err, ecap, "out of memory"); return false; } }
  else out = img;
  return true;
}
