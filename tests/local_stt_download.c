#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ascolto/local_stt.h"
#include "ascolto/storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
/* Explicit network smoke check: one pinned public 6 KB config, no speech weights. */
int main(void){
    char folder[]="/tmp/ascolto-stt-https-XXXXXX";if(!mkdtemp(folder))return 2;
    char config[512];snprintf(config,sizeof(config),"%s/config.json",folder);
    ascolto_local_models_t *m=ascolto_local_models_create(config);if(!m)return 2;
    ascolto_local_models_snapshot_t s;
    for(int n=0;n<1000;n++){ascolto_local_models_snapshot(m,&s);if(s.models[1].state!=ASCOLTO_MODEL_CHECKING)break;struct timespec wait={0,10000000};nanosleep(&wait,NULL);}
    if(ascolto_local_models_download(m,"whisper-turbo-q5")){ascolto_local_models_destroy(m);return 1;}
    for(int n=0;n<9000;n++){ascolto_local_models_snapshot(m,&s);if(s.models[1].state==ASCOLTO_MODEL_READY||s.models[1].state==ASCOLTO_MODEL_ERROR)break;struct timespec wait={0,10000000};nanosleep(&wait,NULL);}
    int failed=s.models[1].state!=ASCOLTO_MODEL_READY||s.models[1].transferred!=6194||strcmp(s.selected,"whistle");
    printf("Pinned public HTTPS download, size/hash verification and no activation: %s (%llu bytes)\n",failed?"FAIL":"PASS",(unsigned long long)s.models[1].transferred);
    if(failed)fprintf(stderr,"%s %s\n",s.error,s.models[1].message);
    ascolto_local_models_destroy(m);return failed;
}
