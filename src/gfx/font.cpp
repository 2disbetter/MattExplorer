#include "gfx/font.h"
#include "gfx/font_db.h"
#include "gfx/canvas.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

bool font_face_open(FontFace& f, const char* path, u32 index) {
  font_face_close(f);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) { strcpy(f.ttf.err, "cannot open"); return false; }
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < 12 || st.st_size > 0x7FFFFFFF) { close(fd); strcpy(f.ttf.err, "bad file"); return false; }
  void* m = mmap(nullptr, (usize)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (m == MAP_FAILED) { strcpy(f.ttf.err, "mmap failed"); return false; }
  f.map = (u8*)m; f.map_size = (usize)st.st_size; f.index = index;
  usize n = strlen(path); if (n >= sizeof f.path) n = sizeof f.path - 1;
  memcpy(f.path, path, n); f.path[n] = 0;
  if (!ttf_parse(f.ttf, f.map, (u32)f.map_size, index)) { munmap(f.map, f.map_size); f.map = nullptr; f.map_size = 0; return false; }
  return true;
}

void font_face_close(FontFace& f) {
  if (f.map) { munmap(f.map, f.map_size); f.map = nullptr; f.map_size = 0; }
  f.ttf.cmap.release(); f.ttf.cmap_glyphs.release(); f.ttf.gpos_pairpos.release();
  f.ttf.ok = false; f.ttf.data = nullptr; f.ttf.size = 0;
  f.path[0] = 0;
}

static const u32 kEmpty = 0xFFFFFFFFu;

static void cache_clear(Font& f, u32 cap) {
  f.keys.resize(cap); f.slots.resize(cap);
  for (u32 i = 0; i < cap; i++) f.keys[i] = kEmpty;
  f.glyphs.clear(); f.atlas.clear(); f.count = 0;
}

void font_set_size(Font& f, const FontFace& face, float px) {
  f.face  = &face;
  f.px    = px;
  f.scale = px / (float)face.ttf.units_per_em;
  f.ascent   = (float)face.ttf.ascender * f.scale;
  f.descent  = (float)-face.ttf.descender * f.scale;
  f.line_gap = (float)face.ttf.line_gap * f.scale;
  f.line_height = (i32)mx_ceilf(f.ascent + f.descent + f.line_gap);
  cache_clear(f, 512);
}

float font_advance(const Font& f, u16 glyph) { return (float)ttf_advance(f.face->ttf, glyph) * f.scale; }
float font_kern(Font& f, u16 left, u16 right) {
  const TtfFace& tf = f.face->ttf;
  if (!tf.kern_count && !tf.gpos_pairpos.len) return 0;
  u32 key = ((u32)left << 16) | right;
  if (!f.kern_keys.len) {
    f.kern_keys.resize(256); f.kern_vals.resize(256);
    for (u32 i = 0; i < 256; i++) f.kern_keys[i] = kEmpty;
  }
  u32 cap = f.kern_keys.len, h = (key * 2654435761u) & (cap - 1);
  while (f.kern_keys[h] != kEmpty) {
    if (f.kern_keys[h] == key) return (float)f.kern_vals[h] * f.scale;
    h = (h + 1) & (cap - 1);
  }
  i32 v = ttf_kern(tf, left, right);
  f.kern_keys[h] = key; f.kern_vals[h] = v;
  if (++f.kern_count * 2 > cap) {
    Array<u32> ok; Array<i32> ov;
    ok = static_cast<Array<u32>&&>(f.kern_keys); ov = static_cast<Array<i32>&&>(f.kern_vals);
    f.kern_keys.resize(cap * 2); f.kern_vals.resize(cap * 2);
    for (u32 i = 0; i < cap * 2; i++) f.kern_keys[i] = kEmpty;
    for (u32 i = 0; i < ok.len; i++) {
      if (ok[i] == kEmpty) continue;
      u32 j = (ok[i] * 2654435761u) & (cap * 2 - 1);
      while (f.kern_keys[j] != kEmpty) j = (j + 1) & (cap * 2 - 1);
      f.kern_keys[j] = ok[i]; f.kern_vals[j] = ov[i];
    }
  }
  return (float)v * f.scale;
}
usize font_cache_bytes(const Font& f) {
  return f.atlas.bytes() + f.glyphs.bytes() + f.keys.bytes() + f.slots.bytes() + f.kern_keys.bytes() + f.kern_vals.bytes();
}

static void cache_grow(Font& f) {
  Array<u32> ok, os;
  ok = static_cast<Array<u32>&&>(f.keys); os = static_cast<Array<u32>&&>(f.slots);
  u32 cap = ok.len * 2;
  f.keys.resize(cap); f.slots.resize(cap);
  for (u32 i = 0; i < cap; i++) f.keys[i] = kEmpty;
  for (u32 i = 0; i < ok.len; i++) {
    if (ok[i] == kEmpty) continue;
    u32 h = (ok[i] * 2654435761u) & (cap - 1);
    while (f.keys[h] != kEmpty) h = (h + 1) & (cap - 1);
    f.keys[h] = ok[i]; f.slots[h] = os[i];
  }
}

static Glyph rasterize(Font& f, Rasterizer& r, Path& path, u16 glyph, u32 subpx) {
  Glyph g = { 0, 0, 0, 0, 0 };
  path.clear();
  if (!ttf_glyph_path(f.face->ttf, glyph, path, f.scale, f.scale, (float)subpx * 0.25f, 0.0f)) return g;
  float bx0, by0, bx1, by1;
  if (!path_bounds(path, bx0, by0, bx1, by1)) return g;
  i32 x0 = (i32)mx_floorf(bx0), y0 = (i32)mx_floorf(by0);
  i32 x1 = (i32)mx_ceilf(bx1), y1 = (i32)mx_ceilf(by1);
  i32 w = x1 - x0, h = y1 - y0;
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return g;
  raster_begin(r, x0, y0, w, h);
  raster_path(r, path);
  raster_end(r);
  g.x0 = (i16)x0; g.y0 = (i16)y0; g.w = (u16)w; g.h = (u16)h; g.off = f.atlas.len;
  memcpy(f.atlas.push_n((u32)(w * h)), r.mask.data, (usize)w * h);
  return g;
}

const Glyph& font_glyph(Font& f, Rasterizer& r, Path& scratch, u16 glyph, u32 subpx) {
  u32 key = ((u32)glyph << 2) | (subpx & 3);
  u32 cap = f.keys.len, h = (key * 2654435761u) & (cap - 1);
  while (f.keys[h] != kEmpty) {
    if (f.keys[h] == key) return f.glyphs[f.slots[h]];
    h = (h + 1) & (cap - 1);
  }
  Glyph g = rasterize(f, r, scratch, glyph, subpx & 3);
  f.keys[h] = key; f.slots[h] = f.glyphs.len;
  f.glyphs.push(g);
  f.count++;
  if (f.count * 2 > cap) cache_grow(f);
  return f.glyphs.last();
}

static void cp_cache_clear(Text& t, u32 cap) {
  t.cp_keys.resize(cap); t.cp_vals.resize(cap);
  for (u32 i = 0; i < cap; i++) t.cp_keys[i] = kEmpty;
  t.cp_count = 0;
}

static void cp_cache_put(Text& t, u32 cp, u32 val) {
  if (!t.cp_keys.len) cp_cache_clear(t, 256);
  if ((t.cp_count + 1) * 2 > t.cp_keys.len) {
    Array<u32> ok, ov;
    ok = static_cast<Array<u32>&&>(t.cp_keys); ov = static_cast<Array<u32>&&>(t.cp_vals);
    cp_cache_clear(t, ok.len * 2);
    for (u32 i = 0; i < ok.len; i++) if (ok[i] != kEmpty) cp_cache_put(t, ok[i], ov[i]);
  }
  u32 cap = t.cp_keys.len, h = (cp * 2654435761u) & (cap - 1);
  while (t.cp_keys[h] != kEmpty && t.cp_keys[h] != cp) h = (h + 1) & (cap - 1);
  if (t.cp_keys[h] == kEmpty) t.cp_count++;
  t.cp_keys[h] = cp; t.cp_vals[h] = val;
}

static bool cp_cache_get(const Text& t, u32 cp, u32& val) {
  if (!t.cp_keys.len) return false;
  u32 cap = t.cp_keys.len, h = (cp * 2654435761u) & (cap - 1);
  while (t.cp_keys[h] != kEmpty) {
    if (t.cp_keys[h] == cp) { val = t.cp_vals[h]; return true; }
    h = (h + 1) & (cap - 1);
  }
  return false;
}

static bool text_add_face(Text& t, const char* path, u32 index) {
  if (t.num_fonts >= TEXT_MAX_FONTS) return false;
  FontFace& f = t.faces[t.num_fonts];
  if (!font_face_open(f, path, index)) return false;
  if (!f.ttf.has_outlines) {
    font_face_close(f);
    strcpy(f.ttf.err, f.ttf.cff_len ? "CFF outlines are not supported yet" : "no glyf outlines");
    return false;
  }
  font_set_size(t.fonts[t.num_fonts], f, t.px);
  t.num_fonts++;
  return true;
}

static void text_lookup(Text& t, u32 cp, u32& font, u16& glyph) {
  u32 v;
  if (cp_cache_get(t, cp, v)) { font = v >> 16; glyph = (u16)v; if (font == 0xFFFF) { font = 0; glyph = 0; } return; }
  for (u32 i = 0; i < t.num_fonts; i++) {
    u16 g = ttf_glyph_index(t.faces[i].ttf, cp);
    if (g) { cp_cache_put(t, cp, (i << 16) | g); font = i; glyph = g; return; }
  }
  if (t.db && t.num_fonts < TEXT_MAX_FONTS) {

    i32 skip[TEXT_MAX_FONTS]; u32 nskip = 0;
    for (u32 i = 0; i < t.num_fonts; i++) {
      for (u32 e = 0; e < t.db->entries.len; e++) {
        const FontEntry& fe = t.db->entries[e];
        if (fe.index == t.faces[i].index && !strcmp(t.db->strings.data + fe.path, t.faces[i].path)) { skip[nskip++] = (i32)e; break; }
      }
    }
    i32 e = font_db_find_glyph(*t.db, cp, skip, nskip);
    if (e >= 0 && text_add_face(t, font_db_path(*t.db, e), t.db->entries[(u32)e].index)) {
      t.fallback_loads++;
      u32 i = t.num_fonts - 1;
      u16 g = ttf_glyph_index(t.faces[i].ttf, cp);
      cp_cache_put(t, cp, (i << 16) | g);
      font = i; glyph = g;
      return;
    }
  }
  cp_cache_put(t, cp, 0xFFFF0000u);
  font = 0; glyph = 0;
}

bool text_open(Text& t, FontDb* db, const char* name, float px) {
  text_close(t);
  t.db = db; t.px = px;
  if (strchr(name, '/')) {
    if (!text_add_face(t, name, 0)) return false;
  } else {
    if (!db) return false;
    char family[96]; u16 weight; bool italic;
    font_db_parse_spec(name, family, sizeof family, weight, italic);
    i32 e = font_db_match(*db, family, weight, italic);
    if (e < 0 || !text_add_face(t, font_db_path(*db, e), db->entries[(u32)e].index)) return false;
  }
  cp_cache_clear(t, 256);
  return true;
}

void text_close(Text& t) {
  for (u32 i = 0; i < t.num_fonts; i++) {
    font_face_close(t.faces[i]);
    Font& f = t.fonts[i];
    f.keys.release(); f.slots.release(); f.glyphs.release(); f.atlas.release(); f.face = nullptr;
    f.kern_keys.release(); f.kern_vals.release(); f.kern_count = 0;
  }
  t.num_fonts = 0;
  t.cp_keys.release(); t.cp_vals.release(); t.cp_count = 0;
  t.fallback_loads = 0;
}

void text_set_size(Text& t, float px) {
  t.px = px;
  for (u32 i = 0; i < t.num_fonts; i++) font_set_size(t.fonts[i], t.faces[i], px);
}

struct GlyphIter {
  Text& t; Str s; u32 i = 0; u32 font = 0; u16 glyph = 0, prev_glyph = 0; u32 prev_font = 0xFFFF;
  u32   start = 0; // byte offset of the current code point
  float adv = 0;
  float kern = 0;
  bool next() {
    if (i >= s.n) return false;
    start = i;
    u32 cp = utf8_next(s, i);
    if (cp == '\t') cp = ' ';
    text_lookup(t, cp, font, glyph);
    kern = (prev_font == font && prev_glyph) ? font_kern(t.fonts[font], prev_glyph, glyph) : 0;
    adv  = font_advance(t.fonts[font], glyph);
    prev_glyph = glyph; prev_font = font;
    return true;
  }
};

float text_width(Text& t, Str s) {
  if (!t.num_fonts) return 0;
  float x = 0;
  GlyphIter it = { t, s };
  while (it.next()) x += it.kern + it.adv;
  return x;
}

u32 text_fit(Text& t, Str s, float max_w) {
  if (!t.num_fonts) return 0;
  float x = 0;
  GlyphIter it = { t, s };
  while (it.next()) {
    x += it.kern + it.adv;
    if (x > max_w) return it.start;
  }
  return s.n;
}

float text_draw(Canvas& c, Text& t, Str s, float x, i32 baseline, u32 color) {
  if (!t.num_fonts) return 0;
  float x0 = x;
  GlyphIter it = { t, s };
  while (it.next()) {
    x += it.kern;
    if (x >= (float)c.clip_x1) { x += it.adv; continue; } // past the clip: nothing more to draw, just measure
    i32   ix   = (i32)mx_floorf(x);
    u32   sub  = (u32)((x - (float)ix) * 4.0f) & 3;
    const Glyph& g = font_glyph(t.fonts[it.font], t.raster, t.path, it.glyph, sub);
    if (g.w && ix + g.x0 + g.w > c.clip_x0)
      canvas_blit_mask(c, t.fonts[it.font].atlas.data + g.off, g.w, g.h, g.w, ix + g.x0, baseline + g.y0, color);
    x += it.adv;
  }
  return x - x0;
}

float text_draw_elided(Canvas& c, Text& t, Str s, float x, i32 baseline, float max_w, u32 color) {
  if (!t.num_fonts || max_w <= 0) return 0;
  float w = text_width(t, s);
  if (w <= max_w) return text_draw(c, t, s, x, baseline, color);
  static const char kEllipsis[] = "\xe2\x80\xa6"; // U+2026
  float ew = text_width(t, kEllipsis);
  u32   n  = text_fit(t, s, max_w - ew);
  while (n > 0 && ((u8)s.p[n - 1] == ' ')) n--; // no dangling space before the ellipsis
  float d = text_draw(c, t, Str(s.p, n), x, baseline, color);
  return d + text_draw(c, t, kEllipsis, x + d, baseline, color);
}
