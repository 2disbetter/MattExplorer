#include "app/app.h"
#include "core/format.h"
#include "core/path.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

static void tabs_follow(App& a, const char* from, const char* to);
static void tabs_gone_under(App& a, const char* dir);

static const char* kReqMagic = "mx-helper-request 1";
static const char* kResMagic = "mx-helper-result 1";

static bool request_write(const FileOpRequest& r, char* path, u32 cap) {
  const char* dir = getenv("XDG_RUNTIME_DIR");
  if (!dir || !dir[0]) dir = "/tmp";
  snprintf(path, cap, "%s/mattexplorer-op-XXXXXX", dir);
  int fd = mkstemp(path);
  if (fd < 0) return false;
  fchmod(fd, 0600);
  FILE* f = fdopen(fd, "w");
  if (!f) { close(fd); unlink(path); return false; }
  fprintf(f, "%s\nkind %u\nmask %o value %o recursive %u policy %u uid %d gid %d\npaths %u\n", kReqMagic, (u32)r.kind, r.mode_mask, r.mode_value,
          r.recursive ? 1u : 0u, (u32)r.policy, r.uid, r.gid, r.count());
  for (u32 i = 0; i < r.count(); i++) fwrite(r.path(i), 1, strlen(r.path(i)) + 1, f);
  bool ok = fclose(f) == 0;
  if (!ok) unlink(path);
  return ok;
}

static bool request_read(const char* path, FileOpRequest& r) {
  FILE* f = fopen(path, "r");
  if (!f) return false;
  char line[256];
  u32 kind = 0, mask = 0, value = 0, rec = 0, policy = 0, n = 0; int uid = -1, gid = -1;
  bool ok = fgets(line, sizeof line, f) && !strncmp(line, kReqMagic, strlen(kReqMagic)) &&
            fgets(line, sizeof line, f) && sscanf(line, "kind %u", &kind) == 1 &&
            fgets(line, sizeof line, f) && sscanf(line, "mask %o value %o recursive %u policy %u uid %d gid %d", &mask, &value, &rec, &policy, &uid, &gid) == 6 &&
            fgets(line, sizeof line, f) && sscanf(line, "paths %u", &n) == 1 && kind < OP_COUNT && n <= 1000000;
  if (ok) {
    r.kind = (FileOpKind)kind; r.mode_mask = mask & 07777; r.mode_value = value & 07777; r.recursive = rec != 0;
    r.uid = uid; r.gid = gid;
    r.policy = policy < 8 ? (ConflictAnswer)policy : CONFLICT_SKIP;
    if (r.policy == CONFLICT_ASK) r.policy = CONFLICT_SKIP; // nobody to ask
    char buf[4096];
    for (u32 i = 0; i < n && ok; i++) {
      u32 o = 0; int c;
      while ((c = fgetc(f)) != EOF && c != 0 && o + 1 < sizeof buf) buf[o++] = (char)c;
      buf[o] = 0;
      if (c != 0 || !o) ok = false; else r.add(buf);
    }
  }
  fclose(f);
  return ok;
}

static FileOpProgress g_helper_prog;
static void helper_sigterm(int) { __atomic_store_n(&g_helper_prog.cancel, 1u, __ATOMIC_RELEASE); }

int helper_main(const char* request_file) {
  FileOpRequest r;
  if (!request_read(request_file, r)) { fprintf(stderr, "mattexplorer --helper: cannot read the request\n"); return 3; }
  unlink(request_file); // one use; the file belongs to the user who asked
  if (r.kind != OP_CHMOD && r.kind != OP_CHMOD_RESTORE && r.kind != OP_CHOWN && r.kind != OP_CHOWN_RESTORE) { fprintf(stderr, "mattexplorer --helper: this operation is not offered as root\n"); return 3; }
  struct sigaction sa = {}; sa.sa_handler = helper_sigterm; sigaction(SIGTERM, &sa, nullptr); sigaction(SIGINT, &sa, nullptr);
  FileOpCtx ctx; ctx.prog = &g_helper_prog;
  FileOpResult res;
  fileop_run(r, ctx, res);
  printf("%s\nok %u cancelled %u done %u skipped %u errors %u perm %u truncated %u\nerr %s\njournal %u\n", kResMagic, res.ok ? 1u : 0u,
         res.cancelled ? 1u : 0u, res.done, res.skipped, res.errors, res.perm_errors, res.journal_truncated ? 1u : 0u, res.err, res.journal.len);
  fwrite(res.journal.data, 1, res.journal.len, stdout);
  fflush(stdout);
  return 0;
}

static void run_elevated(App& a, OpJob* j) {
  FileOpResult& res = j->res;
  res.ok = false; res.cancelled = false; res.errors = res.skipped = res.done = 0; res.err[0] = 0;
  res.journal.clear(); res.joff.clear(); res.journal_truncated = false; res.perm_errors = 0;
  j->needs_password = j->wrong_password = j->no_sudo = false; j->sudo_msg[0] = 0;
  if (!a.exe_path[0]) { snprintf(res.err, sizeof res.err, "cannot find my own binary for the helper"); res.errors = 1; return; }
  char reqfile[4096];
  if (!request_write(j->req, reqfile, sizeof reqfile)) { snprintf(res.err, sizeof res.err, "cannot write the request: %s", strerror(errno)); res.errors = 1; return; }
  bool have_pw = j->password[0] != 0;

  const char* argv_sudo[] = { "sudo", have_pw ? "-S" : "-n", "-p", "", "--", a.exe_path, "--helper", reqfile, nullptr };
  const char* argv_direct[] = { a.exe_path, "--helper", reqfile, nullptr };
  const char* const* argv = a.no_sudo_run ? argv_direct : argv_sudo;
  int in[2], out[2], err[2];
  if (pipe2(in, O_CLOEXEC) != 0 || pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0) { snprintf(res.err, sizeof res.err, "pipe: %s", strerror(errno)); res.errors = 1; unlink(reqfile); return; }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, in[0], 0);
  posix_spawn_file_actions_adddup2(&fa, out[1], 1);
  posix_spawn_file_actions_adddup2(&fa, err[1], 2);
  pid_t pid = 0;
  int rc = a.no_sudo_run ? posix_spawn(&pid, argv[0], &fa, nullptr, (char* const*)argv, environ)
                         : posix_spawnp(&pid, argv[0], &fa, nullptr, (char* const*)argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(in[0]); close(out[1]); close(err[1]);
  if (rc != 0) {
    close(in[1]); close(out[0]); close(err[0]); unlink(reqfile);
    j->no_sudo = true;
    snprintf(j->sudo_msg, sizeof j->sudo_msg, rc == ENOENT ? "sudo is not installed" : "cannot run sudo: %s", strerror(rc));
    snprintf(res.err, sizeof res.err, "%s", j->sudo_msg); res.errors = 1;
    return;
  }
  __atomic_store_n(&j->child, pid, __ATOMIC_RELEASE);
  if (have_pw) {
    u32 n = (u32)strlen(j->password);
    j->password[n] = '\n';
    const char* w = j->password; u32 left = n + 1;
    while (left) { ssize_t k = write(in[1], w, left); if (k < 0 && errno == EINTR) continue; if (k <= 0) break; w += k; left -= (u32)k; }
  }
  memset(j->password, 0, sizeof j->password);
  close(in[1]);

  Array<char> so, se;
  struct pollfd fds[2] = { { out[0], POLLIN, 0 }, { err[0], POLLIN, 0 } };
  bool open_out = true, open_err = true;
  while (open_out || open_err) {
    fds[0].fd = open_out ? out[0] : -1; fds[1].fd = open_err ? err[0] : -1;
    if (poll(fds, 2, -1) < 0) { if (errno == EINTR) continue; break; }
    if (fds[0].revents) { char b[65536]; ssize_t n = read(out[0], b, sizeof b); if (n > 0) memcpy(so.push_n((u32)n), b, (u32)n); else if (n == 0 || errno != EINTR) open_out = false; }
    if (fds[1].revents) { char b[4096]; ssize_t n = read(err[0], b, sizeof b); if (n > 0) memcpy(se.push_n((u32)n), b, (u32)n); else if (n == 0 || errno != EINTR) open_err = false; }
  }
  close(out[0]); close(err[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  __atomic_store_n(&j->child, (pid_t)0, __ATOMIC_RELEASE);
  unlink(reqfile);
  se.push(0); so.push(0);

  { char* nl = strchr(se.data, '\n'); if (nl) *nl = 0; const char* m = se.data; if (!strncmp(m, "sudo: ", 6)) m += 6; snprintf(j->sudo_msg, sizeof j->sudo_msg, "%s", m); }

  if (!strncmp(so.data, kResMagic, strlen(kResMagic))) {
    u32 ok = 0, cancelled = 0, done = 0, skipped = 0, errors = 0, perm = 0, trunc = 0, jl = 0;
    const char* p = strchr(so.data, '\n');
    if (p && sscanf(p + 1, "ok %u cancelled %u done %u skipped %u errors %u perm %u truncated %u", &ok, &cancelled, &done, &skipped, &errors, &perm, &trunc) == 7) {
      res.ok = ok != 0; res.cancelled = cancelled != 0; res.done = done; res.skipped = skipped; res.errors = errors; res.perm_errors = perm; res.journal_truncated = trunc != 0;
      p = strchr(p + 1, '\n');
      if (p && !strncmp(p + 1, "err ", 4)) { const char* e = p + 5; const char* nl = strchr(e, '\n'); snprintf(res.err, sizeof res.err, "%.*s", nl ? (int)(nl - e) : (int)strlen(e), e); p = nl; }
      if (p && sscanf(p + 1, "journal %u", &jl) == 1) {
        const char* jd = strchr(p + 1, '\n');
        if (jd && jl <= so.len - (u32)(jd + 1 - so.data)) {
          jd++;
          memcpy(res.journal.push_n(jl), jd, jl);
          for (u32 o = 0; o < jl;) { res.joff.push(o); o += (u32)strlen(res.journal.data + o) + 1; }
          if (res.joff.len & 1) res.joff.pop(); // a torn pair
        }
      }
      return;
    }
  }

  res.errors = 1;
  const char* m = j->sudo_msg;
  bool exited1 = WIFEXITED(status) && WEXITSTATUS(status) == 1;
  if (!have_pw && exited1 && (strstr(m, "password is required") || strstr(m, "password"))) { j->needs_password = true; snprintf(res.err, sizeof res.err, "needs a password"); return; }
  if (have_pw && exited1 && (strstr(m, "incorrect password") || strstr(m, "Sorry") || strstr(m, "password"))) { j->wrong_password = true; snprintf(res.err, sizeof res.err, "wrong password"); return; }
  if (WIFSIGNALED(status)) { res.cancelled = true; snprintf(res.err, sizeof res.err, "stopped"); return; }
  j->no_sudo = true;
  snprintf(res.err, sizeof res.err, "%s", m[0] ? m : "sudo did not run the helper");
}

static void poke(OpsQueue& q) {
  if (q.wake_fd < 0) return;
  u64 one = 1;
  while (write(q.wake_fd, &one, sizeof one) < 0 && errno == EINTR) {}
}

static ConflictAnswer thread_ask(void* ctx, const FileOpConflict& c, bool* all) {
  App& a = *(App*)ctx;
  OpsQueue& q = a.ops;
  OpConflict& k = q.conflict;
  snprintf(k.src, sizeof k.src, "%s", c.src);
  snprintf(k.dst, sizeof k.dst, "%s", c.dst);
  k.src_dir = c.src_dir; k.dst_dir = c.dst_dir;
  k.src_size = c.src_size; k.dst_size = c.dst_size;
  k.src_mtime = c.src_mtime; k.dst_mtime = c.dst_mtime;
  OpJob* j = q.asking;
  __atomic_store_n(&j->state, (u32)JOB_ASKING, __ATOMIC_RELEASE);
  poke(q);
  while (sem_wait(&q.answer) != 0 && errno == EINTR) {}
  __atomic_store_n(&j->state, (u32)JOB_RUNNING, __ATOMIC_RELEASE);
  *all = __atomic_load_n(&q.answer_all, __ATOMIC_ACQUIRE) != 0;
  return (ConflictAnswer)__atomic_load_n(&q.answer_value, __ATOMIC_ACQUIRE);
}

static ErrorAnswer thread_error(void* ctx, const char* path, const char* reason, bool* skip_all) {
  App& a = *(App*)ctx;
  OpsQueue& q = a.ops;
  snprintf(q.error.path, sizeof q.error.path, "%s", path);
  snprintf(q.error.reason, sizeof q.error.reason, "%s", reason);
  OpJob* j = q.asking;
  __atomic_store_n(&q.asking_error, 1u, __ATOMIC_RELEASE);
  __atomic_store_n(&j->state, (u32)JOB_ASKING, __ATOMIC_RELEASE);
  poke(q);
  while (sem_wait(&q.answer) != 0 && errno == EINTR) {}
  __atomic_store_n(&j->state, (u32)JOB_RUNNING, __ATOMIC_RELEASE);
  __atomic_store_n(&q.asking_error, 0u, __ATOMIC_RELEASE);
  *skip_all = __atomic_load_n(&q.answer_all, __ATOMIC_ACQUIRE) != 0;
  return (ErrorAnswer)__atomic_load_n(&q.answer_value, __ATOMIC_ACQUIRE);
}

static void thread_notify(void* ctx) {
  App& a = *(App*)ctx;
  OpsQueue& q = a.ops;
  i64 now = window_now_ms();
  if (now - __atomic_load_n(&q.last_wake_ms, __ATOMIC_RELAXED) < 80) return;
  __atomic_store_n(&q.last_wake_ms, now, __ATOMIC_RELAXED);
  poke(q);
}

static void* ops_main(void* arg) {
  App& a = *(App*)arg;
  OpsQueue& q = a.ops;
  for (;;) {
    while (sem_wait(&q.work) != 0 && errno == EINTR) {}
    if (!__atomic_load_n(&q.running, __ATOMIC_ACQUIRE)) return nullptr;
    u32 h = __atomic_load_n(&q.head, __ATOMIC_RELAXED);
    u32 t = __atomic_load_n(&q.tail, __ATOMIC_ACQUIRE);
    if (h == t) continue;
    OpJob* j = q.ring[h % MX_ARRAY_COUNT(q.ring)];
    __atomic_store_n(&q.head, h + 1, __ATOMIC_RELEASE);
    q.asking = j;
    __atomic_store_n(&j->state, (u32)JOB_RUNNING, __ATOMIC_RELEASE);
    if (j->elevated) run_elevated(a, j);
    else {
      FileOpCtx ctx;
      ctx.prog = &j->prog; ctx.ask = thread_ask; ctx.ask_ctx = &a; ctx.notify = thread_notify; ctx.notify_ctx = &a;
      ctx.on_error = thread_error; ctx.error_ctx = &a;
      fileop_run(j->req, ctx, j->res);
    }
    q.asking = nullptr;
    __atomic_store_n(&j->state, (u32)JOB_DONE, __ATOMIC_RELEASE);
    __atomic_store_n(&q.last_wake_ms, (i64)0, __ATOMIC_RELAXED);
    poke(q);
  }
}

bool ops_start(App& a) {
  OpsQueue& q = a.ops;
  if (q.running) return true;
  q.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (q.wake_fd < 0) return false;
  if (sem_init(&q.work, 0, 0) != 0 || sem_init(&q.answer, 0, 0) != 0) { close(q.wake_fd); q.wake_fd = -1; return false; }
  __atomic_store_n(&q.running, true, __ATOMIC_RELEASE);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 1024 * 1024);
  int r = pthread_create(&q.thread, &attr, ops_main, &a);
  pthread_attr_destroy(&attr);
  if (r != 0) { __atomic_store_n(&q.running, false, __ATOMIC_RELEASE); sem_destroy(&q.work); sem_destroy(&q.answer); close(q.wake_fd); q.wake_fd = -1; return false; }
  return true;
}

void ops_stop(App& a) {
  OpsQueue& q = a.ops;
  if (!q.running) return;
  for (OpJob* j : a.jobs) __atomic_store_n(&j->prog.cancel, 1u, __ATOMIC_RELEASE); // a copy in flight stops at the next chunk
  if (q.asking) { __atomic_store_n(&q.answer_value, (u32)CONFLICT_CANCEL, __ATOMIC_RELEASE); sem_post(&q.answer); }
  __atomic_store_n(&q.running, false, __ATOMIC_RELEASE);
  sem_post(&q.work);
  pthread_join(q.thread, nullptr);
  sem_destroy(&q.work); sem_destroy(&q.answer);
  close(q.wake_fd); q.wake_fd = -1;
}

bool ops_busy(const App& a) { return a.jobs.len > 0; }

static void redo_free(RedoRecord* r) {
  if (!r) return;
  r->journal.release(); r->joff.release(); r->ujournal.release(); r->ujoff.release();
  free(r);
}
void redo_record_free(RedoRecord* r) { redo_free(r); }

static void job_free(OpJob* j) {
  j->req.paths.release(); j->req.off.release();
  j->res.journal.release(); j->res.joff.release();
  redo_free(j->redo);
  free(j);
}

OpJob* ops_submit(App& a, FileOpRequest& req, u8 undo_kind, const char* label, bool is_undo, bool elevated, const char* password, RedoRecord* redo, bool is_redo) {
  OpsQueue& q = a.ops;
  if (q.running) {
    u32 h = __atomic_load_n(&q.head, __ATOMIC_ACQUIRE), t = __atomic_load_n(&q.tail, __ATOMIC_RELAXED);
    if (t - h >= MX_ARRAY_COUNT(q.ring)) { set_status_error(a, "Too many file operations queued"); return nullptr; }
  }
  OpJob* j = (OpJob*)calloc(1, sizeof(OpJob));
  if (!j) return nullptr;
  j->req.kind = req.kind;
  j->req.paths = static_cast<Array<char>&&>(req.paths);
  j->req.off = static_cast<Array<u32>&&>(req.off);
  memcpy(j->req.dest, req.dest, sizeof j->req.dest);
  j->req.policy = req.policy; j->req.force_copy = req.force_copy;
  j->req.mode_value = req.mode_value; j->req.mode_mask = req.mode_mask; j->req.recursive = req.recursive;
  j->req.uid = req.uid; j->req.gid = req.gid;
  j->state = JOB_QUEUED; j->id = q.next_id++;
  j->undo_kind = undo_kind; j->is_undo = is_undo; j->is_redo = is_redo;
  j->redo = redo;
  j->elevated = elevated;
  if (elevated && password) snprintf(j->password, sizeof j->password, "%s", password);
  j->started_ms = window_now_ms(); j->rate_ms = j->started_ms;
  snprintf(j->label, sizeof j->label, "%s", label);
  snprintf(j->where, sizeof j->where, "%s", path_base(j->req.dest));
  a.jobs.push(j);
  if (q.running) {
    u32 t = __atomic_load_n(&q.tail, __ATOMIC_RELAXED);
    q.ring[t % MX_ARRAY_COUNT(q.ring)] = j;
    __atomic_store_n(&q.tail, t + 1, __ATOMIC_RELEASE);
    sem_post(&q.work);
  } else { // inline: run now, report now
    j->state = JOB_RUNNING;
    j->req.policy = j->req.policy == CONFLICT_ASK ? q.inline_policy : j->req.policy;
    if (elevated) run_elevated(a, j);
    else { FileOpCtx ctx; ctx.prog = &j->prog; fileop_run(j->req, ctx, j->res); }
    j->state = JOB_DONE;
    ops_poll(a);
  }
  a.w.need_redraw = true;
  return j;
}

void ops_cancel(App& a, OpJob* j) {
  __atomic_store_n(&j->prog.cancel, 1u, __ATOMIC_RELEASE);
  if (j->elevated) { pid_t c = __atomic_load_n(&j->child, __ATOMIC_ACQUIRE); if (c > 0) kill(c, SIGTERM); }
  if (a.dialog.kind == DLG_CONFLICT && a.dialog.job == j) ops_answer(a, CONFLICT_CANCEL, false);
  if (a.dialog.kind == DLG_OP_ERROR && a.dialog.job == j) ops_answer_error(a, ERR_CANCEL, false);
  if (j->state == JOB_QUEUED) {
    set_status(a, "Cancelled");
  }
}

void ops_answer(App& a, ConflictAnswer ans, bool all) {
  OpsQueue& q = a.ops;
  OpJob* j = a.dialog.kind == DLG_CONFLICT ? a.dialog.job : nullptr;
  __atomic_store_n(&q.answer_value, (u32)ans, __ATOMIC_RELEASE);
  __atomic_store_n(&q.answer_all, all ? 1u : 0u, __ATOMIC_RELEASE);
  if (a.dialog.kind == DLG_CONFLICT) dialog_close(a);

  if (j) __atomic_store_n(&j->state, (u32)JOB_RUNNING, __ATOMIC_RELEASE);
  if (q.running) sem_post(&q.answer);
}

void ops_answer_error(App& a, ErrorAnswer ans, bool all) {
  OpsQueue& q = a.ops;
  OpJob* j = a.dialog.kind == DLG_OP_ERROR ? a.dialog.job : nullptr;
  __atomic_store_n(&q.answer_value, (u32)ans, __ATOMIC_RELEASE);
  __atomic_store_n(&q.answer_all, all ? 1u : 0u, __ATOMIC_RELEASE);
  if (a.dialog.kind == DLG_OP_ERROR) dialog_close(a);
  if (j) __atomic_store_n(&j->state, (u32)JOB_RUNNING, __ATOMIC_RELEASE);
  if (q.running) sem_post(&q.answer);
}

static const char* undo_what(u8 kind) {
  switch (kind) {
    case UNDO_MOVE: return "move of";
    case UNDO_COPY: return "copy of";
    case UNDO_TRASH: return "trashing";
    case UNDO_RENAME: return "rename of";
    case UNDO_NEW_FOLDER: return "creating";
    case UNDO_CHMOD: return "permission change of";
    case UNDO_CHOWN: return "owner change of";
    case UNDO_RESTORE: return "restoring";
    default: return "";
  }
}

static void redo_clear(App& a) {
  for (u32 i = 0; i < a.redo_count; i++) { RedoRecord& r = a.redo[i]; r.journal.clear(); r.joff.clear(); r.ujournal.clear(); r.ujoff.clear(); r.kind = UNDO_NONE; }
  a.redo_count = 0;
}

static void undo_push(App& a, u8 kind, const FileOpResult& res, const char* label, bool elevated = false) {
  if (kind == UNDO_NONE || !res.pairs() || res.journal_truncated) return;
  if (a.undo_count == MAX_UNDO) { // forget the oldest
    for (u32 i = 1; i < MAX_UNDO; i++) {
      a.undo[i - 1].kind = a.undo[i].kind;
      a.undo[i - 1].journal = static_cast<Array<char>&&>(a.undo[i].journal);
      a.undo[i - 1].joff = static_cast<Array<u32>&&>(a.undo[i].joff);
      memcpy(a.undo[i - 1].label, a.undo[i].label, sizeof a.undo[i].label);
    }
    a.undo_count--;
  }
  UndoRecord& u = a.undo[a.undo_count++];
  u.kind = kind;
  u.journal.clear(); u.joff.clear();
  memcpy(u.journal.push_n(res.journal.len), res.journal.data, res.journal.len);
  memcpy(u.joff.push_n(res.joff.len), res.joff.data, res.joff.len * sizeof(u32));
  snprintf(u.label, sizeof u.label, "%s %s", undo_what(kind), label);
  u.elevated = elevated;
}

void op_undo(App& a) {
  if (!a.undo_count) { set_status(a, "Nothing to undo"); return; }
  UndoRecord& u = a.undo[a.undo_count - 1];
  u32 pairs = u.joff.len / 2;
  auto from = [&](u32 i) { return u.journal.data + u.joff[2 * i]; };
  auto to = [&](u32 i) { return u.journal.data + u.joff[2 * i + 1]; };
  FileOpRequest r;
  char label[160];
  snprintf(label, sizeof label, "%s", u.label);
  bool elevated = u.elevated;

  RedoRecord* rr = (RedoRecord*)calloc(1, sizeof(RedoRecord));
  rr->kind = u.kind; rr->elevated = u.elevated;
  memcpy(rr->label, u.label, sizeof rr->label);
  memcpy(rr->journal.push_n(u.journal.len), u.journal.data, u.journal.len);
  memcpy(rr->joff.push_n(u.joff.len), u.joff.data, u.joff.len * sizeof(u32));
  switch (u.kind) {
    case UNDO_MOVE: case UNDO_RENAME:
      r.kind = OP_RENAMES;
      for (u32 i = 0; i < pairs; i++) { r.add(to(i)); r.add(from(i)); }
      break;
    case UNDO_COPY:
      r.kind = OP_DELETE;
      for (u32 i = 0; i < pairs; i++) r.add(to(i));
      break;
    case UNDO_TRASH:
      r.kind = OP_RESTORE;
      for (u32 i = 0; i < pairs; i++) { r.add(to(i)); r.add(from(i)); }
      break;
    case UNDO_RESTORE: // (trashed, original): trash the originals again
      r.kind = OP_TRASH;
      for (u32 i = 0; i < pairs; i++) r.add(to(i));
      break;
    case UNDO_CHMOD: case UNDO_CHOWN:
      r.kind = u.kind == UNDO_CHMOD ? OP_CHMOD_RESTORE : OP_CHOWN_RESTORE;
      for (u32 i = 0; i < pairs; i++) { r.add(from(i)); r.add(to(i)); }
      break;
    case UNDO_NEW_FOLDER: {
      char msg[600];
      if (rmdir(to(0)) == 0) {
        snprintf(msg, sizeof msg, "Removed %s \xc2\xb7 Ctrl+Y to redo", path_base(to(0))); set_status(a, msg); tabs_gone_under(a, to(0)); refresh_path(a, from(0));
        a.undo_count--;
        if (a.redo_count == MAX_UNDO) { redo_free(rr); return; }
        RedoRecord& d = a.redo[a.redo_count++];
        d.kind = rr->kind; d.elevated = false; memcpy(d.label, rr->label, sizeof d.label);
        d.journal = static_cast<Array<char>&&>(rr->journal); d.joff = static_cast<Array<u32>&&>(rr->joff);
        d.ujournal.clear(); d.ujoff.clear();
        free(rr);
        return;
      }
      snprintf(msg, sizeof msg, "Cannot undo: %s (%s)", path_base(to(0)), errno == ENOTEMPTY ? "folder is not empty" : strerror(errno)); set_status_error(a, msg);
      a.undo_count--;
      redo_free(rr);
      return;
    }
    default: a.undo_count--; redo_free(rr); return;
  }
  a.undo_count--;
  u.journal.clear(); u.joff.clear();
  if (!ops_submit(a, r, u.kind, label, true, elevated, nullptr, rr)) redo_free(rr);
}

static void redo_push(App& a, OpJob* j) {
  RedoRecord* rr = j->redo;
  if (!rr) return;
  j->redo = nullptr;
  const FileOpResult& res = j->res;
  bool needs_journal = rr->kind == UNDO_MOVE || rr->kind == UNDO_RENAME || rr->kind == UNDO_TRASH || rr->kind == UNDO_RESTORE || rr->kind == UNDO_CHMOD || rr->kind == UNDO_CHOWN;
  if ((needs_journal && (!res.pairs() || res.journal_truncated)) || (!needs_journal && !res.ok)) { redo_free(rr); return; }
  if (a.redo_count == MAX_UNDO) { // forget the oldest
    RedoRecord& oldest = a.redo[0];
    oldest.journal.release(); oldest.joff.release(); oldest.ujournal.release(); oldest.ujoff.release();
    for (u32 i = 1; i < MAX_UNDO; i++) {
      a.redo[i - 1].kind = a.redo[i].kind; a.redo[i - 1].elevated = a.redo[i].elevated;
      a.redo[i - 1].journal = static_cast<Array<char>&&>(a.redo[i].journal); a.redo[i - 1].joff = static_cast<Array<u32>&&>(a.redo[i].joff);
      a.redo[i - 1].ujournal = static_cast<Array<char>&&>(a.redo[i].ujournal); a.redo[i - 1].ujoff = static_cast<Array<u32>&&>(a.redo[i].ujoff);
      memcpy(a.redo[i - 1].label, a.redo[i].label, sizeof a.redo[i].label);
    }
    a.redo_count--;
  }
  RedoRecord& d = a.redo[a.redo_count++];
  d.kind = rr->kind; d.elevated = rr->elevated || j->elevated;
  memcpy(d.label, rr->label, sizeof d.label);
  d.journal = static_cast<Array<char>&&>(rr->journal); d.joff = static_cast<Array<u32>&&>(rr->joff);
  d.ujournal.clear(); d.ujoff.clear();
  memcpy(d.ujournal.push_n(res.journal.len), res.journal.data, res.journal.len);
  memcpy(d.ujoff.push_n(res.joff.len), res.joff.data, res.joff.len * sizeof(u32));
  free(rr);
}

void op_redo(App& a) {
  if (!a.redo_count) { set_status(a, "Nothing to redo"); return; }
  RedoRecord& d = a.redo[a.redo_count - 1];
  u32 pairs = d.joff.len / 2, upairs = d.ujoff.len / 2;
  auto from = [&](u32 i) { return d.journal.data + d.joff[2 * i]; };
  auto to = [&](u32 i) { return d.journal.data + d.joff[2 * i + 1]; };
  auto ufrom = [&](u32 i) { return d.ujournal.data + d.ujoff[2 * i]; };
  auto uto = [&](u32 i) { return d.ujournal.data + d.ujoff[2 * i + 1]; };
  FileOpRequest r;
  char label[160];
  snprintf(label, sizeof label, "%s", d.label);
  bool elevated = d.elevated;
  u8 kind = d.kind;
  switch (kind) {
    case UNDO_MOVE: case UNDO_RENAME:
      r.kind = OP_RENAMES;
      for (u32 i = 0; i < upairs; i++) { r.add(uto(i)); r.add(ufrom(i)); }
      break;
    case UNDO_COPY: // copy the originals into the same folder again
      r.kind = OP_COPY;
      for (u32 i = 0; i < pairs; i++) r.add(from(i));
      if (pairs) { snprintf(r.dest, sizeof r.dest, "%s", to(0)); if (char* sl = strrchr(r.dest, '/')) { if (sl == r.dest) sl[1] = 0; else *sl = 0; } }
      break;
    case UNDO_TRASH:
      r.kind = OP_TRASH;
      for (u32 i = 0; i < upairs; i++) r.add(uto(i));
      break;
    case UNDO_RESTORE:
      r.kind = OP_RESTORE;
      for (u32 i = 0; i < upairs; i++) { r.add(uto(i)); r.add(ufrom(i)); }
      if (upairs) { snprintf(r.dest, sizeof r.dest, "%s", ufrom(0)); if (char* sl = strrchr(r.dest, '/')) { if (sl == r.dest) sl[1] = 0; else *sl = 0; } }
      break;
    case UNDO_CHMOD: case UNDO_CHOWN:
      r.kind = kind == UNDO_CHMOD ? OP_CHMOD_RESTORE : OP_CHOWN_RESTORE;
      for (u32 i = 0; i < upairs; i++) { r.add(ufrom(i)); r.add(uto(i)); }
      break;
    case UNDO_NEW_FOLDER: {
      char msg[600];
      if (pairs && mkdir(to(0), 0755) == 0) {
        FileOpResult fake;
        u32 n1 = (u32)strlen(from(0)), n2 = (u32)strlen(to(0));
        fake.joff.push(0); memcpy(fake.journal.push_n(n1 + 1), from(0), n1 + 1);
        fake.joff.push(fake.journal.len); memcpy(fake.journal.push_n(n2 + 1), to(0), n2 + 1);
        snprintf(msg, sizeof msg, "Created %s again \xc2\xb7 Ctrl+Z to undo", path_base(to(0)));
        refresh_path(a, from(0));
        d.journal.clear(); d.joff.clear(); d.ujournal.clear(); d.ujoff.clear();
        a.redo_count--;
        undo_push(a, UNDO_NEW_FOLDER, fake, path_base(fake.journal.data + fake.joff[1]));
        set_status(a, msg);
      } else { snprintf(msg, sizeof msg, "Cannot redo: %s", strerror(errno)); set_status_error(a, msg); a.redo_count--; d.journal.clear(); d.joff.clear(); }
      return;
    }
    default: a.redo_count--; return;
  }
  a.redo_count--;
  d.journal.clear(); d.joff.clear(); d.ujournal.clear(); d.ujoff.clear();
  if (!r.count()) { set_status(a, "Nothing to redo"); return; }
  ops_submit(a, r, kind, label, false, elevated, nullptr, nullptr, true);
}

static void job_message(App& a, OpJob* j) {
  const FileOpRequest& q = j->req;
  const FileOpResult& r = j->res;
  char msg[900], what[300];
  bool restore_or_rename = q.kind == OP_RENAMES || q.kind == OP_RESTORE;
  u32 n = restore_or_rename ? q.count() / 2 : q.count();
  if (j->is_undo) snprintf(what, sizeof what, "Undid %s", j->label);
  else if (j->is_redo) snprintf(what, sizeof what, "Redid %s", j->label);
  else if (q.kind == OP_RESTORE) { if (n == 1) snprintf(what, sizeof what, "Restored %s", j->label); else snprintf(what, sizeof what, "Restored %u items", r.done); }
  else if (q.kind == OP_DELETE && !strcmp(j->label, "the trash")) snprintf(what, sizeof what, "Emptied the trash");
  else if (q.kind == OP_TRASH) { if (n == 1) snprintf(what, sizeof what, "Moved %s to the trash", j->label); else snprintf(what, sizeof what, "Moved %u items to the trash", r.done); }
  else {
    const char* verb = fileop_done_verb(q.kind);
    if (q.kind == OP_CHOWN && q.uid < 0) verb = "Changed the group of";
    if (n == 1) snprintf(what, sizeof what, "%s %s", verb, j->label);
    else snprintf(what, sizeof what, "%s %u items", verb, r.done);
  }
  if (j->elevated) strncat(what, " as root", sizeof what - strlen(what) - 1);
  if (r.cancelled) {
    snprintf(msg, sizeof msg, "%s cancelled%s", fileop_verb(q.kind), r.done ? " after some items" : "");
    set_status(a, msg);
  } else if (!r.ok) {
    if (r.errors == 1 && n == 1) snprintf(msg, sizeof msg, "%s failed: %s", fileop_verb(q.kind), r.err);
    else snprintf(msg, sizeof msg, "%s, %u skipped after errors: %s", what, r.errors, r.err);
    set_status_error(a, msg);
    a.ops_failed++;
  } else {
    const char* undo = j->is_undo ? (j->redo ? " \xc2\xb7 Ctrl+Y to redo" : "") : j->undo_kind != UNDO_NONE ? (r.journal_truncated ? " (too many items to undo)" : " \xc2\xb7 Ctrl+Z to undo") : "";
    if (q.kind == OP_COPY || q.kind == OP_MOVE || q.kind == OP_RESTORE) snprintf(msg, sizeof msg, "%s to %s%s%s", what, j->where, r.skipped ? " (some skipped)" : "", undo);
    else snprintf(msg, sizeof msg, "%s%s%s", what, r.skipped ? " (some skipped)" : "", undo);
    set_status(a, msg);
    a.ops_done++;
  }
  if (a.debug) fprintf(stderr, "ops: job %u %s: ok=%d done=%u skipped=%u errors=%u cancelled=%d in %lld ms%s%s\n", j->id, fileop_verb(q.kind), r.ok, r.done,
                       r.skipped, r.errors, r.cancelled, (long long)(window_now_ms() - j->started_ms), r.err[0] ? " err=" : "", r.err);
}

static bool path_under(const char* path, const char* dir) {
  usize n = strlen(dir);
  return !strncmp(path, dir, n) && (path[n] == 0 || path[n] == '/');
}
static void tabs_follow(App& a, const char* from, const char* to) {
  for (Tab& t : a.tabs) {
    if (!t.used || !path_under(t.path, from)) continue;
    char np[4096];
    snprintf(np, sizeof np, "%s%s", to, t.path + strlen(from));
    navigate(a, t, np, false);
  }
}
static void tabs_gone_under(App& a, const char* dir) {
  for (Tab& t : a.tabs) if (t.used && path_under(t.path, dir)) tab_gone(a, t);
}

static void job_refresh(App& a, OpJob* j) {
  const FileOpRequest& q = j->req;
  const FileOpResult& r = j->res;

  if (q.kind == OP_MOVE || q.kind == OP_RENAMES || q.kind == OP_RESTORE) for (u32 i = 0; i < r.pairs(); i++) tabs_follow(a, r.from(i), r.to(i));
  else if (q.kind == OP_DELETE || q.kind == OP_TRASH) for (u32 i = 0; i < r.pairs(); i++) tabs_gone_under(a, r.from(i));

  bool chmod = q.kind == OP_CHMOD || q.kind == OP_CHMOD_RESTORE || q.kind == OP_CHOWN || q.kind == OP_CHOWN_RESTORE;
  bool pairs = q.kind == OP_RENAMES || q.kind == OP_RESTORE || q.kind == OP_CHMOD_RESTORE || q.kind == OP_CHOWN_RESTORE;
  Array<char> dirs; Array<u32> doff;
  auto add_parent = [&](const char* p) {
    const char* slash = strrchr(p, '/');
    if (!slash) return;
    u32 n = slash == p ? 1 : (u32)(slash - p);
    for (u32 k = 0; k < doff.len; k++) if (!strncmp(dirs.data + doff[k], p, n) && !dirs.data[doff[k] + n]) return;
    if (doff.len >= 64) return;
    doff.push(dirs.len); memcpy(dirs.push_n(n + 1), p, n); dirs.data[dirs.len - 1] = 0;
  };
  for (u32 i = 0; i < q.count(); i += pairs ? 2 : 1) add_parent(q.path(i));
  for (u32 i = 0; i < r.pairs(); i++) add_parent(r.to(i));
  if (q.dest[0]) { u32 n = (u32)strlen(q.dest); bool have = false; for (u32 k = 0; k < doff.len; k++) if (!strcmp(dirs.data + doff[k], q.dest)) have = true; if (!have && doff.len < 64) { doff.push(dirs.len); memcpy(dirs.push_n(n + 1), q.dest, n + 1); } }
  for (u32 k = 0; k < doff.len; k++)
    for (Tab& t : a.tabs) if (t.used && !strcmp(t.path, dirs.data + doff[k])) refresh_tab(a, t, nullptr, 0, chmod);

  Tab& t = tab_active(a);
  if ((q.kind == OP_COPY || q.kind == OP_MOVE) && !strcmp(t.path, q.dest) && r.pairs()) {
    const char* names[256]; u32 n = 0;
    for (u32 i = 0; i < r.pairs() && n < MX_ARRAY_COUNT(names); i++) names[n++] = path_base(r.to(i));
    select_names(a, t, names, n);
  }
  mounts_request_refresh(a);
}

static void trash_failed_dialog(App& a, OpJob* j) {
  Dialog& d = a.dialog;
  d.pending.paths.clear(); d.pending.off.clear();
  d.pending.kind = OP_DELETE; d.pending.dest[0] = 0;
  u32 n = 0; const char* first = nullptr;
  for (u32 i = 0; i < j->req.count(); i++) {
    const char* p = j->req.path(i);
    struct stat st;
    if (lstat(p, &st) != 0) continue;
    d.pending.add(p);
    if (!first) first = p;
    n++;
  }
  if (!n) return;
  d.kind = DLG_TRASH_FAILED;
  snprintf(d.title, sizeof d.title, "Cannot move %s to the trash", n == 1 ? path_base(first) : "these items");
  d.nlines = 0;
  snprintf(d.lines[d.nlines++], sizeof d.lines[0], "%s", j->res.err);
  snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Delete %s permanently instead? This cannot be undone.", n == 1 ? "it" : "them");
  d.nbuttons = 0; d.buttons[d.nbuttons++] = "Delete permanently"; d.buttons[d.nbuttons++] = "Cancel";
  d.focus = 1; d.cancel = 1; d.hover = -1; d.check_label = nullptr; d.checked = false;
  d.pending_undo = UNDO_NONE;
  if (n == 1) snprintf(d.pending_label, sizeof d.pending_label, "%s", path_base(first));
  else snprintf(d.pending_label, sizeof d.pending_label, "%u items", n);
  a.w.need_redraw = true;
}

void ops_poll(App& a) {
  OpsQueue& q = a.ops;
  if (q.wake_fd >= 0) { u64 v; while (read(q.wake_fd, &v, sizeof v) < 0 && errno == EINTR) {} }

  for (OpJob* j : a.jobs)
    if (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) == JOB_DONE && j->elevated && !j->needs_password && !j->wrong_password && !j->no_sudo && !j->res.cancelled)
      a.sudo_ok_ms = window_now_ms();
  for (u32 i = 0; i < a.jobs.len;) {
    OpJob* j = a.jobs[i];
    u32 st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
    if (st == JOB_ASKING && a.dialog.kind == DLG_NONE && !a.menu.open && __atomic_load_n(&q.asking_error, __ATOMIC_ACQUIRE)) {

      Dialog& d = a.dialog;
      d.kind = DLG_OP_ERROR; d.job = j;
      const char* verb = j->req.kind == OP_COPY ? "copy" : j->req.kind == OP_MOVE ? "move" : j->req.kind == OP_DELETE ? "delete" : j->req.kind == OP_TRASH ? "move to the trash" : "process";
      snprintf(d.title, sizeof d.title, "Cannot %s %s", verb, path_base(q.error.path));
      d.nlines = 0;
      snprintf(d.lines[d.nlines++], sizeof d.lines[0], "%s", q.error.reason);
      snprintf(d.lines[d.nlines++], sizeof d.lines[0], "In %s", q.error.path);
      if (char* sl = strrchr(d.lines[1] + 3, '/')) *sl = 0;
      snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Try again, leave it out and carry on, or stop?");
      d.nbuttons = 0;
      d.buttons[d.nbuttons++] = "Retry"; d.buttons[d.nbuttons++] = "Skip"; d.buttons[d.nbuttons++] = "Skip all"; d.buttons[d.nbuttons++] = "Cancel";
      d.focus = 0; d.cancel = 3; d.hover = -1; d.check_label = nullptr; d.checked = false;
      a.w.need_redraw = true;
      i++;
      continue;
    }
    if (st == JOB_ASKING && a.dialog.kind == DLG_NONE && !a.menu.open) {

      OpConflict& k = q.conflict;
      Dialog& d = a.dialog;
      d.kind = DLG_CONFLICT; d.job = j;
      const char* name = path_base(k.dst);
      char dstdir[4096]; snprintf(dstdir, sizeof dstdir, "%s", k.dst);
      if (char* s = strrchr(dstdir, '/')) { if (s == dstdir) s[1] = 0; else *s = 0; }
      snprintf(d.title, sizeof d.title, "%s already exists", k.dst_dir ? "A folder" : "A file");
      d.nlines = 0;
      snprintf(d.lines[d.nlines++], sizeof d.lines[0], "\"%s\" in %s", name, path_base(dstdir));
      char b1[32], tm[32];
      if (!k.src_dir) snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Source: %s, modified %s", fmt_size(k.src_size, b1, sizeof b1), fmt_time(k.src_mtime * 1000000000LL, tm, sizeof tm));
      else snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Source: folder, modified %s", fmt_time(k.src_mtime * 1000000000LL, tm, sizeof tm));
      if (!k.dst_dir) snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Existing: %s, modified %s", fmt_size(k.dst_size, b1, sizeof b1), fmt_time(k.dst_mtime * 1000000000LL, tm, sizeof tm));
      else snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Existing: folder, modified %s", fmt_time(k.dst_mtime * 1000000000LL, tm, sizeof tm));
      d.nbuttons = 0;
      d.buttons[d.nbuttons++] = "Replace"; d.buttons[d.nbuttons++] = "Skip"; d.buttons[d.nbuttons++] = "Keep both"; d.buttons[d.nbuttons++] = "Cancel";
      d.focus = 1; d.cancel = 3; d.hover = -1;
      d.check_label = "Do this for all conflicts"; d.checked = false;
      a.w.need_redraw = true;
      i++;
      continue;
    }
    if (st != JOB_DONE) { i++; continue; }
    bool chmod = j->req.kind == OP_CHMOD || j->req.kind == OP_CHMOD_RESTORE || j->req.kind == OP_CHOWN || j->req.kind == OP_CHOWN_RESTORE;
    if (j->elevated && (j->needs_password || j->wrong_password)) {
      if (a.dialog.kind != DLG_NONE) { i++; continue; } // a dialog is up: ask once it is gone
      if (j->needs_password && !j->reprobed && window_now_ms() - a.sudo_ok_ms < 60000) {

        FileOpRequest r;
        r.kind = j->req.kind; r.paths = static_cast<Array<char>&&>(j->req.paths); r.off = static_cast<Array<u32>&&>(j->req.off);
        r.mode_mask = j->req.mode_mask; r.mode_value = j->req.mode_value; r.recursive = j->req.recursive; r.uid = j->req.uid; r.gid = j->req.gid;
        char label[128]; snprintf(label, sizeof label, "%s", j->label);
        u8 undo_kind = j->undo_kind; bool is_undo = j->is_undo, is_redo = j->is_redo;
        RedoRecord* rr = j->redo; j->redo = nullptr;
        job_free(j); a.jobs.remove_at(i);
        OpJob* n = ops_submit(a, r, undo_kind, label, is_undo, true, nullptr, rr, is_redo);
        if (n) n->reprobed = true; else redo_free(rr);
        return;
      }
      sudo_dialog(a, j); a.jobs.remove_at(i); job_free(j); a.w.need_redraw = true; continue;
    }
    bool retry_as_root = chmod && !j->elevated && j->res.perm_errors && !j->res.cancelled && !j->no_sudo;
    if (!retry_as_root) job_message(a, j);
    if (j->req.kind == OP_TRASH && !j->is_undo && !j->res.cancelled && j->res.errors && a.dialog.kind == DLG_NONE && !a.menu.open) trash_failed_dialog(a, j);
    if (j->is_undo) redo_push(a, j);
    else if (j->res.ok) { if (!j->is_redo) redo_clear(a); undo_push(a, j->undo_kind, j->res, j->label, j->elevated); }
    else if (j->res.pairs() && j->undo_kind != UNDO_NONE) { if (!j->is_redo) redo_clear(a); undo_push(a, j->undo_kind, j->res, j->label, j->elevated); } // partly done: undo what did happen
    job_refresh(a, j);
    if ((a.dialog.kind == DLG_CONFLICT || a.dialog.kind == DLG_OP_ERROR) && a.dialog.job == j) dialog_close(a);
    if (retry_as_root) {

      FileOpRequest r;
      r.kind = j->req.kind; r.paths = static_cast<Array<char>&&>(j->req.paths); r.off = static_cast<Array<u32>&&>(j->req.off);
      r.mode_mask = j->req.mode_mask; r.mode_value = j->req.mode_value; r.recursive = j->req.recursive; r.uid = j->req.uid; r.gid = j->req.gid;
      char label[128]; snprintf(label, sizeof label, "%s", j->label);
      u8 undo_kind = j->undo_kind; bool is_undo = j->is_undo, is_redo = j->is_redo;
      RedoRecord* rr = j->redo; j->redo = nullptr;
      if (a.debug) fprintf(stderr, "ops: job %u: %u permission errors, retrying as root\n", j->id, j->res.perm_errors);
      job_free(j);
      a.jobs.remove_at(i);
      if (!ops_submit(a, r, undo_kind, label, is_undo, true, nullptr, rr, is_redo)) redo_free(rr);
      a.w.need_redraw = true;
      return;
    }
    job_free(j);
    a.jobs.remove_at(i);
    a.w.need_redraw = true;
  }
}

void sudo_dialog(App& a, OpJob* j) {
  Dialog& d = a.dialog;
  const FileOpRequest& q = j->req;
  d.pending.paths.clear(); d.pending.off.clear();
  d.pending.kind = q.kind; d.pending.dest[0] = 0;
  d.pending.mode_mask = q.mode_mask; d.pending.mode_value = q.mode_value; d.pending.recursive = q.recursive;
  d.pending.uid = q.uid; d.pending.gid = q.gid;
  for (u32 i = 0; i < q.count(); i++) d.pending.add(q.path(i));
  d.pending_undo = j->undo_kind; d.pending_is_undo = j->is_undo; d.pending_is_redo = j->is_redo;
  redo_free(d.pending_redo); d.pending_redo = j->redo; j->redo = nullptr;
  snprintf(d.pending_label, sizeof d.pending_label, "%s", j->label);
  d.kind = DLG_SUDO; d.job = nullptr;
  snprintf(d.title, sizeof d.title, "Administrator rights are needed");
  d.nlines = 0;

  bool pairs = q.kind == OP_CHMOD_RESTORE;
  u32 n = pairs ? q.count() / 2 : q.count();
  char owners[3][64]; u32 nowners = 0; bool foreign = false;
  for (u32 i = 0; i < q.count(); i += pairs ? 2 : 1) {
    struct stat st;
    if (lstat(q.path(i), &st) != 0 || st.st_uid == getuid()) continue;
    foreign = true;
    struct passwd* pw = getpwuid(st.st_uid);
    char nm[64]; if (pw) snprintf(nm, sizeof nm, "%s", pw->pw_name); else snprintf(nm, sizeof nm, "uid %u", (u32)st.st_uid);
    bool have = false; for (u32 k = 0; k < nowners; k++) have |= !strcmp(owners[k], nm);
    if (!have && nowners < 3) snprintf(owners[nowners++], sizeof owners[0], "%s", nm);
  }
  const char* what = j->is_undo ? "Undoing this" : "This change";
  bool chown = q.kind == OP_CHOWN || q.kind == OP_CHOWN_RESTORE;
  const char* first = path_base(q.path(0));
  if (chown && (q.uid >= 0 || q.kind == OP_CHOWN_RESTORE)) {
    if (q.uid >= 0 && (uid_t)q.uid == getuid() && n == 1) snprintf(d.lines[d.nlines++], sizeof d.lines[0], "%s belongs to %s. Only root can hand a file over to another user, you included.", first, foreign ? owners[0] : "someone else");
    else if (n == 1) snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Only root can give %s to another user.", first);
    else snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Only root can give these items to another user.");
  } else if (chown) {
    if (foreign && n == 1) snprintf(d.lines[d.nlines++], sizeof d.lines[0], "%s belongs to %s; only its owner or root can change its group.", first, owners[0]);
    else snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Only the owner of an item, or root, can change its group, and only to a group the owner is in.");
  } else if (foreign && n == 1) snprintf(d.lines[d.nlines++], sizeof d.lines[0], "%s belongs to %s; only its owner or root can change its permissions.", first, owners[0]);
  else if (foreign) snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Some of these items belong to %s%s%s; only their owner or root can change their permissions.", owners[0],
                             nowners > 1 ? " and " : "", nowners > 1 ? owners[1] : "");
  else snprintf(d.lines[d.nlines++], sizeof d.lines[0], "Some items inside %s belong to other users; only their owner or root can change their permissions.", j->label);
  snprintf(d.lines[d.nlines++], sizeof d.lines[0], "%s runs MattExplorer's helper once through sudo. The password goes to sudo and is not kept.", what);
  d.error_last = false;
  if (j->wrong_password) { snprintf(d.lines[d.nlines++], sizeof d.lines[0], "%s", j->sudo_msg[0] ? j->sudo_msg : "That password was not accepted."); d.error_last = true; }
  d.nbuttons = 0; d.buttons[d.nbuttons++] = "Change as root"; d.buttons[d.nbuttons++] = "Cancel";
  d.focus = 0; d.cancel = 1; d.hover = -1; d.check_label = nullptr; d.checked = false;
  ui_text_set(d.password, "");
  d.password.mask = true;
  a.path_editing = false;
  a.w.need_redraw = true;
}

static u32 selection_label(App& a, Tab& t, char* out, u32 cap) {
  u32 n = 0; u32 first = 0xFFFFFFFFu;
  for (u32 i = 0; i < t.listing.count(); i++) if (t.selected[i]) { if (first == 0xFFFFFFFFu) first = i; n++; }
  if (n == 1) snprintf(out, cap, "%s", t.listing.cname(first));
  else snprintf(out, cap, "%u items", n);
  return n;
}

u32 selected_paths(App& a, Tab& t, FileOpRequest& into) {
  u32 n = 0;
  char p[4096];
  for (u32 i = 0; i < t.listing.count(); i++) {
    if (!t.selected[i]) continue;
    entry_path(t, i, p, sizeof p);
    into.add(p);
    n++;
  }
  (void)a;
  return n;
}

void op_copy_or_cut(App& a, Tab& t, bool cut) {
  if (!t.selected_count) { set_status(a, "Nothing selected"); return; }
  Clip& c = a.clip;
  c.clear();
  c.cut = cut;
  snprintf(c.dir, sizeof c.dir, "%s", t.path);
  char p[4096];
  for (Tab& ti : a.tabs) if (ti.used) for (u32 i = 0; i < ti.listing.count(); i++) ti.listing.flags[i] &= (u8)~EF_CUT;
  for (u32 i = 0; i < t.listing.count(); i++) {
    if (!t.selected[i]) continue;
    entry_path(t, i, p, sizeof p);
    u32 n = (u32)strlen(p);
    c.off.push(c.paths.len); memcpy(c.paths.push_n(n + 1), p, n + 1);
    if (cut) t.listing.flags[i] |= EF_CUT;
  }
  char label[160], msg[200];
  selection_label(a, t, label, sizeof label);
  snprintf(msg, sizeof msg, "%s %s \xc2\xb7 Ctrl+V to paste", cut ? "Cut" : "Copied", label);
  set_status(a, msg);
  clip_publish(a);
  a.w.need_redraw = true;
}

void op_paste_into(App& a, const char* dest, const Clip& c) {
  if (!c.count()) { set_status(a, "Nothing to paste"); return; }
  FileOpRequest r;
  r.kind = c.cut ? OP_MOVE : OP_COPY;
  snprintf(r.dest, sizeof r.dest, "%s", dest);
  for (u32 i = 0; i < c.count(); i++) r.add(c.path(i));
  if (c.cut && c.dir[0] && !strcmp(c.dir, dest)) { set_status(a, "Already here"); return; }
  char label[160];
  if (c.count() == 1) snprintf(label, sizeof label, "%s", path_base(c.path(0))); else snprintf(label, sizeof label, "%u items", c.count());
  ops_submit(a, r, c.cut ? UNDO_MOVE : UNDO_COPY, label);
}

void op_paste_paths(App& a, Tab& t, const Clip& c) { op_paste_into(a, t.path, c); }

void op_paste(App& a, Tab& t) {
  Window& w = a.w;

  if (w.sel_offer && !w.sel_ours && (w.sel_mimes & (WCLIP_GNOME_FILES | WCLIP_URI_LIST))) {
    u32 mime = (w.sel_mimes & WCLIP_GNOME_FILES) ? WCLIP_GNOME_FILES : WCLIP_URI_LIST;
    int fd = window_clip_receive(w, mime);
    if (fd >= 0) {
      ClipXfer* x = (ClipXfer*)calloc(1, sizeof(ClipXfer));
      x->fd = fd; x->send = false; x->mime = mime;
      snprintf(x->dest, sizeof x->dest, "%s", t.path);
      a.clip_recv_pending++;
      clip_xfer_submit(a, x);
      set_status(a, "Pasting from the clipboard\xe2\x80\xa6");
      return;
    }
  }
  Clip& c = a.clip;
  if (c.text) { set_status(a, "Nothing to paste"); return; }
  op_paste_paths(a, t, c);
  if (c.cut) { // a cut is consumed by the paste, like Explorer
    for (Tab& ti : a.tabs) if (ti.used) for (u32 i = 0; i < ti.listing.count(); i++) ti.listing.flags[i] &= (u8)~EF_CUT;
    c.clear();
    if (a.clip_published) { window_clip_release(a.w); a.clip_published = false; }
  }
}

bool clip_available(const App& a) {
  if (a.clip.count() && !a.clip.text) return true;
  const Window& w = a.w;
  return w.sel_offer && !w.sel_ours && (w.sel_mimes & (WCLIP_URI_LIST | WCLIP_GNOME_FILES));
}

void clip_publish(App& a) {
  a.clip_published = a.clip.count() && window_clip_offer(a.w, a.clip.text ? WCLIP_TEXT : (WCLIP_URI_LIST | WCLIP_GNOME_FILES | WCLIP_TEXT));
}

void clip_send(App& a, int fd, const char* mime, const Clip& c) {
  ClipXfer* x = (ClipXfer*)calloc(1, sizeof(ClipXfer));
  x->fd = fd; x->send = true;
  char uri[4096 * 3];
  bool gnome = !strcmp(mime, "x-special/gnome-copied-files"), uris = gnome || !strncmp(mime, "text/uri-list", 13);
  if (gnome) { const char* head = c.cut ? "cut" : "copy"; memcpy(x->data.push_n((u32)strlen(head)), head, (u32)strlen(head)); }
  for (u32 i = 0; i < c.count(); i++) {
    const char* line = c.path(i);
    if (uris) { path_to_uri(c.path(i), uri, sizeof uri); line = uri; }
    if (gnome) x->data.push('\n');
    else if (i) { if (uris) { x->data.push('\r'); } x->data.push('\n'); }
    u32 n = (u32)strlen(line);
    memcpy(x->data.push_n(n), line, n);
  }
  if (uris && !gnome) { x->data.push('\r'); x->data.push('\n'); }
  clip_xfer_submit(a, x);
}

void clip_send_done(App& a, ClipXfer* x) {
  if (a.debug) fprintf(stderr, "clipboard: sent %u bytes%s\n", x->data.len, x->ok ? "" : " (the reader went away)");
  x->data.release();
  free(x);
}

void clip_recv_done(App& a, ClipXfer* x) {
  if (a.clip_recv_pending) a.clip_recv_pending--;
  Clip c;
  char path[4096];
  bool first = true;
  Str text(x->data.data, x->data.len);
  for (u32 pos = 0; pos < text.n;) {
    u32 s = pos;
    while (pos < text.n && text.p[pos] != '\n') pos++;
    Str l(text.p + s, pos - s);
    if (pos < text.n) pos++;
    while (l.n && (l.p[l.n - 1] == '\r' || l.p[l.n - 1] == ' ')) l.n--;
    if (first && x->mime == WCLIP_GNOME_FILES) { c.cut = str_eq(l, "cut"); first = false; continue; }
    first = false;
    if (!l.n || l.p[0] == '#') continue;
    if (uri_to_path(l, path, sizeof path)) c.add(path);
  }
  if (a.debug) fprintf(stderr, "%s: received %u bytes, %u paths%s\n", x->dnd ? "dnd" : "clipboard", x->data.len, c.count(), c.cut ? " (cut)" : "");
  if (x->dnd) {
    window_dnd_finish(a.w, x->ok && c.count() > 0);
    if (!x->ok) set_status_error(a, "The drag's source did not answer");
    else if (!c.count()) set_status_error(a, "Nothing droppable in that drag");
    else dnd_drop_paths(a, x->dest, c, x->action);
  }
  else if (!x->ok) set_status_error(a, "The clipboard's owner did not answer");
  else if (!c.count()) set_status_error(a, "Nothing on the clipboard to paste here");
  else op_paste_into(a, x->dest, c);
  x->data.release();
  free(x);
  a.w.need_redraw = true;
}

void clip_cancelled(App& a) {
  a.clip_published = false;
  a.w.need_redraw = true;
}

void op_trash(App& a, Tab& t) {
  if (!t.selected_count) { set_status(a, "Nothing selected"); return; }
  FileOpRequest r;
  r.kind = OP_TRASH;
  selected_paths(a, t, r);
  char label[160];
  selection_label(a, t, label, sizeof label);
  ops_submit(a, r, UNDO_TRASH, label);
}

void op_delete(App& a, Tab& t, bool confirmed) {
  if (!t.selected_count) { set_status(a, "Nothing selected"); return; }
  char label[160];
  u32 n = selection_label(a, t, label, sizeof label);
  if (!confirmed) {
    Dialog& d = a.dialog;
    d.kind = DLG_CONFIRM_DELETE;
    snprintf(d.title, sizeof d.title, "Permanently delete %s?", n == 1 ? "this item" : "these items");
    d.nlines = 0;
    snprintf(d.lines[d.nlines++], sizeof d.lines[0], "%s will be deleted, not moved to the trash.", label);
    snprintf(d.lines[d.nlines++], sizeof d.lines[0], "This cannot be undone.");
    d.nbuttons = 0; d.buttons[d.nbuttons++] = "Delete"; d.buttons[d.nbuttons++] = "Cancel";
    d.focus = 1; d.cancel = 1; d.hover = -1; d.check_label = nullptr;
    d.pending.paths.clear(); d.pending.off.clear();
    d.pending.kind = OP_DELETE; d.pending.dest[0] = 0;
    selected_paths(a, t, d.pending);
    d.pending_undo = UNDO_NONE;
    snprintf(d.pending_label, sizeof d.pending_label, "%s", label);
    a.w.need_redraw = true;
    return;
  }
  FileOpRequest r;
  r.kind = OP_DELETE;
  selected_paths(a, t, r);
  ops_submit(a, r, UNDO_NONE, label);
}

void op_new_folder(App& a, Tab& t) {
  if (t.dirfd < 0) return;
  char name[300];
  if (!fileop_new_folder(t.dirfd, "New folder", name, sizeof name)) {
    char msg[300]; snprintf(msg, sizeof msg, "Cannot create a folder here: %s", strerror(errno)); set_status_error(a, msg); return;
  }

  FileOpResult fake;
  char full[4096];
  snprintf(full, sizeof full, "%s/%s", strcmp(t.path, "/") ? t.path : "", name);
  u32 n1 = (u32)strlen(t.path), n2 = (u32)strlen(full);
  fake.joff.push(0); memcpy(fake.journal.push_n(n1 + 1), t.path, n1 + 1);
  fake.joff.push(fake.journal.len); memcpy(fake.journal.push_n(n2 + 1), full, n2 + 1);
  undo_push(a, UNDO_NEW_FOLDER, fake, name);
  refresh_tab(a, t, nullptr, 0, false);
  for (u32 i = 0; i < t.listing.count(); i++)
    if (!strcmp(t.listing.cname(i), name)) { const char* nm = name; select_names(a, t, &nm, 1); rename_begin(a, t, i); break; }
  a.w.need_redraw = true;
}

void rename_commit(App& a, Tab& t) {
  if (t.renaming < 0 || (u32)t.renaming >= t.listing.count()) { rename_cancel(a, t); return; }
  u32 i = (u32)t.renaming;
  char to[1024];
  snprintf(to, sizeof to, "%s", a.rename_input.buf);

  u32 n = (u32)strlen(to);
  while (n && to[n - 1] == ' ') to[--n] = 0;
  const char* from = t.listing.cname(i);
  if (!n || !strcmp(from, to)) { rename_cancel(a, t); return; }
  if (strchr(to, '/')) { set_status_error(a, "A name cannot contain \"/\""); return; }
  if (!fileop_rename(t.dirfd, from, to)) {
    char msg[600];
    if (errno == EEXIST) snprintf(msg, sizeof msg, "\"%s\" already exists here", to);
    else snprintf(msg, sizeof msg, "Cannot rename: %s", strerror(errno));
    set_status_error(a, msg);
    return; // the editor stays open
  }
  char oldp[4096], newp[4096], msg[700];
  entry_path(t, i, oldp, sizeof oldp);
  snprintf(newp, sizeof newp, "%s/%s", strcmp(t.path, "/") ? t.path : "", to);
  FileOpResult fake;
  u32 n1 = (u32)strlen(oldp), n2 = (u32)strlen(newp);
  fake.joff.push(0); memcpy(fake.journal.push_n(n1 + 1), oldp, n1 + 1);
  fake.joff.push(fake.journal.len); memcpy(fake.journal.push_n(n2 + 1), newp, n2 + 1);
  undo_push(a, UNDO_RENAME, fake, from);
  t.renaming = -1;
  snprintf(msg, sizeof msg, "Renamed to \"%s\" \xc2\xb7 Ctrl+Z to undo", to);
  tabs_follow(a, oldp, newp);
  refresh_path(a, t.path, from);
  const char* nm = to;
  select_names(a, t, &nm, 1);
  set_status(a, msg);
  a.w.need_redraw = true;
}
