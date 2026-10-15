/* Native GTK settings, loaded with the existing optional GTK runtime. */
#define SF(ret, name, args) typedef ret (*settings_##name##_t) args; static settings_##name##_t s_##name
SF(void*, gtk_window_new, (int));
SF(void, gtk_window_set_title, (void*, const char*));
SF(void, gtk_window_set_default_size, (void*, int, int));
SF(void, gtk_window_present, (void*));
SF(void*, gtk_box_new, (int, int));
SF(void, gtk_box_pack_start, (void*, void*, gboolean, gboolean, guint));
SF(void, gtk_container_add, (void*, void*));
SF(void, gtk_container_set_border_width, (void*, guint));
SF(GList*, gtk_container_get_children, (void*));
SF(void*, gtk_label_new, (const char*));
SF(void, gtk_label_set_xalign, (void*, float));
SF(void, gtk_label_set_line_wrap, (void*, gboolean));
SF(void, gtk_label_set_text, (void*, const char*));
SF(void, gtk_label_set_markup, (void*, const char*));
SF(void*, gtk_image_new_from_file, (const char*));
SF(void, gtk_button_set_image, (void*, void*));
SF(void, gtk_button_set_always_show_image, (void*, gboolean));
SF(void*, gtk_css_provider_new, (void));
SF(gboolean, gtk_css_provider_load_from_data, (void*, const char*, gssize, GError**));
SF(void*, gtk_widget_get_style_context, (void*));
SF(void, gtk_style_context_add_provider, (void*, void*, guint));
SF(void, gtk_label_set_selectable, (void*, gboolean));
SF(void*, gtk_button_new_with_label, (const char*));
SF(void*, gtk_check_button_new_with_label, (const char*));
SF(void, gtk_toggle_button_set_active, (void*, gboolean));
SF(gboolean, gtk_toggle_button_get_active, (void*));
SF(void*, gtk_entry_new, (void));
SF(void, gtk_entry_set_text, (void*, const char*));
SF(const char*, gtk_entry_get_text, (void*));
SF(void, gtk_entry_set_visibility, (void*, gboolean));
SF(void, gtk_entry_set_placeholder_text, (void*, const char*));
SF(void, gtk_entry_set_max_length, (void*, int));
SF(void*, gtk_combo_box_text_new, (void));
SF(void, gtk_combo_box_text_append, (void*, const char*, const char*));
SF(gboolean, gtk_combo_box_set_active_id, (void*, const char*));
SF(const char*, gtk_combo_box_get_active_id, (void*));
SF(void, gtk_widget_set_size_request, (void*, int, int));
SF(void, gtk_widget_hide, (void*));
SF(void*, gtk_scrolled_window_new, (void*, void*));
SF(void, gtk_scrolled_window_set_policy, (void*, int, int));
#undef SF

static void settings_build(ascolto_tray_ui_t *ui);
static void settings_request(ascolto_tray_ui_t *ui, JsonObject *request);
static const char *setting_string(JsonObject *obj, const char *name, const char *fallback) {
    if (!obj || !json_object_has_member(obj, name)) return fallback;
    JsonNode *node = json_object_get_member(obj, name);
    return JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : fallback;
}
static bool setting_bool(JsonObject *obj, const char *name) {
    return obj && json_object_has_member(obj, name) && json_object_get_boolean_member(obj, name);
}
static JsonObject *setting_object(JsonObject *obj, const char *name) {
    JsonNode *node = obj ? json_object_get_member(obj, name) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *setting_array(JsonObject *obj, const char *name) {
    JsonNode *node = obj ? json_object_get_member(obj, name) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static JsonObject *settings_command(const char *action, const char *provider) {
    JsonObject *request = json_object_new(); json_object_set_string_member(request, "action", action);
    if (provider) json_object_set_string_member(request, "provider", provider);
    return request;
}
static void *settings_label(gtk_tray_priv_t *p, void *box, const char *text) {
    (void)p;
    void *label = s_gtk_label_new(text); s_gtk_label_set_xalign(label, 0);
    s_gtk_label_set_line_wrap(label, TRUE);
    s_gtk_box_pack_start(box, label, FALSE, FALSE, 4); return label;
}
static void *settings_button(ascolto_tray_ui_t *ui, void *box, const char *text, GCallback callback) {
    void *button = s_gtk_button_new_with_label(text);
    if (callback) g_signal_connect_data(button, "clicked", callback, ui, NULL, 0);
    s_gtk_box_pack_start(box, button, FALSE, FALSE, 2); return button;
}
static void settings_cancel(ascolto_tray_ui_t *ui) {
    gtk_tray_priv_t *p = ui->gtk_impl;
    g_clear_pointer(&p->ai_pending_task,g_free);
    if (!p->ai_worker) return;
    g_object_set_data(G_OBJECT(p->ai_worker), "ascolto-ui", NULL);
    g_subprocess_force_exit(p->ai_worker);
    g_clear_object(&p->ai_worker);
}
static void settings_cancel_clicked(void *widget, gpointer data) {
    (void)widget;
    ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    settings_cancel(ui);
    g_free(p->ai_message); p->ai_message = g_strdup("Cancelled. Your existing connection has been kept.");
    settings_build(ui);
}
static void settings_worker_done(GObject *object, GAsyncResult *result, gpointer unused) {
    (void)unused;
    GError *error = NULL; gchar *output = NULL;
    gboolean success = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(object), result, &output, NULL, &error);
    ascolto_tray_ui_t *ui = g_object_get_data(object, "ascolto-ui");
    if (!ui || !ui->gtk_impl) { g_clear_error(&error); g_free(output); return; }
    gtk_tray_priv_t *p = ui->gtk_impl;
    if (p->ai_worker != G_SUBPROCESS(object)) { g_clear_error(&error); g_free(output); return; }
    g_clear_object(&p->ai_worker);
    JsonParser *parser = json_parser_new();
    JsonObject *response = NULL;
    if (success && output && json_parser_load_from_data(parser, output, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) response = json_node_get_object(root);
    }
    g_free(p->ai_message); p->ai_message = NULL;
    if (setting_bool(response, "ok")) {
        JsonObject *state = setting_object(response, "settings");
        if(!p->ai_state){const char *saved=setting_string(state,"settings_section","");const char *sections[]={"General","AI providers","Transcription","Recording","Storage"};for(int i=0;i<5;i++)if(!strcmp(saved,sections[i]))p->settings_category=i;}
        if (p->ai_state) json_object_unref(p->ai_state);
        p->ai_state = state ? json_object_ref(state) : NULL;
        const char *action = g_object_get_data(object, "ascolto-ai-action");
        if ((action && !strcmp(action, "use")) || setting_bool(p->ai_state, "automatic_recaps")) queue_menu_action(ui, MENU_CLOUD, "0");
        p->ai_message = g_strdup(setting_string(response, "message", ""));
        const char *output_dir=setting_string(setting_object(response,"result"),"output_dir","");if(*output_dir){g_free(p->ai_output_dir);p->ai_output_dir=g_strdup(output_dir);}
        JsonObject *profile = setting_object(setting_object(p->ai_state, "profiles"), "openai");
        if (setting_bool(profile, "welcome_pending")) {
            g_free(p->ai_message); p->ai_message = g_strdup("You're using your ChatGPT plan. Eligible requests use your plan. Manage usage in ChatGPT settings. Select Got it to continue.");
        }
    } else p->ai_message = g_strdup(setting_string(response, "message", "Could not start the AI worker. Check Python and run make recap-deps."));
    settings_build(ui);
    g_object_unref(parser); g_clear_error(&error); g_free(output);
    if(p->ai_pending_task){char *pending=p->ai_pending_task;p->ai_pending_task=NULL;settings_request(ui,settings_command(pending,NULL));g_free(pending);}
}
static void settings_request(ascolto_tray_ui_t *ui, JsonObject *request) {
    gtk_tray_priv_t *p = ui->gtk_impl;
    if (p->ai_worker) { json_object_unref(request); return; }
    char script[2300], python[2300];
    if (ascolto_ai_worker_paths("ai_providers.py", script, sizeof(script), python, sizeof(python))) {
        json_object_unref(request); g_free(p->ai_message); p->ai_message = g_strdup("AI worker is missing. Rebuild Ascolto."); settings_build(ui); return;
    }
    JsonNode *root = json_node_new(JSON_NODE_OBJECT); json_node_set_object(root, request);
    gchar *input = json_to_string(root, FALSE); json_node_free(root);
    gchar *action = g_strdup(setting_string(request, "action", ""));
    bool signin = !strcmp(action, "sign_in");
    json_object_unref(request);
    GError *error = NULL;
    p->ai_worker = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &error, python, "-B", script, "--config", ui->core->config.config_path, NULL);
    g_free(p->ai_message);
    p->ai_message = g_strdup(p->ai_worker ? signin ? "Waiting for your browser… Complete sign-in or cancel here to keep the existing connection." : "Updating settings or running the selected AI task…" : "Could not start Python. Install Python 3 and run make recap-deps.");
    if (p->ai_worker) {
        g_object_set_data(G_OBJECT(p->ai_worker), "ascolto-ui", ui);
        g_object_set_data_full(G_OBJECT(p->ai_worker), "ascolto-ai-action", action, g_free); action = NULL;
        g_subprocess_communicate_utf8_async(p->ai_worker, input, NULL, settings_worker_done, NULL);
    }
    g_free(input); g_free(action); g_clear_error(&error); settings_build(ui);
}
static void settings_provider(void *widget, gpointer data) {
    ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    if (p->ai_worker) return;
    snprintf(p->ai_provider, sizeof(p->ai_provider), "%s", (const char*)g_object_get_data(widget, "provider"));
    g_clear_pointer(&p->ai_message, g_free); settings_build(ui);
}
static void settings_reveal(void *widget, gpointer data) {
    gtk_tray_priv_t *p = ((ascolto_tray_ui_t*)data)->gtk_impl;
    s_gtk_entry_set_visibility(p->ai_key, s_gtk_toggle_button_get_active(widget));
}
static void settings_connect(void *widget, gpointer data) {
    (void)widget; ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    JsonObject *request = settings_command(!strcmp(p->ai_provider, "openai") ? "sign_in" : "connect", p->ai_provider);
    if (strcmp(p->ai_provider, "openai")) {
        json_object_set_string_member(request, "key", s_gtk_entry_get_text(p->ai_key));
        s_gtk_entry_set_text(p->ai_key, "");
    } else {
        const char *account = s_gtk_combo_box_get_active_id(p->ai_account);
        if (account && *account) json_object_set_string_member(request, "account_id", account);
    }
    settings_request(ui, request);
}
static void settings_refresh_models(void *widget, gpointer data) {
    (void)widget; ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    settings_request(ui, settings_command("refresh_models", p->ai_provider));
}
static void settings_use(void *widget, gpointer data) {
    (void)widget; ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    const char *model = s_gtk_combo_box_get_active_id(p->ai_model);
    JsonObject *request = settings_command("use", p->ai_provider);
    json_object_set_string_member(request, "model", model ? model : "");
    json_object_set_boolean_member(request, "acknowledge_go_policy", !strcmp(p->ai_provider,"opencode-go")); settings_request(ui, request);
}
static void settings_preferences(void *widget, gpointer data) {
    (void)widget; ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    if (p->settings_building) return;
    JsonObject *request = settings_command("preferences", NULL);
    const char *language = s_gtk_combo_box_get_active_id(p->ai_language);
    json_object_set_string_member(request, "language", language ? language : "Italian");
    json_object_set_boolean_member(request, "automatic_recaps", s_gtk_toggle_button_get_active(p->ai_auto));
    settings_request(ui, request);
}
static void settings_disconnect(void *widget, gpointer data) {
    (void)widget; ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    settings_request(ui, settings_command("disconnect", p->ai_provider));
}
static void settings_get_key(void *widget, gpointer data) {
    (void)widget; gtk_tray_priv_t *p = ((ascolto_tray_ui_t*)data)->gtk_impl;
    g_app_info_launch_default_for_uri(!strncmp(p->ai_provider, "opencode", 8) ? "https://opencode.ai/console" : "https://openrouter.ai/settings/keys", NULL, NULL);
}
static void settings_model_changed(void *widget, gpointer data) {
    gtk_tray_priv_t *p = ((ascolto_tray_ui_t*)data)->gtk_impl;
    if (p->settings_building || !p->ai_use) return;
    const char *model = s_gtk_combo_box_get_active_id(widget);
    p->gtk_widget_set_sensitive(p->ai_use, model && *model && !p->ai_worker);
}
static void settings_usage(void *widget, gpointer data) {
    (void)widget; (void)data;
    g_app_info_launch_default_for_uri("https://chatgpt.com/settings/usage", NULL, NULL);
}
static void settings_acknowledge(void *widget, gpointer data) {
    (void)widget; settings_request(data, settings_command("acknowledge_plan", NULL));
}
static void *settings_combo(gtk_tray_priv_t *p, void *box, const char *label) {
    settings_label(p, box, label); void *combo = s_gtk_combo_box_text_new();
    s_gtk_box_pack_start(box, combo, FALSE, FALSE, 2); return combo;
}
static void settings_native_changed(void *widget, gpointer data) {
    ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    if (p->settings_building) return;
    menu_action_t action = GPOINTER_TO_INT(g_object_get_data(widget, "native-action"));
    const char *value = s_gtk_combo_box_get_active_id(widget);
    if (value) queue_menu_action(ui, action, value);
}
static void settings_native_toggled(void *widget, gpointer data) {
    ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    if (p->settings_building) return;
    menu_action_t action = GPOINTER_TO_INT(g_object_get_data(widget, "native-action"));
    queue_menu_action(ui, action, s_gtk_toggle_button_get_active(widget) ? "1" : "0");
}
static void *settings_native_check(ascolto_tray_ui_t *ui, void *box, const char *label, menu_action_t action, bool value) {
    void *check = s_gtk_check_button_new_with_label(label);
    s_gtk_toggle_button_set_active(check, value);
    g_object_set_data(check, "native-action", GINT_TO_POINTER(action));
    g_signal_connect_data(check, "toggled", G_CALLBACK(settings_native_toggled), ui, NULL, 0);
    s_gtk_box_pack_start(box, check, FALSE, FALSE, 3); return check;
}
static void settings_native_combo(ascolto_tray_ui_t *ui, void *combo, menu_action_t action, const char *value) {
    s_gtk_combo_box_set_active_id(combo, value);
    g_object_set_data(combo, "native-action", GINT_TO_POINTER(action));
    g_signal_connect_data(combo, "changed", G_CALLBACK(settings_native_changed), ui, NULL, 0);
}
static void settings_keywords(void *widget, gpointer data) {
    (void)widget; ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    queue_menu_action(ui, MENU_KEYWORDS, s_gtk_entry_get_text(p->settings_keywords));
}
static void settings_legacy_recap(void *widget, gpointer data) { (void)widget; queue_menu_action(data, MENU_LEGACY_RECAP, ""); }
static void settings_audio_permissions(void *widget, gpointer data) {
    (void)widget; (void)data;
    gchar *argv[] = {"gnome-control-center", "sound", NULL};
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}
static void settings_open_output(void *widget,gpointer data){(void)widget;gtk_tray_priv_t *p=((ascolto_tray_ui_t*)data)->gtk_impl;if(p->ai_output_dir){gchar *uri=g_filename_to_uri(p->ai_output_dir,NULL,NULL);if(uri){g_app_info_launch_default_for_uri(uri,NULL,NULL);g_free(uri);}}}
static void settings_stt_refresh(void *widget,gpointer data){(void)widget;settings_request(data,settings_command("refresh_transcription_models",NULL));}
static void settings_stt_save(void *widget,gpointer data){
    (void)widget;ascolto_tray_ui_t *ui=data;gtk_tray_priv_t *p=ui->gtk_impl;
    JsonObject *request=settings_command("transcription_preferences","openrouter");
    const char *model=s_gtk_combo_box_get_active_id(p->stt_model);
    json_object_set_string_member(request,"model",model?model:"");
    json_object_set_boolean_member(request,"enabled",s_gtk_toggle_button_get_active(p->stt_auto));
    json_object_set_boolean_member(request,"cloud_audio_opt_in",s_gtk_toggle_button_get_active(p->stt_consent));settings_request(ui,request);
}
static void settings_task(void *widget,gpointer data){settings_request(data,settings_command(g_object_get_data(widget,"ai-task"),NULL));}
static void *settings_task_button(ascolto_tray_ui_t *ui,void *box,const char *text,const char *task){void *button=settings_button(ui,box,text,G_CALLBACK(settings_task));g_object_set_data(button,"ai-task",(gpointer)task);return button;}
static void settings_category(void *widget, gpointer data) {
    ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    p->settings_category = GPOINTER_TO_INT(g_object_get_data(widget, "category"));
    settings_build(ui);
    if (!p->ai_worker) {
        const char *sections[]={"General","AI providers","Transcription","Recording","Storage"};
        JsonObject *request=settings_command("section",NULL);json_object_set_string_member(request,"section",sections[p->settings_category]);settings_request(ui,request);
    }
}
static void settings_build(ascolto_tray_ui_t *ui) {
    gtk_tray_priv_t *p = ui->gtk_impl;
    if (!p->settings_page) return;
    p->settings_building = true;
    GList *children = s_gtk_container_get_children(p->settings_page);
    for (GList *it = children; it; it = it->next) p->gtk_widget_destroy(it->data);
    g_list_free(children);
    p->settings_auto = p->settings_save = p->settings_legacy = p->settings_tts = NULL;
    ascolto_tray_snapshot_t snap; ascolto_tray_get_snapshot(ui, &snap);
    if(snap.storage_error)settings_label(p,p->settings_page,"Transcript storage failed. Local artifacts are retained; review the database and storage permissions.");
    const char *categories[] = {"General", "AI providers", "Transcription", "Recording", "Storage"};
    void *heading = settings_label(p, p->settings_page, categories[p->settings_category]);
    gchar *markup = g_strdup_printf("<span size=\"x-large\" weight=\"bold\">%s</span>", categories[p->settings_category]);
    s_gtk_label_set_markup(heading, markup); g_free(markup);
    if (p->settings_category == 0) {
        settings_label(p, p->settings_page, "Choose when Ascolto listens and manage system audio access.");
        p->settings_auto = settings_native_check(ui, p->settings_page, "Automatic monitoring", MENU_AUTO, snap.menu_auto_mic_checked);
        settings_label(p, p->settings_page, "Mirrors the shortcut in the panel. Linux pause applies to the current transcription session.");
        settings_button(ui, p->settings_page, "Open system audio settings…", G_CALLBACK(settings_audio_permissions));
        settings_label(p, p->settings_page, "Microphone and call audio are captured through PipeWire. Use desktop sound settings to select and manage device access.");
    } else if (p->settings_category == 2) {
        settings_label(p, p->settings_page, "Whistle transcribes locally on this device.");
        void *combo = settings_combo(p, p->settings_page, "Transcription language");
        const char *codes[] = {"auto", "it", "en", "de", "fr", "es", "nl", "pl"};
        const char *labels[] = {"Detect automatically", "Italian", "English", "German", "French", "Spanish", "Dutch", "Polish"};
        for (int i=0; i<8; i++) s_gtk_combo_box_text_append(combo, codes[i], labels[i]);
        settings_native_combo(ui, combo, MENU_LANGUAGE, snap.selected_language);
        combo = settings_combo(p, p->settings_page, "Speaker recognition");
        s_gtk_combo_box_text_append(combo, "2", "High sensitivity"); s_gtk_combo_box_text_append(combo, "1", "Balanced · recommended"); s_gtk_combo_box_text_append(combo, "0", "Conservative");
        char sensitivity[8]; snprintf(sensitivity, sizeof(sensitivity), "%d", snap.selected_sensitivity);
        settings_native_combo(ui, combo, MENU_SPEAKERS, sensitivity);
        settings_label(p, p->settings_page, "Custom keywords, names and abbreviations (comma separated)");
        p->settings_keywords = s_gtk_entry_new(); s_gtk_entry_set_max_length(p->settings_keywords, 1023);
        s_gtk_entry_set_text(p->settings_keywords, snap.settings_keywords);
        s_gtk_box_pack_start(p->settings_page, p->settings_keywords, FALSE, FALSE, 2);
        settings_button(ui, p->settings_page, "Save keywords", G_CALLBACK(settings_keywords));
        settings_label(p,p->settings_page,"Optional post-call cloud transcription · OpenRouter transcription");
        settings_label(p,p->settings_page,"Whistle remains the live engine. ChatGPT sign-in supports text recaps only. Enable Save audio in Recording to upload the complete MIC/SYS timeline.");
        JsonObject *preference=setting_object(p->ai_state,"transcription");
        p->stt_model=settings_combo(p,p->settings_page,"Post-call transcription model");s_gtk_combo_box_text_append(p->stt_model,"","Local Whistle only");
        JsonArray *models=setting_array(setting_object(setting_object(p->ai_state,"profiles"),"openrouter"),"transcription_models");
        for(guint index=0;models&&index<json_array_get_length(models);index++){JsonObject *model=json_array_get_object_element(models,index);s_gtk_combo_box_text_append(p->stt_model,setting_string(model,"id",""),setting_string(model,"name",setting_string(model,"id","")));}
        s_gtk_combo_box_set_active_id(p->stt_model,setting_string(preference,"model",""));
        settings_button(ui,p->settings_page,"Refresh transcription models",G_CALLBACK(settings_stt_refresh));
        p->stt_consent=s_gtk_check_button_new_with_label("Allow uploading saved MIC and SYS audio to the selected OpenRouter model");s_gtk_toggle_button_set_active(p->stt_consent,setting_bool(preference,"cloud_audio_opt_in"));s_gtk_box_pack_start(p->settings_page,p->stt_consent,FALSE,FALSE,2);
        p->stt_auto=s_gtk_check_button_new_with_label("Transcribe complete saved audio automatically after each session");s_gtk_toggle_button_set_active(p->stt_auto,setting_bool(preference,"enabled"));s_gtk_box_pack_start(p->settings_page,p->stt_auto,FALSE,FALSE,2);
        settings_button(ui,p->settings_page,"Save cloud preferences",G_CALLBACK(settings_stt_save));
        settings_task_button(ui,p->settings_page,"Preview latest audio upload","plan_transcription");
        settings_task_button(ui,p->settings_page,"Transcribe latest saved session","transcription");
    } else if (p->settings_category == 3) {
        settings_label(p, p->settings_page, "Choose audio sources. Changes apply to the next session.");
        for (int channel=0; channel<2; channel++) {
            void *combo = settings_combo(p, p->settings_page, channel ? "Call audio" : "Microphone");
            s_gtk_combo_box_text_append(combo, "default", channel ? "Automatic" : "System default");
            for (size_t i=0; i<p->device_count; i++) {
                ascolto_device_info_t *device = &p->devices[i];
                if (!device->is_default && device->is_monitor == (channel != 0)) s_gtk_combo_box_text_append(combo, device->name, device->description[0] ? device->description : device->name);
            }
            settings_native_combo(ui, combo, channel ? MENU_SYS : MENU_MIC, channel ? snap.selected_sys : snap.selected_mic);
        }
        settings_button(ui, p->settings_page, "Refresh audio devices", G_CALLBACK(refresh_devices));
        p->settings_save = settings_native_check(ui, p->settings_page, "Save compressed audio (separate lossless FLAC channels)", MENU_AUDIO, snap.menu_save_audio_checked);
        settings_label(p, p->settings_page, "Legacy cloud tools: separate opt-in audio upload to OpenAI, Claude CLI recap and optional OpenAI TTS. Requires its existing setup.");
        p->settings_legacy = settings_native_check(ui, p->settings_page, "Legacy automatic cloud transcription and recap", MENU_CLOUD, snap.menu_cloud_recap_checked);
        p->settings_tts = settings_native_check(ui, p->settings_page, "Legacy spoken recap (OpenAI TTS)", MENU_TTS, snap.menu_recap_tts_checked);
        settings_button(ui, p->settings_page, "Generate latest legacy cloud recap", G_CALLBACK(settings_legacy_recap));
    } else if (p->settings_category == 4) {
        settings_label(p, p->settings_page, "Transcript archive folder");
        void *location = settings_label(p, p->settings_page, ui->core->config.output_dir); s_gtk_label_set_selectable(location, TRUE);
        settings_button(ui, p->settings_page, "Open transcripts folder…", G_CALLBACK(on_menu_open_folder));
        settings_label(p, p->settings_page, "Disconnecting a provider keeps every transcript and audio archive. Recaps are written to a separate recap.md file in the session folder.");
        settings_task_button(ui,p->settings_page,"Provisional recap so far","provisional_recap");
        settings_task_button(ui,p->settings_page,"Generate final recap of latest session","recap");
        settings_task_button(ui,p->settings_page,"Refresh AI job status","status");
        JsonArray *jobs=setting_array(p->ai_state,"jobs");
        if(jobs&&json_array_get_length(jobs)){JsonObject *job=json_array_get_object_element(jobs,0);gchar *status=g_strdup_printf("Latest %s: %s · %s · %s",setting_string(job,"task",""),setting_string(job,"status",""),setting_string(job,"model_label_snapshot",""),setting_string(job,"error_code",""));settings_label(p,p->settings_page,status);g_free(status);}
    } else {
        bool busy = p->ai_worker != NULL;
        JsonObject *profile = setting_object(setting_object(p->ai_state, "profiles"), p->ai_provider);
        bool connected = setting_bool(profile, "connected");
        settings_label(p, p->settings_page, "Choose how Ascolto generates your recaps. Only transcript text is sent.");
        void *cards = s_gtk_box_new(0, 8); s_gtk_box_pack_start(p->settings_page, cards, FALSE, FALSE, 4);
        const char *providers[] = {"opencode-zen", "openrouter", "openai"};
        const char *names[] = {"OpenCode · Zen / Go", "OpenRouter · API key", "OpenAI · ChatGPT plan"};
        for (int i=0; i<3; i++) {
            void *button = settings_button(ui, cards, names[i], G_CALLBACK(settings_provider));
            g_object_set_data(button, "provider", (gpointer)providers[i]); p->gtk_widget_set_sensitive(button, !busy);
        }
        if (!strncmp(p->ai_provider, "opencode", 8)) {
            void *plans = s_gtk_box_new(0, 8); s_gtk_box_pack_start(p->settings_page, plans, FALSE, FALSE, 2);
            void *zen = settings_button(ui, plans, "Zen · pay as you go", G_CALLBACK(settings_provider)); g_object_set_data(zen, "provider", "opencode-zen");
            void *go = settings_button(ui, plans, "Go · subscription", G_CALLBACK(settings_provider)); g_object_set_data(go, "provider", "opencode-go");
            p->gtk_widget_set_sensitive(zen, !busy); p->gtk_widget_set_sensitive(go, !busy);
            settings_label(p, p->settings_page, !strcmp(p->ai_provider, "opencode-go") ? "OpenCode Go — separate API key profile" : "OpenCode Zen — separate API key profile");
        }
        if (!strcmp(p->ai_provider, "openai")) {
            p->ai_account = settings_combo(p, p->settings_page, "ChatGPT account"); s_gtk_combo_box_text_append(p->ai_account, "", "Add another ChatGPT account");
            JsonArray *accounts = setting_array(p->ai_state, "accounts");
            for (guint i=0; accounts && i<json_array_get_length(accounts); i++) {
                JsonObject *account = json_array_get_object_element(accounts, i);
                s_gtk_combo_box_text_append(p->ai_account, setting_string(account, "id", ""), setting_string(account, "label", "ChatGPT account"));
            }
            s_gtk_combo_box_set_active_id(p->ai_account, setting_string(profile, "account_id", ""));
            void *button = settings_button(ui, p->settings_page, "Continue with ChatGPT", G_CALLBACK(settings_connect)); p->gtk_widget_set_sensitive(button, !busy);
            char logo_path[1200]; snprintf(logo_path, sizeof(logo_path), "%s/chatgpt-logo-white.svg", p->icon_dir);
            s_gtk_button_set_image(button, s_gtk_image_new_from_file(logo_path)); s_gtk_button_set_always_show_image(button, TRUE);
            void *css = s_gtk_css_provider_new();
            s_gtk_css_provider_load_from_data(css, "button { background-image: none; background-color: #000; color: #fff; border-color: #000; }", -1, NULL);
            s_gtk_style_context_add_provider(s_gtk_widget_get_style_context(button), css, 600); g_object_unref(css);
            settings_button(ui, p->settings_page, "Manage usage ↗", G_CALLBACK(settings_usage));
            settings_label(p, p->settings_page, connected ? setting_string(profile, "account_label", "Using ChatGPT plan") : "Use an eligible personal ChatGPT plan (Plus / Pro). Your browser opens for secure sign-in.");
            if (connected) settings_label(p, p->settings_page, "Using ChatGPT plan");
        } else {
            settings_button(ui, p->settings_page, "Get an API key ↗", G_CALLBACK(settings_get_key));
            p->ai_key = s_gtk_entry_new(); s_gtk_entry_set_visibility(p->ai_key, FALSE); s_gtk_entry_set_max_length(p->ai_key, 4096);
            s_gtk_entry_set_placeholder_text(p->ai_key, connected ? "Enter a replacement API key" : "Paste your API key");
            s_gtk_box_pack_start(p->settings_page, p->ai_key, FALSE, FALSE, 2);
            void *reveal = s_gtk_check_button_new_with_label("Show key"); s_gtk_box_pack_start(p->settings_page, reveal, FALSE, FALSE, 2);
            g_signal_connect_data(reveal, "toggled", G_CALLBACK(settings_reveal), ui, NULL, 0);
            void *button = settings_button(ui, p->settings_page, connected ? "Verify replacement" : "Verify & connect", G_CALLBACK(settings_connect)); p->gtk_widget_set_sensitive(button, !busy);
            settings_label(p, p->settings_page, connected ? "Connected · Key stored in the system keyring. An invalid replacement keeps the current connection." : "Your key is masked and stored in the system credential manager.");
            if (!strcmp(p->ai_provider, "opencode-go")) settings_label(p, p->settings_page, "Go is intended for coding agents. Confirm provider support before using it for call recaps. Ascolto identifies itself as a call recap app.");
        }
        void *disconnect = settings_button(ui, p->settings_page, "Disconnect (keeps transcripts and audio)", G_CALLBACK(settings_disconnect)); p->gtk_widget_set_sensitive(disconnect, connected && !busy);
        p->ai_model = settings_combo(p, p->settings_page, "Recap model");
        s_gtk_combo_box_text_append(p->ai_model, "", connected ? "Choose a model" : "Connect a provider to load models");
        JsonArray *models = setting_array(profile, "models");
        for (guint i=0; models && i<json_array_get_length(models); i++) {
            JsonObject *model = json_array_get_object_element(models, i); s_gtk_combo_box_text_append(p->ai_model, setting_string(model, "id", ""), setting_string(model, "name", "Model"));
        }
        s_gtk_combo_box_set_active_id(p->ai_model, setting_string(profile, "model", "")); p->gtk_widget_set_sensitive(p->ai_model, connected && !busy);
        void *refresh = settings_button(ui, p->settings_page, "Refresh models", G_CALLBACK(settings_refresh_models)); p->gtk_widget_set_sensitive(refresh, connected && !busy);
        p->ai_language = settings_combo(p, p->settings_page, "Recap language");
        const char *languages[] = {"Italian", "English", "German", "French", "Spanish", "Dutch", "Polish"};
        for (int i=0; i<7; i++) s_gtk_combo_box_text_append(p->ai_language, languages[i], languages[i]);
        s_gtk_combo_box_set_active_id(p->ai_language, setting_string(p->ai_state, "language", "Italian"));
        p->ai_use = settings_button(ui, p->settings_page, !strcmp(p->ai_provider,"opencode-go")?"Acknowledge Go policy and use for recaps":"Use for recaps", G_CALLBACK(settings_use));
        const char *chosen_model = s_gtk_combo_box_get_active_id(p->ai_model);
        p->gtk_widget_set_sensitive(p->ai_use, connected && !busy && chosen_model && *chosen_model);
        g_signal_connect_data(p->ai_model, "changed", G_CALLBACK(settings_model_changed), ui, NULL, 0);
        const char *selected = setting_string(p->ai_state, "selected_provider", "");
        gchar *current = g_strdup_printf("Current recap provider: %s", *selected ? selected : "None · off by default"); settings_label(p, p->settings_page, current); g_free(current);
        p->ai_auto = s_gtk_check_button_new_with_label("Generate a recap after each session"); s_gtk_toggle_button_set_active(p->ai_auto, setting_bool(p->ai_state, "automatic_recaps"));
        s_gtk_box_pack_start(p->settings_page, p->ai_auto, FALSE, FALSE, 2); p->gtk_widget_set_sensitive(p->ai_auto, *selected && !busy);
        g_signal_connect_data(p->ai_auto, "toggled", G_CALLBACK(settings_preferences), ui, NULL, 0);
        g_signal_connect_data(p->ai_language, "changed", G_CALLBACK(settings_preferences), ui, NULL, 0);
        settings_label(p, p->settings_page, p->ai_message && *p->ai_message ? p->ai_message : setting_string(p->ai_state, "secure_store_message", "Whistle transcription stays local. No audio is sent to this provider."));
        if (setting_bool(setting_object(setting_object(p->ai_state, "profiles"), "openai"), "welcome_pending")) settings_button(ui, p->settings_page, "Got it", G_CALLBACK(settings_acknowledge));
        if (busy) settings_button(ui, p->settings_page, "Cancel", G_CALLBACK(settings_cancel_clicked));
    }
    if(p->settings_category==4&&p->ai_output_dir)settings_button(ui,p->settings_page,"Open latest AI output…",G_CALLBACK(settings_open_output));
    if(p->settings_category!=1&&p->ai_message&&*p->ai_message)settings_label(p,p->settings_page,p->ai_message);
    if(p->settings_category!=1&&p->ai_worker)settings_button(ui,p->settings_page,"Cancel",G_CALLBACK(settings_cancel_clicked));
    p->settings_building = false;
    p->gtk_widget_show_all(p->settings_page);
}
static gboolean settings_delete(void *widget, void *event, gpointer data) {
    (void)event; settings_cancel(data); s_gtk_widget_hide(widget); return TRUE;
}
static void on_menu_settings(void *widget, gpointer data) {
    (void)widget; ascolto_tray_ui_t *ui = data; gtk_tray_priv_t *p = ui->gtk_impl;
    if (!p->settings_window) {
#define SL(name) s_##name = (settings_##name##_t)dlsym(p->gtk_lib, #name); if (!s_##name) return
        SL(gtk_window_new); SL(gtk_window_set_title); SL(gtk_window_set_default_size); SL(gtk_window_present);
        SL(gtk_box_new); SL(gtk_box_pack_start); SL(gtk_container_add); SL(gtk_container_set_border_width); SL(gtk_container_get_children);
        SL(gtk_label_new); SL(gtk_label_set_markup); SL(gtk_image_new_from_file); SL(gtk_button_set_image); SL(gtk_button_set_always_show_image); SL(gtk_css_provider_new); SL(gtk_css_provider_load_from_data); SL(gtk_widget_get_style_context); SL(gtk_style_context_add_provider); SL(gtk_label_set_xalign); SL(gtk_label_set_line_wrap); SL(gtk_label_set_text); SL(gtk_label_set_selectable);
        SL(gtk_button_new_with_label); SL(gtk_check_button_new_with_label); SL(gtk_toggle_button_set_active); SL(gtk_toggle_button_get_active);
        SL(gtk_entry_new); SL(gtk_entry_set_text); SL(gtk_entry_get_text); SL(gtk_entry_set_visibility); SL(gtk_entry_set_placeholder_text); SL(gtk_entry_set_max_length);
        SL(gtk_combo_box_text_new); SL(gtk_combo_box_text_append); SL(gtk_combo_box_set_active_id); SL(gtk_combo_box_get_active_id);
        SL(gtk_widget_set_size_request); SL(gtk_widget_hide); SL(gtk_scrolled_window_new); SL(gtk_scrolled_window_set_policy);
#undef SL
        p->settings_window = s_gtk_window_new(0); s_gtk_window_set_title(p->settings_window, "Ascolto Settings"); s_gtk_window_set_default_size(p->settings_window, 1000, 780);
        g_signal_connect_data(p->settings_window, "delete-event", G_CALLBACK(settings_delete), ui, NULL, 0);
        void *layout = s_gtk_box_new(0, 16); s_gtk_container_add(p->settings_window, layout); s_gtk_container_set_border_width(layout, 18);
        void *sidebar = s_gtk_box_new(1, 12); s_gtk_widget_set_size_request(sidebar, 180, -1); s_gtk_box_pack_start(layout, sidebar, FALSE, FALSE, 0);
        settings_label(p, sidebar, "Ascolto\nSettings");
        const char *categories[] = {"General", "AI providers", "Transcription", "Recording", "Storage"};
        for (int i=0; i<5; i++) { void *button = settings_button(ui, sidebar, categories[i], G_CALLBACK(settings_category)); g_object_set_data(button, "category", GINT_TO_POINTER(i)); }
        settings_label(p, sidebar, "Local transcription\nWhistle · on this device");
        void *scroll = s_gtk_scrolled_window_new(NULL, NULL); s_gtk_scrolled_window_set_policy(scroll, 2, 1); s_gtk_box_pack_start(layout, scroll, TRUE, TRUE, 0);
        p->settings_page = s_gtk_box_new(1, 8); s_gtk_container_set_border_width(p->settings_page, 12); s_gtk_container_add(scroll, p->settings_page);
        p->settings_category = 1; snprintf(p->ai_provider, sizeof(p->ai_provider), "opencode-zen");
    }
    settings_build(ui); p->gtk_widget_show_all(p->settings_window); s_gtk_window_present(p->settings_window);
    if (!p->ai_state && !p->ai_worker) settings_request(ui, settings_command("status", NULL));
}
static void settings_sync(ascolto_tray_ui_t *ui, const ascolto_tray_snapshot_t *snapshot) {
    gtk_tray_priv_t *p = ui->gtk_impl;
    if (!p->settings_window) return;
    p->settings_building = true;
    if (p->settings_auto) s_gtk_toggle_button_set_active(p->settings_auto, snapshot->menu_auto_mic_checked);
    if (p->settings_save) s_gtk_toggle_button_set_active(p->settings_save, snapshot->menu_save_audio_checked);
    if (p->settings_legacy) s_gtk_toggle_button_set_active(p->settings_legacy, snapshot->menu_cloud_recap_checked);
    if (p->settings_tts) s_gtk_toggle_button_set_active(p->settings_tts, snapshot->menu_recap_tts_checked);
    void *buttons[] = {p->settings_auto, p->settings_save, p->settings_legacy, p->settings_tts};
    for (int i=0; i<4; i++) if (buttons[i]) p->gtk_widget_set_sensitive(buttons[i], !snapshot->menu_stopping && !g_atomic_int_get(&p->action_pending));
    p->settings_building = false;
}
