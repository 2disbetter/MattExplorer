#include "app/app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 hash_key(const char* path, i64 mtime) {
  u32 h = 2166136261u;
  for (const u8* p = (const u8*)path; *p; p++) h = (h ^ *p) * 16777619u;
  for (u32 i = 0; i < 8; i++) h = (h ^ (u8)(mtime >> (8 * i))) * 16777619u;
  return h ? h : 1;
}

static void slots_rebuild(ThumbCache& c) {
  u32 n = 1024;
  while (n < c.entries.len * 2) n *= 2;
  c.slots.resize_zero(n);
  memset(c.slots.data, 0, (usize)n * sizeof(i32));
  for (u32 i = 0; i < c.entries.len; i++) {
    u32 h = hash_key(c.paths.data + c.entries[i].path_off, c.entries[i].mtime) & (n - 1);
    while (c.slots[h]) h = (h + 1) & (n - 1);
    c.slots[h] = (i32)i + 1;
  }
}

static i32 cache_find(ThumbCache& c, const char* path, i64 mtime) {
  if (!c.slots.len) return -1;
  u32 n = c.slots.len, h = hash_key(path, mtime) & (n - 1);
  while (c.slots[h]) {
    i32 i = c.slots[h] - 1;
    const ThumbEntry& e = c.entries[(u32)i];
    if (e.mtime == mtime && !strcmp(c.paths.data + e.path_off, path)) return i;
    h = (h + 1) & (n - 1);
  }
  return -1;
}

static void entry_release(ThumbEntry& e) { image_free(e.img); image_free(e.disp); e.bytes = 0; }

static void cache_make_room(ThumbCache& c) {
  while (c.entries.len >= THUMB_MAX_ENTRIES || c.bytes > THUMB_MAX_BYTES) {
    if (!c.entries.len) break;
    u32 victim = 0;
    for (u32 i = 1; i < c.entries.len; i++) if (c.entries[i].used_ms < c.entries[victim].used_ms) victim = i;
    c.bytes -= c.entries[victim].bytes;
    entry_release(c.entries[victim]);
    c.entries[victim] = c.entries[c.entries.len - 1];
    c.entries.pop();
    slots_rebuild(c);
  }
  if (c.paths.len > (1u << 20)) { // compact the path buffer
    Array<char> np;
    for (ThumbEntry& e : c.entries) { const char* p = c.paths.data + e.path_off; u32 l = (u32)strlen(p); e.path_off = np.len; memcpy(np.push_n(l + 1), p, l + 1); }
    c.paths = static_cast<Array<char>&&>(np);
    slots_rebuild(c);
  }
}

static i32 cache_insert(ThumbCache& c, const char* path, i64 mtime, Image& img, i64 now) {
  cache_make_room(c);
  ThumbEntry e = {};
  e.path_off = c.paths.len;
  u32 l = (u32)strlen(path);
  memcpy(c.paths.push_n(l + 1), path, l + 1);
  e.mtime = mtime;
  e.img = img; img.px = nullptr; img.w = img.h = 0;
  e.disp_size = 0;
  e.used_ms = now;
  e.bytes = (u32)((usize)e.img.w * e.img.h * 4) + l + sizeof(ThumbEntry);
  c.bytes += e.bytes;
  c.entries.push(e);
  if (c.entries.len * 2 > c.slots.len) slots_rebuild(c);
  else {
    u32 n = c.slots.len, h = hash_key(path, mtime) & (n - 1);
    while (c.slots[h]) h = (h + 1) & (n - 1);
    c.slots[h] = (i32)c.entries.len;
  }
  return (i32)c.entries.len - 1;
}

static void job_thumb(Job& j) {
  ThumbJob* t = (ThumbJob*)j.ctx;
  char app_dir[64]; snprintf(app_dir, sizeof app_dir, "mattexplorer-%s", MX_VERSION);
  t->ok = thumb_get(t->path, t->mtime, t->size, t->want, app_dir, t->img, t->err, sizeof t->err, &t->from_cache);
}

void thumb_frame_begin(App& a) { a.thumbs.frame_asked = 0; }

static void state_sync(Tab& t) {
  if (t.thumb_gen != t.gen || t.thumb_state.len != t.listing.count()) {
    t.thumb_state.resize_zero(t.listing.count());
    memset(t.thumb_state.data, 0, t.thumb_state.len);
    t.thumb_gen = t.gen;
  }
}

const Image* thumb_for(App& a, Tab& t, u32 i, i32 size) {
  if (a.no_thumbs || i >= t.listing.count()) return nullptr;
  if (!(t.listing.flags[i] & EF_STATTED)) return nullptr;
  state_sync(t);
  u8& st = t.thumb_state[i];
  if (st == 3) return nullptr;
  char path[4096];
  entry_path(t, i, path, sizeof path);
  i64 mtime = t.listing.mtime_ns[i] / 1000000000LL;
  ThumbCache& c = a.thumbs;
  i32 k = cache_find(c, path, mtime);
  if (k >= 0) {
    ThumbEntry& e = c.entries[(u32)k];
    e.used_ms = a.ui.now_ms;
    c.hits++;
    if (!e.img.px) { st = 3; return nullptr; }
    st = 2;
    if (e.disp_size != size || !e.disp.px) {
      image_free(e.disp);
      i32 w = e.img.w, h = e.img.h;
      if (w > size || h > size) { if (w >= h) { h = mx_max(1, (i32)((i64)h * size / w)); w = size; } else { w = mx_max(1, (i32)((i64)w * size / h)); h = size; } }
      image_resample(e.img, w, h, e.disp);
      e.disp_size = size;
      c.bytes += (u32)((usize)w * h * 4);
      e.bytes += (u32)((usize)w * h * 4);
    }
    return e.disp.px ? &e.disp : nullptr;
  }
  if (st == 1) return nullptr; // asked already
  if (c.frame_asked >= THUMB_PER_FRAME || c.inflight >= THUMB_MAX_INFLIGHT) return nullptr; // next frame
  if (t.listing.kind[i] != EK_FILE && t.listing.kind[i] != EK_SYMLINK) { st = 3; return nullptr; }
  c.misses++;
  ThumbJob* j = (ThumbJob*)calloc(1, sizeof(ThumbJob));
  snprintf(j->path, sizeof j->path, "%s", path);
  j->mtime = mtime; j->size = t.listing.size[i]; j->want = THUMB_LARGE;
  j->tab = (u32)(&t - a.tabs); j->gen = t.gen; j->index = i;
  if (!worker_submit(a.pool, job_thumb, j, JOB_THUMB, 0)) { free(j); return nullptr; }
  c.inflight++; c.frame_asked++;
  st = 1;
  if (!a.pool.running) pool_drain(a); // inline: it is done already
  return nullptr;
}

void thumb_job_done(App& a, ThumbJob* j) {
  ThumbCache& c = a.thumbs;
  if (c.inflight) c.inflight--;
  if (!j->ok) c.failed++; else if (j->from_cache) c.cached++; else c.generated++;
  if (a.debug && !j->ok) fprintf(stderr, "thumb: %s: %s\n", j->path, j->err);
  i32 k = cache_find(c, j->path, j->mtime);
  if (k < 0) cache_insert(c, j->path, j->mtime, j->img, a.ui.now_ms);
  else image_free(j->img);

  if (j->tab < MAX_TABS) {
    Tab& t = a.tabs[j->tab];
    if (t.used && t.gen == j->gen && j->index < t.thumb_state.len && t.thumb_gen == t.gen) t.thumb_state[j->index] = j->ok ? 2 : 3;
  }
  free(j);
  a.w.need_redraw = true;
}

void thumbs_clear(App& a) {
  ThumbCache& c = a.thumbs;
  for (ThumbEntry& e : c.entries) entry_release(e);
  c.entries.clear(); c.paths.clear(); c.bytes = 0;
  if (c.slots.len) memset(c.slots.data, 0, (usize)c.slots.len * sizeof(i32));
}
