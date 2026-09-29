#include "core/watch.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

static const u32 kMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_MODIFY | IN_CLOSE_WRITE |
                         IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_ONLYDIR | IN_EXCL_UNLINK;
static const i64 kQuietMs = 150, kMaxDelayMs = 700;

bool watch_open(Watcher& w) {
  if (w.fd >= 0) return true;
  w.fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  return w.fd >= 0;
}

void watch_close(Watcher& w) {
  if (w.fd >= 0) close(w.fd);
  w.fd = -1;
  w.watches.clear();
}

i32 watch_add(Watcher& w, const char* path) {
  if (w.fd < 0) return -1;
  for (u32 i = 0; i < w.watches.len; i++)
    if (w.watches[i].wd >= 0 && !strcmp(w.watches[i].path, path)) { w.watches[i].refs++; return (i32)i; }
  int wd = inotify_add_watch(w.fd, path, kMask);
  if (wd < 0) return -1;
  i32 slot = -1;
  for (u32 i = 0; i < w.watches.len; i++) if (w.watches[i].wd < 0) { slot = (i32)i; break; }
  if (slot < 0) { slot = (i32)w.watches.len; w.watches.push(DirWatch()); }
  DirWatch& d = w.watches[(u32)slot];
  d = DirWatch();
  d.wd = wd; d.refs = 1;
  snprintf(d.path, sizeof d.path, "%s", path);
  return slot;
}

void watch_release(Watcher& w, i32 index) {
  if (index < 0 || (u32)index >= w.watches.len) return;
  DirWatch& d = w.watches[(u32)index];
  if (!d.refs) return;
  if (--d.refs) return;
  if (w.fd >= 0 && d.wd >= 0) inotify_rm_watch(w.fd, d.wd); // a gone watch was already removed by the kernel
  d.wd = -1; d.dirty = d.gone = false; d.path[0] = 0; d.names_len = d.name_count = 0;
}

static void note_name(DirWatch& d, const char* name) {
  if (d.overflow) return;
  Str s(name);
  if (watch_name_stale(d, s)) return;
  if (d.name_count >= WATCH_MAX_NAMES || d.names_len + s.n + 1 > sizeof d.names) { d.overflow = true; d.names_len = d.name_count = 0; return; }
  memcpy(d.names + d.names_len, name, s.n + 1);
  d.names_len += s.n + 1;
  d.name_count++;
}

u32 watch_drain(Watcher& w, i64 now_ms) {
  if (w.fd < 0) return 0;
  for (;;) {
    ssize_t n = read(w.fd, w.buf, sizeof w.buf);
    if (n < 0) { if (errno == EINTR) continue; break; } // EAGAIN: drained
    if (n == 0) break;
    ssize_t pos = 0;
    while (pos + (ssize_t)sizeof(struct inotify_event) <= n) {
      const struct inotify_event* e = (const struct inotify_event*)(w.buf + pos);
      pos += (ssize_t)sizeof(struct inotify_event) + e->len;
      w.drained++;
      if (e->mask & IN_Q_OVERFLOW) {
        for (DirWatch& d : w.watches) if (d.wd >= 0) { d.dirty = true; d.overflow = true; d.names_len = d.name_count = 0; if (!d.first_ms) d.first_ms = now_ms; d.last_ms = now_ms; }
        continue;
      }
      for (DirWatch& d : w.watches) {
        if (d.wd != e->wd) continue;
        if (e->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) d.gone = true;
        if (e->mask & IN_IGNORED) { d.gone = true; d.wd = -1; } // the kernel removed the watch
        if (!d.dirty) { d.dirty = true; d.first_ms = now_ms; }
        d.last_ms = now_ms;
        d.events++;
        if (e->len && (e->mask & (IN_ATTRIB | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE))) note_name(d, e->name);
        break;
      }
    }
  }
  u32 dirty = 0;
  for (DirWatch& d : w.watches) dirty += d.dirty;
  return dirty;
}

i64 watch_due_ms(const DirWatch& d) {
  if (!d.dirty) return 0;
  if (d.gone) return d.last_ms; // now
  i64 a = d.last_ms + kQuietMs, b = d.first_ms + kMaxDelayMs;
  return a < b ? a : b;
}

void watch_clear(DirWatch& d) {
  d.dirty = false; d.overflow = false; d.first_ms = d.last_ms = 0; d.events = 0;
  d.names_len = d.name_count = 0;
}

bool watch_name_stale(const DirWatch& d, Str name) {
  for (u32 o = 0; o < d.names_len;) {
    u32 n = (u32)strlen(d.names + o);
    if (n == name.n && !memcmp(d.names + o, name.p, n)) return true;
    o += n + 1;
  }
  return false;
}
