"""Verify accelerated ASR-delay replay against real portable C queue samples."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("replay_nemotron_queue", ROOT / "scripts/replay_nemotron_queue.py")
REPLAY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = REPLAY
SPEC.loader.exec_module(REPLAY)


class NativeReplay(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cc"):
            raise unittest.SkipTest("A C compiler is required for the real portable queue fixture")
        cls.temporary = tempfile.TemporaryDirectory(prefix="ascolto-native-queue-test-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.library = REPLAY.compile_queue(cls.temporary.name)

    def run_replay(self, timings, **options):
        return REPLAY.replay(timings, library=self.library, **options)

    def assert_complete(self, report):
        expected = round(report["capture_s"] * REPLAY.RATE)
        self.assertEqual(report["timeline_samples"], dict(mic=expected, sys=expected))
        self.assertTrue(report["queue_empty"])
        self.assertTrue(report["indexed_pcm_verified"])
        self.assertTrue(report["equal_overflow_padding_verified"])
        self.assertEqual(report["overflow_lost_s"], report["overflow_equal_padding_s"])
        self.assertLessEqual(report["max_retained_backlog_s"], report["capacity_s"])
        self.assertAlmostEqual(report["final_virtual_wall_s"] - report["capture_s"], report["final_drain_s"])

    def test_native_ring_exact_overflow_frames_and_retained_order(self):
        queue = REPLAY.NativeQueue(640, self.library)
        try:
            for frame in range(10):
                queue.push(1, frame)
                queue.push(0, frame)
            self.assertEqual(queue.dropped(), 8 * REPLAY.FRAME)
            for frame in range(10):
                count, mic, sys = queue.pop(False)
                self.assertEqual(count, REPLAY.FRAME)
                expected = bytes(REPLAY.FRAME * 4) if frame < 8 else REPLAY.frame_bytes(frame)
                self.assertEqual(mic, expected)
                self.assertEqual(sys, expected)
            self.assertEqual(queue.available(0), 0)
            self.assertEqual(queue.available(1), 0)
            self.assertEqual(queue.padded(0), queue.dropped())
            self.assertEqual(queue.padded(1), queue.dropped())
        finally:
            queue.close()

    def test_production_capacity_and_fast_consumer(self):
        self.assertEqual(REPLAY.production_capacity_samples(), 16000 * 30)
        report = self.run_replay([REPLAY.Timing("fast", 5, 0)], duration_seconds=2,
                                 gap_seconds=0, capacity_samples=640)
        self.assert_complete(report)
        self.assertEqual(report["asr_calls"], 20)
        self.assertEqual(report["overflow_lost_s"], 0)
        self.assertEqual(report["final_drain_s"], 0)
        self.assertEqual(report["native_padding_s"], dict(mic=0, sys=0))

    def test_realtime_consumer_has_bounded_backlog_and_drains_last_inference(self):
        report = self.run_replay([REPLAY.Timing("realtime", 5, 100_000_000)], duration_seconds=4,
                                 gap_seconds=0, capacity_samples=6400)
        self.assert_complete(report)
        self.assertEqual(report["asr_calls"], 40)
        self.assertEqual(report["overflow_lost_s"], 0)
        self.assertEqual(report["max_backlog_s"], 0.1)
        self.assertEqual(report["final_drain_s"], 0.1)

    def test_sustained_slow_consumer_overflows_both_channels_and_skips_lost_turns(self):
        report = self.run_replay([REPLAY.Timing("slow", 5, 1_000_000_000)], duration_seconds=4,
                                 gap_seconds=0, capacity_samples=3200)
        self.assert_complete(report)
        self.assertGreater(report["overflow_lost_s"], 2)
        self.assertGreater(report["wholly_dropped_turns"], 0)
        self.assertLess(report["asr_calls"], report["planned_turns"])
        self.assertGreater(report["final_drain_s"], 1)
        self.assertEqual(report["native_padding_s"]["mic"], report["overflow_lost_s"])
        self.assertEqual(report["native_padding_s"]["sys"], report["overflow_lost_s"])
        self.assertEqual(report["delivered_zero_s"]["mic"], report["delivered_zero_s"]["sys"])

    def test_differential_channel_starts_before_and_after_missing_timeout(self):
        for delay in (0.2, 1.2):
            with self.subTest(delay=delay):
                report = self.run_replay([REPLAY.Timing("startup", 5, 0)], duration_seconds=2,
                                         gap_seconds=0, mic_start_seconds=delay, capacity_samples=32000)
                self.assert_complete(report)
                self.assertEqual(report["overflow_lost_s"], 0)
                self.assertEqual(report["delivered_zero_s"], dict(mic=delay, sys=0))
                self.assertEqual(report["native_padding_s"], dict(mic=delay, sys=0))

    def test_measured_startup_is_not_repeated_in_warm_cycle(self):
        timings = [REPLAY.Timing("cold", 5, 500_000_000), REPLAY.Timing("warm", 5, 0)]
        self.assertEqual([REPLAY.timing_at(timings, i, 1).identifier for i in range(6)],
                         ["cold", "warm", "warm", "warm", "warm", "warm"])
        report = self.run_replay(timings, duration_seconds=2, gap_seconds=0, capacity_samples=32000)
        self.assert_complete(report)
        self.assertEqual(report["scheduled_asr_pause_s"], 0.5)
        self.assertEqual(report["asr_calls"], 20)
        self.assertEqual(report["overflow_lost_s"], 0)

    def test_thirty_minute_replay_preserves_timeline_with_bounded_native_storage(self):
        report = self.run_replay([REPLAY.Timing("measured-shape", 171, 11_800_000_000)],
                                 duration_seconds=1800, gap_seconds=0.4)
        self.assert_complete(report)
        self.assertGreaterEqual(report["capture_s"], 1800)
        self.assertLess(report["capture_s"], 1804)
        self.assertEqual(report["native_ring_payload_bytes"], 3_840_000)
        self.assertGreater(report["overflow_lost_s"], 0)
        self.assertLessEqual(REPLAY.frame_bytes.cache_info().currsize, 64)

    def test_invalid_schedule_and_capacity_are_rejected(self):
        for arguments in (dict(duration_seconds=0), dict(duration_seconds=float("nan")),
                          dict(gap_seconds=-1), dict(capacity_samples=321),
                          dict(mic_start_seconds=3, sys_start_seconds=3, duration_seconds=1)):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                self.run_replay([REPLAY.Timing("invalid", 5, 0)], **arguments)


class TimingInputs(unittest.TestCase):
    def fixture(self, root, records):
        manifest, results = root / "manifest.tsv", root / "run.jsonl"
        manifest.write_text("id\tpath\tlanguage\texpected_text\nclip\tunused.wav\ten\treference\n")
        results.write_text("\n".join(json.dumps(record) for record in records) + "\n")
        return manifest, results

    def test_duplicate_clip_repeats_keep_measured_order_and_failed_delays(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, results = self.fixture(root, [dict(type="load", elapsed_s=2),
                dict(id="clip", audio_s=0.1, elapsed_s=0.3),
                dict(id="clip", audio_s=0.1, elapsed_ms=25, ok=False, error="failed")])
            timings = REPLAY.load_timings(manifest, results)
            self.assertEqual([t.elapsed_ns for t in timings], [300_000_000, 25_000_000])
            self.assertEqual([t.failed for t in timings], [False, True])

    def test_metadata_duration_and_invalid_record_rejection(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, results = self.fixture(root, [dict(id="clip", elapsed_s=0.2)])
            (root / "metadata.json").write_text(json.dumps(dict(clips=[dict(id="clip", duration_s=0.15)])))
            self.assertEqual(REPLAY.load_timings(manifest, results)[0].audio_frames, 8)
            for record in (dict(id="unknown", audio_s=0.1, elapsed_s=0),
                           dict(id="clip", audio_s=0.1, elapsed_s=-1),
                           dict(id="clip", audio_s=61, elapsed_s=0), ["invalid"]):
                results.write_text(json.dumps(record) + "\n")
                with self.subTest(record=record), self.assertRaises(ValueError):
                    REPLAY.load_timings(manifest, results)

    def test_cli_writes_standalone_native_report_with_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, results = self.fixture(root, [dict(id="clip", audio_s=0.1, elapsed_s=0.02)])
            output = root / "report.json"
            completed = subprocess.run([sys.executable, str(ROOT / "scripts/replay_nemotron_queue.py"),
                "--manifest", str(manifest), "--results", str(results), "--duration-seconds", "1",
                "--output", str(output)], capture_output=True, text=True, timeout=30)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            report = json.loads(output.read_text())
            self.assertTrue(report["indexed_pcm_verified"] and report["queue_empty"])
            self.assertEqual(report["capacity_s"], 30)
            self.assertEqual(len(report["provenance"]["queue_source_sha256"]), 64)
            self.assertGreater(report["replay_peak_rss_bytes"], 0)


if __name__ == "__main__":
    unittest.main()
