#ifndef ASCOLTO_TRAY_UI_H
#define ASCOLTO_TRAY_UI_H

#include "ascolto/types.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ascolto_core ascolto_core_t;

typedef struct {
    bool active;
    bool using_gui_indicator;
    char icon_name[64];
    char label[64];
    char status_text[128];
    bool menu_recording_enabled;
    bool menu_stopping;
    int storage_error;
    char session_app[128];
    char latest_phrase[256];
    bool menu_paused;
    bool menu_auto_mic_checked;
    bool menu_save_audio_checked;
    bool menu_cloud_recap_checked;
    bool menu_recap_tts_checked;
    char selected_language[16];
    char selected_mic[128];
    char selected_sys[128];
    ascolto_sensitivity_t selected_sensitivity;
    char keywords_summary[256];
    char settings_keywords[1024];
    char archive_directory[512];
    char session_directory[512];
    size_t recent_files_count;
    char recent_files[ASCOLTO_MAX_RECENT_FILES][512];
} ascolto_tray_snapshot_t;

typedef struct {
    ascolto_core_t *core;
    bool headless;
    bool running;
    volatile bool dirty;
    void *gtk_impl;
    pthread_mutex_t mutex;
    ascolto_tray_snapshot_t snapshot;
} ascolto_tray_ui_t;

int ascolto_tray_init(ascolto_tray_ui_t *ui, ascolto_core_t *core, bool headless);
void ascolto_tray_update(ascolto_tray_ui_t *ui);
void ascolto_tray_get_snapshot(ascolto_tray_ui_t *ui, ascolto_tray_snapshot_t *out_snap);
void ascolto_tray_step_iteration(ascolto_tray_ui_t *ui);
void ascolto_tray_open_transcripts_folder(ascolto_tray_ui_t *ui);
void ascolto_tray_open_session_folder(ascolto_tray_ui_t *ui);
void ascolto_tray_destroy(ascolto_tray_ui_t *ui);
/* Join menu mutations before shutting down capture and session resources. */
void ascolto_tray_wait_actions(ascolto_tray_ui_t *ui);

#ifdef __cplusplus
}
#endif

#endif /* ASCOLTO_TRAY_UI_H */
