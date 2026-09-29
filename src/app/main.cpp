#include "app/app.h"
#include "core/format.h"
#include "core/path.h"
#include "platform/wayland/proto.h"
#include "platform/xkb/keysym.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static i64 now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (i64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static const UiTheme kDark = {
  /*bg*/ 0x1a1b26, /*sidebar*/ 0x16161e, /*panel*/ 0x1f2335, /*bar*/ 0x24283b, /*row_alt*/ 0x1c1e2b, /*hover*/ 0x292e42,
  /*selected*/ 0x33467c, /*selected_dim*/ 0x2a2f45, /*accent*/ 0x7aa2f7, /*red*/ 0xf7768e, /*text*/ 0xc0caf5, /*muted*/ 0x737aa2,
  /*border*/ 0x2f3549, /*folder*/ 0xe0af68, /*input*/ 0x16161e, /*tooltip*/ 0x2f3549, /*match*/ 0xff9e64 };
static const UiTheme kLight = {
  0xe1e2e7, 0xd5d6db, 0xf0f0f5, 0xd0d5e3, 0xdcdde3, 0xc4c8da,
  0xa9b1d6, 0xc4c8da, 0x2e7de9, 0xf52a65, 0x3760bf, 0x848cb5,
  0xb5b9cf, 0xd7891f, 0xf5f5fa, 0xd0d5e3, 0xb15c00 };

static bool open_ui_font(App& a) {
  const char* candidates[8]; u32 n = 0;
  char omarchy[128];
  if (a.font_spec[0]) candidates[n++] = a.font_spec;
  if (const char* env = getenv("MATTEXPLORER_FONT")) candidates[n++] = env;
  if (font_db_omarchy_family(omarchy, sizeof omarchy)) candidates[n++] = omarchy;
  candidates[n++] = "Cantarell"; candidates[n++] = "Noto Sans"; candidates[n++] = "DejaVu Sans"; candidates[n++] = "Liberation Sans";
  for (u32 i = 0; i < n; i++) {
    if (text_open(a.text, &a.db, candidates[i], a.cfg.font_px)) {
      if (a.debug) fprintf(stderr, "font: \"%s\" -> %s (%s %s)\n", candidates[i], a.text.faces[0].path, a.text.faces[0].ttf.family, a.text.faces[0].ttf.style);
      text_open(a.small, &a.db, a.text.faces[0].path, a.cfg.font_px * 0.86f); // same face, the small size
      return true;
    }
    if (a.debug) fprintf(stderr, "font: \"%s\" not usable (%s)\n", candidates[i], a.text.faces[0].ttf.err[0] ? a.text.faces[0].ttf.err : "not found");
  }
  for (u16 e : a.db.fallback_order)
    if (text_open(a.text, &a.db, font_db_path(a.db, e), a.cfg.font_px)) { text_open(a.small, &a.db, font_db_path(a.db, e), a.cfg.font_px * 0.86f); return true; }
  return false;
}

static int list_fonts(App& a) {
  font_db_scan(a.db);
  for (u32 i = 0; i < a.db.entries.len; i++) {
    const FontEntry& e = a.db.entries[i];
    printf("%-40s %-24s %3u%s%s%s  %s%s\n", font_db_family(a.db, (i32)i), font_db_style(a.db, (i32)i), e.weight,
           e.italic ? " italic" : "", e.fixed_pitch ? " mono" : "", e.cff ? " [cff]" : e.has_outlines ? "" : " [no outlines]",
           font_db_path(a.db, (i32)i), e.index ? "#" : "");
  }
  char om[128];
  printf("%u faces in %u files, scanned in %.1f ms; omarchy font: %s\n", a.db.entries.len, a.db.files, a.db.scan_ns / 1e6,
         font_db_omarchy_family(om, sizeof om) ? om : "(not configured)");
  return 0;
}

static void job_mounts_read(Job& j) { mounts_read(*(MountList*)j.ctx); }
static void job_mounts_stat(Job& j) { mounts_stat_one(*(MountEntry*)j.ctx); }

void mounts_request_refresh(App& a) {
  if (a.mounts_reading) { a.mounts_again = true; return; }
  a.mounts_reading = true;
  if (!worker_submit(a.pool, job_mounts_read, &a.mounts_fresh, JOB_MOUNTS_READ, 0)) a.mounts_reading = false;
}

static void job_apps_scan(Job& j) { app_db_scan(*(AppDb*)j.ctx); }
static void job_mime_scan(Job& j) { mime_db_load(*(MimeDb*)j.ctx); }

void apps_request_scan(App& a) {
  if (!a.mime_ready && !a.mime_scanning && worker_submit(a.pool, job_mime_scan, &a.mime, JOB_MIME_SCAN, 0)) a.mime_scanning = true;
  if (!a.apps_ready && !a.apps_scanning && worker_submit(a.pool, job_apps_scan, &a.apps, JOB_APPS_SCAN, 0)) a.apps_scanning = true;
}

static void job_clip_xfer(Job& j) {
  ClipXfer* x = (ClipXfer*)j.ctx;
  int fd = x->fd;
  x->ok = true;
  if (x->send) {
    const char* p = x->data.data; u32 left = x->data.len;
    while (left) {
      struct pollfd pf = { fd, POLLOUT, 0 };
      int r = poll(&pf, 1, 10000);
      if (r <= 0 || (pf.revents & (POLLERR | POLLHUP))) { x->ok = false; break; }
      ssize_t n = write(fd, p, left);
      if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; x->ok = false; break; }
      p += n; left -= (u32)n;
    }
  } else {
    for (;;) {
      struct pollfd pf = { fd, POLLIN, 0 };
      int r = poll(&pf, 1, 10000);
      if (r <= 0) { x->ok = false; break; }
      char b[65536];
      ssize_t n = read(fd, b, sizeof b);
      if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; x->ok = false; break; }
      if (n == 0) break;
      if (x->data.len + (u32)n > (8u << 20)) { x->ok = false; break; }
      memcpy(x->data.push_n((u32)n), b, (u32)n);
    }
  }
  close(fd);
}

void clip_xfer_submit(App& a, ClipXfer* x) {
  if (!worker_submit(a.pool, job_clip_xfer, x, x->send ? JOB_CLIP_SEND : JOB_CLIP_RECV, 0)) { close(x->fd); x->data.release(); free(x); return; }
  if (!a.pool.running) pool_drain(a);
}

static void mounts_job_done(App& a, Job& j) {
  if (j.kind == JOB_STAT_BATCH) { stat_job_done(a, (StatBatch*)j.ctx); return; }
  if (j.kind == JOB_PROPS_SCAN) { props_scan_done(a, (PropsScan*)j.ctx); return; }
  if (j.kind == JOB_APPS_SCAN) { a.apps_scanning = false; a.apps_ready = true; if (a.debug) fprintf(stderr, "apps: %u .desktop files (%u applications), %u mimeapps.list in %.2f ms on a worker\n", a.apps.files, a.apps.apps.len, a.apps.lists, a.apps.scan_ns / 1e6); return; }
  if (j.kind == JOB_MIME_SCAN) { a.mime_scanning = false; a.mime_ready = true; if (a.debug) fprintf(stderr, "mime: %u globs, %u parents, %u aliases in %.2f ms on a worker\n", a.mime.globs.len, a.mime.parents.len / 2, a.mime.aliases.len / 2, a.mime.scan_ns / 1e6); return; }
  if (j.kind == JOB_UDISKS) { udisks_done(a, (UdisksJob*)j.ctx); return; }
  if (j.kind == JOB_CLIP_SEND) { clip_send_done(a, (ClipXfer*)j.ctx); return; }
  if (j.kind == JOB_CLIP_RECV) { clip_recv_done(a, (ClipXfer*)j.ctx); a.w.need_redraw = true; return; }
  if (j.kind == JOB_THUMB) { thumb_job_done(a, (ThumbJob*)j.ctx); return; }
  if (j.kind == JOB_MOUNTS_READ) {
    a.mounts_reading = false;
    a.mounts_read_once = true;
    mounts_carry_stats(a.mounts, a.mounts_fresh);
    a.mounts.entries = static_cast<Array<MountEntry>&&>(a.mounts_fresh.entries);
    a.mounts.disks   = static_cast<Array<DiskEntry>&&>(a.mounts_fresh.disks);
    a.mounts.read_ns = a.mounts_fresh.read_ns;
    a.mounts_fresh.entries = Array<MountEntry>();
    a.mounts_fresh.disks   = Array<DiskEntry>();
    for (MountEntry& m : a.mounts.entries) {
      if (!m.mounted) continue;
      MountEntry* copy = (MountEntry*)malloc(sizeof(MountEntry));
      *copy = m;
      if (worker_submit(a.pool, job_mounts_stat, copy, JOB_MOUNTS_STAT, 0)) a.mounts_stats_pending++;
      else free(copy);
    }
    if (a.debug) fprintf(stderr, "mounts: %u disks, %u volumes read in %.2f ms on a worker\n", a.mounts.disks.len, a.mounts.entries.len, a.mounts.read_ns / 1e6);
    if (a.mounts_again) { a.mounts_again = false; mounts_request_refresh(a); }
  } else if (j.kind == JOB_MOUNTS_STAT) {
    MountEntry* r = (MountEntry*)j.ctx;
    a.mounts_stats_pending--;
    mounts_merge_stat(a.mounts, *r);
    if (a.debug) fprintf(stderr, "mounts: statvfs %s %s in %lld us\n", r->mountpoint, r->stat_failed ? "failed" : "ok", (long long)(r->stat_ns / 1000));
    free(r);
  }
  a.w.need_redraw = true;
}

void pool_drain(App& a) {
  if (a.pool.running) worker_ack_wake(a.pool);
  Job j;
  while (worker_poll(a.pool, j)) mounts_job_done(a, j);
}

static void write_ppm(const char* out, const Canvas& c) {
  FILE* f = fopen(out, "wb");
  if (!f) { perror(out); return; }
  fprintf(f, "P6\n%d %d\n255\n", c.w, c.h);
  for (i32 i = 0; i < c.w * c.h; i++) { u8 rgb[3] = { (u8)(c.px[i] >> 16), (u8)(c.px[i] >> 8), (u8)c.px[i] }; fwrite(rgb, 1, 3, f); }
  fclose(f);
}

struct ScriptStep { const char* spec; };
static u32 g_script_n = 0;
static ScriptStep g_script[64];

static void script_key(App& a, Canvas& c, u32 ks, u32 mods, const char* text, u32 text_len) {
  WEvent e = {};
  e.type = WE_KEY; e.pressed = 1;
  e.keysym = ks; e.mods[0] = mods;
  if (text_len) { memcpy(e.text, text, mx_min(text_len, (u32)sizeof e.text)); e.text_len = (u8)text_len; }
  a.w.xkb.ok = true; a.w.xkb.mask_alt = XKB_MOD_MOD1; // enough of a keymap for shell_key
  shell_key(a, e);
  shell_frame(a, c);
}

static bool run_script_step(App& a, Canvas& c, float scale, const char* spec) {
  if (!strncmp(spec, "click ", 6) || !strncmp(spec, "dclick ", 7) || !strncmp(spec, "mclick ", 7) || !strncmp(spec, "rclick ", 7)) {
    int x, y; char modw[64] = {};
    const char* nums = strchr(spec, ' ') + 1;
    if (sscanf(nums, "%d,%d %63s", &x, &y, modw) < 2) return false;
    u32 mods = 0; // "shift", "ctrl", "ctrl+shift", "alt"
    if (strstr(modw, "shift")) mods |= XKB_MOD_SHIFT;
    if (strstr(modw, "ctrl")) mods |= XKB_MOD_CONTROL;
    if (strstr(modw, "alt")) mods |= XKB_MOD_MOD1;
    float fx = (float)x * scale, fy = (float)y * scale;
    u32 button = spec[0] == 'm' ? 0x112 : spec[0] == 'r' ? 0x111 : 0x110;
    u32 times = spec[0] == 'd' ? 2 : 1;
    for (u32 k = 0; k < times; k++) {
      ui_input_motion(a.ui, fx, fy, a.ui.now_ms);
      ui_input_button(a.ui, fx, fy, button, true, mods, a.ui.now_ms);
      ui_input_button(a.ui, fx, fy, button, false, mods, a.ui.now_ms);
      shell_frame(a, c);
    }
    return true;
  }
  if (!strncmp(spec, "wheel ", 6)) {
    int x, y, dy; char modw[64] = {};
    if (sscanf(spec + 6, "%d,%d %d %63s", &x, &y, &dy, modw) < 3) return false;
    u32 mods = 0;
    if (strstr(modw, "shift")) mods |= XKB_MOD_SHIFT;
    if (strstr(modw, "ctrl")) mods |= XKB_MOD_CONTROL;
    ui_input_motion(a.ui, (float)x * scale, (float)y * scale, a.ui.now_ms);
    ui_input_scroll(a.ui, (float)x * scale, (float)y * scale, 0, (float)dy * 10.0f * scale, mods);
    shell_frame(a, c);
    return true;
  }
  if (!strncmp(spec, "key ", 4)) {
    WEvent e = {};
    e.type = WE_KEY; e.pressed = 1;
    const char* p = spec + 4;
    u32 mods = 0;
    for (;;) {
      if (!strncmp(p, "ctrl+", 5)) { mods |= XKB_MOD_CONTROL; p += 5; }
      else if (!strncmp(p, "shift+", 6)) { mods |= XKB_MOD_SHIFT; p += 6; }
      else if (!strncmp(p, "alt+", 4)) { mods |= XKB_MOD_MOD1; p += 4; }
      else break;
    }
    u32 ks;
    if (!keysym_from_name(p, ks)) { fprintf(stderr, "script: unknown keysym %s\n", p); return false; }
    char text[8]; u32 text_len = 0;
    if (!(mods & XKB_MOD_CONTROL)) { u32 cp = keysym_to_utf32(ks); if (cp >= 0x20 && cp != 0x7f) text_len = keysym_to_utf8(ks, text); }
    (void)e;
    script_key(a, c, ks, mods, text, text_len);
    return true;
  }
  if (!strncmp(spec, "type ", 5)) { // each character as a key with text (ASCII)
    for (const char* p = spec + 5; *p; p++) { char ch = *p; script_key(a, c, (u32)(u8)ch, 0, &ch, 1); }
    return true;
  }
  if (!strcmp(spec, "wait")) {
    shell_timers(a, a.ui.now_ms);
    shell_frame(a, c);
    return true;
  }
  if (!strncmp(spec, "conflicts ", 10)) { // the policy inline jobs use for conflicts
    const char* p = spec + 10;
    a.ops.inline_policy = !strcmp(p, "replace") ? CONFLICT_REPLACE : !strcmp(p, "keepboth") ? CONFLICT_KEEP_BOTH : !strcmp(p, "cancel") ? CONFLICT_CANCEL : CONFLICT_SKIP;
    return true;
  }
  if (!strcmp(spec, "dialog conflict")) {
    Dialog& d = a.dialog;
    d.kind = DLG_CONFLICT; d.job = nullptr;
    snprintf(d.title, sizeof d.title, "A file already exists");
    d.nlines = 0;
    snprintf(d.lines[d.nlines++], sizeof d.lines[0], "\"report.pdf\" in Documents");
    snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Source: 2.4 MB, modified 12 Sep 2026 14:02");
    snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Existing: 2.1 MB, modified 3 Aug 2026 09:15");
    d.nbuttons = 0; d.buttons[d.nbuttons++] = "Replace"; d.buttons[d.nbuttons++] = "Skip"; d.buttons[d.nbuttons++] = "Keep both"; d.buttons[d.nbuttons++] = "Cancel";
    d.focus = 1; d.cancel = 3; d.hover = -1; d.check_label = "Do this for all conflicts"; d.checked = false;
    shell_frame(a, c);
    return true;
  }
  if (!strcmp(spec, "dialog delete")) { op_delete(a, tab_active(a), false); shell_frame(a, c); return true; }
  if (!strcmp(spec, "settings")) { settings_open(a); shell_frame(a, c); return true; }
  if (!strncmp(spec, "menu place ", 11)) { menu_open_place(a, spec + 11, false, 20, 300); shell_frame(a, c); return true; }
  if (!strcmp(spec, "dialog error")) {
    Dialog& d = a.dialog;
    d.kind = DLG_OP_ERROR; d.job = nullptr;
    snprintf(d.title, sizeof d.title, "Cannot copy report.pdf");
    d.nlines = 0;
    snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Permission denied");
    snprintf(d.lines[d.nlines++], sizeof d.lines[0], "In /home/user/Documents/2026");
    snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Try again, leave it out and carry on, or stop?");
    d.nbuttons = 0; d.buttons[d.nbuttons++] = "Retry"; d.buttons[d.nbuttons++] = "Skip"; d.buttons[d.nbuttons++] = "Skip all"; d.buttons[d.nbuttons++] = "Cancel";
    d.focus = 0; d.cancel = 3; d.hover = -1; d.check_label = nullptr;
    shell_frame(a, c);
    return true;
  }
  if (!strcmp(spec, "dialog sudo")) {
    Tab& t = tab_active(a);
    OpJob j = {};
    j.req.kind = OP_CHMOD; j.req.mode_mask = 04; j.req.mode_value = 0; j.req.recursive = false;
    if (t.cursor >= 0 && (u32)t.cursor < t.rows.len) { char p[4096]; entry_path(t, t.rows[(u32)t.cursor], p, sizeof p); j.req.add(p); snprintf(j.label, sizeof j.label, "%s", path_base(p)); }
    else { j.req.add("/etc/hostname"); snprintf(j.label, sizeof j.label, "hostname"); }
    j.needs_password = true;
    sudo_dialog(a, &j);
    j.req.paths.release(); j.req.off.release();
    shell_frame(a, c);
    return true;
  }
  if (!strcmp(spec, "menu")) { Tab& t = tab_active(a); menu_open(a, t, t.cursor, -1, -1); shell_frame(a, c); return true; }
  if (!strcmp(spec, "menu openwith")) { Tab& t = tab_active(a); menu_open_with(a, t, t.cursor, -1, -1); shell_frame(a, c); return true; }
  if (!strncmp(spec, "menu device ", 12)) { menu_open_device(a, atoi(spec + 12), -1, 20, 300); shell_frame(a, c); return true; }
  if (!strncmp(spec, "menu disk ", 10)) { menu_open_device(a, -1, atoi(spec + 10), 20, 300); shell_frame(a, c); return true; }
  if (!strcmp(spec, "open")) { open_files(a, tab_active(a), -1); shell_frame(a, c); return true; }
  if (!strcmp(spec, "restore")) { op_restore(a, tab_active(a)); shell_frame(a, c); return true; }
  if (!strcmp(spec, "empty-trash")) { op_empty_trash(a, tab_active(a), false); shell_frame(a, c); return true; }
  if (!strcmp(spec, "terminal")) { open_terminal(a, tab_active(a).path); shell_frame(a, c); return true; }
  if (!strncmp(spec, "drop ", 5)) {
    int x, y; char rest[4096] = {};
    if (sscanf(spec + 5, "%d,%d %4095[^\n]", &x, &y, rest) < 3) return false;
    bool mv = false; if (char* sp = strstr(rest, " move")) { *sp = 0; mv = true; }
    char target[4096]; i32 row; Rect side;
    bool have = dnd_target_at(a, (float)x * scale, (float)y * scale, target, sizeof target, &row, &side);
    printf("  drop at %d,%d -> %s (row %d%s)\n", x, y, have ? target : "nowhere", row, side.w ? ", sidebar" : "");
    if (have) { Clip paths; for (char* p = strtok(rest, ";"); p; p = strtok(nullptr, ";")) paths.add(p); dnd_drop_paths(a, target, paths, mv ? WL_DND_MOVE : WL_DND_COPY); }
    shell_frame(a, c);
    return true;
  }
  if (!strncmp(spec, "dragover ", 9)) {
    int x, y; if (sscanf(spec + 9, "%d,%d", &x, &y) != 2) return false;
    a.w.dnd_mimes = WCLIP_URI_LIST;
    dnd_event(a, WE_DND_ENTER, (float)x * scale, (float)y * scale);
    printf("  drag over %d,%d -> %s (row %d%s)\n", x, y, a.dnd_target[0] ? a.dnd_target : "nowhere", a.dnd_row, a.dnd_rect.w ? ", sidebar" : "");
    shell_frame(a, c);
    return true;
  }
  if (!strcmp(spec, "props") || !strcmp(spec, "props folder")) { props_open(a, tab_active(a), spec[5] != 0); shell_frame(a, c); return true; }
  if (!strncmp(spec, "perm ", 5)) {
    Props& p = a.props;
    for (const char* q = spec + 5; *q;) {
      while (*q == ' ' || *q == ',') q++;
      if (!*q) break;
      u32 who = 0;
      for (; *q && *q != '+' && *q != '-'; q++) who |= *q == 'u' ? 0700 : *q == 'g' ? 070 : *q == 'o' ? 07 : *q == 'a' ? 0777 : 0;
      bool set = *q == '+'; if (*q) q++;
      u32 bits = 0;
      for (; *q && *q != ' ' && *q != ','; q++) bits |= *q == 'r' ? 0444 : *q == 'w' ? 0222 : *q == 'x' ? 0111 : *q == 's' ? 06000 : *q == 't' ? 01000 : 0;
      u32 m = (bits & who) | (bits & 07000);
      p.new_mode = set ? p.new_mode | m : p.new_mode & ~m;
    }
    shell_frame(a, c);
    return true;
  }
  if (!strcmp(spec, "recursive")) { a.props.recursive = !a.props.recursive; shell_frame(a, c); return true; }
  if (!strncmp(spec, "owner ", 6)) { ui_text_set(a.props.owner_in, spec + 6); shell_frame(a, c); return true; }
  if (!strncmp(spec, "group ", 6)) { ui_text_set(a.props.group_in, spec + 6); shell_frame(a, c); return true; }
  if (!strcmp(spec, "take")) { ui_text_set(a.props.owner_in, a.props.my_name); ui_text_set(a.props.group_in, a.props.my_group); shell_frame(a, c); return true; }
  if (!strcmp(spec, "progress")) { // a running copy, halfway, for the strip
    OpJob* j = (OpJob*)calloc(1, sizeof(OpJob));
    j->req.kind = OP_COPY; snprintf(j->req.dest, sizeof j->req.dest, "/home/user/Documents");
    j->state = JOB_RUNNING; j->prog.phase = OP_PHASE_RUN;
    j->prog.bytes_done = 96u << 20; j->prog.bytes_total = 200u << 20; j->prog.items_done = 12; j->prog.items_total = 40;
    snprintf((char*)j->prog.current, sizeof j->prog.current, "photos-2026-08.tar");
    snprintf(j->label, sizeof j->label, "40 items"); snprintf(j->where, sizeof j->where, "Documents");
    j->started_ms = a.ui.now_ms - 1200; j->rate_ms = a.ui.now_ms - 1200; j->rate = 80.0f * 1048576.0f;
    a.jobs.push(j);
    shell_frame(a, c);
    return true;
  }
  if (!strncmp(spec, "press ", 6) || !strncmp(spec, "release", 7)) {
    int x = 0, y = 0; char modw[64] = {};
    bool press = spec[0] == 'p';
    if (press && sscanf(spec + 6, "%d,%d %63s", &x, &y, modw) < 2) return false;
    if (!press) sscanf(spec + 7, " %63s", modw);
    u32 mods = 0;
    if (strstr(modw, "shift")) mods |= XKB_MOD_SHIFT;
    if (strstr(modw, "ctrl")) mods |= XKB_MOD_CONTROL;
    float fx = press ? (float)x * scale : a.ui.mx, fy = press ? (float)y * scale : a.ui.my;
    if (press) ui_input_motion(a.ui, fx, fy, a.ui.now_ms);
    ui_input_button(a.ui, fx, fy, 0x110, press, mods, a.ui.now_ms);
    shell_frame(a, c);
    return true;
  }
  if (!strncmp(spec, "move ", 5)) {
    int x, y;
    if (sscanf(spec + 5, "%d,%d", &x, &y) != 2) return false;
    ui_input_motion(a.ui, (float)x * scale, (float)y * scale, a.ui.now_ms);
    shell_frame(a, c);
    return true;
  }
  fprintf(stderr, "script: unknown step \"%s\"\n", spec);
  return false;
}

static int snapshot(App& a, const char* const* starts, u32 nstarts, const char* out, i32 sw, i32 sh, float scale, i32 view, const char* filter, bool path_edit) {
  Window& w = a.w;
  w.logical_w = sw; w.logical_h = sh;
  w.buf_w = (i32)((float)sw * scale + 0.5f); w.buf_h = (i32)((float)sh * scale + 0.5f);
  w.focused = true;
  load_places(a);
  pins_load(a);
  for (u32 i = 0; i < nstarts; i++) tab_open(a, starts[i], i == 0);
  if (!a.num_tabs) { fprintf(stderr, "mattexplorer: could not open %s\n", starts[0]); return 1; }
  Tab& t = tab_active(a);
  if (view >= 0) set_view(a, t, (ViewMode)view);
  t.cursor = t.anchor = mx_min(2, (i32)t.rows.len - 1);
  if (t.cursor >= 0) { t.selected[t.rows[(u32)t.cursor]] = 1; t.selected_count = 1; }
  if (filter) { ui_text_set(t.filter, filter); filter_changed(a, t); }
  if (path_edit) { a.path_editing = true; ui_text_set(a.path_input, t.path); a.path_input.sel_anchor = 0; }

  if (a.demo_devices) mounts_demo(a.mounts);
  else {
    mounts_read(a.mounts);
    for (MountEntry& m : a.mounts.entries) mounts_stat_one(m);
    mounts_aggregate(a.mounts);
  }
  u32* px = (u32*)calloc((usize)w.buf_w * w.buf_h, 4);
  Canvas c = { px, w.buf_w, w.buf_h, w.buf_w, 0, 0, w.buf_w, w.buf_h };
  a.ui.mx = (float)(w.buf_w * 0.6f); a.ui.my = (float)(w.buf_h * 0.42f); a.ui.mouse_inside = true; // a hover row
  i64 t0 = now_ns();
  shell_frame(a, c);
  a.draw_ns = now_ns() - t0;
  t0 = now_ns();
  shell_frame(a, c); // second frame: everything cached
  i64 warm = now_ns() - t0;
  for (u32 i = 0; i < g_script_n; i++) {
    a.ui.now_ms += 1000;
    if (!run_script_step(a, c, scale, g_script[i].spec)) return 2;
  }
  if (g_script_n) {
    Tab& tl = tab_active(a);
    printf("script: %u steps -> %u tabs, active \"%s\", %u rows, cursor %d, %u selected, filter \"%s\", view %s%s%s%s\n", g_script_n,
           a.num_tabs, tl.path, tl.rows.len, tl.cursor, tl.selected_count, tl.filter.buf,
           tl.view == VIEW_ICONS ? "icons" : tl.view == VIEW_LIST ? "list" : "details", a.path_editing ? ", editing path" : "",
           a.band_active ? ", rubber band" : "", a.tab_dragging ? ", dragging a tab" : "");
    if (a.num_tabs > 1) { printf("  tabs:"); for (u32 i = 0; i < a.num_tabs; i++) printf(" [%s%s]", tab_title(a.tabs[a.tab_order[i]]), i == a.active ? "*" : ""); printf("\n"); }
    printf("  ops: %u done, %u failed, %u jobs, %u undo, %u redo, clip %u%s, dialog %s, menu %s, renaming %s, %u launched, %u drops%s\n", a.ops_done, a.ops_failed, a.jobs.len, a.undo_count, a.redo_count,
           a.clip.count(), a.clip.cut ? " (cut)" : "",
           a.dialog.kind == DLG_NONE ? "none" : a.dialog.kind == DLG_CONFLICT ? "conflict" : a.dialog.kind == DLG_CONFIRM_DELETE ? "confirm-delete" : a.dialog.kind == DLG_TRASH_FAILED ? "trash-failed" : a.dialog.kind == DLG_PROPERTIES ? "properties" : a.dialog.kind == DLG_SUDO ? "sudo" : a.dialog.kind == DLG_OP_ERROR ? "op-error" : a.dialog.kind == DLG_SETTINGS ? "settings" : "error",
           a.menu.open ? (a.menu.kind == MENU_OPEN_WITH ? "open-with" : a.menu.kind == MENU_DEVICE ? "device" : a.menu.kind == MENU_PLACE ? "place" : "open") : "closed", tl.renaming >= 0 && (u32)tl.renaming < tl.listing.count() ? tl.listing.cname((u32)tl.renaming) : "-",
           a.launches, a.drops, in_trash(tl) ? ", in the trash" : "");
    if (a.menu.open) { printf("  menu:"); for (u32 i = 0; i < a.menu.count; i++) if (!a.menu.items[i].separator) printf(" [%s%s]", a.menu.items[i].label, a.menu.items[i].enabled ? "" : " (off)"); printf("\n"); }
    if (tl.trash_cols) { printf("  trash columns:"); for (u32 i = 0; i < tl.listing.count(); i++) printf(" [%s <- %s @%lld]", tl.listing.cname(i), trash_original(tl, i), (long long)(i < tl.trash_deleted.len ? tl.trash_deleted[i] : 0)); printf("\n"); }
    if (a.theme_synced) printf("  theme: %s (%s)%s\n", a.theme_name, a.theme_is_light ? "light" : "dark", a.cfg.theme_follow ? "" : ", overridden");
    printf("  settings: sidebar %s (%s), hidden %d, dirs_first %d, thumbnails %d, font %g, theme %s, terminal \"%s\"\n", a.cfg.sidebar_visible ? "on" : "off", a.L.sidebar ? "shown" : "rail",
           a.cfg.show_hidden, a.cfg.sort.dirs_first, a.cfg.thumbnails, (double)a.cfg.font_px, a.cfg.theme_follow ? "omarchy" : a.cfg.light ? "light" : "dark", a.cfg.terminal);
    if (a.thumbs.generated || a.thumbs.cached || a.thumbs.failed || a.thumbs.hits) printf("  thumbs: %u entries, %u generated, %u from the disk cache, %u failed, %u hits, %.1f MB\n", a.thumbs.entries.len, a.thumbs.generated, a.thumbs.cached, a.thumbs.failed, a.thumbs.hits, a.thumbs.bytes / 1048576.0);
    if (a.dialog.kind != DLG_NONE) printf("  dialog: %s%s\n", a.dialog.title, a.dialog.kind == DLG_SUDO ? " (password field)" : "");
    if (a.dialog.kind == DLG_SUDO) printf("  password field: %s, %u pending paths\n", a.dialog.password.len ? "filled" : "empty", a.dialog.pending.count());
    if (a.dialog.kind == DLG_PROPERTIES) {
      const Props& p = a.props;
      printf("  props: %u items (%u files, %u dirs, %u links), mode %04o -> %04o%s, %s, owner %s:%s -> %s:%s%s, field %d, %u users %u groups, size %llu, scan %s: %u files %u dirs %llu bytes\n", p.count,
             p.n_files, p.n_dirs, p.n_links, p.mode, p.new_mode, p.modes_differ ? " (differ)" : "", p.recursive ? "recursive" : "not recursive", p.owner, p.group,
             p.owner_in.buf, p.group_in.buf, p.foreign ? " (foreign)" : "", p.focus_field, p.users.count(), p.groups.count(),
             (unsigned long long)p.size, p.scan ? "running" : p.scanned ? "done" : p.scan_cancelled ? "cancelled" : "none", p.tot_files, p.tot_dirs, (unsigned long long)p.tot_bytes);
    }
    if (a.debug) printf("  timings: list %.2f ms, sort %.2f ms\n", tl.list_ns / 1e6, tl.sort_ns / 1e6);
    if (a.status_msg[0]) printf("  status: %s\n", a.status_msg);
  }
  write_ppm(out, c);
  usize cache = 0;
  for (u32 i = 0; i < a.text.num_fonts; i++) cache += font_cache_bytes(a.text.fonts[i]);
  printf("snapshot: %s %dx%d @%.2fx, %u tabs, %u rows, %u disks, first draw %.2f ms, warm draw %.2f ms, %u fonts, glyph cache %.1f KB\n", out,
         c.w, c.h, scale, a.num_tabs, t.rows.len, a.mounts.disks.len, a.draw_ns / 1e6, warm / 1e6, a.text.num_fonts, cache / 1024.0);
  free(px);
  return 0;
}

static int layout_test(App& a, const char* start) {
  Window& w = a.w;
  w.focused = true;
  load_places(a);
  if (tab_open(a, start, true) < 0) { fprintf(stderr, "mattexplorer: could not open %s\n", start); return 1; }
  Tab& t = tab_active(a);
  static const struct { i32 w, h; float scale; ViewMode view; } steps[] = {
    { 1100, 700, 1.0f, VIEW_DETAILS }, { 900, 600, 1.0f, VIEW_DETAILS }, { 640, 480, 1.0f, VIEW_DETAILS }, { 400, 300, 1.0f, VIEW_DETAILS },
    { 320, 240, 1.0f, VIEW_DETAILS }, { 1400, 900, 1.0f, VIEW_DETAILS }, { 1400, 900, 1.5f, VIEW_DETAILS }, { 1400, 900, 2.0f, VIEW_DETAILS },
    { 1100, 700, 1.25f, VIEW_DETAILS }, { 1100, 700, 1.0f, VIEW_ICONS }, { 700, 500, 1.0f, VIEW_ICONS }, { 1300, 800, 1.5f, VIEW_ICONS },
    { 1100, 700, 1.0f, VIEW_LIST }, { 600, 400, 1.0f, VIEW_LIST }, { 1300, 800, 1.5f, VIEW_LIST }, { 1100, 700, 1.0f, VIEW_DETAILS },
  };
  u32 fails = 0, frames = 0;
  i64 total_ns = 0, worst_ns = 0;
  u32* px = nullptr; usize px_cap = 0;
  i32 expect_first = -1;
  ViewMode last_view = VIEW_DETAILS;
  for (u32 si = 0; si < MX_ARRAY_COUNT(steps); si++) {
    w.logical_w = steps[si].w; w.logical_h = steps[si].h;
    w.buf_w = (i32)((float)steps[si].w * steps[si].scale + 0.5f); w.buf_h = (i32)((float)steps[si].h * steps[si].scale + 0.5f);
    usize need = (usize)w.buf_w * w.buf_h;
    if (need > px_cap) { free(px); px = (u32*)calloc(need, 4); px_cap = need; }
    Canvas c = { px, w.buf_w, w.buf_h, w.buf_w, 0, 0, w.buf_w, w.buf_h };
    if (steps[si].view != last_view) { set_view(a, t, steps[si].view); last_view = steps[si].view; expect_first = -1; }
    if (si == 0 && t.rows.len > 40) { // scroll into the middle and put the cursor there
      shell_frame(a, c);
      t.cursor = t.anchor = 30; t.selected[t.rows[30]] = 1; t.selected_count = 1;
      t.scroll = 20 * a.L.row_h;
    }
    i64 t0 = now_ns();
    shell_frame(a, c);
    i64 dt = now_ns() - t0;
    total_ns += dt; worst_ns = mx_max(worst_ns, dt); frames++;
    const Layout& L = a.L;
    bool ok = true;
    if (L.list.x < 0 || rect_x1(L.list) > c.w || L.list.y < 0 || rect_y1(L.list) > c.h) ok = false;
    if (L.sidebar && L.side.w > c.w / 2) ok = false;
    if (t.scroll < 0 || t.scroll_x < 0) ok = false;
    i32 cw, ch; content_size(a, t, &cw, &ch);
    if (t.scroll > mx_max(ch - L.list.h, 0) || t.scroll_x > mx_max(cw - L.list.w, 0)) ok = false;

    if (expect_first >= 0) {
      i32 span = t.view == VIEW_ICONS ? L.cols : t.view == VIEW_LIST ? L.rows_per_col : 1;
      bool same_top = expect_first >= t.first_vis && expect_first < t.first_vis + span;
      bool at_end = t.scroll >= mx_max(ch - L.list.h, 0) && t.scroll_x >= mx_max(cw - L.list.w, 0);
      bool cursor_pinned = false;
      if (t.cursor >= 0) {
        Rect cr = cell_rect(a, t, t.cursor);
        cursor_pinned = t.view == VIEW_LIST ? rect_x1(cr) > rect_x1(L.list) - L.lcol_w : rect_y1(cr) > rect_y1(L.list) - cr.h;
      }
      if (!same_top && !at_end && !cursor_pinned) { ok = false; fprintf(stderr, "  step %u: first visible row %d, expected %d\n", si, t.first_vis, expect_first); }
    }
    if (t.cursor >= 0 && t.keep_first_vis) {
      Rect cr = cell_rect(a, t, t.cursor);
      if (cr.y < L.list.y || rect_y1(cr) > rect_y1(L.list)) { ok = false; fprintf(stderr, "  step %u: cursor row scrolled out of view\n", si); }
    }
    printf("  %4dx%-4d @%.2fx %-7s side %-3d list %4dx%-4d cols date=%d type=%d first=%-4d scroll=%-5d cursor=%d %s (%.2f ms)\n",
           steps[si].w, steps[si].h, steps[si].scale, steps[si].view == VIEW_ICONS ? "icons" : steps[si].view == VIEW_LIST ? "list" : "details",
           L.sidebar ? L.side.w : 0, L.list.w, L.list.h, L.col_date, L.col_type, t.first_vis, t.scroll, t.cursor, ok ? "ok" : "FAIL", dt / 1e6);
    if (!ok) fails++;
    expect_first = t.first_vis;
  }
  free(px);
  printf("layout-test: %u frames, %u failures, mean %.2f ms, worst %.2f ms\n", frames, fails, total_ns / 1e6 / frames, worst_ns / 1e6);
  return fails ? 1 : 0;
}

static bool write_file(const char* path, u32 bytes, char fill) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  char buf[4096]; memset(buf, fill, sizeof buf);
  for (u32 o = 0; o < bytes; o += sizeof buf) fwrite(buf, 1, mx_min((u32)sizeof buf, bytes - o), f);
  fclose(f);
  return true;
}

static int ops_test(App& a, const char* dir) {
  char p[4096], q[4096];
  auto fail = [&](const char* what) { fprintf(stderr, "ops-test: FAIL: %s (%s)\n", what, strerror(errno)); return 1; };
  snprintf(p, sizeof p, "%s/src", dir); mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/src/sub", dir); mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/dest", dir); mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/src/a.bin", dir); if (!write_file(p, 3u << 20, 'a')) return fail("write a.bin");
  snprintf(p, sizeof p, "%s/src/sub/s.txt", dir); if (!write_file(p, 100, 's')) return fail("write s.txt");
  snprintf(p, sizeof p, "%s/dest/a.bin", dir); if (!write_file(p, 10, 'z')) return fail("write dest/a.bin");
  a.w.focused = true;
  if (!worker_start(a.pool, 2)) return fail("worker_start");
  a.sync_stat = false;
  for (u32 i = 0; i < STAT_POOL; i++) a.stat_pool[i] = (StatBatch*)calloc(1, sizeof(StatBatch));
  a.stat_free = (1u << STAT_POOL) - 1;
  if (!ops_start(a)) return fail("ops_start");
  bool have_watch = watch_open(a.watcher);
  snprintf(p, sizeof p, "%s/dest", dir);
  if (tab_open(a, p, true) < 0) return fail("tab_open");
  Tab& t = tab_active(a);
  u32 conflicts = 0, polls = 0, drains = 0, errors_asked = 0;
  static u32 s_error_mode; // 0: skip, 1: retry once then skip all
  static u32 s_error_seen;
  s_error_mode = 0; s_error_seen = 0;
  auto pump = [&](i64 budget_ms, bool (*done)(App&)) -> bool {
    i64 end = window_now_ms() + budget_ms;
    while (!done(a)) {
      i64 now = window_now_ms();
      if (now >= end) return false;
      i64 wake = shell_next_wake(a);
      int timeout = (int)mx_clamp(wake ? wake - now : (i64)50, (i64)1, (i64)50);
      struct pollfd fds[3]; u32 n = 0;
      fds[n++] = { a.ops.wake_fd, POLLIN, 0 };
      fds[n++] = { a.pool.wake_fd, POLLIN, 0 };
      if (have_watch) fds[n++] = { a.watcher.fd, POLLIN, 0 };
      poll(fds, n, timeout);
      now = window_now_ms();
      a.ui.now_ms = now;
      if (fds[0].revents & POLLIN) { ops_poll(a); polls++; }
      if (fds[1].revents & POLLIN) pool_drain(a);
      if (have_watch && (fds[2].revents & POLLIN)) { watch_drain(a.watcher, now); drains++; }
      if (a.dialog.kind == DLG_CONFLICT) { conflicts++; ops_answer(a, CONFLICT_KEEP_BOTH, false); }
      if (a.dialog.kind == DLG_OP_ERROR) {
        errors_asked++; s_error_seen++;
        if (s_error_mode == 1 && s_error_seen == 1) ops_answer_error(a, ERR_RETRY, false);
        else ops_answer_error(a, ERR_SKIP, s_error_mode == 1);
      }
      shell_timers(a, now);

      for (Tab& ti : a.tabs) if (ti.used) for (u32 i = 0; i < ti.listing.count(); i++) stat_request(a, ti, i);
      stat_flush(a);
    }
    return true;
  };

  FileOpRequest r;
  r.kind = OP_COPY; snprintf(r.dest, sizeof r.dest, "%s/dest", dir);
  snprintf(p, sizeof p, "%s/src/a.bin", dir); r.add(p);
  snprintf(p, sizeof p, "%s/src/sub", dir); r.add(p);
  ops_submit(a, r, UNDO_COPY, "2 items");
  if (!pump(10000, [](App& x) { return x.jobs.len == 0; })) return fail("copy did not finish");
  if (conflicts != 1) { fprintf(stderr, "ops-test: FAIL: %u conflicts, expected 1\n", conflicts); return 1; }
  snprintf(p, sizeof p, "%s/dest/a (2).bin", dir); struct stat st;
  if (stat(p, &st) != 0 || st.st_size != (3 << 20)) return fail("a (2).bin missing");
  snprintf(p, sizeof p, "%s/dest/sub/s.txt", dir); if (stat(p, &st) != 0) return fail("sub/s.txt missing");
  if (a.undo_count != 1) { fprintf(stderr, "ops-test: FAIL: undo_count %u\n", a.undo_count); return 1; }
  bool seen = false;
  for (u32 i = 0; i < t.listing.count(); i++) seen |= !strcmp(t.listing.cname(i), "a (2).bin");
  if (!seen) return fail("tab not refreshed after the copy");
  if (t.selected_count != 2) { fprintf(stderr, "ops-test: FAIL: %u selected after the paste, expected 2\n", t.selected_count); return 1; }

  snprintf(p, sizeof p, "%s/dest/outside.txt", dir); if (!write_file(p, 5, 'o')) return fail("write outside.txt");
  bool watched = have_watch && pump(3000, [](App& x) { Tab& tt = tab_active(x); for (u32 i = 0; i < tt.listing.count(); i++) if (!strcmp(tt.listing.cname(i), "outside.txt")) return true; return false; });
  if (have_watch && !watched) return fail("inotify refresh did not show outside.txt");

  if (!pump(3000, [](App& x) { Tab& tt = tab_active(x); for (u32 i = 0; i < tt.listing.count(); i++) if (!(tt.listing.flags[i] & (EF_STATTED | EF_STAT_FAILED))) return false; return true; }))
    return fail("stat batches did not complete");

  op_undo(a);
  if (!pump(10000, [](App& x) { return x.jobs.len == 0; })) return fail("undo did not finish");
  snprintf(p, sizeof p, "%s/dest/a (2).bin", dir); if (stat(p, &st) == 0) return fail("undo left a (2).bin");
  snprintf(p, sizeof p, "%s/dest/sub", dir); if (stat(p, &st) == 0) return fail("undo left sub");

  snprintf(p, sizeof p, "%s/src/big.bin", dir); if (!write_file(p, 64u << 20, 'b')) return fail("write big.bin");
  FileOpRequest r2; r2.kind = OP_COPY; snprintf(r2.dest, sizeof r2.dest, "%s/dest", dir); r2.add(p);
  OpJob* j = ops_submit(a, r2, UNDO_COPY, "big.bin");
  ops_cancel(a, j);
  if (!pump(10000, [](App& x) { return x.jobs.len == 0; })) return fail("cancelled copy did not finish");
  snprintf(p, sizeof p, "%s/dest/big.bin", dir);
  bool partial = stat(p, &st) == 0 && st.st_size == (64 << 20);
  snprintf(q, sizeof q, "%s/dest/.big.bin.mxtmp", dir);
  if (stat(q, &st) == 0) return fail("cancel left a temp file");

  FileOpRequest r3; r3.kind = OP_TRASH; snprintf(p, sizeof p, "%s/dest/outside.txt", dir); r3.add(p);
  ops_submit(a, r3, UNDO_TRASH, "outside.txt");
  if (!pump(10000, [](App& x) { return x.jobs.len == 0; })) return fail("trash did not finish");
  bool trashed = stat(p, &st) != 0;
  op_undo(a);
  if (!pump(10000, [](App& x) { return x.jobs.len == 0; })) return fail("restore did not finish");
  bool restored = stat(p, &st) == 0;

  props_open(a, t, true);
  if (a.dialog.kind != DLG_PROPERTIES) return fail("properties dialog did not open");
  if (!pump(10000, [](App& x) { return x.props.scan == nullptr; })) return fail("properties scan did not finish");
  bool props_ok = a.props.scanned && a.props.tot_files >= 2;
  u32 props_files = a.props.tot_files, props_dirs = a.props.tot_dirs;
  dialog_close(a);
  props_open(a, t, true);
  dialog_close(a); // cancels the walk; the pool still returns it
  pump(2000, [](App& x) { return x.pool.completed == x.pool.submitted; });

  snprintf(p, sizeof p, "%s/src", dir);
  FileOpRequest r4; r4.kind = OP_CHMOD; r4.add(p); r4.mode_mask = 0002; r4.mode_value = 0002; r4.recursive = true;
  ops_submit(a, r4, UNDO_CHMOD, "src");
  if (!pump(10000, [](App& x) { return x.jobs.len == 0; })) return fail("chmod did not finish");
  snprintf(q, sizeof q, "%s/src/sub/s.txt", dir);
  bool chmod_ok = stat(q, &st) == 0 && (st.st_mode & 0002);
  op_undo(a);
  if (!pump(10000, [](App& x) { return x.jobs.len == 0; })) return fail("chmod undo did not finish");
  bool chmod_undone = stat(q, &st) == 0 && !(st.st_mode & 0002);
  printf("ops-test: props %s (%u files, %u folders), chmod %s, undone %s\n", props_ok ? "ok" : "FAILED", props_files, props_dirs,
         chmod_ok ? "ok" : "FAILED", chmod_undone ? "ok" : "FAILED");
  if (!props_ok || !chmod_ok || !chmod_undone) return 1;

  a.no_sudo_run = true;
  snprintf(p, sizeof p, "%s/src", dir);
  FileOpRequest r5; r5.kind = OP_CHMOD; r5.add(p); r5.mode_mask = 0001; r5.mode_value = 0001; r5.recursive = true;
  ops_submit(a, r5, UNDO_CHMOD, "src", false, true);
  if (!pump(10000, [](App& x) { return x.jobs.len == 0; })) return fail("helper chmod did not finish");
  snprintf(q, sizeof q, "%s/src/sub/s.txt", dir);
  bool helper_ok = stat(q, &st) == 0 && (st.st_mode & 0001) && a.undo_count && a.undo[a.undo_count - 1].elevated;
  u32 helper_pairs = a.undo_count ? a.undo[a.undo_count - 1].joff.len / 2 : 0;
  op_undo(a);
  if (!pump(10000, [](App& x) { return x.jobs.len == 0; })) return fail("helper undo did not finish");
  bool helper_undone = stat(q, &st) == 0 && !(st.st_mode & 0001);

  const char* chown_result = "skipped (not root)";
  if (geteuid() == 0) {
    FileOpRequest r7; r7.kind = OP_CHOWN; r7.add(p); r7.uid = 65534; r7.gid = -1; r7.recursive = true;
    ops_submit(a, r7, UNDO_CHOWN, "src", false, true);
    if (!pump(10000, [](App& x) { return x.jobs.len == 0; })) return fail("helper chown did not finish");
    bool given = stat(q, &st) == 0 && st.st_uid == 65534;
    op_undo(a);
    if (!pump(10000, [](App& x) { return x.jobs.len == 0; })) return fail("helper chown undo did not finish");
    bool back = stat(q, &st) == 0 && st.st_uid == 0;
    chown_result = given && back ? "ok" : given ? "FAILED (undo)" : "FAILED";
    if (!given || !back) return fail(chown_result);
  }
  a.no_sudo_run = false;

  bool have_sudo = access("/usr/bin/sudo", X_OK) == 0 || access("/bin/sudo", X_OK) == 0;
  const char* sudo_result = "not installed";
  if (have_sudo) {
    chmod(p, 0755);
    FileOpRequest r6; r6.kind = OP_CHMOD; r6.add(p); r6.mode_mask = 0002; r6.mode_value = 0002; r6.recursive = false;
    ops_submit(a, r6, UNDO_CHMOD, "src", false, true);
    if (!pump(15000, [](App& x) { return x.jobs.len == 0; })) return fail("sudo probe did not finish");
    if (a.dialog.kind == DLG_SUDO) {
      sudo_result = "asks for a password";
      const char* pw = getenv("MX_TEST_SUDO_PASSWORD"); // a test box with a throwaway user: complete the dialog
      if (pw && pw[0]) {
        ui_text_set(a.dialog.password, pw);
        dialog_confirm(a);
        if (!pump(15000, [](App& x) { return x.jobs.len == 0; })) return fail("sudo run did not finish");
        bool changed = stat(p, &st) == 0 && (st.st_mode & 0002);
        bool again = a.dialog.kind == DLG_SUDO; // a wrong password brings the dialog back
        sudo_result = changed ? "asked, then ran as root with the password; undoing as root" : again ? "asked, password refused, asked again" : "asked, then failed";
        if (again) dialog_close(a);
        if (changed) { op_undo(a); if (!pump(15000, [](App& x) { return x.jobs.len == 0; })) return fail("sudo undo did not finish"); if (stat(p, &st) == 0 && (st.st_mode & 0002)) sudo_result = "asked, ran, but the undo as root did not"; }
      } else dialog_close(a);
    } else sudo_result = stat(p, &st) == 0 && (st.st_mode & 0002) ? "ran as root without asking (root, or cached credentials)" : "refused";
    chmod(p, 0755);
  }
  printf("ops-test: helper %s (%u inodes journaled), undone %s, chown via helper %s, sudo %s\n", helper_ok ? "ok" : "FAILED", helper_pairs, helper_undone ? "ok" : "FAILED", chown_result, sudo_result);
  if (!helper_ok || !helper_undone) return 1;

  a.clip.clear(); a.clip.cut = true;
  snprintf(p, sizeof p, "%s/src/a b.txt", dir); if (!write_file(p, 3, 'q')) return fail("write a b.txt");
  a.clip.add(p);
  int cp[2]; if (pipe2(cp, O_CLOEXEC) != 0) return fail("pipe");
  clip_send(a, cp[1], "x-special/gnome-copied-files", a.clip);
  char got[1024] = {}; u32 gl = 0;
  { i64 end = window_now_ms() + 5000; for (;;) { struct pollfd pf = { cp[0], POLLIN, 0 }; if (poll(&pf, 1, 100) > 0) { ssize_t n = read(cp[0], got + gl, sizeof got - 1 - gl); if (n <= 0) break; gl += (u32)n; } if (window_now_ms() > end) break; } }
  close(cp[0]);
  char want[1200]; snprintf(want, sizeof want, "cut\nfile://%s/src/a%%20b.txt", dir);
  bool send_ok = !strcmp(got, want);
  if (pipe2(cp, O_CLOEXEC) != 0) return fail("pipe");
  clip_send(a, cp[1], "text/uri-list", a.clip);
  gl = 0; memset(got, 0, sizeof got);
  { i64 end = window_now_ms() + 5000; for (;;) { struct pollfd pf = { cp[0], POLLIN, 0 }; if (poll(&pf, 1, 100) > 0) { ssize_t n = read(cp[0], got + gl, sizeof got - 1 - gl); if (n <= 0) break; gl += (u32)n; } if (window_now_ms() > end) break; } }
  close(cp[0]);
  snprintf(want, sizeof want, "file://%s/src/a%%20b.txt\r\n", dir);
  send_ok = send_ok && !strcmp(got, want);
  pump(2000, [](App& x) { return x.pool.completed == x.pool.submitted; });

  a.clip.clear();
  if (pipe2(cp, O_CLOEXEC) != 0) return fail("pipe");
  snprintf(want, sizeof want, "copy\nfile://%s/src/a%%20b.txt\n", dir);
  if (write(cp[1], want, strlen(want)) < 0) return fail("write pipe");
  close(cp[1]);
  ClipXfer* x = (ClipXfer*)calloc(1, sizeof(ClipXfer));
  x->fd = cp[0]; x->send = false; x->mime = WCLIP_GNOME_FILES; snprintf(x->dest, sizeof x->dest, "%s/dest", dir);
  a.clip_recv_pending++;
  clip_xfer_submit(a, x);
  if (!pump(10000, [](App& y) { return y.clip_recv_pending == 0 && y.jobs.len == 0; })) return fail("clipboard paste did not finish");
  snprintf(q, sizeof q, "%s/dest/a b.txt", dir);
  bool recv_ok = stat(q, &st) == 0 && st.st_size == 3;
  printf("ops-test: clipboard send %s, paste from another client %s\n", send_ok ? "ok" : "FAILED", recv_ok ? "ok" : "FAILED");
  if (!send_ok || !recv_ok) return 1;

  snprintf(p, sizeof p, "%s/src/sub/pipe", dir); mkfifo(p, 0644);
  snprintf(p, sizeof p, "%s/src/sub", dir);
  FileOpRequest r8; r8.kind = OP_COPY; snprintf(r8.dest, sizeof r8.dest, "%s/dest", dir); r8.add(p);
  s_error_mode = 0; s_error_seen = 0; errors_asked = 0;
  ops_submit(a, r8, UNDO_COPY, "sub");
  if (!pump(10000, [](App& x) { return x.jobs.len == 0; })) return fail("copy with a FIFO did not finish");
  bool err_skip_ok = errors_asked == 1;
  snprintf(q, sizeof q, "%s/dest/sub/s.txt", dir);
  err_skip_ok = err_skip_ok && stat(q, &st) == 0; // the rest of the tree still arrived
  FileOpRequest r9; r9.kind = OP_COPY; snprintf(r9.dest, sizeof r9.dest, "%s/dest", dir); r9.add(p);
  s_error_mode = 1; s_error_seen = 0; errors_asked = 0;
  ops_submit(a, r9, UNDO_COPY, "sub");
  if (!pump(10000, [](App& x) { return x.jobs.len == 0; })) return fail("copy with a retry did not finish");
  bool err_retry_ok = errors_asked == 2; // asked, retried (fails again), asked once more, skip all

  snprintf(p, sizeof p, "%s/dest/a (2).bin", dir);
  bool trash_cols_ok = false;
  { FileOpRequest r10; r10.kind = OP_TRASH; snprintf(q, sizeof q, "%s/dest/sub/s.txt", dir); r10.add(q);
    ops_submit(a, r10, UNDO_TRASH, "s.txt");
    if (!pump(10000, [](App& x) { return x.jobs.len == 0; })) return fail("trash for the columns did not finish");
    char th[4096]; if (trash_home(th, sizeof th)) {
      strncat(th, "/files", sizeof th - strlen(th) - 1);
      i32 pos = tab_open(a, th, true);
      if (pos >= 0) {
        if (pump(5000, [](App& x) { Tab& tt = tab_active(x); if (!tt.trash_cols) return true; for (u32 i = 0; i < tt.listing.count(); i++) if (!strcmp(tt.listing.cname(i), "s.txt") && trash_original(tt, i)[0]) return true; return false; })) {
          Tab& tt = tab_active(a);
          for (u32 i = 0; i < tt.listing.count(); i++) if (!strcmp(tt.listing.cname(i), "s.txt")) trash_cols_ok = tt.trash_cols && strstr(trash_original(tt, i), "/dest/sub/s.txt") != nullptr && tt.trash_deleted[i] > 0;
        }
        tab_close(a, (u32)pos);
      }
    }
  }
  printf("ops-test: error dialog skip %s, retry + skip all %s, trash columns %s\n", err_skip_ok ? "ok" : "FAILED", err_retry_ok ? "ok" : "FAILED", trash_cols_ok ? "ok" : "FAILED");
  if (!err_skip_ok || !err_retry_ok || !trash_cols_ok) return 1;
  printf("ops-test: ok: %u conflicts answered, %u wakeups, %u inotify drains, %u events, watched=%d, cancel %s, trash %s, restore %s, %u done / %u failed\n",
         conflicts, polls, drains, a.watcher.drained, watched, partial ? "finished anyway (fast disk)" : "stopped early", trashed ? "ok" : "FAILED",
         restored ? "ok" : "FAILED", a.ops_done, a.ops_failed);
  ops_stop(a);
  worker_stop(a.pool);
  watch_close(a.watcher);
  return (trashed && restored) ? 0 : 1;
}

static App g_app;

int main(int argc, char** argv) {
  if (argc == 3 && !strcmp(argv[1], "--helper")) return helper_main(argv[2]);
  i64  test_ms = 0;
  App* a = &g_app;
  const char* starts[MAX_TABS]; u32 nstarts = 0;
  const char* snap = nullptr; i32 snap_w = 1100, snap_h = 700; float snap_scale = 1.0f; i32 snap_view = -1;
  const char* snap_filter = nullptr; bool snap_path_edit = false, do_layout_test = false;
  const char* ops_test_dir = nullptr;
  float font_px_arg = 0;
  bool light_arg = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) { printf("mattexplorer %s\n", MX_VERSION); return 0; }
    else if (!strcmp(argv[i], "--debug")) a->debug = true;
    else if (!strcmp(argv[i], "--light")) light_arg = true;
    else if (!strcmp(argv[i], "--no-workers")) a->no_workers = true;
    else if (!strcmp(argv[i], "--no-thumbs")) a->no_thumbs_arg = true;
    else if (!strcmp(argv[i], "--demo-devices")) a->demo_devices = true;
    else if (!strcmp(argv[i], "--list-fonts")) return list_fonts(*a);
    else if (!strcmp(argv[i], "--test-ms") && i + 1 < argc) test_ms = atoll(argv[++i]);
    else if (!strcmp(argv[i], "--font") && i + 1 < argc) snprintf(a->font_spec, sizeof a->font_spec, "%s", argv[++i]);
    else if (!strcmp(argv[i], "--font-size") && i + 1 < argc) font_px_arg = mx_clamp((float)atof(argv[++i]), 7.0f, 40.0f);
    else if (!strcmp(argv[i], "--snapshot") && i + 1 < argc) snap = argv[++i];
    else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &snap_w, &snap_h);
    else if (!strcmp(argv[i], "--scale") && i + 1 < argc) snap_scale = mx_clamp((float)atof(argv[++i]), 0.5f, 4.0f);
    else if (!strcmp(argv[i], "--view") && i + 1 < argc) { i++; snap_view = !strcmp(argv[i], "icons") ? VIEW_ICONS : !strcmp(argv[i], "list") ? VIEW_LIST : VIEW_DETAILS; }
    else if (!strcmp(argv[i], "--filter") && i + 1 < argc) snap_filter = argv[++i];
    else if (!strcmp(argv[i], "--path-edit")) snap_path_edit = true;
    else if (!strcmp(argv[i], "--layout-test")) do_layout_test = true;
    else if (!strcmp(argv[i], "--ops-test") && i + 1 < argc) ops_test_dir = argv[++i];
    else if (!strcmp(argv[i], "--do") && i + 1 < argc) { if (g_script_n < MX_ARRAY_COUNT(g_script)) g_script[g_script_n++].spec = argv[++i]; }
    else if (argv[i][0] != '-') { if (nstarts < MAX_TABS) starts[nstarts++] = argv[i]; }
    else { fprintf(stderr, "mattexplorer: unknown option %s\n", argv[i]); return 2; }
  }

  signal(SIGPIPE, SIG_IGN);
  a->theme_dark = kDark; a->theme_light = kLight;
  config_load(*a);
  if (const char* env = getenv("MATTEXPLORER_FONT_SIZE")) a->cfg.font_px = mx_clamp((float)atof(env), 7.0f, 40.0f);
  if (font_px_arg > 0) a->cfg.font_px = font_px_arg;
  if (light_arg) { a->cfg.light = true; a->cfg.theme_follow = false; }
  a->light = a->cfg.light;
  if (!getenv("MATTEXPLORER_NO_THEME")) theme_sync_load(*a); // the Omarchy theme, when there is one
  settings_apply_thumbs(*a);

  gfx_init();
  i64 t0 = now_ns();
  font_db_scan(a->db);
  bool have_font = open_ui_font(*a);
  if (a->debug)
    fprintf(stderr, "fonts: %u faces in %u files, scan %.1f ms, open %.1f ms total\n", a->db.entries.len, a->db.files,
            a->db.scan_ns / 1e6, (now_ns() - t0) / 1e6);
  if (!have_font) fprintf(stderr, "mattexplorer: no usable TrueType font found; text will not be drawn\n");

  char cwd[4096];
  if (!nstarts) { starts[nstarts++] = getcwd(cwd, sizeof cwd) ? cwd : (getenv("HOME") ? getenv("HOME") : "/"); }

  { ssize_t n = readlink("/proc/self/exe", a->exe_path, sizeof a->exe_path - 1); a->exe_path[n > 0 ? n : 0] = 0; }
  { struct passwd* pw = getpwuid(getuid()); snprintf(a->user_name, sizeof a->user_name, "%s", pw ? pw->pw_name : ""); }
  if (do_layout_test) return layout_test(*a, starts[0]);
  if (ops_test_dir) return ops_test(*a, ops_test_dir);
  if (snap) return snapshot(*a, starts, nstarts, snap, mx_max(snap_w, 200), mx_max(snap_h, 100), snap_scale, snap_view, snap_filter, snap_path_edit);

  Window& w = a->w;
  if (!window_open(w, a->cfg.win_w, a->cfg.win_h, "MattExplorer", "mattexplorer")) {
    fprintf(stderr, "mattexplorer: %s\n", w.wl.err[0] ? w.wl.err : "could not open a window");
    return 1;
  }
  if (a->debug)
    fprintf(stderr, "window: logical %dx%d buffer %dx%d scale %u/120 (fractional=%s viewporter=%s cursor-shape=%s)\n",
            w.logical_w, w.logical_h, w.buf_w, w.buf_h, w.scale120, w.fractional_mgr ? "yes" : "no",
            w.viewporter ? "yes" : "no", w.cursor_mgr ? "yes" : "no");

  if (!a->no_workers && worker_start(a->pool, 0)) window_add_fd(w, a->pool.wake_fd, POLLIN);
  else if (a->debug) fprintf(stderr, "workers: none (jobs run inline)\n");
  a->sync_stat = !a->pool.running;
  if (!a->sync_stat) {
    for (u32 i = 0; i < STAT_POOL; i++) a->stat_pool[i] = (StatBatch*)calloc(1, sizeof(StatBatch));
    a->stat_free = (1u << STAT_POOL) - 1;
  }

  if (!a->no_workers && ops_start(*a)) window_add_fd(w, a->ops.wake_fd, POLLIN);
  else if (a->debug) fprintf(stderr, "ops: no thread (file operations run inline)\n");
  if (watch_open(a->watcher)) {
    window_add_fd(w, a->watcher.fd, POLLIN);
    char td[512];
    if (theme_omarchy_watch_dir(td, sizeof td)) a->theme_watch = watch_add(a->watcher, td); // theme switches re-skin the window
  } else if (a->debug) fprintf(stderr, "watch: inotify unavailable (%s); F5 refreshes\n", strerror(errno));
  apps_request_scan(*a);
  a->mountinfo_fd = mounts_open_watch();
  if (a->mountinfo_fd >= 0) { mounts_rearm(a->mountinfo_fd); window_add_fd(w, a->mountinfo_fd, POLLPRI); }
  mounts_request_refresh(*a);
  a->mounts_next_ms = window_now_ms() + 30000;

  load_places(*a);
  pins_load(*a);
  for (u32 i = 0; i < nstarts; i++) tab_open(*a, starts[i], i == 0);
  if (!a->num_tabs) { fprintf(stderr, "mattexplorer: nothing to open\n"); return 1; }

  i64  start_ms = window_now_ms();
  u32  counts[20] = {};
  while (a->running) {

    i64 now = window_now_ms();
    i64 wake = 0;
    auto earliest = [&](i64 t) { if (t > 0 && (wake == 0 || t < wake)) wake = t; };
    if (a->ui.wake_at_ms) earliest(a->ui.wake_at_ms);
    if (a->cfg_dirty) earliest(a->cfg_save_at);
    if (a->L.sidebar) earliest(a->mounts_next_ms);
    earliest(shell_next_wake(*a));
    if (test_ms) earliest(start_ms + test_ms);
    int timeout = wake ? (int)mx_clamp(wake - now, (i64)0, (i64)3600000) : -1;
    bool alive = window_pump(w, timeout);
    now = window_now_ms();
    a->ui.now_ms = now;

    for (WEvent& e : w.events) {
      if (e.type < 20) counts[e.type]++;
      switch (e.type) {
        case WE_CLOSE: a->running = false; break;
        case WE_POINTER_MOTION: ui_input_motion(a->ui, e.x, e.y, now); w.need_redraw = true; break;
        case WE_POINTER_LEAVE:  ui_input_leave(a->ui); w.need_redraw = true; break;
        case WE_POINTER_BUTTON: {
          u32 mods = wevent_mods(e) & ~(XKB_MOD_LOCK | w.xkb.mask_numlock);
          if (a->debug) fprintf(stderr, "button %#x %s at %.0f,%.0f mods=%x (keyboard %x/%x/%x, keys %x)\n", e.code, e.pressed ? "down" : "up", e.x, e.y, mods, e.mods[0], e.mods[1], e.mods[2], w.key_mods);
          ui_input_button(a->ui, e.x, e.y, e.code, e.pressed, mods, now);
          w.need_redraw = true;
          break;
        }
        case WE_SCROLL: ui_input_scroll(a->ui, e.x, e.y, e.dx, e.dy, wevent_mods(e) & ~(XKB_MOD_LOCK | w.xkb.mask_numlock)); w.need_redraw = true; break;
        case WE_KEY: {
          if (a->debug && e.pressed) {
            char name[48];
            fprintf(stderr, "key %u %s%s mods=%x/%x/%x group=%u sym=%s", e.code, e.pressed ? "down" : "up",
                    e.repeat ? " (repeat)" : "", e.mods[0], e.mods[1], e.mods[2], e.mods[3], keysym_format(e.keysym, name, sizeof name));
            if (e.text_len) fprintf(stderr, " text=\"%.*s\"", e.text_len, e.text);
            fprintf(stderr, "\n");
          }
          shell_key(*a, e);
          break;
        }
        case WE_KEYMAP:
          if (a->debug) {
            if (w.xkb.ok)
              fprintf(stderr, "keymap: %u bytes, %u keys, %u types, %u layout%s (%s), compose: %u sequences from %s\n",
                      w.keymap_size, w.xkb.keys.len, w.xkb.types.len, w.xkb.num_groups, w.xkb.num_groups == 1 ? "" : "s",
                      keymap_group_name(w.xkb, 0), w.compose.sequences, w.compose.source[0] ? w.compose.source : "nowhere");
            else fprintf(stderr, "keymap: %u bytes, parse failed: %s\n", w.keymap_size, w.xkb.err);
          }
          break;
        case WE_RESIZE: if (a->debug) fprintf(stderr, "resize: buffer %dx%d logical %dx%d%s\n", w.buf_w, w.buf_h, w.logical_w, w.logical_h, w.resizing ? " (resizing)" : ""); break;
        case WE_SCALE:  if (a->debug) fprintf(stderr, "scale: %u/120\n", w.scale120); break;
        case WE_FOCUS_IN: case WE_FOCUS_OUT:
          if (a->debug) fprintf(stderr, "keyboard %s (mods %x/%x/%x, keys held %x)\n", e.type == WE_FOCUS_IN ? "enter" : "leave", w.mods[0], w.mods[1], w.mods[2], w.key_mods);
          shell_focus_changed(*a);
          break;
        case WE_MODIFIERS:
          if (a->debug) fprintf(stderr, "modifiers %x/%x/%x group %u\n", e.mods[0], e.mods[1], e.mods[2], e.mods[3]);
          break;
        case WE_SELECTION:
          if (a->debug) fprintf(stderr, "clipboard: selection %s (offer %u, formats %x)\n", w.sel_ours ? "ours" : w.sel_offer ? "another client's" : "none", w.sel_offer, w.sel_mimes);
          w.need_redraw = true;
          break;
        case WE_DATA_SEND: clip_send(*a, (int)e.code, w.send_mime, e.aux ? a->drag : a->clip); break;
        case WE_DATA_CANCELLED: clip_cancelled(*a); break;
        case WE_DND_ENTER: case WE_DND_MOTION: case WE_DND_LEAVE: case WE_DND_DROP:
          if (a->debug && e.type != WE_DND_MOTION) fprintf(stderr, "dnd: %s at %.0f,%.0f (formats %x)\n", e.type == WE_DND_ENTER ? "enter" : e.type == WE_DND_LEAVE ? "leave" : "drop", e.x, e.y, w.dnd_mimes);
          dnd_event(*a, e.type, e.x, e.y);
          break;
        case WE_DRAG_END: drag_ended(*a, e.aux != 0, e.code); w.need_redraw = true; break;
        case WE_FD:
          if ((int)e.code == a->pool.wake_fd) {
            pool_drain(*a);
          } else if ((int)e.code == a->ops.wake_fd) {
            ops_poll(*a);
            w.need_redraw = true;
          } else if ((int)e.code == a->watcher.fd) {
            watch_drain(a->watcher, now);
          } else if ((int)e.code == a->mountinfo_fd) {
            mounts_rearm(a->mountinfo_fd);
            if (now - a->mounts_last_pri_ms > 500) { // the kernel can report a burst per mount
              a->mounts_last_pri_ms = now;
              if (a->debug) fprintf(stderr, "mounts: table changed\n");
              mounts_request_refresh(*a);
            }
          }
          break;
        default: break;
      }
    }
    if (!alive) break;
    drag_check(*a);

    if (!a->pool.running) pool_drain(*a); // --no-workers: jobs ran inline

    if (a->cfg_dirty && now >= a->cfg_save_at) config_save(*a);
    if (a->L.sidebar && now >= a->mounts_next_ms) { a->mounts_next_ms = now + 30000; mounts_request_refresh(*a); }
    shell_timers(*a, now);
    if (a->ui.wake_at_ms && now >= a->ui.wake_at_ms) { a->ui.wake_at_ms = 0; w.need_redraw = true; }

    if (!w.maximized && !w.fullscreen && w.logical_w > 0 && (w.logical_w != a->cfg.win_w || w.logical_h != a->cfg.win_h)) {
      a->cfg.win_w = w.logical_w; a->cfg.win_h = w.logical_h; config_mark_dirty(*a);
    }

    if (w.need_redraw && !w.frame_pending) {
      Canvas c;
      if (window_acquire(w, c)) {
        i64 d0 = now_ns();
        shell_frame(*a, c);
        a->draw_ns = now_ns() - d0;
        window_present(w, 0, 0, c.w, c.h);
        a->frames++;
        if (test_ms) { Tab& t = tab_active(*a); t.scroll += a->L.row_h / 3; w.need_redraw = true; } // test: redraw + scroll every frame
      }
    }
    if (test_ms && now - start_ms >= test_ms) break;
  }

  if (test_ms) {
    Tab& t = tab_active(*a);
    usize cache = 0;
    for (u32 i = 0; i < a->text.num_fonts; i++) cache += font_cache_bytes(a->text.fonts[i]);
    printf("test: %u frames in %lld ms, final buffer %dx%d, scale %u/120, keymap %s, last draw %.2f ms, configures %u, pool grows %u\n", a->frames,
           (long long)(window_now_ms() - start_ms), w.buf_w, w.buf_h, w.scale120,
           !w.keymap ? "none" : w.xkb.ok ? "parsed" : "unparseable", a->draw_ns / 1e6, w.configures, w.pool_grows);
    printf("text: %u fonts (%u fallbacks loaded), glyph cache %.1f KB; dir: %u entries, list %.2f ms, sort %.2f ms; devices: %u (%u statted); jobs %u/%u; watches %u, %u events; ops %u/%u\n",
           a->text.num_fonts, a->text.fallback_loads, cache / 1024.0, t.listing.count(), t.list_ns / 1e6, t.sort_ns / 1e6,
           a->mounts.entries.len, [&]{ u32 n = 0; for (MountEntry& m : a->mounts.entries) n += m.statted; return n; }(), a->pool.completed, a->pool.submitted,
           a->watcher.watches.len, a->watcher.drained, a->ops_done, a->ops_failed);
    printf("events: resize=%u scale=%u ptr_motion=%u button=%u scroll=%u key=%u focus=%u fd=%u\n", counts[WE_RESIZE],
           counts[WE_SCALE], counts[WE_POINTER_MOTION], counts[WE_POINTER_BUTTON], counts[WE_SCROLL], counts[WE_KEY],
           counts[WE_FOCUS_IN], counts[WE_FD]);
  }
  if (w.wl.dead) fprintf(stderr, "mattexplorer: connection lost: %s\n", w.wl.err);
  if (a->cfg_dirty) config_save(*a);
  ops_stop(*a);
  worker_stop(a->pool);
  watch_close(a->watcher);
  thumbs_clear(*a);
  window_close(w);
  for (Tab& t : a->tabs) if (t.dirfd >= 0) close(t.dirfd);
  return w.wl.dead ? 1 : 0;
}
