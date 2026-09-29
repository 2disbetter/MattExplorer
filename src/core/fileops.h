#pragma once

#include "base/types.h"
#include "base/array.h"
#include "base/str.h"

enum FileOpKind : u8 {
  OP_COPY = 0, // paths -> dest/NAME
  OP_MOVE,
  OP_DELETE, // paths, permanently
  OP_TRASH, // paths -> the trash
  OP_RENAMES,
  OP_RESTORE,
  OP_SCAN,
  OP_CHMOD,
  OP_CHMOD_RESTORE,
  OP_CHOWN,
  OP_CHOWN_RESTORE,
  OP_COUNT
};

enum ConflictAnswer : u8 { CONFLICT_ASK = 0, CONFLICT_SKIP, CONFLICT_REPLACE, CONFLICT_KEEP_BOTH, CONFLICT_CANCEL };

enum OpPhase : u32 { OP_PHASE_QUEUED = 0, OP_PHASE_SCAN, OP_PHASE_RUN, OP_PHASE_ASK, OP_PHASE_DONE };

struct FileOpRequest {
  FileOpKind     kind = OP_COPY;
  Array<char>    paths;
  Array<u32>     off;
  char           dest[4096] = {}; // destination directory for copy / move
  ConflictAnswer policy = CONFLICT_ASK;
  bool           force_copy = false;
  u32            mode_value = 0, mode_mask = 0; // OP_CHMOD: new = (old & ~mask) | (value & mask)
  i32            uid = -1, gid = -1; // OP_CHOWN: the new owner / group, -1 leaves one alone
  bool           recursive = false;

  void add(const char* path) { u32 n = (u32)strlen(path); off.push(paths.len); memcpy(paths.push_n(n + 1), path, n + 1); }
  const char* path(u32 i) const { return paths.data + off[i]; }
  u32 count() const { return off.len; }
};

struct FileOpProgress {
  volatile u32 phase = OP_PHASE_QUEUED;
  volatile u32 cancel = 0;
  volatile u64 bytes_done = 0, bytes_total = 0;
  volatile u32 items_done = 0, items_total = 0; // files + directories + links, counted by the scan

  volatile u64 bytes_alloc = 0; // st_blocks * 512: size on disk
  volatile u32 files = 0, dirs = 0, links = 0, others = 0, unreadable = 0; // unreadable: folders that could not be opened
  volatile u32 cur_seq = 0;
  char         current[256] = {};
};

struct FileOpResult {
  bool ok = false, cancelled = false;
  u32  errors = 0, skipped = 0, done = 0; // top-level items
  u32  retries = 0; // items tried again after an error (on_error said so)
  char err[512] = {}; // the first error, "path: reason"

  Array<char> journal;
  Array<u32>  joff;
  bool journal_truncated = false; // more happened than the journal holds: not undoable
  u32  perm_errors = 0;
  u32 pairs() const { return joff.len / 2; }
  const char* from(u32 i) const { return journal.data + joff[2 * i]; }
  const char* to(u32 i) const   { return journal.data + joff[2 * i + 1]; }
};

struct FileOpConflict {
  const char* src; // full source path
  const char* dst; // full destination path that exists
  bool        src_dir, dst_dir;
  u64         src_size, dst_size;
  i64         src_mtime, dst_mtime; // unix seconds
};

typedef ConflictAnswer (*ConflictFn)(void* ctx, const FileOpConflict& c, bool* apply_all);
typedef void (*ProgressFn)(void* ctx);

enum ErrorAnswer : u8 { ERR_SKIP = 0, ERR_RETRY, ERR_CANCEL };
typedef ErrorAnswer (*ErrorFn)(void* ctx, const char* path, const char* reason, bool* skip_all);

struct FileOpCtx {
  FileOpProgress* prog = nullptr;
  ConflictFn      ask = nullptr; // nullptr: CONFLICT_ASK behaves as SKIP
  void*           ask_ctx = nullptr;
  ProgressFn      notify = nullptr;
  void*           notify_ctx = nullptr;
  ErrorFn         on_error = nullptr; // nullptr: errors are skipped (the first one is reported)
  void*           error_ctx = nullptr;
};

void fileop_run(const FileOpRequest& req, FileOpCtx& ctx, FileOpResult& out);

bool fileop_rename(int dirfd, const char* from, const char* to);

bool fileop_new_folder(int dirfd, const char* base, char* name, u32 cap);

bool fileop_free_name(int dirfd, const char* name, bool copy_suffix, char* out, u32 cap);

bool path_is_inside(const char* src, const char* dst);

bool trash_dir_for(const char* path, char* trash_dir, u32 cap, char* relative_to, u32 rcap);

bool trash_put(const char* path, char* trashed, u32 tcap, char* err, u32 ecap);

bool trash_info_read(const char* trashed_path, char* original, u32 ocap, i64* deleted_unix);

bool trash_restore(const char* trashed_path, const char* original, char* err, u32 ecap);

bool trash_home(char* out, u32 cap);

bool trash_is_files_dir(const char* path);

const char* fileop_verb(FileOpKind k); // "Copying", "Moving", ...
const char* fileop_done_verb(FileOpKind k); // "Copied", "Moved", ...
