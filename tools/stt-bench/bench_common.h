/* Shared measurement helpers for local, persistent native ASR benchmarks. */
#ifndef ASCOLTO_BENCH_COMMON_H
#define ASCOLTO_BENCH_COMMON_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <unistd.h>

typedef struct { char id[128], path[2048]; float *pcm; int n; } bench_clip;
static double bench_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static double bench_cpu(void) {
    struct rusage r; getrusage(RUSAGE_SELF, &r);
    return (r.ru_utime.tv_sec+r.ru_stime.tv_sec)*1000.0+
        (r.ru_utime.tv_usec+r.ru_stime.tv_usec)/1000.0;
}
static double bench_rss(void) {
    struct rusage r; getrusage(RUSAGE_SELF, &r);
#ifdef __APPLE__
    return r.ru_maxrss / 1048576.0;
#else
    return r.ru_maxrss / 1024.0;
#endif
}
static void bench_json(const char *s) {
    putchar('"');
    for (const unsigned char *p=(const unsigned char *)(s?s:""); *p; p++) {
        if (*p=='"' || *p=='\\') { putchar('\\'); putchar(*p); }
        else if (*p=='\n') fputs("\\n",stdout);
        else if (*p=='\r') fputs("\\r",stdout);
        else if (*p=='\t') fputs("\\t",stdout);
        else if (*p<32) printf("\\u%04x",*p);
        else putchar(*p);
    }
    putchar('"');
}
/* Input is TSV: clip identifier, absolute raw float32 path. No private titles. */
static int bench_clips(const char *manifest, bench_clip *clips, int cap) {
    FILE *f=fopen(manifest,"r"); if(!f) return -1;
    char line[4096]; int n=0;
    while(n<cap && fgets(line,sizeof(line),f)) {
        char *tab=strchr(line,'\t'); if(!tab) continue; *tab++=0;
        tab[strcspn(tab,"\r\n")]=0;
        bench_clip *c=&clips[n]; memset(c,0,sizeof(*c));
        snprintf(c->id,sizeof(c->id),"%s",line);
        snprintf(c->path,sizeof(c->path),"%s",tab);
        FILE *audio=fopen(tab,"rb"); if(!audio) { fclose(f); return -1; }
        fseek(audio,0,SEEK_END); long bytes=ftell(audio); rewind(audio);
        c->n=(int)(bytes/4); c->pcm=(float*)malloc(bytes);
        if(!c->pcm || fread(c->pcm,1,bytes,audio)!=(size_t)bytes) { fclose(audio);fclose(f);return -1; }
        fclose(audio); n++;
    }
    fclose(f); return n;
}
static void bench_emit(const char *model, const char *backend, const bench_clip *c,
    int repeat, double load_ms, double total_ms, double first_ms, double cpu_ms,
    const char *first_kind, const char *text, int tokens, int error) {
    fputs("{\"model\":",stdout);bench_json(model);
    fputs(",\"backend\":",stdout);bench_json(backend);
    fputs(",\"clip\":",stdout);bench_json(c->id);
    printf(",\"repeat\":%d,\"warmup\":%s,\"audio_s\":%.6f,\"load_ms\":%.3f,\"total_ms\":%.3f,\"first_token_ms\":",
        repeat,repeat<0?"true":"false",c->n/16000.0,load_ms,total_ms);
    if(first_ms>=0) printf("%.3f",first_ms); else fputs("null",stdout);
    fputs(",\"first_kind\":",stdout);bench_json(first_kind);
    printf(",\"cpu_ms\":%.3f,\"peak_rss_mb\":%.3f,\"tokens\":%d,\"error\":%d,\"pid\":%d,\"text\":",
        cpu_ms,bench_rss(),tokens,error,getpid());bench_json(text);
    fputs("}\n",stdout);fflush(stdout);
}
#endif
