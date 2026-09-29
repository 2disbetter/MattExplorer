#include "gfx/canvas.h"

static u16 g_to_linear[256]; // sRGB byte -> linear, 0..4095
static u8  g_to_srgb[4096]; // linear 0..4095 -> sRGB byte
static bool g_gfx_ready = false;

static double root5(double v) { // y^5 = v, v in [0,1]
  double lo = 0.0, hi = 1.0;
  for (int i = 0; i < 48; i++) {
    double m = 0.5 * (lo + hi), m5 = m * m * m * m * m;
    if (m5 < v) lo = m; else hi = m;
  }
  return 0.5 * (lo + hi);
}

void gfx_init() {
  if (g_gfx_ready) return;
  double lin[256];
  for (u32 i = 0; i < 256; i++) {
    double s = i / 255.0;
    double x = s <= 0.04045 ? s / 12.92 : (s + 0.055) / 1.055;
    lin[i] = s <= 0.04045 ? x : root5(x * x) * x * x; // x^2.4 = x^2 * (x^2)^(1/5)
    g_to_linear[i] = (u16)(lin[i] * 4095.0 + 0.5);
  }
  u32 s = 0;
  for (u32 i = 0; i < 4096; i++) {
    double l = i / 4095.0;
    while (s < 255 && l > 0.5 * (lin[s] + lin[s + 1])) s++;
    g_to_srgb[i] = (u8)s;
  }
  g_gfx_ready = true;
}

void canvas_blit_mask(Canvas& c, const u8* mask, i32 w, i32 h, i32 stride, i32 x, i32 y, u32 color) {
  i32 x0 = mx_max(x, c.clip_x0), y0 = mx_max(y, c.clip_y0);
  i32 x1 = mx_min(x + w, c.clip_x1), y1 = mx_min(y + h, c.clip_y1);
  if (x0 >= x1 || y0 >= y1) return;
  u32 ca = color >> 24;
  if (ca == 0) return;

  u32 cr = (color >> 16) & 255, cg = (color >> 8) & 255, cb = color & 255;
  if (ca != 255) { cr = mx_min(255u, cr * 255 / ca); cg = mx_min(255u, cg * 255 / ca); cb = mx_min(255u, cb * 255 / ca); }
  i32 lr = g_to_linear[cr], lg = g_to_linear[cg], lb = g_to_linear[cb];

  for (i32 yy = y0; yy < y1; yy++) {
    u32*      row = c.px + (usize)yy * c.stride;
    const u8* m   = mask + (usize)(yy - y) * stride + (x0 - x);
    for (i32 xx = x0; xx < x1; xx++, m++) {
      u32 a = *m;
      if (!a) continue;
      if (ca != 255) a = (a * ca + 127) / 255;
      u32 d = row[xx];
      if ((d >> 24) == 255) {
        if (a == 255) { row[xx] = 0xFF000000u | (cr << 16) | (cg << 8) | cb; continue; }
        i32 dr = g_to_linear[(d >> 16) & 255], dg = g_to_linear[(d >> 8) & 255], db = g_to_linear[d & 255];
        dr += ((lr - dr) * (i32)a + 127) / 255;
        dg += ((lg - dg) * (i32)a + 127) / 255;
        db += ((lb - db) * (i32)a + 127) / 255;
        row[xx] = 0xFF000000u | ((u32)g_to_srgb[dr] << 16) | ((u32)g_to_srgb[dg] << 8) | g_to_srgb[db];
      } else {
        row[xx] = blend_over(d, rgba(cr, cg, cb, a));
      }
    }
  }
}

static void corner_row(Canvas& c, i32 px, i32 py, i32 r, u32 color, bool left, i32 dy_row) {

  u8 cov[64];
  i32 n = mx_min(r, 64);
  for (i32 i = 0; i < n; i++) cov[i] = 0;
  u32 acc[64] = {};
  for (i32 s = 0; s < 4; s++) {
    float yy  = (float)dy_row + (s + 0.5f) / 4.0f; // distance from the corner box's outer edge
    float dyc = (float)r - yy; // distance from circle centre
    float rr  = (float)r * (float)r - dyc * dyc;
    float xr  = rr > 0 ? __builtin_sqrtf(rr) : 0; // half-width of the circle at this height

    for (i32 k = 0; k < n; k++) {
      float lo = (float)k, hi = (float)k + 1.0f;
      float ov = mx_clamp(xr, lo, hi) - lo;
      acc[k] += (u32)(ov * 255.0f + 0.5f);
    }
  }
  for (i32 k = 0; k < n; k++) cov[k] = (u8)(acc[k] / 4);

  u8 line[64];
  for (i32 k = 0; k < n; k++) line[left ? (r - 1 - k) : k] = cov[k];
  canvas_blit_mask(c, line, n, 1, n, px, py, color);
}

void canvas_fill_rounded(Canvas& c, i32 x, i32 y, i32 w, i32 h, i32 r, u32 color) {
  r = mx_min(r, mx_min(w, h) / 2);
  if (r <= 0 || r > 64) { canvas_fill(c, x, y, w, h, color); return; }

  canvas_fill(c, x, y + r, w, h - 2 * r, color);
  for (i32 i = 0; i < r; i++) {
    i32 ty = y + i, by = y + h - 1 - i;
    canvas_fill(c, x + r, ty, w - 2 * r, 1, color);
    canvas_fill(c, x + r, by, w - 2 * r, 1, color);
    corner_row(c, x, ty, r, color, true, i);
    corner_row(c, x + w - r, ty, r, color, false, i);
    corner_row(c, x, by, r, color, true, i);
    corner_row(c, x + w - r, by, r, color, false, i);
  }
}

void canvas_blit_argb(Canvas& c, const u32* px, i32 w, i32 h, i32 x, i32 y) {
  i32 x0 = mx_max(x, c.clip_x0), y0 = mx_max(y, c.clip_y0);
  i32 x1 = mx_min(x + w, c.clip_x1), y1 = mx_min(y + h, c.clip_y1);
  if (x0 >= x1 || y0 >= y1) return;
  for (i32 yy = y0; yy < y1; yy++) {
    u32* row = c.px + (usize)yy * c.stride;
    const u32* src = px + (usize)(yy - y) * w + (x0 - x);
    for (i32 xx = x0; xx < x1; xx++, src++) row[xx] = blend_over(row[xx], *src);
  }
}
