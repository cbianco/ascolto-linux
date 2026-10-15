#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIX_DIR="${ROOT_DIR}/tests/fixtures"
TMP_DIR="$(mktemp -d /tmp/ascolto-fixtures.XXXXXX)"
trap 'rm -rf "${TMP_DIR}"' EXIT

mkdir -p "${FIX_DIR}" "${TMP_DIR}"

synth_clip() {
    local voice="$1"
    local text="$2"
    local out_wav="$3"
    local filter="${4:-anull}"
    local aiff="${TMP_DIR}/tmp_clip.aiff"

    say -v "${voice}" -o "${aiff}" "${text}"
    ffmpeg -y -loglevel error -i "${aiff}" -af "${filter}" -ar 16000 -ac 1 -c:a pcm_s16le "${out_wav}"
    # A sandboxed speech service can report success while producing digital silence.
    if ! ffmpeg -hide_banner -i "${out_wav}" -af volumedetect -f null - 2>&1 | rg 'max_volume: -?[0-9]+\.' > /dev/null; then
        echo "[fixtures] ERROR: Silent synthesis for ${voice}; run with access to the macOS speech service." >&2
        return 1
    fi
    rm -f "${aiff}"
}

echo "[fixtures] Generating individual speaker clips..."
# ME (local microphone)
synth_clip "Eddy (Italiano (Italia))" \
    "Ciao ragazzi, benvenuti nella chiamata di progetto." \
    "${FIX_DIR}/me_turn1.wav"

synth_clip "Eddy (Italiano (Italia))" \
    "Ottimo lavoro, allora iniziamo subito con lo sviluppo." \
    "${FIX_DIR}/me_turn2.wav"

synth_clip "Eddy (Italiano (Italia))" \
    "Perfetto, procediamo pure con il rilascio in produzione." \
    "${FIX_DIR}/me_turn3.wav"

# SPEAKER 1 on SYS (Alice - female Italian voice, higher F0 ~210-240 Hz)
synth_clip "Alice" \
    "Ciao Cristian, qui lato design siamo pronti per partire." \
    "${FIX_DIR}/alice_turn1.wav"

synth_clip "Alice" \
    "Va bene, ti mando il documento tra cinque minuti." \
    "${FIX_DIR}/alice_turn2.wav"

# SPEAKER 2 on SYS (Reed/Rocko - lower male Italian voice, low F0 ~120 Hz, preserving consonant clarity)
synth_clip "Eddy (Italiano (Italia))" \
    "Perfetto, io ho preparato il server e i test automatici." \
    "${FIX_DIR}/rocko_turn1.wav" \
    "asetrate=22050*0.80,aresample=16000,atempo=1.25"

synth_clip "Eddy (Italiano (Italia))" \
    "Tutti i controlli sul codice sono passati con successo." \
    "${FIX_DIR}/rocko_turn2.wav" \
    "asetrate=22050*0.80,aresample=16000,atempo=1.25"

# SPEAKER 3 on SYS (mid-range voice ~175 Hz, distinct from the low male voice)
synth_clip "Reed (Italiano (Italia))" \
    "Lato database abbiamo completato la migrazione delle tabelle." \
    "${FIX_DIR}/spk3_turn1.wav" \
    "asetrate=22050*1.60,aresample=16000,atempo=0.625"

# English sample for multilingual check
synth_clip "Daniel" \
    "Hello everyone, the microphone is active and recording." \
    "${FIX_DIR}/english_sample.wav"

echo "[fixtures] Building synchronized dual-channel call (call_mic.wav + call_sys.wav)..."
# Timeline (22 seconds total):
#   0.5s : ME Turn 1 (on MIC)
#   4.5s : Alice Turn 1 (on SYS -> SPEAKER_1)
#   9.0s : Rocko Turn 1 (on SYS -> SPEAKER_2)
#  13.5s : ME Turn 2 (on MIC)
#  17.5s : Alice Turn 2 (on SYS -> should match SPEAKER_1!)

ffmpeg -y -loglevel error \
    -f lavfi -t 22 -i "anullsrc=r=16000:cl=mono" \
    -i "${FIX_DIR}/me_turn1.wav" \
    -i "${FIX_DIR}/me_turn2.wav" \
    -filter_complex "\
        [1:a]adelay=500|500[m1]; \
        [2:a]adelay=13500|13500[m2]; \
        [0:a][m1][m2]amix=inputs=3:normalize=0[out]" \
    -map "[out]" -ar 16000 -ac 1 -c:a pcm_s16le "${FIX_DIR}/call_mic.wav"

ffmpeg -y -loglevel error \
    -f lavfi -t 22 -i "anullsrc=r=16000:cl=mono" \
    -i "${FIX_DIR}/alice_turn1.wav" \
    -i "${FIX_DIR}/rocko_turn1.wav" \
    -i "${FIX_DIR}/alice_turn2.wav" \
    -filter_complex "\
        [1:a]adelay=4500|4500[s1]; \
        [2:a]adelay=9000|9000[s2]; \
        [3:a]adelay=17500|17500[s3]; \
        [0:a][s1][s2][s3]amix=inputs=4:normalize=0[out]" \
    -map "[out]" -ar 16000 -ac 1 -c:a pcm_s16le "${FIX_DIR}/call_sys.wav"

echo "[fixtures] Done. Generated files in ${FIX_DIR}:"
ls -lh "${FIX_DIR}"
