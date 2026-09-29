#include "core/fileops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum OpStatus : u8 { ST_OK = 0, ST_SKIPPED, ST_FAILED, ST_CANCELLED };

struct PathBuf {
  char s[4096];
  u32  len = 0;
  void set(const char* p) { len = (u32)mx_min((usize)strlen(p), sizeof s - 1); memcpy(s, p, len); s[len] = 0; }
  u32  push(const char* name) {
    u32 keep = len;
    usize n = strlen(name);
    if (len + 1 + n < sizeof s) { if (len != 1 || s[0] != '/') s[len++] = '/'; memcpy(s + len, name, n); len += (u32)n; s[len] = 0; }
    return keep;
  }
  void pop(u32 keep) { len = keep; s[len] = 0; }
};

struct KDirent64 { u64 d_ino; i64 d_off; u16 d_reclen; u8 d_type; char d_name[]; };

struct DirIter {
  int  fd;
  long n, pos;
  u8   buf[32768];
};
static DirIter* iter_open(int dirfd, const char* name) {
  int fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return nullptr;
  DirIter* it = (DirIter*)malloc(sizeof(DirIter));
  if (!it) { close(fd); errno = ENOMEM; return nullptr; }
  it->fd = fd; it->n = 0; it->pos = 0;
  return it;
}
static const KDirent64* iter_next(DirIter* it) {
  for (;;) {
    if (it->pos >= it->n) {
      long n;
      do n = syscall(SYS_getdents64, it->fd, it->buf, sizeof it->buf); while (n < 0 && errno == EINTR);
      if (n <= 0) return nullptr;
      it->n = n; it->pos = 0;
    }
    const KDirent64* d = (const KDirent64*)(it->buf + it->pos);
    it->pos += d->d_reclen;
    const char* nm = d->d_name;
    if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
    return d;
  }
}
static void iter_close(DirIter* it) { if (it) { close(it->fd); free(it); } }

static const char* base_of(const char* path) {
  const char* b = strrchr(path, '/');
  return (b && b[1]) ? b + 1 : path;
}

static void dir_of(const char* path, char* out, u32 cap) {
  const char* b = strrchr(path, '/');
  if (!b || b == path) { snprintf(out, cap, "/"); return; }
  u32 n = mx_min((u32)(b - path), cap - 1);
  memcpy(out, path, n); out[n] = 0;
}

static bool mkdir_p(const char* path, u32 mode) {
  char tmp[4096];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char* p = tmp + 1; *p; p++) {
    if (*p != '/') continue;
    *p = 0;
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return false;
    *p = '/';
  }
  if (mkdir(tmp, mode) != 0 && errno != EEXIST) return false;
  struct stat st;
  return stat(tmp, &st) == 0 && S_ISDIR(st.st_mode);
}

struct Op {
  const FileOpRequest* req;
  FileOpCtx*           ctx;
  FileOpResult*        out;
  FileOpProgress*      prog;
  ConflictAnswer       policy;
  u8*                  iobuf = nullptr; // read/write fallback buffer
  u32                  depth = 0;
  u32                  tmp_counter = 0;
  bool                 any_error = false;
  bool                 count_deletes = false;
  bool                 skip_all = false; // on_error answered "skip all": no more asking
  u32                  asks = 0;
  char                 last_err[512]; // the latest error, for on_error
  PathBuf              src, dst;
};

static const usize kIoBuf = 1 << 18;
static const u32   kMaxDepth = 128;

static bool cancelled(Op& op) { return __atomic_load_n(&op.prog->cancel, __ATOMIC_ACQUIRE) != 0; }
static void set_phase(Op& op, u32 ph) { __atomic_store_n(&op.prog->phase, ph, __ATOMIC_RELEASE); }
static void add_bytes(Op& op, u64 n) { __atomic_fetch_add(&op.prog->bytes_done, n, __ATOMIC_RELAXED); }
static void add_item(Op& op) { __atomic_fetch_add(&op.prog->items_done, 1u, __ATOMIC_RELAXED); }
static void notify(Op& op) { if (op.ctx->notify) op.ctx->notify(op.ctx->notify_ctx); }

static void set_current(Op& op, const char* name) {
  FileOpProgress& p = *op.prog;
  u32 seq = __atomic_load_n(&p.cur_seq, __ATOMIC_RELAXED);
  __atomic_store_n(&p.cur_seq, seq + 1, __ATOMIC_RELEASE); // odd: being written
  snprintf(p.current, sizeof p.current, "%s", name);
  __atomic_store_n(&p.cur_seq, seq + 2, __ATOMIC_RELEASE);
}

static void fail(Op& op, const char* path, const char* what) {
  op.any_error = true;
  snprintf(op.last_err, sizeof op.last_err, "%s", what);
  if (!op.out->err[0]) snprintf(op.out->err, sizeof op.out->err, "%s: %s", path, what);
}

static ErrorAnswer ask_error(Op& op, const char* path) {
  op.asks++;
  if (!op.ctx->on_error || op.skip_all || cancelled(op)) return ERR_SKIP;
  bool all = false;
  ErrorAnswer ans = op.ctx->on_error(op.ctx->error_ctx, path, op.last_err, &all);
  if (all) op.skip_all = true;
  if (ans == ERR_CANCEL) __atomic_store_n(&op.prog->cancel, 1u, __ATOMIC_RELEASE);
  else if (ans == ERR_RETRY) op.out->retries++;
  return ans;
}
static void fail_errno(Op& op, const char* path) { fail(op, path, strerror(errno)); }

static void journal(Op& op, const char* from, const char* to) {
  FileOpResult& r = *op.out;
  u32 a = (u32)strlen(from), b = (u32)strlen(to);
  r.joff.push(r.journal.len); memcpy(r.journal.push_n(a + 1), from, a + 1);
  r.joff.push(r.journal.len); memcpy(r.journal.push_n(b + 1), to, b + 1);
}

static void scan_entry(Op& op, int dirfd, const char* name, u32 depth) {
  struct stat st;
  if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return;
  FileOpProgress& p = *op.prog;
  __atomic_fetch_add(&p.items_total, 1u, __ATOMIC_RELAXED);
  __atomic_fetch_add(&p.bytes_alloc, (u64)st.st_blocks * 512u, __ATOMIC_RELAXED);
  if (S_ISREG(st.st_mode)) { __atomic_fetch_add(&p.bytes_total, (u64)st.st_size, __ATOMIC_RELAXED); __atomic_fetch_add(&p.files, 1u, __ATOMIC_RELAXED); return; }
  if (S_ISLNK(st.st_mode)) { __atomic_fetch_add(&p.links, 1u, __ATOMIC_RELAXED); return; }
  if (!S_ISDIR(st.st_mode)) { __atomic_fetch_add(&p.others, 1u, __ATOMIC_RELAXED); return; }
  __atomic_fetch_add(&p.dirs, 1u, __ATOMIC_RELAXED);
  if (depth >= kMaxDepth) return;
  DirIter* it = iter_open(dirfd, name);
  if (!it) { __atomic_fetch_add(&p.unreadable, 1u, __ATOMIC_RELAXED); return; }
  while (const KDirent64* d = iter_next(it)) {
    if (cancelled(op)) break;
    scan_entry(op, it->fd, d->d_name, depth + 1);
  }
  iter_close(it);
}

static void scan_path(Op& op, const char* path) {
  char dir[4096];
  dir_of(path, dir, sizeof dir);
  int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd < 0) return;
  scan_entry(op, dfd, base_of(path), 0);
  close(dfd);
}

static ConflictAnswer resolve_conflict(Op& op, int sdir, const char* sname, int ddir, const char* dname, const struct stat& sst, const struct stat& dst_st) {
  if (op.policy != CONFLICT_ASK) return op.policy;
  if (!op.ctx->ask) return CONFLICT_SKIP;
  FileOpConflict c = {};
  c.src = op.src.s; c.dst = op.dst.s;
  c.src_dir = S_ISDIR(sst.st_mode); c.dst_dir = S_ISDIR(dst_st.st_mode);
  c.src_size = c.src_dir ? 0 : (u64)sst.st_size; c.dst_size = c.dst_dir ? 0 : (u64)dst_st.st_size;
  c.src_mtime = sst.st_mtime; c.dst_mtime = dst_st.st_mtime;
  (void)sdir; (void)sname; (void)ddir; (void)dname;
  bool all = false;
  set_phase(op, OP_PHASE_ASK);
  ConflictAnswer a = op.ctx->ask(op.ctx->ask_ctx, c, &all);
  set_phase(op, OP_PHASE_RUN);
  if (all && a != CONFLICT_CANCEL) op.policy = a;
  if (a == CONFLICT_CANCEL) __atomic_store_n(&op.prog->cancel, 1u, __ATOMIC_RELEASE);
  return a;
}

static OpStatus delete_entry(Op& op, int dirfd, const char* name);

static OpStatus delete_dir_contents(Op& op, int dirfd, const char* name) {
  DirIter* it = iter_open(dirfd, name);
  if (!it) { fail_errno(op, op.src.s); return ST_FAILED; }
  OpStatus st = ST_OK;
  while (const KDirent64* d = iter_next(it)) {
    if (cancelled(op)) { st = ST_CANCELLED; break; }
    u32 keep = op.src.push(d->d_name);
    OpStatus r = delete_entry(op, it->fd, d->d_name);
    op.src.pop(keep);
    if (r == ST_CANCELLED) { st = r; break; }
    if (r == ST_FAILED) st = ST_FAILED;
  }
  iter_close(it);
  return st;
}

static OpStatus delete_entry(Op& op, int dirfd, const char* name) {
  struct stat st;
  if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) { fail_errno(op, op.src.s); return ST_FAILED; }
  set_current(op, name);
  if (S_ISDIR(st.st_mode)) {
    if (op.depth >= kMaxDepth) { fail(op, op.src.s, "directory tree too deep"); return ST_FAILED; }
    op.depth++;
    OpStatus r = delete_dir_contents(op, dirfd, name);
    op.depth--;
    if (r != ST_OK) return r;
    if (unlinkat(dirfd, name, AT_REMOVEDIR) != 0) { fail_errno(op, op.src.s); return ST_FAILED; }
  } else if (unlinkat(dirfd, name, 0) != 0) { fail_errno(op, op.src.s); return ST_FAILED; }
  if (op.count_deletes) {
    add_item(op);
    if (S_ISREG(st.st_mode)) add_bytes(op, (u64)st.st_size);
    notify(op);
  }
  return ST_OK;
}

static OpStatus delete_path(Op& op, const char* path) {
  char dir[4096];
  dir_of(path, dir, sizeof dir);
  int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd < 0) { fail_errno(op, path); return ST_FAILED; }
  op.src.set(path);
  OpStatus r = delete_entry(op, dfd, base_of(path));
  close(dfd);
  return r;
}

static OpStatus copy_entry(Op& op, int sdir, const char* sname, int ddir, const char* dname, char* final_name = nullptr, u32 final_cap = 0);

static OpStatus copy_regular(Op& op, int sdir, const char* sname, int ddir, const char* dname, const struct stat& sst) {
  int in = openat(sdir, sname, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (in < 0) { fail_errno(op, op.src.s); return ST_FAILED; }

  char tmp[300];
  snprintf(tmp, sizeof tmp, ".%.200s.mxtmp%u", dname, ++op.tmp_counter);
  int out = openat(ddir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, sst.st_mode & 0777);
  if (out < 0) { fail_errno(op, op.dst.s); close(in); return ST_FAILED; }
  OpStatus st = ST_OK;
  bool use_cfr = true;
  u64 left = (u64)sst.st_size;
  for (;;) {
    if (cancelled(op)) { st = ST_CANCELLED; break; }
    ssize_t n;
    if (use_cfr) {
      n = copy_file_range(in, nullptr, out, nullptr, (usize)mx_min<u64>(left ? left : (1u << 20), 1u << 22), 0);
      if (n < 0 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP || errno == EPERM)) { use_cfr = false; continue; }
    } else {
      if (!op.iobuf) { op.iobuf = (u8*)malloc(kIoBuf); if (!op.iobuf) { errno = ENOMEM; n = -1; } }
      n = op.iobuf ? read(in, op.iobuf, kIoBuf) : -1;
      if (n > 0) {
        ssize_t w = 0;
        while (w < n) { ssize_t k = write(out, op.iobuf + w, (usize)(n - w)); if (k < 0) { if (errno == EINTR) continue; n = -1; break; } w += k; }
      }
    }
    if (n < 0) { if (errno == EINTR) continue; fail_errno(op, op.dst.s); st = ST_FAILED; break; }
    if (n == 0) break;
    add_bytes(op, (u64)n);
    if (left >= (u64)n) left -= (u64)n;
    notify(op);
  }
  if (st == ST_OK) {
    fchmod(out, sst.st_mode & 07777); // may fail on vfat: not an error
    struct timespec ts[2] = { sst.st_atim, sst.st_mtim };
    futimens(out, ts);
  }
  close(in);
  if (close(out) != 0 && st == ST_OK) { fail_errno(op, op.dst.s); st = ST_FAILED; }
  if (st == ST_OK && renameat(ddir, tmp, ddir, dname) != 0) { fail_errno(op, op.dst.s); st = ST_FAILED; }
  if (st != ST_OK) unlinkat(ddir, tmp, 0);
  return st;
}

static OpStatus copy_symlink(Op& op, int sdir, const char* sname, int ddir, const char* dname) {
  char target[4096];
  ssize_t n = readlinkat(sdir, sname, target, sizeof target - 1);
  if (n < 0) { fail_errno(op, op.src.s); return ST_FAILED; }
  target[n] = 0;
  if (symlinkat(target, ddir, dname) != 0) { fail_errno(op, op.dst.s); return ST_FAILED; }
  return ST_OK;
}

static OpStatus copy_dir_contents(Op& op, int sdir, const char* sname, int ddir, const char* dname) {
  DirIter* it = iter_open(sdir, sname);
  if (!it) { fail_errno(op, op.src.s); return ST_FAILED; }
  int dfd = openat(ddir, dname, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (dfd < 0) { fail_errno(op, op.dst.s); iter_close(it); return ST_FAILED; }
  OpStatus st = ST_OK;
  while (const KDirent64* d = iter_next(it)) {
    if (cancelled(op)) { st = ST_CANCELLED; break; }
    u32 ks = op.src.push(d->d_name), kd = op.dst.push(d->d_name);
    OpStatus r;
    for (;;) {
      u32 before = op.asks;
      r = copy_entry(op, it->fd, d->d_name, dfd, d->d_name);
      if (r != ST_FAILED || op.asks > before) break; // fine, or a failure deeper down was already asked about
      if (ask_error(op, op.src.s) != ERR_RETRY) break;
    }
    op.src.pop(ks); op.dst.pop(kd);
    if (r == ST_CANCELLED || cancelled(op)) { st = ST_CANCELLED; break; }
    if (r == ST_FAILED) st = ST_FAILED;
  }
  close(dfd);
  iter_close(it);
  return st;
}

static OpStatus copy_entry(Op& op, int sdir, const char* sname, int ddir, const char* dname, char* final_name, u32 final_cap) {
  struct stat sst, dst_st;
  if (fstatat(sdir, sname, &sst, AT_SYMLINK_NOFOLLOW) != 0) { fail_errno(op, op.src.s); return ST_FAILED; }
  set_current(op, sname);
  bool exists = fstatat(ddir, dname, &dst_st, AT_SYMLINK_NOFOLLOW) == 0;
  char keep_name[300];
  if (exists) {
    if (sst.st_dev == dst_st.st_dev && sst.st_ino == dst_st.st_ino) { fail(op, op.src.s, "source and destination are the same file"); return ST_FAILED; }
    bool merge = S_ISDIR(sst.st_mode) && S_ISDIR(dst_st.st_mode);
    if (!merge) {
      ConflictAnswer a = resolve_conflict(op, sdir, sname, ddir, dname, sst, dst_st);
      switch (a) {
        case CONFLICT_CANCEL: return ST_CANCELLED;
        case CONFLICT_SKIP:   return ST_SKIPPED;
        case CONFLICT_KEEP_BOTH:
          if (!fileop_free_name(ddir, dname, false, keep_name, sizeof keep_name)) { fail(op, op.dst.s, "no free name"); return ST_FAILED; }
          dname = keep_name;
          if (final_name) snprintf(final_name, final_cap, "%s", keep_name);
          exists = false;
          break;
        case CONFLICT_REPLACE:
          if (S_ISDIR(dst_st.st_mode)) { // a file replacing a directory: the tree goes first
            PathBuf saved = op.src;
            OpStatus r = delete_path(op, op.dst.s);
            op.src = saved;
            if (r != ST_OK) return r;
          } else if (S_ISDIR(sst.st_mode) && unlinkat(ddir, dname, 0) != 0) { fail_errno(op, op.dst.s); return ST_FAILED; }
          exists = S_ISDIR(sst.st_mode) ? false : exists;
          break;
        default: break;
      }
    }
  }
  if (S_ISDIR(sst.st_mode)) {
    if (op.depth >= kMaxDepth) { fail(op, op.src.s, "directory tree too deep"); return ST_FAILED; }
    if (!exists && mkdirat(ddir, dname, (sst.st_mode & 0777) | 0700) != 0 && errno != EEXIST) { fail_errno(op, op.dst.s); return ST_FAILED; }
    op.depth++;
    OpStatus r = copy_dir_contents(op, sdir, sname, ddir, dname);
    op.depth--;
    if (r == ST_OK) {
      int dfd = openat(ddir, dname, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (dfd >= 0) { struct timespec ts[2] = { sst.st_atim, sst.st_mtim }; futimens(dfd, ts); fchmod(dfd, sst.st_mode & 07777); close(dfd); }
      add_item(op);
      notify(op);
    }
    return r;
  }
  OpStatus r;
  if (S_ISREG(sst.st_mode)) r = copy_regular(op, sdir, sname, ddir, dname, sst);
  else if (S_ISLNK(sst.st_mode)) {
    if (exists && unlinkat(ddir, dname, 0) != 0) { fail_errno(op, op.dst.s); return ST_FAILED; }
    r = copy_symlink(op, sdir, sname, ddir, dname);
  } else { fail(op, op.src.s, "special file, not copied"); return ST_FAILED; }
  if (r == ST_OK) { add_item(op); notify(op); }
  return r;
}

static OpStatus move_item(Op& op, const char* path, int ddir, char* dname, u32 dcap) {
  char sdir_path[4096];
  dir_of(path, sdir_path, sizeof sdir_path);
  int sdir = open(sdir_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (sdir < 0) { fail_errno(op, path); return ST_FAILED; }
  const char* sname = base_of(path);
  snprintf(dname, dcap, "%s", sname);
  op.src.set(path);
  op.dst.set(op.req->dest); op.dst.push(dname);
  set_current(op, sname);
  struct stat sst, dst_st;
  OpStatus st = ST_OK;
  if (fstatat(sdir, sname, &sst, AT_SYMLINK_NOFOLLOW) != 0) { fail_errno(op, path); close(sdir); return ST_FAILED; }
  bool exists = fstatat(ddir, dname, &dst_st, AT_SYMLINK_NOFOLLOW) == 0;
  bool merge = false;
  if (exists) {
    if (sst.st_dev == dst_st.st_dev && sst.st_ino == dst_st.st_ino) { fail(op, path, "source and destination are the same file"); close(sdir); return ST_FAILED; }
    merge = S_ISDIR(sst.st_mode) && S_ISDIR(dst_st.st_mode);
    if (!merge) {
      ConflictAnswer a = resolve_conflict(op, sdir, sname, ddir, dname, sst, dst_st);
      if (a == CONFLICT_CANCEL) { close(sdir); return ST_CANCELLED; }
      if (a == CONFLICT_SKIP) { close(sdir); return ST_SKIPPED; }
      if (a == CONFLICT_KEEP_BOTH) {
        char nm[300];
        if (!fileop_free_name(ddir, dname, false, nm, sizeof nm)) { fail(op, op.dst.s, "no free name"); close(sdir); return ST_FAILED; }
        snprintf(dname, dcap, "%s", nm);
        op.dst.set(op.req->dest); op.dst.push(dname);
        exists = false;
      } else { // replace: whatever is in the way goes first
        OpStatus r = ST_OK;
        if (S_ISDIR(dst_st.st_mode)) { PathBuf sv = op.src; r = delete_path(op, op.dst.s); op.src = sv; }
        else if (unlinkat(ddir, dname, 0) != 0) { fail_errno(op, op.dst.s); r = ST_FAILED; }
        if (r != ST_OK) { close(sdir); return r; }
        exists = false;
      }
    }
  }
  if (!merge && !op.req->force_copy) {
    if (renameat(sdir, sname, ddir, dname) == 0) {
      add_item(op); if (S_ISREG(sst.st_mode)) add_bytes(op, (u64)sst.st_size);
      notify(op);
      close(sdir);
      return ST_OK;
    }
    if (errno != EXDEV) { fail_errno(op, path); close(sdir); return ST_FAILED; }
  }

  if (!op.req->force_copy || merge) scan_entry(op, sdir, sname, 0);
  st = copy_entry(op, sdir, sname, ddir, dname);
  if (st == ST_OK) {
    PathBuf saved = op.src;
    st = delete_path(op, path);
    op.src = saved;
  }
  close(sdir);
  return st;
}

static void run_copy_move(Op& op) {
  const FileOpRequest& req = *op.req;
  bool is_move = req.kind == OP_MOVE;
  int ddir = open(req.dest, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (ddir < 0) { fail_errno(op, req.dest); return; }
  if (!is_move || req.force_copy) {
    set_phase(op, OP_PHASE_SCAN);
    for (u32 i = 0; i < req.count() && !cancelled(op); i++) scan_path(op, req.path(i));
  }
  set_phase(op, OP_PHASE_RUN);
  for (u32 i = 0; i < req.count(); i++) {
    if (cancelled(op)) break;
    const char* path = req.path(i);
    if (path_is_inside(path, req.dest)) {
      char dir[4096]; dir_of(path, dir, sizeof dir);
      if (strcmp(dir, req.dest) != 0 || is_move) { fail(op, path, is_move ? "cannot move a folder into itself" : "cannot copy a folder into itself"); op.out->errors++; continue; }
    }
    char dname[300];
    OpStatus st;
    for (;;) { // an item that fails may be tried again (on_error)
      u32 before = op.asks;
      if (is_move) st = move_item(op, path, ddir, dname, sizeof dname);
      else {
        char sdir_path[4096];
        dir_of(path, sdir_path, sizeof sdir_path);
        const char* sname = base_of(path);
        snprintf(dname, sizeof dname, "%s", sname);
        int sdir = open(sdir_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (sdir < 0) { fail_errno(op, path); st = ST_FAILED; }
        else {
          bool named = true;
          if (!strcmp(sdir_path, req.dest)) { // a copy beside the original: "name - Copy"
            if (!fileop_free_name(ddir, sname, true, dname, sizeof dname)) { fail(op, path, "no free name"); named = false; }
          }
          if (named) {
            op.src.set(path);
            op.dst.set(req.dest); op.dst.push(dname);
            st = copy_entry(op, sdir, sname, ddir, dname, dname, sizeof dname);
          } else st = ST_FAILED;
          close(sdir);
        }
      }
      if (st != ST_FAILED || op.asks > before) break;
      if (ask_error(op, path) != ERR_RETRY) break;
    }
    if (cancelled(op) && st != ST_OK) break;
    if (st == ST_OK) { op.out->done++; char to[4096]; snprintf(to, sizeof to, "%s/%s", strcmp(req.dest, "/") ? req.dest : "", dname); journal(op, path, to); }
    else if (st == ST_SKIPPED) op.out->skipped++;
    else if (st == ST_FAILED) op.out->errors++;
    else break;
  }
  close(ddir);
}

static void run_delete(Op& op) {
  const FileOpRequest& req = *op.req;
  op.count_deletes = true;
  set_phase(op, OP_PHASE_SCAN);
  for (u32 i = 0; i < req.count() && !cancelled(op); i++) scan_path(op, req.path(i));
  set_phase(op, OP_PHASE_RUN);
  for (u32 i = 0; i < req.count(); i++) {
    if (cancelled(op)) break;
    OpStatus st;
    for (;;) { st = delete_path(op, req.path(i)); if (st != ST_FAILED || ask_error(op, req.path(i)) != ERR_RETRY) break; }
    if (cancelled(op) && st != ST_OK) break;
    if (st == ST_OK) op.out->done++;
    else if (st == ST_FAILED) op.out->errors++;
    else break;
  }
}

static void run_trash(Op& op) {
  const FileOpRequest& req = *op.req;
  set_phase(op, OP_PHASE_RUN);
  __atomic_store_n(&op.prog->items_total, req.count(), __ATOMIC_RELAXED);
  for (u32 i = 0; i < req.count(); i++) {
    if (cancelled(op)) break;
    const char* path = req.path(i);
    set_current(op, base_of(path));
    char trashed[4096], err[256];
    bool ok;
    for (;;) {
      ok = trash_put(path, trashed, sizeof trashed, err, sizeof err);
      if (ok) break;
      fail(op, path, err);
      if (ask_error(op, path) != ERR_RETRY) break;
    }
    if (ok) { op.out->done++; journal(op, path, trashed); }
    else if (cancelled(op)) break;
    else op.out->errors++;
    add_item(op); notify(op);
  }
}

static void run_scan(Op& op) {
  const FileOpRequest& req = *op.req;
  set_phase(op, OP_PHASE_SCAN);
  for (u32 i = 0; i < req.count() && !cancelled(op); i++) {
    set_current(op, base_of(req.path(i)));
    scan_path(op, req.path(i));
    op.out->done++;
    notify(op);
  }
}

static const u32 kChmodJournalMax = 200000; // pairs; past that the operation is not undoable

static void chmod_one(Op& op, int dirfd, const char* name, const char* full, const struct stat& st) {
  if (S_ISLNK(st.st_mode)) { add_item(op); return; }
  const FileOpRequest& req = *op.req;
  u32 old = st.st_mode & 07777;
  u32 nw = (old & ~req.mode_mask) | (req.mode_value & req.mode_mask);
  if (nw == old) { add_item(op); return; }
  if (fchmodat(dirfd, name, nw, 0) != 0) {
    if (errno == EPERM || errno == EACCES) op.out->perm_errors++;
    fail_errno(op, full); op.out->errors++; add_item(op); return;
  }
  if (op.out->pairs() < kChmodJournalMax) { char o[16]; snprintf(o, sizeof o, "%o", old); journal(op, full, o); }
  else op.out->journal_truncated = true;
  add_item(op);
}

static void chmod_tree(Op& op, int dirfd, const char* name, u32 depth) {
  struct stat st;
  if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) { fail_errno(op, op.src.s); op.out->errors++; return; }
  chmod_one(op, dirfd, name, op.src.s, st);
  if (!S_ISDIR(st.st_mode) || !op.req->recursive || depth >= kMaxDepth) return;
  DirIter* it = iter_open(dirfd, name);
  if (!it) { fail_errno(op, op.src.s); op.out->errors++; return; }
  while (const KDirent64* d = iter_next(it)) {
    if (cancelled(op)) break;
    u32 keep = op.src.push(d->d_name);
    chmod_tree(op, it->fd, d->d_name, depth + 1);
    op.src.pop(keep);
    if ((__atomic_load_n(&op.prog->items_done, __ATOMIC_RELAXED) & 255) == 0) notify(op);
  }
  iter_close(it);
}

static void chown_one(Op& op, int dirfd, const char* name, const char* full, const struct stat& st) {
  const FileOpRequest& req = *op.req;
  i32 nu = req.uid >= 0 ? req.uid : (i32)st.st_uid, ng = req.gid >= 0 ? req.gid : (i32)st.st_gid;
  if (nu == (i32)st.st_uid && ng == (i32)st.st_gid) { add_item(op); return; }
  if (fchownat(dirfd, name, (uid_t)nu, (gid_t)ng, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == EPERM || errno == EACCES) op.out->perm_errors++;
    fail_errno(op, full); op.out->errors++; add_item(op); return;
  }
  if (op.out->pairs() < kChmodJournalMax) { char o[32]; snprintf(o, sizeof o, "%u:%u", (u32)st.st_uid, (u32)st.st_gid); journal(op, full, o); }
  else op.out->journal_truncated = true;
  add_item(op);
}

static void chown_tree(Op& op, int dirfd, const char* name, u32 depth) {
  struct stat st;
  if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) { fail_errno(op, op.src.s); op.out->errors++; return; }
  chown_one(op, dirfd, name, op.src.s, st);
  if (!S_ISDIR(st.st_mode) || !op.req->recursive || depth >= kMaxDepth) return;
  DirIter* it = iter_open(dirfd, name);
  if (!it) { fail_errno(op, op.src.s); op.out->errors++; return; }
  while (const KDirent64* d = iter_next(it)) {
    if (cancelled(op)) break;
    u32 keep = op.src.push(d->d_name);
    chown_tree(op, it->fd, d->d_name, depth + 1);
    op.src.pop(keep);
    if ((__atomic_load_n(&op.prog->items_done, __ATOMIC_RELAXED) & 255) == 0) notify(op);
  }
  iter_close(it);
}

static void run_chown(Op& op) {
  const FileOpRequest& req = *op.req;
  if (req.kind == OP_CHOWN_RESTORE) { // pairs (path, "uid:gid"): exact owners back
    set_phase(op, OP_PHASE_RUN);
    __atomic_store_n(&op.prog->items_total, req.count() / 2, __ATOMIC_RELAXED);
    for (u32 i = 0; i + 1 < req.count(); i += 2) {
      if (cancelled(op)) break;
      const char* path = req.path(i);
      u32 u = 0, g = 0;
      struct stat st;
      bool have = lstat(path, &st) == 0;
      if (sscanf(req.path(i + 1), "%u:%u", &u, &g) == 2 && lchown(path, u, g) == 0) {
        op.out->done++;
        if (have && op.out->pairs() < kChmodJournalMax) { char o[32]; snprintf(o, sizeof o, "%u:%u", (u32)st.st_uid, (u32)st.st_gid); journal(op, path, o); }
      }
      else { if (errno == EPERM || errno == EACCES) op.out->perm_errors++; fail_errno(op, path); op.out->errors++; }
      add_item(op);
    }
    notify(op);
    return;
  }
  if (req.recursive) {
    set_phase(op, OP_PHASE_SCAN);
    for (u32 i = 0; i < req.count() && !cancelled(op); i++) scan_path(op, req.path(i));
  } else __atomic_store_n(&op.prog->items_total, req.count(), __ATOMIC_RELAXED);
  set_phase(op, OP_PHASE_RUN);
  for (u32 i = 0; i < req.count(); i++) {
    if (cancelled(op)) break;
    const char* path = req.path(i);
    set_current(op, base_of(path));
    char dir[4096];
    dir_of(path, dir, sizeof dir);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) { fail_errno(op, path); op.out->errors++; continue; }
    u32 before = op.out->errors;
    op.src.set(path);
    chown_tree(op, dfd, base_of(path), 0);
    close(dfd);
    if (op.out->errors == before) op.out->done++;
    notify(op);
  }
}

static void run_chmod(Op& op) {
  const FileOpRequest& req = *op.req;
  if (req.kind == OP_CHMOD_RESTORE) { // pairs (path, "octal"): exact modes back
    set_phase(op, OP_PHASE_RUN);
    __atomic_store_n(&op.prog->items_total, req.count() / 2, __ATOMIC_RELAXED);
    for (u32 i = 0; i + 1 < req.count(); i += 2) {
      if (cancelled(op)) break;
      const char* path = req.path(i);
      u32 mode = (u32)strtoul(req.path(i + 1), nullptr, 8);
      struct stat st;
      bool have = lstat(path, &st) == 0;
      if (chmod(path, mode) == 0) {
        op.out->done++;

        if (have && op.out->pairs() < kChmodJournalMax) { char o[16]; snprintf(o, sizeof o, "%o", (u32)(st.st_mode & 07777)); journal(op, path, o); }
      }
      else { if (errno == EPERM || errno == EACCES) op.out->perm_errors++; fail_errno(op, path); op.out->errors++; }
      add_item(op);
    }
    notify(op);
    return;
  }
  if (req.recursive) {
    set_phase(op, OP_PHASE_SCAN);
    for (u32 i = 0; i < req.count() && !cancelled(op); i++) scan_path(op, req.path(i));
  } else __atomic_store_n(&op.prog->items_total, req.count(), __ATOMIC_RELAXED);
  set_phase(op, OP_PHASE_RUN);
  for (u32 i = 0; i < req.count(); i++) {
    if (cancelled(op)) break;
    const char* path = req.path(i);
    set_current(op, base_of(path));
    char dir[4096];
    dir_of(path, dir, sizeof dir);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) { fail_errno(op, path); op.out->errors++; continue; }
    u32 before = op.out->errors;
    op.src.set(path);
    chmod_tree(op, dfd, base_of(path), 0);
    close(dfd);
    if (op.out->errors == before) op.out->done++;
    notify(op);
  }
}

static void run_renames(Op& op) {
  const FileOpRequest& req = *op.req;
  bool restore = req.kind == OP_RESTORE;
  set_phase(op, OP_PHASE_RUN);
  __atomic_store_n(&op.prog->items_total, req.count() / 2, __ATOMIC_RELAXED);
  for (u32 i = 0; i + 1 < req.count(); i += 2) {
    if (cancelled(op)) break;
    const char* from = req.path(i); const char* to = req.path(i + 1);
    set_current(op, base_of(from));
    bool ok;
    char err[256] = {};
    if (restore) ok = trash_restore(from, to, err, sizeof err);
    else {
      struct stat st;
      if (lstat(to, &st) == 0) { ok = false; snprintf(err, sizeof err, "%s exists", to); errno = EEXIST; }
      else {
        ok = rename(from, to) == 0;
        if (!ok && errno == EXDEV) { // undoing a cross-device move: copy back and delete
          char ddir[4096]; dir_of(to, ddir, sizeof ddir);
          FileOpRequest sub; sub.kind = OP_MOVE; sub.add(from); snprintf(sub.dest, sizeof sub.dest, "%s", ddir); sub.policy = CONFLICT_SKIP;
          Op inner = op; inner.req = &sub;
          int dfd = open(ddir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
          char dname[300];
          ok = dfd >= 0 && move_item(inner, from, dfd, dname, sizeof dname) == ST_OK;
          if (dfd >= 0) close(dfd);
          op.iobuf = inner.iobuf; op.any_error = inner.any_error;
        }
        if (!ok && !err[0]) snprintf(err, sizeof err, "%s", strerror(errno));
      }
    }
    if (ok) { op.out->done++; journal(op, from, to); }
    else { fail(op, from, err); op.out->errors++; }
    add_item(op); notify(op);
  }
}

void fileop_run(const FileOpRequest& req, FileOpCtx& ctx, FileOpResult& out) {
  FileOpProgress local;
  Op op;
  op.req = &req; op.ctx = &ctx; op.out = &out;
  op.prog = ctx.prog ? ctx.prog : &local;
  op.policy = req.policy;
  out.ok = false; out.cancelled = false; out.errors = out.skipped = out.done = 0; out.err[0] = 0;
  out.journal.clear(); out.joff.clear(); out.journal_truncated = false; out.perm_errors = 0;
  switch (req.kind) {
    case OP_COPY: case OP_MOVE: run_copy_move(op); break;
    case OP_DELETE: run_delete(op); break;
    case OP_TRASH: run_trash(op); break;
    case OP_RENAMES: case OP_RESTORE: run_renames(op); break;
    case OP_SCAN: run_scan(op); break;
    case OP_CHMOD: case OP_CHMOD_RESTORE: run_chmod(op); break;
    case OP_CHOWN: case OP_CHOWN_RESTORE: run_chown(op); break;
    default: fail(op, "", "unknown operation"); out.errors++; break;
  }
  out.cancelled = cancelled(op);
  out.ok = !out.cancelled && out.errors == 0 && !op.any_error;
  if (out.cancelled && !out.err[0]) snprintf(out.err, sizeof out.err, "cancelled");
  free(op.iobuf);
  set_phase(op, OP_PHASE_DONE);
  notify(op);
}

bool fileop_rename(int dirfd, const char* from, const char* to) {
  if (!to[0] || strchr(to, '/') || !strcmp(to, ".") || !strcmp(to, "..")) { errno = EINVAL; return false; }

  if (renameat2(dirfd, from, dirfd, to, RENAME_NOREPLACE) == 0) return true;
  if (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) return false;
  struct stat st;
  if (fstatat(dirfd, to, &st, AT_SYMLINK_NOFOLLOW) == 0) { errno = EEXIST; return false; }
  return renameat(dirfd, from, dirfd, to) == 0;
}

bool fileop_new_folder(int dirfd, const char* base, char* name, u32 cap) {
  for (u32 n = 1; n < 1000; n++) {
    if (n == 1) snprintf(name, cap, "%s", base); else snprintf(name, cap, "%s (%u)", base, n);
    if (mkdirat(dirfd, name, 0755) == 0) return true;
    if (errno != EEXIST) return false;
  }
  errno = EEXIST;
  return false;
}

bool fileop_free_name(int dirfd, const char* name, bool copy_suffix, char* out, u32 cap) {

  usize len = strlen(name);
  usize stem = len;
  for (usize i = len; i > 1; i--) if (name[i - 1] == '.') { stem = i - 1; break; }
  if (stem < len && stem >= 5 && len - stem <= 5) {
    usize j = stem;
    while (j > 1 && name[j - 1] != '.') j--;
    if (j > 1 && stem - j <= 4 && stem - j >= 2) {
      bool alnum = true;
      for (usize k = j; k < stem; k++) if (!is_lower(name[k]) && !is_upper(name[k]) && !is_digit(name[k])) alnum = false;
      if (alnum) stem = j - 1;
    }
  }
  struct stat st;
  for (u32 n = 1; n < 1000; n++) {
    if (copy_suffix) {
      if (n == 1) snprintf(out, cap, "%.*s - Copy%s", (int)stem, name, name + stem);
      else snprintf(out, cap, "%.*s - Copy (%u)%s", (int)stem, name, n, name + stem);
    } else {
      snprintf(out, cap, "%.*s (%u)%s", (int)stem, name, n + 1, name + stem);
    }
    if (fstatat(dirfd, out, &st, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT) return true;
  }
  return false;
}

bool path_is_inside(const char* src, const char* dst) {
  usize n = strlen(src);
  if (n == 1 && src[0] == '/') return true;
  if (strncmp(src, dst, n) != 0) return false;
  return dst[n] == 0 || dst[n] == '/';
}

const char* fileop_verb(FileOpKind k) {
  switch (k) { case OP_COPY: return "Copying"; case OP_MOVE: return "Moving"; case OP_DELETE: return "Deleting"; case OP_TRASH: return "Moving to trash";
               case OP_RENAMES: return "Renaming"; case OP_RESTORE: return "Restoring"; case OP_SCAN: return "Measuring";
               case OP_CHMOD: case OP_CHMOD_RESTORE: return "Changing permissions of";
               case OP_CHOWN: case OP_CHOWN_RESTORE: return "Changing the owner of"; default: return "Working"; }
}
const char* fileop_done_verb(FileOpKind k) {
  switch (k) { case OP_COPY: return "Copied"; case OP_MOVE: return "Moved"; case OP_DELETE: return "Deleted"; case OP_TRASH: return "Moved to trash";
               case OP_RENAMES: return "Renamed"; case OP_RESTORE: return "Restored"; case OP_SCAN: return "Measured";
               case OP_CHMOD: case OP_CHMOD_RESTORE: return "Changed permissions of";
               case OP_CHOWN: case OP_CHOWN_RESTORE: return "Changed the owner of"; default: return "Done"; }
}

bool trash_home(char* out, u32 cap) {
  const char* xdg = getenv("XDG_DATA_HOME");
  if (xdg && xdg[0]) { snprintf(out, cap, "%s/Trash", xdg); return true; }
  const char* home = getenv("HOME");
  if (!home || !home[0]) return false;
  snprintf(out, cap, "%s/.local/share/Trash", home);
  return true;
}

bool trash_is_files_dir(const char* path) {
  char home[4096];
  usize n = strlen(path);
  if (n < 7 || strcmp(path + n - 6, "/files")) return false;
  if (trash_home(home, sizeof home)) {
    usize h = strlen(home);
    if (n == h + 6 && !strncmp(path, home, h)) return true;
  }

  char pat[64];
  u32 uid = (u32)getuid();
  snprintf(pat, sizeof pat, "/.Trash-%u/files", uid);
  usize pl = strlen(pat);
  if (n > pl && !strcmp(path + n - pl, pat)) return true;
  snprintf(pat, sizeof pat, "/.Trash/%u/files", uid);
  pl = strlen(pat);
  return n > pl && !strcmp(path + n - pl, pat);
}

static bool trash_prepare(const char* dir) {
  char sub[4096];
  if (!mkdir_p(dir, 0700)) return false;
  snprintf(sub, sizeof sub, "%s/files", dir);
  if (!mkdir_p(sub, 0700)) return false;
  snprintf(sub, sizeof sub, "%s/info", dir);
  return mkdir_p(sub, 0700);
}

bool trash_dir_for(const char* path, char* trash_dir, u32 cap, char* relative_to, u32 rcap) {
  char parent[4096];
  dir_of(path, parent, sizeof parent);
  struct stat pst, hst;
  if (stat(parent, &pst) != 0) return false;
  relative_to[0] = 0;
  char home[4096];
  if (trash_home(home, sizeof home)) {
    char home_parent[4096];
    dir_of(home, home_parent, sizeof home_parent);
    bool home_ok = (stat(home, &hst) == 0 && S_ISDIR(hst.st_mode)) || (mkdir_p(home_parent, 0755) && stat(home_parent, &hst) == 0);
    if (home_ok && hst.st_dev == pst.st_dev) {
      snprintf(trash_dir, cap, "%s", home);
      if (trash_prepare(trash_dir)) return true;
    }
  }

  char top[4096];
  snprintf(top, sizeof top, "%s", parent);
  for (;;) {
    char up[4096];
    dir_of(top, up, sizeof up);
    struct stat ust;
    if (!strcmp(up, top) || stat(up, &ust) != 0 || ust.st_dev != pst.st_dev) break;
    snprintf(top, sizeof top, "%s", up);
  }
  snprintf(relative_to, rcap, "%s", top);
  u32 uid = (u32)getuid();
  char cand[4096];
  struct stat tst;
  snprintf(cand, sizeof cand, "%s/.Trash", top);
  if (lstat(cand, &tst) == 0 && S_ISDIR(tst.st_mode) && (tst.st_mode & S_ISVTX)) {
    snprintf(trash_dir, cap, "%s/.Trash/%u", top, uid);
    if (trash_prepare(trash_dir)) return true;
  }
  snprintf(trash_dir, cap, "%s/.Trash-%u", top, uid);
  if (trash_prepare(trash_dir)) return true;
  errno = EACCES;
  return false;
}

static void url_encode(const char* s, char* out, u32 cap) {
  static const char hex[] = "0123456789ABCDEF";
  u32 o = 0;
  for (const u8* p = (const u8*)s; *p && o + 4 < cap; p++) {
    u8 c = *p;
    bool keep = is_lower((char)c) || is_upper((char)c) || is_digit((char)c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
    if (keep) out[o++] = (char)c;
    else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
  }
  out[o] = 0;
}
static void url_decode(const char* s, char* out, u32 cap) {
  u32 o = 0;
  auto hv = [](char c) -> int { return is_digit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1; };
  for (const char* p = s; *p && o + 1 < cap; p++) {
    if (*p == '%' && hv(p[1]) >= 0 && hv(p[2]) >= 0) { out[o++] = (char)(hv(p[1]) * 16 + hv(p[2])); p += 2; }
    else out[o++] = *p;
  }
  out[o] = 0;
}

bool trash_put(const char* path, char* trashed, u32 tcap, char* err, u32 ecap) {
  char tdir[4096], top[4096];
  if (!trash_dir_for(path, tdir, sizeof tdir, top, sizeof top)) { snprintf(err, ecap, "no usable trash directory (%s)", strerror(errno)); return false; }
  if (path_is_inside(tdir, path)) { snprintf(err, ecap, "already in the trash"); return false; }
  const char* base = base_of(path);

  char info[4096], name[300];
  int fd = -1;
  for (u32 n = 0; n < 1000; n++) {
    if (!n) snprintf(name, sizeof name, "%.250s", base); else snprintf(name, sizeof name, "%.240s.%u", base, n);
    snprintf(info, sizeof info, "%s/info/%s.trashinfo", tdir, name);
    fd = open(info, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0) break;
    if (errno != EEXIST) { snprintf(err, ecap, "%s: %s", info, strerror(errno)); return false; }
  }
  if (fd < 0) { snprintf(err, ecap, "no free name in the trash"); return false; }
  char rel[4096], enc[8192], body[8400];
  const char* p = path;
  if (top[0]) { usize tl = strlen(top); if (!strncmp(path, top, tl) && path[tl] == '/') p = path + tl + 1; }
  snprintf(rel, sizeof rel, "%s", p);
  url_encode(rel, enc, sizeof enc);
  time_t now = time(nullptr);
  struct tm tm; localtime_r(&now, &tm);
  char date[32]; strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
  int n = snprintf(body, sizeof body, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", enc, date);
  bool wrote = n > 0 && write(fd, body, (usize)n) == n;
  close(fd);
  if (!wrote) { unlink(info); snprintf(err, ecap, "%s: write failed", info); return false; }
  snprintf(trashed, tcap, "%s/files/%s", tdir, name);
  if (rename(path, trashed) != 0) {
    int e = errno;
    unlink(info);
    snprintf(err, ecap, "%s", e == EXDEV ? "cannot move into the trash across filesystems" : strerror(e));
    return false;
  }
  return true;
}

static bool info_path_of(const char* trashed, char* info, u32 cap, char* top, u32 tcap) {

  const char* files = strstr(trashed, "/files/");
  const char* last = nullptr;
  for (const char* f = files; f; f = strstr(f + 1, "/files/")) last = f;
  if (!last) return false;
  snprintf(info, cap, "%.*s/info/%s.trashinfo", (int)(last - trashed), trashed, last + 7);

  top[0] = 0;
  const char* t = strstr(trashed, "/.Trash-");
  if (!t) t = strstr(trashed, "/.Trash/");
  if (t) snprintf(top, tcap, "%.*s", (int)(t - trashed), trashed);
  return true;
}

bool trash_info_read(const char* trashed, char* original, u32 ocap, i64* deleted_unix) {
  char info[4096], top[4096];
  if (!info_path_of(trashed, info, sizeof info, top, sizeof top)) return false;
  FILE* f = fopen(info, "r");
  if (!f) return false;
  char line[8300];
  bool have_path = false;
  if (deleted_unix) *deleted_unix = 0;
  while (fgets(line, sizeof line, f)) {
    if (char* nl = strpbrk(line, "\r\n")) *nl = 0;
    if (!strncmp(line, "Path=", 5)) {
      char dec[4096];
      url_decode(line + 5, dec, sizeof dec);
      if (dec[0] == '/' || !top[0]) snprintf(original, ocap, "%s", dec);
      else snprintf(original, ocap, "%s/%s", top, dec);
      have_path = true;
    } else if (!strncmp(line, "DeletionDate=", 13) && deleted_unix) {
      struct tm tm = {};
      int Y, M, D, h, m, s;
      if (sscanf(line + 13, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6) {
        tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D; tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = s; tm.tm_isdst = -1;
        *deleted_unix = (i64)mktime(&tm);
      }
    }
  }
  fclose(f);
  return have_path;
}

bool trash_restore(const char* trashed, const char* original, char* err, u32 ecap) {
  char info[4096], top[4096];
  if (!info_path_of(trashed, info, sizeof info, top, sizeof top)) { snprintf(err, ecap, "not a trashed path"); return false; }
  struct stat st;
  if (lstat(original, &st) == 0) { snprintf(err, ecap, "%s exists", original); errno = EEXIST; return false; }
  char parent[4096];
  dir_of(original, parent, sizeof parent);
  if (stat(parent, &st) != 0 && !mkdir_p(parent, 0755)) { snprintf(err, ecap, "%s: %s", parent, strerror(errno)); return false; }
  if (rename(trashed, original) != 0) { snprintf(err, ecap, "%s", strerror(errno)); return false; }
  unlink(info);
  return true;
}
