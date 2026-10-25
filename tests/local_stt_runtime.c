#define _POSIX_C_SOURCE 200809L
#include "ascolto/local_stt.h"
#include "ascolto/offline.h"
#include "ascolto/storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
/* Opt-in offline adapter verification. The caller supplies installed, pinned weights
 * in a temporary config directory and a synthetic/public speech WAV. No download. */
static int run_test(int argc,char **argv){
    if(argc!=3){fprintf(stderr,"usage: %s TEMP_CONFIG SPEECH_WAV\n",argv[0]);return 2;}
    ascolto_audio_buffer_t audio={0};if(ascolto_wav_load(argv[2],&audio)||audio.sample_rate!=16000)return 2;
    ascolto_local_models_t *models=ascolto_local_models_create(argv[1]);if(!models)return 2;
    ascolto_local_models_snapshot_t snapshot;
    for(int n=0;n<6000;n++){ascolto_local_models_snapshot(models,&snapshot);bool busy=false;for(int i=1;i<4;i++)if(snapshot.models[i].state==ASCOLTO_MODEL_CHECKING)busy=true;if(!busy)break;struct timespec wait={0,10000000};nanosleep(&wait,NULL);}
    const char *sequence[]={"whisper-turbo-q5","parakeet-v3-q8","qwen3-asr-1.7b","whisper-turbo-q5","parakeet-v3-q8"};
    int result=0;
    for(unsigned i=0;i<sizeof(sequence)/sizeof(sequence[0]);i++){
        ascolto_local_stt_t session={0};
        if(ascolto_local_models_select(models,sequence[i])||ascolto_local_stt_begin(models,&session)){result=1;break;}
        ascolto_stt_text_t text;
        if(ascolto_local_stt_transcribe(models,&session,audio.samples,audio.num_samples,"en",NULL,&text)||!text.text[0])result=1;
        printf("Adapter %s: %s (%s)\n",sequence[i],result?"FAIL":"PASS",session.api->backend);fflush(stdout);
        ascolto_local_stt_end(models,&session);if(result)break;
    }
    ascolto_local_models_snapshot(models,&snapshot);if(result)fprintf(stderr,"%s\n",snapshot.error);
    ascolto_local_models_destroy(models);free(audio.samples);return result;
}

/* Match the macOS audio consumer's default pthread stack. */
typedef struct { int argc; char **argv; int result; } test_args_t;
static void *run_thread(void *context) { test_args_t *args=context; args->result=run_test(args->argc,args->argv); return NULL; }
int main(int argc,char **argv) {
    test_args_t args={argc,argv,2};pthread_t thread;pthread_attr_t attributes;
    pthread_attr_init(&attributes);pthread_attr_setstacksize(&attributes,512*1024);
    int rc=pthread_create(&thread,&attributes,run_thread,&args);pthread_attr_destroy(&attributes);
    if(rc)return 2;pthread_join(thread,NULL);return args.result;
}
