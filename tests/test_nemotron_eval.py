"""Offline fixtures for public-corpus selection and ASR evaluation accounting."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def module(name):
    specification = importlib.util.spec_from_file_location(name, ROOT / "scripts" / (name + ".py"))
    loaded = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(loaded)
    return loaded


prepare = module("prepare_nemotron_eval")
scorer = module("score_nemotron_eval")


class ScoreEvaluation(unittest.TestCase):
    def clip(self, identifier, expected, **extra):
        return dict(id=identifier, expected_text=expected, condition="clean", score=True, duration_s=2, **extra)

    def test_normalization_and_edit_operations(self):
        self.assertEqual(scorer.normalize("ＨＥＬＬＯ, don't — stop!"), "hello dont stop")
        counts = scorer.edit_counts("a b c".split(), "a x c d".split())
        self.assertEqual((counts["substitutions"], counts["deletions"], counts["insertions"]), (1, 0, 1))
        self.assertEqual(counts["rate"], 2 / 3)
        self.assertEqual(scorer.edit_counts(["a"], [])["deletions"], 1)
        self.assertEqual(scorer.edit_counts([], ["a"])["insertions"], 1)
        self.assertIsNone(scorer.edit_counts([], [])["rate"])

    def test_missing_empty_error_results_remain_in_denominator(self):
        clips = [self.clip("missing", "one two"), self.clip("empty", "three"),
                 self.clip("error", "four"), self.clip("valid", "five six seven eight")]
        records = {"empty": {"text": ""}, "error": {"text": "four", "error": "decode_failed"},
                   "valid": {"text": "five six seven eight", "elapsed_s": 1}}
        report = scorer.score(clips, records)
        self.assertEqual(report["aggregate"]["word"]["reference_units"], 8)
        self.assertEqual(report["aggregate"]["word"]["errors"], 4)
        self.assertEqual(report["aggregate"]["word"]["rate"], .5)
        self.assertEqual(report["aggregate"]["failures"], 3)
        self.assertEqual(sum(row["missing"] for row in report["clips"]), 1)

    def test_overlap_is_not_in_accuracy_but_failure_is_counted(self):
        clips = [self.clip("clean", "a b"), dict(id="overlap", expected_text="", condition="overlap", score=False, duration_s=2)]
        report = scorer.score(clips, {"clean": {"text": "a b"}, "overlap": {"text": ""}})
        self.assertEqual(report["aggregate"]["word"]["rate"], 0)
        self.assertEqual(report["aggregate"]["scored_clips"], 1)
        self.assertEqual(report["aggregate"]["failures"], 1)
        self.assertIsNone(report["conditions"]["overlap"]["word"]["rate"])

    def test_unscored_private_text_has_no_error_counts_or_accuracy_delta(self):
        clips = [dict(id="private", expected_text="", condition="private", score=False, duration_s=10)]
        before = scorer.score(clips, {"private": dict(text="one two", elapsed_s=5)})
        after = scorer.score(clips, {"private": dict(text="three", elapsed_s=4)})
        self.assertIsNone(after["clips"][0]["word"]["errors"])
        self.assertIsNone(after["aggregate"]["word"]["rate"])
        delta = scorer.compare(after, before)["clips"][0]
        self.assertIsNone(delta["word_error_delta"])
        self.assertIsNone(delta["character_error_delta"])
        self.assertTrue(delta["transcript_changed"])
        self.assertEqual(delta["elapsed_s_delta"], -1)

    def test_latency_memory_and_comparison(self):
        clips = [self.clip("a", "one"), self.clip("b", "two")]
        baseline = scorer.score(clips, {"a": {"text": "one", "elapsed_s": 3}, "b": {"text": "too", "elapsed_s": 5}})
        current = scorer.score(clips, {"a": {"text": "ONE!", "elapsed_ms": 1000, "rss_bytes": 100, "prefix_hits": 0},
                                       "b": {"text": "two", "elapsed_s": 3, "rss_bytes": 120, "prefix_hits": 1}})
        self.assertEqual(current["aggregate"]["latency_s"]["median"], 2)
        self.assertAlmostEqual(current["aggregate"]["latency_s"]["p95"], 2.9)
        self.assertEqual(current["aggregate"]["weighted_rtf"], 1)
        self.assertEqual(current["memory"]["slope_bytes_per_turn"], 20)
        comparison = scorer.compare(current, baseline)
        self.assertEqual(comparison["changed_transcripts"], 2)
        self.assertEqual(comparison["changed_normalized_transcripts"], 1)
        self.assertEqual(comparison["wer_delta"], -.5)
        self.assertEqual(comparison["clips"][1]["elapsed_s_delta"], -2)

    def test_startup_is_separate_from_decode_and_prefix_not_invented(self):
        report = scorer.score([self.clip("a", "one")], {"a": {"text": "one", "elapsed_s": 2}},
                              [dict(type="load", elapsed_s=100)])
        self.assertEqual(report["aggregate"]["latency_s"]["maximum"], 2)
        self.assertEqual(report["startup_latency_s"]["maximum"], 100)
        self.assertEqual(report["prefix_observations"], [])
        with self.assertRaises(ValueError):
            scorer.score([self.clip("empty-reference", "")], {})

    def test_duplicate_and_unknown_results_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, metadata, results = root / "manifest.tsv", root / "metadata.json", root / "run.jsonl"
            manifest.write_text("id\tpath\tlanguage\texpected_text\na\t/x.wav\ten\tone\n")
            for rows in ([dict(id="a", text="one"), dict(id="a", text="one")], [dict(id="unknown", text="one")]):
                results.write_text("\n".join(json.dumps(row) for row in rows))
                with self.assertRaises(ValueError):
                    scorer.load_inputs(manifest, metadata, results)


class PrepareEvaluation(unittest.TestCase):
    def test_flac_sample_metadata(self):
        stream_info = bytearray(34)
        stream_info[10:18] = ((16000 << 44) | 48000).to_bytes(8, "big")
        header = b"fLaC" + bytes([0, 0, 0, 34]) + stream_info
        self.assertEqual(prepare.flac_duration(header), 3)
        with self.assertRaises(ValueError):
            prepare.flac_duration(b"invalid")

    def test_archive_members_cannot_escape_or_link(self):
        import tarfile
        for name, kind in (("../outside", tarfile.REGTYPE), ("/absolute", tarfile.REGTYPE),
                           ("safe/link", tarfile.SYMTYPE), ("safe/hard", tarfile.LNKTYPE)):
            member = tarfile.TarInfo(name)
            member.type = kind
            with self.assertRaises(ValueError):
                prepare.safe_member(member)

    def test_selection_distinct_speakers_and_order_independence(self):
        clips = [dict(id="%d-%d" % (speaker, index), speaker=str(speaker), duration_s=duration)
                 for speaker in range(20) for index, duration in enumerate((5, 10))]
        selected = prepare.select_clips(clips, 12)
        self.assertEqual(selected, prepare.select_clips(list(reversed(clips)), 12))
        self.assertEqual(len({clip["speaker"] for clip in selected}), 12)
        self.assertEqual(sum(clip["duration_s"] < 8 for clip in selected), 6)

    def test_seeded_noise_is_repeatable_and_not_clipped(self):
        clean = [1000, -1000] * 100
        first, gain = prepare.add_noise(clean, 42)
        second, _ = prepare.add_noise(clean, 42)
        self.assertEqual(first, second)
        self.assertNotEqual(first, prepare.add_noise(clean, 43)[0])
        noise_energy = sum((actual / gain - expected) ** 2 for actual, expected in zip(first, clean))
        self.assertAlmostEqual(sum(x*x for x in clean) / noise_energy, 10, delta=.01)
        self.assertLessEqual(max(abs(value) for value in first), 32760)


if __name__ == "__main__":
    unittest.main()
