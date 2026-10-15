#import <Foundation/Foundation.h>
#include "ascolto/macos_control.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/time.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

@interface AscoltoControlServer : NSObject
@property(nonatomic, assign) ascolto_live_app_t *app;
@property(nonatomic, strong) dispatch_queue_t actions;
@property(nonatomic, strong) dispatch_group_t connections;
@property(nonatomic, assign) int listener;
@property(nonatomic, assign) pthread_t thread;
@property(atomic, assign) BOOL running;
@property(nonatomic, copy) NSString *path;
@end
@implementation AscoltoControlServer
@end

static NSString *safe_string(const char *value) { return [NSString stringWithUTF8String:value] ?: @""; }

static NSDictionary *status(ascolto_live_app_t *app) {
    ascolto_live_snapshot_t snapshot;
    ascolto_live_app_read_snapshot(app, &snapshot);
    return @{@"ok": @YES, @"active": @YES, @"auto_mic_checked": @(snapshot.config.auto_trigger_mic),
        @"save_audio_checked": @(snapshot.config.save_audio), @"cloud_recap_checked": @(snapshot.config.cloud_recap),
        @"recap_tts_checked": @(snapshot.config.recap_tts), @"state": safe_string(ascolto_app_state_label(snapshot.state)),
        @"recording": @(snapshot.state == ASCOLTO_STATE_RECORDING),
        @"paused": @(snapshot.config.paused), @"auto_mic_trigger": @(snapshot.config.auto_trigger_mic),
        @"save_audio": @(snapshot.config.save_audio), @"cloud_recap": @(snapshot.config.cloud_recap),
        @"recap_tts": @(snapshot.config.recap_tts), @"language": safe_string(snapshot.config.language),
        @"utterances": @(snapshot.utterances), @"remote_speakers": @(snapshot.remote_speakers),
        @"mic_samples": @(snapshot.mic_samples), @"transcript": safe_string(snapshot.current_md_path)};
}

static NSDictionary *execute(AscoltoControlServer *server, NSString *command, NSString *argument) {
    ascolto_live_app_t *app = server.app;
    ascolto_live_snapshot_t snapshot;
    ascolto_live_app_read_snapshot(app, &snapshot);
    int result = 0;
    if ([command isEqualToString:@"START"]) result = ascolto_live_app_start_session(app, true);
    else if ([command isEqualToString:@"STOP"]) result = ascolto_live_app_stop_session(app);
    else if ([command isEqualToString:@"PAUSE"]) ascolto_live_app_set_paused(app, true);
    else if ([command isEqualToString:@"RESUME"]) ascolto_live_app_set_paused(app, false);
    else if ([command isEqualToString:@"SET_LANG"]) {
        if (![@[@"auto", @"it", @"en", @"es", @"fr", @"de", @"nl", @"pl"] containsObject:argument]) result = -1;
        else ascolto_live_app_set_language(app, argument.UTF8String);
    } else if ([@[@"SET_AUTO_MIC", @"SET_SAVE_AUDIO", @"SET_CLOUD_RECAP", @"SET_RECAP_TTS"] containsObject:command]) {
        BOOL enabled = [@[@"1", @"on", @"true"] containsObject:argument];
        if (!enabled && ![@[@"0", @"off", @"false"] containsObject:argument]) result = -1;
        else if ([command isEqualToString:@"SET_AUTO_MIC"]) ascolto_live_app_set_auto_trigger(app, enabled);
        else if ([command isEqualToString:@"SET_SAVE_AUDIO"]) ascolto_live_app_set_save_audio(app, enabled);
        else if ([command isEqualToString:@"SET_CLOUD_RECAP"]) ascolto_live_app_set_cloud_recap(app, enabled);
        else ascolto_live_app_set_recap_tts(app, enabled);
    } else if ([@[@"RECAP", @"MAKE_MP3", @"PREPARE_RECAP"] containsObject:command]) {
        if (snapshot.state == ASCOLTO_STATE_RECORDING || snapshot.state == ASCOLTO_STATE_STOPPING) result = -2;
        else result = ascolto_recap_start(&snapshot.config,
            argument.length ? argument.UTF8String : snapshot.current_md_path,
            [command isEqualToString:@"MAKE_MP3"], [command isEqualToString:@"PREPARE_RECAP"]);
    } else return @{@"ok": @NO, @"error": @"Unsupported command on macOS"};
    return result ? @{@"ok": @NO, @"error": @"Command failed; check arguments, session state, and recap setup", @"code": @(result)} : status(app);
}

static void *serve(void *argument) {
    @autoreleasepool {
        AscoltoControlServer *server = (__bridge AscoltoControlServer *)argument;
        while (server.running) {
            struct pollfd poller = {.fd = server.listener, .events = POLLIN};
            if (poll(&poller, 1, 100) <= 0) continue;
            int client = accept(server.listener, NULL, NULL);
            if (client < 0) continue;
            fcntl(client, F_SETFD, FD_CLOEXEC);
            dispatch_group_async(server.connections, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
              @autoreleasepool {
                int one = 1;
                setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
                struct timeval timeout = {.tv_sec = 1};
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                char input[2048];
                size_t length = 0;
                CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 1;
                while (server.running && length < sizeof(input) && CFAbsoluteTimeGetCurrent() < deadline) {
                    ssize_t received = recv(client, input + length, sizeof(input) - length, 0);
                    if (received <= 0) break;
                    length += (size_t)received;
                    if (memchr(input, '\n', length)) break;
                }
                NSString *line = [[NSString alloc] initWithBytes:input length:length encoding:NSUTF8StringEncoding];
                line = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
                NSRange space = [line rangeOfString:@" "];
                NSString *command = (space.location != NSNotFound ? [line substringToIndex:space.location] : line).uppercaseString;
                NSString *value = space.location != NSNotFound ? [[line substringFromIndex:space.location + 1]
                    stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] : @"";
                __block NSDictionary *response = @{@"ok": @NO, @"error": @"Invalid or incomplete command"};
                if (line.length && length < sizeof(input) && memchr(input, '\n', length)) {
                    if ([command isEqualToString:@"STATUS"] || [command isEqualToString:@"GET_TRAY_STATE"]) response = status(server.app);
                    else if (server.running) dispatch_sync(server.actions, ^{
                        if (server.running) response = execute(server, command, value);
                    });
                }
                NSMutableData *data = [[NSJSONSerialization dataWithJSONObject:response options:0 error:NULL] mutableCopy];
                [data appendBytes:"\n" length:1];
                const char *bytes = data.bytes;
                size_t offset = 0;
                while (offset < data.length) {
                    ssize_t sent = send(client, bytes + offset, data.length - offset, 0);
                    if (sent <= 0) break;
                    offset += (size_t)sent;
                }
                close(client);
              }
            });
        }
    }
    return NULL;
}

ascolto_control_t *ascolto_control_start(ascolto_live_app_t *app, dispatch_queue_t actions) {
    /* A disconnected local client is an I/O error, never an app termination. */
    signal(SIGPIPE, SIG_IGN);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(app->config.control_socket) >= sizeof(address.sun_path)) return NULL;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", app->config.control_socket);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) return NULL;
    struct stat info;
    if (!lstat(address.sun_path, &info)) {
        if (!S_ISSOCK(info.st_mode) || info.st_uid != getuid()) { close(listener); return NULL; }
        if (!connect(listener, (struct sockaddr *)&address, sizeof(address)) || errno != ECONNREFUSED) {
            close(listener); return NULL;
        }
        unlink(address.sun_path);
    }
    if (bind(listener, (struct sockaddr *)&address, sizeof(address))) { close(listener); return NULL; }
    if (chmod(address.sun_path, 0600) || listen(listener, 4)) { close(listener); unlink(address.sun_path); return NULL; }
    fcntl(listener, F_SETFD, FD_CLOEXEC);
    AscoltoControlServer *server = [AscoltoControlServer new];
    server.app = app; server.actions = actions; server.connections = dispatch_group_create(); server.listener = listener;
    server.path = safe_string(address.sun_path); server.running = YES;
    pthread_t thread;
    if (pthread_create(&thread, NULL, serve, (__bridge void *)server)) { close(listener); unlink(address.sun_path); return NULL; }
    server.thread = thread;
    return (ascolto_control_t *)CFBridgingRetain(server);
}

void ascolto_control_stop(ascolto_control_t *control) {
    if (!control) return;
    AscoltoControlServer *server = CFBridgingRelease(control);
    server.running = NO;
    shutdown(server.listener, SHUT_RDWR);
    pthread_join(server.thread, NULL);
    dispatch_group_wait(server.connections, DISPATCH_TIME_FOREVER);
    close(server.listener);
    unlink(server.path.fileSystemRepresentation);
}
