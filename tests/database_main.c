#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ascolto/database.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static int scalar(sqlite3 *db,const char *sql) { sqlite3_stmt *s=NULL; assert(sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK); assert(sqlite3_step(s)==SQLITE_ROW); int v=sqlite3_column_int(s,0); sqlite3_finalize(s); return v; }
static void sql(sqlite3 *db,const char *query) { char *error=NULL; int rc=sqlite3_exec(db,query,NULL,NULL,&error); if(rc) fprintf(stderr,"SQL failed: %s\n",error); sqlite3_free(error); assert(rc==SQLITE_OK); }
static int checks;
#define CHECK(v) do { assert(v); checks++; } while(0)

typedef struct { const char *path; int64_t session; int rc; } writer_t;
static void *write_turn(void *context) {
    writer_t *w=context; ascolto_db_t *db=NULL; char error[256];
    w->rc=ascolto_db_open(w->path,1500,&db,error,sizeof(error));
    if(w->rc) return NULL;
    ascolto_db_utterance_t u={.session_id=w->session,.channel="SYS",.speaker_label="SPEAKER_1",.text="Concurrent turn",.start_ms=3000,.end_ms=4000};
    w->rc=ascolto_db_utterance_append(db,&u); ascolto_db_close(db); return NULL;
}
int main(int argc,char **argv) {
    char directory[]="/tmp/ascolto-database-XXXXXX"; assert(mkdtemp(directory));
    char path[1024]; snprintf(path,sizeof(path),"%s/call's-é.sqlite3",directory);
    if(argc>1) snprintf(path,sizeof(path),"%s",argv[1]);
    ascolto_db_t *db=NULL; char error[256];
    CHECK(ascolto_db_open(path,1500,&db,error,sizeof(error))==SQLITE_OK);
    sqlite3 *native=ascolto_db_sqlite(db);
    CHECK(scalar(native,"PRAGMA foreign_keys")==1);
    CHECK(scalar(native,"PRAGMA user_version")==1);
    CHECK(scalar(native,"SELECT count(*) FROM schema_migrations")==1);
    struct stat mode; CHECK(!stat(path,&mode)&&(mode.st_mode&0777)==0600);
    ascolto_db_session_t s={.started_at_ms=100000,.capture_backend="coreaudio",.call_title="L'appel d'Élodie",.metadata_json="{\"manual\":true}"};
    int64_t a,b; CHECK(ascolto_db_session_create(db,&s,&a)==0); CHECK(ascolto_db_session_create(db,&s,&b)==0);
    ascolto_db_utterance_t u={.session_id=a,.channel="MIC",.speaker_label="ME",.text="È l'ora giusta",.start_ms=0,.end_ms=1000,.metadata_json="{\"ttft_ms\":123}"};
    CHECK(ascolto_db_utterance_append(db,&u)==0);
    CHECK(scalar(native,"SELECT json_extract(metadata_json,'$.manual') FROM sessions WHERE id=1")==1);
    CHECK(scalar(native,"SELECT json_extract(metadata_json,'$.ttft_ms') FROM utterances WHERE id=1")==123);
    CHECK(ascolto_db_begin(db)==0);
    u.speaker_label="BATCH_ROLLBACK";
    CHECK(ascolto_db_utterance_append(db,&u)==0);
    u.end_ms=-1;
    CHECK(ascolto_db_utterance_append(db,&u)!=0);
    CHECK(ascolto_db_rollback(db)==0);
    CHECK(scalar(native,"SELECT count(*) FROM speakers")==1);
    CHECK(scalar(native,"SELECT revision FROM export_outbox WHERE session_id=1")==2);
    u.start_ms=2000; u.end_ms=1000; u.speaker_label="ROLLBACK";
    CHECK(ascolto_db_utterance_append(db,&u)!=0);
    CHECK(scalar(native,"SELECT count(*) FROM speakers")==1);
    CHECK(scalar(native,"SELECT count(*) FROM utterances")==1);
    CHECK(sqlite3_exec(native,"INSERT INTO utterances(session_id,speaker_id,channel,start_ms,end_ms,text) VALUES(2,1,'MIC',0,1,'cross call')",NULL,NULL,NULL)!=SQLITE_OK);
    CHECK(sqlite3_exec(native,"INSERT INTO utterances(session_id,speaker_id,channel,start_ms,end_ms,text) VALUES(1,1,'SYS',0,1,'cross channel')",NULL,NULL,NULL)!=SQLITE_OK);
    CHECK(ascolto_db_begin(db)==0); CHECK(ascolto_db_setting_set(db,"output_dir","/tmp/rollback")==0); CHECK(ascolto_db_rollback(db)==0);
    char value[1024]; CHECK(ascolto_db_setting_get(db,"output_dir",value,sizeof(value))==0 && !value[0]);
    CHECK(ascolto_db_setting_set(db,"oauth_host_id","changed")==SQLITE_MISUSE);
    CHECK(ascolto_db_setting_set(db,"output_dir; DROP TABLE sessions;--","bad")==SQLITE_MISUSE);
    ascolto_db_artifact_t art={.session_id=a,.kind="audio_m4a",.path="/tmp/never-delete-audio.m4a",.state="ready",.created_at_ms=3000};
    CHECK(ascolto_db_artifact_put(db,&art)==0);
    art.session_id=b;
    CHECK(ascolto_db_artifact_put(db,&art)!=0);
    CHECK(scalar(native,"SELECT count(*) FROM artifacts")==1);
    CHECK(ascolto_db_begin(db)==0);
    writer_t writer={.path=path,.session=a}; pthread_t thread; assert(!pthread_create(&thread,NULL,write_turn,&writer));
    struct timespec delay={0,100000000}; nanosleep(&delay,NULL);
    CHECK(ascolto_db_commit(db)==0); pthread_join(thread,NULL); CHECK(writer.rc==0);
    ascolto_db_t *blocked=NULL; CHECK(ascolto_db_open(path,40,&blocked,error,sizeof(error))==0);
    CHECK(ascolto_db_begin(db)==0); int64_t before=ascolto_db_now_ms(); int rc=ascolto_db_begin(blocked);
    CHECK((rc&255)==SQLITE_BUSY && ascolto_db_now_ms()-before<1000); CHECK(ascolto_db_rollback(db)==0); ascolto_db_close(blocked);
    CHECK(ascolto_db_session_state(db,a,"completed",110000,"{\"duration_s\":3.8}")==0);
    CHECK(ascolto_db_session_state(db,a,"completed",110000,NULL)==0);
    CHECK(scalar(native,"SELECT json_extract(metadata_json,'$.duration_s')*1000 FROM sessions WHERE id=1")==3800);
    sql(native,"DELETE FROM sessions WHERE id=1"); CHECK(scalar(native,"SELECT count(*) FROM utterances")==0);
    CHECK(scalar(native,"SELECT count(*) FROM pragma_foreign_key_check")==0);
    ascolto_db_close(db);
    CHECK(ascolto_db_open(path,1500,&db,error,sizeof(error))==0); native=ascolto_db_sqlite(db);
    sql(native,"PRAGMA user_version=999"); ascolto_db_close(db);
    CHECK(ascolto_db_open(path,1500,&db,error,sizeof(error))==SQLITE_SCHEMA && strstr(error,"newer"));
    if(argc==1) { unlink(path); rmdir(directory); }
    printf("Database C: %d checks passed\n",checks); return 0;
}
