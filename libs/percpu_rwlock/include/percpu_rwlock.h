/*
 * percpu_rwlock.h - a sharded reader-writer lock for read-mostly caches.
 * A shard is whatever small id a thread consistently uses (its worker
 * index); readers on different shards never touch the same cache line.
 *
 * Readers lock only their own shard. A writer takes every shard in order
 * 0..n-1 - one big lock in effect - which is fine because writes (a state
 * or texture seen for the first time) are rare.
 *
 * Each shard is a spinning lock on C11 atomics (a writer flag and a reader
 * count); no pthread or OS locks.
 */
#ifndef PRISM_RENDER_PERCPU_RWLOCK_H
#define PRISM_RENDER_PERCPU_RWLOCK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    _Alignas(64) atomic_bool writer;
    atomic_int readers;
    /* Pad each shard to a full cache line to avoid false sharing; the array
     * base is 64-aligned too (percpu_rwlock_init). */
    char pad[64 - ((sizeof(atomic_bool) + sizeof(atomic_int)) % 64)];
} percpu_rwlock_shard_t;

typedef struct {
    percpu_rwlock_shard_t *shards;
    uint32_t nshards;
} percpu_rwlock_t;

/* Allocates `nshards` shards. Returns 0 on OOM, 1 on success. `nshards`
 * must be >= 1. */
int percpu_rwlock_init(percpu_rwlock_t *l, uint32_t nshards);

/* Frees the shard array (shards carry no OS resources - plain atomics). */
void percpu_rwlock_destroy(percpu_rwlock_t *l);

/* Reader path: touches shard `shard_id` only, which must be < nshards and
 * stable for the thread. */
void percpu_rwlock_rdlock(percpu_rwlock_t *l, uint32_t shard_id);
void percpu_rwlock_rdunlock(percpu_rwlock_t *l, uint32_t shard_id);

/* Writer path: every shard's write lock in ascending order (no deadlock
 * between writers), excluding all readers and writers. */
void percpu_rwlock_wrlock(percpu_rwlock_t *l);
void percpu_rwlock_wrunlock(percpu_rwlock_t *l);

/* Per-thread shard id, read by the caches' accessors so callers needn't
 * pass one. Defaults to 0. A wrong or shared id only costs contention -
 * writers take every shard - but it must be < nshards. Each worker sets it
 * once at start. */
void prism_set_shard_id(uint32_t id);
uint32_t prism_get_shard_id(void);

#endif /* PRISM_RENDER_PERCPU_RWLOCK_H */
