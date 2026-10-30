#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ascolto/deferred_store.h"
#include <assert.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int checks;
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"Failed at %d: %s\n",__LINE__,#x); abort(); } checks++; } while(0)
static int scalar(ascolto_deferred_store_t *s,const char *query) { sqlite3_stmt *st=NULL; CHECK(sqlite3_prepare_v2(ascolto_db_sqlite(ascolto_deferred_store_database(s)),query,-1,&st,NULL)==0); CHECK(sqlite3_step(st)==SQLITE_ROW); int n=sqlite3_column_int(st,0); sqlite3_finalize(st); return n; }
static void execute(ascolto_deferred_store_t *s,const char *query) { CHECK(sqlite3_exec(ascolto_db_sqlite(ascolto_deferred_store_database(s)),query,NULL,NULL,NULL)==0); }
static int64_t session(ascolto_deferred_store_t *s) { int64_t id=0; ascolto_db_session_t v={.started_at_ms=100,.capture_backend="fixture"}; CHECK(ascolto_db_session_create(ascolto_deferred_store_database(s),&v,&id)==0); return id; }
int main(void) {
    char dir[]="/tmp/ascolto-deferred-store-XXXXXX"; CHECK(mkdtemp(dir)); char config[1024]; snprintf(config,sizeof(config),"%s/config's-é.json",dir);
    ascolto_deferred_store_t *store=NULL,*other=NULL; CHECK(ascolto_deferred_store_open(config,&store)==0); CHECK(ascolto_deferred_store_open(config,&other)==0);
    ascolto_deferred_enqueue_t input={.session_id=session(store),.model_id="nemotron",.language="it",.keywords="One, l'été",.mic_path="/tmp/mic's-é.pcm",.sys_path="/tmp/sys.pcm",.metadata_json="{\"save_audio\":false}",.sample_count=32000,.diarizer_threshold=.6};
    int64_t id=0; CHECK(ascolto_deferred_record(store,&input,&id)==0); CHECK(ascolto_deferred_record(store,&input,NULL)!=0);
    input.session_id=session(store); int64_t failed_recording=0; CHECK(ascolto_deferred_record(other,&input,&failed_recording)==0); CHECK(ascolto_deferred_finish(other,failed_recording,"failed","audio_write_failed")==0); input.session_id=1;
    CHECK(ascolto_deferred_worker_start(store)==0); CHECK(ascolto_deferred_worker_start(other)==SQLITE_BUSY);
    ascolto_deferred_job_t job={0}; CHECK(ascolto_deferred_next_recording(store,&job)==0); CHECK(job.id==id&&!strcmp(job.state,"recording")&&!strcmp(job.keywords,input.keywords)); ascolto_deferred_job_clear(&job);
    CHECK(ascolto_deferred_seal(other,id,input.sample_count)==0); CHECK(ascolto_deferred_claim_next(other,&job)==SQLITE_MISUSE); CHECK(ascolto_deferred_claim_next(store,&job)==0); CHECK(job.id==id&&job.sample_count==32000&&!strcmp(job.metadata_json,input.metadata_json)); ascolto_deferred_job_clear(&job);
    unsigned char data[]={0,1,0,254}; ascolto_deferred_segment_t segment={.ordinal=0,.end_frame=16000,.channel="SYS",.start_ms=0,.end_ms=1000,.checkpoint_version=1,.checkpoint=data,.checkpoint_size=sizeof(data)};
    ascolto_db_utterance_t turn={.session_id=input.session_id,.channel="SYS",.start_ms=0,.end_ms=1000,.text="Persisted turn",.speaker_label="SPEAKER_1"};
    CHECK(ascolto_deferred_segment_commit(other,id,&segment,&turn)==SQLITE_MISUSE);
    CHECK(ascolto_deferred_segment_commit(store,id,&segment,&turn)==0); CHECK(ascolto_deferred_segment_commit(store,id,&segment,&turn)==0); CHECK(scalar(store,"SELECT count(*) FROM utterances")==1);
    data[1]=2; CHECK(ascolto_deferred_segment_commit(store,id,&segment,&turn)==SQLITE_CONSTRAINT); data[1]=1;
    segment.ordinal=2; CHECK(ascolto_deferred_segment_commit(store,id,&segment,&turn)==SQLITE_CONSTRAINT); segment.ordinal=1; turn.end_ms=-1; CHECK(ascolto_deferred_segment_commit(store,id,&segment,&turn)==SQLITE_CONSTRAINT); turn.end_ms=1000;
    CHECK(scalar(store,"SELECT count(*) FROM native_transcription_segments")==1);
    execute(store,"CREATE TRIGGER checkpoint_fault BEFORE UPDATE OF completed_segments ON native_transcription_jobs BEGIN SELECT RAISE(ABORT,'fixture_failure'); END");
    CHECK(ascolto_deferred_segment_commit(store,id,&segment,&turn)!=0); CHECK(scalar(store,"SELECT count(*) FROM utterances")==1); CHECK(scalar(store,"SELECT count(*) FROM native_transcription_segments")==1); execute(store,"DROP TRIGGER checkpoint_fault");
    ascolto_deferred_segment_t cached={0}; CHECK(ascolto_deferred_segment_get(store,id,0,&cached)==0); CHECK(cached.checkpoint_size==sizeof(data)&&!memcmp(cached.checkpoint,data,sizeof(data))); ascolto_deferred_segment_clear(&cached);
    CHECK(ascolto_deferred_progress(store,id,24000)==0); CHECK(ascolto_deferred_progress(store,id,100)==0); CHECK(ascolto_deferred_get(other,id,&job)==0); CHECK(job.processed_frames==24000&&job.completed_segments==1); ascolto_deferred_job_clear(&job);
    /* A terminated worker leaves its running job; only exclusive recovery requeues. */
    ascolto_deferred_store_close(store); store=NULL; CHECK(ascolto_deferred_worker_start(other)==0); CHECK(ascolto_deferred_get(other,id,&job)==0); CHECK(!strcmp(job.state,"queued")&&job.completed_segments==1); ascolto_deferred_job_clear(&job); CHECK(ascolto_deferred_claim_next(other,&job)==0); ascolto_deferred_job_clear(&job);
    CHECK(ascolto_deferred_segment_commit(other,id,&(ascolto_deferred_segment_t){.ordinal=0,.end_frame=16000,.channel="SYS",.start_ms=0,.end_ms=1000,.checkpoint_version=1,.checkpoint=data,.checkpoint_size=sizeof(data)},&turn)==0); CHECK(scalar(other,"SELECT count(*) FROM utterances")==1);
    CHECK(ascolto_deferred_cancel(other,id)==0); CHECK(ascolto_deferred_get(other,id,&job)==0); CHECK(job.cancel_requested); ascolto_deferred_job_clear(&job); CHECK(ascolto_deferred_finish(other,id,"completed",NULL)==0); CHECK(ascolto_deferred_get(other,id,&job)==0); CHECK(!strcmp(job.state,"cancelled")); ascolto_deferred_job_clear(&job);
    CHECK(ascolto_deferred_retry(other,id)==0); CHECK(ascolto_deferred_claim_next(other,&job)==0); ascolto_deferred_job_clear(&job); CHECK(ascolto_deferred_defer(other,id)==0); CHECK(ascolto_deferred_claim_next(other,&job)==0); ascolto_deferred_job_clear(&job); CHECK(ascolto_deferred_finish(other,id,"failed","contains text!")==SQLITE_MISUSE); CHECK(ascolto_deferred_finish(other,id,"failed","fixture_failure")==0); CHECK(ascolto_deferred_retry(other,id)==0); CHECK(ascolto_deferred_claim_next(other,&job)==0); ascolto_deferred_job_clear(&job); CHECK(ascolto_deferred_finish(other,id,"completed",NULL)==0); CHECK(ascolto_deferred_retry(other,id)==SQLITE_NOTFOUND);
    input.session_id=session(other); CHECK(ascolto_deferred_enqueue(other,&input,&id)==0); CHECK(ascolto_deferred_cancel(other,id)==0); CHECK(ascolto_deferred_get(other,id,&job)==0); CHECK(!strcmp(job.state,"cancelled")); ascolto_deferred_job_clear(&job);
    input.session_id=session(other); CHECK(ascolto_deferred_record(other,&input,&id)==0); CHECK(ascolto_deferred_finish(other,id,"failed","source_audio_missing")==0);
    CHECK(scalar(other,"SELECT count(*) FROM pragma_foreign_key_check")==0);
    /* Restore the exact version-one schema shape and verify non-destructive migration. */
    execute(other,"DROP TABLE native_transcription_segments; DROP TABLE native_transcription_jobs; DELETE FROM schema_migrations; INSERT INTO schema_migrations VALUES(1,0); PRAGMA user_version=1"); ascolto_deferred_store_close(other); other=NULL;
    CHECK(ascolto_deferred_store_open(config,&other)==0); CHECK(scalar(other,"PRAGMA user_version")==2); CHECK(scalar(other,"SELECT count(*) FROM sessions")==4); CHECK(scalar(other,"SELECT count(*) FROM utterances")==1); CHECK(scalar(other,"SELECT count(*) FROM schema_migrations")==2);
    char pattern[1200]; snprintf(pattern,sizeof(pattern),"%s/ascolto.sqlite3.schema-v1-backup-??????",dir); glob_t matches={0}; CHECK(glob(pattern,0,NULL,&matches)==0&&matches.gl_pathc==1); struct stat mode; CHECK(!stat(matches.gl_pathv[0],&mode)&&(mode.st_mode&0777)==0600); sqlite3 *backup=NULL; CHECK(sqlite3_open_v2(matches.gl_pathv[0],&backup,SQLITE_OPEN_READONLY,NULL)==0); sqlite3_stmt *stmt=NULL; CHECK(sqlite3_prepare_v2(backup,"PRAGMA user_version",-1,&stmt,NULL)==0); int backup_step=sqlite3_step(stmt); if(backup_step!=SQLITE_ROW) fprintf(stderr,"Backup read error %d: %s\n",backup_step,sqlite3_errmsg(backup)); CHECK(backup_step==SQLITE_ROW&&sqlite3_column_int(stmt,0)==1); sqlite3_finalize(stmt); sqlite3_close(backup); globfree(&matches);
    ascolto_deferred_store_close(other); printf("Deferred store: %d checks passed\n",checks); return 0;
}
