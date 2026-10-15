#import <Foundation/Foundation.h>
#include "ascolto/macos_control.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <math.h>

/* No microphone is opened by this test. */
void ascolto_macos_capture_finish_session(ascolto_macos_capture_t *capture, uint64_t generation) {
    (void)capture; (void)generation;
}

static NSDictionary *query(const char *path, const char *command) {
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    struct timeval timeout = {.tv_sec = 2};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(client, (struct sockaddr *)&address, sizeof(address))) { close(client); return nil; }
    write(client, command, strlen(command));
    NSMutableData *data = [NSMutableData data];
    char bytes[2048];
    ssize_t count;
    while ((count = read(client, bytes, sizeof(bytes))) > 0) [data appendBytes:bytes length:(NSUInteger)count];
    close(client);
    return [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
}

int main(void) {
    @autoreleasepool {
        char directory[] = "/tmp/ascolto-control-XXXXXX";
        if (!mkdtemp(directory)) return 1;
        ascolto_config_t config;
        ascolto_config_init_defaults(&config);
        config.auto_trigger_mic = false;
        config.save_audio = false;
        snprintf(config.output_dir, sizeof(config.output_dir), "%s", directory);
        snprintf(config.config_path, sizeof(config.config_path), "%s/config.json", directory);
        snprintf(config.keywords_path, sizeof(config.keywords_path), "%s/keywords.txt", directory);
        snprintf(config.control_socket, sizeof(config.control_socket), "%s/control.sock", directory);
        snprintf(config.recap_python, sizeof(config.recap_python), "%s/quoted \"python", directory);
        ascolto_config_save(&config, NULL);
        ascolto_live_app_t *app = calloc(1, sizeof(*app));
        if (!app || ascolto_live_app_init(app, NULL, config.config_path)) return 1;
        int failures = strcmp(app->config.recap_python, config.recap_python) != 0;
        dispatch_queue_t actions = dispatch_queue_create("ascolto.test.actions", DISPATCH_QUEUE_SERIAL);
        ascolto_control_t *control = ascolto_control_start(app, actions);
        if (!control) return 1;
        struct stat socket_info;
        stat(config.control_socket, &socket_info);
        if ((socket_info.st_mode & 0777) != 0600) failures++;
        if (ascolto_control_start(app, actions)) failures++;
        pthread_mutex_lock(&app->lock);
        __block NSDictionary *mutation;
        dispatch_group_t clients = dispatch_group_create();
        dispatch_group_async(clients, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            mutation = query(config.control_socket, "SET_LANG en\n");
        });
        /* A mutation can be blocked by processing; a second connection reads the snapshot. */
        CFAbsoluteTime started = CFAbsoluteTimeGetCurrent();
        NSDictionary *snapshot = query(config.control_socket, "STATUS\n");
        double milliseconds = (CFAbsoluteTimeGetCurrent() - started) * 1000;
        if (![snapshot[@"ok"] boolValue] || milliseconds > 100) failures++;
        pthread_mutex_unlock(&app->lock);
        if (dispatch_group_wait(clients, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) failures++;
        if (![mutation[@"ok"] boolValue] || ![query(config.control_socket, "STATUS\n")[@"language"] isEqualToString:@"en"]) failures++;
        if ([query(config.control_socket, "SET_LANG invalid\n")[@"ok"] boolValue]) failures++;
        if (![query(config.control_socket, "START\n")[@"recording"] boolValue]) failures++;
        if (![query(config.control_socket, "STOP\n")[@"state"] isEqualToString:@"IDLE"]) failures++;
        ascolto_control_stop(control);
        if (!access(config.control_socket, F_OK)) failures++;
        FILE *file = fopen(config.control_socket, "w");
        fputs("keep", file); fclose(file);
        if (ascolto_control_start(app, actions)) failures++;
        if (access(config.control_socket, F_OK)) failures++;
        /* Finalize paths containing shell metacharacters, then launch a local fake
           interpreter that verifies the archive exists before any recap starts. */
        char mic_path[512], sys_path[512], archive_path[512], md_path[512], arguments_path[512];
        snprintf(mic_path, sizeof(mic_path), "%s/mic.f32", directory);
        snprintf(sys_path, sizeof(sys_path), "%s/sys.f32", directory);
        snprintf(archive_path, sizeof(archive_path), "%s/call 'quoted' $(unused).m4a", directory);
        snprintf(md_path, sizeof(md_path), "%s/call 'quoted' $(unused).md", directory);
        snprintf(arguments_path, sizeof(arguments_path), "%s/arguments.json", directory);
        FILE *mic = fopen(mic_path, "wb"), *sys = fopen(sys_path, "wb");
        for (int i = 0; i < 16000; i++) {
            float left = 0.2f * sinf(2 * 3.14159265f * 440 * i / 16000);
            float right = 0.2f * sinf(2 * 3.14159265f * 880 * i / 16000);
            fwrite(&left, sizeof(left), 1, mic); fwrite(&right, sizeof(right), 1, sys);
        }
        fclose(mic); fclose(sys);
        snprintf(config.recap_python, sizeof(config.recap_python), "%s/fake-python", directory);
        file = fopen(config.recap_python, "w");
        fputs("#!/usr/bin/python3\nimport json, os, sys\n"
              "archive = sys.argv[sys.argv.index('--archive') + 1]\n"
              "assert os.path.isfile(archive)\n"
              "with open(os.environ['ASCOLTO_TEST_RECAP_ARGS'], 'w') as output:\n"
              "    json.dump(sys.argv, output)\n", file);
        fclose(file); chmod(config.recap_python, 0700);
        setenv("ASCOLTO_TEST_RECAP_ARGS", arguments_path, 1);
        config.cloud_recap = true; config.recap_tts = true;
        if (ascolto_archive_finalize(&config, mic_path, sys_path, archive_path, md_path)) failures++;
        ascolto_archive_wait();
        for (int i = 0; i < 100 && access(arguments_path, R_OK); i++) usleep(20000);
        NSData *encoded = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:arguments_path]];
        NSArray *arguments = encoded ? [NSJSONSerialization JSONObjectWithData:encoded options:0 error:NULL] : nil;
        if (![arguments containsObject:[NSString stringWithUTF8String:archive_path]] || ![arguments containsObject:@"--tts"] ||
            access(archive_path, R_OK) || !access(mic_path, F_OK) || !access(sys_path, F_OK)) failures++;
        unsetenv("ASCOLTO_TEST_RECAP_ARGS");
        ascolto_live_app_free(app);
        free(app);
        printf("macOS CLI/snapshot/socket regressions: %d failures (STATUS %.2f ms)\n", failures, milliseconds);
        return failures ? 1 : 0;
    }
}
