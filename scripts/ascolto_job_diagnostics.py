"""Bounded, content-free AI progress records, separate from database state."""
import json
from pathlib import Path
import time

from ascolto_files import atomic_text

STAGES = {'planning', 'provider_request', 'validation', 'validation_issue', 'citation_resolved', 'chunk_completed', 'publishing', 'registering_artifacts', 'completed', 'failed', 'repair', 'checkpoint_reused', 'checkpoint_saved', 'checkpoint_rejected'}
REASONS = {'invalid_shape', 'unknown_source', 'quote_not_in_source', 'quote_in_other_source',
           'whitespace_mismatch', 'unsupported_owner', 'unquoted_deadline', 'unexpected_fields',
           'invalid_note_text', 'invalid_evidence_count', 'invalid_section', 'invalid_schema', 'invalid_json',
           'missing_choices', 'unsupported_finish_reason'}
SECTIONS = {'summary', 'decisions', 'actions', 'open_questions'}
STATUSES = {'validating', 'running', 'completed', 'failed', 'cancelled'}
FIELDS = {'schema_version', 'summary', 'decisions', 'actions', 'open_questions',
          'text', 'evidence', 'owner', 'due', 'source_id', 'quote'}
FINISH_REASONS = {'stop', 'end_turn', 'stop_sequence', 'length', 'max_tokens', 'MAX_TOKENS',
                  'content_filter', 'tool_calls', 'SAFETY', 'RECITATION', 'OTHER', 'error'}
ERROR_TYPES = {'timeout', 'provider_unavailable', 'provider_overloaded', 'server', 'rate_limit_exceeded',
               'max_tokens_exceeded', 'context_length_exceeded', 'permission_denied', 'authentication',
               'payment_required', 'unmapped', 'invalid_request', 'not_found', 'content_policy_violation',
               'refusal', 'token_limit_exceeded', 'string_too_long'}
MAX_DIAGNOSTIC_BYTES = 128 * 1024


def safe_details(value):
    if not isinstance(value, dict):
        return {}
    result = {k: v for k, v in value.items() if k in
              {'job_id', 'input_budget', 'choice_count', 'chunk', 'total', 'completed', 'request', 'item', 'citation', 'source_index',
               'matching_source_index', 'quote_length', 'source_length', 'duration_ms', 'source_start_ms', 'source_end_ms'}
              and type(v) is int and 0 <= v <= 1_000_000_000}
    for key, choices in (('stage', STAGES), ('reason', REASONS), ('section', SECTIONS), ('status', STATUSES), ('finish_reason', FINISH_REASONS), ('error_type', ERROR_TYPES)):
        if isinstance(value.get(key), str) and value[key] in choices:
            result[key] = value[key]
    if type(value.get('http_status')) is int and 100 <= value['http_status'] <= 599:
        result['http_status'] = value['http_status']
    if type(value.get('upstream_status')) is int and 100 <= value['upstream_status'] <= 599:
        result['upstream_status'] = value['upstream_status']
    for key in ('missing_fields', 'unexpected_known_fields'):
        if isinstance(value.get(key), list):
            result[key] = sorted({v for v in value[key] if isinstance(v, str) and v in FIELDS})
    return result


def diagnostic_path(config, job):
    return Path(config).parent / 'job-diagnostics' / f'job-{int(job)}.json'


class JobDiagnostics:
    def __init__(self, config, job):
        self.path = diagnostic_path(config, job)
        self.records = []

    def record(self, event):
        details = safe_details(event)
        if not details:
            return
        self.records.append({'at_ms': int(time.time() * 1000), **details})
        # Keep the latest failure/progress even for a very long call.
        self.records = self.records[-256:]
        payload = json.dumps({'version': 1, 'events': self.records}, separators=(',', ':')) + '\n'
        while len(payload.encode()) > MAX_DIAGNOSTIC_BYTES:
            self.records.pop(0)
            payload = json.dumps({'version': 1, 'events': self.records}, separators=(',', ':')) + '\n'
        try:
            atomic_text(self.path, payload)
        except OSError:
            raise DiagnosticStorageError('AI progress metadata could not be saved.') from None


class DiagnosticStorageError(OSError):
    code = 'storage_error'


def read_diagnostics(config, job):
    try:
        with diagnostic_path(config, job).open(encoding='utf-8') as stream:
            data = json.loads(stream.read(MAX_DIAGNOSTIC_BYTES + 1))
        if data.get('version') != 1 or not isinstance(data.get('events'), list):
            return []
        records = []
        for event in data['events'][-256:]:
            if isinstance(event, dict) and type(event.get('at_ms')) is int and event['at_ms'] >= 0:
                records.append({'at_ms': event['at_ms'], **safe_details(event)})
        return records
    except (OSError, ValueError, AttributeError):
        return []


def describe(event):
    """Render only allowlisted metadata; never interpolate provider strings."""
    event = safe_details(event)
    parts = []
    if 'chunk' in event:
        parts.append(f"Chunk {event['chunk']} of {event.get('total', '?')}")
    if 'stage' in event:
        parts.append('Stage: ' + event['stage'].replace('_', ' '))
    for key in ('request', 'section', 'item', 'citation', 'source_index', 'matching_source_index', 'reason'):
        if key in event:
            parts.append(f"{key.replace('_', ' ').capitalize()}: {event[key]}")
    if 'duration_ms' in event:
        parts.append(f"Provider call: {event['duration_ms'] / 1000:.1f} s")
    if 'http_status' in event:
        parts.append(f"HTTP status: {event['http_status']}")
    if 'finish_reason' in event:
        parts.append('Finish reason: ' + event['finish_reason'])
    if 'upstream_status' in event:
        parts.append(f"Upstream status: {event['upstream_status']}")
    if 'error_type' in event:
        parts.append('Provider error type: ' + event['error_type'])
    for key in ('missing_fields', 'unexpected_known_fields'):
        if event.get(key):
            parts.append(key.replace('_', ' ').capitalize() + ': ' + ', '.join(event[key]))
    if 'source_start_ms' in event:
        seconds = event['source_start_ms'] // 1000
        parts.append(f"Source time: {seconds // 60:02d}:{seconds % 60:02d}")
    return '; '.join(parts) + '.' if parts else ''
