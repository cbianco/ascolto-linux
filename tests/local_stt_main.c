#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ascolto/local_stt.h"
#include "ascolto/storage.h"
#include "ascolto/filesystem.h"
#include <sqlite3.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static int checks;
#define CHECK(condition) do { ++checks; if(!(condition)){fprintf(stderr,"FAIL %d: %s (line %d)\n",checks,#condition,__LINE__);exit(1);} } while(0)
static void write_file(const char *path,const char *text){FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite(text,1,strlen(text),f)==strlen(text));CHECK(!fclose(f));}
static void wait_worker(ascolto_local_models_t *m){for(int n=0;n<500;n++){ascolto_local_models_snapshot_t s;ascolto_local_models_snapshot(m,&s);bool busy=false;for(int i=1;i<ASCOLTO_LOCAL_MODEL_COUNT;i++)if(s.models[i].state==ASCOLTO_MODEL_CHECKING||s.models[i].state==ASCOLTO_MODEL_DOWNLOADING||s.models[i].state==ASCOLTO_MODEL_VERIFYING)busy=true;if(!busy)return;struct timespec t={0,10000000};nanosleep(&t,NULL);}CHECK(!"worker timed out");}
static void cloud_preferences(ascolto_db_t *db,char *out,size_t cap){sqlite3_stmt *s=NULL;CHECK(!sqlite3_prepare_v2(ascolto_db_sqlite(db),"SELECT enabled||':'||cloud_audio_opt_in||':'||coalesce(model_id,'') FROM transcription_preferences",-1,&s,NULL));CHECK(sqlite3_step(s)==SQLITE_ROW);snprintf(out,cap,"%s",sqlite3_column_text(s,0));sqlite3_finalize(s);}
typedef struct { ascolto_local_models_t *manager; ascolto_local_stt_t session; int result; } loading_t;
static void *begin_loading(void *argument){loading_t *load=argument;load->result=ascolto_local_stt_begin(load->manager,&load->session);return NULL;}
static void explicit_model_tests(void) {
    char temp[]="/tmp/ascolto-frozen-model-XXXXXX"; CHECK(mkdtemp(temp));
    char config[512], directory[768], path[1024], selected[40];
    snprintf(config,sizeof(config),"%s/config.json",temp);
    snprintf(directory,sizeof(directory),"%s/local-models/whisper-turbo-q5",temp); CHECK(!ascolto_mkdir_p(directory));
    snprintf(path,sizeof(path),"%s/model.bin",directory); write_file(path,"abc");
    ascolto_local_models_t *m=ascolto_local_models_create(config); CHECK(m); wait_worker(m);
    CHECK(!ascolto_local_models_select(m,"whistle"));
    ascolto_local_stt_t job={0}, competing={0}; ascolto_local_models_snapshot_t snapshot;
    CHECK(!ascolto_local_stt_begin_id(m,&job,"whisper-turbo-q5"));
    CHECK(!strcmp(job.id,"whisper-turbo-q5")&&job.context);
    ascolto_local_models_snapshot(m,&snapshot);
    CHECK(!strcmp(snapshot.selected,"whistle")&&!strcmp(snapshot.active,"whisper-turbo-q5"));
    CHECK(snapshot.models[0].selected&&!snapshot.models[1].selected&&snapshot.models[1].active);
    ascolto_db_t *db=NULL; CHECK(!ascolto_storage_open(config,&db));
    ascolto_storage_read_text(db,"local_transcription_model",selected,sizeof(selected));
    CHECK(!strcmp(selected,"whistle")); ascolto_db_close(db);
    CHECK(ascolto_local_stt_begin_id(m,&competing,"whisper-turbo-q5"));
    CHECK(ascolto_local_stt_begin_id(m,&competing,"whistle"));
    CHECK(ascolto_local_stt_begin(m,&competing)); CHECK(!competing.id[0]&&!competing.context);
    CHECK(ascolto_local_models_remove(m,"whisper-turbo-q5"));
    CHECK(ascolto_local_models_download(m,"whisper-turbo-q5"));
    /* A worker can retain this decoder across consecutive jobs without another
     * begin, including while the next-call preference changes. */
    float pcm[320]={0}; ascolto_stt_text_t text;
    for(int pass=0;pass<2;++pass) CHECK(!ascolto_local_stt_transcribe(m,&job,pcm,320,"it",NULL,&text));
    ascolto_local_stt_end(m,&job);
    CHECK(!ascolto_local_stt_begin(m,&competing)); CHECK(!strcmp(competing.id,"whistle")&&!competing.context);
    ascolto_local_stt_end(m,&competing);
    CHECK(ascolto_local_stt_begin_id(m,&job,"qwen3-asr-1.7b"));
    CHECK(ascolto_local_stt_begin_id(m,&job,"unknown"));
    CHECK(ascolto_local_stt_begin_id(m,&job,NULL));
    ascolto_local_models_snapshot(m,&snapshot);
    CHECK(!strcmp(snapshot.selected,"whistle")&&!snapshot.active[0]&&!snapshot.loading[0]);
    CHECK(strstr(snapshot.error,"Requested model is unavailable")); CHECK(!job.context&&!job.id[0]);
    write_file(path,"bad"); CHECK(ascolto_local_stt_begin_id(m,&job,"whisper-turbo-q5"));
    ascolto_local_models_snapshot(m,&snapshot);
    CHECK(snapshot.models[1].state==ASCOLTO_MODEL_ERROR&&!snapshot.active[0]&&!snapshot.loading[0]);
    CHECK(!strcmp(snapshot.selected,"whistle")&&strstr(snapshot.error,"Requested model failed verification"));
    ascolto_local_models_destroy(m);
}
static void verification_cache_tests(void) {
    char temp[]="/tmp/ascolto-verification-XXXXXX"; CHECK(mkdtemp(temp));
    char config[512], directory[768], path[1024], other[1100];
    snprintf(config,sizeof(config),"%s/config.json",temp);
    ascolto_db_t *db=NULL; CHECK(!ascolto_storage_open(config,&db));
    CHECK(!ascolto_storage_set(db,"local_transcription_model","nemotron-3.5-bf16-ssd")); ascolto_db_close(db);
    const char *models[]={"whisper-turbo-q5","nemotron-3.5-q8","nemotron-3.5-bf16-ssd"};
    for(size_t i=0;i<3;++i) {
        snprintf(directory,sizeof(directory),"%s/local-models/%s",temp,models[i]); CHECK(!ascolto_mkdir_p(directory));
        snprintf(path,sizeof(path),"%s/%s",directory,i?"decoder.gguf":"model.bin"); write_file(path,"abc");
        if(i){snprintf(path,sizeof(path),"%s/encoder.gguf",directory);write_file(path,"abc");}
    }
    ascolto_local_models_t *m=ascolto_local_models_create(config); CHECK(m); wait_worker(m);
    CHECK(ascolto_local_models_test_first_hash(m)==5);
    CHECK(ascolto_local_models_test_hash_count(m,"nemotron-3.5-bf16-ssd")==2);
    ascolto_local_stt_t session={0}; ascolto_local_models_snapshot_t snapshot;
    for(int pass=0;pass<3;++pass) {
        CHECK(!ascolto_local_stt_begin(m,&session));
        CHECK(ascolto_local_models_test_hash_count(m,"nemotron-3.5-bf16-ssd")==2);
        ascolto_local_models_snapshot(m,&snapshot); CHECK(!snapshot.loading[0]);
        ascolto_local_stt_end(m,&session);
    }
    // Every installed artifact participates, including the encoder rather than
    // only the first/decoder artifact. Equal-size corruption must be rehashed.
    snprintf(path,sizeof(path),"%s/local-models/nemotron-3.5-bf16-ssd/encoder.gguf",temp);
    write_file(path,"bad"); CHECK(ascolto_local_stt_begin(m,&session));
    CHECK(ascolto_local_models_test_hash_count(m,"nemotron-3.5-bf16-ssd")>2);
    ascolto_local_models_snapshot(m,&snapshot); CHECK(!snapshot.loading[0]&&!snapshot.active[0]);
    ascolto_local_models_destroy(m);
    for(int mode=0;mode<6;++mode) {
        char isolated[]="/tmp/ascolto-identity-XXXXXX"; CHECK(mkdtemp(isolated));
        snprintf(config,sizeof(config),"%s/config.json",isolated);
        CHECK(!ascolto_storage_open(config,&db)); CHECK(!ascolto_storage_set(db,"local_transcription_model","whisper-turbo-q5")); ascolto_db_close(db);
        snprintf(directory,sizeof(directory),"%s/local-models/whisper-turbo-q5",isolated); CHECK(!ascolto_mkdir_p(directory));
        snprintf(path,sizeof(path),"%s/model.bin",directory); write_file(path,"abc");
        m=ascolto_local_models_create(config); CHECK(m); wait_worker(m);
        CHECK(ascolto_local_models_test_hash_count(m,"whisper-turbo-q5")==1);
        CHECK(!ascolto_local_stt_begin(m,&session)); ascolto_local_stt_end(m,&session);
        CHECK(ascolto_local_models_test_hash_count(m,"whisper-turbo-q5")==1);
        struct stat original; CHECK(!stat(path,&original));
        if(mode==5) {
            char gate[1024], signal[1100]; snprintf(gate,sizeof(gate),"%s/load",isolated);
            CHECK(!setenv("ASCOLTO_STT_TEST_LOAD_GATE",gate,1));
            loading_t load={.manager=m,.result=0}; pthread_t thread; CHECK(!pthread_create(&thread,NULL,begin_loading,&load));
            snprintf(signal,sizeof(signal),"%s.reached",gate);
            int attempt=0; for(;attempt<5000&&access(signal,F_OK);++attempt){struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
            CHECK(attempt<5000); ascolto_local_models_snapshot(m,&snapshot);
            CHECK(!strcmp(snapshot.loading,"whisper-turbo-q5")&&!snapshot.active[0]&&strstr(snapshot.models[1].message,"Loading"));
            write_file(path,"bad"); snprintf(signal,sizeof(signal),"%s.resume",gate);write_file(signal,"resume");
            CHECK(!pthread_join(thread,NULL)); CHECK(!unsetenv("ASCOLTO_STT_TEST_LOAD_GATE"));
            CHECK(load.result&&!load.session.context&&!load.session.library);
            ascolto_local_models_snapshot(m,&snapshot);CHECK(!snapshot.loading[0]&&!snapshot.active[0]&&snapshot.models[1].state==ASCOLTO_MODEL_ERROR);
            ascolto_local_models_destroy(m); continue;
        } else if(mode==0) {
            write_file(path,"bad");
#ifdef __APPLE__
            struct timespec times[]={original.st_atimespec,original.st_mtimespec};
#else
            struct timespec times[]={original.st_atim,original.st_mtim};
#endif
            CHECK(!utimensat(AT_FDCWD,path,times,0));
        } else if(mode==1||mode==4) {
            snprintf(other,sizeof(other),"%s/replacement",directory); write_file(other,mode==4?"abc":"bad"); CHECK(!rename(other,path));
        } else if(mode==2) {
            snprintf(other,sizeof(other),"%s/saved.bin",directory); CHECK(!rename(path,other)); CHECK(!symlink(other,path));
        } else {
            snprintf(other,sizeof(other),"%s/moved",isolated); CHECK(!rename(directory,other)); CHECK(!symlink(other,directory));
        }
        if(mode==4) {
            CHECK(!ascolto_local_stt_begin(m,&session));ascolto_local_stt_end(m,&session);
            CHECK(ascolto_local_models_test_hash_count(m,"whisper-turbo-q5")==2);
            CHECK(!ascolto_local_stt_begin(m,&session));ascolto_local_stt_end(m,&session);
            CHECK(ascolto_local_models_test_hash_count(m,"whisper-turbo-q5")==2);
            ascolto_local_models_destroy(m);continue;
        }
        CHECK(ascolto_local_stt_begin(m,&session));
        ascolto_local_models_snapshot(m,&snapshot);
        CHECK(snapshot.models[1].state==ASCOLTO_MODEL_ERROR&&!snapshot.active[0]&&!snapshot.loading[0]);
        if(mode<2) CHECK(ascolto_local_models_test_hash_count(m,"whisper-turbo-q5")==2);
        else CHECK(ascolto_local_models_test_hash_count(m,"whisper-turbo-q5")==1);
        ascolto_local_models_destroy(m);
    }
}
int main(void){
    explicit_model_tests();
    verification_cache_tests();
    char temp[]="/tmp/ascolto-local-models-XXXXXX";CHECK(mkdtemp(temp));char config[512],directory[768],path[1024];snprintf(config,sizeof(config),"%s/config.json",temp);
    ascolto_db_t *db=NULL;CHECK(!ascolto_storage_open(config,&db));char before[256],after[256];cloud_preferences(db,before,sizeof(before));
    snprintf(directory,sizeof(directory),"%s/local-models/whisper-turbo-q5",temp);CHECK(!ascolto_mkdir_p(directory));snprintf(path,sizeof(path),"%s/model.bin",directory);write_file(path,"abc");
    snprintf(directory,sizeof(directory),"%s/local-models/parakeet-v3-q8",temp);CHECK(!ascolto_mkdir_p(directory));snprintf(path,sizeof(path),"%s/model.bin",directory);write_file(path,"bad");
    write_file("/tmp/ascolto-local-stt-download-fixture.bin","abc");
    ascolto_local_models_t *m=ascolto_local_models_create(config);CHECK(m);wait_worker(m);ascolto_local_models_snapshot_t s;ascolto_local_models_snapshot(m,&s);
    CHECK(!strcmp(s.selected,"whistle"));CHECK(s.models[0].state==ASCOLTO_MODEL_INCLUDED);CHECK(s.models[1].state==ASCOLTO_MODEL_READY);CHECK(s.models[2].state==ASCOLTO_MODEL_ERROR);CHECK(s.models[3].state==ASCOLTO_MODEL_MISSING);
    CHECK(ascolto_local_models_download(m,"nemotron-3.5-nvfp4"));
    CHECK(ascolto_local_models_select(m,"nemotron-3.5-nvfp4"));
    CHECK(ascolto_local_models_select(m,"unknown"));CHECK(ascolto_local_models_select(m,"qwen3-asr-1.7b"));CHECK(!ascolto_local_models_select(m,"whisper-turbo-q5"));
    ascolto_local_stt_t session={0};CHECK(!ascolto_local_stt_begin(m,&session));CHECK(!strcmp(session.id,"whisper-turbo-q5"));CHECK(!ascolto_local_models_select(m,"whistle"));ascolto_local_models_snapshot(m,&s);CHECK(!strcmp(s.active,"whisper-turbo-q5")&&!strcmp(s.selected,"whistle"));
    CHECK(ascolto_local_models_remove(m,"whisper-turbo-q5"));CHECK(ascolto_local_models_remove(m,"whistle"));CHECK(ascolto_local_models_download(m,"whisper-turbo-q5"));
    float pcm[320]={0};ascolto_stt_text_t text;CHECK(!ascolto_local_stt_transcribe(m,&session,pcm,320,"it",NULL,&text));CHECK(!strcmp(text.text,"Synthetic adapter result"));
    CHECK(ascolto_local_stt_transcribe(m,&session,pcm,320,"it","fail",&text));ascolto_local_models_snapshot(m,&s);CHECK(strstr(s.error,"Synthetic inference failure"));
    ascolto_local_stt_end(m,&session);CHECK(!session.id[0]);CHECK(!ascolto_local_models_remove(m,"whisper-turbo-q5"));
    CHECK(!ascolto_local_models_download(m,"whisper-turbo-q5"));wait_worker(m);ascolto_local_models_snapshot(m,&s);CHECK(s.models[1].state==ASCOLTO_MODEL_READY);CHECK(!strcmp(s.selected,"whistle"));CHECK(s.models[1].transferred==3);
    CHECK(!ascolto_local_models_download(m,"parakeet-v3-q8"));wait_worker(m);ascolto_local_models_snapshot(m,&s);CHECK(s.models[2].state==ASCOLTO_MODEL_READY);
    CHECK(!ascolto_local_models_download(m,"qwen3-asr-1.7b"));ascolto_local_models_cancel(m,"qwen3-asr-1.7b");wait_worker(m);ascolto_local_models_snapshot(m,&s);CHECK(!strcmp(s.selected,"whistle"));CHECK(s.models[3].state==ASCOLTO_MODEL_ERROR||s.models[3].state==ASCOLTO_MODEL_READY);
    CHECK(s.models[4].state==ASCOLTO_MODEL_MISSING);CHECK(s.models[4].bytes==6);
    CHECK(!ascolto_local_models_download(m,"nemotron-3.5-q8"));wait_worker(m);ascolto_local_models_snapshot(m,&s);
    CHECK(s.models[4].state==ASCOLTO_MODEL_READY);CHECK(s.models[4].transferred==6);CHECK(!strcmp(s.selected,"whistle"));
    CHECK(!ascolto_local_models_select(m,"nemotron-3.5-q8"));CHECK(!ascolto_local_stt_begin(m,&session));
    CHECK(!ascolto_local_stt_transcribe(m,&session,pcm,320,"it",NULL,&text));CHECK(text.text[0]);
    CHECK(!ascolto_local_models_select(m,"whistle"));CHECK(ascolto_local_models_remove(m,"nemotron-3.5-q8"));
    ascolto_local_stt_end(m,&session);
    snprintf(path,sizeof(path),"%s/local-models/nemotron-3.5-q8/encoder.gguf",temp);write_file(path,"bad");
    CHECK(!ascolto_local_models_select(m,"nemotron-3.5-q8"));CHECK(ascolto_local_stt_begin(m,&session));
    ascolto_local_models_snapshot(m,&s);CHECK(s.models[4].state==ASCOLTO_MODEL_ERROR);CHECK(!strcmp(s.selected,"nemotron-3.5-q8"));CHECK(!s.active[0]);
    CHECK(!ascolto_local_models_select(m,"whistle"));CHECK(!ascolto_local_models_remove(m,"nemotron-3.5-q8"));
    CHECK(!ascolto_local_models_download(m,"nemotron-3.5-q8"));wait_worker(m);ascolto_local_models_snapshot(m,&s);
    CHECK(s.models[4].state==ASCOLTO_MODEL_READY && s.models[4].bytes==6 && s.models[4].transferred==6);
    CHECK(!strcmp(s.selected,"whistle"));
    CHECK(!ascolto_local_stt_begin_id(m,&session,"nemotron-3.5-q8"));
    CHECK(!strcmp(session.id,"nemotron-3.5-q8"));
    CHECK(!ascolto_local_stt_transcribe(m,&session,pcm,320,"it",NULL,&text));
    CHECK(ascolto_local_models_remove(m,"nemotron-3.5-q8"));
    ascolto_local_stt_end(m,&session);
    CHECK(!ascolto_local_models_select(m,"nemotron-3.5-q8"));
    CHECK(ascolto_local_models_remove(m,"nemotron-3.5-q8"));
    CHECK(!ascolto_local_models_select(m,"whistle"));
    CHECK(!ascolto_local_models_remove(m,"nemotron-3.5-q8"));
    CHECK(!ascolto_local_models_select(m,"whisper-turbo-q5"));CHECK(!setenv("ASCOLTO_STT_TEST_BAD_ABI","1",1));CHECK(ascolto_local_stt_begin(m,&session));CHECK(!unsetenv("ASCOLTO_STT_TEST_BAD_ABI"));ascolto_local_models_snapshot(m,&s);CHECK(!strcmp(s.selected,"whisper-turbo-q5")&&!s.active[0]);snprintf(path,sizeof(path),"%s/local-models/whisper-turbo-q5/model.bin",temp);CHECK(!unlink(path));CHECK(ascolto_local_stt_begin(m,&session));ascolto_local_models_snapshot(m,&s);CHECK(!strcmp(s.selected,"whisper-turbo-q5")&&!s.active[0]);CHECK(s.models[1].state==ASCOLTO_MODEL_ERROR);CHECK(s.error[0]);CHECK(!ascolto_local_models_select(m,"whistle"));CHECK(!ascolto_local_stt_begin(m,&session));CHECK(!session.context);ascolto_local_stt_end(m,&session);
    cloud_preferences(db,after,sizeof(after));CHECK(!strcmp(before,after));
    ascolto_local_models_destroy(m);CHECK(!ascolto_storage_set(db,"local_transcription_model","unsupported-model"));m=ascolto_local_models_create(config);CHECK(m);wait_worker(m);ascolto_local_models_snapshot(m,&s);CHECK(!strcmp(s.selected,"unsupported-model"));CHECK(ascolto_local_stt_begin(m,&session));CHECK(!ascolto_local_models_select(m,"whistle"));
    ascolto_local_models_destroy(m);ascolto_db_close(db);unlink("/tmp/ascolto-local-stt-download-fixture.bin");
    printf("Local model manager: PASS (%d checks; synthetic artifacts, no network, microphone or cloud)\n",checks);return 0;
}
