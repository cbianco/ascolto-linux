"""Actual provider/database/workflow wiring with temporary data and mock transport."""
import copy
import fcntl
import hashlib
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from ai_providers import ProviderManager, ProviderError
from ai_workflows import WorkflowError
from ascolto_database import Database, DatabaseError, now_ms
from ascolto_runtime import bootstrap, handle_request, recover_exports
from test_ai_providers import MemoryStore
from test_ai_workflows import write_audio


class Transport:
    def __init__(self):
        self.calls = []
        self.fail = False
        self.text_models = [{'id': 'fixture/text', 'name': 'Text', 'architecture': {'input_modalities': ['text'], 'output_modalities': ['text']}, 'context_length': 32000}]
        self.speech_models = [{'id': 'fixture/stt', 'name': 'Speech', 'architecture': {'input_modalities': ['audio'], 'output_modalities': ['transcription']}, 'supported_parameters': ['language']}]

    def request(self, url, token=None, body=None, **kwargs):
        self.calls.append((url, body))
        if self.fail and body:
            raise ProviderError('usage_limit', 'The selected provider reports a usage limit.')
        if '?output_modalities=transcription' in url:
            return {'data': copy.deepcopy(self.speech_models)}
        if url.endswith('/models'):
            return {'data': copy.deepcopy(self.text_models)}
        if url.endswith('/key'):
            return {}
        if url.endswith('/audio/transcriptions'):
            return {'text': 'Recovered complete audio.'}
        if url.endswith('/chat/completions'):
            prompt = json.loads(body['messages'][-1]['content'])
            turn = prompt['transcript'][0]
            value = {'schema_version': 1, 'summary': [{'text': 'A supported summary.', 'evidence': [{'source_id': turn['source_id'], 'quote': turn['text']}]}], 'decisions': [], 'actions': [], 'open_questions': []}
            return {'choices': [{'finish_reason': 'stop', 'message': {'content': json.dumps(value)}}]}
        raise AssertionError(url)


class IntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='ascolto-integration-')
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name)
        self.config = self.folder/'config.json'
        self.store, self.transport = MemoryStore(), Transport()
        self.manager = ProviderManager(self.config, self.store, self.transport)

    def connect(self):
        self.manager.handle({'action': 'connect', 'provider': 'openrouter', 'key': 'placeholder-only-key'})
        self.manager.handle({'action': 'use', 'provider': 'openrouter', 'model': 'fixture/text'})

    def session(self, state='completed', audio=False):
        self.connect()
        with self.manager.database() as db:
            cursor = db.connection.execute("INSERT INTO sessions(started_at_ms,state,capture_backend) VALUES(?,?,'coreaudio')", (now_ms(), state))
            session = cursor.lastrowid
            db.append_utterance(session, 'MIC', 100, 900, 'The supported source statement.', 'ME', 'en')
            md, trace = self.folder/'call.md', self.folder/'call.jsonl'
            db.publish_exports(session, md, trace)
            if audio:
                archive = self.folder/'call.wav'
                write_audio(archive, seconds=2, channels=2)
                db.register_artifact(session, 'audio_m4a', archive)
        return session

    def select_stt(self, enabled=False, consent=True):
        self.manager.handle({'action': 'refresh_transcription_models'})
        return self.manager.handle({'action': 'transcription_preferences', 'provider': 'openrouter', 'model': 'fixture/stt', 'enabled': enabled, 'cloud_audio_opt_in': consent})

    def test_bootstrap_adopts_native_config_keywords_and_metadata_once(self):
        output = self.folder/'archive'; output.mkdir()
        self.config.write_text(json.dumps({'save_audio': False, 'language': 'it', 'output_dir': str(output), 'mic_device_id': 94, 'cloud_recap': True}))
        (self.folder/'keywords.txt').write_text('Cristian\nWhistle\n')
        ai = self.folder/'ai-providers.json'
        ai.write_text(json.dumps({'version': 1, 'host_id': 'urn:uuid:preserved-host', 'profiles': {}, 'accounts': [], 'automatic_recaps': False}))
        original = {p: p.read_bytes() for p in (self.config, ai, self.folder/'keywords.txt')}
        # A provider-created DB with default settings must not suppress native adoption.
        self.manager.load()
        namespace = self.manager.namespace
        bootstrap(self.config, 'macos'); bootstrap(self.config, 'macos')
        with self.manager.database() as db:
            self.assertEqual(db.settings()['transcription_language'], 'it')
            self.assertEqual(db.settings()['mic_device_ref'], '94')
            self.assertEqual(db.settings()['oauth_host_id'], 'urn:uuid:preserved-host')
            self.assertEqual(db.connection.execute('SELECT count(*) FROM bootstrap_state').fetchone()[0], 1)
            self.assertFalse(db.load_ai_settings()['automatic_recaps'])
        self.assertEqual(namespace, hashlib.sha256(str(ai.resolve()).encode()).hexdigest()[:24])
        for path, data in original.items():
            self.assertEqual(path.read_bytes(), data)
            backup = next(self.folder.glob(path.name+'.ascolto-backup.*'))
            self.assertEqual(backup.read_bytes(), data)
            self.assertEqual(backup.stat().st_mode & 0o777, 0o600)
        # Later changes to legacy files do not override authoritative settings.
        self.config.write_text('{}'); bootstrap(self.config, 'macos')
        with self.manager.database() as db:
            self.assertEqual(db.settings()['transcription_language'], 'it')

    def test_linux_ini_adoption_and_explicit_session_scope(self):
        config = self.folder/'config.ini'
        output = self.folder/'archive'; output.mkdir()
        config.write_text(f'auto_mic_trigger=false\noutput_dir={output}\nmic_device=alsa_input.fixture\nkeywords=Alpha,Beta\n')
        fixtures = self.folder/'fixtures'; fixtures.mkdir()
        (fixtures/'2026-10-15_10-20-30_call.jsonl').write_text('invalid fixture must never be scanned')
        bootstrap(config, 'linux')
        with Database(config.with_name('ascolto.sqlite3')) as db:
            self.assertFalse(db.settings()['auto_monitoring'])
            self.assertEqual(db.settings()['mic_device_ref'], 'alsa_input.fixture')
            self.assertEqual(db.connection.execute('SELECT count(*) FROM sessions').fetchone()[0], 0)
            self.assertEqual(db.connection.execute('SELECT count(*) FROM keywords').fetchone()[0], 2)

    def test_catalog_and_task_choices_survive_refresh_restart_disconnect(self):
        self.connect(); self.select_stt(enabled=True)
        self.manager.handle({'action': 'refresh_models', 'provider': 'openrouter'})
        restarted = ProviderManager(self.config, self.store, self.transport)
        state = restarted.handle({'action': 'status'})['settings']
        self.assertEqual(state['transcription']['model'], 'fixture/stt')
        self.assertTrue(state['transcription']['enabled'])
        self.assertEqual(state['profiles']['openrouter']['models'][0]['context_length'], 32000)
        self.assertEqual(state['profiles']['openrouter']['transcription_models'][0]['input_modalities'], ['audio'])
        self.assertFalse(self.manager.path.exists())
        self.transport.speech_models = []
        restarted.handle({'action': 'refresh_transcription_models'})
        self.assertFalse(restarted.load()['transcription']['enabled'])
        self.assertEqual(restarted.load()['selected_provider'], 'openrouter')
        restarted.handle({'action': 'disconnect', 'provider': 'openrouter'})
        self.assertFalse(restarted.load()['automatic_recaps'])

    def test_provisional_does_not_suppress_final_and_auto_sends_once_without_audio(self):
        session = self.session(state='recording')
        self.manager.handle({'action': 'preferences', 'automatic_recaps': True})
        provisional = handle_request(self.manager, {'action': 'provisional_recap'})
        with self.manager.database() as db:
            db.connection.execute("UPDATE sessions SET state='completed' WHERE id=?", (session,))
        final = handle_request(self.manager, {'action': 'recap', 'automatic': True})
        count = len([url for url, body in self.transport.calls if url.endswith('/chat/completions')])
        handle_request(self.manager, {'action': 'recap', 'automatic': True})
        self.assertEqual(len([url for url, body in self.transport.calls if url.endswith('/chat/completions')]), count)
        retry = handle_request(self.manager, {'action': 'recap'})
        self.assertNotEqual(provisional['output_dir'], final['output_dir'])
        self.assertNotEqual(final['output_dir'], retry['output_dir'])
        with self.manager.database() as db:
            jobs = list(db.connection.execute('SELECT phase,status,input_hash FROM recap_jobs ORDER BY id'))
            self.assertEqual([(j['phase'], j['status']) for j in jobs], [('provisional', 'completed'), ('final', 'completed'), ('final', 'completed')])
            self.assertEqual(db.connection.execute('SELECT count(*) FROM artifacts WHERE recap_job_id IS NOT NULL').fetchone()[0], 6)
            self.assertEqual(len(jobs[0]['input_hash']), 64)
            self.assertEqual([r[0] for r in db.connection.execute('SELECT status FROM job_events WHERE job_id=1 ORDER BY id')], ['queued','validating','running','completed'])
        self.assertTrue((Path(provisional['output_dir'])/'recap.provisional.ai.md').exists())
        self.assertTrue((self.folder/'call.jsonl').exists())
        self.assertFalse(list(self.folder.glob('*.wav')))

    def test_recaps_stay_with_their_call_and_preserve_attempts_after_output_change(self):
        self.session()
        archive = (self.folder/'archive').resolve()
        archive.mkdir()
        with self.manager.database() as db:
            db.update_settings({'output_dir': str(archive)})
        first = handle_request(self.manager, {'action': 'recap'})
        first_path = Path(first['output_dir'])/'recap.ai.md'
        original = first_path.read_bytes()
        self.assertEqual(first_path.parent.parent.name, 'Recap-history')
        self.assertTrue((Path(first['session_dir'])/'recap.md').is_file())
        self.assertTrue((Path(first['session_dir'])/'Data'/'recap.json').is_file())
        with self.manager.database() as db:
            db.update_settings({'output_dir': str(self.folder/'new-archive')})
        second = handle_request(self.manager, {'action': 'recap'})
        self.assertEqual(first['session_dir'], second['session_dir'])
        opened = handle_request(self.manager, {'action':'archive_path','kind':'recap'})
        self.assertTrue(Path(opened['open_path']).is_file())
        self.assertEqual(first_path.read_bytes(), original)
        with self.manager.database() as db:
            self.assertEqual(db.connection.execute('SELECT path FROM artifacts WHERE recap_job_id=? AND kind=?', (first['job_id'], 'recap_md')).fetchone()[0], str(first_path))

    def test_full_audio_plan_consent_and_job_artifact_persistence(self):
        self.session(audio=True); self.select_stt(consent=False)
        with self.assertRaises(WorkflowError) as error:
            handle_request(self.manager, {'action': 'transcription'})
        self.assertEqual(error.exception.code, 'audio_consent_required')
        self.assertFalse(any(url.endswith('/audio/transcriptions') for url, body in self.transport.calls))
        self.select_stt(enabled=False, consent=True)
        plan = handle_request(self.manager, {'action': 'plan_transcription'})
        self.assertEqual(plan['requests'], 2)
        result = handle_request(self.manager, {'action': 'transcription'})
        records = [json.loads(line) for line in (Path(result['output_dir'])/'transcript.cloud.ai.jsonl').read_text().splitlines()]
        self.assertEqual({r['speaker'] for r in records}, {'ME', 'UNKNOWN'})
        self.assertEqual({r['channel'] for r in records}, {'MIC', 'SYS'})
        self.assertTrue(all(r['end_s'] == 2 for r in records))
        with self.manager.database() as db:
            self.assertEqual(db.connection.execute('SELECT status FROM recap_jobs').fetchone()[0], 'completed')
            self.assertEqual(db.connection.execute('SELECT count(*) FROM artifacts WHERE recap_job_id IS NOT NULL').fetchone()[0], 2)
            self.assertEqual(db.task_selection('recap')['model'], 'fixture/text')

    def test_failed_provider_job_retains_originals_and_safe_error(self):
        self.session(); original = (self.folder/'call.jsonl').read_bytes()
        self.transport.fail = True
        with self.assertRaises(ProviderError):
            handle_request(self.manager, {'action': 'recap'})
        with self.manager.database() as db:
            row = db.connection.execute('SELECT status,error_code FROM recap_jobs').fetchone()
            self.assertEqual(tuple(row), ('failed', 'usage_limit'))
            self.assertEqual(db.connection.execute('SELECT count(*) FROM artifacts WHERE recap_job_id IS NOT NULL').fetchone()[0], 0)
        self.assertEqual((self.folder/'call.jsonl').read_bytes(), original)

    def test_export_recovery_preserves_sample_duration_metadata_and_statistics(self):
        session = self.session()
        with self.manager.database() as db:
            db.connection.execute("UPDATE sessions SET ended_at_ms=started_at_ms+99000,metadata_json=? WHERE id=?", (json.dumps({'duration_s': 3.8, 'calendar_notes': 'Agenda Ω', 'capture_dropped_samples': 60800}), session))
            db.connection.execute("UPDATE utterances SET metadata_json=? WHERE session_id=?", (json.dumps({'rms_db': -27, 'ttft_ms': 123}), session))
            db.queue_export(session)
            recover_exports(db)
            self.assertEqual(db.connection.execute('SELECT count(*) FROM export_outbox').fetchone()[0], 0)
        events = [json.loads(line) for line in (self.folder/'call.jsonl').read_text().splitlines()]
        self.assertEqual(events[-1]['duration_s'], 3.8)
        self.assertEqual(events[-1]['capture_dropped_samples'], 60800)
        self.assertEqual(events[0]['calendar_notes'], 'Agenda Ω')
        self.assertEqual(events[1]['rms_db'], -27)

    def test_source_ids_survive_a_late_earlier_utterance(self):
        from ai_workflows import load_turns
        session=self.session(state='recording')
        before=load_turns(self.folder/'call.jsonl')[0]
        with self.manager.database() as db:
            db.append_utterance(session,'SYS',0,50,'An earlier remote fragment.','SPEAKER_1','en')
            db.publish_exports(session,self.folder/'call.md',self.folder/'call.jsonl')
        after=next(turn for turn in load_turns(self.folder/'call.jsonl') if turn.text==before.text)
        self.assertEqual(before.source_id,after.source_id)
        self.assertTrue(after.source_id.startswith(f'db:{session}:'))

    def test_orphan_jobs_fail_without_retry_while_live_lock_is_preserved(self):
        session = self.session()
        with self.manager.database() as db:
            pref = db.connection.execute('SELECT * FROM recap_preferences').fetchone()
            job = db.enqueue_job('recap', session, pref['connection_id'], 'fixture/text', 'English', 'a'*64)
            db.update_job(job, 'running')
        path = self.folder/f'.session-{session}-recap.lock'
        with path.open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            state = self.manager.handle({'action': 'status'})['settings']
            self.assertEqual(state['jobs'][0]['status'], 'running')
        state = self.manager.handle({'action': 'status'})['settings']
        self.assertEqual(state['jobs'][0]['status'], 'failed')
        self.assertEqual(state['jobs'][0]['error_code'], 'interrupted')
        self.assertFalse(any(url.endswith('/chat/completions') for url, body in self.transport.calls))

    def test_corrupt_future_or_unrelated_database_never_resets(self):
        path = self.folder/'ascolto.sqlite3'
        path.write_bytes(b'not a database')
        with self.assertRaises(Exception): bootstrap(self.config, 'macos')
        self.assertEqual(path.read_bytes(), b'not a database')


if __name__ == '__main__':
    unittest.main()
