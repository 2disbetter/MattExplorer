#include "core/dir_list.h"
#include "core/fileops.h"
#include <stdio.h>

#include <dirent.h> // DT_* constants only
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

struct KDirent64 {
  u64  d_ino;
  i64  d_off;
  u16  d_reclen;
  u8   d_type;
  char d_name[];
};

static const usize kReadBuf = 256 * 1024; // ~10k entries per syscall

static u8 kind_from_dtype(u8 t) {
  switch (t) {
    case DT_REG:     return EK_FILE;
    case DT_DIR:     return EK_DIR;
    case DT_LNK:     return EK_SYMLINK;
    case DT_UNKNOWN: return EK_UNKNOWN;
    default:         return EK_OTHER;
  }
}

static u8 kind_from_mode(u32 mode) {
  switch (mode & S_IFMT) {
    case S_IFREG: return EK_FILE;
    case S_IFDIR: return EK_DIR;
    case S_IFLNK: return EK_SYMLINK;
    default:      return EK_OTHER;
  }
}

void DirListing::clear() {
  names.clear(); name_off.clear(); name_len.clear(); kind.clear(); flags.clear(); ino.clear();
  size.clear(); mtime_ns.clear(); mode.clear();
}

usize DirListing::bytes() const {
  return names.bytes() + name_off.bytes() + name_len.bytes() + kind.bytes() + flags.bytes() +
         ino.bytes() + size.bytes() + mtime_ns.bytes() + mode.bytes();
}

int dir_open(const char* path) {
  return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

bool dir_list_read(int dirfd, DirListing& out) {
  out.clear();
  if (lseek(dirfd, 0, SEEK_SET) < 0) return false;

  u8* buf = (u8*)malloc(kReadBuf);
  if (!buf) { errno = ENOMEM; return false; }

  for (;;) {
    long n = syscall(SYS_getdents64, dirfd, buf, kReadBuf);
    if (n < 0) {
      if (errno == EINTR) continue;
      int e = errno; free(buf); errno = e;
      return false;
    }
    if (n == 0) break;

    long pos = 0;
    while (pos < n) {
      KDirent64* d = (KDirent64*)(buf + pos);
      pos += d->d_reclen;
      const char* nm = d->d_name;
      if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;

      u32   len = (u32)strlen(nm);
      u32   off = out.names.len;
      char* dst = out.names.push_n(len + 1);
      memcpy(dst, nm, len + 1);

      out.name_off.push(off);
      out.name_len.push((u16)len);
      out.kind.push(kind_from_dtype(d->d_type));
      out.flags.push(nm[0] == '.' ? (u8)EF_HIDDEN : (u8)0);
      out.ino.push(d->d_ino);
    }
  }
  free(buf);

  u32 c = out.count();
  out.size.resize_zero(c);
  out.mtime_ns.resize_zero(c);
  out.mode.resize_zero(c);
  return true;
}

void dir_list_stat_range(int dirfd, DirListing& l, u32 first, u32 count) {
  u32 end = mx_min(first + count, l.count());
  for (u32 i = first; i < end; i++) {
    if (l.flags[i] & (EF_STATTED | EF_STAT_FAILED)) continue;

    struct statx sx;
    int r = statx(dirfd, l.cname(i), AT_SYMLINK_NOFOLLOW | AT_STATX_DONT_SYNC,
                  STATX_TYPE | STATX_MODE | STATX_SIZE | STATX_MTIME, &sx);
    if (r != 0) { l.flags[i] |= EF_STAT_FAILED; continue; }

    l.size[i]     = sx.stx_size;
    l.mtime_ns[i] = (i64)sx.stx_mtime.tv_sec * 1000000000LL + sx.stx_mtime.tv_nsec;
    l.mode[i]     = sx.stx_mode;
    l.flags[i]   |= EF_STATTED;
    if (l.kind[i] == EK_UNKNOWN) l.kind[i] = kind_from_mode(sx.stx_mode);

    if (l.kind[i] == EK_SYMLINK) {

      struct statx tx;
      if (statx(dirfd, l.cname(i), AT_STATX_DONT_SYNC, STATX_TYPE, &tx) == 0 && S_ISDIR(tx.stx_mode))
        l.flags[i] |= EF_LINK_TO_DIR;
    }
  }
}

static u32 name_hash(Str s) {
  u32 h = 2166136261u;
  for (u32 i = 0; i < s.n; i++) { h ^= (u8)s.p[i]; h *= 16777619u; }
  return h;
}

bool dir_list_refresh(int dirfd, DirListing& l, const char* stale, u32 stale_n, bool drop_stats, Array<i32>& old_to_new) {
  DirListing fresh;
  if (!dir_list_read(dirfd, fresh)) return false;
  u32 oc = l.count(), nc = fresh.count();
  old_to_new.resize(oc);
  for (u32 i = 0; i < oc; i++) old_to_new[i] = -1;
  if (oc && nc) {
    u32 cap = 16;
    while (cap < oc * 2) cap *= 2;
    u32* table = (u32*)malloc((usize)cap * sizeof(u32));
    if (!table) { errno = ENOMEM; return false; }
    memset(table, 0xFF, (usize)cap * sizeof(u32));
    for (u32 i = 0; i < oc; i++) {
      u32 h = name_hash(l.name(i)) & (cap - 1);
      while (table[h] != 0xFFFFFFFFu) h = (h + 1) & (cap - 1);
      table[h] = i;
    }
    for (u32 j = 0; j < nc; j++) {
      Str nm = fresh.name(j);
      u32 h = name_hash(nm) & (cap - 1);
      while (table[h] != 0xFFFFFFFFu) {
        u32 i = table[h];
        if (str_eq(l.name(i), nm)) {
          old_to_new[i] = (i32)j;
          bool keep = !drop_stats && (l.flags[i] & EF_STATTED) && l.ino[i] == fresh.ino[j] && l.kind[i] == fresh.kind[j];
          if (keep && stale_n) {
            for (u32 o = 0; o < stale_n;) {
              u32 n = (u32)strlen(stale + o);
              if (n == nm.n && !memcmp(stale + o, nm.p, n)) { keep = false; break; }
              o += n + 1;
            }
          }
          if (keep) {
            fresh.size[j] = l.size[i]; fresh.mtime_ns[j] = l.mtime_ns[i]; fresh.mode[j] = l.mode[i];
            fresh.flags[j] |= l.flags[i] & (EF_STATTED | EF_LINK_TO_DIR);
          }
          fresh.flags[j] |= l.flags[i] & EF_CUT;
          break;
        }
        h = (h + 1) & (cap - 1);
      }
    }
    free(table);
  }
  l = static_cast<DirListing&&>(fresh);
  return true;
}

bool stat_batch_begin(StatBatch& b, int dirfd, u32 tab, u32 gen) {
  b.n = 0; b.names_len = 0; b.tab = tab; b.gen = gen; b.inline_done = false;
  b.dirfd = fcntl(dirfd, F_DUPFD_CLOEXEC, 0);
  return b.dirfd >= 0;
}

bool stat_batch_add(StatBatch& b, DirListing& l, u32 i) {
  if (b.n >= STAT_BATCH_MAX) return false;
  u32 len = l.name_len[i];
  if (b.names_len + len + 1 > STAT_BATCH_NAMES) return false;
  b.index[b.n] = i;
  b.name_off[b.n] = b.names_len;
  memcpy(b.names + b.names_len, l.cname(i), len + 1);
  b.names_len += len + 1;
  b.flags[b.n] = 0;
  b.n++;
  l.flags[i] |= EF_STAT_PENDING;
  return true;
}

void stat_batch_run(StatBatch& b) {
  b.trash_len = 0;
  for (u32 k = 0; k < b.n; k++) {
    const char* nm = b.names + b.name_off[k];
    if (b.trash) {
      b.trash_off[k] = 0xFFFFFFFFu; b.trash_deleted[k] = 0;
      char full[4096], orig[4096]; i64 del = 0;
      snprintf(full, sizeof full, "%s/%s", b.trash_dir, nm);
      if (trash_info_read(full, orig, sizeof orig, &del)) {
        u32 l = (u32)strlen(orig);
        if (b.trash_len + l + 1 <= sizeof b.trash_buf) { b.trash_off[k] = b.trash_len; memcpy(b.trash_buf + b.trash_len, orig, l + 1); b.trash_len += l + 1; }
        b.trash_deleted[k] = del;
      }
    }
    struct statx sx;
    int r = statx(b.dirfd, nm, AT_SYMLINK_NOFOLLOW | AT_STATX_DONT_SYNC, STATX_TYPE | STATX_MODE | STATX_SIZE | STATX_MTIME, &sx);
    if (r != 0) { b.flags[k] = EF_STAT_FAILED; continue; }
    b.size[k] = sx.stx_size;
    b.mtime_ns[k] = (i64)sx.stx_mtime.tv_sec * 1000000000LL + sx.stx_mtime.tv_nsec;
    b.mode[k] = sx.stx_mode;
    b.flags[k] = EF_STATTED;
    if (S_ISLNK(sx.stx_mode)) {
      struct statx tx;
      if (statx(b.dirfd, nm, AT_STATX_DONT_SYNC, STATX_TYPE, &tx) == 0 && S_ISDIR(tx.stx_mode)) b.flags[k] |= EF_LINK_TO_DIR;
    }
  }
}

void stat_batch_apply(const StatBatch& b, DirListing& l) {
  for (u32 k = 0; k < b.n; k++) {
    u32 i = b.index[k];
    if (i >= l.count()) continue;
    l.flags[i] &= (u8)~EF_STAT_PENDING;
    if (!b.flags[k]) continue;
    if (b.flags[k] & EF_STAT_FAILED) { l.flags[i] |= EF_STAT_FAILED; continue; }
    if (l.flags[i] & EF_STATTED) continue; // something else statted it first
    l.size[i] = b.size[k]; l.mtime_ns[i] = b.mtime_ns[k]; l.mode[i] = b.mode[k];
    l.flags[i] |= b.flags[k] & (EF_STATTED | EF_LINK_TO_DIR);
    if (l.kind[i] == EK_UNKNOWN) l.kind[i] = kind_from_mode(b.mode[k]);
  }
}

void stat_batch_end(StatBatch& b) {
  if (b.dirfd >= 0) close(b.dirfd);
  b.dirfd = -1;
  b.n = 0;
}
