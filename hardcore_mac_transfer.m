/* macOS live handoff and slurp. Included after the platform implementation.
 * Versioned binary plists carry terminal state; SCM_RIGHTS carries PTY masters.
 * Donors keep their descriptors and stop servicing the main queue until READY.
 * A receiver builds its window and first frame before READY, then waits for GO
 * before reading or writing a PTY. Failure before GO leaves the donor intact.
 * Only same-user peers in an owner-only runtime directory are accepted.
 */
#include <sys/socket.h>
#include <sys/un.h>
#include <spawn.h>
#include <mach-o/dyld.h>
#include <CommonCrypto/CommonDigest.h>

extern char **environ;
#define MAC_HANDOFF_VERSION 1
#define MAC_PACKET_MAX (256u * 1024u * 1024u)
static char g_mac_endpoint[104];
#include "hardcore_mac_terminal.m"
static NSString *g_mac_image, *g_mac_install_exe;
static NSMutableArray *g_mac_imports;
static NSDictionary *g_mac_placement;
static int g_mac_ready_fd = -1;

static int mac_io(int fd, void *data, size_t n, int sending) {
    char *p = data;
    while (n) {
        ssize_t k = sending ? write(fd, p, n) : read(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k; n -= (size_t)k;
    }
    return 1;
}

static void mac_socket_setup(int fd) {
    struct timeval timeout = {15, 0};
    int yes = 1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static int mac_peer_ok(int fd) {
    uid_t uid; gid_t gid;
    return getpeereid(fd, &uid, &gid) == 0 && uid == getuid();
}

static int mac_send(int fd, NSDictionary *value) {
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:value format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
    if (!data || data.length > MAC_PACKET_MAX) return 0;
    uint32_t n = htonl((uint32_t)data.length);
    return mac_io(fd, &n, sizeof(n), 1) && mac_io(fd, (void *)data.bytes, data.length, 1);
}

static NSDictionary *mac_recv(int fd) {
    uint32_t n;
    if (!mac_io(fd, &n, sizeof(n), 0)) return nil;
    n = ntohl(n);
    if (!n || n > MAC_PACKET_MAX) return nil;
    NSMutableData *d = [NSMutableData dataWithLength:n];
    if (!mac_io(fd, d.mutableBytes, n, 0)) return nil;
    id value = [NSPropertyListSerialization propertyListWithData:d options:NSPropertyListImmutable format:NULL error:nil];
    return [value isKindOfClass:[NSDictionary class]] ? value : nil;
}

static int mac_send_fd(int sock, int fd) {
    char byte = 'F', control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {&byte, 1};
    struct msghdr msg = {0};
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = control; msg.msg_controllen = sizeof(control);
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(fd));
    return sendmsg(sock, &msg, 0) == 1;
}

static int mac_recv_fd(int sock) {
    char byte, control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {&byte, 1};
    struct msghdr msg = {0};
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = control; msg.msg_controllen = sizeof(control);
    if (recvmsg(sock, &msg, 0) != 1) return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    int fd = -1;
    if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len == CMSG_LEN(sizeof(int)))
        memcpy(&fd, CMSG_DATA(c), sizeof(fd));
    if (fd >= 0 && (byte != 'F' || (msg.msg_flags & MSG_CTRUNC))) { close(fd); return -1; }
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

static NSString *mac_runtime(void) {
    const char *override = getenv("HARDCORE_RUNTIME_DIR");
    NSString *dir = override ? @(override) : [NSString stringWithFormat:@"/tmp/hardcore-%u", getuid()];
    struct stat st;
    if (mkdir(dir.fileSystemRepresentation, 0700) && errno != EEXIST) return nil;
    if (lstat(dir.fileSystemRepresentation, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) return nil;
    return dir;
}

static int mac_connect(NSString *path) {
    struct sockaddr_un a = {0};
    const char *p = path.fileSystemRepresentation;
    if (!p || strlen(p) >= sizeof(a.sun_path)) return -1;
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, p);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    mac_socket_setup(fd);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) || !mac_peer_ok(fd)) { close(fd); return -1; }
    return fd;
}

static NSString *mac_hash(NSString *path) {
    NSData *d = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
    if (!d || d.length > UINT32_MAX) return @"";
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(d.bytes, (CC_LONG)d.length, hash);
    NSMutableString *s = [NSMutableString string];
    for (unsigned i = 0; i < sizeof(hash); i++) [s appendFormat:@"%02x", hash[i]];
    return s;
}

/* Property names, not raw C structs: the receiving build may have a new layout. */
static NSDictionary *mac_tab_snapshot(Tab *tb) {
    Term *t = &tb->t;
    SBuf snap = {0};
    term_snapshot(t, &snap);
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    d[@"snapshot"] = [NSData dataWithBytes:snap.p length:snap.n]; free(snap.p);
#define NUM(field) d[@#field] = @(t->field)
    NUM(cols); NUM(rows); NUM(view); NUM(st); NUM(esc_which); NUM(np); NUM(priv); NUM(inter);
    NUM(utf8_need); NUM(utf8_acc); NUM(last_ch); NUM(m_sync); NUM(sync_since);
    NUM(ps_state); NUM(ps_pct); NUM(ps_tasks); NUM(progress); NUM(spin_ch); NUM(spin_at); NUM(spin_n);
#undef NUM
#define TEXT(field) d[@#field] = [NSData dataWithBytes:t->field length:sizeof(t->field)]
    TEXT(cwd); TEXT(claude); TEXT(claude_args); TEXT(cmd); TEXT(title); TEXT(ps_kind); TEXT(ps_msg);
    TEXT(ps_task_id); TEXT(ps_task_on);
#undef TEXT
    d[@"osc"] = [NSData dataWithBytes:t->osc length:(NSUInteger)t->osc_n];
    NSMutableArray *params = [NSMutableArray array], *marks = [NSMutableArray array];
    for (int i = 0; i < MAX_PARAMS; i++) [params addObject:@(t->params[i])];
    for (int i = 0; i < t->nmarks; i++) [marks addObject:@(t->hpushed - t->marks[i])];
    d[@"params"] = params; d[@"marks"] = marks;
    d[@"pid"] = @(tb->pid); d[@"activity"] = @(tb->activity); d[@"attention"] = @(tb->attention);
    d[@"dir"] = @(tb->start_dir);
    d[@"run"] = @(tb->run); d[@"hist"] = @(tb->hist);
    return d;
}

static Tab *mac_tab_restore(NSDictionary *d, int fd) {
    int cols = [d[@"cols"] intValue], rows = [d[@"rows"] intValue];
    NSData *snap = d[@"snapshot"], *osc = d[@"osc"];
    NSArray *params = d[@"params"], *marks = d[@"marks"];
    if (cols < 10 || cols > 1000 || rows < 3 || rows > 500 || ![snap isKindOfClass:[NSData class]] ||
        ![osc isKindOfClass:[NSData class]] || osc.length >= OSC_MAX ||
        ![params isKindOfClass:[NSArray class]] || params.count != MAX_PARAMS ||
        ![marks isKindOfClass:[NSArray class]] || marks.count > 512 || [d[@"pid"] intValue] <= 1 ||
        [d[@"st"] intValue] < S_GROUND || [d[@"st"] intValue] > S_STR_ESC ||
        [d[@"np"] intValue] < 0 || [d[@"np"] intValue] > MAX_PARAMS ||
        [d[@"utf8_need"] intValue] < 0 || [d[@"utf8_need"] intValue] > 3 || !isatty(fd)) return NULL;
    Tab *tb = calloc(1, sizeof(Tab));
    if (!tb) return NULL;
    tb->fd = -1;  /* snapshot replay must never send replies to the live PTY */
    if (!term_init(&tb->t, cols, rows)) { free(tb); return NULL; }
    Term *t = &tb->t;
    vt_feed(t, snap.bytes, (int)snap.length);
#define NUM(field) t->field = [d[@#field] longLongValue]
    NUM(view); NUM(st); NUM(esc_which); NUM(np); NUM(priv); NUM(inter);
    NUM(utf8_need); NUM(utf8_acc); NUM(last_ch); NUM(m_sync); NUM(sync_since);
    NUM(ps_state); NUM(ps_pct); NUM(ps_tasks); NUM(progress); NUM(spin_ch); NUM(spin_at); NUM(spin_n);
#undef NUM
#define TEXT(field) do { NSData *v = d[@#field]; if ([v isKindOfClass:[NSData class]] && v.length == sizeof(t->field)) memcpy(t->field, v.bytes, v.length); } while (0)
    TEXT(cwd); TEXT(claude); TEXT(claude_args); TEXT(cmd); TEXT(title); TEXT(ps_kind); TEXT(ps_msg);
    TEXT(ps_task_id); TEXT(ps_task_on);
#undef TEXT
    t->osc_n = (int)osc.length; memcpy(t->osc, osc.bytes, osc.length); t->osc[osc.length] = 0;
    for (int i = 0; i < MAX_PARAMS; i++) t->params[i] = [params[i] intValue];
    t->nmarks = (int)marks.count;
    for (int i = 0; i < t->nmarks; i++) t->marks[i] = t->hpushed - [marks[i] longLongValue];
    t->view = t->view < 0 ? 0 : t->view > t->hcount ? t->hcount : t->view;
    tb->id = g_next_id++; tb->pid = [d[@"pid"] intValue]; tb->fd = fd;
    tb->activity = [d[@"activity"] intValue]; tb->attention = [d[@"attention"] intValue];
    snprintf(tb->start_dir, sizeof(tb->start_dir), "%s", [d[@"dir"] UTF8String] ?: "");
    snprintf(tb->run, sizeof(tb->run), "%s", [d[@"run"] UTF8String] ?: "");
    snprintf(tb->hist, sizeof(tb->hist), "%s", [d[@"hist"] UTF8String] ?: "");
    return tb;
}

static NSDictionary *mac_info(void) {
    NSMutableArray *tabs = [NSMutableArray array];
    for (int i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        if (tb->fd >= 0 && !tb->exited) {
            wchar_t label[256]; tab_label(tb, label, 256);
            NSString *text = [[NSString alloc] initWithBytes:label length:wcslen(label) * sizeof(wchar_t)
                                                  encoding:NSUTF32LittleEndianStringEncoding];
            [tabs addObject:@{@"pid": @(tb->pid), @"cwd": @(tb->t.cwd), @"command": @(tb->t.cmd),
                             @"program": @(tb->program), @"source": @(tb->run), @"label": text ?: @""}];
            [text release];
        }
    }
    return @{@"protocol": @(MAC_HANDOFF_VERSION), @"pid": @(getpid()), @"image": g_mac_image ?: @"",
             @"exe": g_mac_install_exe ?: @(g_mac_exe_path), @"tabs": tabs};
}

static NSArray *mac_windows(void) {
    NSString *dir = mac_runtime();
    NSMutableArray *all = [NSMutableArray array];
    if (!dir) return all;
    for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]) {
        if (![name hasSuffix:@".sock"]) continue;
        NSString *path = [dir stringByAppendingPathComponent:name];
        int fd = mac_connect(path);
        if (fd < 0) continue;   /* stale socket after a crash; never trust the filename's PID */
        NSDictionary *info = mac_send(fd, @{@"op": @"list"}) ? mac_recv(fd) : nil;
        close(fd);
        if ([info[@"protocol"] intValue] != MAC_HANDOFF_VERSION || ![info[@"tabs"] isKindOfClass:[NSArray class]]) continue;
        NSMutableDictionary *entry = [[info mutableCopy] autorelease]; entry[@"socket"] = path;
        [all addObject:entry];
    }
    return all;
}

/* The descriptor-transfer protocol predates browser tabs. Refuse a mixed
   window before reading/exporting any PTY rather than retiring browser state.
   The exec-based reload path serializes all tab kinds and remains available. */
static NSString *mac_transfer_block_reason(void) {
    for (int i = 0; i < g_ntabs; i++)
        if (g_tabs[i]->kind == TK_BROWSER)
            return @"This window has browser tabs. Use hardcore reload to update it in place; cross-window transfer keeps this entire source window open.";
    return nil;
}

static int mac_export(int sock) {
    NSString *blocked = mac_transfer_block_reason();
    if (blocked) { mac_send(sock, @{@"error": blocked}); return 0; }
    NSMutableArray *tabs = [NSMutableArray array];
    int fds[MAX_TABS], active = 0;
    for (int i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        if (tb->fd < 0 || tb->exited) continue;
        if (mac_input_held(tb)) {
            mac_send(sock, @{@"error": @"A Claude draft is still being restored; retry after verification."}); return 0;
        }
        if (tb->wpending && __atomic_load_n(tb->wpending, __ATOMIC_ACQUIRE)) {
            mac_send(sock, @{@"error": @"A paste is still queued; retry when it finishes."}); return 0;
        }
        read_some(tb); tab_drain(tb);
        if (tb->fd < 0) continue;
        if (i == g_active) active = (int)tabs.count;
        fds[tabs.count] = tb->fd;
        [tabs addObject:mac_tab_snapshot(tb)];
    }
    if (!tabs.count) { mac_send(sock, @{@"error": @"No live tabs to transfer."}); return 0; }
    NSDictionary *d = @{@"protocol": @(MAC_HANDOFF_VERSION), @"tabs": tabs, @"active": @(active),
                         @"frame": NSStringFromRect([g_win frame])};
    if (!mac_send(sock, d)) return 0;
    for (NSUInteger i = 0; i < tabs.count; i++) if (!mac_send_fd(sock, fds[i])) return 0;
    NSDictionary *ready = mac_recv(sock);
    if (![ready[@"ready"] boolValue]) return 0;
    /* The receiver is ready but has not touched the PTYs. GO commits ownership.
       Until this succeeds the main-thread donor can simply resume its event loop. */
    return mac_send(sock, @{@"go": @YES});
}

static void mac_retire(void) {
    if (g_mac_endpoint[0]) unlink(g_mac_endpoint);
    if (g_spare && g_spare->pid > 0) kill(g_spare->pid, SIGHUP);
    /* Do not run normal window-close cleanup: the transferred PTYs stay alive. */
    _exit(0);
}

static int mac_spawn(NSString *exe, NSArray *args, int socket_fd, int detached, pid_t *pid) {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);
    /* A receiving window must outlive the launching terminal/agent process.
       Redirecting stdio alone leaves it in that process's session/group, where
       cleanup on a cooperative Claude handoff can terminate the receiver. */
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT | (detached ? POSIX_SPAWN_SETSID : 0));
    for (int i = 0; i < 3; i++) {
        if (detached) posix_spawn_file_actions_addopen(&actions, i, "/dev/null", i ? O_WRONLY : O_RDONLY, 0);
        else posix_spawn_file_actions_adddup2(&actions, i, i);
    }
    if (socket_fd >= 0) posix_spawn_file_actions_adddup2(&actions, socket_fd, 198);
    NSUInteger n = args.count;
    char **argv = calloc(n + 2, sizeof(char *));
    argv[0] = (char *)exe.fileSystemRepresentation;
    for (NSUInteger i = 0; i < n; i++) argv[i + 1] = (char *)[args[i] UTF8String];
    int rc = posix_spawn(pid, argv[0], &actions, &attr, argv, environ);
    free(argv); posix_spawnattr_destroy(&attr); posix_spawn_file_actions_destroy(&actions);
    return rc == 0;
}

static int mac_transfer_reload(NSString *exe) {
    if (mac_transfer_block_reason()) return 0;
    int pair[2]; pid_t child;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 0;
    mac_socket_setup(pair[0]); mac_socket_setup(pair[1]);
    int started = mac_spawn(exe, @[@"--adopt-fd", @"198"], pair[1], 0, &child);
    close(pair[1]);
    int ok = started && mac_export(pair[0]);
    close(pair[0]);
    if (started && !ok) { kill(child, SIGKILL); while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {} }
    return ok;
}

/* A receiver receipt is based on bytes that traversed the attached PTY, not
   merely a helper's claim that attach was attempted. Match the random request
   marker in terminal cells and wait for this tab's frame to finish rendering. */
static int mac_term_contains(Term *t, const char *needle) {
    size_t matched = 0, length = strlen(needle);
    if (!length) return 0;
    int first = t->hcount - t->view;
    for (int line = first; line < first + t->rows; line++) {
        const Cell *cells; int n, wrapped;
        term_line(t, line, &cells, &n, &wrapped);
        if (line >= t->hcount && line - t->hcount < t->rows && t->rlen[t->alt][line - t->hcount] < n)
            n = t->rlen[t->alt][line - t->hcount];
        if (n > t->cols) n = t->cols;
        for (int x = 0; x < n; x++) {
            if (cells[x].wflag == W_CONT || cells[x].wflag == W_PAD) continue;
            uint32_t ch = cells[x].ch;
            matched = ch == (unsigned char)needle[matched] ? matched + 1 :
                      ch == (unsigned char)needle[0] ? 1 : 0;
            if (matched == length) return 1;
        }
        if (!wrapped) matched = 0;
    }
    return 0;
}

static NSDictionary *mac_claude_receipt(NSDictionary *req) {
    NSString *request = req[@"requestId"], *tty = req[@"receiverTty"];
    int pid = [req[@"receiverPid"] intValue];
    int waiting_for_cursor = 0;
    if (![request isKindOfClass:[NSString class]] || request.length != 32 ||
        [request rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location != NSNotFound ||
        ![tty isKindOfClass:[NSString class]] || ![tty hasPrefix:@"/dev/"] || pid <= 1)
        return @{@"ok": @NO, @"error": @"Invalid receiver identity."};
    for (int i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        if (tb->pid != pid || tb->fd < 0 || tb->exited || strncmp(tb->run, "claude-wait:", 12)) continue;
        NSString *path = @(tb->run + 12);
        if (![[path lastPathComponent] isEqual:[request stringByAppendingString:@".json"]]) continue;
        char *slave = ptsname(tb->fd);
        if (!slave || ![tty isEqual:@(slave)]) continue;
        NSString *op = req[@"op"];
        if ([op isEqual:@"claude-guard"]) {
            if (!mac_input_held(tb) || (DWORD)(GetTickCount() - tb->claude_guard_at) >= 110000) continue;
            return @{@"ok": @YES, @"requestId": request, @"windowPid": @(getpid()),
                     @"receiverPid": @(pid), @"receiverTty": tty, @"inputHeld": @YES};
        }
        if ([op isEqual:@"claude-release"]) {
            tb->claude_released = 1;
            return @{@"ok": @YES, @"requestId": request, @"windowPid": @(getpid()),
                     @"receiverPid": @(pid), @"receiverTty": tty, @"released": @YES};
        }
        int cursor = [op isEqual:@"claude-cursor"];
        if (i != g_active || (cursor && !mac_input_held(tb)) ||
            (tb->claude_guard && !mac_input_held(tb))) continue;
        read_some(tb); tab_drain(tb);
        NSString *marker = [(cursor ? @"HC-CURSOR:" : @"HC-RECEIPT:") stringByAppendingString:request];
        if (tb->fd < 0 || tb->exited) continue;
        if (!mac_term_contains(&tb->t, marker.UTF8String)) { waiting_for_cursor = cursor; continue; }
        g_handoff_paint = 1; g_handoff_painted = 0;
        paint_view_now();
        g_handoff_paint = 0;
        if (!g_handoff_painted) { waiting_for_cursor = cursor; continue; }
        if (cursor) {
            if (!tb->claude_cursor_sent) {
                /* One cursor-motion event, never text or Return. The bound mod
                   consumes it and restores the recorded cursor position. */
                tab_write(tb, tb->t.m_appcursor ? "\x1bOD" : "\x1b[D", 3);
                tb->claude_cursor_sent = 1;
            }
            return @{@"ok": @YES, @"requestId": request, @"windowPid": @(getpid()),
                     @"receiverPid": @(pid), @"receiverTty": tty, @"cursorSent": @YES};
        }
        return @{@"ok": @YES, @"requestId": request, @"windowPid": @(getpid()),
                 @"receiverPid": @(pid), @"receiverTty": tty, @"rendered": @YES};
    }
    return @{@"ok": @NO, @"waitingForCursorMarker": @(waiting_for_cursor),
             @"error": @"The matching attached tab has not rendered its verification marker."};
}

#include "hardcore_mac_bridge_refresh.m"

static void mac_server(void) {
    NSString *dir = mac_runtime();
    if (!dir) { fprintf(stderr, "hardcore: live updates unavailable: unsafe runtime directory\n"); return; }
    NSString *path = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%d.sock", getpid()]];
    if (strlen(path.fileSystemRepresentation) >= sizeof(g_mac_endpoint)) return;
    strcpy(g_mac_endpoint, path.fileSystemRepresentation);
    struct sockaddr_un a = {0}; a.sun_family = AF_UNIX; strcpy(a.sun_path, g_mac_endpoint);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    fcntl(fd, F_SETFD, FD_CLOEXEC); fcntl(fd, F_SETFL, O_NONBLOCK);
    unlink(g_mac_endpoint);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || chmod(g_mac_endpoint, 0600) || listen(fd, 8)) { close(fd); return; }
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, fd, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(source, ^{
        int peer = accept(fd, NULL, NULL);
        if (peer < 0) return;
        mac_socket_setup(peer);
        if (!mac_peer_ok(peer)) { close(peer); return; }
        /* Read requests off the UI thread: an idle/malformed client cannot freeze a window. */
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            @autoreleasepool {
                unsigned char first = 0;
                ssize_t peek;
                do { peek = recv(peer, &first, 1, MSG_PEEK); } while (peek < 0 && errno == EINTR);
                /* Older exec-updater clients use one bounded text command.
                   Keep one listener for both protocols; two servers using the
                   same PID socket would silently replace each other. */
                if (peek == 1 && (first == 'i' || first == 'r' || first == 'b')) {
                    char command[32768] = {0};
                    size_t n = 0;
                    while (n < sizeof(command) - 1 && mac_io(peer, command + n, 1, 0))
                        if (command[n++] == '\n') break;
                    int info = !strcmp(command, "info\n"), reload = !strcmp(command, "reload\n");
                    int browser = n > 8 && command[n - 1] == '\n' && !strncmp(command, "browser ", 8);
                    NSData *browserData = browser ? [NSData dataWithBytes:command + 8 length:n - 9] : nil;
                    if (info || reload || browser) dispatch_sync(dispatch_get_main_queue(), ^{
                        if (info) {
                            NSData *json = [NSJSONSerialization dataWithJSONObject:@{@"exe": g_mac_install_exe ?: @(g_mac_exe_path), @"pid": @(getpid()), @"browser_protocol": @1} options:0 error:nil];
                            mac_io(peer, (void *)json.bytes, json.length, 1);
                        } else if (browser) {
                            id request = [NSJSONSerialization JSONObjectWithData:browserData options:0 error:nil];
                            NSDictionary *reply;
                            @try {
                                reply = [request isKindOfClass:NSDictionary.class] ? mac_browser_request(request) :
                                    @{@"ok": @NO, @"error": @"Invalid browser request."};
                            } @catch (NSException *exception) {
                                reply = @{@"ok": @NO, @"error": @"Invalid browser request."};
                            }
                            NSData *json = [NSJSONSerialization dataWithJSONObject:reply options:0 error:nil];
                            mac_io(peer, (void *)json.bytes, json.length, 1);
                        } else {
                            char why[256];
                            mac_reload(peer, why, sizeof(why));
                            dprintf(peer, "error %s\n", why); /* exec returns only on failure */
                        }
                    });
                    close(peer);
                    return;
                }
                NSDictionary *req = peek == 1 ? mac_recv(peer) : nil;
                if ([req[@"op"] isEqual:@"refresh-terminal-bridge"]) {
                    mac_send(peer, mac_bridge_refresh(req));
                } else if (req) dispatch_sync(dispatch_get_main_queue(), ^{
                    @try {
                        NSString *op = req[@"op"];
                        if ([op isEqual:@"list"]) mac_send(peer, mac_info());
                        else if ([op isEqual:@"claude-receipt"] || [op isEqual:@"claude-cursor"] || [op isEqual:@"claude-release"] || [op isEqual:@"claude-guard"]) mac_send(peer, mac_claude_receipt(req));
                        else if ([op isEqual:@"take"]) { if (mac_export(peer)) { close(peer); mac_retire(); } }
                        else if ([op isEqual:@"reload"]) {
                            NSString *exe = req[@"exe"];
                            int ok = [exe isKindOfClass:[NSString class]] && [exe isAbsolutePath] && mac_transfer_reload(exe);
                            mac_send(peer, ok ? @{@"ok": @YES} : @{@"error": mac_transfer_block_reason() ?: @"Replacement could not take over; this window is unchanged."});
                            if (ok) { close(peer); mac_retire(); }
                        } else mac_send(peer, @{@"error": @"Unknown command."});
                    } @catch (NSException *e) { mac_send(peer, @{@"error": @"Invalid request."}); }
                });
                close(peer);
            }
        });
    });
    dispatch_resume(source);
}

/* Staged tabs have no dispatch sources. On failure closing their duplicate
   master descriptors does not hang up the donor's still-open descriptors. */
static int mac_import(int sock) {
    NSDictionary *d = mac_recv(sock);
    NSArray *tabs = d[@"tabs"];
    if ([d[@"protocol"] intValue] != MAC_HANDOFF_VERSION || ![tabs isKindOfClass:[NSArray class]] ||
        !tabs.count || tabs.count + g_ntabs > MAX_TABS) return 0;
    int first = g_ntabs;
    @try {
        for (NSDictionary *entry in tabs) {
            int fd = mac_recv_fd(sock);
            if (fd < 0) break;
            Tab *tb = mac_tab_restore(entry, fd);
            if (!tb) { close(fd); break; }
            g_tabs[g_ntabs++] = tb;
        }
    } @catch (NSException *e) { /* reject malformed snapshots without consuming a donor's PTY */ }
    if (g_ntabs - first != (int)tabs.count) {
        while (g_ntabs > first) { Tab *t = g_tabs[--g_ntabs]; close(t->fd); term_free(&t->t); free(t); }
        return 0;
    }
    if (!g_mac_imports) g_mac_imports = [[NSMutableArray alloc] init];
    [g_mac_imports addObject:@{@"socket": @(sock), @"first": @(first), @"count": @(tabs.count)}];
    if (!g_mac_placement) {
        g_mac_placement = [@{@"frame": d[@"frame"] ?: @"{{0, 0}, {960, 600}}"} retain];
        int active = [d[@"active"] intValue];
        g_active = first + (active >= 0 && active < (int)tabs.count ? active : 0);
        g_cols = g_tabs[g_active]->t.cols; g_rows = g_tabs[g_active]->t.rows;
    }
    return 1;
}

static int mac_commit_imports(void) {
    if (!g_mac_imports.count) return 1;
    g_handoff_paint = 1; g_handoff_painted = 0;
    paint_view_now();
    g_handoff_paint = 0;
    if (!g_handoff_painted) return 0;
    int committed = 0;
    for (NSDictionary *group in g_mac_imports) {
        int fd = [group[@"socket"] intValue], first = [group[@"first"] intValue], count = [group[@"count"] intValue];
        int ok = mac_send(fd, @{@"ready": @YES}) && [mac_recv(fd)[@"go"] boolValue];
        close(fd);
        for (int i = first; i < first + count; i++) {
            Tab *tb = g_tabs[i];
            if (ok) {
                tab_start_io(tb);
                /* Window layout can resize a staged Term before GO, while
                   tab_set_size deliberately leaves the donor PTY untouched.
                   Apply that final grid once we own the PTY; tab_fit may now
                   see matching dimensions and have no further work to do. */
                tab_set_size(tb, tb->t.cols, tb->t.rows);
                committed++;
            }
            else { close(tb->fd); tb->fd = -1; tb->exited = 1; term_message(&tb->t, "\r\nTransfer cancelled; the original window retains this tab.\r\n"); }
        }
    }
    [g_mac_imports release]; g_mac_imports = nil;
    return committed > 0;
}

static NSString *mac_task(NSArray *args, int *status) {
    NSTask *task = [[[NSTask alloc] init] autorelease];
    NSPipe *out = [NSPipe pipe];
    task.launchPath = @"/usr/bin/env"; task.arguments = args;
    task.standardOutput = out; task.standardError = [NSFileHandle fileHandleWithNullDevice];
    @try {
        [task launch];
        NSData *data = [[out fileHandleForReading] readDataToEndOfFile];
        [task waitUntilExit];
        *status = task.terminationStatus;
        return [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease] ?: @"";
    } @catch (NSException *e) { *status = 1; return @""; }
}

static NSArray *mac_tmux_sessions(NSString *socket) {
    NSMutableArray *args = [NSMutableArray arrayWithObject:@"tmux"];
    if (socket) [args addObjectsFromArray:@[@"-S", socket]];
    [args addObjectsFromArray:@[@"list-sessions", @"-F", @"#{session_id}\t#{session_attached}\t#{session_name}"]];
    int status;
    NSString *result = mac_task(args, &status);
    NSMutableArray *sessions = [NSMutableArray array];
    if (status) return sessions;
    for (NSString *line in [result componentsSeparatedByString:@"\n"]) {
        NSArray *v = [line componentsSeparatedByString:@"\t"];
        if (v.count < 3) continue;
        [sessions addObject:@{@"id": v[0], @"attached": @([v[1] intValue]), @"name": v[2], @"socket": socket ?: @""}];
    }
    return sessions;
}

static NSString *mac_host_socket(void) {
    NSString *runtime = mac_runtime();
    return runtime ? [runtime stringByAppendingPathComponent:@"host.tmux"] : nil;
}

#include "hardcore_mac_tmux_metadata.m"

/* Prepared hosts and ordinary tmux clients keep their process tree in tmux.
   Find an exact source TTY on either known server; never infer ownership from
   the title or attach a merely similar session name. */
static NSDictionary *mac_host_for_tty(NSString *tty) {
    if (!mac_terminal_valid_tty(tty)) return nil;
    NSString *host = mac_host_socket();
    for (NSString *socket in host ? @[host, @""] : @[@""]) {
        NSMutableArray *args = [NSMutableArray arrayWithObjects:@"env", @"-u", @"TMUX", @"-u", @"TMUX_PANE", @"tmux", @"-N", nil];
        if (socket.length) [args addObjectsFromArray:@[@"-S", socket]];
        [args addObjectsFromArray:@[@"list-clients", @"-F", @"#{client_tty}\t#{session_id}"]];
        NSData *data = mac_tmux_metadata_output(args);
        NSString *result = data ? [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease] : nil;
        for (NSString *line in [result componentsSeparatedByString:@"\n"]) {
            NSArray *v = [line componentsSeparatedByString:@"\t"];
            if (v.count == 2 && [tty isEqual:v[0]] && mac_tmux_metadata_id(v[1], '$'))
                return @{@"id": v[1], @"socket": socket};
        }
    }
    return nil;
}

static int mac_host(int argc, char **argv) {
    NSString *socket = mac_host_socket();
    if (!socket || !isatty(STDIN_FILENO)) {
        fprintf(stderr, "hardcore: host needs an interactive terminal and a private runtime directory\n"); return 1;
    }
    int first = argc > 2 && !strcmp(argv[2], "--") ? 3 : 2;
    NSString *name = [NSString stringWithFormat:@"hardcore-%d-%@", getpid(), [[[NSUUID UUID] UUIDString] substringToIndex:8]];
    fprintf(stderr, "hardcore: portable session %s; use hardcore slurp --terminal %s to attach in Hardcore\n", name.UTF8String, ttyname(STDIN_FILENO));
    NSMutableArray *args = [NSMutableArray arrayWithObjects:@"tmux", @"-S", socket, @"-f", @"/dev/null", @"new-session", @"-s", name, nil];
    /* tmux treats a single command argument as shell code. env ensures even a
       lone executable path (including spaces) is passed as an argument vector. */
    if (first < argc) [args addObjectsFromArray:@[@"--", @"/usr/bin/env", @"--"]];
    for (int i = first; i < argc; i++) [args addObject:@(argv[i])];
    char **cmd = calloc(args.count + 1, sizeof(char *));
    for (NSUInteger i = 0; i < args.count; i++) cmd[i] = (char *)[args[i] UTF8String];
    execvp(cmd[0], cmd);
    perror("hardcore: host requires tmux on PATH"); free(cmd); return 1;
}

static NSArray *mac_attachable_tmux(NSString *socket) {
    NSMutableArray *found = [NSMutableArray arrayWithArray:mac_tmux_sessions(socket)];
    if (!socket) {
        NSString *host = mac_host_socket();
        if (host) for (NSDictionary *entry in mac_tmux_sessions(host)) {
            NSMutableDictionary *managed = [[entry mutableCopy] autorelease];
            managed[@"managed"] = @YES;
            [found addObject:managed];
        }
    }
    return found;
}

static int mac_slurp_options(int argc, char **argv, int *pid, NSString **session, NSString **socket, NSString **terminal, int *list) {
    *pid = 0; *session = nil; *socket = nil; *terminal = nil; *list = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) *list = 1;
        else if (!strcmp(argv[i], "--bridge")) { /* explicit legacy view, never a transfer fallback */ }
        else if (!strcmp(argv[i], "--terminal") && i + 1 < argc) *terminal = @(argv[++i]);
        else if (!strcmp(argv[i], "--pid") && i + 1 < argc) {
            char *end; long n = strtol(argv[++i], &end, 10);
            if (*end || n <= 1 || n > INT_MAX) return 0;
            *pid = (int)n;
        } else if (!strcmp(argv[i], "--tmux") && i + 1 < argc) *session = @(argv[++i]);
        else if (!strcmp(argv[i], "--tmux-socket") && i + 1 < argc) *socket = @(argv[++i]);
        else if (!strcmp(argv[i], "--ready-fd") && i + 1 < argc) { g_mac_ready_fd = atoi(argv[++i]); }
        else return 0;
    }
    return ((*pid != 0) + (*session != nil) + (*terminal != nil)) <= 1;
}

static int mac_slurp_bridge(int argc, char **argv) {
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--bridge")) return 1;
    return 0;
}

static Tab *mac_tmux_tab(NSString *session, NSString *socket) {
    if ([session lengthOfBytesUsingEncoding:NSUTF8StringEncoding] >= sizeof(((Tab *)0)->run) ||
        [socket lengthOfBytesUsingEncoding:NSUTF8StringEncoding] >= sizeof(((Tab *)0)->hist)) return NULL;
    Tab *tb = calloc(1, sizeof(Tab));
    if (!tb || !term_init(&tb->t, g_cols, g_rows)) { free(tb); return NULL; }
    tb->fd = -1; tb->id = g_next_id++;
    /* Before source discovery completes, identify the source instead of
       presenting the receiving process's HOME as the session's directory. */
    snprintf(tb->start_dir, sizeof(tb->start_dir), "%s", session.UTF8String);
    snprintf(tb->run, sizeof(tb->run), "%s", session.UTF8String);
    snprintf(tb->hist, sizeof(tb->hist), "%s", socket.UTF8String ?: "");
    if ([session hasPrefix:@"terminal:"])
        mac_apply_session_metadata(tb, mac_terminal_metadata([session substringFromIndex:9]));
    else if ([session hasPrefix:@"claude-wait:"])
        snprintf(tb->program, sizeof(tb->program), "claude");
    else
        mac_apply_session_metadata(tb, mac_tmux_metadata(session, socket));
    if (!tab_spawn(tb)) { term_free(&tb->t); free(tb); return NULL; }
    return tb;
}

static NSString *mac_helper(NSString *name) {
    NSString *root = [[@(g_mac_exe_path) stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
    NSString *path = [[root stringByAppendingPathComponent:@"Resources"] stringByAppendingPathComponent:name];
    if (![[NSFileManager defaultManager] isReadableFileAtPath:path])
        path = [[root stringByAppendingPathComponent:@"tools"] stringByAppendingPathComponent:name];
    return [[NSFileManager defaultManager] isReadableFileAtPath:path] ? path : nil;
}

/* Called before AppKit creates a window. Ordinary launches return 0. */
static int mac_prepare(int argc, char **argv) {
    if (argc < 2) return 0;
    if (!strcmp(argv[1], "--adopt-fd")) {
        if (argc != 3) return -1;
        int fd = atoi(argv[2]);
        if (fd < 3 || !mac_peer_ok(fd)) return -1;
        mac_socket_setup(fd);
        if (!mac_import(fd)) { close(fd); return -1; }
        return 1;
    }
    if (!strcmp(argv[1], "--claude-window")) {
        if ((argc != 5 && argc != 6) || strcmp(argv[3], "--ready-fd") ||
            (argc == 6 && strcmp(argv[5], "--guard-input"))) return -1;
        g_mac_ready_fd = atoi(argv[4]);
        if (g_mac_ready_fd < 3 || !mac_peer_ok(g_mac_ready_fd)) return -1;
        mac_socket_setup(g_mac_ready_fd);
        NSString *helper = mac_helper(@"claude_handoff.py");
        if (!helper) return -1;
        g_cols = 120; g_rows = 36;
        Tab *tb = mac_tmux_tab([@"claude-wait:" stringByAppendingString:@(argv[2])], helper);
        if (!tb) return -1;
        if (argc == 6) { tb->claude_guard = 1; tb->claude_guard_at = GetTickCount(); }
        g_tabs[g_ntabs++] = tb;
        return 1;
    }
    if (strcmp(argv[1], "--slurp-window")) return 0;
    int pid, list; NSString *session, *socket, *terminal;
    if (!mac_slurp_options(argc, argv, &pid, &session, &socket, &terminal, &list)) return -1;
    if (g_mac_ready_fd >= 0) {
        if (!mac_peer_ok(g_mac_ready_fd)) return -1;
        mac_socket_setup(g_mac_ready_fd);
    }
    NSArray *terminal_sessions = !pid && !session ? mac_terminal_sessions(1) : @[];
    int bridge = mac_slurp_bridge(argc, argv);
    int need_bridge = 0;
    for (NSDictionary *source in terminal_sessions)
        if (bridge && (!terminal || [terminal isEqual:source[@"tty"]]) && !mac_host_for_tty(source[@"tty"])) need_bridge = 1;
    if (need_bridge && !AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)@{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES})) {
        fprintf(stderr, "hardcore: Terminal input needs Accessibility permission. Enable it for Hardcore or the launching app in System Settings, then retry.\n");
        if (terminal) return -1;
        NSMutableArray *prepared = [NSMutableArray array];
        for (NSDictionary *source in terminal_sessions) if (mac_host_for_tty(source[@"tty"])) [prepared addObject:source];
        terminal_sessions = prepared;
    }
    if (!session && !terminal) for (NSDictionary *window in mac_windows()) {
        if (pid && [window[@"pid"] intValue] != pid) continue;
        if (![window[@"tabs"] count]) continue;
        int fd = mac_connect(window[@"socket"]);
        if (fd < 0) continue;
        if (!mac_send(fd, @{@"op": @"take"}) || !mac_import(fd)) {
            fprintf(stderr, "hardcore: window %d could not transfer; left running\n", [window[@"pid"] intValue]); close(fd);
        }
    }
    if (!g_ntabs) { g_cols = 120; g_rows = 36; }
    if (!pid && !terminal) for (NSDictionary *tmux in mac_attachable_tmux(socket)) {
        if (session ? (![session isEqual:tmux[@"id"]] && ![session isEqual:tmux[@"name"]]) : ([tmux[@"attached"] intValue] != 0 && ![tmux[@"managed"] boolValue])) continue;
        int duplicate = 0;
        for (int i = 0; i < g_ntabs; i++)
            if (!strcmp(g_tabs[i]->run, [tmux[@"id"] UTF8String]) && !strcmp(g_tabs[i]->hist, [tmux[@"socket"] UTF8String])) duplicate = 1;
        if (duplicate) continue;
        if (g_ntabs == MAX_TABS) break;
        Tab *tb = mac_tmux_tab(tmux[@"id"], [tmux[@"socket"] length] ? tmux[@"socket"] : nil);
        if (tb) g_tabs[g_ntabs++] = tb;
    }
    for (NSDictionary *source in terminal_sessions) {
        if (terminal && ![terminal isEqual:source[@"tty"]]) continue;
        NSDictionary *host = mac_host_for_tty(source[@"tty"]);
        if (host) {
            int duplicate = 0;
            for (int i = 0; i < g_ntabs; i++)
                if (!strcmp(g_tabs[i]->run, [host[@"id"] UTF8String]) && !strcmp(g_tabs[i]->hist, [host[@"socket"] UTF8String])) duplicate = 1;
            if (!duplicate && g_ntabs < MAX_TABS) {
                Tab *tb = mac_tmux_tab(host[@"id"], host[@"socket"]);
                if (tb) g_tabs[g_ntabs++] = tb;
            }
            continue;
        }
        if (!bridge) continue;
        NSString *marker = [@"terminal:" stringByAppendingString:source[@"tty"]];
        int duplicate = 0;
        for (int i = 0; i < g_ntabs; i++) if (!strcmp(g_tabs[i]->run, marker.UTF8String)) duplicate = 1;
        if (duplicate || g_ntabs == MAX_TABS) continue;
        Tab *tb = mac_tmux_tab(marker, [source[@"window"] stringValue]);
        if (tb) g_tabs[g_ntabs++] = tb;
    }
    if (!g_ntabs) { fprintf(stderr, "hardcore: no eligible sessions; use hardcore slurp --list\n"); return -1; }
    return 1;
}

static int mac_transfer_reload_windows(NSArray *windows, NSString *exe) {
    int ok = 0, bad = 0;
    for (NSDictionary *window in windows) {
        if (![window[@"tabs"] count]) continue;
        int fd = mac_connect(window[@"socket"]);
        NSDictionary *reply = fd >= 0 && mac_send(fd, @{@"op": @"reload", @"exe": exe}) ? mac_recv(fd) : nil;
        if ([reply[@"ok"] boolValue]) ok++;
        else { bad++; fprintf(stderr, "hardcore: window %d did not reload; original tabs retained\n", [window[@"pid"] intValue]); }
        if (fd >= 0) close(fd);
    }
    printf("hardcore: %d window%s reloaded in place\n", ok, ok == 1 ? "" : "s");
    if (!ok && !bad) printf("No compatible running windows. Older Mac builds need one manual restart.\n");
    return bad ? 1 : 0;
}

/* Describe what the receiver actually opened. A bridge client is not evidence
   that Terminal's process, shell, colors, or cursor were transferred. */
static NSDictionary *mac_slurp_result(void) {
    int native = 0, bridges = 0, tmux = 0, claude = 0;
    for (int i = 0; i < g_ntabs; i++) {
        const char *run = g_tabs[i]->run;
        if (!strncmp(run, "terminal:", 9)) bridges++;
        else if (!strncmp(run, "claude-wait:", 12)) claude++;
        else if (*run) tmux++;
        else native++;
    }
    return @{@"ok": @YES, @"pid": @(getpid()), @"nativeTabs": @(native),
             @"terminalBridges": @(bridges), @"tmuxClients": @(tmux), @"claudeClients": @(claude)};
}

static NSString *mac_slurp_terminal_label(NSDictionary *source) {
    NSString *path = [source[@"cwd"] isKindOfClass:[NSString class]] ? source[@"cwd"] : nil;
    NSString *folder = hc_dock_label(path.lastPathComponent, @"unknown folder");
    NSString *program = hc_dock_label(source[@"program"], hc_dock_label(source[@"shell"], @"shell"));
    return [NSString stringWithFormat:@"%@:%@", folder, program];
}

static void mac_slurp_report(NSDictionary *reply, FILE *out) {
    if (![reply[@"nativeTabs"] isKindOfClass:[NSNumber class]]) {
        fprintf(out, "hardcore: opened receiving window %d\n", [reply[@"pid"] intValue]);
        return;
    }
    int native = [reply[@"nativeTabs"] intValue], bridges = [reply[@"terminalBridges"] intValue];
    int tmux = [reply[@"tmuxClients"] intValue], claude = [reply[@"claudeClients"] intValue];
    if (native) fprintf(out, "  %d native Hardcore tab%s transferred with live PTYs and terminal state.\n", native, native == 1 ? "" : "s");
    if (tmux) fprintf(out, "  %d tmux client%s opened; sessions remain in their tmux server.\n", tmux, tmux == 1 ? "" : "s");
    if (claude) fprintf(out, "  %d Claude attachment client%s opened.\n", claude, claude == 1 ? "" : "s");
    if (bridges) {
        fprintf(out, "  %d Terminal bridge view%s opened; source processes remain in Terminal.\n", bridges, bridges == 1 ? "" : "s");
        fputs("Keep the original Terminal tabs open. Bridge views show plain text; colors and exact cursor position are not preserved.\n", out);
        fputs("For Claude conversation handoff, see hardcore claude install --help and /hardcore-slurp wait. The parent shell and Terminal history still remain at the source.\n", out);
    }
    /* Keep the final window PID readable by existing command-line callers. */
    fprintf(out, "hardcore: opened receiving window %d\n", [reply[@"pid"] intValue]);
}

static NSDictionary *mac_slurp_sources(void) {
    NSMutableArray *terminal = [NSMutableArray array];
    for (NSDictionary *source in mac_terminal_sessions(1)) {
        NSMutableDictionary *entry = [[source mutableCopy] autorelease];
        entry[@"managed"] = mac_host_for_tty(source[@"tty"]) ? @YES : @NO;
        [terminal addObject:entry];
    }
    return @{@"terminal":terminal,
        @"terminalDiscoveryFailed":g_mac_terminal_discovery_failed ? @YES : @NO,
        @"hardcore":mac_windows(), @"tmux":mac_attachable_tmux(nil)};
}

static int mac_cli(int argc, char **argv) {
    uint32_t cap = sizeof(g_mac_exe_path);
    char path[MAX_PATH];
    if (_NSGetExecutablePath(path, &cap) || !realpath(path, g_mac_exe_path)) return 1;
    g_mac_image = [mac_hash(@(g_mac_exe_path)) retain];
    g_mac_install_exe = [@(g_mac_exe_path) copy];
    if (argc < 2) return -1;
    if (!strcmp(argv[1], "browser")) return mac_browser_cli(argc - 2, argv + 2);
    if (!strcmp(argv[1], "--slurp-sources")) {
        if (argc != 2) return 2;
        NSData *data = [NSJSONSerialization dataWithJSONObject:mac_slurp_sources() options:0 error:nil];
        if (!data) return 1;
        fwrite(data.bytes, 1, data.length, stdout); putchar('\n'); return 0;
    }
    if (!strcmp(argv[1], "claude")) {
        if (argc < 3) { fprintf(stderr, "Usage: hardcore claude install|status|move [--project DIR]\n"); return 2; }
        int setup = !strcmp(argv[2], "install") || !strcmp(argv[2], "status") || !strcmp(argv[2], "move") || !strcmp(argv[2], "moves") || !strcmp(argv[2], "inspect") || !strcmp(argv[2], "recover");
        int broker = !strcmp(argv[2], "prepare") || !strcmp(argv[2], "arm") || !strcmp(argv[2], "cancel") || !strcmp(argv[2], "claim") || !strcmp(argv[2], "ready") || !strcmp(argv[2], "queue") || !strcmp(argv[2], "poll") || !strcmp(argv[2], "cancel-intent") || !strcmp(argv[2], "verify") || !strcmp(argv[2], "progress") || !strcmp(argv[2], "cursor") || !strcmp(argv[2], "release") || !strcmp(argv[2], "failed") || !strcmp(argv[2], "guard");
        if (!setup && !broker) { fprintf(stderr, "hardcore: unknown Claude adapter command\n"); return 2; }
        NSString *helper = mac_helper(setup ? @"claude_slurp.py" : @"claude_handoff.py");
        if (!helper) { fprintf(stderr, "hardcore: missing Claude adapter; rebuild or reinstall Hardcore\n"); return 1; }
        char **args = calloc(argc + 6, sizeof(char *));
        if (!args) return 1;
        int n = 0;
        /* Helpers import bundled modules. Bytecode caches inside Resources
           would change the signed bundle on the first invocation. */
        args[n++] = "env"; args[n++] = "python3"; args[n++] = "-B"; args[n++] = (char *)helper.fileSystemRepresentation;
        for (int i = 2; i < argc; i++) args[n++] = argv[i];
        if (broker) { args[n++] = "--executable"; args[n++] = g_mac_exe_path; }
        execv("/usr/bin/env", args);
        perror("hardcore: Claude adapter"); free(args); return 1;
    }
    if (!strcmp(argv[1], "--claude-receipt") || !strcmp(argv[1], "--claude-cursor") || !strcmp(argv[1], "--claude-release") || !strcmp(argv[1], "--claude-guard")) {
        if (argc != 6) return 2;
        char *end = NULL;
        long window = strtol(argv[2], &end, 10);
        if (!end || *end || window <= 1 || window > INT_MAX) return 2;
        long receiver = strtol(argv[4], &end, 10);
        if (!end || *end || receiver <= 1 || receiver > INT_MAX) return 2;
        NSString *dir = mac_runtime();
        if (!dir) return 1;
        NSString *endpoint = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%ld.sock", window]];
        NSDictionary *reply = nil;
        for (int attempt = 0; attempt < 20; attempt++) {
            reply = nil;
            int fd = mac_connect(endpoint);
            if (fd >= 0) {
                if (mac_send(fd, @{@"op": @(argv[1] + 2), @"requestId": @(argv[3]),
                                  @"receiverPid": @(receiver), @"receiverTty": @(argv[5])})) reply = mac_recv(fd);
                close(fd);
            }
            /* ui.log may return before the attached client flushes its output.
               Wait only for the bound cursor marker, off the window's UI thread. */
            if (strcmp(argv[1], "--claude-cursor") || ![reply[@"waitingForCursorMarker"] boolValue]) break;
            usleep(100000);
        }
        if (!reply) reply = @{@"ok": @NO, @"error": @"Receiving Hardcore window is unavailable."};
        NSData *json = [NSJSONSerialization dataWithJSONObject:reply options:0 error:nil];
        fwrite(json.bytes, 1, json.length, stdout); putchar('\n');
        return [reply[@"ok"] boolValue] ? 0 : 1;
    }
    if (!strcmp(argv[1], "--claude-open")) {
        if (argc != 3 && (argc != 4 || strcmp(argv[3], "--guard-input"))) return 2;
        int pair[2]; pid_t child;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 1;
        mac_socket_setup(pair[0]); mac_socket_setup(pair[1]);
        NSMutableArray *args = [NSMutableArray arrayWithObjects:@"--claude-window", @(argv[2]), @"--ready-fd", @"198", nil];
        if (argc == 4) [args addObject:@"--guard-input"];
        int started = mac_spawn(@(g_mac_exe_path), args, pair[1], 1, &child);
        close(pair[1]);
        NSDictionary *reply = started ? mac_recv(pair[0]) : nil;
        close(pair[0]);
        if (![reply[@"ok"] boolValue]) { puts("{\"ok\":false,\"error\":\"Receiving window did not become ready\"}"); return 1; }
        printf("{\"ok\":true,\"pid\":%d,\"inputGuarded\":%s}\n", [reply[@"pid"] intValue], argc == 4 ? "true" : "false"); return 0;
    }
    /* Maintenance for one explicitly identified legacy mirror helper. */
    if (!strcmp(argv[1], "--refresh-terminal-bridge")) {
        if (argc != 4) return 2;
        char *end = NULL;
        long window = strtol(argv[2], &end, 10);
        if (!end || *end || window <= 1 || window > INT_MAX) return 2;
        long helper = strtol(argv[3], &end, 10);
        if (!end || *end || helper <= 1 || helper > INT_MAX) return 2;
        NSString *endpoint = [mac_runtime() stringByAppendingPathComponent:[NSString stringWithFormat:@"%ld.sock", window]];
        int fd = mac_connect(endpoint);
        NSDictionary *reply = nil;
        if (fd >= 0) {
            if (mac_send(fd, @{@"op": @"refresh-terminal-bridge", @"helperPid": @(helper)})) reply = mac_recv(fd);
            close(fd);
        }
        if (!reply) reply = @{@"ok": @NO, @"error": @"Hardcore window is unavailable."};
        NSData *json = [NSJSONSerialization dataWithJSONObject:reply options:0 error:nil];
        fwrite(json.bytes, 1, json.length, stdout); putchar('\n');
        return [reply[@"ok"] boolValue] ? 0 : 1;
    }
    if (!strcmp(argv[1], "host")) return mac_host(argc, argv);
    if (!strcmp(argv[1], "--terminal-bridge")) return argc == 4 ? mac_terminal_bridge(argv[2], argv[3]) : 2;
    if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "version") || !strcmp(argv[1], "-v")) {
        printf("hardcore %s (build %s), macOS handoff protocol %d\n", HARDCORE_VERSION, HARDCORE_BUILD, MAC_HANDOFF_VERSION); return 0;
    }
    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "help") || !strcmp(argv[1], "-h")) {
        puts("hardcore [DIR]                 open a terminal\n"
             "hardcore browser open [URL]     new live browser tab (Hardcore grid by default)\n"
             "hardcore browser --help         browser commands and tab targeting\n"
             "hardcore host [--] [COMMAND...] start a portable tmux-backed session in this terminal\n"
             "hardcore update                use a newly installed build, or check GitHub for an update, then reload live windows\n"
             "hardcore update --local         reload the installed build without downloading\n"
             "hardcore update --from PATH     install a local app bundle or bare executable, then reload\n"
             "hardcore reload / --reload      replace the installed process image, preserving its PID\n"
             "hardcore reload --exe PATH      hand live tabs to a different executable in a new process\n"
             "hardcore slurp [--list]         discover sessions and choose their transfer method\n"
             "hardcore slurp --terminal TTY   transfer one Terminal.app session\n"
             "hardcore slurp --bridge         explicitly open plain-text Terminal bridge views\n"
             "hardcore slurp --pid PID        move one Hardcore window's live tabs\n"
             "hardcore slurp --tmux SESSION [--tmux-socket PATH]\n"
             "                               attach a live tmux session (existing clients stay attached)\n"
             "hardcore claude install [--project DIR]  install the cooperative Claude adapter for this project\n"
             "hardcore claude status [--project DIR]   inspect the project's adapter installation\n"
             "hardcore claude move SESSION_ID [--project DIR]  request a guarded move from outside Claude\n"
             "hardcore claude moves [--json]  list recorded moves and current availability\n"
             "hardcore claude inspect REQUEST_ID [--json]  inspect verification and recovery state\n"
             "hardcore claude recover REQUEST_ID --export-draft FILE  save an unsent draft without overwriting\n"
             "hardcore --version\n\n"
             "Slurp installs and queues Claude's adapter automatically. First-time running sessions\n"
             "still need /reload-plugins in Claude. A loaded adapter accepts requests without replacing your draft.\n"
             "It waits for a verified completed turn and no background work; Terminal stays open.\n"
             "Unsupported transfers stay at the source. Only --bridge opens a plain-text mirror; keep Terminal open.\n"
             "Terminal bridges require Automation + Accessibility permission; colors and exact cursor position are unavailable.\n"
             "Arbitrary iTerm PTYs are not supported yet; tmux-backed sessions can attach.\n"
             "Pre-handoff Hardcore builds require one manual restart."); return 0;
    }
    if (!strcmp(argv[1], "--reload")) {
        if (argc != 2) return 2;
        return mac_reload_windows();
    }
    if (!strcmp(argv[1], "reload")) {
        if (argc == 2) return mac_reload_windows();
        if (argc != 4 || strcmp(argv[2], "--exe")) {
            fprintf(stderr, "usage: hardcore reload [--exe PATH]\n"); return 2;
        }
        char resolved[MAX_PATH];
        if (!realpath(argv[3], resolved)) { perror("hardcore: replacement"); return 1; }
        return mac_transfer_reload_windows(mac_windows(), @(resolved));
    }
    if (!strcmp(argv[1], "update") || !strcmp(argv[1], "--update")) {
        if (argc == 3 && !strcmp(argv[2], "--local")) return mac_reload_windows();
        /* A freshly installed build is already the desired update. This also
           retains the no-download workflow of early handoff-enabled builds. */
        if (argc == 2) for (NSDictionary *window in mac_windows())
            if ([window[@"exe"] isEqual:@(g_mac_exe_path)] && ![window[@"image"] isEqual:g_mac_image])
                return mac_reload_windows();
        return mac_update_cmd(argc - 2, argv + 2);
    }
    if (!strcmp(argv[1], "slurp") || !strcmp(argv[1], "--slurp-native")) {
        int pid, list; NSString *session, *socket, *terminal;
        if (!mac_slurp_options(argc, argv, &pid, &session, &socket, &terminal, &list)) { fprintf(stderr, "hardcore: invalid slurp options; see --help\n"); return 2; }
        int bridge = mac_slurp_bridge(argc, argv), native = !strcmp(argv[1], "--slurp-native");
        if (native && (bridge || pid || session || socket || list)) return 2;
        if (list) {
            if (!pid && !session) for (NSDictionary *t in mac_terminal_sessions(1))
                if (!terminal || [terminal isEqual:t[@"tty"]])
                    printf("terminal tty=%s %s (%s)\n", [t[@"tty"] UTF8String], mac_slurp_terminal_label(t).UTF8String,
                           mac_host_for_tty(t[@"tty"]) ? "portable host; live tmux attachment" :
                           [t[@"program"] isEqual:@"claude"] ? "Claude handoff; adapter installed automatically if needed" :
                           "direct transfer unavailable; source retained");
            for (NSDictionary *w in mac_windows())
                if (!session && !terminal && (!pid || [w[@"pid"] intValue] == pid))
                    printf("hardcore pid=%d tabs=%lu\n", [w[@"pid"] intValue], (unsigned long)[w[@"tabs"] count]);
            if (!pid && !terminal) for (NSDictionary *t in mac_attachable_tmux(socket))
                printf("tmux %s name=%s clients=%d %s\n", [t[@"id"] UTF8String], [t[@"name"] UTF8String], [t[@"attached"] intValue],
                       [t[@"managed"] boolValue] ? "(portable host; eligible)" : [t[@"attached"] intValue] ? "(use --tmux to attach explicitly)" : "(eligible)");
            puts("Default slurp never substitutes a plain-text mirror for a transfer. --bridge explicitly requests that view. iTerm requires tmux for now.");
            return 0;
        }
        if (!native && !bridge && !pid && !session && !socket) {
            NSString *helper = mac_helper(@"slurp_mac.py");
            if (!helper) { fprintf(stderr, "hardcore: missing slurp coordinator; rebuild or reinstall Hardcore\n"); return 1; }
            NSMutableArray *arguments = [NSMutableArray arrayWithObjects:@"env", @"python3", @"-B", helper,
                @"--executable", @(g_mac_exe_path), nil];
            if (terminal) [arguments addObjectsFromArray:@[@"--terminal", terminal]];
            char **command = calloc(arguments.count + 1, sizeof(char *));
            if (!command) return 1;
            for (NSUInteger i = 0; i < arguments.count; i++) command[i] = (char *)[arguments[i] UTF8String];
            execv("/usr/bin/env", command);
            perror("hardcore: slurp coordinator"); free(command); return 1;
        }
        if (native) {
            int eligible = 0;
            if (!terminal) {
                for (NSDictionary *window in mac_windows()) if ([window[@"tabs"] count]) eligible = 1;
                for (NSDictionary *tmux in mac_attachable_tmux(nil))
                    if (![tmux[@"attached"] intValue] || [tmux[@"managed"] boolValue]) eligible = 1;
            }
            for (NSDictionary *source in mac_terminal_sessions(1))
                if ((!terminal || [terminal isEqual:source[@"tty"]]) && mac_host_for_tty(source[@"tty"])) eligible = 1;
            if (!eligible) { puts("hardcore: no native Hardcore or portable sessions to transfer"); return 0; }
        }
        int needs_accessibility = 0;
        if (bridge && !pid && !session) for (NSDictionary *t in mac_terminal_sessions(1))
            if ((!terminal || [terminal isEqual:t[@"tty"]]) && !mac_host_for_tty(t[@"tty"])) needs_accessibility = 1;
        if (needs_accessibility &&
            !AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)@{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES})) {
            fprintf(stderr, "hardcore: Terminal input needs Accessibility permission for Hardcore or its launching app. Enable it in System Settings > Privacy & Security > Accessibility, then retry.\n");
            return 1;
        }
        if (needs_accessibility)
            fputs("hardcore: opening plain-text Terminal bridge views. The source processes stay in Terminal; leave those tabs open.\n", stderr);
        int pair[2]; pid_t child;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 1;
        mac_socket_setup(pair[0]); mac_socket_setup(pair[1]);
        NSMutableArray *args = [NSMutableArray arrayWithObjects:@"--slurp-window", @"--ready-fd", @"198", nil];
        for (int i = 2; i < argc; i++) [args addObject:@(argv[i])];
        int started = mac_spawn(@(g_mac_exe_path), args, pair[1], 1, &child);
        close(pair[1]);
        NSDictionary *reply = started ? mac_recv(pair[0]) : nil;
        close(pair[0]);
        if (![reply[@"ok"] boolValue]) { fprintf(stderr, "hardcore: slurp did not complete; check the original windows\n"); return 1; }
        mac_slurp_report(reply, stdout);
        return 0;
    }
    return -1;
}
