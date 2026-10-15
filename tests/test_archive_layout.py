"""Archive reorganization preserves recoverable originals and database identity."""
from contextlib import closing
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from ascolto_database import Database
from organize_archive import organize


class ArchiveLayoutTests(unittest.TestCase):
    def test_both_native_layouts_have_backups_stable_ids_and_idempotent_reorganization(self):
        for platform in ('coreaudio','pipewire'):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as temporary:
                base=Path(temporary).resolve(); root=base/'Archive'; root.mkdir()
                original=root if platform=='coreaudio' else root/'call_2026-10-15_10-30-00_1'
                original.mkdir(exist_ok=True)
                md=original/('2026-10-15_10-30-00_call.md' if platform=='coreaudio' else 'transcript.md')
                trace=md.with_suffix('.jsonl'); audio=md.with_suffix('.m4a') if platform=='coreaudio' else original/'mic.flac'
                md.write_text('# Original transcript\n');trace.write_text('{"event":"utterance","text":"Original"}\n');audio.write_bytes(b'original-audio-fixture')
                config=base/'config.json'
                with Database(base/'ascolto.sqlite3') as db:
                    db.update_settings({'output_dir':str(root),'control_socket':str(base/'unused.sock')})
                    session=db.connection.execute("INSERT INTO sessions(started_at_ms,state,capture_backend,call_title) VALUES(1792053000000,'completed',?,'Team sync')",(platform,)).lastrowid
                    for path,kind in ((md,'transcript_md'),(trace,'transcript_jsonl'),(audio,'audio_m4a' if platform=='coreaudio' else 'audio_mic_flac')):
                        db.register_artifact(session,kind,path)
                before=audio.read_bytes()
                self.assertEqual(organize(config)['files'],3)
                report=organize(config,apply=True)
                self.assertTrue(report['applied'])
                self.assertEqual((Path(report['backup'])/'Files'/audio.relative_to(root)).read_bytes(),before)
                self.assertFalse(audio.exists())
                with Database(base/'ascolto.sqlite3') as db:
                    rows=list(db.connection.execute('SELECT * FROM artifacts ORDER BY id'))
                    self.assertEqual({r['session_id'] for r in rows},{session})
                    self.assertTrue(all(Path(r['path']).is_file() for r in rows))
                    self.assertEqual(Path(rows[0]['path']).name,'transcript.md')
                    self.assertEqual(Path(rows[1]['path']).parent.name,'Data')
                    self.assertEqual(Path(rows[2]['path']).parent.name,'Audio')
                    self.assertEqual(Path(rows[2]['path']).read_bytes(),before)
                    self.assertEqual(db.connection.execute('PRAGMA quick_check').fetchone()[0],'ok')
                    self.assertFalse(list(db.connection.execute('PRAGMA foreign_key_check')))
                self.assertEqual(organize(config,apply=True)['files'],0)
                with closing(sqlite3.connect(Path(report['backup'])/'ascolto.sqlite3')) as backup:
                    self.assertEqual(backup.execute('SELECT path FROM artifacts WHERE kind=?',('transcript_md',)).fetchone()[0],str(md))

    def test_new_layout_import_retains_backend_and_transcript_locations(self):
        with tempfile.TemporaryDirectory() as temporary:
            base=Path(temporary).resolve(); call=base/'2026-10-15_10-30-00_team-sync'
            call.mkdir(); (call/'Data').mkdir()
            with Database(base/'writer.sqlite3') as db:
                session=db.connection.execute("INSERT INTO sessions(started_at_ms,state,capture_backend) VALUES(1792053000000,'completed','coreaudio')").lastrowid
                db.append_utterance(session,'MIC',0,1000,'A preserved statement.','ME','en')
                db.publish_exports(session,call/'transcript.md',call/'Data'/'transcript.jsonl')
            with Database(base/'reader.sqlite3') as db:
                imported=db.import_session(call/'Data'/'transcript.jsonl')
                self.assertEqual(db.connection.execute('SELECT capture_backend FROM sessions WHERE id=?',(imported,)).fetchone()[0],'coreaudio')
                self.assertEqual(db.connection.execute("SELECT path FROM artifacts WHERE kind='transcript_md'").fetchone()[0],str(call/'transcript.md'))

    def test_failed_copy_keeps_original_paths_and_a_database_backup(self):
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as temporary:
            base=Path(temporary).resolve();root=base/'Archive';root.mkdir()
            md=root/'2026-10-15_10-30-00_call.md';md.write_text('Original transcript')
            with Database(base/'ascolto.sqlite3') as db:
                db.update_settings({'output_dir':str(root),'control_socket':str(base/'unused.sock')})
                session=db.connection.execute("INSERT INTO sessions(started_at_ms,state,capture_backend) VALUES(1792053000000,'completed','coreaudio')").lastrowid
                db.register_artifact(session,'transcript_md',md)
            with patch('organize_archive.shutil.copy2',side_effect=OSError('fixture copy failure')):
                with self.assertRaises(OSError):organize(base/'config.json',apply=True)
            self.assertEqual(md.read_text(),'Original transcript')
            self.assertEqual(len(list(base.glob('Archive.backup-*/ascolto.sqlite3'))),1)
            with Database(base/'ascolto.sqlite3') as db:
                self.assertEqual(db.connection.execute('SELECT path FROM artifacts').fetchone()[0],str(md))


if __name__=='__main__':unittest.main()
