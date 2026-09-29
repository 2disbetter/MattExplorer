#pragma once

#include "base/types.h"
#include "base/array.h"
#include "base/str.h"
#include "gfx/canvas.h"
#include "gfx/raster.h"
#include "gfx/font.h"

struct Rect { i32 x, y, w, h; };
static inline bool rect_has(Rect r, float x, float y) { return x >= (float)r.x && y >= (float)r.y && x < (float)(r.x + r.w) && y < (float)(r.y + r.h); }
static inline Rect rect_inset(Rect r, i32 d) { return { r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d }; }
static inline i32  rect_x1(Rect r) { return r.x + r.w; }
static inline i32  rect_y1(Rect r) { return r.y + r.h; }

struct UiTheme {
  u32 bg, sidebar, panel, bar, row_alt, hover, selected, selected_dim, accent, red, text, muted, border, folder, input, tooltip, match;
};

enum UiIcon : u8 {
  ICON_FOLDER = 0, ICON_FILE, ICON_HOME, ICON_DESKTOP, ICON_DOCUMENTS, ICON_DOWNLOADS, ICON_MUSIC, ICON_PICTURES, ICON_VIDEOS,
  ICON_DRIVE, ICON_USB, ICON_PIN, ICON_CLOSE, ICON_PLUS, ICON_BACK, ICON_FORWARD, ICON_UP, ICON_CHEVRON, ICON_SEARCH,
  ICON_VIEW_ICONS, ICON_VIEW_LIST, ICON_VIEW_DETAILS, ICON_SORT_ASC, ICON_SORT_DESC, ICON_LINK, ICON_ARCHIVE, ICON_IMAGE,
  ICON_CODE, ICON_TEXT, ICON_EXEC, ICON_CHEVRON_DOWN,
  ICON_TRASH, ICON_COPY, ICON_CUT, ICON_PASTE, ICON_RENAME, ICON_NEW_FOLDER, ICON_UNDO, ICON_WARNING, ICON_CHECK, ICON_REFRESH,
  ICON_GEAR, ICON_SIDEBAR,
  ICON_COUNT
};

struct UiClick {
  float x, y;
  u32   button; // BTN_* (0x110 left, 0x111 right, 0x112 middle)
  u32   mods; // modifier bits at the time
  u8    pressed;
  u8    count; // 1 = single, 2 = double (presses only)
  u8    used;
};

struct UiTextInput {
  char buf[1024];
  u32  len = 0, cursor = 0;
  u32  sel_anchor = 0xFFFFFFFF; // != cursor when a selection exists
  i32  scroll_px = 0; // horizontal scroll so the caret stays visible
  bool select_all_on_type = false;
  bool mask = false; // a password: drawn as bullets, edited as usual
};
enum UiTextResult : u8 { UI_TEXT_NONE = 0, UI_TEXT_CHANGED, UI_TEXT_SUBMIT, UI_TEXT_CANCEL, UI_TEXT_IGNORED };

struct Ui {
  Canvas*        c = nullptr;
  Text*          text = nullptr; // UI font
  Text*          small = nullptr;
  const UiTheme* t = nullptr;
  float          s = 1; // pixels per logical unit
  bool           focused = true; // window has keyboard focus
  i64            now_ms = 0;

  float mx = -1, my = -1;
  bool  mouse_inside = false;
  bool  left_down = false;
  Array<UiClick> clicks;
  float scroll_dx = 0, scroll_dy = 0, scroll_x = 0, scroll_y = 0;
  u32   scroll_mods = 0;

  u32   hot = 0, active = 0;
  u32   hot_next = 0;
  float drag_x0 = 0, drag_y0 = 0;
  i32   drag_v0 = 0;
  u32   cursor = 1; // CursorShape wanted after this frame
  bool  want_redraw = false; // a widget changed state that needs another frame
  i64   wake_at_ms = 0; // earliest timer a widget wants (0 = none)

  u32   tip_id = 0; // tooltip being shown (0 = none)
  u32   tip_hover_id = 0; // widget asking for one this frame
  u32   tip_candidate = 0; // widget hovered last frame, and since when
  i64   tip_hover_since = 0;
  char  tip_text[256] = {};
  Rect  tip_anchor = {};

  Path  path;

  i64   last_press_ms = 0; float last_press_x = 0, last_press_y = 0; u32 last_press_button = 0; u8 last_press_count = 0;
};

u32 ui_id(const char* name, u32 salt = 0);

void ui_begin(Ui& ui, Canvas& c, Text& text, Text* small, const UiTheme& theme, float scale, i64 now_ms);
void ui_end(Ui& ui); // draws the tooltip, resolves hot, clears per-frame input

void ui_input_motion(Ui& ui, float x, float y, i64 now_ms);
void ui_input_leave(Ui& ui);
void ui_input_button(Ui& ui, float x, float y, u32 button, bool pressed, u32 mods, i64 now_ms);
void ui_input_scroll(Ui& ui, float x, float y, float dx, float dy, u32 mods = 0);

static inline i32 ui_px(const Ui& ui, float logical) { return (i32)(logical * ui.s + 0.5f); }

i32 ui_baseline(const Ui& ui, Text& t, i32 y, i32 h);

bool ui_hover(Ui& ui, u32 id, Rect r);

bool ui_pressed(Ui& ui, Rect r, u32 button, u32* mods = nullptr, u8* count = nullptr);
bool ui_released(Ui& ui, u32 button); // any release of button this frame (drags)

bool ui_wheel(Ui& ui, Rect r, float* dx, float* dy);

void ui_tooltip(Ui& ui, u32 id, Rect anchor, const char* text);

bool ui_button(Ui& ui, u32 id, Rect r, const char* label, i32 icon, bool enabled, bool toggled = false);

bool ui_vsplitter(Ui& ui, u32 id, i32 y, i32 h, i32* value_px, i32 min_px, i32 max_px);

bool ui_scrollbar_v(Ui& ui, u32 id, Rect track, i32 content_h, i32 view_h, i32* scroll);
bool ui_scrollbar_h(Ui& ui, u32 id, Rect track, i32 content_w, i32 view_w, i32* scroll);

bool ui_text_input(Ui& ui, u32 id, Rect r, UiTextInput& st, const char* placeholder, bool focused, i32 icon = -1);

UiTextResult ui_text_input_key(UiTextInput& st, u32 keysym, u32 mods, const char* text, u32 text_len);
void ui_text_set(UiTextInput& st, Str s);
static inline Str ui_text_str(const UiTextInput& st) { return Str(st.buf, st.len); }

void ui_icon(Ui& ui, i32 icon, i32 x, i32 y, i32 size, u32 color, u32 bg);

bool ui_checkbox(Ui& ui, u32 id, Rect r, const char* label, bool* value);

void ui_capacity_bar(Ui& ui, Rect r, float f, u32 fill, u32 track, u32 border);

float ui_text_highlight(Ui& ui, Str s, float x, i32 baseline, float max_w, u32 color, u32 match_color, const u16* pos, u32 npos);

float ui_text_elide_middle(Ui& ui, Text& t, Str s, float x, i32 baseline, float max_w, u32 color);

u32   ui_text_wrap_centered(Ui& ui, Str s, i32 x, i32 y, i32 w, u32 lines, u32 color);
