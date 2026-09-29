#include "platform/xkb/keysym.h"
#include <stdio.h>
#include <string.h>

struct KeysymUnicode { u32 a, b; };
#include "platform/xkb/keysym_table.inc"

static int cmp_name(Str s, const char* z) {
  for (u32 i = 0; i < s.n; i++) {
    u8 c = (u8)z[i];
    if (c == 0) return 1;
    if ((u8)s.p[i] != c) return (u8)s.p[i] < c ? -1 : 1;
  }
  return z[s.n] ? -1 : 0;
}

static bool parse_hex(Str s, u32& out) {
  if (s.n == 0 || s.n > 8) return false;
  u32 v = 0;
  for (u32 i = 0; i < s.n; i++) {
    char c = s.p[i];
    u32  d;
    if (c >= '0' && c <= '9') d = (u32)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
    else return false;
    v = (v << 4) | d;
  }
  out = v;
  return true;
}

bool keysym_from_name(Str name, u32& out) {
  if (name.n == 0) return false;
  u32 lo = 0, hi = MX_ARRAY_COUNT(kKeysymNameOff);
  while (lo < hi) {
    u32 mid = (lo + hi) / 2;
    int c   = cmp_name(name, kKeysymNamePool + kKeysymNameOff[mid]);
    if (c == 0) { out = kKeysymNameVal[mid]; return true; }
    if (c < 0) hi = mid; else lo = mid + 1;
  }
  u32 v;
  if (name.n > 2 && name.p[0] == '0' && (name.p[1] == 'x' || name.p[1] == 'X') && parse_hex(name.sub(2, name.n - 2), v)) {
    out = v;
    return true;
  }
  if (name.p[0] == 'U') {
    u32 off = (name.n > 1 && name.p[1] == '+') ? 2 : 1;
    if (name.n > off && parse_hex(name.sub(off, name.n - off), v)) {
      if (v < 0x20 || (v > 0x7e && v < 0xa0) || v > 0x10ffff) return false;
      out = v < 0x100 ? v : (XKB_KEYSYM_UNICODE | v);
      return true;
    }
  }
  return false;
}

const char* keysym_name(u32 ks) {
  u32 lo = 0, hi = MX_ARRAY_COUNT(kKeysymByValue);
  while (lo < hi) {
    u32 mid = (lo + hi) / 2;
    u32 v   = kKeysymNameVal[kKeysymByValue[mid]];
    if (v == ks) return kKeysymNamePool + kKeysymNameOff[kKeysymByValue[mid]];
    if (ks < v) hi = mid; else lo = mid + 1;
  }
  return nullptr;
}

const char* keysym_format(u32 ks, char* buf, u32 cap) {
  const char* n = keysym_name(ks);
  if (n) snprintf(buf, cap, "%s", n);
  else if ((ks & 0xff000000u) == XKB_KEYSYM_UNICODE) snprintf(buf, cap, "U+%04X", ks & 0xffffff);
  else snprintf(buf, cap, "0x%x", ks);
  return buf;
}

static u32 table_lookup(const KeysymUnicode* t, u32 n, u32 key) {
  u32 lo = 0, hi = n;
  while (lo < hi) {
    u32 mid = (lo + hi) / 2;
    if (t[mid].a == key) return t[mid].b;
    if (key < t[mid].a) hi = mid; else lo = mid + 1;
  }
  return 0;
}

u32 keysym_to_utf32(u32 ks) {
  if ((ks >= 0x20 && ks <= 0x7e) || (ks >= 0xa0 && ks <= 0xff)) return ks;
  if (ks == XKB_KEY_KP_Space) return ' ';
  if ((ks >= XKB_KEY_BackSpace && ks <= XKB_KEY_Clear) || (ks >= XKB_KEY_KP_Multiply && ks <= XKB_KEY_KP_9) ||
      ks == XKB_KEY_Return || ks == XKB_KEY_Escape || ks == XKB_KEY_Delete || ks == XKB_KEY_KP_Tab ||
      ks == XKB_KEY_KP_Enter || ks == XKB_KEY_KP_Equal)
    return ks & 0x7f;
  if (ks >= XKB_KEYSYM_UNICODE && ks <= XKB_KEYSYM_UNICODE + 0x10ffff) {
    u32 cp = ks - XKB_KEYSYM_UNICODE;
    return (cp >= 0xd800 && cp <= 0xdfff) ? 0 : cp;
  }
  if (ks >= 0x10081200 && ks <= 0x10081209) return ks - 0x10081200 + '0'; // XF86Numeric0..9
  if (ks == 0x1008120a) return '*';
  if (ks == 0x1008120b) return '#';
  if (ks < 0x100 || ks > 0x20ac) return 0;
  return table_lookup(kKeysymUnicode, MX_ARRAY_COUNT(kKeysymUnicode), ks);
}

u32 utf32_to_keysym(u32 cp) {
  if ((cp >= 0x20 && cp <= 0x7e) || (cp >= 0xa0 && cp <= 0xff)) return cp;
  if ((cp >= 0x08 && cp <= 0x0b) || cp == 0x0d || cp == 0x1b) return cp | 0xff00;
  if (cp == 0x7f) return XKB_KEY_Delete;
  if (cp == 0 || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return XKB_KEY_NoSymbol;
  u32 ks = table_lookup(kUnicodeKeysym, MX_ARRAY_COUNT(kUnicodeKeysym), cp);
  return ks ? ks : (XKB_KEYSYM_UNICODE | cp);
}

u32 utf8_encode(u32 cp, char out[4]) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) { out[0] = (char)(0xc0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
  if (cp < 0x10000) {
    out[0] = (char)(0xe0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
  }
  out[0] = (char)(0xf0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[3] = (char)(0x80 | (cp & 0x3f));
  return 4;
}

u32 keysym_to_utf8(u32 ks, char out[4]) {
  u32 cp = keysym_to_utf32(ks);
  return cp ? utf8_encode(cp, out) : 0;
}

static bool pair_even_upper(u32 c, u32 lo, u32 hi) { return c >= lo && c <= hi; }

static u32 uc_upper(u32 c) {
  if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
  if (c < 0x100) {
    if (c == 0xb5) return 0x39c;
    if (c == 0xdf) return 0x1e9e; // capital sharp s, as libX11 and xkeyboard-config intend
    if (c == 0xff) return 0x178;
    return (c >= 0xe0 && c <= 0xfe && c != 0xf7) ? c - 0x20 : c;
  }
  if (c < 0x180) {
    if (c == 0x131) return 'I';
    if (c == 0x17f) return 'S';
    if (pair_even_upper(c, 0x100, 0x137) || pair_even_upper(c, 0x14a, 0x177)) return (c & 1) ? c - 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) return (c & 1) ? c : c - 1;
    return c;
  }
  if (c < 0x250) {
    if (c == 0x180) return 0x243;
    if (c == 0x188 || c == 0x18c || c == 0x192 || c == 0x199 || c == 0x1a1 || c == 0x1a3 || c == 0x1a5 ||
        c == 0x1a8 || c == 0x1ad || c == 0x1b0 || c == 0x1b4 || c == 0x1b6 || c == 0x1b9 || c == 0x1bd)
      return c - 1;
    if (c == 0x1c6 || c == 0x1c9 || c == 0x1cc) return c - 2;
    if (c == 0x1c5 || c == 0x1c8 || c == 0x1cb || c == 0x1f2) return c - 1;
    if (c == 0x1f3) return 0x1f1;
    if (c >= 0x1cd && c <= 0x1dc) return (c & 1) ? c : c - 1;
    if (c == 0x1dd) return 0x18e;
    if (pair_even_upper(c, 0x1de, 0x1ef) || pair_even_upper(c, 0x1f8, 0x21f) || pair_even_upper(c, 0x222, 0x233) ||
        pair_even_upper(c, 0x246, 0x24f))
      return (c & 1) ? c - 1 : c;
    return c;
  }
  if (c >= 0x3ac && c <= 0x3fb) { // Greek
    if (c == 0x3ac) return 0x386;
    if (c >= 0x3ad && c <= 0x3af) return c - 0x25;
    if (c == 0x3c2) return 0x3a3;
    if (c >= 0x3b1 && c <= 0x3cb) return c - 0x20;
    if (c == 0x3cc) return 0x38c;
    if (c == 0x3cd || c == 0x3ce) return c - 0x3f;
    switch (c) { // symbol variants
      case 0x3d0: return 0x392; case 0x3d1: return 0x398; case 0x3d5: return 0x3a6; case 0x3d6: return 0x3a0;
      case 0x3f0: return 0x39a; case 0x3f1: return 0x3a1; case 0x3f2: return 0x3f9; case 0x3f5: return 0x395;
      case 0x3f8: return 0x3f7; case 0x3fb: return 0x3fa;
      default: break;
    }
    if (c >= 0x3d8 && c <= 0x3ef) return (c & 1) ? c - 1 : c; // archaic letters, even = upper
    return c;
  }
  if (c >= 0x430 && c <= 0x44f) return c - 0x20; // Cyrillic
  if (c >= 0x450 && c <= 0x45f) return c - 0x50;
  if (pair_even_upper(c, 0x460, 0x481) || pair_even_upper(c, 0x48a, 0x4bf) || pair_even_upper(c, 0x4d0, 0x52f))
    return (c & 1) ? c - 1 : c;
  if (c >= 0x4c1 && c <= 0x4ce) return (c & 1) ? c : c - 1;
  if (c == 0x4cf) return 0x4c0;
  if (c >= 0x561 && c <= 0x586) return c - 0x30; // Armenian
  if (c >= 0x2d00 && c <= 0x2d25) return c - 0x1c60; // Georgian Nuskhuri -> Asomtavruli
  if (c >= 0x10d0 && c <= 0x10fa) return c + 0xbc0; // Mkhedruli -> Mtavruli
  if (pair_even_upper(c, 0x1e00, 0x1e95) || pair_even_upper(c, 0x1ea0, 0x1eff)) return (c & 1) ? c - 1 : c;
  if (c == 0x1e9b) return 0x1e60;
  if (pair_even_upper(c, 0x2c80, 0x2ce3)) return (c & 1) ? c - 1 : c; // Coptic
  if (c >= 0xff41 && c <= 0xff5a) return c - 0x20; // fullwidth
  return c;
}

static u32 uc_lower(u32 c) {
  if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
  if (c < 0x100) return (c >= 0xc0 && c <= 0xde && c != 0xd7) ? c + 0x20 : c;
  if (c < 0x180) {
    if (c == 0x130) return 'i';
    if (c == 0x178) return 0xff;
    if (pair_even_upper(c, 0x100, 0x137) || pair_even_upper(c, 0x14a, 0x177)) return (c & 1) ? c : c + 1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) return (c & 1) ? c + 1 : c;
    return c;
  }
  if (c < 0x250) {
    if (c == 0x187 || c == 0x18b || c == 0x191 || c == 0x198 || c == 0x1a0 || c == 0x1a2 || c == 0x1a4 ||
        c == 0x1a7 || c == 0x1ac || c == 0x1af || c == 0x1b3 || c == 0x1b5 || c == 0x1b8 || c == 0x1bc)
      return c + 1;
    if (c == 0x1c4 || c == 0x1c7 || c == 0x1ca || c == 0x1f1) return c + 2;
    if (c == 0x1c5 || c == 0x1c8 || c == 0x1cb || c == 0x1f2) return c + 1;
    if (c == 0x18e) return 0x1dd;
    if (c == 0x243) return 0x180;
    if (c >= 0x1cd && c <= 0x1dc) return (c & 1) ? c + 1 : c;
    if (pair_even_upper(c, 0x1de, 0x1ef) || pair_even_upper(c, 0x1f8, 0x21f) || pair_even_upper(c, 0x222, 0x233) ||
        pair_even_upper(c, 0x246, 0x24f))
      return (c & 1) ? c : c + 1;
    return c;
  }
  if (c >= 0x386 && c <= 0x3fa) {
    if (c == 0x386) return 0x3ac;
    if (c >= 0x388 && c <= 0x38a) return c + 0x25;
    if (c == 0x38c) return 0x3cc;
    if (c == 0x38e || c == 0x38f) return c + 0x3f;
    if (c >= 0x391 && c <= 0x3ab && c != 0x3a2) return c + 0x20;
    if (c == 0x3f4) return 0x3b8;
    if (c == 0x3f7) return 0x3f8;
    if (c == 0x3f9) return 0x3f2;
    if (c == 0x3fa) return 0x3fb;
    if (c >= 0x3d8 && c <= 0x3ef) return (c & 1) ? c : c + 1;
    return c;
  }
  if (c >= 0x410 && c <= 0x42f) return c + 0x20;
  if (c >= 0x400 && c <= 0x40f) return c + 0x50;
  if (pair_even_upper(c, 0x460, 0x481) || pair_even_upper(c, 0x48a, 0x4bf) || pair_even_upper(c, 0x4d0, 0x52f))
    return (c & 1) ? c : c + 1;
  if (c >= 0x4c1 && c <= 0x4ce) return (c & 1) ? c + 1 : c;
  if (c == 0x4c0) return 0x4cf;
  if (c >= 0x531 && c <= 0x556) return c + 0x30;
  if (c >= 0x10a0 && c <= 0x10c5) return c + 0x1c60;
  if (c >= 0x1c90 && c <= 0x1cba) return c - 0xbc0;
  if (pair_even_upper(c, 0x1e00, 0x1e95) || pair_even_upper(c, 0x1ea0, 0x1eff)) return (c & 1) ? c : c + 1;
  if (c == 0x1e9e) return 0xdf;
  if (pair_even_upper(c, 0x2c80, 0x2ce3)) return (c & 1) ? c : c + 1;
  if (c >= 0xff21 && c <= 0xff3a) return c + 0x20;
  return c;
}

static u32 keysym_case(u32 ks, bool upper) {
  u32 cp = keysym_to_utf32(ks);
  if (cp == 0 || cp < 0x20) return ks;
  u32 mapped = upper ? uc_upper(cp) : uc_lower(cp);
  if (mapped == cp) return ks;
  if (ks >= XKB_KEYSYM_UNICODE) return XKB_KEYSYM_UNICODE | mapped;
  return utf32_to_keysym(mapped);
}

u32  keysym_to_upper(u32 ks) { return keysym_case(ks, true); }
u32  keysym_to_lower(u32 ks) { return keysym_case(ks, false); }
bool keysym_is_lower(u32 ks) { return keysym_to_lower(ks) == ks && keysym_to_upper(ks) != ks; }
bool keysym_is_upper(u32 ks) { return keysym_to_upper(ks) == ks && keysym_to_lower(ks) != ks; }
