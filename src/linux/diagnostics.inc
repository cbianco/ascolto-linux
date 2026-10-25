/* Native diagnostics consume sanitized, asynchronously loaded runtime snapshots. */
static void *(*d_gtk_expander_new)(const char*);
static void (*d_gtk_expander_set_expanded)(void*, gboolean);
static gboolean (*d_gtk_expander_get_expanded)(void*);
static void *(*d_gtk_clipboard_get)(void*);
static void *(*d_gdk_atom_intern_static_string)(const char*);
static void (*d_gtk_clipboard_set_text)(void*, const char*, int);
static void diagnostics_build(ascolto_tray_ui_t *ui);
static void diagnostics_request(ascolto_tray_ui_t *ui, JsonObject *request, int lane);
static gint64 diagnostics_number(JsonObject *obj, const char *key) {
    JsonNode *node = obj ? json_object_get_member(obj, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) ? json_node_get_int(node) : 0;
}
static JsonObject *diagnostics_selected(ascolto_tray_ui_t *ui) {
    gtk_tray_priv_t *p = ui->gtk_impl;
    JsonArray *calls = setting_array(p->diagnostics_state, "calls");
    for (guint i = 0; calls && i < json_array_get_length(calls); i++) {
        JsonObject *call = json_array_get_object_element(calls, i);
        if (diagnostics_number(call, "id") == p->diagnostics_selected) return call;
    }
    return NULL;
}
static void diagnostics_clear(gtk_tray_priv_t *p, void *box) {
    GList *children = s_gtk_container_get_children(box);
    for (GList *item = children; item; item = item->next) p->gtk_widget_destroy(item->data);
    g_list_free(children);
}
static void diagnostics_worker_done(GObject *object, GAsyncResult *result, gpointer unused) {
    (void)unused;
    gchar *output = NULL; GError *error = NULL;
    gboolean success = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(object), result, &output, NULL, &error);
    ascolto_tray_ui_t *ui = g_object_get_data(object, "diagnostics-ui");
    if (!ui || !ui->gtk_impl) { g_free(output); g_clear_error(&error); return; }
    gtk_tray_priv_t *p = ui->gtk_impl;
    int lane = GPOINTER_TO_INT(g_object_get_data(object, "diagnostics-lane"));
    GSubprocess **slot = lane == 0 ? &p->diagnostics_worker : lane == 1 ? &p->diagnostics_task : &p->diagnostics_cancel;
    if (*slot != G_SUBPROCESS(object)) { g_free(output); g_clear_error(&error); return; }
    g_clear_object(slot);
    JsonParser *parser = json_parser_new(); JsonObject *response = NULL;
    if (success && output && json_parser_load_from_data(parser, output, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) response = json_node_get_object(root);
    }
    if (setting_bool(response, "ok")) {
        if (!lane) {
            JsonObject *snapshot = setting_object(response, "result");
            if (snapshot) {
                /* Avoid replacing focused widgets when only the refresh timestamp changed. */
                json_object_remove_member(snapshot, "generated_at_ms");
                JsonNode *next = json_node_new(JSON_NODE_OBJECT); json_node_set_object(next, snapshot);
                JsonNode *previous = json_node_new(JSON_NODE_OBJECT);
                if (p->diagnostics_state) json_node_set_object(previous, p->diagnostics_state);
                bool unchanged = p->diagnostics_state && json_node_equal(previous, next);
                json_node_free(previous); json_node_free(next);
                if (unchanged) { g_object_unref(parser); g_free(output); g_clear_error(&error); return; }
                if (p->diagnostics_state) json_object_unref(p->diagnostics_state);
                p->diagnostics_state = json_object_ref(snapshot);
            }
        } else {
            g_free(p->diagnostics_message);
            p->diagnostics_message = g_strdup(lane == 2 ? "Cancellation requested. A request already received by the provider may continue; publication will be stopped." : "Request finished. Review the recorded outcome below.");
        }
    } else {
        g_free(p->diagnostics_message);
        p->diagnostics_message = g_strdup(setting_string(response, "message", "Could not refresh activity. Check the Python runtime and retry."));
    }
    g_object_unref(parser); g_free(output); g_clear_error(&error);
    if (!lane && p->item_diagnostics) {
        gint64 attention = diagnostics_number(setting_object(p->diagnostics_state, "summary"), "needs_attention");
        char *label = attention ? g_strdup_printf("Activity & diagnostics… · %lld need attention", (long long)attention) : g_strdup("Activity & diagnostics…");
        p->gtk_menu_item_set_label(p->item_diagnostics, label); g_free(label);
    }
    diagnostics_build(ui);
    if (lane && p->diagnostics_visible) diagnostics_request(ui, settings_command("diagnostics", NULL), 0);
}
static void diagnostics_request(ascolto_tray_ui_t *ui, JsonObject *request, int lane) {
    gtk_tray_priv_t *p = ui->gtk_impl;
    GSubprocess **slot = lane == 0 ? &p->diagnostics_worker : lane == 1 ? &p->diagnostics_task : &p->diagnostics_cancel;
    if (*slot) { json_object_unref(request); return; }
    char script[2300], python[2300];
    if (ascolto_ai_worker_paths("ai_providers.py", script, sizeof(script), python, sizeof(python))) {
        json_object_unref(request); g_free(p->diagnostics_message);
        p->diagnostics_message = g_strdup("AI worker is missing. Rebuild Ascolto."); diagnostics_build(ui); return;
    }
    JsonNode *root = json_node_new(JSON_NODE_OBJECT); json_node_take_object(root, request);
    gchar *input = json_to_string(root, FALSE); json_node_free(root);
    *slot = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        NULL, python, "-B", script, "--config", ui->core->config.config_path, NULL);
    if (*slot) {
        g_object_set_data(G_OBJECT(*slot), "diagnostics-ui", ui);
        g_object_set_data(G_OBJECT(*slot), "diagnostics-lane", GINT_TO_POINTER(lane));
        g_subprocess_communicate_utf8_async(*slot, input, NULL, diagnostics_worker_done, NULL);
        if (lane) { g_free(p->diagnostics_message); p->diagnostics_message = g_strdup(lane == 1 ? "Starting the selected recap request…" : "Requesting cancellation…"); }
    } else { g_free(p->diagnostics_message); p->diagnostics_message = g_strdup("Could not start Python. Check the installed runtime."); }
    g_free(input); diagnostics_build(ui);
}
static void diagnostics_refresh(void *widget, gpointer data) {
    (void)widget; diagnostics_request(data, settings_command("diagnostics", NULL), 0);
}
static gboolean diagnostics_tick(gpointer data) {
    ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    if (!p) return G_SOURCE_REMOVE;
    diagnostics_refresh(NULL, data); return G_SOURCE_CONTINUE;
}
static void diagnostics_choose(void *widget, gpointer data) {
    ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    gint64 *id = g_object_get_data(G_OBJECT(widget), "call-id");
    if (id) p->diagnostics_selected = *id;
    diagnostics_build(ui);
}
static void diagnostics_filter(void *widget, gpointer data) {
    ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    p->diagnostics_attention = s_gtk_toggle_button_get_active(widget); diagnostics_build(ui);
}
static void diagnostics_action(void *widget, gpointer data) {
    JsonObject *request = g_object_get_data(G_OBJECT(widget), "diagnostics-request");
    int lane = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "diagnostics-lane"));
    if (request) diagnostics_request(data, json_object_ref(request), lane);
}
static void diagnostics_open(void *widget, gpointer data) {
    (void)data; const char *path = g_object_get_data(G_OBJECT(widget), "artifact-path");
    launch_path_nonblocking(path, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "artifact-directory")));
}
static void diagnostics_settings(void *widget, gpointer data) { on_menu_settings(widget, data); }
static void diagnostics_preview_close(void *widget, gpointer data) {
    (void)widget; ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    if (p->diagnostics_preview) { p->gtk_widget_destroy(p->diagnostics_preview); p->diagnostics_preview = NULL; }
    p->gtk_widget_show_all(p->diagnostics_columns);
}
static void diagnostics_copy_confirm(void *widget, gpointer data) {
    (void)data;
    const char *report = g_object_get_data(G_OBJECT(widget), "safe-report");
    if (report) d_gtk_clipboard_set_text(d_gtk_clipboard_get(d_gdk_atom_intern_static_string("CLIPBOARD")), report, -1);
}
static void diagnostics_preview(void *widget, gpointer data) {
    (void)widget; ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    JsonObject *call = diagnostics_selected(ui); if (!call) return;
    if (p->diagnostics_preview) p->gtk_widget_destroy(p->diagnostics_preview);
    p->diagnostics_preview = s_gtk_box_new(1, 10);
    void *box = p->diagnostics_preview; s_gtk_container_set_border_width(box, 18);
    s_gtk_box_pack_start(p->diagnostics_root, box, TRUE, TRUE, 0);
    s_gtk_widget_hide(p->diagnostics_columns);
    settings_button(ui, box, "Back to activity", G_CALLBACK(diagnostics_preview_close));
    settings_label(p, box, "Review before copying. Transcript text, audio, credentials and tokens are excluded. Nothing is uploaded.");
    void *scroll = s_gtk_scrolled_window_new(NULL, NULL); s_gtk_scrolled_window_set_policy(scroll, 2, 1); s_gtk_box_pack_start(box, scroll, TRUE, TRUE, 0);
    void *contents = s_gtk_box_new(1, 0); s_gtk_container_add(scroll, contents);
    const char *report = setting_string(call, "safe_report", "Diagnostic metadata unavailable.");
    void *label = settings_label(p, contents, report); s_gtk_label_set_selectable(label, TRUE);
    void *copy = settings_button(ui, box, "Copy displayed diagnostics", G_CALLBACK(diagnostics_copy_confirm));
    g_object_set_data_full(G_OBJECT(copy), "safe-report", g_strdup(report), g_free);
    p->gtk_widget_show_all(p->diagnostics_preview);
}
static void diagnostics_expanded(GObject *object, void *spec, gpointer data) {
    (void)spec; ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    p->diagnostics_expanded = d_gtk_expander_get_expanded(object);
}
static void diagnostics_request_button(ascolto_tray_ui_t *ui, void *box, const char *label, JsonObject *request, int lane) {
    gtk_tray_priv_t *p = ui->gtk_impl;
    void *button = settings_button(ui, box, label, G_CALLBACK(diagnostics_action));
    if (request) g_object_set_data_full(G_OBJECT(button), "diagnostics-request", json_object_ref(request), (GDestroyNotify)json_object_unref);
    g_object_set_data(G_OBJECT(button), "diagnostics-lane", GINT_TO_POINTER(lane));
    p->gtk_widget_set_sensitive(button, request && !(lane == 1 ? p->diagnostics_task : p->diagnostics_cancel));
}
static char *diagnostics_time(gint64 millis) {
    if (!millis) return g_strdup("Unavailable");
    GDateTime *date = g_date_time_new_from_unix_local(millis / 1000);
    if (!date) return g_strdup("Unavailable");
    char *text = g_date_time_format(date, "%Y-%m-%d %H:%M:%S %Z"); g_date_time_unref(date); return text;
}
static const char *diagnostics_status(const char *status) {
    static const char *codes[] = {"needs_attention", "not_requested", "validating", "queued", "running", "completed", "failed", "cancelled", "interrupted", "skipped", "ready", "recording", "saving", "not_available"};
    static const char *labels[] = {"Needs attention", "Not requested", "Validating input", "Queued", "Running", "Completed", "Failed", "Cancelled", "Interrupted", "Skipped", "Ready", "Recording", "Saving", "Unavailable"};
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) if (!strcmp(status, codes[i])) return labels[i];
    return status;
}
typedef struct { void *adjustment; double position; char *focus_label; void *detail; } diagnostics_restore_t;
static void diagnostics_focus_label(void *box, const char *text) {
    GList *children = s_gtk_container_get_children(box);
    for (GList *item = children; item; item = item->next) {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(item->data), "label")) {
            char *label = NULL; g_object_get(item->data, "label", &label, NULL);
            if (label && !strcmp(label, text)) { void (*focus)(void*) = dlsym(RTLD_DEFAULT, "gtk_widget_grab_focus"); if (focus) focus(item->data); }
            g_free(label);
        }
    }
    g_list_free(children);
}
static gboolean diagnostics_restore(gpointer data) {
    diagnostics_restore_t *saved = data;
    void (*set_value)(void*, double) = dlsym(RTLD_DEFAULT, "gtk_adjustment_set_value");
    if (set_value) set_value(saved->adjustment, saved->position);
    if (saved->focus_label) diagnostics_focus_label(saved->detail, saved->focus_label);
    return G_SOURCE_REMOVE;
}
static void diagnostics_restore_free(gpointer data) {
    diagnostics_restore_t *saved = data;
    g_object_unref(saved->adjustment); g_object_unref(saved->detail); g_free(saved->focus_label); g_free(saved);
}
static void diagnostics_build(ascolto_tray_ui_t *ui) {
    gtk_tray_priv_t *p = ui->gtk_impl; if (!p->diagnostics_window) return;
    void *(*get_adjustment)(void*) = dlsym(p->gtk_lib, "gtk_scrolled_window_get_vadjustment");
    double (*get_value)(void*) = dlsym(p->gtk_lib, "gtk_adjustment_get_value");
    void *(*get_focus)(void*) = dlsym(p->gtk_lib, "gtk_window_get_focus");
    diagnostics_restore_t *saved = NULL;
    if (get_adjustment && get_value && p->diagnostics_detail_scroll) {
        saved = g_new0(diagnostics_restore_t, 1); saved->adjustment = g_object_ref(get_adjustment(p->diagnostics_detail_scroll));
        saved->position = get_value(saved->adjustment); saved->detail = g_object_ref(p->diagnostics_detail);
        void *focus = get_focus ? get_focus(p->settings_window) : NULL;
        if (focus && g_object_class_find_property(G_OBJECT_GET_CLASS(focus), "label")) g_object_get(focus, "label", &saved->focus_label, NULL);
    }
    diagnostics_clear(p, p->diagnostics_list); diagnostics_clear(p, p->diagnostics_detail);
    JsonObject *summary = setting_object(p->diagnostics_state, "summary");
    char *counts = g_strdup_printf("%lld calls · %lld need attention · %lld running", (long long)diagnostics_number(summary, "total_calls"), (long long)diagnostics_number(summary, "needs_attention"), (long long)diagnostics_number(summary, "running"));
    settings_label(p, p->diagnostics_list, counts); g_free(counts);
    JsonArray *calls = setting_array(p->diagnostics_state, "calls");
    JsonObject *selected = NULL; guint shown = 0;
    for (guint i = 0; calls && i < json_array_get_length(calls); i++) {
        JsonObject *call = json_array_get_object_element(calls, i);
        if (p->diagnostics_attention && !setting_bool(call, "needs_attention")) continue;
        if (diagnostics_number(call, "id") == p->diagnostics_selected) selected = call;
        char *title = g_strdup_printf("%s%s\n%s", diagnostics_number(call, "id") == p->diagnostics_selected ? "● " : "", setting_string(call, "label", "Call"), diagnostics_status(setting_string(call, "status", "Unknown")));
        void *button = settings_button(ui, p->diagnostics_list, title, G_CALLBACK(diagnostics_choose)); g_free(title);
        gint64 *id = g_new(gint64, 1); *id = diagnostics_number(call, "id");
        g_object_set_data_full(G_OBJECT(button), "call-id", id, g_free); shown++;
    }
    if (!selected) {
        for (guint i = 0; calls && i < json_array_get_length(calls); i++) {
            JsonObject *call = json_array_get_object_element(calls, i);
            if (!p->diagnostics_attention || setting_bool(call, "needs_attention")) { selected = call; p->diagnostics_selected = diagnostics_number(call, "id"); break; }
        }
    }
    if (p->diagnostics_message && *p->diagnostics_message) settings_label(p, p->diagnostics_detail, p->diagnostics_message);
    if (!shown) settings_label(p, p->diagnostics_detail, p->diagnostics_state ? "No calls match this view." : "Loading activity…");
    if (selected) {
        settings_label(p, p->diagnostics_detail, setting_string(selected, "label", "Selected call"));
        settings_label(p, p->diagnostics_detail, setting_string(selected, "outcome", "Outcome unavailable"));
        settings_label(p, p->diagnostics_detail, setting_string(selected, "reason", ""));
        JsonArray *operations = setting_array(selected, "operations");
        for (guint i = 0; operations && i < json_array_get_length(operations); i++) {
            JsonObject *operation = json_array_get_object_element(operations, i);
            char *text = g_strdup_printf("%s: %s\n%s", setting_string(operation, "name", "Operation"), diagnostics_status(setting_string(operation, "status", "Unknown")), setting_string(operation, "reason", ""));
            settings_label(p, p->diagnostics_detail, text); g_free(text);
        }
        JsonArray *artifacts = setting_array(selected, "artifacts");
        for (guint i = 0; artifacts && i < json_array_get_length(artifacts); i++) {
            JsonObject *artifact = json_array_get_object_element(artifacts, i);
            void *button = settings_button(ui, p->diagnostics_detail, setting_string(artifact, "label", "Open artifact"), G_CALLBACK(diagnostics_open));
            g_object_set_data_full(G_OBJECT(button), "artifact-path", g_strdup(setting_string(artifact, "path", "")), g_free);
            g_object_set_data(G_OBJECT(button), "artifact-directory", GINT_TO_POINTER(setting_bool(artifact, "is_directory")));
        }
        JsonArray *active_attempts = setting_array(selected, "attempts");
        for (guint i = 0; active_attempts && i < json_array_get_length(active_attempts); i++) {
            JsonObject *attempt = json_array_get_object_element(active_attempts, i);
            JsonObject *cancel = setting_object(attempt, "cancel_request");
            if (cancel || !strcmp(setting_string(attempt, "status", ""), "running") || !strcmp(setting_string(attempt, "status", ""), "queued") || !strcmp(setting_string(attempt, "status", ""), "validating")) {
                char *active = g_strdup_printf("%s · %s / %s\nProvider: %s\nModel: %s\nElapsed: %.1f s\n%s", diagnostics_status(setting_string(attempt, "status", "Unknown")), setting_string(attempt, "task", ""), setting_string(attempt, "phase", ""), setting_string(attempt, "provider", "Unavailable"), setting_string(attempt, "model_label", setting_string(attempt, "model", "Unavailable")), (double)diagnostics_number(attempt, "elapsed_ms") / 1000.0, setting_bool(attempt, "cancel_requested") ? "Cancellation requested. The provider may continue; publication will be stopped." : setting_string(attempt, "reason", ""));
                settings_label(p, p->diagnostics_detail, active); g_free(active);
            }
            if (cancel) diagnostics_request_button(ui, p->diagnostics_detail, "Cancel request", cancel, 2);
        }
        diagnostics_request_button(ui, p->diagnostics_detail, "Retry / generate recap", setting_object(selected, "retry_request"), 1);
        settings_label(p, p->diagnostics_detail, setting_string(selected, "retry_reason", ""));
        settings_button(ui, p->diagnostics_detail, "Check connection and settings…", G_CALLBACK(diagnostics_settings));
        settings_button(ui, p->diagnostics_detail, "Copy diagnostics…", G_CALLBACK(diagnostics_preview));
        void *expander = d_gtk_expander_new("Technical details and attempts");
        d_gtk_expander_set_expanded(expander, p->diagnostics_expanded);
        g_signal_connect_data(expander, "notify::expanded", G_CALLBACK(diagnostics_expanded), ui, NULL, 0);
        s_gtk_box_pack_start(p->diagnostics_detail, expander, FALSE, FALSE, 6);
        void *technical = s_gtk_box_new(1, 8); s_gtk_container_add(expander, technical);
        JsonArray *attempts = setting_array(selected, "attempts");
        if (!attempts || !json_array_get_length(attempts)) settings_label(p, technical, "No recorded AI attempts for this call.");
        for (guint i = 0; attempts && i < json_array_get_length(attempts); i++) {
            JsonObject *attempt = json_array_get_object_element(attempts, i);
            char *queued = diagnostics_time(diagnostics_number(attempt, "queued_at_ms"));
            char *started = diagnostics_time(diagnostics_number(attempt, "started_at_ms"));
            char *finished = diagnostics_time(diagnostics_number(attempt, "finished_at_ms"));
            JsonNode *elapsed_node = json_object_get_member(attempt, "elapsed_ms");
            char *elapsed = elapsed_node && !JSON_NODE_HOLDS_NULL(elapsed_node) ? g_strdup_printf("%.1f s", (double)diagnostics_number(attempt, "elapsed_ms") / 1000.0) : g_strdup("Unavailable");
            char *text = g_strdup_printf("Job %lld · attempt %lld · %s / %s\n%s · %s\nProvider: %s\nModel: %s\nQueued: %s\nStarted: %s\nFinished: %s\nElapsed: %s\nError code: %s\n%s", (long long)diagnostics_number(attempt, "id"), (long long)diagnostics_number(attempt, "attempt"), setting_string(attempt, "task", ""), setting_string(attempt, "phase", ""), diagnostics_status(setting_string(attempt, "status", "Unknown")), setting_bool(attempt, "cancel_requested") ? "Cancellation requested" : "", setting_string(attempt, "provider", "Unavailable"), setting_string(attempt, "model", "Unavailable"), queued, started, finished, elapsed, setting_string(attempt, "error_code", "None"), setting_string(attempt, "reason", ""));
            void *label = settings_label(p, technical, text); s_gtk_label_set_selectable(label, TRUE);
            g_free(text); g_free(queued); g_free(started); g_free(finished); g_free(elapsed);
            JsonArray *events = setting_array(attempt, "events");
            for (guint j = 0; events && j < json_array_get_length(events); j++) {
                JsonObject *event = json_array_get_object_element(events, j); char *when = diagnostics_time(diagnostics_number(event, "at_ms"));
                char *line = g_strdup_printf("%s — %s %s", when, diagnostics_status(setting_string(event, "status", "")), setting_string(event, "error_code", ""));
                settings_label(p, technical, line); g_free(line); g_free(when);
            }
            JsonObject *cancel = setting_object(attempt, "cancel_request");
            if (cancel) diagnostics_request_button(ui, technical, "Cancel request", cancel, 2);
            JsonArray *progress = setting_array(attempt, "progress_events");
            for (guint j = 0; progress && j < json_array_get_length(progress); j++) {
                JsonObject *event = json_array_get_object_element(progress, j);
                char *when = diagnostics_time(diagnostics_number(event, "at_ms"));
                char *line = g_strdup_printf("%s — %s", when, setting_string(event, "description", ""));
                settings_label(p, technical, line); g_free(line); g_free(when);
            }
        }
        settings_label(p, technical, "Only recorded stages are shown. Stage timings and failed provider responses may be unavailable.");
    }
    p->gtk_widget_show_all(p->diagnostics_list); p->gtk_widget_show_all(p->diagnostics_detail);
    if (p->diagnostics_restore_source && g_main_context_find_source_by_id(NULL, p->diagnostics_restore_source)) g_source_remove(p->diagnostics_restore_source);
    if (saved) p->diagnostics_restore_source = g_timeout_add_full(G_PRIORITY_DEFAULT, 25, diagnostics_restore, saved, diagnostics_restore_free);
}
static void diagnostics_show_section(ascolto_tray_ui_t *ui) {
    gtk_tray_priv_t *p = ui->gtk_impl;
    if (!settings_load_symbols(p)) return;
    if (!p->diagnostics_root) {
#define DL(name) d_##name = dlsym(p->gtk_lib, #name); if (!d_##name) return
        DL(gtk_expander_new); DL(gtk_expander_set_expanded); DL(gtk_expander_get_expanded);
        DL(gtk_clipboard_get); DL(gdk_atom_intern_static_string); DL(gtk_clipboard_set_text);
#undef DL
        p->diagnostics_window = p->settings_window;
        void *outer = s_gtk_box_new(1, 10); p->diagnostics_root = outer;
        s_gtk_box_pack_start(p->settings_layout, outer, TRUE, TRUE, 0);
        void *toolbar = s_gtk_box_new(0, 12); s_gtk_box_pack_start(outer, toolbar, FALSE, FALSE, 0);
        settings_label(p, toolbar, "Activity & diagnostics");
        void *filter = s_gtk_check_button_new_with_label("Needs attention"); s_gtk_box_pack_start(toolbar, filter, FALSE, FALSE, 0);
        s_gtk_toggle_button_set_active(filter, p->diagnostics_attention);
        g_signal_connect_data(filter, "toggled", G_CALLBACK(diagnostics_filter), ui, NULL, 0);
        settings_button(ui, toolbar, "Refresh", G_CALLBACK(diagnostics_refresh));
        void *columns = s_gtk_box_new(0, 18); p->diagnostics_columns = columns; s_gtk_box_pack_start(outer, columns, TRUE, TRUE, 0);
        void *list_scroll = s_gtk_scrolled_window_new(NULL, NULL); s_gtk_scrolled_window_set_policy(list_scroll, 2, 1); s_gtk_widget_set_size_request(list_scroll, 240, -1); s_gtk_box_pack_start(columns, list_scroll, FALSE, FALSE, 0);
        p->diagnostics_list = s_gtk_box_new(1, 6); s_gtk_container_add(list_scroll, p->diagnostics_list);
        void *detail_scroll = s_gtk_scrolled_window_new(NULL, NULL); p->diagnostics_detail_scroll = detail_scroll;
        s_gtk_scrolled_window_set_policy(detail_scroll, 2, 1); s_gtk_box_pack_start(columns, detail_scroll, TRUE, TRUE, 0);
        p->diagnostics_detail = s_gtk_box_new(1, 8); s_gtk_container_set_border_width(p->diagnostics_detail, 8); s_gtk_container_add(detail_scroll, p->diagnostics_detail);
        diagnostics_build(ui);
    }
    p->diagnostics_visible = true;
    p->gtk_widget_show_all(p->diagnostics_root);
    if (p->diagnostics_preview) s_gtk_widget_hide(p->diagnostics_columns);
    s_gtk_widget_hide(p->settings_scroll);
}
static void on_menu_diagnostics(void *widget, gpointer data) {
    (void)widget; ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    if (!p->settings_window) on_menu_settings(NULL, ui);
    if (!p->settings_window) return;
    p->settings_category = 5; diagnostics_show_section(ui);
    p->gtk_widget_show_all(p->settings_window); s_gtk_widget_hide(p->settings_scroll);
    if (p->diagnostics_preview) s_gtk_widget_hide(p->diagnostics_columns);
    s_gtk_window_present(p->settings_window);
    diagnostics_refresh(NULL, ui);
}
static void diagnostics_destroy(ascolto_tray_ui_t *ui) {
    gtk_tray_priv_t *p = ui->gtk_impl;
    if (p->diagnostics_timer) g_source_remove(p->diagnostics_timer);
    if (p->diagnostics_restore_source && g_main_context_find_source_by_id(NULL, p->diagnostics_restore_source)) g_source_remove(p->diagnostics_restore_source);
    GSubprocess **slots[] = {&p->diagnostics_worker, &p->diagnostics_task, &p->diagnostics_cancel};
    for (size_t i = 0; i < 3; i++) if (*slots[i]) { g_object_set_data(G_OBJECT(*slots[i]), "diagnostics-ui", NULL); g_subprocess_force_exit(*slots[i]); g_clear_object(slots[i]); }

    if (p->diagnostics_state) json_object_unref(p->diagnostics_state);
    g_free(p->diagnostics_message);
}
