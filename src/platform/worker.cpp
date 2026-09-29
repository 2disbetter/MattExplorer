#include "platform/worker.h"

#include <errno.h>
#include <sched.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

static i64 now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (i64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static bool ring_push(WorkerRing& r, const Job& j) { // single producer
  u32 t = __atomic_load_n(&r.tail, __ATOMIC_RELAXED);
  u32 h = __atomic_load_n(&r.head, __ATOMIC_ACQUIRE);
  if (t - h >= WORKER_QUEUE) return false;
  r.items[t % WORKER_QUEUE] = j;
  __atomic_store_n(&r.tail, t + 1, __ATOMIC_RELEASE);
  return true;
}

static bool ring_pop_single(WorkerRing& r, Job& out) { // single consumer
  u32 h = __atomic_load_n(&r.head, __ATOMIC_RELAXED);
  u32 t = __atomic_load_n(&r.tail, __ATOMIC_ACQUIRE);
  if (h == t) return false;
  out = r.items[h % WORKER_QUEUE];
  __atomic_store_n(&r.head, h + 1, __ATOMIC_RELEASE);
  return true;
}

static bool ring_pop_multi(WorkerRing& r, Job& out) {
  for (;;) {
    u32 h = __atomic_load_n(&r.head, __ATOMIC_ACQUIRE);
    u32 t = __atomic_load_n(&r.tail, __ATOMIC_ACQUIRE);
    if (h == t) return false;
    Job j = r.items[h % WORKER_QUEUE];
    if (__atomic_compare_exchange_n(&r.head, &h, h + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { out = j; return true; }
  }
}

struct WorkerArg { WorkerPool* pool; u32 index; };
static WorkerArg g_args[WORKER_MAX];

static void* worker_main(void* varg) {
  WorkerArg& a = *(WorkerArg*)varg;
  WorkerPool& p = *a.pool;
  for (;;) {
    while (sem_wait(&p.jobs_sem) != 0 && errno == EINTR) {}
    if (!__atomic_load_n(&p.running, __ATOMIC_ACQUIRE)) return nullptr;
    Job j;
    if (!ring_pop_multi(p.submit, j)) continue; // another worker took it
    i64 t0 = now_ns();
    if (!worker_cancelled(p, j.gen)) j.run(j);
    j.took_ns = now_ns() - t0;

    while (!ring_push(p.results[a.index], j)) sched_yield();
    u64 one = 1;
    while (write(p.wake_fd, &one, sizeof one) < 0 && errno == EINTR) {}
  }
}

u32 worker_cpu_count() {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (u32)n : 1;
}

bool worker_start(WorkerPool& p, u32 threads) {
  if (p.running) return true;
  if (!threads) threads = worker_cpu_count();
  threads = mx_clamp(threads, 1u, (u32)WORKER_MAX);
  p.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (p.wake_fd < 0) return false;
  if (sem_init(&p.jobs_sem, 0, 0) != 0) { close(p.wake_fd); p.wake_fd = -1; return false; }
  __atomic_store_n(&p.running, true, __ATOMIC_RELEASE);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024); // jobs are small; 8 MB default stacks are waste
  for (u32 i = 0; i < threads; i++) {
    g_args[i] = { &p, i };
    if (pthread_create(&p.threads[p.count], &attr, worker_main, &g_args[i]) != 0) break;
    p.count++;
  }
  pthread_attr_destroy(&attr);
  if (!p.count) { __atomic_store_n(&p.running, false, __ATOMIC_RELEASE); sem_destroy(&p.jobs_sem); close(p.wake_fd); p.wake_fd = -1; return false; }
  return true;
}

void worker_stop(WorkerPool& p) {
  if (!p.running) return;
  __atomic_store_n(&p.running, false, __ATOMIC_RELEASE);
  for (u32 i = 0; i < p.count; i++) sem_post(&p.jobs_sem);
  for (u32 i = 0; i < p.count; i++) pthread_join(p.threads[i], nullptr);
  p.count = 0;
  sem_destroy(&p.jobs_sem);
  close(p.wake_fd); p.wake_fd = -1;
}

u32 worker_submit(WorkerPool& p, JobFn fn, void* ctx, u32 kind, u32 gen) {
  Job j = { fn, ctx, kind, gen, p.next_id++, 0 };
  if (!p.next_id) p.next_id = 1;
  if (!p.running) { // no workers: run inline, deliver through ring 0
    i64 t0 = now_ns();
    fn(j);
    j.took_ns = now_ns() - t0;
    ring_push(p.results[0], j);
    return j.id;
  }
  if (!ring_push(p.submit, j)) return 0;
  p.submitted++;
  sem_post(&p.jobs_sem);
  return j.id;
}

void worker_cancel_before(WorkerPool& p, u32 new_gen) {
  u32 cur = __atomic_load_n(&p.cancel_gen, __ATOMIC_RELAXED);
  if (new_gen > cur) __atomic_store_n(&p.cancel_gen, new_gen, __ATOMIC_RELEASE);
}

void worker_ack_wake(WorkerPool& p) {
  if (p.wake_fd < 0) return;
  u64 v;
  while (read(p.wake_fd, &v, sizeof v) < 0 && errno == EINTR) {}
}

bool worker_poll(WorkerPool& p, Job& out) {
  u32 rings = p.running ? p.count : 1;
  for (u32 i = 0; i < rings; i++) {
    while (ring_pop_single(p.results[i], out)) {
      p.completed++;
      if (worker_cancelled(p, out.gen)) { p.dropped++; continue; }
      return true;
    }
  }
  return false;
}
