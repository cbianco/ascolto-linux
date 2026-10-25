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
typedef struct { const artifact_t *files; size_t count; } package_t;
#ifdef ASCOLTO_LOCAL_STT_TEST_MANIFEST
#include "../tests/local_stt_manifest.inc"
#else
#include "local_stt_manifest.inc"
#endif
struct ascolto_local_models {
    pthread_mutex_t mutex, operations;
    pthread_t worker;
    bool joinable, running, cancel, closing;
    int downloading;
    char config[512], directory[768], runtime[1024];
    ascolto_local_models_snapshot_t snapshot;
};
static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static void init_curl(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }
static const char *ids[] = {"whistle", "whisper-turbo-q5", "parakeet-v3-q8", "qwen3-asr-1.7b"};
static const char *names[] = {"Whistle", "Whisper large-v3-turbo Q5", "Parakeet TDT v3", "Qwen3-ASR 1.7B"};
static const char *descriptions[] = {
    "Current baseline. Lower agreement in our sample; very lightweight.",
    "Closest to the cloud reference in our Italian sample.",
    "Good agreement in our sample, with very low processing delay.",
    "Good agreement in our sample. Uses more memory than the other options."};
static const char *hints[] = {"Default · included · 17 MB", "Best test agreement", "Speed / quality balance", "CPU option · about 7 GB memory"};
static int model_index(const char *id) { for (int i = 0; id && i < 4; ++i) if (!strcmp(id, ids[i])) return i; return -1; }
static void state(ascolto_local_models_t *m, int i, ascolto_model_state_t value, const char *message) {
    pthread_mutex_lock(&m->mutex);
    m->snapshot.models[i].state = value;
    snprintf(m->snapshot.models[i].message, sizeof(m->snapshot.models[i].message), "%s", message ? message : "");
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
static int verify_file(ascolto_local_models_t *m, const char *path, const artifact_t *a) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != a->bytes) { close(fd); return -1; }
    digest_t digest; if (!digest_init(&digest)) { close(fd); return -1; }
    unsigned char buffer[64 * 1024], hash[32]; ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        if (pthread_equal(pthread_self(), m->worker) && cancelled(m)) {
#ifndef __APPLE__
            EVP_MD_CTX_free(digest);
#endif
            close(fd); return -2;
        }
        digest_update(&digest, buffer, (unsigned)n);
    }
    close(fd);
    int finalized = digest_final(hash, &digest);
    if (n < 0 || !finalized) return -1;
    char hex[65]; for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    return strcmp(hex, a->sha256) ? -1 : 0;
}
static int verify_package(ascolto_local_models_t *m, int i, bool staging) {
    char directory[1024], path[1280]; model_directory(m, i, staging, directory, sizeof(directory));
    struct stat st; if (lstat(directory, &st) || !S_ISDIR(st.st_mode)) return -1;
    for (size_t f = 0; f < packages[i].count; ++f) {
        snprintf(path, sizeof(path), "%s/%s", directory, packages[i].files[f].name);
        int rc = verify_file(m, path, &packages[i].files[f]); if (rc) return rc;
    }
    return 0;
}
static void disk_space(ascolto_local_models_t *m) {
    struct statvfs v; uint64_t bytes = statvfs(m->directory, &v) ? 0 : (uint64_t)v.f_bavail * v.f_frsize;
    pthread_mutex_lock(&m->mutex); m->snapshot.available_bytes = bytes; ++m->snapshot.revision; pthread_mutex_unlock(&m->mutex);
}
static int remove_directory(ascolto_local_models_t *, int, bool);
static void *scan(void *userdata) {
    ascolto_local_models_t *m = userdata;
    for (int i = 1; i < 4 && !cancelled(m); ++i) {
        char runtime[1280], directory[1024], staging[1024]; struct stat st;
        model_directory(m, i, true, staging, sizeof(staging));
        bool interrupted = lstat(staging, &st) == 0;
        if (interrupted) remove_directory(m, i, true);
        runtime_path(m, i, runtime, sizeof(runtime)); model_directory(m, i, false, directory, sizeof(directory));
        if (access(runtime, R_OK)) state(m, i, ASCOLTO_MODEL_UNSUPPORTED, "Native runtime is unavailable in this build. Install a build with local model support.");
        else if (lstat(directory, &st)) state(m, i, ASCOLTO_MODEL_MISSING, interrupted ? "Previous download was interrupted. Retry download." : "Not downloaded");
        else if (verify_package(m, i, false)) state(m, i, ASCOLTO_MODEL_ERROR, "Model files are incomplete or corrupt. Retry download or use Whistle.");
        else state(m, i, ASCOLTO_MODEL_READY, "Downloaded · verified");
    }
    disk_space(m);
    pthread_mutex_lock(&m->mutex); m->running = false; pthread_mutex_unlock(&m->mutex); return NULL;
}
ascolto_local_models_t *ascolto_local_models_create(const char *config) {
    if (!config || !*config) return NULL;
    ascolto_local_models_t *m = calloc(1, sizeof(*m)); if (!m) return NULL;
    pthread_mutex_init(&m->mutex, NULL); pthread_mutex_init(&m->operations, NULL); m->downloading = -1;
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
    for (int i = 0; i < 4; ++i) {
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
    pthread_mutex_destroy(&m->operations); pthread_mutex_destroy(&m->mutex); free(m);
}
/* Only known files are ever deleted; a symlink is unlinked, never traversed. */
static int remove_directory(ascolto_local_models_t *m, int i, bool staging) {
    char directory[1024], path[1280]; model_directory(m, i, staging, directory, sizeof(directory));
    struct stat st; if (lstat(directory, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(directory);
    for (size_t f = 0; f < packages[i].count; ++f) { snprintf(path, sizeof(path), "%s/%s", directory, packages[i].files[f].name); if (unlink(path) && errno != ENOENT) return -1; }
    return rmdir(directory);
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
    char directory[1024], path[1280]; model_directory(m, i, true, directory, sizeof(directory));
    int rc = remove_directory(m, i, true); if (!rc && mkdir(directory, 0700)) rc = -1;
    transfer_t t = {.manager = m, .index = i};
    for (size_t f = 0; !rc && f < packages[i].count; ++f) {
        const artifact_t *a = &packages[i].files[f]; snprintf(path, sizeof(path), "%s/%s", directory, a->name);
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
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
    if (!rc) { state(m, i, ASCOLTO_MODEL_VERIFYING, "Verifying download…"); rc = verify_package(m, i, true); }
    if (!rc && !cancelled(m)) {
        char destination[1024]; model_directory(m, i, false, destination, sizeof(destination));
        pthread_mutex_lock(&m->operations);
        rc = cancelled(m) ? -1 : remove_directory(m, i, false);
        if (!rc) rc = rename(directory, destination);
        if (!rc) { int fd = open(m->directory, O_RDONLY); if (fd >= 0) { fsync(fd); close(fd); } }
        pthread_mutex_unlock(&m->operations);
    } else rc = -1;
    if (rc) { remove_directory(m, i, true); state(m, i, ASCOLTO_MODEL_ERROR, cancelled(m) ? "Download cancelled. Retry download when ready." : "Download or verification failed. Check connection and disk space, then retry."); }
    else state(m, i, ASCOLTO_MODEL_READY, "Downloaded · verified");
    disk_space(m);
    pthread_mutex_lock(&m->mutex); m->running = false; m->downloading = -1; pthread_mutex_unlock(&m->mutex); return NULL;
}
int ascolto_local_models_download(ascolto_local_models_t *m, const char *id) {
    int i = model_index(id); if (!m || i <= 0) return -1;
    pthread_mutex_lock(&m->operations);
    pthread_mutex_lock(&m->mutex);
    bool busy = m->running || m->closing || m->snapshot.models[i].active || m->snapshot.models[i].state == ASCOLTO_MODEL_UNSUPPORTED;
    uint64_t needed = m->snapshot.models[i].bytes;
    pthread_mutex_unlock(&m->mutex);
    if (busy) { pthread_mutex_unlock(&m->operations); report(m, "Model checks, a download or an active session are using these files. Wait, cancel the download or finish that session before retrying."); return -1; }
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
        for (int j = 0; j < 4; ++j) m->snapshot.models[j].selected = j == i;
        m->snapshot.error[0] = 0; ++m->snapshot.revision; pthread_mutex_unlock(&m->mutex);
    } else report(m, "Could not save the local model choice. Retry.");
    pthread_mutex_unlock(&m->operations); return rc ? -1 : 0;
}
int ascolto_local_models_remove(ascolto_local_models_t *m, const char *id) {
    int i = model_index(id); if (!m || i <= 0) return -1;
    pthread_mutex_lock(&m->operations); pthread_mutex_lock(&m->mutex);
    bool busy = m->snapshot.models[i].selected || m->snapshot.models[i].active || m->running;
    pthread_mutex_unlock(&m->mutex);
    int rc = busy ? -1 : remove_directory(m, i, false);
    if (!rc) { remove_directory(m, i, true); state(m, i, ASCOLTO_MODEL_MISSING, "Not downloaded"); disk_space(m); }
    else report(m, busy ? "Choose another installed model, finish its session and wait for model checks or downloads before removing it." : "Could not remove model files. Check folder permissions.");
    pthread_mutex_unlock(&m->operations); return rc;
}
int ascolto_local_stt_begin(ascolto_local_models_t *m, ascolto_local_stt_t *session) {
    if (!m || !session) return -1;
    pthread_mutex_lock(&m->operations);
    pthread_mutex_lock(&m->mutex); int i = model_index(m->snapshot.selected);
    bool ready = i == 0 || (i > 0 && m->snapshot.models[i].state == ASCOLTO_MODEL_READY);
    bool busy = m->snapshot.active[0] != 0;
    pthread_mutex_unlock(&m->mutex);
    if (!ready || busy) { report(m, "Selected model is unavailable or still being checked. Retry download or choose Whistle explicitly."); pthread_mutex_unlock(&m->operations); return -1; }
    memset(session, 0, sizeof(*session));
    char error[256] = {0};
    if (i) {
        if (verify_package(m, i, false)) { state(m, i, ASCOLTO_MODEL_ERROR, "Model files are missing or corrupt. Retry download or choose Whistle."); report(m, "Selected model failed verification. Session was not started."); pthread_mutex_unlock(&m->operations); return -1; }
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
        if (!session->context) {
            if (session->library) dlclose(session->library); memset(session, 0, sizeof(*session));
            report(m, error[0] ? error : "Selected native runtime is incompatible or unavailable. Reinstall this build or choose Whistle.");
            pthread_mutex_unlock(&m->operations); return -1;
        }
    }
    snprintf(session->id, sizeof(session->id), "%s", ids[i]);
    pthread_mutex_lock(&m->mutex); snprintf(m->snapshot.active, sizeof(m->snapshot.active), "%s", ids[i]);
    m->snapshot.models[i].active = true; m->snapshot.error[0] = 0; ++m->snapshot.revision; pthread_mutex_unlock(&m->mutex);
    pthread_mutex_unlock(&m->operations); return 0;
}
void ascolto_local_stt_end(ascolto_local_models_t *m, ascolto_local_stt_t *session) {
    if (!m || !session || !session->id[0]) return;
    pthread_mutex_lock(&m->operations);
    if (session->context) session->api->destroy(session->context);
    if (session->library) dlclose(session->library);
    memset(session, 0, sizeof(*session));
    pthread_mutex_lock(&m->mutex); m->snapshot.active[0] = 0; for (int i = 0; i < 4; ++i) m->snapshot.models[i].active = false; ++m->snapshot.revision; pthread_mutex_unlock(&m->mutex);
    pthread_mutex_unlock(&m->operations);
}
int ascolto_local_stt_transcribe(ascolto_local_models_t *m, ascolto_local_stt_t *s, const float *pcm, int n, const char *lang, const char *keywords, ascolto_stt_text_t *out) {
    if (!s || !s->context || !out || !pcm || n <= 0) return -1;
    char error[256] = {0}; memset(out, 0, sizeof(*out));
    int rc = s->api->transcribe(s->context, pcm, n, lang, keywords, out, error, sizeof(error));
    if (rc) report(m, error[0] ? error : "Local transcription failed. Finish this session, then retry or choose Whistle.");
    return rc;
}
