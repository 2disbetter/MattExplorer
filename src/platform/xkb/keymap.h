#pragma once

#include "base/types.h"
#include "base/array.h"
#include "base/str.h"

enum : u32 {
  XKB_MOD_SHIFT = 1u << 0, XKB_MOD_LOCK = 1u << 1, XKB_MOD_CONTROL = 1u << 2, XKB_MOD_MOD1 = 1u << 3,
  XKB_MOD_MOD2 = 1u << 4, XKB_MOD_MOD3 = 1u << 5, XKB_MOD_MOD4 = 1u << 6, XKB_MOD_MOD5 = 1u << 7,
};

struct XkbTypeEntry { u8 mods; u8 level; u8 preserve; u8 _pad; }; // preserve: mods not consumed
struct XkbType {
  u8  mask;
  u8  num_levels;
  u16 first_entry, num_entries;
  u16 name; // offset into XkbKeymap::names
};
struct XkbGroup { u16 type; u16 first_sym; u16 num_levels; u16 _pad; };
enum : u8 { XKB_KEY_REPEATS = 1, XKB_KEY_GROUPS_CLAMP = 2, XKB_KEY_GROUPS_REDIRECT = 4 };
struct XkbKey {
  u16 first_group;
  u8  num_groups;
  u8  flags;
  u8  redirect_group;
  u8  modmap; // real modifiers this key sets (from modifier_map)
  u16 code; // evdev keycode
};

struct XkbKeymap {
  Array<XkbKey>       keys;
  Array<XkbGroup>     groups;
  Array<u32>          syms;
  Array<XkbType>      types;
  Array<XkbTypeEntry> type_entries;
  Array<u16>          key_of_code; // evdev keycode -> index into keys, 0xFFFF = none
  Array<char>         names; // NUL-terminated strings; offset 0 is ""
  u16 group_name[4] = {}; // offsets into names, 0 = unnamed
  u32 num_groups    = 0; // layouts (max over keys)

  u32 mask_alt = 0, mask_super = 0, mask_meta = 0, mask_hyper = 0;
  u32 mask_level3 = 0, mask_level5 = 0, mask_numlock = 0, mask_scroll = 0;
  u32  unknown_syms = 0; // keysym names not in our table, kept as VoidSymbol
  bool ok = false;
  char err[128] = {};
};

bool keymap_parse(XkbKeymap& km, Str text);

u32  keymap_key_sym(const XkbKeymap& km, u32 evdev_code, u32 mods, u32 group, u32* consumed = nullptr);
bool keymap_key_repeats(const XkbKeymap& km, u32 evdev_code);
static inline const char* keymap_group_name(const XkbKeymap& km, u32 group) {
  return group < 4 && km.names.len ? km.names.data + km.group_name[group] : "";
}
static inline const char* keymap_type_name(const XkbKeymap& km, const XkbType& t) { return km.names.data + t.name; }
