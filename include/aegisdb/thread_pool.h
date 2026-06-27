/* Worker thread pool with a job queue (T017).
 *
 * Research R-009 specifies a lock-free MPMC queue; this is a mutex+condvar
 * queue — functionally equivalent and simpler, to be swapped for a lock-free
 * ring if profiling shows contention. */
#ifndef AEGISDB_THREAD_POOL_H
#define AEGISDB_THREAD_POOL_H

typedef struct ThreadPool ThreadPool;
typedef void (*job_fn)(void *arg);

ThreadPool *thread_pool_create(int nthreads);

/* Enqueue a job. Returns 0/-1. */
int thread_pool_submit(ThreadPool *tp, job_fn fn, void *arg);

/* Stop accepting jobs, drain in-flight work, join threads, free. */
void thread_pool_destroy(ThreadPool *tp);

#endif /* AEGISDB_THREAD_POOL_H */