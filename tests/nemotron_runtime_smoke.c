#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ascolto/local_stt.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Exercise the real native library's failure cleanup on the audio consumer stack.
 * This is not a recognition or performance test: no weights are downloaded. */
static void *check(void *argument) {
    const char *library = argument;
    char directory[] = "/tmp/ascolto-nemotron-smoke-XXXXXX";
    if (!mkdtemp(directory)) return (void *)1;
    int failed = 0;
    for (int i = 0; i < 3 && !failed; ++i) {
        void *handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
        ascolto_stt_entry_t entry = handle ? (ascolto_stt_entry_t)dlsym(handle, "ascolto_stt_get_api") : NULL;
        const ascolto_stt_api_t *api = entry ? entry() : NULL;
        if (!api || api->abi != ASCOLTO_STT_ABI || !api->load || !api->transcribe || !api->destroy) {
            fprintf(stderr, "Native Nemotron ABI could not load: %s\n", dlerror());
            failed = 1;
        } else {
            char error[256] = {0};
            void *context = api->load(directory, error, sizeof(error));
            if (context) { api->destroy(context); failed = 1; }
            if (!error[0]) failed = 1;
        }
        if (handle && dlclose(handle)) failed = 1;
    }
    rmdir(directory);
    return failed ? (void *)1 : NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 512 * 1024);
    pthread_t thread;
    int rc = pthread_create(&thread, &attributes, check, argv[1]);
    pthread_attr_destroy(&attributes);
    if (rc) return 2;
    void *result = NULL;
    pthread_join(thread, &result);
    puts(result ? "Native Nemotron load/unload: FAIL" : "Native Nemotron load/unload: PASS (missing weights; 512 KiB stack)");
    return result ? 1 : 0;
}
