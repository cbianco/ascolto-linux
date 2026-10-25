"""Content-free activity snapshots and cooperative AI job cancellation."""
from datetime import datetime
import json
import os
from pathlib import Path
import stat

from ascolto_database import now_ms
from ascolto_job_diagnostics import read_diagnostics, describe

ACTIVE = ('queued', 'validating', 'running')
REASONS = {
    'invalid_evidence': 'A generated citation did not match the saved transcript. Recap publication was blocked.',
    'invalid_deadline': 'A generated action deadline was not quoted in its evidence. Recap publication was blocked.',
    'invalid_owner': 'A generated action owner did not match a cited speaker. Recap publication was blocked.',
    'invalid_output': 'The provider returned an invalid or incomplete result. Existing artifacts were kept.',
    'unexpected_model': 'The provider returned a different model. No fallback was accepted.',
    'authentication_failed': 'The selected provider rejected the credential. Review the connection in Settings.',
    'not_connected': 'The provider is disconnected. Review the connection in Settings.',
    'permission_denied': 'The account does not have permission for this request. Review provider access.',
    'usage_limit': 'The provider reported a usage limit. Review the account before retrying.',
    'network_error': 'The provider could not be reached. Check the connection before retrying.',
    'provider_timeout': 'The selected provider timed out during generation. The saved transcript is intact; retry explicitly.',
    'provider_unavailable': 'The selected provider is temporarily unavailable or overloaded. No provider or model fallback was used.',
    'provider_error': 'The selected provider returned an error. Review the recorded upstream status and error type.',
    'incomplete_response': 'The provider did not complete the response. Review the recorded finish reason before retrying.',
    'empty_response': 'The provider returned no recap text. The saved transcript and previous recap are intact.',
    'invalid_response': 'The provider returned a malformed response. Review the recorded response-shape metadata; local artifacts are intact.',
    'model_unavailable': 'The selected model is unavailable. Refresh the catalog and explicitly choose a model.',
    'audio_consent_required': 'Saved audio upload is not authorized. Review Transcription settings.',
    'provider_policy_acknowledgement_required': 'Review and acknowledge this provider policy in Settings.',
    'interrupted': 'The worker ended before completion. A new attempt must be requested explicitly.',
    'cancelled': 'Local processing was cancelled. A provider may have continued an already received request.',
    'storage_error': 'The task could not save its output. Check storage and permissions.',
    'empty_source': 'The saved transcript contains no speech records.',
    'invalid_source': 'The required saved input is unavailable or has not finished saving.',
    'request_limit': 'The complete input exceeds the configured request limit. No silent truncation is allowed.',
    'turn_too_large': 'A speech turn exceeds the selected model input budget. Review the model context size.',
    'context_too_small': 'The selected model context is too small for the evidence schema.',
    'audio_decode_failed': 'The saved channel audio could not be decoded.',
    'audio_timeline_mismatch': 'The decoded channels do not preserve the original audio timeline.',
}


def error_reason(code):
    return REASONS.get(code, 'The task failed. Review its recorded error code and connection or storage settings.')


def cancellation_path(config, job):
    return Path(config).with_name(f'.job-{int(job)}.cancel')


def cancellation_requested(config, job):
    path = cancellation_path(config, job)
    try:
        info = path.lstat()
        return stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid()
    except FileNotFoundError:
        return False


def clear_cancellation(config, job):
    # The terminal database row retains the outcome. Do not leave a stale
    # marker that could affect an ID reused after explicit history deletion.
    if cancellation_requested(config, job):
        try:
            cancellation_path(config, job).unlink()
        except OSError:
            pass


def cancel_job(db, config, job):
    from ai_workflows import WorkflowError
    if type(job) is not int or job <= 0:
        raise WorkflowError('invalid_request', 'Choose an existing job to cancel.')
    # Serialize the request against final artifact publication, not network I/O.
    with db.transaction():
        row = db.connection.execute('SELECT status FROM recap_jobs WHERE id=?', (job,)).fetchone()
        if not row:
            raise WorkflowError('not_available', 'This job is not available.')
        if row[0] not in ACTIVE:
            return {'job_id': job, 'status': row[0], 'message': 'The attempt has already finished.'}
        path = cancellation_path(config, job)
        try:
            descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0), 0o600)
        except FileExistsError:
            if not cancellation_requested(config, job):
                raise WorkflowError('storage_error', 'The cancellation marker could not be created safely.') from None
        else:
            os.close(descriptor)
    return {'job_id': job, 'status': 'cancellation_requested',
            'message': 'Cancellation requested. Waiting for the current provider request to return; remote processing may continue.'}


class CancellableManager:
    """Stop before and after provider requests, without killing an unrelated PID."""
    def __init__(self, manager, config, job):
        self.manager, self.config, self.job = manager, config, job

    def check(self):
        if cancellation_requested(self.config, self.job):
            from ai_workflows import WorkflowError
            raise WorkflowError('cancelled', REASONS['cancelled'])

    def __getattr__(self, name):
        return getattr(self.manager, name)

    def complete(self, *args, **kwargs):
        self.check()
        try:
            result = self.manager.complete(*args, **kwargs)
        except Exception:
            self.check()
            raise
        self.check()
        return result

    @property
    def transport(self):
        owner = self
        class Transport:
            def request(self, *args, **kwargs):
                owner.check()
                try:
                    result = owner.manager.transport.request(*args, **kwargs)
                except Exception:
                    owner.check()
                    raise
                owner.check()
                return result
        return Transport()


def _job(db, row, stamp, config):
    item = {key: row[key] for key in ('id', 'task', 'phase', 'attempt', 'status', 'error_code', 'queued_at_ms', 'started_at_ms', 'finished_at_ms')}
    item.update(provider=row['provider_code_snapshot'], model=row['model_id_snapshot'], model_label=row['model_label_snapshot'])
    item['elapsed_ms'] = max(0, (row['finished_at_ms'] or stamp) - (row['started_at_ms'] or row['queued_at_ms']))
    item['cancel_requested'] = row['status'] in ACTIVE and cancellation_requested(config, row['id'])
    item['reason'] = (error_reason(row['error_code']) if row['status'] in ('failed', 'cancelled') else
                      'Cancellation requested; waiting for the current provider request to return.' if item['cancel_requested'] else
                      'Validated output is saved.' if row['status'] == 'completed' else
                      'Checking the input and selected model.' if row['status'] == 'validating' else
                      'Provider workflow is running. Stage-level timing is unavailable.' if row['status'] == 'running' else
                      'The request is queued.')
    item['events'] = [dict(r) for r in db.connection.execute('SELECT status,at_ms,error_code FROM job_events WHERE job_id=? ORDER BY id', (row['id'],))]
    item['progress_events'] = read_diagnostics(config, row['id'])
    for event in item['progress_events']:
        event['description'] = describe(event)
    item['progress'] = item['progress_events'][-1] if item['progress_events'] else None
    if item['progress']:
        item['reason'] += ' Last recorded progress: ' + describe(item['progress'])
        if row['status'] in ACTIVE:
            item['stage_elapsed_ms'] = max(0, stamp - item['progress']['at_ms'])
            item['reason'] += f" Current stage elapsed: {item['stage_elapsed_ms'] / 1000:.1f} s."
    item['cancel_request'] = {'action': 'cancel_job', 'job_id': row['id']} if row['status'] in ACTIVE and not item['cancel_requested'] else None
    return item


def snapshot(db, config):
    """Build metadata only: never read transcript content, credentials or titles."""
    stamp = now_ms()
    recap_pref = dict(db.connection.execute('SELECT * FROM recap_preferences WHERE id=1').fetchone())
    cloud_pref = dict(db.connection.execute('SELECT * FROM transcription_preferences WHERE id=1').fetchone())
    try:
        selection = db.task_selection('recap')
    except Exception:
        selection = None
    calls = []
    for session in db.connection.execute('SELECT id,started_at_ms,ended_at_ms,state FROM sessions ORDER BY started_at_ms DESC,id DESC'):
        sid = session['id']
        count = db.connection.execute('SELECT count(*) FROM utterances WHERE session_id=?', (sid,)).fetchone()[0]
        artifacts = []
        raw = list(db.connection.execute("SELECT a.kind,a.path,a.recap_job_id,j.phase,j.status job_status FROM artifacts a LEFT JOIN recap_jobs j ON j.id=a.recap_job_id WHERE a.session_id=? AND a.state='ready' ORDER BY a.id DESC", (sid,)))
        kinds = {}
        labels = {'transcript_md': 'Open transcript', 'transcript_jsonl': 'Transcript data', 'recap_md': 'Open recap', 'cloud_md': 'Open cloud transcript', 'audio_m4a': 'Open recording', 'audio_mic_flac': 'Open microphone recording', 'audio_sys_flac': 'Open system recording'}
        for artifact in raw:
            kind = artifact['kind']
            if artifact['recap_job_id'] and artifact['job_status'] != 'completed':
                continue
            identity = 'recap_provisional_md' if kind == 'recap_md' and artifact['phase'] == 'provisional' else kind
            if identity not in kinds and Path(artifact['path']).is_file():
                kinds[identity] = artifact['path']
                if kind in labels:
                    label = 'Open provisional recap' if kind == 'recap_md' and artifact['phase'] == 'provisional' else labels[kind]
                    artifacts.append(dict(label=label,kind=kind,path=artifact['path'],is_directory=False))
        md = kinds.get('transcript_md')
        if md:
            path = Path(md)
            folder = path.parent if path.name == 'transcript.md' else path.with_suffix('')
            if folder.is_dir():
                artifacts.insert(0, dict(label='Open call folder',kind='call_directory',path=str(folder),is_directory=True))
        attempts = [_job(db,r,stamp,config) for r in db.connection.execute('SELECT * FROM recap_jobs WHERE session_id=? ORDER BY id DESC', (sid,))]
        operations = []
        live = session['state'] in ('recording', 'paused', 'stopping')
        pending_export = bool(db.connection.execute('SELECT 1 FROM export_outbox WHERE session_id=?', (sid,)).fetchone())
        empty = not count and session['state'] == 'completed' and not pending_export
        transcript_status = 'running' if live else 'skipped' if empty else 'needs_attention' if pending_export or not md else 'ready'
        transcript_reason = ('The session is still saving.' if session['state'] == 'stopping' else
                             'Capture is paused; committed speech remains available.' if session['state'] == 'paused' else
                             'Capture and local transcription are active.' if live else
                             'Transcript export is pending. Existing ready files may contain an earlier revision.' if pending_export else
                             'No speech records were committed for this completed session.' if empty else
                             'The saved transcript is unavailable.' if not md else
                             f'{count} committed speech turns. The local transcript is saved.')
        operations.append(dict(task='transcript', name='Local transcript', status=transcript_status, reason=transcript_reason))
        if session['state'] == 'failed':
            operations.append(dict(task='session', name='Session', status='needs_attention',
                                   reason='The session ended unsuccessfully. Its exact cause is not retained in the activity metadata.'))
        for task, name, pref_key, pref in [('recap','Final recap','auto_generate',recap_pref),('transcription','Cloud transcription','enabled',cloud_pref)]:
            final = next((j for j in attempts if j['task']==task and j['phase']=='final'),None)
            if final:
                status = 'needs_attention' if final['status']=='failed' else 'cancelled' if final['status']=='cancelled' else 'ready' if final['status']=='completed' else final['status']
                reason = final['reason']
                if final['status'] == 'completed':
                    output_kind = 'recap_md' if task == 'recap' else 'cloud_md'
                    readable = any(a['recap_job_id'] == final['id'] and a['kind'] == output_kind and Path(a['path']).is_file() for a in raw)
                    if not readable:
                        status = 'needs_attention'
                        reason = 'This attempt completed, but its saved output is no longer readable. Earlier ready artifacts, if any, remain available.'
            elif task == 'recap' and not count and not live:
                status,reason = 'skipped','No speech records were committed. No recap input is available.'
            elif live:
                status,reason = 'not_requested','Post-call work has not been requested for this active session.'
            else:
                status = 'not_requested'
                reason = f"No request is recorded. Automatic {name.lower()} is currently {'on' if pref[pref_key] else 'off'}."
            operations.append(dict(task=task,name=name,status=status,reason=reason))
        provisional = next((j for j in attempts if j['task']=='recap' and j['phase']=='provisional'),None)
        if provisional:
            status = 'needs_attention' if provisional['status']=='failed' else 'ready' if provisional['status']=='completed' else provisional['status']
            operations.append(dict(task='recap_provisional',name='Provisional recap',status=status,reason=provisional['reason']))
        active = any(j['status'] in ACTIVE for j in attempts)
        attention = any(o['status']=='needs_attention' for o in operations) or session['state']=='failed'
        status = 'running' if active or live else 'needs_attention' if attention else 'ready' if count else 'skipped'
        outcome = ('Processing in progress' if active else 'Saving session' if session['state']=='stopping' else 'Recording paused' if session['state']=='paused' else 'Recording in progress' if live else 'Call needs attention' if attention else 'Transcript and recap ready' if any(j['task']=='recap' and j['phase']=='final' and j['status']=='completed' for j in attempts) else 'Transcript saved' if count else 'No speech detected')
        reason = next((o['reason'] for o in operations if o['status']=='needs_attention'),transcript_reason)
        retry, retry_reason = None, ''
        if not live and count and md and kinds.get('transcript_jsonl') and not pending_export and not any(j['task']=='recap' and j['status'] in ACTIVE for j in attempts):
            if selection:
                retry = dict(action='recap',session_id=sid,expected_provider=selection['provider'],expected_model=selection['model'])
                retry_reason = f"A new final recap attempt will send the saved transcript to {selection['provider']} / {selection['model']}."
            else:
                retry_reason = 'Select an available recap connection and model in Settings first.'
        elif active:
            retry_reason = 'Wait for the current recap attempt to finish or cancel it.'
        elif live:
            retry_reason = 'Wait until the session finishes saving.'
        else:
            retry_reason = 'A nonempty, fully exported local transcript is required.'
        label = datetime.fromtimestamp(session['started_at_ms']/1000).astimezone().strftime('%d %b %Y · %H:%M:%S')
        call = dict(id=sid,label=label,started_at_ms=session['started_at_ms'],state=session['state'],status=status,outcome=outcome,reason=reason,turn_count=count,needs_attention=attention,operations=operations,artifacts=artifacts,attempts=attempts,retry_request=retry,retry_reason=retry_reason)
        # Explicit allowlist excludes paths, app/calendar labels, content and identity.
        report = {k:call[k] for k in ('id','started_at_ms','state','status','turn_count')}
        report['operations'] = [{k:o[k] for k in ('task','status')} for o in operations]
        report['attempts'] = [{k:j[k] for k in ('id','task','phase','attempt','status','provider','model','error_code','queued_at_ms','started_at_ms','finished_at_ms','elapsed_ms','progress_events')} for j in attempts]
        report['evidence_note'] = 'Failed raw responses and exact mismatched quotations are unavailable.'
        call['safe_report'] = json.dumps(report,ensure_ascii=False,indent=2)
        calls.append(call)
    return dict(generated_at_ms=stamp,summary=dict(total_calls=len(calls),needs_attention=sum(c['needs_attention'] for c in calls),running=sum(c['status']=='running' for c in calls),automatic_recaps=bool(recap_pref['auto_generate'])),calls=calls)
