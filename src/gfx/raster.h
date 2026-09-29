#pragma once

#include "base/types.h"
#include "base/array.h"

enum PathVerb : u8 { PATH_MOVE = 0, PATH_LINE, PATH_QUAD, PATH_CUBIC, PATH_CLOSE };

struct Path {
  Array<float> pts; // x,y pairs; MOVE/LINE 1 point, QUAD 2, CUBIC 3, CLOSE 0
  Array<u8>    verbs;
  float        start_x = 0, start_y = 0; // of the current contour
  float        cur_x = 0, cur_y = 0;

  void clear() { pts.clear(); verbs.clear(); }
  void move_to(float x, float y) { verbs.push(PATH_MOVE); pts.push(x); pts.push(y); start_x = cur_x = x; start_y = cur_y = y; }
  void line_to(float x, float y) { verbs.push(PATH_LINE); pts.push(x); pts.push(y); cur_x = x; cur_y = y; }
  void quad_to(float cx, float cy, float x, float y) {
    verbs.push(PATH_QUAD); pts.push(cx); pts.push(cy); pts.push(x); pts.push(y); cur_x = x; cur_y = y;
  }
  void cubic_to(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    verbs.push(PATH_CUBIC);
    pts.push(c1x); pts.push(c1y); pts.push(c2x); pts.push(c2y); pts.push(x); pts.push(y);
    cur_x = x; cur_y = y;
  }
  void close() { verbs.push(PATH_CLOSE); cur_x = start_x; cur_y = start_y; }
  bool empty() const { return verbs.len == 0; }
};

bool path_bounds(const Path& p, float& x0, float& y0, float& x1, float& y1);

void path_rect(Path& p, float x, float y, float w, float h);
void path_rounded_rect(Path& p, float x, float y, float w, float h, float r);
void path_circle(Path& p, float cx, float cy, float r);

struct Rasterizer {
  Array<float> acc; // (w + 2) * h signed-area accumulation
  Array<u8>    mask; // w * h coverage, valid after raster_end
  i32          x0 = 0, y0 = 0, w = 0, h = 0; // the pixel box being rasterized
  float        tolerance = 0.025f; // max flattening error in pixels
};

void raster_begin(Rasterizer& r, i32 x0, i32 y0, i32 w, i32 h);

void raster_line(Rasterizer& r, float x0, float y0, float x1, float y1);
void raster_quad(Rasterizer& r, float x0, float y0, float cx, float cy, float x1, float y1);
void raster_cubic(Rasterizer& r, float x0, float y0, float c1x, float c1y, float c2x, float c2y, float x1, float y1);
void raster_path(Rasterizer& r, const Path& p); // every contour is implicitly closed

void raster_end(Rasterizer& r);

struct Canvas;
void canvas_fill_path(Canvas& c, Rasterizer& r, const Path& p, u32 color);

static inline float mx_floorf(float v) { i32 i = (i32)v; return (float)(i - (v < (float)i)); }
static inline float mx_ceilf(float v)  { i32 i = (i32)v; return (float)(i + (v > (float)i)); }
static inline float mx_fabsf(float v)  { return v < 0 ? -v : v; }
