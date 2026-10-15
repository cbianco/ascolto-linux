#include "ascolto.h"
#include <dispatch/dispatch.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static dispatch_queue_t diagnostic_queue;
static FILE* diagnostic_file;

void ascolto_diagnostics_init(const char* config_path) {
    if (!config_path || diagnostic_queue) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s", config_path);
    char* slash = strrchr(path, '/');
    if (!slash) return;
    snprintf(slash + 1, sizeof(path) - (size_t)(slash + 1 - path), "diagnostics.log");
    struct stat info;
    if (stat(path, &info) == 0 && info.st_size > 2 * 1024 * 1024) {
        char previous[1056];
        snprintf(previous, sizeof(previous), "%s.previous", path);
        rename(path, previous);
    }
    diagnostic_file = fopen(path, "a");
    if (!diagnostic_file) return;
    setvbuf(diagnostic_file, NULL, _IOLBF, 0);
    diagnostic_queue = dispatch_queue_create("com.cristianbianco.ascolto.diagnostics", DISPATCH_QUEUE_SERIAL);
    ascolto_diagnostic("Process started pid=%d", getpid());
}

void ascolto_diagnostic(const char* format, ...) {
    if (!diagnostic_queue) return;
    struct {
        char message[512];
        struct timeval time;
        uint64_t thread;
    } record;
    record.thread = 0;
    va_list args;
    va_start(args, format);
    vsnprintf(record.message, sizeof(record.message), format, args);
    va_end(args);
    gettimeofday(&record.time, NULL);
    pthread_threadid_np(NULL, &record.thread);
    dispatch_async(diagnostic_queue, ^{
        fprintf(diagnostic_file, "%lld.%03d thread=%llu %s\n",
                (long long)record.time.tv_sec, (int)(record.time.tv_usec / 1000),
                (unsigned long long)record.thread, record.message);
    });
}
