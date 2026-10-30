#ifndef ASCOLTO_EXPERT_CACHE_H
#define ASCOLTO_EXPERT_CACHE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Internal native cache API. A pair is one expert's up/down projections, copied
 * byte-for-byte from the file. Cache capacity excludes model/graph/OS memory. */
/* One complete Nemotron layer union can be reserved atomically. */
#define ASCOLTO_EXPERT_BATCH_MAX 128
#define ASCOLTO_EXPERT_READERS_MAX 16
typedef struct { uint64_t offset[2], bytes[2]; } ascolto_expert_extent_t;
typedef struct ascolto_expert_cache ascolto_expert_cache_t;
typedef struct {
    const unsigned char *data;
    size_t slot;
    uint64_t generation;
} ascolto_expert_lease_t;
typedef struct {
    uint64_t capacity_bytes, bytes_read, hits, misses;
    size_t slots, pinned_slots, highest_slot;
} ascolto_expert_cache_stats_t;
enum { ASCOLTO_EXPERT_OK = 0, ASCOLTO_EXPERT_INVALID = -1,
       ASCOLTO_EXPERT_IO = -2, ASCOLTO_EXPERT_CANCELLED = -3,
       ASCOLTO_EXPERT_BUSY = -4 };
/* The cache opens its own read-only descriptor. No weight pages are mmaped.
 * max_expert_bytes bounds the combined size of the two projections. Reader
 * parallelism is clamped to [1, ASCOLTO_EXPERT_READERS_MAX], including the
 * calling thread; at most readers - 1 native workers are created per batch. */
ascolto_expert_cache_t *ascolto_expert_cache_create(const char *path, uint64_t budget,
    size_t max_expert_bytes, unsigned readers);
/* A successful batch pins every returned lease until exactly one release; leases
 * own their pin and must not be copied to duplicate ownership. The caller must
 * retain pins until GPU work using the bytes has completed. Cancellation can be
 * called concurrently by reader threads and must be thread-safe. Acquisition
 * is serialized; release and stats may run concurrently. Existing batch hits
 * are protected before any miss can evict a slot. Capacity failure leaves the
 * cache and its pins unchanged. On failure no lease
 * is returned, and no partially read expert can become a cache hit. */
int ascolto_expert_cache_acquire(ascolto_expert_cache_t *, const ascolto_expert_extent_t *,
    size_t count, bool (*cancel)(void *), void *userdata, ascolto_expert_lease_t *);
int ascolto_expert_cache_release(ascolto_expert_cache_t *, ascolto_expert_lease_t *);
void ascolto_expert_cache_stats(ascolto_expert_cache_t *, ascolto_expert_cache_stats_t *);
/* Stable storage geometry for a non-owning unified-memory backend wrapper.
 * Only cache readers may write these pages; consumers still acquire leases. */
const void *ascolto_expert_cache_storage(ascolto_expert_cache_t *, size_t *allocation_bytes,
                                       size_t *slot_stride, size_t *slot_count);
/* The owner must quiesce all acquire/release/stats calls before destroy and must
 * prevent new calls. Returns BUSY while a lease is pinned; the cache remains
 * usable in that case. Destruction does not provide a concurrent object handle. */
int ascolto_expert_cache_destroy(ascolto_expert_cache_t *);
#ifdef __cplusplus
}
#endif
#endif
