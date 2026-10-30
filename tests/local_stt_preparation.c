#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ascolto/local_stt.h"
#include "ascolto/filesystem.h"
#include "ascolto/storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int checks;
static atomic_bool verification_gate, verification_reached, verification_resume;
void ascolto_stt_test_verify_gate(const char *name) {
    if (!atomic_load(&verification_gate) || strcmp(name,"decoder.gguf")) return;
    atomic_store(&verification_reached,true);
    while(!atomic_load(&verification_resume)){struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
}
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s at %d\n",checks,#x,__LINE__);exit(1); } } while(0)
static void file(const char *path, const char *text) { FILE *f=fopen(path,"wb");CHECK(f);CHECK(fputs(text,f)>=0);CHECK(!fclose(f)); }
static void wait_for(ascolto_local_models_t *manager, bool preparing) {
    for(int i=0;i<1000;++i) {
        ascolto_local_models_snapshot_t snapshot;ascolto_local_models_snapshot(manager,&snapshot);
        bool busy=false;
        for(int j=1;j<ASCOLTO_LOCAL_MODEL_COUNT;++j)if(snapshot.models[j].state==ASCOLTO_MODEL_CHECKING||snapshot.models[j].state==ASCOLTO_MODEL_DOWNLOADING||snapshot.models[j].state==ASCOLTO_MODEL_VERIFYING)busy=true;
        if(preparing ? strstr(snapshot.models[5].message,"Preparing native")!=NULL : !busy)return;
        struct timespec pause={0,10000000};nanosleep(&pause,NULL);
    }
    CHECK(!"Worker timeout");
}
int main(int argc, char **argv) {
    char temp[]="/tmp/ascolto-preparation-XXXXXX";CHECK(mkdtemp(temp));
    char config[512],root[768],stage[1024],installed[1024],path[1280],moved[1024];
    snprintf(config,sizeof(config),"%s/config.json",temp);snprintf(root,sizeof(root),"%s/local-models",temp);
    snprintf(stage,sizeof(stage),"%s/nemotron-3.5-bf16-ssd.download",root);
    snprintf(installed,sizeof(installed),"%s/nemotron-3.5-bf16-ssd",root);
    snprintf(moved,sizeof(moved),"%s/moved",temp);
    file("/tmp/ascolto-local-stt-download-fixture.bin","abc");
    ascolto_local_models_t *manager=ascolto_local_models_create(config);CHECK(manager);wait_for(manager,false);
    if(argc==2&&!strcmp(argv[1],"missing-symbol")){
        CHECK(!ascolto_local_models_download(manager,"nemotron-3.5-bf16-ssd"));wait_for(manager,false);
        ascolto_local_models_snapshot_t missing;ascolto_local_models_snapshot(manager,&missing);
        CHECK(missing.models[5].state==ASCOLTO_MODEL_ERROR);CHECK(access(installed,F_OK));CHECK(access(stage,F_OK));
        CHECK(strstr(missing.error,"preparation"));ascolto_local_models_destroy(manager);
        unlink("/tmp/ascolto-local-stt-download-fixture.bin");
        printf("Missing preparation ABI symbol: PASS (%d checks)\n",checks);return 0;
    }
    CHECK(!ascolto_local_models_download(manager,"nemotron-3.5-bf16-ssd"));wait_for(manager,false);
    ascolto_local_models_snapshot_t snapshot;ascolto_local_models_snapshot(manager,&snapshot);
    CHECK(snapshot.models[5].state==ASCOLTO_MODEL_READY);CHECK(snapshot.models[5].transferred==3);
    snprintf(path,sizeof(path),"%s/model.safetensors",installed);CHECK(access(path,F_OK));
    snprintf(path,sizeof(path),"%s/decoder.gguf",installed);CHECK(!access(path,F_OK));
    CHECK(!ascolto_local_models_select(manager,"nemotron-3.5-bf16-ssd"));
    ascolto_local_stt_t session={0};CHECK(!ascolto_local_stt_begin(manager,&session));ascolto_local_stt_end(manager,&session);
    CHECK(!ascolto_local_models_select(manager,"whistle"));
    ascolto_local_models_destroy(manager);
    manager=ascolto_local_models_create(config);CHECK(manager);wait_for(manager,false);ascolto_local_models_snapshot(manager,&snapshot);
    CHECK(snapshot.models[5].state==ASCOLTO_MODEL_READY);CHECK(!ascolto_local_models_remove(manager,"nemotron-3.5-bf16-ssd"));CHECK(access(installed,F_OK));
    const char *modes[]={"fail","corrupt","slow","replace"};
    for(size_t i=0;i<4;++i) {
        CHECK(!setenv("ASCOLTO_STT_TEST_PREPARE",modes[i],1));
        if(i==3){CHECK(!setenv("ASCOLTO_STT_TEST_REPLACE_FROM",stage,1));CHECK(!setenv("ASCOLTO_STT_TEST_REPLACE_TO",moved,1));}
        CHECK(!ascolto_local_models_download(manager,"nemotron-3.5-bf16-ssd"));
        if(i==2){wait_for(manager,true);ascolto_local_models_cancel(manager,"nemotron-3.5-bf16-ssd");}
        wait_for(manager,false);ascolto_local_models_snapshot(manager,&snapshot);
        CHECK(snapshot.models[5].state==ASCOLTO_MODEL_ERROR);CHECK(access(installed,F_OK));CHECK(!strcmp(snapshot.selected,"whistle"));
        if(i==3){snprintf(path,sizeof(path),"%s/untouched",stage);CHECK(!access(path,F_OK));CHECK(!unlink(path));CHECK(!rmdir(stage));}
        else CHECK(access(stage,F_OK));
    }
    CHECK(!unsetenv("ASCOLTO_STT_TEST_PREPARE"));
    atomic_store(&verification_gate,true);
    CHECK(!ascolto_local_models_download(manager,"nemotron-3.5-bf16-ssd"));
    for(int attempt=0;attempt<1000&&!atomic_load(&verification_reached);++attempt){struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
    CHECK(atomic_load(&verification_reached));
    snprintf(path,sizeof(path),"%s/model.safetensors",stage);CHECK(!access(path,F_OK));
    ascolto_local_models_cancel(manager,"nemotron-3.5-bf16-ssd");
    atomic_store(&verification_resume,true);atomic_store(&verification_gate,false);
    wait_for(manager,false);ascolto_local_models_snapshot(manager,&snapshot);
    CHECK(snapshot.models[5].state==ASCOLTO_MODEL_ERROR);CHECK(access(stage,F_OK));CHECK(access(installed,F_OK));
    /* Restart after a crash with partially written derived files. */
    ascolto_local_models_destroy(manager);CHECK(!ascolto_mkdir_p(stage));
    snprintf(path,sizeof(path),"%s/model.safetensors",stage);file(path,"abc");
    snprintf(path,sizeof(path),"%s/decoder.gguf.writing",stage);file(path,"partial");
    snprintf(path,sizeof(path),"%s/encoder.gguf",stage);file(path,"abc");
    manager=ascolto_local_models_create(config);CHECK(manager);wait_for(manager,false);CHECK(access(stage,F_OK));
    CHECK(!ascolto_local_models_download(manager,"nemotron-3.5-bf16-ssd"));wait_for(manager,false);ascolto_local_models_snapshot(manager,&snapshot);
    CHECK(snapshot.models[5].state==ASCOLTO_MODEL_READY);CHECK(!ascolto_local_models_remove(manager,"nemotron-3.5-bf16-ssd"));
    ascolto_local_models_destroy(manager);
    // Even an equal-byte rewrite with restored mtime during SHA invalidates
    // the result: ctime/fingerprint changed while the descriptor was read.
    for(int mutation=0;mutation<2;++mutation) {
        CHECK(!ascolto_mkdir_p(installed));
        snprintf(path,sizeof(path),"%s/decoder.gguf",installed);file(path,"abc");
        char encoder[1280];snprintf(encoder,sizeof(encoder),"%s/encoder.gguf",installed);file(encoder,"abc");
        struct stat original;CHECK(!stat(path,&original));
        ascolto_db_t *db=NULL;CHECK(!ascolto_storage_open(config,&db));CHECK(!ascolto_storage_set(db,"local_transcription_model","nemotron-3.5-bf16-ssd"));ascolto_db_close(db);
        atomic_store(&verification_reached,false);atomic_store(&verification_resume,false);atomic_store(&verification_gate,true);
        manager=ascolto_local_models_create(config);CHECK(manager);
        for(int attempt=0;attempt<5000&&!atomic_load(&verification_reached);++attempt){struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
        CHECK(atomic_load(&verification_reached));CHECK(ascolto_local_stt_begin(manager,&session));
        ascolto_local_models_snapshot(manager,&snapshot);CHECK(strstr(snapshot.error,"still being checked"));CHECK(snapshot.models[5].state==ASCOLTO_MODEL_CHECKING);
        CHECK(ascolto_local_stt_begin_id(manager,&session,"nemotron-3.5-bf16-ssd"));
        ascolto_local_models_snapshot(manager,&snapshot);
        CHECK(!strcmp(snapshot.selected,"nemotron-3.5-bf16-ssd")&&!snapshot.active[0]&&!snapshot.loading[0]);
        CHECK(strstr(snapshot.error,"still being checked"));
        CHECK(ascolto_local_stt_begin(manager,&session));
        if(!mutation) file(path,"abc");
#ifdef __APPLE__
        struct timespec times[]={original.st_atimespec,original.st_mtimespec};
#else
        struct timespec times[]={original.st_atim,original.st_mtim};
#endif
        if(!mutation) CHECK(!utimensat(AT_FDCWD,path,times,0));
        atomic_store(&verification_resume,true);atomic_store(&verification_gate,false);
        wait_for(manager,false);ascolto_local_models_snapshot(manager,&snapshot);
        CHECK(snapshot.models[5].state==(mutation?ASCOLTO_MODEL_READY:ASCOLTO_MODEL_ERROR));
        if(mutation) CHECK(!snapshot.error[0]);
        ascolto_local_models_destroy(manager);
    }
    unlink("/tmp/ascolto-local-stt-download-fixture.bin");
    printf("Native model preparation manager: PASS (%d checks; verified descriptor, commit, corruption, cancellation, replacement, restart)\n",checks);
    return 0;
}
