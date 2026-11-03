"""Offline regressions for the opt-in native/OpenRouter evaluation driver."""
from array import array
import base64
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import compare_nemotron_openrouter as evaluation


class Evaluation(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def suite(self):
        source = self.root / "source.wav"
        pcm = array("h", [100]) * (31 * evaluation.RATE + 8)
        evaluation.write_pcm(source, pcm)
        metadata = dict(clips=[dict(id="long", path=str(source), sha256=evaluation.digest(source),
                                    expected_text="one two three four", condition="long", score=True)],
                        corpus="fixture", source="fixture", license="fixture", license_url="fixture",
                        attribution="fixture")
        (self.root / "metadata.json").write_text(json.dumps(metadata))
        path = self.root / "suite" / "suite.json"
        evaluation.prepare(self.root, path.parent)
        return path

    def results(self, path, texts, missing=(), failed=()):
        suite = evaluation.load_suite(path)
        records = [dict(type="run", suite_sha256=evaluation.digest(path))]
        for case in suite["cases"]:
            for chunk in case["chunks"]:
                if chunk["id"] in missing:
                    continue
                records.append(dict(type="chunk", id=chunk["id"], sha256=chunk["sha256"],
                                    ok=chunk["id"] not in failed, text=texts.get(chunk["id"], ""), elapsed_s=1))
        target = self.root / "results.jsonl"
        target.write_text("\n".join(json.dumps(record) for record in records))
        return target

    def test_contiguous_exact_pcm_chunking_repeats_and_short_silent_tail(self):
        path = self.suite()
        suite = evaluation.load_suite(path)
        case = suite["cases"][0]
        pcm = array("h")
        for chunk in case["chunks"]:
            self.assertLessEqual(chunk["samples"], 30 * evaluation.RATE)
            pcm.extend(evaluation.read_pcm(path.parent / chunk["path"]))
        self.assertEqual(pcm, evaluation.read_pcm(self.root / "source.wav"))
        tail = next(case for case in suite["cases"] if case["id"] == "silence-tail")
        self.assertEqual(tail["chunks"][0]["samples"], 8)
        self.assertEqual(suite["cases"][-1]["chunks"][0]["sha256"], case["chunks"][0]["sha256"])
        self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)
        recovery = evaluation.load_suite(path.with_name("recovery-probe.json"))
        self.assertEqual(recovery["cases"][0]["chunks"][0]["sha256"],
                         recovery["cases"][2]["chunks"][0]["sha256"])

    def test_tampered_audio_and_noncontiguous_timeline_fail_before_upload(self):
        path = self.suite()
        suite = json.loads(path.read_text())
        suite["cases"][0]["chunks"][1]["start_sample"] += 1
        path.write_text(json.dumps(suite))
        with self.assertRaisesRegex(ValueError, "Noncontiguous"):
            evaluation.load_suite(path)
        suite["cases"][0]["chunks"][1]["start_sample"] -= 1
        path.write_text(json.dumps(suite))
        audio = path.parent / suite["cases"][0]["chunks"][0]["path"]
        audio.write_bytes(audio.read_bytes() + b"changed")
        with self.assertRaisesRegex(ValueError, "checksum"):
            evaluation.load_suite(path)

    def test_boundary_scoring_joins_text_without_deduplication(self):
        path = self.suite()
        results = self.results(path, {"long-000": "one two", "long-001": "three four"})
        report = evaluation.score_run(path, results)
        self.assertEqual(report["cases"][0]["word"]["rate"], 0)
        results = self.results(path, {"long-000": "one two", "long-001": "two three four"})
        self.assertEqual(evaluation.score_run(path, results)["cases"][0]["word"]["insertions"], 1)

    def test_failures_stay_in_reference_denominator_silence_is_separate(self):
        path = self.suite()
        results = self.results(path, {"long-000": "one two", "long-001": "three four",
                                      "silence-000": "imagined words"}, missing=("long-001",))
        report = evaluation.score_run(path, results)
        long = report["cases"][0]
        self.assertEqual(long["word"]["reference_units"], 4)
        self.assertEqual(long["word"]["deletions"], 2)
        self.assertEqual(report["aggregate"]["failed_chunks"], 1)
        self.assertIsNone(report["aggregate"]["weighted_rtf"])
        silence = next(row for row in report["cases"] if row["id"] == "silence")
        self.assertEqual(silence["silence_inserted_words"], 2)
        self.assertIsNone(silence["word"])
        # Failed chunks must not appear to agree with successful empty results.
        comparison = evaluation.comparison(path, {"a": results, "b": results})
        self.assertIsNone(comparison["pairs"]["a -> b"][0]["normalized_equal"])
        shared = comparison["recognition_on_shared_complete_cases"]
        self.assertNotIn("long", shared["case_ids"])
        self.assertEqual(shared["results"]["a"]["word"]["reference_units"], 8)

    def test_duplicate_unknown_and_wrong_suite_results_rejected(self):
        path = self.suite()
        result = self.results(path, {})
        original = result.read_text()
        result.write_text(original + "\n" + original.splitlines()[1])
        with self.assertRaisesRegex(ValueError, "duplicate"):
            evaluation.score_run(path, result)
        result.write_text(original.replace('"id": "long-000"', '"id": "unknown"'))
        with self.assertRaisesRegex(ValueError, "Unknown"):
            evaluation.score_run(path, result)
        result.write_text(original.replace(evaluation.digest(path), "incorrect"))
        with self.assertRaisesRegex(ValueError, "different suite"):
            evaluation.score_run(path, result)

    def test_upload_budget_checked_before_credentials_or_network(self):
        path = self.suite()
        with patch.object(evaluation, "Cloud") as cloud:
            with self.assertRaisesRegex(ValueError, "budget"):
                evaluation.run(path, self.root / "cloud.jsonl", "openrouter", max_requests=1)
            cloud.assert_not_called()

    def test_cloud_uses_saved_key_exact_wav_documented_endpoint_and_no_fallback(self):
        path = self.root / "input.wav"
        evaluation.write_pcm(path, array("h", [0, 123, -456]))
        calls, credentials = [], ["saved-key", "replacement-key", None]

        class Store:
            def get(self, account):
                self_account = account
                self_test.assertEqual(self_account, "openrouter")
                key = credentials.pop(0)
                return {"key": key} if key else None

        class Transport:
            def request(self, url, token=None, **kwargs):
                calls.append((url, token, kwargs))
                if kwargs:
                    return dict(text="transcript", usage=dict(cost=.01, seconds=1), secret="do not copy")
                return dict(data=[dict(id=evaluation.MODEL, input_modalities=["audio"],
                                       output_modalities=["transcription"])])

        class Manager:
            store, transport = Store(), Transport()

        self_test = self
        cloud = evaluation.Cloud(None, Manager())
        result = cloud.transcribe(path, "it")
        url, token, kwargs = calls[-1]
        self.assertEqual(url, evaluation.OPENROUTER_STT)
        self.assertEqual(token, "replacement-key")
        self.assertEqual(kwargs["body"]["model"], evaluation.MODEL)
        self.assertEqual(kwargs["body"]["temperature"], 0)
        self.assertEqual(base64.b64decode(kwargs["body"]["input_audio"]["data"]), path.read_bytes())
        self.assertNotIn("secret", result)
        with self.assertRaisesRegex(ValueError, "credential"):
            cloud.transcribe(path, "it")
        self.assertEqual(len(calls), 2)

    def test_provider_failure_stops_and_persists_only_sanitized_error(self):
        path = self.suite()
        target = self.root / "failure.jsonl"

        class BrokenCloud:
            metadata = {}
            calls = 0

            def transcribe(self, *_):
                self.calls += 1
                raise evaluation.ProviderError("usage_limit", "secret provider body", http_status=402)

            def close(self):
                pass

        cloud = BrokenCloud()
        with patch.object(evaluation, "Cloud", return_value=cloud):
            self.assertEqual(evaluation.run(path, target, "openrouter"), 1)
        self.assertEqual(cloud.calls, 1)
        self.assertNotIn("secret provider body", target.read_text())
        self.assertEqual(evaluation.score_run(path, target)["aggregate"]["failed_chunks"],
                         evaluation.plan(evaluation.load_suite(path))["requests"])

    def test_cloud_payload_is_unchanged_after_rejected_input(self):
        valid, short = self.root / "valid.wav", self.root / "short.wav"
        evaluation.write_pcm(valid, array("h", [123]) * 4000)
        evaluation.write_pcm(short, array("h", [0]) * 8)
        bodies = []

        class Store:
            def get(self, _):
                return {"key": "saved-key"}

        class Transport:
            def request(self, url, token=None, **kwargs):
                if "body" not in kwargs:
                    return dict(data=[dict(id=evaluation.MODEL, input_modalities=["audio"],
                                           output_modalities=["transcription"])])
                body = kwargs["body"]
                bodies.append(body)
                if base64.b64decode(body["input_audio"]["data"]) == short.read_bytes():
                    raise evaluation.ProviderError("provider_error", "rejected", http_status=400)
                return dict(text="recognized")

        class Manager:
            store, transport = Store(), Transport()

        cloud = evaluation.Cloud(None, Manager())
        self.assertTrue(cloud.transcribe(valid, "en")["ok"])
        with self.assertRaises(evaluation.ProviderError):
            cloud.transcribe(short, "en")
        self.assertTrue(cloud.transcribe(valid, "en")["ok"])
        self.assertEqual(bodies[0], bodies[2])

    def test_input_rejection_is_recorded_once_and_later_cases_still_run(self):
        path = self.suite()
        target = self.root / "rejection.jsonl"

        class RejectOne:
            metadata = {}
            calls = 0

            def transcribe(self, *_):
                self.calls += 1
                if self.calls == 1:
                    raise evaluation.ProviderError("provider_error", "private error", http_status=400)
                return dict(ok=True, text="", error="")

            def close(self):
                pass

        cloud = RejectOne()
        with patch.object(evaluation, "Cloud", return_value=cloud), patch("builtins.print"):
            self.assertEqual(evaluation.run(path, target, "openrouter"), 1)
        self.assertEqual(cloud.calls, evaluation.plan(evaluation.load_suite(path))["requests"])
        report = evaluation.score_run(path, target)
        self.assertEqual(report["aggregate"]["failed_chunks"], 1)
        self.assertEqual(report["cases"][0]["errors"][0]["details"], {"http_status": 400})
        with patch.object(evaluation, "Cloud") as constructor:
            with self.assertRaisesRegex(ValueError, "already exists"):
                evaluation.run(path, target, "openrouter")
            constructor.assert_not_called()

    @unittest.skipUnless(shutil.which("cc"), "C compiler required for native ABI fixture")
    def test_real_c_abi_pcm_conversion_language_empty_success_and_destroy(self):
        source = self.root / "stub.c"
        source.write_text('''
#include "ascolto/local_stt.h"
#include <stdlib.h>
#include <stdio.h>
static void *load(const char *p, char *e, size_t n) { return calloc(1, sizeof(int)); }
static int transcribe(void *p, const float *pcm, int n, const char *lang,
                      const char *keywords, ascolto_stt_text_t *out, char *e, size_t cap) {
    if (n != 3 || pcm[0] != 0 || pcm[1] != 0.5f || pcm[2] != -1 || *keywords) return -1;
    if (!strcmp(lang, "it")) snprintf(out->text, sizeof(out->text), "native transcript");
    else out->text[0] = 0;
    return 0;
}
static void destroy(void *p) { free(p); }
const ascolto_stt_api_t *ascolto_stt_get_api(void) {
    static const ascolto_stt_api_t api = {ASCOLTO_STT_ABI, load, transcribe, destroy, "fixture"};
    return &api;
}
''')
        library = self.root / ("stub.dylib" if sys.platform == "darwin" else "stub.so")
        include = Path(__file__).resolve().parents[1] / "include"
        subprocess.run(["cc", "-dynamiclib" if sys.platform == "darwin" else "-shared", "-fPIC",
                        "-I", str(include), str(source), "-o", str(library)], check=True, capture_output=True)
        audio = self.root / "abi.wav"
        evaluation.write_pcm(audio, array("h", [0, 16384, -32768]))
        native = evaluation.Native(library, self.root)
        try:
            self.assertEqual(native.transcribe(audio, "it")["text"], "native transcript")
            self.assertEqual(native.transcribe(audio, "en"), dict(ok=True, text="", error=""))
            evaluation.write_pcm(audio, array("h", [1, 16384, -32768]))
            self.assertEqual(native.transcribe(audio, "it"),
                             dict(ok=False, text="", error="native_inference_failed"))
        finally:
            native.close()


if __name__ == "__main__":
    unittest.main()
