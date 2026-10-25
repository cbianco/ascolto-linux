#define _POSIX_C_SOURCE 200809L
#include "ascolto/filesystem.h"
#include "ascolto/ai_settings.h"
#include <json-glib/json-glib.h>
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
    void *config_menu;
    void *diagnostics_window, *diagnostics_root, *diagnostics_list, *diagnostics_detail;
    void *diagnostics_detail_scroll, *settings_layout, *settings_scroll;
    void *diagnostics_preview, *diagnostics_columns, *item_diagnostics;
    JsonObject *diagnostics_state;
    GSubprocess *diagnostics_worker, *diagnostics_task, *diagnostics_cancel;
    guint diagnostics_timer, diagnostics_restore_source;
    gint64 diagnostics_selected;
    bool diagnostics_attention, diagnostics_visible, diagnostics_expanded;
    char *diagnostics_message;
    void *settings_window, *settings_page;
    void *settings_auto, *settings_save, *settings_legacy, *settings_tts, *settings_keywords;
    void *settings_archive_label;
    void *ai_key, *ai_model, *ai_language, *ai_auto, *ai_account, *ai_use;
    void *stt_model, *stt_consent, *stt_auto;
    int settings_category;
    bool settings_cloud_tab;
    uint64_t local_revision;
    bool local_action_pending;
    void *local_models_box;
    bool settings_building;
    JsonObject *ai_state;
    GSubprocess *ai_worker;
    char ai_provider[32];
    char *ai_message;
    char *ai_pending_task;
    char *ai_output_dir;
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
    void *item_open_latest;

    void *item_details[5];
    void *mic_root;
    void *sys_root;
    GPtrArray *choices;
    GPtrArray *device_choices;
    GThread *device_thread;
    gint device_ready;
    ascolto_device_info_t devices[ASCOLTO_MAX_DEVICES];
    size_t device_count;
    ascolto_device_info_t discovered_devices[ASCOLTO_MAX_DEVICES];
    size_t discovered_device_count;
    GThreadPool *actions;
    gint action_pending;
    gint action_kind;
    gint action_failed;
    char icon_dir[1024];
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
    ascolto_tray_snapshot_t snapshot; ascolto_tray_get_snapshot(ui,&snapshot);
    launch_path_nonblocking(snapshot.archive_directory, true);
}

void ascolto_tray_open_session_folder(ascolto_tray_ui_t *ui) {
    if (!ui || !ui->core) return;
    ascolto_tray_snapshot_t snapshot; ascolto_tray_get_snapshot(ui,&snapshot);
    const char *dir = snapshot.session_directory[0] ? snapshot.session_directory : snapshot.archive_directory;
    launch_path_nonblocking(dir, true);
}

static void apply_snapshot_to_gtk_main_thread(ascolto_tray_ui_t *ui);

typedef enum { MENU_START, MENU_STOP, MENU_PAUSE, MENU_RESUME, MENU_AUTO,
    MENU_AUDIO, MENU_CLOUD, MENU_TTS, MENU_LANGUAGE, MENU_MIC, MENU_SYS, MENU_SPEAKERS,
    MENU_RECAP, MENU_MP3, MENU_LEGACY_RECAP, MENU_KEYWORDS, MENU_OUTPUT, MENU_LOCAL_USE, MENU_LOCAL_DOWNLOAD, MENU_LOCAL_REMOVE } menu_action_t;
typedef struct { ascolto_tray_ui_t *ui; menu_action_t kind; char value[1024]; } menu_request_t;

static void menu_action_worker(gpointer data, gpointer unused) {
    (void)unused;
    menu_request_t *r = data;
    ascolto_core_t *c = r->ui->core;
    gtk_tray_priv_t *p = r->ui->gtk_impl;
    bool enabled = atoi(r->value) != 0;
    int result = 0;
    switch (r->kind) {
        case MENU_START: result = ascolto_core_start_manual(c); break;
        case MENU_STOP: result = ascolto_core_stop(c); break;
        case MENU_PAUSE: result = ascolto_core_pause(c); break;
        case MENU_RESUME: result = ascolto_core_resume(c); break;
        case MENU_AUTO: result = ascolto_core_set_auto_mic(c, enabled); break;
        case MENU_AUDIO: result = ascolto_core_set_save_audio(c, enabled); break;
        case MENU_CLOUD: result = ascolto_core_set_cloud_recap(c, enabled); break;
        case MENU_TTS: result = ascolto_core_set_recap_tts(c, enabled); break;
        case MENU_LANGUAGE: result = ascolto_core_set_language(c, r->value); break;
        case MENU_MIC: result = ascolto_core_set_mic_device(c, r->value); break;
        case MENU_SYS: result = ascolto_core_set_sys_device(c, r->value); break;
        case MENU_SPEAKERS: result = ascolto_core_set_sensitivity(c, atoi(r->value)); break;
        case MENU_RECAP: {
            char config[512], transcript[512];
            pthread_mutex_lock(&c->mutex);
            snprintf(config, sizeof(config), "%s", c->config.config_path);
            snprintf(transcript, sizeof(transcript), "%s", c->current_md_path);
            pthread_mutex_unlock(&c->mutex);
            result = ascolto_ai_recap_start(config, transcript, false); break;
        }
        case MENU_LEGACY_RECAP: result = ascolto_core_run_recap(c, NULL); break;
        case MENU_KEYWORDS: result = ascolto_core_set_keywords(c, r->value); break;
        case MENU_LOCAL_USE: result = ascolto_local_models_select(c->local_models, r->value); break;
        case MENU_LOCAL_DOWNLOAD: result = ascolto_local_models_download(c->local_models, r->value); break;
        case MENU_LOCAL_REMOVE: result = ascolto_local_models_remove(c->local_models, r->value); break;
        case MENU_OUTPUT: result = ascolto_core_set_output_directory(c, r->value); break;
        case MENU_MP3: result = ascolto_core_make_mp3(c, NULL); break;
    }
    g_atomic_int_set(&p->action_failed, result ? r->kind + 1 : 0);
    g_atomic_int_set(&p->action_pending, 0);
    ascolto_tray_update(r->ui);
    g_free(r);
}

static void queue_menu_action(ascolto_tray_ui_t *ui, menu_action_t kind, const char *value) {
    gtk_tray_priv_t *p = ui->gtk_impl;
    if (!p->actions) return;
    if (!g_atomic_int_compare_and_exchange(&p->action_pending, 0, 1)) return;
    g_atomic_int_set(&p->action_kind, kind);
    g_atomic_int_set(&p->action_failed, 0);
    menu_request_t *r = g_new0(menu_request_t, 1);
    r->ui = ui; r->kind = kind;
    snprintf(r->value, sizeof(r->value), "%s", value ? value : "");
    if (!g_thread_pool_push(p->actions, r, NULL)) {
        g_atomic_int_set(&p->action_pending, 0); g_free(r);
    }
    ascolto_tray_update(ui);
}

static void on_menu_start_stop(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core) return;
    ascolto_tray_snapshot_t snap;
    ascolto_tray_get_snapshot(ui, &snap);
    if (snap.menu_stopping) return;
    queue_menu_action(ui, snap.menu_recording_enabled ? MENU_STOP : MENU_START, NULL);
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_pause_resume(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core) return;
    ascolto_tray_snapshot_t snap;
    ascolto_tray_get_snapshot(ui, &snap);
    queue_menu_action(ui, snap.menu_paused ? MENU_RESUME : MENU_PAUSE, NULL);
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_auto_mic_toggled(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->updating_widgets) return;
    gboolean active = p->gtk_check_menu_item_get_active(widget);
    queue_menu_action(ui, MENU_AUTO, active ? "1" : "0");
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_save_audio_toggled(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->updating_widgets) return;
    gboolean active = p->gtk_check_menu_item_get_active(widget);
    queue_menu_action(ui, MENU_AUDIO, active ? "1" : "0");
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_cloud_recap_toggled(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->updating_widgets) return;
    queue_menu_action(ui, MENU_CLOUD, p->gtk_check_menu_item_get_active(widget) ? "1" : "0");
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_recap_tts_toggled(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->updating_widgets) return;
    queue_menu_action(ui, MENU_TTS, p->gtk_check_menu_item_get_active(widget) ? "1" : "0");
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_settings(void *widget, gpointer data);
static void settings_request(ascolto_tray_ui_t *ui, JsonObject *request);
static JsonObject *settings_command(const char *action, const char *provider);
static void on_menu_recap_now(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core) return;
    gtk_tray_priv_t *p=ui->gtk_impl;
    on_menu_settings(NULL,ui);p->settings_category=4;
    if(p->ai_worker){g_free(p->ai_pending_task);p->ai_pending_task=g_strdup("recap");}
    else settings_request(ui,settings_command("recap",NULL));
}

static void on_menu_make_mp3(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core) return;
    queue_menu_action(ui, MENU_MP3, NULL);
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
        (gchar *)"--title=Ascolto — Custom keywords",
        (gchar *)"--text=Enter keywords, names or acronyms, separated by commas:",
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
    ascolto_tray_open_transcripts_folder(ui);
}

static void on_menu_quit(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (ui && ui->core) {
        ui->core->running = false;
    }
}

static void *menu_item(gtk_tray_priv_t *p, void *menu, const char *label,
                       GCallback callback, ascolto_tray_ui_t *ui) {
    void *item = p->gtk_menu_item_new_with_label(label);
    if (callback) g_signal_connect_data(item, "activate", callback, ui, NULL, 0);
    p->gtk_menu_shell_append(menu, item);
    return item;
}
static void *submenu(gtk_tray_priv_t *p, const char *label, void **root) {
    void *menu = p->gtk_menu_new();
    void *item = menu_item(p, p->menu, label, NULL, NULL);
    p->gtk_menu_item_set_submenu(item, menu);
    if (root) *root = item;
    return menu;
}
static void on_choice(void *widget, gpointer unused) {
    (void)unused;
    menu_request_t *r = g_object_get_data(widget, "ascolto-choice");
    gtk_tray_priv_t *p = r->ui->gtk_impl;
    if (!p->updating_widgets) queue_menu_action(r->ui, r->kind, r->value);
}
static void choice(ascolto_tray_ui_t *ui, void *menu, GPtrArray *items,
                   const char *label, menu_action_t kind, const char *value) {
    gtk_tray_priv_t *p = ui->gtk_impl;
    void *item = p->gtk_check_menu_item_new_with_label(label);
    menu_request_t *r = g_new0(menu_request_t, 1);
    r->ui = ui; r->kind = kind;
    snprintf(r->value, sizeof(r->value), "%s", value);
    g_object_set_data_full(item, "ascolto-choice", r, g_free);
    g_signal_connect_data(item, "activate", G_CALLBACK(on_choice), NULL, NULL, 0);
    p->gtk_menu_shell_append(menu, item);
    g_ptr_array_add(items, item);
}
static gpointer device_worker(gpointer data) {
    gtk_tray_priv_t *p = data;
    ascolto_pipewire_enumerate_devices(p->discovered_devices, ASCOLTO_MAX_DEVICES, &p->discovered_device_count);
    g_atomic_int_set(&p->device_ready, 1);
    return NULL;
}
static void refresh_devices(void *widget, gpointer data) {
    (void)widget;
    ascolto_tray_ui_t *ui = data;
    gtk_tray_priv_t *p = ui->gtk_impl;
    if (!p->device_thread) {
        g_atomic_int_set(&p->device_ready, 0);
        p->device_thread = g_thread_new("ascolto-devices", device_worker, p);
    }
}
static void build_devices(ascolto_tray_ui_t *ui) {
    gtk_tray_priv_t *p = ui->gtk_impl;
    g_ptr_array_set_size(p->device_choices, 0);
    for (int channel = 0; channel < 2; channel++) {
        void *menu = p->gtk_menu_new();
        choice(ui, menu, p->device_choices, channel ? "Automatic" : "System default",
               channel ? MENU_SYS : MENU_MIC, "default");
        for (size_t i = 0; i < p->device_count; i++) {
            ascolto_device_info_t *d = &p->devices[i];
            if (d->is_default || d->is_monitor != (channel != 0)) continue;
            choice(ui, menu, p->device_choices, d->description[0] ? d->description : d->name,
                   channel ? MENU_SYS : MENU_MIC, d->name);
        }
        if (p->device_thread && !g_atomic_int_get(&p->device_ready)) {
            void *loading = menu_item(p, menu, "Loading audio devices…", NULL, NULL);
            p->gtk_widget_set_sensitive(loading, FALSE);
        }
        p->gtk_menu_shell_append(menu, p->gtk_separator_menu_item_new());
        menu_item(p, menu, "Refresh audio devices", G_CALLBACK(refresh_devices), ui);
        p->gtk_widget_show_all(menu);
        p->gtk_menu_item_set_submenu(channel ? p->sys_root : p->mic_root, menu);
    }
}
static void on_open_latest(void *widget, gpointer data) {
    (void)widget;
    ascolto_tray_ui_t *ui = data;
    ascolto_tray_snapshot_t snap;
    ascolto_tray_get_snapshot(ui, &snap);
    if (snap.recent_files_count) launch_path_nonblocking(snap.recent_files[0], false);
}

static void on_menu_diagnostics(void *widget, gpointer data);
static void diagnostics_show_section(ascolto_tray_ui_t *ui);
#include "linux/settings.inc"
#include "linux/diagnostics.inc"

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

    ui->gtk_impl = p;
    p->actions = g_thread_pool_new(menu_action_worker, NULL, 1, FALSE, NULL);
    p->choices = g_ptr_array_new();
    p->device_choices = g_ptr_array_new();
    ssize_t executable_len = readlink("/proc/self/exe", p->icon_dir, sizeof(p->icon_dir)-1);
    if (executable_len > 0) {
        p->icon_dir[executable_len] = 0;
        char *slash = strrchr(p->icon_dir, '/');
        if (slash) snprintf(slash, sizeof(p->icon_dir)-(size_t)(slash-p->icon_dir), "/../assets");
        void (*set_path)(void *, const char *) = dlsym(p->ind_lib, "app_indicator_set_icon_theme_path");
        if (set_path) set_path(p->indicator, p->icon_dir);
    }
    p->menu = p->gtk_menu_new();
    p->item_status = menu_item(p, p->menu, "Ready for a call", NULL, NULL);
    p->gtk_widget_set_sensitive(p->item_status, FALSE);
    p->gtk_menu_shell_append(p->menu, p->gtk_separator_menu_item_new());
    p->item_start_stop = menu_item(p, p->menu, "Start transcription", G_CALLBACK(on_menu_start_stop), ui);
    void *details = submenu(p, "Session details", NULL);
    for (int i = 0; i < 5; i++) {
        p->item_details[i] = menu_item(p, details, "—", NULL, NULL);
        p->gtk_widget_set_sensitive(p->item_details[i], FALSE);
    }
    p->gtk_menu_shell_append(p->menu, p->gtk_separator_menu_item_new());
    p->item_auto_mic = p->gtk_check_menu_item_new_with_label("Automatic monitoring");
    g_signal_connect_data(p->item_auto_mic, "toggled", G_CALLBACK(on_menu_auto_mic_toggled), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_auto_mic);
    /* Linux pauses the current transcription, not automatic monitoring. */
    p->item_pause_resume = menu_item(p, p->menu, "Pause transcription", G_CALLBACK(on_menu_pause_resume), ui);
    p->gtk_menu_shell_append(p->menu, p->gtk_separator_menu_item_new());
    void *visible_menu = p->menu;
    p->config_menu = p->gtk_menu_new();
    p->menu = p->config_menu;
    void *languages = submenu(p, "Transcription language", NULL);
    const char *codes[] = {"auto","it","en","de","fr","es","nl","pl"};
    const char *labels[] = {"Detect automatically","Italiano (it)","English (en)","Deutsch (de)","Français (fr)","Español (es)","Nederlands (nl)","Polski (pl)"};
    for (size_t i = 0; i < 8; i++) choice(ui, languages, p->choices, labels[i], MENU_LANGUAGE, codes[i]);
    submenu(p, "Microphone", &p->mic_root);
    submenu(p, "Call audio", &p->sys_root);
    refresh_devices(NULL, ui);
    build_devices(ui);
    void *speakers = submenu(p, "Speaker recognition", NULL);
    choice(ui, speakers, p->choices, "High sensitivity", MENU_SPEAKERS, "2");
    choice(ui, speakers, p->choices, "Balanced · recommended", MENU_SPEAKERS, "1");
    choice(ui, speakers, p->choices, "Conservative", MENU_SPEAKERS, "0");
    void *recap = submenu(p, "Recording & recap", NULL);
    p->item_save_audio = p->gtk_check_menu_item_new_with_label("Save compressed audio (FLAC · next session)");
    p->gtk_check_menu_item_set_active(p->item_save_audio, ui->core->config.save_audio ? TRUE : FALSE);
    g_signal_connect_data(p->item_save_audio, "toggled", G_CALLBACK(on_menu_save_audio_toggled), ui, NULL, 0);
    p->gtk_menu_shell_append(recap, p->item_save_audio);

    p->item_cloud_recap = p->gtk_check_menu_item_new_with_label("Cloud recap (OpenAI + Claude)");
    p->gtk_check_menu_item_set_active(p->item_cloud_recap, ui->core->config.cloud_recap ? TRUE : FALSE);
    g_signal_connect_data(p->item_cloud_recap, "toggled", G_CALLBACK(on_menu_cloud_recap_toggled), ui, NULL, 0);
    p->gtk_menu_shell_append(recap, p->item_cloud_recap);

    p->item_recap_tts = p->gtk_check_menu_item_new_with_label("Spoken recap (OpenAI TTS)");
    p->gtk_check_menu_item_set_active(p->item_recap_tts, ui->core->config.recap_tts ? TRUE : FALSE);
    g_signal_connect_data(p->item_recap_tts, "toggled", G_CALLBACK(on_menu_recap_tts_toggled), ui, NULL, 0);
    p->gtk_menu_shell_append(recap, p->item_recap_tts);

    p->item_recap_now = p->gtk_menu_item_new_with_label("Generate latest recap");
    g_signal_connect_data(p->item_recap_now, "activate", G_CALLBACK(on_menu_recap_now), ui, NULL, 0);
    p->gtk_menu_shell_append(details, p->item_recap_now);

    p->item_make_mp3 = p->gtk_menu_item_new_with_label("Generate latest spoken recap");
    g_signal_connect_data(p->item_make_mp3, "activate", G_CALLBACK(on_menu_make_mp3), ui, NULL, 0);
    p->gtk_menu_shell_append(details, p->item_make_mp3);


    p->menu = visible_menu;
    menu_item(p, details, "Open latest session folder…", G_CALLBACK(on_menu_open_session_folder), ui);
    menu_item(p, p->menu, "Settings…", G_CALLBACK(on_menu_settings), ui);
    p->item_diagnostics = menu_item(p, p->menu, "Activity & diagnostics…", G_CALLBACK(on_menu_diagnostics), ui);
    p->gtk_menu_shell_append(p->menu, p->gtk_separator_menu_item_new());
    p->item_open_latest = menu_item(p, p->menu, "Open latest transcript", G_CALLBACK(on_open_latest), ui);
    menu_item(p, p->menu, "Open transcripts folder…", G_CALLBACK(on_menu_open_folder), ui);

    p->item_keywords = menu_item(p, p->config_menu, "Edit custom keywords…", G_CALLBACK(on_menu_keywords_dialog), ui);
    p->gtk_menu_shell_append(p->menu, p->gtk_separator_menu_item_new());
    menu_item(p, p->menu, "Quit Ascolto", G_CALLBACK(on_menu_quit), ui);
    p->gtk_widget_show_all(p->menu);
    p->app_indicator_set_menu(p->indicator, p->menu);
    p->diagnostics_timer = g_timeout_add_seconds(5, diagnostics_tick, ui);
    diagnostics_refresh(NULL, ui);
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
    settings_sync(ui, &snap);

    if (first || strcmp(snap.icon_name, p->applied.icon_name) != 0) {
        p->app_indicator_set_icon_full(p->indicator, snap.icon_name, snap.status_text);
    }
    if (first || strcmp(snap.label, p->applied.label) != 0) {
        p->app_indicator_set_label(p->indicator, "", "");
    }
    bool pending = g_atomic_int_get(&p->action_pending);
    bool busy = snap.menu_stopping || pending;
    int kind = g_atomic_int_get(&p->action_kind);
    int failed = g_atomic_int_get(&p->action_failed);
    const char *progress = snap.menu_stopping || (pending && kind == MENU_STOP) ? "Saving session…" :
        pending && kind == MENU_START ? "Starting transcription…" : "Updating settings…";
    ascolto_local_models_snapshot_t local_models;ascolto_local_models_snapshot(ui->core->local_models,&local_models);
    p->gtk_menu_item_set_label(p->item_status, local_models.error[0] && !busy ? "Local transcription needs attention · open Transcription Settings" : failed ?
        (failed == MENU_RECAP + 1 || failed == MENU_MP3 + 1 ? "Could not generate recap · check setup" : "Action failed · check diagnostics") :
        busy ? progress : snap.status_text);
    p->gtk_menu_item_set_label(p->item_start_stop, busy ? progress :
        snap.menu_recording_enabled ? "Stop transcription" : "Start transcription");
    p->gtk_widget_set_sensitive(p->item_start_stop, !busy);
    p->gtk_menu_item_set_label(p->item_pause_resume, snap.menu_paused ? "Resume transcription" : "Pause transcription");
    p->gtk_widget_set_sensitive(p->item_pause_resume, snap.menu_recording_enabled && !busy);
    bool can_recap = snap.recent_files_count && !snap.menu_recording_enabled && !busy;
    p->gtk_widget_set_sensitive(p->item_recap_now, can_recap);
    p->gtk_widget_set_sensitive(p->item_make_mp3, can_recap);
    p->gtk_widget_set_sensitive(p->item_open_latest, snap.recent_files_count > 0);
    void *toggles[] = {p->item_auto_mic, p->item_save_audio, p->item_cloud_recap, p->item_recap_tts};
    for (size_t i = 0; i < 4; i++) p->gtk_widget_set_sensitive(toggles[i], !busy);
    for (int group = 0; group < 2; group++) {
        GPtrArray *items = group ? p->device_choices : p->choices;
        for (guint i = 0; i < items->len; i++) {
            void *item = g_ptr_array_index(items, i);
            menu_request_t *r = g_object_get_data(item, "ascolto-choice");
            bool checked = r->kind == MENU_LANGUAGE ? !strcmp(r->value, snap.selected_language) :
                r->kind == MENU_MIC ? !strcmp(r->value, snap.selected_mic) :
                r->kind == MENU_SYS ? !strcmp(r->value, snap.selected_sys) : atoi(r->value) == (int)snap.selected_sensitivity;
            p->gtk_check_menu_item_set_active(item, checked);
            p->gtk_widget_set_sensitive(item, !busy);
        }
    }
    char detail[384];
    const char *values[] = {snap.session_app, snap.selected_mic, snap.selected_sys, snap.selected_language, snap.latest_phrase};
    const char *labels[] = {"Microphone app", "Microphone", "Call audio", "Language", "Latest phrase"};
    for (int i = 0; i < 5; i++) {
        snprintf(detail, sizeof(detail), "%s: %.300s", labels[i], values[i][0] ? values[i] : "None");
        p->gtk_menu_item_set_label(p->item_details[i], detail);
    }
    if (first || !busy || snap.menu_auto_mic_checked != p->applied.menu_auto_mic_checked) {
        p->gtk_check_menu_item_set_active(p->item_auto_mic,
                                          snap.menu_auto_mic_checked ? TRUE : FALSE);
    }
    if (first || !busy || snap.menu_save_audio_checked != p->applied.menu_save_audio_checked) {
        p->gtk_check_menu_item_set_active(p->item_save_audio,
                                          snap.menu_save_audio_checked ? TRUE : FALSE);
    }
    if (first || !busy || snap.menu_cloud_recap_checked != p->applied.menu_cloud_recap_checked) {
        p->gtk_check_menu_item_set_active(p->item_cloud_recap,
                                          snap.menu_cloud_recap_checked ? TRUE : FALSE);
    }
    if (first || !busy || snap.menu_recap_tts_checked != p->applied.menu_recap_tts_checked) {
        p->gtk_check_menu_item_set_active(p->item_recap_tts,
                                          snap.menu_recap_tts_checked ? TRUE : FALSE);
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

    if (pthread_mutex_trylock(&core->mutex)) { ui->dirty = true; return; }
    uint64_t elapsed = ascolto_core_elapsed_sec(core);
    unsigned int mins = (unsigned int)(elapsed / 60);
    unsigned int secs = (unsigned int)(elapsed % 60);

    pthread_mutex_lock(&ui->mutex);
    ascolto_tray_snapshot_t *s = &ui->snapshot;
    s->active = true;
    s->storage_error = core->storage_error;
    snprintf(s->archive_directory,sizeof(s->archive_directory),"%s",core->config.output_dir);
    snprintf(s->session_directory,sizeof(s->session_directory),"%s",core->current_session_dir);
    s->menu_stopping = core->state == ASCOLTO_STATE_STOPPING;
    snprintf(s->session_app, sizeof(s->session_app), "%s", core->active_mic_app);
    snprintf(s->latest_phrase, sizeof(s->latest_phrase), "%.255s",
             core->segments_count ? core->segments[core->segments_count-1].text : "");
    s->menu_auto_mic_checked = core->config.auto_mic_trigger;
    s->menu_save_audio_checked = core->config.save_audio;
    s->menu_cloud_recap_checked = core->config.cloud_recap;
    s->menu_recap_tts_checked = core->config.recap_tts;
    snprintf(s->selected_language, sizeof(s->selected_language), "%s", core->config.language);
    snprintf(s->selected_mic, sizeof(s->selected_mic), "%s", core->config.mic_device);
    snprintf(s->selected_sys, sizeof(s->selected_sys), "%s", core->config.sys_device);
    s->selected_sensitivity = core->config.diarization_sensitivity;
    snprintf(s->settings_keywords, sizeof(s->settings_keywords), "%s", core->config.keywords);
    snprintf(s->keywords_summary, sizeof(s->keywords_summary), "%.255s",
             core->config.keywords[0] ? core->config.keywords : "(nessuna)");

    s->recent_files_count = core->recent_files_count;
    for (size_t i = 0; i < core->recent_files_count && i < ASCOLTO_MAX_RECENT_FILES; i++) {
        snprintf(s->recent_files[i], sizeof(s->recent_files[i]), "%s", core->recent_files[core->recent_files_count - i - 1]);
    }

    if (core->state == ASCOLTO_STATE_IDLE) {
        snprintf(s->icon_name, sizeof(s->icon_name), "ascolto-idle-symbolic");
        s->label[0] = 0;
        snprintf(s->status_text, sizeof(s->status_text), "Ready for a call");
        s->menu_recording_enabled = false;
        s->menu_paused = false;
    } else if (core->state == ASCOLTO_STATE_PAUSED) {
        snprintf(s->icon_name, sizeof(s->icon_name), "ascolto-paused-symbolic");
        s->label[0] = 0;
        snprintf(s->status_text, sizeof(s->status_text), "Transcription paused · %02u:%02u", mins, secs);
        s->menu_recording_enabled = true;
        s->menu_paused = true;
    } else if (core->state == ASCOLTO_STATE_STOPPING) {
        snprintf(s->icon_name, sizeof(s->icon_name), "ascolto-saving-symbolic");
        s->label[0] = 0;
        snprintf(s->status_text, sizeof(s->status_text), "Saving session…");
        s->menu_recording_enabled = true;
        s->menu_paused = false;
    } else {
        snprintf(s->icon_name, sizeof(s->icon_name), "ascolto-recording-symbolic");
        s->label[0] = 0;
        snprintf(s->status_text, sizeof(s->status_text),
                 "Recording · %02u:%02u · %s",
                 mins, secs,
                 core->state == ASCOLTO_STATE_RECORDING_AUTO ? "Automatic" : "Manual");
        s->menu_recording_enabled = true;
        s->menu_paused = false;
    }

    if(core->storage_error)snprintf(s->status_text,sizeof(s->status_text),"Transcript storage failed · local artifacts retained");
    ui->dirty = true;
    pthread_mutex_unlock(&ui->mutex);
    pthread_mutex_unlock(&core->mutex);
}

void ascolto_tray_get_snapshot(ascolto_tray_ui_t *ui, ascolto_tray_snapshot_t *out_snap) {
    if (!ui || !out_snap) return;
    pthread_mutex_lock(&ui->mutex);
    *out_snap = ui->snapshot;
    pthread_mutex_unlock(&ui->mutex);
}

void ascolto_tray_step_iteration(ascolto_tray_ui_t *ui) {
    if (!ui || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->device_thread && g_atomic_int_get(&p->device_ready)) {
        g_thread_join(p->device_thread); p->device_thread = NULL;
        memcpy(p->devices, p->discovered_devices, sizeof(p->devices));
        p->device_count = p->discovered_device_count;
        build_devices(ui);
        if (p->settings_window && p->settings_category == 3) settings_build(ui);
        pthread_mutex_lock(&ui->mutex); ui->dirty = true; pthread_mutex_unlock(&ui->mutex);
    }
    if (p->local_models_box) {
        ascolto_local_models_snapshot_t models; ascolto_local_models_snapshot(ui->core->local_models, &models);
        if (models.revision != p->local_revision || p->local_action_pending != !!g_atomic_int_get(&p->action_pending)) settings_render_local_models(ui);
    }
    apply_snapshot_to_gtk_main_thread(ui);
    int guard = 0;
    while (p->gtk_events_pending && p->gtk_events_pending() && guard++ < 64) {
        p->gtk_main_iteration_do(FALSE);
    }
    if (ui->dirty) {
        apply_snapshot_to_gtk_main_thread(ui);
    }
}

void ascolto_tray_wait_actions(ascolto_tray_ui_t *ui) {
    if (!ui || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = ui->gtk_impl;
    if (p->actions) {
        g_thread_pool_free(p->actions, FALSE, TRUE);
        p->actions = NULL;
    }
}

void ascolto_tray_destroy(ascolto_tray_ui_t *ui) {
    if (!ui) return;
    gtk_tray_priv_t *pending = ui->gtk_impl;
    if (pending) {
        ascolto_tray_wait_actions(ui);
        if (pending->device_thread) g_thread_join(pending->device_thread);
    }
    pthread_mutex_lock(&ui->mutex);
    ui->running = false;
    if (ui->gtk_impl) {
        gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
        settings_cancel(ui);
        diagnostics_destroy(ui);
        if (p->settings_window) p->gtk_widget_destroy(p->settings_window);
        if (p->ai_state) json_object_unref(p->ai_state);
        g_free(p->ai_message);
        g_free(p->ai_pending_task);
        g_free(p->ai_output_dir);
        if (p->config_menu) p->gtk_widget_destroy(p->config_menu);
        p->gtk_widget_destroy(p->menu);
        g_object_unref(p->indicator);
        g_ptr_array_free(p->choices, TRUE);
        g_ptr_array_free(p->device_choices, TRUE);
        if (p->ind_lib) dlclose(p->ind_lib);
        if (p->gtk_lib) dlclose(p->gtk_lib);
        free(p);
        ui->gtk_impl = NULL;
    }
    pthread_mutex_unlock(&ui->mutex);
    pthread_mutex_destroy(&ui->mutex);
}
