#include "ascolto/local_stt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *load(const char *path, char *error, size_t cap) { (void)path;(void)error;(void)cap;return malloc(1); }
static int transcribe(void *ctx,const float *pcm,int n,const char *lang,const char *keywords,ascolto_stt_text_t *out,char *error,size_t cap) {
    (void)ctx;(void)pcm;(void)n;
    if(keywords&&!strcmp(keywords,"fail")){snprintf(error,cap,"Synthetic inference failure");return -1;}
    memset(out,0,sizeof(*out));snprintf(out->text,sizeof(out->text),"Synthetic adapter result");snprintf(out->language,sizeof(out->language),"%s",lang);return 0;
}
__attribute__((visibility("default"))) const ascolto_stt_api_t *ascolto_stt_get_api(void) {
    static ascolto_stt_api_t api={ASCOLTO_STT_ABI,load,transcribe,free,"synthetic test"};api.abi=getenv("ASCOLTO_STT_TEST_BAD_ABI")?999:ASCOLTO_STT_ABI;return &api;
}
