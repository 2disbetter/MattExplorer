#include "app/app.h"
#include <dirent.h>
#include <sys/stat.h>
#include "core/format.h"
#include "core/path.h"
#include "platform/wayland/proto.h"
#include "platform/xkb/keysym.h"

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { BTN_LEFT = 0x110, BTN_RIGHT = 0x111, BTN_MIDDLE = 0x112 };

static const UiTheme& theme(const App& a) { return theme_current(a); }

void shell_layout(App& a, i32 cw, i32 ch) {
  Layout& L = a.L;
  const Window& w = a.w;
  float s = (float)cw / (float)mx_max(w.logical_w, 1);
  if (w.logical_w <= 0) s = 1;
  auto S = [&](float v) { return (i32)(v * s + 0.5f); };
  i32 lh = text_line_height(a.text);
  L.s = s;
  L.pad = S(8); L.sb_w = S(10);
  L.row_h = mx_max(lh + S(8), S(22));
  L.icon_sz = S(14);

  L.sidebar_fits = true;
  L.sidebar = a.cfg.sidebar_visible;
  i32 rail_w = S(30);
  i32 side_w = L.sidebar ? mx_clamp(S((float)a.cfg.sidebar_w), mx_min(S(140), cw / 2), cw / 2) : mx_min(rail_w, cw / 2);
  L.side    = { 0, 0, side_w, ch };
  L.side_footer = { 0, ch - L.row_h - S(8), side_w, L.row_h + S(8) };
  L.content = { side_w, 0, cw - side_w, ch };
  L.tabs    = { L.content.x, 0, L.content.w, S(36) };
  L.crumbs  = { L.content.x, rect_y1(L.tabs), L.content.w, S(34) };
  L.status  = { L.content.x, ch - S(24), L.content.w, S(24) };
  i32 ops_h = a.jobs.len ? L.row_h + S(16) : 0;
  L.ops     = { L.content.x, L.status.y - ops_h, L.content.w, ops_h };
  const Tab& t = a.tabs[a.tab_order[a.active]];
  i32 header_h = t.view == VIEW_DETAILS ? L.row_h : 0;
  L.header  = { L.content.x, rect_y1(L.crumbs), L.content.w, header_h };
  i32 list_y = rect_y1(L.header);
  L.list    = { L.content.x, list_y, L.content.w, mx_max(L.ops.y - list_y, 0) };

  L.col_type = L.list.w >= S(620);
  L.col_date = L.list.w >= S(460);
  i32 right = rect_x1(L.list) - L.sb_w - L.pad;
  L.type_w = L.col_type ? (t.trash_cols ? mx_min(S(260), L.list.w / 3) : S(100)) : 0; L.type_x = right - L.type_w;
  L.date_w = L.col_date ? S(130) : 0; L.date_x = L.type_x - (L.col_type ? L.pad : 0) - L.date_w;
  L.size_w = S(80); L.size_x = L.date_x - (L.col_date ? L.pad : 0) - L.size_w;
  L.name_x = L.list.x + S(12) + L.icon_sz + S(10);
  L.name_w = mx_max(L.size_x - L.pad - L.name_x, S(60));

  L.lcol_w = mx_clamp((i32)t.max_name_w + L.icon_sz + S(30), S(120), S(360));
  L.rows_per_col = mx_max(1, (L.list.h - L.sb_w) / L.row_h);

  L.big_icon = S(48);
  L.cell_w = S(104);
  L.cell_h = L.big_icon + S(6) + 2 * lh + S(14);
  L.cols = mx_max(1, (L.list.w - L.sb_w) / L.cell_w);
}

Rect cell_rect(const App& a, const Tab& t, i32 row) {
  const Layout& L = a.L;
  switch (t.view) {
    case VIEW_ICONS: {
      i32 col = row % L.cols, r = row / L.cols;
      i32 x0 = L.list.x + (L.list.w - L.sb_w - L.cols * L.cell_w) / 2;
      return { x0 + col * L.cell_w, L.list.y + r * L.cell_h - t.scroll, L.cell_w, L.cell_h };
    }
    case VIEW_LIST: {
      i32 col = row / L.rows_per_col, r = row % L.rows_per_col;
      return { L.list.x + col * L.lcol_w - t.scroll_x, L.list.y + r * L.row_h, L.lcol_w, L.row_h };
    }
    default:
      return { L.list.x, L.list.y + row * L.row_h - t.scroll, L.list.w - L.sb_w, L.row_h };
  }
}

i32 cell_at(const App& a, const Tab& t, float x, float y) {
  const Layout& L = a.L;
  if (!rect_has(L.list, x, y)) return -1;
  i32 r;
  switch (t.view) {
    case VIEW_ICONS: {
      i32 x0 = L.list.x + (L.list.w - L.sb_w - L.cols * L.cell_w) / 2;
      i32 col = ((i32)x - x0) / L.cell_w;
      if ((i32)x < x0 || col >= L.cols) return -1;
      r = (((i32)y - L.list.y + t.scroll) / L.cell_h) * L.cols + col;
      break;
    }
    case VIEW_LIST: {
      i32 col = ((i32)x - L.list.x + t.scroll_x) / L.lcol_w;
      i32 rr  = ((i32)y - L.list.y) / L.row_h;
      if (rr >= L.rows_per_col) return -1;
      r = col * L.rows_per_col + rr;
      break;
    }
    default:
      if (x >= (float)(rect_x1(L.list) - L.sb_w)) return -1;
      r = ((i32)y - L.list.y + t.scroll) / L.row_h;
      break;
  }
  return (r >= 0 && (u32)r < t.rows.len) ? r : -1;
}

void content_size(const App& a, const Tab& t, i32* w, i32* h) {
  const Layout& L = a.L;
  i32 n = (i32)t.rows.len;
  switch (t.view) {
    case VIEW_ICONS: *w = L.list.w; *h = ((n + L.cols - 1) / L.cols) * L.cell_h; break;
    case VIEW_LIST:  *w = ((n + L.rows_per_col - 1) / L.rows_per_col) * L.lcol_w; *h = L.list.h; break;
    default:         *w = L.list.w; *h = n * L.row_h; break;
  }
}

i32 rows_per_page(const App& a, const Tab& t) {
  const Layout& L = a.L;
  switch (t.view) {
    case VIEW_ICONS: return mx_max(1, L.list.h / L.cell_h) * L.cols;
    case VIEW_LIST:  return mx_max(1, L.list.w / L.lcol_w) * L.rows_per_col;
    default:         return mx_max(1, L.list.h / L.row_h);
  }
}

static i32 first_visible(const App& a, const Tab& t) {
  const Layout& L = a.L;
  switch (t.view) {
    case VIEW_ICONS: return (t.scroll / L.cell_h) * L.cols;
    case VIEW_LIST:  return (t.scroll_x / L.lcol_w) * L.rows_per_col;
    default:         return t.scroll / L.row_h;
  }
}
static void scroll_to_first(const App& a, Tab& t, i32 first) {
  const Layout& L = a.L;
  switch (t.view) {
    case VIEW_ICONS: t.scroll = (first / L.cols) * L.cell_h; t.scroll_x = 0; break;
    case VIEW_LIST:  t.scroll_x = (first / L.rows_per_col) * L.lcol_w; t.scroll = 0; break;
    default:         t.scroll = first * L.row_h; t.scroll_x = 0; break;
  }
}

static void clamp_scroll(const App& a, Tab& t) {
  i32 cw, ch;
  content_size(a, t, &cw, &ch);
  t.scroll   = mx_clamp(t.scroll, 0, mx_max(ch - a.L.list.h, 0));
  t.scroll_x = mx_clamp(t.scroll_x, 0, mx_max(cw - a.L.list.w, 0));
}

static void measure_names(App& a, Tab& t) {
  if (t.max_name_px == a.cur_px) return;
  u32 longest = 0;
  for (u32 i = 0; i < t.listing.count(); i++) longest = mx_max(longest, (u32)t.listing.name_len[i]);
  u32 threshold = longest > 6 ? longest - 6 : 0;
  float m = 0; u32 measured = 0;
  for (u32 i = 0; i < t.listing.count() && measured < 256; i++) {
    if (t.listing.name_len[i] < threshold) continue;
    float w = text_width(a.text, t.listing.name(i));
    if (w > m) m = w;
    measured++;
  }
  t.max_name_w = m; t.max_name_px = a.cur_px;
}

struct SideCtx { App* a; Ui* ui; Tab* t; i32 y; i32 x0, w; };

static void sidebar_header(SideCtx& sc, const char* label) {
  App& a = *sc.a; Ui& ui = *sc.ui; Layout& L = a.L;
  text_draw(*ui.c, a.text, label, (float)(sc.x0 + L.pad + ui_px(ui, 4)), ui_baseline(ui, a.text, sc.y, L.row_h), rgb_hex(theme(a).muted));
  sc.y += L.row_h;
}

static i32 sidebar_place(SideCtx& sc, u32 id, const Place& p, bool pinned) {
  App& a = *sc.a; Ui& ui = *sc.ui; Layout& L = a.L; const UiTheme& th = theme(a);
  Canvas& c = *ui.c;
  Rect r = { sc.x0 + ui_px(ui, 4), sc.y, sc.w - ui_px(ui, 8), L.row_h };
  bool current = !strcmp(p.path, sc.t->path);
  bool hov = ui_hover(ui, id, r);
  i32 result = 0;
  if (current || hov) canvas_fill_rounded(c, r.x, r.y, r.w, r.h, ui_px(ui, 4), rgb_hex(current ? th.selected : th.hover));
  if (hov) ui.cursor = CURSOR_POINTER;
  i32 ix = r.x + L.pad, iy = sc.y + (L.row_h - L.icon_sz) / 2;
  u32 icol = current ? rgb_hex(th.text) : rgb_hex(p.icon == ICON_FOLDER ? th.folder : th.accent);
  ui_icon(ui, p.icon, ix, iy, L.icon_sz, icol, rgb_hex(current ? th.selected : hov ? th.hover : th.sidebar));
  i32 label_x = ix + L.icon_sz + ui_px(ui, 10);
  i32 label_w = rect_x1(r) - L.pad - label_x;
  if (pinned && hov) { // unpin button
    i32 sz = ui_px(ui, 16);
    Rect br = { rect_x1(r) - sz - ui_px(ui, 4), sc.y + (L.row_h - sz) / 2, sz, sz };
    label_w -= sz + ui_px(ui, 8);
    if (ui_button(ui, ui_id("unpin", id), br, nullptr, ICON_CLOSE, true)) { toggle_pin(a, p.path); return 0; }
  }
  text_draw_elided(c, a.text, p.label, (float)label_x, ui_baseline(ui, a.text, sc.y, L.row_h), (float)label_w, rgb_hex(th.text));
  if (ui_pressed(ui, r, BTN_LEFT)) result = 1;
  else if (ui_pressed(ui, r, BTN_MIDDLE)) result = 2;
  else if (ui_pressed(ui, r, BTN_RIGHT)) result = 4;
  sc.y += L.row_h;
  return result;
}

static i32 sidebar_volume(SideCtx& sc, u32 id, const MountEntry& m, i32 indent) {
  App& a = *sc.a; Ui& ui = *sc.ui; Layout& L = a.L; const UiTheme& th = theme(a);
  Canvas& c = *ui.c;
  i32 small_lh = text_line_height(a.small);
  i32 bar_h = ui_px(ui, 6);
  i32 h = m.mounted ? L.row_h + bar_h + ui_px(ui, 4) + small_lh + ui_px(ui, 6) : L.row_h + small_lh + ui_px(ui, 4);
  Rect r = { sc.x0 + ui_px(ui, 4), sc.y, sc.w - ui_px(ui, 8), h };
  bool current = m.mounted && !strcmp(m.mountpoint, sc.t->path);
  bool hov = ui_hover(ui, id, r);
  i32 result = 0;
  if (current || hov) canvas_fill_rounded(c, r.x, r.y, r.w, r.h, ui_px(ui, 4), rgb_hex(current ? th.selected : th.hover));
  if (hov) ui.cursor = CURSOR_POINTER;
  u32 bgc = rgb_hex(current ? th.selected : hov ? th.hover : th.sidebar);
  i32 ix = r.x + L.pad + indent, iy = sc.y + (L.row_h - L.icon_sz) / 2;
  ui_icon(ui, m.removable ? ICON_USB : ICON_DRIVE, ix, iy, L.icon_sz, rgb_hex(current ? th.text : th.muted), bgc);
  i32 tx = ix + L.icon_sz + ui_px(ui, 10);
  i32 tw = rect_x1(r) - L.pad - tx;
  char name[96], buf1[32], buf2[32], line[128];
  if (m.mounted) snprintf(name, sizeof name, "%s (%s)", m.name, m.mountpoint);
  else snprintf(name, sizeof name, "%s", m.name);
  text_draw_elided(c, a.text, name, (float)tx, ui_baseline(ui, a.text, sc.y, L.row_h), (float)tw, rgb_hex(m.system ? th.muted : th.text));
  i32 y = sc.y + L.row_h;
  if (m.mounted) {
    float f = mount_used_fraction(m);
    u32 fill = rgb_hex(f >= 0.9f ? th.red : th.accent);
    if (!m.statted) fill = rgba_hex(th.muted, 90); // not answered yet: dimmed
    Rect bar = { tx, y, tw, bar_h };
    ui_capacity_bar(ui, bar, m.statted ? f : 0.0f, fill, rgb_hex(th.panel), rgba_hex(th.border, 255));
    y += bar_h + ui_px(ui, 4);
    if (m.stat_failed) snprintf(line, sizeof line, "Unavailable");
    else if (!m.statted) snprintf(line, sizeof line, "\xe2\x80\xa6");
    else snprintf(line, sizeof line, "%s free of %s%s", fmt_size(m.free, buf1, sizeof buf1), fmt_size(m.total, buf2, sizeof buf2),
                  m.system ? " \xc2\xb7 system" : !m.counted ? " \xc2\xb7 same filesystem" : "");
    text_draw_elided(c, a.small, line, (float)tx, ui_baseline(ui, a.small, y, small_lh), (float)tw, rgb_hex(th.muted));
    if (hov) {
      char tip[256], b3[32];
      if (m.statted)
        snprintf(tip, sizeof tip, "Used %s \xc2\xb7 Free %s \xc2\xb7 Total %s \xc2\xb7 %s \xc2\xb7 %s%s", fmt_size(m.used, buf1, sizeof buf1),
                 fmt_size(m.free, buf2, sizeof buf2), fmt_size(m.total, b3, sizeof b3), m.fstype, m.device,
                 m.system ? " \xc2\xb7 system partition, not counted" : !m.counted ? " \xc2\xb7 counted once for the disk" : "");
      else if (m.stat_failed) snprintf(tip, sizeof tip, "%s on %s: statvfs failed", m.fstype, m.device);
      else snprintf(tip, sizeof tip, "%s on %s: waiting for statvfs", m.fstype, m.device);
      ui_tooltip(ui, id, r, tip);
    }
  } else {
    if (!strcmp(m.fstype, "swap")) snprintf(line, sizeof line, "Swap \xc2\xb7 %s", fmt_size(m.raw_size, buf1, sizeof buf1));
    else snprintf(line, sizeof line, "Not mounted \xc2\xb7 %s", fmt_size(m.raw_size, buf1, sizeof buf1));
    text_draw_elided(c, a.small, line, (float)tx, ui_baseline(ui, a.small, y, small_lh), (float)tw, rgb_hex(th.muted));
    if (hov) { char tip[200]; snprintf(tip, sizeof tip, "%s \xc2\xb7 %s", m.device, !strcmp(m.fstype, "swap") ? "swap space" : "not mounted \xc2\xb7 click to mount"); ui_tooltip(ui, id, r, tip); }
  }
  if (ui_pressed(ui, r, BTN_LEFT)) result = 1;
  else if (ui_pressed(ui, r, BTN_MIDDLE)) result = 2;
  else if (ui_pressed(ui, r, BTN_RIGHT)) result = 4;
  sc.y += h;
  return result;
}

static i32 sidebar_disk(SideCtx& sc, u32 id, const DiskEntry& d, bool expanded) {
  App& a = *sc.a; Ui& ui = *sc.ui; Layout& L = a.L; const UiTheme& th = theme(a);
  Canvas& c = *ui.c;
  const MountList& ml = a.mounts;
  i32 small_lh = text_line_height(a.small);
  i32 bar_h = ui_px(ui, 6);
  bool has_bar = d.volumes > 0;
  i32 h = has_bar ? L.row_h + bar_h + ui_px(ui, 4) + small_lh + ui_px(ui, 6) : L.row_h + small_lh + ui_px(ui, 4);
  Rect r = { sc.x0 + ui_px(ui, 4), sc.y, sc.w - ui_px(ui, 8), h };
  const char* primary = disk_primary_mount(ml, d);
  bool current = primary[0] && !strcmp(primary, sc.t->path);
  i32 chev = ui_px(ui, 18);
  Rect cr = { r.x, r.y, chev, L.row_h };
  bool hov_chev = ui_hover(ui, ui_id("disk-chev", id), cr);
  bool hov = !hov_chev && ui_hover(ui, id, r);
  i32 result = 0;
  if (current || hov) canvas_fill_rounded(c, r.x, r.y, r.w, r.h, ui_px(ui, 4), rgb_hex(current ? th.selected : th.hover));
  if (hov || hov_chev) ui.cursor = CURSOR_POINTER;
  u32 bgc = rgb_hex(current ? th.selected : hov ? th.hover : th.sidebar);

  i32 csz = ui_px(ui, 14);
  ui_icon(ui, expanded ? ICON_CHEVRON_DOWN : ICON_CHEVRON, r.x + (chev - csz) / 2, sc.y + (L.row_h - csz) / 2, csz,
          rgb_hex(hov_chev ? th.text : th.muted), bgc);
  i32 ix = r.x + chev, iy = sc.y + (L.row_h - L.icon_sz) / 2;
  ui_icon(ui, d.removable ? ICON_USB : ICON_DRIVE, ix, iy, L.icon_sz, rgb_hex(current ? th.text : th.muted), bgc);
  i32 tx = ix + L.icon_sz + ui_px(ui, 10);
  i32 tw = rect_x1(r) - L.pad - tx;
  char name[128], buf1[32], buf2[32], line[128];
  if (!strcmp(d.label, d.name)) snprintf(name, sizeof name, "%s", d.name);
  else snprintf(name, sizeof name, "%s (%s)", d.label, d.name);
  text_draw_elided(c, a.text, name, (float)tx, ui_baseline(ui, a.text, sc.y, L.row_h), (float)tw, rgb_hex(th.text));
  i32 y = sc.y + L.row_h;
  if (has_bar) {
    float f = disk_used_fraction(d);
    u32 fill = rgb_hex(f >= 0.9f ? th.red : th.accent);
    if (!d.any_statted) fill = rgba_hex(th.muted, 90);
    Rect bar = { tx, y, tw, bar_h };
    ui_capacity_bar(ui, bar, d.any_statted ? f : 0.0f, fill, rgb_hex(th.panel), rgba_hex(th.border, 255));
    y += bar_h + ui_px(ui, 4);
    if (d.any_statted) snprintf(line, sizeof line, "%s free of %s", fmt_size(d.free, buf1, sizeof buf1), fmt_size(d.total, buf2, sizeof buf2));
    else if (d.any_pending) snprintf(line, sizeof line, "\xe2\x80\xa6");
    else snprintf(line, sizeof line, "Unavailable");
  } else {
    bool any_mounted = false;
    for (u32 i = d.first; i < d.first + d.count && i < ml.entries.len; i++) any_mounted |= ml.entries[i].mounted;
    snprintf(line, sizeof line, "%s \xc2\xb7 %s", any_mounted ? "System only" : "Not mounted", fmt_size(d.size, buf1, sizeof buf1));
  }
  text_draw_elided(c, a.small, line, (float)tx, ui_baseline(ui, a.small, y, small_lh), (float)tw, rgb_hex(th.muted));
  if (hov) {
    char tip[256], b3[32];
    if (d.any_statted)
      snprintf(tip, sizeof tip, "%s \xc2\xb7 %s \xc2\xb7 %u volume%s counted \xc2\xb7 Used %s \xc2\xb7 Free %s", d.model[0] ? d.model : d.name,
               fmt_size(d.size, buf1, sizeof buf1), d.volumes, d.volumes == 1 ? "" : "s", fmt_size(d.used, buf2, sizeof buf2), fmt_size(d.free, b3, sizeof b3));
    else snprintf(tip, sizeof tip, "%s \xc2\xb7 %s \xc2\xb7 %u volume%s", d.model[0] ? d.model : d.name, fmt_size(d.size, buf1, sizeof buf1), d.count, d.count == 1 ? "" : "s");
    ui_tooltip(ui, id, r, tip);
  }
  if (hov_chev) ui_tooltip(ui, ui_id("disk-chev", id), cr, expanded ? "Hide the volumes" : "Show the volumes");
  if (ui_pressed(ui, cr, BTN_LEFT)) result = 3;
  else if (ui_pressed(ui, r, BTN_LEFT)) result = 1;
  else if (ui_pressed(ui, r, BTN_MIDDLE)) result = 2;
  else if (ui_pressed(ui, r, BTN_RIGHT)) result = 4;
  sc.y += h;
  return result;
}

static void sidebar_footer(App& a, Ui& ui) {
  Layout& L = a.L; const UiTheme& th = theme(a);
  Canvas& c = *ui.c;
  Rect F = L.side_footer;
  i32 btn = mx_min(L.row_h, ui_px(ui, 26)), pad = ui_px(ui, 4);
  canvas_hline(c, F.x, F.y, F.w, rgb_hex(th.border));
  Rect gear, toggle;
  if (L.sidebar) {
    gear = { F.x + pad, F.y + (F.h - btn) / 2, btn, btn };
    toggle = { rect_x1(F) - pad - btn, F.y + (F.h - btn) / 2, btn, btn };
  } else {
    gear = { F.x + (F.w - btn) / 2, F.y + (F.h - btn) / 2, btn, btn };
    toggle = { F.x + (F.w - btn) / 2, F.y - btn - pad, btn, btn };
  }
  u32 gid = ui_id("sidebar-gear"), tid = ui_id("sidebar-toggle");
  if (ui_button(ui, gid, gear, nullptr, ICON_GEAR, true)) settings_open(a);
  ui_tooltip(ui, gid, gear, "Settings");
  if (ui_button(ui, tid, toggle, nullptr, ICON_SIDEBAR, true, L.sidebar)) { a.cfg.sidebar_visible = !L.sidebar; config_mark_dirty(a); ui.want_redraw = true; }
  ui_tooltip(ui, tid, toggle, L.sidebar ? "Hide the sidebar (Ctrl+B)" : "Show the sidebar (Ctrl+B)");
}

static void draw_sidebar(App& a, Ui& ui, Tab& t) {
  Layout& L = a.L; const UiTheme& th = theme(a);
  Canvas& c = *ui.c;
  if (L.side.w <= 0) return;
  canvas_fill(c, L.side.x, L.side.y, L.side.w, L.side.h, rgb_hex(th.sidebar));
  if (!L.sidebar) { // the rail
    sidebar_footer(a, ui);
    canvas_vline(c, rect_x1(L.side) - 1, 0, c.h, rgb_hex(th.border));
    a.drop_targets.clear();
    return;
  }
  i32 saved[4];
  canvas_push_clip(c, L.side.x, L.side.y, L.side.w, L.side.h - L.side_footer.h, saved);
  SideCtx sc = { &a, &ui, &t, L.pad - a.side_scroll, L.side.x, L.side.w - L.sb_w };
  i32 y_start = sc.y;
  a.drop_targets.clear();
  auto target = [&](Rect r, const char* path) { if (a.drop_targets.len < 64 && path && path[0]) { DropTarget d; d.r = r; snprintf(d.path, sizeof d.path, "%s", path); a.drop_targets.push(d); } };

  sidebar_header(sc, "Places");
  for (u32 i = 0; i < a.num_places; i++) {
    target(Rect{ sc.x0 + ui_px(ui, 4), sc.y, sc.w - ui_px(ui, 8), L.row_h }, a.places[i].path);
    i32 r = sidebar_place(sc, ui_id("place", i), a.places[i], false);
    if (r == 1) navigate(a, t, a.places[i].path, true);
    else if (r == 2) tab_open(a, a.places[i].path, false);
    else if (r == 4) { menu_open_place(a, a.places[i].path, false, (i32)ui.mx, (i32)ui.my); break; }
  }
  if (a.num_pins) {
    sc.y += L.pad;
    sidebar_header(sc, "Pinned");
    for (u32 i = 0; i < a.num_pins; i++) {
      target(Rect{ sc.x0 + ui_px(ui, 4), sc.y, sc.w - ui_px(ui, 8), L.row_h }, a.pins[i].path);
      i32 r = sidebar_place(sc, ui_id("pin", i), a.pins[i], true);
      if (r == 1) navigate(a, t, a.pins[i].path, true);
      else if (r == 2) tab_open(a, a.pins[i].path, false);
      else if (r == 4) menu_open_place(a, a.pins[i].path, true, (i32)ui.mx, (i32)ui.my);
      if (r) break; // toggle_pin may have shifted the array
    }
  }
  sc.y += L.pad;
  sidebar_header(sc, "Devices");
  i32 indent = ui_px(ui, 18);
  for (u32 di = 0; di < a.mounts.disks.len; di++) {
    const DiskEntry& d = a.mounts.disks[di];
    bool expanded = disk_expanded(a, d.name);
    i32 y_disk = sc.y;
    i32 r = sidebar_disk(sc, ui_id("disk", di), d, expanded);
    target(Rect{ sc.x0 + ui_px(ui, 4), y_disk, sc.w - ui_px(ui, 8), sc.y - y_disk }, disk_primary_mount(a.mounts, d));
    if (r == 3) { disk_toggle(a, d.name); break; }
    if (r == 4) { menu_open_device(a, -1, (i32)di, (i32)ui.mx, (i32)ui.my); break; }
    if (r) {
      const char* mp = disk_primary_mount(a.mounts, d);
      if (mp[0]) { if (r == 1) navigate(a, t, mp, true); else tab_open(a, mp, false); }
      else {
        i32 first = -1;
        for (u32 i = d.first; i < d.first + d.count && i < a.mounts.entries.len; i++) { const MountEntry& m = a.mounts.entries[i]; if (!m.mounted && strcmp(m.fstype, "swap") && m.device[0]) { first = (i32)i; break; } }
        if (first >= 0) device_mount(a, (u32)first, true, r == 2);
        else set_status(a, "Nothing on this disk can be mounted");
      }
    }
    if (!expanded) continue;
    for (u32 i = d.first; i < d.first + d.count && i < a.mounts.entries.len; i++) {
      const MountEntry& m = a.mounts.entries[i];
      i32 y_vol = sc.y;
      i32 rv = sidebar_volume(sc, ui_id("vol", i), m, indent);
      if (m.mounted) target(Rect{ sc.x0 + ui_px(ui, 4), y_vol, sc.w - ui_px(ui, 8), sc.y - y_vol }, m.mountpoint);
      if (!rv) continue;
      if (rv == 4) { menu_open_device(a, (i32)i, (i32)di, (i32)ui.mx, (i32)ui.my); break; }
      if (!m.mounted) device_mount(a, i, true, rv == 2); // a click mounts and opens
      else if (rv == 1) navigate(a, t, m.mountpoint, true);
      else tab_open(a, m.mountpoint, false);
    }
  }
  if (!a.mounts.disks.len) {
    text_draw(c, a.small, a.mounts_read_once ? "none" : "\xe2\x80\xa6", (float)(sc.x0 + L.pad + ui_px(ui, 4)),
              ui_baseline(ui, a.small, sc.y, L.row_h), rgb_hex(th.muted));
    sc.y += L.row_h;
  }
  if (a.dnd_over && a.dnd_rect.w) canvas_stroke(c, a.dnd_rect.x, a.dnd_rect.y, a.dnd_rect.w, a.dnd_rect.h, rgb_hex(th.accent)); // the drop target
  canvas_pop_clip(c, saved);
  sidebar_footer(a, ui);

  i32 view_h = L.side.h - L.side_footer.h;
  i32 content_h = sc.y - y_start + L.pad;
  float dy;
  Rect scroll_area = { L.side.x, L.side.y, L.side.w, view_h };
  if (ui_wheel(ui, scroll_area, nullptr, &dy)) a.side_scroll += (i32)(dy * 3.0f);
  Rect track = { rect_x1(L.side) - L.sb_w, 0, L.sb_w, view_h };
  if (ui_scrollbar_v(ui, ui_id("side-sb"), track, content_h, view_h, &a.side_scroll)) ui.want_redraw = true;
  a.side_scroll = mx_clamp(a.side_scroll, 0, mx_max(content_h - view_h, 0));

  canvas_vline(c, rect_x1(L.side) - 1, 0, c.h, rgb_hex(th.border));
  u32 sid = ui_id("splitter");
  if (ui.hot_next == sid || ui.active == sid) canvas_fill(c, rect_x1(L.side) - 1, 0, 2, c.h, rgba_hex(th.accent, ui.active == sid ? 255 : 120));
}

static void splitter_input(App& a, Ui& ui) {
  Layout& L = a.L;
  if (!L.sidebar) return;
  i32 side_px = L.side.w;
  if (ui_vsplitter(ui, ui_id("splitter"), 0, ui.c->h, &side_px, mx_min(ui_px(ui, 140), ui.c->w / 2), ui.c->w / 2)) {
    a.cfg.sidebar_w = (i32)((float)side_px / L.s + 0.5f);
    config_mark_dirty(a);
    ui.want_redraw = true;
  }
}

static void draw_tabs(App& a, Ui& ui, Tab& t) {
  Layout& L = a.L; const UiTheme& th = theme(a);
  Canvas& c = *ui.c;
  Rect R = L.tabs;
  canvas_fill(c, R.x, R.y, R.w, R.h, rgb_hex(th.panel));
  i32 pad = L.pad, top = ui_px(ui, 6), gap = ui_px(ui, 3);
  i32 close_sz = ui_px(ui, 16), btn = ui_px(ui, 26);
  i32 right_tools = btn * 3 + pad; // view-mode buttons
  i32 avail = R.w - pad * 2 - btn - gap - right_tools; // minus the "+" button

  i32 widths[MAX_TABS], total = 0;
  for (u32 i = 0; i < a.num_tabs; i++) {
    const Tab& ti = a.tabs[a.tab_order[i]];
    widths[i] = mx_clamp((i32)text_width(a.text, tab_title(ti)) + pad * 3 + L.icon_sz + ui_px(ui, 6) + close_sz, ui_px(ui, 110), ui_px(ui, 220));
    total += widths[i] + (i ? gap : 0);
  }
  if (total > avail && a.num_tabs) {
    i32 each = mx_max((avail - gap * (i32)(a.num_tabs - 1)) / (i32)a.num_tabs, ui_px(ui, 56));
    for (u32 i = 0; i < a.num_tabs; i++) widths[i] = each;
  }
  i32 x = R.x + pad;
  i32 close_pos = -1, activate_pos = -1;

  if (a.tab_drag >= 0) {
    if (!ui.left_down || (u32)a.tab_drag >= a.num_tabs) { a.tab_drag = -1; a.tab_dragging = false; }
    else {
      if (!a.tab_dragging && mx_fabsf(ui.mx - a.tab_drag_x) > 6 * ui.s) a.tab_dragging = true;
      if (a.tab_dragging) {
        i32 xs[MAX_TABS]; i32 xx = R.x + pad;
        for (u32 i = 0; i < a.num_tabs; i++) { xs[i] = xx; xx += widths[i] + gap; }
        float left = ui.mx - a.tab_drag_grab;
        u32 d = (u32)a.tab_drag;
        if (d > 0 && left < (float)xs[d - 1] + (float)widths[d - 1] * 0.5f) {
          u32 tmp = a.tab_order[d]; a.tab_order[d] = a.tab_order[d - 1]; a.tab_order[d - 1] = tmp;
          if (a.active == d) a.active = d - 1; else if (a.active == d - 1) a.active = d;
          a.tab_drag = (i32)d - 1;
        } else if (d + 1 < a.num_tabs && left + (float)widths[d] > (float)xs[d + 1] + (float)widths[d + 1] * 0.5f) {
          u32 tmp = a.tab_order[d]; a.tab_order[d] = a.tab_order[d + 1]; a.tab_order[d + 1] = tmp;
          if (a.active == d) a.active = d + 1; else if (a.active == d + 1) a.active = d;
          a.tab_drag = (i32)d + 1;
        }
        ui.want_redraw = true;
      }
    }
  }
  i32 drag_slot_x = 0, drag_w = 0;
  for (u32 i = 0; i < a.num_tabs; i++) {
    const Tab& ti = a.tabs[a.tab_order[i]];
    Rect r = { x, top, widths[i], R.h - top };
    bool active = i == a.active;
    u32 id = ui_id("tab", i);
    if (a.tab_dragging && (i32)i == a.tab_drag) { drag_slot_x = x; drag_w = r.w; x += r.w + gap; continue; } // drawn last, at the pointer
    bool hov = ui_hover(ui, id, r);
    if (active) {
      canvas_fill_rounded(c, r.x, r.y, r.w, r.h + ui_px(ui, 6), ui_px(ui, 6), rgb_hex(th.bar));
    } else if (hov) {
      canvas_fill_rounded(c, r.x, r.y, r.w, r.h + ui_px(ui, 6), ui_px(ui, 6), rgb_hex(th.hover));
    }
    if (hov) ui.cursor = CURSOR_POINTER;
    bool show_close = active || hov;
    i32 label_w = r.w - pad * 2 - (show_close ? close_sz + ui_px(ui, 4) : 0);
    i32 bl = ui_baseline(ui, a.text, r.y, r.h);
    ui_icon(ui, ICON_FOLDER, r.x + pad, r.y + (r.h - L.icon_sz) / 2, L.icon_sz, rgb_hex(th.folder), rgb_hex(active ? th.bar : hov ? th.hover : th.panel));
    text_draw_elided(c, a.text, tab_title(ti), (float)(r.x + pad + L.icon_sz + ui_px(ui, 6)), bl,
                     (float)(label_w - L.icon_sz - ui_px(ui, 6)), rgb_hex(active ? th.text : th.muted));
    if (show_close) {
      Rect cr = { rect_x1(r) - pad - close_sz, r.y + (r.h - close_sz) / 2, close_sz, close_sz };
      if (ui_button(ui, ui_id("tab-close", i), cr, nullptr, ICON_CLOSE, true)) close_pos = (i32)i;
    }
    u32 mods; u8 count;
    if (ui_pressed(ui, r, BTN_LEFT, &mods, &count)) { activate_pos = (i32)i; a.tab_drag = (i32)i; a.tab_drag_x = ui.mx; a.tab_drag_grab = ui.mx - (float)r.x; a.tab_dragging = false; }
    else if (ui_pressed(ui, r, BTN_MIDDLE)) close_pos = (i32)i;
    x += r.w + gap;
  }
  if (a.tab_dragging && a.tab_drag >= 0 && (u32)a.tab_drag < a.num_tabs) { // the dragged tab, under the pointer
    const Tab& ti = a.tabs[a.tab_order[(u32)a.tab_drag]];
    i32 dx = mx_clamp((i32)(ui.mx - a.tab_drag_grab), R.x + pad, drag_slot_x + drag_w + ui_px(ui, 60));
    Rect r = { dx, top, drag_w, R.h - top };
    canvas_fill_rounded(c, r.x + ui_px(ui, 2), r.y + ui_px(ui, 3), r.w, r.h + ui_px(ui, 6), ui_px(ui, 6), rgba_hex(0x000000, 60));
    canvas_fill_rounded(c, r.x, r.y, r.w, r.h + ui_px(ui, 6), ui_px(ui, 6), rgb_hex(th.bar));
    i32 bl = ui_baseline(ui, a.text, r.y, r.h);
    ui_icon(ui, ICON_FOLDER, r.x + pad, r.y + (r.h - L.icon_sz) / 2, L.icon_sz, rgb_hex(th.folder), rgb_hex(th.bar));
    text_draw_elided(c, a.text, tab_title(ti), (float)(r.x + pad + L.icon_sz + ui_px(ui, 6)), bl, (float)(r.w - pad * 2 - L.icon_sz - ui_px(ui, 6)), rgb_hex(th.text));
    ui.cursor = CURSOR_GRABBING;
  }
  {
    Rect pr = { x, top + (R.h - top - btn) / 2 + ui_px(ui, 2), btn, btn - ui_px(ui, 4) };
    if (ui_button(ui, ui_id("tab-new"), pr, nullptr, ICON_PLUS, a.num_tabs < MAX_TABS)) tab_open(a, t.path, true);
    ui_tooltip(ui, ui_id("tab-new"), pr, "New tab (Ctrl+T)");
  }

  { u8 count; Rect empty = { x + btn, R.y, mx_max(rect_x1(R) - right_tools - x - btn, 0), R.h };
    if (ui_pressed(ui, empty, BTN_LEFT, nullptr, &count) && count == 2) tab_open(a, t.path, true); }

  {
    i32 bx = rect_x1(R) - pad - btn * 3;
    i32 by = top + (R.h - top - btn) / 2 + ui_px(ui, 2);
    static const struct { u8 icon; ViewMode v; const char* tip; } views[] = {
      { ICON_VIEW_ICONS, VIEW_ICONS, "Icons (Ctrl+1)" }, { ICON_VIEW_LIST, VIEW_LIST, "List (Ctrl+2)" }, { ICON_VIEW_DETAILS, VIEW_DETAILS, "Details (Ctrl+3)" } };
    for (u32 i = 0; i < 3; i++) {
      Rect br = { bx + (i32)i * btn, by, btn, btn - ui_px(ui, 4) };
      u32 id = ui_id("view", i);
      if (ui_button(ui, id, br, nullptr, views[i].icon, true, t.view == views[i].v)) set_view(a, t, views[i].v);
      ui_tooltip(ui, id, br, views[i].tip);
    }
  }
  if (a.w.focused) canvas_fill(c, R.x, rect_y1(R) - ui_px(ui, 2), R.w, ui_px(ui, 2), rgb_hex(th.accent));
  if (activate_pos >= 0 && close_pos != activate_pos) tab_activate(a, (u32)activate_pos);
  if (close_pos >= 0) tab_close(a, (u32)close_pos);
}

static void navigate_crumb(App& a, Tab& t, i32 seg, bool new_tab) {
  char target[4096];
  if (seg <= 0) { if (new_tab) tab_open(a, "/", false); else navigate(a, t, "/", true); return; }
  u32 o = 0; i32 k = 0;
  for (const char* p = t.path + 1; *p && o < sizeof target - 2;) {
    const char* e = strchr(p, '/');
    u32 len = e ? (u32)(e - p) : (u32)strlen(p);
    if (len) {
      k++;
      target[o++] = '/';
      u32 n = mx_min(len, (u32)(sizeof target - o - 1));
      memcpy(target + o, p, n); o += n;
      if (k == seg) break;
    }
    p += len; if (*p == '/') p++;
  }
  target[o] = 0;
  if (new_tab) tab_open(a, o ? target : "/", false); else navigate(a, t, o ? target : "/", true);
}

static void draw_crumbs(App& a, Ui& ui, Tab& t) {
  Layout& L = a.L; const UiTheme& th = theme(a);
  Canvas& c = *ui.c;
  Rect R = L.crumbs;
  canvas_fill(c, R.x, R.y, R.w, R.h, rgb_hex(th.bar));
  i32 pad = L.pad, btn = ui_px(ui, 26);
  i32 by = R.y + (R.h - btn) / 2;
  i32 x = R.x + pad;
  Rect b1 = { x, by, btn, btn }, b2 = { x + btn, by, btn, btn }, b3 = { x + btn * 2 + ui_px(ui, 6), by, btn, btn };
  if (ui_button(ui, ui_id("back"), b1, nullptr, ICON_BACK, can_go_back(t))) go_back(a, t);
  if (ui_button(ui, ui_id("fwd"), b2, nullptr, ICON_FORWARD, can_go_forward(t))) go_forward(a, t);
  if (ui_button(ui, ui_id("up"), b3, nullptr, ICON_UP, strcmp(t.path, "/") != 0)) go_parent(a, t);
  ui_tooltip(ui, ui_id("back"), b1, "Back (Alt+Left)");
  ui_tooltip(ui, ui_id("fwd"), b2, "Forward (Alt+Right)");
  ui_tooltip(ui, ui_id("up"), b3, "Up (Backspace, Alt+Up)");
  x = rect_x1(b3) + pad;
  Rect area = { x, R.y + ui_px(ui, 4), rect_x1(R) - pad - x, R.h - ui_px(ui, 8) };

  if (a.path_editing) {
    ui_text_input(ui, ui_id("path"), area, a.path_input, "Path", true, -1);
    return;
  }

  const char* segs[64]; u32 seg_len[64]; u32 n = 0;
  segs[n] = "/"; seg_len[n++] = 1;
  for (const char* p = t.path + 1; *p && n < 64;) {
    const char* e = strchr(p, '/');
    u32 len = e ? (u32)(e - p) : (u32)strlen(p);
    if (len) { segs[n] = p; seg_len[n++] = len; }
    p += len; if (*p == '/') p++;
  }
  i32 chev = ui_px(ui, 14);
  float widths[64], total = 0;
  for (u32 i = 0; i < n; i++) { widths[i] = text_width(a.text, Str(segs[i], seg_len[i])) + (float)pad * 2; total += widths[i] + (i ? (float)chev : 0); }
  u32 first = 0;
  float ell_w = text_width(a.text, "\xe2\x80\xa6") + (float)pad * 2;
  while (first + 1 < n && total > (float)area.w - ell_w - (float)chev) { total -= widths[first] + (float)chev; first++; }
  float fx = (float)area.x;
  i32 bl = ui_baseline(ui, a.text, area.y, area.h);
  bool clicked_seg = false;
  i32  go_seg = -1; bool go_new_tab = false;
  if (first) { text_draw(c, a.text, "\xe2\x80\xa6", fx + (float)pad, bl, rgb_hex(th.muted)); fx += ell_w; }
  for (u32 i = first; i < n; i++) {
    if (i > first || first) { ui_icon(ui, ICON_CHEVRON, (i32)fx, area.y + (area.h - chev) / 2, chev, rgb_hex(th.muted), rgb_hex(th.bar)); fx += (float)chev; }
    bool last = i + 1 == n;
    Rect sr = { (i32)fx, area.y, (i32)widths[i], area.h };
    u32 id = ui_id("crumb", i);
    bool hov = ui_hover(ui, id, sr);
    if (hov) { canvas_fill_rounded(c, sr.x, sr.y, sr.w, sr.h, ui_px(ui, 4), rgb_hex(th.hover)); ui.cursor = CURSOR_POINTER; }
    text_draw(c, a.text, Str(segs[i], seg_len[i]), fx + (float)pad, bl, rgb_hex(last ? th.text : th.muted));
    if (ui_pressed(ui, sr, BTN_LEFT)) { clicked_seg = true; if (!last) { go_seg = (i32)i; go_new_tab = false; } }
    else if (ui_pressed(ui, sr, BTN_MIDDLE)) { clicked_seg = true; go_seg = (i32)i; go_new_tab = true; }
    fx += widths[i];
  }
  if (go_seg >= 0) navigate_crumb(a, t, go_seg, go_new_tab); // after the loop: segments point into t.path

  if (!clicked_seg && ui_pressed(ui, area, BTN_LEFT)) {
    a.path_editing = true;
    ui_text_set(a.path_input, t.path);
    a.path_input.sel_anchor = 0;
    ui.want_redraw = true;
  }
  if (rect_has(area, ui.mx, ui.my) && ui.hot_next == 0) ui.cursor = CURSOR_TEXT;
}

static void draw_header(App& a, Ui& ui, Tab& t) {
  Layout& L = a.L; const UiTheme& th = theme(a);
  Canvas& c = *ui.c;
  Rect R = L.header;
  canvas_fill(c, R.x, R.y, R.w, R.h, rgb_hex(th.panel));
  canvas_hline(c, R.x, rect_y1(R) - 1, R.w, rgb_hex(th.border));
  i32 bl = ui_baseline(ui, a.text, R.y, R.h);
  i32 arrow = ui_px(ui, 12);
  struct Col { const char* label; SortKey key; i32 x, w; bool right, shown; } cols[] = {
    { "Name", SORT_NAME, L.name_x, L.name_w, false, true }, { "Size", SORT_SIZE, L.size_x, L.size_w, true, true },
    { t.trash_cols ? "Deleted" : "Date modified", SORT_MTIME, L.date_x, L.date_w, false, L.col_date }, { t.trash_cols ? "Original location" : "Type", SORT_KIND, L.type_x, L.type_w, false, L.col_type } };
  for (u32 i = 0; i < 4; i++) {
    Col& k = cols[i];
    if (!k.shown) continue;
    Rect hr = { k.x - L.pad / 2, R.y, k.w + L.pad, R.h - 1 };
    u32 id = ui_id("hdr", i);
    bool hov = ui_hover(ui, id, hr);
    if (hov) { canvas_fill(c, hr.x, hr.y, hr.w, hr.h, rgb_hex(th.hover)); ui.cursor = CURSOR_POINTER; }
    float lw = text_width(a.text, k.label);
    float lx = k.right ? (float)(k.x + k.w) - lw : (float)k.x;
    text_draw(c, a.text, k.label, lx, bl, rgb_hex(th.muted));
    if (t.sort.key == k.key) {
      i32 ax = k.right ? (i32)lx - arrow - ui_px(ui, 2) : (i32)(lx + lw) + ui_px(ui, 2);
      ui_icon(ui, t.sort.ascending ? ICON_SORT_ASC : ICON_SORT_DESC, ax, R.y + (R.h - arrow) / 2, arrow, rgb_hex(th.muted), rgb_hex(th.panel));
    }
    if (ui_pressed(ui, hr, BTN_LEFT)) set_sort(a, t, k.key, true);
  }
}

static void draw_view(App& a, Ui& ui, Tab& t) {
  Layout& L = a.L; const UiTheme& th = theme(a);
  Canvas& c = *ui.c;
  Text& tx = a.text;
  if (t.view == VIEW_DETAILS) draw_header(a, ui, t);
  if (t.view == VIEW_LIST) measure_names(a, t);
  clamp_scroll(a, t);

  float dy;
  u32 wmods = ui.scroll_mods;
  if (ui_wheel(ui, L.list, nullptr, &dy)) {
    if (wmods & XKB_MOD_CONTROL) {
      if (dy < 0) a.cfg.font_px = mx_min(a.cfg.font_px + 1.0f, 40.0f); else if (dy > 0) a.cfg.font_px = mx_max(a.cfg.font_px - 1.0f, 7.0f);
      config_mark_dirty(a);
    } else if ((wmods & XKB_MOD_SHIFT) || t.view == VIEW_LIST) t.scroll_x += (i32)(dy * 3.0f);
    else t.scroll += (i32)(dy * 3.0f);
    clamp_scroll(a, t);
  }

  i32 saved[4];
  canvas_push_clip(c, L.list.x, L.list.y, L.list.w, L.list.h, saved);
  i32 first, last;
  i32 n = (i32)t.rows.len;
  switch (t.view) {
    case VIEW_ICONS: first = (t.scroll / L.cell_h) * L.cols; last = mx_min(n - 1, ((t.scroll + L.list.h) / L.cell_h + 1) * L.cols - 1); break;
    case VIEW_LIST:  first = (t.scroll_x / L.lcol_w) * L.rows_per_col; last = mx_min(n - 1, ((t.scroll_x + L.list.w) / L.lcol_w + 1) * L.rows_per_col - 1); break;
    default:         first = t.scroll / L.row_h; last = mx_min(n - 1, (t.scroll + L.list.h) / L.row_h); break;
  }
  bool stat_ok = !a.w.resizing;
  bool filtering = t.filtering && t.filter.len;
  Str pattern = ui_text_str(t.filter);
  u16 positions[256];
  char buf[64];
  i32 open_row_idx = -1, open_new_tab = -1, click_row = -1, menu_row = -2; u32 click_mods = 0;
  float menu_x = 0, menu_y = 0;
  u32 text_c = rgb_hex(th.text), muted = rgb_hex(th.muted);
  bool want_positions = filtering && pattern.n <= MX_ARRAY_COUNT(positions);
  Rect rename_r = {};
  bool rename_drawn = false;
  for (const UiClick& k : ui.clicks) if (k.pressed) a.pending_rename_row = -1; // any press cancels a slow-click rename in the making
  i32 slow_click_row = -1;

  thumb_frame_begin(a);
  for (i32 r = first; r <= last; r++) {
    u32 i = t.rows[(u32)r];
    Rect cr = cell_rect(a, t, r);
    if (t.view == VIEW_DETAILS && stat_ok) stat_request(a, t, i); // lazy statx, visible rows only
    bool thumb_cand = !a.no_thumbs && (t.listing.kind[i] == EK_FILE || t.listing.kind[i] == EK_SYMLINK) && thumb_candidate_name(t.listing.cname(i));
    if (thumb_cand && stat_ok) stat_request(a, t, i); // the mtime keys the thumbnail cache
    u32 id = ui_id("row", (u32)r);
    bool hov = ui_hover(ui, id, cr);
    bool sel = t.selected[i] != 0;
    bool renaming = t.renaming == (i32)i;
    u32 bg = th.bg;
    if (sel) bg = a.w.focused ? th.selected : th.selected_dim;
    else if (hov) bg = th.hover;
    else if (t.view == VIEW_DETAILS && (r & 1)) bg = th.row_alt;
    if (bg != th.bg) {
      if (t.view == VIEW_DETAILS) canvas_fill(c, cr.x, cr.y, cr.w, cr.h, rgb_hex(bg));
      else canvas_fill_rounded(c, cr.x + ui_px(ui, 2), cr.y + ui_px(ui, 1), cr.w - ui_px(ui, 4), cr.h - ui_px(ui, 2), ui_px(ui, 5), rgb_hex(bg));
    }
    if (r == t.cursor && a.w.focused)
      canvas_stroke(c, cr.x + ui_px(ui, 2), cr.y + (t.view == VIEW_DETAILS ? 0 : ui_px(ui, 1)), cr.w - ui_px(ui, 4), cr.h - (t.view == VIEW_DETAILS ? 0 : ui_px(ui, 2)), rgba_hex(th.accent, 160));
    if (a.dnd_over && a.dnd_row == r) { // the folder a drag would land in
      canvas_fill_rounded(c, cr.x + ui_px(ui, 2), cr.y + ui_px(ui, 1), cr.w - ui_px(ui, 4), cr.h - ui_px(ui, 2), ui_px(ui, 4), rgba_hex(th.accent, 70));
      canvas_stroke(c, cr.x + ui_px(ui, 2), cr.y + ui_px(ui, 1), cr.w - ui_px(ui, 4), cr.h - ui_px(ui, 2), rgb_hex(th.accent));
    }
    if (hov) ui.cursor = CURSOR_POINTER;

    bool dir = t.listing.is_dir(i), hidden = (t.listing.flags[i] & EF_HIDDEN) != 0, cut = (t.listing.flags[i] & EF_CUT) != 0;
    u32 fg = hidden ? muted : text_c;
    u8  icon = entry_icon(t.listing, i);
    u32 icol = rgb_hex(dir ? th.folder : th.muted);
    if (cut) { fg = rgba_hex(hidden ? th.muted : th.text, 120); icol = rgba_hex(dir ? th.folder : th.muted, 120); } // cut: faded until pasted
    u32 npos = 0;
    if (want_positions) { i32 sc = fuzzy_score(pattern, t.listing.name(i), positions); npos = sc > 0 ? pattern.n : 0; }
    Str name = t.listing.name(i);
    Rect name_r;

    auto small_icon = [&](i32 ix, i32 iy) {
      const Image* im = thumb_cand ? thumb_for(a, t, i, L.icon_sz) : nullptr;
      if (im && im->px) canvas_blit_argb(c, im->px, im->w, im->h, ix + (L.icon_sz - im->w) / 2, iy + (L.icon_sz - im->h) / 2);
      else ui_icon(ui, icon, ix, iy, L.icon_sz, icol, rgb_hex(bg));
    };
    if (t.view == VIEW_DETAILS) {
      i32 bl = ui_baseline(ui, tx, cr.y, cr.h);
      small_icon(L.list.x + ui_px(ui, 12), cr.y + (cr.h - L.icon_sz) / 2);
      name_r = { L.name_x - ui_px(ui, 4), cr.y + ui_px(ui, 1), L.name_w + ui_px(ui, 8), cr.h - ui_px(ui, 2) };
      if (renaming) { rename_r = name_r; rename_drawn = true; }
      else ui_text_highlight(ui, name, (float)L.name_x, bl, (float)L.name_w, fg, rgb_hex(th.match), positions, npos);
      if (t.listing.flags[i] & EF_STATTED) {
        if (!dir) {
          fmt_size(t.listing.size[i], buf, sizeof buf);
          float w = text_width(tx, buf);
          text_draw(c, tx, buf, (float)(L.size_x + L.size_w) - w, bl, muted);
        }
        if (L.col_date && !t.trash_cols) text_draw(c, tx, fmt_time(t.listing.mtime_ns[i], buf, sizeof buf), (float)L.date_x, bl, muted);
        if (L.col_date && t.trash_cols && i < t.trash_deleted.len && t.trash_deleted[i] > 0) text_draw(c, tx, fmt_time(t.trash_deleted[i] * 1000000000LL, buf, sizeof buf), (float)L.date_x, bl, muted);
      }
      if (L.col_type && !t.trash_cols) text_draw_elided(c, tx, entry_kind(t.listing, i, buf, sizeof buf), (float)L.type_x, bl, (float)L.type_w, muted);
      if (L.col_type && t.trash_cols) { // the folder it was deleted from
        const char* orig = trash_original(t, i);
        char dir[4096]; snprintf(dir, sizeof dir, "%s", orig);
        if (char* sl = strrchr(dir, '/')) { if (sl == dir) sl[1] = 0; else *sl = 0; }
        if (orig[0]) ui_text_elide_middle(ui, tx, dir, (float)L.type_x, bl, (float)L.type_w, muted);
      }
    } else if (t.view == VIEW_LIST) {
      i32 bl = ui_baseline(ui, tx, cr.y, cr.h);
      i32 ix = cr.x + ui_px(ui, 8);
      small_icon(ix, cr.y + (cr.h - L.icon_sz) / 2);
      float nx = (float)(ix + L.icon_sz + ui_px(ui, 8));
      name_r = { (i32)nx - ui_px(ui, 4), cr.y + ui_px(ui, 1), rect_x1(cr) - ui_px(ui, 4) - (i32)nx, cr.h - ui_px(ui, 2) };
      if (renaming) { rename_r = name_r; rename_drawn = true; }
      else ui_text_highlight(ui, name, nx, bl, (float)(rect_x1(cr) - ui_px(ui, 8)) - nx, fg, rgb_hex(th.match), positions, npos);
    } else {
      i32 ix = cr.x + (cr.w - L.big_icon) / 2, iy = cr.y + ui_px(ui, 6);
      const Image* im = thumb_cand ? thumb_for(a, t, i, L.big_icon) : nullptr;
      if (im && im->px) { // a thumbnail in the icon's box, framed, centred
        i32 tx = cr.x + (cr.w - im->w) / 2, tyy = iy + (L.big_icon - im->h) / 2;
        canvas_blit_argb(c, im->px, im->w, im->h, tx, tyy);
        canvas_stroke(c, tx - 1, tyy - 1, im->w + 2, im->h + 2, rgba_hex(th.border, cut ? 90 : 200));
        if (cut) canvas_fill(c, tx, tyy, im->w, im->h, rgba_hex(bg, 130));
      } else ui_icon(ui, icon, ix, iy, L.big_icon, icol, rgb_hex(bg));
      i32 ty = iy + L.big_icon + ui_px(ui, 4);
      name_r = { cr.x + ui_px(ui, 2), ty, cr.w - ui_px(ui, 4), L.row_h };
      if (renaming) { rename_r = name_r; rename_drawn = true; }
      else ui_text_wrap_centered(ui, name, cr.x + ui_px(ui, 4), ty, cr.w - ui_px(ui, 8), 2, fg);
    }

    u32 mods; u8 count;
    if (renaming && rect_has(rename_r, ui.mx, ui.my)) continue; // the editor takes the clicks on it
    if (ui_pressed(ui, cr, BTN_LEFT, &mods, &count)) {
      bool ctrl = (mods & XKB_MOD_CONTROL) != 0, shift = (mods & XKB_MOD_SHIFT) != 0;
      if (count == 2 && !ctrl && !shift) open_row_idx = r;
      else {

        if (count == 1 && !ctrl && !shift && sel && t.selected_count == 1 && r == t.cursor && rect_has(name_r, ui.mx, ui.my) && !filtering) slow_click_row = r;
        click_row = r; click_mods = mods;
        if (count == 1 && !renaming) { a.drag_press_row = r; a.drag_press_x = ui.mx; a.drag_press_y = ui.my; }
      }
      a.path_editing = false;
    } else if (ui_pressed(ui, cr, BTN_MIDDLE)) {
      open_new_tab = r;
    } else if (ui_pressed(ui, cr, BTN_RIGHT)) {
      menu_row = r; menu_x = ui.mx; menu_y = ui.my;
    }
  }
  if (t.renaming >= 0 && !rename_drawn && (u32)t.renaming < t.listing.count()) {
    for (u32 r = 0; r < t.rows.len; r++) if (t.rows[r] == (u32)t.renaming) { Rect cr = cell_rect(a, t, (i32)r); rename_r = { cr.x + ui_px(ui, 4), cr.y, cr.w - ui_px(ui, 8), cr.h }; rename_drawn = true; break; }
  }
  if (rename_drawn) {
    ui_text_input(ui, ui_id("rename"), rename_r, a.rename_input, nullptr, true, -1);
  }
  if (a.dnd_over && a.dnd_target[0] && a.dnd_row < 0 && !a.dnd_rect.w) { // dropping into the folder shown
    canvas_stroke(c, L.list.x + 1, L.list.y + 1, L.list.w - 2, L.list.h - 2, rgb_hex(th.accent));
    canvas_stroke(c, L.list.x + 2, L.list.y + 2, L.list.w - 4, L.list.h - 4, rgba_hex(th.accent, 120));
  }
  canvas_pop_clip(c, saved);
  stat_flush(a);

  if (t.renaming >= 0 && (click_row >= 0 || open_row_idx >= 0 || menu_row >= -1)) rename_commit(a, t); // clicking elsewhere ends the edit
  if (click_row >= 0) set_cursor(a, t, click_row, (click_mods & XKB_MOD_SHIFT) != 0, (click_mods & XKB_MOD_CONTROL) != 0);
  if (slow_click_row >= 0 && t.cursor == slow_click_row && t.selected_count == 1) { a.pending_rename_row = slow_click_row; a.pending_rename_at = ui.now_ms + 500; }

  { u32 mods;
    if (menu_row == -2 && ui_pressed(ui, L.list, BTN_RIGHT, &mods)) { menu_row = -1; menu_x = ui.mx; menu_y = ui.my; } }

  { u32 mods;
    if (ui_pressed(ui, L.list, BTN_LEFT, &mods)) {
      if (t.renaming >= 0) rename_commit(a, t);
      a.path_editing = false;
      a.band_add = (mods & XKB_MOD_CONTROL) != 0;
      if (a.band_add) { a.band_base.resize(t.selected.len); memcpy(a.band_base.data, t.selected.data, t.selected.len); }
      else clear_selection(a, t);
      a.band_active = true;
      a.band_x0 = ui.mx - (float)L.list.x + (float)t.scroll_x; a.band_y0 = ui.my - (float)L.list.y + (float)t.scroll;
    } }
  if (a.band_active) {
    if (!ui.left_down || (!ui.mouse_inside && !a.w.ptr_inside)) a.band_active = false;
    else {

      float x1 = ui.mx - (float)L.list.x + (float)t.scroll_x, y1 = ui.my - (float)L.list.y + (float)t.scroll;
      float bx0 = mx_min(a.band_x0, x1), by0 = mx_min(a.band_y0, y1), bx1 = mx_max(a.band_x0, x1), by1 = mx_max(a.band_y0, y1);
      if (a.band_add && a.band_base.len == t.selected.len) memcpy(t.selected.data, a.band_base.data, t.selected.len); else memset(t.selected.data, 0, t.selected.len);
      t.selected_count = 0;
      i32 last_hit = -1;
      for (u32 r = 0; r < t.rows.len; r++) {
        Rect cr = cell_rect(a, t, (i32)r);
        float cx0 = (float)(cr.x - L.list.x + t.scroll_x), cy0 = (float)(cr.y - L.list.y + t.scroll), cx1 = cx0 + (float)cr.w, cy1 = cy0 + (float)cr.h;
        if (t.view == VIEW_DETAILS) { cx0 = 0; cx1 = 1e9f; } // a row is as wide as the view
        if (cx0 < bx1 && bx0 < cx1 && cy0 < by1 && by0 < cy1) { t.selected[t.rows[r]] = 1; last_hit = (i32)r; }
      }
      for (u32 i = 0; i < t.selected.len; i++) t.selected_count += t.selected[i];
      if (last_hit >= 0) { t.cursor = last_hit; t.anchor = last_hit; }

      i32 step = mx_max(L.row_h / 2, 4);
      if (ui.my < (float)L.list.y) { if (t.view == VIEW_LIST) t.scroll_x -= step; else t.scroll -= step; }
      else if (ui.my > (float)rect_y1(L.list)) { if (t.view == VIEW_LIST) t.scroll_x += step; else t.scroll += step; }
      if (t.view == VIEW_LIST) { if (ui.mx < (float)L.list.x) t.scroll_x -= step; else if (ui.mx > (float)rect_x1(L.list)) t.scroll_x += step; }
      clamp_scroll(a, t);

      i32 saved2[4];
      canvas_push_clip(c, L.list.x, L.list.y, L.list.w, L.list.h, saved2);
      i32 rx = (i32)bx0 + L.list.x - t.scroll_x, ry = (i32)by0 + L.list.y - t.scroll, rw = (i32)(bx1 - bx0), rh = (i32)(by1 - by0);
      canvas_fill(c, rx, ry, rw, rh, rgba_hex(th.accent, 50));
      canvas_stroke(c, rx, ry, rw, rh, rgba_hex(th.accent, 200));
      canvas_pop_clip(c, saved2);
      ui.want_redraw = true;
    }
  }

  if (!n) {
    const char* msg = t.err[0] ? t.err : filtering ? "No matches" : "This folder is empty";
    float w = text_width(tx, msg);
    text_draw(c, tx, msg, (float)L.list.x + ((float)L.list.w - w) * 0.5f, ui_baseline(ui, tx, L.list.y, mx_min(L.list.h, L.row_h * 3)), muted);
  }

  i32 cw, ch;
  content_size(a, t, &cw, &ch);
  if (t.view == VIEW_LIST) {
    Rect track = { L.list.x, rect_y1(L.list) - L.sb_w, L.list.w, L.sb_w };
    if (ui_scrollbar_h(ui, ui_id("list-sbh"), track, cw, L.list.w, &t.scroll_x)) ui.want_redraw = true;
  } else {
    Rect track = { rect_x1(L.list) - L.sb_w, L.list.y, L.sb_w, L.list.h };
    if (ui_scrollbar_v(ui, ui_id("list-sbv"), track, ch, L.list.h, &t.scroll)) ui.want_redraw = true;
  }
  if (open_row_idx >= 0) open_row(a, t, open_row_idx, false);
  if (open_new_tab >= 0) open_row(a, t, open_new_tab, true);
  if (menu_row >= -1) menu_open(a, t, menu_row, (i32)menu_x, (i32)menu_y);
}

static void draw_status(App& a, Ui& ui, Tab& t) {
  Layout& L = a.L; const UiTheme& th = theme(a);
  Canvas& c = *ui.c;
  Rect R = L.status;
  canvas_fill(c, R.x, R.y, R.w, R.h, rgb_hex(th.panel));
  canvas_hline(c, R.x, R.y, R.w, rgb_hex(th.border));
  i32 pad = L.pad;
  i32 bl = ui_baseline(ui, a.text, R.y, R.h);
  char st[512], b1[32];
  float x = (float)(R.x + pad);
  float right_w = 0;
  if (a.debug) {
    snprintf(st, sizeof st, "list %.1f ms  sort %.1f ms  draw %.2f ms  %.0f%%  cfg %d  pool %u",
             t.list_ns / 1e6, t.sort_ns / 1e6, a.draw_ns / 1e6, L.s * 100.0f, a.w.configures, a.w.pool_grows);
    right_w = text_width(a.text, st) + pad;
    text_draw(c, a.text, st, (float)(rect_x1(R) - pad) - right_w + pad, bl, rgb_hex(th.muted));
  }
  float avail = (float)(rect_x1(R) - pad) - right_w - x;

  if (a.dnd_over) { // what a drop would do
    if (a.dnd_target[0]) snprintf(st, sizeof st, "%s into %s", a.w.dnd_action == WL_DND_MOVE ? "Move" : "Copy", path_base(a.dnd_target));
    else snprintf(st, sizeof st, "Cannot drop here");
    text_draw_elided(c, a.text, st, x, bl, avail, rgb_hex(th.text));
    return;
  }
  if (t.filtering || t.filter.len) {

    i32 fw = mx_min((i32)avail - ui_px(ui, 120), ui_px(ui, 320));
    Rect fr = { (i32)x, R.y + ui_px(ui, 2), mx_max(fw, ui_px(ui, 120)), R.h - ui_px(ui, 4) };
    if (ui_text_input(ui, ui_id("filter"), fr, t.filter, "Filter", !a.path_editing, ICON_SEARCH)) a.path_editing = false;
    snprintf(st, sizeof st, "%u of %u", t.rows.len, t.listing.count());
    text_draw(c, a.text, st, (float)(rect_x1(fr) + pad), bl, rgb_hex(th.muted));
    return;
  }
  if (a.status_msg[0] && a.ui.now_ms < a.status_msg_until) {
    text_draw_elided(c, a.text, a.status_msg, x, bl, avail, rgb_hex(a.status_error ? th.red : th.text));
    return;
  }
  if (t.err[0]) { text_draw_elided(c, a.text, t.err, x, bl, avail, rgb_hex(th.red)); return; }
  if (t.stat_all) { // sort by size / date is waiting for the worker pool
    u32 have = 0;
    for (u32 i = 0; i < t.listing.count(); i++) have += (t.listing.flags[i] & (EF_STATTED | EF_STAT_FAILED)) != 0;
    snprintf(st, sizeof st, "Reading %s\xe2\x80\xa6 %u%%", t.sort.key == SORT_SIZE ? "sizes" : "dates", t.listing.count() ? have * 100 / t.listing.count() : 100);
    text_draw_elided(c, a.text, st, x, bl, avail, rgb_hex(th.muted));
    return;
  }
  u32 hidden = t.hidden_count;
  if (t.selected_count) {

    u64 bytes = 0; u32 statted = 0, want = 0, files = 0;
    for (u32 i = 0; i < t.listing.count() && want <= 200; i++) {
      if (!t.selected[i]) continue;
      want++;
      if (!(t.listing.flags[i] & EF_STATTED) && !a.w.resizing && t.selected_count <= 200) dir_list_stat_range(t.dirfd, t.listing, i, 1);
      if (t.listing.flags[i] & EF_STATTED) { statted++; if (!t.listing.is_dir(i)) { bytes += t.listing.size[i]; files++; } }
    }
    if (statted == t.selected_count && files) snprintf(st, sizeof st, "%u items, %u selected (%s)", t.rows.len, t.selected_count, fmt_size(bytes, b1, sizeof b1));
    else snprintf(st, sizeof st, "%u items, %u selected", t.rows.len, t.selected_count);
  } else if (hidden && !a.cfg.show_hidden) snprintf(st, sizeof st, "%u items (%u hidden)", t.rows.len, hidden);
  else snprintf(st, sizeof st, "%u items", t.rows.len);
  text_draw_elided(c, a.text, st, x, bl, avail, rgb_hex(th.muted));
}

static const char* fmt_eta(i64 secs, char* b, u32 cap) {
  if (secs < 0) secs = 0;
  if (secs < 60) snprintf(b, cap, "%lld s left", (long long)secs);
  else if (secs < 3600) snprintf(b, cap, "%lld min %lld s left", (long long)(secs / 60), (long long)(secs % 60));
  else snprintf(b, cap, "%lld h %lld min left", (long long)(secs / 3600), (long long)(secs / 60 % 60));
  return b;
}

static void job_current(const OpJob* j, char* out, u32 cap) {
  const FileOpProgress& p = j->prog;
  for (u32 tries = 0;; tries++) {
    u32 s1 = __atomic_load_n(&p.cur_seq, __ATOMIC_ACQUIRE);
    if (!(s1 & 1)) {
      memcpy(out, (const char*)p.current, mx_min(cap, (u32)sizeof p.current));
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      if (__atomic_load_n(&p.cur_seq, __ATOMIC_ACQUIRE) == s1) break;
    }
    if (tries > 16) { out[0] = 0; break; }
  }
  out[cap - 1] = 0;
}

static void draw_ops(App& a, Ui& ui) {
  Layout& L = a.L; const UiTheme& th = theme(a);
  Canvas& c = *ui.c;
  Rect R = L.ops;
  if (!a.jobs.len || R.h <= 0) return;
  canvas_fill(c, R.x, R.y, R.w, R.h, rgb_hex(th.bar));
  canvas_hline(c, R.x, R.y, R.w, rgb_hex(th.border));
  OpJob* j = a.jobs[0];
  const FileOpRequest& q = j->req;
  FileOpProgress& p = j->prog;
  u32 st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
  u32 phase = __atomic_load_n(&p.phase, __ATOMIC_ACQUIRE);
  u64 bd = __atomic_load_n(&p.bytes_done, __ATOMIC_RELAXED), bt = __atomic_load_n(&p.bytes_total, __ATOMIC_RELAXED);
  u32 id = __atomic_load_n(&p.items_done, __ATOMIC_RELAXED), it = __atomic_load_n(&p.items_total, __ATOMIC_RELAXED);
  char cur[256];
  job_current(j, cur, sizeof cur);

  i64 now = ui.now_ms;
  if (now - j->rate_ms >= 500 && bd >= j->rate_bytes) {
    float inst = (float)(bd - j->rate_bytes) * 1000.0f / (float)(now - j->rate_ms);
    j->rate = j->rate > 0 ? j->rate * 0.6f + inst * 0.4f : inst;
    j->rate_ms = now; j->rate_bytes = bd;
  }

  i32 pad = L.pad, bh = ui_px(ui, 24), bw = ui_px(ui, 84);
  i32 y = R.y + (R.h - L.row_h) / 2;
  i32 bl = ui_baseline(ui, a.text, y, L.row_h);

  char what[600];
  bool moving = q.kind == OP_COPY || q.kind == OP_MOVE;
  if (j->is_undo) snprintf(what, sizeof what, "Undoing %s", j->label);
  else if (st == JOB_QUEUED) snprintf(what, sizeof what, "%s %s \xc2\xb7 queued", fileop_verb(q.kind), j->label);
  else if (st == JOB_ASKING) snprintf(what, sizeof what, "%s %s \xc2\xb7 waiting for an answer", fileop_verb(q.kind), j->label);
  else if (phase == OP_PHASE_SCAN) snprintf(what, sizeof what, "%s %s \xc2\xb7 counting\xe2\x80\xa6", fileop_verb(q.kind), j->label);
  else if (moving) snprintf(what, sizeof what, "%s %s to %s", fileop_verb(q.kind), cur[0] ? cur : j->label, j->where);
  else snprintf(what, sizeof what, "%s %s", fileop_verb(q.kind), cur[0] ? cur : j->label);
  u8 icon = q.kind == OP_COPY ? ICON_COPY : q.kind == OP_MOVE ? ICON_CUT : q.kind == OP_TRASH ? ICON_TRASH : q.kind == OP_DELETE ? ICON_CLOSE : ICON_UNDO;
  i32 x = R.x + pad;
  ui_icon(ui, icon, x, y + (L.row_h - L.icon_sz) / 2, L.icon_sz, rgb_hex(th.accent), rgb_hex(th.bar));
  x += L.icon_sz + pad;

  Rect cancel = { rect_x1(R) - pad - bw, R.y + (R.h - bh) / 2, bw, bh };
  bool cancelled = __atomic_load_n(&p.cancel, __ATOMIC_RELAXED) != 0;
  if (ui_button(ui, ui_id("ops-cancel"), cancel, cancelled ? "Stopping" : "Cancel", -1, !cancelled)) ops_cancel(a, j);
  canvas_stroke(c, cancel.x, cancel.y, cancel.w, cancel.h, rgba_hex(th.border, 255));
  ui_tooltip(ui, ui_id("ops-cancel"), cancel, "Stop after the current item; what is done stays done");
  char stats[200], b1[32], b2[32], b3[32], eta[48];
  float frac = -1;
  if (st == JOB_QUEUED) stats[0] = 0;
  else if (bt > 0) {
    frac = (float)bd / (float)bt;
    if (j->rate > 1 && bd < bt) snprintf(stats, sizeof stats, "%s of %s \xc2\xb7 %s/s \xc2\xb7 %s", fmt_size(bd, b1, sizeof b1), fmt_size(bt, b2, sizeof b2),
                                         fmt_size((u64)j->rate, b3, sizeof b3), fmt_eta((i64)((float)(bt - bd) / j->rate), eta, sizeof eta));
    else snprintf(stats, sizeof stats, "%s of %s", fmt_size(bd, b1, sizeof b1), fmt_size(bt, b2, sizeof b2));
  } else if (it > 0) {
    frac = (float)id / (float)it;
    snprintf(stats, sizeof stats, "%u of %u", id, it);
  } else stats[0] = 0;
  if (a.jobs.len > 1) { char more[32]; snprintf(more, sizeof more, "%s+%u queued", stats[0] ? " \xc2\xb7 " : "", a.jobs.len - 1); strncat(stats, more, sizeof stats - strlen(stats) - 1); }
  float sw = stats[0] ? text_width(a.text, stats) : 0;
  i32 stats_x = cancel.x - pad - (i32)sw;
  if (stats[0]) text_draw(c, a.text, stats, (float)stats_x, bl, rgb_hex(th.muted));

  i32 avail = stats_x - pad - x;
  i32 bar_w = mx_clamp(avail / 3, ui_px(ui, 60), ui_px(ui, 260));
  if (avail < ui_px(ui, 200)) bar_w = 0;
  i32 text_w = avail - bar_w - (bar_w ? pad : 0);
  text_draw_elided(c, a.text, what, (float)x, bl, (float)mx_max(text_w, 0), rgb_hex(th.text));
  if (bar_w) {
    Rect bar = { stats_x - pad - bar_w, y + (L.row_h - ui_px(ui, 6)) / 2, bar_w, ui_px(ui, 6) };
    if (frac >= 0) ui_capacity_bar(ui, bar, mx_clamp(frac, 0.0f, 1.0f), rgb_hex(th.accent), rgb_hex(th.panel), rgba_hex(th.border, 255));
    else { // indeterminate: a sliding block
      ui_capacity_bar(ui, bar, 0.0f, rgb_hex(th.accent), rgb_hex(th.panel), rgba_hex(th.border, 255));
      i32 span = bar.w / 4, pos = (i32)((now / 12) % (i64)(bar.w + span)) - span;
      i32 x0 = mx_max(bar.x + pos, bar.x), x1 = mx_min(bar.x + pos + span, rect_x1(bar));
      if (x1 > x0) canvas_fill_rounded(c, x0, bar.y, x1 - x0, bar.h, bar.h / 2, rgb_hex(th.accent));
      ui.wake_at_ms = now + 40;
    }
  }
}

enum { PROPS_ROWS = 12, PROPS_BOXES = 12 };
struct PropsRow { const char* label; char value[4400]; bool muted; };
struct PropsGeom {
  PropsRow rows[PROPS_ROWS]; u32 nrows;
  i32  label_w, row_h;
  Rect owner_in, group_in, take_btn;
  Rect popup; i32 popup_n; u32 popup_items[8]; // name suggestions under the focused field
  Rect grid; // the whole permission block
  Rect boxes[PROPS_BOXES];
  Rect recursive; // empty when no folder is involved
  i32  body_h; // total height the body needs
};

static bool name_prefix(const char* name, const char* typed) {
  for (u32 i = 0; typed[i]; i++) { char a = name[i], b = typed[i]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32); if (b >= 'A' && b <= 'Z') b = (char)(b + 32); if (a != b) return false; }
  return true;
}

static void props_popup(App& a, PropsGeom& g) {
  Props& p = a.props;
  g.popup_n = 0; g.popup = {};
  if (!p.focus_field || p.popup_hide) return;
  const NameList& L = p.focus_field == 1 ? p.users : p.groups;
  const UiTextInput& in = p.focus_field == 1 ? p.owner_in : p.group_in;
  for (u32 i = 0; i < L.count() && g.popup_n < 8; i++) if (name_prefix(L.name(i), in.buf)) g.popup_items[g.popup_n++] = i;
  if (g.popup_n == 1 && !strcmp(L.name(g.popup_items[0]), in.buf)) g.popup_n = 0;
  if (!g.popup_n) return;
  Rect f = p.focus_field == 1 ? g.owner_in : g.group_in;
  g.popup = { f.x, rect_y1(f) + ui_px(a.ui, 2), mx_max(f.w, ui_px(a.ui, 200)), g.popup_n * g.row_h + ui_px(a.ui, 8) };
  if (p.popup_sel >= g.popup_n) p.popup_sel = g.popup_n - 1;
}

static const char* fmt_bytes_long(u64 v, char* b, u32 cap) { // "2.4 MB (2,512,345 bytes)"
  char s1[32], s2[40];
  if (v < 1024) snprintf(b, cap, "%s", fmt_size(v, s1, sizeof s1));
  else snprintf(b, cap, "%s (%s bytes)", fmt_size(v, s1, sizeof s1), fmt_count(v, s2, sizeof s2));
  return b;
}

static bool alloc_notable(u64 size, u64 alloc) {
  u64 diff = alloc > size ? alloc - size : size - alloc;
  return alloc && diff > 65536 && diff * 10 > size;
}

static void props_body(App& a, Ui& ui, i32 box_x, i32 box_w, i32 y, i32 pad, PropsGeom& g) {
  Props& p = a.props;
  i32 lh = text_line_height(a.text);
  g.row_h = lh + ui_px(ui, 5);
  g.nrows = 0;
  auto row = [&](const char* label, const char* value, bool muted = false) {
    if (g.nrows >= PROPS_ROWS) return;
    PropsRow& r = g.rows[g.nrows++];
    r.label = label; r.muted = muted;
    snprintf(r.value, sizeof r.value, "%s", value);
  };
  char v[4400], b1[64], b2[64], b3[64];
  if (p.count == 1) {
    row("Kind", p.is_link ? "Symbolic link" : p.kind);
    if (p.is_link) row("Link to", p.target[0] ? p.target : "(unreadable)");
  } else {
    u32 o = 0;
    if (p.n_files) o += (u32)snprintf(v + o, sizeof v - o, "%s%u file%s", o ? ", " : "", p.n_files, p.n_files == 1 ? "" : "s");
    if (p.n_dirs) o += (u32)snprintf(v + o, sizeof v - o, "%s%u folder%s", o ? ", " : "", p.n_dirs, p.n_dirs == 1 ? "" : "s");
    if (p.n_links) o += (u32)snprintf(v + o, sizeof v - o, "%s%u link%s", o ? ", " : "", p.n_links, p.n_links == 1 ? "" : "s");
    if (p.n_others) o += (u32)snprintf(v + o, sizeof v - o, "%s%u other%s", o ? ", " : "", p.n_others, p.n_others == 1 ? "" : "s");
    row("Kind", v);
  }
  row("Location", p.location);

  if (!p.any_dir) {
    snprintf(v, sizeof v, "%s", fmt_bytes_long(p.size, b1, sizeof b1));
    if (alloc_notable(p.size, p.alloc)) snprintf(v + strlen(v), sizeof v - strlen(v), ", %s on disk", fmt_size(p.alloc, b2, sizeof b2));
    row("Size", v);
  } else {
    u64 bytes = p.tot_bytes, alloc = p.tot_alloc; u32 files = p.tot_files, dirs = p.tot_dirs, links = p.tot_links;
    if (p.scan) { // live numbers while the walk runs
      const FileOpProgress& pr = p.scan->prog;
      bytes = __atomic_load_n(&pr.bytes_total, __ATOMIC_RELAXED); alloc = __atomic_load_n(&pr.bytes_alloc, __ATOMIC_RELAXED);
      files = __atomic_load_n(&pr.files, __ATOMIC_RELAXED); dirs = __atomic_load_n(&pr.dirs, __ATOMIC_RELAXED); links = __atomic_load_n(&pr.links, __ATOMIC_RELAXED);
    }
    if (p.scan) snprintf(v, sizeof v, "%s so far\xe2\x80\xa6", fmt_bytes_long(bytes, b1, sizeof b1));
    else if (p.scan_cancelled) snprintf(v, sizeof v, "%s (walk stopped)", fmt_bytes_long(bytes, b1, sizeof b1));
    else if (alloc_notable(bytes, alloc)) snprintf(v, sizeof v, "%s, %s on disk", fmt_bytes_long(bytes, b1, sizeof b1), fmt_size(alloc, b2, sizeof b2));
    else snprintf(v, sizeof v, "%s", fmt_bytes_long(bytes, b1, sizeof b1));
    row("Size", v);

    u32 cf = files - mx_min(files, p.n_files), cd = dirs - mx_min(dirs, p.n_dirs), cl = links - mx_min(links, p.n_links);
    snprintf(v, sizeof v, "%s files, %s folders%s%s%s", fmt_count(cf, b1, sizeof b1), fmt_count(cd, b2, sizeof b2),
             cl ? ", " : "", cl ? fmt_count(cl, b3, sizeof b3) : "", cl ? " links" : "");
    if (!p.scan && p.tot_unreadable) snprintf(v + strlen(v), sizeof v - strlen(v), " \xc2\xb7 %u folder%s unreadable", p.tot_unreadable, p.tot_unreadable == 1 ? "" : "s");
    row("Contains", v, p.scan != nullptr);
  }
  if (p.count == 1) {
    row("Modified", fmt_time(p.mtime_ns, b1, sizeof b1));
    row("Accessed", fmt_time(p.atime_ns, b1, sizeof b1));
    if (p.has_btime) row("Created", fmt_time(p.btime_ns, b1, sizeof b1));
  }
  if (p.count == 1 && !p.is_dir && p.nlink > 1) { snprintf(v, sizeof v, "%u hard links", p.nlink); row("Links", v); }
  if (p.owners_differ) row("", "Owners differ between the items: the fields show the first; a name you enter applies to all.", true);
  if (p.modes_differ) row("", "Permissions differ: only the boxes you change apply.", true);
  bool links_only = p.count == p.n_links;
  if (links_only) row("Permissions", "a link has none of its own; its target's apply", true);
  g.label_w = (i32)text_width(a.text, "Permissions");
  for (u32 i = 0; i < g.nrows; i++) g.label_w = mx_max(g.label_w, (i32)text_width(a.text, g.rows[i].label));
  g.label_w += ui_px(ui, 16);
  i32 yy = y + (i32)g.nrows * g.row_h + ui_px(ui, 6);
  i32 col0 = box_x + pad, colw = ui_px(ui, 86);

  {
    i32 ih = ui_px(ui, 28), gap = ui_px(ui, 8);
    i32 glw = (i32)text_width(a.text, "Group") + gap;
    i32 btn_w = p.foreign ? (i32)text_width(a.text, "Take ownership") + ui_px(ui, 24) : 0;
    i32 avail = box_w - 2 * pad - g.label_w - glw - (btn_w ? btn_w + gap : 0) - gap;
    i32 iw = mx_clamp(avail / 2, ui_px(ui, 90), ui_px(ui, 200));
    i32 x = col0 + g.label_w;
    g.owner_in = { x, yy, iw, ih }; x += iw + gap + glw;
    g.group_in = { x, yy, iw, ih }; x += iw + gap;
    g.take_btn = btn_w ? Rect{ x, yy, btn_w, ih } : Rect{};
    yy += ih + ui_px(ui, 10);
  }
  if (links_only) { // no grid at all
    g.grid = {}; for (u32 i = 0; i < PROPS_BOXES; i++) g.boxes[i] = {};
    g.recursive = {};
    g.body_h = yy - y - ui_px(ui, 10);
    props_popup(a, g);
    return;
  }

  i32 gx = col0 + g.label_w;
  g.grid = { col0, yy, box_w - 2 * pad, 4 * g.row_h + g.row_h + ui_px(ui, 4) };
  i32 bx0 = gx;
  for (u32 r = 0; r < 3; r++)
    for (u32 c = 0; c < 3; c++) g.boxes[r * 3 + c] = { bx0 + (i32)c * colw, yy + g.row_h * (i32)(r + 1), colw - ui_px(ui, 8), g.row_h };
  i32 sy = yy + g.row_h * 4 + ui_px(ui, 4);
  i32 sw = ui_px(ui, 74);
  for (u32 i = 0; i < 3; i++) g.boxes[9 + i] = { bx0 + (i32)i * sw, sy, sw - ui_px(ui, 6), g.row_h };
  yy = sy + g.row_h + ui_px(ui, 8);
  if (p.any_dir) { g.recursive = { col0, yy, box_w - 2 * pad, g.row_h }; yy += g.row_h + ui_px(ui, 4); }
  else g.recursive = {};
  g.body_h = yy - y;
  props_popup(a, g);
}

static bool props_changed(const Props& p) { return p.new_mode != p.mode; }

static bool resolve_account(const UiTextInput& in, const char* current, const NameList& L, bool user, i32* out, char* err, u32 cap) {
  *out = -1;
  if (!in.len || !strcmp(in.buf, current)) return true;
  for (u32 i = 0; i < L.count(); i++) if (!strcmp(L.name(i), in.buf)) { *out = (i32)L.ids[i]; return true; }
  if (user) { if (struct passwd* pw = getpwnam(in.buf)) { *out = (i32)pw->pw_uid; return true; } }
  else { if (struct group* gr = getgrnam(in.buf)) { *out = (i32)gr->gr_gid; return true; } }
  bool digits = true; for (u32 i = 0; i < in.len; i++) digits &= in.buf[i] >= '0' && in.buf[i] <= '9';
  if (digits && in.len < 10) { *out = (i32)strtoul(in.buf, nullptr, 10); return true; }
  snprintf(err, cap, "There is no %s named \"%s\".", user ? "user" : "group", in.buf);
  return false;
}

static void names_sort(NameList& L, const u32* rank) {
  u32 n = L.count();
  u32* idx = (u32*)malloc(n * sizeof(u32));
  if (!idx) return;
  for (u32 i = 0; i < n; i++) idx[i] = i;
  for (u32 i = 1; i < n; i++) { // insertion sort: a few hundred entries at most
    u32 v = idx[i]; u32 k = i;
    while (k > 0 && (rank[idx[k - 1]] > rank[v] || (rank[idx[k - 1]] == rank[v] && strcmp(L.name(idx[k - 1]), L.name(v)) > 0))) { idx[k] = idx[k - 1]; k--; }
    idx[k] = v;
  }
  NameList sorted;
  for (u32 i = 0; i < n; i++) sorted.add(L.name(idx[i]), L.ids[idx[i]]);
  L.names.release(); L.off.release(); L.ids.release();
  L = static_cast<NameList&&>(sorted);
  free(idx);
}

static void props_load_accounts(App& a) {
  Props& p = a.props;
  p.users.clear(); p.groups.clear();
  u32 rank[1024]; u32 n = 0;
  setpwent();
  while (struct passwd* pw = getpwent()) { if (n >= 1024) break; p.users.add(pw->pw_name, (u32)pw->pw_uid); rank[n++] = pw->pw_uid == 0 ? 1 : pw->pw_uid >= 1000 && pw->pw_uid < 60000 ? 0 : 2; }
  endpwent();
  names_sort(p.users, rank);
  gid_t mine[256]; int nm = getgroups(256, mine); if (nm < 0) nm = 0;
  n = 0;
  setgrent();
  while (struct group* gr = getgrent()) {
    if (n >= 1024) break;
    bool member = gr->gr_gid == getgid();
    for (int k = 0; k < nm && !member; k++) member = mine[k] == gr->gr_gid;
    p.groups.add(gr->gr_name, (u32)gr->gr_gid); rank[n++] = member ? 0 : gr->gr_gid == 0 ? 1 : 2;
  }
  endgrent();
  names_sort(p.groups, rank);
  struct passwd* me = getpwuid(getuid());
  snprintf(p.my_name, sizeof p.my_name, "%s", me ? me->pw_name : "");
  struct group* mg = me ? getgrgid(me->pw_gid) : nullptr;
  snprintf(p.my_group, sizeof p.my_group, "%s", mg ? mg->gr_name : "");
}

static void props_job_scan(Job& j) {
  PropsScan* s = (PropsScan*)j.ctx;
  FileOpCtx ctx; ctx.prog = &s->prog;
  fileop_run(s->req, ctx, s->res);
}

static void props_scan_start(App& a) {
  Props& p = a.props;
  PropsScan* s = (PropsScan*)calloc(1, sizeof(PropsScan));
  if (!s) return;
  s->req.kind = OP_SCAN;
  for (u32 i = 0; i < p.paths.count(); i++) s->req.add(p.paths.path(i));
  s->gen = ++p.scan_gen;
  p.scan = s; p.scanned = false; p.scan_cancelled = false;
  if (!worker_submit(a.pool, props_job_scan, s, JOB_PROPS_SCAN, 0)) { p.scan = nullptr; s->req.paths.release(); s->req.off.release(); free(s); return; }
  if (!a.pool.running) pool_drain(a); // inline: the result is already waiting
}

static void props_scan_free(PropsScan* s) {
  s->req.paths.release(); s->req.off.release(); s->res.journal.release(); s->res.joff.release();
  free(s);
}

void props_scan_done(App& a, PropsScan* s) {
  Props& p = a.props;
  if (p.scan == s && a.dialog.kind == DLG_PROPERTIES) {
    p.tot_bytes = s->prog.bytes_total; p.tot_alloc = s->prog.bytes_alloc;
    p.tot_files = s->prog.files; p.tot_dirs = s->prog.dirs; p.tot_links = s->prog.links; p.tot_unreadable = s->prog.unreadable;
    p.scanned = !s->res.cancelled; p.scan_cancelled = s->res.cancelled;
    p.scan = nullptr;
    a.w.need_redraw = true;
  } else if (p.scan == s) p.scan = nullptr;
  props_scan_free(s);
}

static void props_scan_cancel(App& a) {
  Props& p = a.props;
  if (!p.scan) return;
  __atomic_store_n(&p.scan->prog.cancel, 1u, __ATOMIC_RELEASE);
  p.scan = nullptr; // freed when the pool returns it
}

void props_open(App& a, Tab& t, bool folder_itself) {
  if (a.dialog.kind != DLG_NONE) return;
  if (a.menu.open) menu_close(a);
  if (t.renaming >= 0) rename_commit(a, t);
  Props& p = a.props;
  props_scan_cancel(a);
  p.paths.paths.clear(); p.paths.off.clear();
  p.count = p.n_files = p.n_dirs = p.n_links = p.n_others = 0;
  p.name[0] = p.location[0] = p.kind[0] = p.target[0] = 0;
  p.is_dir = p.is_link = p.any_dir = p.modes_differ = p.foreign = p.owners_differ = false;
  p.focus_field = 0; p.popup_hide = false; p.popup_sel = 0;
  p.mode = p.new_mode = 0; p.uid = p.gid = 0; p.owner[0] = p.group[0] = 0;
  p.size = p.alloc = 0; p.mtime_ns = p.atime_ns = p.btime_ns = 0; p.has_btime = false; p.ino = 0; p.nlink = 0;
  p.recursive = false; p.folder_itself = folder_itself || !t.selected_count;
  p.scanned = p.scan_cancelled = false; p.tot_bytes = p.tot_alloc = 0; p.tot_files = p.tot_dirs = p.tot_links = p.tot_unreadable = 0;
  if (p.folder_itself) p.paths.add(t.path);
  else selected_paths(a, t, p.paths);
  bool first = true, mode_known = false;
  for (u32 i = 0; i < p.paths.count(); i++) {
    const char* path = p.paths.path(i);
    struct statx sx;
    if (statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW | AT_STATX_DONT_SYNC, STATX_BASIC_STATS | STATX_BTIME, &sx) != 0) continue;
    u32 m = sx.stx_mode;
    bool dir = S_ISDIR(m), lnk = S_ISLNK(m), reg = S_ISREG(m);
    p.count++;
    if (dir) p.n_dirs++; else if (lnk) p.n_links++; else if (reg) p.n_files++; else p.n_others++;
    p.any_dir |= dir;
    p.size += reg || lnk ? sx.stx_size : 0;
    p.alloc += (u64)sx.stx_blocks * 512u;
    if (!lnk) {
      if (!mode_known) { p.mode = m & 07777; mode_known = true; }
      else if ((m & 07777) != p.mode) p.modes_differ = true;
      if (sx.stx_uid != getuid() && getuid() != 0) p.foreign = true;
    }
    if (!first && (sx.stx_uid != p.uid || sx.stx_gid != p.gid)) p.owners_differ = true;
    if (first) {
      first = false;
      p.is_dir = dir; p.is_link = lnk;
      p.uid = sx.stx_uid; p.gid = sx.stx_gid; p.ino = sx.stx_ino; p.nlink = sx.stx_nlink;
      p.mtime_ns = (i64)sx.stx_mtime.tv_sec * 1000000000LL + sx.stx_mtime.tv_nsec;
      p.atime_ns = (i64)sx.stx_atime.tv_sec * 1000000000LL + sx.stx_atime.tv_nsec;
      if (sx.stx_mask & STATX_BTIME) { p.has_btime = true; p.btime_ns = (i64)sx.stx_btime.tv_sec * 1000000000LL + sx.stx_btime.tv_nsec; }
      if (lnk) { ssize_t n = readlink(path, p.target, sizeof p.target - 1); p.target[n > 0 ? n : 0] = 0; }
    }
  }
  if (!p.count) { set_status_error(a, "Cannot read the item's details"); return; }
  p.new_mode = p.mode;

  const char* base = path_base(p.paths.path(0));
  if (p.count == 1) snprintf(p.name, sizeof p.name, "%s", base[0] ? base : "/");
  else snprintf(p.name, sizeof p.name, "%u items", p.count);
  if (p.folder_itself) {
    char up[4096]; snprintf(up, sizeof up, "%s", t.path);
    char* slash = strrchr(up, '/');
    if (slash) { if (slash == up) up[1] = 0; else *slash = 0; }
    snprintf(p.location, sizeof p.location, "%s", strcmp(t.path, "/") ? up : "/");
  } else snprintf(p.location, sizeof p.location, "%s", t.path);
  if (p.count == 1) {
    if (p.folder_itself) snprintf(p.kind, sizeof p.kind, "Folder");
    else { char kb[64]; u32 li = 0; for (u32 i = 0; i < t.listing.count(); i++) if (t.selected[i]) { li = i; break; } snprintf(p.kind, sizeof p.kind, "%s", entry_kind(t.listing, li, kb, sizeof kb)); }
  }
  if (struct passwd* pw = getpwuid(p.uid)) snprintf(p.owner, sizeof p.owner, "%s", pw->pw_name); else snprintf(p.owner, sizeof p.owner, "%u", p.uid);
  if (struct group* gr = getgrgid(p.gid)) snprintf(p.group, sizeof p.group, "%s", gr->gr_name); else snprintf(p.group, sizeof p.group, "%u", p.gid);
  ui_text_set(p.owner_in, p.owner); ui_text_set(p.group_in, p.group);
  p.owner_in.sel_anchor = p.group_in.sel_anchor = 0xFFFFFFFF;
  props_load_accounts(a);

  Dialog& d = a.dialog;
  d.kind = DLG_PROPERTIES; d.job = nullptr;
  snprintf(d.title, sizeof d.title, "Properties of %s", p.name);
  d.nlines = 0;
  d.nbuttons = 0; d.buttons[d.nbuttons++] = "OK"; d.buttons[d.nbuttons++] = "Cancel";
  d.focus = 0; d.cancel = 1; d.hover = -1; d.check_label = nullptr; d.checked = false;
  a.path_editing = false;
  a.w.need_redraw = true;
  if (p.any_dir) props_scan_start(a);
}

static void props_apply(App& a) {
  Props& p = a.props;
  Dialog& d = a.dialog;
  i32 uid = -1, gid = -1;
  char err[300];
  if (!resolve_account(p.owner_in, p.owner, p.users, true, &uid, err, sizeof err) || !resolve_account(p.group_in, p.group, p.groups, false, &gid, err, sizeof err)) {
    d.nlines = 0; snprintf(d.lines[d.nlines++], sizeof d.lines[0], "%s", err); d.error_last = true; // the dialog stays
    a.w.need_redraw = true;
    return;
  }
  if (uid >= 0 && (u32)uid == p.uid && !p.owners_differ) uid = -1;
  if (gid >= 0 && (u32)gid == p.gid && !p.owners_differ) gid = -1;
  bool mode_change = props_changed(p), owner_change = uid >= 0 || gid >= 0;
  if (!mode_change && !owner_change) { dialog_close(a); return; }
  FileOpRequest r, r2;
  if (mode_change) {
    r.kind = OP_CHMOD;
    for (u32 i = 0; i < p.paths.count(); i++) r.add(p.paths.path(i));
    r.mode_mask = p.mode ^ p.new_mode; // only the boxes that were toggled
    r.mode_value = p.new_mode;
    r.recursive = p.recursive && p.any_dir;
  }
  if (owner_change) {
    r2.kind = OP_CHOWN;
    for (u32 i = 0; i < p.paths.count(); i++) r2.add(p.paths.path(i));
    r2.uid = uid; r2.gid = gid;
    r2.recursive = p.recursive && p.any_dir;
  }
  char label[128]; snprintf(label, sizeof label, "%s", p.name);
  bool foreign = p.foreign; // someone else's: only root can, so go there directly
  bool root_needed = uid >= 0 && getuid() != 0; // giving a file away, or taking it, is root's alone
  dialog_close(a);
  if (mode_change) ops_submit(a, r, UNDO_CHMOD, label, false, foreign);
  if (owner_change) ops_submit(a, r2, UNDO_CHOWN, label, false, foreign || root_needed);
}

enum { SET_THEME_BUTTONS = 3, SET_VIEW_BUTTONS = 3, SET_CHECKS = 4 };
struct SettingsGeom {
  Rect theme[SET_THEME_BUTTONS]; // Omarchy / Light / Dark
  Rect font_minus, font_plus, font_reset, font_value;
  Rect view[SET_VIEW_BUTTONS]; // Icons / List / Details
  Rect checks[SET_CHECKS]; // hidden files, folders first, thumbnails, sidebar
  Rect terminal_in, terminal_label;
  Rect config_open;
  i32  row_h, label_w, body_h;
  i32  y_theme, y_font, y_view, y_checks, y_terminal, y_config;
};
static const char* const kSetThemeLabels[SET_THEME_BUTTONS] = { "Omarchy", "Light", "Dark" };
static const char* const kSetViewLabels[SET_VIEW_BUTTONS] = { "Icons", "List", "Details" };
static const char* const kSetCheckLabels[SET_CHECKS] = { "Show hidden files", "Folders before files", "Thumbnails for pictures", "Show the sidebar" };

static void settings_body(App& a, Ui& ui, i32 box_x, i32 box_w, i32 y, i32 pad, SettingsGeom& g) {
  i32 lh = text_line_height(a.text);
  g.row_h = lh + ui_px(ui, 12);
  g.label_w = ui_px(ui, 110);
  i32 x0 = box_x + pad, cx = x0 + g.label_w, cw = box_w - 2 * pad - g.label_w;
  i32 bh = ui_px(ui, 26), gap = ui_px(ui, 6);

  g.y_theme = y;
  { i32 bw = mx_min((cw - 2 * gap) / 3, ui_px(ui, 96));
    for (u32 i = 0; i < SET_THEME_BUTTONS; i++) g.theme[i] = { cx + (i32)i * (bw + gap), y + (g.row_h - bh) / 2, bw, bh }; }
  y += g.row_h + ui_px(ui, 4);

  g.y_font = y;
  g.font_minus = { cx, y + (g.row_h - bh) / 2, bh, bh };
  g.font_value = { rect_x1(g.font_minus) + gap, y, ui_px(ui, 52), g.row_h };
  g.font_plus = { rect_x1(g.font_value) + gap, y + (g.row_h - bh) / 2, bh, bh };
  g.font_reset = { rect_x1(g.font_plus) + gap * 2, y + (g.row_h - bh) / 2, ui_px(ui, 64), bh };
  y += g.row_h + ui_px(ui, 4);

  g.y_view = y;
  { i32 bw = mx_min((cw - 2 * gap) / 3, ui_px(ui, 96));
    for (u32 i = 0; i < SET_VIEW_BUTTONS; i++) g.view[i] = { cx + (i32)i * (bw + gap), y + (g.row_h - bh) / 2, bw, bh }; }
  y += g.row_h + ui_px(ui, 8);

  g.y_checks = y;
  for (u32 i = 0; i < SET_CHECKS; i++) { g.checks[i] = { cx, y, cw, lh + ui_px(ui, 6) }; y += lh + ui_px(ui, 10); }
  y += ui_px(ui, 4);

  g.y_terminal = y;
  g.terminal_label = { x0, y, g.label_w, g.row_h };
  g.terminal_in = { cx, y + (g.row_h - bh - ui_px(ui, 2)) / 2, mx_min(cw, ui_px(ui, 320)), bh + ui_px(ui, 2) };
  y += g.row_h + ui_px(ui, 8);

  g.y_config = y;
  g.config_open = { cx, y + (g.row_h - bh) / 2, ui_px(ui, 110), bh };
  y += g.row_h;
  g.body_h = y - (g.y_theme);
}

static SettingsGeom g_settings_geom;

void settings_open(App& a) {
  if (a.dialog.kind != DLG_NONE) return;
  if (a.menu.open) menu_close(a);
  Dialog& d = a.dialog;
  d.kind = DLG_SETTINGS;
  snprintf(d.title, sizeof d.title, "Settings");
  d.nlines = 0;
  d.nbuttons = 0; d.buttons[d.nbuttons++] = "Close";
  d.focus = 0; d.cancel = 0; d.hover = -1; d.check_label = nullptr; d.checked = false;
  ui_text_set(a.settings.terminal_in, a.cfg.terminal);
  a.settings.focus_field = 0;
  a.path_editing = false;
  a.w.need_redraw = true;
}

void settings_apply_thumbs(App& a) { a.no_thumbs = a.no_thumbs_arg || !a.cfg.thumbnails; }

static void settings_set_theme(App& a, u32 which) {
  if (which == 0 && !a.theme_synced) return;
  a.cfg.theme_follow = which == 0;
  if (which == 0) a.light = a.theme_is_light; else a.light = which == 1;
  a.cfg.light = a.light; config_mark_dirty(a);
}
static void settings_toggle(App& a, u32 which) {
  switch (which) {
    case 0: a.cfg.show_hidden = !a.cfg.show_hidden; for (Tab& ti : a.tabs) if (ti.used) { clear_selection(a, ti); ti.cursor = ti.anchor = -1; rebuild_rows(a, ti); } break;
    case 1: a.cfg.sort.dirs_first = !a.cfg.sort.dirs_first; for (Tab& ti : a.tabs) if (ti.used) { ti.sort.dirs_first = a.cfg.sort.dirs_first; resort(a, ti); } break;
    case 2: a.cfg.thumbnails = !a.cfg.thumbnails; settings_apply_thumbs(a); break;
    case 3: a.cfg.sidebar_visible = !a.cfg.sidebar_visible; break;
    default: break;
  }
  config_mark_dirty(a);
}
static void settings_open_config(App& a) {
  char dir[512], path[600];
  const char* xdg = getenv("XDG_CONFIG_HOME");
  const char* home = getenv("HOME");
  if (xdg && xdg[0]) snprintf(dir, sizeof dir, "%s/mattexplorer", xdg); else if (home) snprintf(dir, sizeof dir, "%s/.config/mattexplorer", home); else return;
  config_save(a); // so the file exists with what is set now
  snprintf(path, sizeof path, "%s/config", dir);
  char err[256], msg[600];
  if (ensure_apps(a)) {
    u32 apps[4]; u32 n = apps_for_mime(a.mime, a.apps, "text/plain", apps, 4);
    if (n) {
      Array<char> buf; Array<char*> argv; const char* paths[1] = { path };
      if (desktop_argv(a.apps, a.apps.apps[apps[0]], paths, 1, buf, argv, err, sizeof err, a.cfg.terminal) && spawn_detached(argv.data, nullptr, err, sizeof err)) {
        snprintf(msg, sizeof msg, "Opened %s with %s (it is rewritten on exit)", path, a.apps.s(a.apps.apps[apps[0]].name)); set_status(a, msg); a.launches++; return;
      }
    }
  }
  const char* argv2[3] = { "xdg-open", path, nullptr };
  if (command_exists("xdg-open") && spawn_detached(argv2, nullptr, err, sizeof err)) { snprintf(msg, sizeof msg, "Opened %s", path); set_status(a, msg); a.launches++; return; }
  snprintf(msg, sizeof msg, "No text editor found for %s", path); set_status_error(a, msg);
}

struct DialogGeom { Rect box; Rect buttons[DIALOG_BUTTONS]; Rect check; Rect password; i32 pad, lh; PropsGeom* props; SettingsGeom* settings; };

static PropsGeom g_props_geom;

static u32 wrap_left(App& a, const char* text, i32 x, i32 y, i32 w, i32 line_h, u32 max_lines, u32 color, bool draw) {
  Canvas& c = *a.ui.c;
  u32 n = (u32)strlen(text), start = 0, lines = 0;
  while (start < n && lines < max_lines) {
    u32 end = n, last_space = 0;
    for (u32 i = start; i <= n; i++) {
      if (i == n || text[i] == ' ') {
        if (text_width(a.text, Str(text + start, i - start)) > (float)w) { end = last_space > start ? last_space : (i == start ? i + 1 : i); break; }
        last_space = i;
        if (i == n) end = n;
      }
    }
    bool last = lines + 1 == max_lines;
    if (draw) {
      i32 bl = ui_baseline(a.ui, a.text, y + (i32)lines * line_h, line_h);
      if (last && end < n) text_draw_elided(c, a.text, Str(text + start), (float)x, bl, (float)w, color);
      else text_draw(c, a.text, Str(text + start, end - start), (float)x, bl, color);
    }
    lines++;
    start = end;
    while (start < n && text[start] == ' ') start++;
  }
  return lines ? lines : 1;
}

static DialogGeom dialog_geom(App& a, Ui& ui) {
  Dialog& d = a.dialog; Canvas& c = *ui.c;
  DialogGeom g = {};
  g.pad = ui_px(ui, 18); g.lh = text_line_height(a.text);
  i32 bh = ui_px(ui, 30), gap = ui_px(ui, 8);
  i32 bw = ui_px(ui, 84);
  for (u32 i = 0; i < d.nbuttons; i++) bw = mx_max(bw, (i32)text_width(a.text, d.buttons[i]) + ui_px(ui, 28));
  float w = text_width(a.text, d.title) + (float)(g.pad * 2 + ui_px(ui, 26));
  for (u32 i = 0; i < d.nlines; i++) w = mx_max(w, mx_min(text_width(a.text, d.lines[i]), (float)ui_px(ui, 520)) + (float)g.pad * 2); // long lines wrap
  if (d.check_label) w = mx_max(w, text_width(a.text, d.check_label) + (float)(g.pad * 2 + ui_px(ui, 24)));
  w = mx_max(w, (float)((i32)d.nbuttons * bw + (i32)(d.nbuttons ? d.nbuttons - 1 : 0) * gap + g.pad * 2));
  bool props = d.kind == DLG_PROPERTIES, sudo = d.kind == DLG_SUDO, settings = d.kind == DLG_SETTINGS;
  if (props) w = mx_max(w, (float)ui_px(ui, 540));
  if (sudo) w = mx_max(w, (float)ui_px(ui, 520));
  if (settings) w = mx_max(w, (float)ui_px(ui, 480));
  i32 box_w = mx_clamp((i32)w + 1, ui_px(ui, 360), mx_max(c.w - ui_px(ui, 40), ui_px(ui, 240)));
  i32 lines_h = 0;
  for (u32 i = 0; i < d.nlines; i++) lines_h += (i32)wrap_left(a, d.lines[i], 0, 0, box_w - 2 * g.pad, g.lh + ui_px(ui, 2), 3, 0, false) * (g.lh + ui_px(ui, 2)) + ui_px(ui, 2);
  i32 body_h = lines_h;
  if (props) { props_body(a, ui, 0, box_w, 0, g.pad, g_props_geom); body_h = lines_h + g_props_geom.body_h; g.props = &g_props_geom; }
  if (settings) { settings_body(a, ui, 0, box_w, 0, g.pad, g_settings_geom); body_h = lines_h + g_settings_geom.body_h; g.settings = &g_settings_geom; }
  i32 pw_h = ui_px(ui, 30);
  if (sudo) body_h += ui_px(ui, 10) + g.lh + ui_px(ui, 6) + pw_h;
  i32 box_h = g.pad + g.lh + ui_px(ui, 12) + body_h + (d.check_label ? g.lh + ui_px(ui, 14) : 0) + ui_px(ui, 14) + bh + g.pad;
  box_h = mx_min(box_h, c.h);
  g.box = { (c.w - box_w) / 2, mx_max((c.h - box_h) / 2 - ui_px(ui, 24), 0), box_w, box_h };
  if (props) props_body(a, ui, g.box.x, box_w, g.box.y + g.pad + g.lh + ui_px(ui, 12) + lines_h, g.pad, g_props_geom); // at its real place
  if (settings) settings_body(a, ui, g.box.x, box_w, g.box.y + g.pad + g.lh + ui_px(ui, 12) + lines_h, g.pad, g_settings_geom);
  if (sudo) {
    i32 py = g.box.y + g.pad + g.lh + ui_px(ui, 12) + lines_h + ui_px(ui, 10) + g.lh + ui_px(ui, 6);
    g.password = { g.box.x + g.pad, py, mx_min(box_w - 2 * g.pad, ui_px(ui, 300)), pw_h };
  }
  i32 bx = rect_x1(g.box) - g.pad - (i32)d.nbuttons * bw - (i32)(d.nbuttons ? d.nbuttons - 1 : 0) * gap;
  i32 by = rect_y1(g.box) - g.pad - bh;
  for (u32 i = 0; i < d.nbuttons; i++) g.buttons[i] = { bx + (i32)i * (bw + gap), by, bw, bh };
  if (d.check_label) g.check = { g.box.x + g.pad, by - ui_px(ui, 14) - g.lh, box_w - 2 * g.pad, g.lh + ui_px(ui, 4) };
  return g;
}

static void dialog_submit_pending(App& a, bool elevated = false) {
  Dialog& d = a.dialog;
  FileOpRequest r;
  r.kind = d.pending.kind;
  r.paths = static_cast<Array<char>&&>(d.pending.paths);
  r.off = static_cast<Array<u32>&&>(d.pending.off);
  memcpy(r.dest, d.pending.dest, sizeof r.dest);
  r.mode_mask = d.pending.mode_mask; r.mode_value = d.pending.mode_value; r.recursive = d.pending.recursive;
  r.uid = d.pending.uid; r.gid = d.pending.gid;
  u8 undo = d.pending_undo; bool is_undo = d.pending_is_undo, is_redo = d.pending_is_redo;
  RedoRecord* rr = d.pending_redo; d.pending_redo = nullptr;
  char label[128]; snprintf(label, sizeof label, "%s", d.pending_label);
  char password[256];
  snprintf(password, sizeof password, "%s", elevated ? d.password.buf : "");
  dialog_close(a); // zeroes the field
  if (!ops_submit(a, r, undo, label, is_undo, elevated, elevated ? password : nullptr, rr, is_redo)) redo_record_free(rr);
  memset(password, 0, sizeof password);
}

static void dialog_activate(App& a, u32 i) {
  Dialog& d = a.dialog;
  if (i >= d.nbuttons) return;
  switch (d.kind) {
    case DLG_CONFLICT: {
      static const ConflictAnswer answers[DIALOG_BUTTONS] = { CONFLICT_REPLACE, CONFLICT_SKIP, CONFLICT_KEEP_BOTH, CONFLICT_CANCEL };
      ops_answer(a, answers[i], d.checked && answers[i] != CONFLICT_CANCEL);
      break;
    }
    case DLG_CONFIRM_DELETE: case DLG_TRASH_FAILED:
      if (i == 0) dialog_submit_pending(a); else dialog_close(a);
      break;
    case DLG_OP_ERROR: {
      static const ErrorAnswer answers[DIALOG_BUTTONS] = { ERR_RETRY, ERR_SKIP, ERR_SKIP, ERR_CANCEL };
      ops_answer_error(a, answers[i], i == 2);
      break;
    }
    case DLG_PROPERTIES:
      if (i == 0) props_apply(a); else dialog_close(a);
      break;
    case DLG_SUDO:
      if (i == 0) { if (d.password.len) dialog_submit_pending(a, true); } // an empty password is a no-op, not a submit
      else dialog_close(a);
      break;
    default: dialog_close(a); break;
  }
  a.w.need_redraw = true;
}

static const u32 kPropsBits[PROPS_BOXES] = { 0400, 0200, 0100, 040, 020, 010, 04, 02, 01, 04000, 02000, 01000 };

static void dialog_input(App& a, Ui& ui) {
  Dialog& d = a.dialog;
  DialogGeom g = dialog_geom(a, ui);
  i32 clicked = -1; bool toggle = false;
  for (u32 i = 0; i < d.nbuttons; i++) if (ui_pressed(ui, g.buttons[i], BTN_LEFT)) clicked = (i32)i;
  if (d.check_label && ui_pressed(ui, g.check, BTN_LEFT)) toggle = true;
  Rect keep = {};
  if (d.kind == DLG_SUDO) keep = g.password;
  if (g.props) {
    Props& p = a.props; PropsGeom& pg = *g.props;
    for (u32 i = 0; i < PROPS_BOXES; i++) if (ui_pressed(ui, pg.boxes[i], BTN_LEFT)) { p.new_mode ^= kPropsBits[i]; ui.want_redraw = true; }
    if (pg.recursive.w && ui_pressed(ui, pg.recursive, BTN_LEFT)) { p.recursive = !p.recursive; ui.want_redraw = true; }
    for (UiClick& k : ui.clicks) {
      if (k.used || !k.pressed || k.button != BTN_LEFT) continue;
      if (pg.popup_n && rect_has(pg.popup, k.x, k.y)) {
        i32 row = (i32)((k.y - (float)(pg.popup.y + ui_px(ui, 4))) / (float)pg.row_h);
        if (row >= 0 && row < pg.popup_n) {
          const NameList& L = p.focus_field == 1 ? p.users : p.groups;
          ui_text_set(p.focus_field == 1 ? p.owner_in : p.group_in, L.name(pg.popup_items[row]));
          p.popup_hide = true;
        }
        k.used = 1;
      } else if (rect_has(pg.owner_in, k.x, k.y)) { if (p.focus_field != 1) { p.focus_field = 1; p.popup_hide = false; p.popup_sel = 0; } }
      else if (rect_has(pg.group_in, k.x, k.y)) { if (p.focus_field != 2) { p.focus_field = 2; p.popup_hide = false; p.popup_sel = 0; } }
      else if (pg.take_btn.w && rect_has(pg.take_btn, k.x, k.y)) { ui_text_set(p.owner_in, p.my_name); ui_text_set(p.group_in, p.my_group); p.focus_field = 0; k.used = 1; }
      else p.focus_field = 0;
      ui.want_redraw = true;
    }
    if (p.focus_field == 1) keep = pg.owner_in; else if (p.focus_field == 2) keep = pg.group_in;
  }
  if (g.settings) { // the settings controls apply at once
    SettingsGeom& sg = *g.settings; Settings& st = a.settings;
    for (u32 i = 0; i < SET_THEME_BUTTONS; i++) if (ui_pressed(ui, sg.theme[i], BTN_LEFT)) { settings_set_theme(a, i); ui.want_redraw = true; }
    if (ui_pressed(ui, sg.font_minus, BTN_LEFT)) { a.cfg.font_px = mx_max(a.cfg.font_px - 1.0f, 7.0f); config_mark_dirty(a); ui.want_redraw = true; }
    if (ui_pressed(ui, sg.font_plus, BTN_LEFT)) { a.cfg.font_px = mx_min(a.cfg.font_px + 1.0f, 40.0f); config_mark_dirty(a); ui.want_redraw = true; }
    if (ui_pressed(ui, sg.font_reset, BTN_LEFT)) { a.cfg.font_px = 13.0f; config_mark_dirty(a); ui.want_redraw = true; }
    for (u32 i = 0; i < SET_VIEW_BUTTONS; i++) if (ui_pressed(ui, sg.view[i], BTN_LEFT)) { set_view(a, tab_active(a), i == 0 ? VIEW_ICONS : i == 1 ? VIEW_LIST : VIEW_DETAILS); ui.want_redraw = true; }
    for (u32 i = 0; i < SET_CHECKS; i++) if (ui_pressed(ui, sg.checks[i], BTN_LEFT)) { settings_toggle(a, i); ui.want_redraw = true; }
    if (ui_pressed(ui, sg.config_open, BTN_LEFT)) settings_open_config(a);
    for (UiClick& k : ui.clicks) {
      if (k.used || !k.pressed || k.button != BTN_LEFT) continue;
      if (rect_has(sg.terminal_in, k.x, k.y)) { if (st.focus_field != 1) st.focus_field = 1; }
      else st.focus_field = 0;
    }
    if (st.focus_field == 1) keep = sg.terminal_in;
  }
  for (UiClick& k : ui.clicks) if (!(keep.w && rect_has(keep, k.x, k.y))) k.used = 1;
  ui.scroll_dx = ui.scroll_dy = 0;
  ui.mouse_inside = false;
  if (toggle) { d.checked = !d.checked; ui.want_redraw = true; }
  if (clicked >= 0) { dialog_activate(a, (u32)clicked); ui.want_redraw = true; }
}

static void draw_dialog(App& a, Ui& ui) {
  Dialog& d = a.dialog;
  if (d.kind == DLG_NONE) return;
  const UiTheme& th = theme(a); Canvas& c = *ui.c;
  DialogGeom g = dialog_geom(a, ui);
  if (d.kind == DLG_PROPERTIES && a.props.scan) ui.wake_at_ms = ui.now_ms + 250; // the walk's live numbers
  canvas_fill(c, 0, 0, c.w, c.h, rgba_hex(0x000000, a.light ? 70 : 120));
  canvas_fill_rounded(c, g.box.x, g.box.y, g.box.w, g.box.h, ui_px(ui, 8), rgb_hex(th.panel));
  canvas_stroke(c, g.box.x, g.box.y, g.box.w, g.box.h, rgba_hex(th.border, 255));
  i32 y = g.box.y + g.pad;
  i32 isz = ui_px(ui, 18);
  bool danger = d.kind == DLG_CONFIRM_DELETE || d.kind == DLG_TRASH_FAILED || d.kind == DLG_ERROR;
  ui_icon(ui, danger || d.kind == DLG_SUDO ? ICON_WARNING : d.kind == DLG_SETTINGS ? ICON_GEAR : ICON_COPY, g.box.x + g.pad, y + (g.lh - isz) / 2, isz, rgb_hex(danger ? th.red : th.accent), rgb_hex(th.panel));
  text_draw_elided(c, a.text, d.title, (float)(g.box.x + g.pad + isz + ui_px(ui, 8)), ui_baseline(ui, a.text, y, g.lh), (float)(g.box.w - 2 * g.pad - isz - ui_px(ui, 8)), rgb_hex(th.text));
  y += g.lh + ui_px(ui, 12);
  for (u32 i = 0; i < d.nlines; i++) { // long explanations wrap, three lines at most
    bool red = d.error_last && i + 1 == d.nlines;
    u32 used = wrap_left(a, d.lines[i], g.box.x + g.pad, y, g.box.w - 2 * g.pad, g.lh + ui_px(ui, 2), 3, rgb_hex(red ? th.red : th.muted), true);
    y += (i32)used * (g.lh + ui_px(ui, 2)) + ui_px(ui, 2);
  }
  if (d.kind == DLG_SUDO) {
    char who[100]; snprintf(who, sizeof who, "Password for %s", a.user_name[0] ? a.user_name : "you");
    text_draw(c, a.text, who, (float)g.password.x, ui_baseline(ui, a.text, g.password.y - ui_px(ui, 6) - g.lh, g.lh), rgb_hex(th.text));
    ui_text_input(ui, ui_id("dlg-password"), g.password, d.password, nullptr, true, -1);
  }
  if (g.settings) {
    SettingsGeom& sg = *g.settings; Settings& st = a.settings;
    i32 x0 = g.box.x + g.pad;
    u32 muted = rgb_hex(th.muted), textc = rgb_hex(th.text);
    auto label = [&](const char* s2, i32 yy) { text_draw(c, a.text, s2, (float)x0, ui_baseline(ui, a.text, yy, sg.row_h), muted); };
    label("Theme", sg.y_theme);
    for (u32 i = 0; i < SET_THEME_BUTTONS; i++) {
      bool on = i == 0 ? (a.theme_synced && a.cfg.theme_follow) : (!a.cfg.theme_follow || !a.theme_synced) && (i == 1) == a.light;
      ui_button(ui, ui_id("set-theme", i), sg.theme[i], kSetThemeLabels[i], -1, i != 0 || a.theme_synced, on);
    }
    if (!a.theme_synced) ui_tooltip(ui, ui_id("set-theme", 0), sg.theme[0], "No Omarchy theme found");
    label("Font size", sg.y_font);
    ui_button(ui, ui_id("set-font-minus"), sg.font_minus, "\xe2\x88\x92", -1, a.cfg.font_px > 7.0f);
    { char v[32]; snprintf(v, sizeof v, "%g px", (double)a.cfg.font_px); float vw = text_width(a.text, v);
      text_draw(c, a.text, v, (float)sg.font_value.x + ((float)sg.font_value.w - vw) * 0.5f, ui_baseline(ui, a.text, sg.font_value.y, sg.font_value.h), textc); }
    ui_button(ui, ui_id("set-font-plus"), sg.font_plus, "+", -1, a.cfg.font_px < 40.0f);
    ui_button(ui, ui_id("set-font-reset"), sg.font_reset, "Reset", -1, a.cfg.font_px != 13.0f);
    label("View", sg.y_view);
    ViewMode cur = tab_active(a).view;
    for (u32 i = 0; i < SET_VIEW_BUTTONS; i++) ui_button(ui, ui_id("set-view", i), sg.view[i], kSetViewLabels[i], -1, true, cur == (i == 0 ? VIEW_ICONS : i == 1 ? VIEW_LIST : VIEW_DETAILS));
    bool vals[SET_CHECKS] = { a.cfg.show_hidden, a.cfg.sort.dirs_first, a.cfg.thumbnails, a.cfg.sidebar_visible };
    for (u32 i = 0; i < SET_CHECKS; i++) { bool v = vals[i]; ui_checkbox(ui, ui_id("set-check", i), sg.checks[i], kSetCheckLabels[i], &v); } // clicks were applied in dialog_input
    if (a.no_thumbs_arg) text_draw(c, a.small, "(off for this run: --no-thumbs)", (float)rect_x1(sg.checks[2]) - text_width(a.small, "(off for this run: --no-thumbs)"), ui_baseline(ui, a.small, sg.checks[2].y, sg.checks[2].h), muted);
    label("Terminal", sg.y_terminal);
    ui_text_input(ui, ui_id("set-terminal"), sg.terminal_in, st.terminal_in, "$TERMINAL, else alacritty, ghostty, kitty, foot\xe2\x80\xa6", st.focus_field == 1, -1);
    label("Settings file", sg.y_config);
    ui_button(ui, ui_id("set-config"), sg.config_open, "Open in editor", -1, true);
    { const char* home = getenv("HOME"); char shown[600]; const char* xdg = getenv("XDG_CONFIG_HOME");
      if (xdg && xdg[0]) snprintf(shown, sizeof shown, "%s/mattexplorer/config", xdg); else snprintf(shown, sizeof shown, "~/.config/mattexplorer/config");
      (void)home;
      text_draw_elided(c, a.small, shown, (float)(rect_x1(sg.config_open) + ui_px(ui, 10)), ui_baseline(ui, a.small, sg.config_open.y, sg.config_open.h), (float)(rect_x1(g.box) - g.pad - rect_x1(sg.config_open) - ui_px(ui, 10)), muted); }
  }
  if (g.props) {
    PropsGeom& pg = *g.props; Props& p = a.props;
    i32 x0 = g.box.x + g.pad;
    for (u32 i = 0; i < pg.nrows; i++) {
      i32 bl = ui_baseline(ui, a.text, y, pg.row_h);
      i32 lw = pg.rows[i].label[0] ? pg.label_w : 0;
      if (lw) text_draw(c, a.text, pg.rows[i].label, (float)x0, bl, rgb_hex(th.muted));
      text_draw_elided(c, a.text, pg.rows[i].value, (float)(x0 + lw), bl, (float)(g.box.w - 2 * g.pad - lw), rgb_hex(pg.rows[i].muted ? th.muted : th.text));
      y += pg.row_h;
    }

    {
      i32 fb = ui_baseline(ui, a.text, pg.owner_in.y, pg.owner_in.h);
      text_draw(c, a.text, "Owner", (float)x0, fb, rgb_hex(th.muted));
      ui_text_input(ui, ui_id("prop-owner"), pg.owner_in, p.owner_in, "user", p.focus_field == 1, -1);
      text_draw(c, a.text, "Group", (float)(rect_x1(pg.owner_in) + ui_px(ui, 8)), fb, rgb_hex(th.muted));
      ui_text_input(ui, ui_id("prop-group"), pg.group_in, p.group_in, "group", p.focus_field == 2, -1);
      if (pg.take_btn.w) { ui_button(ui, ui_id("prop-take"), pg.take_btn, "Take ownership", -1, true); canvas_stroke(c, pg.take_btn.x, pg.take_btn.y, pg.take_btn.w, pg.take_btn.h, rgba_hex(th.border, 255)); }
    }

    if (!pg.grid.h) goto grid_done;
    {
    i32 gy = pg.grid.y, bl = ui_baseline(ui, a.text, gy, pg.row_h);
    text_draw(c, a.text, "Permissions", (float)x0, bl, rgb_hex(th.muted));
    static const char* heads[3] = { "Read", "Write", "Execute" };
    static const char* whos[3] = { "Owner", "Group", "Others" };
    for (u32 cc = 0; cc < 3; cc++) text_draw(c, a.text, heads[cc], (float)pg.boxes[cc].x, bl, rgb_hex(th.muted));
    for (u32 r = 0; r < 3; r++) {
      i32 rb = ui_baseline(ui, a.text, pg.boxes[r * 3].y, pg.row_h);
      text_draw(c, a.text, whos[r], (float)(x0 + ui_px(ui, 12)), rb, rgb_hex(th.text));
      for (u32 cc = 0; cc < 3; cc++) {
        bool on = (p.new_mode & kPropsBits[r * 3 + cc]) != 0;
        bool was = (p.mode & kPropsBits[r * 3 + cc]) != 0;
        ui_checkbox(ui, ui_id("perm", r * 3 + cc), pg.boxes[r * 3 + cc], nullptr, &on);
        if (on != was) canvas_stroke(c, pg.boxes[r * 3 + cc].x - ui_px(ui, 2), pg.boxes[r * 3 + cc].y, ui_px(ui, 19), pg.boxes[r * 3 + cc].h, rgba_hex(th.accent, 160)); // a changed box
      }
    }
    static const char* specials[3] = { "setuid", "setgid", "sticky" };
    for (u32 i = 0; i < 3; i++) { bool on = (p.new_mode & kPropsBits[9 + i]) != 0; ui_checkbox(ui, ui_id("perm", 9 + i), pg.boxes[9 + i], specials[i], &on); }
    char oct[64];
    snprintf(oct, sizeof oct, "%s%04o%s", p.modes_differ ? "first: " : "", p.new_mode, props_changed(p) ? "  \xc2\xb7 changed" : "");
    float ow = text_width(a.text, oct);
    text_draw(c, a.text, oct, (float)(rect_x1(g.box) - g.pad) - ow, ui_baseline(ui, a.text, pg.boxes[0].y, pg.row_h), rgb_hex(props_changed(p) ? th.accent : th.muted)); // right of the Owner row
    if (pg.recursive.w) {
      bool rec = p.recursive;
      ui_checkbox(ui, ui_id("perm-recursive"), pg.recursive, p.count == 1 ? "Apply the changes to everything inside the folder too" : "Apply the changes to everything inside the folders too", &rec);
    }
    }
    grid_done:;
  }
  if (d.check_label) ui_checkbox(ui, ui_id("dlg-check"), g.check, d.check_label, &d.checked);
  for (u32 i = 0; i < d.nbuttons; i++) {
    Rect r = g.buttons[i];
    bool hov = ui.mouse_inside && rect_has(r, ui.mx, ui.my);
    bool focus = i == d.focus && !(d.kind == DLG_PROPERTIES && a.props.focus_field);
    canvas_fill_rounded(c, r.x, r.y, r.w, r.h, ui_px(ui, 5), rgb_hex(hov ? th.hover : th.bar));
    if (focus) canvas_stroke(c, r.x, r.y, r.w, r.h, rgba_hex(th.accent, 220));
    else canvas_stroke(c, r.x, r.y, r.w, r.h, rgba_hex(th.border, 200));
    u32 fg = rgb_hex(danger && i == 0 ? th.red : th.text);
    float lw = text_width(a.text, d.buttons[i]);
    text_draw(c, a.text, d.buttons[i], (float)r.x + ((float)r.w - lw) * 0.5f, ui_baseline(ui, a.text, r.y, r.h), fg);
    if (hov) ui.cursor = CURSOR_POINTER;
  }

  if (g.props && g.props->popup_n) {
    PropsGeom& pg = *g.props; Props& p = a.props;
    const NameList& L = p.focus_field == 1 ? p.users : p.groups;
    Rect pr = pg.popup;
    canvas_fill_rounded(c, pr.x, pr.y, pr.w, pr.h, ui_px(ui, 5), rgb_hex(th.panel));
    canvas_stroke(c, pr.x, pr.y, pr.w, pr.h, rgba_hex(th.border, 255));
    for (i32 i = 0; i < pg.popup_n; i++) {
      Rect r = { pr.x + ui_px(ui, 4), pr.y + ui_px(ui, 4) + i * pg.row_h, pr.w - ui_px(ui, 8), pg.row_h };
      bool hov = ui.mouse_inside && rect_has(r, ui.mx, ui.my);
      if (hov) p.popup_sel = i;
      if (i == p.popup_sel) canvas_fill_rounded(c, r.x, r.y, r.w, r.h, ui_px(ui, 4), rgb_hex(th.hover));
      char idb[24]; snprintf(idb, sizeof idb, "%u", L.ids[pg.popup_items[i]]);
      i32 bl = ui_baseline(ui, a.text, r.y, r.h);
      text_draw(c, a.text, L.name(pg.popup_items[i]), (float)(r.x + ui_px(ui, 8)), bl, rgb_hex(th.text));
      text_draw(c, a.text, idb, (float)(rect_x1(r) - ui_px(ui, 8)) - text_width(a.text, idb), bl, rgb_hex(th.muted));
      if (hov) ui.cursor = CURSOR_POINTER;
    }
  }
}

void dialog_confirm(App& a) { if (a.dialog.kind != DLG_NONE) dialog_activate(a, 0); }

void dialog_close(App& a) {
  Dialog& d = a.dialog;
  if (d.kind == DLG_PROPERTIES) props_scan_cancel(a);
  memset(d.password.buf, 0, sizeof d.password.buf); d.password.len = d.password.cursor = 0; d.password.sel_anchor = 0xFFFFFFFF;
  d.error_last = false; d.pending_is_undo = d.pending_is_redo = false;
  redo_record_free(d.pending_redo); d.pending_redo = nullptr;
  d.kind = DLG_NONE; d.job = nullptr; d.nlines = 0; d.nbuttons = 0; d.check_label = nullptr; d.checked = false;
  d.pending.paths.clear(); d.pending.off.clear();
  a.ui.want_redraw = true;
  a.w.need_redraw = true;
}

void dialog_error(App& a, const char* title, const char* line) {
  Dialog& d = a.dialog;
  if (d.kind != DLG_NONE) { set_status_error(a, line ? line : title); return; } // never over another dialog
  d.kind = DLG_ERROR;
  snprintf(d.title, sizeof d.title, "%s", title);
  d.nlines = 0;
  if (line && line[0]) snprintf(d.lines[d.nlines++], sizeof d.lines[0], "%s", line);
  d.nbuttons = 0; d.buttons[d.nbuttons++] = "OK";
  d.focus = 0; d.cancel = 0; d.hover = -1; d.check_label = nullptr;
  a.w.need_redraw = true;
}

static void dialog_key(App& a, u32 ks, u32 mods, const char* text, u32 text_len) {
  Dialog& d = a.dialog;
  bool shift = (mods & XKB_MOD_SHIFT) != 0;
  if (d.kind == DLG_SETTINGS) {
    Settings& st = a.settings;
    bool tab = ks == XKB_KEY_Tab || ks == XKB_KEY_ISO_Left_Tab;
    if (tab) { st.focus_field = st.focus_field ? 0 : 1; if (st.focus_field) { st.terminal_in.sel_anchor = 0; st.terminal_in.cursor = st.terminal_in.len; } a.w.need_redraw = true; return; }
    if (st.focus_field == 1) {
      switch (ui_text_input_key(st.terminal_in, ks, mods, text, text_len)) {
        case UI_TEXT_CHANGED: snprintf(a.cfg.terminal, sizeof a.cfg.terminal, "%.*s", (int)st.terminal_in.len, st.terminal_in.buf); config_mark_dirty(a); break;
        case UI_TEXT_SUBMIT: st.focus_field = 0; break;
        case UI_TEXT_CANCEL: dialog_close(a); break;
        default: break;
      }
      a.w.need_redraw = true;
      return;
    }
    if (ks == XKB_KEY_Escape || ks == XKB_KEY_Return || ks == XKB_KEY_KP_Enter) { dialog_close(a); return; }
    return;
  }
  if (d.kind == DLG_PROPERTIES) {
    Props& p = a.props; PropsGeom& pg = g_props_geom;
    bool tab = ks == XKB_KEY_Tab || ks == XKB_KEY_ISO_Left_Tab, back = shift || ks == XKB_KEY_ISO_Left_Tab;
    if (tab) {
      i32 before = p.focus_field;
      if (p.focus_field == 0) { if (!back && d.focus + 1 < d.nbuttons) d.focus++; else if (back && d.focus > 0) d.focus--; else p.focus_field = back ? 2 : 1; }
      else if (p.focus_field == 1) { if (back) { p.focus_field = 0; d.focus = d.nbuttons ? d.nbuttons - 1 : 0; } else p.focus_field = 2; }
      else { if (back) p.focus_field = 1; else { p.focus_field = 0; d.focus = 0; } }
      if (p.focus_field && p.focus_field != before) { UiTextInput& in = p.focus_field == 1 ? p.owner_in : p.group_in; in.sel_anchor = 0; in.cursor = in.len; p.popup_hide = false; p.popup_sel = 0; }
      return;
    }
    if (p.focus_field) {
      UiTextInput& in = p.focus_field == 1 ? p.owner_in : p.group_in;
      bool popup = pg.popup_n > 0 && !p.popup_hide;
      switch (ks) {
        case XKB_KEY_Escape: if (popup) p.popup_hide = true; else dialog_activate(a, d.cancel); return;
        case XKB_KEY_Up: case XKB_KEY_KP_Up: if (popup) p.popup_sel = mx_max(p.popup_sel - 1, 0); else p.popup_hide = false; return;
        case XKB_KEY_Down: case XKB_KEY_KP_Down: if (popup) p.popup_sel = mx_min(p.popup_sel + 1, pg.popup_n - 1); else p.popup_hide = false; return;
        case XKB_KEY_Return: case XKB_KEY_KP_Enter:
          if (popup && p.popup_sel >= 0 && p.popup_sel < pg.popup_n) {
            const NameList& L = p.focus_field == 1 ? p.users : p.groups;
            ui_text_set(in, L.name(pg.popup_items[p.popup_sel]));
            p.popup_hide = true;
          } else dialog_activate(a, 0);
          return;
        default:
          if (ui_text_input_key(in, ks, mods, text, text_len) == UI_TEXT_CHANGED) { p.popup_hide = false; p.popup_sel = 0; }
          return;
      }
    }
  }
  if (d.kind == DLG_SUDO && ks != XKB_KEY_Escape && ks != XKB_KEY_Tab && ks != XKB_KEY_ISO_Left_Tab) { // the field owns the keyboard
    switch (ui_text_input_key(d.password, ks, mods, text, text_len)) {
      case UI_TEXT_SUBMIT: dialog_activate(a, 0); return;
      case UI_TEXT_CANCEL: dialog_activate(a, d.cancel); return;
      default: return;
    }
  }
  switch (ks) {
    case XKB_KEY_Escape: dialog_activate(a, d.cancel); return;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_space: dialog_activate(a, d.focus); return;
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab:
      if (d.nbuttons) d.focus = shift || ks == XKB_KEY_ISO_Left_Tab ? (d.focus + d.nbuttons - 1) % d.nbuttons : (d.focus + 1) % d.nbuttons;
      return;
    case XKB_KEY_Left: case XKB_KEY_KP_Left: if (d.nbuttons) d.focus = (d.focus + d.nbuttons - 1) % d.nbuttons; return;
    case XKB_KEY_Right: case XKB_KEY_KP_Right: if (d.nbuttons) d.focus = (d.focus + 1) % d.nbuttons; return;
    default: break;
  }

  if (ks < 128 && d.kind != DLG_PROPERTIES) {
    u32 lower = ks | 0x20;
    for (u32 i = 0; i < d.nbuttons; i++) if (((u32)d.buttons[i][0] | 0x20) == lower) { dialog_activate(a, i); return; }
    if (d.check_label && lower == 'a') d.checked = !d.checked;
  }
}

static void menu_add(Menu& m, const char* label, const char* shortcut, i32 icon, u8 action, bool enabled, u16 arg = 0) {
  if (m.count >= MAX_MENU_ITEMS) return;
  m.items[m.count++] = { label, shortcut, (i8)icon, action, enabled, false, arg };
}
static void menu_sep(Menu& m) {
  if (!m.count || m.items[m.count - 1].separator || m.count >= MAX_MENU_ITEMS) return;
  m.items[m.count++] = { nullptr, nullptr, -1, MA_NONE, false, true, 0 };
}
static void menu_show(App& a, Tab* t, i32 row, i32 x, i32 y);

static bool pinned_here(const App& a, const char* path) {
  for (u32 i = 0; i < a.num_pins; i++) if (!strcmp(a.pins[i].path, path)) return true;
  return false;
}

void menu_open(App& a, Tab& t, i32 row, i32 x, i32 y) {
  if (a.dialog.kind != DLG_NONE) return;
  Menu& m = a.menu;
  if (t.renaming >= 0) rename_commit(a, t);
  a.path_editing = false;
  a.pending_rename_row = -1;
  if (row >= 0 && (u32)row < t.rows.len) {
    if (!t.selected[t.rows[(u32)row]]) set_cursor(a, t, row, false, false); // an unselected row becomes the selection, like Explorer
    else t.cursor = row;
  } else row = -1;
  m.count = 0; m.hover = -1; m.focus = -1; m.row = row; m.kind = MENU_ROWS; m.dev_entry = m.dev_disk = -1; m.mime[0] = 0;
  if (!a.pool.running && (!a.apps_ready || !a.mime_ready)) ensure_apps(a); // no pool: read the databases now
  bool trash = in_trash(t);
  if (row >= 0) {
    u32 i = t.rows[(u32)row];
    if (!(t.listing.flags[i] & (EF_STATTED | EF_STAT_FAILED)) && t.dirfd >= 0) dir_list_stat_range(t.dirfd, t.listing, i, 1);
    bool dir = t.listing.is_dir(i), one = t.selected_count == 1;
    bool exec = !dir && (t.listing.flags[i] & EF_STATTED) && (t.listing.mode[i] & 0111) && t.listing.kind[i] != EK_OTHER;
    if (trash) {
      menu_add(m, "Restore", nullptr, ICON_UNDO, MA_RESTORE, true);
      menu_add(m, "Delete permanently", "Shift+Delete", ICON_TRASH, MA_DELETE, true);
      menu_sep(m);
    }
    menu_add(m, "Open", "Enter", -1, MA_OPEN, true);
    if (dir) menu_add(m, "Open in new tab", "Ctrl+Enter", -1, MA_OPEN_TAB, true);
    else {
      if (a.apps_ready && a.mime_ready && one) entry_mime(a, t, i, m.mime, sizeof m.mime);
      menu_add(m, "Open with\xe2\x80\xa6", nullptr, -1, MA_OPEN_WITH_PAGE, a.apps_ready && a.mime_ready);
      if (exec) menu_add(m, "Run", nullptr, ICON_EXEC, MA_RUN, one);
    }
    if (dir && one) menu_add(m, "Open in terminal", nullptr, -1, MA_TERMINAL, true);
    menu_sep(m);
    menu_add(m, "Cut", "Ctrl+X", ICON_CUT, MA_CUT, !trash);
    menu_add(m, "Copy", "Ctrl+C", ICON_COPY, MA_COPY, true);
    menu_add(m, "Copy path", nullptr, -1, MA_COPY_PATH, true);
    menu_sep(m);
    if (!trash) {
      menu_add(m, "Rename", "F2", ICON_RENAME, MA_RENAME, one);
      menu_add(m, "Move to trash", "Delete", ICON_TRASH, MA_TRASH, true);
      menu_add(m, "Delete permanently", "Shift+Delete", -1, MA_DELETE, true);
    }
    if (dir && one && !trash) {
      char p[4096]; entry_path(t, i, p, sizeof p);
      menu_sep(m);
      snprintf(m.text[1], sizeof m.text[1], "%s", pinned_here(a, p) ? "Unpin from sidebar" : "Pin to sidebar");
      menu_add(m, m.text[1], nullptr, ICON_PIN, MA_PIN, true);
    }
    menu_sep(m);
    menu_add(m, "Properties", "Alt+Enter", -1, MA_PROPERTIES, true);
  } else {
    if (trash) { menu_add(m, "Empty trash", nullptr, ICON_TRASH, MA_EMPTY_TRASH, t.rows.len > 0 || t.hidden_count > 0); menu_sep(m); }
    menu_add(m, "Paste", "Ctrl+V", ICON_PASTE, MA_PASTE, clip_available(a) && !trash);
    menu_add(m, "New folder", "Ctrl+Shift+N", ICON_NEW_FOLDER, MA_NEW_FOLDER, t.dirfd >= 0 && !trash);
    menu_add(m, "Open in terminal", "F4", -1, MA_TERMINAL, true);
    menu_sep(m);
    if (a.undo_count) snprintf(m.text[0], sizeof m.text[0], "Undo %s", a.undo[a.undo_count - 1].label);
    else snprintf(m.text[0], sizeof m.text[0], "Undo");
    menu_add(m, m.text[0], "Ctrl+Z", ICON_UNDO, MA_UNDO, a.undo_count > 0);
    if (a.redo_count) snprintf(m.text[2], sizeof m.text[2], "Redo %s", a.redo[a.redo_count - 1].label);
    else snprintf(m.text[2], sizeof m.text[2], "Redo");
    menu_add(m, m.text[2], "Ctrl+Y", ICON_REFRESH, MA_REDO, a.redo_count > 0);
    menu_sep(m);
    menu_add(m, "Select all", "Ctrl+A", -1, MA_SELECT_ALL, t.rows.len > 0);
    menu_add(m, "Refresh", "F5", ICON_REFRESH, MA_REFRESH, true);
    menu_sep(m);
    snprintf(m.text[1], sizeof m.text[1], "%s", pinned_here(a, t.path) ? "Unpin from sidebar" : "Pin to sidebar");
    menu_add(m, m.text[1], "Ctrl+D", ICON_PIN, MA_PIN, true);
    menu_sep(m);
    menu_add(m, "Properties", "Alt+Enter", -1, MA_PROPERTIES, t.dirfd >= 0);
  }
  menu_show(a, &t, row, x, y);
}

void menu_open_with(App& a, Tab& t, i32 row, i32 x, i32 y) {
  if (a.dialog.kind != DLG_NONE || row < 0 || (u32)row >= t.rows.len) return;
  if (!ensure_apps(a)) return;
  Menu& m = a.menu;
  u32 i = t.rows[(u32)row];
  m.count = 0; m.hover = -1; m.focus = -1; m.row = row; m.kind = MENU_OPEN_WITH; m.dev_entry = m.dev_disk = -1;
  entry_mime(a, t, i, m.mime, sizeof m.mime);
  m.napps = apps_for_mime(a.mime, a.apps, m.mime, m.apps, MX_ARRAY_COUNT(m.apps));
  snprintf(m.text[3], sizeof m.text[3], "%s", m.mime);
  menu_add(m, m.text[3], nullptr, -1, MA_NONE, false);
  menu_sep(m);
  if (!m.napps) { snprintf(m.text[0], sizeof m.text[0], "No application for this type"); menu_add(m, m.text[0], nullptr, -1, MA_NONE, false); }
  for (u32 k = 0; k < m.napps && k < 14; k++) menu_add(m, a.apps.s(a.apps.apps[m.apps[k]].name), k == 0 ? "default" : nullptr, -1, MA_OPEN_WITH, true, (u16)k);
  bool exec = (t.listing.flags[i] & EF_STATTED) && (t.listing.mode[i] & 0111) && !t.listing.is_dir(i) && t.listing.kind[i] != EK_OTHER;
  if (exec) { menu_sep(m); menu_add(m, "Run", nullptr, ICON_EXEC, MA_RUN, true); }
  if (command_exists("xdg-open")) { menu_sep(m); menu_add(m, "xdg-open", nullptr, -1, MA_OPEN_WITH, true, 0xFFFF); }
  menu_show(a, &t, row, x, y);
}

void menu_open_device(App& a, i32 entry, i32 disk, i32 x, i32 y) {
  if (a.dialog.kind != DLG_NONE) return;
  Menu& m = a.menu;
  m.count = 0; m.hover = -1; m.focus = -1; m.row = -1; m.kind = MENU_DEVICE; m.dev_entry = entry; m.dev_disk = disk;
  if (entry >= 0 && (u32)entry < a.mounts.entries.len) {
    const MountEntry& e = a.mounts.entries[(u32)entry];
    bool swap = !strcmp(e.fstype, "swap");
    if (e.mounted) {
      menu_add(m, "Open", nullptr, -1, MA_OPEN_MOUNT, true);
      menu_add(m, "Open in new tab", nullptr, -1, MA_OPEN_MOUNT_TAB, true);
      menu_add(m, "Open in terminal", nullptr, -1, MA_TERMINAL, true);
      menu_sep(m);
      menu_add(m, "Unmount", nullptr, -1, MA_UNMOUNT, strcmp(e.mountpoint, "/") != 0);
    } else {
      menu_add(m, "Mount", nullptr, -1, MA_MOUNT, !swap);
      menu_add(m, "Mount and open", nullptr, -1, MA_OPEN_MOUNT, !swap);
    }
    snprintf(m.text[0], sizeof m.text[0], "%s", e.device);
    menu_sep(m);
    menu_add(m, m.text[0], nullptr, -1, MA_COPY_PATH, true);
  } else if (disk >= 0 && (u32)disk < a.mounts.disks.len) {
    const DiskEntry& d = a.mounts.disks[(u32)disk];
    const char* mp = disk_primary_mount(a.mounts, d);
    menu_add(m, "Open", nullptr, -1, MA_OPEN_MOUNT, mp[0] != 0);
    menu_add(m, "Open in new tab", nullptr, -1, MA_OPEN_MOUNT_TAB, mp[0] != 0);
    menu_add(m, "Open in terminal", nullptr, -1, MA_TERMINAL, mp[0] != 0);
    menu_sep(m);
    menu_add(m, disk_expanded(a, d.name) ? "Hide the volumes" : "Show the volumes", nullptr, ICON_CHEVRON_DOWN, MA_REFRESH, true);
    if (d.removable) {
      bool root = false;
      for (u32 i = d.first; i < d.first + d.count && i < a.mounts.entries.len; i++) root |= a.mounts.entries[i].mounted && !strcmp(a.mounts.entries[i].mountpoint, "/");
      menu_sep(m);
      menu_add(m, "Eject", nullptr, ICON_USB, MA_POWER_OFF, !root);
    }
  } else return;
  menu_show(a, nullptr, -1, x, y);
}

void menu_open_place(App& a, const char* path, bool pinned, i32 x, i32 y) {
  if (a.dialog.kind != DLG_NONE) return;
  Menu& m = a.menu;
  m.count = 0; m.hover = -1; m.focus = -1; m.row = -1; m.kind = MENU_PLACE; m.dev_entry = m.dev_disk = -1;
  snprintf(m.place_path, sizeof m.place_path, "%s", path);
  m.place_trash = trash_is_files_dir(path); m.place_pinned = pinned;
  menu_add(m, "Open", nullptr, -1, MA_PLACE_OPEN, true);
  menu_add(m, "Open in new tab", nullptr, -1, MA_PLACE_OPEN_TAB, true);
  menu_add(m, "Open in terminal", nullptr, -1, MA_TERMINAL, true);
  if (m.place_trash) {
    bool empty = true;
    if (DIR* d = opendir(path)) { struct dirent* e; while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { empty = false; break; } closedir(d); }
    menu_sep(m);
    menu_add(m, "Empty trash", nullptr, ICON_TRASH, MA_EMPTY_TRASH, !empty);
  }
  if (pinned) { menu_sep(m); menu_add(m, "Unpin from sidebar", nullptr, ICON_PIN, MA_PIN, true); }
  menu_sep(m);
  menu_add(m, "Properties", nullptr, -1, MA_PROPERTIES, true);
  menu_show(a, nullptr, -1, x, y);
}

static void menu_show(App& a, Tab* t, i32 row, i32 x, i32 y) {
  Menu& m = a.menu; Layout& L = a.L;
  if (x < 0 || y < 0) {
    if (t && row >= 0) { Rect cr = cell_rect(a, *t, row); x = cr.x + ui_px(a.ui, 28); y = rect_y1(cr) - ui_px(a.ui, 2); }
    else { x = L.list.x + L.pad; y = L.list.y + L.pad; }
  }
  m.x = x; m.y = y; m.open = true;
  a.ui.want_redraw = true;
  a.w.need_redraw = true;
}

void menu_close(App& a) {
  a.menu.open = false; a.menu.hover = a.menu.focus = -1; a.menu.count = 0;
  a.ui.want_redraw = true;
  a.w.need_redraw = true;
}

static Rect menu_box(App& a, Ui& ui, i32* item_h, i32* sep_h, i32* pad) {
  Menu& m = a.menu; Layout& L = a.L; Canvas& c = *ui.c;
  *item_h = L.row_h; *sep_h = ui_px(ui, 9); *pad = ui_px(ui, 5);
  float w = 0;
  for (u32 i = 0; i < m.count; i++) {
    const MenuItem& it = m.items[i];
    if (it.separator) continue;
    float lw = text_width(a.text, it.label) + (it.shortcut ? text_width(a.text, it.shortcut) + (float)ui_px(ui, 28) : 0);
    w = mx_max(w, lw + (float)ui_px(ui, 34 + 16));
  }
  i32 box_w = mx_clamp((i32)w + 1, ui_px(ui, 180), mx_max(ui_px(ui, 200), c.w - ui_px(ui, 8)));
  i32 h = *pad * 2;
  for (u32 i = 0; i < m.count; i++) h += m.items[i].separator ? *sep_h : *item_h;
  i32 x = m.x, y = m.y;
  if (x + box_w > c.w) x = mx_max(c.w - box_w, 0);
  if (y + h > c.h) y = m.y - h >= 0 ? m.y - h : mx_max(c.h - h, 0); // flip up, like a menu at the bottom of the screen
  return { x, y, box_w, h };
}

static i32 menu_hit(const App& a, Rect box, i32 item_h, i32 sep_h, i32 pad, float mx, float my) {
  const Menu& m = a.menu;
  if (!rect_has(box, mx, my)) return -1;
  i32 y = box.y + pad;
  for (u32 i = 0; i < m.count; i++) {
    i32 h = m.items[i].separator ? sep_h : item_h;
    if (my >= (float)y && my < (float)(y + h)) return m.items[i].separator ? -1 : (i32)i;
    y += h;
  }
  return -1;
}

static void menu_activate(App& a, u8 action, u16 arg) {
  Tab& t = tab_active(a);
  Menu& m = a.menu;
  i32 row = m.row;
  i32 mx = m.x, my = m.y;
  i32 dev_entry = m.dev_entry, dev_disk = m.dev_disk;
  u32 app = m.kind == MENU_OPEN_WITH && arg < m.napps ? m.apps[arg] : 0xFFFFFFFFu;
  char place[512]; snprintf(place, sizeof place, "%s", m.place_path);
  bool place_pinned = m.place_pinned;
  u8 kind = m.kind;
  menu_close(a);
  if (kind == MENU_PLACE) { // a sidebar place or pin
    switch (action) {
      case MA_PLACE_OPEN: navigate(a, t, place, true); break;
      case MA_PLACE_OPEN_TAB: tab_open(a, place, false); break;
      case MA_TERMINAL: open_terminal(a, place); break;
      case MA_EMPTY_TRASH: op_empty_trash_dir(a, place, false); break;
      case MA_PIN: if (place_pinned) toggle_pin(a, place); break;
      case MA_PROPERTIES: {
        i32 pos = -1;
        for (u32 i = 0; i < a.num_tabs; i++) if (!strcmp(a.tabs[a.tab_order[i]].path, place)) pos = (i32)i;
        if (pos >= 0) props_open(a, a.tabs[a.tab_order[(u32)pos]], true);
        else { navigate(a, t, place, true); props_open(a, t, true); }
        break;
      }
      default: break;
    }
    a.w.need_redraw = true;
    return;
  }

  if (kind == MENU_DEVICE) {
    const MountEntry* e = dev_entry >= 0 && (u32)dev_entry < a.mounts.entries.len ? &a.mounts.entries[(u32)dev_entry] : nullptr;
    const DiskEntry* d = dev_disk >= 0 && (u32)dev_disk < a.mounts.disks.len ? &a.mounts.disks[(u32)dev_disk] : nullptr;
    const char* mp = e ? (e->mounted ? e->mountpoint : "") : d ? disk_primary_mount(a.mounts, *d) : "";
    switch (action) {
      case MA_OPEN_MOUNT: case MA_OPEN_MOUNT_TAB:
        if (e && !e->mounted) device_mount(a, (u32)dev_entry, true, action == MA_OPEN_MOUNT_TAB);
        else if (mp[0]) { if (action == MA_OPEN_MOUNT_TAB) tab_open(a, mp, false); else navigate(a, t, mp, true); }
        break;
      case MA_MOUNT: if (e) device_mount(a, (u32)dev_entry, false, false); break;
      case MA_UNMOUNT: if (e) device_unmount(a, (u32)dev_entry); break;
      case MA_POWER_OFF: if (d) device_power_off(a, (u32)dev_disk); break;
      case MA_TERMINAL: if (mp[0]) open_terminal(a, mp); break;
      case MA_REFRESH: if (d) disk_toggle(a, d->name); break;
      case MA_COPY_PATH: if (e) { a.clip.clear(); a.clip.text = true; a.clip.add(e->device); clip_publish(a); char msg[200]; snprintf(msg, sizeof msg, "Copied %s", e->device); set_status(a, msg); } break;
      default: break;
    }
    a.w.need_redraw = true;
    return;
  }
  switch (action) {
    case MA_OPEN: if (row >= 0) open_row(a, t, row, false); break;
    case MA_OPEN_WITH_PAGE: menu_open_with(a, t, row, mx, my); break;
    case MA_OPEN_WITH:
      if (arg == 0xFFFF) { // xdg-open
        if (t.cursor >= 0 && (u32)t.cursor < t.rows.len) {
          char p[4096], err[256], msg[600]; entry_path(t, t.rows[(u32)t.cursor], p, sizeof p);
          const char* argv[3] = { "xdg-open", p, nullptr };
          if (spawn_detached(argv, nullptr, err, sizeof err)) { a.launches++; snprintf(msg, sizeof msg, "Opened %s with xdg-open", path_base(p)); set_status(a, msg); }
          else { snprintf(msg, sizeof msg, "xdg-open: %s", err); set_status_error(a, msg); }
        }
      } else if (app != 0xFFFFFFFFu) open_files(a, t, (i32)app);
      break;
    case MA_RUN: run_file(a, t); break;
    case MA_TERMINAL:
      if (row >= 0 && (u32)row < t.rows.len && t.listing.is_dir(t.rows[(u32)row])) { char p[4096]; entry_path(t, t.rows[(u32)row], p, sizeof p); open_terminal(a, p); }
      else open_terminal(a, t.path);
      break;
    case MA_RESTORE: op_restore(a, t); break;
    case MA_EMPTY_TRASH: op_empty_trash(a, t, false); break;
    case MA_REDO: op_redo(a); break;
    case MA_COPY_PATH: {
      a.clip.clear(); a.clip.text = true;
      char p[4096]; u32 n = 0;
      for (u32 i = 0; i < t.listing.count(); i++) if (t.selected[i]) { entry_path(t, i, p, sizeof p); a.clip.add(p); n++; }
      for (Tab& ti : a.tabs) if (ti.used) for (u32 i = 0; i < ti.listing.count(); i++) ti.listing.flags[i] &= (u8)~EF_CUT;
      clip_publish(a);
      char msg[200]; snprintf(msg, sizeof msg, n == 1 ? "Copied the path as text" : "Copied %u paths as text", n); set_status(a, msg);
      break;
    }
    case MA_OPEN_TAB: if (row >= 0) open_row(a, t, row, true); break;
    case MA_CUT: op_copy_or_cut(a, t, true); break;
    case MA_COPY: op_copy_or_cut(a, t, false); break;
    case MA_PASTE: op_paste(a, t); break;
    case MA_RENAME: if (t.cursor >= 0 && (u32)t.cursor < t.rows.len) rename_begin(a, t, t.rows[(u32)t.cursor]); break;
    case MA_TRASH: op_trash(a, t); break;
    case MA_DELETE: op_delete(a, t, false); break;
    case MA_NEW_FOLDER: op_new_folder(a, t); break;
    case MA_PIN:
      if (row >= 0 && (u32)row < t.rows.len && t.listing.is_dir(t.rows[(u32)row])) { char p[4096]; entry_path(t, t.rows[(u32)row], p, sizeof p); toggle_pin(a, p); }
      else toggle_pin(a, t.path);
      break;
    case MA_UNDO: op_undo(a); break;
    case MA_SELECT_ALL: select_all(a, t); break;
    case MA_REFRESH: reload(a, t); break;
    case MA_PROPERTIES: props_open(a, t, row < 0); break;
    default: break;
  }
  a.w.need_redraw = true;
}

static void menu_input(App& a, Ui& ui) {
  Menu& m = a.menu;
  i32 ih, sh, pad;
  Rect box = menu_box(a, ui, &ih, &sh, &pad);
  m.hover = ui.mouse_inside ? menu_hit(a, box, ih, sh, pad, ui.mx, ui.my) : -1;
  if (m.hover >= 0 && m.items[m.hover].enabled) m.focus = m.hover;
  i32 activate = -1; bool close = false;
  for (UiClick& k : ui.clicks) {
    if (k.used) continue;
    if (!k.pressed) { k.used = 1; continue; }
    if (rect_has(box, k.x, k.y)) {
      k.used = 1;
      i32 h = menu_hit(a, box, ih, sh, pad, k.x, k.y);
      if (h >= 0 && m.items[h].enabled && k.button == BTN_LEFT) activate = h;
    } else {
      close = true;
      if (!(k.button == BTN_RIGHT && rect_has(a.L.list, k.x, k.y))) k.used = 1;
    }
  }
  if (activate >= 0) menu_activate(a, m.items[activate].action, m.items[activate].arg);
  else if (close) menu_close(a);
  if (m.open) { ui.scroll_dx = ui.scroll_dy = 0; ui.mouse_inside = false; }
}

static void draw_menu(App& a, Ui& ui) {
  Menu& m = a.menu;
  if (!m.open) return;
  const UiTheme& th = theme(a); Canvas& c = *ui.c;
  i32 ih, sh, pad;
  Rect box = menu_box(a, ui, &ih, &sh, &pad);
  canvas_fill_rounded(c, box.x + ui_px(ui, 2), box.y + ui_px(ui, 3), box.w, box.h, ui_px(ui, 6), rgba_hex(0x000000, 70)); // shadow
  canvas_fill_rounded(c, box.x, box.y, box.w, box.h, ui_px(ui, 6), rgb_hex(th.panel));
  canvas_stroke(c, box.x, box.y, box.w, box.h, rgba_hex(th.border, 255));
  i32 hl = m.hover >= 0 ? m.hover : m.focus;
  i32 y = box.y + pad;
  i32 isz = ui_px(ui, 14);
  for (u32 i = 0; i < m.count; i++) {
    const MenuItem& it = m.items[i];
    if (it.separator) { canvas_hline(c, box.x + pad + ui_px(ui, 4), y + sh / 2, box.w - 2 * pad - ui_px(ui, 8), rgb_hex(th.border)); y += sh; continue; }
    Rect r = { box.x + pad, y, box.w - 2 * pad, ih };
    bool on = (i32)i == hl && it.enabled;
    if (on) canvas_fill_rounded(c, r.x, r.y, r.w, r.h, ui_px(ui, 4), rgb_hex(th.hover));
    u32 fg = rgb_hex(it.enabled ? th.text : th.muted);
    u32 bg = rgb_hex(on ? th.hover : th.panel);
    if (it.icon >= 0) ui_icon(ui, it.icon, r.x + ui_px(ui, 8), r.y + (r.h - isz) / 2, isz, rgb_hex(it.enabled ? th.muted : th.border), bg);
    i32 bl = ui_baseline(ui, a.text, r.y, r.h);
    text_draw(c, a.text, it.label, (float)(r.x + ui_px(ui, 32)), bl, fg);
    if (it.shortcut) { float sw = text_width(a.text, it.shortcut); text_draw(c, a.text, it.shortcut, (float)(rect_x1(r) - ui_px(ui, 10)) - sw, bl, rgb_hex(th.muted)); }
    if (m.hover == (i32)i && it.enabled) ui.cursor = CURSOR_POINTER;
    y += ih;
  }
}

static void menu_key(App& a, u32 ks) {
  Menu& m = a.menu;
  auto step = [&](i32 dir) {
    if (!m.count) return;
    i32 i = m.focus;
    for (u32 n = 0; n < m.count; n++) {
      i = (i + dir + (i32)m.count) % (i32)m.count;
      if (!m.items[i].separator && m.items[i].enabled) { m.focus = i; return; }
    }
  };
  switch (ks) {
    case XKB_KEY_Escape: menu_close(a); return;
    case XKB_KEY_Up: case XKB_KEY_KP_Up: if (m.focus < 0) m.focus = 0; step(-1); return;
    case XKB_KEY_Down: case XKB_KEY_KP_Down: if (m.focus < 0) m.focus = (i32)m.count - 1; step(+1); return;
    case XKB_KEY_Home: case XKB_KEY_KP_Home: m.focus = (i32)m.count - 1; step(+1); return;
    case XKB_KEY_End: case XKB_KEY_KP_End: m.focus = 0; step(-1); return;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_space:
      if (m.focus >= 0 && m.items[m.focus].enabled) menu_activate(a, m.items[m.focus].action, m.items[m.focus].arg);
      return;
    default: break;
  }
  if (ks < 128) { // first letter of an item
    u32 lower = ks | 0x20;
    for (u32 i = 0; i < m.count; i++)
      if (!m.items[i].separator && m.items[i].enabled && ((u32)m.items[i].label[0] | 0x20) == lower) { menu_activate(a, m.items[i].action, m.items[i].arg); return; }
  }
}

void rename_begin(App& a, Tab& t, u32 i) {
  if (i >= t.listing.count() || a.dialog.kind != DLG_NONE || t.dirfd < 0) return;
  if (a.menu.open) menu_close(a);
  a.path_editing = false;
  a.pending_rename_row = -1;
  t.renaming = (i32)i;
  ui_text_set(a.rename_input, t.listing.name(i));

  const char* nm = t.listing.cname(i);
  u32 stem = a.rename_input.len;
  if (!t.listing.is_dir(i)) { const char* dot = strrchr(nm, '.'); if (dot && dot != nm) stem = (u32)(dot - nm); }
  a.rename_input.sel_anchor = 0;
  a.rename_input.cursor = stem;
  for (u32 r = 0; r < t.rows.len; r++) if (t.rows[r] == i) { ensure_visible(a, t, (i32)r); break; }
  a.w.need_redraw = true;
}

void rename_cancel(App& a, Tab& t) {
  t.renaming = -1;
  a.w.need_redraw = true;
}

void shell_frame(App& a, Canvas& c) {
  Ui& ui = a.ui;
  Window& w = a.w;
  Tab& t = tab_active(a);
  float s = w.logical_w > 0 ? (float)c.w / (float)w.logical_w : 1.0f;
  float px = a.cfg.font_px * s;
  if (px != a.cur_px) { text_set_size(a.text, px); a.cur_px = px; }
  float spx = mx_max(7.0f, px * 0.86f);
  if (spx != a.cur_small_px && a.small.num_fonts) { text_set_size(a.small, spx); a.cur_small_px = spx; }

  Layout before = a.L;
  shell_layout(a, c.w, c.h);
  bool geometry_changed = before.list.w != 0 &&
      (before.s != s || before.content.w + before.side.w != c.w || before.content.h != c.h || before.row_h != a.L.row_h ||
       a.L.sidebar != before.sidebar || a.L.side.w != before.side.w || before.ops.h != a.L.ops.h);
  if (geometry_changed) for (Tab& ti : a.tabs) if (ti.used) ti.restore_scroll = true;
  if (t.restore_scroll) {
    t.restore_scroll = false;
    scroll_to_first(a, t, t.first_vis);
    clamp_scroll(a, t);
    if (t.cursor >= 0 && t.keep_first_vis) ensure_visible(a, t, t.cursor);
  }
  ui_begin(ui, c, a.text, a.small.num_fonts ? &a.small : nullptr, theme(a), s, ui.now_ms);
  ui.focused = w.focused;
  canvas_clear(c, rgb_hex(theme(a).bg));

  bool was_inside = ui.mouse_inside;
  if (a.dialog.kind != DLG_NONE) dialog_input(a, ui);
  else if (a.menu.open) menu_input(a, ui);

  splitter_input(a, ui); // before anything that overlaps its grab zone
  draw_view(a, ui, tab_active(a)); // first so the chrome paints over its edges
  Tab& t2 = tab_active(a); // a click may have switched or closed tabs
  draw_ops(a, ui);
  draw_status(a, ui, t2);
  draw_crumbs(a, ui, t2);
  draw_tabs(a, ui, t2);
  Tab& t3 = tab_active(a);
  draw_sidebar(a, ui, t3);
  ui.mouse_inside = was_inside;
  draw_dialog(a, ui);
  draw_menu(a, ui);
  ui_end(ui);

  Tab& tf = tab_active(a);
  tf.first_vis = first_visible(a, tf);
  Rect cr = tf.cursor >= 0 && (u32)tf.cursor < tf.rows.len ? cell_rect(a, tf, tf.cursor) : Rect{};
  tf.keep_first_vis = tf.cursor >= 0 && cr.y >= a.L.list.y && rect_y1(cr) <= rect_y1(a.L.list) && cr.x >= a.L.list.x && rect_x1(cr) <= rect_x1(a.L.list);

  i64 wake = shell_next_wake(a);
  if (wake && (!ui.wake_at_ms || wake < ui.wake_at_ms)) ui.wake_at_ms = wake;
  window_set_cursor(w, ui.cursor);
  if (ui.want_redraw) w.need_redraw = true;
}

void shell_focus_changed(App& a) { a.w.need_redraw = true; }

i64 shell_next_wake(const App& a) {
  i64 wake = 0;
  auto earliest = [&](i64 t) { if (t > 0 && (wake == 0 || t < wake)) wake = t; };
  if (a.pending_rename_row >= 0) earliest(a.pending_rename_at);
  if (a.status_msg[0]) earliest(a.status_msg_until);
  for (const DirWatch& d : a.watcher.watches) if (d.refs) earliest(watch_due_ms(d));
  return wake;
}

void shell_timers(App& a, i64 now) {
  Tab& t = tab_active(a);
  if (a.pending_rename_row >= 0 && now >= a.pending_rename_at) {
    i32 row = a.pending_rename_row;
    a.pending_rename_row = -1;
    if (row == t.cursor && t.selected_count == 1 && (u32)row < t.rows.len && a.dialog.kind == DLG_NONE && !a.menu.open && !a.path_editing)
      rename_begin(a, t, t.rows[(u32)row]);
  }
  if (a.status_msg[0] && now >= a.status_msg_until) { a.status_msg[0] = 0; a.status_error = false; a.w.need_redraw = true; }

  if (a.jobs.len && a.dialog.kind == DLG_NONE && !a.menu.open) ops_poll(a);

  for (u32 wi = 0; wi < a.watcher.watches.len; wi++) {
    DirWatch* d = &a.watcher.watches[wi];
    if (!d->refs || !d->dirty) continue;
    i64 due = watch_due_ms(*d);
    if (now < due) continue;
    char names[4096];
    u32 names_len = d->names_len;
    memcpy(names, d->names, names_len);
    bool drop = d->overflow, gone = d->gone;
    u32 events = d->events;
    watch_clear(*d);
    if ((i32)wi == a.theme_watch) {
      char old[64]; snprintf(old, sizeof old, "%s", a.theme_name);
      theme_sync_load(a);
      if (a.theme_synced && (strcmp(old, a.theme_name) || !a.cfg.theme_follow)) {
        a.cfg.theme_follow = true; a.light = a.theme_is_light; a.cfg.light = a.light; config_mark_dirty(a); // a switch in Omarchy ends an F10 override
        char msg[128]; snprintf(msg, sizeof msg, "Theme: %s", a.theme_name); set_status(a, msg);
      }
      if (gone) a.theme_watch = -1;
      a.w.need_redraw = true;
      continue;
    }
    if (a.debug) fprintf(stderr, "watch: %s: %u events, %s\n", a.watcher.watches[wi].path, events, gone ? "gone" : drop ? "refresh (stats dropped)" : "refresh");
    for (u32 s = 0; s < MAX_TABS; s++) {
      Tab& ti = a.tabs[s];
      if (!ti.used || ti.watch != (i32)wi) continue;
      if (gone) tab_gone(a, ti); else refresh_tab(a, ti, names, names_len, drop);
    }
    a.w.need_redraw = true;
  }
}

void path_complete(App& a) {
  UiTextInput& in = a.path_input;
  char text[4096], dir[4096], prefix[512];
  snprintf(text, sizeof text, "%.*s", (int)in.len, in.buf);
  if (text[0] == '~') { char ex[4096]; expand_path(text, ex, sizeof ex); if (in.len && text[in.len - 1] == '/' && ex[strlen(ex) - 1] != '/') strncat(ex, "/", sizeof ex - strlen(ex) - 1); snprintf(text, sizeof text, "%s", ex); }
  const char* slash = strrchr(text, '/');
  if (!slash) { snprintf(dir, sizeof dir, "%s", tab_active(a).path); snprintf(prefix, sizeof prefix, "%s", text); }
  else { snprintf(dir, sizeof dir, "%.*s", (int)mx_max((i32)(slash - text), 1), text); snprintf(prefix, sizeof prefix, "%s", slash + 1); }
  DIR* d = opendir(dir);
  if (!d) { set_status(a, "No such folder"); return; }
  u32 plen = (u32)strlen(prefix);
  char common[512] = {}; u32 n = 0, scanned = 0;
  char names[8][256]; u32 nn = 0;
  struct dirent* e;
  while ((e = readdir(d)) && scanned < 5000) {
    scanned++;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if (e->d_name[0] == '.' && prefix[0] != '.') continue;
    if (strncmp(e->d_name, prefix, plen)) continue;
    bool is_dir = e->d_type == DT_DIR;
    if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) { char full[4096]; snprintf(full, sizeof full, "%s/%s", dir, e->d_name); struct stat st; is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode); }
    if (!is_dir) continue;
    if (!n) snprintf(common, sizeof common, "%s", e->d_name);
    else { u32 k = 0; while (common[k] && e->d_name[k] == common[k]) k++; common[k] = 0; }
    if (nn < 8) snprintf(names[nn++], sizeof names[0], "%s", e->d_name);
    n++;
  }
  closedir(d);
  if (!n) { set_status(a, "No matching folder"); return; }
  char out[4096];
  const char* base = !strcmp(dir, "/") ? "" : dir;
  if (n == 1) snprintf(out, sizeof out, "%s/%s/", base, common);
  else snprintf(out, sizeof out, "%s/%s", base, common);
  ui_text_set(in, out);
  if (n > 1) {
    char msg[600]; int o = snprintf(msg, sizeof msg, "%u folders: ", n);
    for (u32 i = 0; i < nn && o < (int)sizeof msg - 4; i++) o += snprintf(msg + o, sizeof msg - (usize)o, "%s%s", i ? ", " : "", names[i]);
    if (n > nn) snprintf(msg + mx_min(o, (int)sizeof msg - 4), 4, "\xe2\x80\xa6");
    set_status(a, msg);
  }
  a.w.need_redraw = true;
}

static void move_cursor(App& a, Tab& t, i32 delta, bool shift) {
  i32 from = t.cursor < 0 ? (delta > 0 ? -1 : 0) : t.cursor;
  set_cursor(a, t, from + delta, shift, false);
}

void shell_key(App& a, const WEvent& e) {
  Window& w = a.w;
  Tab& t = tab_active(a);
  Layout& L = a.L;
  if (!e.pressed) return;
  u32 mods  = wevent_mods(e) & ~(e.consumed | XKB_MOD_LOCK | w.xkb.mask_numlock);
  bool ctrl = (mods & XKB_MOD_CONTROL) != 0, shift = (mods & XKB_MOD_SHIFT) != 0;
  bool alt  = w.xkb.mask_alt && (mods & w.xkb.mask_alt) != 0;
  u32 ks = e.keysym;
  w.need_redraw = true;
  if (!w.xkb.ok) {
    if (e.code == 1) clear_selection(a, t);
    if (e.code == 14) go_parent(a, t);
    if (e.code == 103) move_cursor(a, t, -1, false);
    if (e.code == 108) move_cursor(a, t, +1, false);
    if (e.code == 28) open_row(a, t, t.cursor, false);
    return;
  }
  a.pending_rename_row = -1; // any key cancels a slow-click rename in the making

  u32 kmods = wevent_mods(e) & ~(XKB_MOD_LOCK | w.xkb.mask_numlock);
  if (a.dialog.kind != DLG_NONE) { dialog_key(a, ks, kmods, e.text, e.text_len); return; }
  if (a.menu.open) { menu_key(a, ks); return; }
  if (t.renaming >= 0) {
    switch (ui_text_input_key(a.rename_input, ks, kmods, e.text, e.text_len)) {
      case UI_TEXT_SUBMIT: rename_commit(a, t); break;
      case UI_TEXT_CANCEL: rename_cancel(a, t); break;
      default: break;
    }
    return;
  }
  if (a.path_editing) {
    if (ks == XKB_KEY_Tab || ks == XKB_KEY_ISO_Left_Tab) { path_complete(a); return; }
    switch (ui_text_input_key(a.path_input, ks, kmods, e.text, e.text_len)) {
      case UI_TEXT_SUBMIT: a.path_editing = false; navigate(a, t, a.path_input.buf, true); break;
      case UI_TEXT_CANCEL: a.path_editing = false; break;
      default: break;
    }
    return;
  }

  u32 lower = (ks < 128) ? (ks | 0x20) : ks;
  if (ctrl && !alt) {
    switch (lower) {
      case 'q': a.running = false; return;
      case 't': tab_open(a, t.path, true); return;
      case 'w': tab_close(a, a.active); return;
      case 'l': a.path_editing = true; ui_text_set(a.path_input, t.path); a.path_input.sel_anchor = 0; return;
      case 'h':
        a.cfg.show_hidden = !a.cfg.show_hidden; config_mark_dirty(a);
        for (Tab& ti : a.tabs) if (ti.used) { clear_selection(a, ti); ti.cursor = ti.anchor = -1; rebuild_rows(a, ti); }
        return;
      case 'b': a.cfg.sidebar_visible = !a.cfg.sidebar_visible; config_mark_dirty(a); return;
      case 'd': toggle_pin(a, t.path); return;
      case 'a': select_all(a, t); return;
      case 'r': reload(a, t); return;
      case 'c': op_copy_or_cut(a, t, false); return;
      case 'x': op_copy_or_cut(a, t, true); return;
      case 'v': op_paste(a, t); return;
      case 'z': if (shift) op_redo(a); else op_undo(a); return;
      case 'y': op_redo(a); return;
      case 'n': if (shift) op_new_folder(a, t); return;
      case '1': set_view(a, t, VIEW_ICONS); return;
      case '2': set_view(a, t, VIEW_LIST); return;
      case '3': set_view(a, t, VIEW_DETAILS); return;
      case '0': case XKB_KEY_KP_0: a.cfg.font_px = 13.0f; config_mark_dirty(a); return;
      case '+': case '=': case XKB_KEY_KP_Add: a.cfg.font_px = mx_min(a.cfg.font_px + 1.0f, 40.0f); config_mark_dirty(a); return;
      case '-': case XKB_KEY_KP_Subtract: a.cfg.font_px = mx_max(a.cfg.font_px - 1.0f, 7.0f); config_mark_dirty(a); return;
      case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: case XKB_KEY_Page_Down: case XKB_KEY_Page_Up:
        if (a.num_tabs > 1) {
          bool prev = shift || lower == XKB_KEY_ISO_Left_Tab || lower == XKB_KEY_Page_Up;
          tab_activate(a, prev ? (a.active + a.num_tabs - 1) % a.num_tabs : (a.active + 1) % a.num_tabs);
        }
        return;
      case XKB_KEY_space: if (t.cursor >= 0) set_cursor(a, t, t.cursor, false, true); return;
      case XKB_KEY_Return: case XKB_KEY_KP_Enter: open_row(a, t, t.cursor, true); return;
      default: break;
    }
  }
  if (alt && !ctrl) {
    if (ks >= '1' && ks <= '8') { tab_activate(a, mx_min((u32)(ks - '1'), a.num_tabs - 1)); return; }
    if (ks == '9') { tab_activate(a, a.num_tabs - 1); return; }
    if (ks == XKB_KEY_Left)  { go_back(a, t); return; }
    if (ks == XKB_KEY_Right) { go_forward(a, t); return; }
    if (ks == XKB_KEY_Up)    { go_parent(a, t); return; }
    if (ks == XKB_KEY_Return || ks == XKB_KEY_KP_Enter) { props_open(a, t, false); return; }
  }
  bool filtering = t.filter.len > 0;
  switch (ks) {
    case XKB_KEY_F5:  reload(a, t); return;
    case XKB_KEY_F4: open_terminal(a, t.path); return;
    case XKB_KEY_F10:
      if (shift) menu_open(a, t, t.cursor, -1, -1);
      else if (a.theme_synced) {
        a.cfg.theme_follow = !a.cfg.theme_follow;
        a.light = a.cfg.theme_follow ? a.theme_is_light : !a.theme_is_light;
        a.cfg.light = a.light; config_mark_dirty(a);
        char msg[128]; snprintf(msg, sizeof msg, a.cfg.theme_follow ? "Theme: %s" : "Theme: built-in %s (F10 returns to %s)", a.cfg.theme_follow ? a.theme_name : a.light ? "light" : "dark", a.theme_name); set_status(a, msg);
      } else { a.light = !a.light; a.cfg.light = a.light; config_mark_dirty(a); }
      return;
    case XKB_KEY_Menu: menu_open(a, t, t.cursor, -1, -1); return;
    case XKB_KEY_F2:
      if (t.cursor >= 0 && (u32)t.cursor < t.rows.len && t.selected_count == 1) rename_begin(a, t, t.rows[(u32)t.cursor]);
      else if (t.selected_count > 1) set_status(a, "Select one item to rename it");
      return;
    case XKB_KEY_Delete: case XKB_KEY_KP_Delete:
      if (filtering && !shift) { ui_text_input_key(t.filter, ks, mods, nullptr, 0); filter_changed(a, t); return; }
      if (shift || in_trash(t)) op_delete(a, t, false); else op_trash(a, t); // in the trash, Delete is permanent
      return;
    case XKB_KEY_Escape:
      if (a.jobs.len && !a.jobs[0]->prog.cancel && !filtering && !t.selected_count) { ops_cancel(a, a.jobs[0]); return; }
      break;
    default: break;
  }

  i32 cols = L.cols, rpc = L.rows_per_col;
  switch (ks) {
    case XKB_KEY_Up: case XKB_KEY_KP_Up:
      move_cursor(a, t, t.view == VIEW_ICONS ? -cols : -1, shift); return;
    case XKB_KEY_Down: case XKB_KEY_KP_Down:
      move_cursor(a, t, t.view == VIEW_ICONS ? cols : 1, shift); return;
    case XKB_KEY_Left: case XKB_KEY_KP_Left:
      if (t.view == VIEW_ICONS) move_cursor(a, t, -1, shift);
      else if (t.view == VIEW_LIST) move_cursor(a, t, -rpc, shift);
      else if (!filtering) go_parent(a, t);
      return;
    case XKB_KEY_Right: case XKB_KEY_KP_Right:
      if (t.view == VIEW_ICONS) move_cursor(a, t, 1, shift);
      else if (t.view == VIEW_LIST) move_cursor(a, t, rpc, shift);
      else if (!filtering) open_row(a, t, t.cursor, false);
      return;
    case XKB_KEY_Page_Up: case XKB_KEY_KP_Page_Up:
      move_cursor(a, t, -rows_per_page(a, t), shift); return;
    case XKB_KEY_Page_Down: case XKB_KEY_KP_Page_Down:
      move_cursor(a, t, rows_per_page(a, t), shift); return;
    case XKB_KEY_Home: case XKB_KEY_KP_Home:
      if (filtering && !shift && !ctrl) { ui_text_input_key(t.filter, ks, mods, nullptr, 0); return; }
      set_cursor(a, t, 0, shift, false); return;
    case XKB_KEY_End: case XKB_KEY_KP_End:
      if (filtering && !shift && !ctrl) { ui_text_input_key(t.filter, ks, mods, nullptr, 0); return; }
      set_cursor(a, t, (i32)t.rows.len - 1, shift, false); return;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter:
      open_row(a, t, t.cursor, false); return;
    case XKB_KEY_BackSpace:
      if (filtering) { ui_text_input_key(t.filter, ks, mods, nullptr, 0); filter_changed(a, t); }
      else go_parent(a, t);
      return;
    case XKB_KEY_Escape:
      if (filtering) filter_clear(a, t); else clear_selection(a, t);
      return;
    default: break;
  }

  if (!ctrl && !alt && e.text_len) {
    if (ui_text_input_key(t.filter, ks, mods, e.text, e.text_len) == UI_TEXT_CHANGED) filter_changed(a, t);
    return;
  }
  if (filtering && (ks == XKB_KEY_Left || ks == XKB_KEY_Right)) ui_text_input_key(t.filter, ks, mods, nullptr, 0);
}
