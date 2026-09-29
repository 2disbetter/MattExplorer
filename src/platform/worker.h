#pragma once

#include "base/types.h"
#include <pthread.h>
#include <semaphore.h>

struct Job;
typedef void (*JobFn)(Job& j);

struct Job {
  JobFn  run; // runs on a worker
  void*  ctx; // whatever the submitter wants; owned by the submitter
  u32    kind;
  u32    gen; // generation this job belongs to
  u32    id; // unique per submission
  i64    took_ns; // filled in by the worker
};

enum { WORKER_MAX = 8, WORKER_QUEUE = 256 };

struct WorkerRing { // SPSC ring of jobs
  Job          items[WORKER_QUEUE];
  volatile u32 head = 0, tail = 0; // head: consumer, tail: producer; accessed with __atomic
};

struct WorkerPool {
  pthread_t threads[WORKER_MAX];
  u32       count = 0;
  bool      running = false;
  sem_t     jobs_sem;
  WorkerRing submit; // main -> workers (multi-consumer pop via CAS on head)
  WorkerRing results[WORKER_MAX]; // worker i -> main
  int       wake_fd = -1; // eventfd the main loop polls
  volatile u32 cancel_gen = 0; // jobs with gen < cancel_gen should stop
  u32       next_id = 1;
  u32       submitted = 0, completed = 0, dropped = 0;
};

bool worker_start(WorkerPool& p, u32 threads);
void worker_stop(WorkerPool& p); // waits for the workers to exit (jobs in flight finish)

u32  worker_submit(WorkerPool& p, JobFn fn, void* ctx, u32 kind, u32 gen);

void worker_cancel_before(WorkerPool& p, u32 new_gen);

static inline bool worker_cancelled(const WorkerPool& p, u32 gen) { return gen < __atomic_load_n(&p.cancel_gen, __ATOMIC_ACQUIRE); }

bool worker_poll(WorkerPool& p, Job& out);

void worker_ack_wake(WorkerPool& p);

u32  worker_cpu_count();
