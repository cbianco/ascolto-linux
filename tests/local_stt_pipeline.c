#define _POSIX_C_SOURCE 200809L
#include "ascolto.h"
#include "ascolto/offline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
/* Production VAD/ASR/Whistle-embedding/SQLite path, without a capture device. */
static int run_test(int argc,char **argv){
    if(argc!=4){fprintf(stderr,"usage: %s TEMP_CONFIG SPEECH_WAV WHISTLE_MODEL\n",argv[0]);return 2;}
    ascolto_audio_buffer_t audio={0};if(ascolto_wav_load(argv[2],&audio)||audio.sample_rate!=16000)return 2;
    ascolto_whistle_t whistle;if(ascolto_whistle_init(&whistle,argv[3]))return 2;
    ascolto_config_t config;ascolto_config_init_defaults(&config);config.auto_trigger_mic=false;config.save_audio=false;
    snprintf(config.config_path,sizeof(config.config_path),"%s",argv[1]);snprintf(config.output_dir,sizeof(config.output_dir),"%s-calls",argv[1]);snprintf(config.language,sizeof(config.language),"en");
    if(ascolto_config_save(&config,NULL))return 2;
    ascolto_live_app_t app;if(ascolto_live_app_init(&app,&whistle,argv[1]))return 2;
    for(int n=0;n<6000;n++){ascolto_local_models_snapshot_t s;ascolto_local_models_snapshot(app.local_models,&s);bool busy=false;for(int i=1;i<4;i++)if(s.models[i].state==ASCOLTO_MODEL_CHECKING)busy=true;if(!busy)break;struct timespec wait={0,10000000};nanosleep(&wait,NULL);}
    const char *models[]={"whisper-turbo-q5","parakeet-v3-q8","qwen3-asr-1.7b"};int result=0;float silence[ASCOLTO_FRAME_SAMPLES]={0};
    for(int engine=0;engine<3&&!result;engine++){
        if(ascolto_local_models_select(app.local_models,models[engine])||ascolto_live_app_start_session(&app,true)){result=1;break;}
        /* Selecting another model must leave this session's decoder unchanged. */
        if(ascolto_local_models_select(app.local_models,"whistle")||strcmp(app.local_stt.id,models[engine])){result=1;break;}
        for(int n=0;n<audio.num_samples;n+=ASCOLTO_FRAME_SAMPLES){int count=audio.num_samples-n;if(count>ASCOLTO_FRAME_SAMPLES)count=ASCOLTO_FRAME_SAMPLES;ascolto_live_app_feed_audio(&app,ASCOLTO_CHANNEL_SYS,audio.samples+n,count);ascolto_live_app_feed_audio(&app,ASCOLTO_CHANNEL_MIC,silence,count);}
        for(int frame=0;frame<30;frame++){ascolto_live_app_feed_audio(&app,ASCOLTO_CHANNEL_SYS,silence,ASCOLTO_FRAME_SAMPLES);ascolto_live_app_feed_audio(&app,ASCOLTO_CHANNEL_MIC,silence,ASCOLTO_FRAME_SAMPLES);}
        if(ascolto_live_app_stop_session(&app)||app.transcription_error||app.files.storage_error||app.session_utterance_count<1||app.diarizer.num_speakers<1)result=1;
        printf("Production pipeline %s: %s (%d turns, %d Whistle speaker identities)\n",models[engine],result?"FAIL":"PASS",app.session_utterance_count,app.diarizer.num_speakers);fflush(stdout);
        if(result)break;
        if(ascolto_local_models_select(app.local_models,models[engine])||ascolto_live_app_start_session(&app,true)){result=1;break;}
        for(int frame=0;frame<100;frame++){ascolto_live_app_feed_audio(&app,ASCOLTO_CHANNEL_SYS,silence,ASCOLTO_FRAME_SAMPLES);ascolto_live_app_feed_audio(&app,ASCOLTO_CHANNEL_MIC,silence,ASCOLTO_FRAME_SAMPLES);}
        if(ascolto_live_app_stop_session(&app)||app.session_utterance_count)result=1;
        printf("Silence gate %s: %s\n",models[engine],result?"FAIL":"PASS");fflush(stdout);
    }
    ascolto_live_app_free(&app);ascolto_whistle_free(&whistle);free(audio.samples);return result;
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
