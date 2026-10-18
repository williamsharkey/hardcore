/* macOS live updates. exec keeps our PID, children, process groups and PTY
   masters. Only the Cocoa/Metal process image is replaced. The versioned plist
   carries terminal snapshots, metadata and incomplete parser sequences; no
   Term/Tab struct layout is part of the protocol. Included by hardcore_mac.m. */
#include <mach-o/dyld.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <CommonCrypto/CommonDigest.h>

#ifndef HARDCORE_BUILD
#define HARDCORE_BUILD "unknown"
#endif
static NSString *g_mac_exe, *g_mac_target;
static NSDictionary *g_mac_adopt;
static int g_mac_state_fd = -1, g_mac_ack_fd = -1;

static void mac_executable_init(void) {
    char path[4096], real[4096]; uint32_t n = sizeof(path);
    if (_NSGetExecutablePath(path, &n) || !realpath(path, real)) { fprintf(stderr, "hardcore: cannot locate executable\n"); exit(1); }
    signal(SIGPIPE, SIG_IGN);
    g_mac_exe = [[NSString stringWithUTF8String:real] copy];
    snprintf(g_mac_bin_dir, sizeof(g_mac_bin_dir), "%s", [[g_mac_exe stringByDeletingLastPathComponent] fileSystemRepresentation]);
    NSString *bundle = [[[g_mac_exe stringByDeletingLastPathComponent] stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
    g_mac_target = [([[bundle pathExtension] isEqualToString:@"app"] ? bundle : g_mac_exe) copy];
}
/* Subprocess arguments are passed directly, never interpreted by a shell.
   Use a file for output so a full pipe cannot deadlock the updater. */
static int mac_run(NSString *exe, NSArray *args, NSTimeInterval timeout, NSString **output) {
    FILE *tmp = tmpfile();
    if (!tmp) return 0;
    fcntl(fileno(tmp), F_SETFD, FD_CLOEXEC);
    NSFileHandle *file = [[[NSFileHandle alloc] initWithFileDescriptor:fileno(tmp) closeOnDealloc:NO] autorelease];
    NSTask *task = [[[NSTask alloc] init] autorelease];
    [task setExecutableURL:[NSURL fileURLWithPath:exe]];
    [task setArguments:args]; [task setStandardOutput:file]; [task setStandardError:file];
    [task setStandardInput:[NSFileHandle fileHandleWithNullDevice]];
    NSError *err = nil;
    if (![task launchAndReturnError:&err]) { if (output) *output = [err localizedDescription]; fclose(tmp); return 0; }
    double until = uptime_ms() + timeout * 1000;
    while ([task isRunning] && uptime_ms() < until) usleep(10000);
    if ([task isRunning]) { kill([task processIdentifier], SIGKILL); [task waitUntilExit]; }
    int ok = [task terminationStatus] == 0;
    if (output) {
        rewind(tmp); char buf[8193]; size_t n = fread(buf, 1, sizeof(buf)-1, tmp); buf[n] = 0;
        *output = [NSString stringWithUTF8String:buf] ?: @"invalid output";
    }
    fclose(tmp); return ok;
}
static int mac_probe(NSString *exe) {
    NSString *out = nil;
    return mac_run(exe, @[@"--mac-update-protocol"], 10, &out) && [out isEqualToString:@"hardcore-mac-reload-1\n"];
}
static int mac_write_all(int fd, const void *bytes, size_t n) {
    const char *p = bytes;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k; n -= (size_t)k;
    }
    return 1;
}

/* Only the owning user can discover or contact these sockets. The peer UID
   is also checked on both ends. A request can reload only the server's own
   executable, never an arbitrary path supplied over IPC. */
static NSString *mac_runtime_dir(void) { return mac_runtime(); }

#define MAC_TERM_NUMBERS(X) \
    X(st) X(esc_which) X(np) X(priv) X(inter) X(sub) X(csi_bad) X(utf8_need) X(utf8_acc) X(utf8_min) X(osc_n) X(osc_over) \
    X(last_ch) X(m_sync) X(sync_since) X(bell) X(spin_ch) X(spin_at) X(spin_n) \
    X(progress) X(ps_state) X(ps_pct) X(ps_tasks)
#define MAC_TERM_STRINGS(X) X(cwd) X(claude) X(claude_args) X(cmd) X(ps_kind)

static NSDictionary *mac_pack_tab(Tab *tb) {
    tab_drain(tb);
    Term *t = &tb->t; SBuf snap = {0};
    term_snapshot(t, &snap);
    NSMutableDictionary *d = [NSMutableDictionary dictionaryWithDictionary:@{
        @"id": @(tb->id), @"kind": @(tb->kind), @"parent": @(tb->parent_id),
        @"browser_url": [NSString stringWithUTF8String:tb->browser_url] ?: @"",
        @"browser_target": [NSString stringWithUTF8String:tb->browser_target] ?: @"",
        @"browser_site": [NSString stringWithUTF8String:tb->browser_site] ?: @"",
        @"browser_native": @(tb->browser_native),
        @"browser_recording": @(tb->browser_recording),
        @"fd": @(tb->fd), @"pid": @(tb->pid), @"cols": @(t->cols), @"rows": @(t->rows),
        @"view": @(t->view), @"activity": @(tb->activity), @"attention": @(tb->attention),
        @"exited": @(tb->exited), @"help": @(tb->help), @"born": @(tb->born),
        @"dir": [NSString stringWithUTF8String:tb->start_dir] ?: @"",
        @"run": [NSString stringWithUTF8String:tb->run] ?: @"",
        @"hist": [NSString stringWithUTF8String:tb->hist] ?: @"",
        @"claude_guard": @(tb->claude_guard), @"claude_released": @(tb->claude_released),
        @"claude_cursor_sent": @(tb->claude_cursor_sent), @"claude_guard_at": @(tb->claude_guard_at),
        @"snapshot": [NSData dataWithBytes:snap.p length:snap.n],
        @"title": @""
    }];
    free(snap.p);
#define PACK_NUMBER(field) d[@#field] = @(t->field);
    MAC_TERM_NUMBERS(PACK_NUMBER)
#undef PACK_NUMBER
#define PACK_STRING(field) d[@#field] = [NSString stringWithUTF8String:t->field] ?: @"";
    MAC_TERM_STRINGS(PACK_STRING)
#undef PACK_STRING
    char utf[2048];
    /* wchar_t is UTF-32 on macOS; NSString's unichar is UTF-16. */
    int j, n = 0;
    for (j = 0; t->title[j] && j < 255; j++) n += utf8_encode(t->title[j], utf+n);
    utf[n] = 0; d[@"title"] = [NSString stringWithUTF8String:utf] ?: @"";
    n = 0;
    for (j = 0; t->ps_msg[j] && j < 199; j++) n += utf8_encode(t->ps_msg[j], utf+n);
    utf[n] = 0; d[@"ps_msg"] = [NSString stringWithUTF8String:utf] ?: @"";
    d[@"osc"] = [NSData dataWithBytes:t->osc length:(NSUInteger)t->osc_n];
    NSMutableArray *params = [NSMutableArray array], *marks = [NSMutableArray array], *tasks = [NSMutableArray array];
    for (j = 0; j < MAX_PARAMS; j++) [params addObject:@(t->params[j])];
    for (j = 0; j < t->nmarks; j++) [marks addObject:@(t->hpushed - t->marks[j])];
    for (j = 0; j < 16; j++) [tasks addObject:@{ @"id": [NSString stringWithUTF8String:t->ps_task_id[j]] ?: @"", @"on": @(t->ps_task_on[j]) }];
    d[@"params"] = params; d[@"marks"] = marks; d[@"tasks"] = tasks;
    return d;
}
static Tab *mac_unpack_tab(NSDictionary *d) {
    int cols = [d[@"cols"] intValue], rows = [d[@"rows"] intValue], fd = [d[@"fd"] intValue];
    if (cols < 1 || cols > 1000 || rows < 1 || rows > 500 || fd < -1 ||
        (fd >= 0 && (!isatty(fd) || fcntl(fd, F_GETFD) < 0))) return NULL;
    NSData *snap = d[@"snapshot"], *osc = d[@"osc"];
    if (![snap isKindOfClass:[NSData class]] || [snap length] > INT_MAX ||
        ![osc isKindOfClass:[NSData class]] || [osc length] >= OSC_MAX ||
        [d[@"params"] count] != MAX_PARAMS || [d[@"marks"] count] > 512 || [d[@"tasks"] count] != 16) return NULL;
    Tab *tb = calloc(1, sizeof(Tab));
    if (!tb || !term_init(&tb->t, cols, rows)) { free(tb); return NULL; }
    tb->fd = -1; /* snapshot replies must never go to the running shell */
    int saved_id=[d[@"id"] intValue];
    tb->id=saved_id>0 && saved_id<INT_MAX-1 ? saved_id : g_next_id++;
    if (g_next_id<=tb->id) g_next_id=tb->id+1;
    if ([d[@"kind"] intValue]==TK_BROWSER) {
        tb->kind=TK_BROWSER; tb->parent_id=[d[@"parent"] intValue];
        tb->browser_native=[d[@"browser_native"] boolValue];
        int recording=[d[@"browser_recording"] intValue];tb->browser_recording=recording==1 || recording==2 ? recording : 0;
        snprintf(tb->browser_url,sizeof(tb->browser_url),"%s",[browser_string(d[@"browser_url"]) UTF8String]);
        snprintf(tb->browser_target,sizeof(tb->browser_target),"%s",[browser_string(d[@"browser_target"]) UTF8String]);
        snprintf(tb->browser_site,sizeof(tb->browser_site),"%s",[browser_string(d[@"browser_site"]) UTF8String]);
        if (fd>=0 || [d[@"pid"] intValue]>0) { term_free(&tb->t); free(tb); return NULL; }
    }
    Term *t = &tb->t;
    vt_feed(t, [snap bytes], (int)[snap length]);
#define UNPACK_NUMBER(field) t->field = [d[@#field] longLongValue];
    MAC_TERM_NUMBERS(UNPACK_NUMBER)
#undef UNPACK_NUMBER
#define UNPACK_STRING(field) snprintf(t->field, sizeof(t->field), "%s", [d[@#field] UTF8String] ?: "");
    MAC_TERM_STRINGS(UNPACK_STRING)
#undef UNPACK_STRING
    if (t->st < S_GROUND || t->st > S_STR_ESC || t->np < 0 || t->np > MAX_PARAMS || t->utf8_need < 0 || t->utf8_need > 3 || t->osc_n != (int)[osc length]) {
        term_free(t); free(tb); return NULL;
    }
    if ([osc length]) {          /* the OSC buffer grows on demand: give it room for what was pending */
        t->osc_cap = (int)[osc length] + 1024;
        if (!(t->osc = malloc((size_t)t->osc_cap))) { term_free(t); free(tb); return NULL; }
        memcpy(t->osc, [osc bytes], [osc length]);
    }
    int j = 0;
    for (NSNumber *v in d[@"params"]) t->params[j++] = [v intValue];
    t->nmarks = 0;
    for (NSNumber *v in d[@"marks"]) t->marks[t->nmarks++] = t->hpushed - [v longLongValue];
    j = 0;
    for (NSDictionary *task in d[@"tasks"]) {
        snprintf(t->ps_task_id[j], sizeof(t->ps_task_id[j]), "%s", [task[@"id"] UTF8String] ?: "");
        t->ps_task_on[j++] = [task[@"on"] intValue];
    }
    utf8_to_wide([d[@"title"] UTF8String] ?: "", t->title, 256);
    utf8_to_wide([d[@"ps_msg"] UTF8String] ?: "", t->ps_msg, 200);
    t->view = [d[@"view"] intValue];
    if (t->view < 0) t->view = 0;
    if (t->view > t->hcount) t->view = t->hcount;
    tb->pid = [d[@"pid"] intValue]; tb->fd = fd;
    tb->exited = [d[@"exited"] intValue]; tb->help = [d[@"help"] intValue];
    tb->activity = [d[@"activity"] intValue]; tb->attention = [d[@"attention"] intValue];
    tb->born = [d[@"born"] unsignedIntValue];
    snprintf(tb->start_dir, sizeof(tb->start_dir), "%s", [d[@"dir"] UTF8String] ?: "");
    snprintf(tb->run, sizeof(tb->run), "%s", [d[@"run"] UTF8String] ?: "");
    snprintf(tb->hist, sizeof(tb->hist), "%s", [d[@"hist"] UTF8String] ?: "");
    tb->claude_guard = [d[@"claude_guard"] intValue];
    tb->claude_released = [d[@"claude_released"] intValue];
    tb->claude_cursor_sent = [d[@"claude_cursor_sent"] intValue];
    tb->claude_guard_at = [d[@"claude_guard_at"] unsignedIntValue];
    return tb;
}

static int mac_reload(int ack, char *why, size_t cap) {
    if(mac_record_active()){snprintf(why,cap,"save or discard the camera recording before updating");return 0;}
    if(mac_record_exports_active()){snprintf(why,cap,"wait for clip exports to finish before updating");return 0;}
    if (!mac_probe(g_mac_exe)) { snprintf(why, cap, "replacement is missing or does not support live updates"); return 0; }
    /* A blocked paste may wait indefinitely. Do not discard it or freeze UI. */
    for (int i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        if (mac_input_held(tb)) {
            snprintf(why, cap, "a Claude draft is still being restored; retry after verification"); return 0;
        }
        if (tb->wpending && __atomic_load_n(tb->wpending, __ATOMIC_ACQUIRE)) {
            snprintf(why, cap, "a paste is still pending; try again when it finishes"); return 0;
        }
    }
    NSMutableArray *tabs = [NSMutableArray array];
    for (int i = 0; i < g_ntabs; i++) [tabs addObject:mac_pack_tab(g_tabs[i])];
    for (int i = 0; i < g_ntabs; i++) if (g_tabs[i]->wpending && __atomic_load_n(g_tabs[i]->wpending, __ATOMIC_ACQUIRE)) {
        snprintf(why, cap, "terminal replies are pending; try again"); return 0;
    }
    NSDictionary *state = @{
        @"protocol": @1, @"tabs": tabs, @"active": @(g_active), @"frame": NSStringFromRect([g_win frame]),
        @"spare_pid": @(g_spare ? g_spare->pid : 0),
        @"focused": @([NSApp isActive]), @"miniaturized": @([g_win isMiniaturized]),
        @"fullscreen": @(([g_win styleMask] & NSWindowStyleMaskFullScreen) != 0),
        @"settings_open": @(g_settings_open), @"settings_tab": @(g_settings_tab),
        @"music": mac_music_view_pack(),
        @"grid": mac_grid_pack(),
        @"tab_offset": @(g_tab_offset),
        @"setting_offsets": @[@(g_setting_offset[0]), @(g_setting_offset[1]), @(g_setting_offset[2])],
        @"exe": g_mac_exe, @"target": g_mac_target
    };
    /* The suffix belongs outside the .app bundle; construct its executable explicitly. */
    NSMutableDictionary *saved = [state mutableCopy];
    saved[@"fallback"] = [[g_mac_target pathExtension] isEqualToString:@"app"] ?
        [[g_mac_target stringByAppendingString:@".previous"] stringByAppendingPathComponent:@"Contents/MacOS/hardcore"] :
        [g_mac_target stringByAppendingString:@".previous"];
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:saved format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
    [saved release];
    FILE *file = tmpfile();
    if (!data || !file) { if (file) fclose(file); snprintf(why, cap, "cannot save handoff state"); return 0; }
    int fd = fileno(file);
    fchmod(fd, 0600);
    if (!mac_write_all(fd, [data bytes], [data length]) || lseek(fd, 0, SEEK_SET) < 0) {
        fclose(file); snprintf(why, cap, "cannot write handoff state"); return 0;
    }
    for (int i = 0; i < g_ntabs; i++) if (g_tabs[i]->fd >= 0) fcntl(g_tabs[i]->fd, F_SETFD, 0);
    fcntl(fd, F_SETFD, 0); fcntl(ack, F_SETFD, 0);
    char state_arg[32], ack_arg[32];
    snprintf(state_arg, sizeof(state_arg), "%d", fd); snprintf(ack_arg, sizeof(ack_arg), "%d", ack);
    execl([g_mac_exe fileSystemRepresentation], [g_mac_exe fileSystemRepresentation], "--mac-adopt", state_arg, ack_arg, (char *)NULL);
    int err = errno;
    for (int i = 0; i < g_ntabs; i++) if (g_tabs[i]->fd >= 0) fcntl(g_tabs[i]->fd, F_SETFD, FD_CLOEXEC);
    fcntl(ack, F_SETFD, FD_CLOEXEC); fclose(file);
    snprintf(why, cap, "exec failed: %s (existing sessions retained)", strerror(err)); return 0;
}
static int mac_adopt_read(void) {
    struct stat st;
    if (fstat(g_mac_state_fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 1 || st.st_size > 1024LL*1024*1024) return 0;
    lseek(g_mac_state_fd, 0, SEEK_SET);
    NSMutableData *data = [NSMutableData dataWithLength:(NSUInteger)st.st_size];
    size_t done = 0;
    while (done < [data length]) {
        ssize_t n = read(g_mac_state_fd, (char *)[data mutableBytes]+done, [data length]-done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        done += (size_t)n;
    }
    id state = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:NULL error:nil];
    if (![state isKindOfClass:[NSDictionary class]] || [state[@"protocol"] intValue] != 1 ||
        ![state[@"tabs"] isKindOfClass:[NSArray class]] || [state[@"tabs"] count] > MAX_TABS) return 0;
    g_mac_adopt = [state retain];
    for (NSDictionary *d in state[@"tabs"]) {
        Tab *tb = mac_unpack_tab(d);
        if (!tb) return 0;
        if (tab_by_id(tb->id)) { term_free(&tb->t); free(tb); return 0; }
        g_tabs[g_ntabs++] = tb;
    }
    g_active = [state[@"active"] intValue];
    if (g_active < 0 || g_active >= g_ntabs) g_active = 0;
    g_settings_open = [state[@"settings_open"] boolValue]; g_settings_tab = [state[@"settings_tab"] intValue];
    if (g_settings_tab < 0 || g_settings_tab > 1) g_settings_tab = 0;
    g_tab_offset = [state[@"tab_offset"] intValue];
    NSArray *offsets = state[@"setting_offsets"];
    if ([offsets isKindOfClass:[NSArray class]] && [offsets count] == 3)
        for (int i = 0; i < 3; i++) g_setting_offset[i] = [offsets[i] intValue];
    g_layout_active = g_active; g_layout_ntabs = g_ntabs;
    g_mac_exe = [state[@"exe"] copy]; g_mac_target = [state[@"target"] copy];
    /* A rollback runs the .previous image, but still belongs to the original
       installation for subsequent discovery and another reload attempt. */
    [g_mac_install_exe release]; g_mac_install_exe = [g_mac_exe copy];
    snprintf(g_mac_bin_dir, sizeof(g_mac_bin_dir), "%s", [[g_mac_exe stringByDeletingLastPathComponent] fileSystemRepresentation]);
    for (int i = 0; i < g_ntabs; i++) if (g_tabs[i]->fd >= 0) {
        DWORD born = g_tabs[i]->born;
        tab_start_io(g_tabs[i]);
        g_tabs[i]->born = born;
        /* A failed renderer may still exec the previous image. Keep every
           inherited PTY through that fallback until mac_adopt_ready commits. */
        fcntl(g_tabs[i]->fd, F_SETFD, 0);
    }
    return 1;
}
static void mac_adopt_failure(const char *why) {
    if (g_mac_adopt && !getenv("HARDCORE_MAC_ROLLBACK")) {
        NSString *fallback = g_mac_adopt[@"fallback"];
        if (access([fallback fileSystemRepresentation], X_OK) == 0) {
            char a[32], b[32]; snprintf(a, sizeof(a), "%d", g_mac_state_fd); snprintf(b, sizeof(b), "%d", g_mac_ack_fd);
            setenv("HARDCORE_MAC_ROLLBACK", "1", 1);
            execl([fallback fileSystemRepresentation], [fallback fileSystemRepresentation], "--mac-adopt", a, b, (char *)NULL);
        }
    }
    if (g_mac_ack_fd >= 0) { dprintf(g_mac_ack_fd, "error %s\n", why); close(g_mac_ack_fd); }
}
/* exec closes the unused spare's CLOEXEC master, but the child still belongs
   to this PID. Preserve its identity so the replacement reaps it as well. */
static void mac_reap_previous_spare(pid_t pid) {
    if (pid <= 1) return;
    int status;
    pid_t waited;
    do { waited = waitpid(pid, &status, WNOHANG); } while (waited < 0 && errno == EINTR);
    if (waited != 0) return;
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)pid,
        DISPATCH_PROC_EXIT, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (!source) return;
    dispatch_source_set_event_handler(source, ^{
        pid_t result;
        do { result = waitpid(pid, NULL, WNOHANG); } while (result < 0 && errno == EINTR);
        if (result == 0) return;
        dispatch_source_cancel(source);
        dispatch_release(source);
    });
    dispatch_resume(source);
}

static void mac_adopt_ready(void) {
    if (!g_mac_adopt) return;
    mac_reap_previous_spare([g_mac_adopt[@"spare_pid"] intValue]);
    for (int i = 0; i < g_ntabs; i++) if (g_tabs[i]->fd >= 0) fcntl(g_tabs[i]->fd, F_SETFD, FD_CLOEXEC);
    close(g_mac_state_fd); g_mac_state_fd = -1;
    if ([g_mac_adopt[@"miniaturized"] boolValue]) [g_win miniaturize:nil];
    if ([g_mac_adopt[@"fullscreen"] boolValue]) [g_win toggleFullScreen:nil];
    if (g_mac_ack_fd >= 0) {
        dprintf(g_mac_ack_fd, getenv("HARDCORE_MAC_ROLLBACK") ? "error rolled back to the previous executable; sessions retained\n" : "ok\n");
        close(g_mac_ack_fd); g_mac_ack_fd = -1;
    }
    unsetenv("HARDCORE_MAC_ROLLBACK");
    update_title(); update_busy(1); schedule_spare();
}
static NSData *mac_request(NSString *socket_path, const char *command, int seconds) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return nil;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct sockaddr_un addr = {0}; addr.sun_family = AF_UNIX;
    if ([socket_path lengthOfBytesUsingEncoding:NSUTF8StringEncoding] >= sizeof(addr.sun_path)) { close(fd); return nil; }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", [socket_path fileSystemRepresentation]);
    struct timeval tv = {seconds, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) || !mac_peer_ok(fd) || !mac_write_all(fd, command, strlen(command))) { close(fd); return nil; }
    NSMutableData *data = [NSMutableData data]; char buf[4096]; ssize_t n;
    while ([data length] < 1024*1024) {
        n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        [data appendBytes:buf length:n];
    }
    close(fd); return data;
}
static int mac_reload_windows(void) {
    NSString *dir = mac_runtime_dir(); int ok = 0, bad = 0;
    if (!dir) { fprintf(stderr, "hardcore: update IPC directory unavailable\n"); return 1; }
    for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]) {
        if (![[name pathExtension] isEqualToString:@"sock"]) continue;
        NSString *path = [dir stringByAppendingPathComponent:name];
        NSData *info = mac_request(path, "info\n", 2);
        NSDictionary *d = info ? [NSJSONSerialization JSONObjectWithData:info options:0 error:nil] : nil;
        if (![d isKindOfClass:[NSDictionary class]]) {
            /* Early slurp-enabled builds only understand plist/FD handoff.
               Reload those once through their original safe transfer path. */
            int peer = mac_connect(path);
            NSDictionary *legacy = peer >= 0 && mac_send(peer, @{@"op": @"list"}) ? mac_recv(peer) : nil;
            if (peer >= 0) close(peer);
            if ([legacy[@"protocol"] intValue] != MAC_HANDOFF_VERSION ||
                ![legacy[@"exe"] isEqual:g_mac_exe] || ![legacy[@"tabs"] count]) continue;
            peer = mac_connect(path);
            NSDictionary *result = peer >= 0 && mac_send(peer, @{@"op": @"reload", @"exe": g_mac_exe}) ? mac_recv(peer) : nil;
            if (peer >= 0) close(peer);
            if ([result[@"ok"] boolValue]) ok++;
            else { bad++; fprintf(stderr, "hardcore: legacy window %s did not reload; check the original window\n", [name UTF8String]); }
            continue;
        }
        if (![d[@"exe"] isEqualToString:g_mac_exe]) continue;
        NSData *reply = mac_request(path, "reload\n", 30);
        NSString *s = reply ? [[[NSString alloc] initWithData:reply encoding:NSUTF8StringEncoding] autorelease] : nil;
        if ([s isEqualToString:@"ok\n"]) ok++;
        else { bad++; fprintf(stderr, "hardcore: window %s: %s\n", [name UTF8String], [s UTF8String] ?: "no acknowledgement; check the window"); }
    }
    printf("hardcore: %d window%s reloaded in place%s\n", ok, ok == 1 ? "" : "s", bad ? "; some windows could not reload" : "");
    return bad ? 1 : 0;
}

static long mac_version(NSString *s) {
    int a=0,b=0,c=0; const char *v = [s UTF8String]; if (!v) return -1;
    if (*v == 'v') v++;
    if (sscanf(v, "%d.%d.%d", &a, &b, &c) < 2) return -1;
    return a*1000000L+b*1000L+c;
}
static int mac_download(NSString *url, NSString *path) {
    if (![[[NSURL URLWithString:url] scheme] isEqualToString:@"https"]) return 0;
    NSString *err = nil;
    int ok = mac_run(@"/usr/bin/curl", @[@"--fail", @"--location", @"--proto", @"=https", @"--proto-redir", @"=https", @"--max-time", @"120", @"--silent", @"--show-error", @"-H", @"Accept: application/vnd.github+json", @"-o", path, url], 125, &err);
    if (!ok) fprintf(stderr, "hardcore: download failed: %s\n", [err UTF8String]);
    return ok;
}
static NSString *mac_digest(NSString *path) {
    FILE *f = fopen([path fileSystemRepresentation], "rb"); if (!f) return nil;
    CC_SHA256_CTX ctx; CC_SHA256_Init(&ctx);
    unsigned char buf[65536], hash[CC_SHA256_DIGEST_LENGTH]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f))) CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    int ok = !ferror(f); fclose(f); if (!ok) return nil;
    CC_SHA256_Final(hash, &ctx); char hex[65];
    for (int i=0;i<32;i++) sprintf(hex+2*i, "%02x", hash[i]);
    return [NSString stringWithUTF8String:hex];
}
static int mac_install(NSString *source) {
    NSFileManager *fm = [NSFileManager defaultManager];
    BOOL bundle = [[g_mac_target pathExtension] isEqualToString:@"app"];
    if (bundle != [[source pathExtension] isEqualToString:@"app"]) { fprintf(stderr, "hardcore: update source must match the installation (app bundle or bare executable)\n"); return 1; }
    NSString *stage = [g_mac_target stringByAppendingFormat:@".stage-%@", [[NSUUID UUID] UUIDString]];
    NSString *backup = [g_mac_target stringByAppendingString:@".previous"];
    NSString *exe = bundle ? [stage stringByAppendingPathComponent:@"Contents/MacOS/hardcore"] : stage;
    NSError *err = nil;
    if (![fm copyItemAtPath:source toPath:stage error:&err]) { fprintf(stderr, "hardcore: cannot stage update: %s\n", [[err localizedDescription] UTF8String]); return 1; }
    if ((bundle && !mac_run(@"/usr/bin/codesign", @[@"--verify", @"--deep", @"--strict", stage], 15, NULL)) || !mac_probe(exe)) {
        fprintf(stderr, "hardcore: replacement failed compatibility/signature checks; installation unchanged\n");
        [fm removeItemAtPath:stage error:nil]; return 1;
    }
    if ([fm fileExistsAtPath:backup] && ![fm removeItemAtPath:backup error:&err]) {
        fprintf(stderr, "hardcore: cannot remove old backup\n"); [fm removeItemAtPath:stage error:nil]; return 1;
    }
    if (rename([g_mac_target fileSystemRepresentation], [backup fileSystemRepresentation])) {
        fprintf(stderr, "hardcore: cannot replace installation: %s\n", strerror(errno)); [fm removeItemAtPath:stage error:nil]; return 1;
    }
    if (rename([stage fileSystemRepresentation], [g_mac_target fileSystemRepresentation])) {
        int e = errno; rename([backup fileSystemRepresentation], [g_mac_target fileSystemRepresentation]);
        fprintf(stderr, "hardcore: cannot install replacement: %s\n", strerror(e)); [fm removeItemAtPath:stage error:nil]; return 1;
    }
    printf("hardcore: installed %s (previous copy at %s)\n", [g_mac_target fileSystemRepresentation], [backup fileSystemRepresentation]);
    return mac_reload_windows();
}
static int mac_update_cmd(int argc, char **argv) {
    int force = 0; NSString *source = nil;
    for (int i=0;i<argc;i++) {
        if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "--from") && i+1<argc) source = [[NSString stringWithUTF8String:argv[++i]] stringByStandardizingPath];
        else { fprintf(stderr, "usage: hardcore update [--force] [--from PATH]\n"); return 1; }
    }
    NSString *lockPath = [g_mac_target stringByAppendingString:@".update-lock"];
    int lock = open([lockPath fileSystemRepresentation], O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX|LOCK_NB)) { if (lock >= 0) close(lock); fprintf(stderr, "hardcore: another update is running, or installation is not writable\n"); return 1; }
    if (source) { int rc = mac_install([source stringByResolvingSymlinksInPath]); close(lock); return rc; }
    if (![[g_mac_target pathExtension] isEqualToString:@"app"]) { fprintf(stderr, "hardcore: release updates require an app bundle; use update --from PATH for a bare executable\n"); close(lock); return 1; }
    char tmp[] = "/tmp/hardcore-update.XXXXXX";
    if (!mkdtemp(tmp)) { close(lock); return 1; }
    NSString *dir = [NSString stringWithUTF8String:tmp], *jsonPath = [dir stringByAppendingPathComponent:@"release.json"];
    int rc = 1;
    printf("hardcore %s: checking GitHub for the latest macOS release...\n", HARDCORE_VERSION);
    if (!mac_download(@"https://api.github.com/repos/williamsharkey/hardcore/releases/latest", jsonPath)) goto done;
    {
        NSDictionary *release = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:jsonPath] options:0 error:nil];
        if (![release isKindOfClass:[NSDictionary class]] || mac_version(release[@"tag_name"]) < 0) { fprintf(stderr, "hardcore: invalid release metadata\n"); goto done; }
        if (!force && mac_version(release[@"tag_name"]) <= mac_version(@HARDCORE_VERSION)) { printf("hardcore: up to date (%s)\n", HARDCORE_VERSION); rc = 0; goto done; }
        NSDictionary *asset = nil;
        for (NSDictionary *a in release[@"assets"]) if ([a[@"name"] isEqualToString:@"hardcore-macos.zip"]) asset = a;
        NSString *digest = asset[@"digest"];
        if (!asset || ![digest isKindOfClass:[NSString class]] || ![digest hasPrefix:@"sha256:"] || [digest length] != 71) {
            fprintf(stderr, "hardcore: release has no hardcore-macos.zip with a SHA-256 digest; installation unchanged\n"); goto done;
        }
        NSString *zip = [dir stringByAppendingPathComponent:@"release.zip"];
        if (!mac_download(asset[@"browser_download_url"], zip)) goto done;
        if (![[mac_digest(zip) lowercaseString] isEqualToString:[[digest substringFromIndex:7] lowercaseString]]) { fprintf(stderr, "hardcore: archive checksum mismatch\n"); goto done; }
        NSString *unpack = [dir stringByAppendingPathComponent:@"unpacked"];
        if (!mac_run(@"/usr/bin/ditto", @[@"-x", @"-k", zip, unpack], 30, NULL)) { fprintf(stderr, "hardcore: cannot unpack release\n"); goto done; }
        rc = mac_install([unpack stringByAppendingPathComponent:@"hardcore.app"]);
    }
done:
    [[NSFileManager defaultManager] removeItemAtPath:dir error:nil]; close(lock); return rc;
}

/* Verify the serialized protocol, including a split UTF-8 character and
   split OSC/CSI sequences. These are the bytes arriving during a reload. */
static int mac_update_unit(FILE *f) {
    const char *tails[] = {"\xe2\x94", "\x1b[38;2;11;22", "\x1b]0;half a title"};
    const char *rest[] = {"\x80", ";33mX", " finished\x07"};
    int fails = 0;
    for (int k=0;k<3;k++) {
        Tab *a = calloc(1, sizeof(Tab));
        if (!a || !term_init(&a->t, 40, 6)) { free(a); return fails+1; }
        a->fd = -1; a->id = 100+k;
        for (int j=0;j<30;j++) vt_feed(&a->t, (const unsigned char *)"history\r\n", 9);
        const char *alt = "\x1b[?1049h\x1b[?2004h\x1b[?1006h\x1b[?1000hALT";
        vt_feed(&a->t, (const unsigned char *)alt, (int)strlen(alt));
        vt_feed(&a->t, (const unsigned char *)tails[k], (int)strlen(tails[k]));
        snprintf(a->t.cwd, sizeof(a->t.cwd), "/tmp/space here");
        snprintf(a->run, sizeof(a->run), "claude-wait:/tmp/café/request.json");
        snprintf(a->hist, sizeof(a->hist), "/tmp/handoff helper.py");
        a->claude_guard = 1; a->claude_released = 1; a->claude_cursor_sent = 1;
        a->claude_guard_at = 12345; a->born = 100;
        a->t.view = 2; a->t.marks[a->t.nmarks++] = a->t.hpushed - 3;
        NSData *data = [NSPropertyListSerialization dataWithPropertyList:mac_pack_tab(a) format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
        NSDictionary *decoded = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:NULL error:nil];
        Tab *b = mac_unpack_tab(decoded);
        int ok = b != NULL;
        if (b) {
            ok = b->id == a->id && b->t.view == a->t.view && b->t.hcount == a->t.hcount && b->t.st == a->t.st &&
                 b->t.utf8_need == a->t.utf8_need && b->t.m_bracketed && b->t.m_mouse == 1000 &&
                 b->t.hpushed-b->t.marks[0] == a->t.hpushed-a->t.marks[0] && !strcmp(a->t.cwd,b->t.cwd) &&
                 !strcmp(a->run, b->run) && !strcmp(a->hist, b->hist) && b->born == a->born &&
                 b->claude_guard && b->claude_released && b->claude_cursor_sent && b->claude_guard_at == a->claude_guard_at;
            vt_feed(&a->t, (const unsigned char *)rest[k], (int)strlen(rest[k]));
            vt_feed(&b->t, (const unsigned char *)rest[k], (int)strlen(rest[k]));
            ok = ok && a->t.st == b->t.st && a->t.cx == b->t.cx && !wcscmp(a->t.title,b->t.title);
            for (int screen=0;screen<2;screen++) for (int y=0;y<6;y++) for (int x=0;x<40;x++)
                ok = ok && cell_same(&a->t, &a->t.row[screen][y][x], &b->t, &b->t.row[screen][y][x]);
            term_free(&b->t); free(b);
        }
        fprintf(f, "%s mac update snapshot/parser #%d\n", ok ? "ok  " : "FAIL", k);
        fails += !ok; term_free(&a->t); free(a);
    }
    Tab *browser=calloc(1,sizeof(Tab));
    if (!browser || !term_init(&browser->t,40,6)) { free(browser); return fails+1; }
    browser->fd=-1; browser->id=200; browser->parent_id=100; browser->kind=TK_BROWSER; browser->browser_native=1;
    snprintf(browser->browser_url,sizeof(browser->browser_url),"https://example.com/a?b=c");
    snprintf(browser->browser_site,sizeof(browser->browser_site),"Example");
    snprintf(browser->browser_target,sizeof(browser->browser_target),"A1B2C3D4");
    utf8_to_wide("Page title - Example",browser->t.title,256);
    Tab *restored=mac_unpack_tab(mac_pack_tab(browser));
    int ok=restored && restored->id==200 && restored->parent_id==100 && restored->kind==TK_BROWSER &&
        restored->browser_native==1 && !restored->browser && restored->fd==-1 && restored->pid==0 &&
        !strcmp(restored->browser_url,browser->browser_url) && !strcmp(restored->browser_site,browser->browser_site) &&
        !strcmp(restored->browser_target,browser->browser_target) &&
        !wcscmp(restored->t.title,browser->t.title);
    fprintf(f,"%s browser reload metadata, stable IDs and lazy engine restoration\n",ok ? "ok  " : "FAIL");
    fails+=!ok;
    if (restored) { term_free(&restored->t); free(restored); }
    for(int marker=0;marker<=3;marker++) {
        browser->browser_recording=marker;
        restored=mac_unpack_tab(mac_pack_tab(browser));
        ok=restored && restored->browser_recording==(marker<=2?marker:0);
        fprintf(f,"%s Clips reload host marker %d\n",ok ? "ok  " : "FAIL",marker);fails+=!ok;
        if(restored){term_free(&restored->t);free(restored);}
    }
    NSMutableDictionary *legacy=[[mac_pack_tab(browser) mutableCopy] autorelease];[legacy removeObjectForKey:@"browser_recording"];
    restored=mac_unpack_tab(legacy);ok=restored && !restored->browser_recording;
    fprintf(f,"%s old browser snapshots do not acquire a Clips bridge\n",ok ? "ok  " : "FAIL");fails+=!ok;
    if(restored){term_free(&restored->t);free(restored);}
    term_free(&browser->t); free(browser);
    return fails;
}
