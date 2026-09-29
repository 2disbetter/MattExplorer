#include "app/app.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool config_dir(char* out, u32 cap, bool create) {
  const char* xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0]) snprintf(out, cap, "%s/mattexplorer", xdg);
  else {
    const char* home = getenv("HOME");
    if (!home) return false;
    snprintf(out, cap, "%s/.config/mattexplorer", home);
  }
  if (create) {
    char parent[512];
    snprintf(parent, sizeof parent, "%s", out);
    if (char* slash = strrchr(parent, '/')) { *slash = 0; mkdir(parent, 0755); }
    mkdir(out, 0755);
  }
  return true;
}

static const char* view_name(ViewMode v) { return v == VIEW_ICONS ? "icons" : v == VIEW_LIST ? "list" : "details"; }
static const char* sort_name(SortKey k)  { return k == SORT_SIZE ? "size" : k == SORT_MTIME ? "date" : k == SORT_KIND ? "type" : "name"; }

void config_load(App& a) {
  char dir[512], path[600], line[1200];
  if (!config_dir(dir, sizeof dir, false)) return;
  snprintf(path, sizeof path, "%s/config", dir);
  FILE* f = fopen(path, "r");
  if (!f) return;
  Config& c = a.cfg;
  while (fgets(line, sizeof line, f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\n' || !*p) continue;
    char* eq = strchr(p, '=');
    if (!eq) continue;
    *eq = 0;
    char* key = p; char* val = eq + 1;
    for (char* e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = 0;
    while (*val == ' ' || *val == '\t') val++;
    if (char* nl = strpbrk(val, "\r\n")) *nl = 0;
    if (!strcmp(key, "window")) { int w, h; if (sscanf(val, "%dx%d", &w, &h) == 2) { c.win_w = mx_clamp(w, 320, 16384); c.win_h = mx_clamp(h, 240, 16384); } }
    else if (!strcmp(key, "sidebar")) c.sidebar_w = mx_clamp(atoi(val), 120, 800);
    else if (!strcmp(key, "sidebar_visible")) c.sidebar_visible = atoi(val) != 0;
    else if (!strcmp(key, "show_hidden")) c.show_hidden = atoi(val) != 0;
    else if (!strcmp(key, "font_size")) c.font_px = mx_clamp((float)atof(val), 7.0f, 40.0f);
    else if (!strcmp(key, "theme")) { c.theme_follow = !strcmp(val, "omarchy"); if (!c.theme_follow) c.light = !strcmp(val, "light"); }
    else if (!strcmp(key, "terminal")) snprintf(c.terminal, sizeof c.terminal, "%s", val);
    else if (!strcmp(key, "thumbnails")) c.thumbnails = atoi(val) != 0;
    else if (!strcmp(key, "expanded")) snprintf(c.expanded, sizeof c.expanded, "%s", val);
    else if (!strcmp(key, "view")) c.view = !strcmp(val, "icons") ? VIEW_ICONS : !strcmp(val, "list") ? VIEW_LIST : VIEW_DETAILS;
    else if (!strcmp(key, "sort")) {
      c.sort.key = strstr(val, "size") ? SORT_SIZE : strstr(val, "date") ? SORT_MTIME : strstr(val, "type") ? SORT_KIND : SORT_NAME;
      c.sort.ascending = !strstr(val, "desc");
      c.sort.dirs_first = !strstr(val, "mixed");
    }
  }
  fclose(f);
}

static bool write_atomic(const char* path, const char* data, usize n) {
  char tmp[640];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE* f = fopen(tmp, "w");
  if (!f) return false;
  bool ok = fwrite(data, 1, n, f) == n;
  ok = fclose(f) == 0 && ok;
  if (!ok) { unlink(tmp); return false; }
  return rename(tmp, path) == 0;
}

void config_save(App& a) {
  char dir[512], path[600], buf[1024];
  if (!config_dir(dir, sizeof dir, true)) return;
  snprintf(path, sizeof path, "%s/config", dir);
  const Config& c = a.cfg;
  int n = snprintf(buf, sizeof buf,
                   "# MattExplorer settings (rewritten on exit; edit while the app is closed)\n"
                   "window = %dx%d\n"
                   "sidebar = %d\n"
                   "sidebar_visible = %d\n"
                   "view = %s\n"
                   "show_hidden = %d\n"
                   "sort = %s %s %s\n"
                   "font_size = %g\n"
                   "theme = %s\n"
                   "expanded = %s\n"
                   "terminal = %s\n"
                   "thumbnails = %d\n",
                   c.win_w, c.win_h, c.sidebar_w, c.sidebar_visible ? 1 : 0, view_name(c.view), c.show_hidden ? 1 : 0,
                   sort_name(c.sort.key), c.sort.ascending ? "asc" : "desc", c.sort.dirs_first ? "dirs_first" : "mixed",
                   (double)c.font_px, c.theme_follow ? "omarchy" : c.light ? "light" : "dark", c.expanded, c.terminal, c.thumbnails ? 1 : 0);
  if (n > 0) write_atomic(path, buf, (usize)n);
  a.cfg_dirty = false;
}

bool disk_expanded(const App& a, const char* disk) {
  usize n = strlen(disk);
  for (const char* p = a.cfg.expanded; *p;) {
    const char* e = strchr(p, ',');
    usize len = e ? (usize)(e - p) : strlen(p);
    if (len == n && !strncmp(p, disk, n)) return true;
    if (!e) break;
    p = e + 1;
  }
  return false;
}

void disk_toggle(App& a, const char* disk) {
  char out[sizeof a.cfg.expanded] = {};
  u32  o = 0;
  bool removed = false;
  usize n = strlen(disk);
  for (const char* p = a.cfg.expanded; *p;) {
    const char* e = strchr(p, ',');
    usize len = e ? (usize)(e - p) : strlen(p);
    if (len == n && !strncmp(p, disk, n)) removed = true;
    else if (len && o + len + 2 < sizeof out) { if (o) out[o++] = ','; memcpy(out + o, p, len); o += (u32)len; }
    if (!e) break;
    p = e + 1;
  }
  if (!removed && o + n + 2 < sizeof out) { if (o) out[o++] = ','; memcpy(out + o, disk, n); o += (u32)n; }
  out[o] = 0;
  memcpy(a.cfg.expanded, out, sizeof out);
  config_mark_dirty(a);
  a.w.need_redraw = true;
}

void config_mark_dirty(App& a) {
  a.cfg_dirty = true;
  a.cfg_save_at = window_now_ms() + 1000;
}

void pins_load(App& a) {
  char dir[512], path[600], line[1200];
  a.num_pins = 0;
  if (!config_dir(dir, sizeof dir, false)) return;
  snprintf(path, sizeof path, "%s/pins", dir);
  FILE* f = fopen(path, "r");
  if (!f) return;
  while (fgets(line, sizeof line, f) && a.num_pins < MAX_PINS) {
    if (char* nl = strpbrk(line, "\r\n")) *nl = 0;
    if (!line[0] || line[0] == '#') continue;
    Place& p = a.pins[a.num_pins++];
    snprintf(p.path, sizeof p.path, "%s", line);
    const char* base = strrchr(line, '/');
    snprintf(p.label, sizeof p.label, "%s", (base && base[1]) ? base + 1 : line);
    p.icon = ICON_FOLDER;
  }
  fclose(f);
}

void pins_save(App& a) {
  char dir[512], path[600];
  if (!config_dir(dir, sizeof dir, true)) return;
  snprintf(path, sizeof path, "%s/pins", dir);
  Array<char> buf;
  for (u32 i = 0; i < a.num_pins; i++) {
    u32 n = (u32)strlen(a.pins[i].path);
    memcpy(buf.push_n(n), a.pins[i].path, n);
    buf.push('\n');
  }
  write_atomic(path, buf.data ? buf.data : "", buf.len);
}
