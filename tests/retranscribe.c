#include "ascolto/retranscribe.h"
#include "ascolto/deferred_store.h"
#include "ascolto/storage.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "Failed at %d: %s\n", __LINE__, #x); abort(); } checks++; } while (0)
static void u16(FILE *f, unsigned n) { fputc(n & 255, f); fputc((n >> 8) & 255, f); }
static void u32(FILE *f, unsigned n) { u16(f, n & 65535); u16(f, n >> 16); }
static void wav(const char *path, unsigned channels, unsigned rate) {
    FILE *f = fopen(path, "wb"); CHECK(f);
    unsigned bytes = rate * channels * 2;
    fwrite("RIFF", 1, 4, f); u32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
    u32(f, 16); u16(f, 1); u16(f, channels); u32(f, rate); u32(f, rate * channels * 2);
    u16(f, channels * 2); u16(f, 16); fwrite("data", 1, 4, f); u32(f, bytes);
    for (unsigned i = 0; i < rate; i++) {
        u16(f, 8192);
        if (channels == 2) u16(f, (uint16_t)-16384);
    }
    CHECK(!fclose(f));
}
static void m4a(const char *path) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CHECK(url);
    AudioStreamBasicDescription compressed = {.mSampleRate = 16000, .mFormatID = kAudioFormatMPEG4AAC,
        .mChannelsPerFrame = 2, .mFramesPerPacket = 1024};
    UInt32 format_size = sizeof(compressed);
    CHECK(!AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, NULL, &format_size, &compressed));
    ExtAudioFileRef file = NULL;
    CHECK(!ExtAudioFileCreateWithURL(url, kAudioFileM4AType, &compressed, NULL, kAudioFileFlags_EraseFile, &file));
    CFRelease(url);
    AudioStreamBasicDescription raw = {.mSampleRate = 16000, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagsNativeFloatPacked, .mBytesPerPacket = 8,
        .mFramesPerPacket = 1, .mBytesPerFrame = 8, .mChannelsPerFrame = 2, .mBitsPerChannel = 32};
    CHECK(!ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof(raw), &raw));
    float tones[8192];
    for (unsigned position = 0; position < 16000; position += 4096) {
        unsigned count = 16000 - position > 4096 ? 4096 : 16000 - position;
        for (unsigned i = 0; i < count; i++) {
            double time = (double)(position + i) / 16000;
            tones[2 * i] = (float)(.2 * sin(2 * M_PI * 440 * time));
            tones[2 * i + 1] = (float)(.6 * sin(2 * M_PI * 880 * time));
        }
        AudioBufferList buffers = {.mNumberBuffers = 1,
            .mBuffers = {{.mNumberChannels = 2, .mDataByteSize = count * 8, .mData = tones}}};
        CHECK(!ExtAudioFileWrite(file, count, &buffers));
    }
    CHECK(!ExtAudioFileDispose(file));
}
static void digest(const char *path, unsigned char out[CC_SHA256_DIGEST_LENGTH]) {
    CC_SHA256_CTX state;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_SHA256_Init(&state);
    FILE *f = fopen(path, "rb"); CHECK(f);
    unsigned char buffer[4096]; size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f))) CC_SHA256_Update(&state, buffer, (CC_LONG)n);
    CHECK(!ferror(f)); CHECK(!fclose(f)); CC_SHA256_Final(out, &state);
#pragma clang diagnostic pop
}
static int scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *q = NULL; CHECK(!sqlite3_prepare_v2(db, sql, -1, &q, NULL));
    CHECK(sqlite3_step(q) == SQLITE_ROW); int n = sqlite3_column_int(q, 0); sqlite3_finalize(q); return n;
}
static int versions(const char *call) {
    char path[1200]; snprintf(path, sizeof(path), "%s/Data/Local-transcription-history", call);
    DIR *d = opendir(path); if (!d) return 0;
    int n = 0; struct dirent *entry;
    while ((entry = readdir(d))) if (!strncmp(entry->d_name, "version-", 8)) n++;
    closedir(d); return n;
}
static bool cancel_decode(void *context) { int *calls = context; return ++*calls >= 3; }
static int64_t source(ascolto_db_t *db, const char *call, const char *audio) {
    char md[1200], jsonl[1200]; snprintf(md, sizeof(md), "%s/transcript.md", call);
    snprintf(jsonl, sizeof(jsonl), "%s/Data/transcript.jsonl", call);
    FILE *f = fopen(md, "wb"); CHECK(f); fputs("Original transcript.\n", f); CHECK(!fclose(f));
    f = fopen(jsonl, "wb"); CHECK(f); fputs("{\"original\":true}\n", f); CHECK(!fclose(f));
    ascolto_db_session_t value = {.started_at_ms = 1000, .capture_backend = "coreaudio",
        .call_title = "Fixture's call", .meeting_title = "Fixture meeting", .source_app = "Fixture app",
        .vocabulary_snapshot_json = "[\"Original keyword\",\"l'été\"]",
        .metadata_json = "{\"local_model\":\"whistle\",\"in_app\":\"Original app\",\"out_app\":\"Remote app\",\"mic\":\"Fixture mic\",\"diarizer_threshold\":0.72,\"cloud_recap\":true,\"recap_tts\":true,\"retain_audio\":true}"};
    int64_t id = 0; CHECK(!ascolto_db_session_create(db, &value, &id));
    CHECK(!ascolto_db_session_state(db, id, "completed", 2000, NULL));
    ascolto_db_artifact_t a = {.session_id = id, .state = "ready", .created_at_ms = 2000};
    a.kind = "transcript_md"; a.path = md; CHECK(!ascolto_db_artifact_put(db, &a));
    a.kind = "transcript_jsonl"; a.path = jsonl; CHECK(!ascolto_db_artifact_put(db, &a));
    a.kind = "audio_m4a"; a.path = audio; CHECK(!ascolto_db_artifact_put(db, &a));
    ascolto_db_utterance_t turn = {.session_id = id, .channel = "SYS", .start_ms = 0,
        .end_ms = 1000, .text = "Original text", .language = "it"};
    CHECK(!ascolto_db_utterance_append(db, &turn));
    return id;
}
static void verify_channels(const ascolto_deferred_job_t *job, bool resampled) {
    CHECK(job->sample_count >= 15990 && job->sample_count <= 16010);
    FILE *mic = fopen(job->mic_path, "rb"), *sys = fopen(job->sys_path, "rb"); CHECK(mic && sys);
    double mic_sum = 0, sys_sum = 0; size_t total = 0;
    float left[4096], right[4096];
    for (;;) {
        size_t n = fread(left, sizeof(float), 4096, mic);
        CHECK(n == fread(right, sizeof(float), 4096, sys));
        if (!n) break;
        for (size_t i = 0; i < n; i++) { mic_sum += left[i]; sys_sum += right[i]; }
        total += n;
    }
    CHECK((int64_t)total == job->sample_count);
    double tolerance = resampled ? .001 : .000001;
    CHECK(fabs(mic_sum / total - .25) < tolerance && fabs(sys_sum / total + .5) < tolerance);
    CHECK(!fclose(mic)); CHECK(!fclose(sys));
    struct stat st; CHECK(!stat(job->mic_path, &st) && (st.st_mode & 0777) == 0600);
}
static void verify_encoded_channels(const ascolto_deferred_job_t *job) {
    CHECK(job->sample_count >= 15990 && job->sample_count <= 16010);
    for (int ch = 0; ch < 2; ch++) {
        FILE *f = fopen(ch ? job->sys_path : job->mic_path, "rb"); CHECK(f);
        double real[2] = {0}, imaginary[2] = {0};
        float value; int64_t i = 0;
        while (fread(&value, sizeof(value), 1, f) == 1) {
            for (int tone = 0; tone < 2; tone++) {
                double phase = 2 * M_PI * (tone ? 880 : 440) * (double)i / 16000;
                real[tone] += value * cos(phase); imaginary[tone] += value * sin(phase);
            }
            i++;
        }
        CHECK(i == job->sample_count && !ferror(f)); CHECK(!fclose(f));
        CHECK(hypot(real[ch], imaginary[ch]) > 10 * hypot(real[1 - ch], imaginary[1 - ch]));
    }
}
int main(void) {
    char root[] = "/tmp/ascolto-retranscribe-XXXXXX"; CHECK(mkdtemp(root));
    char config[1024], call[1024], data[1200], audio[1200], original_md[1200], original_json[1200];
    snprintf(config, sizeof(config), "%s/config.json", root);
    snprintf(call, sizeof(call), "%s/Original's-é-call", root); CHECK(!mkdir(call, 0700));
    snprintf(data, sizeof(data), "%s/Data", call); CHECK(!mkdir(data, 0700));
    snprintf(audio, sizeof(audio), "%s/source.wav", call); wav(audio, 2, 16000);
    snprintf(original_md, sizeof(original_md), "%s/transcript.md", call);
    snprintf(original_json, sizeof(original_json), "%s/Data/transcript.jsonl", call);
    ascolto_deferred_store_t *store = NULL; CHECK(!ascolto_deferred_store_open(config, &store));
    ascolto_db_t *database = ascolto_deferred_store_database(store); sqlite3 *db = ascolto_db_sqlite(database);
    int64_t original = source(database, call, audio);
    CHECK(!ascolto_storage_set(database, "local_transcription_model", "whistle"));
    CHECK(!ascolto_storage_set(database, "transcription_language", "it"));
    CHECK(!ascolto_storage_set(database, "diarizer_threshold", ".48"));
    CHECK(!ascolto_storage_set_keywords(database, "Changed keyword"));
    unsigned char hashes[3][CC_SHA256_DIGEST_LENGTH], after[CC_SHA256_DIGEST_LENGTH];
    const char *originals[] = {audio, original_md, original_json};
    for (int i = 0; i < 3; i++) digest(originals[i], hashes[i]);
    ascolto_local_models_snapshot_t catalog = {0};
    snprintf(catalog.models[0].id, sizeof(catalog.models[0].id), "whistle");
    catalog.models[0].state = ASCOLTO_MODEL_INCLUDED;
    snprintf(catalog.models[1].id, sizeof(catalog.models[1].id), "whisper-turbo-q5");
    catalog.models[1].state = ASCOLTO_MODEL_READY;
    snprintf(catalog.models[4].id, sizeof(catalog.models[4].id), "nemotron-3.5-q8");
    catalog.models[4].state = ASCOLTO_MODEL_READY;
    char error[80]; int64_t id = 0;
    CHECK(!ascolto_retranscribe_enqueue(config, &catalog, original, "whisper-turbo-q5", "en", NULL, NULL, &id, error, sizeof(error)));
    CHECK(id > 0 && !*error); CHECK(versions(call) == 1);
    ascolto_deferred_job_t job = {0}; CHECK(!ascolto_deferred_get(store, id, &job));
    CHECK(job.session_id != original && !strcmp(job.state, "queued"));
    CHECK(!strcmp(job.model_id, "whisper-turbo-q5") && !strcmp(job.language, "en"));
    CHECK(!strcmp(job.keywords, "Original keyword\nl'été") && fabs(job.diarizer_threshold - .72) < .000001);
    int64_t child = job.session_id;
    CHECK(strstr(job.mic_path, "/Data/Local-transcription-history/version-"));
    verify_channels(&job, false);
    CHECK(scalar(db, "SELECT count(*) FROM sessions WHERE state='completed' AND started_at_ms=1000 AND ended_at_ms=2000") == 2);
    CHECK(scalar(db, "SELECT count(*) FROM sessions WHERE json_extract(metadata_json,'$.native_transcription_source_session')=1 AND json_extract(metadata_json,'$.cloud_recap')=0 AND json_extract(metadata_json,'$.recap_tts')=0 AND json_extract(metadata_json,'$.retain_audio')=0 AND json_extract(metadata_json,'$.audio_m4a')=''") == 1);
    CHECK(scalar(db, "SELECT count(*) FROM artifacts WHERE session_id=2 AND state='pending'") == 2);
    CHECK(scalar(db, "SELECT count(*) FROM utterances WHERE session_id=1 AND text='Original text'") == 1);
    char setting[80]; CHECK(!ascolto_storage_get(database, "local_transcription_model", setting, sizeof(setting)) && !strcmp(setting, "whistle"));
    CHECK(!ascolto_storage_get(database, "transcription_language", setting, sizeof(setting)) && !strcmp(setting, "it"));
    int64_t duplicate = 0;
    CHECK(ascolto_retranscribe_enqueue(config, &catalog, original, "whisper-turbo-q5", "en", NULL, NULL, &duplicate, error, sizeof(error)) == SQLITE_BUSY);
    CHECK(!duplicate && !strcmp(error, "already_queued") && versions(call) == 1);
    CHECK(ascolto_retranscribe_enqueue(config, &catalog, child, "whistle", "it", NULL, NULL, &duplicate, error, sizeof(error)) == SQLITE_MISUSE);
    CHECK(!strcmp(error, "source_unsupported"));
    CHECK(ascolto_retranscribe_enqueue(config, &catalog, original, "whistle", "unsupported", NULL, NULL, &duplicate, error, sizeof(error)) == SQLITE_MISUSE);
    CHECK(!strcmp(error, "invalid_language"));
    CHECK(ascolto_retranscribe_enqueue(config, &catalog, original, "invented", "it", NULL, NULL, &duplicate, error, sizeof(error)) == SQLITE_NOTFOUND);
    CHECK(!strcmp(error, "model_unavailable"));
    int cancel_calls = 0;
    CHECK(ascolto_retranscribe_enqueue(config, &catalog, original, "whistle", "it", cancel_decode, &cancel_calls, &duplicate, error, sizeof(error)) == SQLITE_INTERRUPT);
    CHECK(!strcmp(error, "cancelled") && versions(call) == 1 && scalar(db, "SELECT count(*) FROM sessions") == 2);
    CHECK(!sqlite3_exec(db, "CREATE TRIGGER enqueue_fault BEFORE INSERT ON native_transcription_jobs BEGIN SELECT RAISE(ABORT,'fixture_failure'); END", NULL, NULL, NULL));
    CHECK(ascolto_retranscribe_enqueue(config, &catalog, original, "whistle", "it", NULL, NULL, &duplicate, error, sizeof(error)) != SQLITE_OK);
    CHECK(versions(call) == 1 && scalar(db, "SELECT count(*) FROM sessions") == 2 && scalar(db, "SELECT count(*) FROM artifacts") == 5);
    CHECK(!sqlite3_exec(db, "DROP TRIGGER enqueue_fault", NULL, NULL, NULL));
    CHECK(!ascolto_db_session_state(database, original, "recording", 0, NULL));
    CHECK(ascolto_retranscribe_enqueue(config, &catalog, original, "whistle", "it", NULL, NULL, &duplicate, error, sizeof(error)) == SQLITE_BUSY);
    CHECK(!strcmp(error, "source_busy"));
    CHECK(!ascolto_db_session_state(database, original, "completed", 2000, NULL));
    for (int i = 0; i < 3; i++) { digest(originals[i], after); CHECK(!memcmp(hashes[i], after, sizeof(after))); }
    ascolto_deferred_job_clear(&job);
    ascolto_deferred_store_close(store); store = NULL;
    CHECK(!ascolto_deferred_store_open(config, &store)); database = ascolto_deferred_store_database(store); db = ascolto_db_sqlite(database);
    CHECK(!ascolto_deferred_get(store, id, &job)); CHECK(!strcmp(job.state, "queued") && !strcmp(job.language, "en")); ascolto_deferred_job_clear(&job);
    /* A second native input exercises sample-rate conversion without a model. */
    char converted_call[1024], converted_audio[1200];
    snprintf(converted_call, sizeof(converted_call), "%s/Resampled-call", root); CHECK(!mkdir(converted_call, 0700));
    snprintf(data, sizeof(data), "%s/Data", converted_call); CHECK(!mkdir(data, 0700));
    snprintf(converted_audio, sizeof(converted_audio), "%s/source.wav", converted_call); wav(converted_audio, 2, 48000);
    int64_t converted = source(database, converted_call, converted_audio);
    int64_t original_job = 0;
    ascolto_deferred_enqueue_t frozen = {.session_id = converted, .sample_count = 16000,
        .model_id = "whistle", .language = "it", .keywords = "Frozen keyword\nFrozen attendee",
        .mic_path = "/fixture/mic", .sys_path = "/fixture/sys", .diarizer_threshold = .65};
    CHECK(!ascolto_deferred_enqueue(store, &frozen, &original_job));
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE native_transcription_jobs SET state='completed',finished_at_ms=queued_at_ms WHERE id=%lld", (long long)original_job);
    CHECK(!sqlite3_exec(db, sql, NULL, NULL, NULL));
    CHECK(!ascolto_retranscribe_enqueue(config, &catalog, converted, "whistle", "auto", NULL, NULL, &id, error, sizeof(error)));
    CHECK(!ascolto_deferred_get(store, id, &job)); verify_channels(&job, true);
    CHECK(!strcmp(job.keywords, frozen.keywords) && fabs(job.diarizer_threshold - .65) < .000001);
    ascolto_deferred_job_clear(&job);
    snprintf(sql, sizeof(sql), "UPDATE sessions SET capture_backend='pipewire' WHERE id=%lld", (long long)converted);
    CHECK(!sqlite3_exec(db, sql, NULL, NULL, NULL));
    CHECK(ascolto_retranscribe_enqueue(config, &catalog, converted, "whistle", "it", NULL, NULL, &id, error, sizeof(error)) == SQLITE_MISUSE);
    CHECK(!strcmp(error, "source_unsupported"));
    /* Exercise the actual retained macOS codec using only synthetic tones. */
    char encoded_call[1024], encoded_audio[1200];
    snprintf(encoded_call, sizeof(encoded_call), "%s/Encoded-call", root); CHECK(!mkdir(encoded_call, 0700));
    snprintf(data, sizeof(data), "%s/Data", encoded_call); CHECK(!mkdir(data, 0700));
    snprintf(encoded_audio, sizeof(encoded_audio), "%s/source.m4a", encoded_call); m4a(encoded_audio);
    int64_t encoded = source(database, encoded_call, encoded_audio);
    digest(encoded_audio, hashes[0]);
    CHECK(!ascolto_retranscribe_enqueue(config, &catalog, encoded, "nemotron-3.5-q8", "en", NULL, NULL, &id, error, sizeof(error)));
    CHECK(!ascolto_deferred_get(store, id, &job)); verify_encoded_channels(&job); ascolto_deferred_job_clear(&job);
    digest(encoded_audio, after); CHECK(!memcmp(hashes[0], after, sizeof(after)));
    /* Legacy flat originals keep their names; history belongs to one call. */
    char legacy_fixture[1024], legacy_call[1024], legacy_audio[1200], legacy_md[1200], legacy_json[1200];
    snprintf(legacy_fixture, sizeof(legacy_fixture), "%s/Legacy-fixture", root); CHECK(!mkdir(legacy_fixture, 0700));
    snprintf(data, sizeof(data), "%s/Data", legacy_fixture); CHECK(!mkdir(data, 0700));
    snprintf(legacy_call, sizeof(legacy_call), "%s/legacy_call", root);
    snprintf(legacy_audio, sizeof(legacy_audio), "%s.wav", legacy_call); wav(legacy_audio, 2, 16000);
    snprintf(legacy_md, sizeof(legacy_md), "%s.md", legacy_call);
    snprintf(legacy_json, sizeof(legacy_json), "%s.jsonl", legacy_call);
    int64_t legacy = source(database, legacy_fixture, legacy_audio);
    char old_md[1200], old_json[1200];
    snprintf(old_md, sizeof(old_md), "%s/transcript.md", legacy_fixture);
    snprintf(old_json, sizeof(old_json), "%s/Data/transcript.jsonl", legacy_fixture);
    CHECK(!rename(old_md, legacy_md)); CHECK(!rename(old_json, legacy_json));
    sqlite3_stmt *legacy_update = NULL;
    CHECK(!sqlite3_prepare_v2(db, "UPDATE artifacts SET path=CASE kind WHEN 'transcript_md' THEN ?2 ELSE ?3 END WHERE session_id=?1 AND kind IN ('transcript_md','transcript_jsonl')", -1, &legacy_update, NULL));
    sqlite3_bind_int64(legacy_update, 1, legacy);
    sqlite3_bind_text(legacy_update, 2, legacy_md, -1, SQLITE_STATIC);
    sqlite3_bind_text(legacy_update, 3, legacy_json, -1, SQLITE_STATIC);
    CHECK(sqlite3_step(legacy_update) == SQLITE_DONE); sqlite3_finalize(legacy_update);
    CHECK(!rmdir(data)); CHECK(!rmdir(legacy_fixture));
    const char *legacy_originals[] = {legacy_audio, legacy_md, legacy_json};
    for (int i = 0; i < 3; i++) digest(legacy_originals[i], hashes[i]);
    CHECK(!ascolto_retranscribe_enqueue(config, &catalog, legacy, "whistle", "it", NULL, NULL, &id, error, sizeof(error)));
    CHECK(!ascolto_deferred_get(store, id, &job));
    char canonical_legacy[1024]; CHECK(realpath(legacy_call, canonical_legacy));
    char legacy_history[1200]; snprintf(legacy_history, sizeof(legacy_history), "%s/Data/Local-transcription-history/version-", canonical_legacy);
    CHECK(!strncmp(job.mic_path, legacy_history, strlen(legacy_history)));
    CHECK(versions(legacy_call) == 1); ascolto_deferred_job_clear(&job);
    for (int i = 0; i < 3; i++) { digest(legacy_originals[i], after); CHECK(!memcmp(hashes[i], after, sizeof(after))); }
    /* Mono archives are ambiguous; never invent channel identity. */
    char mono_call[1024], mono_audio[1200]; snprintf(mono_call, sizeof(mono_call), "%s/Mono-call", root); CHECK(!mkdir(mono_call, 0700));
    snprintf(data, sizeof(data), "%s/Data", mono_call); CHECK(!mkdir(data, 0700));
    snprintf(mono_audio, sizeof(mono_audio), "%s/source.wav", mono_call); wav(mono_audio, 1, 16000);
    int64_t mono = source(database, mono_call, mono_audio);
    CHECK(ascolto_retranscribe_enqueue(config, &catalog, mono, "whistle", "it", NULL, NULL, &id, error, sizeof(error)) == SQLITE_FORMAT);
    CHECK(!strcmp(error, "source_audio_not_stereo") && versions(mono_call) == 0);
    CHECK(!unlink(mono_audio));
    CHECK(ascolto_retranscribe_enqueue(config, &catalog, mono, "whistle", "it", NULL, NULL, &id, error, sizeof(error)) != SQLITE_OK);
    CHECK(!strcmp(error, "source_audio_missing") && versions(mono_call) == 0);
    CHECK(scalar(db, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
    ascolto_deferred_store_close(store);
    printf("Historical retranscription: %d checks passed\n", checks);
    return 0;
}
