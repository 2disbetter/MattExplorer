#pragma once

#include "base/types.h"
#include "base/str.h"
#include "ui/ui.h"

struct ThemeColors {
  u32  bg = 0, fg = 0;
  u32  ansi[16] = {}; // normal 0-7, bright 8-15
  u32  have_ansi = 0; // bitmask of the ones the file set
  u32  accent = 0;
  bool have_bg = false, have_fg = false, have_accent = false;
  bool light = false; // light.mode present, or a light background
  char name[64] = {}; // the theme directory's name
};

void theme_parse_alacritty(Str toml, ThemeColors& out);

void theme_parse_hyprland(Str conf, ThemeColors& out);

bool theme_load_dir(const char* dir, ThemeColors& out);

bool theme_omarchy_dir(char* out, u32 cap);

bool theme_omarchy_watch_dir(char* out, u32 cap);

void theme_derive(const ThemeColors& c, UiTheme& out, bool* light);

u32  theme_mix(u32 a, u32 b, float t); // a*(1-t) + b*t per channel, 0xRRGGBB
float theme_luma(u32 c); // 0..1
