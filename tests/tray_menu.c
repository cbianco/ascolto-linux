/* Exercise native GTK menu callbacks under Xvfb, including the background queue.
 * Including the adapter gives this test access to its dynamically loaded widgets. */
#include "../src/tray_ui.c"
#include <assert.h>

static void settle(ascolto_core_t *core) {
    gtk_tray_priv_t *p = core->tray.gtk_impl;
    for (int i = 0; i < 500; i++) {
        ascolto_tray_step_iteration(&core->tray);
        if (!g_atomic_int_get(&p->action_pending)) {
            ascolto_tray_step_iteration(&core->tray);
            return;
        }
        g_usleep(10000);
    }
    assert(!"menu action timed out");
}
static void activate_choice(ascolto_core_t *core, menu_action_t kind, const char *value) {
    gtk_tray_priv_t *p = core->tray.gtk_impl;
    for (guint i = 0; i < p->choices->len; i++) {
        void *item = g_ptr_array_index(p->choices, i);
        menu_request_t *r = g_object_get_data(item, "ascolto-choice");
        if (r->kind == kind && !strcmp(r->value, value)) {
            g_signal_emit_by_name(item, "activate");
            settle(core);
            assert(p->gtk_check_menu_item_get_active(item));
            return;
        }
    }
    assert(!"missing menu choice");
}
static void preview_settings(ascolto_core_t *core) {
    gtk_tray_priv_t *p = core->tray.gtk_impl;
    JsonParser *parser = json_parser_new();
    const char *fixture = "{\"automatic_recaps\":false,\"selected_provider\":\"openrouter\",\"language\":\"Italian\",\"runtime_ready\":true,\"profiles\":{\"openrouter\":{\"connected\":true,\"model\":\"fixture-model\",\"models\":[{\"id\":\"fixture-model\",\"name\":\"Available model from provider\"}]}}}";
    assert(json_parser_load_from_data(parser, fixture, -1, NULL));
    p->ai_state = json_object_ref(json_node_get_object(json_parser_get_root(parser))); g_object_unref(parser);
    on_menu_settings(NULL, &core->tray);
    assert(p->settings_window && !p->ai_worker);
    void *(*gtk_settings_get_default)(void) = dlsym(p->gtk_lib, "gtk_settings_get_default");
    int (*width)(void*) = dlsym(p->gtk_lib, "gtk_widget_get_allocated_width");
    int (*height)(void*) = dlsym(p->gtk_lib, "gtk_widget_get_allocated_height");
    void (*draw)(void*, void*) = dlsym(p->gtk_lib, "gtk_widget_draw");
    void *(*surface_create)(int,int,int) = dlsym(p->gtk_lib, "cairo_image_surface_create");
    void *(*cairo_create)(void*) = dlsym(p->gtk_lib, "cairo_create");
    int (*write_png)(void*,const char*) = dlsym(p->gtk_lib, "cairo_surface_write_to_png");
    void (*cairo_destroy)(void*) = dlsym(p->gtk_lib, "cairo_destroy");
    void (*surface_destroy)(void*) = dlsym(p->gtk_lib, "cairo_surface_destroy");
    assert(draw && surface_create && cairo_create && write_png && cairo_destroy && surface_destroy);
    char logo_path[1200];snprintf(logo_path,sizeof(logo_path),"%s/chatgpt-logo-white.svg",p->icon_dir);
    assert(!access(logo_path,R_OK));
    void *(*load_image)(const char*,GError**)=dlsym(p->gtk_lib,"gdk_pixbuf_new_from_file");assert(load_image);
    GError *image_error=NULL;void *logo=load_image(logo_path,&image_error);
    if(!logo)fprintf(stderr,"SVG loader missing for %s: %s\n",logo_path,image_error?image_error->message:"unknown error");
    assert(logo&&"Install the GdkPixbuf SVG loader (librsvg2-common on Debian/Ubuntu)");g_object_unref(logo);g_clear_error(&image_error);
    ascolto_mkdir_p("build/settings-previews");
    for (int theme=0; theme<2; theme++) {
        g_object_set(gtk_settings_get_default(), "gtk-theme-name", "Adwaita", "gtk-application-prefer-dark-theme", theme, NULL);
        for (int category=0; category<5; category++) {
            p->settings_category = category; snprintf(p->ai_provider, sizeof(p->ai_provider), "openrouter"); settings_build(&core->tray);
            for (int i=0; i<20; i++) { ascolto_tray_step_iteration(&core->tray); g_usleep(5000); }
            void *surface = surface_create(0, width(p->settings_window), height(p->settings_window));
            void *context = cairo_create(surface); draw(p->settings_window, context);
            char path[160]; snprintf(path, sizeof(path), "build/settings-previews/gtk-%s-%d.png", theme ? "dark" : "light", category);
            assert(!write_png(surface, path)); cairo_destroy(context); surface_destroy(surface);
        }
    }
    /* Render all connection variants with fixture state; no keyring or network. */
    JsonObject *profiles = setting_object(p->ai_state, "profiles");
    JsonObject *profile = setting_object(profiles, "openrouter");
    const char *providers[] = {"opencode-zen", "opencode-go", "openai"};
    for (int i=0; i<3; i++) json_object_set_object_member(profiles, providers[i], json_object_ref(profile));
    p->settings_category = 1;
    for (int i=0; i<3; i++) {
        snprintf(p->ai_provider, sizeof(p->ai_provider), "%s", providers[i]); settings_build(&core->tray);
        for (int tick=0; tick<20; tick++) { ascolto_tray_step_iteration(&core->tray); g_usleep(5000); }
        void *surface = surface_create(0, width(p->settings_window), height(p->settings_window));
        void *context = cairo_create(surface); draw(p->settings_window, context);
        char path[160]; snprintf(path, sizeof(path), "build/settings-previews/gtk-dark-%s.png", providers[i]);
        assert(!write_png(surface, path)); cairo_destroy(context); surface_destroy(surface);
    }
    /* All native settings must remain usable alongside the serial menu queue. */
    p->settings_category = 2; settings_build(&core->tray);
    assert(p->settings_keywords && p->local_models_box);
    /* The cloud tab keeps a distinct model and consent surface. */
    p->settings_cloud_tab=true;settings_build(&core->tray);assert(p->stt_model&&p->stt_consent&&!p->local_models_box);
    p->settings_cloud_tab=false;settings_build(&core->tray);assert(p->settings_keywords&&p->local_models_box);
    s_gtk_entry_set_text(p->settings_keywords, "Fixture, glossary"); settings_keywords(NULL, &core->tray); settle(core);
    assert(!strcmp(core->config.keywords, "Fixture, glossary"));
    /* Diagnostics use fixture metadata only; no keyring or provider request. */
    on_menu_diagnostics(NULL, &core->tray);
    assert(p->diagnostics_window == p->settings_window && p->diagnostics_timer);
    if (p->diagnostics_worker) {
        g_object_set_data(G_OBJECT(p->diagnostics_worker), "diagnostics-ui", NULL);
        g_subprocess_force_exit(p->diagnostics_worker); g_clear_object(&p->diagnostics_worker);
    }
    g_source_remove(p->diagnostics_timer); p->diagnostics_timer = 0;
    parser = json_parser_new();
    const char *diagnostic_fixture = "{\"summary\":{\"total_calls\":2,\"needs_attention\":1,\"running\":0},\"calls\":[{\"id\":45,\"label\":\"Call 12:21\",\"status\":\"Needs attention\",\"outcome\":\"Recap publication blocked\",\"reason\":\"A citation did not match the transcript. Local artifacts are retained.\",\"needs_attention\":true,\"operations\":[{\"name\":\"Local transcript\",\"status\":\"Ready\"}],\"artifacts\":[],\"attempts\":[{\"id\":6,\"attempt\":1,\"task\":\"recap\",\"phase\":\"final\",\"status\":\"failed\",\"provider\":\"openrouter\",\"model\":\"fixture-model\",\"error_code\":\"invalid_evidence\",\"elapsed_ms\":33000,\"events\":[{\"status\":\"failed\",\"at_ms\":1792146442002}]}],\"safe_report\":\"Session: 45\\nError: invalid_evidence\"},{\"id\":46,\"label\":\"Call 13:05\",\"status\":\"Ready\",\"outcome\":\"Recap ready\",\"needs_attention\":false,\"attempts\":[],\"safe_report\":\"Session: 46\"}]}";
    assert(json_parser_load_from_data(parser, diagnostic_fixture, -1, NULL));
    if (p->diagnostics_state) json_object_unref(p->diagnostics_state);
    p->diagnostics_state = json_object_ref(json_node_get_object(json_parser_get_root(parser))); g_object_unref(parser);
    p->diagnostics_selected = 45; p->diagnostics_expanded = true; diagnostics_build(&core->tray);
    assert(diagnostics_number(diagnostics_selected(&core->tray), "id") == 45);
    GList *diagnostic_children = s_gtk_container_get_children(p->diagnostics_list);
    assert(g_list_length(diagnostic_children) == 3); g_list_free(diagnostic_children);
    p->diagnostics_attention = true; diagnostics_build(&core->tray);
    diagnostic_children = s_gtk_container_get_children(p->diagnostics_list);
    assert(g_list_length(diagnostic_children) == 2); g_list_free(diagnostic_children);
    diagnostics_preview(NULL, &core->tray); assert(p->diagnostics_preview);
    assert(p->diagnostics_window == p->settings_window);
    diagnostics_preview_close(NULL, &core->tray); assert(!p->diagnostics_preview);
    for (int theme = 0; theme < 2; theme++) {
        g_object_set(gtk_settings_get_default(), "gtk-theme-name", "Adwaita", "gtk-application-prefer-dark-theme", theme, NULL);
        for (int tick = 0; tick < 20; tick++) { ascolto_tray_step_iteration(&core->tray); g_usleep(5000); }
        void *surface = surface_create(0, width(p->diagnostics_window), height(p->diagnostics_window));
        void *context = cairo_create(surface); draw(p->diagnostics_window, context);
        char path[180]; snprintf(path, sizeof(path), "build/settings-previews/gtk-diagnostics-%s.png", theme ? "dark" : "light");
        assert(!write_png(surface, path)); cairo_destroy(context); surface_destroy(surface);
    }
    void *unified_window = p->settings_window;
    GSubprocess *pending_task = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, NULL, "/bin/sleep", "60", NULL);
    p->diagnostics_task = pending_task;
    on_menu_settings(NULL, &core->tray);
    assert(p->settings_window == unified_window && !p->diagnostics_visible && p->diagnostics_task == pending_task);
    on_menu_diagnostics(NULL, &core->tray);
    assert(p->settings_window == unified_window && p->diagnostics_visible && p->diagnostics_task == pending_task);
    assert(p->diagnostics_selected == 45 && p->diagnostics_attention && p->diagnostics_expanded);
    g_subprocess_force_exit(p->diagnostics_task); g_clear_object(&p->diagnostics_task);
    settings_delete(p->settings_window, NULL, &core->tray); assert(!p->diagnostics_visible);
    s_gtk_widget_hide(p->settings_window);
}

int main(void) {
    ascolto_config_t cfg;
    ascolto_config_init_defaults(&cfg);
    char dir[] = "/tmp/ascolto-tray-XXXXXX";
    assert(mkdtemp(dir));
    snprintf(cfg.config_path, sizeof(cfg.config_path), "%s/config.ini", dir);
    snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s/sessions", dir);
    snprintf(cfg.control_socket, sizeof(cfg.control_socket), "%s/control.sock", dir);
    snprintf(cfg.telemetry_log, sizeof(cfg.telemetry_log), "%s/events.jsonl", dir);
    snprintf(cfg.weights_path, sizeof(cfg.weights_path), "third_party/whistle/whistle.cact");
    cfg.headless = false;
    cfg.auto_mic_trigger = false;
    cfg.cloud_recap = false;
    cfg.save_audio = false;
    /* Adopt a real legacy-shaped archive, then start with a different future output directory. */
    char old_archive[512],old_call[512],old_md[512],old_jsonl[512];
    snprintf(old_archive,sizeof(old_archive),"%s/previous-archive",dir);
    snprintf(old_call,sizeof(old_call),"%s/call_2026-10-15_10-20-30_1",old_archive);assert(!ascolto_mkdir_p(old_call));
    snprintf(old_md,sizeof(old_md),"%s/transcript.md",old_call);snprintf(old_jsonl,sizeof(old_jsonl),"%s/transcript.jsonl",old_call);
    FILE *history=fopen(old_md,"w");assert(history);fputs("# Historical transcript\nSessione terminata (stop)\n",history);fclose(history);
    history=fopen(old_jsonl,"w");assert(history);fputs("{\"channel\":\"MIC\",\"speaker\":\"ME\",\"start_sec\":0.1,\"end_sec\":0.8,\"text\":\"Historical statement.\"}\n",history);fclose(history);
    history=fopen(cfg.config_path,"w");assert(history);fprintf(history,"auto_mic_trigger=false\noutput_dir=%s\n",old_archive);fclose(history);
    assert(!ascolto_storage_bootstrap(cfg.config_path,"linux"));
    cfg.headless=true;
    ascolto_core_t *core = calloc(1, sizeof(*core));
    assert(!ascolto_core_init(core, &cfg, true));
    assert(!strcmp(core->current_md_path,old_md)&&!strcmp(core->current_jsonl_path,old_jsonl));
    assert(!strcmp(core->current_session_dir,old_call));
    assert(core->state==ASCOLTO_STATE_IDLE&&!core->session_id&&!core->database_session&&!core->total_segments&&!core->segments_count);
    ascolto_core_destroy(core);free(core);cfg.headless=false;core=calloc(1,sizeof(*core));assert(!ascolto_core_init(core,&cfg,true));
    assert(!strcmp(core->current_md_path,old_md)&&!strcmp(core->current_jsonl_path,old_jsonl));
    ascolto_tray_snapshot_t restored;ascolto_tray_get_snapshot(&core->tray,&restored);
    assert(restored.recent_files_count&&!strcmp(restored.recent_files[0],old_md)&&!restored.menu_recording_enabled);
    assert(!core->session_id&&!core->total_segments&&!core->segments_count);
    gtk_tray_priv_t *p = core->tray.gtk_impl;
    assert(p && "GTK/AppIndicator must initialize on the test display");
    gboolean (*sensitive)(void*)=dlsym(p->gtk_lib,"gtk_widget_get_sensitive");assert(sensitive);
    assert(sensitive(p->item_open_latest)&&sensitive(p->item_recap_now));
    puts("Imported history/restart: GTK snapshot and archive actions PASS; live counters remain zero");
    assert(p && "GTK/AppIndicator must initialize on the test display");
    for (int i = 0; i < 200 && p->device_thread; i++) {
        ascolto_tray_step_iteration(&core->tray); g_usleep(10000);
    }
    assert(!p->device_thread && p->device_choices->len >= 2);
    preview_settings(core);
    /* A busy SQLite writer must not block a GTK action, copied snapshot or timer. */
    ascolto_db_t *writer=NULL; assert(!ascolto_storage_open(cfg.config_path,&writer));
    assert(!ascolto_db_begin(writer));
    gint64 setting_started=g_get_monotonic_time();queue_menu_action(&core->tray,MENU_LANGUAGE,"en");
    assert(g_get_monotonic_time()-setting_started<100000);
    g_usleep(30000);
    ascolto_tray_snapshot_t writer_snapshot;setting_started=g_get_monotonic_time();
    ascolto_tray_get_snapshot(&core->tray,&writer_snapshot);ascolto_tray_step_iteration(&core->tray);
    assert(g_get_monotonic_time()-setting_started<100000);
    assert(!ascolto_db_rollback(writer));ascolto_db_close(writer);settle(core);
    assert(!strcmp(core->config.language, "en"));
    activate_choice(core, MENU_SPEAKERS, "2");
    assert(core->config.diarization_sensitivity == ASCOLTO_SENSITIVITY_HIGH);
    gint64 started = g_get_monotonic_time();
    g_signal_emit_by_name(p->item_start_stop, "activate");
    assert(g_get_monotonic_time() - started < 100000);
    settle(core);
    assert(core->state == ASCOLTO_STATE_RECORDING_MANUAL);
    assert(!strcmp(core->tray.snapshot.icon_name, "ascolto-recording-symbolic"));
    assert(!core->tray.snapshot.label[0]);
    g_signal_emit_by_name(p->item_pause_resume, "activate"); settle(core);
    assert(core->state == ASCOLTO_STATE_PAUSED);
    g_signal_emit_by_name(p->item_pause_resume, "activate"); settle(core);
    assert(core->state == ASCOLTO_STATE_RECORDING_MANUAL);
    g_signal_emit_by_name(p->item_start_stop, "activate"); settle(core);
    assert(core->state == ASCOLTO_STATE_IDLE);
    /* Quit immediately after another start; teardown must join before final stop. */
    g_signal_emit_by_name(p->item_start_stop, "activate");
    ascolto_core_destroy(core);
    assert(core->state == ASCOLTO_STATE_IDLE);
    free(core);
    puts("GTK menu choices, session actions, compact icons and shutdown: PASS");
    return 0;
}
