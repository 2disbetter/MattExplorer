#pragma once

#include "base/types.h"
#include "base/array.h"
#include "base/str.h"

enum MimeGlobKind : u8 { MG_SUFFIX = 0, MG_LITERAL, MG_PATTERN };

struct MimeGlob {
  u32  mime; // offset into MimeDb::strings
  u32  pattern; // offset; MG_SUFFIX stores the part after "*" (".tar.gz")
  u16  weight;
  u8   kind;
  bool case_sensitive;
};

struct MimeDb {
  Array<char>    strings;
  Array<MimeGlob> globs; // by weight, then pattern length, descending
  Array<u32>     parents; // pairs (child, parent) from `subclasses`
  Array<u32>     aliases; // pairs (alias, canonical) from `aliases`
  u32            files = 0;
  i64            scan_ns = 0;
  bool           loaded = false;
  const char* s(u32 off) const { return strings.data + off; }
};

bool mime_db_load(MimeDb& db);

void mime_db_parse_globs(MimeDb& db, Str text);
void mime_db_parse_pairs(MimeDb& db, Str text, bool subclasses); // subclasses / aliases
void mime_db_finish(MimeDb& db); // sort the globs

const char* mime_from_name(const MimeDb& db, Str name, bool is_dir);

const char* mime_sniff(const u8* head, u32 n);

bool mime_is_a(const MimeDb& db, const char* mime, const char* parent);

const char* mime_canonical(const MimeDb& db, const char* mime);

u32  mime_parents(const MimeDb& db, const char* mime, const char** out, u32 cap);

struct DesktopApp {
  u32  id; // "org.gnome.Loupe.desktop" (offsets into AppDb::strings)
  u32  name; // "Image Viewer"
  u32  exec; // the Exec line, unexpanded
  u32  icon; // Icon=, "" if none
  u32  file; // the .desktop path
  u32  mimes;
  u32  mime_count;
  bool terminal; // Terminal=true: runs inside a terminal emulator
  bool no_display;
  bool terminal_emulator; // Categories has TerminalEmulator
};

struct AppDb {
  Array<char>       strings;
  Array<DesktopApp> apps;
  Array<u32>        defaults;
  Array<u32>        added; // pairs from [Added Associations]
  Array<u32>        removed; // pairs from [Removed Associations]
  u32               files = 0, lists = 0;
  i64               scan_ns = 0;
  bool              loaded = false;
  const char* s(u32 off) const { return strings.data + off; }
};

bool app_db_scan(AppDb& db);

bool app_db_parse_desktop(AppDb& db, Str text, const char* id, const char* path);

void app_db_parse_mimeapps(AppDb& db, Str text);
i32  app_find(const AppDb& db, const char* id); // index or -1

u32  apps_for_mime(const MimeDb& mdb, const AppDb& db, const char* mime, u32* out, u32 cap);

bool app_removed_for(const AppDb& db, u32 i, const char* mime);

bool desktop_argv(const AppDb& db, const DesktopApp& app, const char* const* paths, u32 n, Array<char>& buf, Array<char*>& argv, char* err, u32 ecap, const char* terminal = nullptr);

u32  path_to_uri(const char* path, char* out, u32 cap);

bool uri_to_path(Str uri, char* out, u32 cap);

bool spawn_detached(const char* const* argv, const char* cwd, char* err, u32 ecap);

bool terminal_find(const char* preferred, char* out, u32 cap);

bool terminal_open(const char* terminal, const char* dir, char* err, u32 ecap);

u32  terminal_exec_prefix(const char* terminal, const char** out, u32 cap);

bool command_exists(const char* cmd);
