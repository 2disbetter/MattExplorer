#include "ui/ui.h"
#include "platform/wayland/proto.h" // CursorShape values
#include "platform/xkb/keysym.h"
#include "platform/xkb/keymap.h"

#include <stdio.h>
#include <string.h>

enum { BTN_LEFT = 0x110, BTN_RIGHT = 0x111, BTN_MIDDLE = 0x112 };
static const i64 kTooltipDelayMs = 600;
static const i64 kDoubleClickMs  = 400;

u32 ui_id(const char* name, u32 salt) {
  u32 h = 2166136261u;
  for (const char* p = name; *p; p++) { h ^= (u8)*p; h *= 16777619u; }
  for (u32 i = 0; i < 4; i++) { h ^= (salt >> (i * 8)) & 255; h *= 16777619u; }
  return h ? h : 1;
}

void ui_begin(Ui& ui, Canvas& c, Text& text, Text* small, const UiTheme& theme, float scale, i64 now_ms) {
  ui.c = &c; ui.text = &text; ui.small = small ? small : &text; ui.t = &theme; ui.s = scale; ui.now_ms = now_ms;
  ui.hot_next = 0;
  ui.cursor = CURSOR_DEFAULT;
  ui.want_redraw = false;
  ui.wake_at_ms = 0;
  ui.tip_hover_id = 0;
}

static void draw_tooltip(Ui& ui) {
  if (!ui.tip_id || !ui.tip_text[0]) return;
  Canvas& c = *ui.c;
  Text& t = *ui.small;
  i32 pad = ui_px(ui, 6);
  float tw = text_width(t, ui.tip_text);
  i32 w = (i32)tw + pad * 2, h = text_line_height(t) + pad * 2;
  i32 x = ui.tip_anchor.x + ui_px(ui, 12), y = rect_y1(ui.tip_anchor) + ui_px(ui, 4);
  if (x + w > c.w - pad) x = c.w - pad - w;
  if (y + h > c.h - pad) y = ui.tip_anchor.y - h - ui_px(ui, 4);
  x = mx_max(x, pad); y = mx_max(y, pad);
  canvas_fill_rounded(c, x, y, w, h, ui_px(ui, 4), rgb_hex(ui.t->tooltip));
  canvas_stroke(c, x, y, w, h, rgba_hex(ui.t->border, 200));
  text_draw(c, t, ui.tip_text, (float)(x + pad), ui_baseline(ui, t, y, h), rgb_hex(ui.t->text));
}

void ui_end(Ui& ui) {

  if (ui.tip_hover_id) {
    if (ui.tip_hover_id != ui.tip_candidate) { ui.tip_candidate = ui.tip_hover_id; ui.tip_hover_since = ui.now_ms; }
    if (ui.tip_id != ui.tip_hover_id) {
      if (ui.now_ms - ui.tip_hover_since >= kTooltipDelayMs) { ui.tip_id = ui.tip_hover_id; ui.want_redraw = true; }
      else ui.wake_at_ms = ui.tip_hover_since + kTooltipDelayMs;
    }
  } else {
    if (ui.tip_id) ui.want_redraw = true; // it was showing: erase it
    ui.tip_id = 0; ui.tip_candidate = 0;
  }
  if (ui.tip_id && ui.tip_id == ui.tip_hover_id) draw_tooltip(ui);
  ui.hot = ui.hot_next;

  ui.clicks.clear();
  ui.scroll_dx = ui.scroll_dy = 0;
}

void ui_input_motion(Ui& ui, float x, float y, i64 now_ms) {
  ui.mx = x; ui.my = y; ui.mouse_inside = true;
  if (ui.tip_id) { // a shown tooltip survives motion inside its anchor
    if (!rect_has(ui.tip_anchor, x, y)) ui.tip_id = 0;
  } else {
    ui.tip_hover_since = now_ms;
  }
}
void ui_input_leave(Ui& ui) { ui.mouse_inside = false; ui.mx = ui.my = -1; }

void ui_input_button(Ui& ui, float x, float y, u32 button, bool pressed, u32 mods, i64 now_ms) {
  UiClick k = { x, y, button, mods, (u8)pressed, 1, 0 };
  ui.mx = x; ui.my = y;
  if (button == 0x110) ui.left_down = pressed;
  if (pressed) {
    bool dbl = button == ui.last_press_button && now_ms - ui.last_press_ms < kDoubleClickMs &&
               mx_fabsf(x - ui.last_press_x) < 4 * ui.s && mx_fabsf(y - ui.last_press_y) < 4 * ui.s && ui.last_press_count == 1;
    k.count = dbl ? 2 : 1;
    ui.last_press_ms = now_ms; ui.last_press_x = x; ui.last_press_y = y; ui.last_press_button = button; ui.last_press_count = k.count;
  }
  ui.clicks.push(k);
  ui.tip_id = 0; ui.tip_candidate = 0;
}

void ui_input_scroll(Ui& ui, float x, float y, float dx, float dy, u32 mods) {
  ui.scroll_x = x; ui.scroll_y = y; ui.scroll_dx += dx; ui.scroll_dy += dy; ui.scroll_mods = mods;
}

i32 ui_baseline(const Ui& ui, Text& t, i32 y, i32 h) {
  float asc = text_ascent(t), desc = text_descent(t);
  return y + (i32)(((float)h - (asc + desc)) * 0.5f + asc + 0.5f);
}

bool ui_hover(Ui& ui, u32 id, Rect r) {
  if (!ui.mouse_inside) return false;
  if (ui.active && ui.active != id) return false;
  if (!rect_has(r, ui.mx, ui.my)) return false;
  ui.hot_next = id;
  return true;
}

bool ui_pressed(Ui& ui, Rect r, u32 button, u32* mods, u8* count) {
  for (UiClick& k : ui.clicks) {
    if (k.used || !k.pressed || k.button != button || !rect_has(r, k.x, k.y)) continue;
    k.used = 1;
    if (mods) *mods = k.mods;
    if (count) *count = k.count;
    return true;
  }
  return false;
}

bool ui_released(Ui& ui, u32 button) {
  for (UiClick& k : ui.clicks) if (!k.pressed && k.button == button) return true;
  return false;
}

bool ui_wheel(Ui& ui, Rect r, float* dx, float* dy) {
  if (ui.scroll_dx == 0 && ui.scroll_dy == 0) return false;
  if (!rect_has(r, ui.scroll_x, ui.scroll_y)) return false;
  if (dx) *dx = ui.scroll_dx;
  if (dy) *dy = ui.scroll_dy;
  ui.scroll_dx = ui.scroll_dy = 0;
  return true;
}

void ui_tooltip(Ui& ui, u32 id, Rect anchor, const char* text) {
  if (ui.active) return;
  if (ui.hot_next != id) return;
  ui.tip_hover_id = id;
  ui.tip_anchor   = anchor;
  snprintf(ui.tip_text, sizeof ui.tip_text, "%s", text);
}

bool ui_button(Ui& ui, u32 id, Rect r, const char* label, i32 icon, bool enabled, bool toggled) {
  Canvas& c = *ui.c;
  const UiTheme& t = *ui.t;
  bool hov = enabled && ui_hover(ui, id, r);
  bool clicked = false;
  u32 mods;
  if (enabled && ui_pressed(ui, r, BTN_LEFT, &mods)) clicked = true;
  if (hov || toggled) canvas_fill_rounded(c, r.x, r.y, r.w, r.h, ui_px(ui, 4), rgb_hex(toggled ? t.selected : t.hover));
  if (hov) ui.cursor = CURSOR_POINTER;
  u32 fg = rgb_hex(enabled ? t.text : t.muted);
  i32 pad = ui_px(ui, 6);
  i32 x = r.x + pad;
  if (icon >= 0) {
    i32 sz = mx_min(ui_px(ui, 16), r.h - pad);
    i32 ix = label && label[0] ? x : r.x + (r.w - sz) / 2;
    ui_icon(ui, icon, ix, r.y + (r.h - sz) / 2, sz, fg, rgb_hex(t.bar));
    x = ix + sz + pad;
  }
  if (label && label[0]) {
    Text& tx = *ui.text;
    float w = text_width(tx, label);
    float lx = icon >= 0 ? (float)x : (float)r.x + ((float)r.w - w) * 0.5f;
    text_draw(c, tx, label, lx, ui_baseline(ui, tx, r.y, r.h), fg);
  }
  return clicked;
}

bool ui_vsplitter(Ui& ui, u32 id, i32 y, i32 h, i32* value_px, i32 min_px, i32 max_px) {
  i32  grab = ui_px(ui, 5);
  Rect r = { *value_px - grab, y, grab * 2, h };
  bool hov = ui_hover(ui, id, r);
  bool moved = false;
  if (ui.active == id) {
    i32 v = mx_clamp(ui.drag_v0 + (i32)(ui.mx - ui.drag_x0), min_px, max_px);
    if (v != *value_px) { *value_px = v; moved = true; }
    ui.cursor = CURSOR_COL_RESIZE;
    if (ui_released(ui, BTN_LEFT)) ui.active = 0;
    canvas_fill(*ui.c, *value_px - 1, y, 2, h, rgb_hex(ui.t->accent));
  } else if (hov) {
    ui.cursor = CURSOR_COL_RESIZE;
    if (ui_pressed(ui, r, BTN_LEFT)) { ui.active = id; ui.drag_x0 = ui.mx; ui.drag_v0 = *value_px; }
    canvas_fill(*ui.c, *value_px - 1, y, 2, h, rgba_hex(ui.t->accent, 120));
  }
  return moved;
}

static bool scrollbar(Ui& ui, u32 id, Rect track, i32 content, i32 view, i32* scroll, bool vertical) {
  i32 maxs = mx_max(content - view, 0);
  i32 before = *scroll;
  *scroll = mx_clamp(*scroll, 0, maxs);
  if (content <= view || view <= 0) return *scroll != before;
  Canvas& c = *ui.c;
  i32 len = vertical ? track.h : track.w;
  i32 thumb_len = mx_max((i32)((i64)len * view / content), ui_px(ui, 24));
  thumb_len = mx_min(thumb_len, len);
  i32 thumb_pos = maxs ? (i32)((i64)*scroll * (len - thumb_len) / maxs) : 0;
  Rect thumb = vertical ? Rect{ track.x + ui_px(ui, 2), track.y + thumb_pos, track.w - ui_px(ui, 4), thumb_len }
                        : Rect{ track.x + thumb_pos, track.y + ui_px(ui, 2), thumb_len, track.h - ui_px(ui, 4) };
  bool hov = ui_hover(ui, id, thumb);
  if (ui.active == id) {
    float d = vertical ? ui.my - ui.drag_y0 : ui.mx - ui.drag_x0;
    i32 pos = mx_clamp(ui.drag_v0 + (i32)d, 0, len - thumb_len);
    *scroll = (len - thumb_len) ? (i32)((i64)pos * maxs / (len - thumb_len)) : 0;
    if (ui_released(ui, BTN_LEFT)) ui.active = 0;
  } else if (hov && ui_pressed(ui, thumb, BTN_LEFT)) {
    ui.active = id; ui.drag_x0 = ui.mx; ui.drag_y0 = ui.my; ui.drag_v0 = thumb_pos;
  } else if (ui_pressed(ui, track, BTN_LEFT)) {

    float p = vertical ? ui.my : ui.mx;
    *scroll = mx_clamp(*scroll + (p > (vertical ? thumb.y : thumb.x) ? view : -view), 0, maxs);
  }
  u32 col = rgba_hex(ui.t->muted, (ui.active == id || hov) ? 220 : 140);
  i32 r = (vertical ? thumb.w : thumb.h) / 2;
  canvas_fill_rounded(c, thumb.x, thumb.y, thumb.w, thumb.h, r, col);
  return *scroll != before;
}

bool ui_scrollbar_v(Ui& ui, u32 id, Rect track, i32 content_h, i32 view_h, i32* scroll) { return scrollbar(ui, id, track, content_h, view_h, scroll, true); }
bool ui_scrollbar_h(Ui& ui, u32 id, Rect track, i32 content_w, i32 view_w, i32* scroll) { return scrollbar(ui, id, track, content_w, view_w, scroll, false); }

static u32 prev_cp(const UiTextInput& st, u32 i) {
  if (!i) return 0;
  i--;
  while (i && ((u8)st.buf[i] & 0xC0) == 0x80) i--;
  return i;
}
static u32 next_cp(const UiTextInput& st, u32 i) {
  if (i >= st.len) return st.len;
  i++;
  while (i < st.len && ((u8)st.buf[i] & 0xC0) == 0x80) i++;
  return i;
}
static bool word_char(char c) { return c == '_' || is_digit(c) || is_upper(c) || is_lower(c) || (u8)c >= 0x80; }
static u32 prev_word(const UiTextInput& st, u32 i) {
  while (i && !word_char(st.buf[i - 1])) i--;
  while (i && word_char(st.buf[i - 1])) i--;
  return i;
}
static u32 next_word(const UiTextInput& st, u32 i) {
  while (i < st.len && word_char(st.buf[i])) i++;
  while (i < st.len && !word_char(st.buf[i])) i++;
  return i;
}
static bool has_sel(const UiTextInput& st) { return st.sel_anchor != 0xFFFFFFFF && st.sel_anchor != st.cursor; }
static void sel_range(const UiTextInput& st, u32& a, u32& b) {
  a = mx_min(st.sel_anchor, st.cursor); b = mx_max(st.sel_anchor, st.cursor);
}
static void erase(UiTextInput& st, u32 a, u32 b) {
  if (b > st.len) b = st.len;
  if (a >= b) return;
  memmove(st.buf + a, st.buf + b, st.len - b);
  st.len -= b - a;
  st.buf[st.len] = 0;
  st.cursor = a;
  st.sel_anchor = 0xFFFFFFFF;
}
static bool insert(UiTextInput& st, const char* s, u32 n) {
  if (has_sel(st)) { u32 a, b; sel_range(st, a, b); erase(st, a, b); }
  if (st.len + n >= sizeof st.buf) return false;
  memmove(st.buf + st.cursor + n, st.buf + st.cursor, st.len - st.cursor);
  memcpy(st.buf + st.cursor, s, n);
  st.len += n; st.cursor += n; st.buf[st.len] = 0;
  st.sel_anchor = 0xFFFFFFFF;
  return true;
}

void ui_text_set(UiTextInput& st, Str s) {
  st.len = mx_min(s.n, (u32)sizeof st.buf - 1);
  memcpy(st.buf, s.p, st.len);
  st.buf[st.len] = 0;
  st.cursor = st.len;
  st.sel_anchor = 0xFFFFFFFF;
  st.scroll_px = 0;
}

UiTextResult ui_text_input_key(UiTextInput& st, u32 ks, u32 mods, const char* text, u32 text_len) {
  bool ctrl = (mods & XKB_MOD_CONTROL) != 0, shift = (mods & XKB_MOD_SHIFT) != 0;
  auto move = [&](u32 to) {
    if (shift) { if (st.sel_anchor == 0xFFFFFFFF) st.sel_anchor = st.cursor; }
    else st.sel_anchor = 0xFFFFFFFF;
    st.cursor = to;
  };
  switch (ks) {
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: return UI_TEXT_SUBMIT;
    case XKB_KEY_Escape: return UI_TEXT_CANCEL;
    case XKB_KEY_Left: case XKB_KEY_KP_Left:
      if (!shift && has_sel(st)) { u32 a, b; sel_range(st, a, b); st.cursor = a; st.sel_anchor = 0xFFFFFFFF; }
      else move(ctrl ? prev_word(st, st.cursor) : prev_cp(st, st.cursor));
      return UI_TEXT_CHANGED;
    case XKB_KEY_Right: case XKB_KEY_KP_Right:
      if (!shift && has_sel(st)) { u32 a, b; sel_range(st, a, b); st.cursor = b; st.sel_anchor = 0xFFFFFFFF; }
      else move(ctrl ? next_word(st, st.cursor) : next_cp(st, st.cursor));
      return UI_TEXT_CHANGED;
    case XKB_KEY_Home: case XKB_KEY_KP_Home: move(0); return UI_TEXT_CHANGED;
    case XKB_KEY_End:  case XKB_KEY_KP_End:  move(st.len); return UI_TEXT_CHANGED;
    case XKB_KEY_BackSpace:
      if (has_sel(st)) { u32 a, b; sel_range(st, a, b); erase(st, a, b); }
      else if (st.cursor) erase(st, ctrl ? prev_word(st, st.cursor) : prev_cp(st, st.cursor), st.cursor);
      else return UI_TEXT_IGNORED;
      return UI_TEXT_CHANGED;
    case XKB_KEY_Delete: case XKB_KEY_KP_Delete:
      if (has_sel(st)) { u32 a, b; sel_range(st, a, b); erase(st, a, b); }
      else if (st.cursor < st.len) erase(st, st.cursor, ctrl ? next_word(st, st.cursor) : next_cp(st, st.cursor));
      return UI_TEXT_CHANGED;
    default: break;
  }
  if (ctrl) {
    u32 lower = ks | 0x20;
    if (lower == 'a') { st.sel_anchor = 0; st.cursor = st.len; return UI_TEXT_CHANGED; }
    if (lower == 'u') { erase(st, 0, st.cursor); return UI_TEXT_CHANGED; }
    if (lower == 'k') { erase(st, st.cursor, st.len); return UI_TEXT_CHANGED; }
    if (lower == 'w') { erase(st, prev_word(st, st.cursor), st.cursor); return UI_TEXT_CHANGED; }
    return UI_TEXT_IGNORED;
  }
  if (text_len) {
    if (st.select_all_on_type) { st.sel_anchor = 0; st.cursor = st.len; st.select_all_on_type = false; }
    insert(st, text, text_len);
    return UI_TEXT_CHANGED;
  }
  return UI_TEXT_IGNORED;
}

static bool text_input_impl(Ui& ui, u32 id, Rect r, UiTextInput& st, const char* placeholder, bool focused, i32 icon);

static u32 cp_index(const UiTextInput& st, u32 i) { u32 n = 0; for (u32 k = 0; k < i && k < st.len; k = next_cp(st, k)) n++; return n; }
static u32 cp_offset(const UiTextInput& st, u32 n) { u32 k = 0; while (n-- && k < st.len) k = next_cp(st, k); return k; }

bool ui_text_input(Ui& ui, u32 id, Rect r, UiTextInput& st, const char* placeholder, bool focused, i32 icon) {
  if (!st.mask) return text_input_impl(ui, id, r, st, placeholder, focused, icon);

  UiTextInput m;
  u32 n = cp_index(st, st.len);
  u32 shown = mx_min(n, (u32)(sizeof m.buf / 3 - 1));
  for (u32 k = 0; k < shown; k++) memcpy(m.buf + k * 3, "\xe2\x80\xa2", 3);
  m.len = shown * 3; m.buf[m.len] = 0;
  m.cursor = mx_min(cp_index(st, st.cursor), shown) * 3;
  m.sel_anchor = st.sel_anchor == 0xFFFFFFFF ? 0xFFFFFFFF : mx_min(cp_index(st, st.sel_anchor), shown) * 3;
  m.scroll_px = st.scroll_px;
  bool took = text_input_impl(ui, id, r, m, placeholder, focused, icon);
  if (took) {
    st.cursor = cp_offset(st, m.cursor / 3);
    st.sel_anchor = m.sel_anchor == 0xFFFFFFFF ? 0xFFFFFFFF : cp_offset(st, m.sel_anchor / 3);
    st.select_all_on_type = false;
  }
  st.scroll_px = m.scroll_px;
  memset(m.buf, 0, sizeof m.buf);
  return took;
}

static bool text_input_impl(Ui& ui, u32 id, Rect r, UiTextInput& st, const char* placeholder, bool focused, i32 icon) {
  Canvas& c = *ui.c;
  Text& tx = *ui.text;
  const UiTheme& t = *ui.t;
  i32 pad = ui_px(ui, 8), rad = ui_px(ui, 5);
  canvas_fill_rounded(c, r.x, r.y, r.w, r.h, rad, rgb_hex(t.input));
  u32 border = focused ? rgba_hex(t.accent, 200) : rgba_hex(t.border, 200);
  canvas_stroke(c, r.x, r.y, r.w, r.h, border);
  bool hov = ui_hover(ui, id, r);
  if (hov) ui.cursor = CURSOR_TEXT;
  bool took = false;
  u32 mods; u8 count;
  if (ui_pressed(ui, r, BTN_LEFT, &mods, &count)) {
    took = true;

    i32 x = r.x + pad + (icon >= 0 ? ui_px(ui, 22) : 0) - st.scroll_px;
    float px = ui.mx - (float)x;
    u32 best = 0; float bestd = 1e9f;
    for (u32 i = 0; i <= st.len; i = i < st.len ? next_cp(st, i) : st.len + 1) {
      float w = text_width(tx, Str(st.buf, i));
      float d = mx_fabsf(w - px);
      if (d < bestd) { bestd = d; best = i; }
      if (i == st.len) break;
    }
    if (count == 2) { st.sel_anchor = 0; st.cursor = st.len; }
    else { st.cursor = best; st.sel_anchor = (mods & XKB_MOD_SHIFT) ? (st.sel_anchor == 0xFFFFFFFF ? st.cursor : st.sel_anchor) : 0xFFFFFFFF; }
    st.select_all_on_type = false;
  }
  i32 saved[4];
  canvas_push_clip(c, r.x + ui_px(ui, 2), r.y, r.w - ui_px(ui, 4), r.h, saved);
  i32 tx0 = r.x + pad;
  if (icon >= 0) {
    i32 sz = ui_px(ui, 14);
    ui_icon(ui, icon, tx0, r.y + (r.h - sz) / 2, sz, rgb_hex(t.muted), rgb_hex(t.input));
    tx0 += ui_px(ui, 22);
  }
  i32 avail = rect_x1(r) - pad - tx0;
  i32 bl = ui_baseline(ui, tx, r.y, r.h);
  if (!st.len) {
    if (placeholder) text_draw_elided(c, tx, placeholder, (float)tx0, bl, (float)avail, rgb_hex(t.muted));
    st.scroll_px = 0;
    if (focused) canvas_fill(c, tx0, r.y + ui_px(ui, 5), mx_max(1, ui_px(ui, 1)), r.h - ui_px(ui, 10), rgb_hex(t.text));
  } else {
    float caret_x = text_width(tx, Str(st.buf, st.cursor));
    if ((i32)caret_x - st.scroll_px > avail) st.scroll_px = (i32)caret_x - avail;
    if ((i32)caret_x - st.scroll_px < 0) st.scroll_px = (i32)caret_x;
    float total = text_width(tx, Str(st.buf, st.len));
    if (total - st.scroll_px < avail) st.scroll_px = mx_max(0, (i32)total - avail);
    float x0 = (float)tx0 - (float)st.scroll_px;
    if (has_sel(st) && focused) {
      u32 a, b; sel_range(st, a, b);
      float sa = text_width(tx, Str(st.buf, a)), sb = text_width(tx, Str(st.buf, b));
      canvas_fill(c, (i32)(x0 + sa), r.y + ui_px(ui, 4), (i32)(sb - sa) + 1, r.h - ui_px(ui, 8), rgba_hex(t.accent, 110));
    }
    text_draw(c, tx, Str(st.buf, st.len), x0, bl, rgb_hex(t.text));
    if (focused) canvas_fill(c, (i32)(x0 + caret_x), r.y + ui_px(ui, 5), mx_max(1, ui_px(ui, 1)), r.h - ui_px(ui, 10), rgb_hex(t.text));
  }
  canvas_pop_clip(c, saved);
  return took;
}

void ui_capacity_bar(Ui& ui, Rect r, float f, u32 fill, u32 track, u32 border) {
  Canvas& c = *ui.c;
  i32 rad = mx_max(1, r.h / 3);
  canvas_fill_rounded(c, r.x, r.y, r.w, r.h, rad, track);
  canvas_stroke(c, r.x, r.y, r.w, r.h, border);
  i32 fw = (i32)((float)(r.w - 2) * mx_clamp(f, 0.0f, 1.0f) + 0.5f);
  if (fw > 0) canvas_fill_rounded(c, r.x + 1, r.y + 1, fw, r.h - 2, mx_max(1, rad - 1), fill);
}

float ui_text_highlight(Ui& ui, Str s, float x, i32 baseline, float max_w, u32 color, u32 match_color, const u16* pos, u32 npos) {
  Text& tx = *ui.text;
  if (!npos) return text_draw_elided(*ui.c, tx, s, x, baseline, max_w, color);

  u32 fit = s.n;
  float w = text_width(tx, s);
  static const char kEllipsis[] = "\xe2\x80\xa6";
  bool elide = w > max_w;
  if (elide) fit = text_fit(tx, s, max_w - text_width(tx, kEllipsis));
  float x0 = x;
  u32 i = 0, k = 0;
  while (i < fit) {
    while (k < npos && pos[k] < i) k++;
    bool m = k < npos && pos[k] == i;
    u32 j = i;

    for (;;) {
      u32 nj = j + 1;
      while (nj < fit && ((u8)s.p[nj] & 0xC0) == 0x80) nj++;
      j = nj;
      if (j >= fit) break;
      u32 kk = k; while (kk < npos && pos[kk] < j) kk++;
      bool mj = kk < npos && pos[kk] == j;
      if (mj != m) break;
    }
    x += text_draw(*ui.c, tx, Str(s.p + i, j - i), x, baseline, m ? match_color : color);
    i = j;
  }
  if (elide) x += text_draw(*ui.c, tx, kEllipsis, x, baseline, color);
  return x - x0;
}

float ui_text_elide_middle(Ui& ui, Text& t, Str s, float x, i32 baseline, float max_w, u32 color) {
  float w = text_width(t, s);
  if (w <= max_w) return text_draw(*ui.c, t, s, x, baseline, color);
  static const char kEllipsis[] = "\xe2\x80\xa6";
  float ew = text_width(t, kEllipsis);

  Str ext = str_extension(s);
  u32 tail = ext.n ? mx_min(ext.n + 4, s.n) : mx_min(4u, s.n);
  while (tail < s.n && ((u8)s.p[s.n - tail] & 0xC0) == 0x80) tail++;
  Str tail_s(s.p + s.n - tail, tail);
  float tw = text_width(t, tail_s);
  if (tw + ew > max_w) return text_draw_elided(*ui.c, t, s, x, baseline, max_w, color); // even the tail does not fit
  u32 head = text_fit(t, s, max_w - ew - tw);
  if (head > s.n - tail) head = s.n - tail;
  float d = text_draw(*ui.c, t, Str(s.p, head), x, baseline, color);
  d += text_draw(*ui.c, t, kEllipsis, x + d, baseline, color);
  d += text_draw(*ui.c, t, tail_s, x + d, baseline, color);
  return d;
}

u32 ui_text_wrap_centered(Ui& ui, Str s, i32 x, i32 y, i32 w, u32 lines, u32 color) {
  Text& tx = *ui.text;
  i32 lh = text_line_height(tx);
  u32 line = 0, i = 0;
  while (i < s.n && line < lines) {
    Str rest(s.p + i, s.n - i);
    u32 n = text_fit(tx, rest, (float)w);
    bool last = line + 1 == lines;
    if (n < rest.n) {
      if (last) {
        text_draw_elided(*ui.c, tx, rest, (float)x + ((float)w - mx_min(text_width(tx, rest), (float)w)) * 0.5f, ui_baseline(ui, tx, y + (i32)line * lh, lh), (float)w, color);
        return line + 1;
      }

      u32 b = n;
      while (b > 0 && rest.p[b] != ' ' && rest.p[b] != '-' && rest.p[b] != '_' && rest.p[b] != '.') b--;
      if (b > n / 2) n = b + (rest.p[b] == ' ' ? 0 : 1);
      if (!n) n = 1;
    }
    Str part(rest.p, n);
    while (part.n && part.p[part.n - 1] == ' ') part.n--;
    float pw = text_width(tx, part);
    text_draw(*ui.c, tx, part, (float)x + ((float)w - pw) * 0.5f, ui_baseline(ui, tx, y + (i32)line * lh, lh), color);
    i += n;
    while (i < s.n && s.p[i] == ' ') i++;
    line++;
  }
  return line;
}

static void ccw_circle(Path& p, float cx, float cy, float r) {
  const float k = 0.5522847f * r;
  p.move_to(cx + r, cy);
  p.cubic_to(cx + r, cy - k, cx + k, cy - r, cx, cy - r);
  p.cubic_to(cx - k, cy - r, cx - r, cy - k, cx - r, cy);
  p.cubic_to(cx - r, cy + k, cx - k, cy + r, cx, cy + r);
  p.cubic_to(cx + k, cy + r, cx + r, cy + k, cx + r, cy);
  p.close();
}

static void stroke_seg(Path& p, float x0, float y0, float x1, float y1, float t) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = __builtin_sqrtf(dx * dx + dy * dy);
  if (len < 1e-4f) return;
  float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
  p.move_to(x0 + nx, y0 + ny); p.line_to(x1 + nx, y1 + ny); p.line_to(x1 - nx, y1 - ny); p.line_to(x0 - nx, y0 - ny); p.close();
}

void ui_icon(Ui& ui, i32 icon, i32 x, i32 y, i32 size, u32 color, u32 bg) {
  Path& p = ui.path; p.clear();
  Rasterizer& rz = ui.text->raster;
  Canvas& c = *ui.c;
  float fx = (float)x, fy = (float)y, S = (float)size;
  auto X = [&](float u) { return fx + u * S; };
  auto Y = [&](float v) { return fy + v * S; };
  auto fill = [&](u32 col) { canvas_fill_path(c, rz, p, col); p.clear(); };
  float t = mx_max(1.2f, S * 0.11f); // stroke width for line icons

  switch (icon) {
    case ICON_FOLDER:
      path_rounded_rect(p, X(0), Y(0.14f), S * 0.45f, S * 0.30f, S * 0.08f);
      path_rounded_rect(p, X(0), Y(0.26f), S, S * 0.64f, S * 0.10f);
      fill(color);
      break;
    case ICON_FILE: case ICON_TEXT: case ICON_CODE: case ICON_IMAGE: case ICON_ARCHIVE: case ICON_EXEC: case ICON_LINK: {
      float x0 = X(0.16f), x1 = X(0.84f), y0 = Y(0.02f), y1 = Y(0.98f), f = S * 0.28f;
      p.move_to(x0, y0); p.line_to(x1 - f, y0); p.line_to(x1, y0 + f); p.line_to(x1, y1); p.line_to(x0, y1); p.close();
      fill(color);
      p.move_to(x1 - f, y0); p.line_to(x1 - f, y0 + f); p.line_to(x1, y0 + f); p.close();
      fill(bg);
      if (icon == ICON_TEXT) { for (u32 i = 0; i < 3; i++) path_rect(p, X(0.28f), Y(0.44f + 0.15f * (float)i), S * (i == 2 ? 0.3f : 0.44f), mx_max(1.0f, S * 0.06f)); fill(bg); }
      if (icon == ICON_CODE) { stroke_seg(p, X(0.42f), Y(0.5f), X(0.3f), Y(0.64f), t * 0.8f); stroke_seg(p, X(0.3f), Y(0.64f), X(0.42f), Y(0.78f), t * 0.8f);
                               stroke_seg(p, X(0.58f), Y(0.5f), X(0.7f), Y(0.64f), t * 0.8f); stroke_seg(p, X(0.7f), Y(0.64f), X(0.58f), Y(0.78f), t * 0.8f); fill(bg); }
      if (icon == ICON_IMAGE) { p.move_to(X(0.28f), Y(0.84f)); p.line_to(X(0.45f), Y(0.56f)); p.line_to(X(0.56f), Y(0.7f)); p.line_to(X(0.62f), Y(0.62f)); p.line_to(X(0.74f), Y(0.84f)); p.close();
                                path_circle(p, X(0.62f), Y(0.48f), S * 0.06f); fill(bg); }
      if (icon == ICON_ARCHIVE) { for (u32 i = 0; i < 4; i++) path_rect(p, X(0.44f + 0.08f * (float)(i & 1)), Y(0.1f + 0.14f * (float)i), S * 0.08f, S * 0.08f); fill(bg); }
      if (icon == ICON_EXEC) { p.move_to(X(0.4f), Y(0.48f)); p.line_to(X(0.68f), Y(0.66f)); p.line_to(X(0.4f), Y(0.84f)); p.close(); fill(bg); }
      if (icon == ICON_LINK) { path_circle(p, X(0.66f), Y(0.8f), S * 0.2f); fill(bg); path_circle(p, X(0.66f), Y(0.8f), S * 0.13f); fill(color);
                               stroke_seg(p, X(0.58f), Y(0.88f), X(0.74f), Y(0.72f), t * 0.7f); fill(bg); }
      break;
    }
    case ICON_HOME:
      p.move_to(X(0.5f), Y(0.06f)); p.line_to(X(0.98f), Y(0.5f)); p.line_to(X(0.86f), Y(0.5f)); p.line_to(X(0.86f), Y(0.96f));
      p.line_to(X(0.14f), Y(0.96f)); p.line_to(X(0.14f), Y(0.5f)); p.line_to(X(0.02f), Y(0.5f)); p.close();
      fill(color);
      path_rect(p, X(0.4f), Y(0.62f), S * 0.2f, S * 0.34f); fill(bg);
      break;
    case ICON_DESKTOP:
      path_rounded_rect(p, X(0.04f), Y(0.12f), S * 0.92f, S * 0.6f, S * 0.08f); fill(color);
      path_rect(p, X(0.42f), Y(0.72f), S * 0.16f, S * 0.14f); path_rect(p, X(0.26f), Y(0.86f), S * 0.48f, S * 0.08f); fill(color);
      path_rect(p, X(0.12f), Y(0.2f), S * 0.76f, S * 0.44f); fill(bg);
      break;
    case ICON_DOCUMENTS: {
      float x0 = X(0.14f), x1 = X(0.86f), y0 = Y(0.02f), y1 = Y(0.98f), f = S * 0.28f;
      p.move_to(x0, y0); p.line_to(x1 - f, y0); p.line_to(x1, y0 + f); p.line_to(x1, y1); p.line_to(x0, y1); p.close(); fill(color);
      for (u32 i = 0; i < 3; i++) path_rect(p, X(0.28f), Y(0.46f + 0.15f * (float)i), S * (i == 2 ? 0.3f : 0.44f), mx_max(1.0f, S * 0.07f));
      fill(bg);
      break;
    }
    case ICON_DOWNLOADS:
      path_rect(p, X(0.42f), Y(0.04f), S * 0.16f, S * 0.46f); fill(color);
      p.move_to(X(0.2f), Y(0.44f)); p.line_to(X(0.8f), Y(0.44f)); p.line_to(X(0.5f), Y(0.74f)); p.close(); fill(color);
      p.move_to(X(0.04f), Y(0.62f)); p.line_to(X(0.16f), Y(0.62f)); p.line_to(X(0.16f), Y(0.84f)); p.line_to(X(0.84f), Y(0.84f));
      p.line_to(X(0.84f), Y(0.62f)); p.line_to(X(0.96f), Y(0.62f)); p.line_to(X(0.96f), Y(0.96f)); p.line_to(X(0.04f), Y(0.96f)); p.close(); fill(color);
      break;
    case ICON_MUSIC:
      path_rect(p, X(0.58f), Y(0.04f), S * 0.12f, S * 0.66f); fill(color);
      p.move_to(X(0.58f), Y(0.04f)); p.line_to(X(0.92f), Y(0.18f)); p.line_to(X(0.92f), Y(0.4f)); p.line_to(X(0.7f), Y(0.28f)); p.close(); fill(color);
      path_circle(p, X(0.44f), Y(0.74f), S * 0.22f); fill(color);
      break;
    case ICON_PICTURES:
      path_rounded_rect(p, X(0.04f), Y(0.1f), S * 0.92f, S * 0.8f, S * 0.08f); fill(color);
      p.move_to(X(0.14f), Y(0.8f)); p.line_to(X(0.38f), Y(0.44f)); p.line_to(X(0.54f), Y(0.64f)); p.line_to(X(0.64f), Y(0.52f)); p.line_to(X(0.86f), Y(0.8f)); p.close();
      path_circle(p, X(0.72f), Y(0.32f), S * 0.09f); fill(bg);
      break;
    case ICON_VIDEOS:
      path_rounded_rect(p, X(0.04f), Y(0.14f), S * 0.92f, S * 0.72f, S * 0.08f); fill(color);
      p.move_to(X(0.38f), Y(0.32f)); p.line_to(X(0.7f), Y(0.5f)); p.line_to(X(0.38f), Y(0.68f)); p.close(); fill(bg);
      break;
    case ICON_DRIVE:
      path_rounded_rect(p, X(0.02f), Y(0.26f), S * 0.96f, S * 0.48f, S * 0.1f); fill(color);
      path_circle(p, X(0.8f), Y(0.5f), S * 0.07f); path_rect(p, X(0.14f), Y(0.46f), S * 0.5f, S * 0.08f); fill(bg);
      break;
    case ICON_USB:
      path_rounded_rect(p, X(0.08f), Y(0.3f), S * 0.62f, S * 0.4f, S * 0.08f); fill(color);
      path_rect(p, X(0.7f), Y(0.38f), S * 0.24f, S * 0.24f); fill(color);
      path_rect(p, X(0.76f), Y(0.43f), S * 0.06f, S * 0.05f); path_rect(p, X(0.76f), Y(0.52f), S * 0.06f, S * 0.05f); fill(bg);
      path_circle(p, X(0.22f), Y(0.5f), S * 0.06f); fill(bg);
      break;
    case ICON_PIN:
      path_circle(p, X(0.5f), Y(0.3f), S * 0.22f); fill(color);
      p.move_to(X(0.3f), Y(0.44f)); p.line_to(X(0.7f), Y(0.44f)); p.line_to(X(0.6f), Y(0.66f)); p.line_to(X(0.4f), Y(0.66f)); p.close(); fill(color);
      path_rect(p, X(0.46f), Y(0.64f), S * 0.08f, S * 0.34f); fill(color);
      break;
    case ICON_CLOSE:
      stroke_seg(p, X(0.2f), Y(0.2f), X(0.8f), Y(0.8f), t); stroke_seg(p, X(0.8f), Y(0.2f), X(0.2f), Y(0.8f), t); fill(color);
      break;
    case ICON_PLUS:
      path_rect(p, X(0.5f) - t * 0.5f, Y(0.14f), t, S * 0.72f); path_rect(p, X(0.14f), Y(0.5f) - t * 0.5f, S * 0.72f, t); fill(color);
      break;
    case ICON_BACK: case ICON_FORWARD: {
      float d = icon == ICON_BACK ? 1.0f : -1.0f;
      float cx = X(0.5f), cy = Y(0.5f);
      stroke_seg(p, cx - d * S * 0.4f, cy, cx + d * S * 0.4f, cy, t);
      stroke_seg(p, cx - d * S * 0.4f, cy, cx - d * S * 0.1f, cy - S * 0.3f, t);
      stroke_seg(p, cx - d * S * 0.4f, cy, cx - d * S * 0.1f, cy + S * 0.3f, t);
      fill(color);
      break;
    }
    case ICON_UP: {
      float cx = X(0.5f), cy = Y(0.5f);
      stroke_seg(p, cx, cy - S * 0.4f, cx, cy + S * 0.4f, t);
      stroke_seg(p, cx, cy - S * 0.4f, cx - S * 0.3f, cy - S * 0.1f, t);
      stroke_seg(p, cx, cy - S * 0.4f, cx + S * 0.3f, cy - S * 0.1f, t);
      fill(color);
      break;
    }
    case ICON_CHEVRON:
      stroke_seg(p, X(0.36f), Y(0.22f), X(0.64f), Y(0.5f), t * 0.9f); stroke_seg(p, X(0.64f), Y(0.5f), X(0.36f), Y(0.78f), t * 0.9f); fill(color);
      break;
    case ICON_CHEVRON_DOWN:
      stroke_seg(p, X(0.22f), Y(0.36f), X(0.5f), Y(0.64f), t * 0.9f); stroke_seg(p, X(0.5f), Y(0.64f), X(0.78f), Y(0.36f), t * 0.9f); fill(color);
      break;
    case ICON_SEARCH:
      path_circle(p, X(0.42f), Y(0.42f), S * 0.32f); ccw_circle(p, X(0.42f), Y(0.42f), S * 0.32f - t); fill(color);
      stroke_seg(p, X(0.64f), Y(0.64f), X(0.92f), Y(0.92f), t * 1.2f); fill(color);
      break;
    case ICON_VIEW_ICONS:
      for (u32 i = 0; i < 4; i++) path_rounded_rect(p, X(0.1f + 0.46f * (float)(i & 1)), Y(0.1f + 0.46f * (float)(i >> 1)), S * 0.34f, S * 0.34f, S * 0.06f);
      fill(color);
      break;
    case ICON_VIEW_LIST:
      for (u32 i = 0; i < 3; i++) { path_rounded_rect(p, X(0.1f), Y(0.12f + 0.3f * (float)i), S * 0.18f, S * 0.18f, S * 0.04f); path_rect(p, X(0.38f), Y(0.16f + 0.3f * (float)i), S * 0.52f, S * 0.1f); }
      fill(color);
      break;
    case ICON_VIEW_DETAILS:
      for (u32 i = 0; i < 4; i++) path_rect(p, X(0.1f), Y(0.1f + 0.24f * (float)i), S * 0.8f, S * (i ? 0.1f : 0.14f));
      fill(color);
      break;
    case ICON_SORT_ASC:
      p.move_to(X(0.5f), Y(0.3f)); p.line_to(X(0.8f), Y(0.7f)); p.line_to(X(0.2f), Y(0.7f)); p.close(); fill(color);
      break;
    case ICON_SORT_DESC:
      p.move_to(X(0.2f), Y(0.3f)); p.line_to(X(0.8f), Y(0.3f)); p.line_to(X(0.5f), Y(0.7f)); p.close(); fill(color);
      break;
    case ICON_TRASH:
      path_rect(p, X(0.14f), Y(0.16f), S * 0.72f, mx_max(1.0f, S * 0.1f)); // lid
      path_rect(p, X(0.38f), Y(0.06f), S * 0.24f, S * 0.1f); // handle
      p.move_to(X(0.2f), Y(0.3f)); p.line_to(X(0.8f), Y(0.3f)); p.line_to(X(0.72f), Y(0.96f)); p.line_to(X(0.28f), Y(0.96f)); p.close();
      fill(color);
      path_rect(p, X(0.38f), Y(0.4f), mx_max(1.0f, S * 0.07f), S * 0.44f); path_rect(p, X(0.55f), Y(0.4f), mx_max(1.0f, S * 0.07f), S * 0.44f); fill(bg);
      break;
    case ICON_COPY:
      path_rounded_rect(p, X(0.3f), Y(0.3f), S * 0.62f, S * 0.66f, S * 0.08f); fill(color);
      path_rounded_rect(p, X(0.3f) + t, Y(0.3f) + t, S * 0.62f - 2 * t, S * 0.66f - 2 * t, S * 0.04f); fill(bg);
      path_rounded_rect(p, X(0.08f), Y(0.04f), S * 0.62f, S * 0.66f, S * 0.08f); fill(color);
      path_rounded_rect(p, X(0.08f) + t, Y(0.04f) + t, S * 0.62f - 2 * t, S * 0.66f - 2 * t, S * 0.04f); fill(bg);
      break;
    case ICON_CUT:
      stroke_seg(p, X(0.3f), Y(0.62f), X(0.84f), Y(0.08f), t); stroke_seg(p, X(0.7f), Y(0.62f), X(0.16f), Y(0.08f), t); fill(color);
      path_circle(p, X(0.28f), Y(0.76f), S * 0.18f); ccw_circle(p, X(0.28f), Y(0.76f), S * 0.18f - t); fill(color);
      path_circle(p, X(0.72f), Y(0.76f), S * 0.18f); ccw_circle(p, X(0.72f), Y(0.76f), S * 0.18f - t); fill(color);
      break;
    case ICON_PASTE:
      path_rounded_rect(p, X(0.14f), Y(0.14f), S * 0.72f, S * 0.82f, S * 0.08f); fill(color);
      path_rect(p, X(0.14f) + t, Y(0.14f) + t, S * 0.72f - 2 * t, S * 0.82f - 2 * t); fill(bg);
      path_rounded_rect(p, X(0.34f), Y(0.04f), S * 0.32f, S * 0.18f, S * 0.04f); fill(color);
      for (u32 i = 0; i < 3; i++) path_rect(p, X(0.3f), Y(0.42f + 0.16f * (float)i), S * (i == 2 ? 0.26f : 0.4f), mx_max(1.0f, S * 0.07f));
      fill(color);
      break;
    case ICON_RENAME: { // a pencil
      float w = t * 1.6f;
      p.move_to(X(0.62f), Y(0.12f)); p.line_to(X(0.88f), Y(0.38f)); p.line_to(X(0.36f), Y(0.9f)); p.line_to(X(0.1f), Y(0.9f)); p.line_to(X(0.1f), Y(0.64f)); p.close();
      fill(color);
      stroke_seg(p, X(0.52f), Y(0.22f), X(0.78f), Y(0.48f), w * 0.5f); fill(bg);
      break;
    }
    case ICON_NEW_FOLDER:
      path_rounded_rect(p, X(0), Y(0.14f), S * 0.45f, S * 0.30f, S * 0.08f);
      path_rounded_rect(p, X(0), Y(0.26f), S, S * 0.64f, S * 0.10f);
      fill(color);
      path_rect(p, X(0.5f) - t * 0.5f, Y(0.42f), t, S * 0.34f); path_rect(p, X(0.33f), Y(0.59f) - t * 0.5f, S * 0.34f, t); fill(bg);
      break;
    case ICON_UNDO: { // a curved arrow back
      float cx = X(0.54f), cy = Y(0.56f), r = S * 0.32f;
      p.move_to(cx + r, cy); p.cubic_to(cx + r, cy + r * 0.55f, cx + r * 0.55f, cy + r, cx, cy + r);
      p.cubic_to(cx - r * 0.55f, cy + r, cx - r, cy + r * 0.55f, cx - r, cy);
      p.line_to(cx - r + t, cy); p.cubic_to(cx - r + t, cy + r * 0.55f - t * 0.55f, cx - r * 0.55f + t * 0.55f, cy + r - t, cx, cy + r - t);
      p.cubic_to(cx + r * 0.55f - t * 0.55f, cy + r - t, cx + r - t, cy + r * 0.55f - t * 0.55f, cx + r - t, cy); p.close();
      p.move_to(X(0.06f), Y(0.34f)); p.line_to(X(0.4f), Y(0.14f)); p.line_to(X(0.4f), Y(0.54f)); p.close();
      p.move_to(X(0.22f), Y(0.34f) - t * 0.5f); p.line_to(cx, Y(0.34f) - t * 0.5f); p.line_to(cx, Y(0.34f) + t * 0.5f); p.line_to(X(0.22f), Y(0.34f) + t * 0.5f); p.close();
      fill(color);
      break;
    }
    case ICON_WARNING:
      p.move_to(X(0.5f), Y(0.06f)); p.line_to(X(0.96f), Y(0.9f)); p.line_to(X(0.04f), Y(0.9f)); p.close(); fill(color);
      path_rect(p, X(0.46f), Y(0.36f), S * 0.08f, S * 0.3f); path_circle(p, X(0.5f), Y(0.76f), S * 0.05f); fill(bg);
      break;
    case ICON_CHECK:
      stroke_seg(p, X(0.14f), Y(0.54f), X(0.4f), Y(0.8f), t * 1.3f); stroke_seg(p, X(0.4f), Y(0.8f), X(0.88f), Y(0.24f), t * 1.3f); fill(color);
      break;
    case ICON_GEAR: {
      static const float g[64] = { 0.812f, 0.428f, 0.956f, 0.436f, 0.956f, 0.564f, 0.812f, 0.572f, 0.771f, 0.670f, 0.867f, 0.777f, 0.777f, 0.867f, 0.670f, 0.771f,
                                   0.572f, 0.812f, 0.564f, 0.956f, 0.436f, 0.956f, 0.428f, 0.812f, 0.330f, 0.771f, 0.223f, 0.867f, 0.133f, 0.777f, 0.229f, 0.670f,
                                   0.188f, 0.572f, 0.044f, 0.564f, 0.044f, 0.436f, 0.188f, 0.428f, 0.229f, 0.330f, 0.133f, 0.223f, 0.223f, 0.133f, 0.330f, 0.229f,
                                   0.428f, 0.188f, 0.436f, 0.044f, 0.564f, 0.044f, 0.572f, 0.188f, 0.670f, 0.229f, 0.777f, 0.133f, 0.867f, 0.223f, 0.771f, 0.330f };
      p.move_to(X(g[0]), Y(g[1]));
      for (u32 i = 1; i < 32; i++) p.line_to(X(g[2 * i]), Y(g[2 * i + 1]));
      p.close(); fill(color);
      path_circle(p, X(0.5f), Y(0.5f), S * 0.13f); fill(bg);
      break;
    }
    case ICON_SIDEBAR: // a window with its left pane
      path_rounded_rect(p, X(0.06f), Y(0.14f), S * 0.88f, S * 0.72f, S * 0.08f); fill(color);
      path_rect(p, X(0.14f), Y(0.22f), S * 0.72f, S * 0.56f); fill(bg);
      path_rect(p, X(0.14f), Y(0.22f), S * 0.24f, S * 0.56f); fill(color);
      break;
    case ICON_REFRESH: {
      float cx = X(0.5f), cy = Y(0.52f), r = S * 0.34f;
      path_circle(p, cx, cy, r); ccw_circle(p, cx, cy, r - t); fill(color);
      path_rect(p, cx, cy - r - t, r + t, r * 0.7f); fill(bg); // the gap
      p.move_to(cx + r * 0.55f, cy - r * 0.45f); p.line_to(cx + r + t, cy - r * 0.45f); p.line_to(cx + r + t, cy - r - t); p.close(); fill(color);
      break;
    }
    default: break;
  }
}

bool ui_checkbox(Ui& ui, u32 id, Rect r, const char* label, bool* value) {
  Canvas& c = *ui.c;
  const UiTheme& t = *ui.t;
  bool hov = ui_hover(ui, id, r);
  bool toggled = false;
  if (ui_pressed(ui, r, BTN_LEFT)) { *value = !*value; toggled = true; }
  if (hov) ui.cursor = CURSOR_POINTER;
  i32 sz = mx_min(ui_px(ui, 15), r.h);
  i32 bx = r.x, by = r.y + (r.h - sz) / 2;
  canvas_fill_rounded(c, bx, by, sz, sz, ui_px(ui, 3), rgb_hex(*value ? t.accent : t.input));
  if (!*value) canvas_stroke(c, bx, by, sz, sz, rgba_hex(hov ? t.accent : t.border, 220));
  if (*value) ui_icon(ui, ICON_CHECK, bx + ui_px(ui, 1), by + ui_px(ui, 1), sz - ui_px(ui, 2), rgb_hex(t.bg), rgb_hex(t.accent));
  if (label && label[0]) text_draw(c, *ui.text, label, (float)(bx + sz + ui_px(ui, 8)), ui_baseline(ui, *ui.text, r.y, r.h), rgb_hex(t.text));
  return toggled;
}
