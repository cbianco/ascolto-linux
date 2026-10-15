#!/usr/bin/env python3
"""Storage regressions without audio hardware, network, or actual credentials."""
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import ascolto_database as storage


class DatabaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ascolto-db-é'")
        self.root = Path(self.temporary.name)
        self.path = self.root / "l'appel.sqlite3"
        self.db = storage.Database(self.path)

    def tearDown(self):
        self.db.close()
        self.temporary.cleanup()

    def scalar(self, sql, parameters=()):
        return self.db.connection.execute(sql, parameters).fetchone()[0]

    def session(self):
        return self.db.connection.execute("INSERT INTO sessions(started_at_ms,state,capture_backend) VALUES(1000,'recording','test')").lastrowid

    def ai(self):
        return {'version': 1, 'automatic_recaps': True, 'selected_provider': 'openrouter', 'language': 'Italian', 'host_id': 'urn:uuid:original', 'profiles': {'openrouter': {'models': [{'id': 'actual/model', 'name': 'Actual model', 'api': {'family': 'openai', 'base_url': 'https://example.invalid/v1'}}], 'model': 'actual/model', 'verified_at': 100}}, 'accounts': [], 'chatgpt_welcomed': True}

    def test_connection_pragmas_and_permissions(self):
        self.assertEqual(self.scalar('PRAGMA foreign_keys'), 1)
        self.assertEqual(self.scalar('PRAGMA busy_timeout'), 1500)
        self.assertEqual(self.scalar('PRAGMA journal_mode'), 'wal')
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.scalar('SELECT count(*) FROM schema_migrations'), 1)
        self.assertEqual(self.scalar('SELECT enabled FROM transcription_preferences'), 0)
        self.assertEqual(self.scalar('SELECT cloud_audio_opt_in FROM transcription_preferences'), 0)

    def test_native_schema_is_current(self):
        subprocess.run([sys.executable, str(Path(storage.__file__).with_name('generate_database_schema.py')), '--check'], check=True)

    def test_transaction_and_nested_rollback(self):
        with self.assertRaises(ValueError):
            with self.db.transaction():
                self.db.update_settings({'output_dir': "/tmp/È l'ora"})
                raise ValueError('stop')
        self.assertEqual(self.db.settings()['output_dir'], '')
        session = self.session()
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.append_utterance(session, 'MIC', 1000, 10, 'bad', 'new speaker')
        self.assertEqual(self.scalar('SELECT count(*) FROM speakers'), 0)
        self.assertEqual(self.scalar('SELECT count(*) FROM export_outbox'), 0)

    def test_speaker_session_and_channel_constraints(self):
        a, b = self.session(), self.session()
        self.db.append_utterance(a, 'MIC', 0, 1000, "L'appel d'Élodie", 'ME')
        speaker = self.scalar('SELECT id FROM speakers')
        for session, channel in ((b, 'MIC'), (a, 'SYS')):
            with self.assertRaises(sqlite3.IntegrityError):
                self.db.connection.execute('INSERT INTO utterances(session_id,speaker_id,channel,start_ms,end_ms,text) VALUES(?,?,?,?,?,?)', (session, speaker, channel, 0, 1, 'invalid'))

    def test_ai_roundtrip_retains_registration_and_routes(self):
        settings = self.ai()
        settings['accounts'] = [{'id': 'account-a', 'client_id': 'client-a', 'subject': 'same-user', 'workspace_id': 'work-a', 'email': 'a@example.invalid', 'label': 'Account A'}, {'id': 'account-b', 'client_id': 'client-b', 'subject': 'same-user', 'workspace_id': 'work-b', 'email': 'a@example.invalid', 'label': 'Account B'}]
        settings['profiles']['openai'] = {'account_id': 'account-a', 'account_label': 'Account A', 'models': [{'id': 'chat-model', 'name': 'Chat model'}], 'model': 'chat-model', 'verified_at': 200, 'welcome_pending': True}
        self.db.save_ai_settings(settings)
        loaded = self.db.load_ai_settings()
        self.assertEqual(loaded['profiles']['openrouter']['models'], settings['profiles']['openrouter']['models'])
        self.assertEqual(loaded['profiles']['openai']['model'], 'chat-model')
        self.assertEqual(loaded['accounts'], settings['accounts'])
        self.db.save_ai_settings(loaded)
        self.assertEqual(self.scalar('SELECT count(*) FROM ai_connections'), 3)
        self.assertEqual(self.scalar('SELECT count(DISTINCT oauth_client_id) FROM ai_connections WHERE auth_kind=\'chatgpt\''), 2)
        self.assertEqual(self.scalar('SELECT auto_generate FROM recap_preferences'), 1)

    def test_api_key_identity_has_no_null_duplicate_escape(self):
        self.db.save_ai_settings(self.ai())
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.connection.execute("INSERT INTO ai_connections(provider_code,profile_code,auth_kind) VALUES('openrouter','default','api_key')")

    def test_unknown_setting_and_secret_metadata_rejected(self):
        for values in ({'api_key': 'never-store-this'}, {'output_dir;DROP TABLE sessions;--': 'x'}):
            with self.assertRaises(storage.DatabaseError):
                self.db.update_settings(values)
        settings = self.ai()
        settings['profiles']['openrouter']['access_token'] = 'never-store-this'
        with self.assertRaises(storage.DatabaseError):
            self.db.save_ai_settings(settings)
        self.assertNotIn('never-store-this', '\n'.join(self.db.connection.iterdump()))

    def test_catalog_refresh_does_not_change_selected_model(self):
        self.db.save_ai_settings(self.ai())
        connection = self.scalar('SELECT id FROM ai_connections')
        self.db.replace_models(connection, [{'id': 'actual/model', 'name': 'Changed label'}, {'id': 'second', 'name': 'Second'}])
        self.assertEqual(self.scalar('SELECT auto_generate FROM recap_preferences'), 1)
        self.db.replace_models(connection, [{'id': 'second', 'name': 'Second'}])
        self.assertEqual(self.scalar('SELECT model_id FROM recap_preferences'), 'actual/model')
        self.assertEqual(self.scalar('SELECT auto_generate FROM recap_preferences'), 0)
        with self.assertRaises(storage.DatabaseError):
            self.db.task_selection('recap')

    def test_disconnect_preserves_metadata_and_history(self):
        self.db.save_ai_settings(self.ai())
        session = self.session()
        connection = self.scalar('SELECT id FROM ai_connections')
        self.db.connection.execute("INSERT INTO recap_jobs(session_id,connection_id,provider_code_snapshot,model_id_snapshot,model_label_snapshot,language,input_hash,attempt,status,queued_at_ms) VALUES(?,?,'openrouter','actual/model','Original name','Italian',?,1,'queued',1000)", (session, connection, 'a' * 64))
        self.db.disconnect(connection)
        self.assertEqual(self.scalar('SELECT auto_generate FROM recap_preferences'), 0)
        self.assertEqual(self.scalar('SELECT credential_ref FROM ai_connections'), 'openrouter')
        self.db.connection.execute("DELETE FROM providers WHERE code='openrouter'")
        self.assertIsNone(self.scalar('SELECT connection_id FROM recap_jobs'))
        self.assertEqual(self.scalar('SELECT model_label_snapshot FROM recap_jobs'), 'Original name')
        self.assertIsNone(self.scalar('SELECT connection_id FROM recap_preferences'))

    def test_transcription_preferences_are_independent_and_opt_in(self):
        self.db.save_ai_settings(self.ai())
        connection = self.scalar('SELECT id FROM ai_connections')
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.connection.execute("UPDATE transcription_preferences SET connection_id=?,model_id='actual/model',enabled=1", (connection,))
        self.db.connection.execute("UPDATE transcription_preferences SET connection_id=?,model_id='actual/model',enabled=1,cloud_audio_opt_in=1", (connection,))
        self.assertTrue(self.db.task_selection('transcription')['cloud_audio_opt_in'])
        self.db.disconnect(connection)
        self.assertEqual(self.scalar('SELECT enabled FROM transcription_preferences'), 0)

    def test_mac_config_backup_import_and_idempotence(self):
        config = self.root / 'config.json'
        data = json.dumps({'auto_trigger_mic': False, 'save_audio': True, 'language': 'it', 'mic_device_id': 42, 'output_dir': "/tmp/È l'ora", 'cloud_recap': True}).encode()
        config.write_bytes(data)
        (self.root / 'keywords.txt').write_text('Élodie\nAscolto\nÉlodie\n')
        self.assertTrue(self.db.import_config(config, 'macos'))
        self.assertFalse(self.db.import_config(config, 'macos'))
        self.assertEqual(config.read_bytes(), data)
        self.assertEqual(next(self.root.glob('config.json.ascolto-backup.*')).read_bytes(), data)
        settings = self.db.settings()
        self.assertEqual((settings['auto_monitoring'], settings['save_audio'], settings['mic_device_ref']), (0, 1, '42'))
        self.assertEqual(settings['output_dir'], "/tmp/È l'ora")
        self.assertEqual(self.scalar('SELECT auto_generate FROM recap_preferences'), 0)
        self.assertEqual(self.scalar('SELECT count(*) FROM keywords'), 2)
        config.write_text('{}')
        with self.assertRaises(storage.DatabaseError):
            self.db.import_config(config, 'macos')

    def test_linux_config_preserves_native_device_strings(self):
        path = self.root / 'config.ini'
        path.write_text("[ascolto]\nauto_mic_trigger=true\nsave_audio=false\nlanguage=en\nmic_device=alsa_input.device's-name\nsys_device=monitor.é\nkeywords=Needle,Whistle\nweights_path=/tmp/model.cact\nmic_debounce_ms=700\ncustom_threshold=0.87\n")
        self.db.import_config(path, 'linux')
        settings = self.db.settings()
        self.assertEqual(settings['mic_device_ref'], "alsa_input.device's-name")
        self.assertEqual(settings['sys_device_ref'], 'monitor.é')
        self.assertEqual(settings['mic_debounce_ms'], 700)
        self.assertEqual(settings['model_path'], '/tmp/model.cact')
        self.assertEqual(settings['diarizer_threshold'], '0.87')

    def test_ai_import_keeps_host_and_never_overwrites_existing(self):
        path = self.root / 'ai-providers.json'
        path.write_text(json.dumps(self.ai()))
        self.assertTrue(self.db.import_ai_settings(path))
        self.assertFalse(self.db.import_ai_settings(path))
        self.assertEqual(self.db.settings()['oauth_host_id'], 'urn:uuid:original')
        other = self.root / 'other.json'
        other.write_text(path.read_text())
        with self.assertRaises(storage.DatabaseError):
            self.db.import_ai_settings(other)

    def mac_fixture(self):
        path = self.root / '2026-10-15_10-20-30-123456_call.jsonl'
        events = [{'event': 'session_start', 'window_title': "L'appel d'Élodie", 'calendar_title': 'Planning', 'in_app': 'Meet'}, {'event': 'utterance', 'channel': 'MIC', 'speaker': 'ME', 'start_s': .125, 'end_s': 1.625, 'text': "L'azione è pronta", 'language': 'it'}, {'event': 'utterance', 'channel': 'SYS', 'speaker': 'SPEAKER_1', 'start_s': 1.75, 'end_s': 3, 'text': 'Ready'}, {'event': 'session_end', 'duration_s': 4.125}]
        path.write_text(''.join(json.dumps(event, ensure_ascii=False) + '\n' for event in events))
        path.with_suffix('.md').write_text('# Trascrizione Call\n')
        path.with_suffix('.m4a').write_bytes(b'original audio bytes')
        return path

    def test_mac_session_import_is_idempotent_and_audio_survives_delete(self):
        path = self.mac_fixture()
        originals = {p: p.read_bytes() for p in self.root.iterdir() if p.suffix in ('.md', '.jsonl', '.m4a')}
        session = self.db.import_session(path, 'Europe/Rome')
        self.assertIsNone(self.db.import_session(path.with_suffix('.md'), 'Europe/Rome'))
        self.assertEqual(self.scalar('SELECT count(*) FROM utterances'), 2)
        self.assertEqual(self.scalar('SELECT start_ms FROM utterances ORDER BY id LIMIT 1'), 125)
        self.assertEqual(self.scalar('SELECT ended_at_ms-started_at_ms FROM sessions'), 4125)
        self.assertEqual(self.scalar('SELECT count(*) FROM artifacts'), 3)
        self.assertEqual(self.scalar('SELECT count(*) FROM export_outbox'), 0)
        self.db.connection.execute('DELETE FROM sessions WHERE id=?', (session,))
        for path, data in originals.items():
            self.assertEqual(path.read_bytes(), data)
        self.assertEqual(self.scalar('SELECT count(*) FROM pragma_foreign_key_check'), 0)

    def test_linux_session_import_no_fabricated_end(self):
        directory = self.root / 'call_2026-10-15_10-30-00_17'
        directory.mkdir()
        path = directory / 'transcript.jsonl'
        path.write_text(json.dumps({'seq_id': 1, 'channel': 'SYS', 'speaker': 'SPEAKER_1', 'start_sec': .25, 'end_sec': 1.75, 'text': 'Remote speech'}) + '\n')
        path.with_suffix('.md').write_text('# Trascrizione Call\n- **Keywords**: `Needle,Whistle`\n*Sessione terminata (manual) - Interlocutori remoti rilevati: 1*\n')
        (directory / 'mic.flac').write_bytes(b'mic')
        (directory / 'sys.flac').write_bytes(b'sys')
        self.db.import_session(path, 'UTC')
        self.assertEqual(self.scalar('SELECT state FROM sessions'), 'completed')
        self.assertIsNone(self.scalar('SELECT ended_at_ms FROM sessions'))
        self.assertEqual(self.scalar('SELECT count(*) FROM artifacts'), 4)
        self.assertEqual(json.loads(self.scalar('SELECT vocabulary_snapshot_json FROM sessions')), ['Needle', 'Whistle'])

    def test_markdown_fallback_both_platforms(self):
        mac = self.root / '2026-10-15_10-30-00_call.md'
        mac.write_text('- **[00:01 - 00:03] ME** *(canale MIC · app: App, lingua: it)*: Ciao\n')
        self.db.import_session(mac, 'UTC')
        directory = self.root / 'call_2026-10-15_11-30-00_18'
        directory.mkdir()
        linux = directory / 'transcript.md'
        linux.write_text('**[00:02 - 00:04] [SPEAKER_1]**: Hello\n\n')
        self.db.import_session(linux, 'UTC')
        self.assertEqual(self.scalar('SELECT count(*) FROM utterances'), 2)

    def test_invalid_session_import_rolls_back_all_rows(self):
        path = self.mac_fixture()
        path.write_text(path.read_text() + '{invalid}\n')
        with self.assertRaises(json.JSONDecodeError):
            self.db.import_session(path, 'UTC')
        self.assertEqual(self.scalar('SELECT count(*) FROM sessions'), 0)
        self.assertEqual(self.scalar('SELECT count(*) FROM legacy_imports'), 0)

    def test_future_unknown_and_corrupt_database_never_reset(self):
        for name, sql in (('future', 'PRAGMA user_version=999'), ('unknown', 'CREATE TABLE important(value TEXT)')):
            path = self.root / (name + '.sqlite3')
            with sqlite3.connect(path) as connection:
                connection.execute(sql)
            before = path.read_bytes()
            with self.assertRaises(storage.DatabaseError):
                storage.Database(path)
            self.assertEqual(path.read_bytes(), before)
        path = self.root / 'corrupt.sqlite3'
        path.write_bytes(b'this is not a database')
        with self.assertRaises(sqlite3.DatabaseError):
            storage.Database(path)
        self.assertEqual(path.read_bytes(), b'this is not a database')

    def test_multiwriter_wait_and_bounded_busy(self):
        other = storage.Database(self.path, 50)
        try:
            with self.db.transaction():
                before = time.monotonic()
                with self.assertRaises(sqlite3.OperationalError):
                    other.update_settings({'output_dir': 'must rollback'})
                self.assertLess(time.monotonic() - before, .75)
            self.assertEqual(other.settings()['output_dir'], '')
        finally:
            other.close()
        failures = []
        def write():
            try:
                with storage.Database(self.path) as database:
                    database.update_settings({'output_dir': "l'ultimo"})
            except Exception as error:
                failures.append(error)
        with self.db.transaction():
            thread = threading.Thread(target=write)
            thread.start()
            time.sleep(.1)
        thread.join()
        self.assertEqual(failures, [])
        self.assertEqual(self.db.settings()['output_dir'], "l'ultimo")

    def test_export_crash_recovery_and_no_audio_changes(self):
        session = self.session()
        self.db.append_utterance(session, 'MIC', 0, 1000, "C'è una voce", 'ME')
        md, jsonl = self.root / 'new.md', self.root / 'new.jsonl'
        real = storage._atomic_text
        def fail_second(path, text):
            if path == jsonl.resolve():
                raise OSError('simulated publication failure')
            real(path, text)
        with patch.object(storage, '_atomic_text', side_effect=fail_second):
            with self.assertRaises(OSError):
                self.db.publish_exports(session, md, jsonl)
        self.assertEqual(self.scalar('SELECT count(*) FROM export_outbox'), 1)
        self.assertTrue(self.db.publish_exports(session, md, jsonl))
        self.assertIn("C'è una voce", md.read_text())
        self.assertEqual(json.loads(jsonl.read_text().splitlines()[1])['end_sec'], 1)
        self.assertEqual(self.scalar('SELECT count(*) FROM export_outbox'), 0)
        self.assertEqual(self.scalar('SELECT count(*) FROM artifacts WHERE sha256 IS NOT NULL'), 2)

    def test_integer_offsets_reject_floating_point(self):
        session = self.session()
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.connection.execute("INSERT INTO utterances(session_id,channel,start_ms,end_ms,text) VALUES(?,'MIC',0.5,1,'invalid')", (session,))

    def test_export_publisher_serializes_concurrent_transcript_writer(self):
        session = self.session()
        self.db.append_utterance(session, 'MIC', 0, 1000, 'First', 'ME')
        md, jsonl = self.root / 'serial.md', self.root / 'serial.jsonl'
        started, done = threading.Event(), threading.Event()
        failures = []
        def append():
            started.set()
            try:
                with storage.Database(self.path) as database:
                    database.append_utterance(session, 'SYS', 1000, 2000, 'Second', 'SPEAKER_1')
            except Exception as error:
                failures.append(error)
            finally:
                done.set()
        real = storage._atomic_text
        worker = None
        def during_publish(path, text):
            nonlocal worker
            if worker is None:
                worker = threading.Thread(target=append)
                worker.start()
                self.assertTrue(started.wait(1))
                time.sleep(.05)
                self.assertFalse(done.is_set())
            real(path, text)
        with patch.object(storage, '_atomic_text', side_effect=during_publish):
            self.db.publish_exports(session, md, jsonl)
        worker.join()
        self.assertEqual(failures, [])
        self.assertEqual(self.scalar('SELECT count(*) FROM export_outbox'), 1)
        self.db.publish_exports(session, md, jsonl)
        self.assertIn('Second', md.read_text())
        self.assertEqual(self.scalar('SELECT count(*) FROM export_outbox'), 0)


    def test_job_adapter_snapshots_attempts_errors_and_artifacts(self):
        self.db.save_ai_settings(self.ai())
        session = self.session()
        connection = self.scalar('SELECT id FROM ai_connections')
        first = self.db.enqueue_job('recap', session, connection, 'actual/model', 'Italian', 'a' * 64, 1000)
        second = self.db.enqueue_job('recap', session, connection, 'actual/model', 'Italian', 'a' * 64, 1000)
        transcribe = self.db.enqueue_job('transcription', session, connection, 'actual/model', 'auto', 'b' * 64, 1000)
        self.assertEqual(self.scalar('SELECT attempt FROM recap_jobs WHERE id=?', (second,)), 2)
        self.assertEqual(self.scalar('SELECT attempt FROM recap_jobs WHERE id=?', (transcribe,)), 1)
        self.db.update_job(first, 'validating', at_ms=1100)
        self.db.update_job(first, 'running', at_ms=1200)
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.update_job(first, 'failed', 'Bearer secret token', 1300)
        self.db.update_job(first, 'completed', at_ms=1400)
        artifact = self.db.register_artifact(session, 'recap_md', self.root / 'recap.md', first, sha256='c' * 64)
        self.assertGreater(artifact, 0)
        with self.assertRaises(storage.DatabaseError):
            self.db.update_job(first, 'running', at_ms=1500)
        self.db.connection.execute('DELETE FROM recap_jobs WHERE id=?', (first,))
        self.assertIsNone(self.scalar('SELECT recap_job_id FROM artifacts WHERE id=?', (artifact,)))


    def test_artifact_cross_call_job_rejected(self):
        self.db.save_ai_settings(self.ai())
        a, b = self.session(), self.session()
        connection = self.scalar('SELECT id FROM ai_connections')
        job = self.db.connection.execute("INSERT INTO recap_jobs(session_id,connection_id,provider_code_snapshot,model_id_snapshot,model_label_snapshot,language,input_hash,attempt,status,queued_at_ms) VALUES(?,?,'openrouter','actual/model','Original name','Italian',?,1,'queued',1000)", (a, connection, 'a' * 64)).lastrowid
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.connection.execute("INSERT INTO artifacts(session_id,recap_job_id,kind,path,state,created_at_ms) VALUES(?,?,'recap_md','wrong.md','ready',1000)", (b, job))


if __name__ == '__main__':
    unittest.main()
