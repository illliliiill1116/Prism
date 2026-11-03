/* Stress test: readers hammer their own shards while a counter is bumped
 * under the all-shard writer lock. Meant for TSan; asserts the counter is
 * never seen mid-update and ends at the number of writes. */
#include "percpu_rwlock.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NSHARDS 8
#define READS_PER_THREAD 200000
#define WRITES_PER_THREAD 50

typedef struct {
    percpu_rwlock_t lock;
    volatile long counter;
    volatile int in_write; /* set only while holding the write lock */
} shared_t;

typedef struct {
    shared_t *sh;
    uint32_t shard_id;
} worker_arg_t;

static void *worker(void *arg_)
{
    worker_arg_t *arg = (worker_arg_t *)arg_;
    shared_t *sh = arg->sh;

    for (int i = 0; i < READS_PER_THREAD; i++) {
        percpu_rwlock_rdlock(&sh->lock, arg->shard_id);
        assert(sh->in_write == 0);
        long c = sh->counter;
        (void)c;
        percpu_rwlock_rdunlock(&sh->lock, arg->shard_id);

        if (i % (READS_PER_THREAD / WRITES_PER_THREAD) == 0) {
            percpu_rwlock_wrlock(&sh->lock);
            sh->in_write = 1;
            sh->counter++;
            sh->in_write = 0;
            percpu_rwlock_wrunlock(&sh->lock);
        }
    }
    return NULL;
}

int main(void)
{
    shared_t sh;
    assert(percpu_rwlock_init(&sh.lock, NSHARDS));
    sh.counter = 0;
    sh.in_write = 0;

    pthread_t threads[NSHARDS];
    worker_arg_t args[NSHARDS];
    for (uint32_t i = 0; i < NSHARDS; i++) {
        args[i].sh = &sh;
        args[i].shard_id = i;
        assert(pthread_create(&threads[i], NULL, worker, &args[i]) == 0);
    }
    for (uint32_t i = 0; i < NSHARDS; i++) pthread_join(threads[i], NULL);

    long expect = (long)NSHARDS * WRITES_PER_THREAD;
    if (sh.counter != expect) {
        fprintf(stderr, "counter mismatch: got %ld, expected %ld\n", sh.counter, expect);
        return 1;
    }

    percpu_rwlock_destroy(&sh.lock);
    printf("test_percpu_rwlock: OK (%ld writes observed)\n", sh.counter);
    return 0;
}
