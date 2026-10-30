#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "expert_cache.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    ascolto_expert_extent_t extent;
    uint64_t generation, used;
    size_t pins;
    bool valid;
} entry_t;
struct ascolto_expert_cache {
    int fd;
    uint64_t file_bytes, clock;
    size_t slots, stride, max_expert_bytes, allocation_bytes;
    unsigned readers;
    unsigned char *storage;
    entry_t *entries;
    pthread_mutex_t operation, mutex;
    ascolto_expert_cache_stats_t stats;
};

ascolto_expert_cache_t *ascolto_expert_cache_create(const char *path, uint64_t budget,
    size_t expert_bytes, unsigned readers) {
    if (!path || !expert_bytes || budget < expert_bytes || budget > SIZE_MAX) return NULL;
    /* Page-align slots for direct/no-cache reads and future shared Metal buffers. */
    if (expert_bytes > SIZE_MAX - 4095) return NULL;
    size_t stride = (expert_bytes + 4095) & ~(size_t)4095;
    size_t slots = (size_t)(budget / stride);
    if (!slots || slots > SIZE_MAX / sizeof(entry_t)) return NULL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    struct stat st;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) { close(fd); return NULL; }
#ifdef __APPLE__
    if (fcntl(fd, F_NOCACHE, 1)) { close(fd); return NULL; }
#else
    posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
#endif
    ascolto_expert_cache_t *c = calloc(1, sizeof(*c));
    if (!c) { close(fd); return NULL; }
    long page = sysconf(_SC_PAGESIZE);
    size_t alignment = page > 4096 ? (size_t)page : 4096;
    if ((alignment & (alignment - 1)) || slots * stride > SIZE_MAX - (alignment - 1)) { free(c); close(fd); return NULL; }
    c->allocation_bytes = (slots * stride + alignment - 1) & ~(alignment - 1);
    c->entries = calloc(slots, sizeof(*c->entries));
    if (!c->entries || posix_memalign((void **)&c->storage, alignment, c->allocation_bytes)) {
        free(c->entries); free(c); close(fd); return NULL;
    }
    if (pthread_mutex_init(&c->operation, NULL)) {
        free(c->storage); free(c->entries); free(c); close(fd); return NULL;
    }
    if (pthread_mutex_init(&c->mutex, NULL)) {
        pthread_mutex_destroy(&c->operation);
        free(c->storage); free(c->entries); free(c); close(fd); return NULL;
    }
    c->fd = fd; c->file_bytes = (uint64_t)st.st_size;
    c->slots = slots; c->stride = stride; c->max_expert_bytes = expert_bytes;
    c->readers = readers < 1 ? 1 : readers > ASCOLTO_EXPERT_READERS_MAX ? ASCOLTO_EXPERT_READERS_MAX : readers;
    c->stats.capacity_bytes = slots * stride; c->stats.slots = slots;
    return c;
}
const void *ascolto_expert_cache_storage(ascolto_expert_cache_t *c, size_t *bytes, size_t *stride, size_t *slots) {
    if (!c) return NULL;
    *bytes = c->allocation_bytes; *stride = c->stride; *slots = c->slots;
    return c->storage;
}

static bool same(const ascolto_expert_extent_t *a, const ascolto_expert_extent_t *b) {
    return a->offset[0] == b->offset[0] && a->offset[1] == b->offset[1] &&
           a->bytes[0] == b->bytes[0] && a->bytes[1] == b->bytes[1];
}
typedef struct { size_t slot; uint64_t offset, read; int error; } read_t;
typedef struct {
    ascolto_expert_cache_t *cache;
    read_t *jobs;
    unsigned count;
    atomic_uint next;
    bool (*cancel)(void *);
    void *userdata;
} work_t;
static void *read_experts(void *arg) {
    work_t *work = arg;
    unsigned index;
    while ((index = atomic_fetch_add(&work->next, 1)) < work->count) {
        read_t *job = &work->jobs[index];
        const ascolto_expert_extent_t *e = &work->cache->entries[job->slot].extent;
        unsigned char *destination = work->cache->storage + job->slot * work->cache->stride;
        for (int part = 0; part < 2 && !job->error; ++part) {
            uint64_t read = 0;
            while (read < e->bytes[part]) {
                if (work->cancel && work->cancel(work->userdata)) {
                    job->error = ASCOLTO_EXPERT_CANCELLED; break;
                }
                size_t remaining = (size_t)(e->bytes[part] - read);
                size_t want = remaining < 1024 * 1024 ? remaining : 1024 * 1024;
                ssize_t got = pread(work->cache->fd, destination + read, want, (off_t)(e->offset[part] + read));
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) { job->error = ASCOLTO_EXPERT_IO; break; }
                read += (uint64_t)got; job->read += (uint64_t)got;
            }
            destination += e->bytes[part];
#ifndef __APPLE__
            posix_fadvise(work->cache->fd, (off_t)e->offset[part], (off_t)e->bytes[part], POSIX_FADV_DONTNEED);
#endif
        }
    }
    return NULL;
}
static int file_order(const void *left, const void *right) {
    const read_t *a = left, *b = right;
    return (a->offset > b->offset) - (a->offset < b->offset);
}
int ascolto_expert_cache_acquire(ascolto_expert_cache_t *c, const ascolto_expert_extent_t *extents,
    size_t count, bool (*cancel)(void *), void *userdata, ascolto_expert_lease_t *leases) {
    if (!c || !extents || !leases || !count || count > ASCOLTO_EXPERT_BATCH_MAX) return ASCOLTO_EXPERT_INVALID;
    memset(leases, 0, count * sizeof(*leases));
    for (size_t i = 0; i < count; ++i) {
        const ascolto_expert_extent_t *e = extents + i;
        if (!e->bytes[0] || !e->bytes[1] || e->bytes[0] > c->max_expert_bytes || e->bytes[1] > c->max_expert_bytes - e->bytes[0]) return ASCOLTO_EXPERT_INVALID;
        for (int part = 0; part < 2; ++part)
            if (e->offset[part] > INT64_MAX || e->offset[part] > c->file_bytes ||
                e->bytes[part] > c->file_bytes - e->offset[part]) return ASCOLTO_EXPERT_INVALID;
    }
    pthread_mutex_lock(&c->operation);
    pthread_mutex_lock(&c->mutex);
    read_t jobs[ASCOLTO_EXPERT_BATCH_MAX] = {{0}};
    unsigned n_jobs = 0;
    int error = 0;
    size_t existing[ASCOLTO_EXPERT_BATCH_MAX];
    size_t missing = 0, available = 0;
    /* Discover the complete batch before evicting anything. A miss at the
     * front must not evict a resident hit requested later in the same batch. */
    for (size_t i = 0; i < count; ++i) {
        size_t slot = c->slots;
        for (size_t j = 0; j < c->slots; ++j)
            if (c->entries[j].valid && same(&c->entries[j].extent, extents + i)) { slot = j; break; }
        existing[i] = slot;
        if (slot != c->slots) {
            size_t requested = 1;
            for (size_t j = 0; j < i; ++j) if (existing[j] == slot) ++requested;
            if (c->entries[slot].pins > SIZE_MAX - requested) error = ASCOLTO_EXPERT_BUSY;
        } else {
            bool duplicate = false;
            for (size_t j = 0; j < i; ++j) if (same(extents + j, extents + i)) { duplicate = true; break; }
            if (!duplicate) ++missing;
        }
    }
    for (size_t slot = 0; slot < c->slots; ++slot) {
        bool needed = false;
        for (size_t i = 0; i < count; ++i) if (existing[i] == slot) { needed = true; break; }
        if (!needed && !c->entries[slot].pins && c->entries[slot].generation != UINT64_MAX) ++available;
    }
    if (missing > available || c->clock > UINT64_MAX - count) error = ASCOLTO_EXPERT_BUSY;
    if (!error) {
        /* Pin all existing hits first, including duplicate lease ownership.
         * The preflight above makes capacity/pin failure non-mutating. */
        for (size_t i = 0; i < count; ++i) if (existing[i] != c->slots) {
            size_t slot = existing[i];
            entry_t *entry = &c->entries[slot];
            ++entry->pins;
            leases[i] = (ascolto_expert_lease_t){c->storage + slot * c->stride, slot, entry->generation};
            ++c->stats.hits;
        }
        for (size_t i = 0; i < count; ++i) if (existing[i] == c->slots) {
            size_t slot = c->slots;
            /* A duplicate miss shares its first reserved slot and read job. */
            for (size_t j = 0; j < i; ++j) if (same(extents + j, extents + i)) { slot = leases[j].slot; break; }
            if (slot == c->slots) {
                for (size_t j = 0; j < c->slots; ++j)
                    if (!c->entries[j].pins && c->entries[j].generation != UINT64_MAX &&
                        (slot == c->slots || c->entries[j].used < c->entries[slot].used)) slot = j;
                entry_t *entry = &c->entries[slot];
                entry->extent = extents[i]; entry->valid = false; ++entry->generation;
                jobs[n_jobs++] = (read_t){.slot = slot, .offset = extents[i].offset[0]};
                ++c->stats.misses;
            }
            entry_t *entry = &c->entries[slot];
            ++entry->pins;
            leases[i] = (ascolto_expert_lease_t){c->storage + slot * c->stride, slot, entry->generation};
        }
        for (size_t i = 0; i < count; ++i) {
            size_t slot = leases[i].slot;
            c->entries[slot].used = ++c->clock;
            if (slot > c->stats.highest_slot) c->stats.highest_slot = slot;
        }
    }
    pthread_mutex_unlock(&c->mutex);
    if (!error) {
        qsort(jobs, n_jobs, sizeof(*jobs), file_order);
        work_t work = {.cache = c, .jobs = jobs, .count = n_jobs, .cancel = cancel, .userdata = userdata};
        atomic_init(&work.next, 0);
        pthread_t threads[ASCOLTO_EXPERT_READERS_MAX - 1]; unsigned created = 0;
        for (unsigned i = 1; i < c->readers && i < n_jobs; ++i)
            if (!pthread_create(&threads[created], NULL, read_experts, &work)) ++created;
        read_experts(&work);
        for (unsigned i = 0; i < created; ++i) pthread_join(threads[i], NULL);
        if (cancel && cancel(userdata)) error = ASCOLTO_EXPERT_CANCELLED;
        pthread_mutex_lock(&c->mutex);
        for (unsigned i = 0; i < n_jobs; ++i) {
            c->stats.bytes_read += jobs[i].read;
            c->entries[jobs[i].slot].valid = !jobs[i].error;
            if (jobs[i].error && !error) error = jobs[i].error;
        }
        pthread_mutex_unlock(&c->mutex);
    }
    if (error) {
        pthread_mutex_lock(&c->mutex);
        for (size_t i = 0; i < count; ++i) if (leases[i].data) --c->entries[leases[i].slot].pins;
        pthread_mutex_unlock(&c->mutex);
        memset(leases, 0, count * sizeof(*leases));
    }
    pthread_mutex_unlock(&c->operation);
    return error;
}
int ascolto_expert_cache_release(ascolto_expert_cache_t *c, ascolto_expert_lease_t *lease) {
    if (!c || !lease || !lease->data || lease->slot >= c->slots) return ASCOLTO_EXPERT_INVALID;
    pthread_mutex_lock(&c->mutex);
    entry_t *entry = &c->entries[lease->slot];
    bool valid = lease->generation == entry->generation && entry->pins &&
                 lease->data == c->storage + lease->slot * c->stride;
    if (valid) { --entry->pins; memset(lease, 0, sizeof(*lease)); }
    pthread_mutex_unlock(&c->mutex);
    return valid ? 0 : ASCOLTO_EXPERT_INVALID;
}
void ascolto_expert_cache_stats(ascolto_expert_cache_t *c, ascolto_expert_cache_stats_t *out) {
    if (!c || !out) return;
    pthread_mutex_lock(&c->mutex); *out = c->stats; out->pinned_slots = 0;
    for (size_t i = 0; i < c->slots; ++i) if (c->entries[i].pins) ++out->pinned_slots;
    pthread_mutex_unlock(&c->mutex);
}
int ascolto_expert_cache_destroy(ascolto_expert_cache_t *c) {
    if (!c) return 0;
    pthread_mutex_lock(&c->operation); pthread_mutex_lock(&c->mutex);
    for (size_t i = 0; i < c->slots; ++i) if (c->entries[i].pins) {
        pthread_mutex_unlock(&c->mutex); pthread_mutex_unlock(&c->operation); return ASCOLTO_EXPERT_BUSY;
    }
    pthread_mutex_unlock(&c->mutex); pthread_mutex_unlock(&c->operation);
    close(c->fd); free(c->storage); free(c->entries);
    pthread_mutex_destroy(&c->mutex); pthread_mutex_destroy(&c->operation); free(c);
    return 0;
}
