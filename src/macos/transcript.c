#include "ascolto/filesystem.h"
#include "transcript.h"
#include "ascolto/json.h"
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void format_mm_ss(double seconds, char* out, size_t cap) {
    if (seconds < 0.0) seconds = 0.0;
    int total = (int)seconds;
    int mm = total / 60;
    int ss = total % 60;
    snprintf(out, cap, "%02d:%02d", mm, ss);
}

void ascolto_transcript_open(ascolto_live_app_t *app, bool is_manual) {
    ascolto_mkdir_p(app->config.output_dir);

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm_now);
    struct timeval wall_time;
    gettimeofday(&wall_time, NULL);
    size_t stamp_length = strlen(stamp);
    snprintf(stamp + stamp_length, sizeof(stamp) - stamp_length, "-%06u", (unsigned)wall_time.tv_usec);

    snprintf(app->files.md_path, sizeof(app->files.md_path),
             "%s/%s_call.md", app->config.output_dir, stamp);
    snprintf(app->files.jsonl_path, sizeof(app->files.jsonl_path),
             "%s/%s_call.jsonl", app->config.output_dir, stamp);
    snprintf(app->files.mic_raw_path, sizeof(app->files.mic_raw_path),
             "%s/.%s_mic.f32", app->config.output_dir, stamp);
    snprintf(app->files.sys_raw_path, sizeof(app->files.sys_raw_path),
             "%s/.%s_sys.f32", app->config.output_dir, stamp);
    snprintf(app->files.m4a_path, sizeof(app->files.m4a_path),
             "%s/%s_call.m4a", app->config.output_dir, stamp);

    app->files.md = fopen(app->files.md_path, "w");
    app->files.jsonl = fopen(app->files.jsonl_path, "w");
    app->files.save_audio = app->config.save_audio || app->config.cloud_recap;
    if (app->files.save_audio) {
        app->files.mic_raw = fopen(app->files.mic_raw_path, "wb");
        app->files.sys_raw = fopen(app->files.sys_raw_path, "wb");
    }

    /* Use the metadata cached by the monitor. AX/EventKit queries can block and must
       never run while holding the session lock used by the menu and audio worker. */

    if (app->files.md) {
        char human_date[64];
        strftime(human_date, sizeof(human_date), "%Y-%m-%d %H:%M:%S", &tm_now);
        fprintf(app->files.md, "# Trascrizione Call — %s\n\n", human_date);
        if (app->call_meta.window_title[0]) {
            fprintf(app->files.md, "- **Titolo Finestra Call**: %s\n", app->call_meta.window_title);
        }
        if (app->call_meta.calendar_title[0]) {
            fprintf(app->files.md, "- **Riunione a Calendario**: %s\n", app->call_meta.calendar_title);
        }
        if (app->call_meta.calendar_organizer[0]) {
            fprintf(app->files.md, "- **Organizzatore**: %s\n", app->call_meta.calendar_organizer);
        }
        if (app->call_meta.calendar_attendees[0]) {
            fprintf(app->files.md, "- **Partecipanti invitati**: %s\n", app->call_meta.calendar_attendees);
        }
        if (app->call_meta.calendar_notes[0]) {
            fprintf(app->files.md, "- **Descrizione / Agenda**: %s\n", app->call_meta.calendar_notes);
        }
        fprintf(app->files.md, "- **Microfono (`ME`)**: %s\n", app->active_mic_name);
        fprintf(app->files.md, "- **App Input (`MIC`)**: %s\n", app->active_in_apps[0] ? app->active_in_apps : "Manuale");
        fprintf(app->files.md, "- **App Output (`SYS`)**: %s\n", app->active_out_apps[0] ? app->active_out_apps : "Sistema");
        fprintf(app->files.md, "- **Lingua impostata**: `%s`\n\n---\n\n", app->config.language);
        fflush(app->files.md);
        app->files.meta_written = (app->call_meta.window_title[0] != '\0' || app->call_meta.calendar_title[0] != '\0');
    }
    if (app->files.jsonl) {
        fprintf(app->files.jsonl,
                "{\"event\":\"session_start\",\"manual\":%s,\"mic\":\"%s\",\"in_app\":\"%s\",\"out_app\":\"%s\","
                "\"window_title\":\"%s\",\"calendar_title\":\"%s\",\"attendees\":\"%s\"}\n",
                is_manual ? "true" : "false",
                app->active_mic_name,
                app->active_in_apps,
                app->active_out_apps,
                app->call_meta.window_title,
                app->call_meta.calendar_title,
                app->call_meta.calendar_attendees);
        fflush(app->files.jsonl);
    }

}

void ascolto_transcript_write_turn(ascolto_live_app_t *app, ascolto_channel_t channel,
    double start_s, double end_s, float rms_db, const ascolto_voiceprint_t *vp,
    const ascolto_transcript_result_t *tr, const char *speaker, float sim, bool is_new) {
    char t_start[16], t_end[16];
    format_mm_ss(start_s, t_start, sizeof(t_start));
    format_mm_ss(end_s, t_end, sizeof(t_end));

    const char* src_app = (channel == ASCOLTO_CHANNEL_MIC)
        ? (app->active_in_apps[0] ? app->active_in_apps : app->active_mic_name)
        : (app->active_out_apps[0] ? app->active_out_apps : "Sistema");

    if (!app->files.meta_written &&
        (app->call_meta.window_title[0] != '\0' || app->call_meta.calendar_title[0] != '\0')) {
        if (app->files.md) {
            if (app->call_meta.window_title[0]) {
                fprintf(app->files.md, "> 📋 **Titolo Finestra Call**: %s\n", app->call_meta.window_title);
            }
            if (app->call_meta.calendar_title[0]) {
                fprintf(app->files.md, "> 📅 **Riunione a Calendario**: %s\n", app->call_meta.calendar_title);
            }
            if (app->call_meta.calendar_attendees[0]) {
                fprintf(app->files.md, "> 👥 **Partecipanti**: %s\n", app->call_meta.calendar_attendees);
            }
            fprintf(app->files.md, "\n");
            fflush(app->files.md);
        }
        if (app->files.jsonl) {
            fprintf(app->files.jsonl,
                    "{\"event\":\"call_metadata\",\"window_title\":\"%s\",\"calendar_title\":\"%s\",\"attendees\":\"%s\"}\n",
                    app->call_meta.window_title,
                    app->call_meta.calendar_title,
                    app->call_meta.calendar_attendees);
            fflush(app->files.jsonl);
        }
        app->files.meta_written = true;
    }

    if (app->files.md) {
        fprintf(app->files.md, "- **[%s - %s] %s** *(canale %s · app: %s, lingua: %s)*: %s\n",
                t_start, t_end, speaker, ascolto_channel_name(channel), src_app, tr->language, tr->text);
        fflush(app->files.md);
    }

    if (app->files.jsonl) {
        fprintf(app->files.jsonl,
                "{\"event\":\"utterance\",\"channel\":\"%s\",\"source_app\":\"%s\",\"speaker\":\"%s\","
                "\"is_new_speaker\":%s,\"similarity\":%.3f,\"start_s\":%.2f,\"end_s\":%.2f,"
                "\"f0_hz\":%.1f,\"rms_db\":%.1f,\"language\":\"%s\",\"ttft_ms\":%.1f,\"text\":",
                ascolto_channel_name(channel),
                src_app,
                speaker,
                is_new ? "true" : "false",
                sim,
                start_s,
                end_s,
                vp->f0_mean_hz,
                rms_db,
                tr->language,
                tr->ttft_ms);
        ascolto_write_json_string(app->files.jsonl, tr->text);
        fprintf(app->files.jsonl, "}\n");
        fflush(app->files.jsonl);
    }
}

void ascolto_transcript_close(ascolto_live_app_t *app, double elapsed) {
    if (app->files.md) {
        char dur[16];
        format_mm_ss(elapsed, dur, sizeof(dur));
        fprintf(app->files.md, "\n---\n*Fine chiamata (durata: %s, frasi: %d, interlocutori remoti: %d)*\n",
                dur, app->session_utterance_count, app->diarizer.num_speakers);
        fclose(app->files.md);
        app->files.md = NULL;
    }
    if (app->files.jsonl) {
        fprintf(app->files.jsonl,
                "{\"event\":\"session_end\",\"duration_s\":%.2f,\"utterances\":%d,\"remote_speakers\":%d,\"audio_m4a\":\"%s\",\"bleed_frames\":%d,\"echo_turns\":%d,\"capture_dropped_samples\":%lld,\"padded_mic_samples\":%lld,\"padded_sys_samples\":%lld}\n",
                elapsed, app->session_utterance_count, app->diarizer.num_speakers,
                app->files.save_audio ? app->files.m4a_path : "",
                app->mic_bleed_frames_suppressed, app->mic_echo_turns_suppressed,
                (long long)app->capture_dropped_samples,
                (long long)app->capture_padded_samples[0], (long long)app->capture_padded_samples[1]);
        fclose(app->files.jsonl);
        app->files.jsonl = NULL;
    }

    if (app->files.mic_raw) {
        fclose(app->files.mic_raw);
        app->files.mic_raw = NULL;
    }
    if (app->files.sys_raw) {
        fclose(app->files.sys_raw);
        app->files.sys_raw = NULL;
    }

    /* If a brief (< 3.0s) transient session produced 0 utterances, remove the empty stub files */
    if (app->session_utterance_count == 0 && elapsed < 3.0) {
        if (app->files.md_path[0]) unlink(app->files.md_path);
        if (app->files.jsonl_path[0]) unlink(app->files.jsonl_path);
        if (app->files.mic_raw_path[0]) unlink(app->files.mic_raw_path);
        if (app->files.sys_raw_path[0]) unlink(app->files.sys_raw_path);
        return;
    }

    if (app->files.save_audio && app->files.mic_raw_path[0] && app->files.sys_raw_path[0]) {
        ascolto_archive_finalize(&app->config, app->files.mic_raw_path, app->files.sys_raw_path,
                                app->files.m4a_path, app->files.md_path);
    }

}
