"""Activity, cancellation and retry contracts with temporary data and no network."""
import fcntl
import itertools
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from ai_workflows import WorkflowError
from ascolto_diagnostics import cancel_job, snapshot
from ascolto_runtime import archive_status, handle_request, reconcile_jobs
from ascolto_job_diagnostics import JobDiagnostics, DiagnosticStorageError, read_diagnostics, diagnostic_path
from ascolto_database import Database
import test_integration as fixtures


class DiagnosticsTests(unittest.TestCase):
    setUp = fixtures.IntegrationTests.setUp
    connect = fixtures.IntegrationTests.connect
    session = fixtures.IntegrationTests.session

    def enqueue(self, session, state='failed', code='invalid_evidence'):
        with self.manager.database() as db:
            pref = db.connection.execute('SELECT * FROM recap_preferences').fetchone()
            job = db.enqueue_job('recap',session,pref['connection_id'],pref['model_id'],'Italian','a'*64)
            db.update_job(job,'running')
            if state != 'running':
                db.update_job(job,state,error_code=code)
            return job

    def test_failed_recap_explains_preserved_transcript_without_content(self):
        session = self.session()
        self.enqueue(session)
        result = self.manager.handle({'action':'diagnostics'})['result']
        call = result['calls'][0]
        self.assertEqual(call['status'],'needs_attention')
        self.assertEqual(call['turn_count'],1)
        self.assertEqual(call['operations'][0]['status'],'ready')
        self.assertIn('citation',call['reason'])
        self.assertTrue(any(a['kind']=='transcript_md' for a in call['artifacts']))
        self.assertNotIn('The supported source statement.',json.dumps(result))
        self.assertNotIn(str(self.folder),call['safe_report'])
        self.assertNotIn('placeholder-only-key',json.dumps(result))
        self.assertEqual(result['summary']['needs_attention'],1)

    def test_polling_does_not_read_credentials_or_provider_catalog(self):
        self.session()
        with patch.object(self.store,'get',side_effect=AssertionError('No keychain polling')):
            with patch.object(self.transport,'request',side_effect=AssertionError('No network polling')):
                self.assertTrue(self.manager.handle({'action':'diagnostics'})['ok'])

    def test_attempt_progress_persists_and_is_visible_in_safe_report(self):
        session = self.session()
        result = handle_request(self.manager, {'action': 'recap', 'session_id': session})
        job = result['job_id']
        records = read_diagnostics(self.config, job)
        self.assertEqual(records[-1]['stage'], 'completed')
        self.assertTrue(any(e['stage'] == 'provider_request' for e in records))
        self.assertTrue(any(e['stage'] == 'validation' for e in records))
        self.assertEqual(diagnostic_path(self.config, job).stat().st_mode & 0o777, 0o600)
        call = self.manager.handle({'action': 'diagnostics'})['result']['calls'][0]
        self.assertIn('Chunk 1 of 1', call['attempts'][0]['reason'])
        self.assertIn('progress_events', call['safe_report'])
        self.assertNotIn('The supported source statement.', call['safe_report'])

    def test_diagnostic_metadata_is_allowlisted_and_bounded_on_write_and_read(self):
        self.session()
        writer = JobDiagnostics(self.config, 123)
        for i in range(260):
            writer.record({'stage': 'validation', 'chunk': i, 'total': 260,
                           'quote': 'private content', 'reason': 'private content',
                           'source_index': True})
        records = read_diagnostics(self.config, 123)
        self.assertEqual(len(records), 256)
        self.assertEqual(records[0]['chunk'], 4)
        self.assertNotIn('private content', writer.path.read_text())
        self.assertNotIn('source_index', records[0])
        writer.path.write_text(json.dumps({'version': 1, 'events': [{'at_ms': 1, 'stage': 'private content',
                                                                  'quote': 'private content', 'chunk': -1}]}))
        self.assertEqual(read_diagnostics(self.config, 123), [{'at_ms': 1}])
        writer.path.write_text('partial file')
        self.assertEqual(read_diagnostics(self.config, 123), [])

    def test_failed_repair_surfaces_validation_location_through_native_worker(self):
        session = self.session()
        complete = self.manager.complete
        def malformed(*args, **kwargs):
            raw, model = complete(*args, **kwargs)
            value = json.loads(raw)
            value['summary'][0]['evidence'][0]['quote'] = 'Unsupported private quote.'
            return json.dumps(value), model
        with patch.object(self.manager, 'complete', side_effect=malformed):
            with self.assertRaises(Exception) as error:
                self.manager.handle({'action': 'recap', 'session_id': session})
        self.assertEqual(error.exception.code, 'invalid_evidence')
        self.assertEqual(error.exception.details['section'], 'summary')
        self.assertEqual(error.exception.details['item'], 1)
        job = error.exception.details['job_id']
        call = self.manager.handle({'action': 'diagnostics'})['result']['calls'][0]
        self.assertEqual(call['attempts'][0]['id'], job)
        self.assertIn('quote_not_in_source', call['attempts'][0]['reason'])
        self.assertEqual(call['attempts'][0]['progress']['status'], 'failed')
        self.assertNotIn('Unsupported private quote.', call['safe_report'])
        self.assertEqual(sum(e.get('stage') == 'repair' for e in call['attempts'][0]['progress_events']), 2)

    def test_progress_storage_failure_is_recorded_before_any_provider_request(self):
        session = self.session()
        with patch('ascolto_job_diagnostics.atomic_text', side_effect=OSError('private filesystem detail')):
            with self.assertRaises(DiagnosticStorageError):
                handle_request(self.manager, {'action': 'recap', 'session_id': session})
        with self.manager.database() as db:
            job = db.connection.execute('SELECT status,error_code FROM recap_jobs').fetchone()
            self.assertEqual(tuple(job), ('failed', 'storage_error'))
        self.assertFalse(any(url.endswith('/chat/completions') for url, _ in self.transport.calls))

    def test_artifact_registration_failure_does_not_look_like_completed_progress(self):
        session = self.session()
        with patch.object(Database, 'register_artifact', side_effect=OSError('private filesystem detail')):
            with self.assertRaises(OSError):
                handle_request(self.manager, {'action': 'recap', 'session_id': session})
        call = self.manager.handle({'action': 'diagnostics'})['result']['calls'][0]
        attempt = call['attempts'][0]
        self.assertEqual((attempt['status'], attempt['error_code']), ('failed', 'storage_error'))
        self.assertEqual(attempt['progress']['stage'], 'registering_artifacts')
        self.assertEqual(attempt['progress']['status'], 'failed')
        self.assertNotIn('private filesystem detail', call['safe_report'])

    def test_last_progress_write_failure_keeps_durably_completed_recap(self):
        session = self.session()
        record = JobDiagnostics.record
        def fail_final(writer, event):
            if event.get('stage') == 'completed':
                raise DiagnosticStorageError('Progress write failed.')
            return record(writer, event)
        with patch.object(JobDiagnostics, 'record', fail_final):
            result = handle_request(self.manager, {'action': 'recap', 'session_id': session})
        self.assertTrue((Path(result['session_dir']) / 'recap.md').is_file())
        with self.manager.database() as db:
            self.assertEqual(db.connection.execute('SELECT status FROM recap_jobs').fetchone()[0], 'completed')

    def test_new_empty_call_does_not_hide_completed_final_recap(self):
        session = self.session()
        result = handle_request(self.manager,{'action':'recap'})
        with self.manager.database() as db:
            newer = db.connection.execute("INSERT INTO sessions(started_at_ms,state,capture_backend) VALUES(9999999999999,'completed','test')").lastrowid
            db.publish_exports(newer,self.folder/'empty.md',self.folder/'empty.jsonl')
            archive = archive_status(db)
        self.assertEqual(Path(archive['recap']),Path(result['output_dir'])/'recap.ai.md')
        result = self.manager.handle({'action':'diagnostics'})['result']
        self.assertEqual(result['calls'][0]['status'],'skipped')
        self.assertEqual(next(c for c in result['calls'] if c['id']==session)['status'],'ready')

    def test_cancelled_retry_keeps_previous_valid_recap(self):
        session = self.session()
        original = handle_request(self.manager,{'action':'recap'})
        alias = Path(original['session_dir'])/'recap.md'
        before = alias.read_bytes()
        complete = self.manager.complete
        def cancel_during_response(*args,**kwargs):
            with self.manager.database() as db:
                job = db.connection.execute('SELECT max(id) FROM recap_jobs').fetchone()[0]
                cancel_job(db,self.manager.path,job)
            return complete(*args,**kwargs)
        with patch.object(self.manager,'complete',side_effect=cancel_during_response):
            with self.assertRaises(WorkflowError) as error:
                handle_request(self.manager,{'action':'recap','session_id':session})
        self.assertEqual(error.exception.code,'cancelled')
        self.assertEqual(alias.read_bytes(),before)
        call = self.manager.handle({'action':'diagnostics'})['result']['calls'][0]
        self.assertEqual(call['attempts'][0]['status'],'cancelled')
        self.assertTrue(any(a['kind']=='recap_md' for a in call['artifacts']))
        with self.manager.database() as db:
            self.assertEqual(db.connection.execute('SELECT count(*) FROM artifacts WHERE recap_job_id=(SELECT max(id) FROM recap_jobs)').fetchone()[0],0)

    def test_cancellation_waits_truthfully_for_live_worker(self):
        session = self.session()
        job = self.enqueue(session,'running')
        with (self.folder/f'.session-{session}-recap.lock').open('a') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            result = self.manager.handle({'action':'cancel_job','job_id':job})
            self.assertEqual(result['result']['status'],'cancellation_requested')
            call = self.manager.handle({'action':'diagnostics'})['result']['calls'][0]
            self.assertEqual(call['attempts'][0]['status'],'running')
            self.assertTrue(call['attempts'][0]['cancel_requested'])
            self.assertIsNone(call['attempts'][0]['cancel_request'])
            self.assertIsNone(call['retry_request'])
        with self.manager.database() as db:
            reconcile_jobs(db,self.manager.path)
            self.assertEqual(db.connection.execute('SELECT status FROM recap_jobs WHERE id=?',(job,)).fetchone()[0],'cancelled')

    def test_explicit_retry_targets_selected_session_and_preserves_attempt(self):
        session = self.session()
        previous = self.enqueue(session)
        with self.manager.database() as db:
            newer = db.connection.execute("INSERT INTO sessions(started_at_ms,state,capture_backend) VALUES(9999999999999,'completed','test')").lastrowid
            db.publish_exports(newer,self.folder/'empty.md',self.folder/'empty.jsonl')
            request = next(c for c in snapshot(db,self.manager.path)['calls'] if c['id']==session)['retry_request']
        result = handle_request(self.manager,request)
        with self.manager.database() as db:
            row = db.connection.execute('SELECT session_id,attempt,status FROM recap_jobs WHERE id=?',(result['job_id'],)).fetchone()
            self.assertEqual(tuple(row),(session,2,'completed'))
            self.assertEqual(db.connection.execute('SELECT status FROM recap_jobs WHERE id=?',(previous,)).fetchone()[0],'failed')

    def test_stale_model_selection_cannot_send_retry(self):
        session = self.session()
        with self.assertRaises(WorkflowError) as error:
            handle_request(self.manager,{'action':'recap','session_id':session,'expected_provider':'openrouter','expected_model':'changed/model'})
        self.assertEqual(error.exception.code,'selection_changed')
        self.assertFalse(any(url.endswith('/chat/completions') for url,_ in self.transport.calls))

    def test_invalid_per_attempt_budget_does_not_enqueue_or_send(self):
        session = self.session()
        for budget in (True, 4095, 16777217, '60000'):
            with self.assertRaises(WorkflowError) as error:
                handle_request(self.manager, {'action': 'recap', 'session_id': session, 'input_budget': budget})
            self.assertEqual(error.exception.code, 'invalid_request')
        with self.manager.database() as db:
            self.assertEqual(db.connection.execute('SELECT count(*) FROM recap_jobs').fetchone()[0], 0)

    def test_explicit_budget_uses_same_saved_selection_and_records_the_plan(self):
        session = self.session()
        result = handle_request(self.manager, {'action': 'recap', 'session_id': session, 'input_budget': 60000})
        events = read_diagnostics(self.config, result['job_id'])
        planned = next(e for e in events if 'input_budget' in e)
        # The fixture catalog context (32000) still caps the larger request.
        self.assertLess(planned['input_budget'], 16000)
        self.assertEqual(planned['total'], 1)

    def test_duplicate_request_fails_without_waiting_or_queueing(self):
        session = self.session()
        with (self.folder/f'.session-{session}-recap.lock').open('a') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            with self.assertRaises(WorkflowError) as error:
                handle_request(self.manager,{'action':'recap','session_id':session})
        self.assertEqual(error.exception.code,'task_busy')
        self.assertFalse(any(url.endswith('/chat/completions') for url,_ in self.transport.calls))

    def test_failed_new_attempt_keeps_ready_recap_visible(self):
        session = self.session()
        handle_request(self.manager,{'action':'recap'})
        self.enqueue(session)
        call = self.manager.handle({'action':'diagnostics'})['result']['calls'][0]
        self.assertEqual(call['status'],'needs_attention')
        self.assertTrue(any(a['kind']=='recap_md' for a in call['artifacts']))
        self.assertEqual(len(call['attempts']),2)

    def test_provisional_artifact_does_not_replace_final_in_activity(self):
        self.session()
        handle_request(self.manager,{'action':'recap'})
        handle_request(self.manager,{'action':'provisional_recap'})
        call = self.manager.handle({'action':'diagnostics'})['result']['calls'][0]
        labels = [a['label'] for a in call['artifacts']]
        self.assertIn('Open recap',labels)
        self.assertIn('Open provisional recap',labels)

    def test_active_session_explains_why_final_work_is_not_requested(self):
        self.session(state='recording')
        call = self.manager.handle({'action':'diagnostics'})['result']['calls'][0]
        self.assertEqual(call['status'],'running')
        self.assertEqual(call['operations'][0]['status'],'running')
        self.assertEqual(call['operations'][1]['status'],'not_requested')
        self.assertIsNone(call['retry_request'])

    def test_pending_export_does_not_claim_current_revision_is_saved(self):
        session = self.session()
        with self.manager.database() as db:
            db.queue_export(session)
        call = self.manager.handle({'action':'diagnostics'})['result']['calls'][0]
        self.assertEqual(call['operations'][0]['status'],'needs_attention')
        self.assertIn('earlier revision',call['operations'][0]['reason'])
        self.assertIsNone(call['retry_request'])

    def test_invalid_cancel_identifiers_never_create_markers(self):
        for value in (True,-1,'../escape',None):
            with self.assertRaises(Exception):
                self.manager.handle({'action':'cancel_job','job_id':value})
        self.assertFalse(list(self.folder.glob('.job-*.cancel')))

    def test_historical_error_has_no_fabricated_quotation(self):
        session = self.session()
        job = self.enqueue(session)
        call = self.manager.handle({'action':'diagnostics'})['result']['calls'][0]
        self.assertEqual(call['attempts'][0]['id'],job)
        report = json.loads(call['safe_report'])
        self.assertIn('unavailable',report['evidence_note'])
        self.assertEqual([e['status'] for e in call['attempts'][0]['events']],['queued','running','failed'])

    def test_missing_completed_output_is_not_reported_as_ready(self):
        self.session()
        result = handle_request(self.manager,{'action':'recap'})
        (Path(result['output_dir'])/'recap.ai.md').unlink()
        call = self.manager.handle({'action':'diagnostics'})['result']['calls'][0]
        self.assertEqual(call['operations'][1]['status'],'needs_attention')
        self.assertIn('no longer readable',call['operations'][1]['reason'])
        self.assertFalse(any(a['kind']=='recap_md' for a in call['artifacts']))

    def test_cancellation_wins_when_provider_returns_error_after_request(self):
        session = self.session()
        def cancelled_error(*args,**kwargs):
            with self.manager.database() as db:
                job = db.connection.execute('SELECT max(id) FROM recap_jobs').fetchone()[0]
                cancel_job(db,self.manager.path,job)
            raise WorkflowError('network_error','Mock network error')
        with patch.object(self.manager,'complete',side_effect=cancelled_error):
            with self.assertRaises(WorkflowError) as error:
                handle_request(self.manager,{'action':'recap','session_id':session})
        self.assertEqual(error.exception.code,'cancelled')
        with self.manager.database() as db:
            self.assertEqual(db.connection.execute('SELECT status FROM recap_jobs ORDER BY id DESC LIMIT 1').fetchone()[0],'cancelled')

    def test_failed_provisional_attempt_remains_visible_as_separate_workflow(self):
        session = self.session()
        job = self.enqueue(session)
        with self.manager.database() as db:
            db.connection.execute("UPDATE recap_jobs SET phase='provisional' WHERE id=?",(job,))
        call = self.manager.handle({'action':'diagnostics'})['result']['calls'][0]
        provisional = next(o for o in call['operations'] if o['task']=='recap_provisional')
        self.assertEqual(provisional['status'],'needs_attention')
        self.assertEqual(call['status'],'needs_attention')

    def test_multichunk_processing_keeps_first_request_start_time(self):
        session = self.session()
        with self.manager.database() as db:
            for i in range(60):
                db.append_utterance(session,'MIC',1000+i*1000,1800+i*1000,'A supported statement. '*12,'ME','en')
            db.publish_exports(session,self.folder/'call.md',self.folder/'call.jsonl')
        starts = []
        complete = self.manager.complete
        def observe(*args,**kwargs):
            with self.manager.database() as db:
                starts.append(db.connection.execute('SELECT started_at_ms FROM recap_jobs ORDER BY id DESC LIMIT 1').fetchone()[0])
            return complete(*args,**kwargs)
        with patch('ascolto_database.now_ms',side_effect=itertools.count(2000000000000,1000)):
            with patch.object(self.manager,'complete',side_effect=observe):
                handle_request(self.manager,{'action':'recap','session_id':session})
        self.assertGreater(len(starts),1)
        self.assertEqual(len(set(starts)),1)


if __name__ == '__main__':
    unittest.main()
