#include "app/app.h"
#include "core/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const float kDragThreshold = 6.0f; // logical pixels

static void drag_icon_render(App& a, u32 count, u8 icon, bool dir, Image& out) {
  float s = a.L.s > 0 ? a.L.s : 1.0f;
  i32 size = (i32)(52 * s + 0.5f);
  out.w = out.h = size;
  out.px = (u32*)calloc((usize)size * size, 4);
  if (!out.px) { out.w = out.h = 0; return; }
  Canvas c = { out.px, size, size, size, 0, 0, size, size };
  const UiTheme& th = theme_current(a);
  i32 pad = (i32)(4 * s), isz = size - 2 * pad;
  canvas_fill_rounded(c, pad, pad, isz, isz, (i32)(8 * s), rgba_hex(th.panel, 235));
  canvas_stroke(c, pad, pad, isz, isz, rgba_hex(th.accent, 220));
  Ui ui;
  ui_begin(ui, c, a.text, a.small.num_fonts ? &a.small : nullptr, th, s, a.ui.now_ms);
  i32 ic = (i32)(28 * s);
  ui_icon(ui, icon, (size - ic) / 2, (size - ic) / 2 - (i32)(4 * s), ic, rgb_hex(dir ? th.folder : th.muted), rgb_hex(th.panel));
  if (count > 1) { // a count badge in the corner
    char b[16]; snprintf(b, sizeof b, "%u", count);
    float tw = text_width(a.small, b);
    i32 bw = (i32)(tw + 10 * s), bh = text_line_height(a.small) + (i32)(2 * s);
    i32 bx = size - pad - bw, by = size - pad - bh;
    canvas_fill_rounded(c, bx, by, bw, bh, bh / 2, rgb_hex(th.accent));
    text_draw(c, a.small, b, (float)bx + 5 * s, ui_baseline(ui, a.small, by, bh), rgb_hex(th.bg));
  }
  ui.clicks.release();
}

void drag_check(App& a) {
  Window& w = a.w;
  Ui& ui = a.ui;
  if (a.drag_press_row < 0) return;
  if (!ui.left_down || w.dragging || !w.data_device) { a.drag_press_row = -1; return; }
  float dx = ui.mx - a.drag_press_x, dy = ui.my - a.drag_press_y;
  float thr = kDragThreshold * (a.L.s > 0 ? a.L.s : 1.0f);
  if (dx * dx + dy * dy < thr * thr) return;
  Tab& t = tab_active(a);
  a.drag_press_row = -1;
  a.pending_rename_row = -1;
  if (t.renaming >= 0 || a.dialog.kind != DLG_NONE || a.menu.open || !t.selected_count) return;
  a.drag.clear();
  char p[4096];
  u8 icon = ICON_FILE; bool dir = false; u32 n = 0;
  for (u32 i = 0; i < t.listing.count(); i++) {
    if (!t.selected[i]) continue;
    entry_path(t, i, p, sizeof p);
    a.drag.add(p);
    if (!n) { icon = entry_icon(t.listing, i); dir = t.listing.is_dir(i); }
    n++;
  }
  snprintf(a.drag.dir, sizeof a.drag.dir, "%s", t.path);
  Image icon_img;
  drag_icon_render(a, n, icon, dir, icon_img);
  bool ok = window_drag_start(w, WCLIP_URI_LIST | WCLIP_GNOME_FILES | WCLIP_TEXT, WL_DND_COPY | WL_DND_MOVE, icon_img.px, icon_img.w, icon_img.h);
  image_free(icon_img);
  if (!ok) { a.drag.clear(); return; }
  a.drags++;
  ui.left_down = false;
  if (a.debug) fprintf(stderr, "dnd: dragging %u item%s from %s\n", n, n == 1 ? "" : "s", t.path);
}

void drag_ended(App& a, bool finished, u32 action) {
  if (a.debug) fprintf(stderr, "dnd: drag %s%s\n", finished ? "dropped" : "cancelled", finished ? (action == WL_DND_MOVE ? " (move)" : action == WL_DND_COPY ? " (copy)" : "") : "");

  a.drag.clear();
  a.w.need_redraw = true;
}

bool dnd_target_at(App& a, float x, float y, char* out, u32 cap, i32* row, Rect* side) {
  *row = -1; *side = Rect{};
  for (const DropTarget& d : a.drop_targets) if (rect_has(d.r, x, y)) { snprintf(out, cap, "%s", d.path); *side = d.r; return true; }
  Tab& t = tab_active(a);
  if (!rect_has(a.L.list, x, y) || t.dirfd < 0) { out[0] = 0; return false; }
  i32 r = cell_at(a, t, x, y);
  if (r >= 0 && (u32)r < t.rows.len) {
    u32 i = t.rows[(u32)r];
    if (!(t.listing.flags[i] & (EF_STATTED | EF_STAT_FAILED))) dir_list_stat_range(t.dirfd, t.listing, i, 1);
    if (t.listing.is_dir(i)) { entry_path(t, i, out, cap); *row = r; return true; }
  }
  snprintf(out, cap, "%s", t.path);
  return true;
}

static bool same_device(const char* p1, const char* p2) {
  struct stat a, b;
  return stat(p1, &a) == 0 && stat(p2, &b) == 0 && a.st_dev == b.st_dev;
}

void dnd_event(App& a, u8 type, float x, float y) {
  Window& w = a.w;
  if (type == WE_DND_LEAVE) {
    a.dnd_over = false; a.dnd_target[0] = 0; a.dnd_row = -1; a.dnd_rect = Rect{}; a.dnd_mime = 0;
    a.w.need_redraw = true;
    return;
  }
  if (type == WE_DND_ENTER || type == WE_DND_MOTION) {
    char target[4096]; i32 row; Rect side;
    bool have = dnd_target_at(a, x, y, target, sizeof target, &row, &side);
    u32 mime = (w.dnd_mimes & WCLIP_GNOME_FILES) ? WCLIP_GNOME_FILES : (w.dnd_mimes & WCLIP_URI_LIST) ? WCLIP_URI_LIST : 0;

    if (have && w.dragging && a.drag.count() && !strcmp(target, a.drag.dir)) have = false;
    bool changed = type == WE_DND_ENTER || strcmp(target, a.dnd_target) != 0 || a.dnd_row != row || mime != a.dnd_mime;
    a.dnd_over = true;
    if (changed) {
      if (have && mime) {
        snprintf(a.dnd_target, sizeof a.dnd_target, "%s", target);
        a.dnd_row = row; a.dnd_rect = side; a.dnd_mime = mime;
        u32 preferred = WL_DND_COPY;
        if (w.dragging && a.drag.count() && same_device(a.drag.dir, target)) preferred = WL_DND_MOVE;
        window_dnd_accept(w, mime, WL_DND_COPY | WL_DND_MOVE, preferred);
      } else {
        a.dnd_target[0] = 0; a.dnd_row = -1; a.dnd_rect = Rect{}; a.dnd_mime = 0;
        window_dnd_accept(w, 0, 0, 0);
      }
      a.w.need_redraw = true;
    }
    return;
  }
  if (type == WE_DND_DROP) {
    a.dnd_over = false;
    if (!a.dnd_target[0] || !a.dnd_mime) { window_dnd_finish(w, false); a.dnd_row = -1; a.dnd_rect = Rect{}; a.w.need_redraw = true; return; }
    int fd = window_dnd_receive(w, a.dnd_mime);
    if (fd < 0) { window_dnd_finish(w, false); a.dnd_target[0] = 0; a.dnd_row = -1; a.dnd_rect = Rect{}; return; }
    ClipXfer* x2 = (ClipXfer*)calloc(1, sizeof(ClipXfer));
    x2->fd = fd; x2->send = false; x2->mime = a.dnd_mime; x2->dnd = true;
    x2->action = w.dnd_action ? w.dnd_action : (u32)WL_DND_COPY;
    snprintf(x2->dest, sizeof x2->dest, "%s", a.dnd_target);
    a.clip_recv_pending++;
    clip_xfer_submit(a, x2);
    a.dnd_target[0] = 0; a.dnd_row = -1; a.dnd_rect = Rect{}; a.dnd_mime = 0;
    a.w.need_redraw = true;
  }
}

void dnd_drop_paths(App& a, const char* dest, const Clip& paths, u32 action) {
  a.drops++;
  Clip c;
  for (u32 i = 0; i < paths.count(); i++) c.add(paths.path(i));
  c.cut = action == WL_DND_MOVE;
  if (c.count()) { char d[4096]; snprintf(d, sizeof d, "%s", c.path(0)); if (char* sl = strrchr(d, '/')) { if (sl == d) sl[1] = 0; else *sl = 0; } snprintf(c.dir, sizeof c.dir, "%s", d); }
  if (a.debug) fprintf(stderr, "dnd: drop of %u item%s into %s (%s)\n", c.count(), c.count() == 1 ? "" : "s", dest, c.cut ? "move" : "copy");
  op_paste_into(a, dest, c);
}
