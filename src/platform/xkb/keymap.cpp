#include "platform/xkb/keymap.h"
#include "platform/xkb/keysym.h"

#include <stdio.h>
#include <string.h>

enum TokKind : u8 { T_EOF, T_IDENT, T_KEYNAME, T_STRING, T_NUMBER, T_PUNCT };

struct Tok {
  u8   kind;
  char ch; // T_PUNCT
  Str  text;
  u32  num; // T_NUMBER
};

struct Lexer {
  const char* p;
  const char* end;
  Tok         la;
  bool        has_la = false;

  Lexer(const char* s, const char* e) : p(s), end(e) {}

  static bool ident_start(char c) { return is_upper(c) || is_lower(c) || c == '_'; }
  static bool ident_char(char c)  { return ident_start(c) || is_digit(c); }

  void skip_space() {
    for (;;) {
      while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
      if (p + 1 < end && p[0] == '/' && p[1] == '/') { while (p < end && *p != '\n') p++; continue; }
      if (p < end && *p == '#') { while (p < end && *p != '\n') p++; continue; }
      if (p + 1 < end && p[0] == '/' && p[1] == '*') {
        p += 2;
        while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
        p = mx_min(p + 2, end);
        continue;
      }
      return;
    }
  }

  Tok lex() {
    Tok t = {};
    skip_space();
    if (p >= end) { t.kind = T_EOF; return t; }
    char c = *p;
    if (ident_start(c)) {
      const char* s = p;
      while (p < end && ident_char(*p)) p++;
      t.kind = T_IDENT; t.text = Str(s, (u32)(p - s));
      return t;
    }
    if (is_digit(c)) {
      const char* s = p;
      u32 v = 0;
      if (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while (p < end) {
          char d = *p; u32 x;
          if (is_digit(d)) x = (u32)(d - '0');
          else if (d >= 'a' && d <= 'f') x = (u32)(d - 'a' + 10);
          else if (d >= 'A' && d <= 'F') x = (u32)(d - 'A' + 10);
          else break;
          v = (v << 4) | x; p++;
        }
      } else {
        while (p < end && is_digit(*p)) { v = v * 10 + (u32)(*p - '0'); p++; }
        while (p < end && ident_char(*p)) p++; // "1e3", "2nd": swallow, not a number we care about
      }
      t.kind = T_NUMBER; t.num = v; t.text = Str(s, (u32)(p - s));
      return t;
    }
    if (c == '<') {
      const char* s = ++p;
      while (p < end && *p != '>' && *p != '\n') p++;
      t.kind = T_KEYNAME; t.text = Str(s, (u32)(p - s));
      if (p < end && *p == '>') p++;
      return t;
    }
    if (c == '"') {
      const char* s = ++p;
      while (p < end && *p != '"') { if (*p == '\\' && p + 1 < end) p++; p++; }
      t.kind = T_STRING; t.text = Str(s, (u32)(p - s));
      if (p < end) p++;
      return t;
    }
    p++;
    t.kind = T_PUNCT; t.ch = c;
    return t;
  }

  Tok peek() { if (!has_la) { la = lex(); has_la = true; } return la; }
  Tok next() { Tok t = peek(); has_la = false; return t; }
  bool accept(char c)      { Tok t = peek(); if (t.kind == T_PUNCT && t.ch == c) { has_la = false; return true; } return false; }
  bool at(char c)          { Tok t = peek(); return t.kind == T_PUNCT && t.ch == c; }
  bool at_eof()            { return peek().kind == T_EOF; }

  void skip_until(char stop, char alt = 0) {
    int depth = 0;
    for (;;) {
      Tok t = peek();
      if (t.kind == T_EOF) return;
      if (t.kind == T_PUNCT) {
        if (depth == 0 && (t.ch == stop || t.ch == alt)) return;
        if (t.ch == '{' || t.ch == '[' || t.ch == '(') depth++;
        else if (t.ch == '}' || t.ch == ']' || t.ch == ')') { if (depth == 0) return; depth--; }
      }
      has_la = false;
    }
  }
  void skip_statement() { skip_until(';'); accept(';'); }
};

static bool ieq(Str a, Str b) {
  if (a.n != b.n) return false;
  for (u32 i = 0; i < a.n; i++) if (ascii_lower(a.p[i]) != ascii_lower(b.p[i])) return false;
  return true;
}
static bool ieq(Str a, const char* b) { return ieq(a, Str(b, (u32)strlen(b))); }

enum { MAX_VMODS = 24, VMOD_SHIFT = 8, MAX_GROUPS = 4, NO_KEY = 0xFFFF };
enum Match : u8 { M_NONE, M_ANY_OR_NONE, M_ANY, M_ALL, M_EXACTLY };

struct VMod       { Str name; u32 mapping; };
struct TypeEntryDef { u32 vmask; u32 preserve; u8 level; };
struct TypeDef    { Str name; u32 vmask; u16 first_entry, num_entries; u8 num_levels; };
struct Interp     { u32 sym; u8 match; u8 mods; i8 vmod; bool level1; bool repeat; };
struct GroupDef   { Str type; u16 first_sym, num_levels; bool explicit_actions; };
struct KeyName    { Str name; u16 code; };
struct KeyDef {
  u16  code;
  Str  type; // key-level type= (applies to groups without their own)
  u16  first_group;
  u8   num_groups;
  i8   repeat; // -1 unset
  u8   policy; // XKB_KEY_GROUPS_*
  u8   redirect;
  u8   modmap;
  bool explicit_vmodmap;
  bool repeats; // resolved
  u32  vmodmap; // vmask encoding (bits 8+)
};

struct Parser {
  XkbKeymap& km;
  Str        text;
  Array<KeyName>      key_names;
  Array<u16>          name_slots; // hash table -> index+1 into key_names
  Array<VMod>         vmods;
  Array<TypeDef>      types;
  Array<TypeEntryDef> entries;
  Array<Interp>       interps;
  Array<KeyDef>       keys;
  Array<GroupDef>     groups;
  Array<u32>          syms;
  Array<u16>          keydef_of_code;
  u32  max_code = 0;
  bool dflt_repeat = false, dflt_level1 = false;

  Parser(XkbKeymap& k, Str t) : km(k), text(t) {}

  void fail(const char* what, Lexer& lx) {
    u32 line = 1;
    for (const char* q = text.p; q < lx.p && q < text.p + text.n; q++) line += *q == '\n';
    snprintf(km.err, sizeof km.err, "keymap line %u: %s", line, what);
  }

  static u32 hash(Str s) { u32 h = 2166136261u; for (u32 i = 0; i < s.n; i++) { h ^= (u8)s.p[i]; h *= 16777619u; } return h; }
  void name_insert(Str name, u16 code) {
    if (key_names.len * 2 >= name_slots.len) return;
    u32 mask = name_slots.len - 1, i = hash(name) & mask;
    for (;;) {
      u16 e = name_slots[i];
      if (!e) { name_slots[i] = (u16)(key_names.len + 1); key_names.push({name, code}); return; }
      if (str_eq(key_names[e - 1].name, name)) { key_names[e - 1].code = code; return; }
      i = (i + 1) & mask;
    }
  }
  i32 name_lookup(Str name) {
    if (!name_slots.len) return -1;
    u32 mask = name_slots.len - 1, i = hash(name) & mask;
    for (;;) {
      u16 e = name_slots[i];
      if (!e) return -1;
      if (str_eq(key_names[e - 1].name, name)) return key_names[e - 1].code;
      i = (i + 1) & mask;
    }
  }

  i32 vmod_find(Str name) {
    for (u32 i = 0; i < vmods.len; i++) if (str_eq(vmods[i].name, name)) return (i32)i;
    for (u32 i = 0; i < vmods.len; i++) if (ieq(vmods[i].name, name)) return (i32)i;
    return -1;
  }
  i32 vmod_add(Str name) {
    i32 i = vmod_find(name);
    if (i >= 0) return i;
    if (vmods.len >= MAX_VMODS) return -1;
    vmods.push({name, 0});
    return (i32)vmods.len - 1;
  }
  static i32 real_mod(Str s) {
    if (ieq(s, "shift")) return 0;
    if (ieq(s, "lock")) return 1;
    if (ieq(s, "control") || ieq(s, "ctrl")) return 2;
    if (s.n == 4 && ieq(s.sub(0, 3), "mod") && s.p[3] >= '1' && s.p[3] <= '5') return 3 + (s.p[3] - '1');
    return -1;
  }

  u32 parse_mask(Lexer& lx, bool declare) {
    u32 m = 0;
    for (;;) {
      Tok t = lx.peek();
      if (t.kind == T_IDENT) {
        lx.next();
        if (ieq(t.text, "none")) {}
        else if (ieq(t.text, "all")) m |= 0xff;
        else {
          i32 r = real_mod(t.text);
          if (r >= 0) m |= 1u << r;
          else {
            i32 v = declare ? vmod_add(t.text) : vmod_find(t.text);
            if (v < 0 && !declare) v = vmod_add(t.text); // undeclared virtual modifier: tolerate
            if (v >= 0) m |= 1u << (VMOD_SHIFT + v);
          }
        }
      } else if (t.kind == T_NUMBER) { lx.next(); m |= t.num & 0xff; }
      else break;
      if (!lx.accept('+') && !lx.accept('|')) break;
    }
    return m;
  }
  u32 resolve_mask(u32 vmask) const {
    u32 m = vmask & 0xff;
    for (u32 i = 0; i < vmods.len; i++) if (vmask & (1u << (VMOD_SHIFT + i))) m |= vmods[i].mapping;
    return m & 0xff;
  }
  void parse_vmods_decl(Lexer& lx) { // after "virtual_modifiers": A, B = Mod1, C;
    for (;;) {
      Tok t = lx.next();
      if (t.kind != T_IDENT) break;
      i32 v = vmod_add(t.text);
      if (lx.accept('=')) { u32 m = parse_mask(lx, false); if (v >= 0) vmods[v].mapping |= m & 0xff; }
      if (!lx.accept(',')) break;
    }
    lx.skip_statement();
  }

  u32 parse_keysym_token(Tok t) {
    u32 ks;
    if (t.kind == T_NUMBER) {
      if (keysym_from_name(t.text, ks)) return ks;
      return t.num < 10 ? '0' + t.num : t.num;
    }
    if (t.kind != T_IDENT) return XKB_KEY_NoSymbol;
    if (str_eq(t.text, "NoSymbol")) return XKB_KEY_NoSymbol;
    if (keysym_from_name(t.text, ks)) return ks;
    km.unknown_syms++;
    return XKB_KEY_VoidSymbol;
  }

  u32 parse_symlist(Lexer& lx) {
    u32 n = 0;
    if (!lx.accept('[')) return 0;
    while (!lx.at(']') && !lx.at_eof()) {
      u32 ks = XKB_KEY_NoSymbol;
      if (lx.accept('{')) {
        while (!lx.at('}') && !lx.at_eof()) {
          u32 k = parse_keysym_token(lx.next());
          if (ks == XKB_KEY_NoSymbol) ks = k;
          lx.accept(',');
        }
        lx.accept('}');
      } else {
        ks = parse_keysym_token(lx.next());
      }
      syms.push(ks);
      n++;
      if (!lx.accept(',')) break;
    }
    lx.accept(']');
    return n;
  }

  i32 parse_group_index(Lexer& lx) {
    if (!lx.accept('[')) return -1;
    Tok t = lx.next();
    i32 g = -1;
    if (t.kind == T_NUMBER) g = (i32)t.num - 1;
    else if (t.kind == T_IDENT && t.text.n > 5 && ieq(t.text.sub(0, 5), "group")) {
      g = 0;
      for (u32 i = 5; i < t.text.n; i++) g = g * 10 + (t.text.p[i] - '0');
      g--;
    }
    lx.skip_until(']'); lx.accept(']');
    return g;
  }
  static i32 parse_bool(Lexer& lx) {
    Tok t = lx.next();
    if (t.kind == T_NUMBER) return t.num != 0;
    if (t.kind != T_IDENT) return -1;
    if (ieq(t.text, "yes") || ieq(t.text, "true") || ieq(t.text, "on")) return 1;
    if (ieq(t.text, "no") || ieq(t.text, "false") || ieq(t.text, "off")) return 0;
    return -1;
  }

  bool parse_keycodes(Lexer& lx) {
    struct Alias { Str from, to; };
    Array<Alias> aliases;
    while (!lx.at_eof()) {
      Tok t = lx.next();
      if (t.kind == T_KEYNAME) {
        if (!lx.accept('=')) { lx.skip_statement(); continue; }
        Tok n = lx.next();
        if (n.kind == T_NUMBER && n.num >= 8 && n.num < 0x10000) {
          name_insert(t.text, (u16)(n.num - 8));
          max_code = mx_max(max_code, n.num - 8);
        }
        lx.skip_statement();
      } else if (t.kind == T_IDENT && ieq(t.text, "alias")) {
        Tok a = lx.next();
        if (a.kind == T_KEYNAME && lx.accept('=')) {
          Tok b = lx.next();
          if (b.kind == T_KEYNAME) aliases.push({a.text, b.text});
        }
        lx.skip_statement();
      } else {
        lx.skip_statement(); // minimum, maximum, indicator, virtual indicator, include
      }
    }
    for (u32 pass = 0; pass < 4; pass++) // alias chains
      for (Alias& a : aliases) {
        i32 c = name_lookup(a.to);
        if (c >= 0 && name_lookup(a.from) < 0) name_insert(a.from, (u16)c);
      }
    return true;
  }

  bool parse_types(Lexer& lx) {
    while (!lx.at_eof()) {
      Tok t = lx.next();
      if (t.kind != T_IDENT) { lx.skip_statement(); continue; }
      if (ieq(t.text, "virtual_modifiers")) { parse_vmods_decl(lx); continue; }
      if (!ieq(t.text, "type")) { lx.skip_statement(); continue; }
      Tok name = lx.next();
      if (name.kind != T_STRING || !lx.accept('{')) { fail("bad type", lx); return false; }
      if (entries.len > 60000 || types.len > 60000) { fail("keymap too large", lx); return false; }
      TypeDef td = { name.text, 0, (u16)entries.len, 0, 1 };
      while (!lx.at('}') && !lx.at_eof()) {
        Tok f = lx.next();
        if (f.kind != T_IDENT) { lx.skip_statement(); continue; }
        if (ieq(f.text, "modifiers") || ieq(f.text, "mods")) {
          lx.accept('=');
          td.vmask = parse_mask(lx, false);
        } else if (ieq(f.text, "map") || ieq(f.text, "preserve")) {
          bool preserve = ieq(f.text, "preserve");
          if (!lx.accept('[')) { lx.skip_statement(); continue; }
          u32 vmask = parse_mask(lx, false);
          lx.skip_until(']'); lx.accept(']'); lx.accept('=');
          u32 idx = NO_KEY;
          for (u32 i = td.first_entry; i < entries.len; i++) if (entries[i].vmask == vmask) { idx = i; break; }
          if (idx == NO_KEY) { entries.push({vmask, 0, 0}); idx = entries.len - 1; }
          if (preserve) {
            entries[idx].preserve = parse_mask(lx, false);
          } else {
            Tok l = lx.next();
            u32 level = 0;
            if (l.kind == T_NUMBER) level = l.num;
            else if (l.kind == T_IDENT && l.text.n > 5 && ieq(l.text.sub(0, 5), "level"))
              for (u32 i = 5; i < l.text.n; i++) level = level * 10 + (u32)(l.text.p[i] - '0');
            level = level ? level - 1 : 0;
            entries[idx].level = (u8)mx_min(level, 254u);
            td.num_levels = (u8)mx_max((u32)td.num_levels, mx_min(level + 1, 255u));
          }
        } else if (ieq(f.text, "level_name")) {
          if (lx.accept('[')) {
            Tok l = lx.next();
            u32 level = 0;
            if (l.kind == T_NUMBER) level = l.num;
            else if (l.kind == T_IDENT && l.text.n > 5) for (u32 i = 5; i < l.text.n; i++) level = level * 10 + (u32)(l.text.p[i] - '0');
            td.num_levels = (u8)mx_max((u32)td.num_levels, mx_min(level, 255u));
          }
        }
        lx.skip_statement();
      }
      lx.accept('}'); lx.accept(';');
      td.num_entries = (u16)(entries.len - td.first_entry);
      types.push(td);
    }
    return true;
  }

  bool parse_compat(Lexer& lx) {
    while (!lx.at_eof()) {
      Tok t = lx.next();
      if (t.kind != T_IDENT) { lx.skip_statement(); continue; }
      if (ieq(t.text, "virtual_modifiers")) { parse_vmods_decl(lx); continue; }
      if (!ieq(t.text, "interpret")) { lx.skip_statement(); continue; } // indicator, group, include
      if (lx.accept('.')) {
        Tok f = lx.next();
        lx.accept('=');
        if (f.kind == T_IDENT && ieq(f.text, "repeat")) { i32 b = parse_bool(lx); if (b >= 0) dflt_repeat = b; }
        else if (f.kind == T_IDENT && (ieq(f.text, "useModMapMods") || ieq(f.text, "useModMap"))) {
          Tok v = lx.next();
          dflt_level1 = v.kind == T_IDENT && ieq(v.text, "level1");
        }
        lx.skip_statement();
        continue;
      }
      Interp it = { 0, M_ANY_OR_NONE, 0xff, -1, dflt_level1, dflt_repeat };
      Tok s = lx.next();
      if (!(s.kind == T_IDENT && ieq(s.text, "any"))) it.sym = parse_keysym_token(s);
      if (lx.accept('+')) {
        Tok m = lx.peek();
        it.match = M_EXACTLY;
        if (m.kind == T_IDENT && ieq(m.text, "any")) { lx.next(); it.match = M_ANY; it.mods = 0xff; }
        else if (m.kind == T_IDENT && (ieq(m.text, "NoneOf") || ieq(m.text, "AnyOfOrNone") || ieq(m.text, "AnyOf") ||
                                       ieq(m.text, "AllOf") || ieq(m.text, "Exactly"))) {
          lx.next();
          it.match = ieq(m.text, "NoneOf") ? M_NONE : ieq(m.text, "AnyOfOrNone") ? M_ANY_OR_NONE :
                     ieq(m.text, "AnyOf") ? M_ANY : ieq(m.text, "AllOf") ? M_ALL : M_EXACTLY;
          lx.accept('(');
          it.mods = (u8)resolve_mask(parse_mask(lx, false));
          lx.skip_until(')'); lx.accept(')');
        } else {
          it.mods = (u8)resolve_mask(parse_mask(lx, false));
        }
      }
      if (!lx.accept('{')) { lx.skip_statement(); continue; }
      while (!lx.at('}') && !lx.at_eof()) {
        Tok f = lx.next();
        if (f.kind == T_IDENT) {
          lx.accept('=');
          if (ieq(f.text, "virtualModifier") || ieq(f.text, "virtualMod")) {
            Tok v = lx.next();
            if (v.kind == T_IDENT) { i32 idx = vmod_add(v.text); it.vmod = (i8)idx; }
          } else if (ieq(f.text, "useModMapMods") || ieq(f.text, "useModMap")) {
            Tok v = lx.next();
            it.level1 = v.kind == T_IDENT && ieq(v.text, "level1");
          } else if (ieq(f.text, "repeat")) {
            i32 b = parse_bool(lx);
            if (b >= 0) it.repeat = b;
          }
        }
        lx.skip_statement(); // action= ..., locking= ...
      }
      lx.accept('}'); lx.accept(';');
      interps.push(it);
    }
    return true;
  }

  KeyDef* key_for(u16 code) {
    if (code >= keydef_of_code.len) keydef_of_code.resize_zero(code + 1); // 0 = none, else index+1
    if (keydef_of_code[code]) return &keys[keydef_of_code[code] - 1];
    KeyDef k = {};
    k.code = code; k.repeat = -1;
    keys.push(k);
    keydef_of_code[code] = (u16)keys.len;
    return &keys.last();
  }

  bool parse_key(Lexer& lx, Tok name) {
    i32 code = name_lookup(name.text);
    if (!lx.accept('{')) { fail("bad key", lx); return false; }
    if (syms.len > 60000 || groups.len > 60000 || keys.len > 60000) { fail("keymap too large", lx); return false; }

    GroupDef gd[MAX_GROUPS] = {};
    u32      ngroups = 0, next_bare = 0;
    Str      key_type;
    i8       repeat = -1;
    u32      vmodmap = 0;
    bool     explicit_vmodmap = false;
    u8       policy = 0, redirect = 0;
    while (!lx.at('}') && !lx.at_eof()) {
      Tok f = lx.peek();
      if (f.kind == T_PUNCT && f.ch == '[') { // bare list: next group in order
        u32 first = syms.len, n = parse_symlist(lx);
        if (next_bare < MAX_GROUPS) { gd[next_bare].first_sym = (u16)first; gd[next_bare].num_levels = (u16)n; ngroups = mx_max(ngroups, next_bare + 1); }
        next_bare++;
      } else if (f.kind == T_IDENT) {
        lx.next();
        if (ieq(f.text, "type")) {
          i32 g = parse_group_index(lx);
          lx.accept('=');
          Tok v = lx.next();
          if (v.kind == T_STRING) { if (g < 0) key_type = v.text; else if (g < MAX_GROUPS) { gd[g].type = v.text; ngroups = mx_max(ngroups, (u32)g + 1); } }
        } else if (ieq(f.text, "symbols")) {
          i32 g = parse_group_index(lx);
          lx.accept('=');
          u32 first = syms.len, n = parse_symlist(lx);
          if (g >= 0 && g < MAX_GROUPS) { gd[g].first_sym = (u16)first; gd[g].num_levels = (u16)n; ngroups = mx_max(ngroups, (u32)g + 1); }
        } else if (ieq(f.text, "actions")) {
          i32 g = parse_group_index(lx);
          lx.accept('=');
          if (g >= 0 && g < MAX_GROUPS) { gd[g].explicit_actions = true; ngroups = mx_max(ngroups, (u32)g + 1); }
          lx.skip_until(',', '}');
        } else if (ieq(f.text, "repeat") || ieq(f.text, "repeats") || ieq(f.text, "repeating")) {
          lx.accept('=');
          i32 b = parse_bool(lx);
          if (b >= 0) repeat = (i8)b;
        } else if (ieq(f.text, "virtualMods") || ieq(f.text, "vmods")) {
          lx.accept('=');
          vmodmap = parse_mask(lx, false); explicit_vmodmap = true;
        } else if (ieq(f.text, "groupsClamp")) {
          policy = XKB_KEY_GROUPS_CLAMP;
          if (lx.accept('=')) { i32 b = parse_bool(lx); if (b == 0) policy = 0; }
        } else if (ieq(f.text, "groupsWrap")) {
          policy = 0;
          if (lx.accept('=')) parse_bool(lx);
        } else if (ieq(f.text, "groupsRedirect")) {
          policy = XKB_KEY_GROUPS_REDIRECT;
          if (lx.accept('=')) {
            Tok v = lx.next();
            u32 g = 1;
            if (v.kind == T_NUMBER) g = v.num;
            else if (v.kind == T_IDENT && v.text.n > 5) { g = 0; for (u32 i = 5; i < v.text.n; i++) g = g * 10 + (u32)(v.text.p[i] - '0'); }
            redirect = (u8)(g ? g - 1 : 0);
          }
        } else {
          lx.skip_until(',', '}'); // overlay1=, locking=, locks, ...
        }
      } else {
        lx.next();
      }
      if (!lx.accept(',')) break;
    }
    lx.skip_until('}'); lx.accept('}'); lx.accept(';');
    if (code < 0) return true; // unknown key name: ignore, like xkbcommon

    KeyDef* k = key_for((u16)code);
    k->first_group = (u16)groups.len;
    k->num_groups  = (u8)ngroups;
    for (u32 g = 0; g < ngroups; g++) groups.push(gd[g]);
    if (key_type.n) k->type = key_type;
    if (repeat >= 0) k->repeat = repeat;
    if (explicit_vmodmap) { k->vmodmap = vmodmap; k->explicit_vmodmap = true; }
    k->policy = policy; k->redirect = redirect;
    return true;
  }

  bool parse_symbols(Lexer& lx) {
    while (!lx.at_eof()) {
      Tok t = lx.next();
      if (t.kind != T_IDENT) { lx.skip_statement(); continue; }
      if (ieq(t.text, "key")) {
        Tok name = lx.next();
        if (name.kind != T_KEYNAME) { lx.skip_statement(); continue; }
        if (!parse_key(lx, name)) return false;
      } else if (ieq(t.text, "name")) {
        i32 g = parse_group_index(lx);
        lx.accept('=');
        Tok v = lx.next();
        if (v.kind == T_STRING && g >= 0 && g < MAX_GROUPS) km.group_name[g] = add_name(v.text);
        lx.skip_statement();
      } else if (ieq(t.text, "modifier_map") || ieq(t.text, "modmap")) {
        Tok m = lx.next();
        i32 r = m.kind == T_IDENT ? real_mod(m.text) : -1;
        if (r < 0 || !lx.accept('{')) { lx.skip_statement(); continue; }
        u8 bit = (u8)(1u << r);
        while (!lx.at('}') && !lx.at_eof()) {
          Tok e = lx.next();
          if (e.kind == T_KEYNAME) {
            i32 code = name_lookup(e.text);
            if (code >= 0) key_for((u16)code)->modmap |= bit;
          } else {
            u32 ks = parse_keysym_token(e); // by keysym: first key (in keycode order) producing it
            if (ks && ks != XKB_KEY_VoidSymbol) {
              i32 best = -1;
              for (KeyDef& k : keys)
                for (u32 g = 0; g < k.num_groups && best < 0; g++) {
                  GroupDef& gd = groups[k.first_group + g];
                  for (u32 l = 0; l < gd.num_levels; l++)
                    if (syms[gd.first_sym + l] == ks && (best < 0 || k.code < keys[best].code)) { best = (i32)(&k - keys.data); break; }
                }
              if (best >= 0) keys[best].modmap |= bit;
            }
          }
          lx.accept(',');
        }
        lx.accept('}'); lx.accept(';');
      } else {
        lx.skip_statement(); // include, augment, replace, key.type = ...
      }
    }
    return true;
  }

  u16 add_name(Str s) {
    u16 off = (u16)km.names.len;
    char* p = km.names.push_n(s.n + 1);
    memcpy(p, s.p, s.n);
    p[s.n] = 0;
    return off;
  }

  i32 find_interp(const KeyDef& k, u32 group, u32 level) {
    const GroupDef& g = groups[k.first_group + group];
    if (level >= g.num_levels) return -2;
    u32 sym = syms[g.first_sym + level];
    if (sym == XKB_KEY_NoSymbol) return -2;
    for (u32 i = 0; i < interps.len; i++) {
      const Interp& it = interps[i];
      if (it.sym != sym && it.sym != XKB_KEY_NoSymbol) continue;
      u32 mods = (it.level1 && level != 0) ? 0 : k.modmap;
      bool found = false;
      switch (it.match) {
        case M_NONE:        found = !(it.mods & mods); break;
        case M_ANY_OR_NONE: found = !mods || (it.mods & mods); break;
        case M_ANY:         found = (it.mods & mods) != 0; break;
        case M_ALL:         found = (it.mods & mods) == it.mods; break;
        default:            found = it.mods == mods; break;
      }
      if (found) return (i32)i;
    }
    return -1;
  }

  i32 type_by_name(Str name) {
    for (u32 i = 0; i < types.len; i++) if (str_eq(types[i].name, name)) return (i32)i;
    return -1;
  }
  const char* automatic_type(const GroupDef& g) {
    u32 n = g.num_levels;
    auto S = [&](u32 l) { return l < n ? syms[g.first_sym + l] : XKB_KEY_NoSymbol; };
    if (n <= 1) return "ONE_LEVEL";
    bool alpha = keysym_is_lower(S(0)) && keysym_is_upper(S(1));
    bool kp    = keysym_is_keypad(S(0)) || keysym_is_keypad(S(1));
    if (n == 2) return alpha ? "ALPHABETIC" : kp ? "KEYPAD" : "TWO_LEVEL";
    if (n <= 4) {
      if (alpha) return (keysym_is_lower(S(2)) && keysym_is_upper(S(3))) ? "FOUR_LEVEL_ALPHABETIC" : "FOUR_LEVEL_SEMIALPHABETIC";
      return kp ? "FOUR_LEVEL_KEYPAD" : "FOUR_LEVEL";
    }
    return nullptr;
  }
  u16 pick_type(const KeyDef& k, const GroupDef& g) {
    i32 t = -1;
    if (g.type.n) t = type_by_name(g.type);
    else if (k.type.n) t = type_by_name(k.type);
    else {
      const char* a = automatic_type(g);
      if (a) {
        t = type_by_name(a);
        if (t < 0) t = type_by_name(g.num_levels <= 2 ? "TWO_LEVEL" : "FOUR_LEVEL");
      }
    }
    return (u16)(t < 0 ? 0 : t);
  }

  bool resolve() {
    if (!types.len) { snprintf(km.err, sizeof km.err, "keymap has no types"); return false; }

    for (KeyDef& k : keys) {
      u32 vmodmap = 0;
      bool repeats = k.repeat == 1;
      for (u32 g = 0; g < k.num_groups; g++) {
        if (groups[k.first_group + g].explicit_actions) continue;
        for (u32 l = 0; l < groups[k.first_group + g].num_levels; l++) {
          i32 i = find_interp(k, g, l);
          if (i == -2) continue;
          bool rep = i < 0 ? true : interps[i].repeat;
          if (g == 0 && l == 0 && k.repeat < 0 && rep) repeats = true;
          if (i >= 0 && ((g == 0 && l == 0) || !interps[i].level1) && interps[i].vmod >= 0)
            vmodmap |= 1u << (VMOD_SHIFT + interps[i].vmod);
        }
      }
      if (!k.explicit_vmodmap) k.vmodmap = vmodmap;
      k.repeats = repeats;
      for (u32 v = 0; v < vmods.len; v++) if (k.vmodmap & (1u << (VMOD_SHIFT + v))) vmods[v].mapping |= k.modmap;
    }

    for (TypeDef& td : types) {
      XkbType t = {};
      t.mask = (u8)resolve_mask(td.vmask);
      t.num_levels = td.num_levels;
      t.name = add_name(td.name);
      t.first_entry = (u16)km.type_entries.len;
      for (u32 i = td.first_entry; i < (u32)td.first_entry + td.num_entries; i++) {
        const TypeEntryDef& e = entries[i];
        u8 mask = (u8)resolve_mask(e.vmask);
        if (e.vmask && !mask) continue;
        km.type_entries.push({mask, e.level, (u8)resolve_mask(e.preserve), 0});
      }
      t.num_entries = (u16)(km.type_entries.len - t.first_entry);
      km.types.push(t);
    }

    km.key_of_code.resize(max_code + 1);
    for (u32 i = 0; i <= max_code; i++) km.key_of_code[i] = NO_KEY;
    km.num_groups = 0;
    for (KeyDef& k : keys) {
      XkbKey out = {};
      out.code = k.code; out.num_groups = k.num_groups; out.modmap = k.modmap;
      out.flags = (u8)(k.policy | (k.repeats ? XKB_KEY_REPEATS : 0));
      out.redirect_group = k.redirect;
      out.first_group = (u16)km.groups.len;
      for (u32 g = 0; g < k.num_groups; g++) {
        const GroupDef& gd = groups[k.first_group + g];
        XkbGroup og = { pick_type(k, gd), (u16)km.syms.len, gd.num_levels, 0 };
        for (u32 l = 0; l < gd.num_levels; l++) km.syms.push(syms[gd.first_sym + l]);
        km.groups.push(og);
      }
      km.num_groups = mx_max(km.num_groups, (u32)k.num_groups);
      if (k.code <= max_code) km.key_of_code[k.code] = (u16)km.keys.len;
      km.keys.push(out);
    }

    auto vm = [&](const char* name) { i32 v = vmod_find(name); return v < 0 ? 0u : vmods[v].mapping; };
    km.mask_alt = vm("Alt"); km.mask_super = vm("Super"); km.mask_meta = vm("Meta"); km.mask_hyper = vm("Hyper");
    km.mask_level3 = vm("LevelThree"); if (!km.mask_level3) km.mask_level3 = vm("AltGr");
    km.mask_level5 = vm("LevelFive"); km.mask_numlock = vm("NumLock"); km.mask_scroll = vm("ScrollLock");
    return true;
  }
};

bool keymap_parse(XkbKeymap& km, Str text) {
  km.keys.clear(); km.groups.clear(); km.syms.clear(); km.types.clear(); km.type_entries.clear();
  km.key_of_code.clear(); km.names.clear();
  km.num_groups = 0; km.unknown_syms = 0; km.ok = false; km.err[0] = 0;
  km.mask_alt = km.mask_super = km.mask_meta = km.mask_hyper = 0;
  km.mask_level3 = km.mask_level5 = km.mask_numlock = km.mask_scroll = 0;
  km.names.push(0);
  for (u32 g = 0; g < MAX_GROUPS; g++) km.group_name[g] = 0;

  Parser P(km, text);
  P.name_slots.resize_zero(2048); // > 2x the keys any keymap has; names are <= ~700

  struct Span { const char* s; const char* e; };
  Span kc = {}, ty = {}, co = {}, sy = {};
  {
    Lexer lx(text.p, text.p + text.n);
    while (!lx.at_eof()) {
      Tok t = lx.next();
      if (t.kind != T_IDENT) continue;
      if (ieq(t.text, "xkb_keymap")) { lx.accept('{'); continue; }
      Span* dst = nullptr;
      if (ieq(t.text, "xkb_keycodes")) dst = &kc;
      else if (ieq(t.text, "xkb_types")) dst = &ty;
      else if (ieq(t.text, "xkb_compatibility") || ieq(t.text, "xkb_compat")) dst = &co;
      else if (ieq(t.text, "xkb_symbols")) dst = &sy;
      else if (ieq(t.text, "xkb_geometry") || ieq(t.text, "xkb_semantics") || ieq(t.text, "xkb_layout")) dst = nullptr;
      else continue;
      while (!lx.at('{') && !lx.at_eof()) lx.next(); // optional flags and "name"
      if (!lx.accept('{')) break;
      const char* s = lx.p;
      lx.has_la = false;
      lx.skip_until('}');
      const char* e = (lx.has_la && lx.la.kind == T_PUNCT) ? lx.p - 1 : lx.p;
      lx.accept('}'); lx.accept(';');
      if (dst) { dst->s = s; dst->e = e; }
    }
  }
  if (!kc.s || !ty.s || !sy.s) { snprintf(km.err, sizeof km.err, "keymap lacks %s", !kc.s ? "xkb_keycodes" : !ty.s ? "xkb_types" : "xkb_symbols"); return false; }

  { Lexer lx(kc.s, kc.e); if (!P.parse_keycodes(lx)) return false; }
  { Lexer lx(ty.s, ty.e); if (!P.parse_types(lx)) return false; }
  if (co.s) { Lexer lx(co.s, co.e); if (!P.parse_compat(lx)) return false; }
  { Lexer lx(sy.s, sy.e); if (!P.parse_symbols(lx)) return false; }
  if (!P.resolve()) return false;
  km.ok = true;
  return true;
}

static const XkbKey* key_for_code(const XkbKeymap& km, u32 code) {
  if (!km.ok || code >= km.key_of_code.len) return nullptr;
  u16 i = km.key_of_code[code];
  return i == NO_KEY ? nullptr : &km.keys[i];
}

u32 keymap_key_sym(const XkbKeymap& km, u32 evdev_code, u32 mods, u32 group, u32* consumed) {
  if (consumed) *consumed = 0;
  const XkbKey* k = key_for_code(km, evdev_code);
  if (!k || k->num_groups == 0) return XKB_KEY_NoSymbol;
  if (group >= k->num_groups) {
    if (k->flags & XKB_KEY_GROUPS_REDIRECT) group = k->redirect_group < k->num_groups ? k->redirect_group : 0;
    else if (k->flags & XKB_KEY_GROUPS_CLAMP) group = k->num_groups - 1u;
    else group %= k->num_groups;
  }
  const XkbGroup& g = km.groups[k->first_group + group];
  const XkbType&  t = km.types[g.type];
  u32 active = mods & t.mask, level = 0, preserve = 0;
  for (u32 i = t.first_entry; i < (u32)t.first_entry + t.num_entries; i++) {
    const XkbTypeEntry& e = km.type_entries[i];
    if (e.mods == active) { level = e.level; preserve = e.preserve; break; }
  }
  u32 used = t.mask & ~preserve;
  if (consumed) *consumed = used;
  if (level >= g.num_levels) return XKB_KEY_NoSymbol;
  u32 ks = km.syms[g.first_sym + level];
  if (ks != XKB_KEY_NoSymbol && (mods & XKB_MOD_LOCK) && !(used & XKB_MOD_LOCK)) ks = keysym_to_upper(ks);
  return ks;
}

bool keymap_key_repeats(const XkbKeymap& km, u32 evdev_code) {
  const XkbKey* k = key_for_code(km, evdev_code);
  return k && (k->flags & XKB_KEY_REPEATS);
}
