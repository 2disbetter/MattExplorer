#include "ui/theme.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

float theme_luma(u32 c) {
  float r = (float)((c >> 16) & 255) / 255.0f, g = (float)((c >> 8) & 255) / 255.0f, b = (float)(c & 255) / 255.0f;
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

u32 theme_mix(u32 a, u32 b, float t) {
  t = mx_clamp(t, 0.0f, 1.0f);
  u32 out = 0;
  for (u32 shift = 0; shift < 24; shift += 8) {
    float ca = (float)((a >> shift) & 255), cb = (float)((b >> shift) & 255);
    u32 v = (u32)(ca * (1.0f - t) + cb * t + 0.5f);
    out |= mx_min(v, 255u) << shift;
  }
  return out;
}

static bool parse_hex6(Str s, u32* out) {
  while (s.n && (s.p[0] == ' ' || s.p[0] == '"' || s.p[0] == '\'')) { s.p++; s.n--; }
  while (s.n && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '"' || s.p[s.n - 1] == '\'')) s.n--;
  if (s.n && s.p[0] == '#') { s.p++; s.n--; }
  else if (s.n >= 2 && s.p[0] == '0' && (s.p[1] == 'x' || s.p[1] == 'X')) { s.p += 2; s.n -= 2; }
  if (s.n < 6) return false;
  u32 v = 0;
  for (u32 i = 0; i < 6; i++) {
    char c = s.p[i];
    u32 d = c >= '0' && c <= '9' ? (u32)(c - '0') : c >= 'a' && c <= 'f' ? (u32)(c - 'a' + 10) : c >= 'A' && c <= 'F' ? (u32)(c - 'A' + 10) : 16;
    if (d > 15) return false;
    v = (v << 4) | d;
  }
  *out = v;
  return true;
}

static Str line_at(Str text, u32& pos) {
  u32 s = pos;
  while (pos < text.n && text.p[pos] != '\n') pos++;
  Str l(text.p + s, pos - s);
  if (pos < text.n) pos++;
  while (l.n && (l.p[0] == ' ' || l.p[0] == '\t')) { l.p++; l.n--; }
  while (l.n && (l.p[l.n - 1] == ' ' || l.p[l.n - 1] == '\t' || l.p[l.n - 1] == '\r')) l.n--;
  return l;
}

void theme_parse_alacritty(Str toml, ThemeColors& out) {
  static const char* const names[8] = { "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white" };
  char section[64] = {};
  for (u32 pos = 0; pos < toml.n;) {
    Str l = line_at(toml, pos);
    if (!l.n || l.p[0] == '#') continue;
    if (l.p[0] == '[') {
      u32 e = 1; while (e < l.n && l.p[e] != ']') e++;
      snprintf(section, sizeof section, "%.*s", (int)mx_min(e - 1, (u32)sizeof section - 1), l.p + 1);
      continue;
    }
    const char* eq = (const char*)memchr(l.p, '=', l.n);
    if (!eq) continue;
    Str k(l.p, (u32)(eq - l.p)), v(eq + 1, (u32)(l.p + l.n - eq - 1));
    while (k.n && (k.p[k.n - 1] == ' ' || k.p[k.n - 1] == '\t')) k.n--;

    char full[128];
    if (memchr(k.p, '.', k.n)) snprintf(full, sizeof full, "%.*s", (int)k.n, k.p);
    else snprintf(full, sizeof full, "%s.%.*s", section, (int)k.n, k.p);
    u32 c;
    if (!parse_hex6(v, &c)) continue;
    if (!strcmp(full, "colors.primary.background")) { out.bg = c; out.have_bg = true; }
    else if (!strcmp(full, "colors.primary.foreground")) { out.fg = c; out.have_fg = true; }
    else if (!strncmp(full, "colors.normal.", 14) || !strncmp(full, "colors.bright.", 14)) {
      u32 base = full[7] == 'b' ? 8 : 0;
      for (u32 i = 0; i < 8; i++) if (!strcmp(full + 14, names[i])) { out.ansi[base + i] = c; out.have_ansi |= 1u << (base + i); }
    }
  }
}

void theme_parse_hyprland(Str conf, ThemeColors& out) {
  for (u32 pos = 0; pos < conf.n;) {
    Str l = line_at(conf, pos);
    if (!l.n || l.p[0] == '#') continue;
    const char* k = (const char*)memmem(l.p, l.n, "col.active_border", 17);
    if (!k) continue;
    const char* rgb = (const char*)memmem(k, (u32)(l.p + l.n - k), "rgb", 3);
    if (!rgb) continue;
    const char* open = (const char*)memchr(rgb, '(', (u32)(l.p + l.n - rgb));
    if (!open) continue;
    u32 c;
    if (parse_hex6(Str(open + 1, (u32)mx_min((u32)(l.p + l.n - open - 1), 8u)), &c)) { out.accent = c; out.have_accent = true; return; }
  }
}

static bool read_small(const char* path, Array<char>& out) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  out.clear();
  for (;;) {
    char b[8192];
    ssize_t n = read(fd, b, sizeof b);
    if (n <= 0) break;
    memcpy(out.push_n((u32)n), b, (u32)n);
    if (out.len > (1u << 20)) break;
  }
  close(fd);
  return true;
}

bool theme_omarchy_dir(char* out, u32 cap) {
  const char* env = getenv("OMARCHY_THEME_DIR");
  if (env && env[0]) { snprintf(out, cap, "%s", env); return true; }
  const char* home = getenv("HOME");
  if (!home) return false;
  snprintf(out, cap, "%s/.config/omarchy/current/theme", home);
  return true;
}

bool theme_omarchy_watch_dir(char* out, u32 cap) {
  const char* home = getenv("HOME");
  if (!home) return false;
  snprintf(out, cap, "%s/.config/omarchy/current", home);
  return true;
}

bool theme_load_dir(const char* dir, ThemeColors& out) {
  out = ThemeColors();
  Array<char> text;
  char path[1024];
  snprintf(path, sizeof path, "%s/alacritty.toml", dir);
  if (!read_small(path, text)) return false;
  theme_parse_alacritty(Str(text.data, text.len), out);
  if (!out.have_bg || !out.have_fg) return false;
  snprintf(path, sizeof path, "%s/hyprland.conf", dir);
  if (read_small(path, text)) theme_parse_hyprland(Str(text.data, text.len), out);
  snprintf(path, sizeof path, "%s/light.mode", dir);
  struct stat st;
  out.light = stat(path, &st) == 0 || theme_luma(out.bg) > 0.5f;

  char target[1024];
  ssize_t n = readlink(dir, target, sizeof target - 1);
  const char* name = dir;
  if (n > 0) { target[n] = 0; while (n > 1 && target[n - 1] == '/') target[--n] = 0; name = target; }
  const char* slash = strrchr(name, '/');
  snprintf(out.name, sizeof out.name, "%s", slash ? slash + 1 : name);
  return true;
}

void theme_derive(const ThemeColors& c, UiTheme& t, bool* light) {
  bool lt = c.light;
  u32 bg = c.bg, fg = c.fg;
  auto ansi = [&](u32 i, u32 fallback) { return (c.have_ansi & (1u << i)) ? c.ansi[i] : fallback; };
  u32 red = ansi(1, lt ? 0xf52a65 : 0xf7768e), yellow = ansi(3, lt ? 0xd7891f : 0xe0af68), blue = ansi(4, lt ? 0x2e7de9 : 0x7aa2f7);
  u32 magenta = ansi(5, lt ? 0x9854f1 : 0xbb9af7);
  u32 accent = c.have_accent ? c.accent : blue;

  if (mx_max(theme_luma(accent), theme_luma(bg)) - mx_min(theme_luma(accent), theme_luma(bg)) < 0.15f) accent = blue;

  auto up   = [&](float k) { return theme_mix(bg, lt ? 0x000000 : 0xffffff, k); }; // "raised": panels, hover rows
  auto down = [&](float k) { return theme_mix(bg, lt ? 0xffffff : 0x000000, k); }; // "sunk": sidebar, inputs
  t.bg = bg;
  t.text = fg;
  t.sidebar = lt ? theme_mix(bg, 0x000000, 0.05f) : down(0.14f);
  t.panel = lt ? down(0.35f) : up(0.045f);
  t.bar = lt ? up(0.06f) : up(0.075f);
  t.row_alt = lt ? up(0.025f) : up(0.02f);
  t.hover = up(lt ? 0.11f : 0.10f);
  t.border = up(lt ? 0.19f : 0.15f);
  t.input = lt ? down(0.55f) : down(0.14f);
  t.tooltip = lt ? t.bar : t.border;
  t.muted = theme_mix(fg, bg, 0.48f);
  t.accent = accent;
  t.selected = theme_mix(bg, accent, lt ? 0.42f : 0.40f);
  t.selected_dim = theme_mix(bg, accent, lt ? 0.24f : 0.20f);
  t.red = red;
  t.folder = yellow;
  u32 match = theme_mix(yellow, red, 0.45f);
  if (mx_max(theme_luma(match), theme_luma(bg)) - mx_min(theme_luma(match), theme_luma(bg)) < 0.2f) match = magenta;
  t.match = match;
  if (light) *light = lt;
}
