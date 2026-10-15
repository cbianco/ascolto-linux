#define _POSIX_C_SOURCE 200809L
#include "ascolto/database.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct ascolto_db { sqlite3 *sql; char error[256]; };
static const char schema[] =
#include "database_schema.inc"
;
static void copy_error(char *out, size_t cap, const char *message) {
    if (out && cap) snprintf(out, cap, "%s", message);
}
static int remember(ascolto_db_t *db, int rc) {
    if (db) copy_error(db->error, sizeof(db->error), rc == SQLITE_OK ? "" : sqlite3_errmsg(db->sql));
    return rc;
}
static int exec(ascolto_db_t *db, const char *sql) { return remember(db, sqlite3_exec(db->sql, sql, NULL, NULL, NULL)); }
static int prepare(ascolto_db_t *db, const char *sql, sqlite3_stmt **s) { return remember(db, sqlite3_prepare_v2(db->sql, sql, -1, s, NULL)); }
static void text(sqlite3_stmt *s, int col, const char *v) { if(v) sqlite3_bind_text(s,col,v,-1,SQLITE_TRANSIENT); else sqlite3_bind_null(s,col); }
static void nullable_id(sqlite3_stmt *s, int col, int64_t v) { if(v>0) sqlite3_bind_int64(s,col,v); else sqlite3_bind_null(s,col); }
static int done(ascolto_db_t *db, sqlite3_stmt *s) {
    int rc=sqlite3_step(s); if(rc==SQLITE_DONE) rc=SQLITE_OK;
    remember(db,rc); sqlite3_finalize(s); return rc;
}
static int pragma(sqlite3 *db, const char *sql, int *value) {
    sqlite3_stmt *s=NULL; int rc=sqlite3_prepare_v2(db,sql,-1,&s,NULL);
    if(rc==SQLITE_OK) { rc=sqlite3_step(s); if(rc==SQLITE_ROW) { *value=sqlite3_column_int(s,0); rc=SQLITE_OK; } }
    sqlite3_finalize(s); return rc;
}
int64_t ascolto_db_now_ms(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); return (int64_t)ts.tv_sec*1000+ts.tv_nsec/1000000; }
int ascolto_db_open(const char *path, int timeout, ascolto_db_t **out, char *error, size_t cap) {
    if(out) *out=NULL;
    if(!path || !out || timeout<0 || timeout>10000) { copy_error(error,cap,"Invalid database arguments"); return SQLITE_MISUSE; }
    if(strcmp(path,":memory:")) {
        int fd=open(path,O_RDWR|O_CREAT|O_EXCL,0600);
        if(fd>=0) close(fd); else if(errno!=EEXIST) { copy_error(error,cap,"Cannot create database file"); return SQLITE_CANTOPEN; }
    }
    ascolto_db_t *db=calloc(1,sizeof(*db)); if(!db) return SQLITE_NOMEM;
    int rc=sqlite3_open_v2(path,&db->sql,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL);
    if(rc!=SQLITE_OK) goto failed;
    sqlite3_extended_result_codes(db->sql,1); sqlite3_busy_timeout(db->sql,timeout);
    int version=0, app=0, tables=0;
    if((rc=pragma(db->sql,"PRAGMA user_version",&version))!=SQLITE_OK) goto failed;
    if(version>ASCOLTO_DATABASE_VERSION) { copy_error(db->error,sizeof(db->error),"Database schema is newer than this application"); rc=SQLITE_SCHEMA; goto failed; }
    if((rc=pragma(db->sql,"PRAGMA application_id",&app))!=SQLITE_OK) goto failed;
    if((rc=pragma(db->sql,"SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'",&tables))!=SQLITE_OK) goto failed;
    if((version && app!=ASCOLTO_DATABASE_APPLICATION_ID) || (!version && (tables || app))) {
        copy_error(db->error,sizeof(db->error),"Unrecognized database; refusing to overwrite it"); rc=SQLITE_NOTADB; goto failed;
    }
    if((rc=exec(db,"PRAGMA foreign_keys=ON"))!=SQLITE_OK) goto failed;
    if((rc=exec(db,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL"))!=SQLITE_OK) goto failed;
    if(!version) {
        if((rc=exec(db,"BEGIN IMMEDIATE"))!=SQLITE_OK) goto failed;
        /* Another opener may have initialized the database while this connection waited. */
        if((rc=pragma(db->sql,"PRAGMA user_version",&version))!=SQLITE_OK) { exec(db,"ROLLBACK"); goto failed; }
        if(!version) {
            rc=exec(db,schema);
            if(rc==SQLITE_OK) rc=exec(db,"INSERT INTO schema_migrations VALUES(1,CAST(strftime('%s','now') AS INTEGER)*1000); PRAGMA application_id=1095975756; PRAGMA user_version=1");
        } else if(version!=ASCOLTO_DATABASE_VERSION) rc=SQLITE_SCHEMA;
        if(rc!=SQLITE_OK) { char saved[256]; copy_error(saved,sizeof(saved),db->error); exec(db,"ROLLBACK"); copy_error(db->error,sizeof(db->error),saved); goto failed; }
        if((rc=exec(db,"COMMIT"))!=SQLITE_OK) goto failed;
    }
    copy_error(error,cap,""); *out=db; return SQLITE_OK;
failed:
    copy_error(error,cap,db->error[0]?db->error:(db->sql?sqlite3_errmsg(db->sql):"Cannot open database"));
    if(db->sql) sqlite3_close(db->sql); free(db); return rc;
}
void ascolto_db_close(ascolto_db_t *db) { if(db) { sqlite3_close(db->sql); free(db); } }
sqlite3 *ascolto_db_sqlite(ascolto_db_t *db) { return db?db->sql:NULL; }
int ascolto_db_begin(ascolto_db_t *db) { return db?exec(db,"BEGIN IMMEDIATE"):SQLITE_MISUSE; }
int ascolto_db_commit(ascolto_db_t *db) { return db?exec(db,"COMMIT"):SQLITE_MISUSE; }
int ascolto_db_rollback(ascolto_db_t *db) { return db?exec(db,"ROLLBACK"):SQLITE_MISUSE; }
static int savepoint_end(ascolto_db_t *db, int rc) {
    if(rc==SQLITE_OK) { rc=exec(db,"RELEASE ascolto_write"); if(rc==SQLITE_OK) return rc; }
    char saved[256]; copy_error(saved,sizeof(saved),db->error);
    exec(db,"ROLLBACK TO ascolto_write; RELEASE ascolto_write"); copy_error(db->error,sizeof(db->error),saved); return rc;
}
static int queue_export(ascolto_db_t *db,int64_t session) {
    sqlite3_stmt *s=NULL; int rc=prepare(db,"INSERT INTO export_outbox(session_id,updated_at_ms) VALUES(?,?) ON CONFLICT(session_id) DO UPDATE SET revision=revision+1,updated_at_ms=excluded.updated_at_ms",&s);
    if(rc) return rc; sqlite3_bind_int64(s,1,session); sqlite3_bind_int64(s,2,ascolto_db_now_ms()); return done(db,s);
}
int ascolto_db_session_create(ascolto_db_t *db,const ascolto_db_session_t *v,int64_t *id) {
    if(!db||!v) return SQLITE_MISUSE; int rc=exec(db,"SAVEPOINT ascolto_write"); if(rc) return rc;
    sqlite3_stmt *s=NULL; rc=prepare(db,"INSERT INTO sessions(started_at_ms,state,capture_backend,call_title,meeting_title,source_app,vocabulary_snapshot_json,metadata_json) VALUES(?,'recording',?,?,?,?,?,?)",&s);
    if(!rc) { sqlite3_bind_int64(s,1,v->started_at_ms); text(s,2,v->capture_backend); text(s,3,v->call_title); text(s,4,v->meeting_title); text(s,5,v->source_app); text(s,6,v->vocabulary_snapshot_json?v->vocabulary_snapshot_json:"[]"); text(s,7,v->metadata_json?v->metadata_json:"{}"); rc=done(db,s); }
    int64_t row=sqlite3_last_insert_rowid(db->sql); if(!rc) rc=queue_export(db,row); rc=savepoint_end(db,rc); if(!rc&&id) *id=row; return rc;
}
int ascolto_db_session_state(ascolto_db_t *db,int64_t id,const char *state,int64_t ended,const char *metadata) {
    if(!db || ended<0) return SQLITE_MISUSE; int rc=exec(db,"SAVEPOINT ascolto_write"); if(rc) return rc;
    sqlite3_stmt *s=NULL; rc=prepare(db,"UPDATE sessions SET state=?,ended_at_ms=?,metadata_json=coalesce(?,metadata_json) WHERE id=?",&s);
    if(!rc) { text(s,1,state); nullable_id(s,2,ended); text(s,3,metadata); sqlite3_bind_int64(s,4,id); rc=done(db,s); if(!rc && sqlite3_changes(db->sql)!=1) rc=SQLITE_NOTFOUND; }
    if(!rc) rc=queue_export(db,id); return savepoint_end(db,rc);
}
int ascolto_db_utterance_append(ascolto_db_t *db,const ascolto_db_utterance_t *v) {
    if(!db||!v) return SQLITE_MISUSE; int rc=exec(db,"SAVEPOINT ascolto_write"); if(rc) return rc;
    sqlite3_stmt *s=NULL; int64_t speaker=0;
    if(v->speaker_label) {
        rc=prepare(db,"INSERT INTO speakers(session_id,channel,label) VALUES(?,?,?) ON CONFLICT(session_id,channel,label) DO NOTHING",&s);
        if(!rc) { sqlite3_bind_int64(s,1,v->session_id); text(s,2,v->channel); text(s,3,v->speaker_label); rc=done(db,s); }
        if(!rc) rc=prepare(db,"SELECT id FROM speakers WHERE session_id=? AND channel=? AND label=?",&s);
        if(!rc) { sqlite3_bind_int64(s,1,v->session_id); text(s,2,v->channel); text(s,3,v->speaker_label); int step=sqlite3_step(s); if(step==SQLITE_ROW) speaker=sqlite3_column_int64(s,0); else rc=step; sqlite3_finalize(s); }
    }
    if(!rc) rc=prepare(db,"INSERT INTO utterances(session_id,speaker_id,channel,start_ms,end_ms,text,language,source_app,metadata_json) VALUES(?,?,?,?,?,?,?,?,?)",&s);
    if(!rc) { sqlite3_bind_int64(s,1,v->session_id); nullable_id(s,2,speaker); text(s,3,v->channel); sqlite3_bind_int64(s,4,v->start_ms); sqlite3_bind_int64(s,5,v->end_ms); text(s,6,v->text); text(s,7,v->language); text(s,8,v->source_app); text(s,9,v->metadata_json?v->metadata_json:"{}"); rc=done(db,s); }
    if(!rc) rc=queue_export(db,v->session_id); return savepoint_end(db,rc);
}
int ascolto_db_artifact_put(ascolto_db_t *db,const ascolto_db_artifact_t *v) {
    if(!db||!v) return SQLITE_MISUSE; sqlite3_stmt *s=NULL;
    int rc=prepare(db,"INSERT INTO artifacts(session_id,kind,path,state,created_at_ms) VALUES(?,?,?,?,?) ON CONFLICT(path) DO UPDATE SET state=excluded.state,sha256=NULL WHERE artifacts.session_id=excluded.session_id AND artifacts.recap_job_id IS NULL AND artifacts.kind=excluded.kind",&s);
    if(rc) return rc; sqlite3_bind_int64(s,1,v->session_id); text(s,2,v->kind); text(s,3,v->path); text(s,4,v->state); sqlite3_bind_int64(s,5,v->created_at_ms); rc=done(db,s);
    return !rc && sqlite3_changes(db->sql)!=1 ? SQLITE_CONSTRAINT : rc;
}
static int known_setting(const char *key,int write) {
    static const char *keys[]={"platform","auto_monitoring","paused","transcription_language","capture_backend","mic_device_ref","sys_device_ref","save_audio","output_dir","settings_section","model_path","keywords_path","control_socket","recap_python","recap_tts","tts_language","diarizer_threshold","diarization_sensitivity","mic_debounce_ms","chatgpt_welcomed",NULL};
    if(!key) return 0; if(!write&&!strcmp(key,"oauth_host_id")) return 1;
    for(int i=0;keys[i];i++) if(!strcmp(key,keys[i])) return 1; return 0;
}
int ascolto_db_setting_get(ascolto_db_t *db,const char *key,char *out,size_t cap) {
    if(!db||!out||!cap||!known_setting(key,0)) return SQLITE_MISUSE;
    char query[128]; snprintf(query,sizeof(query),"SELECT %s FROM app_settings WHERE id=1",key); sqlite3_stmt *s=NULL; int rc=prepare(db,query,&s); if(rc) return rc;
    rc=sqlite3_step(s); if(rc==SQLITE_ROW) { const char *v=(const char*)sqlite3_column_text(s,0); if(v&&strlen(v)>=cap) rc=SQLITE_TOOBIG; else { copy_error(out,cap,v?v:""); rc=SQLITE_OK; } } sqlite3_finalize(s); return rc;
}
int ascolto_db_setting_set(ascolto_db_t *db,const char *key,const char *value) {
    if(!db||!known_setting(key,1)) return SQLITE_MISUSE; char query[128]; snprintf(query,sizeof(query),"UPDATE app_settings SET %s=? WHERE id=1",key); sqlite3_stmt *s=NULL; int rc=prepare(db,query,&s); if(rc) return rc; text(s,1,value); return done(db,s);
}
int ascolto_db_export_ack(ascolto_db_t *db,int64_t session,int64_t revision) {
    if(!db) return SQLITE_MISUSE; sqlite3_stmt *s=NULL; int rc=prepare(db,"DELETE FROM export_outbox WHERE session_id=? AND revision=?",&s); if(rc) return rc;
    sqlite3_bind_int64(s,1,session); sqlite3_bind_int64(s,2,revision); return done(db,s);
}
