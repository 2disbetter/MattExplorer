#pragma once

#include "base/types.h"
#include "base/array.h"
#include "base/str.h"

struct FontEntry {
  u32 path, family, style; // offsets into FontDb::strings (NUL-terminated)
  u16 index; // face index within the file
  u16 weight; // OS/2 usWeightClass (400 regular, 700 bold)
  u8  italic, fixed_pitch, has_outlines, cff;
};

struct FontDb {
  Array<char>      strings;
  Array<FontEntry> entries;
  Array<u16>       fallback_order; // entries sorted by how good a fallback they make
  u32  files = 0; // font files seen
  i64  scan_ns = 0;
  bool scanned = false;
};

void font_db_scan(FontDb& db);

void font_db_add_path(FontDb& db, const char* path);

i32 font_db_match(const FontDb& db, Str family, u16 weight, bool italic);

i32 font_db_find_glyph(const FontDb& db, u32 cp, const i32* skip, u32 nskip);

static inline const char* font_db_path(const FontDb& db, i32 e)   { return db.strings.data + db.entries[(u32)e].path; }
static inline const char* font_db_family(const FontDb& db, i32 e) { return db.strings.data + db.entries[(u32)e].family; }
static inline const char* font_db_style(const FontDb& db, i32 e)  { return db.strings.data + db.entries[(u32)e].style; }

bool font_db_omarchy_family(char* out, u32 cap);

void font_db_parse_spec(Str spec, char* family, u32 cap, u16& weight, bool& italic);
