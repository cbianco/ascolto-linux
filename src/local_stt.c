#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ascolto/local_stt.h"
#include "ascolto/storage.h"
#include "ascolto/filesystem.h"
#include <curl/curl.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#define digest_t CC_SHA256_CTX
#define digest_init CC_SHA256_Init
#define digest_update CC_SHA256_Update
#define digest_final CC_SHA256_Final
#define LIB_SUFFIX ".dylib"
#else
#include <openssl/evp.h>
typedef EVP_MD_CTX *digest_t;
static int digest_init(digest_t *ctx) { *ctx = EVP_MD_CTX_new(); if (!*ctx) return 0; if (EVP_DigestInit_ex(*ctx, EVP_sha256(), NULL)) return 1; EVP_MD_CTX_free(*ctx); *ctx = NULL; return 0; }
static int digest_update(digest_t *ctx, const void *data, unsigned size) { return EVP_DigestUpdate(*ctx, data, size); }
static int digest_final(unsigned char *hash, digest_t *ctx) { int rc = EVP_DigestFinal_ex(*ctx, hash, NULL); EVP_MD_CTX_free(*ctx); return rc; }
#define LIB_SUFFIX ".so"
#endif

typedef struct { const char *name, *url, *sha256; uint64_t bytes; } artifact_t;
typedef struct {
    const artifact_t *files; size_t count;
    const artifact_t *installed; size_t installed_count;
    uint64_t preparation_bytes; /* Additional peak storage beyond the download. */
} package_t;
#ifdef ASCOLTO_LOCAL_STT_TEST_MANIFEST
#include "../tests/local_stt_manifest.inc"
#else
#include "local_stt_manifest.inc"
#endif
typedef struct {
    bool valid;
    struct stat directory;
    struct stat *files;
    uint64_t epoch;
} verification_t;
typedef struct {
    int root, directory;
    int *fds;
    struct stat root_identity, directory_identity, *files;
    size_t count;
} verified_package_t;
struct ascolto_local_models {
    pthread_mutex_t mutex, operations;
    pthread_t worker;
    bool joinable, running, cancel, closing;
    int downloading;
    char config[512], directory[768], runtime[1024];
    ascolto_local_models_snapshot_t snapshot;
    verification_t verified[ASCOLTO_LOCAL_MODEL_COUNT];
#ifdef ASCOLTO_LOCAL_STT_TEST_MANIFEST
    uint64_t hashes[ASCOLTO_LOCAL_MODEL_COUNT];
    int first_hash;
#endif
};
static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static void init_curl(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }
static const char *ids[] = {"whistle", "whisper-turbo-q5", "parakeet-v3-q8", "qwen3-asr-1.7b", "nemotron-3.5-q8", "nemotron-3.5-bf16-ssd"};
static const char *names[] = {"Whistle", "Whisper large-v3-turbo Q5", "Parakeet TDT v3", "Qwen3-ASR 1.7B", "Nemotron 3.5 Transcribe Q8_0", "Nemotron 3.5 Transcribe BF16 SSD"};
static const char *descriptions[] = {
    "Current baseline. Lower agreement in our sample; very lightweight.",
    "Closest to the cloud reference in our Italian sample.",
    "Good agreement in our sample, with very low processing delay.",
    "Good agreement in our sample. Uses more memory than the other options.",
    "8-bit Q8_0 decoder with F16 audio encoder and a bounded SSD expert cache. Native CPU/Metal.",
    "Original BF16 weights with a bounded SSD expert cache. Experimental: high processing delay. Installation temporarily needs about 134 GB."};
static const char *hints[] = {"Default · included · 17 MB", "Best test agreement", "Speed / quality balance", "CPU option · about 7 GB memory", "Q8_0 · SSD cache 4 GiB · CPU / Metal", "BF16 · SSD cache 4 GiB · high delay"};
_Static_assert(sizeof(packages) / sizeof(packages[0]) == ASCOLTO_LOCAL_MODEL_COUNT, "Model manifest/catalog mismatch");
static int model_index(const char *id) { for (int i = 0; id && i < ASCOLTO_LOCAL_MODEL_COUNT; ++i) if (!strcmp(id, ids[i])) return i; return -1; }
static const char checking_message[] = "Selected model is still being checked. Wait for verification to finish.";
static void state(ascolto_local_models_t *m, int i, ascolto_model_state_t value, const char *message) {
    pthread_mutex_lock(&m->mutex);
    m->snapshot.models[i].state = value;
    snprintf(m->snapshot.models[i].message, sizeof(m->snapshot.models[i].message), "%s", message ? message : "");
    if (value == ASCOLTO_MODEL_READY && m->snapshot.models[i].selected && !strcmp(m->snapshot.error, checking_message))
        m->snapshot.error[0] = 0;
    ++m->snapshot.revision;
    pthread_mutex_unlock(&m->mutex);
}
static void report(ascolto_local_models_t *m, const char *message) {
    pthread_mutex_lock(&m->mutex);
    snprintf(m->snapshot.error, sizeof(m->snapshot.error), "%s", message);
    ++m->snapshot.revision;
    pthread_mutex_unlock(&m->mutex);
}
static bool cancelled(ascolto_local_models_t *m) {
    pthread_mutex_lock(&m->mutex); bool stop = m->cancel || m->closing; pthread_mutex_unlock(&m->mutex); return stop;
}
static void model_directory(ascolto_local_models_t *m, int i, bool staging, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s%s", m->directory, ids[i], staging ? ".download" : "");
}
static void runtime_path(ascolto_local_models_t *m, int i, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s" LIB_SUFFIX, m->runtime, ids[i]);
}
static bool same_identity(const struct stat *a, const struct stat *b) {
#ifdef __APPLE__
    const struct timespec am = a->st_mtimespec, bm = b->st_mtimespec, ac = a->st_ctimespec, bc = b->st_ctimespec;
#else
    const struct timespec am = a->st_mtim, bm = b->st_mtim, ac = a->st_ctim, bc = b->st_ctim;
#endif
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode && a->st_size == b->st_size &&
        am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec && ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}
static void invalidate_verification(ascolto_local_models_t *m, int i) {
    pthread_mutex_lock(&m->mutex);
    m->verified[i].valid = false; ++m->verified[i].epoch;
    pthread_mutex_unlock(&m->mutex);
}
static int verify_descriptor(ascolto_local_models_t *m, int i, int fd, const artifact_t *a) {
    struct stat st, after;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != a->bytes) { return -1; }
    digest_t digest; if (!digest_init(&digest)) { return -1; }
#ifdef ASCOLTO_LOCAL_STT_TEST_MANIFEST
    pthread_mutex_lock(&m->mutex); ++m->hashes[i]; if (m->first_hash < 0) m->first_hash = i; pthread_mutex_unlock(&m->mutex);
#else
    (void)i;
#endif
#ifdef ASCOLTO_LOCAL_STT_TEST_VERIFICATION_GATE
    extern void ascolto_stt_test_verify_gate(const char *name);
    ascolto_stt_test_verify_gate(a->name);
#endif
    const size_t block = 1024 * 1024;
    unsigned char *buffer = malloc(block), hash[32]; ssize_t n;
    if (!buffer) {
#ifndef __APPLE__
        EVP_MD_CTX_free(digest);
#endif
        return -1;
    }
    while ((n = read(fd, buffer, block)) > 0) {
        if (pthread_equal(pthread_self(), m->worker) && cancelled(m)) {
#ifndef __APPLE__
            EVP_MD_CTX_free(digest);
#endif
            free(buffer); return -2;
        }
        digest_update(&digest, buffer, (unsigned)n);
    }
    int finalized = digest_final(hash, &digest);
    free(buffer);
    if (n < 0 || !finalized || fstat(fd, &after) || !same_identity(&st, &after)) return -1;
    char hex[65]; for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    return strcmp(hex, a->sha256) ? -1 : 0;
}
static int verify_installed_at(ascolto_local_models_t *m, int i, int directory_fd) {
    const artifact_t *files = packages[i].installed ? packages[i].installed : packages[i].files;
    size_t count = packages[i].installed ? packages[i].installed_count : packages[i].count;
    for (size_t f = 0; f < count; ++f) {
        int fd = openat(directory_fd, files[f].name, O_RDONLY | O_NOFOLLOW);
        int rc = fd < 0 ? -1 : verify_descriptor(m, i, fd, &files[f]);
        if (fd >= 0) close(fd);
        if (rc) return rc;
    }
    return 0;
}
static void close_verified(verified_package_t *p) {
    if (p->fds) for (size_t f = 0; f < p->count; ++f) if (p->fds[f] >= 0) close(p->fds[f]);
    if (p->directory >= 0) close(p->directory);
    if (p->root >= 0) close(p->root);
    free(p->fds); free(p->files); memset(p, 0, sizeof(*p)); p->directory = p->root = -1;
}
static bool unchanged_package(ascolto_local_models_t *m, int i, const verified_package_t *p) {
    struct stat current;
    if (lstat(m->directory, &current) || !S_ISDIR(current.st_mode) ||
        current.st_dev != p->root_identity.st_dev || current.st_ino != p->root_identity.st_ino) return false;
    if (fstat(p->directory, &current) || !same_identity(&p->directory_identity, &current) ||
        fstatat(p->root, ids[i], &current, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(current.st_mode) ||
        !same_identity(&p->directory_identity, &current)) return false;
    const artifact_t *files = packages[i].installed ? packages[i].installed : packages[i].files;
    for (size_t f = 0; f < p->count; ++f)
        if (fstat(p->fds[f], &current) || !S_ISREG(current.st_mode) || !same_identity(p->files + f, &current) ||
            fstatat(p->directory, files[f].name, &current, AT_SYMLINK_NOFOLLOW) || !S_ISREG(current.st_mode) ||
            !same_identity(p->files + f, &current)) return false;
    return true;
}
static int verify_installed(ascolto_local_models_t *m, int i, verified_package_t *retained) {
    const artifact_t *files = packages[i].installed ? packages[i].installed : packages[i].files;
    verified_package_t p = {.root = -1, .directory = -1, .count = packages[i].installed ? packages[i].installed_count : packages[i].count};
    int rc = -1;
    p.fds = malloc(p.count * sizeof(*p.fds)); p.files = calloc(p.count, sizeof(*p.files));
    if (p.fds) for (size_t f = 0; f < p.count; ++f) p.fds[f] = -1;
    if (!p.fds || !p.files) goto finished;
    p.root = open(m->directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (p.root < 0 || fstat(p.root, &p.root_identity) || !S_ISDIR(p.root_identity.st_mode)) goto finished;
    p.directory = openat(p.root, ids[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (p.directory < 0 || fstat(p.directory, &p.directory_identity) || !S_ISDIR(p.directory_identity.st_mode)) goto finished;
    for (size_t f = 0; f < p.count; ++f) {
        p.fds[f] = openat(p.directory, files[f].name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        if (p.fds[f] < 0 || fstat(p.fds[f], p.files + f) || !S_ISREG(p.files[f].st_mode) ||
            p.files[f].st_size < 0 || (uint64_t)p.files[f].st_size != files[f].bytes) goto finished;
    }
    pthread_mutex_lock(&m->mutex);
    verification_t *cached = &m->verified[i];
    uint64_t epoch = cached->epoch;
    bool hit = cached->valid && same_identity(&cached->directory, &p.directory_identity);
    for (size_t f = 0; hit && f < p.count; ++f) hit = same_identity(cached->files + f, p.files + f);
    if (!hit) cached->valid = false;
    pthread_mutex_unlock(&m->mutex);
    if (!hit) for (size_t f = 0; f < p.count; ++f) {
        rc = verify_descriptor(m, i, p.fds[f], files + f);
        if (rc) goto finished;
    }
    if (!unchanged_package(m, i, &p)) { rc = -1; goto finished; }
    if (!hit) {
        struct stat *copy = malloc(p.count * sizeof(*copy));
        if (copy) {
            memcpy(copy, p.files, p.count * sizeof(*copy));
            pthread_mutex_lock(&m->mutex);
            cached = &m->verified[i];
            if (cached->epoch == epoch) {
                free(cached->files); cached->files = copy; copy = NULL;
                cached->directory = p.directory_identity; cached->valid = true;
            }
            pthread_mutex_unlock(&m->mutex); free(copy);
        }
    }
    rc = 0;
finished:
    if (rc) invalidate_verification(m, i);
    if (!rc && retained) *retained = p; else close_verified(&p);
    return rc;
}
static bool preparation_cancel(void *m) { return cancelled(m); }
static int prepare_package(ascolto_local_models_t *m, int i, int directory_fd) {
    if (!packages[i].preparation_bytes) return 0;
    char path[1280], error[256] = {0};
    runtime_path(m, i, path, sizeof(path));
    if (directory_fd < 0 || !packages[i].count) return -1;
    int input_fd = -1;
    for (size_t f = 0; f < packages[i].count; ++f) {
        int fd = openat(directory_fd, packages[i].files[f].name, O_RDONLY | O_NOFOLLOW);
        int rc = fd < 0 ? -1 : verify_descriptor(m, i, fd, &packages[i].files[f]);
        if (rc) { if (fd >= 0) close(fd); if (input_fd >= 0) close(input_fd); return -1; }
        if (!f) input_fd = fd; else close(fd);
    }
    state(m, i, ASCOLTO_MODEL_VERIFYING, "Preparing native model files…");
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    typedef int (*prepare_v1_t)(int, int, bool (*)(void *), void *, char *, size_t);
    prepare_v1_t prepare = library ? (prepare_v1_t)dlsym(library, "ascolto_stt_prepare_package_v1") : NULL;
    int rc = prepare && !cancelled(m) ? prepare(directory_fd, input_fd, preparation_cancel, m, error, sizeof(error)) : -1;
    if (library) dlclose(library);
    close(input_fd);
    if (rc) { report(m, error[0] ? error : "Native model preparation is unavailable or failed."); return -1; }
    state(m, i, ASCOLTO_MODEL_VERIFYING, "Verifying prepared model…");
    rc = verify_installed_at(m, i, directory_fd);
    if (rc || cancelled(m)) return -1;
    /* Remove only verified inputs that are absent from the installed manifest. */
    for (size_t f = 0; !rc && f < packages[i].count; ++f) {
        bool keep = false;
        for (size_t j = 0; j < packages[i].installed_count; ++j)
            if (!strcmp(packages[i].files[f].name, packages[i].installed[j].name)) keep = true;
        if (!keep) rc = unlinkat(directory_fd, packages[i].files[f].name, 0);
    }
    if (!rc) rc = fsync(directory_fd);
    return rc || cancelled(m) ? -1 : 0;
}
static void disk_space(ascolto_local_models_t *m) {
    struct statvfs v; uint64_t bytes = statvfs(m->directory, &v) ? 0 : (uint64_t)v.f_bavail * v.f_frsize;
    pthread_mutex_lock(&m->mutex); m->snapshot.available_bytes = bytes; ++m->snapshot.revision; pthread_mutex_unlock(&m->mutex);
}
static int remove_directory(ascolto_local_models_t *, int, bool);
static void *scan(void *userdata) {
    ascolto_local_models_t *m = userdata;
    int order[ASCOLTO_LOCAL_MODEL_COUNT], count = 0;
    pthread_mutex_lock(&m->mutex); int selected = model_index(m->snapshot.selected); pthread_mutex_unlock(&m->mutex);
    if (selected > 0) order[count++] = selected;
    for (int i = 1; i < ASCOLTO_LOCAL_MODEL_COUNT; ++i) if (i != selected) order[count++] = i;
    for (int next = 0; next < count && !cancelled(m); ++next) {
        int i = order[next];
        char runtime[1280], directory[1024], staging[1024]; struct stat st;
        model_directory(m, i, true, staging, sizeof(staging));
        bool interrupted = lstat(staging, &st) == 0;
        if (interrupted) remove_directory(m, i, true);
        runtime_path(m, i, runtime, sizeof(runtime)); model_directory(m, i, false, directory, sizeof(directory));
        if (access(runtime, R_OK)) state(m, i, ASCOLTO_MODEL_UNSUPPORTED, "Native runtime is unavailable in this build. Install a build with local model support.");
        else if (lstat(directory, &st)) state(m, i, ASCOLTO_MODEL_MISSING, interrupted ? "Previous download was interrupted. Retry download." : "Not downloaded");
        else if (verify_installed(m, i, NULL)) state(m, i, ASCOLTO_MODEL_ERROR, "Model files are incomplete or corrupt. Retry download or use Whistle.");
        else state(m, i, ASCOLTO_MODEL_READY, "Downloaded · verified");
    }
    disk_space(m);
    pthread_mutex_lock(&m->mutex); m->running = false; pthread_mutex_unlock(&m->mutex); return NULL;
}
ascolto_local_models_t *ascolto_local_models_create(const char *config) {
    if (!config || !*config) return NULL;
    ascolto_local_models_t *m = calloc(1, sizeof(*m)); if (!m) return NULL;
    pthread_mutex_init(&m->mutex, NULL); pthread_mutex_init(&m->operations, NULL); m->downloading = -1;
#ifdef ASCOLTO_LOCAL_STT_TEST_MANIFEST
    m->first_hash = -1;
#endif
    snprintf(m->config, sizeof(m->config), "%s", config);
    char parent[512]; snprintf(parent, sizeof(parent), "%s", config); char *slash = strrchr(parent, '/');
    if (slash) *slash = 0; else snprintf(parent, sizeof(parent), ".");
    snprintf(m->directory, sizeof(m->directory), "%s/local-models", parent);
    ascolto_mkdir_p(m->directory); chmod(m->directory, 0700);
    char executable[1024] = {0};
#ifdef __APPLE__
    uint32_t len = sizeof(executable); _NSGetExecutablePath(executable, &len);
#else
    ssize_t len = readlink("/proc/self/exe", executable, sizeof(executable) - 1); if (len > 0) executable[len] = 0;
#endif
    slash = strrchr(executable, '/'); if (slash) *slash = 0;
#ifdef __APPLE__
    if (strstr(executable, ".app/Contents/MacOS")) snprintf(m->runtime, sizeof(m->runtime), "%s/../Resources/local-stt", executable);
    else
#endif
    snprintf(m->runtime, sizeof(m->runtime), "%s/local-stt", executable);
    snprintf(m->snapshot.selected, sizeof(m->snapshot.selected), "whistle");
    ascolto_db_t *db = NULL;
    if (!ascolto_storage_open(config, &db)) {
        ascolto_storage_read_text(db, "local_transcription_model", m->snapshot.selected, sizeof(m->snapshot.selected)); ascolto_db_close(db);
    }
    for (int i = 0; i < ASCOLTO_LOCAL_MODEL_COUNT; ++i) {
        ascolto_model_snapshot_t *s = &m->snapshot.models[i];
        snprintf(s->id, sizeof(s->id), "%s", ids[i]); snprintf(s->name, sizeof(s->name), "%s", names[i]);
        snprintf(s->description, sizeof(s->description), "%s", descriptions[i]); snprintf(s->hint, sizeof(s->hint), "%s", hints[i]);
        s->state = i ? ASCOLTO_MODEL_CHECKING : ASCOLTO_MODEL_INCLUDED; s->selected = !strcmp(ids[i], m->snapshot.selected);
        for (size_t f = 0; f < packages[i].count; ++f) s->bytes += packages[i].files[f].bytes;
    }
    if (model_index(m->snapshot.selected) < 0) report(m, "The saved local model is unsupported. Choose Whistle explicitly.");
    pthread_once(&curl_once, init_curl);
    m->running = true;
    if (!pthread_create(&m->worker, NULL, scan, m)) m->joinable = true;
    else { m->running = false; report(m, "Could not check local models. Restart Ascolto."); }
    return m;
}
void ascolto_local_models_snapshot(ascolto_local_models_t *m, ascolto_local_models_snapshot_t *out) {
    if (!out) return; memset(out, 0, sizeof(*out)); if (!m) return;
    pthread_mutex_lock(&m->mutex); *out = m->snapshot; pthread_mutex_unlock(&m->mutex);
}
void ascolto_local_models_destroy(ascolto_local_models_t *m) {
    if (!m) return;
    pthread_mutex_lock(&m->mutex); m->closing = true; m->cancel = true; pthread_mutex_unlock(&m->mutex);
    if (m->joinable) pthread_join(m->worker, NULL);
    for (int i = 0; i < ASCOLTO_LOCAL_MODEL_COUNT; ++i) free(m->verified[i].files);
    pthread_mutex_destroy(&m->operations); pthread_mutex_destroy(&m->mutex); free(m);
}
/* Only known files are ever deleted; a symlink is unlinked, never traversed. */
static bool directory_identity(int root, const char *name, int fd) {
    struct stat expected, actual;
    return !fstat(fd, &expected) && !fstatat(root, name, &actual, AT_SYMLINK_NOFOLLOW) &&
        S_ISDIR(actual.st_mode) && expected.st_dev == actual.st_dev && expected.st_ino == actual.st_ino;
}
static int remove_directory_at(int root, int i, bool staging) {
    char name[128]; snprintf(name, sizeof(name), "%s%s", ids[i], staging ? ".download" : "");
    struct stat st; if (fstatat(root, name, &st, AT_SYMLINK_NOFOLLOW)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlinkat(root, name, 0);
    int fd = openat(root, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return -1;
    int rc = 0;
    for (size_t f = 0; !rc && f < packages[i].count; ++f)
        if (unlinkat(fd, packages[i].files[f].name, 0) && errno != ENOENT) rc = -1;
    for (size_t f = 0; !rc && f < packages[i].installed_count; ++f) {
        if (unlinkat(fd, packages[i].installed[f].name, 0) && errno != ENOENT) rc = -1;
        char temporary[256]; snprintf(temporary, sizeof(temporary), "%s.writing", packages[i].installed[f].name);
        if (unlinkat(fd, temporary, 0) && errno != ENOENT) rc = -1;
    }
    if (!rc) rc = directory_identity(root, name, fd) ? unlinkat(root, name, AT_REMOVEDIR) : -1;
    close(fd); return rc;
}
static int remove_directory(ascolto_local_models_t *m, int i, bool staging) {
    int fd = open(m->directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return -1;
    int rc = remove_directory_at(fd, i, staging); close(fd); return rc;
}
typedef struct { ascolto_local_models_t *manager; FILE *file; uint64_t base, received, limit; int index; } transfer_t;
static size_t write_download(char *data, size_t size, size_t count, void *userdata) {
    transfer_t *t = userdata; size_t bytes = size * count;
    if (bytes > t->limit - t->received || cancelled(t->manager)) return 0;
    size_t got = fwrite(data, 1, bytes, t->file); t->received += got; return got;
}
static int progress(void *userdata, curl_off_t total, curl_off_t now, curl_off_t up_total, curl_off_t up) {
    (void)total; (void)now; (void)up_total; (void)up; transfer_t *t = userdata;
    pthread_mutex_lock(&t->manager->mutex);
    t->manager->snapshot.models[t->index].transferred = t->base + t->received;
    ++t->manager->snapshot.revision;
    bool stop = t->manager->cancel || t->manager->closing;
    pthread_mutex_unlock(&t->manager->mutex); return stop;
}
static void *download(void *userdata) {
    ascolto_local_models_t *m = userdata; int i = m->downloading;
    int root_fd = open(m->directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    char stage_name[128]; snprintf(stage_name, sizeof(stage_name), "%s.download", ids[i]);
    int rc = root_fd < 0 ? -1 : remove_directory_at(root_fd, i, true);
    if (!rc && mkdirat(root_fd, stage_name, 0700)) rc = -1;
    int directory_fd = !rc ? openat(root_fd, stage_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW) : -1;
    if (directory_fd < 0) rc = -1;
    transfer_t t = {.manager = m, .index = i};
    for (size_t f = 0; !rc && f < packages[i].count; ++f) {
        const artifact_t *a = &packages[i].files[f];
        int fd = openat(directory_fd, a->name, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
        t.file = fd >= 0 ? fdopen(fd, "wb") : NULL; if (!t.file) { if (fd >= 0) close(fd); rc = -1; break; }
        CURL *curl = curl_easy_init(); t.limit = a->bytes; t.received = 0;
        if (!curl) rc = -1;
        else {
            curl_easy_setopt(curl, CURLOPT_URL, a->url); curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#if defined(ASCOLTO_LOCAL_STT_TEST_MANIFEST) && !defined(ASCOLTO_LOCAL_STT_TEST_HTTPS)
            curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "file");
#else
            curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#endif
            curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L); curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L); curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L); curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "Ascolto/1 local-model-download");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_download); curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L); curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress); curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
            if (curl_easy_perform(curl) != CURLE_OK || t.received != a->bytes) rc = -1;
            curl_easy_cleanup(curl);
        }
        if (fflush(t.file) || fsync(fileno(t.file))) rc = -1;
        if (fclose(t.file)) rc = -1;
        t.base += t.received;
    }
    if (!rc) {
        state(m, i, ASCOLTO_MODEL_VERIFYING, "Verifying download…");
        if (!packages[i].preparation_bytes) rc = verify_installed_at(m, i, directory_fd);
    }
    if (!rc) rc = prepare_package(m, i, directory_fd);
    if (!rc && !cancelled(m)) {
        pthread_mutex_lock(&m->operations);
        rc = cancelled(m) || !directory_identity(root_fd, stage_name, directory_fd) ? -1 : remove_directory_at(root_fd, i, false);
        if (!rc && (cancelled(m) || !directory_identity(root_fd, stage_name, directory_fd))) rc = -1;
        if (!rc) rc = renameat(root_fd, stage_name, root_fd, ids[i]);
        if (!rc) rc = fsync(root_fd);
        pthread_mutex_unlock(&m->operations);
    } else rc = -1;
    if (directory_fd >= 0) close(directory_fd);
    if (rc && root_fd >= 0) remove_directory_at(root_fd, i, true);
    if (root_fd >= 0) close(root_fd);
    if (rc) { state(m, i, ASCOLTO_MODEL_ERROR, cancelled(m) ? "Download cancelled. Retry download when ready." : "Download or verification failed. Check connection and disk space, then retry."); }
    else state(m, i, ASCOLTO_MODEL_READY, "Downloaded · verified");
    disk_space(m);
    pthread_mutex_lock(&m->mutex); m->running = false; m->downloading = -1; pthread_mutex_unlock(&m->mutex); return NULL;
}
int ascolto_local_models_download(ascolto_local_models_t *m, const char *id) {
    int i = model_index(id); if (!m || i <= 0) return -1;
    pthread_mutex_lock(&m->operations);
    pthread_mutex_lock(&m->mutex);
    bool busy = m->running || m->closing || m->snapshot.models[i].active || m->snapshot.models[i].state == ASCOLTO_MODEL_UNSUPPORTED;
    uint64_t needed = m->snapshot.models[i].bytes + packages[i].preparation_bytes;
    pthread_mutex_unlock(&m->mutex);
    if (busy) { pthread_mutex_unlock(&m->operations); report(m, "Model checks, a download or an active session are using these files. Wait, cancel the download or finish that session before retrying."); return -1; }
    invalidate_verification(m, i);
    disk_space(m);
    pthread_mutex_lock(&m->mutex); bool space = m->snapshot.available_bytes >= needed + 64 * 1024 * 1024; pthread_mutex_unlock(&m->mutex);
    if (!space) { pthread_mutex_unlock(&m->operations); state(m, i, ASCOLTO_MODEL_ERROR, "Not enough free disk space for this package. Free space and retry."); return -1; }
    if (m->joinable) { pthread_join(m->worker, NULL); m->joinable = false; }
    pthread_mutex_lock(&m->mutex); m->running = true; m->cancel = false; m->downloading = i; m->snapshot.models[i].transferred = 0; pthread_mutex_unlock(&m->mutex);
    state(m, i, ASCOLTO_MODEL_DOWNLOADING, "Downloading…");
    int rc = pthread_create(&m->worker, NULL, download, m);
    if (!rc) m->joinable = true;
    else { pthread_mutex_lock(&m->mutex); m->running = false; m->downloading = -1; pthread_mutex_unlock(&m->mutex); state(m, i, ASCOLTO_MODEL_ERROR, "Could not start download. Retry."); }
    pthread_mutex_unlock(&m->operations); return rc ? -1 : 0;
}
void ascolto_local_models_cancel(ascolto_local_models_t *m, const char *id) {
    if (!m) return; pthread_mutex_lock(&m->mutex); if (model_index(id) == m->downloading) m->cancel = true; pthread_mutex_unlock(&m->mutex);
}
int ascolto_local_models_select(ascolto_local_models_t *m, const char *id) {
    int i = model_index(id); if (!m || i < 0) return -1;
    pthread_mutex_lock(&m->operations); pthread_mutex_lock(&m->mutex);
    bool ready = i == 0 || m->snapshot.models[i].state == ASCOLTO_MODEL_READY;
    pthread_mutex_unlock(&m->mutex);
    if (!ready) { pthread_mutex_unlock(&m->operations); report(m, "Download and verify this model before selecting it."); return -1; }
    ascolto_db_t *db = NULL; int rc = ascolto_storage_open(m->config, &db);
    if (!rc) { rc = ascolto_storage_set(db, "local_transcription_model", id); ascolto_db_close(db); }
    if (!rc) {
        pthread_mutex_lock(&m->mutex); snprintf(m->snapshot.selected, sizeof(m->snapshot.selected), "%s", id);
        for (int j = 0; j < ASCOLTO_LOCAL_MODEL_COUNT; ++j) m->snapshot.models[j].selected = j == i;
        m->snapshot.error[0] = 0; ++m->snapshot.revision; pthread_mutex_unlock(&m->mutex);
    } else report(m, "Could not save the local model choice. Retry.");
    pthread_mutex_unlock(&m->operations); return rc ? -1 : 0;
}
int ascolto_local_models_remove(ascolto_local_models_t *m, const char *id) {
    int i = model_index(id); if (!m || i <= 0) return -1;
    pthread_mutex_lock(&m->operations); pthread_mutex_lock(&m->mutex);
    bool busy = m->snapshot.models[i].selected || m->snapshot.models[i].active || m->running;
    pthread_mutex_unlock(&m->mutex);
    if (!busy) invalidate_verification(m, i);
    int rc = busy ? -1 : remove_directory(m, i, false);
    if (!rc) { remove_directory(m, i, true); state(m, i, ASCOLTO_MODEL_MISSING, "Not downloaded"); disk_space(m); }
    else report(m, busy ? "Choose another installed model, finish its session and wait for model checks or downloads before removing it." : "Could not remove model files. Check folder permissions.");
    pthread_mutex_unlock(&m->operations); return rc;
}
static int begin_model(ascolto_local_models_t *m, ascolto_local_stt_t *session, const char *id, bool selected) {
    if (!m || !session) return -1;
    pthread_mutex_lock(&m->operations);
    pthread_mutex_lock(&m->mutex); int i = model_index(selected ? m->snapshot.selected : id);
    bool ready = i == 0 || (i > 0 && m->snapshot.models[i].state == ASCOLTO_MODEL_READY);
    bool checking = i > 0 && (m->snapshot.models[i].state == ASCOLTO_MODEL_CHECKING || m->snapshot.models[i].state == ASCOLTO_MODEL_VERIFYING);
    bool busy = m->snapshot.active[0] != 0;
    pthread_mutex_unlock(&m->mutex);
    if (!ready || busy) {
        report(m, busy ? "A transcription session is already active." : checking ?
            (selected ? checking_message : "Requested model is still being checked. Wait for verification to finish.") :
            (selected ? "Selected model is unavailable. Retry download or choose Whistle explicitly." :
                "Requested model is unavailable. Restore this model before retrying the job."));
        pthread_mutex_unlock(&m->operations); return -1;
    }
    memset(session, 0, sizeof(*session));
    char error[256] = {0};
    if (i) {
        pthread_mutex_lock(&m->mutex); snprintf(m->snapshot.loading, sizeof(m->snapshot.loading), "%s", ids[i]);
        snprintf(m->snapshot.models[i].message, sizeof(m->snapshot.models[i].message), "Checking model before starting…");
        ++m->snapshot.revision; pthread_mutex_unlock(&m->mutex);
        verified_package_t verified = {.directory = -1};
        if (verify_installed(m, i, &verified)) {
            state(m, i, ASCOLTO_MODEL_ERROR, "Model files are missing or corrupt. Retry download or choose Whistle.");
            report(m, selected ? "Selected model failed verification. Session was not started." :
                "Requested model failed verification. Job was not started.");
            pthread_mutex_lock(&m->mutex); m->snapshot.loading[0] = 0; ++m->snapshot.revision; pthread_mutex_unlock(&m->mutex);
            pthread_mutex_unlock(&m->operations); return -1;
        }
        pthread_mutex_lock(&m->mutex); snprintf(m->snapshot.models[i].message, sizeof(m->snapshot.models[i].message), "Loading model…");
        ++m->snapshot.revision; pthread_mutex_unlock(&m->mutex);
        char path[1280], directory[1024]; runtime_path(m, i, path, sizeof(path));
        /* RTLD_LOCAL plus hidden, statically linked runtime symbols prevent ggml interposition. */
        session->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        ascolto_stt_entry_t entry = session->library ? (ascolto_stt_entry_t)dlsym(session->library, "ascolto_stt_get_api") : NULL;
        session->api = entry ? entry() : NULL;
        if (session->api && session->api->abi == ASCOLTO_STT_ABI && session->api->load && session->api->transcribe && session->api->destroy) {
            model_directory(m, i, false, directory, sizeof(directory));
            if (i < 3) snprintf(path, sizeof(path), "%s/%s", directory, packages[i].files[0].name); else snprintf(path, sizeof(path), "%s", directory);
            session->context = session->api->load(path, error, sizeof(error));
        }
        if (!unchanged_package(m, i, &verified)) {
            invalidate_verification(m, i);
            if (session->context) { session->api->destroy(session->context); session->context = NULL; }
            state(m, i, ASCOLTO_MODEL_ERROR, "Model files changed while loading. Retry download or choose Whistle.");
            snprintf(error, sizeof(error), "%s model changed while loading. %s was not started.",
                selected ? "Selected" : "Requested", selected ? "Session" : "Job");
        }
        close_verified(&verified);
        if (!session->context) {
            if (session->library) dlclose(session->library); memset(session, 0, sizeof(*session));
            report(m, error[0] ? error : (selected ?
                "Selected native runtime is incompatible or unavailable. Reinstall this build or choose Whistle." :
                "Requested native runtime is incompatible or unavailable. Reinstall this build before retrying the job."));
            pthread_mutex_lock(&m->mutex); m->snapshot.loading[0] = 0; ++m->snapshot.revision; pthread_mutex_unlock(&m->mutex);
            pthread_mutex_unlock(&m->operations); return -1;
        }
        state(m, i, ASCOLTO_MODEL_READY, "Downloaded · verified");
    }
    snprintf(session->id, sizeof(session->id), "%s", ids[i]);
    pthread_mutex_lock(&m->mutex); snprintf(m->snapshot.active, sizeof(m->snapshot.active), "%s", ids[i]);
    m->snapshot.loading[0] = 0;
    m->snapshot.models[i].active = true; m->snapshot.error[0] = 0; ++m->snapshot.revision; pthread_mutex_unlock(&m->mutex);
    pthread_mutex_unlock(&m->operations); return 0;
}
int ascolto_local_stt_begin(ascolto_local_models_t *m, ascolto_local_stt_t *session) {
    return begin_model(m, session, NULL, true);
}
int ascolto_local_stt_begin_id(ascolto_local_models_t *m, ascolto_local_stt_t *session, const char *id) {
    return begin_model(m, session, id, false);
}
#ifdef ASCOLTO_LOCAL_STT_TEST_MANIFEST
uint64_t ascolto_local_models_test_hash_count(ascolto_local_models_t *m, const char *id) {
    int i = model_index(id); if (!m || i < 0) return 0;
    pthread_mutex_lock(&m->mutex); uint64_t count = m->hashes[i]; pthread_mutex_unlock(&m->mutex); return count;
}
int ascolto_local_models_test_first_hash(ascolto_local_models_t *m) {
    pthread_mutex_lock(&m->mutex); int i = m->first_hash; pthread_mutex_unlock(&m->mutex); return i;
}
#endif
void ascolto_local_stt_end(ascolto_local_models_t *m, ascolto_local_stt_t *session) {
    if (!m || !session || !session->id[0]) return;
    pthread_mutex_lock(&m->operations);
    if (session->context) session->api->destroy(session->context);
    if (session->library) dlclose(session->library);
    memset(session, 0, sizeof(*session));
    pthread_mutex_lock(&m->mutex); m->snapshot.active[0] = 0; for (int i = 0; i < ASCOLTO_LOCAL_MODEL_COUNT; ++i) m->snapshot.models[i].active = false; ++m->snapshot.revision; pthread_mutex_unlock(&m->mutex);
    pthread_mutex_unlock(&m->operations);
}
int ascolto_local_stt_transcribe(ascolto_local_models_t *m, ascolto_local_stt_t *s, const float *pcm, int n, const char *lang, const char *keywords, ascolto_stt_text_t *out) {
    if (!s || !s->context || !out || !pcm || n <= 0) return -1;
    char error[256] = {0}; memset(out, 0, sizeof(*out));
    int rc = s->api->transcribe(s->context, pcm, n, lang, keywords, out, error, sizeof(error));
    if (rc) report(m, error[0] ? error : "Local transcription failed. Finish this session, then retry or choose Whistle.");
    return rc;
}
