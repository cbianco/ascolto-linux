#!/usr/bin/env bash
# Send a synthetic recap request using an interactively supplied OpenCode key.
set +x
set -euo pipefail

command -v curl >/dev/null || { echo 'curl is required.' >&2; exit 1; }

MODEL='deepseek-v4.1-flash-free'
ENDPOINT='https://opencode.ai/inference/openai/v1/chat/completions'
SESSION="ascolto-test-$(date +%s)-$$"
API_KEY=''
RESPONSE_FILE="$(mktemp "${TMPDIR:-/tmp}/ascolto-opencode.XXXXXX")"
trap 'unset API_KEY; rm -f "$RESPONSE_FILE"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

IFS= read -r -s -p 'OpenCode Zen API key: ' API_KEY
printf '\n'
if [[ -z "$API_KEY" || "$API_KEY" == *$'\r'* ]]; then
    echo 'Enter a non-empty key without line breaks.' >&2
    exit 1
fi

printf 'Model: %s\nEndpoint: %s\n' "$MODEL" "$ENDPOINT"

# Feed the secret header through stdin so it is absent from curl's arguments.
HTTP_STATUS="$(printf 'Authorization: Bearer %s\n' "$API_KEY" | \
    curl --silent --show-error \
        --connect-timeout 15 --max-time 120 --retry 0 \
        --request POST "$ENDPOINT" \
        --user-agent 'Ascolto/1.0 (call recap; local transcription)' \
        --header @- \
        --header 'Content-Type: application/json' \
        --header 'Accept: application/json' \
        --header "x-opencode-session: $SESSION" \
        --data-binary '{"model":"deepseek-v4.1-flash-free","stream":false,"messages":[{"role":"system","content":"Summarize the meeting in three short bullet points. Use only the supplied facts."},{"role":"user","content":"Synthetic meeting: The team agreed to release the search improvement on Friday. Alex will prepare release notes by Thursday. The team has not decided whether exports should include speaker names."}]}' \
        --output "$RESPONSE_FILE" --write-out '%{http_code}')"
unset API_KEY

printf '\nHTTP %s\n' "$HTTP_STATUS"
cat "$RESPONSE_FILE"
printf '\n'

case "$HTTP_STATUS" in
    2??) exit 0 ;;
    *) exit 1 ;;
esac
