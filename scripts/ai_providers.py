#!/usr/bin/env python3
"""Shared AI settings worker. Secrets only cross stdin and the OS credential store.

The native UIs run one request per process. Settings and rotating OAuth sessions
are serialized across UIs and recap workers. This module never transmits audio.
"""
import argparse
import base64
import contextlib
import ctypes
import ctypes.util
import fcntl
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import importlib.util
import os
from pathlib import Path
import secrets
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import webbrowser

PROVIDERS = ("opencode-zen", "opencode-go", "openrouter", "openai")
NAMES = {"opencode-zen": "OpenCode Zen", "opencode-go": "OpenCode Go",
         "openrouter": "OpenRouter", "openai": "OpenAI"}
AUTH = "https://auth.openai.com"
RESOURCE = "https://api.openai.com/v1"
SCOPE = "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"
MAX_RESPONSE = 8 * 1024 * 1024
LANGUAGES = ("Italian", "English", "German", "French", "Spanish", "Dutch", "Polish")


class ProviderError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


class SecureStore:
    """Use Keychain Services or libsecret's Secret Service; never a file fallback."""
    def __init__(self, namespace):
        self.namespace = namespace
        try:
            if sys.platform == "darwin":
                self._init_macos()
            else:
                self._init_linux()
        except (OSError, AttributeError):
            raise ProviderError("secure_store_unavailable", "System credential storage is unavailable. On Linux, install libsecret and start an unlocked Secret Service keyring.") from None

    def _init_macos(self):
        self.cf = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
        self.sec = ctypes.CDLL("/System/Library/Frameworks/Security.framework/Security")
        self.cf.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
        self.cf.CFStringCreateWithCString.restype = ctypes.c_void_p
        self.cf.CFDictionaryCreate.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p), ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p]
        self.cf.CFDictionaryCreate.restype = ctypes.c_void_p
        self.cf.CFDataCreate.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_long]
        self.cf.CFDataCreate.restype = ctypes.c_void_p
        self.cf.CFDataGetLength.argtypes = [ctypes.c_void_p]
        self.cf.CFDataGetLength.restype = ctypes.c_long
        self.cf.CFDataGetBytePtr.argtypes = [ctypes.c_void_p]
        self.cf.CFDataGetBytePtr.restype = ctypes.c_void_p
        self.cf.CFRelease.argtypes = [ctypes.c_void_p]
        self.sec.SecItemCopyMatching.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        self.sec.SecItemAdd.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.sec.SecItemUpdate.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.sec.SecItemDelete.argtypes = [ctypes.c_void_p]

    def _constant(self, name):
        return ctypes.c_void_p.in_dll(self.sec if name.startswith("kSec") else self.cf, name).value

    @contextlib.contextmanager
    def _dictionary(self, values):
        owned, keys, vals = [], [], []
        for key, value in values.items():
            keys.append(self._constant(key))
            if isinstance(value, str):
                ref = self.cf.CFStringCreateWithCString(None, value.encode(), 0x08000100)
                owned.append(ref)
            elif isinstance(value, bytes):
                ref = self.cf.CFDataCreate(None, value, len(value))
                owned.append(ref)
            else:
                ref = value
            vals.append(ref)
        array = ctypes.c_void_p * len(keys)
        ref = self.cf.CFDictionaryCreate(None, array(*keys), array(*vals), len(keys), None, None)
        try:
            yield ref
        finally:
            self.cf.CFRelease(ref)
            for value in owned:
                self.cf.CFRelease(value)

    def _query(self, account):
        return {"kSecClass": self._constant("kSecClassGenericPassword"),
                "kSecAttrService": "com.cristianbianco.ascolto.ai." + self.namespace,
                "kSecAttrAccount": account}

    def _init_linux(self):
        class Attribute(ctypes.Structure):
            _fields_ = [("name", ctypes.c_char_p), ("type", ctypes.c_int)]
        class Schema(ctypes.Structure):
            _fields_ = [("name", ctypes.c_char_p), ("flags", ctypes.c_int),
                        ("attributes", Attribute * 32), ("reserved", ctypes.c_int),
                        ("padding", ctypes.c_void_p * 7)]
        class GError(ctypes.Structure):
            _fields_ = [("domain", ctypes.c_uint), ("code", ctypes.c_int), ("message", ctypes.c_char_p)]
        self.GError = GError
        self.secret = ctypes.CDLL(ctypes.util.find_library("secret-1") or "libsecret-1.so.0")
        self.glib = ctypes.CDLL(ctypes.util.find_library("glib-2.0") or "libglib-2.0.so.0")
        self.glib.g_error_free.argtypes = [ctypes.c_void_p]
        self.secret.secret_password_free.argtypes = [ctypes.c_void_p]
        self.schema = Schema()
        self.schema.name = b"com.cristianbianco.Ascolto.AI"
        self.schema.attributes[0] = Attribute(b"namespace", 0)
        self.schema.attributes[1] = Attribute(b"account", 0)
        self.secret.secret_password_lookup_sync.restype = ctypes.c_void_p
        self.secret.secret_password_lookup_sync.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        self.secret.secret_password_store_sync.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p]
        self.secret.secret_password_clear_sync.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]

    def _linux_call(self, operation, account, value=None):
        error = ctypes.POINTER(self.GError)()
        attrs = (ctypes.c_char_p(b"namespace"), ctypes.c_char_p(self.namespace.encode()),
                 ctypes.c_char_p(b"account"), ctypes.c_char_p(account.encode()), ctypes.c_void_p())
        if operation == "get":
            result = self.secret.secret_password_lookup_sync(ctypes.byref(self.schema), None, ctypes.byref(error), *attrs)
        elif operation == "set":
            result = self.secret.secret_password_store_sync(ctypes.byref(self.schema), b"default", b"Ascolto AI provider", value.encode(), None, ctypes.byref(error), *attrs)
        else:
            result = self.secret.secret_password_clear_sync(ctypes.byref(self.schema), None, ctypes.byref(error), *attrs)
        if error:
            self.glib.g_error_free(error)
            raise ProviderError("secure_store_unavailable", "The system keyring is locked or unavailable. Unlock your Secret Service keyring and try again.")
        if operation == "get" and result:
            try:
                return ctypes.string_at(result).decode()
            finally:
                self.secret.secret_password_free(result)
        if operation == "set" and not result:
            raise ProviderError("secure_store_unavailable", "The system keyring could not save this credential. The previous connection has been kept.")
        return None

    def get(self, account):
        if sys.platform != "darwin":
            value = self._linux_call("get", account)
            return json.loads(value) if value else None
        query = self._query(account)
        query["kSecReturnData"] = self._constant("kCFBooleanTrue")
        result = ctypes.c_void_p()
        with self._dictionary(query) as ref:
            status = self.sec.SecItemCopyMatching(ref, ctypes.byref(result))
        if status == -25300:
            return None
        self._check(status)
        try:
            data = ctypes.string_at(self.cf.CFDataGetBytePtr(result), self.cf.CFDataGetLength(result))
            return json.loads(data)
        finally:
            self.cf.CFRelease(result)

    def set(self, account, value):
        data = json.dumps(value, separators=(",", ":"))
        if sys.platform != "darwin":
            return self._linux_call("set", account, data)
        with self._dictionary(self._query(account)) as query, self._dictionary({"kSecValueData": data.encode()}) as update:
            status = self.sec.SecItemUpdate(query, update)
        if status == -25300:
            values = self._query(account)
            values.update({"kSecValueData": data.encode(), "kSecAttrAccessible": self._constant("kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly")})
            with self._dictionary(values) as query:
                status = self.sec.SecItemAdd(query, None)
        self._check(status)

    def delete(self, account):
        if sys.platform != "darwin":
            return self._linux_call("delete", account)
        with self._dictionary(self._query(account)) as query:
            status = self.sec.SecItemDelete(query)
        if status != -25300:
            self._check(status)

    @staticmethod
    def _check(status):
        if status:
            raise ProviderError("secure_store_unavailable", "Keychain access was denied or the keychain is locked. Unlock it and try again.")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Transport:
    def request(self, url, token=None, body=None, form=None, headers=None, stream=False):
        request_headers = {"User-Agent": "Ascolto/1.0 (call recap; local transcription)", "Accept": "application/json"}
        request_headers.update(headers or {})
        if token:
            request_headers["Authorization"] = "Bearer " + token
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            request_headers["Content-Type"] = "application/json"
        elif form is not None:
            data = urllib.parse.urlencode(form).encode()
            request_headers["Content-Type"] = "application/x-www-form-urlencoded"
        request = urllib.request.Request(url, data=data, headers=request_headers)
        try:
            response = urllib.request.build_opener(NoRedirect()).open(request, timeout=90 if stream or body else 20)
        except urllib.error.HTTPError as error:
            # Provider responses may echo secrets or transcript input. Never surface raw bodies.
            try:
                with error:
                    payload = json.loads(error.read(65536))
            except (ValueError, OSError):
                payload = {}
            code = payload.get("error", payload) if isinstance(payload, dict) else {}
            code = (code.get("code") or code.get("type", "")) if isinstance(code, dict) else code
            raise http_error(error.code, code) from None
        except (urllib.error.URLError, TimeoutError, OSError):
            raise ProviderError("network_error", "Could not reach the provider. Check your connection and try again; the saved connection has been kept.") from None
        if stream:
            return response
        with response:
            data = response.read(MAX_RESPONSE + 1)
        if len(data) > MAX_RESPONSE:
            raise ProviderError("invalid_response", "The provider returned an oversized response.")
        if not data:
            return {}
        try:
            value = json.loads(data)
            if not isinstance(value, dict):
                raise ValueError()
            return value
        except ValueError:
            raise ProviderError("invalid_response", "The provider returned an invalid response.") from None


def http_error(status, code=""):
    if status == 403 and code == "FreeTierError":
        return ProviderError("free_tier_restricted", "OpenCode restricts this free model to its own client. Choose a model available to third-party API clients in AI providers. Your current selection and saved key have been kept.")
    if code in ("subscription_sharing_usage_limit_exceeded", "subscription_sharing_usage_unavailable"):
        return ProviderError("usage_limit", "ChatGPT usage is unavailable or its limit has been reached. Open Manage usage to review your plan and app limit.")
    if code in ("invalid_grant", "invalid_token") or status == 401:
        return ProviderError("authentication_failed", "The credential was rejected or expired. Verify the key or sign in again; the previous connection has been kept.")
    if status == 403:
        return ProviderError("permission_denied", "This account does not have permission for the selected provider or model. Review access with the provider; the saved connection has been kept.")
    if status in (402, 429):
        return ProviderError("usage_limit", "Provider credits or usage limits prevent this request. Review usage with the provider.")
    return ProviderError("provider_error", f"The provider could not complete this request (HTTP {status}). Try again later.")


def safe_opencode_url(url):
    parsed = urllib.parse.urlsplit(url)
    return parsed.scheme == "https" and parsed.netloc == "opencode.ai" and parsed.path.startswith("/inference/") and not parsed.query and not parsed.fragment and ".." not in parsed.path


def opencode_api(config, metadata):
    """Retain only documented routing metadata, never provider API keys/headers."""
    package = metadata.get("package", config.get("package", ""))
    family = {"aisdk:@ai-sdk/openai": "responses", "aisdk:@ai-sdk/openai-compatible": "chat",
              "aisdk:@ai-sdk/anthropic": "messages", "aisdk:@ai-sdk/google": "google"}.get(package)
    settings = {**config.get("settings", {}), **metadata.get("settings", {})}
    base = settings.get("baseURL", "").rstrip("/")
    if not family or not safe_opencode_url(base):
        return None
    result = {"family": family, "base_url": base}
    if metadata.get("modelID"):
        result["model_id"] = metadata["modelID"]
    return result


class ProviderManager:
    def __init__(self, config_path, store=None, transport=None):
        self.path = Path(config_path).expanduser().absolute().parent.resolve() / "ai-providers.json"
        self.namespace = hashlib.sha256(str(self.path).encode()).hexdigest()[:24]
        self._store = store
        self.transport = transport or Transport()

    @property
    def store(self):
        if self._store is None:
            self._store = SecureStore(self.namespace)
        return self._store

    @contextlib.contextmanager
    def locked(self):
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        fd = os.open(self.path.with_suffix(".lock"), os.O_CREAT | os.O_RDWR | getattr(os, "O_NOFOLLOW", 0), 0o600)
        with os.fdopen(fd, "w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            yield

    def database(self):
        from ascolto_database import Database
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        return Database(self.path.with_name("ascolto.sqlite3"))

    def load(self):
        try:
            with self.database() as database:
                # Preserve the original JSON namespace and host registration during adoption.
                if self.path.is_file() and not database.connection.execute(
                    'SELECT 1 FROM legacy_imports WHERE source_path=?', (str(self.path),)).fetchone():
                    database.import_ai_settings(self.path)
                return database.load_ai_settings()
        except Exception:
            raise ProviderError("settings_error", "The settings database could not be read. Keep the database and legacy files; review their format and permissions.") from None

    def save(self, settings):
        try:
            with self.database() as database:
                database.save_ai_settings(settings)
        except Exception:
            raise ProviderError("settings_error", "The settings database could not be updated. The previous committed settings have been kept.") from None

    def status(self, settings):
        result = {"automatic_recaps": bool(settings.get("automatic_recaps")),
                  "selected_provider": settings.get("selected_provider", ""),
                  "language": settings.get("language", "Italian"), "transcription": settings.get("transcription", {}), "profiles": {},
                  "accounts": settings.get("accounts", []), "secure_store": "available",
                  "runtime_ready": all(importlib.util.find_spec(module) for module in ("numpy", "requests", "jwt", "cryptography"))}
        with self.database() as database:
            from ascolto_runtime import reconcile_jobs, archive_status
            reconcile_jobs(database, self.path)
            result['keywords'] = ', '.join(row[0] for row in database.connection.execute('SELECT text FROM keywords WHERE enabled=1 ORDER BY id'))
            result['settings_section'] = database.settings()['settings_section']
            result['archive'] = archive_status(database)
            result['jobs'] = [dict(row) for row in database.connection.execute('SELECT id,task,status,model_label_snapshot,error_code FROM recap_jobs ORDER BY id DESC LIMIT 5')]
            result['latest_session_state'] = (database.connection.execute('SELECT state FROM sessions ORDER BY started_at_ms DESC,id DESC LIMIT 1').fetchone() or [''])[0]
        for provider in PROVIDERS:
            profile = dict(settings.get("profiles", {}).get(provider, {}))
            try:
                credential = self.store.get(self.credential_id(provider, profile))
                profile["connected"] = bool(credential)
            except ProviderError as error:
                profile["connected"] = False
                result["secure_store"] = error.code
                result["secure_store_message"] = str(error)
            result["profiles"][provider] = profile
        return result

    @staticmethod
    def credential_id(provider, profile):
        return "openai:" + profile["account_id"] if provider == "openai" and profile.get("account_id") else provider

    def get_credential(self, provider, settings):
        profile = settings["profiles"].get(provider, {})
        credential = self.store.get(self.credential_id(provider, profile))
        if not credential:
            raise ProviderError("not_connected", "Connect this provider before choosing it for recaps.")
        if provider == "openai":
            credential = self.refresh(credential, self.credential_id(provider, profile))
        return credential

    def models(self, provider, credential):
        token = credential.get("key") or credential["access_token"]
        if provider.startswith("opencode-"):
            # The public model list accepts anonymous requests and does not verify a key.
            workspace = self.transport.request("https://opencode.ai/console/api/v2/config", token)
            wanted = "opencode-go" if provider == "opencode-go" else "opencode"
            config = workspace.get("providers", {}).get(wanted)
            if not isinstance(config, dict):
                raise ProviderError("profile_unavailable", f"This key has no {NAMES[provider]} profile. Check the access plan and key in OpenCode Console.")
            models = []
            for model, metadata in config.get("models", {}).items():
                if not isinstance(metadata, dict):
                    continue
                if metadata.get("disabled") or "text" not in metadata.get("capabilities", {}).get("output", ["text"]):
                    continue
                api = opencode_api(config, metadata)
                if not api:
                    continue
                models.append({"id": model, "name": metadata.get("name", model), "api": api,
                               "input_modalities": metadata.get("capabilities", {}).get("input", ["text"]),
                               "output_modalities": metadata.get("capabilities", {}).get("output", ["text"])})
            if not models:
                raise ProviderError("no_models", "This provider has no models available to your account.")
            return models
        if provider == "openrouter":
            self.transport.request("https://openrouter.ai/api/v1/key", token)
            data = self.transport.request("https://openrouter.ai/api/v1/models", token)
            return [{"id": m["id"], "name": m.get("name", m["id"]), "input_modalities": m.get("architecture", {}).get("input_modalities", ["text"]), "output_modalities": m.get("architecture", {}).get("output_modalities", ["text"]), "context_length": m.get("context_length"), "supported_parameters": m.get("supported_parameters", [])} for m in data.get("data", []) if isinstance(m, dict) and isinstance(m.get("id"), str) and "text" in m.get("architecture", {}).get("output_modalities", ["text"])]
        data = self.transport.request(RESOURCE + "/models", token)
        return [{"id": m["slug"], "name": m.get("display_name", m["slug"])} for m in data.get("models", []) if isinstance(m, dict) and m.get("visibility") == "list" and isinstance(m.get("slug"), str)]

    def connect_key(self, settings, provider, key):
        if provider == "openai":
            raise ProviderError("invalid_request", "Use Continue with ChatGPT for OpenAI.")
        if not isinstance(key, str) or not 8 <= len(key.strip()) <= 4096 or any(c.isspace() for c in key.strip()):
            raise ProviderError("invalid_key", "Enter a valid API key without spaces.")
        credential = {"key": key.strip()}
        models = self.models(provider, credential)
        if not models:
            raise ProviderError("no_models", "This account has no text models available.")
        profile = settings["profiles"].get(provider, {})
        # Verify first. A bad replacement must never remove the working credential.
        previous = self.store.get(provider)
        self.store.set(provider, credential)
        settings["profiles"][provider] = {"transcription_models": profile.get("transcription_models", []), "models": models, "model": profile.get("model", "") if any(m["id"] == profile.get("model") for m in models) else "", "verified_at": int(time.time())}
        if settings.get("selected_provider") == provider and not settings["profiles"][provider]["model"]:
            settings["selected_provider"] = ""
            settings["automatic_recaps"] = False
        self.save_with_credential_rollback(settings, provider, previous)

    def save_with_credential_rollback(self, settings, account, previous):
        try:
            self.save(settings)
        except Exception:
            try:
                if previous:
                    self.store.set(account, previous)
                else:
                    self.store.delete(account)
            except Exception:
                raise ProviderError("credential_commit_failed", "Settings could not be saved and the credential could not be restored. Reconnect this provider before using it.") from None
            raise ProviderError("settings_error", "Settings could not be saved. The previous connection has been kept. Check the settings directory permissions.") from None

    def refresh(self, credential, account):
        if credential.get("expires_at", 0) > time.time() + 60:
            return credential
        if not credential.get("refresh_token"):
            raise ProviderError("authentication_failed", "Your ChatGPT session expired. Sign in again.")
        data = self.transport.request(AUTH + "/api/accounts/oauth/token", form={"grant_type": "refresh_token", "client_id": credential["client_id"], "refresh_token": credential["refresh_token"], "resource": RESOURCE})
        if not data.get("access_token"):
            raise ProviderError("invalid_response", "ChatGPT did not return a renewed session. Sign in again.")
        refreshed = dict(credential)
        refreshed.update({k: data[k] for k in ("access_token", "refresh_token", "id_token") if data.get(k)})
        refreshed["expires_at"] = time.time() + int(data.get("expires_in", 3600))
        if data.get("scope"):
            refreshed["scopes"] = data["scope"].split()
        if "chatgpt.tokens.use.direct" not in refreshed.get("scopes", []):
            raise ProviderError("plan_permission_required", "ChatGPT plan use is not enabled. Allow it in ChatGPT settings and sign in again.")
        self.store.set(account, refreshed)
        return refreshed

    def validate_id_token(self, token, client_id, nonce):
        try:
            import jwt
        except ImportError:
            raise ProviderError("dependency_missing", "ChatGPT sign-in needs PyJWT and cryptography. Run make recap-deps, then reopen Settings.") from None
        try:
            discovery = self.transport.request(AUTH + "/.well-known/openid-configuration")
            jwks_url = discovery.get("jwks_uri", "")
            if not jwks_url.startswith(AUTH + "/") or discovery.get("issuer") != AUTH:
                raise ValueError()
            header = jwt.get_unverified_header(token)
            if header.get("alg") not in ("RS256", "ES256"):
                raise ValueError()
            keys = self.transport.request(jwks_url).get("keys", [])
            matching = [key for key in keys if key.get("kid") == header.get("kid")]
            if len(matching) != 1:
                raise ValueError()
            key = jwt.PyJWK.from_dict(matching[0], algorithm=header["alg"]).key
            claims = jwt.decode(token, key, algorithms=[header["alg"]], audience=client_id, issuer=AUTH,
                                options={"require": ["exp", "iat", "sub", "iss", "aud", "nonce"]}, leeway=30)
            if not secrets.compare_digest(str(claims["nonce"]), nonce) or not claims["sub"]:
                raise ValueError()
            return claims
        except ProviderError:
            raise
        except Exception:
            raise ProviderError("identity_validation_failed", "The ChatGPT account identity could not be verified. The existing connection has been kept. Start a new sign-in.") from None

    def sign_in(self, settings, account_id=None):
        # Force dependencies and secure-store availability before opening a browser.
        try:
            import jwt  # noqa: F401
        except ImportError:
            raise ProviderError("dependency_missing", "ChatGPT sign-in needs PyJWT and cryptography. Run make recap-deps, then reopen Settings.") from None
        self.store.get("openai")
        existing = next((a for a in settings["accounts"] if a["id"] == account_id), None)
        if account_id and not existing:
            raise ProviderError("invalid_account", "Choose a saved ChatGPT account or add another account.")
        previous = self.store.get("openai:" + account_id) if existing else None
        self.save(settings)  # Persist the stable opaque host ID before first authorization.
        state, nonce, verifier = (secrets.token_urlsafe(32) for _ in range(3))
        challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip("=")
        callback = {}
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                parsed = urllib.parse.urlsplit(self.path)
                params = urllib.parse.parse_qs(parsed.query)
                valid = parsed.path == "/auth/callback" and len(params.get("state", [])) == 1 and secrets.compare_digest(params["state"][0], state)
                if valid:
                    callback.update({k: v[0] for k, v in params.items() if len(v) == 1})
                content = ("You can return to Ascolto. This window can be closed." if valid else "This sign-in response was rejected. Return to Ascolto and try again.").encode()
                self.send_response(200 if valid else 400)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(content)))
                self.end_headers()
                self.wfile.write(content)
            def log_message(self, *args):
                pass  # Callback query parameters contain authorization codes.
        with HTTPServer(("127.0.0.1", 0), Handler) as server:
            server.timeout = 0.5
            redirect = f"http://127.0.0.1:{server.server_port}/auth/callback"
            client_id = existing["client_id"] if existing else "dynamic_agent_client"
            params = {"client_id": client_id, "ext_agent_host_id": settings["host_id"], "response_type": "code", "redirect_uri": redirect, "scope": SCOPE, "resource": RESOURCE, "state": state, "nonce": nonce, "code_challenge_method": "S256", "code_challenge": challenge}
            if existing:
                # Keep retained ID tokens in the secure store. Linux browser
                # launchers can expose URL arguments in the process table, so
                # use only the optional non-secret login hint here.
                if existing.get("email"):
                    params["login_hint"] = existing["email"]
            else:
                params["agent_name_hint"] = "Ascolto"
            if not webbrowser.open(AUTH + "/api/accounts/authorize?" + urllib.parse.urlencode(params)):
                raise ProviderError("browser_unavailable", "Could not open your default browser. Set a default browser and try again.")
            deadline = time.monotonic() + 300
            while not callback and time.monotonic() < deadline:
                server.handle_request()
        if not callback:
            raise ProviderError("sign_in_timeout", "Sign-in timed out. Start again; your existing connection has been kept.")
        if callback.get("error"):
            raise ProviderError("sign_in_cancelled", "Sign-in was cancelled or ChatGPT plan use was not allowed. Your existing connection has been kept.")
        issued = callback.get("client_id", client_id)
        if not issued or issued == "dynamic_agent_client" or (existing and issued != client_id) or not callback.get("code"):
            raise ProviderError("identity_validation_failed", "ChatGPT registration did not complete. Start a new sign-in.")
        data = self.transport.request(AUTH + "/api/accounts/oauth/token", form={"grant_type": "authorization_code", "client_id": issued, "code": callback["code"], "code_verifier": verifier, "redirect_uri": redirect, "resource": RESOURCE})
        claims = self.validate_id_token(data.get("id_token", ""), issued, nonce)
        if existing and (claims["sub"] != existing["subject"] or issued != existing["client_id"]):
            raise ProviderError("account_mismatch", "A different ChatGPT account was returned. Add it as a separate account; the selected account has been kept.")
        scopes = data.get("scope", "").split()
        if not data.get("access_token") or "chatgpt.tokens.use.direct" not in scopes:
            raise ProviderError("plan_permission_required", "ChatGPT plan use is not enabled. Allow it in ChatGPT settings and sign in again.")
        account_id = existing["id"] if existing else str(uuid.uuid4())
        credential = {"client_id": issued, "subject": claims["sub"], "access_token": data["access_token"], "refresh_token": data.get("refresh_token", ""), "id_token": data["id_token"], "scopes": scopes, "expires_at": time.time() + int(data.get("expires_in", 3600))}
        models = self.models("openai", credential)
        if not models:
            raise ProviderError("no_models", "This ChatGPT account has no eligible text models. Check plan access in ChatGPT settings.")
        previous = self.store.get("openai:" + account_id)
        self.store.set("openai:" + account_id, credential)
        account = {"id": account_id, "client_id": issued, "subject": claims["sub"], "email": claims.get("email", "ChatGPT account"), "label": (claims.get("email") or "ChatGPT account") + " · " + issued[-6:]}
        settings["accounts"] = [a for a in settings["accounts"] if a["id"] != account_id] + [account]
        settings["profiles"]["openai"] = {"account_id": account_id, "account_label": account["label"], "models": models, "model": "", "verified_at": int(time.time()), "welcome_pending": not settings.get("chatgpt_welcomed", False)}
        if settings.get("selected_provider") == "openai":
            settings["automatic_recaps"] = False
            settings["selected_provider"] = ""  # A new identity/model needs explicit selection again.
        self.save_with_credential_rollback(settings, "openai:" + account_id, previous)

    def disconnect(self, settings, provider):
        profile = settings["profiles"].get(provider, {})
        warning = ""
        account = self.credential_id(provider, profile)
        credential = self.store.get(account)
        if provider == "openai" and credential and credential.get("refresh_token"):
            try:
                discovery = self.transport.request(AUTH + "/.well-known/openid-configuration")
                endpoint = discovery.get("revocation_endpoint", "")
                if not endpoint.startswith(AUTH + "/"):
                    raise ProviderError("invalid_response", "Invalid revocation endpoint.")
                for attempt in range(3):
                    try:
                        self.transport.request(endpoint, form={"token": credential["refresh_token"], "token_type_hint": "refresh_token", "client_id": credential["client_id"]})
                        break
                    except ProviderError:
                        if attempt == 2:
                            raise
                        time.sleep(0.25 * (2 ** attempt))
            except ProviderError:
                warning = "Disconnected locally. Remote revocation was not confirmed. Disconnect Ascolto in ChatGPT Settings to end its remote access."
        self.store.delete(account)
        settings["profiles"][provider] = {"account_id": profile["account_id"], "account_label": profile.get("account_label", "")} if provider == "openai" and profile.get("account_id") else {}
        if settings.get("selected_provider") == provider:
            settings["selected_provider"] = ""
            settings["automatic_recaps"] = False
        self.save(settings)
        return warning

    def handle(self, request):
        operation = request.get("action", "status")
        if operation in ("archive_path", "refresh_transcription_models", "transcription_preferences", "recap", "provisional_recap", "transcription", "plan_transcription", "keywords", "section"):
            from ascolto_runtime import handle_request
            try:
                result = handle_request(self, request)
            except Exception as error:
                if hasattr(error, "code"):
                    raise ProviderError(error.code, str(error)) from None
                raise ProviderError("storage_error", "The task could not update its database or local artifacts. Existing files have been kept.") from None
            with self.locked():
                settings = self.load()
                state = self.status(settings)
            return {"ok": True, "settings": state, "message": result.get("message", ""), "result": result}
        provider = request.get("provider", "opencode-zen")
        if provider not in PROVIDERS:
            raise ProviderError("invalid_request", "Choose a supported AI provider.")
        with self.locked():
            settings = self.load()
            warning = ""
            if operation == "connect":
                self.connect_key(settings, provider, request.get("key", ""))
            elif operation == "sign_in":
                self.sign_in(settings, request.get("account_id"))
            elif operation == "refresh_models":
                models = self.models(provider, self.get_credential(provider, settings))
                profile = settings["profiles"][provider]
                profile["models"] = models
                if not any(m["id"] == profile.get("model") for m in models):
                    profile["model"] = ""
                    if settings["selected_provider"] == provider:
                        settings["automatic_recaps"] = False
                self.save(settings)
            elif operation == "use":
                profile = settings["profiles"].get(provider, {})
                self.get_credential(provider, settings)
                model = request.get("model", "")
                if not any(m["id"] == model for m in profile.get("models", [])):
                    raise ProviderError("model_required", "Choose a model loaded from this provider before using it for recaps.")
                if provider == "opencode-go":
                    if not request.get("acknowledge_go_policy"):
                        raise ProviderError("provider_policy_acknowledgement_required", "Acknowledge OpenCode Go's coding-plan restriction before selecting it for call recaps.")
                    with self.database() as database:
                        database.connection.execute("INSERT INTO native_settings VALUES('go_policy_acknowledged','1') ON CONFLICT(key) DO UPDATE SET value='1'")
                profile["model"] = model
                settings["selected_provider"] = provider
                self.save(settings)
            elif operation == "preferences":
                language = request.get("language", settings["language"])
                if language not in LANGUAGES:
                    raise ProviderError("invalid_request", "Choose a supported recap language.")
                automatic = bool(request.get("automatic_recaps", settings["automatic_recaps"]))
                if automatic:
                    selected = settings.get("selected_provider")
                    if not selected or not settings["profiles"].get(selected, {}).get("model"):
                        raise ProviderError("model_required", "Verify a provider, choose a model, and select Use for recaps first.")
                    self.get_credential(selected, settings)
                settings.update(language=language, automatic_recaps=automatic)
                self.save(settings)
            elif operation == "disconnect":
                warning = self.disconnect(settings, provider)
            elif operation == "acknowledge_plan":
                settings["chatgpt_welcomed"] = True
                settings["profiles"].get("openai", {})["welcome_pending"] = False
                self.save(settings)
            elif operation != "status":
                raise ProviderError("invalid_request", "Unknown AI settings action.")
            return {"ok": True, "settings": self.status(settings), "message": warning}

    def complete(self, instructions, prompt, session_id, provider=None, model=None):
        # All callers hold this lock through refresh and inference: disconnect cannot
        # race a stale token replacement or re-enable a removed connection.
        with self.locked():
            settings = self.load()
            provider = provider or settings.get("selected_provider", "")
            if provider not in PROVIDERS:
                raise ProviderError("not_selected", "Choose Use for recaps in Settings > AI providers first.")
            profile = settings["profiles"].get(provider, {})
            model = model or profile.get("model")
            if not model or not any(m["id"] == model for m in profile.get("models", [])):
                raise ProviderError("model_required", "Choose an available provider model in Settings.")
            credential = self.get_credential(provider, settings)
            token = credential.get("key") or credential["access_token"]
            if provider == "openai":
                response = self.transport.request(RESOURCE + "/responses", token, body={"model": model, "instructions": instructions, "input": prompt, "store": False, "stream": True}, stream=True)
                return consume_responses(response), model
            headers = {"x-opencode-session": session_id} if provider.startswith("opencode-") else {"X-Title": "Ascolto"}
            if provider == "openrouter":
                base, family = "https://openrouter.ai/api/v1", "chat"
            else:
                metadata = next(m for m in profile["models"] if m["id"] == model)
                api = metadata.get("api", {})
                if not safe_opencode_url(api.get("base_url", "")) or api.get("family") not in ("responses", "chat", "messages", "google"):
                    raise ProviderError("models_outdated", "Refresh this provider's models before generating a recap.")
                base, family = api["base_url"], api["family"]
                model = api.get("model_id", model)
            if family == "responses":
                response = self.transport.request(base + "/responses", token, body={"model": model, "instructions": instructions, "input": prompt, "store": False, "stream": True}, headers=headers, stream=True)
                return consume_responses(response), model
            if family == "messages":
                data = self.transport.request(base + "/messages", token, body={"model": model, "system": instructions, "max_tokens": 8192, "messages": [{"role": "user", "content": prompt}]}, headers={**headers, "anthropic-version": "2023-06-01"})
                if data.get("stop_reason") not in ("end_turn", "stop_sequence"):
                    raise ProviderError("incomplete_response", "The recap was truncated or interrupted. The previous recap has been kept.")
                text = "".join(c.get("text", "") for c in data.get("content", []) if c.get("type") == "text")
            elif family == "google":
                data = self.transport.request(base + "/models/" + urllib.parse.quote(model, safe="") + ":generateContent", token, body={"system_instruction": {"parts": [{"text": instructions}]}, "contents": [{"role": "user", "parts": [{"text": prompt}]}]}, headers=headers)
                candidates = data.get("candidates", [])
                if not candidates or any(c.get("finishReason") != "STOP" for c in candidates):
                    raise ProviderError("incomplete_response", "The recap was truncated, filtered or interrupted. The previous recap has been kept.")
                text = "".join(p.get("text", "") for c in candidates for p in c.get("content", {}).get("parts", []))
            else:
                data = self.transport.request(base + "/chat/completions", token, body={"model": model, "messages": [{"role": "system", "content": instructions}, {"role": "user", "content": prompt}], "stream": False}, headers=headers)
                choices = data.get("choices", [])
                if not choices or choices[0].get("finish_reason") not in ("stop", "end_turn"):
                    raise ProviderError("incomplete_response", "The recap response was interrupted or incomplete. The previous recap has been kept.")
                text = choices[0].get("message", {}).get("content", "")
            if not isinstance(text, str) or not text.strip():
                raise ProviderError("empty_response", "The provider returned no recap text. The previous recap has been kept.")
            return text.strip(), model


def consume_responses(response):
    completed, chunks, size = False, [], 0
    try:
        with response:
            event_lines = []
            for raw in response:
                size += len(raw)
                if size > MAX_RESPONSE:
                    raise ProviderError("invalid_response", "The recap response exceeded its size limit.")
                line = raw.decode("utf-8").rstrip("\r\n")
                if line.startswith("data:"):
                    event_lines.append(line[5:].lstrip())
                elif not line and event_lines:
                    data = "\n".join(event_lines)
                    event_lines = []
                    if data == "[DONE]":
                        continue
                    event = json.loads(data)
                    kind = event.get("type")
                    if kind == "response.output_text.delta":
                        chunks.append(event.get("delta", ""))
                    elif kind == "response.completed":
                        completed = True
                    elif kind in ("response.failed", "response.incomplete", "error"):
                        detail = event.get("response", {}).get("error") or event.get("error") or {}
                        code = detail.get("code", "") if isinstance(detail, dict) else ""
                        if code.startswith("subscription_sharing_"):
                            raise http_error(429, code)
                        raise ProviderError("incomplete_response", "The recap request failed or ended early. The previous recap has been kept.")
    except (OSError, TimeoutError, ValueError, UnicodeError):
        raise ProviderError("incomplete_response", "The recap stream was interrupted. The previous recap has been kept.") from None
    if not completed or not "".join(chunks).strip():
        raise ProviderError("incomplete_response", "The recap stream did not complete. The previous recap has been kept.")
    return "".join(chunks).strip()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, help="Path of the native app configuration; no credentials")
    args = parser.parse_args(argv)
    try:
        request = json.loads(sys.stdin.read(65537))
        if not isinstance(request, dict):
            raise ValueError()
        result = ProviderManager(args.config).handle(request)
    except ProviderError as error:
        result = {"ok": False, "error": error.code, "message": str(error)}
    except Exception:
        result = {"ok": False, "error": "settings_error", "message": "AI settings could not be updated. The saved connection has been kept. Check the settings directory permissions."}
    print(json.dumps(result, ensure_ascii=False), flush=True)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
