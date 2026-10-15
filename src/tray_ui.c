#define _POSIX_C_SOURCE 200809L
#include "ascolto/filesystem.h"
#include "ascolto/tray_ui.h"
#include "ascolto/core.h"
#include <dlfcn.h>
#include <errno.h>
#include <gio/gio.h>
#include <glib-object.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef gboolean (*fn_gtk_init_check_t)(int *argc, char ***argv);
typedef gboolean (*fn_gtk_events_pending_t)(void);
typedef gboolean (*fn_gtk_main_iteration_do_t)(gboolean blocking);
typedef void *(*fn_gtk_menu_new_t)(void);
typedef void *(*fn_gtk_menu_item_new_with_label_t)(const char *label);
typedef void (*fn_gtk_menu_item_set_label_t)(void *item, const char *label);
typedef void (*fn_gtk_menu_item_set_submenu_t)(void *item, void *submenu);
typedef void *(*fn_gtk_check_menu_item_new_with_label_t)(const char *label);
typedef void (*fn_gtk_check_menu_item_set_active_t)(void *item, gboolean is_active);
typedef gboolean (*fn_gtk_check_menu_item_get_active_t)(void *item);
typedef void *(*fn_gtk_separator_menu_item_new_t)(void);
typedef void (*fn_gtk_menu_shell_append_t)(void *menu_shell, void *child);
typedef void (*fn_gtk_widget_show_all_t)(void *widget);
typedef void (*fn_gtk_widget_set_sensitive_t)(void *widget, gboolean sensitive);
typedef void (*fn_gtk_widget_destroy_t)(void *widget);

typedef void *(*fn_app_indicator_new_t)(const char *id, const char *icon_name, int category);
typedef void (*fn_app_indicator_set_status_t)(void *self, int status);
typedef void (*fn_app_indicator_set_icon_full_t)(void *self, const char *icon_name, const char *icon_desc);
typedef void (*fn_app_indicator_set_label_t)(void *self, const char *label, const char *guide);
typedef void (*fn_app_indicator_set_title_t)(void *self, const char *title);
typedef void (*fn_app_indicator_set_menu_t)(void *self, void *menu);

typedef struct {
    void *gtk_lib;
    void *ind_lib;
    fn_gtk_init_check_t gtk_init_check;
    fn_gtk_events_pending_t gtk_events_pending;
    fn_gtk_main_iteration_do_t gtk_main_iteration_do;
    fn_gtk_menu_new_t gtk_menu_new;
    fn_gtk_menu_item_new_with_label_t gtk_menu_item_new_with_label;
    fn_gtk_menu_item_set_label_t gtk_menu_item_set_label;
    fn_gtk_menu_item_set_submenu_t gtk_menu_item_set_submenu;
    fn_gtk_check_menu_item_new_with_label_t gtk_check_menu_item_new_with_label;
    fn_gtk_check_menu_item_set_active_t gtk_check_menu_item_set_active;
    fn_gtk_check_menu_item_get_active_t gtk_check_menu_item_get_active;
    fn_gtk_separator_menu_item_new_t gtk_separator_menu_item_new;
    fn_gtk_menu_shell_append_t gtk_menu_shell_append;
    fn_gtk_widget_show_all_t gtk_widget_show_all;
    fn_gtk_widget_set_sensitive_t gtk_widget_set_sensitive;
    fn_gtk_widget_destroy_t gtk_widget_destroy;

    fn_app_indicator_new_t app_indicator_new;
    fn_app_indicator_set_status_t app_indicator_set_status;
    fn_app_indicator_set_icon_full_t app_indicator_set_icon_full;
    fn_app_indicator_set_label_t app_indicator_set_label;
    fn_app_indicator_set_title_t app_indicator_set_title;
    fn_app_indicator_set_menu_t app_indicator_set_menu;

    void *indicator;
    void *menu;
    void *item_status;
    void *item_start_stop;
    void *item_pause_resume;
    void *item_auto_mic;
    void *item_save_audio;
    void *item_cloud_recap;
    void *item_recap_tts;
    void *item_recap_now;
    void *item_keywords;
    void *item_make_mp3;

    bool has_applied;
    ascolto_tray_snapshot_t applied;
    bool updating_widgets;
    volatile bool keywords_dialog_open;
} gtk_tray_priv_t;

static void launch_path_nonblocking(const char *path, bool is_dir) {
    if (!path || !*path) return;
    if (is_dir) {
        ascolto_mkdir_p(path);
    }
    gchar *uri = g_filename_to_uri(path, NULL, NULL);
    if (!uri) return;

    if (is_dir) {
        /* Use standard D-Bus org.freedesktop.FileManager1.ShowFolders so GNOME raises Nautilus on Wayland */
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        if (bus) {
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
            g_variant_builder_add(&builder, "s", uri);
            g_dbus_connection_call(
                bus,
                "org.freedesktop.FileManager1",
                "/org/freedesktop/FileManager1",
                "org.freedesktop.FileManager1",
                "ShowFolders",
                g_variant_new("(ass)", &builder, ""),
                NULL,
                G_DBUS_CALL_FLAGS_NONE,
                3000,
                NULL,
                NULL,
                NULL
            );
            g_object_unref(bus);
            g_free(uri);
            return;
        }
    }

    g_app_info_launch_default_for_uri(uri, NULL, NULL);
    g_free(uri);
}

void ascolto_tray_open_transcripts_folder(ascolto_tray_ui_t *ui) {
    if (!ui || !ui->core) return;
    launch_path_nonblocking(ui->core->config.output_dir, true);
}

void ascolto_tray_open_session_folder(ascolto_tray_ui_t *ui) {
    if (!ui || !ui->core) return;
    const char *dir = ui->core->current_session_dir[0] ? ui->core->current_session_dir : ui->core->config.output_dir;
    launch_path_nonblocking(dir, true);
}

static void apply_snapshot_to_gtk_main_thread(ascolto_tray_ui_t *ui);

static void on_menu_start_stop(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core) return;
    if (ui->core->state == ASCOLTO_STATE_IDLE) {
        ascolto_core_start_manual(ui->core);
    } else {
        ascolto_core_stop(ui->core);
    }
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_pause_resume(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core) return;
    if (ui->core->state == ASCOLTO_STATE_PAUSED) {
        ascolto_core_resume(ui->core);
    } else if (ui->core->state == ASCOLTO_STATE_RECORDING_AUTO ||
               ui->core->state == ASCOLTO_STATE_RECORDING_MANUAL) {
        ascolto_core_pause(ui->core);
    }
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_auto_mic_toggled(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->updating_widgets) return;
    gboolean active = p->gtk_check_menu_item_get_active(widget);
    ascolto_core_set_auto_mic(ui->core, active ? true : false);
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_save_audio_toggled(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->updating_widgets) return;
    gboolean active = p->gtk_check_menu_item_get_active(widget);
    ascolto_core_set_save_audio(ui->core, active ? true : false);
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_cloud_recap_toggled(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->updating_widgets) return;
    ascolto_core_set_cloud_recap(ui->core, p->gtk_check_menu_item_get_active(widget) ? true : false);
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_recap_tts_toggled(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->updating_widgets) return;
    ascolto_core_set_recap_tts(ui->core, p->gtk_check_menu_item_get_active(widget) ? true : false);
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_recap_now(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core) return;
    ascolto_core_run_recap(ui->core, NULL);
}

static void on_menu_make_mp3(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core) return;
    ascolto_core_make_mp3(ui->core, NULL);
}

static void on_menu_open_session_folder(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (ui) ascolto_tray_open_session_folder(ui);
}

static void *keywords_dialog_worker(void *arg) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)arg;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;

    char current_kw[1024] = {0};
    pthread_mutex_lock(&ui->core->mutex);
    snprintf(current_kw, sizeof(current_kw), "%s", ui->core->config.keywords);
    pthread_mutex_unlock(&ui->core->mutex);

    gchar *argv[] = {
        (gchar *)"zenity",
        (gchar *)"--entry",
        (gchar *)"--title=Ascolto - Keywords personalizzate",
        (gchar *)"--text=Inserisci parole chiave, nomi propri o acronimi (separati da virgola):",
        (gchar *)"--entry-text",
        (gchar *)current_kw,
        (gchar *)"--width=460",
        NULL
    };
    gchar *std_out = NULL;
    gint exit_status = 0;
    if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, &std_out, NULL, &exit_status, NULL) && exit_status == 0 && std_out) {
        size_t len = strlen(std_out);
        while (len > 0 && (std_out[len - 1] == '\n' || std_out[len - 1] == '\r')) {
            std_out[--len] = '\0';
        }
        ascolto_core_set_keywords(ui->core, std_out);
    }
    if (std_out) g_free(std_out);
    p->keywords_dialog_open = false;
    return NULL;
}

static void on_menu_keywords_dialog(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->keywords_dialog_open) return;
    p->keywords_dialog_open = true;
    pthread_t th;
    if (pthread_create(&th, NULL, keywords_dialog_worker, ui) == 0) {
        pthread_detach(th);
    } else {
        p->keywords_dialog_open = false;
    }
}

static void on_menu_open_folder(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core) return;
    launch_path_nonblocking(ui->core->config.output_dir, true);
}

static void on_menu_quit(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (ui && ui->core) {
        ui->core->running = false;
    }
}

static bool init_gtk_indicator(ascolto_tray_ui_t *ui) {
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)calloc(1, sizeof(gtk_tray_priv_t));
    if (!p) return false;

    p->gtk_lib = dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_GLOBAL);
    p->ind_lib = dlopen("libayatana-appindicator3.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!p->gtk_lib || !p->ind_lib) {
        if (p->gtk_lib) dlclose(p->gtk_lib);
        if (p->ind_lib) dlclose(p->ind_lib);
        free(p);
        return false;
    }

    p->gtk_init_check = (fn_gtk_init_check_t)dlsym(p->gtk_lib, "gtk_init_check");
    p->gtk_events_pending = (fn_gtk_events_pending_t)dlsym(p->gtk_lib, "gtk_events_pending");
    p->gtk_main_iteration_do = (fn_gtk_main_iteration_do_t)dlsym(p->gtk_lib, "gtk_main_iteration_do");
    p->gtk_menu_new = (fn_gtk_menu_new_t)dlsym(p->gtk_lib, "gtk_menu_new");
    p->gtk_menu_item_new_with_label = (fn_gtk_menu_item_new_with_label_t)dlsym(p->gtk_lib, "gtk_menu_item_new_with_label");
    p->gtk_menu_item_set_label = (fn_gtk_menu_item_set_label_t)dlsym(p->gtk_lib, "gtk_menu_item_set_label");
    p->gtk_menu_item_set_submenu = (fn_gtk_menu_item_set_submenu_t)dlsym(p->gtk_lib, "gtk_menu_item_set_submenu");
    p->gtk_check_menu_item_new_with_label = (fn_gtk_check_menu_item_new_with_label_t)dlsym(p->gtk_lib, "gtk_check_menu_item_new_with_label");
    p->gtk_check_menu_item_set_active = (fn_gtk_check_menu_item_set_active_t)dlsym(p->gtk_lib, "gtk_check_menu_item_set_active");
    p->gtk_check_menu_item_get_active = (fn_gtk_check_menu_item_get_active_t)dlsym(p->gtk_lib, "gtk_check_menu_item_get_active");
    p->gtk_separator_menu_item_new = (fn_gtk_separator_menu_item_new_t)dlsym(p->gtk_lib, "gtk_separator_menu_item_new");
    p->gtk_menu_shell_append = (fn_gtk_menu_shell_append_t)dlsym(p->gtk_lib, "gtk_menu_shell_append");
    p->gtk_widget_show_all = (fn_gtk_widget_show_all_t)dlsym(p->gtk_lib, "gtk_widget_show_all");
    p->gtk_widget_set_sensitive = (fn_gtk_widget_set_sensitive_t)dlsym(p->gtk_lib, "gtk_widget_set_sensitive");
    p->gtk_widget_destroy = (fn_gtk_widget_destroy_t)dlsym(p->gtk_lib, "gtk_widget_destroy");

    p->app_indicator_new = (fn_app_indicator_new_t)dlsym(p->ind_lib, "app_indicator_new");
    p->app_indicator_set_status = (fn_app_indicator_set_status_t)dlsym(p->ind_lib, "app_indicator_set_status");
    p->app_indicator_set_icon_full = (fn_app_indicator_set_icon_full_t)dlsym(p->ind_lib, "app_indicator_set_icon_full");
    p->app_indicator_set_label = (fn_app_indicator_set_label_t)dlsym(p->ind_lib, "app_indicator_set_label");
    p->app_indicator_set_title = (fn_app_indicator_set_title_t)dlsym(p->ind_lib, "app_indicator_set_title");
    p->app_indicator_set_menu = (fn_app_indicator_set_menu_t)dlsym(p->ind_lib, "app_indicator_set_menu");

    if (!p->gtk_init_check || !p->app_indicator_new || !p->gtk_init_check(NULL, NULL)) {
        dlclose(p->ind_lib);
        dlclose(p->gtk_lib);
        free(p);
        return false;
    }

    p->indicator = p->app_indicator_new("ascolto-indicator", "audio-input-microphone-symbolic", 0);
    if (!p->indicator) {
        dlclose(p->ind_lib);
        dlclose(p->gtk_lib);
        free(p);
        return false;
    }
    p->app_indicator_set_status(p->indicator, 1);
    if (p->app_indicator_set_title) {
        p->app_indicator_set_title(p->indicator, "Ascolto Call Transcriber");
    }

    p->menu = p->gtk_menu_new();

    p->item_status = p->gtk_menu_item_new_with_label("Stato: In attesa del microfono");
    p->gtk_widget_set_sensitive(p->item_status, FALSE);
    p->gtk_menu_shell_append(p->menu, p->item_status);

    p->gtk_menu_shell_append(p->menu, p->gtk_separator_menu_item_new());

    p->item_start_stop = p->gtk_menu_item_new_with_label("Avvia trascrizione manuale");
    g_signal_connect_data(p->item_start_stop, "activate", G_CALLBACK(on_menu_start_stop), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_start_stop);

    p->item_pause_resume = p->gtk_menu_item_new_with_label("Metti in pausa");
    p->gtk_widget_set_sensitive(p->item_pause_resume, FALSE);
    g_signal_connect_data(p->item_pause_resume, "activate", G_CALLBACK(on_menu_pause_resume), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_pause_resume);

    p->item_auto_mic = p->gtk_check_menu_item_new_with_label("Attivazione automatica col microfono");
    p->gtk_check_menu_item_set_active(p->item_auto_mic, ui->core->config.auto_mic_trigger ? TRUE : FALSE);
    g_signal_connect_data(p->item_auto_mic, "toggled", G_CALLBACK(on_menu_auto_mic_toggled), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_auto_mic);

    p->item_save_audio = p->gtk_check_menu_item_new_with_label("Salva audio FLAC (dalla prossima sessione)");
    p->gtk_check_menu_item_set_active(p->item_save_audio, ui->core->config.save_audio ? TRUE : FALSE);
    g_signal_connect_data(p->item_save_audio, "toggled", G_CALLBACK(on_menu_save_audio_toggled), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_save_audio);

    p->item_cloud_recap = p->gtk_check_menu_item_new_with_label("Recap a fine call (trascrizione cloud + Claude)");
    p->gtk_check_menu_item_set_active(p->item_cloud_recap, ui->core->config.cloud_recap ? TRUE : FALSE);
    g_signal_connect_data(p->item_cloud_recap, "toggled", G_CALLBACK(on_menu_cloud_recap_toggled), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_cloud_recap);

    p->item_recap_tts = p->gtk_check_menu_item_new_with_label("Aggiungi MP3 vocale del recap (TTS)");
    p->gtk_check_menu_item_set_active(p->item_recap_tts, ui->core->config.recap_tts ? TRUE : FALSE);
    g_signal_connect_data(p->item_recap_tts, "toggled", G_CALLBACK(on_menu_recap_tts_toggled), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_recap_tts);

    p->item_recap_now = p->gtk_menu_item_new_with_label("Genera recap dell'ultima sessione");
    g_signal_connect_data(p->item_recap_now, "activate", G_CALLBACK(on_menu_recap_now), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_recap_now);

    p->item_make_mp3 = p->gtk_menu_item_new_with_label("Genera MP3 dell'ultimo recap");
    g_signal_connect_data(p->item_make_mp3, "activate", G_CALLBACK(on_menu_make_mp3), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_make_mp3);

    p->gtk_menu_shell_append(p->menu, p->gtk_separator_menu_item_new());

    /* Keywords dialog item */
    p->item_keywords = p->gtk_menu_item_new_with_label("Keywords personalizzate...");
    g_signal_connect_data(p->item_keywords, "activate", G_CALLBACK(on_menu_keywords_dialog), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_keywords);

    p->gtk_menu_shell_append(p->menu, p->gtk_separator_menu_item_new());

    /* Open transcripts folder */
    void *item_open = p->gtk_menu_item_new_with_label("Apri cartella trascrizioni");
    g_signal_connect_data(item_open, "activate", G_CALLBACK(on_menu_open_folder), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, item_open);

    void *item_open_session = p->gtk_menu_item_new_with_label("Apri cartella dell'ultima sessione");
    g_signal_connect_data(item_open_session, "activate", G_CALLBACK(on_menu_open_session_folder), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, item_open_session);

    p->gtk_menu_shell_append(p->menu, p->gtk_separator_menu_item_new());

    void *item_quit = p->gtk_menu_item_new_with_label("Esci");
    g_signal_connect_data(item_quit, "activate", G_CALLBACK(on_menu_quit), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, item_quit);

    p->gtk_widget_show_all(p->menu);
    p->app_indicator_set_menu(p->indicator, p->menu);

    ui->gtk_impl = p;
    return true;
}

static void apply_snapshot_to_gtk_main_thread(ascolto_tray_ui_t *ui) {
    if (!ui || !ui->gtk_impl) return;
    pthread_mutex_lock(&ui->mutex);
    if (!ui->dirty) {
        pthread_mutex_unlock(&ui->mutex);
        return;
    }
    ascolto_tray_snapshot_t snap = ui->snapshot;
    ui->dirty = false;
    pthread_mutex_unlock(&ui->mutex);

    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    bool first = !p->has_applied;
    p->updating_widgets = true;

    if (first || strcmp(snap.icon_name, p->applied.icon_name) != 0) {
        p->app_indicator_set_icon_full(p->indicator, snap.icon_name, snap.status_text);
    }
    if (first || strcmp(snap.label, p->applied.label) != 0) {
        p->app_indicator_set_label(p->indicator, snap.label, "REC 99:99");
    }
    if (first || strcmp(snap.status_text, p->applied.status_text) != 0) {
        p->gtk_menu_item_set_label(p->item_status, snap.status_text);
    }
    if (first || snap.menu_recording_enabled != p->applied.menu_recording_enabled) {
        p->gtk_menu_item_set_label(p->item_start_stop,
                                   (!snap.menu_recording_enabled)
                                       ? "Avvia trascrizione manuale"
                                       : "Ferma trascrizione");
        p->gtk_widget_set_sensitive(p->item_pause_resume,
                                    snap.menu_recording_enabled ? TRUE : FALSE);
    }
    if (first || snap.menu_paused != p->applied.menu_paused ||
        snap.menu_recording_enabled != p->applied.menu_recording_enabled) {
        p->gtk_menu_item_set_label(p->item_pause_resume,
                                   snap.menu_paused
                                       ? "Riprendi trascrizione"
                                       : "Metti in pausa");
    }
    if (first || snap.menu_auto_mic_checked != p->applied.menu_auto_mic_checked) {
        p->gtk_check_menu_item_set_active(p->item_auto_mic,
                                          snap.menu_auto_mic_checked ? TRUE : FALSE);
    }
    if (first || snap.menu_save_audio_checked != p->applied.menu_save_audio_checked) {
        p->gtk_check_menu_item_set_active(p->item_save_audio,
                                          snap.menu_save_audio_checked ? TRUE : FALSE);
    }
    if (first || snap.menu_cloud_recap_checked != p->applied.menu_cloud_recap_checked) {
        p->gtk_check_menu_item_set_active(p->item_cloud_recap,
                                          snap.menu_cloud_recap_checked ? TRUE : FALSE);
    }
    if (first || snap.menu_recap_tts_checked != p->applied.menu_recap_tts_checked) {
        p->gtk_check_menu_item_set_active(p->item_recap_tts,
                                          snap.menu_recap_tts_checked ? TRUE : FALSE);
    }
    if (first || strcmp(snap.keywords_summary, p->applied.keywords_summary) != 0) {
        char kw_lbl[320];
        snprintf(kw_lbl, sizeof(kw_lbl), "Keywords personalizzate (%s)...", snap.keywords_summary);
        p->gtk_menu_item_set_label(p->item_keywords, kw_lbl);
    }

    p->applied = snap;
    p->has_applied = true;
    p->updating_widgets = false;

    int guard = 0;
    while (p->gtk_events_pending && p->gtk_events_pending() && guard++ < 64) {
        p->gtk_main_iteration_do(FALSE);
    }
}

int ascolto_tray_init(ascolto_tray_ui_t *ui, ascolto_core_t *core, bool headless) {
    if (!ui || !core) return -1;
    memset(ui, 0, sizeof(*ui));
    ui->core = core;
    ui->headless = headless;
    ui->running = true;
    pthread_mutex_init(&ui->mutex, NULL);

    bool gui_ok = false;
    if (!headless && (getenv("DISPLAY") || getenv("WAYLAND_DISPLAY"))) {
        gui_ok = init_gtk_indicator(ui);
    }
    ui->snapshot.active = true;
    ui->snapshot.using_gui_indicator = gui_ok;
    ascolto_tray_update(ui);
    apply_snapshot_to_gtk_main_thread(ui);
    return 0;
}

void ascolto_tray_update(ascolto_tray_ui_t *ui) {
    if (!ui || !ui->core) return;
    ascolto_core_t *core = ui->core;

    uint64_t elapsed = ascolto_core_elapsed_sec(core);
    unsigned int mins = (unsigned int)(elapsed / 60);
    unsigned int secs = (unsigned int)(elapsed % 60);

    pthread_mutex_lock(&ui->mutex);
    ascolto_tray_snapshot_t *s = &ui->snapshot;
    s->active = true;
    s->menu_auto_mic_checked = core->config.auto_mic_trigger;
    s->menu_save_audio_checked = core->config.save_audio;
    s->menu_cloud_recap_checked = core->config.cloud_recap;
    s->menu_recap_tts_checked = core->config.recap_tts;
    snprintf(s->selected_language, sizeof(s->selected_language), "%s", core->config.language);
    snprintf(s->selected_mic, sizeof(s->selected_mic), "%s", core->config.mic_device);
    snprintf(s->selected_sys, sizeof(s->selected_sys), "%s", core->config.sys_device);
    s->selected_sensitivity = core->config.diarization_sensitivity;
    snprintf(s->keywords_summary, sizeof(s->keywords_summary), "%.255s",
             core->config.keywords[0] ? core->config.keywords : "(nessuna)");

    s->recent_files_count = core->recent_files_count;
    for (size_t i = 0; i < core->recent_files_count && i < ASCOLTO_MAX_RECENT_FILES; i++) {
        snprintf(s->recent_files[i], sizeof(s->recent_files[i]), "%s", core->recent_files[i]);
    }

    if (core->state == ASCOLTO_STATE_IDLE) {
        snprintf(s->icon_name, sizeof(s->icon_name), "microphone-sensitivity-muted-symbolic");
        snprintf(s->label, sizeof(s->label), "Ascolto");
        snprintf(s->status_text, sizeof(s->status_text), "Stato: In attesa del microfono");
        s->menu_recording_enabled = false;
        s->menu_paused = false;
    } else if (core->state == ASCOLTO_STATE_PAUSED) {
        snprintf(s->icon_name, sizeof(s->icon_name), "media-playback-pause-symbolic");
        snprintf(s->label, sizeof(s->label), "PAUSA %02u:%02u", mins, secs);
        snprintf(s->status_text, sizeof(s->status_text), "Stato: In pausa (%02u:%02u)", mins, secs);
        s->menu_recording_enabled = true;
        s->menu_paused = true;
    } else {
        snprintf(s->icon_name, sizeof(s->icon_name), "audio-input-microphone-symbolic");
        snprintf(s->label, sizeof(s->label), "REC %02u:%02u", mins, secs);
        snprintf(s->status_text, sizeof(s->status_text),
                 "Stato: Trascrizione attiva (%02u:%02u) [%s]",
                 mins, secs,
                 core->state == ASCOLTO_STATE_RECORDING_AUTO ? "Auto-Mic" : "Manuale");
        s->menu_recording_enabled = true;
        s->menu_paused = false;
    }

    ui->dirty = true;
    pthread_mutex_unlock(&ui->mutex);
}

void ascolto_tray_get_snapshot(ascolto_tray_ui_t *ui, ascolto_tray_snapshot_t *out_snap) {
    if (!ui || !out_snap) return;
    pthread_mutex_lock(&ui->mutex);
    *out_snap = ui->snapshot;
    pthread_mutex_unlock(&ui->mutex);
}

void ascolto_tray_step_iteration(ascolto_tray_ui_t *ui) {
    if (!ui || !ui->gtk_impl) return;
    apply_snapshot_to_gtk_main_thread(ui);
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    int guard = 0;
    while (p->gtk_events_pending && p->gtk_events_pending() && guard++ < 64) {
        p->gtk_main_iteration_do(FALSE);
    }
    if (ui->dirty) {
        apply_snapshot_to_gtk_main_thread(ui);
    }
}

void ascolto_tray_destroy(ascolto_tray_ui_t *ui) {
    if (!ui) return;
    pthread_mutex_lock(&ui->mutex);
    ui->running = false;
    if (ui->gtk_impl) {
        gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
        if (p->ind_lib) dlclose(p->ind_lib);
        if (p->gtk_lib) dlclose(p->gtk_lib);
        free(p);
        ui->gtk_impl = NULL;
    }
    pthread_mutex_unlock(&ui->mutex);
    pthread_mutex_destroy(&ui->mutex);
}
