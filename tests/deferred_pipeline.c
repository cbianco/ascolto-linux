#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ascolto.h"
#include "ascolto/deferred_store.h"
#include "ascolto/deferred.h"
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern int ascolto_deferred_process_job(ascolto_live_app_t *,ascolto_deferred_store_t *,const ascolto_deferred_job_t *,bool (*)(void *),void *);
extern void ascolto_deferred_finalize_job(ascolto_live_app_t *,const ascolto_deferred_job_t *);
static int checks, asr_calls, begin_calls, fail_call, stop_after, mutated, archive_calls;
static const char *mutate_path;
static bool authority_fixture;
static bool complete_fixture;
static int fixture_samples = 170 * ASCOLTO_FRAME_SAMPLES;
static float *expected_pcm[2];
static int expected_count;
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"Failed at %d: %s\n",__LINE__,#x); abort(); } checks++; } while(0)

/* All PCM transport, native diarization, capture lifecycle, chunk
 * checkpointing and exports are production code. Only model/hardware/cloud calls
 * are replaced; no weights, microphone, service or external account is used. */
ascolto_local_models_t *ascolto_local_models_create(const char *path) { (void)path; return (void*)1; }
void ascolto_local_models_destroy(ascolto_local_models_t *models) { (void)models; }
void ascolto_local_models_snapshot(ascolto_local_models_t *models,ascolto_local_models_snapshot_t *snapshot) {
    (void)models; memset(snapshot,0,sizeof(*snapshot)); snprintf(snapshot->selected,sizeof(snapshot->selected),"fixture-model");
}
int ascolto_local_stt_begin(ascolto_local_models_t *models,ascolto_local_stt_t *session) { (void)models;(void)session;begin_calls++;return -1; }
void ascolto_local_stt_end(ascolto_local_models_t *models,ascolto_local_stt_t *session) { (void)models;(void)session; }
int ascolto_local_stt_transcribe(ascolto_local_models_t *models,ascolto_local_stt_t *session,const float *pcm,int count,const char *language,const char *keywords,ascolto_stt_text_t *out) {
    (void)models;(void)session;(void)language;(void)keywords;
    asr_calls++; if(asr_calls==fail_call)return -1;
    if(complete_fixture) {
        int chunk = (asr_calls - 1) / 2;
        int offset = chunk * ASCOLTO_TRANSCRIPTION_CHUNK_SECONDS * ASCOLTO_SAMPLE_RATE;
        int remaining = expected_count - offset;
        int expected = remaining > ASCOLTO_TRANSCRIPTION_CHUNK_SECONDS * ASCOLTO_SAMPLE_RATE
            ? ASCOLTO_TRANSCRIPTION_CHUNK_SECONDS * ASCOLTO_SAMPLE_RATE : remaining;
        CHECK(asr_calls<=6);CHECK(count==expected);
        CHECK(!memcmp(pcm,expected_pcm[asr_calls%2?ASCOLTO_CHANNEL_SYS:ASCOLTO_CHANNEL_MIC]+offset,(size_t)count*sizeof(float)));
        memset(out,0,sizeof(*out));return 0;
    }
    if(authority_fixture) {
        CHECK(asr_calls<=2);CHECK(count==expected_count);
        CHECK(!memcmp(pcm,expected_pcm[asr_calls==1?ASCOLTO_CHANNEL_SYS:ASCOLTO_CHANNEL_MIC],(size_t)count*sizeof(float)));
        snprintf(out->text,sizeof(out->text),"%s",asr_calls==1?"Sì, sì, sì, sì, sì, poi tieni premuto L.":"Buongiorno.");
        snprintf(out->language,sizeof(out->language),"it");return 0;
    }
    snprintf(out->text,sizeof(out->text),"Synthetic remote statement %d.",asr_calls==1?1:2); snprintf(out->language,sizeof(out->language),"en");return 0;
}
int ascolto_whistle_transcribe(ascolto_whistle_t *w,const float *pcm,int n,const char *lang,const char *kw,bool words,ascolto_transcript_result_t *out) {
    (void)w;(void)pcm;(void)n;(void)lang;(void)kw;(void)words;(void)out;abort();
}
int ascolto_whistle_embed_frames(ascolto_whistle_t *w,const float *pcm,int n,float **out,int *frames) {
    (void)w;(void)pcm;(void)n;*out=NULL;*frames=0;return 0;
}
void ascolto_macos_capture_finish_session(ascolto_macos_capture_t *capture,uint64_t generation) { (void)capture;(void)generation; }
int ascolto_macos_default_mic_status(ascolto_macos_device_info_t *info) { (void)info;return -1; }
bool ascolto_macos_query_audio_apps(char *in,size_t icap,char *out,size_t ocap) { (void)in;(void)icap;(void)out;(void)ocap;return false; }
void ascolto_diagnostic(const char *format,...) { (void)format; }
void ascolto_deferred_wake(ascolto_deferred_worker_t *worker) { (void)worker; }
void ascolto_deferred_stop(ascolto_deferred_worker_t *worker) { (void)worker; }
void ascolto_archive_wait(void) {}
int ascolto_archive_finalize(const ascolto_config_t *config,const char *mic,const char *sys,const char *archive,const char *md) {
    (void)config;(void)mic;(void)sys;(void)archive;(void)md;archive_calls++;return -1;
}
static bool stop_replay(void *context) {
    (void)context;
    if(mutate_path&&!mutated&&asr_calls>0) {
        FILE *source=fopen(mutate_path,"r+b");CHECK(source);float value=.42f;CHECK(fwrite(&value,sizeof(value),1,source)==1);CHECK(!fclose(source));mutated=1;
    }
    return stop_after>0&&asr_calls>=stop_after;
}
static int scalar(ascolto_deferred_store_t *store,const char *query) {
    sqlite3_stmt *s=NULL;CHECK(!sqlite3_prepare_v2(ascolto_db_sqlite(ascolto_deferred_store_database(store)),query,-1,&s,NULL));CHECK(sqlite3_step(s)==SQLITE_ROW);int value=sqlite3_column_int(s,0);sqlite3_finalize(s);return value;
}
static void reset_engine(ascolto_live_app_t *app) { app->local_stt.context=(void*)1;snprintf(app->local_stt.id,sizeof(app->local_stt.id),"fixture-model"); }
static void capture_fixture(ascolto_live_app_t *app) {
    CHECK(!ascolto_live_app_start_session(app,true));
    float sys[ASCOLTO_FRAME_SAMPLES],mic[ASCOLTO_FRAME_SAMPLES]={0};
    for(int position=0;position<fixture_samples;position+=ASCOLTO_FRAME_SAMPLES) {
        int frame=position/ASCOLTO_FRAME_SAMPLES;
        int count=fixture_samples-position>ASCOLTO_FRAME_SAMPLES?ASCOLTO_FRAME_SAMPLES:fixture_samples-position;
        bool voiced=frame<50||(frame>=80&&frame<130);
        for(int i=0;i<ASCOLTO_FRAME_SAMPLES;i++) {
            sys[i]=voiced?.10f*sinf(2.f*3.14159265358979323846f*(frame<80?220.f:330.f)*(frame*ASCOLTO_FRAME_SAMPLES+i)/ASCOLTO_SAMPLE_RATE):0;
            mic[i]=authority_fixture?.001f*cosf((float)(frame*ASCOLTO_FRAME_SAMPLES+i)/12):0;
        }
        ascolto_live_app_feed_audio(app,ASCOLTO_CHANNEL_SYS,sys,count);
        ascolto_live_app_feed_audio(app,ASCOLTO_CHANNEL_MIC,mic,count);
    }
    CHECK(asr_calls==0&&begin_calls==0);CHECK(!ascolto_live_app_stop_session(app));CHECK(!app->files.storage_error);
}
static void claim(ascolto_deferred_store_t *store,ascolto_deferred_job_t *job) { CHECK(!ascolto_deferred_claim_next(store,job)); }
static int64_t bytes(const char *path) { struct stat info;CHECK(!stat(path,&info));return info.st_size; }
static char *read_text(const char *path) { FILE *f=fopen(path,"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long size=ftell(f);CHECK(size>=0);rewind(f);char *text=calloc((size_t)size+1,1);CHECK(text);CHECK(fread(text,1,(size_t)size,f)==(size_t)size);fclose(f);return text; }
static void *run_pipeline(void *context) {
    (void)context;
    char directory[]="/tmp/ascolto-deferred-pipeline-XXXXXX";CHECK(mkdtemp(directory));
    ascolto_live_app_t *capture=calloc(1,sizeof(*capture)),*replay=calloc(1,sizeof(*replay));CHECK(capture&&replay);
    pthread_mutex_init(&capture->lock,NULL);pthread_cond_init(&capture->session_cond,NULL);pthread_mutex_init(&capture->snapshot_lock,NULL);
    ascolto_config_init_defaults(&capture->config);capture->config.auto_trigger_mic=false;capture->config.save_audio=false;
    snprintf(capture->config.config_path,sizeof(capture->config.config_path),"%s/config.json",directory);snprintf(capture->config.output_dir,sizeof(capture->config.output_dir),"%s/calls",directory);
    capture->deferred_worker=(void*)1;capture->local_models=(void*)1;capture->state=ASCOLTO_STATE_IDLE;
    CHECK(!ascolto_audio_chunker_init(&capture->mic_chunks,ASCOLTO_CHANNEL_MIC));CHECK(!ascolto_audio_chunker_init(&capture->sys_chunks,ASCOLTO_CHANNEL_SYS));
    ascolto_whistle_t whistle={.loaded=true};replay->whistle=&whistle;replay->config=capture->config;replay->local_models=(void*)1;reset_engine(replay);
    ascolto_deferred_store_t *store=NULL;CHECK(!ascolto_deferred_store_open(capture->config.config_path,&store));CHECK(!ascolto_deferred_worker_start(store));
    capture_fixture(capture);ascolto_deferred_job_t job={0};claim(store,&job);
    CHECK(job.sample_count==170*ASCOLTO_FRAME_SAMPLES);CHECK(!strcmp(job.model_id,"fixture-model"));CHECK(scalar(store,"SELECT count(*) FROM utterances")==0);
    int64_t source_size=bytes(job.sys_path);char *original=read_text(job.sys_path);stop_after=1;
    CHECK(ascolto_deferred_process_job(replay,store,&job,stop_replay,NULL)==1);CHECK(asr_calls==1);CHECK(scalar(store,"SELECT count(*) FROM native_transcription_segments")==1);CHECK(scalar(store,"SELECT count(*) FROM utterances")==1);CHECK(bytes(job.sys_path)==source_size);char *retained=read_text(job.sys_path);CHECK(!memcmp(original,retained,(size_t)source_size));free(retained);
    CHECK(!ascolto_deferred_defer(store,job.id));ascolto_deferred_job_clear(&job);claim(store,&job);stop_after=0;
    CHECK(!ascolto_deferred_process_job(replay,store,&job,stop_replay,NULL));CHECK(asr_calls==2);CHECK(scalar(store,"SELECT count(*) FROM utterances")==2);
    char *before=read_text(replay->files.md_path);CHECK(strstr(before,"Synthetic remote statement 1."));CHECK(strstr(before,"Synthetic remote statement 2."));
    /* Explicit retry after a publication/final-state failure must reproduce the
     * same exports from immutable utterance IDs without another model call. */
    CHECK(!ascolto_deferred_finish(store,job.id,"failed","fixture_publication_failure"));CHECK(!ascolto_deferred_retry(store,job.id));ascolto_deferred_job_clear(&job);claim(store,&job);
    CHECK(!ascolto_deferred_process_job(replay,store,&job,stop_replay,NULL));CHECK(asr_calls==2);char *after=read_text(replay->files.md_path);CHECK(!strcmp(before,after));free(before);free(after);
    CHECK(bytes(job.mic_path)==source_size);retained=read_text(job.sys_path);CHECK(!memcmp(original,retained,(size_t)source_size));free(original);free(retained);CHECK(!ascolto_deferred_finish(store,job.id,"completed",NULL));
    ascolto_deferred_finalize_job(replay,&job);CHECK(access(job.mic_path,F_OK)!=0&&access(job.sys_path,F_OK)!=0);ascolto_deferred_job_clear(&job);

    asr_calls=0;fail_call=2;capture->config.save_audio=true;capture_fixture(capture);claim(store,&job);
    CHECK(ascolto_deferred_process_job(replay,store,&job,stop_replay,NULL)==-1);CHECK(asr_calls==2);CHECK(bytes(job.sys_path)==source_size);
    CHECK(!ascolto_deferred_finish(store,job.id,"failed","fixture_asr_failure"));CHECK(!ascolto_deferred_retry(store,job.id));ascolto_deferred_job_clear(&job);claim(store,&job);fail_call=0;
    CHECK(!ascolto_deferred_process_job(replay,store,&job,stop_replay,NULL));CHECK(asr_calls==3);CHECK(scalar(store,"SELECT count(*) FROM utterances")==4);
    CHECK(!ascolto_deferred_finish(store,job.id,"completed",NULL));
    /* Compression committed a valid archive before a crash interrupted removal
     * of its two raw sources. Retry must trust the ready archive, retain its
     * exact bytes and remove the single remaining raw without recompression. */
    FILE *archive=fopen(replay->files.m4a_path,"wb");CHECK(archive);const char payload[]="Synthetic ready archive";CHECK(fwrite(payload,1,sizeof(payload),archive)==sizeof(payload));CHECK(!fclose(archive));
    ascolto_db_artifact_t artifact={.session_id=job.session_id,.kind="audio_m4a",.path=replay->files.m4a_path,.state="ready"};CHECK(!ascolto_db_artifact_put(ascolto_deferred_store_database(store),&artifact));
    CHECK(!unlink(job.mic_path));CHECK(access(job.sys_path,F_OK)==0);
    ascolto_deferred_finalize_job(replay,&job);CHECK(archive_calls==0);CHECK(access(job.sys_path,F_OK)!=0);char *saved_archive=read_text(replay->files.m4a_path);CHECK(!memcmp(saved_archive,payload,sizeof(payload)));free(saved_archive);
    ascolto_deferred_job_clear(&job);capture->config.save_audio=false;

    /* Changing a committed segment before retry invalidates its source digest. */
    asr_calls=0;capture_fixture(capture);claim(store,&job);stop_after=1;CHECK(ascolto_deferred_process_job(replay,store,&job,stop_replay,NULL)==1);stop_after=0;
    FILE *source=fopen(job.sys_path,"r+b");CHECK(source);CHECK(!fseek(source,1000*(long)sizeof(float),SEEK_SET));float changed=.5f;CHECK(fwrite(&changed,sizeof(changed),1,source)==1);fclose(source);
    CHECK(ascolto_deferred_process_job(replay,store,&job,stop_replay,NULL)==-1);CHECK(asr_calls==1);CHECK(bytes(job.sys_path)==source_size);CHECK(!ascolto_deferred_finish(store,job.id,"failed","fixture_source_changed"));ascolto_deferred_job_clear(&job);

    /* A source changing while a job is replayed also fails before completion. */
    asr_calls=0;capture_fixture(capture);claim(store,&job);mutate_path=job.sys_path;mutated=0;
    CHECK(ascolto_deferred_process_job(replay,store,&job,stop_replay,NULL)==-1);CHECK(mutated);CHECK(!ascolto_deferred_finish(store,job.id,"failed","fixture_source_changed"));mutate_path=NULL;ascolto_deferred_job_clear(&job);
    /* The application must neither mask a quiet concurrent MIC nor discard
     * model greetings/repetitions after successful recognition. */
    asr_calls=0;authority_fixture=true;capture_fixture(capture);claim(store,&job);
    expected_count=(int)job.sample_count;
    expected_pcm[ASCOLTO_CHANNEL_MIC]=(float*)read_text(job.mic_path);
    expected_pcm[ASCOLTO_CHANNEL_SYS]=(float*)read_text(job.sys_path);
    CHECK(!ascolto_deferred_process_job(replay,store,&job,stop_replay,NULL));CHECK(asr_calls==2);
    CHECK(scalar(store,"SELECT count(*) FROM utterances WHERE text='Buongiorno.'")==1);
    CHECK(scalar(store,"SELECT count(*) FROM utterances WHERE text='Sì, sì, sì, sì, sì, poi tieni premuto L.'")==1);
    CHECK(scalar(store,"SELECT count(*) FROM native_transcription_segments WHERE checkpoint_version=2")>0);
    /* An old filtered checkpoint must never masquerade as complete-audio ASR. */
    sqlite3_stmt *old_checkpoint=NULL;
    CHECK(!sqlite3_prepare_v2(ascolto_db_sqlite(ascolto_deferred_store_database(store)),
        "UPDATE native_transcription_segments SET checkpoint_version=1 WHERE job_id=?1",-1,&old_checkpoint,NULL));
    sqlite3_bind_int64(old_checkpoint,1,job.id);CHECK(sqlite3_step(old_checkpoint)==SQLITE_DONE);sqlite3_finalize(old_checkpoint);
    CHECK(ascolto_deferred_process_job(replay,store,&job,stop_replay,NULL)==-1);CHECK(asr_calls==2);
    CHECK(scalar(store,"SELECT count(*) FROM utterances WHERE text='Buongiorno.'")==1);
    CHECK(bytes(job.mic_path)==source_size&&bytes(job.sys_path)==source_size);
    CHECK(!ascolto_deferred_finish(store,job.id,"failed","fixture_old_checkpoint"));ascolto_deferred_job_clear(&job);
    free(expected_pcm[0]);free(expected_pcm[1]);authority_fixture=false;
    /* Cancellation at a full window must resume in SYS/MIC order, including
     * cached empty results and the final 13 samples, without another inference. */
    asr_calls=0;complete_fixture=true;fixture_samples=60*ASCOLTO_SAMPLE_RATE+13;
    capture_fixture(capture);claim(store,&job);expected_count=(int)job.sample_count;
    expected_pcm[0]=(float*)read_text(job.mic_path);expected_pcm[1]=(float*)read_text(job.sys_path);
    int previous_turns=scalar(store,"SELECT count(*) FROM utterances");
    int previous_segments=scalar(store,"SELECT count(*) FROM native_transcription_segments");
    stop_after=1;CHECK(ascolto_deferred_process_job(replay,store,&job,stop_replay,NULL)==1);CHECK(asr_calls==1);
    CHECK(scalar(store,"SELECT count(*) FROM native_transcription_segments")==previous_segments+1);
    CHECK(!ascolto_deferred_defer(store,job.id));ascolto_deferred_job_clear(&job);claim(store,&job);stop_after=0;
    CHECK(!ascolto_deferred_process_job(replay,store,&job,stop_replay,NULL));CHECK(asr_calls==6);
    CHECK(scalar(store,"SELECT count(*) FROM native_transcription_segments")==previous_segments+6);
    CHECK(scalar(store,"SELECT count(*) FROM utterances")==previous_turns);
    CHECK(!ascolto_deferred_process_job(replay,store,&job,stop_replay,NULL));CHECK(asr_calls==6);
    const char *benchmark=getenv("ASCOLTO_REPLAY_BENCHMARK");
    if(benchmark) {
        clock_t start=clock();
        for(int i=0;i<atoi(benchmark);i++)CHECK(!ascolto_deferred_process_job(replay,store,&job,stop_replay,NULL));
        printf("Cached replay CPU: %.6f seconds (%s repetitions, 60 seconds + 13 samples)\n",(double)(clock()-start)/CLOCKS_PER_SEC,benchmark);
    }
    CHECK(!ascolto_deferred_finish(store,job.id,"completed",NULL));ascolto_deferred_job_clear(&job);
    free(expected_pcm[0]);free(expected_pcm[1]);complete_fixture=false;
    ascolto_deferred_store_close(store);ascolto_audio_chunker_free(&capture->mic_chunks);ascolto_audio_chunker_free(&capture->sys_chunks);ascolto_audio_chunker_free(&replay->mic_chunks);ascolto_audio_chunker_free(&replay->sys_chunks);pthread_mutex_destroy(&capture->snapshot_lock);pthread_cond_destroy(&capture->session_cond);pthread_mutex_destroy(&capture->lock);free(capture);free(replay);
    printf("Deferred pipeline: %d checks passed\n",checks);return NULL;
}

int main(void) {
    pthread_attr_t attributes;
    pthread_t worker;
    CHECK(!pthread_attr_init(&attributes));
    CHECK(!pthread_attr_setstacksize(&attributes,512*1024));
    /* No CHECK after launch: the worker owns the shared counter until it joins. */
    if(pthread_create(&worker,&attributes,run_pipeline,NULL))abort();
    int joined=pthread_join(worker,NULL);
    pthread_attr_destroy(&attributes);
    return joined?1:0;
}
