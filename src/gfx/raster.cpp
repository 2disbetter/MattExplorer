#include "gfx/raster.h"
#include "gfx/canvas.h"

#include <string.h>

bool path_bounds(const Path& p, float& x0, float& y0, float& x1, float& y1) {
  if (p.pts.len < 2) return false;
  x0 = x1 = p.pts[0]; y0 = y1 = p.pts[1];
  for (u32 i = 2; i + 1 < p.pts.len; i += 2) {
    float x = p.pts[i], y = p.pts[i + 1];
    x0 = mx_min(x0, x); x1 = mx_max(x1, x);
    y0 = mx_min(y0, y); y1 = mx_max(y1, y);
  }
  return true;
}

void path_rect(Path& p, float x, float y, float w, float h) {
  p.move_to(x, y); p.line_to(x + w, y); p.line_to(x + w, y + h); p.line_to(x, y + h); p.close();
}

static const float kArc = 0.5522847f;

void path_rounded_rect(Path& p, float x, float y, float w, float h, float r) {
  float m = 0.5f * (w < h ? w : h);
  if (r > m) r = m;
  if (r <= 0) { path_rect(p, x, y, w, h); return; }
  float k = kArc * r, x1 = x + w, y1 = y + h;
  p.move_to(x + r, y);
  p.line_to(x1 - r, y);      p.cubic_to(x1 - r + k, y, x1, y + r - k, x1, y + r);
  p.line_to(x1, y1 - r);     p.cubic_to(x1, y1 - r + k, x1 - r + k, y1, x1 - r, y1);
  p.line_to(x + r, y1);      p.cubic_to(x + r - k, y1, x, y1 - r + k, x, y1 - r);
  p.line_to(x, y + r);       p.cubic_to(x, y + r - k, x + r - k, y, x + r, y);
  p.close();
}

void path_circle(Path& p, float cx, float cy, float r) {
  float k = kArc * r;
  p.move_to(cx + r, cy);
  p.cubic_to(cx + r, cy + k, cx + k, cy + r, cx, cy + r);
  p.cubic_to(cx - k, cy + r, cx - r, cy + k, cx - r, cy);
  p.cubic_to(cx - r, cy - k, cx - k, cy - r, cx, cy - r);
  p.cubic_to(cx + k, cy - r, cx + r, cy - k, cx + r, cy);
  p.close();
}

void raster_begin(Rasterizer& r, i32 x0, i32 y0, i32 w, i32 h) {
  r.x0 = x0; r.y0 = y0; r.w = mx_max(w, 0); r.h = mx_max(h, 0);
  u32 n = (u32)(r.w + 2) * (u32)r.h + 2;
  r.acc.resize(n);
  memset(r.acc.data, 0, (usize)n * sizeof(float));
}

static void accumulate_line(Rasterizer& r, float px0, float py0, float px1, float py1) {
  if (mx_fabsf(py0 - py1) <= 1e-6f) return;
  float dir;
  if (py0 < py1) dir = 1.0f;
  else { dir = -1.0f; float t = px0; px0 = px1; px1 = t; t = py0; py0 = py1; py1 = t; }
  const i32   aw   = r.w + 2;
  const float h    = (float)r.h;
  const float dxdy = (px1 - px0) / (py1 - py0);
  float       x    = px0;
  i32         y0   = py0 < 0 ? 0 : (i32)py0;
  if (py0 < 0) x -= py0 * dxdy;
  float  ylim = py1 < h ? py1 : h;
  i32    yend = (i32)mx_ceilf(ylim);
  float* a    = r.acc.data;
  for (i32 y = y0; y < yend; y++) {
    const i32   linestart = y * aw;
    const float ytop      = (float)y > py0 ? (float)y : py0;
    const float ybot      = (float)(y + 1) < py1 ? (float)(y + 1) : py1;
    const float dy        = ybot - ytop;
    if (dy <= 0) { continue; }
    const float xnext = x + dxdy * dy;
    const float d     = dy * dir;
    float x0 = x, x1 = xnext;
    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; }
    const float x0floor = mx_floorf(x0);
    const i32   x0i     = (i32)x0floor;
    const float x1ceil  = mx_ceilf(x1);
    const i32   x1i     = (i32)x1ceil;
    if (x1i <= x0i + 1) {

      const float xmf = 0.5f * (x + xnext) - x0floor;
      a[linestart + x0i] += d - d * xmf;
      a[linestart + x0i + 1] += d * xmf;
    } else {
      const float s   = 1.0f / (x1 - x0);
      const float x0f = x0 - x0floor;
      const float a0  = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
      const float x1f = x1 - x1ceil + 1.0f;
      const float am  = 0.5f * s * x1f * x1f;
      a[linestart + x0i] += d * a0;
      if (x1i == x0i + 2) {
        a[linestart + x0i + 1] += d * (1.0f - a0 - am);
      } else {
        const float a1 = s * (1.5f - x0f);
        a[linestart + x0i + 1] += d * (a1 - a0);
        for (i32 xi = x0i + 2; xi < x1i - 1; xi++) a[linestart + xi] += d * s;
        const float a2 = a1 + (float)(x1i - x0i - 3) * s;
        a[linestart + x1i - 1] += d * (1.0f - a2 - am);
      }
      a[linestart + x1i] += d * am;
    }
    x = xnext;
  }
}

static void clip_x_and_accumulate(Rasterizer& r, float x0, float y0, float x1, float y1, float lo, float hi, i32 level) {
  if (x0 < lo && x1 < lo) { accumulate_line(r, lo, y0, lo, y1); return; }
  if (x0 > hi && x1 > hi) { accumulate_line(r, hi, y0, hi, y1); return; }
  if (level < 2) {
    float b = level == 0 ? lo : hi;
    if ((x0 < b) != (x1 < b)) {
      float t  = (b - x0) / (x1 - x0);
      float ym = y0 + t * (y1 - y0);
      clip_x_and_accumulate(r, x0, y0, b, ym, lo, hi, level + 1);
      clip_x_and_accumulate(r, b, ym, x1, y1, lo, hi, level + 1);
      return;
    }
    clip_x_and_accumulate(r, x0, y0, x1, y1, lo, hi, level + 1);
    return;
  }
  accumulate_line(r, mx_clamp(x0, lo, hi), y0, mx_clamp(x1, lo, hi), y1);
}

void raster_line(Rasterizer& r, float x0, float y0, float x1, float y1) {
  if (r.w <= 0 || r.h <= 0) return;
  x0 -= (float)r.x0; x1 -= (float)r.x0; y0 -= (float)r.y0; y1 -= (float)r.y0;

  float ymin = y0 < y1 ? y0 : y1, ymax = y0 < y1 ? y1 : y0;
  if (ymax <= 0 || ymin >= (float)r.h) return;
  clip_x_and_accumulate(r, x0, y0, x1, y1, 0.0f, (float)r.w, 0);
}

void raster_quad(Rasterizer& r, float x0, float y0, float cx, float cy, float x1, float y1) {

  float ddx = x0 - 2 * cx + x1, ddy = y0 - 2 * cy + y1;
  float dd  = __builtin_sqrtf(ddx * ddx + ddy * ddy);
  i32   n   = (i32)mx_ceilf(__builtin_sqrtf(dd / (4.0f * r.tolerance)));
  n = mx_clamp(n, 1, 64);
  float px = x0, py = y0;
  for (i32 i = 1; i <= n; i++) {
    float t = (float)i / (float)n, mt = 1.0f - t;
    float x = mt * mt * x0 + 2 * mt * t * cx + t * t * x1;
    float y = mt * mt * y0 + 2 * mt * t * cy + t * t * y1;
    raster_line(r, px, py, x, y);
    px = x; py = y;
  }
}

void raster_cubic(Rasterizer& r, float x0, float y0, float c1x, float c1y, float c2x, float c2y, float x1, float y1) {
  float d1x = x0 - 2 * c1x + c2x, d1y = y0 - 2 * c1y + c2y;
  float d2x = c1x - 2 * c2x + x1, d2y = c1y - 2 * c2y + y1;
  float dd  = __builtin_sqrtf(mx_max(d1x * d1x + d1y * d1y, d2x * d2x + d2y * d2y));
  i32   n   = (i32)mx_ceilf(__builtin_sqrtf(0.75f * dd / r.tolerance));
  n = mx_clamp(n, 1, 96);
  float px = x0, py = y0;
  for (i32 i = 1; i <= n; i++) {
    float t = (float)i / (float)n, mt = 1.0f - t;
    float a = mt * mt * mt, b = 3 * mt * mt * t, c = 3 * mt * t * t, d = t * t * t;
    float x = a * x0 + b * c1x + c * c2x + d * x1;
    float y = a * y0 + b * c1y + c * c2y + d * y1;
    raster_line(r, px, py, x, y);
    px = x; py = y;
  }
}

void raster_path(Rasterizer& r, const Path& p) {
  const float* v = p.pts.data;
  u32 k = 0;
  float sx = 0, sy = 0, cx = 0, cy = 0;
  bool open = false;
  for (u32 i = 0; i < p.verbs.len; i++) {
    switch (p.verbs[i]) {
      case PATH_MOVE:
        if (open && (cx != sx || cy != sy)) raster_line(r, cx, cy, sx, sy);
        sx = cx = v[k]; sy = cy = v[k + 1]; k += 2; open = true;
        break;
      case PATH_LINE:
        raster_line(r, cx, cy, v[k], v[k + 1]);
        cx = v[k]; cy = v[k + 1]; k += 2;
        break;
      case PATH_QUAD:
        raster_quad(r, cx, cy, v[k], v[k + 1], v[k + 2], v[k + 3]);
        cx = v[k + 2]; cy = v[k + 3]; k += 4;
        break;
      case PATH_CUBIC:
        raster_cubic(r, cx, cy, v[k], v[k + 1], v[k + 2], v[k + 3], v[k + 4], v[k + 5]);
        cx = v[k + 4]; cy = v[k + 5]; k += 6;
        break;
      case PATH_CLOSE:
        if (open && (cx != sx || cy != sy)) raster_line(r, cx, cy, sx, sy);
        cx = sx; cy = sy;
        break;
    }
  }
  if (open && (cx != sx || cy != sy)) raster_line(r, cx, cy, sx, sy);
}

void raster_end(Rasterizer& r) {
  const i32 aw = r.w + 2;
  r.mask.resize((u32)(r.w * r.h));
  for (i32 y = 0; y < r.h; y++) {
    const float* a   = r.acc.data + (usize)y * aw;
    u8*          out = r.mask.data + (usize)y * r.w;
    float        acc = 0;
    for (i32 x = 0; x < r.w; x++) {
      acc += a[x];
      float c = mx_fabsf(acc);
      out[x] = c >= 1.0f ? 255 : (u8)(c * 255.0f + 0.5f);
    }
  }
}

void canvas_fill_path(Canvas& c, Rasterizer& r, const Path& p, u32 color) {
  float bx0, by0, bx1, by1;
  if (!path_bounds(p, bx0, by0, bx1, by1)) return;
  i32 x0 = mx_max((i32)mx_floorf(bx0), c.clip_x0), y0 = mx_max((i32)mx_floorf(by0), c.clip_y0);
  i32 x1 = mx_min((i32)mx_ceilf(bx1), c.clip_x1), y1 = mx_min((i32)mx_ceilf(by1), c.clip_y1);
  if (x0 >= x1 || y0 >= y1) return;
  raster_begin(r, x0, y0, x1 - x0, y1 - y0);
  raster_path(r, p);
  raster_end(r);
  canvas_blit_mask(c, r.mask.data, r.w, r.h, r.w, x0, y0, color);
}
