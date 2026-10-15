#!/usr/bin/env python3
"""Shared SQLite adapter and explicit non-destructive adoption of legacy files.

Credentials belong exclusively to the OS secure store. This module must receive
only registration metadata. Native builds embed the same database_schema.sql.
"""
from __future__ import annotations
import argparse
import contextlib
from datetime import datetime
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import time
from zoneinfo import ZoneInfo

from ascolto_files import atomic_text as _atomic_text

VERSION, APPLICATION_ID, BUSY_TIMEOUT_MS = 1, 0x4153434C, 1500
SECRET_KEYS = {'api_key', 'access_token', 'refresh_token', 'id_token', 'authorization', 'client_secret', 'password', 'secret', 'token'}
PROVIDERS = {'opencode-zen': ('opencode', 'zen'), 'opencode-go': ('opencode', 'go'), 'openrouter': ('openrouter', 'default'), 'openai': ('openai', 'default')}
SETTINGS = {'platform', 'auto_monitoring', 'paused', 'transcription_language', 'capture_backend', 'mic_device_ref', 'sys_device_ref', 'save_audio', 'output_dir', 'settings_section', 'model_path', 'keywords_path', 'control_socket', 'recap_python', 'recap_tts', 'tts_language', 'diarizer_threshold', 'diarization_sensitivity', 'mic_debounce_ms', 'chatgpt_welcomed'}


class DatabaseError(Exception):
    """A readable error without SQL parameters, transcripts or provider responses."""


def now_ms():
    return time.time_ns() // 1_000_000


def database_path(config_path):
    return Path(config_path).expanduser().parent / 'ascolto.sqlite3'


def reject_secrets(value):
    if isinstance(value, dict):
        if any(str(key).lower() in SECRET_KEYS for key in value):
            raise DatabaseError('Credential fields are forbidden in database metadata')
        for item in value.values():
            reject_secrets(item)
    elif isinstance(value, list):
        for item in value:
            reject_secrets(item)


def _json(value):
    reject_secrets(value)
    return json.dumps(value, ensure_ascii=False, separators=(',', ':'))


class Database:
    def __init__(self, path, busy_timeout_ms=BUSY_TIMEOUT_MS):
        if not 0 <= busy_timeout_ms <= 10000:
            raise ValueError('Busy timeout must be between 0 and 10000 milliseconds')
        self._depth = 0
        if str(path) != ':memory:':
            try:
                descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
            except FileExistsError:
                pass
            else:
                os.close(descriptor)
        self.connection = sqlite3.connect(str(path), timeout=busy_timeout_ms / 1000, isolation_level=None)
        self.connection.row_factory = sqlite3.Row
        try:
            version = self.connection.execute('PRAGMA user_version').fetchone()[0]
            application = self.connection.execute('PRAGMA application_id').fetchone()[0]
            tables = self.connection.execute("SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'").fetchone()[0]
            if version > VERSION:
                raise DatabaseError('Database schema is newer than this application')
            if (version and application != APPLICATION_ID) or (not version and (tables or application)):
                raise DatabaseError('Unrecognized database; refusing to overwrite it')
            self.connection.execute('PRAGMA foreign_keys=ON')
            self.connection.execute(f'PRAGMA busy_timeout={busy_timeout_ms}')
            self.connection.execute('PRAGMA journal_mode=WAL')
            self.connection.execute('PRAGMA synchronous=FULL')
            if not version:
                with self.transaction():
                    version = self.connection.execute('PRAGMA user_version').fetchone()[0]
                    if not version:
                        # executescript would implicitly commit this transaction.
                        statement = ''
                        for line in Path(__file__).with_name('database_schema.sql').read_text().splitlines(True):
                            statement += line
                            if sqlite3.complete_statement(statement):
                                self.connection.execute(statement)
                                statement = ''
                        if statement.strip():
                            raise DatabaseError('Incomplete schema statement')
                        self.connection.execute('INSERT INTO schema_migrations VALUES(?,?)', (VERSION, now_ms()))
                        self.connection.execute(f'PRAGMA application_id={APPLICATION_ID}')
                        self.connection.execute(f'PRAGMA user_version={VERSION}')
                    elif version != VERSION:
                        raise DatabaseError('Database schema changed during initialization')
        except BaseException:
            self.close()
            raise

    def close(self):
        self.connection.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    @contextlib.contextmanager
    def transaction(self):
        depth, name = self._depth, f'ascolto_{self._depth}'
        self.connection.execute('BEGIN IMMEDIATE' if not depth else f'SAVEPOINT {name}')
        self._depth += 1
        try:
            yield self.connection
            self.connection.execute('COMMIT' if not depth else f'RELEASE {name}')
        except BaseException:
            if self.connection.in_transaction:
                self.connection.execute('ROLLBACK' if not depth else f'ROLLBACK TO {name}')
                if depth:
                    self.connection.execute(f'RELEASE {name}')
            raise
        finally:
            self._depth -= 1

    def settings(self):
        return dict(self.connection.execute('SELECT * FROM app_settings WHERE id=1').fetchone())

    def update_settings(self, values):
        reject_secrets(values)
        if set(values) - SETTINGS:
            raise DatabaseError('Unknown application setting')
        with self.transaction():
            for key, value in values.items():
                self.connection.execute(f'UPDATE app_settings SET {key}=? WHERE id=1', (value,))

    def queue_export(self, session_id):
        self.connection.execute('INSERT INTO export_outbox(session_id,updated_at_ms) VALUES(?,?) ON CONFLICT(session_id) DO UPDATE SET revision=revision+1,updated_at_ms=excluded.updated_at_ms', (session_id, now_ms()))

    def append_utterance(self, session_id, channel, start_ms, end_ms, text, speaker=None, language=None, source_app=None, metadata=None):
        with self.transaction():
            speaker_id = None
            if speaker:
                self.connection.execute('INSERT INTO speakers(session_id,channel,label) VALUES(?,?,?) ON CONFLICT(session_id,channel,label) DO NOTHING', (session_id, channel, speaker))
                speaker_id = self.connection.execute('SELECT id FROM speakers WHERE session_id=? AND channel=? AND label=?', (session_id, channel, speaker)).fetchone()[0]
            cursor = self.connection.execute('INSERT INTO utterances(session_id,speaker_id,channel,start_ms,end_ms,text,language,source_app,metadata_json) VALUES(?,?,?,?,?,?,?,?,?)', (session_id, speaker_id, channel, start_ms, end_ms, text, language, source_app, _json(metadata or {})))
            self.queue_export(session_id)
            return cursor.lastrowid

    def disconnect(self, connection_id, at_ms=None):
        # Caller removes the actual secure-store credential first; registration survives.
        with self.transaction():
            self.connection.execute('UPDATE ai_connections SET disconnected_at_ms=? WHERE id=?', (now_ms() if at_ms is None else at_ms, connection_id))

    def replace_models(self, connection_id, models, at_ms=None):
        reject_secrets(models)
        stamp = now_ms() if at_ms is None else at_ms
        with self.transaction():
            old = {row[0] for row in self.connection.execute('SELECT model_id FROM model_catalog WHERE connection_id=?', (connection_id,))}
            seen = set()
            for index, model in enumerate(models):
                identifier = model['id']
                if identifier in seen:
                    raise DatabaseError('Duplicate model identifier')
                seen.add(identifier)
                metadata = {key: model[key] for key in ('api', 'capabilities', 'protocol', 'route', 'input_modalities', 'output_modalities', 'architecture', 'context_length', 'supported_parameters', 'tasks') if key in model}
                self.connection.execute('INSERT INTO model_catalog VALUES(?,?,?,?,?,1,?) ON CONFLICT(connection_id,model_id) DO UPDATE SET display_name=excluded.display_name,capabilities_json=excluded.capabilities_json,server_order=excluded.server_order,available=1,refreshed_at_ms=excluded.refreshed_at_ms', (connection_id, identifier, model.get('name', identifier), _json(metadata), index, stamp))
            for missing in old - seen:
                self.connection.execute('UPDATE model_catalog SET available=0,refreshed_at_ms=? WHERE connection_id=? AND model_id=?', (stamp, connection_id, missing))

    def save_ai_settings(self, settings):
        """Replace ProviderManager.load/save without a parallel metadata JSON write.

        The secure-store namespace must still derive from the ORIGINAL JSON path.
        credential_ref contains the existing account identifier, never its value.
        """
        reject_secrets(settings)
        accounts = {a['id']: a for a in settings.get('accounts', [])}
        with self.transaction():
            self.connection.execute('UPDATE app_settings SET chatgpt_welcomed=? WHERE id=1', (int(bool(settings.get('chatgpt_welcomed'))),))
            current = {}
            for account in accounts.values():
                self._save_connection('openai', {'account_id': account['id'], 'account_label': account.get('label', '')}, account, None)
            for provider, profile in settings.get('profiles', {}).items():
                if provider not in PROVIDERS:
                    raise DatabaseError('Unknown provider')
                account = accounts.get(profile.get('account_id'), {})
                identifier = self._save_connection(provider, profile, account, bool(profile.get('verified_at')))
                current[provider] = identifier
                if 'models' in profile and identifier:
                    catalog = {}
                    for task, key in (('recap', 'models'), ('transcription', 'transcription_models')):
                        for item in profile.get(key, []):
                            entry = catalog.setdefault(item['id'], dict(item, tasks=[]))
                            if task not in entry['tasks']:
                                entry['tasks'].append(task)
                    self.replace_models(identifier, list(catalog.values()), int(profile.get('verified_at', 0)) * 1000)
                if identifier:
                    self.connection.execute('INSERT INTO connection_model_preferences VALUES(?,?) ON CONFLICT(connection_id) DO UPDATE SET model_id=excluded.model_id', (identifier, profile.get('model') or None))
            selected = settings.get('selected_provider', '')
            profile = settings.get('profiles', {}).get(selected, {})
            identifier = current.get(selected)
            model = profile.get('model') if identifier else None
            if not model:
                identifier, model = None, None
            self.connection.execute('UPDATE recap_preferences SET connection_id=?,model_id=?,language=?,auto_generate=? WHERE id=1', (identifier, model, settings.get('language', 'Italian'), int(bool(settings.get('automatic_recaps')))))

    def _save_connection(self, provider, profile, account, active):
        family, plan = PROVIDERS[provider]
        auth = 'chatgpt' if provider == 'openai' else 'api_key'
        account_id = profile.get('account_id')
        reference = f'openai:{account_id}' if account_id else provider
        row = self.connection.execute('SELECT id FROM ai_connections WHERE credential_ref=?', (reference,)).fetchone()
        # ChatGPT without account registration is an empty UI placeholder, not a connection.
        if provider == 'openai' and not account_id:
            return None
        values = (account_id, profile.get('account_label') or account.get('label'), account.get('email'), account.get('client_id'), account.get('subject'), account.get('workspace_id'), account.get('workspace_label') or profile.get('workspace_label'), int(profile.get('verified_at', 0)) * 1000 or None, None if active else now_ms(), int(bool(profile.get('welcome_pending'))))
        if row and active is None:
            self.connection.execute('UPDATE ai_connections SET account_label=?,account_email=?,oauth_client_id=?,oauth_subject=?,workspace_id=?,workspace_label=? WHERE id=?', (values[1],values[2],values[3],values[4],values[5],values[6],row[0]))
            return row[0]
        if row:
            self.connection.execute('UPDATE ai_connections SET account_id=?,account_label=?,account_email=?,oauth_client_id=?,oauth_subject=?,workspace_id=?,workspace_label=?,verified_at_ms=coalesce(?,verified_at_ms),disconnected_at_ms=?,welcome_pending=? WHERE id=?', values + (row[0],))
            return row[0]
        cursor = self.connection.execute('INSERT INTO ai_connections(provider_code,profile_code,auth_kind,credential_ref,account_id,account_label,account_email,oauth_client_id,oauth_subject,workspace_id,workspace_label,verified_at_ms,disconnected_at_ms,welcome_pending) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)', (family, plan, auth, reference) + values)
        return cursor.lastrowid

    def load_ai_settings(self):
        app = self.settings()
        pref = self.connection.execute('SELECT * FROM recap_preferences WHERE id=1').fetchone()
        result = {'version': 1, 'automatic_recaps': bool(pref['auto_generate']), 'selected_provider': '', 'language': pref['language'], 'profiles': {}, 'accounts': [], 'host_id': app['oauth_host_id'], 'chatgpt_welcomed': bool(app['chatgpt_welcomed'])}
        for c in self.connection.execute('SELECT * FROM ai_connections ORDER BY id'):
            provider = 'opencode-' + c['profile_code'] if c['provider_code'] == 'opencode' else c['provider_code']
            profile = {}
            if c['account_id']:
                profile.update(account_id=c['account_id'], account_label=c['account_label'] or '')
                account = {'id': c['account_id'], 'client_id': c['oauth_client_id'] or '', 'subject': c['oauth_subject'] or '', 'email': c['account_email'] or '', 'label': c['account_label'] or ''}
                if c['workspace_id']:
                    account['workspace_id'] = c['workspace_id']
                if c['workspace_label']:
                    account['workspace_label'] = c['workspace_label']
                result['accounts'].append(account)
            if c['disconnected_at_ms'] is None:
                models = [{'id': m['model_id'], 'name': m['display_name'], **json.loads(m['capabilities_json'])} for m in self.connection.execute('SELECT * FROM model_catalog WHERE connection_id=? AND available=1 ORDER BY server_order,model_id', (c['id'],))]
                preferred = self.connection.execute('SELECT model_id FROM connection_model_preferences WHERE connection_id=?', (c['id'],)).fetchone()
                profile.update(models=[{k:v for k,v in m.items() if k != 'tasks'} for m in models if 'recap' in m.get('tasks', ['recap'])], transcription_models=[{k:v for k,v in m.items() if k != 'tasks'} for m in models if 'transcription' in m.get('tasks', [])], model=(preferred[0] if preferred else '') or '', verified_at=(c['verified_at_ms'] or 0) // 1000, welcome_pending=bool(c['welcome_pending']))
            if pref['connection_id'] == c['id']:
                result['selected_provider'] = provider
                profile['model'] = pref['model_id']
            if provider not in result['profiles'] or c['disconnected_at_ms'] is None or pref['connection_id'] == c['id']:
                result['profiles'][provider] = profile
        result['transcription'] = dict(self.connection.execute('SELECT * FROM transcription_preferences WHERE id=1').fetchone())
        try:
            result['transcription'].update(self.task_selection('transcription'))
        except DatabaseError:
            result['transcription'].update(provider='openrouter', model='')
        return result

    def task_selection(self, task):
        table = {'recap': 'recap_preferences', 'transcription': 'transcription_preferences'}.get(task)
        if not table:
            raise DatabaseError('Unknown AI task')
        row = self.connection.execute(f'SELECT p.*,c.provider_code,c.profile_code,c.disconnected_at_ms,m.available FROM {table} p LEFT JOIN ai_connections c ON c.id=p.connection_id LEFT JOIN model_catalog m ON m.connection_id=p.connection_id AND m.model_id=p.model_id WHERE p.id=1').fetchone()
        if row['connection_id'] is None or row['disconnected_at_ms'] is not None or not row['available']:
            raise DatabaseError('Select an available connection and model for this task')
        provider = 'opencode-' + row['profile_code'] if row['provider_code'] == 'opencode' else row['provider_code']
        return {'provider': provider, 'model': row['model_id'], 'language': row['language'], 'cloud_audio_opt_in': bool(row['cloud_audio_opt_in']) if task == 'transcription' else False}

    def enqueue_job(self, task, session_id, connection_id, model_id, language, input_hash, at_ms=None):
        """Snapshot catalog identity atomically; no provider call or prompt is stored."""
        stamp = now_ms() if at_ms is None else at_ms
        with self.transaction():
            cursor = self.connection.execute("INSERT INTO recap_jobs(session_id,connection_id,provider_code_snapshot,model_id_snapshot,model_label_snapshot,language,input_hash,task,attempt,status,queued_at_ms) SELECT ?,c.id,c.provider_code,m.model_id,m.display_name,?,?,?,coalesce((SELECT max(attempt)+1 FROM recap_jobs WHERE session_id=? AND task=?),1),'queued',? FROM ai_connections c JOIN model_catalog m ON m.connection_id=c.id WHERE c.id=? AND m.model_id=? AND m.available=1 AND c.disconnected_at_ms IS NULL AND c.verified_at_ms IS NOT NULL", (session_id, language, input_hash, task, session_id, task, stamp, connection_id, model_id))
            if not cursor.rowcount:
                raise DatabaseError('Job connection or model is unavailable')
            return cursor.lastrowid

    def update_job(self, job_id, status, error_code=None, at_ms=None):
        stamp = now_ms() if at_ms is None else at_ms
        with self.transaction():
            cursor = self.connection.execute("UPDATE recap_jobs SET status=?,started_at_ms=CASE WHEN ?='running' THEN ? ELSE started_at_ms END,finished_at_ms=CASE WHEN ? IN ('completed','failed','cancelled') THEN ? ELSE NULL END,error_code=? WHERE id=? AND status IN ('queued','validating','running')", (status, status, stamp, status, stamp, error_code, job_id))
            if not cursor.rowcount:
                raise DatabaseError('Job is missing or already terminal')

    def register_artifact(self, session_id, kind, path, job_id=None, state='ready', sha256=None, at_ms=None):
        path = str(Path(path).resolve())
        with self.transaction():
            cursor = self.connection.execute("INSERT INTO artifacts(session_id,recap_job_id,kind,path,state,created_at_ms,sha256) VALUES(?,?,?,?,?,?,?) ON CONFLICT(path) DO UPDATE SET state=excluded.state,sha256=excluded.sha256 WHERE artifacts.session_id=excluded.session_id AND artifacts.recap_job_id IS excluded.recap_job_id AND artifacts.kind=excluded.kind", (session_id, job_id, kind, path, state, now_ms() if at_ms is None else at_ms, sha256))
            if not cursor.rowcount:
                raise DatabaseError('Artifact path already belongs to another session or job')
            return self.connection.execute('SELECT id FROM artifacts WHERE path=?', (path,)).fetchone()[0]

    def _already_imported(self, path, digest):
        row = self.connection.execute('SELECT source_sha256 FROM legacy_imports WHERE source_path=?', (str(path),)).fetchone()
        if row and row[0] != digest:
            raise DatabaseError('Legacy source changed after import; reconcile explicitly without overwriting database history')
        return bool(row)

    def _record_import(self, path, digest, kind, backup=None, session=None):
        self.connection.execute('INSERT INTO legacy_imports VALUES(?,?,?,?,?,?)', (str(path), digest, now_ms(), session, str(backup) if backup else None, kind))

    def import_ai_settings(self, path):
        path = Path(path).resolve()
        data = path.read_bytes()
        digest, settings = hashlib.sha256(data).hexdigest(), json.loads(data)
        reject_secrets(settings)
        if settings.get('version') != 1:
            raise DatabaseError('Unsupported AI settings version')
        with self.transaction():
            if self._already_imported(path, digest):
                return False
            if self.connection.execute('SELECT count(*) FROM ai_connections').fetchone()[0]:
                raise DatabaseError('AI metadata already exists; refusing to replace account registrations')
            backup = _backup(path, data, digest)
            if settings.get('host_id'):
                self.connection.execute('UPDATE app_settings SET oauth_host_id=? WHERE id=1', (settings['host_id'],))
            self.save_ai_settings(settings)
            self._record_import(path, digest, 'ai_settings', backup)
        return True

    def import_config(self, path, platform, keywords_path=None):
        path = Path(path).resolve()
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if platform == 'macos':
            config = json.loads(data)
        elif platform == 'linux':
            config = dict(line.split('=', 1) for raw in data.decode().splitlines() if (line := raw.strip()) and not line.startswith(('#', ';', '[')) and '=' in line)
            config = {key.strip(): value.strip() for key, value in config.items()}
        else:
            raise DatabaseError('Explicit macos or linux platform is required')
        values = {'platform': platform, 'capture_backend': 'coreaudio' if platform == 'macos' else 'pipewire'}
        mapping = {'auto_trigger_mic': 'auto_monitoring', 'auto_mic_trigger': 'auto_monitoring', 'language': 'transcription_language', 'mic_device_id': 'mic_device_ref', 'sys_device_id': 'sys_device_ref', 'mic_device': 'mic_device_ref', 'sys_device': 'sys_device_ref', 'weights_path': 'model_path', 'custom_threshold': 'diarizer_threshold', 'tts_lang': 'tts_language'}
        for key, value in config.items():
            mapped = mapping.get(key, key)
            if mapped not in SETTINGS:
                continue
            if mapped in {'auto_monitoring', 'paused', 'save_audio', 'recap_tts'}:
                value = int(str(value).lower() in ('1', 'true'))
            else:
                value = int(value) if mapped == 'mic_debounce_ms' else str(value)
            values[mapped] = value
        with self.transaction():
            if self._already_imported(path, digest):
                return False
            backup = _backup(path, data, digest)
            self.update_settings(values)
            self._record_import(path, digest, 'config', backup)
            if platform == 'linux' and config.get('keywords'):
                self._insert_keywords(config['keywords'])
            keyword_file = Path(keywords_path or config.get('keywords_path') or path.with_name('keywords.txt')).expanduser()
            if keyword_file.is_file():
                self.import_keywords(keyword_file)
        return True

    def _insert_keywords(self, value):
        for keyword in re.split(r'[,\n\r]+', value):
            if keyword.strip():
                self.connection.execute('INSERT INTO keywords(text) VALUES(?) ON CONFLICT(text) DO NOTHING', (keyword.strip(),))

    def import_keywords(self, path):
        path = Path(path).resolve()
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        with self.transaction():
            if self._already_imported(path, digest):
                return False
            backup = _backup(path, data, digest)
            self._insert_keywords(data.decode())
            self._record_import(path, digest, 'keywords', backup)
        return True

    def import_session(self, path, legacy_timezone=None):
        path = Path(path).resolve()
        if path.suffix == '.md' and path.with_suffix('.jsonl').is_file():
            path = path.with_suffix('.jsonl')
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        with self.transaction():
            if self._already_imported(path, digest):
                return None
            backend = 'pipewire' if path.name in ('transcript.jsonl', 'transcript.md') else 'coreaudio'
            stamp_source = path.parent.name if backend == 'pipewire' else path.name
            match = re.search(r'(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2})(?:-(\d{6}))?', stamp_source)
            if not match:
                raise DatabaseError('Legacy session filename has no recognizable timestamp')
            wall = datetime.strptime(match[1], '%Y-%m-%d_%H-%M-%S')
            if match[2]:
                wall = wall.replace(microsecond=int(match[2]))
            aware = wall.replace(tzinfo=ZoneInfo(legacy_timezone)) if legacy_timezone else wall.astimezone()
            start = int(aware.timestamp() * 1000)
            md_path = path.with_suffix('.md')
            md = md_path.read_text() if md_path.is_file() else ''
            rows = [json.loads(line) for line in data.decode().splitlines() if line.strip()] if path.suffix == '.jsonl' else _markdown_utterances(data.decode(), backend)
            duration, metadata = None, {'timestamp_source': 'legacy_filename', 'legacy_timezone': str(aware.tzinfo)}
            vocabulary_match = re.search(r'^- \*\*Keywords\*\*: `([^`]*)`', md, re.MULTILINE)
            vocabulary = list(dict.fromkeys(word.strip() for word in re.split(r'[,\n\r]+', vocabulary_match[1]) if word.strip())) if vocabulary_match else []
            state = 'completed' if 'Sessione terminata (' in md or 'Fine chiamata (durata:' in md else 'failed'
            title, meeting, source = None, None, None
            for row in rows:
                if row.get('event') in ('session_start', 'call_metadata'):
                    title, meeting, source = row.get('window_title', title), row.get('calendar_title', meeting), row.get('in_app', source)
                    metadata.update({k: row[k] for k in ('manual', 'mic', 'in_app', 'out_app', 'attendees', 'calendar_organizer', 'calendar_notes') if k in row})
                elif row.get('event') == 'session_end':
                    duration, state = _milliseconds(row['duration_s']), 'completed'
                    metadata.update({k:row[k] for k in ('duration_s','utterances','remote_speakers','audio_m4a','bleed_frames','echo_turns','capture_dropped_samples','padded_mic_samples','padded_sys_samples') if k in row})
            for label, key in (('Organizzatore', 'calendar_organizer'), ('Descrizione / Agenda', 'calendar_notes')):
                value = re.search(r'^- \*\*' + re.escape(label) + r'\*\*: (.*)$', md, re.MULTILINE)
                if value and key not in metadata:
                    metadata[key] = value[1]
            cursor = self.connection.execute('INSERT INTO sessions(started_at_ms,ended_at_ms,state,call_title,meeting_title,source_app,capture_backend,legacy_source,metadata_json,vocabulary_snapshot_json) VALUES(?,?,?,?,?,?,?,?,?,?)', (start, start + duration if duration is not None else None, state, title, meeting, source, backend, str(path), _json(metadata), _json(vocabulary)))
            session = cursor.lastrowid
            for row in rows:
                if row.get('event') not in (None, 'utterance') or 'text' not in row:
                    continue
                diagnostics = {k:row[k] for k in ('seq_id','speaker_id','is_new_speaker','similarity','f0_hz','pitch_hz','rms_db','ttft_ms') if k in row}
                self.append_utterance(session, row['channel'], _milliseconds(row.get('start_s', row.get('start_sec'))), _milliseconds(row.get('end_s', row.get('end_sec'))), row['text'], row.get('speaker'), row.get('language'), row.get('source_app'), metadata=diagnostics)
            last = self.connection.execute('SELECT max(end_ms) FROM utterances WHERE session_id=?', (session,)).fetchone()[0]
            if duration is not None and last is not None and last > duration:
                raise DatabaseError('Legacy utterance extends beyond the recorded session duration')
            pairs = [(path.with_suffix('.md'), 'transcript_md'), (path.with_suffix('.jsonl'), 'transcript_jsonl')]
            pairs += [(path.parent / 'mic.flac', 'audio_mic_flac'), (path.parent / 'sys.flac', 'audio_sys_flac')] if backend == 'pipewire' else [(path.with_suffix('.m4a'), 'audio_m4a')]
            for candidate, kind in pairs:
                if candidate.is_file():
                    self.connection.execute("INSERT INTO artifacts(session_id,kind,path,state,created_at_ms) VALUES(?,?,?,'ready',?)", (session, kind, str(candidate), candidate.stat().st_mtime_ns // 1_000_000))
            # Import never republishes or overwrites original transcripts.
            self.connection.execute('DELETE FROM export_outbox WHERE session_id=?', (session,))
            self._record_import(path, digest, 'session', session=session)
            return session

    def publish_exports(self, session_id, markdown_path, jsonl_path):
        """Render on a background worker under the database publication transaction.

        The write reservation serializes publishers and new transcript writes, so
        an older renderer cannot overwrite a newer export after its outbox ack.
        A crash between per-file renames rolls back the ack; retry writes both.
        Never call from an audio callback or the UI thread.
        """
        with self.transaction():
            request = self.connection.execute('SELECT revision FROM export_outbox WHERE session_id=?', (session_id,)).fetchone()
            if not request:
                return False
            revision = request[0]
            session = dict(self.connection.execute('SELECT * FROM sessions WHERE id=?', (session_id,)).fetchone())
            rows = [dict(row) for row in self.connection.execute('SELECT u.*,s.label AS speaker FROM utterances u LEFT JOIN speakers s ON s.id=u.speaker_id WHERE u.session_id=? ORDER BY u.start_ms,u.id', (session_id,))]
            targets = (Path(markdown_path).resolve(), Path(jsonl_path).resolve())
            if targets[0] == targets[1]:
                raise DatabaseError('Transcript exports require two distinct paths')
            for path, expected in zip(targets, ('transcript_md', 'transcript_jsonl')):
                owner = self.connection.execute('SELECT session_id,kind FROM artifacts WHERE path=?', (str(path),)).fetchone()
                if owner and (owner[0] != session_id or owner[1] != expected):
                    raise DatabaseError('Export path belongs to another artifact')
            lines = ['# Call transcript\n\n']
            events = [{**json.loads(session['metadata_json']), 'event': 'session_start', 'started_at_ms': session['started_at_ms'], 'window_title': session['call_title'] or '', 'calendar_title': session['meeting_title'] or ''}]
            for row in rows:
                speaker = row['speaker'] or row['channel']
                if session['capture_backend'] == 'pipewire':
                    lines.append(f"**[{_mmss(row['start_ms'])} - {_mmss(row['end_ms'])}] [{speaker}]**: {row['text']}\n\n")
                else:
                    lines.append(f"- **[{_mmss(row['start_ms'])} - {_mmss(row['end_ms'])}] {speaker}** *(channel {row['channel']} · app: {row['source_app'] or ''}, language: {row['language'] or ''})*: {row['text']}\n")
                events.append({**json.loads(row.get('metadata_json') or '{}'), 'event': 'utterance', 'source_id': f"db:{session_id}:{row['id']}", 'channel': row['channel'], 'speaker': speaker, 'start_s': row['start_ms'] / 1000, 'end_s': row['end_ms'] / 1000, 'start_sec': row['start_ms'] / 1000, 'end_sec': row['end_ms'] / 1000, 'text': row['text'], 'language': row['language'] or '', 'source_app': row['source_app'] or ''})
            if session['ended_at_ms'] is not None:
                events.append({'event': 'session_end', **json.loads(session['metadata_json']), 'duration_s': json.loads(session['metadata_json']).get('duration_s', (session['ended_at_ms'] - session['started_at_ms']) / 1000)})
            output = [(targets[0], 'transcript_md', ''.join(lines)), (targets[1], 'transcript_jsonl', ''.join(json.dumps(event, ensure_ascii=False) + '\n' for event in events))]
            for path, kind, content in output:
                _atomic_text(path, content)
                digest = hashlib.sha256(content.encode()).hexdigest()
                self.connection.execute("INSERT INTO artifacts(session_id,kind,path,state,created_at_ms,sha256) VALUES(?,?,?,'ready',?,?) ON CONFLICT(path) DO UPDATE SET state='ready',sha256=excluded.sha256", (session_id, kind, str(path), now_ms(), digest))
            self.connection.execute('DELETE FROM export_outbox WHERE session_id=? AND revision=?', (session_id, revision))
        return True


def _milliseconds(value):
    try:
        numeric = Decimal(str(value))
        if not numeric.is_finite() or numeric < 0:
            raise ValueError()
        return int((numeric * 1000).to_integral_value(rounding=ROUND_HALF_UP))
    except (InvalidOperation, ValueError):
        raise DatabaseError('Invalid legacy audio offset') from None


def _mmss(milliseconds):
    seconds = milliseconds // 1000
    return f'{seconds // 60:02d}:{seconds % 60:02d}'


def _markdown_utterances(content, backend):
    rows = []
    mac = re.compile(r'^- \*\*\[(\d+):(\d+) - (\d+):(\d+)\] (.+?)\*\* \*\((?:canale|channel) (MIC|SYS).*?\)\*: (.*)$')
    linux = re.compile(r'^\*\*\[(\d+):(\d+) - (\d+):(\d+)\] \[(.+?)\]\*\*: (.*)$')
    for line in content.splitlines():
        match = (mac if backend == 'coreaudio' else linux).match(line)
        if not match:
            if '**[' in line and ' - ' in line:
                raise DatabaseError('Unrecognized legacy Markdown utterance')
            continue
        parts = match.groups()
        channel = parts[5] if backend == 'coreaudio' else ('MIC' if parts[4] == 'ME' else 'SYS')
        rows.append({'channel': channel, 'speaker': parts[4], 'start_s': int(parts[0]) * 60 + int(parts[1]), 'end_s': int(parts[2]) * 60 + int(parts[3]), 'text': parts[-1]})
    return rows


def _backup(path, data, digest):
    target = path.with_name(path.name + '.ascolto-backup.' + digest[:16])
    try:
        descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        if target.read_bytes() != data:
            raise DatabaseError('Existing migration backup does not match its source')
    else:
        with os.fdopen(descriptor, 'wb') as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
    return target


def main():
    parser = argparse.ArgumentParser(description='Explicit non-destructive Ascolto legacy adoption')
    parser.add_argument('--database', required=True)
    parser.add_argument('--config')
    parser.add_argument('--platform', choices=('macos', 'linux'))
    parser.add_argument('--ai-settings')
    parser.add_argument('--sessions', nargs='*', default=[])
    parser.add_argument('--legacy-timezone', help='IANA timezone of legacy filenames; default is machine local timezone')
    args = parser.parse_args()
    with Database(args.database) as database:
        with database.transaction():
            if args.config:
                database.import_config(args.config, args.platform)
            if args.ai_settings:
                database.import_ai_settings(args.ai_settings)
            for session in args.sessions:
                database.import_session(session, args.legacy_timezone)
    print('Legacy adoption completed; original files were retained.')


if __name__ == '__main__':
    main()
