#include "percpu_rwlock.h"

#include <sched.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define PERCPU_RWLOCK_PAUSE() _mm_pause()
#else
#define PERCPU_RWLOCK_PAUSE() ((void)0)
#endif

/* Pause first, then yield: short waits resolve without a syscall. (A copy
 * of render's spin_backoff.h idea, to keep this library standalone.) */
static inline void percpu_rwlock_spin_backoff(int *spins) {
    if (*spins < 2000) {
        PERCPU_RWLOCK_PAUSE();
        (*spins)++;
    } else {
        sched_yield();
    }
}

static __thread uint32_t t_shard_id = 0;

void prism_set_shard_id(uint32_t id) { t_shard_id = id; }
uint32_t prism_get_shard_id(void) { return t_shard_id; }

int percpu_rwlock_init(percpu_rwlock_t *l, uint32_t nshards)
{
    if (nshards == 0) nshards = 1;
    /* aligned_alloc needs a size that is a multiple of 64, which the
     * padded shard is. */
    size_t bytes = (size_t)nshards * sizeof(percpu_rwlock_shard_t);
    l->shards = (percpu_rwlock_shard_t *)aligned_alloc(64, bytes);
    if (!l->shards) return 0;
    memset(l->shards, 0, bytes);
    for (uint32_t i = 0; i < nshards; i++) {
        atomic_init(&l->shards[i].writer, false);
        atomic_init(&l->shards[i].readers, 0);
    }
    l->nshards = nshards;
    return 1;
}

void percpu_rwlock_destroy(percpu_rwlock_t *l)
{
    if (!l || !l->shards) return;
    free(l->shards);
    l->shards = NULL;
    l->nshards = 0;
}

void percpu_rwlock_rdlock(percpu_rwlock_t *l, uint32_t shard_id)
{
    percpu_rwlock_shard_t *s = &l->shards[shard_id];

    atomic_fetch_add_explicit(&s->readers, 1, memory_order_relaxed);
    while (atomic_load_explicit(&s->writer, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&s->readers, 1, memory_order_relaxed);
        int spins = 0;
        while (atomic_load_explicit(&s->writer, memory_order_relaxed))
            percpu_rwlock_spin_backoff(&spins);
        atomic_fetch_add_explicit(&s->readers, 1, memory_order_relaxed);
    }
}

void percpu_rwlock_rdunlock(percpu_rwlock_t *l, uint32_t shard_id)
{
    atomic_fetch_sub_explicit(&l->shards[shard_id].readers, 1, memory_order_release);
}

/* CAS the writer flag, then wait for readers already inside to leave (a
 * reader backs out its count when it sees the flag). */
static void shard_wrlock(percpu_rwlock_shard_t *s)
{
    int spins = 0;
    for (;;) {
        bool expected = false;
        if (atomic_compare_exchange_weak_explicit(&s->writer, &expected, true,
                                                    memory_order_acq_rel, memory_order_relaxed))
            break;
        percpu_rwlock_spin_backoff(&spins);
    }
    spins = 0;
    while (atomic_load_explicit(&s->readers, memory_order_acquire) != 0)
        percpu_rwlock_spin_backoff(&spins);
}

static void shard_wrunlock(percpu_rwlock_shard_t *s)
{
    atomic_store_explicit(&s->writer, false, memory_order_release);
}

void percpu_rwlock_wrlock(percpu_rwlock_t *l)
{
    for (uint32_t i = 0; i < l->nshards; i++) shard_wrlock(&l->shards[i]);
}

void percpu_rwlock_wrunlock(percpu_rwlock_t *l)
{
    for (uint32_t i = 0; i < l->nshards; i++) shard_wrunlock(&l->shards[i]);
}
