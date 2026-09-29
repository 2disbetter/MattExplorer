#pragma once

#include "base/types.h"
#include "base/str.h"

enum : u32 {
  XKB_KEY_NoSymbol = 0, XKB_KEY_VoidSymbol = 0xffffff,
  XKB_KEY_BackSpace = 0xff08, XKB_KEY_Tab = 0xff09, XKB_KEY_Linefeed = 0xff0a, XKB_KEY_Clear = 0xff0b,
  XKB_KEY_Return = 0xff0d, XKB_KEY_Pause = 0xff13, XKB_KEY_Escape = 0xff1b, XKB_KEY_Delete = 0xffff,
  XKB_KEY_Multi_key = 0xff20,
  XKB_KEY_Home = 0xff50, XKB_KEY_Left = 0xff51, XKB_KEY_Up = 0xff52, XKB_KEY_Right = 0xff53,
  XKB_KEY_Down = 0xff54, XKB_KEY_Page_Up = 0xff55, XKB_KEY_Page_Down = 0xff56, XKB_KEY_End = 0xff57,
  XKB_KEY_Insert = 0xff63, XKB_KEY_Menu = 0xff67, XKB_KEY_Mode_switch = 0xff7e, XKB_KEY_Num_Lock = 0xff7f,
  XKB_KEY_KP_Space = 0xff80, XKB_KEY_KP_Tab = 0xff89, XKB_KEY_KP_Enter = 0xff8d, XKB_KEY_KP_Delete = 0xff9f,
  XKB_KEY_KP_Home = 0xff95, XKB_KEY_KP_Left = 0xff96, XKB_KEY_KP_Up = 0xff97, XKB_KEY_KP_Right = 0xff98,
  XKB_KEY_KP_Down = 0xff99, XKB_KEY_KP_Page_Up = 0xff9a, XKB_KEY_KP_Page_Down = 0xff9b, XKB_KEY_KP_End = 0xff9c,
  XKB_KEY_KP_Multiply = 0xffaa, XKB_KEY_KP_Add = 0xffab, XKB_KEY_KP_Subtract = 0xffad,
  XKB_KEY_KP_0 = 0xffb0, XKB_KEY_KP_9 = 0xffb9, XKB_KEY_KP_Equal = 0xffbd,
  XKB_KEY_F1 = 0xffbe, XKB_KEY_F2 = 0xffbf, XKB_KEY_F4 = 0xffc1, XKB_KEY_F5 = 0xffc2, XKB_KEY_F10 = 0xffc7, XKB_KEY_F12 = 0xffc9,
  XKB_KEY_space = 0x20,
  XKB_KEY_Shift_L = 0xffe1, XKB_KEY_Shift_R = 0xffe2, XKB_KEY_Control_L = 0xffe3, XKB_KEY_Control_R = 0xffe4,
  XKB_KEY_Caps_Lock = 0xffe5, XKB_KEY_Shift_Lock = 0xffe6, XKB_KEY_Meta_L = 0xffe7, XKB_KEY_Meta_R = 0xffe8, XKB_KEY_Alt_L = 0xffe9, XKB_KEY_Alt_R = 0xffea,
  XKB_KEY_Super_L = 0xffeb, XKB_KEY_Hyper_R = 0xffee,
  XKB_KEY_ISO_Lock = 0xfe01, XKB_KEY_ISO_Level3_Shift = 0xfe03, XKB_KEY_ISO_Next_Group = 0xfe08,
  XKB_KEY_ISO_Level5_Shift = 0xfe11, XKB_KEY_ISO_Level5_Lock = 0xfe13, XKB_KEY_ISO_Left_Tab = 0xfe20,
  XKB_KEY_dead_grave = 0xfe50, XKB_KEY_dead_greek = 0xfe8c, XKB_KEY_dead_hamza = 0xfe8d,
  XKB_KEY_XF86HomePage = 0x1008ff18, XKB_KEY_XF86Search = 0x1008ff1b, XKB_KEY_XF86Back = 0x1008ff26,
  XKB_KEY_XF86Forward = 0x1008ff27, XKB_KEY_XF86Copy = 0x1008ff57, XKB_KEY_XF86Cut = 0x1008ff58,
  XKB_KEY_XF86Explorer = 0x1008ff5d, XKB_KEY_XF86Paste = 0x1008ff6d,
  XKB_KEYSYM_UNICODE = 0x1000000,
};

bool keysym_from_name(Str name, u32& out);

const char* keysym_name(u32 ks);
const char* keysym_format(u32 ks, char* buf, u32 cap);

u32 keysym_to_utf32(u32 ks); // 0 if the keysym has no character
u32 keysym_to_utf8(u32 ks, char out[4]); // bytes written, 0 if none
u32 utf32_to_keysym(u32 cp);
u32 utf8_encode(u32 cp, char out[4]);

u32  keysym_to_upper(u32 ks);
u32  keysym_to_lower(u32 ks);
bool keysym_is_lower(u32 ks);
bool keysym_is_upper(u32 ks);

static inline bool keysym_is_keypad(u32 ks) { return ks >= XKB_KEY_KP_Space && ks <= XKB_KEY_KP_Equal; }
static inline bool keysym_is_dead(u32 ks)   { return ks >= XKB_KEY_dead_grave && ks <= 0xfe93; }
static inline bool keysym_is_modifier(u32 ks) {
  return (ks >= XKB_KEY_Shift_L && ks <= XKB_KEY_Hyper_R) || (ks >= XKB_KEY_ISO_Lock && ks <= XKB_KEY_ISO_Level5_Lock) ||
         ks == XKB_KEY_Mode_switch || ks == XKB_KEY_Num_Lock;
}
