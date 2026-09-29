#include "app/app.h"
#include "core/path.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static i64 now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (i64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

Tab& tab_active(App& a) { return a.tabs[a.tab_order[a.active]]; }

const char* tab_title(const Tab& t) {
  const char* base = strrchr(t.path, '/');
  return (base && base[1]) ? base + 1 : t.path;
}

void window_title(App& a) {
  if (!a.w.toplevel) return;
  char title[4200];
  snprintf(title, sizeof title, "%s - MattExplorer", tab_title(tab_active(a)));
  window_set_title(a.w, title);
}

static void tab_unwatch(App& a, Tab& t);
static void tab_reset(App& a, Tab& t) {
  tab_unwatch(a, t);
  if (t.dirfd >= 0) close(t.dirfd);
  t.dirfd = -1;
  t.listing.clear();
  t.order.clear(); t.rows.clear(); t.selected.clear(); t.hits.clear();
  t.hist_buf.clear(); t.hist_off.clear(); t.hist_pos = 0;
  t.selected_count = 0; t.cursor = t.anchor = -1; t.scroll = t.scroll_x = 0; t.first_vis = 0;
  t.filtering = false; ui_text_set(t.filter, Str());
  t.path[0] = 0; t.err[0] = 0;
  t.max_name_w = t.max_name_px = 0;
  t.renaming = -1; t.stat_all = false; t.stat_all_cursor = 0; t.gen++;
  t.used = false;
}

static void tab_unwatch(App& a, Tab& t) {
  if (t.watch >= 0) watch_release(a.watcher, t.watch);
  t.watch = -1;
}

void tab_watch(App& a, Tab& t) {
  tab_unwatch(a, t);
  if (a.watcher.fd < 0 || !t.used || !t.path[0]) return;
  t.watch = watch_add(a.watcher, t.path);
  if (a.debug && t.watch < 0) fprintf(stderr, "watch: cannot watch %s: %s\n", t.path, strerror(errno));
}

i32 tab_open(App& a, const char* path, bool make_active) {
  if (a.num_tabs >= MAX_TABS) { set_status(a, "No more tabs"); return -1; }
  u32 slot = 0;
  while (slot < MAX_TABS && a.tabs[slot].used) slot++;
  if (slot >= MAX_TABS) return -1;
  Tab& t = a.tabs[slot];
  t.used = true;
  t.sort = a.cfg.sort;
  t.view = a.cfg.view;
  if (!navigate(a, t, path, true)) {
    const char* home = getenv("HOME");
    if (!navigate(a, t, home ? home : "/", true) && !navigate(a, t, "/", true)) { tab_reset(a, t); return -1; }
  }

  u32 pos = a.num_tabs ? a.active + 1 : 0;
  for (u32 i = a.num_tabs; i > pos; i--) a.tab_order[i] = a.tab_order[i - 1];
  a.tab_order[pos] = slot;
  a.num_tabs++;
  if (make_active || a.num_tabs == 1) tab_activate(a, pos);
  a.w.need_redraw = true;
  return (i32)pos;
}

void tab_close(App& a, u32 pos) {
  if (pos >= a.num_tabs) return;
  if (a.num_tabs == 1) { a.running = false; return; } // closing the last tab closes the window
  tab_reset(a, a.tabs[a.tab_order[pos]]);
  for (u32 i = pos; i + 1 < a.num_tabs; i++) a.tab_order[i] = a.tab_order[i + 1];
  a.num_tabs--;
  if (a.active >= a.num_tabs) a.active = a.num_tabs - 1;
  else if (a.active > pos) a.active--;
  a.path_editing = false;
  window_title(a);
  a.w.need_redraw = true;
}

void tab_activate(App& a, u32 pos) {
  if (pos >= a.num_tabs) return;
  a.active = pos;
  a.tabs[a.tab_order[pos]].restore_scroll = true;
  a.path_editing = false;
  window_title(a);
  a.w.need_redraw = true;
}

static void history_push(Tab& t, const char* path) {

  if (t.hist_off.len) {
    u32 keep = t.hist_pos + 1;
    if (keep < t.hist_off.len) { t.hist_off.resize(keep); t.hist_buf.resize(t.hist_off[keep - 1] + (u32)strlen(t.hist_buf.data + t.hist_off[keep - 1]) + 1); }
    if (!strcmp(t.hist_buf.data + t.hist_off[t.hist_pos], path)) return; // same place: no new entry
  }
  if (t.hist_off.len >= HISTORY_CAP) { // forget the oldest entry
    u32 cut = t.hist_off[1];
    memmove(t.hist_buf.data, t.hist_buf.data + cut, t.hist_buf.len - cut);
    t.hist_buf.len -= cut;
    for (u32 i = 1; i < t.hist_off.len; i++) t.hist_off[i - 1] = t.hist_off[i] - cut;
    t.hist_off.pop();
  }
  u32 n = (u32)strlen(path);
  t.hist_off.push(t.hist_buf.len);
  memcpy(t.hist_buf.push_n(n + 1), path, n + 1);
  t.hist_pos = t.hist_off.len - 1;
}

bool can_go_back(const Tab& t)    { return t.hist_pos > 0; }
bool can_go_forward(const Tab& t) { return t.hist_pos + 1 < t.hist_off.len; }

void go_back(App& a, Tab& t) {
  if (!can_go_back(t)) return;
  u32 pos = t.hist_pos - 1;
  char target[4096];
  snprintf(target, sizeof target, "%s", t.hist_buf.data + t.hist_off[pos]);
  if (navigate(a, t, target, false)) t.hist_pos = pos;
}

void go_forward(App& a, Tab& t) {
  if (!can_go_forward(t)) return;
  u32 pos = t.hist_pos + 1;
  char target[4096];
  snprintf(target, sizeof target, "%s", t.hist_buf.data + t.hist_off[pos]);
  if (navigate(a, t, target, false)) t.hist_pos = pos;
}

void rebuild_rows(App& a, Tab& t) {
  t.rows.clear();
  t.hidden_count = 0;
  for (u32 i = 0; i < t.listing.count(); i++) t.hidden_count += (t.listing.flags[i] & EF_HIDDEN) != 0;
  if (t.filtering && t.filter.len) {
    fuzzy_filter(t.listing, ui_text_str(t.filter), t.hits);
    for (FuzzyHit& h : t.hits) if (a.cfg.show_hidden || !(t.listing.flags[h.index] & EF_HIDDEN)) t.rows.push(h.index);
  } else {
    for (u32 i : t.order) if (a.cfg.show_hidden || !(t.listing.flags[i] & EF_HIDDEN)) t.rows.push(i);
  }
}

void resort(App& a, Tab& t) {
  i64 t0 = now_ns();
  if (t.sort.key == SORT_SIZE || t.sort.key == SORT_MTIME) {
    if (a.sync_stat) dir_list_stat_range(t.dirfd, t.listing, 0, t.listing.count());
    else {
      bool missing = false;
      for (u32 i = 0; i < t.listing.count() && !missing; i++) missing = !(t.listing.flags[i] & (EF_STATTED | EF_STAT_FAILED));
      if (missing && !t.stat_all) { t.stat_all = true; t.stat_all_cursor = 0; }
    }
  } else t.stat_all = false;
  sort_listing(t.listing, t.sort, t.order);
  rebuild_rows(a, t);
  t.sort_ns = now_ns() - t0;
}

void expand_path(const char* in, char* out, u32 cap) { path_normalize(in, out, cap); }

bool navigate(App& a, Tab& t, const char* raw, bool push_history) {
  char path[4096];
  expand_path(raw, path, sizeof path);
  int fd = dir_open(path);
  if (fd < 0) { snprintf(t.err, sizeof t.err, "%s: %s", path, strerror(errno)); set_status(a, t.err); a.w.need_redraw = true; return false; }
  i64 t0 = now_ns();
  DirListing fresh;
  if (!dir_list_read(fd, fresh)) { snprintf(t.err, sizeof t.err, "%s: %s", path, strerror(errno)); set_status(a, t.err); close(fd); a.w.need_redraw = true; return false; }
  t.list_ns = now_ns() - t0;
  if (t.dirfd >= 0) close(t.dirfd);
  t.dirfd = fd;
  t.listing = static_cast<DirListing&&>(fresh);
  snprintf(t.path, sizeof t.path, "%s", path);
  t.err[0] = 0;
  t.selected.resize_zero(t.listing.count());
  memset(t.selected.data, 0, t.selected.len);
  t.selected_count = 0;
  t.scroll = t.scroll_x = 0; t.cursor = t.anchor = -1; t.first_vis = 0; t.keep_first_vis = false;
  t.filtering = false; ui_text_set(t.filter, Str());
  t.max_name_w = t.max_name_px = 0;
  t.gen++; t.renaming = -1; t.stat_all = false; t.stat_all_cursor = 0;
  trash_cols_reset(t);
  resort(a, t);
  if (push_history) history_push(t, path);
  if (&t == &tab_active(a)) { window_title(a); a.path_editing = false; }
  tab_watch(a, t);
  a.w.need_redraw = true;
  return true;
}

void reload(App& a, Tab& t) { refresh_tab(a, t, nullptr, 0, true); }

void tab_gone(App& a, Tab& t) {
  char up[4096], msg[4200];
  snprintf(up, sizeof up, "%s", t.path);
  snprintf(msg, sizeof msg, "%s is gone", t.path);
  while (strcmp(up, "/")) {
    char* slash = strrchr(up, '/');
    if (slash == up) up[1] = 0; else *slash = 0;
    struct stat st;
    if (stat(up, &st) == 0 && S_ISDIR(st.st_mode)) break;
  }
  navigate(a, t, up, true);
  set_status(a, msg);
}

void refresh_tab(App& a, Tab& t, const char* stale, u32 stale_n, bool drop_stats) {
  if (t.dirfd < 0) return;
  { struct stat st;
    if (stat(t.path, &st) != 0 && (errno == ENOENT || errno == ENOTDIR || errno == ESTALE)) { tab_gone(a, t); return; } }
  Array<i32> map;
  u32 first_idx = (t.first_vis >= 0 && (u32)t.first_vis < t.rows.len) ? t.rows[(u32)t.first_vis] : 0xFFFFFFFFu;
  u32 cur_idx = (t.cursor >= 0 && (u32)t.cursor < t.rows.len) ? t.rows[(u32)t.cursor] : 0xFFFFFFFFu;
  u32 anchor_idx = (t.anchor >= 0 && (u32)t.anchor < t.rows.len) ? t.rows[(u32)t.anchor] : 0xFFFFFFFFu;
  i32 renaming = t.renaming, cur_row = t.cursor;
  i64 t0 = now_ns();
  if (!dir_list_refresh(t.dirfd, t.listing, stale, stale_n, drop_stats, map)) {
    if (errno == ENOENT || errno == ESTALE || errno == ENOTDIR) tab_gone(a, t);
    else { snprintf(t.err, sizeof t.err, "%s: %s", t.path, strerror(errno)); set_status_error(a, t.err); }
    return;
  }
  t.list_ns = now_ns() - t0;
  t.gen++;

  Array<u8> sel;
  sel.resize_zero(t.listing.count());
  u32 count = 0;
  for (u32 i = 0; i < map.len && i < t.selected.len; i++) if (t.selected[i] && map[i] >= 0) { sel[(u32)map[i]] = 1; count++; }
  t.selected = static_cast<Array<u8>&&>(sel);
  t.selected_count = count;
  if (t.trash_cols) {
    Array<u32> off; Array<i64> del;
    off.resize_zero(t.listing.count()); del.resize_zero(t.listing.count());
    memset(off.data, 0, (usize)off.len * sizeof(u32)); memset(del.data, 0, (usize)del.len * sizeof(i64));
    for (u32 i = 0; i < map.len && i < t.trash_orig_off.len; i++) if (map[i] >= 0 && (t.listing.flags[(u32)map[i]] & EF_STATTED)) { off[(u32)map[i]] = t.trash_orig_off[i]; del[(u32)map[i]] = t.trash_deleted[i]; }
    t.trash_orig_off = static_cast<Array<u32>&&>(off); t.trash_deleted = static_cast<Array<i64>&&>(del);
  }
  t.err[0] = 0;
  t.max_name_w = t.max_name_px = 0;
  t.stat_all = false; t.stat_all_cursor = 0;
  resort(a, t);
  auto row_of = [&](u32 old_idx) -> i32 {
    if (old_idx == 0xFFFFFFFFu || old_idx >= map.len || map[old_idx] < 0) return -1;
    u32 ni = (u32)map[old_idx];
    for (u32 r = 0; r < t.rows.len; r++) if (t.rows[r] == ni) return (i32)r;
    return -1;
  };
  t.cursor = row_of(cur_idx);
  t.anchor = row_of(anchor_idx);
  if (t.cursor < 0 && cur_idx != 0xFFFFFFFFu && t.rows.len) {
    t.cursor = t.anchor = mx_clamp(cur_row, 0, (i32)t.rows.len - 1);
    if (!t.selected_count) { t.selected[t.rows[(u32)t.cursor]] = 1; t.selected_count = 1; }
  }
  i32 first_row = row_of(first_idx);
  if (first_row >= 0) { t.first_vis = first_row; t.keep_first_vis = t.cursor >= 0; t.restore_scroll = true; }
  if (renaming >= 0) t.renaming = ((u32)renaming < map.len && map[(u32)renaming] >= 0) ? map[(u32)renaming] : -1;
  a.w.need_redraw = true;
}

void refresh_path(App& a, const char* dir, const char* stale) {
  for (Tab& t : a.tabs)
    if (t.used && !strcmp(t.path, dir)) refresh_tab(a, t, stale, stale ? (u32)strlen(stale) + 1 : 0, false);
}

void select_names(App& a, Tab& t, const char* const* names, u32 n) {
  clear_selection(a, t);
  t.cursor = t.anchor = -1;
  for (u32 r = 0; r < t.rows.len; r++) {
    u32 i = t.rows[r];
    for (u32 k = 0; k < n; k++) {
      if (strcmp(t.listing.cname(i), names[k])) continue;
      t.selected[i] = 1; t.selected_count++;
      if (t.cursor < 0 || (k == 0 && strcmp(t.listing.cname(t.rows[(u32)t.cursor]), names[0]))) t.cursor = t.anchor = (i32)r;
      break;
    }
  }
  if (t.cursor >= 0) ensure_visible(a, t, t.cursor);
  a.w.need_redraw = true;
}

void entry_path(const Tab& t, u32 i, char* out, u32 cap) {
  if (!strcmp(t.path, "/")) snprintf(out, cap, "/%s", t.listing.cname(i));
  else snprintf(out, cap, "%s/%s", t.path, t.listing.cname(i));
}

static StatBatch* g_frame_batch = nullptr; // the batch being filled during this frame
static u32        g_frame_batch_tab = 0;

static StatBatch* stat_take(App& a) {
  for (u32 k = 0; k < STAT_POOL; k++)
    if (a.stat_free & (1u << k)) { a.stat_free &= ~(1u << k); return a.stat_pool[k]; }
  return nullptr;
}
static void stat_give(App& a, StatBatch* b) {
  for (u32 k = 0; k < STAT_POOL; k++) if (a.stat_pool[k] == b) a.stat_free |= 1u << k;
}
static void job_stat_batch(Job& j) { stat_batch_run(*(StatBatch*)j.ctx); }

static bool stat_submit(App& a, StatBatch* b) {
  Tab& t = a.tabs[b->tab];
  if (!b->n) { stat_batch_end(*b); stat_give(a, b); return true; }
  b->trash = t.trash_cols;
  if (b->trash) snprintf(b->trash_dir, sizeof b->trash_dir, "%s", t.path);
  if (!worker_submit(a.pool, job_stat_batch, b, JOB_STAT_BATCH, 0)) { // queue full: undo the marks, try next frame
    for (u32 k = 0; k < b->n; k++) if (b->index[k] < t.listing.count()) t.listing.flags[b->index[k]] &= (u8)~EF_STAT_PENDING;
    stat_batch_end(*b); stat_give(a, b);
    return false;
  }
  t.stat_pending++;
  return true;
}

static void trash_info_set(Tab& t, u32 i, const char* orig, i64 deleted) {
  if (i >= t.trash_orig_off.len) return;
  if (orig && orig[0]) { u32 l = (u32)strlen(orig); t.trash_orig_off[i] = t.trash_orig.len; memcpy(t.trash_orig.push_n(l + 1), orig, l + 1); }
  else t.trash_orig_off[i] = 0;
  t.trash_deleted[i] = deleted ? deleted : -1;
}
void trash_cols_reset(Tab& t) {
  t.trash_cols = trash_is_files_dir(t.path);
  t.trash_orig.clear(); t.trash_orig.push(0); // offset 0: unknown
  t.trash_orig_off.resize_zero(t.listing.count()); t.trash_deleted.resize_zero(t.listing.count());
  memset(t.trash_orig_off.data, 0, (usize)t.trash_orig_off.len * sizeof(u32)); memset(t.trash_deleted.data, 0, (usize)t.trash_deleted.len * sizeof(i64));
}
const char* trash_original(const Tab& t, u32 i) { return t.trash_cols && i < t.trash_orig_off.len && t.trash_orig_off[i] ? t.trash_orig.data + t.trash_orig_off[i] : ""; }

void stat_request(App& a, Tab& t, u32 i) {
  if (a.sync_stat) {
    bool was = (t.listing.flags[i] & (EF_STATTED | EF_STAT_FAILED)) != 0;
    dir_list_stat_range(t.dirfd, t.listing, i, 1);
    if (t.trash_cols && !was && i < t.trash_deleted.len && !t.trash_deleted[i]) {
      char p[4096], orig[4096]; i64 del = 0;
      entry_path(t, i, p, sizeof p);
      bool have = trash_info_read(p, orig, sizeof orig, &del);
      trash_info_set(t, i, have ? orig : nullptr, del);
    }
    return;
  }
  if (t.listing.flags[i] & (EF_STATTED | EF_STAT_FAILED | EF_STAT_PENDING)) return;
  u32 slot = (u32)(&t - a.tabs);
  if (g_frame_batch && g_frame_batch_tab != slot) { stat_submit(a, g_frame_batch); g_frame_batch = nullptr; }
  if (!g_frame_batch) {
    g_frame_batch = stat_take(a);
    if (!g_frame_batch) return; // every batch is in flight: next frame
    if (!stat_batch_begin(*g_frame_batch, t.dirfd, slot, t.gen)) { stat_give(a, g_frame_batch); g_frame_batch = nullptr; return; }
    g_frame_batch_tab = slot;
  }
  if (!stat_batch_add(*g_frame_batch, t.listing, i)) { // full: send it, start another
    stat_submit(a, g_frame_batch);
    g_frame_batch = stat_take(a);
    if (!g_frame_batch) return;
    if (!stat_batch_begin(*g_frame_batch, t.dirfd, slot, t.gen)) { stat_give(a, g_frame_batch); g_frame_batch = nullptr; return; }
    stat_batch_add(*g_frame_batch, t.listing, i);
  }
}

void stat_flush(App& a) {
  if (g_frame_batch) { stat_submit(a, g_frame_batch); g_frame_batch = nullptr; }
  if (a.sync_stat) return;

  for (u32 slot = 0; slot < MAX_TABS; slot++) {
    Tab& t = a.tabs[slot];
    if (!t.used || !t.stat_all) continue;
    while (t.stat_all_cursor < t.listing.count()) {
      StatBatch* b = stat_take(a);
      if (!b) break;
      if (!stat_batch_begin(*b, t.dirfd, slot, t.gen)) { stat_give(a, b); break; }
      while (t.stat_all_cursor < t.listing.count()) {
        u32 i = t.stat_all_cursor;
        if (t.listing.flags[i] & (EF_STATTED | EF_STAT_FAILED | EF_STAT_PENDING)) { t.stat_all_cursor++; continue; }
        if (!stat_batch_add(*b, t.listing, i)) break;
        t.stat_all_cursor++;
      }
      if (!stat_submit(a, b)) break;
    }
    if (t.stat_all_cursor >= t.listing.count() && !t.stat_pending) { // everything answered: the real sort
      t.stat_all = false;
      u32 keep = t.cursor >= 0 && (u32)t.cursor < t.rows.len ? t.rows[(u32)t.cursor] : 0xFFFFFFFFu;
      resort(a, t);
      t.cursor = -1;
      for (u32 i = 0; i < t.rows.len; i++) if (t.rows[i] == keep) { t.cursor = t.anchor = (i32)i; break; }
      if (&t == &tab_active(a)) ensure_visible(a, t, t.cursor);
      a.w.need_redraw = true;
    }
  }
}

void stat_job_done(App& a, StatBatch* b) {
  Tab& t = a.tabs[b->tab];
  if (t.used && t.gen == b->gen) {
    stat_batch_apply(*b, t.listing);
    if (b->trash && t.trash_cols) for (u32 k = 0; k < b->n; k++) if (b->index[k] < t.trash_deleted.len && !t.trash_deleted[b->index[k]]) trash_info_set(t, b->index[k], b->trash_off[k] != 0xFFFFFFFFu ? b->trash_buf + b->trash_off[k] : nullptr, b->trash_deleted[k]);
    if (t.stat_pending) t.stat_pending--;
  } else if (t.used && t.stat_pending) t.stat_pending--;
  stat_batch_end(*b);
  stat_give(a, b);
  a.w.need_redraw = true;
  if (t.used && t.stat_all) stat_flush(a);
}

void go_parent(App& a, Tab& t) {
  if (!strcmp(t.path, "/")) return;
  char up[4096];
  snprintf(up, sizeof up, "%s", t.path);
  char* slash = strrchr(up, '/');
  if (!slash) return;
  if (slash == up) up[1] = 0; else *slash = 0;
  const char* from = strrchr(t.path, '/');
  from = from ? from + 1 : t.path;
  char came[4096];
  snprintf(came, sizeof came, "%s", from);
  if (!navigate(a, t, up, true)) return;
  for (u32 r = 0; r < t.rows.len; r++)
    if (str_eq(t.listing.name(t.rows[r]), came)) {
      t.cursor = t.anchor = (i32)r; t.selected[t.rows[r]] = 1; t.selected_count = 1;
      ensure_visible(a, t, (i32)r);
      break;
    }
}

void open_row(App& a, Tab& t, i32 row, bool new_tab) {
  if (row < 0 || (u32)row >= t.rows.len) return;
  u32 i = t.rows[(u32)row];
  if (!(t.listing.flags[i] & EF_STATTED)) dir_list_stat_range(t.dirfd, t.listing, i, 1);
  if (!t.listing.is_dir(i)) { open_files(a, t, -1); return; }
  char child[4096];
  int  n = !strcmp(t.path, "/") ? snprintf(child, sizeof child, "/%s", t.listing.cname(i))
                                : snprintf(child, sizeof child, "%s/%s", t.path, t.listing.cname(i));
  if (n < 0 || n >= (int)sizeof child) { set_status(a, "Path too long"); return; }
  if (new_tab) tab_open(a, child, false);
  else navigate(a, t, child, true);
}

void set_sort(App& a, Tab& t, SortKey key, bool toggle_dir) {
  if (t.sort.key == key && toggle_dir) t.sort.ascending = !t.sort.ascending;
  else if (t.sort.key != key) { t.sort.key = key; t.sort.ascending = key == SORT_NAME || key == SORT_KIND; }
  u32 keep = t.cursor >= 0 ? t.rows[(u32)t.cursor] : 0xFFFFFFFFu;
  resort(a, t);
  t.cursor = -1;
  for (u32 i = 0; i < t.rows.len; i++) if (t.rows[i] == keep) { t.cursor = t.anchor = (i32)i; break; }
  ensure_visible(a, t, t.cursor);
  a.cfg.sort = t.sort;
  config_mark_dirty(a);
  a.w.need_redraw = true;
}

void set_view(App& a, Tab& t, ViewMode v) {
  if (t.view == v) return;
  t.view = v;
  t.scroll = t.scroll_x = 0;
  ensure_visible(a, t, t.cursor);
  a.cfg.view = v;
  config_mark_dirty(a);
  a.w.need_redraw = true;
}

void filter_changed(App& a, Tab& t) {
  t.filtering = t.filter.len > 0;
  u32 keep = t.cursor >= 0 && (u32)t.cursor < t.rows.len ? t.rows[(u32)t.cursor] : 0xFFFFFFFFu;
  rebuild_rows(a, t);
  t.cursor = -1;
  if (t.filtering) { if (t.rows.len) t.cursor = t.anchor = 0; }
  else for (u32 i = 0; i < t.rows.len; i++) if (t.rows[i] == keep) { t.cursor = t.anchor = (i32)i; break; }
  clear_selection(a, t);
  if (t.cursor >= 0) { t.selected[t.rows[(u32)t.cursor]] = 1; t.selected_count = 1; }
  t.scroll = t.scroll_x = 0;
  ensure_visible(a, t, t.cursor);
  a.w.need_redraw = true;
}

void filter_clear(App& a, Tab& t) {
  if (!t.filtering && !t.filter.len) return;
  ui_text_set(t.filter, Str());
  filter_changed(a, t);
}

void clear_selection(App& a, Tab& t) {
  if (t.selected_count) memset(t.selected.data, 0, t.selected.len);
  t.selected_count = 0;
  a.w.need_redraw = true;
}

void select_range(Tab& t, i32 from, i32 to) {
  if (from > to) { i32 x = from; from = to; to = x; }
  for (i32 r = mx_max(from, 0); r <= to && (u32)r < t.rows.len; r++) {
    u32 i = t.rows[(u32)r];
    if (!t.selected[i]) { t.selected[i] = 1; t.selected_count++; }
  }
}

void select_all(App& a, Tab& t) {
  select_range(t, 0, (i32)t.rows.len - 1);
  a.w.need_redraw = true;
}

void ensure_visible(App& a, Tab& t, i32 row) {
  if (row < 0 || (u32)row >= t.rows.len) return;
  const Layout& L = a.L;
  if (L.list.w <= 0 || L.list.h <= 0) return;
  Rect r = cell_rect(a, t, row);

  i32 top = r.y - L.list.y + t.scroll, bot = top + r.h;
  i32 left = r.x - L.list.x + t.scroll_x, right = left + r.w;
  if (top < t.scroll) t.scroll = top;
  else if (bot > t.scroll + L.list.h) t.scroll = bot - L.list.h;
  if (left < t.scroll_x) t.scroll_x = left;
  else if (right > t.scroll_x + L.list.w) t.scroll_x = right - L.list.w;
  if (t.scroll < 0) t.scroll = 0;
  if (t.scroll_x < 0) t.scroll_x = 0;
}

void set_cursor(App& a, Tab& t, i32 row, bool extend, bool toggle) {
  if (!t.rows.len) return;
  row = mx_clamp(row, 0, (i32)t.rows.len - 1);
  if (toggle && extend && t.anchor >= 0) { // Ctrl+Shift: the range joins what is selected
    select_range(t, t.anchor, row);
  } else if (toggle) {
    u32 i = t.rows[(u32)row];
    t.selected[i] ^= 1;
    t.selected_count += t.selected[i] ? 1 : (u32)-1;
    t.anchor = row;
  } else if (extend && t.anchor >= 0) {
    clear_selection(a, t);
    select_range(t, t.anchor, row);
  } else {
    clear_selection(a, t);
    select_range(t, row, row);
    t.anchor = row;
  }
  t.cursor = row;
  ensure_visible(a, t, row);
  a.w.need_redraw = true;
}

void set_status(App& a, const char* msg) {
  snprintf(a.status_msg, sizeof a.status_msg, "%s", msg);
  a.status_msg_until = window_now_ms() + 4000;
  a.status_error = false;
  a.w.need_redraw = true;
}

void set_status_error(App& a, const char* msg) {
  snprintf(a.status_msg, sizeof a.status_msg, "%s", msg);
  a.status_msg_until = window_now_ms() + 15000;
  a.status_error = true;
  a.w.need_redraw = true;
}

static void add_place(App& a, const char* label, const char* path, u8 icon) {
  if (a.num_places >= MAX_PLACES) return;
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return;
  for (u32 i = 0; i < a.num_places; i++) if (!strcmp(a.places[i].path, path)) return;
  Place& p = a.places[a.num_places++];
  snprintf(p.label, sizeof p.label, "%s", label);
  snprintf(p.path, sizeof p.path, "%s", path);
  p.icon = icon;
}

void load_places(App& a) {
  a.num_places = 0;
  const char* home = getenv("HOME");
  if (!home) home = "/";
  add_place(a, "Home", home, ICON_HOME);
  static const struct { const char* key; const char* label; const char* dflt; u8 icon; } dirs[] = {
    { "XDG_DESKTOP_DIR", "Desktop", "Desktop", ICON_DESKTOP },   { "XDG_DOCUMENTS_DIR", "Documents", "Documents", ICON_DOCUMENTS },
    { "XDG_DOWNLOAD_DIR", "Downloads", "Downloads", ICON_DOWNLOADS }, { "XDG_MUSIC_DIR", "Music", "Music", ICON_MUSIC },
    { "XDG_PICTURES_DIR", "Pictures", "Pictures", ICON_PICTURES }, { "XDG_VIDEOS_DIR", "Videos", "Videos", ICON_VIDEOS },
  };
  char cfg[512], line[600], resolved[MX_ARRAY_COUNT(dirs)][512] = {};
  snprintf(cfg, sizeof cfg, "%s/.config/user-dirs.dirs", home);
  if (FILE* f = fopen(cfg, "r")) {
    while (fgets(line, sizeof line, f)) {
      for (u32 i = 0; i < MX_ARRAY_COUNT(dirs); i++) {
        usize kl = strlen(dirs[i].key);
        if (strncmp(line, dirs[i].key, kl) || line[kl] != '=') continue;
        char* v = line + kl + 1;
        if (*v == '"') v++;
        char* e = strpbrk(v, "\"\n");
        if (e) *e = 0;
        if (!strncmp(v, "$HOME", 5)) snprintf(resolved[i], 512, "%s%s", home, v + 5);
        else snprintf(resolved[i], 512, "%s", v);
      }
    }
    fclose(f);
  }
  for (u32 i = 0; i < MX_ARRAY_COUNT(dirs); i++) {
    if (!resolved[i][0]) snprintf(resolved[i], 512, "%s/%s", home, dirs[i].dflt);
    if (strcmp(resolved[i], home)) add_place(a, dirs[i].label, resolved[i], dirs[i].icon);
  }
  char trash[4096];
  if (trash_home(trash, sizeof trash)) { snprintf(cfg, sizeof cfg, "%s/files", trash); add_place(a, "Trash", cfg, ICON_TRASH); }
}

bool toggle_pin(App& a, const char* path) {
  for (u32 i = 0; i < a.num_pins; i++) {
    if (strcmp(a.pins[i].path, path)) continue;
    for (u32 k = i; k + 1 < a.num_pins; k++) a.pins[k] = a.pins[k + 1];
    a.num_pins--;
    pins_save(a);
    set_status(a, "Unpinned");
    a.w.need_redraw = true;
    return false;
  }
  if (a.num_pins >= MAX_PINS) { set_status(a, "No more pins"); return false; }
  Place& p = a.pins[a.num_pins++];
  snprintf(p.path, sizeof p.path, "%s", path);
  const char* base = strrchr(path, '/');
  snprintf(p.label, sizeof p.label, "%s", (base && base[1]) ? base + 1 : path);
  p.icon = ICON_FOLDER;
  pins_save(a);
  set_status(a, "Pinned to the sidebar");
  a.w.need_redraw = true;
  return true;
}

static bool ext_in(Str ext, const char* const* list, u32 n) {
  for (u32 i = 0; i < n; i++) if (str_ieq(ext, list[i])) return true;
  return false;
}

u8 entry_icon(const DirListing& l, u32 i) {
  if (l.is_dir(i)) return ICON_FOLDER;
  if (l.kind[i] == EK_SYMLINK) return ICON_LINK;
  if ((l.flags[i] & EF_STATTED) && (l.mode[i] & 0111)) return ICON_EXEC;
  Str ext = str_extension(l.name(i));
  if (ext.empty() || ext.n > 8) return ICON_FILE;
  static const char* const img[]  = { "png", "jpg", "jpeg", "gif", "webp", "svg", "bmp", "avif", "heic", "tiff", "ico" };
  static const char* const arc[]  = { "zip", "tar", "gz", "xz", "zst", "7z", "rar", "bz2", "tgz", "deb", "rpm", "iso" };
  static const char* const code[] = { "c", "cpp", "cc", "h", "hpp", "py", "rs", "js", "ts", "sh", "go", "java", "toml", "json",
                                      "yaml", "yml", "cmake", "lua", "rb", "css", "html", "zig", "nix" };
  static const char* const text[] = { "txt", "md", "log", "cfg", "conf", "ini", "csv", "rst", "org" };
  if (ext_in(ext, img, MX_ARRAY_COUNT(img)))   return ICON_IMAGE;
  if (ext_in(ext, arc, MX_ARRAY_COUNT(arc)))   return ICON_ARCHIVE;
  if (ext_in(ext, code, MX_ARRAY_COUNT(code))) return ICON_CODE;
  if (ext_in(ext, text, MX_ARRAY_COUNT(text))) return ICON_TEXT;
  return ICON_FILE;
}

const char* entry_kind(const DirListing& l, u32 i, char* b, u32 cap) {
  if (l.is_dir(i)) return "Folder";
  if (l.kind[i] == EK_SYMLINK) return "Link";
  if (l.kind[i] == EK_OTHER) return "Device";
  Str ext = str_extension(l.name(i));
  if (ext.empty() || ext.n > 7) return "File";
  u32 o = 0;
  for (u32 k = 0; k < ext.n && o + 6 < cap; k++) { char c = ext.p[k]; b[o++] = is_lower(c) ? (char)(c - 32) : c; }
  memcpy(b + o, " file", 6);
  return b;
}
