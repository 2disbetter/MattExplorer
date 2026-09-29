#pragma once

#include "base/types.h"
#include "base/array.h"
#include "base/str.h"

enum EntryKind : u8 { EK_UNKNOWN = 0, EK_FILE, EK_DIR, EK_SYMLINK, EK_OTHER };

enum EntryFlags : u8 {
  EF_HIDDEN      = 1 << 0, // name starts with '.'
  EF_STATTED     = 1 << 1, // size/mtime/mode are valid
  EF_STAT_FAILED = 1 << 2, // statx returned an error (e.g. entry vanished)
  EF_LINK_TO_DIR = 1 << 3, // symlink whose target is a directory (set during stat)
  EF_STAT_PENDING = 1 << 4, // a statx batch for this entry is on the worker pool
  EF_CUT         = 1 << 5, // marked for a cut (drawn faded until pasted)
};

struct DirListing {
  Array<char> names; // NUL-terminated names, back to back
  Array<u32>  name_off; // offset of entry i's name in `names`
  Array<u16>  name_len;
  Array<u8>   kind; // EntryKind, from d_type
  Array<u8>   flags; // EntryFlags
  Array<u64>  ino;

  Array<u64>  size;
  Array<i64>  mtime_ns;
  Array<u32>  mode;

  u32         count() const      { return name_off.len; }
  Str         name(u32 i) const  { return Str(names.data + name_off[i], name_len[i]); }
  const char* cname(u32 i) const { return names.data + name_off[i]; }
  bool        is_dir(u32 i) const { return kind[i] == EK_DIR || (flags[i] & EF_LINK_TO_DIR); }

  void  clear();
  usize bytes() const; // heap footprint of all columns
};

int dir_open(const char* path);

bool dir_list_read(int dirfd, DirListing& out);

void dir_list_stat_range(int dirfd, DirListing& l, u32 first, u32 count);

bool dir_list_refresh(int dirfd, DirListing& l, const char* stale, u32 stale_n, bool drop_stats, Array<i32>& old_to_new);

enum { STAT_BATCH_MAX = 256, STAT_BATCH_NAMES = 8192 };
struct StatBatch {
  int  dirfd = -1; // dup'ed by stat_batch_begin, closed by stat_batch_end
  u32  tab = 0, gen = 0; // whose listing, which generation
  u32  n = 0;
  bool inline_done = false;
  u32  index[STAT_BATCH_MAX];
  u32  name_off[STAT_BATCH_MAX];
  u64  size[STAT_BATCH_MAX];
  i64  mtime_ns[STAT_BATCH_MAX];
  u32  mode[STAT_BATCH_MAX];
  u8   flags[STAT_BATCH_MAX];
  u32  names_len = 0;
  char names[STAT_BATCH_NAMES];

  bool trash = false;
  char trash_dir[4096]; // the files/ directory
  u32  trash_off[STAT_BATCH_MAX]; // offset into trash_buf, 0xFFFFFFFF none
  i64  trash_deleted[STAT_BATCH_MAX];
  u32  trash_len = 0;
  char trash_buf[STAT_BATCH_NAMES * 4];
};

bool stat_batch_begin(StatBatch& b, int dirfd, u32 tab, u32 gen);

bool stat_batch_add(StatBatch& b, DirListing& l, u32 i);

void stat_batch_run(StatBatch& b);

void stat_batch_apply(const StatBatch& b, DirListing& l);
void stat_batch_end(StatBatch& b); // close the fd without applying (stale generation)
