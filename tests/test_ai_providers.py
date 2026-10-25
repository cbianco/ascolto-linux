"""Provider/auth/privacy contracts with mock transport and in-memory secrets only."""
import base64
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import ai_providers as ai
import recap
import jwt
from cryptography.hazmat.primitives.asymmetric import rsa


class MemoryStore:
    def __init__(self):
        self.records = {}
    def get(self, account):
        value = self.records.get(account)
        return dict(value) if value else None
    def set(self, account, value):
        self.records[account] = dict(value)
    def delete(self, account):
        self.records.pop(account, None)


class MockTransport:
    def __init__(self):
        self.requests = []
        self.callback = None
        self.response = None
        self.key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        self.public = json.loads(jwt.algorithms.RSAAlgorithm.to_jwk(self.key.public_key()))
        self.public["kid"] = "test-signing-key"
        self.refreshes = 0
    def token(self, nonce="nonce", audience="oaiapp_test", **claims):
        payload = {"sub": "account-subject", "email": "example@example.com", "iss": ai.AUTH, "aud": audience, "iat": int(time.time()), "exp": int(time.time())+3600, "nonce": nonce}
        payload.update(claims)
        return jwt.encode(payload, self.key, algorithm="RS256", headers={"kid": self.public["kid"]})
    def request(self, url, token=None, **kwargs):
        self.requests.append((url, token, kwargs))
        if self.callback:
            result = self.callback(url, token, kwargs)
            if result is not None:
                return result
        if token == "invalid-key":
            raise ai.http_error(401)
        if url.endswith("openid-configuration"):
            return {"issuer": ai.AUTH, "jwks_uri": ai.AUTH+"/.well-known/jwks.json", "revocation_endpoint": ai.AUTH+"/revoke"}
        if url.endswith("jwks.json"):
            return {"keys": [self.public]}
        if url.endswith("/key") or url.endswith("/revoke"):
            return {}
        if url.endswith("/oauth/token"):
            self.refreshes += 1
            return {"access_token": "renewed-access", "refresh_token": "rotated-refresh", "expires_in": 3600}
        if url.endswith("api/v2/config"):
            return {"providers": {name: {"package": "aisdk:@ai-sdk/openai-compatible", "settings": {"baseURL": "https://opencode.ai/inference/go/v1" if name.endswith("go") else "https://opencode.ai/inference/openai/v1"}, "models": {"model-a": {"name": "Model A"}}} for name in ("opencode", "opencode-go")}}
        if url == ai.RESOURCE+"/models":
            return {"models": [{"slug": "second", "display_name": "Second", "visibility": "list"}, {"slug": "first", "display_name": "First", "visibility": "list"}, {"slug": "hidden", "visibility": "hide"}]}
        if url.endswith("/models"):
            return {"data": [{"id": "model-a", "name": "Model A", "architecture": {"input_modalities": ["text"], "output_modalities": ["text"]}}]}
        if self.response is not None:
            return self.response() if callable(self.response) else self.response
        return {"choices": [{"finish_reason": "stop", "message": {"content": "Recap"}}]}


def stream(*events):
    return io.BytesIO(b"".join(b"data: " + json.dumps(event).encode() + b"\n\n" for event in events))


class ProviderTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ascolto-ai-test-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.store, self.transport = MemoryStore(), MockTransport()
        self.manager = ai.ProviderManager(self.directory/"config.json", self.store, self.transport)
    def connect(self, provider="openrouter", key="valid-test-key"):
        return self.manager.handle({"action": "connect", "provider": provider, "key": key})
    def test_free_tier_restriction_is_sanitized_for_both_error_shapes(self):
        for payload in ({"type": "FreeTierError"}, {"type": "error", "error": {"type": "FreeTierError"}}):
            payload["message"] = "Never expose echoed credentials or transcript"
            error = urllib.error.HTTPError("https://opencode.ai/inference/v1/chat/completions", 403, "Forbidden", {}, io.BytesIO(json.dumps(payload).encode()))
            with self.subTest(payload=payload), patch("urllib.request.OpenerDirector.open", side_effect=error):
                with self.assertRaises(ai.ProviderError) as caught:
                    ai.Transport().request(error.url, "placeholder-key", body={"model": "fixture-free"})
                self.assertEqual(caught.exception.code, "free_tier_restricted")
                self.assertNotIn(payload["message"], str(caught.exception))
                self.assertNotIn("ChatGPT", str(caught.exception))
                self.assertEqual(caught.exception.details, {'http_status': 403})

    def test_structured_recap_requires_catalog_support_and_parameter_aware_routing(self):
        self.connect(); self.select()
        schema = {'type': 'object', 'properties': {}, 'additionalProperties': False}
        with self.assertRaises(ai.ProviderError) as error:
            self.manager.complete('system', 'transcript', 'session', response_schema=schema)
        self.assertEqual(error.exception.code, 'unsupported_capability')
        settings = self.manager.load()
        next(m for m in settings['profiles']['openrouter']['models'] if m['id'] == 'model-a')['supported_parameters'] = ['response_format', 'structured_outputs']
        self.manager.save(settings)
        self.manager.complete('system', 'transcript', 'session', response_schema=schema)
        body = self.transport.requests[-1][2]['body']
        self.assertEqual(body['model'], 'model-a')
        self.assertEqual(body['response_format']['json_schema']['schema'], schema)
        self.assertTrue(body['response_format']['json_schema']['strict'])
        self.assertEqual(body['provider'], {'require_parameters': True})
        self.manager.complete('system', 'transcript', 'session')
        self.assertNotIn('response_format', self.transport.requests[-1][2]['body'])

    def test_openrouter_generation_error_inside_success_response_is_sanitized(self):
        self.connect(); self.select()
        for wrapped in (False, True):
            detail = {'code': 503, 'message': 'private provider body',
                      'metadata': {'error_type': 'timeout', 'raw': 'private transcript or credential'}}
            self.transport.response = {'choices': [{'finish_reason': 'error', 'error': detail}]} if wrapped else {'error': detail}
            with self.assertRaises(ai.ProviderError) as error:
                self.manager.complete('system', 'transcript', 'session')
            self.assertEqual(error.exception.code, 'provider_timeout')
            self.assertEqual(error.exception.details['upstream_status'], 503)
            self.assertEqual(error.exception.details['error_type'], 'timeout')
            self.assertNotIn('http_status', error.exception.details)
            self.assertNotIn('private', str(error.exception) + json.dumps(error.exception.details))
    def select(self, provider="openrouter", model="model-a"):
        return self.manager.handle({"action": "use", "provider": provider, "model": model, "acknowledge_go_policy": provider == "opencode-go"})
    def openai(self, expiry=0):
        settings = self.manager.load()
        settings["accounts"] = [{"id": "account", "client_id": "oaiapp_test", "subject": "account-subject", "email": "example@example.com", "label": "Account · test"}]
        settings["profiles"]["openai"] = {"account_id": "account", "verified_at": 1, "models": [{"id": "first", "name": "First"}], "model": "first"}
        settings["selected_provider"] = "openai"
        self.manager.save(settings)
        self.store.set("openai:account", {"access_token": "old-access", "refresh_token": "old-refresh", "id_token": "old-id", "client_id": "oaiapp_test", "expires_at": expiry, "scopes": ["chatgpt.tokens.use.direct"]})
    def test_recaps_are_off_until_explicit_selection(self):
        result = self.connect()["settings"]
        self.assertFalse(result["automatic_recaps"])
        self.assertEqual(result["selected_provider"], "")
        with self.assertRaises(ai.ProviderError):
            self.manager.handle({"action": "preferences", "automatic_recaps": True})
        with self.assertRaises(ai.ProviderError):
            self.select(model="unknown")
        self.select()
        self.assertFalse(self.manager.load()["automatic_recaps"])
    def test_invalid_replacement_keeps_key_model_and_consent(self):
        self.connect(); self.select()
        before = self.manager.load()
        with self.assertRaises(ai.ProviderError):
            self.connect(key="invalid-key")
        self.assertEqual(self.store.records["openrouter"]["key"], "valid-test-key")
        self.assertEqual(self.manager.load(), before)
    def test_metadata_save_failure_restores_previous_key(self):
        self.connect(); self.select(); before = self.manager.load()
        with patch.object(self.manager, "save", side_effect=OSError("simulated disk failure")), self.assertRaises(ai.ProviderError):
            self.connect(key="new-valid-key")
        self.assertEqual(self.store.records["openrouter"]["key"], "valid-test-key")
        self.assertEqual(self.manager.load(), before)
    def test_zen_and_go_are_separate_and_key_checks_are_read_only(self):
        self.connect("opencode-zen", "zen-test-key"); self.connect("opencode-go", "go-test-key")
        self.assertEqual(self.store.records["opencode-zen"]["key"], "zen-test-key")
        self.assertEqual(self.store.records["opencode-go"]["key"], "go-test-key")
        self.assertTrue(all(url.endswith("api/v2/config") for url, _, _ in self.transport.requests))
        self.assertTrue(all(not request.get("body") for _, _, request in self.transport.requests))
    def test_missing_keyring_has_no_plaintext_fallback(self):
        with patch.object(self.store, "get", side_effect=ai.ProviderError("secure_store_unavailable", "Unlock keyring")):
            state = self.manager.handle({"action": "status"})["settings"]
            self.assertEqual(state["secure_store"], "secure_store_unavailable")
        self.assertFalse(self.manager.path.exists())
        self.assertFalse(list(self.directory.glob("*.key")))
    def test_config_and_status_never_contain_credentials(self):
        self.connect()
        combined = json.dumps(self.manager.load()) + json.dumps(self.manager.handle({"action": "status"}))
        self.assertNotIn("valid-test-key", combined)
        self.assertEqual(self.manager.path.with_name("ascolto.sqlite3").stat().st_mode & 0o777, 0o600)
    def test_disconnect_keeps_artifacts(self):
        self.connect(); self.select()
        self.manager.handle({"action": "preferences", "automatic_recaps": True})
        transcript = self.directory/"transcript.md"; transcript.write_text("Original")
        self.manager.handle({"action": "disconnect", "provider": "openrouter"})
        self.assertNotIn("openrouter", self.store.records)
        self.assertEqual(transcript.read_text(), "Original")
        self.assertFalse(self.manager.load()["automatic_recaps"])
    def test_openai_catalog_preserves_account_order(self):
        result = self.manager.models("openai", {"access_token": "mock"})
        self.assertEqual([model["id"] for model in result], ["second", "first"])
    def test_id_token_checks_signature_issuer_audience_expiry_nonce(self):
        valid = self.transport.token()
        self.assertEqual(self.manager.validate_id_token(valid, "oaiapp_test", "nonce")["sub"], "account-subject")
        for token in (self.transport.token(audience="other-client"), self.transport.token(nonce="wrong"), self.transport.token(iss="https://evil.example"), self.transport.token(exp=int(time.time())-100)):
            with self.subTest(token=token[-10:]), self.assertRaises(ai.ProviderError):
                self.manager.validate_id_token(token, "oaiapp_test", "nonce")
        forged = jwt.encode({"sub": "fake", "aud": "oaiapp_test"}, "x"*32, algorithm="HS256")
        with self.assertRaises(ai.ProviderError): self.manager.validate_id_token(forged, "oaiapp_test", "nonce")
    def test_registration_pkce_and_returning_client_are_preserved(self):
        authorization = []; callback_threads = []
        def browser(url):
            params = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query); authorization.append(params)
            def callback():
                query = urllib.parse.urlencode({"code": "mock-code", "state": params["state"][0], "client_id": "oaiapp_test"})
                urllib.request.urlopen(params["redirect_uri"][0]+"?"+query, timeout=5).read()
            thread = threading.Thread(target=callback); callback_threads.append(thread); thread.start(); return True
        def transport(url, token, kwargs):
            if url.endswith("/oauth/token") and kwargs["form"]["grant_type"] == "authorization_code":
                params = authorization[-1]; form = kwargs["form"]
                challenge = base64.urlsafe_b64encode(__import__('hashlib').sha256(form["code_verifier"].encode()).digest()).decode().rstrip("=")
                self.assertEqual(challenge, params["code_challenge"][0]); self.assertEqual(form["redirect_uri"], params["redirect_uri"][0]); self.assertEqual(form["client_id"], "oaiapp_test")
                return {"id_token": self.transport.token(nonce=params["nonce"][0]), "access_token": "mock-access", "refresh_token": "mock-refresh", "scope": ai.SCOPE, "expires_in": 3600}
        self.transport.callback = transport
        with patch.object(ai.webbrowser, "open", side_effect=browser):
            self.manager.handle({"action": "sign_in"})
            settings = self.manager.load(); account = settings["accounts"][0]
            self.assertEqual(authorization[0]["client_id"], ["dynamic_agent_client"])
            self.assertEqual(authorization[0]["agent_name_hint"], ["Ascolto"])
            self.assertEqual(authorization[0]["code_challenge_method"], ["S256"])
            host = settings["host_id"]
            self.manager.handle({"action": "sign_in", "account_id": account["id"]})
            self.assertEqual(authorization[1]["client_id"], ["oaiapp_test"])
            self.assertNotIn("agent_name_hint", authorization[1])
            self.assertNotIn("id_token_hint", authorization[1])
            self.assertEqual(authorization[1]["ext_agent_host_id"], [host])
            self.assertEqual(len(self.manager.load()["accounts"]), 1)
        for thread in callback_threads: thread.join()
    def test_callback_wrong_state_is_ignored_before_valid_callback(self):
        def browser(url):
            params = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)
            def callback():
                try:
                    urllib.request.urlopen(params["redirect_uri"][0]+"?"+urllib.parse.urlencode({"code": "bad-code", "state": "wrong"}), timeout=5).read()
                except urllib.error.HTTPError as error:
                    error.close()
                urllib.request.urlopen(params["redirect_uri"][0]+"?"+urllib.parse.urlencode({"error": "access_denied", "state": params["state"][0]}), timeout=5).read()
            threading.Thread(target=callback).start(); return True
        with patch.object(ai.webbrowser, "open", side_effect=browser), self.assertRaises(ai.ProviderError) as raised:
            self.manager.handle({"action": "sign_in"})
        self.assertEqual(raised.exception.code, "sign_in_cancelled")
        self.assertFalse(any(url.endswith("/oauth/token") for url, _, _ in self.transport.requests))
    def test_rotating_refresh_is_serialized_between_workers(self):
        self.openai()
        managers = [ai.ProviderManager(self.directory/"config.json", self.store, self.transport) for _ in range(2)]
        threads = [threading.Thread(target=lambda manager=manager: manager.handle({"action": "refresh_models", "provider": "openai"})) for manager in managers]
        for thread in threads: thread.start()
        for thread in threads: thread.join()
        self.assertEqual(self.transport.refreshes, 1)
        self.assertEqual(self.store.records["openai:account"]["refresh_token"], "rotated-refresh")
    def test_disconnect_preserves_registration_but_revokes_and_erases_tokens(self):
        self.openai(time.time()+3600)
        self.manager.handle({"action": "disconnect", "provider": "openai"})
        self.assertEqual(len(self.manager.load()["accounts"]), 1)
        self.assertNotIn("openai:account", self.store.records)
        requests = [kwargs["form"] for url, _, kwargs in self.transport.requests if url.endswith("/revoke")]
        self.assertEqual(requests[0]["client_id"], "oaiapp_test")
        self.assertEqual(requests[0]["token_type_hint"], "refresh_token")
    def test_disconnect_reports_remote_revocation_failure(self):
        self.openai()
        def fail(url, token, kwargs):
            if url.endswith("openid-configuration"): raise ai.ProviderError("network_error", "offline")
        self.transport.callback = fail
        result = self.manager.handle({"action": "disconnect", "provider": "openai"})
        self.assertIn("Remote revocation was not confirmed", result["message"])
        self.assertNotIn("openai:account", self.store.records)
    def test_responses_requires_terminal_completion_and_surfaces_usage(self):
        for response in (stream({"type": "response.output_text.delta", "delta": "Partial"}), stream({"type": "response.output_text.delta", "delta": "Partial"}, {"type": "response.failed", "response": {"error": {"code": "subscription_sharing_usage_limit_exceeded"}}})):
            with self.assertRaises(ai.ProviderError): ai.consume_responses(response)
        self.assertEqual(ai.consume_responses(stream({"type": "response.output_text.delta", "delta": "Done"}, {"type": "response.completed"})), "Done")
    def test_openai_uses_public_streaming_text_responses_without_storage(self):
        self.openai(time.time()+3600)
        self.transport.response = lambda: stream({"type": "response.output_text.delta", "delta": "Recap"}, {"type": "response.completed"})
        text, _ = self.manager.complete("Safe instruction", "Transcript", "session")
        self.assertEqual(text, "Recap")
        url, token, kwargs = self.transport.requests[-1]
        self.assertEqual(url, ai.RESOURCE+"/responses"); self.assertEqual(token, "old-access")
        self.assertTrue(kwargs["body"]["stream"]); self.assertFalse(kwargs["body"]["store"])
    def test_catalog_api_metadata_routes_arbitrary_model_names(self):
        for family, package, base, response in (("chat", "openai-compatible", "openai/v1", {"choices": [{"finish_reason": "stop", "message": {"content": "Done"}}]}), ("responses", "openai", "openai/v1", None), ("messages", "anthropic", "anthropic/v1", {"stop_reason": "end_turn", "content": [{"type": "text", "text": "Done"}]}), ("google", "google", "google/v1beta", {"candidates": [{"finishReason": "STOP", "content": {"parts": [{"text": "Done"}]}}]})):
            with self.subTest(family=family):
                config = {"package": "aisdk:@ai-sdk/"+package, "settings": {"baseURL": "https://opencode.ai/inference/"+base}}
                metadata = {"modelID": "runtime-model", "name": "Odd model"}
                api = ai.opencode_api(config, metadata)
                self.assertEqual(api["family"], family)
                settings = self.manager.load(); settings["selected_provider"] = "opencode-zen"; settings["profiles"]["opencode-zen"] = {"model": "odd-name", "verified_at": 1, "models": [{"id": "odd-name", "name": "Odd model", "api": api}]}; self.manager.save(settings); self.store.set("opencode-zen", {"key": "mock-key"})
                self.transport.response = response if response is not None else lambda: stream({"type": "response.output_text.delta", "delta": "Done"}, {"type": "response.completed"})
                self.assertEqual(self.manager.complete("system", "transcript", "session-123")[0], "Done")
                url, _, kwargs = self.transport.requests[-1]; self.assertIn("/inference/"+base+"/", url)
                self.assertEqual(kwargs["headers"]["x-opencode-session"], "session-123")
        self.assertIsNone(ai.opencode_api({"package": "aisdk:@ai-sdk/openai", "settings": {"baseURL": "https://evil.example/v1"}}, {}))
    def test_provider_truncation_keeps_previous_recap(self):
        for family, response in (("messages", {"stop_reason": "max_tokens", "content": [{"type": "text", "text": "Partial"}]}), ("google", {"candidates": [{"finishReason": "MAX_TOKENS", "content": {"parts": [{"text": "Partial"}]}}]}), ("chat", {"choices": [{"finish_reason": "length", "message": {"content": "Partial"}}]})):
            settings = self.manager.load(); settings["selected_provider"] = "opencode-zen"; settings["profiles"]["opencode-zen"] = {"model": "odd", "models": [{"id": "odd", "api": {"family": family, "base_url": "https://opencode.ai/inference/v1"}}]}; self.manager.save(settings); self.store.set("opencode-zen", {"key": "mock-key"}); self.transport.response = response
            with self.subTest(family=family), self.assertRaises(ai.ProviderError): self.manager.complete("system", "transcript", "session")
    def test_text_only_recap_works_without_audio_and_scrubs_before_network(self):
        transcript = self.directory/"transcript.md"; original = "[00:01] [ME]: Release tomorrow. Email example@example.com"; transcript.write_text(original)
        self.connect(); self.select()
        with patch.object(ai, "ProviderManager", return_value=self.manager), patch.object(recap, "get_openai_key", side_effect=AssertionError("legacy credentials must never be read")), patch.object(recap, "prepare_archive", side_effect=AssertionError("audio must never be read")):
            self.assertEqual(recap.main(["--transcript", str(transcript), "--ai-config", str(self.directory/"config.json")]), 0)
        self.assertEqual(transcript.read_text(), original)
        request = self.transport.requests[-1][2]["body"]
        self.assertNotIn("example@example.com", json.dumps(request)); self.assertIn("[omitted]", json.dumps(request))
        self.assertTrue((self.directory/"recap.md").exists())
        self.assertFalse((self.directory/"transcript.cloud.md").exists())
    def test_automatic_recap_off_never_reads_credentials_or_makes_requests(self):
        transcript = self.directory/"call.md"; transcript.write_text("Local transcript")
        with patch.object(ai, "ProviderManager", return_value=self.manager), patch.object(self.store, "get", side_effect=AssertionError("no credentials")):
            self.assertEqual(recap.main(["--transcript", str(transcript), "--ai-config", str(self.directory/"config.json"), "--automatic"]), 0)
        self.assertFalse(self.transport.requests)
    def test_disconnect_waits_for_active_session_without_resurrecting_credentials(self):
        self.connect(); self.select()
        started, release, disconnected = threading.Event(), threading.Event(), threading.Event()
        def inference(url, token, kwargs):
            if url.endswith("/chat/completions"):
                started.set(); self.assertTrue(release.wait(3)); return {"choices": [{"finish_reason": "stop", "message": {"content": "Done"}}]}
        self.transport.callback = inference
        worker = threading.Thread(target=lambda: self.manager.complete("system", "text", "session")); worker.start(); self.assertTrue(started.wait(3))
        other = ai.ProviderManager(self.directory/"config.json", self.store, self.transport)
        disconnector = threading.Thread(target=lambda: (other.handle({"action": "disconnect", "provider": "openrouter"}), disconnected.set())); disconnector.start()
        self.assertFalse(disconnected.wait(.1)); release.set(); worker.join(); disconnector.join()
        self.assertNotIn("openrouter", self.store.records)
    def test_stdio_does_not_echo_key_or_raw_errors(self):
        request = json.dumps({"action": "connect", "provider": "openrouter", "key": "valid-test-key"})
        output = io.StringIO()
        with patch.object(ai, "ProviderManager", return_value=self.manager), patch.object(sys, "stdin", io.StringIO(request)), contextlib.redirect_stdout(output):
            self.assertEqual(ai.main(["--config", str(self.directory/"config.json")]), 0)
        self.assertNotIn("valid-test-key", output.getvalue())


if __name__ == "__main__":
    unittest.main()
