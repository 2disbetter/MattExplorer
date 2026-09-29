#pragma once

#include "base/types.h"
#include "base/array.h"
#include "base/str.h"
#include "gfx/ttf.h"
#include "gfx/raster.h"

struct Canvas;
struct FontDb;

struct FontFace {
  TtfFace ttf;
  u8*     map = nullptr;
  usize   map_size = 0;
  u32     index = 0;
  char    path[256] = {};
};

bool font_face_open(FontFace& f, const char* path, u32 index = 0); // mmap + parse; false and f.ttf.err on failure
void font_face_close(FontFace& f);

struct Glyph {
  i16 x0, y0; // mask origin relative to (floor(pen x), baseline)
  u16 w, h; // mask size; 0x0 for blank glyphs
  u32 off; // into Font::atlas
};

struct Font {
  const FontFace* face = nullptr;
  float px = 0, scale = 0; // pixels per em; scale = px / units_per_em
  float ascent = 0, descent = 0, line_gap = 0; // pixels; ascent and descent positive
  i32   line_height = 0; // ascent + descent + line_gap, rounded up

  Array<u32>   keys;
  Array<u32>   slots;
  Array<Glyph> glyphs;
  Array<u8>    atlas;
  u32          count = 0;

  Array<u32>   kern_keys;
  Array<i32>   kern_vals;
  u32          kern_count = 0;
};

void         font_set_size(Font& f, const FontFace& face, float px); // (re)initialises and clears the cache
const Glyph& font_glyph(Font& f, Rasterizer& r, Path& scratch, u16 glyph, u32 subpx); // subpx 0..3; rasterizes on a miss
float        font_advance(const Font& f, u16 glyph);
float        font_kern(Font& f, u16 left, u16 right);
usize        font_cache_bytes(const Font& f);

enum { TEXT_MAX_FONTS = 8 };

struct Text {
  FontDb*    db = nullptr;
  FontFace   faces[TEXT_MAX_FONTS];
  Font       fonts[TEXT_MAX_FONTS];
  u32        num_fonts = 0;
  float      px = 0;
  Rasterizer raster;
  Path       path;

  Array<u32> cp_keys;
  Array<u32> cp_vals;
  u32        cp_count = 0;
  u32        fallback_loads = 0; // stats
};

bool  text_open(Text& t, FontDb* db, const char* name, float px);
void  text_close(Text& t);
void  text_set_size(Text& t, float px); // clears every glyph cache

static inline i32   text_line_height(const Text& t) { return t.num_fonts ? t.fonts[0].line_height : 0; }
static inline float text_ascent(const Text& t)      { return t.num_fonts ? t.fonts[0].ascent : 0; }
static inline float text_descent(const Text& t)     { return t.num_fonts ? t.fonts[0].descent : 0; }
static inline const TtfFace* text_primary(const Text& t) { return t.num_fonts ? &t.faces[0].ttf : nullptr; }

float text_width(Text& t, Str s);

u32   text_fit(Text& t, Str s, float max_w);

float text_draw(Canvas& c, Text& t, Str s, float x, i32 baseline, u32 color);

float text_draw_elided(Canvas& c, Text& t, Str s, float x, i32 baseline, float max_w, u32 color);
