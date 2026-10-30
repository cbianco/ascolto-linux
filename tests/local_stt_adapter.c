#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ascolto/local_stt.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
static void *load(const char *path, char *error, size_t cap) {
    const char *gate=getenv("ASCOLTO_STT_TEST_LOAD_GATE");
    if(gate) {
        char signal[1400]; snprintf(signal,sizeof(signal),"%s.reached",gate);
        FILE *f=fopen(signal,"wb"); if(!f)return NULL; fclose(f);
        snprintf(signal,sizeof(signal),"%s.resume",gate);
        int attempt=0;
        for(;attempt<5000&&access(signal,F_OK);++attempt){struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
        if(attempt==5000){snprintf(error,cap,"Synthetic load gate timed out");return NULL;}
    }
    if (strstr(path,"nemotron-3.5-")) {
        char file[1400];
        snprintf(file,sizeof(file),"%s/decoder.gguf",path);
        if (access(file,R_OK)) { snprintf(error,cap,"Missing synthetic decoder"); return NULL; }
        snprintf(file,sizeof(file),"%s/encoder.gguf",path);
        if (access(file,R_OK)) { snprintf(error,cap,"Missing synthetic encoder"); return NULL; }
    }
    return malloc(1);
}
static int transcribe(void *ctx,const float *pcm,int n,const char *lang,const char *keywords,ascolto_stt_text_t *out,char *error,size_t cap) {
    (void)ctx;(void)pcm;(void)n;
    if(keywords&&!strcmp(keywords,"fail")){snprintf(error,cap,"Synthetic inference failure");return -1;}
    memset(out,0,sizeof(*out));snprintf(out->text,sizeof(out->text),"Synthetic adapter result");snprintf(out->language,sizeof(out->language),"%s",lang);return 0;
}
__attribute__((visibility("default"))) const ascolto_stt_api_t *ascolto_stt_get_api(void) {
    static ascolto_stt_api_t api={ASCOLTO_STT_ABI,load,transcribe,free,"synthetic test"};api.abi=getenv("ASCOLTO_STT_TEST_BAD_ABI")?999:ASCOLTO_STT_ABI;return &api;
}
#ifndef ASCOLTO_STT_TEST_NO_PREPARE
__attribute__((visibility("default"))) int ascolto_stt_prepare_package_v1(
    int directory, int input, bool (*cancel)(void *), void *userdata, char *error, size_t capacity) {
    char bytes[3];
    if (pread(input, bytes, 3, 0) != 3 || memcmp(bytes, "abc", 3) || faccessat(directory, "model.safetensors", F_OK, 0)) {
        snprintf(error, capacity, "Missing verified input descriptor"); return -1;
    }
    const char *mode = getenv("ASCOLTO_STT_TEST_PREPARE");
    if (mode && !strcmp(mode, "slow")) {
        for (int i = 0; i < 3000; ++i) {
            if (cancel && cancel(userdata)) { snprintf(error, capacity, "Preparation cancelled"); return -1; }
            struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
        }
    }
    if (mode && !strcmp(mode, "fail")) { snprintf(error, capacity, "Synthetic preparation failure"); return -1; }
    if (mode && !strcmp(mode, "replace")) {
        const char *from = getenv("ASCOLTO_STT_TEST_REPLACE_FROM"), *to = getenv("ASCOLTO_STT_TEST_REPLACE_TO");
        if (!from || !to || rename(from, to) || mkdir(from, 0700)) return -1;
        char marker[2048]; snprintf(marker, sizeof(marker), "%s/untouched", from);
        FILE *file = fopen(marker, "w"); if (!file) return -1; fputs("untouched", file); fclose(file);
    }
    const char *names[] = {"decoder.gguf", "encoder.gguf"};
    for (size_t i = 0; i < 2; ++i) {
        if (cancel && cancel(userdata)) return -1;
        int fd = openat(directory, names[i], O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
        if (fd < 0) return -1;
        const char *data = mode && !strcmp(mode, "corrupt") ? "bad" : "abc";
        int rc = write(fd, data, 3) == 3 && !fsync(fd) ? 0 : -1; close(fd); if (rc) return rc;
    }
    return 0;
}
#endif
