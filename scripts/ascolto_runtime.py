#!/usr/bin/env python3
"""Background bootstrap, recovery and persisted AI task orchestration."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import sys

from ascolto_database import Database, DatabaseError, now_ms
from ascolto_files import atomic_text


def session_directory(db, session):
    source = db.connection.execute("SELECT path FROM artifacts WHERE session_id=? AND kind='transcript_md' AND state='ready'", (session,)).fetchone()
    if not source:
        raise DatabaseError('The session has no saved transcript')
    path = Path(source[0])
    return path.parent if path.name == 'transcript.md' else path.with_suffix('')


def archive_status(db):
    result = {'directory': db.settings()['output_dir']}
    row = db.connection.execute("SELECT session_id,path FROM artifacts WHERE kind='transcript_md' AND state='ready' ORDER BY (SELECT started_at_ms FROM sessions WHERE id=session_id) DESC,session_id DESC LIMIT 1").fetchone()
    if row:
        result.update(transcript=row['path'], call_directory=str(session_directory(db, row['session_id'])))
        recap = db.connection.execute("SELECT a.path FROM artifacts a JOIN recap_jobs j ON a.recap_job_id=j.id WHERE a.session_id=? AND a.kind='recap_md' AND a.state='ready' AND j.status='completed' ORDER BY (j.phase='final') DESC,j.id DESC,a.id DESC LIMIT 1", (row['session_id'],)).fetchone()
        if recap:
            alias = session_directory(db, row['session_id'])/'recap.md'
            result['recap'] = str(alias) if alias.is_file() else recap[0]
    return result


def bootstrap(config, platform):
    config = Path(config).expanduser().absolute()
    config.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with Database(config.with_name('ascolto.sqlite3')) as db:
        if not db.connection.execute('SELECT 1 FROM bootstrap_state WHERE config_path=?', (str(config),)).fetchone():
            with db.transaction():
                if config.is_file():
                    db.import_config(config, platform)
                ai = config.with_name('ai-providers.json')
                if ai.is_file() and not db.connection.execute('SELECT 1 FROM legacy_imports WHERE source_path=?', (str(ai.resolve()),)).fetchone():
                    db.import_ai_settings(ai)
                keyword_path = Path(db.settings()['keywords_path'] or config.with_name('keywords.txt'))
                if keyword_path.is_file() and not db.connection.execute('SELECT 1 FROM legacy_imports WHERE source_path=?', (str(keyword_path.resolve()),)).fetchone():
                    db.import_keywords(keyword_path)
                output = db.settings()['output_dir']
                # Only real native session names in the configured archive root are candidates.
                # No repository, fixture, benchmark, recursive or global search is performed.
                if output:
                    folder = Path(output).expanduser()
                    candidates = list(folder.glob('????-??-??_??-??-??*_call.jsonl')) if platform == 'macos' else list(folder.glob('call_????-??-??_??-??-??_*/transcript.jsonl'))
                    candidates += list(folder.glob('????-??-??_*/Data/transcript.jsonl'))
                    candidates += [p for p in (folder.glob('????-??-??_??-??-??*_call.md') if platform == 'macos' else folder.glob('call_????-??-??_??-??-??_*/transcript.md')) if not p.with_suffix('.jsonl').exists()]
                    known = {r[0] for r in db.connection.execute('SELECT path FROM artifacts')}
                    for source in sorted(candidates):
                        if str(source.resolve()) not in known:
                            db.import_session(source)
                db.connection.execute('INSERT INTO bootstrap_state VALUES(?,?)', (str(config), now_ms()))
        recover_exports(db)
        reconcile_jobs(db, config)


def reconcile_jobs(db, config):
    """Fail orphaned attempts without retrying a provider call or interrupting a live worker."""
    jobs = list(db.connection.execute("SELECT id,session_id,task FROM recap_jobs WHERE status IN ('queued','validating','running')"))
    for job in jobs:
        path = Path(config).with_name(f".session-{job['session_id']}-{job['task']}.lock")
        descriptor = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
        with os.fdopen(descriptor, 'a') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                continue
            current = db.connection.execute('SELECT status FROM recap_jobs WHERE id=?', (job['id'],)).fetchone()
            if current and current[0] in ('queued','validating','running'):
                db.update_job(job['id'], 'failed', error_code='interrupted')


def recover_exports(db):
    pending = list(db.connection.execute("SELECT o.session_id, max(CASE a.kind WHEN 'transcript_md' THEN a.path END) md,max(CASE a.kind WHEN 'transcript_jsonl' THEN a.path END) jsonl FROM export_outbox o JOIN artifacts a ON a.session_id=o.session_id GROUP BY o.session_id"))
    for row in pending:
        if row['md'] and row['jsonl']:
            db.publish_exports(row['session_id'], row['md'], row['jsonl'])


def _session(db, transcript=None):
    if transcript:
        row = db.connection.execute('SELECT session_id FROM artifacts WHERE path=?', (str(Path(transcript).resolve()),)).fetchone()
    else:
        row = db.connection.execute("SELECT id FROM sessions WHERE state IN ('recording','paused','completed') ORDER BY started_at_ms DESC,id DESC LIMIT 1").fetchone()
    if not row:
        raise DatabaseError('No persisted session is available for this task')
    return row[0]


def handle_request(manager, request):
    from ai_workflows import refresh_transcription_models, TaskSelection, WorkflowRunner, load_turns, decoded_channels, channel_durations, WorkflowError
    operation = request['action']
    if operation == 'refresh_transcription_models':
        models = refresh_transcription_models(manager)
        return {'message': f'Loaded {len(models)} dedicated OpenRouter transcription models.'}
    with manager.database() as db:
        if operation == 'archive_path':
            kind = request.get('kind')
            if kind not in ('call_directory', 'transcript', 'recap'):
                raise DatabaseError('Choose a supported archive item')
            path = archive_status(db).get(kind)
            if not path or not Path(path).exists():
                raise WorkflowError('not_available', 'No saved file is available for this session yet.')
            return {'open_path': path, 'is_directory': kind == 'call_directory'}
        if operation == 'section':
            db.update_settings({'settings_section': request['section']})
            return {}
        if operation == 'keywords':
            with db.transaction():
                db.connection.execute('DELETE FROM keywords')
                db._insert_keywords(request.get('text', ''))
            return {'message': 'Custom keywords saved.'}
        if operation == 'transcription_preferences':
            provider, model = request.get('provider', 'openrouter'), request.get('model', '')
            enabled, consent = bool(request.get('enabled')), bool(request.get('cloud_audio_opt_in'))
            if model:
                WorkflowRunner(manager)._catalog(TaskSelection(provider, model), 'transcription')
                connection = db.connection.execute('SELECT id FROM ai_connections WHERE credential_ref=? AND disconnected_at_ms IS NULL', (provider,)).fetchone()
                if not connection:
                    raise WorkflowError('not_connected', 'Connect OpenRouter and refresh its transcription catalog first.')
                identifier = connection[0]
            else:
                identifier, model, enabled = None, None, False
            if enabled and not consent:
                raise WorkflowError('audio_consent_required', 'Allow uploading saved channel audio before enabling cloud transcription.')
            language = request.get('language', 'auto')
            with db.transaction():
                db.connection.execute('UPDATE transcription_preferences SET connection_id=?,model_id=?,language=?,enabled=?,cloud_audio_opt_in=? WHERE id=1', (identifier, model, language, int(enabled), int(consent)))
            return {'message': 'Post-call transcription preferences saved. Live transcription continues with Whistle.'}
        session = _session(db, request.get('transcript'))
        source = db.connection.execute("SELECT path FROM artifacts WHERE session_id=? AND kind='transcript_jsonl' AND state='ready'", (session,)).fetchone()
        if not source:
            raise WorkflowError('invalid_source', 'The committed local transcript has not been exported. Retry after saving finishes.')
        transcription = operation in ('transcription', 'plan_transcription')
        task = 'transcription' if transcription else 'recap'
        state = db.connection.execute('SELECT state FROM sessions WHERE id=?', (session,)).fetchone()[0]
        if request.get('automatic') and state != 'completed':
            raise WorkflowError('invalid_source', 'Automatic AI tasks require a successfully saved session.')
        if operation != 'provisional_recap' and state not in ('completed', 'failed'):
            raise WorkflowError('invalid_source', 'Wait for the session to finish saving, or request a provisional recap so far.')
        preference = db.connection.execute('SELECT * FROM ' + ('transcription_preferences' if transcription else 'recap_preferences') + ' WHERE id=1').fetchone()
        if request.get('automatic') and not preference['enabled' if transcription else 'auto_generate']:
            return {'message': 'Automatic task is disabled.'}
        selection = db.task_selection(task)
        chosen = TaskSelection(selection['provider'], selection['model'])
        turns = load_turns(source[0], allow_empty=transcription)
        archive = None
        plan = None
        if transcription:
            if operation != 'plan_transcription' and not selection['cloud_audio_opt_in']:
                raise WorkflowError('audio_consent_required', 'Allow saved channel audio uploads in Transcription settings first.')
            audio = {r['kind']: r['path'] for r in db.connection.execute("SELECT kind,path FROM artifacts WHERE session_id=? AND state='ready' AND kind LIKE 'audio_%'", (session,))}
            archive = audio.get('audio_m4a') or ({'MIC': audio['audio_mic_flac'], 'SYS': audio['audio_sys_flac']} if 'audio_mic_flac' in audio and 'audio_sys_flac' in audio else None)
            if not archive:
                raise WorkflowError('invalid_source', 'Save audio for this session before requesting full post-call transcription.')
            runner = WorkflowRunner(manager, max_requests=4096, session_id=str(session))
            runner._catalog(chosen, task)
            with decoded_channels(archive, {'MIC', 'SYS'}, 'ffmpeg') as channels:
                spans = runner.plan_transcription(chosen, turns, scope='full', durations=channel_durations(channels), enforce_limit=False)
            plan = {'requests': len(spans), 'upload_seconds': sum(end-start for _, start, end in spans), 'provider': chosen.provider, 'model': chosen.model, 'max_requests': 4096}
            if len(spans) > 4096:
                raise WorkflowError('request_limit', 'This complete audio plan exceeds 4096 requests. No audio was uploaded.')
            if operation == 'plan_transcription':
                return {**plan, 'message': f"Full audio plan: {len(spans)} requests, {plan['upload_seconds']:.0f} channel-seconds to {chosen.provider} / {chosen.model}. No audio uploaded."}
            if not selection['cloud_audio_opt_in']:
                raise WorkflowError('audio_consent_required', 'Allow saved channel audio uploads in Transcription settings first.')
        lock_path = manager.path.with_name(f'.session-{session}-{task}.lock')
        with open(lock_path, 'a') as lock:
            os.chmod(lock_path, 0o600)
            fcntl.flock(lock, fcntl.LOCK_EX)
            if request.get('automatic') and db.connection.execute("SELECT 1 FROM recap_jobs WHERE session_id=? AND task=? AND phase='final'", (session, task)).fetchone():
                return {'message': 'The automatic task is already queued or completed.'}
            from dataclasses import asdict
            source_digest = hashlib.sha256(json.dumps([asdict(turn) for turn in turns], sort_keys=True, ensure_ascii=False, separators=(',', ':')).encode())
            if archive:
                for path in sorted(archive.values()) if isinstance(archive, dict) else [archive]:
                    with open(path, 'rb') as audio_input:
                        for block in iter(lambda: audio_input.read(1024*1024), b''):
                            source_digest.update(block)
            digest = source_digest.hexdigest()
            job = db.enqueue_job(task, session, preference['connection_id'], chosen.model, selection['language'], digest)
            if operation == 'provisional_recap':
                db.connection.execute("UPDATE recap_jobs SET phase='provisional' WHERE id=?", (job,))
            call_directory = session_directory(db, session)
            output_root = call_directory / ('Data/Transcription-history' if transcription else 'Recap-history')
            output = output_root / f'job-{job}'
            for directory in (output_root, output):
                directory.mkdir(parents=True, exist_ok=True, mode=0o700)
                os.chmod(directory, 0o700)
            def event_sink(event):
                if event['status'] not in ('completed', 'failed'):
                    db.update_job(job, event['status'])
            runner = WorkflowRunner(manager, event_sink=event_sink, max_requests=max(64, plan['requests']) if plan else 4096, session_id=str(session))
            try:
                if transcription:
                    runner.transcribe(chosen, archive, turns, output, cloud_audio_opt_in=True, scope='full', language=None if selection['language'] == 'auto' else selection['language'])
                    artifacts = [('transcript.cloud.ai.jsonl', 'cloud_jsonl'), ('transcript.cloud.ai.md', 'cloud_md')]
                else:
                    mode = 'provisional' if operation == 'provisional_recap' else 'final'
                    # Go requires saved explicit acknowledgment, never client impersonation.
                    acknowledged = db.connection.execute("SELECT value FROM native_settings WHERE key='go_policy_acknowledged'").fetchone()
                    runner.recap(chosen, turns, output, language=selection['language'], mode=mode, allow_go=bool(acknowledged and acknowledged[0] == '1'))
                    prefix = 'recap.provisional.ai' if mode == 'provisional' else 'recap.ai'
                    artifacts = [(prefix+'.json', 'recap_json'), (prefix+'.md', 'recap_md')]
                with db.transaction():
                    for filename, kind in artifacts:
                        target = output / filename
                        db.register_artifact(session, kind, target, job_id=job, sha256=hashlib.sha256(target.read_bytes()).hexdigest())
                    if not transcription and mode == 'final':
                        for filename, target in ((prefix+'.json', call_directory/'Data'/'recap.json'), (prefix+'.md', call_directory/'recap.md')):
                            atomic_text(target, (output/filename).read_text(encoding='utf-8'))
                    db.update_job(job, 'completed')
                return {'job_id': job, 'output_dir': str(output), 'session_dir': str(call_directory), 'message': f'{task.capitalize()} completed. Artifacts: {call_directory}', 'plan': plan}
            except BaseException as error:
                code = getattr(error, 'code', 'workflow_error')
                db.update_job(job, 'failed', error_code=code if isinstance(code, str) and code.replace('_', '').isalnum() and len(code) <= 64 else 'workflow_error')
                raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True)
    parser.add_argument('--platform', choices=['macos', 'linux'])
    parser.add_argument('--transcript')
    parser.add_argument('action', choices=['bootstrap', 'automatic', 'automatic_audio', 'recap', 'transcription', 'plan_transcription'])
    args = parser.parse_args(argv)
    try:
        if args.action == 'bootstrap':
            bootstrap(args.config, args.platform)
            result = {'message': 'Storage bootstrap and export recovery completed.'}
        else:
            from ai_providers import ProviderManager
            manager = ProviderManager(args.config)
            action = 'transcription' if args.action == 'automatic_audio' else 'recap' if args.action == 'automatic' else args.action
            result = handle_request(manager, {'action': action, 'transcript': args.transcript, 'automatic': args.action.startswith('automatic')})
        print(json.dumps({'ok': True, 'result': result}))
        return 0
    except Exception as error:
        print(json.dumps({'ok': False, 'error_code': getattr(error, 'code', 'storage_error'), 'message': str(error) if isinstance(error, DatabaseError) or hasattr(error, 'code') else 'Storage or workflow failed. Existing local artifacts have been retained.'}))
        if args.action == 'bootstrap':
            print('Storage bootstrap failed: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
