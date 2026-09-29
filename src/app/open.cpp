#include "app/app.h"
#include "core/format.h"
#include "core/path.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

bool ensure_apps(App& a) {
  if (a.apps_ready && a.mime_ready) return true;
  if (!a.pool.running) { // no workers (tests, --no-workers): read them now
    if (!a.mime_ready) { mime_db_load(a.mime); a.mime_ready = true; }
    if (!a.apps_ready) { app_db_scan(a.apps); a.apps_ready = true; }
    return true;
  }
  if (!a.apps_scanning && !a.mime_scanning) apps_request_scan(a);
  set_status(a, "Still reading the list of applications, try again in a moment");
  return false;
}

const char* entry_mime(App& a, Tab& t, u32 i, char* buf, u32 cap) {
  if (!(t.listing.flags[i] & (EF_STATTED | EF_STAT_FAILED)) && t.dirfd >= 0) dir_list_stat_range(t.dirfd, t.listing, i, 1);
  if (t.listing.kind[i] == EK_OTHER) { snprintf(buf, cap, "inode/special"); return buf; }
  bool dir = t.listing.is_dir(i);
  const char* m = a.mime_ready ? mime_from_name(a.mime, t.listing.name(i), dir) : (dir ? "inode/directory" : "");
  if (m[0]) { snprintf(buf, cap, "%s", m); return buf; }

  u8 head[512]; ssize_t n = -1;
  int fd = t.dirfd >= 0 ? openat(t.dirfd, t.listing.cname(i), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY) : -1;
  if (fd >= 0) {
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) n = read(fd, head, sizeof head);
    close(fd);
  }
  snprintf(buf, cap, "%s", mime_sniff(head, n > 0 ? (u32)n : 0));
  return buf;
}

static bool exec_takes_many(const AppDb& db, const DesktopApp& app) {
  const char* e = db.s(app.exec);
  return strstr(e, "%F") || strstr(e, "%U");
}

static bool launch_app(App& a, u32 app, const char* const* paths, u32 n, u32* spawned, char* err, u32 ecap) {
  const DesktopApp& d = a.apps.apps[app];
  Array<char> buf; Array<char*> argv;
  if (exec_takes_many(a.apps, d) || n == 1) {
    if (!desktop_argv(a.apps, d, paths, n, buf, argv, err, ecap, a.cfg.terminal)) return false;
    if (!spawn_detached(argv.data, nullptr, err, ecap)) return false;
    (*spawned)++;
    return true;
  }
  for (u32 i = 0; i < n; i++) {
    if (*spawned >= 16) { snprintf(err, ecap, "not opening more than 16 at once"); return false; }
    if (!desktop_argv(a.apps, d, paths + i, 1, buf, argv, err, ecap, a.cfg.terminal)) return false;
    if (!spawn_detached(argv.data, nullptr, err, ecap)) return false;
    (*spawned)++;
  }
  return true;
}

static bool run_path(const char* path, u32* spawned, char* err, u32 ecap) {
  char dir[4096]; snprintf(dir, sizeof dir, "%s", path);
  if (char* s = strrchr(dir, '/')) { if (s == dir) s[1] = 0; else *s = 0; }
  const char* argv[2] = { path, nullptr };
  if (!spawn_detached(argv, dir, err, ecap)) return false;
  (*spawned)++;
  return true;
}

void open_files(App& a, Tab& t, i32 app_index) {
  if (!ensure_apps(a)) return;
  if (t.selected_count > 32) { set_status(a, "Select fewer items to open them (32 at most)"); return; } // Explorer asks at 15

  Array<char> pb; Array<u32> po; Array<u32> idx;
  auto take = [&](u32 i) { char p[4096]; entry_path(t, i, p, sizeof p); u32 n = (u32)strlen(p); po.push(pb.len); memcpy(pb.push_n(n + 1), p, n + 1); idx.push(i); };
  if (t.selected_count) { for (u32 i = 0; i < t.listing.count(); i++) if (t.selected[i]) { if (!(t.listing.flags[i] & EF_STATTED)) dir_list_stat_range(t.dirfd, t.listing, i, 1); if (!t.listing.is_dir(i)) take(i); } }
  else if (t.cursor >= 0 && (u32)t.cursor < t.rows.len) { u32 i = t.rows[(u32)t.cursor]; if (!(t.listing.flags[i] & EF_STATTED)) dir_list_stat_range(t.dirfd, t.listing, i, 1); if (!t.listing.is_dir(i)) take(i); }
  if (!po.len) { set_status(a, "Nothing to open"); return; }
  Array<const char*> paths; for (u32 o : po) paths.push(pb.data + o);
  u32 spawned = 0; char err[512] = {}; char msg[700];
  const char* app_name = nullptr;
  if (app_index >= 0 && (u32)app_index < a.apps.apps.len) {
    app_name = a.apps.s(a.apps.apps[(u32)app_index].name);
    if (!launch_app(a, (u32)app_index, paths.data, paths.len, &spawned, err, sizeof err)) { snprintf(msg, sizeof msg, "Cannot open with %s: %s", app_name, err); set_status_error(a, msg); return; }
  } else {

    Array<i32> app_of; Array<u8> done;
    char mime[128];
    for (u32 k = 0; k < paths.len; k++) {
      entry_mime(a, t, idx[k], mime, sizeof mime);
      u32 apps[4]; u32 n = apps_for_mime(a.mime, a.apps, mime, apps, 4);
      app_of.push(n ? (i32)apps[0] : -1); done.push(0);
    }
    bool xdg = command_exists("xdg-open");
    for (u32 k = 0; k < paths.len; k++) {
      if (done[k]) continue;
      if (app_of[k] >= 0) {
        Array<const char*> group;
        for (u32 j = k; j < paths.len; j++) if (!done[j] && app_of[j] == app_of[k]) { group.push(paths[j]); done[j] = 1; }
        app_name = a.apps.s(a.apps.apps[(u32)app_of[k]].name);
        if (!launch_app(a, (u32)app_of[k], group.data, group.len, &spawned, err, sizeof err)) { snprintf(msg, sizeof msg, "Cannot open with %s: %s", app_name, err); set_status_error(a, msg); return; }
        continue;
      }
      done[k] = 1;
      u32 i = idx[k];
      bool exec = (t.listing.flags[i] & EF_STATTED) && (t.listing.mode[i] & 0111) && t.listing.kind[i] != EK_DIR;
      if (xdg) {
        const char* argv[3] = { "xdg-open", paths[k], nullptr };
        if (!spawn_detached(argv, nullptr, err, sizeof err)) { snprintf(msg, sizeof msg, "xdg-open: %s", err); set_status_error(a, msg); return; }
        spawned++; app_name = "xdg-open";
      } else if (exec) {
        if (!run_path(paths[k], &spawned, err, sizeof err)) { snprintf(msg, sizeof msg, "Cannot run %s: %s", path_base(paths[k]), err); set_status_error(a, msg); return; }
        app_name = nullptr;
      } else {
        entry_mime(a, t, i, mime, sizeof mime);
        snprintf(msg, sizeof msg, "No application for %s (%s) \xc2\xb7 right-click for Open with", path_base(paths[k]), mime);
        set_status_error(a, msg);
        return;
      }
    }
  }
  a.launches += spawned;
  if (paths.len == 1) snprintf(msg, sizeof msg, app_name ? "Opened %s with %s" : "Running %s", path_base(paths[0]), app_name);
  else snprintf(msg, sizeof msg, "Opened %u items", paths.len);
  set_status(a, msg);
  if (a.debug) fprintf(stderr, "open: %u items, %u processes%s%s\n", paths.len, spawned, app_name ? " via " : "", app_name ? app_name : "");
}

void run_file(App& a, Tab& t) {
  if (t.cursor < 0 || (u32)t.cursor >= t.rows.len) return;
  u32 i = t.rows[(u32)t.cursor];
  char p[4096], err[256], msg[600];
  entry_path(t, i, p, sizeof p);
  u32 spawned = 0;
  if (!run_path(p, &spawned, err, sizeof err)) { snprintf(msg, sizeof msg, "Cannot run %s: %s", path_base(p), err); set_status_error(a, msg); return; }
  a.launches++;
  snprintf(msg, sizeof msg, "Running %s", path_base(p));
  set_status(a, msg);
}

void open_terminal(App& a, const char* dir) {
  char term[256], err[256], msg[600];
  if (!terminal_find(a.cfg.terminal, term, sizeof term)) { set_status_error(a, "No terminal emulator found (set $TERMINAL, or terminal = ... in the config)"); return; }
  if (!terminal_open(term, dir, err, sizeof err)) { snprintf(msg, sizeof msg, "Cannot open %s: %s", term, err); set_status_error(a, msg); return; }
  a.launches++;
  const char* b = strrchr(term, '/'); b = b ? b + 1 : term;
  snprintf(msg, sizeof msg, "Opened %s in %s", b, path_base(dir));
  set_status(a, msg);
}

bool in_trash(const Tab& t) { return t.used && trash_is_files_dir(t.path); }

void op_restore(App& a, Tab& t) {
  if (!t.selected_count) { set_status(a, "Nothing selected"); return; }
  FileOpRequest r; r.kind = OP_RESTORE;
  u32 n = 0, unknown = 0;
  char p[4096], orig[4096], first[4096] = {};
  i64 deleted;
  for (u32 i = 0; i < t.listing.count(); i++) {
    if (!t.selected[i]) continue;
    entry_path(t, i, p, sizeof p);
    if (trash_info_read(p, orig, sizeof orig, &deleted)) { r.add(p); r.add(orig); if (!n) snprintf(first, sizeof first, "%s", orig); n++; }
    else unknown++;
  }
  if (!n) { set_status_error(a, unknown == 1 ? "No trash information for it: where it came from is unknown" : "No trash information for the selection"); return; }
  snprintf(r.dest, sizeof r.dest, "%s", first);
  if (char* s = strrchr(r.dest, '/')) { if (s == r.dest) s[1] = 0; else *s = 0; }
  char label[160];
  if (n == 1) snprintf(label, sizeof label, "%s", path_base(first)); else snprintf(label, sizeof label, "%u items", n);
  if (unknown) { char msg[200]; snprintf(msg, sizeof msg, "%u item%s without trash information skipped", unknown, unknown == 1 ? "" : "s"); set_status(a, msg); }
  ops_submit(a, r, UNDO_RESTORE, label);
}

void op_empty_trash_dir(App& a, const char* files_dir, bool confirmed) {
  if (!trash_is_files_dir(files_dir)) return;
  char info[4096];
  snprintf(info, sizeof info, "%s", files_dir);
  if (char* s = strrchr(info, '/')) *s = 0;
  strncat(info, "/info", sizeof info - strlen(info) - 1);
  FileOpRequest r; r.kind = OP_DELETE;
  char p[4096];
  u32 n = 0;
  if (DIR* d = opendir(files_dir)) {
    struct dirent* e;
    while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; snprintf(p, sizeof p, "%s/%s", files_dir, e->d_name); r.add(p); n++; }
    closedir(d);
  }
  if (DIR* d = opendir(info)) {
    struct dirent* e;
    while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; snprintf(p, sizeof p, "%s/%s", info, e->d_name); r.add(p); }
    closedir(d);
  }
  if (!n) { set_status(a, "The trash is empty"); return; }
  if (!confirmed) {
    Dialog& d = a.dialog;
    if (d.kind != DLG_NONE) return;
    d.kind = DLG_CONFIRM_DELETE;
    snprintf(d.title, sizeof d.title, "Empty the trash?");
    d.nlines = 0;
    snprintf(d.lines[d.nlines++], sizeof d.lines[0], "%u item%s will be deleted permanently.", n, n == 1 ? "" : "s");
    snprintf(d.lines[d.nlines++], sizeof d.lines[0], "This cannot be undone.");
    d.nbuttons = 0; d.buttons[d.nbuttons++] = "Empty trash"; d.buttons[d.nbuttons++] = "Cancel";
    d.focus = 1; d.cancel = 1; d.hover = -1; d.check_label = nullptr;
    d.pending.paths = static_cast<Array<char>&&>(r.paths); d.pending.off = static_cast<Array<u32>&&>(r.off);
    d.pending.kind = OP_DELETE; d.pending.dest[0] = 0;
    d.pending_undo = UNDO_NONE;
    snprintf(d.pending_label, sizeof d.pending_label, "the trash");
    a.w.need_redraw = true;
    return;
  }
  ops_submit(a, r, UNDO_NONE, "the trash");
}

void op_empty_trash(App& a, Tab& t, bool confirmed) {
  if (!in_trash(t)) return;
  op_empty_trash_dir(a, t.path, confirmed);
}

static bool run_capture(const char* const* argv, Array<char>& out, Array<char>& err, int* status, int* spawn_errno) {
  int o[2], e[2];
  if (pipe2(o, O_CLOEXEC) != 0 || pipe2(e, O_CLOEXEC) != 0) { *spawn_errno = errno; return false; }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, o[1], 1);
  posix_spawn_file_actions_adddup2(&fa, e[1], 2);
  pid_t pid = 0;
  int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, (char* const*)argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(o[1]); close(e[1]);
  if (rc != 0) { close(o[0]); close(e[0]); *spawn_errno = rc; return false; }
  out.clear(); err.clear();
  struct pollfd fds[2] = { { o[0], POLLIN, 0 }, { e[0], POLLIN, 0 } };
  bool open_o = true, open_e = true;
  while (open_o || open_e) {
    fds[0].fd = open_o ? o[0] : -1; fds[1].fd = open_e ? e[0] : -1;
    if (poll(fds, 2, -1) < 0) { if (errno == EINTR) continue; break; }
    if (fds[0].revents) { char b[4096]; ssize_t n = read(o[0], b, sizeof b); if (n > 0) memcpy(out.push_n((u32)n), b, (u32)n); else if (n == 0 || errno != EINTR) open_o = false; }
    if (fds[1].revents) { char b[4096]; ssize_t n = read(e[0], b, sizeof b); if (n > 0) memcpy(err.push_n((u32)n), b, (u32)n); else if (n == 0 || errno != EINTR) open_e = false; }
  }
  close(o[0]); close(e[0]);
  while (waitpid(pid, status, 0) < 0 && errno == EINTR) {}
  out.push(0); err.push(0);
  return true;
}

static void udisks_message(const char* stderr_text, char* out, u32 cap) {
  char line[512];
  snprintf(line, sizeof line, "%s", stderr_text);
  if (char* nl = strchr(line, '\n')) *nl = 0;
  const char* m = line;
  if (const char* g = strstr(line, "GDBus.Error:")) { const char* c = strstr(g, ": "); if (c) m = c + 2; }
  if (!m[0]) m = "udisksctl failed";
  snprintf(out, cap, "%s", m);
}

static bool udisks_run(const char* verb, const char* device, Array<char>& out, Array<char>& err, char* msg, u32 cap) {
  const char* argv[] = { "udisksctl", verb, "-b", device, "--no-user-interaction", nullptr };
  int status = 0, e = 0;
  if (!run_capture(argv, out, err, &status, &e)) {
    snprintf(msg, cap, e == ENOENT ? "udisksctl is not installed (package udisks2)" : "cannot run udisksctl: %s", strerror(e));
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { udisks_message(err.data, msg, cap); return false; }
  return true;
}

static void job_udisks(Job& j) {
  UdisksJob* u = (UdisksJob*)j.ctx;
  Array<char> out, err;
  u->ok = false; u->err[0] = 0; u->mountpoint[0] = 0;
  if (u->kind == UD_MOUNT) {
    if (!udisks_run("mount", u->device, out, err, u->err, sizeof u->err)) return;

    const char* at = strstr(out.data, " at ");
    if (at) {
      snprintf(u->mountpoint, sizeof u->mountpoint, "%s", at + 4);
      u32 n = (u32)strlen(u->mountpoint);
      while (n && (u->mountpoint[n - 1] == '\n' || u->mountpoint[n - 1] == '.' || u->mountpoint[n - 1] == ' ')) u->mountpoint[--n] = 0;
    }
    u->ok = true;
  } else if (u->kind == UD_UNMOUNT) {
    u->ok = udisks_run("unmount", u->device, out, err, u->err, sizeof u->err);
  } else {
    for (u32 i = 0; i < u->n_unmount; i++)
      if (!udisks_run("unmount", u->unmount_first[i], out, err, u->err, sizeof u->err)) return;
    u->ok = udisks_run("power-off", u->device, out, err, u->err, sizeof u->err);
  }
}

void udisks_submit(App& a, UdisksJob* u) {
  if (!worker_submit(a.pool, job_udisks, u, JOB_UDISKS, 0)) { set_status_error(a, "Too many jobs queued"); free(u); return; }
  a.udisks_pending++;
  if (!a.pool.running) pool_drain(a);
}

static void tabs_leave(App& a, const char* mountpoint) {
  usize n = strlen(mountpoint);
  char parent[4096];
  snprintf(parent, sizeof parent, "%s", mountpoint);
  if (char* s = strrchr(parent, '/')) { if (s == parent) s[1] = 0; else *s = 0; }
  for (Tab& t : a.tabs) {
    if (!t.used) continue;
    if (strncmp(t.path, mountpoint, n) || (t.path[n] != 0 && t.path[n] != '/')) continue;
    if (!navigate(a, t, parent, true)) tab_gone(a, t);
  }
}

void device_mount(App& a, u32 entry, bool open_after, bool new_tab) {
  if (entry >= a.mounts.entries.len) return;
  const MountEntry& m = a.mounts.entries[entry];
  if (m.mounted) { if (new_tab) tab_open(a, m.mountpoint, false); else navigate(a, tab_active(a), m.mountpoint, true); return; }
  if (!strcmp(m.fstype, "swap")) { set_status(a, "That is swap space"); return; }
  if (!a.pool.running && a.no_workers) { set_status(a, "Mounting needs the worker pool"); return; }
  UdisksJob* u = (UdisksJob*)calloc(1, sizeof(UdisksJob));
  u->kind = UD_MOUNT;
  snprintf(u->device, sizeof u->device, "%s", m.device);
  snprintf(u->name, sizeof u->name, "%s", m.name);
  u->open_after = open_after; u->new_tab = new_tab;
  char msg[200]; snprintf(msg, sizeof msg, "Mounting %s\xe2\x80\xa6", m.name); set_status(a, msg);
  udisks_submit(a, u);
}

void device_unmount(App& a, u32 entry) {
  if (entry >= a.mounts.entries.len) return;
  const MountEntry& m = a.mounts.entries[entry];
  if (!m.mounted) return;
  if (!strcmp(m.mountpoint, "/")) { set_status_error(a, "The root filesystem cannot be unmounted"); return; }
  UdisksJob* u = (UdisksJob*)calloc(1, sizeof(UdisksJob));
  u->kind = UD_UNMOUNT;
  snprintf(u->device, sizeof u->device, "%s", m.device);
  snprintf(u->name, sizeof u->name, "%s", m.name);
  tabs_leave(a, m.mountpoint);
  char msg[200]; snprintf(msg, sizeof msg, "Unmounting %s\xe2\x80\xa6", u->name); set_status(a, msg);
  udisks_submit(a, u);
}

void device_power_off(App& a, u32 disk) {
  if (disk >= a.mounts.disks.len) return;
  const DiskEntry& d = a.mounts.disks[disk];
  UdisksJob* u = (UdisksJob*)calloc(1, sizeof(UdisksJob));
  u->kind = UD_POWER_OFF;
  snprintf(u->device, sizeof u->device, "/dev/%s", d.name);
  snprintf(u->name, sizeof u->name, "%s", d.label[0] ? d.label : d.name);
  for (u32 i = d.first; i < d.first + d.count && i < a.mounts.entries.len; i++) {
    const MountEntry& m = a.mounts.entries[i];
    if (!m.mounted || u->n_unmount >= MX_ARRAY_COUNT(u->unmount_first)) continue;
    if (!strcmp(m.mountpoint, "/")) { set_status_error(a, "That disk holds the root filesystem"); free(u); return; }
    tabs_leave(a, m.mountpoint);
    snprintf(u->unmount_first[u->n_unmount++], sizeof u->unmount_first[0], "%s", m.device);
  }
  char msg[200]; snprintf(msg, sizeof msg, "Removing %s\xe2\x80\xa6", u->name); set_status(a, msg);
  udisks_submit(a, u);
}

void udisks_done(App& a, UdisksJob* u) {
  a.udisks_pending--;
  char msg[600];
  if (!u->ok) {
    snprintf(msg, sizeof msg, "%s %s: %s", u->kind == UD_MOUNT ? "Cannot mount" : u->kind == UD_UNMOUNT ? "Cannot unmount" : "Cannot remove", u->name, u->err);
    set_status_error(a, msg);
  } else if (u->kind == UD_MOUNT) {
    snprintf(msg, sizeof msg, "Mounted %s at %s", u->name, u->mountpoint[0] ? u->mountpoint : "its mount point");
    set_status(a, msg);
    if (u->open_after && u->mountpoint[0]) { if (u->new_tab) tab_open(a, u->mountpoint, false); else navigate(a, tab_active(a), u->mountpoint, true); }
  } else if (u->kind == UD_UNMOUNT) {
    snprintf(msg, sizeof msg, "Unmounted %s", u->name); set_status(a, msg);
  } else {
    snprintf(msg, sizeof msg, "%s can be removed now", u->name); set_status(a, msg);
  }
  if (a.debug) fprintf(stderr, "udisks: %s %s: %s%s%s\n", u->kind == UD_MOUNT ? "mount" : u->kind == UD_UNMOUNT ? "unmount" : "power-off", u->device, u->ok ? "ok" : "failed", u->ok ? "" : ": ", u->ok ? "" : u->err);
  mounts_request_refresh(a);
  free(u);
  a.w.need_redraw = true;
}

const UiTheme& theme_current(const App& a) {
  if (a.theme_synced && a.cfg.theme_follow) return a.theme_omarchy;
  return a.light ? a.theme_light : a.theme_dark;
}

void theme_sync_load(App& a) {
  char dir[512];
  ThemeColors c;
  if (!theme_omarchy_dir(dir, sizeof dir) || !theme_load_dir(dir, c)) {
    if (a.debug) fprintf(stderr, "theme: no Omarchy theme at %s\n", dir);
    return; // an earlier load, if any, stays in force
  }
  bool light = false;
  theme_derive(c, a.theme_omarchy, &light);
  a.theme_synced = true;
  a.theme_is_light = light;
  snprintf(a.theme_name, sizeof a.theme_name, "%s", c.name);
  if (a.cfg.theme_follow) { a.light = light; a.cfg.light = light; }
  if (a.debug) fprintf(stderr, "theme: %s (%s) bg=%06x fg=%06x accent=%06x\n", c.name, light ? "light" : "dark", c.bg, c.fg, a.theme_omarchy.accent);
  a.w.need_redraw = true;
}
