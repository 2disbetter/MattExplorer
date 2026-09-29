#include "gfx/font_db.h"
#include "gfx/ttf.h"
#include "base/algo.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static i64 now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (i64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static u32 intern(FontDb& db, const char* s) {
  u32 off = db.strings.len;
  u32 n   = (u32)strlen(s);
  memcpy(db.strings.push_n(n + 1), s, n + 1);
  return off;
}

static bool has_font_ext(const char* name) {
  const char* dot = strrchr(name, '.');
  if (!dot) return false;
  Str e(dot + 1);
  return str_ieq(e, "ttf") || str_ieq(e, "otf") || str_ieq(e, "ttc") || str_ieq(e, "otc");
}

static void add_file(FontDb& db, const char* path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < 12 || st.st_size > 0x7FFFFFFF) { close(fd); return; }
  void* m = mmap(nullptr, (usize)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (m == MAP_FAILED) return;
  const u8* data = (const u8*)m;
  u32 size = (u32)st.st_size;
  u32 faces = ttf_face_count(data, size);
  if (faces) db.files++;
  u32 path_off = 0;
  for (u32 i = 0; i < faces && i < 64; i++) {
    TtfFace f;
    if (!ttf_parse(f, data, size, i, TTF_PARSE_INFO) || !f.family[0]) continue;
    if (!path_off) path_off = intern(db, path);
    FontEntry e;
    e.path = path_off; e.family = intern(db, f.family); e.style = intern(db, f.style[0] ? f.style : "Regular");
    e.index = (u16)i; e.weight = f.weight;
    e.italic = f.italic; e.fixed_pitch = f.fixed_pitch; e.has_outlines = f.has_outlines; e.cff = f.cff_len > 0;
    db.entries.push(e);
  }
  munmap(m, (usize)st.st_size);
}

static void scan_dir(FontDb& db, const char* dir, u32 depth) {
  if (depth > 6) return;
  DIR* d = opendir(dir);
  if (!d) return;
  char path[1024];
  while (struct dirent* de = readdir(d)) {
    const char* nm = de->d_name;
    if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
    if (snprintf(path, sizeof path, "%s/%s", dir, nm) >= (int)sizeof path) continue;
    u8 type = de->d_type;
    if (type == DT_UNKNOWN || type == DT_LNK) {
      struct stat st;
      if (stat(path, &st) != 0) continue;
      type = S_ISDIR(st.st_mode) ? DT_DIR : S_ISREG(st.st_mode) ? DT_REG : DT_UNKNOWN;
    }
    if (type == DT_DIR) scan_dir(db, path, depth + 1);
    else if (type == DT_REG && has_font_ext(nm)) add_file(db, path);
  }
  closedir(d);
}

void font_db_add_path(FontDb& db, const char* path) {
  struct stat st;
  if (stat(path, &st) != 0) return;
  if (S_ISDIR(st.st_mode)) scan_dir(db, path, 0);
  else if (S_ISREG(st.st_mode)) add_file(db, path);
}

static i32 fallback_score(const FontDb& db, const FontEntry& e) {
  if (!e.has_outlines) return -1;
  i32 s = 1000;
  s -= (i32)(e.weight > 400 ? e.weight - 400 : 400 - e.weight) / 10;
  if (e.italic) s -= 200;
  Str fam(db.strings.data + e.family);
  static const char* preferred[] = { "Noto Sans", "DejaVu Sans", "Liberation Sans", "Cantarell", "Inter",
                                     "Symbols Nerd Font", "Noto Sans Symbols", "Noto Sans Symbols 2",
                                     "Noto Sans CJK", "Noto Sans Math", "Unifont" };
  for (u32 i = 0; i < MX_ARRAY_COUNT(preferred); i++) {
    u32 n = (u32)strlen(preferred[i]);
    if (fam.n >= n && str_ieq(Str(fam.p, n), preferred[i])) { s += 300 - (i32)i * 10; break; }
  }
  if (fam.n >= 5 && str_ieq(Str(fam.p, 5), "Noto ")) s += 20; // any Noto script face
  const char* sty = db.strings.data + e.style;
  if (strstr(sty, "Condensed") || strstr(sty, "Mono") || strstr(fam.p, "Mono")) s -= 30;
  return s;
}

void font_db_scan(FontDb& db) {
  i64 t0 = now_ns();
  db.strings.clear(); db.entries.clear(); db.fallback_order.clear(); db.files = 0;
  db.strings.push(0); // offset 0 = ""
  const char* home = getenv("HOME");
  char path[1024];
  font_db_add_path(db, "/usr/share/fonts");
  font_db_add_path(db, "/usr/local/share/fonts");
  if (home) {
    snprintf(path, sizeof path, "%s/.local/share/fonts", home); font_db_add_path(db, path);
    snprintf(path, sizeof path, "%s/.fonts", home);             font_db_add_path(db, path);
  }
  if (const char* dirs = getenv("XDG_DATA_DIRS")) {
    const char* p = dirs;
    while (*p) {
      const char* q = strchr(p, ':');
      u32 n = q ? (u32)(q - p) : (u32)strlen(p);
      if (n && n < sizeof path - 8 && !(n == 14 && !strncmp(p, "/usr/share", 10))) { // /usr/share already done
        snprintf(path, sizeof path, "%.*s/fonts", (int)n, p);
        if (strcmp(path, "/usr/share/fonts") && strcmp(path, "/usr/local/share/fonts")) font_db_add_path(db, path);
      }
      if (!q) break;
      p = q + 1;
    }
  }

  Array<u32> keyed; // score << 16 | entry, sorted descending by score
  for (u32 i = 0; i < db.entries.len && i < 0xFFFF; i++) {
    i32 s = fallback_score(db, db.entries[i]);
    if (s >= 0) keyed.push(((u32)s << 16) | i);
  }
  Array<u32> tmp; tmp.resize(keyed.len / 2 + 1);
  auto greater = [](u32 a, u32 b) { return a > b; };
  merge_sort(keyed.data, tmp.data, keyed.len, greater);
  for (u32 k : keyed) db.fallback_order.push((u16)(k & 0xFFFF));
  db.scan_ns = now_ns() - t0;
  db.scanned = true;
}

static bool str_contains(Str hay, const char* needle) {
  u32 n = (u32)strlen(needle);
  for (u32 i = 0; i + n <= hay.n; i++) if (!memcmp(hay.p + i, needle, n)) return true;
  return false;
}

static bool family_eq(Str a, Str b) {
  u32 i = 0, j = 0;
  for (;;) {
    while (i < a.n && (a.p[i] == ' ' || a.p[i] == '-' || a.p[i] == '_')) i++;
    while (j < b.n && (b.p[j] == ' ' || b.p[j] == '-' || b.p[j] == '_')) j++;
    if (i == a.n || j == b.n) return i == a.n && j == b.n;
    if (ascii_lower(a.p[i]) != ascii_lower(b.p[j])) return false;
    i++; j++;
  }
}

static Str strip_suffix(Str s, const char* suffix) {
  u32 n = (u32)strlen(suffix);
  if (s.n > n && str_ieq(Str(s.p + s.n - n, n), suffix)) return Str(s.p, s.n - n);
  return s;
}

static i32 best_of_family(const FontDb& db, Str family, u16 weight, bool italic) {
  i32 best = -1, best_score = -1;
  for (u32 i = 0; i < db.entries.len; i++) {
    const FontEntry& e = db.entries[i];
    if (!family_eq(Str(db.strings.data + e.family), family)) continue;
    i32 s = 1000 - (i32)(e.weight > weight ? e.weight - weight : weight - e.weight);
    if ((e.italic != 0) != italic) s -= 500;
    if (!e.has_outlines) s -= 2000;

    const char* sty = db.strings.data + e.style;
    static const char* widths[] = { "Condensed", "Narrow", "Extended", "Expanded", "Compressed" };
    for (const char* w : widths) if (strstr(sty, w) && !str_contains(family, w)) { s -= 100; break; }
    if (s > best_score) { best_score = s; best = (i32)i; }
  }
  return best;
}

i32 font_db_match(const FontDb& db, Str family, u16 weight, bool italic) {
  i32 e = best_of_family(db, family, weight, italic);
  if (e >= 0) return e;

  Str base = family;
  base = strip_suffix(base, " Mono"); base = strip_suffix(base, " Propo");
  if (!str_eq(base, family)) {
    if ((e = best_of_family(db, base, weight, italic)) >= 0) return e;
  }
  char buf[128];
  snprintf(buf, sizeof buf, "%.*s Mono", (int)base.n, base.p);
  if ((e = best_of_family(db, buf, weight, italic)) >= 0) return e;
  snprintf(buf, sizeof buf, "%.*s Propo", (int)base.n, base.p);
  if ((e = best_of_family(db, buf, weight, italic)) >= 0) return e;
  base = strip_suffix(base, " Nerd Font"); base = strip_suffix(base, " NF");
  if (!str_eq(base, family) && (e = best_of_family(db, base, weight, italic)) >= 0) return e;

  for (u32 i = 0; i < db.entries.len; i++) {
    Str fam(db.strings.data + db.entries[i].family);
    if (fam.n > family.n && str_ieq(Str(fam.p, family.n), family) && db.entries[i].has_outlines) return (i32)i;
  }
  return -1;
}

i32 font_db_find_glyph(const FontDb& db, u32 cp, const i32* skip, u32 nskip) {
  for (u16 ei : db.fallback_order) {
    bool skipped = false;
    for (u32 k = 0; k < nskip; k++) if (skip[k] == (i32)ei) { skipped = true; break; }
    if (skipped) continue;
    const FontEntry& e = db.entries[ei];
    const char* path = db.strings.data + e.path;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 12) { close(fd); continue; }
    void* m = mmap(nullptr, (usize)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) continue;
    TtfFace f;
    bool hit = ttf_parse(f, (const u8*)m, (u32)st.st_size, e.index) && ttf_glyph_index(f, cp) != 0;
    munmap(m, (usize)st.st_size);
    if (hit) return (i32)ei;
  }
  return -1;
}

void font_db_parse_spec(Str spec, char* family, u32 cap, u16& weight, bool& italic) {
  weight = 400; italic = false;
  Str fam = spec, style;
  for (u32 i = 0; i < spec.n; i++) if (spec.p[i] == ':') { fam = Str(spec.p, i); style = Str(spec.p + i + 1, spec.n - i - 1); break; }

  static const struct { const char* word; u16 weight; bool italic; } words[] = {
    { "Thin", 100, false }, { "ExtraLight", 200, false }, { "Light", 300, false }, { "Regular", 400, false },
    { "Medium", 500, false }, { "SemiBold", 600, false }, { "Bold", 700, false }, { "ExtraBold", 800, false },
    { "Black", 900, false }, { "Italic", 0, true }, { "Oblique", 0, true },
  };
  auto apply = [&](Str s) {
    for (auto& w : words) {
      u32 n = (u32)strlen(w.word);
      for (u32 i = 0; i + n <= s.n; i++) {
        if (!str_ieq(Str(s.p + i, n), w.word)) continue;
        bool start = i == 0 || s.p[i - 1] == ' ' || s.p[i - 1] == '-';
        bool end   = i + n == s.n || s.p[i + n] == ' ' || s.p[i + n] == '-';
        if (!start || !end) continue;
        if (w.weight) weight = w.weight;
        if (w.italic) italic = true;
      }
    }
  };
  apply(style);

  for (;;) {
    while (fam.n && fam.p[fam.n - 1] == ' ') fam.n--;
    bool stripped = false;
    for (auto& w : words) {
      u32 n = (u32)strlen(w.word);
      if (fam.n > n + 1 && fam.p[fam.n - n - 1] == ' ' && str_ieq(Str(fam.p + fam.n - n, n), w.word)) {
        if (w.weight) weight = w.weight;
        if (w.italic) italic = true;
        fam.n -= n + 1; stripped = true; break;
      }
    }
    if (!stripped) break;
  }
  u32 n = mx_min(fam.n, cap - 1);
  memcpy(family, fam.p, n); family[n] = 0;
}

static bool line_value(const char* line, const char* key, char* out, u32 cap) {
  const char* p = strstr(line, key);
  if (!p) return false;
  p += strlen(key);
  while (*p == ' ' || *p == '\t' || *p == '=') p++;
  char quote = (*p == '"' || *p == '\'') ? *p++ : 0;
  u32 o = 0;
  while (*p && *p != '\n' && *p != '\r' && o + 1 < cap) {
    if (quote ? *p == quote : (*p == ':' || *p == ',' || *p == '#' || *p == '<')) break;
    out[o++] = *p++;
  }
  while (o && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;
  out[o] = 0;
  return o > 0;
}

static bool grep_config(const char* rel, const char* key, char* out, u32 cap, const char* after = nullptr) {
  const char* home = getenv("HOME");
  if (!home) return false;
  char path[512];
  snprintf(path, sizeof path, "%s/.config/%s", home, rel);
  FILE* f = fopen(path, "r");
  if (!f) return false;
  char line[512];
  bool armed = after == nullptr, found = false;
  while (!found && fgets(line, sizeof line, f)) {
    if (!armed) { if (strstr(line, after)) armed = true; else continue; }
    const char* s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#' || *s == ';') continue;
    found = line_value(s, key, out, cap);
  }
  fclose(f);
  return found;
}

bool font_db_omarchy_family(char* out, u32 cap) {
  if (grep_config("alacritty/alacritty.toml", "family", out, cap)) return true;
  if (grep_config("ghostty/config", "font-family", out, cap)) return true;
  if (grep_config("kitty/kitty.conf", "font_family", out, cap)) return true;
  if (grep_config("foot/foot.ini", "font=", out, cap)) return true;
  if (grep_config("fontconfig/fonts.conf", "<family>", out, cap, "<prefer>")) return true;
  return false;
}
