#!/usr/bin/env python3
"""Explicit, backed-up archive reorganization. Quit Ascolto before applying."""
import argparse
from contextlib import closing
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import sqlite3

from ascolto_database import Database, DatabaseError
from ascolto_files import atomic_text


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def organize(config, apply=False):
    config = Path(config).expanduser().resolve()
    database_path = config.with_name('ascolto.sqlite3')
    with Database(database_path) as db:
        root = Path(db.settings()['output_dir']).expanduser().resolve()
        if not root.is_dir() or root in (Path('/'), Path.home()):
            raise DatabaseError('Choose a dedicated archive directory first')
        if db.connection.execute("SELECT 1 FROM sessions WHERE state IN ('recording','paused','stopping') LIMIT 1").fetchone() or db.connection.execute("SELECT 1 FROM recap_jobs WHERE status IN ('queued','validating','running') LIMIT 1").fetchone():
            raise DatabaseError('Finish recording and AI jobs before reorganizing')
        if apply:
            address = db.settings()['control_socket'] or f'/tmp/ascolto-{os.getuid()}.sock'
            with socket.socket(socket.AF_UNIX) as probe:
                probe.settimeout(1)
                try:
                    probe.connect(address)
                except (FileNotFoundError, ConnectionRefusedError):
                    pass
                else:
                    raise DatabaseError('Quit Ascolto before reorganizing its archive')
        plan, calls, missing = [], {}, []
        for session in db.connection.execute('SELECT * FROM sessions ORDER BY id'):
            rows = list(db.connection.execute("SELECT a.*,j.phase FROM artifacts a LEFT JOIN recap_jobs j ON j.id=a.recap_job_id WHERE a.session_id=? AND a.state='ready' ORDER BY a.id", (session['id'],)))
            md = next((Path(r['path']) for r in rows if r['kind'] == 'transcript_md'), None)
            if md is None or not md.is_relative_to(root):
                continue
            if not md.is_file():
                missing.append(session['id'])
                continue
            slug = re.sub('[^a-z0-9]+', '-', (session['meeting_title'] or session['call_title'] or session['source_app'] or 'call').lower()).strip('-')[:48] or 'call'
            stamp = datetime.fromtimestamp(session['started_at_ms']/1000).strftime('%Y-%m-%d_%H-%M-%S')
            folder = md.parent if md.name == 'transcript.md' and (md.parent/'Data').is_dir() else root/f'{stamp}_{slug}_s{session["id"]}'
            calls[session['id']] = folder
            for row in rows:
                source = Path(row['path'])
                if not source.is_relative_to(root) or not source.resolve().is_relative_to(root) or not source.is_file():
                    continue
                relative = {'transcript_md':'transcript.md','transcript_jsonl':'Data/transcript.jsonl','audio_m4a':'Audio/recording.m4a','audio_mic_flac':'Audio/mic.flac','audio_sys_flac':'Audio/sys.flac'}.get(row['kind'])
                if relative is None:
                    history = 'Recap-history' if row['kind'].startswith('recap_') else 'Data/Transcription-history'
                    relative = f'{history}/job-{row["recap_job_id"] or row["id"]}/{source.name}'
                target = folder/relative
                if source != target:
                    plan.append((row['id'], session['id'], source, target))
        report = {'sessions':len(calls), 'files':len(plan), 'missing_transcript_sessions':missing, 'applied':False}
        if not apply or not plan:
            return report
        stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
        backup = root.with_name(root.name+'.backup-'+stamp)
        backup.mkdir(mode=0o700)
        shutil.copytree(root, backup/'Files', symlinks=True)
        with closing(sqlite3.connect(backup/'ascolto.sqlite3')) as snapshot:
            db.connection.backup(snapshot)
        os.chmod(backup/'ascolto.sqlite3', 0o600)
        report['backup'] = str(backup)
        created = []
        try:
            for folder in calls.values():
                folder.mkdir(mode=0o700,parents=True,exist_ok=True)
            for _, _, source, target in plan:
                target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
                if target.exists():
                    if digest(target) != digest(source):
                        raise DatabaseError('An archive destination already contains a different file')
                else:
                    shutil.copy2(source, target)
                    os.chmod(target, 0o600)
                    created.append(target)
                if digest(target) != digest(source):
                    raise DatabaseError('Archive copy verification failed; originals were kept')
            replacements = {str(source):str(target) for _,_,source,target in plan}
            def relocate(value):
                if isinstance(value, str): return replacements.get(value, value)
                if isinstance(value, dict): return {key:relocate(item) for key,item in value.items()}
                if isinstance(value, list): return [relocate(item) for item in value]
                return value
            with db.transaction():
                for identifier, _, _, target in plan:
                    db.connection.execute('UPDATE artifacts SET path=? WHERE id=?', (str(target),identifier))
                for session, folder in calls.items():
                    metadata = db.connection.execute('SELECT metadata_json FROM sessions WHERE id=?',(session,)).fetchone()[0]
                    db.connection.execute('UPDATE sessions SET metadata_json=? WHERE id=?',(json.dumps(relocate(json.loads(metadata))),session))
                for identifier, _, _, target in plan:
                    if target.suffix == '.jsonl':
                        rows = [json.loads(line) for line in target.read_text(encoding='utf-8').splitlines() if line.strip()]
                        changed = [relocate(row) for row in rows]
                        if changed != rows:
                            atomic_text(target, ''.join(json.dumps(row,ensure_ascii=False)+'\n' for row in changed))
                            db.connection.execute('UPDATE artifacts SET sha256=? WHERE id=?',(digest(target),identifier))
        except BaseException:
            for target in created:
                target.unlink(missing_ok=True)
            raise
        # The DB now points to verified copies. Retained backups make cleanup recoverable.
        for _, _, source, _ in plan:
            source.unlink()
        for session, folder in calls.items():
            rows = db.connection.execute("SELECT a.path,a.kind FROM artifacts a JOIN recap_jobs j ON j.id=a.recap_job_id WHERE a.session_id=? AND j.phase='final' AND j.status='completed' AND a.kind IN ('recap_md','recap_json') ORDER BY j.id DESC", (session,))
            published = set()
            for row in rows:
                if row['kind'] in published: continue
                target = folder/('recap.md' if row['kind']=='recap_md' else 'Data/recap.json')
                atomic_text(target,Path(row['path']).read_text(encoding='utf-8'))
                published.add(row['kind'])
        for directory in sorted(root.rglob('*'), key=lambda p:len(p.parts), reverse=True):
            if directory.is_dir() and not directory.is_symlink():
                try: directory.rmdir()
                except OSError: pass
        report['applied'] = True
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True)
    parser.add_argument('--apply', action='store_true', help='Back up, verify copies and update artifact references; default is a dry run')
    args = parser.parse_args()
    print(json.dumps(organize(args.config,args.apply),indent=2))


if __name__ == '__main__':
    main()
