/* Worker thread pool with a mutex+condvar job queue (T017). */
#include "aegisdb/thread_pool.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct Job {
    job_fn fn;
    void *arg;
    struct Job *next;
} Job;

struct ThreadPool {
    pthread_t *threads;
    int nthreads;
    Job *head, *tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int shutdown;
};

static void *worker(void *arg) {
    ThreadPool *tp = arg;
    for (;;) {
        pthread_mutex_lock(&tp->lock);
        while (!tp->head && !tp->shutdown)
            pthread_cond_wait(&tp->cond, &tp->lock);
        if (tp->shutdown && !tp->head) {
            pthread_mutex_unlock(&tp->lock);
            return NULL;
        }
        Job *j = tp->head;
        tp->head = j->next;
        if (!tp->head) tp->tail = NULL;
        pthread_mutex_unlock(&tp->lock);

        j->fn(j->arg);
        free(j);
    }
}

ThreadPool *thread_pool_create(int nthreads) {
    if (nthreads < 1) nthreads = 1;
    ThreadPool *tp = calloc(1, sizeof(*tp));
    if (!tp) return NULL;
    tp->nthreads = nthreads;
    tp->threads = calloc(nthreads, sizeof(pthread_t));
    if (!tp->threads) {
        free(tp);
        return NULL;
    }
    pthread_mutex_init(&tp->lock, NULL);
    pthread_cond_init(&tp->cond, NULL);
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tp->threads[i], NULL, worker, tp) != 0) {
            tp->nthreads = i;
            thread_pool_destroy(tp);
            return NULL;
        }
    }
    return tp;
}

int thread_pool_submit(ThreadPool *tp, job_fn fn, void *arg) {
    Job *j = malloc(sizeof(*j));
    if (!j) return -1;
    j->fn = fn;
    j->arg = arg;
    j->next = NULL;
    pthread_mutex_lock(&tp->lock);
    if (tp->shutdown) {
        pthread_mutex_unlock(&tp->lock);
        free(j);
        return -1;
    }
    if (tp->tail)
        tp->tail->next = j;
    else
        tp->head = j;
    tp->tail = j;
    pthread_cond_signal(&tp->cond);
    pthread_mutex_unlock(&tp->lock);
    return 0;
}

void thread_pool_destroy(ThreadPool *tp) {
    if (!tp) return;
    pthread_mutex_lock(&tp->lock);
    tp->shutdown = 1;
    pthread_cond_broadcast(&tp->cond);
    pthread_mutex_unlock(&tp->lock);
    for (int i = 0; i < tp->nthreads; i++) pthread_join(tp->threads[i], NULL);
    Job *j = tp->head;
    while (j) {
        Job *nx = j->next;
        free(j);
        j = nx;
    }
    pthread_mutex_destroy(&tp->lock);
    pthread_cond_destroy(&tp->cond);
    free(tp->threads);
    free(tp);
}