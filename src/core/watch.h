#pragma once

#include "base/types.h"
#include "base/array.h"
#include "base/str.h"

enum { WATCH_MAX_NAMES = 48 };

struct DirWatch {
  int   wd = -1;
  u32   refs = 0; // tabs showing this directory
  char  path[4096] = {};
  bool  dirty = false, gone = false, overflow = false;
  i64   first_ms = 0, last_ms = 0; // when the dirty period started, and the last event
  u32   events = 0; // since the last refresh
  char  names[4096] = {};
  u32   names_len = 0, name_count = 0;
};

struct Watcher {
  int             fd = -1;
  Array<DirWatch> watches;
  u32             drained = 0; // stats
  u8              buf[16384];
};

bool watch_open(Watcher& w);
void watch_close(Watcher& w);

i32  watch_add(Watcher& w, const char* path);

void watch_release(Watcher& w, i32 index);

u32  watch_drain(Watcher& w, i64 now_ms);

i64  watch_due_ms(const DirWatch& d);

void watch_clear(DirWatch& d);

bool watch_name_stale(const DirWatch& d, Str name);
