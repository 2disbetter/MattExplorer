#include "platform/xkb/compose.h"
#include "platform/xkb/keysym.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MAX_SEQ = 32, MAX_INCLUDE_DEPTH = 5 };
static const char* const kLocaleDir = "/usr/share/X11/locale";

static bool read_file(const char* path, Array<char>& out) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (64 << 20)) { close(fd); return false; }
  out.resize((u32)st.st_size);
  usize got = 0;
  while (got < (usize)st.st_size) {
    ssize_t n = read(fd, out.data + got, (usize)st.st_size - got);
    if (n <= 0) break;
    got += (usize)n;
  }
  close(fd);
  out.len = (u32)got;
  return got == (usize)st.st_size;
}

static bool file_exists(const char* path) {
  struct stat st;
  return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool registry_lookup(const char* file, bool left_to_right, Str name, char* out, u32 cap) {
  Array<char> buf;
  if (!read_file(file, buf)) return false;
  for (u32 pass = 0; pass < 2; pass++) {
    const char* p = buf.data;
    const char* end = buf.data + buf.len;
    while (p < end) {
      while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
      if (p < end && *p == '#') { while (p < end && *p != '\n') p++; continue; }
      const char* l = p;
      while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != ':') p++;
      Str left(l, (u32)(p - l));
      if (p < end && *p == ':') p++;
      while (p < end && (*p == ' ' || *p == '\t')) p++;
      const char* r = p;
      while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
      Str right(r, (u32)(p - r));
      while (p < end && *p != '\n') p++;
      if (!left.n || !right.n) continue;
      Str key = left_to_right ? left : right, val = left_to_right ? right : left;
      bool eq = key.n == name.n;
      for (u32 i = 0; eq && i < key.n; i++) eq = pass ? ascii_lower(key.p[i]) == ascii_lower(name.p[i]) : key.p[i] == name.p[i];
      if (eq) { snprintf(out, cap, "%.*s", (int)val.n, val.p); return true; }
    }
  }
  return false;
}

static void locale_compose_path(char* out, u32 cap) {
  const char* loc = getenv("LC_ALL");
  if (!loc || !*loc) loc = getenv("LC_CTYPE");
  if (!loc || !*loc) loc = getenv("LANG");
  if (!loc || !*loc || !strcmp(loc, "C") || !strcmp(loc, "POSIX")) loc = "en_US.UTF-8";
  char registry[300], resolved[128], file[200];
  snprintf(registry, sizeof registry, "%s/locale.alias", kLocaleDir);
  if (registry_lookup(registry, true, loc, resolved, sizeof resolved)) loc = resolved;
  snprintf(registry, sizeof registry, "%s/compose.dir", kLocaleDir);
  if (!registry_lookup(registry, false, loc, file, sizeof file)) snprintf(file, sizeof file, "en_US.UTF-8/Compose");
  if (file[0] == '/') snprintf(out, cap, "%s", file);
  else snprintf(out, cap, "%s/%s", kLocaleDir, file);
}

static void home_path(const char* rel, char* out, u32 cap) {
  const char* home = getenv("HOME");
  if (!home || !*home) { out[0] = 0; return; }
  snprintf(out, cap, "%s/%s", home, rel);
}

void compose_clear(ComposeTable& t) {
  t.nodes.clear(); t.utf8.clear(); t.sequences = 0; t.source[0] = 0;
}

static void ensure_init(ComposeTable& t) {
  if (t.nodes.len) return;
  t.nodes.push({}); // sentinel: index 0 means "none"
  t.utf8.push(0); // offset 0 means ""
}

static void insert(ComposeTable& t, const u32* seq, u32 n, const char* str, u32 len, u32 sym) {
  ensure_init(t);
  u32 curr = t.nodes.len == 1 ? 0 : 1;
  u32 parent = 0, which = 0; // which: 0 lo, 1 hi, 2 eq
  for (u32 i = 0;;) {
    u32  ks   = seq[i];
    bool last = i + 1 == n;
    if (curr == 0) {
      ComposeNode nn = { ks, 0, 0, 0, 0, 0 };
      curr = t.nodes.len;
      t.nodes.push(nn);
      if (parent) { ComposeNode& p = t.nodes[parent]; (which == 0 ? p.lo : which == 1 ? p.hi : p.eq) = curr; }
    }
    ComposeNode& nd = t.nodes[curr];
    bool leaf = nd.utf8 || nd.sym;
    if (ks < nd.keysym) { parent = curr; which = 0; curr = nd.lo; }
    else if (ks > nd.keysym) { parent = curr; which = 1; curr = nd.hi; }
    else if (!last) {
      if (leaf) { nd.utf8 = 0; nd.sym = 0; nd.eq = 0; } // shorter sequence existed: the new, longer one wins
      i++; parent = curr; which = 2; curr = nd.eq;
    } else {
      nd.eq = 0; // longer sequences existed: this one wins
      if (len) {
        nd.utf8 = t.utf8.len;
        char* p = t.utf8.push_n(len + 1);
        memcpy(p, str, len); p[len] = 0;
      } else {
        nd.utf8 = 0;
      }
      nd.sym = sym;
      if (!nd.utf8 && !nd.sym) nd.sym = XKB_KEY_VoidSymbol; // keep it a leaf
      t.sequences++;
      return;
    }
  }
}

struct Line { const char* p; const char* end; };

static void skip_ws(Line& l) { while (l.p < l.end && (*l.p == ' ' || *l.p == '\t')) l.p++; }
static bool at_word(Line& l, const char* w) {
  u32 n = (u32)strlen(w);
  return (u32)(l.end - l.p) >= n && memcmp(l.p, w, n) == 0 && (l.p + n == l.end || !(is_lower(l.p[n]) || is_upper(l.p[n]) || is_digit(l.p[n]) || l.p[n] == '_'));
}

static bool parse_string(Line& l, char* out, u32 cap, u32& len) {
  if (l.p >= l.end || *l.p != '"') return false;
  l.p++;
  len = 0;
  while (l.p < l.end && *l.p != '"') {
    char c = *l.p++;
    if (c == '\\' && l.p < l.end) {
      char e = *l.p++;
      if (e == '\\' || e == '"') c = e;
      else if (e == 'x' || e == 'X') {
        u32 v = 0, k = 0;
        while (k < 2 && l.p < l.end) {
          char d = *l.p; u32 x;
          if (is_digit(d)) x = (u32)(d - '0'); else if (d >= 'a' && d <= 'f') x = (u32)(d - 'a' + 10);
          else if (d >= 'A' && d <= 'F') x = (u32)(d - 'A' + 10); else break;
          v = v * 16 + x; l.p++; k++;
        }
        if (!k) continue;
        c = (char)v;
      } else if (e >= '0' && e <= '7') {
        u32 v = (u32)(e - '0'), k = 1;
        while (k < 3 && l.p < l.end && *l.p >= '0' && *l.p <= '7') { v = v * 8 + (u32)(*l.p - '0'); l.p++; k++; }
        c = (char)v;
      } else {
        continue; // unknown escape: ignore, like libxkbcommon
      }
    }
    if (len < cap) out[len++] = c;
  }
  if (l.p >= l.end) return false;
  l.p++;
  return true;
}

static void expand_include(const char* spec, u32 n, const char* locale_file, char* out, u32 cap) {
  u32 o = 0;
  for (u32 i = 0; i < n && o + 1 < cap; i++) {
    if (spec[i] == '%' && i + 1 < n) {
      char c = spec[++i];
      const char* rep = nullptr;
      char home[256];
      if (c == 'L') rep = locale_file;
      else if (c == 'S') rep = kLocaleDir;
      else if (c == 'H') { const char* h = getenv("HOME"); snprintf(home, sizeof home, "%s", h ? h : ""); rep = home; }
      else if (c == '%') rep = "%";
      if (rep) { u32 rn = (u32)strlen(rep); if (o + rn + 1 < cap) { memcpy(out + o, rep, rn); o += rn; } continue; }
      out[o++] = '%';
      out[o++] = c;
      continue;
    }
    out[o++] = spec[i];
  }
  out[o] = 0;
}

static void parse_into(ComposeTable& t, Str text, const char* locale_file, u32 depth);

static void parse_line(ComposeTable& t, Line l, const char* locale_file, u32 depth) {
  skip_ws(l);
  if (l.p >= l.end || *l.p == '#') return;
  if (at_word(l, "include")) {
    l.p += 7;
    skip_ws(l);
    char raw[512], path[512];
    u32  n;
    if (!parse_string(l, raw, sizeof raw, n) || depth >= MAX_INCLUDE_DEPTH) return;
    expand_include(raw, n, locale_file, path, sizeof path);
    Array<char> buf;
    if (read_file(path, buf)) parse_into(t, Str(buf.data, buf.len), locale_file, depth + 1);
    return;
  }
  u32 seq[MAX_SEQ], n = 0;
  for (;;) {
    skip_ws(l);
    if (l.p >= l.end) return;
    if (*l.p == ':') break;
    if (*l.p != '<') return;
    const char* s = ++l.p;
    while (l.p < l.end && *l.p != '>') l.p++;
    if (l.p >= l.end) return;
    u32 ks;
    if (!keysym_from_name(Str(s, (u32)(l.p - s)), ks)) return;
    l.p++;
    if (n < MAX_SEQ) seq[n++] = ks;
  }
  if (!n || n >= MAX_SEQ) return;
  l.p++; // ':'
  skip_ws(l);
  char str[64];
  u32  len = 0, sym = 0;
  if (l.p < l.end && *l.p == '"') {
    if (!parse_string(l, str, sizeof str, len)) return;
    skip_ws(l);
  }
  if (l.p < l.end && *l.p != '#') {
    const char* s = l.p;
    while (l.p < l.end && *l.p != ' ' && *l.p != '\t' && *l.p != '#') l.p++;
    u32 ks;
    if (keysym_from_name(Str(s, (u32)(l.p - s)), ks)) sym = ks;
  }
  if (!len && !sym) return;
  insert(t, seq, n, str, len, sym);
}

static void parse_into(ComposeTable& t, Str text, const char* locale_file, u32 depth) {
  const char* p = text.p;
  const char* end = text.p + text.n;
  while (p < end) {
    const char* e = p;
    while (e < end && *e != '\n') e++;
    const char* le = e;
    if (le > p && le[-1] == '\r') le--;
    parse_line(t, Line{p, le}, locale_file, depth);
    p = e + 1;
  }
}

void compose_parse(ComposeTable& t, Str text, const char* locale_file) {
  ensure_init(t);
  char lf[300];
  if (!locale_file) { locale_compose_path(lf, sizeof lf); locale_file = lf; }
  parse_into(t, text, locale_file, 0);
}

bool compose_load_file(ComposeTable& t, const char* path) {
  Array<char> buf;
  if (!read_file(path, buf)) return false;
  compose_parse(t, Str(buf.data, buf.len), nullptr);
  snprintf(t.source, sizeof t.source, "%s", path);
  return true;
}

bool compose_load_locale(ComposeTable& t) {
  compose_clear(t);
  char path[300];
  const char* env = getenv("XCOMPOSEFILE");
  if (env && *env && compose_load_file(t, env)) return true;
  const char* xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0] == '/') snprintf(path, sizeof path, "%s/XCompose", xdg); else home_path(".config/XCompose", path, sizeof path);
  if (file_exists(path) && compose_load_file(t, path)) return true;
  home_path(".XCompose", path, sizeof path);
  if (file_exists(path) && compose_load_file(t, path)) return true;
  locale_compose_path(path, sizeof path);
  return compose_load_file(t, path);
}

ComposeStatus compose_feed(const ComposeTable& t, ComposeState& s, u32 keysym) {
  if (compose_empty(t)) return COMPOSE_NOTHING;
  if (s.composed) { s.node = 0; s.composed = false; }
  bool at_root = s.node == 0;
  u32  curr    = at_root ? 1 : t.nodes[s.node].eq;
  while (curr) {
    const ComposeNode& nd = t.nodes[curr];
    if (keysym < nd.keysym) curr = nd.lo;
    else if (keysym > nd.keysym) curr = nd.hi;
    else break;
  }
  if (!curr) {
    if (at_root) return COMPOSE_NOTHING;
    s.node = 0;
    return COMPOSE_CANCELLED;
  }
  s.node = curr;
  const ComposeNode& nd = t.nodes[curr];
  if (nd.utf8 || nd.sym) { s.composed = true; return COMPOSE_COMPOSED; }
  return COMPOSE_COMPOSING;
}

const char* compose_result(const ComposeTable& t, const ComposeState& s, u32* sym) {
  if (!s.composed || !s.node) { if (sym) *sym = 0; return ""; }
  const ComposeNode& nd = t.nodes[s.node];
  if (sym) *sym = nd.sym == XKB_KEY_VoidSymbol ? 0 : nd.sym;
  return t.utf8.data + nd.utf8;
}
