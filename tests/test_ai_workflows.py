"""Workflow contract regressions. No credentials, provider calls or microphone."""
import base64
import contextlib
import copy
import io
import json
from pathlib import Path
import shutil
import stat
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import ai_workflows as workflow
from ai_providers import ProviderError

TEXT_MODEL = {"id": "fixture/text", "input_modalities": ["text"],
              "output_modalities": ["text"], "context_length": 32000}
STT_MODEL = {"id": "fixture/stt", "input_modalities": ["audio"],
             "output_modalities": ["transcription"]}
TEXT_SELECTION = workflow.TaskSelection("openrouter", "fixture/text")
STT_SELECTION = workflow.TaskSelection("openrouter", "fixture/stt")


def empty_notes():
    return {"schema_version": 1, "summary": [], "decisions": [], "actions": [], "open_questions": []}


def note(turn, text="A supported point"):
    return {"text": text, "evidence": [{"source_id": turn.source_id, "quote": turn.text[:80]}]}


def write_audio(path, seconds=5, channels=1):
    # Distinct constant channel values make channel routing observable.
    with wave.open(str(path), "wb") as output:
        output.setparams((channels, 2, 16000, 0, "NONE", "not compressed"))
        frame = struct.pack("<" + "h" * channels, *([1000, -2000] if channels == 2 else [1000]))
        output.writeframes(frame * (seconds * 16000))


class FakeTransport:
    def __init__(self):
        self.calls = []
        self.responses = []

    def request(self, url, token=None, body=None, **kwargs):
        self.calls.append((url, token, body, kwargs))
        result = self.responses.pop(0) if self.responses else {"text": "Refined words"}
        if isinstance(result, Exception):
            raise result
        return result


class FakeManager:
    def __init__(self):
        self.settings = {"profiles": {"openrouter": {"models": [copy.deepcopy(TEXT_MODEL)],
                                                    "transcription_models": [copy.deepcopy(STT_MODEL)]}},
                         "selected_provider": "openai"}
        self.transport = FakeTransport()
        self.calls = []
        self.respond = None
        self.connected = True
        self.depth = 0

    @contextlib.contextmanager
    def locked(self):
        if self.depth:
            raise AssertionError("Nested provider lock")
        self.depth += 1
        try:
            yield
        finally:
            self.depth -= 1

    def load(self):
        return self.settings

    def save(self, value):
        self.settings = value

    def get_credential(self, provider, settings):
        if not self.connected:
            raise ProviderError("not_connected", "Connect the provider.")
        return {"key": "mock-key-only"}

    def complete(self, instructions, prompt, session_id, provider=None, model=None, response_schema=None):
        self.response_schema = response_schema
        self.calls.append((provider, model, json.loads(prompt), session_id))
        if self.respond:
            return self.respond(self.calls[-1]), model
        turns = [workflow.Turn(**t) for t in self.calls[-1][2]["transcript"]]
        result = empty_notes()
        result["summary"] = [note(t, t.text[:40]) for t in turns]
        return json.dumps(result), model


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.manager = FakeManager()
        self.events = []
        self.runner = workflow.WorkflowRunner(self.manager, self.events.append)
        self.turns = [workflow.Turn("turn-2", "MIC", "ME", 1.0, 2.0, "I will ship on Friday."),
                      workflow.Turn("turn-3", "SYS", "SPEAKER_2", 2.0, 3.0, "We agree on the release.")]

    def tearDown(self):
        self.temporary.cleanup()

    def assert_code(self, code, function, *args, **kwargs):
        with self.assertRaises(workflow.WorkflowError) as error:
            function(*args, **kwargs)
        self.assertEqual(error.exception.code, code)

    def test_native_jsonl_schemas_keep_identity_and_nonzero_offsets(self):
        source = self.root / "local.jsonl"
        source.write_text('\n'.join(json.dumps(x) for x in (
            {"event": "session_start"},
            {"channel": "SYS", "speaker": "SPEAKER_3", "start_s": 12, "end_s": 14, "text": "Remote words"},
            {"channel": "MIC", "speaker": "ME", "start_sec": 11, "end_sec": 13, "text": "Local words"})))
        turns = workflow.load_turns(source)
        self.assertEqual([(t.source_id, t.speaker, t.start_s) for t in turns],
                         [("turn-3", "ME", 11), ("turn-2", "SPEAKER_3", 12)])
        source.write_text('{"text":"bad","channel":"MIC","speaker":"Alice","start_s":0,"end_s":1}')
        self.assert_code("invalid_source", workflow.load_turns, source)
        source.write_text('{"text":"bad","channel":"MIC","speaker":"ME","start_s":NaN,"end_s":1}')
        self.assert_code("invalid_source", workflow.load_turns, source)

    def test_capability_intersection_rejects_multimodal_siwc_and_chat_audio(self):
        self.assertTrue(workflow.capabilities("openrouter", STT_MODEL).transcription)
        self.assertFalse(workflow.capabilities("openai", STT_MODEL).transcription)
        self.assertFalse(workflow.capabilities("opencode-go", STT_MODEL).transcription)
        self.assertFalse(workflow.capabilities("opencode-zen", STT_MODEL).transcription)
        self.assertFalse(workflow.capabilities("openrouter", {"id": "gemini-audio"}).transcription)
        chat_audio = {"id": "fixture/chat-audio", "input_modalities": ["text", "audio"], "output_modalities": ["text"]}
        self.assertTrue(workflow.capabilities("openrouter", chat_audio).recap)
        self.assertFalse(workflow.capabilities("openrouter", chat_audio).transcription)

    def test_catalog_discovery_uses_documented_stt_filter_and_preserves_recap(self):
        architecture = {"input_modalities": ["audio"], "output_modalities": ["transcription"]}
        self.manager.transport.responses = [{"data": {"label": "mock"}}, {"data": [
            {"id": "fixture/new-stt", "architecture": architecture},
            {"id": "fixture/chat", "architecture": {"input_modalities": ["audio"], "output_modalities": ["text"]}}]}]
        models = workflow.refresh_transcription_models(self.manager)
        self.assertEqual([m["id"] for m in models], ["fixture/new-stt"])
        self.assertIn("output_modalities=transcription", self.manager.transport.calls[1][0])
        self.assertEqual(self.manager.settings["profiles"]["openrouter"]["models"], [TEXT_MODEL])
        self.assertEqual(self.manager.settings["selected_provider"], "openai")

    def test_evidence_schema_rejects_wrong_quotes_unknown_sources_and_extra_fields(self):
        value = empty_notes()
        value["summary"] = [note(self.turns[0])]
        result = workflow.validate_recap(value, self.turns)
        citation = result["summary"][0]["evidence"][0]
        self.assertEqual((citation["start_s"], citation["end_s"], citation["speaker"]), (1, 2, "ME"))
        bad = copy.deepcopy(value)
        bad["summary"][0]["evidence"][0]["quote"] = "Invented words"
        self.assert_code("invalid_evidence", workflow.validate_recap, bad, self.turns)
        bad = copy.deepcopy(value)
        bad["summary"][0]["evidence"][0]["source_id"] = "absent"
        self.assert_code("invalid_evidence", workflow.validate_recap, bad, self.turns)
        bad = copy.deepcopy(value)
        bad["summary"][0]["evidence"][0]["start_s"] = 99
        self.assert_code("invalid_evidence", workflow.validate_recap, bad, self.turns)

    def test_owner_and_due_are_grounded_or_explicitly_unknown(self):
        value = empty_notes()
        value["actions"] = [{**note(self.turns[0], "Ship"), "owner": "ME", "due": "Friday"}]
        workflow.validate_recap(value, self.turns)
        value["actions"][0]["owner"] = "Alice"
        self.assert_code("invalid_owner", workflow.validate_recap, value, self.turns)
        value["actions"][0]["owner"] = "SPEAKER_2"
        self.assert_code("invalid_owner", workflow.validate_recap, value, self.turns)
        value["actions"][0]["owner"] = None
        value["actions"][0]["due"] = "2026-10-16"
        self.assert_code("invalid_deadline", workflow.validate_recap, value, self.turns)
        value["actions"][0]["due"] = None
        result = workflow.validate_recap(value, self.turns)
        self.assertIn("Owner: Unknown; due: Unknown", workflow.recap_markdown(result))

    def test_long_recap_chunks_all_turns_without_mutating_selected_role(self):
        turns = [workflow.Turn(f"t{i}", "MIC", "ME", i, i + 1, f"Topic {i}. " + "Words " * 300) for i in range(12)]
        runner = workflow.WorkflowRunner(self.manager, self.events.append, input_budget=7000)
        artifact = runner.recap(TEXT_SELECTION, turns, self.root)
        self.assertGreater(artifact["passes"], 1)
        sent = [turn["source_id"] for call in self.manager.calls for turn in call[2]["transcript"]]
        self.assertEqual(sent, [turn.source_id for turn in turns])
        self.assertEqual(len(artifact["summary"]), 12)
        self.assertTrue(all(call[:2] == ("openrouter", "fixture/text") for call in self.manager.calls))
        self.assertEqual(self.manager.settings["selected_provider"], "openai")
        self.assertEqual(len({call[3] for call in self.manager.calls}), 1)
        self.assertEqual(stat.S_IMODE((self.root / "recap.ai.json").stat().st_mode), 0o600)
        self.assertEqual(self.events[-1]["status"], "completed")
        self.assertNotIn("Words", json.dumps(self.events))

    def test_invalid_later_pass_does_not_replace_previous_recap(self):
        previous = self.root / "recap.ai.json"
        previous.write_text("previous result")
        self.manager.respond = lambda call: json.dumps(empty_notes()) if len(self.manager.calls) == 1 else "unstructured response"
        turns = [workflow.Turn(f"t{i}", "MIC", "ME", i, i + 1, "Word " * 500) for i in range(3)]
        runner = workflow.WorkflowRunner(self.manager, input_budget=7000)
        self.assert_code("invalid_output", runner.recap, TEXT_SELECTION, turns, self.root)
        self.assertEqual(previous.read_text(), "previous result")
        self.assertFalse((self.root / "recap.ai.md").exists())

    def test_wrong_reference_is_resolved_only_by_a_unique_exact_quote(self):
        value = empty_notes()
        value['summary'] = [note(self.turns[0])]
        value['summary'][0]['evidence'][0]['quote'] = self.turns[1].text
        self.manager.respond = lambda call: json.dumps(value)
        result = self.runner.recap(TEXT_SELECTION, self.turns, self.root)
        event = next(e for e in self.events if e.get('stage') == 'citation_resolved')
        self.assertEqual((event['chunk'], event['total'], event['section'], event['item'], event['citation']),
                         (1, 1, 'summary', 1, 1))
        self.assertEqual(event['reason'], 'quote_in_other_source')
        self.assertEqual(event['source_index'], 1)
        self.assertEqual(event['matching_source_index'], 2)
        self.assertNotIn(self.turns[1].text, json.dumps(self.events))
        self.assertEqual(result['summary'][0]['evidence'][0]['source_id'], self.turns[1].source_id)
        self.assertEqual(result['summary'][0]['evidence'][0]['speaker'], self.turns[1].speaker)
        self.assertEqual(result['requests'], 1)
        self.assertEqual(result['resolved_citations'], 1)
        self.assertEqual(value['summary'][0]['evidence'][0]['source_id'], self.turns[0].source_id)

    def test_ambiguous_or_changed_quotations_are_never_resolved_heuristically(self):
        shared = workflow.Turn('duplicate', 'SYS', 'SPEAKER_3', 4, 5, self.turns[1].text)
        value = empty_notes()
        value['summary'] = [note(self.turns[1])]
        value['summary'][0]['evidence'][0]['source_id'] = 'unknown'
        result, resolutions = workflow.resolve_exact_citations(value, self.turns + [shared])
        self.assertEqual(resolutions, [])
        self.assert_code('invalid_evidence', workflow.validate_recap, result, self.turns + [shared])
        value['summary'][0]['evidence'][0]['quote'] = self.turns[1].text.lower()
        result, resolutions = workflow.resolve_exact_citations(value, self.turns)
        self.assertEqual(resolutions, [])

    def test_reference_resolution_cannot_bypass_owner_validation(self):
        value = empty_notes()
        value['actions'] = [{**note(self.turns[1]), 'owner': 'ME', 'due': None}]
        value['actions'][0]['evidence'][0]['source_id'] = self.turns[0].source_id
        result, resolutions = workflow.resolve_exact_citations(value, self.turns)
        self.assertEqual(len(resolutions), 1)
        self.assert_code('invalid_owner', workflow.validate_recap, result, self.turns)

    def test_structured_output_is_requested_only_when_declared_by_selected_catalog(self):
        model = self.manager.settings['profiles']['openrouter']['models'][0]
        model['supported_parameters'] = ['response_format', 'structured_outputs']
        self.runner.recap(TEXT_SELECTION, self.turns, self.root)
        self.assertEqual(self.manager.response_schema, workflow.RECAP_SCHEMA)

    def test_non_object_json_fails_as_invalid_output_without_an_unsafe_repair(self):
        self.manager.respond = lambda call: '[]'
        self.assert_code('invalid_output', self.runner.recap, TEXT_SELECTION, self.turns, self.root)
        self.assertEqual(len(self.manager.calls), 1)

    def test_retry_reuses_only_revalidated_matching_chunks_after_a_later_failure(self):
        turns = [workflow.Turn(f't{i}', 'MIC', 'ME', i, i + 1, 'Word ' * 500) for i in range(3)]
        runner = workflow.WorkflowRunner(self.manager, self.events.append, input_budget=7000,
                                         checkpoint_dir=self.root / 'checkpoints', session_id='same-call')
        respond = self.manager.respond
        def fail_second(call):
            if len(self.manager.calls) == 2:
                raise ProviderError('network_error', 'Provider unavailable.')
            result = empty_notes()
            result['summary'] = [note(workflow.Turn(**call[2]['transcript'][0]))]
            return json.dumps(result)
        self.manager.respond = fail_second
        with self.assertRaises(ProviderError) as error:
            runner.recap(TEXT_SELECTION, turns, self.root)
        self.assertEqual(error.exception.code, 'network_error')
        checkpoints = list((self.root / 'checkpoints').glob('*.json'))
        self.assertEqual(len(checkpoints), 1)
        self.assertEqual(checkpoints[0].stat().st_mode & 0o777, 0o600)
        self.manager.respond = respond
        self.manager.calls.clear()
        result = runner.recap(TEXT_SELECTION, turns, self.root)
        self.assertEqual(result['cached_chunks'], 1)
        self.assertEqual(result['requests'], result['passes'] - 1)
        self.assertTrue(any(e.get('stage') == 'checkpoint_reused' for e in self.events))
        # A corrupted cache is never trusted merely because its key matches.
        saved = json.loads(checkpoints[0].read_text())
        saved['value']['summary'][0]['evidence'][0]['quote'] = 'Invented cached quote.'
        checkpoints[0].write_text(json.dumps(saved))
        self.manager.calls.clear()
        result = runner.recap(TEXT_SELECTION, turns, self.root)
        self.assertGreater(result['requests'], 0)
        self.assertTrue(any(e.get('stage') == 'checkpoint_rejected' for e in self.events))

    def test_whitespace_and_unknown_source_have_distinct_diagnostics(self):
        value = empty_notes()
        turn = workflow.Turn('private-source-id', 'MIC', 'ME', 0, 1, 'Ship  on Friday.')
        value['summary'] = [note(turn)]
        value['summary'][0]['evidence'][0]['quote'] = 'Ship on Friday.'
        with self.assertRaises(workflow.WorkflowError) as error:
            workflow.validate_recap(value, [turn])
        self.assertEqual(error.exception.details['reason'], 'whitespace_mismatch')
        value['summary'][0]['evidence'][0]['source_id'] = 'untrusted-provider-text'
        with self.assertRaises(workflow.WorkflowError) as error:
            workflow.validate_recap(value, [turn])
        self.assertEqual(error.exception.details['reason'], 'unknown_source')
        self.assertNotIn('untrusted-provider-text', json.dumps(error.exception.details))

    def test_citation_repair_keeps_notes_and_selected_model_and_validates_again(self):
        bad, good = empty_notes(), empty_notes()
        good['summary'] = [note(self.turns[0], 'Keep this note exactly.')]
        bad = copy.deepcopy(good)
        bad['summary'][0]['evidence'][0]['quote'] = 'An invented quotation.'
        self.manager.respond = lambda call: json.dumps(bad if len(self.manager.calls) == 1 else good)
        runner = workflow.WorkflowRunner(self.manager, self.events.append, max_repairs=2)
        result = runner.recap(TEXT_SELECTION, self.turns, self.root)
        self.assertEqual(result['requests'], 2)
        self.assertEqual(result['summary'][0]['text'], 'Keep this note exactly.')
        self.assertEqual(result['summary'][0]['evidence'][0]['quote'], self.turns[0].text)
        self.assertEqual(len(self.manager.calls), 2)
        self.assertTrue(all(call[:2] == ('openrouter', 'fixture/text') for call in self.manager.calls))
        self.assertIn('previous_candidate', self.manager.calls[1][2])
        self.assertTrue(any(e.get('stage') == 'repair' and e.get('reason') == 'quote_not_in_source' for e in self.events))
        self.assertNotIn('An invented quotation.', json.dumps(self.events))

    def test_repair_is_bounded_and_cannot_remove_or_rewrite_claims(self):
        bad = empty_notes()
        bad['summary'] = [note(self.turns[0], 'A note that must remain.')]
        bad['summary'][0]['evidence'][0]['quote'] = 'Unsupported quote.'
        self.manager.respond = lambda call: json.dumps(bad)
        runner = workflow.WorkflowRunner(self.manager, max_repairs=2)
        self.assert_code('invalid_evidence', runner.recap, TEXT_SELECTION, self.turns, self.root)
        self.assertEqual(len(self.manager.calls), 3)
        self.assertFalse((self.root / 'recap.ai.json').exists())
        self.manager.calls.clear()
        self.manager.respond = lambda call: json.dumps(bad if len(self.manager.calls) == 1 else empty_notes())
        self.assert_code('invalid_output', runner.recap, TEXT_SELECTION, self.turns, self.root)
        rewritten = copy.deepcopy(bad)
        rewritten['summary'][0]['text'] = 'Changed note.'
        self.assert_code('invalid_output', workflow.validate_repair, rewritten, bad)

    def test_citation_repairs_share_the_global_request_budget(self):
        bad = empty_notes()
        bad['summary'] = [note(self.turns[0])]
        bad['summary'][0]['evidence'][0]['source_id'] = 'missing'
        bad['summary'][0]['evidence'][0]['quote'] = 'A quote absent from every source.'
        self.manager.respond = lambda call: json.dumps(bad)
        runner = workflow.WorkflowRunner(self.manager, max_requests=1, max_repairs=2)
        self.assert_code('request_limit', runner.recap, TEXT_SELECTION, self.turns, self.root)
        self.assertEqual(len(self.manager.calls), 1)

    def test_repair_preserves_valid_citations_when_model_changes_them(self):
        original = empty_notes()
        original['summary'] = [note(self.turns[0], 'Keep the first note.'), note(self.turns[1], 'Keep the second note.')]
        original['summary'][1]['evidence'][0]['quote'] = 'Invalid quote.'
        proposed = copy.deepcopy(original)
        proposed['summary'][0]['evidence'][0]['quote'] = 'A new regression in a valid citation.'
        proposed['summary'][1]['evidence'][0]['quote'] = self.turns[1].text
        self.manager.respond = lambda call: json.dumps(original if len(self.manager.calls) == 1 else proposed)
        result = self.runner.recap(TEXT_SELECTION, self.turns, self.root)
        self.assertEqual(result['requests'], 2)
        self.assertEqual(result['summary'][0]['evidence'][0]['quote'], self.turns[0].text)
        self.assertEqual(result['summary'][1]['evidence'][0]['quote'], self.turns[1].text)

    def test_repair_preserves_supported_assignments_and_uses_redacted_context(self):
        original = empty_notes()
        original['actions'] = [{**note(self.turns[0]), 'owner': 'ME', 'due': 'Friday'}]
        proposed = copy.deepcopy(original)
        proposed['actions'][0].update(owner=None, due=None)
        merged = workflow.merge_repair(original, proposed, self.turns)
        self.assertEqual(merged['actions'][0]['owner'], 'ME')
        self.assertEqual(merged['actions'][0]['due'], 'Friday')
        details = {'section': 'actions', 'item': 1, 'citation': 1}
        context = workflow.repair_context(original, details, self.turns)
        self.assertEqual(context['cited_turn']['source_id'], self.turns[0].source_id)
        self.assertEqual(len(context['nearby_turns']), 2)

    def test_joined_quote_can_be_replaced_by_separate_exact_source_citations(self):
        original = empty_notes()
        original['summary'] = [note(self.turns[0])]
        original['summary'][0]['evidence'][0]['quote'] = self.turns[0].text + ' ' + self.turns[1].text
        corrected = copy.deepcopy(original)
        corrected['summary'][0]['evidence'] = [note(t)['evidence'][0] for t in self.turns]
        self.manager.respond = lambda call: json.dumps(original if len(self.manager.calls) == 1 else corrected)
        result = self.runner.recap(TEXT_SELECTION, self.turns, self.root)
        self.assertEqual(result['requests'], 2)
        self.assertEqual(len(result['summary'][0]['evidence']), 2)

    def test_repair_can_neither_drop_valid_evidence_nor_omit_bad_evidence_without_replacement(self):
        original = empty_notes()
        original['summary'] = [{'text': 'Keep this claim.', 'evidence': [note(t)['evidence'][0] for t in self.turns]}]
        corrected = copy.deepcopy(original)
        corrected['summary'][0]['evidence'] = []
        merged = workflow.merge_repair(original, corrected, self.turns)
        self.assertEqual(merged['summary'][0]['evidence'], original['summary'][0]['evidence'])
        original['summary'][0]['evidence'][1]['quote'] = 'Missing evidence.'
        corrected['summary'][0]['evidence'] = [note(self.turns[0])['evidence'][0]]
        merged = workflow.merge_repair(original, corrected, self.turns)
        self.assert_code('invalid_evidence', workflow.validate_recap, merged, self.turns)

    def test_every_bad_citation_is_reported_in_one_correction_request(self):
        good = empty_notes()
        good['summary'] = [note(t, f'Keep note {i}.') for i, t in enumerate(self.turns)]
        bad = copy.deepcopy(good)
        for item in bad['summary']:
            item['evidence'][0]['quote'] = 'Unsupported joined text.'
        def respond(call):
            if len(self.manager.calls) == 1:
                return json.dumps(bad)
            self.assertEqual(len(call[2]['validation_issues']), 2)
            self.assertEqual([i['item'] for i in call[2]['validation_issues']], [1, 2])
            self.assertEqual(len(call[2]['repair_contexts']), 2)
            return json.dumps(good)
        self.manager.respond = respond
        result = self.runner.recap(TEXT_SELECTION, self.turns, self.root)
        self.assertEqual(result['requests'], 2)
        self.assertEqual(len(result['summary']), 2)

    def test_repair_cannot_change_action_owner_or_deadline_to_another_value(self):
        original = empty_notes()
        original['actions'] = [{**note(self.turns[0]), 'owner': 'ME', 'due': 'Friday'}]
        updated = copy.deepcopy(original)
        updated['actions'][0].update(owner='SPEAKER_2')
        self.assert_code('invalid_output', workflow.validate_repair, updated, original)
        updated['actions'][0].update(owner=None, due=None)
        workflow.validate_repair(updated, original)

    def test_malformed_item_fields_are_diagnosed_and_repaired_without_changing_notes(self):
        good = empty_notes()
        good['actions'] = [{**note(self.turns[0]), 'owner': None, 'due': None}]
        bad = copy.deepcopy(good)
        del bad['actions'][0]['due']
        bad['actions'][0]['private-unexpected-field'] = 'private provider text'
        self.manager.respond = lambda call: json.dumps(bad if len(self.manager.calls) == 1 else good)
        result = self.runner.recap(TEXT_SELECTION, self.turns, self.root)
        repair = next(e for e in self.events if e.get('stage') == 'repair')
        self.assertEqual(repair['reason'], 'unexpected_fields')
        self.assertEqual(repair['missing_fields'], ['due'])
        self.assertNotIn('private', json.dumps(self.events))
        self.assertEqual(result['requests'], 2)
        self.assertEqual(result['actions'][0]['text'], good['actions'][0]['text'])

    def test_provisional_and_final_outputs_coexist_and_redact_outbound_identifiers(self):
        turns = [workflow.Turn("t", "MIC", "ME", 0, 1, "Contact me@example.test about the release.")]
        self.runner.recap(TEXT_SELECTION, turns, self.root, mode="provisional")
        self.runner.recap(TEXT_SELECTION, turns, self.root, mode="final")
        self.assertTrue((self.root / "recap.provisional.ai.json").exists())
        self.assertTrue((self.root / "recap.ai.json").exists())
        self.assertNotIn("me@example.test", json.dumps(self.manager.calls))
        self.assertIn("[omitted]", json.dumps(self.manager.calls))

    def test_provider_limit_or_expiry_is_not_retried_with_a_fallback(self):
        def reject(call):
            raise ProviderError("usage_limit", "Mock limit.")
        self.manager.respond = reject
        with self.assertRaises(ProviderError):
            self.runner.recap(TEXT_SELECTION, self.turns, self.root)
        self.assertEqual(len(self.manager.calls), 1)
        self.assertEqual(self.events[-1]["error_code"], "usage_limit")
        self.assertEqual(self.manager.transport.calls, [])

    def test_unknown_model_go_policy_and_request_budget_stop_before_inference(self):
        self.assert_code("model_unavailable", self.runner.recap, workflow.TaskSelection("openrouter", "missing"), self.turns, self.root)
        self.manager.settings["profiles"]["opencode-go"] = {"models": [TEXT_MODEL]}
        self.assert_code("provider_policy_acknowledgement_required", self.runner.recap,
                         workflow.TaskSelection("opencode-go", "fixture/text"), self.turns, self.root)
        runner = workflow.WorkflowRunner(self.manager, max_requests=1, input_budget=7000)
        turns = [workflow.Turn(f"t{i}", "MIC", "ME", i, i + 1, "Word " * 500) for i in range(3)]
        self.assert_code("request_limit", runner.recap, TEXT_SELECTION, turns, self.root)
        self.assertEqual(self.manager.calls, [])

    def test_audio_requires_explicit_true_consent_before_decode_or_network(self):
        for opt_in in (False, "true", 1):
            self.assert_code("audio_consent_required", self.runner.transcribe, STT_SELECTION,
                             self.root / "absent.m4a", self.turns, self.root, cloud_audio_opt_in=opt_in)
        self.assertEqual(self.manager.transport.calls, [])

    @unittest.skipUnless(shutil.which("ffmpeg"), "Local ffmpeg is unavailable")
    def test_real_local_channel_decode_keeps_interleaving_offsets_and_originals(self):
        archive = self.root / "call.wav"
        write_audio(archive, seconds=6, channels=2)
        original = archive.read_bytes()
        turns = [workflow.Turn("s", "SYS", "SPEAKER_3", 4, 5, "Remote original"),
                 workflow.Turn("m", "MIC", "ME", 2, 3, "Local original")]
        temporary_paths = []
        original_decode = workflow.decoded_channels
        @contextlib.contextmanager
        def observed_decode(*args):
            with original_decode(*args) as channels:
                for path in channels.values():
                    temporary_paths.append(path)
                    self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
                    self.assertEqual(stat.S_IMODE(path.parent.stat().st_mode), 0o700)
                yield channels
        with patch.object(workflow, "decoded_channels", observed_decode):
            records = self.runner.transcribe(STT_SELECTION, archive, turns, self.root, cloud_audio_opt_in=True, language="it", scope="refine")
        self.assertEqual([(r["speaker"], r["start_s"], r["end_s"]) for r in records], [("ME", 2, 3), ("SPEAKER_3", 4, 5)])
        pcm_values = []
        for url, token, body, kwargs in self.manager.transport.calls:
            self.assertEqual(url, workflow.OPENROUTER_STT)
            self.assertEqual(body["response_format"], "json")
            self.assertNotIn("timestamp_granularities", body)
            with wave.open(io.BytesIO(base64.b64decode(body["input_audio"]["data"])), "rb") as source:
                self.assertEqual(source.getnframes(), 16000)
                pcm_values.append(struct.unpack("<h", source.readframes(1))[0])
        self.assertEqual(pcm_values, [1000, -2000])
        self.assertEqual(archive.read_bytes(), original)
        self.assertTrue(all(not path.exists() for path in temporary_paths))
        self.assertTrue((self.root / "transcript.refined.ai.jsonl").exists())
        self.assertNotIn("Refined words", json.dumps(self.events))

    @unittest.skipUnless(shutil.which("ffmpeg"), "Local ffmpeg is unavailable")
    def test_linux_separate_channels_and_failure_cleanup_without_publication(self):
        mic, sys_audio = self.root / "mic.wav", self.root / "sys.wav"
        write_audio(mic)
        write_audio(sys_audio)
        original_temp = tempfile.TemporaryDirectory
        created = []
        def observed_temp(*args, **kwargs):
            temporary = original_temp(*args, **kwargs)
            created.append(Path(temporary.name))
            return temporary
        self.manager.transport.responses = [ProviderError("authentication_failed", "Mock expiry.")]
        with patch.object(workflow.tempfile, "TemporaryDirectory", observed_temp):
            with self.assertRaises(ProviderError):
                self.runner.transcribe(STT_SELECTION, {"MIC": mic, "SYS": sys_audio}, self.turns,
                                       self.root, cloud_audio_opt_in=True, scope="refine")
        self.assertEqual(len(self.manager.transport.calls), 1)
        self.assertTrue(created and all(not path.exists() for path in created))
        self.assertFalse((self.root / "transcript.refined.ai.jsonl").exists())

    @unittest.skipUnless(shutil.which("ffmpeg"), "Local ffmpeg is unavailable")
    def test_full_audio_recovers_unrecognized_time_without_fabricating_speakers(self):
        archive = self.root / "whole-call.wav"
        write_audio(archive, seconds=62, channels=2)
        original = archive.read_bytes()
        self.manager.transport.responses = [{"text": ""}, {"text": "Recovered missing remote speech"}] + [
            {"text": "Additional speech"}] * 4
        local = [workflow.Turn("local", "SYS", "SPEAKER_3", 40, 41, "Only local recognition")]
        records = self.runner.transcribe(STT_SELECTION, archive, local, self.root,
                                         cloud_audio_opt_in=True, scope="full")
        self.assertEqual([(r["channel"], r["start_s"], r["end_s"]) for r in records],
                         [("MIC", 0, 30), ("SYS", 0, 30), ("MIC", 30, 60),
                          ("SYS", 30, 60), ("MIC", 60, 62), ("SYS", 60, 62)])
        self.assertTrue(records[0]["empty_result"])
        self.assertEqual(records[1]["text"], "Recovered missing remote speech")
        self.assertTrue(all(r["speaker"] == "UNKNOWN" for r in records if r["channel"] == "SYS"))
        self.assertTrue(all(r["speaker"] == "ME" for r in records if r["channel"] == "MIC"))
        self.assertEqual(records[3]["local_speaker_evidence"], [
            {"source_id": "local", "speaker": "SPEAKER_3", "start_s": 40, "end_s": 41}])
        self.assertEqual(archive.read_bytes(), original)
        self.assertTrue((self.root / "transcript.cloud.ai.jsonl").exists())
        self.assertFalse((self.root / "transcript.refined.ai.jsonl").exists())

    @unittest.skipUnless(shutil.which("ffmpeg"), "Local ffmpeg is unavailable")
    def test_full_audio_works_with_no_recognized_turns_and_checks_request_budget(self):
        archive = self.root / "empty-local-call.wav"
        write_audio(archive, channels=2)
        local = self.root / "local.jsonl"
        local.write_text('{"event":"session_start"}\n')
        self.assertEqual(workflow.load_turns(local, allow_empty=True), [])
        runner = workflow.WorkflowRunner(self.manager, max_requests=1)
        self.assert_code("request_limit", runner.transcribe, STT_SELECTION, archive, [], self.root,
                         cloud_audio_opt_in=True, scope="full")
        self.assertEqual(self.manager.transport.calls, [])
        records = self.runner.transcribe(STT_SELECTION, archive, [], self.root,
                                         cloud_audio_opt_in=True, scope="full")
        self.assertEqual(len(records), 2)
        self.assertTrue(all(not r["local_speaker_evidence"] for r in records))

    @unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "Local audio tools are unavailable")
    def test_mono_archive_is_rejected_without_inventing_a_system_channel(self):
        archive = self.root / "mono.wav"
        write_audio(archive)
        self.assert_code("audio_decode_failed", self.runner.transcribe, STT_SELECTION, archive,
                         [], self.root, cloud_audio_opt_in=True, scope="full")
        self.assertEqual(self.manager.transport.calls, [])

    def test_cloud_result_can_be_an_explicit_recap_source_without_losing_provenance(self):
        transcript = self.root / "cloud.jsonl"
        transcript.write_text(json.dumps({"event": "cloud_transcription", "channel": "MIC", "speaker": "ME",
                                          "start_s": 0, "end_s": 30, "text": "", "empty_result": True}) + "\n" +
                              json.dumps({"event": "cloud_transcription", "channel": "SYS", "speaker": "UNKNOWN",
                                          "start_s": 30, "end_s": 60, "text": "Recovered words"}) + "\n")
        turns = workflow.load_turns(transcript)
        self.assertEqual(len(turns), 1)
        result = self.runner.recap(TEXT_SELECTION, turns, self.root, source_kind="cloud_transcription")
        self.assertEqual(result["source"], "cloud_transcription")
        self.assertEqual(result["summary"][0]["evidence"][0]["start_s"], 30)
        self.assertEqual(result["summary"][0]["evidence"][0]["speaker"], "UNKNOWN")

    def test_cli_preflight_reports_a_plan_above_the_execution_budget_without_upload(self):
        mic, sys_audio = self.root / "mic.wav", self.root / "sys.wav"
        write_audio(mic)
        write_audio(sys_audio)
        @contextlib.contextmanager
        def decoded(*args):
            yield {"MIC": mic, "SYS": sys_audio}
        request = {"action": "plan_transcription", "selection": {"provider": "openrouter", "model": "fixture/stt"},
                   "archive": "/unused/mock.m4a", "scope": "full", "max_requests": 1}
        output = io.StringIO()
        with patch("ai_providers.ProviderManager", return_value=self.manager), patch.object(workflow, "decoded_channels", decoded), \
                patch.object(sys, "stdin", io.StringIO(json.dumps(request))), contextlib.redirect_stdout(output):
            code = workflow.main(["--config", str(self.root / "config.json")])
        result = json.loads(output.getvalue())
        self.assertEqual(code, 0)
        self.assertTrue(result["ok"])
        self.assertEqual(result["result"]["requests"], 2)
        self.assertFalse(result["result"]["within_request_limit"])
        self.assertEqual(self.manager.transport.calls, [])

    def test_long_local_turn_is_bounded_and_provider_speaker_guesses_are_ignored(self):
        turns = [workflow.Turn("t", "SYS", "SPEAKER_8", 100, 165, "Original")]
        spans = self.runner.plan_transcription(STT_SELECTION, turns)
        self.assertEqual([(s, e) for _, s, e in spans], [(100, 130), (130, 160), (160, 165)])
        valid = workflow.validated_provider_segments([{"start": 0, "end": 2, "text": "Words", "speaker": "Alice"}], 2)
        self.assertEqual(valid, [{"start": 0, "end": 2, "text": "Words"}])
        self.assert_code("invalid_output", workflow.validated_provider_segments,
                         [{"start": 0, "end": 10, "text": "Words"}], 2)


if __name__ == "__main__":
    unittest.main()
