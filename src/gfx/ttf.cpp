#include "gfx/ttf.h"
#include "gfx/raster.h"
#include "base/algo.h"

#include <string.h>

static inline u32 tag4(const char* s) { return ((u32)(u8)s[0] << 24) | ((u32)(u8)s[1] << 16) | ((u32)(u8)s[2] << 8) | (u8)s[3]; }
static inline bool in_file(const TtfFace& f, u32 off, u32 len) { return off <= f.size && len <= f.size - off; }
static inline u32 popcount16(u32 v) { return (u32)__builtin_popcount(v & 0xFFFF); }

static void set_err(TtfFace& f, const char* msg) {
  usize n = strlen(msg);
  if (n >= sizeof f.err) n = sizeof f.err - 1;
  memcpy(f.err, msg, n);
  f.err[n] = 0;
}

u32 ttf_face_count(const u8* data, u32 size) {
  TtfFace f; f.data = data; f.size = size;
  if (size < 12) return 0;
  u32 tag = ttf_u32(f, 0);
  if (tag == tag4("ttcf")) return ttf_u32(f, 8);
  if (tag == 0x00010000 || tag == tag4("true") || tag == tag4("OTTO")) return 1;
  return 0;
}

struct TableRef { u32 off, len; };

static TableRef find_table(const TtfFace& f, const char* name) {
  u32 n = ttf_u16(f, f.base + 4);
  u32 want = tag4(name);
  for (u32 i = 0; i < n; i++) {
    u32 rec = f.base + 12 + i * 16;
    if (!in_file(f, rec, 16)) break;
    if (ttf_u32(f, rec) != want) continue;
    u32 off = ttf_u32(f, rec + 8), len = ttf_u32(f, rec + 12);
    if (!in_file(f, off, len)) return { 0, 0 };
    return { off, len };
  }
  return { 0, 0 };
}

static void copy_name(const TtfFace& f, u32 off, u32 len, bool utf16, char* out, u32 cap) {
  u32 o = 0;
  if (utf16) {
    for (u32 i = 0; i + 1 < len; i += 2) {
      u32 cp = ttf_u16(f, off + i);
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len) { // surrogate pair
        u32 lo = ttf_u16(f, off + i + 2);
        if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
      }
      char b[4];
      u32  n = utf8_put(cp, b);
      if (o + n >= cap) break;
      memcpy(out + o, b, n); o += n;
    }
  } else {
    for (u32 i = 0; i < len && o + 1 < cap; i++) { u8 c = f.data[off + i]; out[o++] = c < 0x80 ? (char)c : '?'; }
  }
  out[o] = 0;
}

static void parse_names(TtfFace& f) {
  if (!f.name_len || f.name_len < 6) return;
  u32 count = ttf_u16(f, f.name + 2), strings = f.name + ttf_u16(f, f.name + 4);

  i32 best_fam = -1, best_sty = -1; u32 fam_rec = 0, sty_rec = 0;
  for (u32 i = 0; i < count; i++) {
    u32 rec = f.name + 6 + i * 12;
    if (!in_file(f, rec, 12)) break;
    u32 plat = ttf_u16(f, rec), enc = ttf_u16(f, rec + 2), lang = ttf_u16(f, rec + 4), id = ttf_u16(f, rec + 6);
    i32 score;
    if (plat == 3 && (enc == 1 || enc == 10)) score = lang == 0x409 ? 40 : 30;
    else if (plat == 0) score = 20;
    else if (plat == 1 && enc == 0) score = 10;
    else continue;
    if (id == 16 || id == 17) score += 5;
    else if (id != 1 && id != 2) continue;
    if (id == 16 || id == 1) { if (score > best_fam) { best_fam = score; fam_rec = rec; } }
    else                     { if (score > best_sty) { best_sty = score; sty_rec = rec; } }
  }
  u32 recs[2] = { fam_rec, sty_rec };
  for (u32 k = 0; k < 2; k++) {
    if (!recs[k]) continue;
    u32 rec = recs[k], plat = ttf_u16(f, rec), len = ttf_u16(f, rec + 8), off = strings + ttf_u16(f, rec + 10);
    if (!in_file(f, off, len)) continue;
    if (k == 0) copy_name(f, off, len, plat != 1, f.family, sizeof f.family);
    else        copy_name(f, off, len, plat != 1, f.style, sizeof f.style);
  }
}

static bool range_less(const TtfCmapRange& a, const TtfCmapRange& b) { return a.start < b.start; }

static void parse_cmap_subtable(TtfFace& f, u32 st) {
  u32 format = ttf_u16(f, st);
  if (format == 0) {
    if (!in_file(f, st + 6, 256)) return;
    u32 idx = f.cmap_glyphs.len;
    bool any = false;
    for (u32 c = 0; c < 256; c++) { u16 g = f.data[st + 6 + c]; f.cmap_glyphs.push(g); any |= g != 0; }
    if (any) f.cmap.push({ 0, 255, idx, 1 });
  } else if (format == 4) {
    u32 seg2 = ttf_u16(f, st + 6), segs = seg2 / 2;
    u32 ends = st + 14, starts = ends + seg2 + 2, deltas = starts + seg2, ranges = deltas + seg2;
    if (!in_file(f, ranges, seg2)) return;
    for (u32 i = 0; i < segs; i++) {
      u32 end = ttf_u16(f, ends + i * 2), start = ttf_u16(f, starts + i * 2);
      u32 delta = ttf_u16(f, deltas + i * 2), ro = ttf_u16(f, ranges + i * 2);
      if (start > end || start == 0xFFFF) continue;
      if (ro == 0) {
        f.cmap.push({ start, end, (start + delta) & 0xFFFF, 0 });
      } else {
        u32 idx = f.cmap_glyphs.len;
        u32 arr = ranges + i * 2 + ro;
        for (u32 c = start; c <= end; c++) {
          u32 a = arr + (c - start) * 2;
          u16 g = in_file(f, a, 2) ? ttf_u16(f, a) : 0;
          if (g) g = (u16)((g + delta) & 0xFFFF);
          f.cmap_glyphs.push(g);
        }
        f.cmap.push({ start, end, idx, 1 });
      }
    }
  } else if (format == 6) {
    u32 first = ttf_u16(f, st + 6), count = ttf_u16(f, st + 8);
    if (!count || !in_file(f, st + 10, count * 2)) return;
    u32 idx = f.cmap_glyphs.len;
    for (u32 i = 0; i < count; i++) f.cmap_glyphs.push(ttf_u16(f, st + 10 + i * 2));
    f.cmap.push({ first, first + count - 1, idx, 1 });
  } else if (format == 12) {
    u32 groups = ttf_u32(f, st + 12);
    if (groups > 100000 || !in_file(f, st + 16, groups * 12)) return;
    for (u32 i = 0; i < groups; i++) {
      u32 g = st + 16 + i * 12;
      u32 start = ttf_u32(f, g), end = ttf_u32(f, g + 4), glyph = ttf_u32(f, g + 8);
      if (start > end || end > 0x10FFFF) continue;
      f.cmap.push({ start, end, glyph, 0 });
    }
  }
}

static void parse_cmap(TtfFace& f, TableRef t) {
  if (!t.len) return;
  u32 n = ttf_u16(f, t.off + 2);
  i32 best = -1; u32 best_off = 0;
  for (u32 i = 0; i < n; i++) {
    u32 rec = t.off + 4 + i * 8;
    if (!in_file(f, rec, 8)) break;
    u32 plat = ttf_u16(f, rec), enc = ttf_u16(f, rec + 2), off = ttf_u32(f, rec + 4);
    if (!in_file(f, t.off + off, 4)) continue;
    i32 score = -1;
    if (plat == 3 && enc == 10) score = 50;
    else if (plat == 0 && (enc == 4 || enc == 6)) score = 45;
    else if (plat == 3 && enc == 1) score = 40;
    else if (plat == 0) score = 30;
    else if (plat == 3 && enc == 0) score = 20; // symbol fonts
    else if (plat == 1 && enc == 0) score = 10;
    if (score > best) { best = score; best_off = t.off + off; }
  }
  if (best < 0) return;
  parse_cmap_subtable(f, best_off);

  if (best == 20) for (TtfCmapRange& r : f.cmap) if (r.start >= 0xF000 && r.end <= 0xF0FF) { r.start -= 0xF000; r.end -= 0xF000; }
  if (f.cmap.len > 1) {
    Array<TtfCmapRange> tmp;
    tmp.resize(f.cmap.len / 2 + 1);
    merge_sort(f.cmap.data, tmp.data, f.cmap.len, range_less);
  }
}

u16 ttf_glyph_index(const TtfFace& f, u32 cp) {
  u32 lo = 0, hi = f.cmap.len;
  while (lo < hi) { // last range with start <= cp
    u32 mid = (lo + hi) / 2;
    if (f.cmap[mid].start <= cp) lo = mid + 1; else hi = mid;
  }
  if (lo == 0) return 0;
  const TtfCmapRange& r = f.cmap[lo - 1];
  if (cp > r.end) return 0;
  u32 g;
  if (r.indexed) g = f.cmap_glyphs[r.value + (cp - r.start)];
  else g = (r.value + (cp - r.start)) & 0xFFFF;
  return g < f.num_glyphs ? (u16)g : 0;
}

static void parse_kern(TtfFace& f, TableRef t) {
  if (t.len < 4) return;
  f.kern = t.off; f.kern_len = t.len;
  u32 version = ttf_u16(f, t.off);
  if (version == 0) {
    u32 n = ttf_u16(f, t.off + 2), st = t.off + 4;
    for (u32 i = 0; i < n && in_file(f, st, 14); i++) {
      u32 len = ttf_u16(f, st + 2), cov = ttf_u16(f, st + 4);
      if ((cov >> 8) == 0 && (cov & 1)) {
        u32 pairs = ttf_u16(f, st + 6);
        if (in_file(f, st + 14, pairs * 6)) { f.kern_pairs = st + 14; f.kern_count = pairs; return; }
      }
      st += mx_max(len, 14u);
    }
  } else if (ttf_u32(f, t.off) == 0x00010000) { // Apple
    u32 n = ttf_u32(f, t.off + 4), st = t.off + 8;
    for (u32 i = 0; i < n && in_file(f, st, 16); i++) {
      u32 len = ttf_u32(f, st), cov = ttf_u16(f, st + 4);
      if ((cov & 0xFF) == 0 && !(cov & 0x8000)) {
        u32 pairs = ttf_u16(f, st + 8);
        if (in_file(f, st + 16, pairs * 6)) { f.kern_pairs = st + 16; f.kern_count = pairs; return; }
      }
      st += mx_max(len, 16u);
    }
  }
}

static i16 kern_table_lookup(const TtfFace& f, u16 left, u16 right) {
  u32 key = ((u32)left << 16) | right, lo = 0, hi = f.kern_count;
  while (lo < hi) {
    u32 mid = (lo + hi) / 2, rec = f.kern_pairs + mid * 6, k = ttf_u32(f, rec);
    if (k == key) return ttf_i16(f, rec + 4);
    if (k < key) lo = mid + 1; else hi = mid;
  }
  return 0;
}

static void parse_gpos(TtfFace& f, TableRef t) {
  if (t.len < 10) return;
  f.gpos = t.off; f.gpos_len = t.len;
  u32 ll = t.off + ttf_u16(f, t.off + 8);
  u32 n  = ttf_u16(f, ll);
  for (u32 i = 0; i < n && i < 512; i++) {
    u32 lk = ll + ttf_u16(f, ll + 2 + i * 2);
    if (!in_file(f, lk, 6)) continue;
    u32 type = ttf_u16(f, lk), subs = ttf_u16(f, lk + 4);
    if (type != 2 && type != 9) continue;
    for (u32 s = 0; s < subs && s < 64; s++) {
      u32 st = lk + ttf_u16(f, lk + 6 + s * 2);
      if (type == 9) {
        if (ttf_u16(f, st) != 1 || ttf_u16(f, st + 2) != 2) continue;
        st = st + ttf_u32(f, st + 4);
      }
      if (in_file(f, st, 8)) f.gpos_pairpos.push(st);
    }
  }
}

static i32 coverage_index(const TtfFace& f, u32 cov, u16 g) {
  u32 fmt = ttf_u16(f, cov);
  if (fmt == 1) {
    u32 lo = 0, hi = ttf_u16(f, cov + 2);
    while (lo < hi) {
      u32 mid = (lo + hi) / 2, v = ttf_u16(f, cov + 4 + mid * 2);
      if (v == g) return (i32)mid;
      if (v < g) lo = mid + 1; else hi = mid;
    }
  } else if (fmt == 2) {
    u32 lo = 0, hi = ttf_u16(f, cov + 2);
    while (lo < hi) {
      u32 mid = (lo + hi) / 2, r = cov + 4 + mid * 6;
      u32 s = ttf_u16(f, r), e = ttf_u16(f, r + 2);
      if (g < s) hi = mid;
      else if (g > e) lo = mid + 1;
      else return (i32)(ttf_u16(f, r + 4) + (g - s));
    }
  }
  return -1;
}

static u32 class_of(const TtfFace& f, u32 cd, u16 g) {
  u32 fmt = ttf_u16(f, cd);
  if (fmt == 1) {
    u32 s = ttf_u16(f, cd + 2), n = ttf_u16(f, cd + 4);
    if (g >= s && g < s + n) return ttf_u16(f, cd + 6 + (g - s) * 2);
  } else if (fmt == 2) {
    u32 lo = 0, hi = ttf_u16(f, cd + 2);
    while (lo < hi) {
      u32 mid = (lo + hi) / 2, r = cd + 4 + mid * 6;
      u32 s = ttf_u16(f, r), e = ttf_u16(f, r + 2);
      if (g < s) hi = mid;
      else if (g > e) lo = mid + 1;
      else return ttf_u16(f, r + 4);
    }
  }
  return 0;
}

static bool gpos_lookup(const TtfFace& f, u16 g1, u16 g2, i16& out) {
  for (u32 st : f.gpos_pairpos) {
    u32 fmt = ttf_u16(f, st), cov = st + ttf_u16(f, st + 2), vf1 = ttf_u16(f, st + 4), vf2 = ttf_u16(f, st + 6);
    if (!(vf1 & 4)) continue; // no x-advance in value1: not a kerning subtable
    i32 ci = coverage_index(f, cov, g1);
    if (ci < 0) continue;
    u32 adv_off = 2 * popcount16(vf1 & 3);
    if (fmt == 1) {
      u32 sets = ttf_u16(f, st + 8);
      if ((u32)ci >= sets) continue;
      u32 ps = st + ttf_u16(f, st + 10 + (u32)ci * 2);
      u32 n = ttf_u16(f, ps), rec = 2 + 2 * (popcount16(vf1) + popcount16(vf2));
      u32 lo = 0, hi = n;
      while (lo < hi) {
        u32 mid = (lo + hi) / 2, r = ps + 2 + mid * rec, sg = ttf_u16(f, r);
        if (sg == g2) { out = ttf_i16(f, r + 2 + adv_off); return true; }
        if (sg < g2) lo = mid + 1; else hi = mid;
      }
    } else if (fmt == 2) {
      u32 cd1 = st + ttf_u16(f, st + 8), cd2 = st + ttf_u16(f, st + 10);
      u32 c1n = ttf_u16(f, st + 12), c2n = ttf_u16(f, st + 14);
      u32 c1 = class_of(f, cd1, g1), c2 = class_of(f, cd2, g2);
      if (c1 >= c1n || c2 >= c2n) continue;
      u32 rec = 2 * (popcount16(vf1) + popcount16(vf2));
      u32 r   = st + 16 + (c1 * c2n + c2) * rec;
      if (!in_file(f, r, rec)) continue;
      out = ttf_i16(f, r + adv_off);
      return true;
    }
  }
  return false;
}

i16 ttf_kern(const TtfFace& f, u16 left, u16 right) {
  if (f.kern_count) { i16 v = kern_table_lookup(f, left, right); if (v) return v; }
  i16 v = 0;
  if (f.gpos_pairpos.len && gpos_lookup(f, left, right, v)) return v;
  return 0;
}

u16 ttf_advance(const TtfFace& f, u16 glyph) {
  if (!f.num_hmetrics) return 0;
  u32 i = glyph < f.num_hmetrics ? glyph : f.num_hmetrics - 1u;
  return ttf_u16(f, f.hmtx + i * 4);
}

static bool glyph_range(const TtfFace& f, u16 glyph, u32& off, u32& len) {
  if (glyph >= f.num_glyphs || !f.loca_len || !f.glyf_len) return false;
  u32 a, b;
  if (f.long_loca) {
    if ((u32)(glyph + 1) * 4 + 4 > f.loca_len) return false;
    a = ttf_u32(f, f.loca + glyph * 4); b = ttf_u32(f, f.loca + glyph * 4 + 4);
  } else {
    if ((u32)(glyph + 1) * 2 + 2 > f.loca_len) return false;
    a = ttf_u16(f, f.loca + glyph * 2) * 2u; b = ttf_u16(f, f.loca + glyph * 2 + 2) * 2u;
  }
  if (b < a || b > f.glyf_len) return false;
  off = f.glyf + a; len = b - a;
  return len >= 10;
}

static const u32 kMaxPoints = 8192;

static inline void emit(Path& p, const float m[6], float x, float y, bool on, float& px, float& py, bool& poff) {

  float tx = m[0] * x + m[2] * y + m[4];
  float ty = m[1] * x + m[3] * y + m[5];
  if (on) {
    if (poff) p.quad_to(px, py, tx, ty); else p.line_to(tx, ty);
    poff = false;
  } else {
    if (poff) { p.quad_to(px, py, 0.5f * (px + tx), 0.5f * (py + ty)); }
    px = tx; py = ty; poff = true;
  }
}

static bool outline(const TtfFace& f, u16 glyph, Path& p, const float m[6], u32 depth);

static bool simple_outline(const TtfFace& f, u32 off, u32 len, i32 nc, Path& p, const float m[6]) {
  u32 end = off + len;
  u32 ep = off + 10;
  if (ep + (u32)nc * 2 + 2 > end) return false;
  u32 npts = nc ? (u32)ttf_u16(f, ep + (u32)(nc - 1) * 2) + 1 : 0;
  if (!npts || npts > kMaxPoints) return false;
  u32 ins = ttf_u16(f, ep + (u32)nc * 2);
  u32 pos = ep + (u32)nc * 2 + 2 + ins;
  if (pos > end) return false;

  i16 xs[kMaxPoints]; i16 ys[kMaxPoints]; u8 fl[kMaxPoints];
  for (u32 i = 0; i < npts;) {
    if (pos >= end) return false;
    u8 flag = f.data[pos++];
    fl[i++] = flag;
    if (flag & 8) {
      if (pos >= end) return false;
      u32 rep = f.data[pos++];
      while (rep-- && i < npts) fl[i++] = flag;
    }
  }
  i32 v = 0;
  for (u32 i = 0; i < npts; i++) {
    u8 flag = fl[i];
    if (flag & 2) { if (pos >= end) return false; i32 d = f.data[pos++]; v += (flag & 16) ? d : -d; }
    else if (!(flag & 16)) { if (pos + 2 > end) return false; v += ttf_i16(f, pos); pos += 2; }
    xs[i] = (i16)v;
  }
  v = 0;
  for (u32 i = 0; i < npts; i++) {
    u8 flag = fl[i];
    if (flag & 4) { if (pos >= end) return false; i32 d = f.data[pos++]; v += (flag & 32) ? d : -d; }
    else if (!(flag & 32)) { if (pos + 2 > end) return false; v += ttf_i16(f, pos); pos += 2; }
    ys[i] = (i16)v;
  }

  u32 s = 0;
  for (i32 c = 0; c < nc; c++) {
    u32 e = ttf_u16(f, ep + (u32)c * 2);
    if (e < s || e >= npts) return false;
    u32 n = e - s + 1;

    u32   first = s; bool found = false;
    for (u32 i = s; i <= e; i++) if (fl[i] & 1) { first = i; found = true; break; }
    float sx, sy;
    if (found) { sx = xs[first]; sy = ys[first]; }
    else { u32 nx = s + (1 % n); sx = 0.5f * (xs[s] + xs[nx]); sy = 0.5f * (ys[s] + ys[nx]); }
    p.move_to(m[0] * sx + m[2] * sy + m[4], m[1] * sx + m[3] * sy + m[5]);
    float px = 0, py = 0; bool poff = false;
    for (u32 k = 1; k <= n; k++) {
      u32 i = found ? s + ((first - s + k) % n) : s + (k % n);
      if (found && k == n) break;
      emit(p, m, xs[i], ys[i], fl[i] & 1, px, py, poff);
    }

    float ex = m[0] * sx + m[2] * sy + m[4], ey = m[1] * sx + m[3] * sy + m[5];
    if (poff) p.quad_to(px, py, ex, ey);
    p.close();
    s = e + 1;
  }
  return true;
}

static bool composite_outline(const TtfFace& f, u32 off, u32 len, Path& p, const float m[6], u32 depth) {
  u32 pos = off + 10, end = off + len;
  bool any = false;
  for (u32 guard = 0; guard < 64; guard++) {
    if (pos + 4 > end) break;
    u32 flags = ttf_u16(f, pos), gi = ttf_u16(f, pos + 2);
    pos += 4;
    float dx = 0, dy = 0;
    if (flags & 1) { // ARG_1_AND_2_ARE_WORDS
      if (pos + 4 > end) break;
      if (flags & 2) { dx = ttf_i16(f, pos); dy = ttf_i16(f, pos + 2); }
      pos += 4;
    } else {
      if (pos + 2 > end) break;
      if (flags & 2) { dx = (i8)f.data[pos]; dy = (i8)f.data[pos + 1]; }
      pos += 2;
    }
    float a = 1, b = 0, c = 0, d = 1;
    auto f2d = [&](u32 o) { return (float)ttf_i16(f, o) / 16384.0f; };
    if (flags & 8) { a = d = f2d(pos); pos += 2; } // WE_HAVE_A_SCALE
    else if (flags & 0x40) { a = f2d(pos); d = f2d(pos + 2); pos += 4; } // X_AND_Y_SCALE
    else if (flags & 0x80) { a = f2d(pos); b = f2d(pos + 2); c = f2d(pos + 4); d = f2d(pos + 6); pos += 8; }

    float cm[6] = {
      m[0] * a + m[2] * b, m[1] * a + m[3] * b,
      m[0] * c + m[2] * d, m[1] * c + m[3] * d,
      m[0] * dx + m[2] * dy + m[4], m[1] * dx + m[3] * dy + m[5],
    };
    any |= outline(f, (u16)gi, p, cm, depth + 1);
    if (!(flags & 0x20)) break; // MORE_COMPONENTS
  }
  return any;
}

static bool outline(const TtfFace& f, u16 glyph, Path& p, const float m[6], u32 depth) {
  if (depth > 8) return false;
  u32 off, len;
  if (!glyph_range(f, glyph, off, len)) return false;
  i32 nc = ttf_i16(f, off);
  if (nc >= 0) return simple_outline(f, off, len, nc, p, m);
  return composite_outline(f, off, len, p, m, depth);
}

bool ttf_glyph_path(const TtfFace& f, u16 glyph, Path& p, float sx, float sy, float tx, float ty) {
  if (!f.has_outlines) return false;
  const float m[6] = { sx, 0, 0, -sy, tx, ty };
  u32 verbs = p.verbs.len, pts = p.pts.len;
  if (!outline(f, glyph, p, m, 0)) { p.verbs.len = verbs; p.pts.len = pts; return false; }
  return true;
}

bool ttf_parse(TtfFace& f, const u8* data, u32 size, u32 index, u32 flags) {
  f.cmap.clear(); f.cmap_glyphs.clear(); f.gpos_pairpos.clear();
  f.data = data; f.size = size; f.ok = false; f.err[0] = 0;
  f.family[0] = f.style[0] = 0;
  f.glyf_len = f.loca_len = f.hmtx_len = f.kern_len = f.gpos_len = f.cff_len = f.name_len = f.os2_len = 0;
  f.kern_count = 0; f.has_outlines = false; f.italic = f.fixed_pitch = false; f.weight = 400;
  f.x_height = f.cap_height = 0;

  if (size < 12) { set_err(f, "too small"); return false; }
  u32 tag = ttf_u32(f, 0);
  if (tag == tag4("ttcf")) {
    u32 n = ttf_u32(f, 8);
    if (index >= n) { set_err(f, "no such face"); return false; }
    f.base = ttf_u32(f, 12 + index * 4);
  } else if (tag == 0x00010000 || tag == tag4("true") || tag == tag4("OTTO")) {
    if (index != 0) { set_err(f, "no such face"); return false; }
    f.base = 0;
  } else { set_err(f, "not a TrueType/OpenType font"); return false; }
  if (!in_file(f, f.base, 12)) { set_err(f, "bad directory"); return false; }

  TableRef head = find_table(f, "head"), hhea = find_table(f, "hhea"), maxp = find_table(f, "maxp");
  TableRef hmtx = find_table(f, "hmtx"), loca = find_table(f, "loca"), glyf = find_table(f, "glyf");
  TableRef cff = find_table(f, "CFF "), name = find_table(f, "name"), os2 = find_table(f, "OS/2");
  TableRef post = find_table(f, "post");
  if (head.len < 54 || hhea.len < 36 || maxp.len < 6) { set_err(f, "missing head/hhea/maxp"); return false; }
  if (ttf_u32(f, head.off + 12) != 0x5F0F3CF5) { set_err(f, "bad head magic"); return false; }

  f.units_per_em = ttf_u16(f, head.off + 18);
  if (!f.units_per_em) f.units_per_em = 1000;
  f.x_min = ttf_i16(f, head.off + 36); f.y_min = ttf_i16(f, head.off + 38);
  f.x_max = ttf_i16(f, head.off + 40); f.y_max = ttf_i16(f, head.off + 42);
  f.long_loca = ttf_i16(f, head.off + 50) != 0;
  u32 mac_style = ttf_u16(f, head.off + 44);
  f.num_glyphs = ttf_u16(f, maxp.off + 4);
  f.ascender = ttf_i16(f, hhea.off + 4); f.descender = ttf_i16(f, hhea.off + 6); f.line_gap = ttf_i16(f, hhea.off + 8);
  f.num_hmetrics = ttf_u16(f, hhea.off + 34);
  f.hmtx = hmtx.off; f.hmtx_len = hmtx.len;
  if (f.num_hmetrics * 4u > hmtx.len) f.num_hmetrics = (u16)(hmtx.len / 4);
  f.loca = loca.off; f.loca_len = loca.len; f.glyf = glyf.off; f.glyf_len = glyf.len;
  f.cff = cff.off; f.cff_len = cff.len; f.name = name.off; f.name_len = name.len; f.os2 = os2.off; f.os2_len = os2.len;
  f.has_outlines = glyf.len > 0 && loca.len > 0;
  f.italic = (mac_style & 2) != 0;
  if (mac_style & 1) f.weight = 700;

  if (os2.len >= 64) {
    u32 v = ttf_u16(f, os2.off);
    f.weight = ttf_u16(f, os2.off + 4);
    if (!f.weight) f.weight = 400;
    u32 sel = ttf_u16(f, os2.off + 62);
    f.italic = (sel & 1) != 0 || f.italic;
    if (f.data[os2.off + 32 + 3] == 9) f.fixed_pitch = true; // PANOSE bProportion = monospaced
    bool use_typo = (sel & 0x80) != 0 || (f.ascender == 0 && f.descender == 0);
    if (use_typo && os2.len >= 74) {
      i16 ta = ttf_i16(f, os2.off + 68), td = ttf_i16(f, os2.off + 70);
      if (ta || td) { f.ascender = ta; f.descender = td; f.line_gap = ttf_i16(f, os2.off + 72); }
    }
    if (f.ascender == 0 && f.descender == 0 && os2.len >= 78) {
      f.ascender = (i16)ttf_u16(f, os2.off + 74); f.descender = (i16)-(i32)ttf_u16(f, os2.off + 76); f.line_gap = 0;
    }
    if (v >= 2 && os2.len >= 90) { f.x_height = ttf_i16(f, os2.off + 86); f.cap_height = ttf_i16(f, os2.off + 88); }
  }
  if (post.len >= 16 && ttf_u32(f, post.off + 12)) f.fixed_pitch = true;
  if (f.ascender == 0 && f.descender == 0) { f.ascender = f.y_max; f.descender = f.y_min; }

  parse_names(f);
  if (!(flags & TTF_PARSE_INFO)) {
    parse_cmap(f, find_table(f, "cmap"));
    parse_kern(f, find_table(f, "kern"));
    parse_gpos(f, find_table(f, "GPOS"));
  }
  f.ok = true;
  return true;
}
