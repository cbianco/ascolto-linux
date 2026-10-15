#include "ascolto.h"
#include "needle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal, safe string/number JSON extractors for Whistle's deterministic output shape:
   {"text":"...","language":"it","words":[{"word":"...","start":0.08,"end":0.40,"probability":0.642}],"ttft_ms":9.1,"decode_tps":674.8}
*/
static const char* json_extract_string(const char* src, const char* key, char* dst, size_t dst_cap) {
    if (!src || !key || !dst || dst_cap == 0) return NULL;
    dst[0] = '\0';
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char* p = strstr(src, pat);
    if (!p) return NULL;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < dst_cap) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            if (*p == 'n') dst[i++] = '\n';
            else if (*p == 't') dst[i++] = '\t';
            else dst[i++] = *p;
            p++;
        } else {
            dst[i++] = *p++;
        }
    }
    dst[i] = '\0';
    return (*p == '"') ? (p + 1) : p;
}

static double json_extract_number(const char* src, const char* key, double fallback) {
    if (!src || !key) return fallback;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char* p = strstr(src, pat);
    if (!p) return fallback;
    p += strlen(pat);
    while (*p == ' ') p++;
    return strtod(p, NULL);
}

static void parse_whistle_json(const char* json, ascolto_transcript_result_t* out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->raw_json, sizeof(out->raw_json), "%s", json ? json : "");
    if (!json) return;

    json_extract_string(json, "text", out->text, sizeof(out->text));
    json_extract_string(json, "language", out->language, sizeof(out->language));
    out->ttft_ms = (float)json_extract_number(json, "ttft_ms", 0.0);
    out->decode_tps = (float)json_extract_number(json, "decode_tps", 0.0);

    const char* words_p = strstr(json, "\"words\":[");
    if (!words_p) return;
    words_p += strlen("\"words\":[");

    int count = 0;
    const char* cur = words_p;
    while (*cur && *cur != ']' && count < ASCOLTO_MAX_WORDS) {
        const char* obj_start = strchr(cur, '{');
        if (!obj_start) break;
        const char* obj_end = strchr(obj_start, '}');
        if (!obj_end) break;

        size_t len = (size_t)(obj_end - obj_start + 1);
        char obj_buf[512];
        if (len >= sizeof(obj_buf)) len = sizeof(obj_buf) - 1;
        memcpy(obj_buf, obj_start, len);
        obj_buf[len] = '\0';

        ascolto_word_t* w = &out->words[count];
        json_extract_string(obj_buf, "word", w->word, sizeof(w->word));
        w->start_s = json_extract_number(obj_buf, "start", 0.0);
        w->end_s = json_extract_number(obj_buf, "end", 0.0);
        w->probability = (float)json_extract_number(obj_buf, "probability", 0.0);
        count++;

        cur = obj_end + 1;
    }
    out->num_words = count;
}

int ascolto_whistle_init(ascolto_whistle_t* w, const char* model_path) {
    if (!w || !model_path) return -1;
    memset(w, 0, sizeof(*w));

    /* Disable any telemetry in needle runtime */
    setenv("NEEDLE_TELEMETRY", "0", 1);
    setenv("DO_NOT_TRACK", "1", 1);

    FILE* f = fopen(model_path, "rb");
    if (!f) return -2;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return -3;
    }

    unsigned char* buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return -4;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -5;
    }
    fclose(f);

    int rc = needle_load(buf, (unsigned long long)sz);
    if (rc < 0 || (needle_models() & NEEDLE_SPEECH) == 0) {
        free(buf);
        return -6;
    }

    w->model_bytes = buf;
    w->model_size = (size_t)sz;
    w->loaded = true;
    return 0;
}

void ascolto_whistle_free(ascolto_whistle_t* w) {
    if (!w) return;
    free(w->model_bytes);
    w->model_bytes = NULL;
    w->model_size = 0;
    w->loaded = false;
}

int ascolto_whistle_transcribe(
    ascolto_whistle_t* w,
    const float* pcm,
    int num_samples,
    const char* language,
    const char* keywords,
    bool word_timestamps,
    ascolto_transcript_result_t* out_result
) {
    if (!w || !w->loaded || !pcm || num_samples <= 0 || !out_result) return -1;
    char json_buf[8192];
    json_buf[0] = '\0';

    const char* lang_arg = (language && language[0] && strcmp(language, "auto") != 0) ? language : NULL;
    const char* kw_arg = (keywords && keywords[0]) ? keywords : NULL;

    int rc = needle_transcribe(
        pcm,
        num_samples,
        lang_arg,
        kw_arg,
        word_timestamps ? 1 : 0,
        json_buf,
        (int)sizeof(json_buf)
    );
    if (rc < 0) {
        return rc;
    }
    parse_whistle_json(json_buf, out_result);
    return rc;
}

int ascolto_whistle_embed_frames(
    ascolto_whistle_t* w,
    const float* pcm,
    int num_samples,
    float** out_embeddings,
    int* out_frames
) {
    if (!w || !w->loaded || !pcm || num_samples <= 0 || !out_embeddings || !out_frames) return -1;
    *out_embeddings = NULL;
    *out_frames = 0;

    int total_floats = needle_embed(NULL, pcm, num_samples, NULL, 0);
    if (total_floats <= 0) return -2;

    float* buf = (float*)malloc((size_t)total_floats * sizeof(float));
    if (!buf) return -3;

    int got = needle_embed(NULL, pcm, num_samples, buf, total_floats);
    if (got <= 0) {
        free(buf);
        return -4;
    }

    *out_embeddings = buf;
    *out_frames = got / ASCOLTO_WHISTLE_EMBED_DIM;
    return *out_frames;
}

int ascolto_whistle_stream_push(
    ascolto_whistle_t* w,
    const float* pcm,
    int num_samples,
    const char* language,
    const char* keywords,
    char* out_json,
    int out_capacity
) {
    if (!w || !w->loaded || !pcm || num_samples <= 0 || !out_json || out_capacity <= 0) return -1;
    const char* lang_arg = (language && language[0] && strcmp(language, "auto") != 0) ? language : NULL;
    const char* kw_arg = (keywords && keywords[0]) ? keywords : NULL;
    return needle_stream_transcribe_process(pcm, num_samples, lang_arg, kw_arg, out_json, out_capacity);
}

int ascolto_whistle_stream_finalize(
    ascolto_whistle_t* w,
    char* out_json,
    int out_capacity
) {
    if (!w || !w->loaded || !out_json || out_capacity <= 0) return -1;
    return needle_stream_transcribe_stop(out_json, out_capacity);
}
