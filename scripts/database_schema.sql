-- Authoritative schema. Regenerate src/database_schema.inc with generate_database_schema.py.
-- All timestamps are UTC epoch milliseconds; utterance offsets are call-relative milliseconds.
CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY, applied_at_ms INTEGER NOT NULL CHECK(typeof(applied_at_ms)='integer' AND applied_at_ms >= 0));
CREATE TABLE providers(code TEXT PRIMARY KEY NOT NULL CHECK(code IN ('opencode','openrouter','openai')), display_name TEXT NOT NULL);
INSERT INTO providers VALUES('opencode','OpenCode'),('openrouter','OpenRouter'),('openai','OpenAI');
CREATE TABLE ai_connections(
 id INTEGER PRIMARY KEY, provider_code TEXT NOT NULL REFERENCES providers(code) ON DELETE CASCADE,
 profile_code TEXT NOT NULL, auth_kind TEXT NOT NULL CHECK(auth_kind IN ('api_key','chatgpt')),
 credential_ref TEXT UNIQUE, account_id TEXT UNIQUE, account_label TEXT, account_email TEXT, workspace_label TEXT, oauth_client_id TEXT,
 oauth_subject TEXT, workspace_id TEXT, verified_at_ms INTEGER CHECK(verified_at_ms IS NULL OR (typeof(verified_at_ms)='integer' AND verified_at_ms >= 0)),
 disconnected_at_ms INTEGER CHECK(disconnected_at_ms IS NULL OR (typeof(disconnected_at_ms)='integer' AND disconnected_at_ms >= 0)), welcome_pending INTEGER NOT NULL DEFAULT 0 CHECK(welcome_pending IN (0,1)),
 CHECK((provider_code='opencode' AND profile_code IN ('zen','go')) OR (provider_code<>'opencode' AND profile_code='default')),
 CHECK(auth_kind<>'chatgpt' OR provider_code='openai'),
 CHECK(auth_kind<>'chatgpt' OR oauth_subject IS NOT NULL));
CREATE UNIQUE INDEX api_key_identity ON ai_connections(provider_code,profile_code) WHERE auth_kind='api_key';
CREATE UNIQUE INDEX oauth_identity ON ai_connections(provider_code,profile_code,oauth_subject,coalesce(workspace_id,'')) WHERE auth_kind='chatgpt';
CREATE INDEX ai_connections_provider ON ai_connections(provider_code);
CREATE TABLE model_catalog(
 connection_id INTEGER NOT NULL REFERENCES ai_connections(id) ON DELETE CASCADE,
 model_id TEXT NOT NULL CHECK(length(model_id)>0), display_name TEXT NOT NULL,
 capabilities_json TEXT NOT NULL DEFAULT '{}', server_order INTEGER NOT NULL CHECK(typeof(server_order)='integer' AND server_order>=0),
 available INTEGER NOT NULL DEFAULT 1 CHECK(available IN (0,1)),
 refreshed_at_ms INTEGER NOT NULL CHECK(typeof(refreshed_at_ms)='integer' AND refreshed_at_ms>=0), PRIMARY KEY(connection_id,model_id));
CREATE TABLE recap_preferences(
 id INTEGER PRIMARY KEY CHECK(id=1), connection_id INTEGER, model_id TEXT,
 language TEXT NOT NULL DEFAULT 'Italian' CHECK(length(language)>0), auto_generate INTEGER NOT NULL DEFAULT 0 CHECK(auto_generate IN (0,1)),
 CHECK((connection_id IS NULL)=(model_id IS NULL)), CHECK(auto_generate=0 OR connection_id IS NOT NULL),
 FOREIGN KEY(connection_id,model_id) REFERENCES model_catalog(connection_id,model_id) ON DELETE RESTRICT);
INSERT INTO recap_preferences(id) VALUES(1);
CREATE INDEX recap_preferences_model ON recap_preferences(connection_id,model_id);
-- Clear both halves before deletion; SET NULL on an artifact FK would also clear its required session_id.
CREATE TRIGGER clear_deleted_model_preferences BEFORE DELETE ON model_catalog BEGIN
 UPDATE recap_preferences SET connection_id=NULL,model_id=NULL,auto_generate=0
 WHERE connection_id=OLD.connection_id AND model_id=OLD.model_id; END;
CREATE TRIGGER stop_disconnected_recaps AFTER UPDATE OF disconnected_at_ms ON ai_connections
 WHEN NEW.disconnected_at_ms IS NOT NULL BEGIN
 UPDATE recap_preferences SET auto_generate=0 WHERE connection_id=NEW.id; END;
CREATE TRIGGER stop_unavailable_model AFTER UPDATE OF available ON model_catalog
 WHEN NEW.available=0 BEGIN UPDATE recap_preferences SET auto_generate=0
 WHERE connection_id=NEW.connection_id AND model_id=NEW.model_id; END;
CREATE TRIGGER require_connected_recap BEFORE UPDATE ON recap_preferences
 WHEN NEW.auto_generate=1 AND NOT EXISTS(SELECT 1 FROM model_catalog m JOIN ai_connections c ON c.id=m.connection_id
 WHERE m.connection_id=NEW.connection_id AND m.model_id=NEW.model_id AND m.available=1 AND c.disconnected_at_ms IS NULL AND c.verified_at_ms IS NOT NULL)
 BEGIN SELECT RAISE(ABORT,'recap_connection_unavailable'); END;
CREATE TABLE app_settings(
 id INTEGER PRIMARY KEY CHECK(id=1), platform TEXT NOT NULL DEFAULT 'unknown' CHECK(platform IN ('macos','linux','unknown')),
 auto_monitoring INTEGER NOT NULL DEFAULT 1 CHECK(auto_monitoring IN (0,1)), paused INTEGER NOT NULL DEFAULT 0 CHECK(paused IN (0,1)),
 transcription_language TEXT NOT NULL DEFAULT 'auto', capture_backend TEXT NOT NULL DEFAULT '',
 mic_device_ref TEXT NOT NULL DEFAULT 'default', sys_device_ref TEXT NOT NULL DEFAULT 'default',
 save_audio INTEGER NOT NULL DEFAULT 0 CHECK(save_audio IN (0,1)), output_dir TEXT NOT NULL DEFAULT '',
 settings_section TEXT NOT NULL DEFAULT 'general', chatgpt_welcomed INTEGER NOT NULL DEFAULT 0 CHECK(chatgpt_welcomed IN (0,1)), oauth_host_id TEXT NOT NULL UNIQUE CHECK(length(oauth_host_id)>0),
 model_path TEXT NOT NULL DEFAULT '', keywords_path TEXT NOT NULL DEFAULT '', control_socket TEXT NOT NULL DEFAULT '',
 recap_python TEXT NOT NULL DEFAULT 'python3', recap_tts INTEGER NOT NULL DEFAULT 0 CHECK(recap_tts IN (0,1)),
 tts_language TEXT NOT NULL DEFAULT 'it', diarizer_threshold TEXT NOT NULL DEFAULT '0',
 diarization_sensitivity TEXT NOT NULL DEFAULT 'medium', mic_debounce_ms INTEGER NOT NULL DEFAULT 600 CHECK(mic_debounce_ms>=0));
INSERT INTO app_settings(id,oauth_host_id) VALUES(1,'urn:uuid:'||lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-4'||substr(lower(hex(randomblob(2))),2)||'-8'||substr(lower(hex(randomblob(2))),2)||'-'||lower(hex(randomblob(6))));
CREATE TABLE keywords(id INTEGER PRIMARY KEY, text TEXT NOT NULL UNIQUE CHECK(length(text)>0), enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1)));
CREATE TABLE sessions(
 id INTEGER PRIMARY KEY, started_at_ms INTEGER NOT NULL CHECK(typeof(started_at_ms)='integer' AND started_at_ms>=0), ended_at_ms INTEGER,
 state TEXT NOT NULL CHECK(state IN ('recording','paused','stopping','completed','failed')),
 call_title TEXT, meeting_title TEXT, source_app TEXT, vocabulary_snapshot_json TEXT NOT NULL DEFAULT '[]',
 capture_backend TEXT NOT NULL, legacy_source TEXT UNIQUE, metadata_json TEXT NOT NULL DEFAULT '{}',
 CHECK(ended_at_ms IS NULL OR (typeof(ended_at_ms)='integer' AND ended_at_ms>=started_at_ms)),
 CHECK(ended_at_ms IS NULL OR state IN ('completed','failed')));
CREATE INDEX sessions_started ON sessions(started_at_ms);
CREATE TABLE speakers(
 id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
 channel TEXT NOT NULL CHECK(channel IN ('MIC','SYS')), label TEXT NOT NULL CHECK(length(label)>0),
 UNIQUE(session_id,channel,label), UNIQUE(id,session_id), UNIQUE(id,session_id,channel));
CREATE TRIGGER retain_speaker_utterances BEFORE DELETE ON speakers BEGIN UPDATE utterances SET speaker_id=NULL WHERE speaker_id=OLD.id; END;
CREATE TABLE utterances(
 id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
 speaker_id INTEGER, channel TEXT NOT NULL CHECK(channel IN ('MIC','SYS')),
 start_ms INTEGER NOT NULL CHECK(typeof(start_ms)='integer' AND start_ms>=0), end_ms INTEGER NOT NULL CHECK(typeof(end_ms)='integer' AND end_ms>=start_ms),
 text TEXT NOT NULL, language TEXT, source_app TEXT, metadata_json TEXT NOT NULL DEFAULT '{}',
 FOREIGN KEY(speaker_id,session_id,channel) REFERENCES speakers(id,session_id,channel) ON DELETE RESTRICT);
CREATE INDEX utterances_session_start ON utterances(session_id,start_ms,id);
CREATE INDEX utterances_speaker ON utterances(speaker_id,session_id,channel);
CREATE TABLE recap_jobs(
 id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
 connection_id INTEGER REFERENCES ai_connections(id) ON DELETE SET NULL,
 provider_code_snapshot TEXT NOT NULL CHECK(provider_code_snapshot IN ('opencode','openrouter','openai')),
 model_id_snapshot TEXT NOT NULL CHECK(length(model_id_snapshot)>0), model_label_snapshot TEXT NOT NULL,
 language TEXT NOT NULL, input_hash TEXT NOT NULL CHECK(length(input_hash)=64 AND input_hash NOT GLOB '*[^0-9a-f]*'),
 phase TEXT NOT NULL DEFAULT 'final' CHECK(phase IN ('provisional','final')),
 task TEXT NOT NULL DEFAULT 'recap' CHECK(task IN ('recap','transcription')), attempt INTEGER NOT NULL CHECK(typeof(attempt)='integer' AND attempt>0), status TEXT NOT NULL CHECK(status IN ('queued','validating','running','completed','failed','cancelled')),
 queued_at_ms INTEGER NOT NULL CHECK(typeof(queued_at_ms)='integer' AND queued_at_ms>=0), started_at_ms INTEGER, finished_at_ms INTEGER, error_code TEXT,
 CHECK(started_at_ms IS NULL OR (typeof(started_at_ms)='integer' AND started_at_ms>=queued_at_ms)), CHECK(finished_at_ms IS NULL OR (typeof(finished_at_ms)='integer' AND finished_at_ms>=coalesce(started_at_ms,queued_at_ms))),
 CHECK((status IN ('completed','failed','cancelled'))=(finished_at_ms IS NOT NULL)),
 CHECK(status<>'running' OR started_at_ms IS NOT NULL),
 CHECK(error_code IS NULL OR (length(error_code) BETWEEN 1 AND 64 AND error_code NOT GLOB '*[^A-Za-z0-9_.-]*')),
 UNIQUE(id,session_id), UNIQUE(session_id,task,attempt));
CREATE INDEX recap_jobs_connection ON recap_jobs(connection_id);
CREATE TABLE artifacts(
 id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE, recap_job_id INTEGER,
 kind TEXT NOT NULL CHECK(kind IN ('transcript_md','transcript_jsonl','audio_m4a','audio_mic_flac','audio_sys_flac','recap_md','recap_json','recap_spoken','recap_mp3','cloud_md','cloud_jsonl')),
 path TEXT NOT NULL UNIQUE CHECK(length(path)>0), state TEXT NOT NULL CHECK(state IN ('pending','ready','failed')),
 created_at_ms INTEGER NOT NULL CHECK(typeof(created_at_ms)='integer' AND created_at_ms>=0), sha256 TEXT CHECK(sha256 IS NULL OR (length(sha256)=64 AND sha256 NOT GLOB '*[^0-9a-f]*')),
 FOREIGN KEY(recap_job_id,session_id) REFERENCES recap_jobs(id,session_id) ON DELETE RESTRICT);
CREATE INDEX artifacts_session ON artifacts(session_id);
CREATE INDEX artifacts_job ON artifacts(recap_job_id,session_id);
CREATE TRIGGER retain_job_artifacts BEFORE DELETE ON recap_jobs BEGIN UPDATE artifacts SET recap_job_id=NULL WHERE recap_job_id=OLD.id; END;
-- One durable export request per session. revision prevents a publisher from acknowledging a newer write.
CREATE TABLE export_outbox(session_id INTEGER PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,
 revision INTEGER NOT NULL DEFAULT 1 CHECK(typeof(revision)='integer' AND revision>0), updated_at_ms INTEGER NOT NULL CHECK(typeof(updated_at_ms)='integer' AND updated_at_ms>=0));
CREATE TABLE legacy_imports(source_path TEXT PRIMARY KEY NOT NULL, source_sha256 TEXT NOT NULL, imported_at_ms INTEGER NOT NULL CHECK(typeof(imported_at_ms)='integer' AND imported_at_ms>=0),
 session_id INTEGER REFERENCES sessions(id) ON DELETE SET NULL, backup_path TEXT, kind TEXT NOT NULL CHECK(kind IN ('config','keywords','session','ai_settings')));

-- Optional cloud transcription has its own selection and explicit audio opt-in. Whistle remains the default.
CREATE TABLE transcription_preferences(
 id INTEGER PRIMARY KEY CHECK(id=1), connection_id INTEGER, model_id TEXT, language TEXT NOT NULL DEFAULT 'auto',
 enabled INTEGER NOT NULL DEFAULT 0 CHECK(enabled IN (0,1)), cloud_audio_opt_in INTEGER NOT NULL DEFAULT 0 CHECK(cloud_audio_opt_in IN (0,1)),
 CHECK((connection_id IS NULL)=(model_id IS NULL)), CHECK(enabled=0 OR (connection_id IS NOT NULL AND cloud_audio_opt_in=1)),
 FOREIGN KEY(connection_id,model_id) REFERENCES model_catalog(connection_id,model_id) ON DELETE RESTRICT);
INSERT INTO transcription_preferences(id) VALUES(1);
CREATE INDEX transcription_preferences_model ON transcription_preferences(connection_id,model_id);
CREATE TRIGGER clear_deleted_transcription_model BEFORE DELETE ON model_catalog BEGIN
 UPDATE transcription_preferences SET connection_id=NULL,model_id=NULL,enabled=0,cloud_audio_opt_in=0
 WHERE connection_id=OLD.connection_id AND model_id=OLD.model_id; END;
CREATE TRIGGER stop_disconnected_transcription AFTER UPDATE OF disconnected_at_ms ON ai_connections WHEN NEW.disconnected_at_ms IS NOT NULL BEGIN
 UPDATE transcription_preferences SET enabled=0 WHERE connection_id=NEW.id; END;
CREATE TRIGGER stop_unavailable_transcription AFTER UPDATE OF available ON model_catalog WHEN NEW.available=0 BEGIN
 UPDATE transcription_preferences SET enabled=0 WHERE connection_id=NEW.connection_id AND model_id=NEW.model_id; END;
CREATE TRIGGER require_connected_transcription BEFORE UPDATE ON transcription_preferences
 WHEN NEW.enabled=1 AND NOT EXISTS(SELECT 1 FROM model_catalog m JOIN ai_connections c ON c.id=m.connection_id
 WHERE m.connection_id=NEW.connection_id AND m.model_id=NEW.model_id AND m.available=1 AND c.disconnected_at_ms IS NULL AND c.verified_at_ms IS NOT NULL)
 BEGIN SELECT RAISE(ABORT,'transcription_connection_unavailable'); END;

CREATE TABLE connection_model_preferences(connection_id INTEGER PRIMARY KEY REFERENCES ai_connections(id) ON DELETE CASCADE, model_id TEXT,
 FOREIGN KEY(connection_id,model_id) REFERENCES model_catalog(connection_id,model_id) ON DELETE RESTRICT);
CREATE TRIGGER clear_deleted_connection_model BEFORE DELETE ON model_catalog BEGIN
 UPDATE connection_model_preferences SET model_id=NULL WHERE connection_id=OLD.connection_id AND model_id=OLD.model_id; END;

CREATE INDEX legacy_imports_session ON legacy_imports(session_id);
CREATE TRIGGER require_connected_recap_insert BEFORE INSERT ON recap_preferences
 WHEN NEW.auto_generate=1 AND NOT EXISTS(SELECT 1 FROM model_catalog m JOIN ai_connections c ON c.id=m.connection_id
 WHERE m.connection_id=NEW.connection_id AND m.model_id=NEW.model_id AND m.available=1 AND c.disconnected_at_ms IS NULL AND c.verified_at_ms IS NOT NULL)
 BEGIN SELECT RAISE(ABORT,'recap_connection_unavailable'); END;
CREATE TRIGGER require_connected_transcription_insert BEFORE INSERT ON transcription_preferences
 WHEN NEW.enabled=1 AND NOT EXISTS(SELECT 1 FROM model_catalog m JOIN ai_connections c ON c.id=m.connection_id
 WHERE m.connection_id=NEW.connection_id AND m.model_id=NEW.model_id AND m.available=1 AND c.disconnected_at_ms IS NULL AND c.verified_at_ms IS NOT NULL)
 BEGIN SELECT RAISE(ABORT,'transcription_connection_unavailable'); END;

-- Native compatibility values are persisted without enabling a new cloud route.
CREATE TABLE native_settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE bootstrap_state(config_path TEXT PRIMARY KEY, adopted_at_ms INTEGER NOT NULL);

-- Content-free durable transition history; model/provider identity remains on the job snapshot.
CREATE TABLE job_events(
 id INTEGER PRIMARY KEY, job_id INTEGER NOT NULL REFERENCES recap_jobs(id) ON DELETE CASCADE,
 status TEXT NOT NULL CHECK(status IN ('queued','validating','running','completed','failed','cancelled')),
 at_ms INTEGER NOT NULL CHECK(typeof(at_ms)='integer' AND at_ms>=0), error_code TEXT,
 CHECK(error_code IS NULL OR (length(error_code) BETWEEN 1 AND 64 AND error_code NOT GLOB '*[^A-Za-z0-9_.-]*')));
CREATE INDEX job_events_job ON job_events(job_id,id);
CREATE TRIGGER job_queued_event AFTER INSERT ON recap_jobs BEGIN
 INSERT INTO job_events(job_id,status,at_ms) VALUES(NEW.id,NEW.status,NEW.queued_at_ms); END;
CREATE TRIGGER job_state_event AFTER UPDATE OF status ON recap_jobs BEGIN
 INSERT INTO job_events(job_id,status,at_ms,error_code) VALUES(NEW.id,NEW.status,max(NEW.queued_at_ms,CAST((julianday('now')-2440587.5)*86400000 AS INTEGER)),NEW.error_code); END;
