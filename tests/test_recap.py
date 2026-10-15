"""Local artifact adaptation and mocked cloud contracts; never uses real credentials."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import recap


class RecapTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ascolto-recap-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def archive(self):
        source = self.directory / "call 'quoted' $(unused).wav"
        archive = source.with_suffix(".m4a")
        time = np.arange(16000) / 16000
        pcm = (np.column_stack((np.sin(2 * np.pi * 440 * time), np.sin(2 * np.pi * 880 * time))) * 8000).astype("<i2")
        with wave.open(str(source), "wb") as output:
            output.setnchannels(2)
            output.setsampwidth(2)
            output.setframerate(16000)
            output.writeframes(pcm.tobytes())
        subprocess.run(["ffmpeg", "-nostdin", "-y", "-v", "error", "-i", str(source), "-c:a", "aac", str(archive)], check=True)
        archive.with_suffix(".md").write_text("Original local transcript\n")
        archive.with_suffix(".jsonl").write_text(json.dumps({"channel": "SYS", "start_s": 0.2, "end_s": 0.8, "speaker": "SPEAKER_2"}) + "\n")
        return archive

    def test_prepare_preserves_sources_and_channel_identity(self):
        archive = self.archive()
        originals = {path: path.read_bytes() for path in (archive, archive.with_suffix(".md"), archive.with_suffix(".jsonl"))}
        with patch.object(recap, "get_openai_key", side_effect=AssertionError("credentials must not be read")):
            self.assertEqual(recap.main(["--archive", str(archive), "--prepare-only"]), 0)
        output = archive.with_suffix("").with_name(archive.stem + "_recap")
        for name, frequency in (("mic", 440), ("sys", 880)):
            wav = self.directory / (name + ".wav")
            subprocess.run(["ffmpeg", "-nostdin", "-y", "-v", "error", "-i", str(output / (name + ".flac")), str(wav)], check=True)
            samples = recap.read_wav(wav)
            peak = np.argmax(abs(np.fft.rfft(samples))) * 16000 / len(samples)
            self.assertLess(abs(peak - frequency), 3)
            self.assertEqual((output / (name + ".flac")).stat().st_mode & 0o777, 0o600)
        for path, content in originals.items():
            self.assertEqual(path.read_bytes(), content)
        self.assertEqual(recap.load_whistle_speakers(output / "transcript.jsonl"), [(0.2, 0.8, "SPEAKER_2")])

    def test_speaker_reader_accepts_both_schemas_and_skips_metadata(self):
        path = self.directory / "transcript.jsonl"
        path.write_text('\n'.join(("not json", "null", '{"event":"session_start"}',
            '{"channel":"SYS","start_sec":1,"end_sec":2,"speaker":"SPEAKER_1"}',
            '{"channel":"SYS","start_s":2,"end_s":3,"speaker":"SPEAKER_2"}',
            '{"channel":"SYS","start_s":null,"speaker":"bad"}')))
        self.assertEqual(recap.load_whistle_speakers(path), [(1, 2, "SPEAKER_1"), (2, 3, "SPEAKER_2")])

    def mock_run(self, failure=False):
        directory = self.directory
        (directory / "mic.flac").write_bytes(b"fixture")
        (directory / "transcript.md").write_text("Original Markdown")
        (directory / "transcript.jsonl").write_text('{"event":"local"}\n')
        segment = {"start": 0, "end": 1, "text": "Release tomorrow.", "label": "ME", "channel": "mic"}
        with patch.object(recap, "get_openai_key", return_value="test-only-key"), \
             patch.object(recap, "transcribe_channel", return_value=([segment], 1)), \
             patch.object(recap, "notify"), \
             patch.object(recap, "run_claude", side_effect=RuntimeError("simulated provider failure") if failure else None,
                          return_value=("Summary\n---SPOKEN---\nSpoken summary", "mock-model")):
            return recap.main(["--dir", str(directory)])

    def test_cloud_results_leave_local_transcripts_intact(self):
        self.assertEqual(self.mock_run(), 0)
        self.assertEqual((self.directory / "transcript.md").read_text(), "Original Markdown")
        self.assertEqual((self.directory / "transcript.jsonl").read_text(), '{"event":"local"}\n')
        self.assertTrue((self.directory / "transcript.cloud.md").exists())
        self.assertEqual((self.directory / "recap.spoken.txt").read_text(), "Spoken summary\n")

    def test_provider_failure_keeps_previous_recap_and_originals(self):
        (self.directory / "recap.md").write_text("Previous recap")
        self.assertEqual(self.mock_run(failure=True), 1)
        self.assertEqual((self.directory / "recap.md").read_text(), "Previous recap")
        self.assertEqual((self.directory / "transcript.md").read_text(), "Original Markdown")

    def test_concurrent_recap_is_rejected_before_credentials(self):
        with recap.session_lock(self.directory), patch.object(recap, "get_openai_key", side_effect=AssertionError("unexpected credentials")):
            self.assertEqual(recap.main(["--dir", str(self.directory), "--prepare-only"]), 1)

    def test_failed_second_conversion_keeps_both_existing_channels(self):
        archive = self.archive()
        target = self.directory / "prepared"
        target.mkdir()
        for name in ("mic.flac", "sys.flac"):
            (target / name).write_bytes(b"previous")
        real_run = subprocess.run
        conversions = 0

        def convert(arguments, **kwargs):
            nonlocal conversions
            if Path(arguments[0]).name == "ffprobe":
                return real_run(arguments, **kwargs)
            conversions += 1
            if conversions == 2:
                raise subprocess.CalledProcessError(1, arguments)
            Path(arguments[-1]).write_bytes(b"new channel")

        with patch.object(recap.subprocess, "run", side_effect=convert), self.assertRaises(subprocess.CalledProcessError):
            recap.prepare_archive(archive, target)
        for name in ("mic.flac", "sys.flac"):
            self.assertEqual((target / name).read_bytes(), b"previous")

    def test_mono_archive_is_rejected_locally(self):
        archive = self.archive()
        mono = self.directory / "mono.m4a"
        subprocess.run(["ffmpeg", "-nostdin", "-y", "-v", "error", "-i", str(archive), "-ac", "1", str(mono)], check=True)
        with patch.object(recap, "get_openai_key", side_effect=AssertionError("unexpected credentials")):
            self.assertEqual(recap.main(["--archive", str(mono), "--prepare-only"]), 1)


if __name__ == "__main__":
    unittest.main()
