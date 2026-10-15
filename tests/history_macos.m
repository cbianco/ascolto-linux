#include "ascolto.h"
#include "ascolto/json.h"
#import <Cocoa/Cocoa.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Exercise the production menu builder without starting a status item, capture
 * or control server. */
@interface AscoltoMenuController : NSObject
@property(nonatomic, assign) ascolto_live_app_t *app;
@property(nonatomic, strong) NSMenu *statusMenu;
- (void)rebuildMenu;
@end

static NSMenuItem *find_action(NSMenu *menu, SEL action) {
  for (NSMenuItem *item in menu.itemArray) {
    if (item.action == action)
      return item;
    NSMenuItem *nested = find_action(item.submenu, action);
    if (nested)
      return nested;
  }
  return nil;
}

int main(void) {
  @autoreleasepool {
    char directory[] = "/tmp/ascolto-history-macos-XXXXXX";
    assert(mkdtemp(directory));
    char config[512], md[512], jsonl[512], future_output[512];
    snprintf(config, sizeof(config), "%s/config.json", directory);
    snprintf(md, sizeof(md), "%s/2026-10-15_10-20-30-123456_call.md",
             directory);
    snprintf(jsonl, sizeof(jsonl), "%s/2026-10-15_10-20-30-123456_call.jsonl",
             directory);
    snprintf(future_output, sizeof(future_output), "%s/new-output", directory);
    FILE *file = fopen(config, "w");
    assert(file);
    fprintf(file,
            "{\"auto_trigger_mic\":false,\"save_audio\":false,\"output_dir\":");
    ascolto_write_json_string(file, directory);
    fprintf(file, "}");
    fclose(file);
    file = fopen(md, "w");
    assert(file);
    fputs("# Original historical transcript\n", file);
    fclose(file);
    file = fopen(jsonl, "w");
    assert(file);
    fputs("{\"event\":\"session_start\",\"manual\":true}\n{\"event\":"
          "\"utterance\",\"channel\":\"MIC\",\"speaker\":\"ME\",\"start_s\":0."
          "1,\"end_s\":0.8,\"text\":\"Historical "
          "statement.\"}\n{\"event\":\"session_end\",\"duration_s\":1.0}\n",
          file);
    fclose(file);
    char canonical_md[4096], canonical_jsonl[4096];
    assert(realpath(md, canonical_md) && realpath(jsonl, canonical_jsonl));
    for (int launch = 0; launch < 2; launch++) {
      ascolto_live_app_t app;
      assert(!ascolto_live_app_init(&app, NULL, config));
      ascolto_live_snapshot_t snapshot;
      ascolto_live_app_read_snapshot(&app, &snapshot);
      assert(!strcmp(snapshot.current_md_path, canonical_md));
      assert(!strcmp(app.files.jsonl_path, canonical_jsonl));
      assert(snapshot.state == ASCOLTO_STATE_IDLE && !snapshot.utterances &&
             !snapshot.remote_speakers && !snapshot.mic_samples &&
             !snapshot.session_start_s);
      assert(!app.files.database_session && !app.session_generation);
      [NSApplication sharedApplication];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
      AscoltoMenuController *controller = [[AscoltoMenuController alloc] init];
      controller.app = &app;
      controller.statusMenu =
          [[NSMenu alloc] initWithTitle:@"History regression"];
      controller.statusMenu.autoenablesItems = NO;
      [controller rebuildMenu];
      NSMenuItem *open =
          find_action(controller.statusMenu, @selector(onOpenLastTranscript:));
      NSMenuItem *recap =
          find_action(controller.statusMenu, @selector(onRunRecap:));
      assert(open && open.enabled && recap && recap.enabled);
      controller.app = NULL;
      /* A changed future output directory must not hide an existing archive
       * reference. */
      if (!launch) {
        snprintf(app.config.output_dir, sizeof(app.config.output_dir), "%s",
                 future_output);
        assert(!ascolto_config_save(&app.config, NULL));
      }
      ascolto_live_app_free(&app);
    }
    puts("Imported history/restart: AppKit snapshot and archive actions PASS; "
         "live counters remain zero");
  }
  return 0;
}
