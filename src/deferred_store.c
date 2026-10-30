#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ascolto/deferred_store.h"
#include "ascolto/storage.h"
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

struct ascolto_deferred_store { ascolto_db_t *db; int lock_fd; char owner[65]; };
static sqlite3 *sql(ascolto_deferred_store_t *s) { return s?ascolto_db_sqlite(s->db):NULL; }
static int prepare(ascolto_deferred_store_t *s,const char *query,sqlite3_stmt **out) { return s?sqlite3_prepare_v2(sql(s),query,-1,out,NULL):SQLITE_MISUSE; }
static void text(sqlite3_stmt *s,int index,const char *v) { if(v) sqlite3_bind_text(s,index,v,-1,SQLITE_TRANSIENT); else sqlite3_bind_null(s,index); }
static int done(sqlite3_stmt *s) { int rc=sqlite3_step(s); sqlite3_finalize(s); return rc==SQLITE_DONE?SQLITE_OK:rc; }
static int changed(ascolto_deferred_store_t *s,sqlite3_stmt *stmt) { int rc=done(stmt); return !rc&&sqlite3_changes(sql(s))!=1?SQLITE_NOTFOUND:rc; }
static int exec(ascolto_deferred_store_t *s,const char *query) { return s?sqlite3_exec(sql(s),query,NULL,NULL,NULL):SQLITE_MISUSE; }
static int transaction_end(ascolto_deferred_store_t *s,int rc) { if(!rc) { rc=exec(s,"RELEASE deferred_write"); if(!rc) return rc; } exec(s,"ROLLBACK TO deferred_write; RELEASE deferred_write"); return rc; }
ascolto_db_t *ascolto_deferred_store_database(ascolto_deferred_store_t *s) { return s?s->db:NULL; }
int ascolto_deferred_store_open(const char *config,ascolto_deferred_store_t **out) {
    if(!out||!config) return SQLITE_MISUSE; *out=NULL;
    ascolto_deferred_store_t *s=calloc(1,sizeof(*s)); if(!s) return SQLITE_NOMEM; s->lock_fd=-1;
    int rc=ascolto_storage_open(config,&s->db); if(rc) { free(s); return rc; } *out=s; return SQLITE_OK;
}
void ascolto_deferred_store_close(ascolto_deferred_store_t *s) { if(!s) return; ascolto_db_close(s->db); if(s->lock_fd>=0) close(s->lock_fd); free(s); }
void ascolto_deferred_job_clear(ascolto_deferred_job_t *v) {
    if(!v) return; free(v->state); free(v->model_id); free(v->language); free(v->keywords); free(v->mic_path); free(v->sys_path); free(v->metadata_json); free(v->error_code); memset(v,0,sizeof(*v));
}
static int copy_column(sqlite3_stmt *s,int col,char **out) { const char *p=(const char*)sqlite3_column_text(s,col); *out=p?strdup(p):NULL; return p&&!*out?SQLITE_NOMEM:SQLITE_OK; }
static const char job_columns[]="id,session_id,sample_count,completed_segments,processed_frames,queued_at_ms,started_at_ms,finished_at_ms,cancel_requested,diarizer_threshold,state,model_id,language,keywords,mic_path,sys_path,metadata_json,error_code";
static int read_job(sqlite3_stmt *stmt,ascolto_deferred_job_t *v) {
    memset(v,0,sizeof(*v)); int rc=sqlite3_step(stmt);
    if(rc!=SQLITE_ROW) { sqlite3_finalize(stmt); return rc==SQLITE_DONE?SQLITE_NOTFOUND:rc; }
    v->id=sqlite3_column_int64(stmt,0); v->session_id=sqlite3_column_int64(stmt,1); v->sample_count=sqlite3_column_int64(stmt,2); v->completed_segments=sqlite3_column_int64(stmt,3); v->processed_frames=sqlite3_column_int64(stmt,4);
    v->queued_at_ms=sqlite3_column_int64(stmt,5); v->started_at_ms=sqlite3_column_int64(stmt,6); v->finished_at_ms=sqlite3_column_int64(stmt,7); v->cancel_requested=sqlite3_column_int(stmt,8); v->diarizer_threshold=sqlite3_column_double(stmt,9);
    char **strings[]={&v->state,&v->model_id,&v->language,&v->keywords,&v->mic_path,&v->sys_path,&v->metadata_json,&v->error_code}; rc=SQLITE_OK;
    for(int i=0;i<8&&!rc;i++) rc=copy_column(stmt,10+i,strings[i]); sqlite3_finalize(stmt); if(rc) ascolto_deferred_job_clear(v); return rc;
}
static int job_query(ascolto_deferred_store_t *s,const char *where,int64_t id,ascolto_deferred_job_t *out) {
    if(!s||!out) return SQLITE_MISUSE; char query[1024]; snprintf(query,sizeof(query),"SELECT %s FROM native_transcription_jobs %s",job_columns,where); sqlite3_stmt *stmt=NULL; int rc=prepare(s,query,&stmt); if(rc) return rc; if(id>0) sqlite3_bind_int64(stmt,1,id); return read_job(stmt,out);
}
int ascolto_deferred_get(ascolto_deferred_store_t *s,int64_t id,ascolto_deferred_job_t *out) { if(id<=0) return SQLITE_MISUSE; return job_query(s,"WHERE id=?",id,out); }
int ascolto_deferred_latest(ascolto_deferred_store_t *s,ascolto_deferred_job_t *out) { return job_query(s,"ORDER BY CASE WHEN state='running' THEN 0 WHEN state='queued' THEN 1 WHEN state='recording' THEN 2 ELSE 3 END,id DESC LIMIT 1",0,out); }
int ascolto_deferred_next_recording(ascolto_deferred_store_t *s,ascolto_deferred_job_t *out) { if(!s||s->lock_fd<0) return SQLITE_MISUSE; return job_query(s,"WHERE state='recording' ORDER BY id LIMIT 1",0,out); }
static int insert_job(ascolto_deferred_store_t *s,const ascolto_deferred_enqueue_t *v,int recording,int64_t *id) {
    if(!s||!v||v->session_id<=0||v->sample_count<0||!v->model_id||!v->model_id[0]||!v->mic_path||v->mic_path[0]!='/'||!v->sys_path||v->sys_path[0]!='/'||!isfinite(v->diarizer_threshold)||v->diarizer_threshold<0||v->diarizer_threshold>1) return SQLITE_MISUSE;
    sqlite3_stmt *stmt=NULL; int rc=prepare(s,"INSERT INTO native_transcription_jobs(session_id,state,model_id,language,keywords,diarizer_threshold,mic_path,sys_path,metadata_json,sample_count,queued_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?)",&stmt); if(rc) return rc;
    sqlite3_bind_int64(stmt,1,v->session_id); text(stmt,2,recording?"recording":"queued"); text(stmt,3,v->model_id); text(stmt,4,v->language?v->language:"auto"); text(stmt,5,v->keywords?v->keywords:""); sqlite3_bind_double(stmt,6,v->diarizer_threshold); text(stmt,7,v->mic_path); text(stmt,8,v->sys_path); text(stmt,9,v->metadata_json?v->metadata_json:"{}"); sqlite3_bind_int64(stmt,10,recording?0:v->sample_count); sqlite3_bind_int64(stmt,11,ascolto_db_now_ms()); rc=done(stmt); if(!rc&&id) *id=sqlite3_last_insert_rowid(sql(s)); return rc;
}
int ascolto_deferred_record(ascolto_deferred_store_t *s,const ascolto_deferred_enqueue_t *v,int64_t *id) { return insert_job(s,v,1,id); }
int ascolto_deferred_enqueue(ascolto_deferred_store_t *s,const ascolto_deferred_enqueue_t *v,int64_t *id) { return insert_job(s,v,0,id); }
int ascolto_deferred_seal(ascolto_deferred_store_t *s,int64_t id,int64_t samples) {
    if(!s||id<=0||samples<0) return SQLITE_MISUSE; sqlite3_stmt *stmt=NULL; int rc=prepare(s,"UPDATE native_transcription_jobs SET state=CASE WHEN cancel_requested THEN 'cancelled' ELSE 'queued' END,sample_count=?,finished_at_ms=CASE WHEN cancel_requested THEN ? ELSE NULL END WHERE id=? AND state='recording'",&stmt); if(rc) return rc;
    sqlite3_bind_int64(stmt,1,samples); sqlite3_bind_int64(stmt,2,ascolto_db_now_ms()); sqlite3_bind_int64(stmt,3,id); return changed(s,stmt);
}
int ascolto_deferred_worker_start(ascolto_deferred_store_t *s) {
    if(!s) return SQLITE_MISUSE; if(s->lock_fd>=0) return SQLITE_OK;
    const char *path=sqlite3_db_filename(sql(s),"main"); if(!path||!path[0]) return SQLITE_CANTOPEN;
    size_t size=strlen(path)+40; char *lock=malloc(size); if(!lock) return SQLITE_NOMEM; snprintf(lock,size,"%s.native-transcription.lock",path);
    int fd=open(lock,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600); free(lock); if(fd<0) return SQLITE_CANTOPEN;
    if(flock(fd,LOCK_EX|LOCK_NB)) { close(fd); return SQLITE_BUSY; }
    unsigned char bytes[32]; sqlite3_randomness(sizeof(bytes),bytes); for(size_t i=0;i<sizeof(bytes);i++) snprintf(s->owner+2*i,3,"%02x",bytes[i]);
    int rc=exec(s,"SAVEPOINT deferred_write");
    if(!rc) rc=exec(s,"UPDATE native_transcription_jobs SET state=CASE WHEN cancel_requested THEN 'cancelled' ELSE 'queued' END,owner=NULL,finished_at_ms=CASE WHEN cancel_requested THEN CAST(strftime('%s','now') AS INTEGER)*1000 ELSE NULL END WHERE state='running'");
    if(!rc) rc=transaction_end(s,rc); else exec(s,"ROLLBACK TO deferred_write; RELEASE deferred_write");
    if(rc) { close(fd); s->owner[0]=0; return rc; } s->lock_fd=fd; return SQLITE_OK;
}
int ascolto_deferred_claim_next(ascolto_deferred_store_t *s,ascolto_deferred_job_t *out) {
    if(!s||s->lock_fd<0||!out) return SQLITE_MISUSE; memset(out,0,sizeof(*out)); int rc=exec(s,"SAVEPOINT deferred_write"); if(rc) return rc;
    sqlite3_stmt *stmt=NULL; rc=prepare(s,"UPDATE native_transcription_jobs SET state='running',owner=?,started_at_ms=coalesce(started_at_ms,?) WHERE id=(SELECT id FROM native_transcription_jobs WHERE state='queued' AND cancel_requested=0 ORDER BY id LIMIT 1) AND NOT EXISTS(SELECT 1 FROM native_transcription_jobs WHERE state='running')",&stmt);
    if(!rc) { text(stmt,1,s->owner); sqlite3_bind_int64(stmt,2,ascolto_db_now_ms()); rc=changed(s,stmt); }
    if(!rc) rc=job_query(s,"WHERE state='running'",0,out);
    rc=transaction_end(s,rc); if(rc) ascolto_deferred_job_clear(out); return rc;
}
int ascolto_deferred_defer(ascolto_deferred_store_t *s,int64_t id) {
    if(!s||s->lock_fd<0||id<=0) return SQLITE_MISUSE; sqlite3_stmt *stmt=NULL; int rc=prepare(s,"UPDATE native_transcription_jobs SET state=CASE WHEN cancel_requested THEN 'cancelled' ELSE 'queued' END,owner=NULL,finished_at_ms=CASE WHEN cancel_requested THEN ? ELSE NULL END WHERE id=? AND state='running' AND owner=?",&stmt); if(rc) return rc;
    sqlite3_bind_int64(stmt,1,ascolto_db_now_ms()); sqlite3_bind_int64(stmt,2,id); text(stmt,3,s->owner); return changed(s,stmt);
}
int ascolto_deferred_cancel(ascolto_deferred_store_t *s,int64_t id) {
    if(!s||id<=0) return SQLITE_MISUSE; sqlite3_stmt *stmt=NULL; int rc=prepare(s,"UPDATE native_transcription_jobs SET cancel_requested=1,state=CASE WHEN state='queued' THEN 'cancelled' ELSE state END,finished_at_ms=CASE WHEN state='queued' THEN ? ELSE finished_at_ms END WHERE id=? AND state IN ('recording','queued','running')",&stmt); if(rc) return rc;
    sqlite3_bind_int64(stmt,1,ascolto_db_now_ms()); sqlite3_bind_int64(stmt,2,id); return changed(s,stmt);
}
int ascolto_deferred_retry(ascolto_deferred_store_t *s,int64_t id) {
    if(!s||id<=0) return SQLITE_MISUSE; sqlite3_stmt *stmt=NULL; int rc=prepare(s,"UPDATE native_transcription_jobs SET state='queued',owner=NULL,cancel_requested=0,finished_at_ms=NULL,error_code=NULL WHERE id=? AND state IN ('failed','cancelled')",&stmt); if(rc) return rc; sqlite3_bind_int64(stmt,1,id); return changed(s,stmt);
}
void ascolto_deferred_segment_clear(ascolto_deferred_segment_t *v) { if(!v) return; free(v->channel); free(v->checkpoint); memset(v,0,sizeof(*v)); }
int ascolto_deferred_segment_get(ascolto_deferred_store_t *s,int64_t job,int64_t ordinal,ascolto_deferred_segment_t *out) {
    if(!s||job<=0||ordinal<0||!out) return SQLITE_MISUSE; memset(out,0,sizeof(*out)); sqlite3_stmt *stmt=NULL; int rc=prepare(s,"SELECT ordinal,end_frame,channel,start_ms,end_ms,checkpoint_version,checkpoint FROM native_transcription_segments WHERE job_id=? AND ordinal=?",&stmt); if(rc) return rc;
    sqlite3_bind_int64(stmt,1,job); sqlite3_bind_int64(stmt,2,ordinal); rc=sqlite3_step(stmt); if(rc==SQLITE_ROW) {
        out->ordinal=sqlite3_column_int64(stmt,0); out->end_frame=sqlite3_column_int64(stmt,1); out->start_ms=sqlite3_column_int64(stmt,3); out->end_ms=sqlite3_column_int64(stmt,4); out->checkpoint_version=(unsigned)sqlite3_column_int(stmt,5); rc=copy_column(stmt,2,&out->channel);
        int bytes=sqlite3_column_bytes(stmt,6); out->checkpoint_size=(size_t)bytes;
        if(!rc&&bytes>0) { out->checkpoint=malloc((size_t)bytes); if(!out->checkpoint) rc=SQLITE_NOMEM; else memcpy(out->checkpoint,sqlite3_column_blob(stmt,6),(size_t)bytes); }
    } else if(rc==SQLITE_DONE) rc=SQLITE_NOTFOUND; sqlite3_finalize(stmt); if(rc) ascolto_deferred_segment_clear(out); return rc;
}
static int owned(ascolto_deferred_store_t *s,int64_t id,int64_t *session,int64_t *segments,int64_t *frames) {
    if(!s||s->lock_fd<0) return SQLITE_MISUSE; sqlite3_stmt *stmt=NULL; int rc=prepare(s,"SELECT session_id,completed_segments,processed_frames FROM native_transcription_jobs WHERE id=? AND state='running' AND owner=?",&stmt); if(rc) return rc;
    sqlite3_bind_int64(stmt,1,id); text(stmt,2,s->owner); rc=sqlite3_step(stmt); if(rc==SQLITE_ROW) { if(session) *session=sqlite3_column_int64(stmt,0); if(segments) *segments=sqlite3_column_int64(stmt,1); if(frames) *frames=sqlite3_column_int64(stmt,2); rc=SQLITE_OK; } else if(rc==SQLITE_DONE) rc=SQLITE_NOTFOUND; sqlite3_finalize(stmt); return rc;
}
int ascolto_deferred_segment_commit(ascolto_deferred_store_t *s,int64_t id,const ascolto_deferred_segment_t *v,const ascolto_db_utterance_t *turn) {
    if(!s||!v||id<=0||v->ordinal<0||v->end_frame<0||!v->channel||(!v->checkpoint&&v->checkpoint_size)||v->checkpoint_size>INT_MAX||!v->checkpoint_version||v->checkpoint_version>INT_MAX) return SQLITE_MISUSE;
    int rc=exec(s,"SAVEPOINT deferred_write"); if(rc) return rc; int64_t session=0,count=0; rc=owned(s,id,&session,&count,NULL);
    if(!rc&&turn&&(turn->session_id!=session||strcmp(turn->channel?turn->channel:"",v->channel)||turn->start_ms!=v->start_ms||turn->end_ms!=v->end_ms)) rc=SQLITE_CONSTRAINT;
    if(!rc&&v->ordinal<count) {
        ascolto_deferred_segment_t old={0}; rc=ascolto_deferred_segment_get(s,id,v->ordinal,&old);
        if(!rc&&(old.end_frame!=v->end_frame||strcmp(old.channel,v->channel)||old.start_ms!=v->start_ms||old.end_ms!=v->end_ms||old.checkpoint_version!=v->checkpoint_version||old.checkpoint_size!=v->checkpoint_size||(v->checkpoint_size&&memcmp(old.checkpoint,v->checkpoint,v->checkpoint_size)))) rc=SQLITE_CONSTRAINT;
        ascolto_deferred_segment_clear(&old); return transaction_end(s,rc);
    }
    if(!rc&&v->ordinal!=count) rc=SQLITE_CONSTRAINT;
    sqlite3_stmt *stmt=NULL;
    if(!rc) rc=prepare(s,"INSERT INTO native_transcription_segments(job_id,ordinal,end_frame,channel,start_ms,end_ms,checkpoint_version,checkpoint) VALUES(?,?,?,?,?,?,?,?)",&stmt);
    if(!rc) { sqlite3_bind_int64(stmt,1,id); sqlite3_bind_int64(stmt,2,v->ordinal); sqlite3_bind_int64(stmt,3,v->end_frame); text(stmt,4,v->channel); sqlite3_bind_int64(stmt,5,v->start_ms); sqlite3_bind_int64(stmt,6,v->end_ms); sqlite3_bind_int(stmt,7,(int)v->checkpoint_version); if(v->checkpoint_size) sqlite3_bind_blob(stmt,8,v->checkpoint,(int)v->checkpoint_size,SQLITE_TRANSIENT); else sqlite3_bind_zeroblob(stmt,8,0); rc=done(stmt); }
    if(!rc&&turn) rc=ascolto_db_utterance_append(s->db,turn);
    if(!rc) rc=prepare(s,"UPDATE native_transcription_jobs SET completed_segments=completed_segments+1,processed_frames=max(processed_frames,?) WHERE id=?",&stmt);
    if(!rc) { sqlite3_bind_int64(stmt,1,v->end_frame); sqlite3_bind_int64(stmt,2,id); rc=changed(s,stmt); }
    return transaction_end(s,rc);
}
int ascolto_deferred_progress(ascolto_deferred_store_t *s,int64_t id,int64_t frames) {
    if(!s||s->lock_fd<0||id<=0||frames<0) return SQLITE_MISUSE; sqlite3_stmt *stmt=NULL; int rc=prepare(s,"UPDATE native_transcription_jobs SET processed_frames=max(processed_frames,?) WHERE id=? AND state='running' AND owner=?",&stmt); if(rc) return rc; sqlite3_bind_int64(stmt,1,frames); sqlite3_bind_int64(stmt,2,id); text(stmt,3,s->owner); return changed(s,stmt);
}
int ascolto_deferred_finish(ascolto_deferred_store_t *s,int64_t id,const char *state,const char *error) {
    if(!s||id<=0||!state||(strcmp(state,"completed")&&strcmp(state,"failed")&&strcmp(state,"cancelled"))) return SQLITE_MISUSE;
    if(s->lock_fd<0&&strcmp(state,"failed")) return SQLITE_MISUSE;
    if(error) { size_t n=strlen(error); if(!n||n>96) return SQLITE_MISUSE; for(size_t i=0;i<n;i++) if(!((error[i]>='a'&&error[i]<='z')||(error[i]>='0'&&error[i]<='9')||error[i]=='_')) return SQLITE_MISUSE; }
    sqlite3_stmt *stmt=NULL; int rc=prepare(s,"UPDATE native_transcription_jobs SET state=CASE WHEN cancel_requested THEN 'cancelled' ELSE ? END,owner=NULL,finished_at_ms=?,error_code=? WHERE id=? AND ((state='running' AND owner=?) OR (state='recording' AND ?<>'completed'))",&stmt); if(rc) return rc;
    text(stmt,1,state); sqlite3_bind_int64(stmt,2,ascolto_db_now_ms()); text(stmt,3,error); sqlite3_bind_int64(stmt,4,id); text(stmt,5,s->owner); text(stmt,6,state); return changed(s,stmt);
}
