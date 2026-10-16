/* Read-only metadata for a tmux tab's active pane. Included by the transfer
 * implementation, after mac_terminal_metadata. Never reads pane output or
 * sends input. The main-thread lookup is a cache read; subprocesses and kernel
 * inspection run on one background queue with bounded output and a deadline. */
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static double mac_tmux_metadata_time(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}

/* Spawn only a read-only client. -N forbids accidentally starting a server;
 * the client's stdin/stderr and unrelated application descriptors stay closed.
 * Its argv never passes through a shell. */
static NSData *mac_tmux_metadata_output(NSArray *arguments) {
    int descriptors[2];
    if (pipe(descriptors)) return nil;
    if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(descriptors[0], F_SETFL, O_NONBLOCK) < 0) {
        close(descriptors[0]); close(descriptors[1]); return nil;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions)) { close(descriptors[0]); close(descriptors[1]); return nil; }
    if (posix_spawnattr_init(&attributes)) {
        posix_spawn_file_actions_destroy(&actions); close(descriptors[0]); close(descriptors[1]); return nil;
    }
    int error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
    error |= posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    error |= posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
    error |= posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    char **argv = calloc(arguments.count + 1, sizeof(char *));
    if (!argv) error = ENOMEM;
    else for (NSUInteger i = 0; i < arguments.count; i++) argv[i] = (char *)[arguments[i] UTF8String];
    pid_t pid = -1;
    if (!error) error = posix_spawn(&pid, "/usr/bin/env", &actions, &attributes, argv, environ);
    free(argv);
    posix_spawnattr_destroy(&attributes); posix_spawn_file_actions_destroy(&actions);
    close(descriptors[1]);
    if (error) { close(descriptors[0]); return nil; }

    NSMutableData *output = [NSMutableData data];
    double deadline = mac_tmux_metadata_time() + 500;
    int status = 0, reaped = 0, eof = 0, failed = 0;
    while (!failed && (!eof || !reaped)) {
        unsigned char buffer[2048];
        ssize_t length;
        while ((length = read(descriptors[0], buffer, sizeof(buffer))) > 0) {
            if (output.length + (NSUInteger)length > 16384) { failed = 1; break; }
            [output appendBytes:buffer length:(NSUInteger)length];
        }
        if (length == 0) eof = 1;
        else if (length < 0 && errno != EAGAIN && errno != EINTR) failed = 1;
        if (!reaped) {
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) reaped = 1;
            else if (result < 0 && errno != EINTR) { reaped = 1; failed = 1; }
        }
        if (failed || (eof && reaped)) break;
        double remaining = deadline - mac_tmux_metadata_time();
        if (remaining <= 0) { failed = 1; break; }
        struct pollfd descriptor = {descriptors[0], POLLIN, 0};
        /* A pipe already at EOF is always readable, so sleep without it while
         * waiting for a child that closed stdout but has not exited. */
        int result = poll(eof ? NULL : &descriptor, eof ? 0 : 1, (int)MIN(remaining + 1, 20));
        if (result < 0 && errno != EINTR) failed = 1;
    }
    close(descriptors[0]);
    if (!reaped) {
        /* pid belongs to this unreaped direct child; never signal the pane or
         * tmux server. SIGKILL also bounds a client stuck talking to a server. */
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    return !failed && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? output : nil;
}

static int mac_tmux_metadata_id(NSString *value, unichar prefix) {
    if (value.length < 2 || [value characterAtIndex:0] != prefix) return 0;
    for (NSUInteger i = 1; i < value.length; i++) {
        unichar c = [value characterAtIndex:i];
        if (c < '0' || c > '9') return 0;
    }
    return 1;
}

static NSDictionary *mac_tmux_metadata_query(NSString *session, NSString *socket) {
    NSString *target = [NSString stringWithFormat:mac_tmux_metadata_id(session, '$') ? @"%@:" : @"=%@:", session];
    /* Unlike tabs/newlines, the separator is not commonly present in a path.
     * If it does occur in source metadata, reject the ambiguous result. */
    NSString *format = @"#{session_id}\036#{session_name}\036#{pane_id}\036#{pane_tty}\036#{pane_current_path}\036#{pane_current_command}\036#{pane_pid}";
    NSMutableArray *arguments = [NSMutableArray arrayWithObjects:@"env", @"-u", @"TMUX", @"-u", @"TMUX_PANE", @"tmux", @"-N", nil];
    if (socket.length) [arguments addObjectsFromArray:@[@"-S", socket]];
    [arguments addObjectsFromArray:@[@"display-message", @"-p", @"-t", target, @"-F", format]];
    NSData *output = mac_tmux_metadata_output(arguments);
    NSString *text = output ? [[[NSString alloc] initWithData:output encoding:NSUTF8StringEncoding] autorelease] : nil;
    if (![text hasSuffix:@"\n"]) return nil;
    NSArray *fields = [[text substringToIndex:text.length-1] componentsSeparatedByString:@"\036"];
    if (fields.count != 7 || !mac_tmux_metadata_id(fields[0], '$') || !mac_tmux_metadata_id(fields[2], '%')) return nil;
    if (mac_tmux_metadata_id(session, '$') ? ![fields[0] isEqual:session] : ![fields[1] isEqual:session]) return nil;
    NSString *tty = fields[3];
    NSDictionary *native = mac_terminal_metadata(tty);
    NSString *cwd = [native[@"cwd"] length] ? native[@"cwd"] : fields[4];
    NSString *program = native[@"program"], *shell = native[@"shell"] ?: @"";
    if (![program isKindOfClass:[NSString class]]) {
        program = fields[5];
        if ([@[@"sh", @"zsh", @"bash", @"fish", @"dash", @"ksh", @"csh", @"tcsh"] containsObject:program]) {
            shell = program; program = @"";
        }
    }
    int pid = [native[@"pid"] intValue];
    if (pid <= 1) pid = [fields[6] intValue];
    return @{@"sessionId":fields[0], @"sessionName":fields[1], @"paneId":fields[2], @"tty":tty,
             @"cwd":cwd, @"program":program, @"shell":shell, @"pid":@(pid)};
}

/* Main thread only. Cache keys include the socket as well as session identity:
 * separate servers both commonly have a $0. Repeated tabs coalesce one query,
 * and a failed refresh clears stale metadata instead of naming the old pane. */
static NSMutableDictionary *g_mac_tmux_metadata_cache;
static dispatch_queue_t g_mac_tmux_metadata_queue;
static NSDictionary *mac_tmux_metadata(NSString *session, NSString *socket) {
    if (![NSThread isMainThread] || ![session isKindOfClass:[NSString class]] || !session.length || session.length > 512 ||
        (socket && (![socket isKindOfClass:[NSString class]] || socket.length > 4096))) return nil;
    const char *sessionBytes = session.UTF8String, *socketBytes = socket.UTF8String;
    if (!sessionBytes || strlen(sessionBytes) != [session lengthOfBytesUsingEncoding:NSUTF8StringEncoding] ||
        (socket && (!socketBytes || strlen(socketBytes) != [socket lengthOfBytesUsingEncoding:NSUTF8StringEncoding]))) return nil;
    if (!g_mac_tmux_metadata_cache) g_mac_tmux_metadata_cache = [[NSMutableDictionary alloc] init];
    if (!g_mac_tmux_metadata_queue) g_mac_tmux_metadata_queue = dispatch_queue_create("hardcore.tmux.metadata", DISPATCH_QUEUE_SERIAL);
    NSArray *key = @[[[session copy] autorelease], socket.length ? [[socket copy] autorelease] : @""];
    NSMutableDictionary *entry = g_mac_tmux_metadata_cache[key];
    double now = mac_tmux_metadata_time();
    if (!entry) {
        if (g_mac_tmux_metadata_cache.count >= 64) {
            id oldest = nil; double oldestTime = DBL_MAX;
            for (id candidate in g_mac_tmux_metadata_cache) {
                NSDictionary *value = g_mac_tmux_metadata_cache[candidate];
                if (![value[@"busy"] boolValue] && [value[@"used"] doubleValue] < oldestTime) {
                    oldest = candidate; oldestTime = [value[@"used"] doubleValue];
                }
            }
            if (!oldest) return nil;
            [g_mac_tmux_metadata_cache removeObjectForKey:oldest];
        }
        entry = [NSMutableDictionary dictionary]; g_mac_tmux_metadata_cache[key] = entry;
    }
    entry[@"used"] = @(now);
    if (![entry[@"busy"] boolValue] && now >= [entry[@"next"] doubleValue]) {
        entry[@"busy"] = @YES;
        dispatch_async(g_mac_tmux_metadata_queue, ^{
            @autoreleasepool {
                NSDictionary *snapshot = mac_tmux_metadata_query(key[0], [key[1] length] ? key[1] : nil);
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (g_mac_tmux_metadata_cache[key] != entry) return;
                    if (snapshot) entry[@"snapshot"] = snapshot;
                    else [entry removeObjectForKey:@"snapshot"];
                    entry[@"busy"] = @NO;
                    entry[@"next"] = @(mac_tmux_metadata_time() + 1000);
                });
            }
        });
    }
    return entry[@"snapshot"];
}
