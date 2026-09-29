#include "core/desktop.h"
#include "base/algo.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static i64 now_ns() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (i64)ts.tv_sec * 1000000000LL + ts.tv_nsec; }

static u32 str_push(Array<char>& s, Str v) {
  u32 off = s.len;
  memcpy(s.push_n(v.n + 1), v.p, v.n);
  s.data[off + v.n] = 0;
  return off;
}

struct StrTable {
  Array<u32> slots; // offset + 1, 0 = empty
  u32 count = 0;
  static u32 hash(Str v) { u32 h = 2166136261u; for (u32 i = 0; i < v.n; i++) h = (h ^ (u8)v.p[i]) * 16777619u; return h; }
  u32 intern(Array<char>& s, Str v) {
    if (slots.len == 0 || count * 2 >= slots.len) grow(s);
    u32 m = slots.len - 1, i = hash(v) & m;
    for (;;) {
      u32 e = slots[i];
      if (!e) { u32 off = str_push(s, v); slots[i] = off + 1; count++; return off; }
      if (str_eq(Str(s.data + e - 1), v)) return e - 1;
      i = (i + 1) & m;
    }
  }
  void grow(Array<char>& s) {
    u32 n = slots.len ? slots.len * 2 : 1024;
    Array<u32> old = static_cast<Array<u32>&&>(slots);
    slots = Array<u32>(); slots.resize_zero(n);
    for (u32 e : old) if (e) { u32 i = hash(Str(s.data + e - 1)) & (n - 1); while (slots[i]) i = (i + 1) & (n - 1); slots[i] = e; }
  }
};

static bool read_file(const char* path, Array<char>& out, u32 max) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  out.clear();
  for (;;) {
    char b[16384];
    ssize_t n = read(fd, b, sizeof b);
    if (n < 0) { if (errno == EINTR) continue; break; }
    if (n == 0) break;
    if (out.len + (u32)n > max) n = (ssize_t)(max - out.len);
    memcpy(out.push_n((u32)n), b, (u32)n);
    if (out.len >= max) break;
  }
  close(fd);
  return true;
}

static Str next_line(Str text, u32& pos) {
  u32 start = pos;
  while (pos < text.n && text.p[pos] != '\n') pos++;
  Str l(text.p + start, pos - start);
  if (pos < text.n) pos++;
  if (l.n && l.p[l.n - 1] == '\r') l.n--;
  return l;
}
static Str trim(Str s) {
  while (s.n && (s.p[0] == ' ' || s.p[0] == '\t')) { s.p++; s.n--; }
  while (s.n && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '\t')) s.n--;
  return s;
}
static bool starts_with(Str s, const char* p) { u32 n = (u32)strlen(p); return s.n >= n && !memcmp(s.p, p, n); }

static u32 xdg_data_dirs(const char* suffix, char (*out)[512], u32 cap) {
  u32 n = 0;
  const char* home = getenv("HOME");
  const char* xdh = getenv("XDG_DATA_HOME");
  if (xdh && xdh[0]) { if (n < cap) snprintf(out[n++], 512, "%s/%s", xdh, suffix); }
  else if (home) { if (n < cap) snprintf(out[n++], 512, "%s/.local/share/%s", home, suffix); }
  const char* dirs = getenv("XDG_DATA_DIRS");
  if (!dirs || !dirs[0]) dirs = "/usr/local/share:/usr/share";
  for (const char* p = dirs; *p && n < cap;) {
    const char* e = strchr(p, ':');
    u32 len = e ? (u32)(e - p) : (u32)strlen(p);
    if (len) { snprintf(out[n], 512, "%.*s/%s", (int)len, p, suffix); bool dup = false; for (u32 i = 0; i < n; i++) dup |= !strcmp(out[i], out[n]); if (!dup) n++; }
    if (!e) break;
    p = e + 1;
  }
  return n;
}

void mime_db_parse_globs(MimeDb& db, Str text) {
  StrTable table; // mime names deduplicated within this file
  for (u32 pos = 0; pos < text.n;) {
    Str l = next_line(text, pos);
    if (!l.n || l.p[0] == '#') continue;

    const char* c1 = (const char*)memchr(l.p, ':', l.n); if (!c1) continue;
    const char* c2 = (const char*)memchr(c1 + 1, ':', (u32)(l.p + l.n - c1 - 1)); if (!c2) continue;
    const char* c3 = (const char*)memchr(c2 + 1, ':', (u32)(l.p + l.n - c2 - 1));
    u32 weight = (u32)atoi(l.p);
    Str mime(c1 + 1, (u32)(c2 - c1 - 1));
    Str pat(c2 + 1, c3 ? (u32)(c3 - c2 - 1) : (u32)(l.p + l.n - c2 - 1));
    Str flags = c3 ? Str(c3 + 1, (u32)(l.p + l.n - c3 - 1)) : Str();
    if (!mime.n || !pat.n) continue;
    MimeGlob g = {};
    g.weight = (u16)mx_min(weight, 65535u);
    g.case_sensitive = flags.n && strstr(flags.p, "cs") != nullptr && (flags.n >= 2);
    g.mime = table.intern(db.strings, mime);
    bool wild = false;
    for (u32 i = 1; i < pat.n; i++) if (pat.p[i] == '*' || pat.p[i] == '?' || pat.p[i] == '[') wild = true;
    if (pat.p[0] == '*' && !wild) { g.kind = MG_SUFFIX; g.pattern = str_push(db.strings, Str(pat.p + 1, pat.n - 1)); }
    else if (pat.p[0] != '*' && pat.p[0] != '?' && pat.p[0] != '[' && !wild) { g.kind = MG_LITERAL; g.pattern = str_push(db.strings, pat); }
    else { g.kind = MG_PATTERN; g.pattern = str_push(db.strings, pat); }
    db.globs.push(g);
  }
}

void mime_db_parse_pairs(MimeDb& db, Str text, bool subclasses) {
  Array<u32>& out = subclasses ? db.parents : db.aliases;
  for (u32 pos = 0; pos < text.n;) {
    Str l = trim(next_line(text, pos));
    if (!l.n || l.p[0] == '#') continue;
    const char* sp = (const char*)memchr(l.p, ' ', l.n); if (!sp) continue;
    Str a(l.p, (u32)(sp - l.p)), b = trim(Str(sp + 1, (u32)(l.p + l.n - sp - 1)));
    if (!a.n || !b.n) continue;
    out.push(str_push(db.strings, a));
    out.push(str_push(db.strings, b));
  }
}

static bool glob_before(const MimeGlob& a, const MimeGlob& b, const Array<char>& s) {
  if (a.weight != b.weight) return a.weight > b.weight;
  u32 la = (u32)strlen(s.data + a.pattern), lb = (u32)strlen(s.data + b.pattern);
  if (la != lb) return la > lb;
  return a.pattern < b.pattern; // file order: what came first stays first
}

void mime_db_finish(MimeDb& db) {
  u32 n = db.globs.len;
  if (n >= 2) {
    Array<MimeGlob> tmp; tmp.resize(n / 2 + 1);
    const Array<char>& s = db.strings;
    auto less = [&](const MimeGlob& a, const MimeGlob& b) { return glob_before(a, b, s); };
    merge_sort(db.globs.data, tmp.data, n, less);
  }
  db.loaded = true;
}

bool mime_db_load(MimeDb& db) {
  i64 t0 = now_ns();
  db.strings.clear(); db.globs.clear(); db.parents.clear(); db.aliases.clear(); db.files = 0;
  char dirs[16][512];
  u32 nd = xdg_data_dirs("mime", dirs, 16);
  Array<char> text;
  char path[600];
  for (u32 i = 0; i < nd; i++) {
    snprintf(path, sizeof path, "%s/globs2", dirs[i]);
    if (read_file(path, text, 4u << 20)) { mime_db_parse_globs(db, Str(text.data, text.len)); db.files++; }
    snprintf(path, sizeof path, "%s/subclasses", dirs[i]);
    if (read_file(path, text, 1u << 20)) { mime_db_parse_pairs(db, Str(text.data, text.len), true); db.files++; }
    snprintf(path, sizeof path, "%s/aliases", dirs[i]);
    if (read_file(path, text, 1u << 20)) { mime_db_parse_pairs(db, Str(text.data, text.len), false); db.files++; }
  }
  mime_db_finish(db);
  db.scan_ns = now_ns() - t0;
  return db.globs.len > 0;
}

const char* mime_from_name(const MimeDb& db, Str name, bool is_dir) {
  if (is_dir) return "inode/directory";
  if (!name.n || name.n > 255) return "";
  char lower[256];
  for (u32 i = 0; i < name.n; i++) lower[i] = ascii_lower(name.p[i]);
  lower[name.n] = 0;
  char exact[256]; memcpy(exact, name.p, name.n); exact[name.n] = 0;
  const Array<char>& s = db.strings;
  for (const MimeGlob& g : db.globs) {
    const char* nm = g.case_sensitive ? exact : lower;
    const char* pat = s.data + g.pattern;
    bool hit = false;
    if (g.kind == MG_SUFFIX) { u32 pl = (u32)strlen(pat); hit = pl <= name.n && !memcmp(nm + name.n - pl, pat, pl); }
    else if (g.kind == MG_LITERAL) hit = !strcmp(nm, pat);
    else hit = fnmatch(pat, nm, 0) == 0;
    if (hit) return s.data + g.mime;
  }
  return "";
}

const char* mime_sniff(const u8* h, u32 n) {
  auto at = [&](u32 off, const char* m) { u32 l = (u32)strlen(m); return off + l <= n && !memcmp(h + off, m, l); };
  if (at(0, "\x89PNG\r\n\x1a\n")) return "image/png";
  if (n >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) return "image/jpeg";
  if (at(0, "GIF87a") || at(0, "GIF89a")) return "image/gif";
  if (at(0, "RIFF") && at(8, "WEBP")) return "image/webp";
  if (at(0, "RIFF") && at(8, "WAVE")) return "audio/x-wav";
  if (at(0, "%PDF-")) return "application/pdf";
  if (at(0, "\x7f" "ELF")) return n > 16 && h[16] == 3 ? "application/x-sharedlib" : "application/x-executable";
  if (n >= 2 && h[0] == 0x1f && h[1] == 0x8b) return "application/gzip";
  if (at(0, "PK\x03\x04")) return "application/zip";
  if (at(0, "BZh")) return "application/x-bzip2";
  if (at(0, "\xfd" "7zXZ")) return "application/x-xz";
  if (at(0, "7z\xbc\xaf\x27\x1c")) return "application/x-7z-compressed";
  if (at(0, "Rar!")) return "application/vnd.rar";
  if (at(0, "!<arch>")) return "application/x-archive";
  if (at(257, "ustar")) return "application/x-tar";
  if (at(0, "SQLite format 3")) return "application/vnd.sqlite3";
  if (at(0, "ID3") || (n >= 2 && h[0] == 0xFF && (h[1] & 0xE6) == 0xE2)) return "audio/mpeg";
  if (at(0, "OggS")) return "audio/ogg";
  if (at(0, "fLaC")) return "audio/flac";
  if (at(4, "ftyp")) return "video/mp4";
  if (n >= 4 && h[0] == 0x1a && h[1] == 0x45 && h[2] == 0xdf && h[3] == 0xa3) return "video/x-matroska";
  if (at(0, "{\\rtf")) return "text/rtf";
  if (at(0, "#!")) {
    char line[128]; u32 l = 0;
    for (u32 i = 2; i < n && l + 1 < sizeof line && h[i] != '\n'; i++) line[l++] = (char)h[i];
    line[l] = 0;
    if (strstr(line, "python")) return "text/x-python";
    if (strstr(line, "perl")) return "application/x-perl";
    if (strstr(line, "node")) return "application/javascript";
    if (strstr(line, "ruby")) return "application/x-ruby";
    return "application/x-shellscript";
  }
  if (at(0, "<?xml")) return "application/xml";
  if (at(0, "<!DOCTYPE html") || at(0, "<!doctype html") || at(0, "<html")) return "text/html";

  u32 ctl = 0;
  for (u32 i = 0; i < n; i++) { if (h[i] == 0) return "application/octet-stream"; if (h[i] < 0x20 && h[i] != '\n' && h[i] != '\r' && h[i] != '\t' && h[i] != '\f' && h[i] != 0x1b) ctl++; }
  return n && ctl * 20 < n + 20 ? "text/plain" : "application/octet-stream";
}

const char* mime_canonical(const MimeDb& db, const char* mime) {
  for (u32 i = 0; i + 1 < db.aliases.len; i += 2) if (!strcmp(db.s(db.aliases[i]), mime)) return db.s(db.aliases[i + 1]);
  return mime;
}

static u32 aliases_of(const MimeDb& db, const char* canonical, const char** out, u32 cap) {
  u32 n = 0;
  for (u32 i = 0; i + 1 < db.aliases.len && n < cap; i += 2) if (!strcmp(db.s(db.aliases[i + 1]), canonical)) out[n++] = db.s(db.aliases[i]);
  return n;
}

u32 mime_parents(const MimeDb& db, const char* mime, const char** out, u32 cap) {
  u32 n = 0;
  for (u32 i = 0; i + 1 < db.parents.len && n < cap; i += 2) if (!strcmp(db.s(db.parents[i]), mime)) out[n++] = db.s(db.parents[i + 1]);
  if (n < cap && !strncmp(mime, "text/", 5) && strcmp(mime, "text/plain")) {
    bool have = false; for (u32 i = 0; i < n; i++) have |= !strcmp(out[i], "text/plain");
    if (!have) out[n++] = "text/plain";
  }
  return n;
}

bool mime_is_a(const MimeDb& db, const char* mime, const char* parent) {
  const char* queue[64]; u32 head = 0, tail = 0;
  queue[tail++] = mime_canonical(db, mime);
  const char* want = mime_canonical(db, parent);
  while (head < tail) {
    const char* m = queue[head++];
    if (!strcmp(m, want)) return true;
    const char* ps[16];
    u32 np = mime_parents(db, m, ps, 16);
    for (u32 i = 0; i < np && tail < 64; i++) {
      bool seen = false; for (u32 k = 0; k < tail; k++) seen |= !strcmp(queue[k], ps[i]);
      if (!seen) queue[tail++] = ps[i];
    }
  }
  return false;
}

static bool key_is(Str k, const char* name) { return str_eq(k, name); }

bool app_db_parse_desktop(AppDb& db, Str text, const char* id, const char* path) {
  DesktopApp app = {};
  bool in_entry = false, seen_entry = false, is_app = false, hidden = false;
  Str name, exec, icon, mimes, tryexec;
  for (u32 pos = 0; pos < text.n;) {
    Str l = trim(next_line(text, pos));
    if (!l.n || l.p[0] == '#') continue;
    if (l.p[0] == '[') { in_entry = str_eq(l, "[Desktop Entry]"); if (in_entry) seen_entry = true; else if (seen_entry) break; continue; }
    if (!in_entry) continue;
    const char* eq = (const char*)memchr(l.p, '=', l.n); if (!eq) continue;
    Str k = trim(Str(l.p, (u32)(eq - l.p))), v = trim(Str(eq + 1, (u32)(l.p + l.n - eq - 1)));
    if (key_is(k, "Type")) is_app = str_eq(v, "Application");
    else if (key_is(k, "Name")) name = v;
    else if (key_is(k, "Exec")) exec = v;
    else if (key_is(k, "Icon")) icon = v;
    else if (key_is(k, "MimeType")) mimes = v;
    else if (key_is(k, "TryExec")) tryexec = v;
    else if (key_is(k, "NoDisplay")) app.no_display = str_eq(v, "true");
    else if (key_is(k, "Hidden")) hidden = str_eq(v, "true");
    else if (key_is(k, "Terminal")) app.terminal = str_eq(v, "true");
    else if (key_is(k, "Categories")) { char c[512]; snprintf(c, sizeof c, "%.*s", (int)mx_min(v.n, 511u), v.p); app.terminal_emulator = strstr(c, "TerminalEmulator") != nullptr; }
  }
  if (!seen_entry || !is_app || hidden || !exec.n) return false;
  if (tryexec.n) { char t[512]; snprintf(t, sizeof t, "%.*s", (int)mx_min(tryexec.n, 511u), tryexec.p); if (!command_exists(t)) return false; }
  app.id = str_push(db.strings, id);
  app.name = str_push(db.strings, name.n ? name : Str(id));
  app.exec = str_push(db.strings, exec);
  app.icon = str_push(db.strings, icon);
  app.file = str_push(db.strings, path);
  app.mimes = str_push(db.strings, mimes);
  for (u32 i = 0; i < mimes.n; i++) if (mimes.p[i] == ';') app.mime_count++;
  if (mimes.n && mimes.p[mimes.n - 1] != ';') app.mime_count++;
  db.apps.push(app);
  return true;
}

void app_db_parse_mimeapps(AppDb& db, Str text) {
  Array<u32>* section = nullptr;
  for (u32 pos = 0; pos < text.n;) {
    Str l = trim(next_line(text, pos));
    if (!l.n || l.p[0] == '#') continue;
    if (l.p[0] == '[') {
      section = str_eq(l, "[Default Applications]") ? &db.defaults : str_eq(l, "[Added Associations]") ? &db.added : str_eq(l, "[Removed Associations]") ? &db.removed : nullptr;
      continue;
    }
    if (!section) continue;
    const char* eq = (const char*)memchr(l.p, '=', l.n); if (!eq) continue;
    Str k = trim(Str(l.p, (u32)(eq - l.p))), v = trim(Str(eq + 1, (u32)(l.p + l.n - eq - 1)));
    if (!k.n) continue;
    u32 koff = str_push(db.strings, k);
    for (u32 i = 0; i < v.n;) { // app1;app2;
      u32 s = i; while (i < v.n && v.p[i] != ';') i++;
      Str a = trim(Str(v.p + s, i - s));
      if (a.n) { section->push(koff); section->push(str_push(db.strings, a)); }
      if (i < v.n) i++;
    }
  }
  db.lists++;
}

i32 app_find(const AppDb& db, const char* id) {
  for (u32 i = 0; i < db.apps.len; i++) if (!strcmp(db.s(db.apps[i].id), id)) return (i32)i;
  return -1;
}

static void scan_app_dir(AppDb& db, StrTable& ids, const char* dir, const char* prefix, u32 depth, Array<char>& text) {
  DIR* d = opendir(dir);
  if (!d) return;
  char path[1024], id[300];
  struct dirent* e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    u32 nl = (u32)strlen(e->d_name);
    bool desktop = nl > 8 && !strcmp(e->d_name + nl - 8, ".desktop");
    if (!desktop) {
      if (depth < 1) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) { char sub[300]; snprintf(sub, sizeof sub, "%s%s-", prefix, e->d_name); scan_app_dir(db, ids, path, sub, depth + 1, text); }
      }
      continue;
    }
    snprintf(id, sizeof id, "%s%s", prefix, e->d_name);
    u32 before = ids.count;
    ids.intern(db.strings, id);
    if (ids.count == before) continue; // an earlier (higher-priority) directory had this id
    db.files++;
    if (read_file(path, text, 256u << 10)) app_db_parse_desktop(db, Str(text.data, text.len), id, path);
  }
  closedir(d);
}

bool app_db_scan(AppDb& db) {
  i64 t0 = now_ns();
  db.strings.clear(); db.apps.clear(); db.defaults.clear(); db.added.clear(); db.removed.clear(); db.files = db.lists = 0;
  Array<char> text;
  char path[600];

  const char* home = getenv("HOME");
  const char* xch = getenv("XDG_CONFIG_HOME");
  char cfg[512]; cfg[0] = 0;
  if (xch && xch[0]) snprintf(cfg, sizeof cfg, "%s", xch); else if (home) snprintf(cfg, sizeof cfg, "%s/.config", home);
  const char* desktops = getenv("XDG_CURRENT_DESKTOP");
  auto read_lists = [&](const char* base) {
    if (desktops) for (const char* p = desktops; *p;) {
      const char* e = strchr(p, ':'); u32 len = e ? (u32)(e - p) : (u32)strlen(p);
      char dn[64]; u32 k = 0; for (u32 i = 0; i < len && k + 1 < sizeof dn; i++) dn[k++] = ascii_lower(p[i]); dn[k] = 0;
      snprintf(path, sizeof path, "%s/%s-mimeapps.list", base, dn);
      if (read_file(path, text, 1u << 20)) app_db_parse_mimeapps(db, Str(text.data, text.len));
      if (!e) break;
      p = e + 1;
    }
    snprintf(path, sizeof path, "%s/mimeapps.list", base);
    if (read_file(path, text, 1u << 20)) app_db_parse_mimeapps(db, Str(text.data, text.len));
  };
  if (cfg[0]) read_lists(cfg);
  { const char* xcd = getenv("XDG_CONFIG_DIRS"); if (!xcd || !xcd[0]) xcd = "/etc/xdg";
    for (const char* p = xcd; *p;) { const char* e = strchr(p, ':'); u32 len = e ? (u32)(e - p) : (u32)strlen(p); char b[512]; snprintf(b, sizeof b, "%.*s", (int)len, p); read_lists(b); if (!e) break; p = e + 1; } }
  char dirs[16][512];
  u32 nd = xdg_data_dirs("applications", dirs, 16);
  for (u32 i = 0; i < nd; i++) read_lists(dirs[i]);
  StrTable ids;
  for (u32 i = 0; i < nd; i++) scan_app_dir(db, ids, dirs[i], "", 0, text);
  db.loaded = true;
  db.scan_ns = now_ns() - t0;
  return db.apps.len > 0;
}

static bool app_handles(const AppDb& db, const DesktopApp& app, const char* const* types, u32 n) {
  const char* m = db.s(app.mimes);
  while (*m) {
    const char* e = strchr(m, ';');
    u32 len = e ? (u32)(e - m) : (u32)strlen(m);
    for (u32 i = 0; i < n; i++) if (strlen(types[i]) == len && !memcmp(types[i], m, len)) return true;
    if (!e) break;
    m = e + 1;
  }
  return false;
}

bool app_removed_for(const AppDb& db, u32 i, const char* mime) {
  const char* id = db.s(db.apps[i].id);
  for (u32 k = 0; k + 1 < db.removed.len; k += 2) if (!strcmp(db.s(db.removed[k]), mime) && !strcmp(db.s(db.removed[k + 1]), id)) return true;
  return false;
}

u32 apps_for_mime(const MimeDb& mdb, const AppDb& db, const char* mime, u32* out, u32 cap) {
  u32 n = 0;
  auto add = [&](i32 i) { if (i < 0 || n >= cap) return; for (u32 k = 0; k < n; k++) if (out[k] == (u32)i) return; out[n++] = (u32)i; };

  const char* types[32]; u32 nt = 0;
  types[nt++] = mime_canonical(mdb, mime);
  for (u32 head = 0; head < nt; head++) {
    const char* ps[16];
    u32 np = mime_parents(mdb, types[head], ps, 16);
    for (u32 i = 0; i < np && nt < 32; i++) { bool seen = false; for (u32 k = 0; k < nt; k++) seen |= !strcmp(types[k], ps[i]); if (!seen) types[nt++] = ps[i]; }
  }
  for (u32 ti = 0; ti < nt && n < cap; ti++) {
    const char* t = types[ti];
    const char* names[8]; u32 nn = 0;
    names[nn++] = t;
    nn += aliases_of(mdb, t, names + nn, 7);
    for (u32 k = 0; k + 1 < db.defaults.len; k += 2) if (!strcmp(db.s(db.defaults[k]), t)) { i32 i = app_find(db, db.s(db.defaults[k + 1])); if (i >= 0 && !app_removed_for(db, (u32)i, t)) add(i); }
    for (u32 k = 0; k + 1 < db.added.len; k += 2) if (!strcmp(db.s(db.added[k]), t)) { i32 i = app_find(db, db.s(db.added[k + 1])); if (i >= 0 && !app_removed_for(db, (u32)i, t)) add(i); }
    for (u32 i = 0; i < db.apps.len && n < cap; i++)
      if (!db.apps[i].no_display && app_handles(db, db.apps[i], names, nn) && !app_removed_for(db, i, t)) add((i32)i);
  }
  return n;
}

u32 path_to_uri(const char* path, char* out, u32 cap) {
  static const char hex[] = "0123456789ABCDEF";
  u32 o = 0;
  auto put = [&](char c) { if (o + 1 < cap) out[o++] = c; };
  const char* pre = "file://";
  for (const char* p = pre; *p; p++) put(*p);
  for (const u8* p = (const u8*)path; *p; p++) {
    u8 c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
    if (ok) put((char)c);
    else { put('%'); put(hex[c >> 4]); put(hex[c & 15]); }
  }
  if (cap) out[o] = 0;
  return o;
}

bool uri_to_path(Str uri, char* out, u32 cap) {
  Str u = trim(uri);
  if (!starts_with(u, "file:")) return false;
  u.p += 5; u.n -= 5;
  if (starts_with(u, "//")) { // file://host/path: only localhost or empty
    u.p += 2; u.n -= 2;
    const char* slash = (const char*)memchr(u.p, '/', u.n);
    if (!slash) return false;
    Str host(u.p, (u32)(slash - u.p));
    if (host.n && !str_eq(host, "localhost")) return false;
    u.n -= (u32)(slash - u.p); u.p = slash;
  }
  auto hv = [](char h) -> int { return h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1; };
  u32 o = 0;
  for (u32 i = 0; i < u.n; i++) {
    char c = u.p[i];
    if (c == '%' && i + 2 < u.n) { int x = hv(u.p[i + 1]), y = hv(u.p[i + 2]); if (x >= 0 && y >= 0) { c = (char)(x * 16 + y); i += 2; } }
    if (o + 1 >= cap) return false;
    out[o++] = c;
  }
  out[o] = 0;
  return o > 0 && out[0] == '/';
}

static bool exec_word(Str exec, u32& pos, char* out, u32 cap, u32* out_len) {
  while (pos < exec.n && exec.p[pos] == ' ') pos++;
  if (pos >= exec.n) return false;
  u32 o = 0;
  bool quoted = false;
  while (pos < exec.n) {
    char c = exec.p[pos];
    if (!quoted && c == ' ') break;
    if (c == '"') { quoted = !quoted; pos++; continue; }
    if (quoted && c == '\\' && pos + 1 < exec.n) { pos++; c = exec.p[pos]; }
    if (o + 1 < cap) out[o++] = c;
    pos++;
  }
  out[o] = 0;
  *out_len = o;
  return true;
}

bool desktop_argv(const AppDb& db, const DesktopApp& app, const char* const* paths, u32 n, Array<char>& buf, Array<char*>& argv, char* err, u32 ecap, const char* terminal) {
  Str exec(db.s(app.exec));
  buf.clear(); argv.clear();
  Array<u32> offs;
  auto push = [&](const char* s) { offs.push(buf.len); u32 l = (u32)strlen(s); memcpy(buf.push_n(l + 1), s, l + 1); };
  bool file_code = false;
  char word[4096], expanded[8192], uri[4096];
  for (u32 pos = 0; ;) {
    u32 wl;
    if (!exec_word(exec, pos, word, sizeof word, &wl)) break;

    if (!strcmp(word, "%F") || !strcmp(word, "%U")) {
      file_code = true;
      for (u32 i = 0; i < n; i++) { if (word[1] == 'U') { path_to_uri(paths[i], uri, sizeof uri); push(uri); } else push(paths[i]); }
      continue;
    }
    if (!strcmp(word, "%i")) { const char* ic = db.s(app.icon); if (ic[0]) { push("--icon"); push(ic); } continue; }
    u32 o = 0; bool drop = false;
    for (u32 i = 0; i < wl && o + 1 < sizeof expanded; i++) {
      if (word[i] != '%' || i + 1 >= wl) { expanded[o++] = word[i]; continue; }
      char code = word[++i];
      const char* ins = nullptr;
      switch (code) {
        case '%': ins = "%"; break;
        case 'f': file_code = true; if (n) ins = paths[0]; break;
        case 'u': file_code = true; if (n) { path_to_uri(paths[0], uri, sizeof uri); ins = uri; } break;
        case 'F': case 'U': file_code = true; if (n) { if (code == 'U') { path_to_uri(paths[0], uri, sizeof uri); ins = uri; } else ins = paths[0]; } break;
        case 'c': ins = db.s(app.name); break;
        case 'k': ins = db.s(app.file); break;
        case 'i': break; // only valid alone
        default: break; // deprecated codes: dropped
      }
      if (ins) { u32 l = (u32)strlen(ins); if (o + l + 1 >= sizeof expanded) { snprintf(err, ecap, "command line too long"); return false; } memcpy(expanded + o, ins, l); o += l; }
      else if ((code == 'f' || code == 'u' || code == 'F' || code == 'U') && wl == 2) drop = true; // "%f" alone with nothing to open
    }
    expanded[o] = 0;
    if (!drop) push(expanded);
  }
  if (!offs.len) { snprintf(err, ecap, "empty Exec line"); return false; }
  if (!file_code) for (u32 i = 0; i < n; i++) push(paths[i]);

  if (app.terminal) {
    char term[256];
    if (!terminal_find(terminal, term, sizeof term)) { snprintf(err, ecap, "no terminal emulator found for %s", db.s(app.name)); return false; }
    const char* pre[4]; u32 np = terminal_exec_prefix(term, pre, 4);
    Array<char> b2; Array<u32> o2;
    for (u32 i = 0; i < np; i++) { o2.push(b2.len); u32 l = (u32)strlen(pre[i]); memcpy(b2.push_n(l + 1), pre[i], l + 1); }
    for (u32 i = 0; i < offs.len; i++) { const char* s = buf.data + offs[i]; o2.push(b2.len); u32 l = (u32)strlen(s); memcpy(b2.push_n(l + 1), s, l + 1); }
    buf = static_cast<Array<char>&&>(b2);
    offs = static_cast<Array<u32>&&>(o2);
  }
  for (u32 i = 0; i < offs.len; i++) argv.push(buf.data + offs[i]);
  argv.push(nullptr);
  return true;
}

bool command_exists(const char* cmd) {
  if (!cmd || !cmd[0]) return false;
  if (strchr(cmd, '/')) return access(cmd, X_OK) == 0;
  const char* path = getenv("PATH");
  if (!path) path = "/usr/local/bin:/usr/bin:/bin";
  char full[1024];
  for (const char* p = path; *p;) {
    const char* e = strchr(p, ':');
    u32 len = e ? (u32)(e - p) : (u32)strlen(p);
    if (len) { snprintf(full, sizeof full, "%.*s/%s", (int)len, p, cmd); if (access(full, X_OK) == 0) return true; }
    if (!e) break;
    p = e + 1;
  }
  return false;
}

bool spawn_detached(const char* const* argv, const char* cwd, char* err, u32 ecap) {
  int p[2];
  if (pipe2(p, O_CLOEXEC) != 0) { snprintf(err, ecap, "pipe: %s", strerror(errno)); return false; }
  pid_t pid = fork();
  if (pid < 0) { snprintf(err, ecap, "fork: %s", strerror(errno)); close(p[0]); close(p[1]); return false; }
  if (pid == 0) {

    setsid();
    pid_t g = fork();
    if (g < 0) { int e = errno; if (write(p[1], &e, sizeof e) < 0) {} _exit(1); }
    if (g > 0) _exit(0);
    if (cwd && cwd[0] && chdir(cwd) != 0) {}
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0) { dup2(null, 0); if (null > 2) close(null); }
    execvp(argv[0], (char* const*)argv);
    int e = errno;
    if (write(p[1], &e, sizeof e) < 0) {}
    _exit(127);
  }
  close(p[1]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  int e = 0;
  ssize_t n;
  while ((n = read(p[0], &e, sizeof e)) < 0 && errno == EINTR) {}
  close(p[0]);
  if (n == (ssize_t)sizeof e) {
    if (e == ENOENT) snprintf(err, ecap, "%s: command not found", argv[0]);
    else snprintf(err, ecap, "%s: %s", argv[0], strerror(e));
    return false;
  }
  return true;
}

bool terminal_find(const char* preferred, char* out, u32 cap) {
  if (preferred && preferred[0] && command_exists(preferred)) { snprintf(out, cap, "%s", preferred); return true; }
  const char* env = getenv("TERMINAL");
  if (env && env[0] && command_exists(env)) { snprintf(out, cap, "%s", env); return true; }
  static const char* const known[] = { "alacritty", "ghostty", "kitty", "foot", "wezterm", "xterm" };
  for (const char* k : known) if (command_exists(k)) { snprintf(out, cap, "%s", k); return true; }
  return false;
}

static const char* base_name(const char* p) { const char* s = strrchr(p, '/'); return s ? s + 1 : p; }

u32 terminal_exec_prefix(const char* terminal, const char** out, u32 cap) {
  const char* b = base_name(terminal);
  u32 n = 0;
  if (cap) out[n++] = terminal;
  if (!strcmp(b, "wezterm")) { if (n < cap) out[n++] = "start"; if (n < cap) out[n++] = "--"; }
  else if (!strcmp(b, "foot") || !strcmp(b, "kitty")) {} // take the command directly
  else if (n < cap) out[n++] = "-e";
  return n;
}

bool terminal_open(const char* terminal, const char* dir, char* err, u32 ecap) {
  const char* argv[2] = { terminal, nullptr };
  return spawn_detached(argv, dir, err, ecap);
}
