#pragma once

#include "base/types.h"

struct Canvas {
  u32* px;
  i32  w, h;
  i32  stride; // in pixels
  i32  clip_x0, clip_y0, clip_x1, clip_y1; // half-open, in pixels
};

static inline u32 rgba(u32 r, u32 g, u32 b, u32 a = 255) {
  if (a == 255) return 0xFF000000u | (r << 16) | (g << 8) | b;
  r = (r * a + 127) / 255; g = (g * a + 127) / 255; b = (b * a + 127) / 255;
  return (a << 24) | (r << 16) | (g << 8) | b;
}
static inline u32 rgb_hex(u32 hex) { return 0xFF000000u | (hex & 0xFFFFFF); }
static inline u32 rgba_hex(u32 hex, u32 a) { return rgba((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, a); }

static inline u32 mul_lanes(u32 packed, u32 a) {
  u32 t = packed * a + 0x00800080u;
  return ((t + ((t >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
}

static inline u32 blend_over(u32 dst, u32 src) {
  u32 sa = src >> 24;
  if (sa == 255) return src;
  if (sa == 0) return dst;
  u32 ia = 255 - sa;
  u32 rb = mul_lanes(dst & 0x00FF00FF, ia) + (src & 0x00FF00FF);
  u32 ag = mul_lanes((dst >> 8) & 0x00FF00FF, ia) + ((src >> 8) & 0x00FF00FF);
  return (rb & 0x00FF00FF) | ((ag & 0x00FF00FF) << 8);
}

void gfx_init();

static inline void canvas_reset_clip(Canvas& c) { c.clip_x0 = 0; c.clip_y0 = 0; c.clip_x1 = c.w; c.clip_y1 = c.h; }
static inline void canvas_set_clip(Canvas& c, i32 x, i32 y, i32 w, i32 h) {
  c.clip_x0 = mx_max(x, 0); c.clip_y0 = mx_max(y, 0);
  c.clip_x1 = mx_min(x + w, c.w); c.clip_y1 = mx_min(y + h, c.h);
  if (c.clip_x1 < c.clip_x0) c.clip_x1 = c.clip_x0;
  if (c.clip_y1 < c.clip_y0) c.clip_y1 = c.clip_y0;
}

static inline void canvas_push_clip(Canvas& c, i32 x, i32 y, i32 w, i32 h, i32 saved[4]) {
  saved[0] = c.clip_x0; saved[1] = c.clip_y0; saved[2] = c.clip_x1; saved[3] = c.clip_y1;
  c.clip_x0 = mx_max(c.clip_x0, x); c.clip_y0 = mx_max(c.clip_y0, y);
  c.clip_x1 = mx_min(c.clip_x1, x + w); c.clip_y1 = mx_min(c.clip_y1, y + h);
  if (c.clip_x1 < c.clip_x0) c.clip_x1 = c.clip_x0;
  if (c.clip_y1 < c.clip_y0) c.clip_y1 = c.clip_y0;
}
static inline void canvas_pop_clip(Canvas& c, const i32 saved[4]) {
  c.clip_x0 = saved[0]; c.clip_y0 = saved[1]; c.clip_x1 = saved[2]; c.clip_y1 = saved[3];
}

static inline void canvas_fill(Canvas& c, i32 x, i32 y, i32 w, i32 h, u32 color) {
  i32 x0 = mx_max(x, c.clip_x0), y0 = mx_max(y, c.clip_y0);
  i32 x1 = mx_min(x + w, c.clip_x1), y1 = mx_min(y + h, c.clip_y1);
  if (x0 >= x1 || y0 >= y1) return;
  if ((color >> 24) == 255) {
    for (i32 yy = y0; yy < y1; yy++) {
      u32* row = c.px + (usize)yy * c.stride;
      for (i32 xx = x0; xx < x1; xx++) row[xx] = color;
    }
  } else {
    for (i32 yy = y0; yy < y1; yy++) {
      u32* row = c.px + (usize)yy * c.stride;
      for (i32 xx = x0; xx < x1; xx++) row[xx] = blend_over(row[xx], color);
    }
  }
}

static inline void canvas_clear(Canvas& c, u32 color) { canvas_fill(c, 0, 0, c.w, c.h, color); }

static inline void canvas_stroke(Canvas& c, i32 x, i32 y, i32 w, i32 h, u32 color) {
  canvas_fill(c, x, y, w, 1, color);
  canvas_fill(c, x, y + h - 1, w, 1, color);
  canvas_fill(c, x, y, 1, h, color);
  canvas_fill(c, x + w - 1, y, 1, h, color);
}
static inline void canvas_hline(Canvas& c, i32 x, i32 y, i32 w, u32 color) { canvas_fill(c, x, y, w, 1, color); }
static inline void canvas_vline(Canvas& c, i32 x, i32 y, i32 h, u32 color) { canvas_fill(c, x, y, 1, h, color); }

void canvas_fill_rounded(Canvas& c, i32 x, i32 y, i32 w, i32 h, i32 r, u32 color);

void canvas_blit_mask(Canvas& c, const u8* mask, i32 w, i32 h, i32 stride, i32 x, i32 y, u32 color);

void canvas_blit_argb(Canvas& c, const u32* px, i32 w, i32 h, i32 x, i32 y);
