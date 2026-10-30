#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ascolto.h"
#include "ascolto/deferred.h"
#include "ascolto/deferred_store.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"Failed at %d: %s\n",__LINE__,#x); abort(); } checks++; } while (0)
static atomic_int loads, releases, processed, finalized, inference, entered, blocked, checking, fail_next;
static char config_path[1024];

/* Models and replay are deterministic fixtures. The real worker, lock, queue,
 * SQLite durability, lifecycle and copied UI snapshot remain in use. */
int ascolto_local_stt_begin_id(ascolto_local_models_t *models, ascolto_local_stt_t *stt, const char *id) {
    (void)models;
    /* The native manager rejects begin while any session is active. */
    if (stt->id[0]) return -1;
    if (!strcmp(id,"missing") || atomic_load(&checking)) return -1;
    if (strcmp(stt->id,id)) { atomic_fetch_add(&loads,1); snprintf(stt->id,sizeof(stt->id),"%s",id); }
    return 0;
}
void ascolto_local_stt_end(ascolto_local_models_t *models, ascolto_local_stt_t *stt) {
    (void)models; if (stt->id[0]) atomic_fetch_add(&releases,1); memset(stt,0,sizeof(*stt));
}
void ascolto_local_models_snapshot(ascolto_local_models_t *models, ascolto_local_models_snapshot_t *out) {
    (void)models; memset(out,0,sizeof(*out)); snprintf(out->models[0].id,sizeof(out->models[0].id),"model-a");
    out->models[0].state=atomic_load(&checking)?ASCOLTO_MODEL_CHECKING:ASCOLTO_MODEL_READY;
}
void ascolto_live_app_read_snapshot(ascolto_live_app_t *app, ascolto_live_snapshot_t *out) {
    pthread_mutex_lock(&app->snapshot_lock); *out=app->snapshot; pthread_mutex_unlock(&app->snapshot_lock);
}
void ascolto_online_vad_free(ascolto_online_vad_t *vad) { free(vad->buffer); memset(vad,0,sizeof(*vad)); }
int ascolto_deferred_process_job(ascolto_live_app_t *app, ascolto_deferred_store_t *store,
    const ascolto_deferred_job_t *job, bool (*stop)(void *), void *context) {
    (void)app; atomic_fetch_add(&entered,1);
    if (stop(context)) return 1;
    if (atomic_exchange(&fail_next,0)) return -1;
    ascolto_deferred_segment_t cached={0};
    if (ascolto_deferred_segment_get(store,job->id,0,&cached)==SQLITE_NOTFOUND) {
        unsigned checkpoint=42;
        ascolto_deferred_segment_t segment={.ordinal=0,.end_frame=job->sample_count,.channel="SYS",.checkpoint_version=1,.checkpoint=&checkpoint,.checkpoint_size=sizeof(checkpoint)};
        if (ascolto_deferred_segment_commit(store,job->id,&segment,NULL)) return -1;
        atomic_fetch_add(&inference,1);
    }
    ascolto_deferred_segment_clear(&cached);
    while (atomic_load(&blocked)) { if (stop(context)) return 1; usleep(1000); }
    if (stop(context)) return 1;
    atomic_fetch_add(&processed,1); return 0;
}
void ascolto_deferred_finalize_job(ascolto_live_app_t *app,const ascolto_deferred_job_t *job) {
    (void)app; ascolto_deferred_store_t *store=NULL;
    if (ascolto_deferred_store_open(config_path,&store)) abort();
    ascolto_deferred_job_t saved={0};
    if (ascolto_deferred_get(store,job->id,&saved)||strcmp(saved.state,"completed")) abort();
    ascolto_deferred_job_clear(&saved); ascolto_deferred_store_close(store); atomic_fetch_add(&finalized,1);
}
static int64_t enqueue(ascolto_deferred_store_t *store,const char *model,int recording,const char *mic,const char *sys) {
    int64_t session=0,id=0;
    ascolto_db_session_t values={.started_at_ms=100,.capture_backend="fixture"};
    CHECK(!ascolto_db_session_create(ascolto_deferred_store_database(store),&values,&session));
    ascolto_deferred_enqueue_t job={.session_id=session,.model_id=model,.sample_count=320,.mic_path=mic,.sys_path=sys,.diarizer_threshold=.6};
    CHECK(!(recording?ascolto_deferred_record(store,&job,&id):ascolto_deferred_enqueue(store,&job,&id))); return id;
}
static bool state_is(ascolto_deferred_store_t *store,int64_t id,const char *state) {
    ascolto_deferred_job_t job={0}; int rc=ascolto_deferred_get(store,id,&job);
    bool match=!rc&&!strcmp(job.state,state); ascolto_deferred_job_clear(&job); return match;
}
static void wait_state(ascolto_deferred_store_t *store,int64_t id,const char *state) {
    for(int i=0;i<5000;i++) { if(state_is(store,id,state)) return; usleep(1000); } CHECK(false);
}
static void wait_count(atomic_int *counter,int count) {
    for(int i=0;i<5000;i++) { if(atomic_load(counter)>=count) return; usleep(1000); } CHECK(false);
}
static void capture(ascolto_live_app_t *app,bool active) {
    pthread_mutex_lock(&app->snapshot_lock); app->snapshot.state=active?ASCOLTO_STATE_RECORDING:ASCOLTO_STATE_IDLE; pthread_mutex_unlock(&app->snapshot_lock);
}
int main(void) {
    char directory[]="/tmp/ascolto-deferred-worker-XXXXXX"; CHECK(mkdtemp(directory));
    snprintf(config_path,sizeof(config_path),"%s/config.json",directory);
    ascolto_deferred_store_t *store=NULL; CHECK(!ascolto_deferred_store_open(config_path,&store));
    ascolto_live_app_t *app=calloc(1,sizeof(*app)); CHECK(app); pthread_mutex_init(&app->snapshot_lock,NULL);
    snprintf(app->config.config_path,sizeof(app->config.config_path),"%s",config_path);
    capture(app,true);
    int64_t first=enqueue(store,"model-a",0,"/missing/mic","/missing/sys");
    int64_t second=enqueue(store,"model-a",0,"/missing/mic","/missing/sys");
    int64_t third=enqueue(store,"model-b",0,"/missing/mic","/missing/sys");
    int64_t missing=enqueue(store,"missing",0,"/missing/mic","/missing/sys");
    ascolto_deferred_worker_t *worker=ascolto_deferred_start(app); CHECK(worker);
    CHECK(ascolto_deferred_start(app)==NULL); /* Another processor already owns the durable lock. */
    usleep(30000); CHECK(atomic_load(&loads)==0); CHECK(state_is(store,first,"queued"));
    capture(app,false); ascolto_deferred_wake(worker);
    wait_state(store,missing,"failed"); wait_count(&finalized,3);
    CHECK(atomic_load(&loads)==2); CHECK(atomic_load(&releases)==2); CHECK(state_is(store,second,"completed")); CHECK(state_is(store,third,"completed"));
    ascolto_deferred_snapshot_t snapshot; ascolto_deferred_snapshot(worker,&snapshot); CHECK(snapshot.available);
    ascolto_deferred_stop(worker);

    /* A replay failure releases potentially poisoned engine state before the
     * next job, even when its frozen model choice is identical. */
    atomic_store(&fail_next,1); int before_failed_loads=atomic_load(&loads);
    int64_t failed=enqueue(store,"model-a",0,"/missing/mic","/missing/sys");
    int64_t after_failed=enqueue(store,"model-a",0,"/missing/mic","/missing/sys");
    worker=ascolto_deferred_start(app); CHECK(worker); wait_state(store,after_failed,"completed");
    CHECK(state_is(store,failed,"failed")); CHECK(atomic_load(&loads)==before_failed_loads+2); ascolto_deferred_stop(worker);

    /* Cancellation retains the committed segment. Explicit retry replays from
     * that checkpoint without redoing the simulated model call. */
    atomic_store(&blocked,1); int inference_before=atomic_load(&inference), entered_before=atomic_load(&entered);
    int64_t cancel=enqueue(store,"model-a",0,"/missing/mic","/missing/sys");
    worker=ascolto_deferred_start(app); CHECK(worker); wait_count(&entered,entered_before+1); wait_count(&inference,inference_before+1);
    CHECK(!ascolto_deferred_request_cancel(worker,cancel)); wait_state(store,cancel,"cancelled");
    atomic_store(&blocked,0); CHECK(!ascolto_deferred_request_retry(worker,cancel)); wait_state(store,cancel,"completed");
    CHECK(atomic_load(&inference)==inference_before+1); ascolto_deferred_stop(worker);

    /* Shutdown leaves work recoverable; the next exclusive owner continues
     * after the cached segment and performs finalization only after commit. */
    atomic_store(&blocked,1); entered_before=atomic_load(&entered); inference_before=atomic_load(&inference);
    int64_t interrupted=enqueue(store,"model-a",0,"/missing/mic","/missing/sys");
    worker=ascolto_deferred_start(app); CHECK(worker); wait_count(&entered,entered_before+1); wait_count(&inference,inference_before+1);
    ascolto_deferred_shutdown(worker); CHECK(state_is(store,interrupted,"running"));
    ascolto_deferred_snapshot(worker,&snapshot); CHECK(!snapshot.available&&!snapshot.processing&&!strcmp(snapshot.state,"stopped"));
    ascolto_deferred_shutdown(worker); /* Repeated lifecycle shutdown must not join twice. */
    ascolto_deferred_snapshot(worker,&snapshot); CHECK(!strcmp(snapshot.state,"stopped"));
    ascolto_deferred_stop(worker); /* Frees only after the retained UI snapshot is no longer needed. */
    atomic_store(&blocked,0); worker=ascolto_deferred_start(app); CHECK(worker); wait_state(store,interrupted,"completed");
    CHECK(atomic_load(&inference)==inference_before+1); ascolto_deferred_stop(worker);

    /* Verification waits preserve queue identity and never select a fallback. */
    atomic_store(&checking,1); int64_t verifying=enqueue(store,"model-a",0,"/missing/mic","/missing/sys");
    int loads_before=atomic_load(&loads); worker=ascolto_deferred_start(app); CHECK(worker); usleep(30000);
    CHECK(atomic_load(&loads)==loads_before); CHECK(!state_is(store,verifying,"failed"));
    atomic_store(&checking,0); ascolto_deferred_wake(worker); wait_state(store,verifying,"completed"); ascolto_deferred_stop(worker);

    /* Crash recovery preserves the longer existing raw channel without editing
     * either source; missing recordings become actionable failures. */
    char mic[1024],sys[1024]; snprintf(mic,sizeof(mic),"%s/mic.pcm",directory); snprintf(sys,sizeof(sys),"%s/sys.pcm",directory);
    FILE *file=fopen(mic,"wb"); CHECK(file); float pcm[640]={0}; CHECK(fwrite(pcm,sizeof(float),640,file)==640); fclose(file);
    file=fopen(sys,"wb"); CHECK(file); CHECK(fwrite(pcm,sizeof(float),320,file)==320); fclose(file);
    int64_t raw=enqueue(store,"model-a",1,mic,sys), absent=enqueue(store,"model-a",1,"/missing/mic","/missing/sys");
    worker=ascolto_deferred_start(app); CHECK(worker); wait_state(store,raw,"completed"); CHECK(state_is(store,absent,"failed"));
    ascolto_deferred_job_t recovered={0}; CHECK(!ascolto_deferred_get(store,raw,&recovered)); CHECK(recovered.sample_count==640); ascolto_deferred_job_clear(&recovered);
    /* A capture close can commit its session while queue sealing encounters a
     * transient busy writer. Recover that durable handoff without restarting. */
    capture(app,true);
    int64_t late=enqueue(store,"model-a",1,mic,sys);
    CHECK(!ascolto_deferred_get(store,late,&recovered));
    CHECK(!ascolto_db_session_state(ascolto_deferred_store_database(store),recovered.session_id,"completed",1000,"{}"));
    ascolto_deferred_job_clear(&recovered); ascolto_deferred_wake(worker); usleep(30000);
    CHECK(state_is(store,late,"recording")); /* Capture still has priority. */
    capture(app,false); ascolto_deferred_wake(worker); wait_state(store,late,"completed");

    /* A newly registered recording without a committed end timestamp must not
     * be swept, even during a brief idle source snapshot before capture starts. */
    int64_t live=enqueue(store,"model-a",1,mic,sys); ascolto_deferred_wake(worker); usleep(300000);
    CHECK(state_is(store,live,"recording")); CHECK(!ascolto_deferred_finish(store,live,"failed","fixture_cleanup"));

    /* Raw writes that failed during capture are terminal, never automatically
     * promoted into a transcription of the remaining partial PCM. */
    capture(app,true); int before_write_failure=atomic_load(&inference);
    int64_t write_failure=enqueue(store,"model-a",1,mic,sys);
    CHECK(!ascolto_deferred_get(store,write_failure,&recovered));
    CHECK(!ascolto_db_session_state(ascolto_deferred_store_database(store),recovered.session_id,"failed",1000,"{\"audio_write_failed\":true}"));
    ascolto_deferred_job_clear(&recovered); capture(app,false); ascolto_deferred_wake(worker);
    wait_state(store,write_failure,"failed"); CHECK(atomic_load(&inference)==before_write_failure);
    CHECK(!ascolto_deferred_get(store,write_failure,&recovered)); CHECK(!strcmp(recovered.error_code,"audio_write_failed")); ascolto_deferred_job_clear(&recovered);
    CHECK(access(mic,F_OK)==0&&access(sys,F_OK)==0);
    ascolto_deferred_stop(worker);
    file=fopen(sys,"rb"); CHECK(file); CHECK(!fseek(file,0,SEEK_END)); CHECK(ftell(file)==320*(long)sizeof(float)); fclose(file);
    pthread_mutex_destroy(&app->snapshot_lock); free(app); ascolto_deferred_store_close(store);
    printf("Deferred worker: %d checks passed\n",checks); return 0;
}
