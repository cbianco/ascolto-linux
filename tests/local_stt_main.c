#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ascolto/local_stt.h"
#include "ascolto/storage.h"
#include "ascolto/filesystem.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static int checks;
#define CHECK(condition) do { ++checks; if(!(condition)){fprintf(stderr,"FAIL %d: %s (line %d)\n",checks,#condition,__LINE__);exit(1);} } while(0)
static void write_file(const char *path,const char *text){FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite(text,1,strlen(text),f)==strlen(text));CHECK(!fclose(f));}
static void wait_worker(ascolto_local_models_t *m){for(int n=0;n<500;n++){ascolto_local_models_snapshot_t s;ascolto_local_models_snapshot(m,&s);bool busy=false;for(int i=1;i<4;i++)if(s.models[i].state==ASCOLTO_MODEL_CHECKING||s.models[i].state==ASCOLTO_MODEL_DOWNLOADING||s.models[i].state==ASCOLTO_MODEL_VERIFYING)busy=true;if(!busy)return;struct timespec t={0,10000000};nanosleep(&t,NULL);}CHECK(!"worker timed out");}
static void cloud_preferences(ascolto_db_t *db,char *out,size_t cap){sqlite3_stmt *s=NULL;CHECK(!sqlite3_prepare_v2(ascolto_db_sqlite(db),"SELECT enabled||':'||cloud_audio_opt_in||':'||coalesce(model_id,'') FROM transcription_preferences",-1,&s,NULL));CHECK(sqlite3_step(s)==SQLITE_ROW);snprintf(out,cap,"%s",sqlite3_column_text(s,0));sqlite3_finalize(s);}
int main(void){
    char temp[]="/tmp/ascolto-local-models-XXXXXX";CHECK(mkdtemp(temp));char config[512],directory[768],path[1024];snprintf(config,sizeof(config),"%s/config.json",temp);
    ascolto_db_t *db=NULL;CHECK(!ascolto_storage_open(config,&db));char before[256],after[256];cloud_preferences(db,before,sizeof(before));
    snprintf(directory,sizeof(directory),"%s/local-models/whisper-turbo-q5",temp);CHECK(!ascolto_mkdir_p(directory));snprintf(path,sizeof(path),"%s/model.bin",directory);write_file(path,"abc");
    snprintf(directory,sizeof(directory),"%s/local-models/parakeet-v3-q8",temp);CHECK(!ascolto_mkdir_p(directory));snprintf(path,sizeof(path),"%s/model.bin",directory);write_file(path,"bad");
    write_file("/tmp/ascolto-local-stt-download-fixture.bin","abc");
    ascolto_local_models_t *m=ascolto_local_models_create(config);CHECK(m);wait_worker(m);ascolto_local_models_snapshot_t s;ascolto_local_models_snapshot(m,&s);
    CHECK(!strcmp(s.selected,"whistle"));CHECK(s.models[0].state==ASCOLTO_MODEL_INCLUDED);CHECK(s.models[1].state==ASCOLTO_MODEL_READY);CHECK(s.models[2].state==ASCOLTO_MODEL_ERROR);CHECK(s.models[3].state==ASCOLTO_MODEL_MISSING);
    CHECK(ascolto_local_models_select(m,"unknown"));CHECK(ascolto_local_models_select(m,"qwen3-asr-1.7b"));CHECK(!ascolto_local_models_select(m,"whisper-turbo-q5"));
    ascolto_local_stt_t session={0};CHECK(!ascolto_local_stt_begin(m,&session));CHECK(!strcmp(session.id,"whisper-turbo-q5"));CHECK(!ascolto_local_models_select(m,"whistle"));ascolto_local_models_snapshot(m,&s);CHECK(!strcmp(s.active,"whisper-turbo-q5")&&!strcmp(s.selected,"whistle"));
    CHECK(ascolto_local_models_remove(m,"whisper-turbo-q5"));CHECK(ascolto_local_models_remove(m,"whistle"));CHECK(ascolto_local_models_download(m,"whisper-turbo-q5"));
    float pcm[320]={0};ascolto_stt_text_t text;CHECK(!ascolto_local_stt_transcribe(m,&session,pcm,320,"it",NULL,&text));CHECK(!strcmp(text.text,"Synthetic adapter result"));
    CHECK(ascolto_local_stt_transcribe(m,&session,pcm,320,"it","fail",&text));ascolto_local_models_snapshot(m,&s);CHECK(strstr(s.error,"Synthetic inference failure"));
    ascolto_local_stt_end(m,&session);CHECK(!session.id[0]);CHECK(!ascolto_local_models_remove(m,"whisper-turbo-q5"));
    CHECK(!ascolto_local_models_download(m,"whisper-turbo-q5"));wait_worker(m);ascolto_local_models_snapshot(m,&s);CHECK(s.models[1].state==ASCOLTO_MODEL_READY);CHECK(!strcmp(s.selected,"whistle"));CHECK(s.models[1].transferred==3);
    CHECK(!ascolto_local_models_download(m,"parakeet-v3-q8"));wait_worker(m);ascolto_local_models_snapshot(m,&s);CHECK(s.models[2].state==ASCOLTO_MODEL_READY);
    CHECK(!ascolto_local_models_download(m,"qwen3-asr-1.7b"));ascolto_local_models_cancel(m,"qwen3-asr-1.7b");wait_worker(m);ascolto_local_models_snapshot(m,&s);CHECK(!strcmp(s.selected,"whistle"));CHECK(s.models[3].state==ASCOLTO_MODEL_ERROR||s.models[3].state==ASCOLTO_MODEL_READY);
    CHECK(!ascolto_local_models_select(m,"whisper-turbo-q5"));CHECK(!setenv("ASCOLTO_STT_TEST_BAD_ABI","1",1));CHECK(ascolto_local_stt_begin(m,&session));CHECK(!unsetenv("ASCOLTO_STT_TEST_BAD_ABI"));ascolto_local_models_snapshot(m,&s);CHECK(!strcmp(s.selected,"whisper-turbo-q5")&&!s.active[0]);snprintf(path,sizeof(path),"%s/local-models/whisper-turbo-q5/model.bin",temp);CHECK(!unlink(path));CHECK(ascolto_local_stt_begin(m,&session));ascolto_local_models_snapshot(m,&s);CHECK(!strcmp(s.selected,"whisper-turbo-q5")&&!s.active[0]);CHECK(s.models[1].state==ASCOLTO_MODEL_ERROR);CHECK(s.error[0]);CHECK(!ascolto_local_models_select(m,"whistle"));CHECK(!ascolto_local_stt_begin(m,&session));CHECK(!session.context);ascolto_local_stt_end(m,&session);
    cloud_preferences(db,after,sizeof(after));CHECK(!strcmp(before,after));
    ascolto_local_models_destroy(m);CHECK(!ascolto_storage_set(db,"local_transcription_model","unsupported-model"));m=ascolto_local_models_create(config);CHECK(m);wait_worker(m);ascolto_local_models_snapshot(m,&s);CHECK(!strcmp(s.selected,"unsupported-model"));CHECK(ascolto_local_stt_begin(m,&session));CHECK(!ascolto_local_models_select(m,"whistle"));
    ascolto_local_models_destroy(m);ascolto_db_close(db);unlink("/tmp/ascolto-local-stt-download-fixture.bin");
    printf("Local model manager: PASS (%d checks; synthetic artifacts, no network, microphone or cloud)\n",checks);return 0;
}
