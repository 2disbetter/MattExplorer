#pragma once

#include "base/types.h"
#include "base/array.h"
#include "base/str.h"

struct Path;

struct TtfCmapRange {
  u32 start, end; // inclusive codepoint range
  u32 value;
  u8  indexed;
};

struct TtfFace {
  const u8* data = nullptr;
  u32       size = 0;
  u32       base = 0; // offset of this face's table directory

  u32 glyf = 0, glyf_len = 0, loca = 0, loca_len = 0, hmtx = 0, hmtx_len = 0;
  u32 kern = 0, kern_len = 0, gpos = 0, gpos_len = 0, cff = 0, cff_len = 0;
  u32 name = 0, name_len = 0, os2 = 0, os2_len = 0;

  u16 units_per_em = 1000;
  i16 ascender = 0, descender = 0, line_gap = 0; // hhea (or OS/2 typo when hhea is empty)
  i16 x_min = 0, y_min = 0, x_max = 0, y_max = 0;
  i16 x_height = 0, cap_height = 0; // OS/2 v2+, 0 if unknown
  u16 num_glyphs = 0, num_hmetrics = 0;
  u16 weight = 400;
  bool long_loca = false, italic = false, fixed_pitch = false, has_outlines = false;

  Array<TtfCmapRange> cmap;
  Array<u16>          cmap_glyphs;

  u32 kern_pairs = 0, kern_count = 0;
  Array<u32> gpos_pairpos; // offsets of PairPos subtables, in lookup order

  char family[96] = {};
  char style[48]  = {};
  bool ok = false;
  char err[64] = {};
};

enum : u32 { TTF_PARSE_INFO = 1 };

u32  ttf_face_count(const u8* data, u32 size);

bool ttf_parse(TtfFace& f, const u8* data, u32 size, u32 index, u32 flags = 0);

u16  ttf_glyph_index(const TtfFace& f, u32 codepoint); // 0 = .notdef
u16  ttf_advance(const TtfFace& f, u16 glyph); // font units
i16  ttf_kern(const TtfFace& f, u16 left, u16 right); // font units, 0 if none (kern, then GPOS)

bool ttf_glyph_path(const TtfFace& f, u16 glyph, Path& p, float sx, float sy, float tx, float ty);

static inline u16 ttf_u16(const TtfFace& f, u32 off) {
  return off + 2 <= f.size ? (u16)((f.data[off] << 8) | f.data[off + 1]) : 0;
}
static inline i16 ttf_i16(const TtfFace& f, u32 off) { return (i16)ttf_u16(f, off); }
static inline u32 ttf_u32(const TtfFace& f, u32 off) {
  return off + 4 <= f.size ? ((u32)f.data[off] << 24) | ((u32)f.data[off + 1] << 16) | ((u32)f.data[off + 2] << 8) | f.data[off + 3] : 0;
}
