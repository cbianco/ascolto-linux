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
    if(argc<3||argc>6){fprintf(stderr,"usage: %s TEMP_CONFIG SPEECH_WAV [MODEL_ID [LANGUAGE [EXPECTED_WORD]]]\n",argv[0]);return 2;}
    ascolto_audio_buffer_t audio={0};if(ascolto_wav_load(argv[2],&audio)||audio.sample_rate!=16000)return 2;
    ascolto_local_models_t *models=ascolto_local_models_create(argv[1]);if(!models)return 2;
    ascolto_local_models_snapshot_t snapshot;
    for(int n=0;n<6000;n++){ascolto_local_models_snapshot(models,&snapshot);bool busy=false;for(int i=1;i<ASCOLTO_LOCAL_MODEL_COUNT;i++)if(snapshot.models[i].state==ASCOLTO_MODEL_CHECKING)busy=true;if(!busy)break;struct timespec wait={0,10000000};nanosleep(&wait,NULL);}
    const char *sequence[]={"whisper-turbo-q5","parakeet-v3-q8","qwen3-asr-1.7b","whisper-turbo-q5","parakeet-v3-q8"};
    const char *nemotron_sequence[]={"nemotron-3.5-q8","nemotron-3.5-bf16-ssd","nemotron-3.5-q8"};
    bool switching=argc>=4&&!strcmp(argv[3],"nemotron-switch");
    int result=0;
    unsigned count=switching?3:argc>=4?2:sizeof(sequence)/sizeof(sequence[0]);
    for(unsigned i=0;i<count;i++){
        const char *model=switching?nemotron_sequence[i]:argc>=4?argv[3]:sequence[i];
        ascolto_local_stt_t session={0};
        if(ascolto_local_models_select(models,model)||ascolto_local_stt_begin(models,&session)){result=1;break;}
        ascolto_stt_text_t text;
        struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);
        if(ascolto_local_stt_transcribe(models,&session,audio.samples,audio.num_samples,argc>=5?argv[4]:"en",NULL,&text)||!text.text[0])result=1;
        clock_gettime(CLOCK_MONOTONIC,&end);
        if(argc==6&&!strstr(text.text,argv[5]))result=1;
        double elapsed=(end.tv_sec-start.tv_sec)+(end.tv_nsec-start.tv_nsec)/1e9;
        printf("Adapter %s: %s (%s; %.3f s for %.3f s audio)\n",model,result?"FAIL":"PASS",session.api->backend,elapsed,audio.num_samples/16000.0);
        if(argc>=4)printf("Synthetic/public fixture transcript: %s\n",text.text);fflush(stdout);
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
