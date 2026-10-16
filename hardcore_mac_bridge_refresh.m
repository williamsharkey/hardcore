/* One-shot recovery for an already-running Terminal mirror helper. This never
   restarts the source Terminal job or native Hardcore shells. Included by the
   transfer server; the request is deliberately scoped to an exact helper PID. */
typedef struct {
    int tab_id, fd, ready, cols, rows;
    pid_t old_pid, pid;
    unsigned long long write_seq;
} MacBridgeRefresh;

static Tab *mac_bridge_refresh_target(int id, pid_t pid) {
    Tab *tb = tab_by_id(id);
    if (!tb || tb->pid != pid || tb->exited || tb->fd < 0 ||
        strncmp(tb->run, "terminal:/dev/", 14) || mac_input_held(tb) ||
        (tb->wpending && __atomic_load_n(tb->wpending, __ATOMIC_ACQUIRE)) ||
        tcgetpgrp(tb->fd) != pid || kill(pid, 0)) return NULL;
    return tb;
}

/* Main queue only. The candidate owns a separate, initially empty PTY and
   cannot forward input until its readiness socket receives GO. */
static int mac_bridge_refresh_stage(pid_t pid, MacBridgeRefresh *r) {
    memset(r, 0, sizeof(*r)); r->fd = r->ready = -1;
    Tab *tb = NULL;
    for (int i = 0; i < g_ntabs; i++)
        if (g_tabs[i]->pid == pid) tb = mac_bridge_refresh_target(g_tabs[i]->id, pid);
    if (!tb) return 0;
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 0;
    fcntl(pair[0], F_SETFD, FD_CLOEXEC);
    fcntl(pair[1], F_SETFD, FD_CLOEXEC);
    struct winsize ws = {0};
    ws.ws_col = (unsigned short)tb->t.cols; ws.ws_row = (unsigned short)tb->t.rows;
    ws.ws_xpixel = (unsigned short)(tb->t.cols * CW); ws.ws_ypixel = (unsigned short)(tb->t.rows * CH);
    pid_t child = forkpty(&r->fd, NULL, NULL, &ws);
    if (child < 0) { close(pair[0]); close(pair[1]); return 0; }
    if (!child) {
        close(pair[0]);
        child_env();
        char descriptor[32]; snprintf(descriptor, sizeof(descriptor), "%d", pair[1]);
        setenv("HARDCORE_BRIDGE_READY_FD", descriptor, 1);
        fcntl(pair[1], F_SETFD, 0);
        execl(g_mac_exe_path, "hardcore", "--terminal-bridge", tb->run + 9, tb->hist, (char *)NULL);
        _exit(127);
    }
    close(pair[1]);
    fcntl(r->fd, F_SETFD, FD_CLOEXEC);
    r->ready = pair[0]; r->pid = child; r->old_pid = pid; r->tab_id = tb->id;
    r->write_seq = tb->write_seq;
    r->cols = tb->t.cols; r->rows = tb->t.rows;
    return 1;
}

/* Worker queue only. A replacement must have successfully read the original
   Terminal tab and become a background process before the old view is touched. */
static int mac_bridge_refresh_wait(MacBridgeRefresh *r) {
    struct pollfd p = {r->ready, POLLIN, 0};
    int result = 0;
    double until = uptime_ms() + 5000;
    for (;;) {
        double remaining = until - uptime_ms();
        if (remaining <= 0) break;
        result = poll(&p, 1, (int)remaining + 1);
        if (result >= 0 || errno != EINTR) break;
    }
    char byte = 0;
    return result > 0 && (p.revents & POLLIN) && read(r->ready, &byte, 1) == 1 && byte == 'R';
}

/* Main queue only. Change identity before the old PROC_EXIT callback runs;
   that callback still reaps its helper but cannot close the replacement tab. */
static int mac_bridge_refresh_commit(MacBridgeRefresh *r) {
    Tab *tb = mac_bridge_refresh_target(r->tab_id, r->old_pid);
    if (!tb || tb->write_seq != r->write_seq || tb->t.cols != r->cols || tb->t.rows != r->rows || kill(r->pid, 0)) return 0;
    read_some(tb); tab_drain(tb);
    if (!mac_bridge_refresh_target(r->tab_id, r->old_pid) || tb->write_seq != r->write_seq) return 0;
    if (send(r->ready, "G", 1, 0) != 1) return 0;
    close(r->ready); r->ready = -1;
    stop_reading(tb);
    if (tb->wq) dispatch_release((dispatch_queue_t)tb->wq);
    tb->rsrc = tb->psrc = tb->wq = NULL; tb->wpending = NULL;
    tb->id = g_next_id++;
    tb->fd = r->fd; tb->pid = r->pid;
    r->fd = -1; r->pid = 0;
    tab_start_io(tb);
    /* The bridge's own PID, never its Terminal source PID or process group. */
    kill(r->old_pid, SIGHUP);
    update_title(); plat_invalidate();
    return 1;
}

/* Worker queue only; the candidate is our child and has never been published.
   Failure closes only its descriptors, leaving the original tab untouched. */
static void mac_bridge_refresh_discard(MacBridgeRefresh *r) {
    if (r->ready >= 0) close(r->ready);
    if (r->fd >= 0) close(r->fd);
    if (r->pid > 0) {
        kill(r->pid, SIGKILL);
        while (waitpid(r->pid, NULL, 0) < 0 && errno == EINTR) {}
    }
}

static NSDictionary *mac_bridge_refresh(NSDictionary *req) {
    NSNumber *number = req[@"helperPid"];
    if (![number isKindOfClass:[NSNumber class]] || number.longLongValue <= 1 || number.longLongValue > INT_MAX ||
        number.doubleValue != (double)number.longLongValue)
        return @{@"ok": @NO, @"error": @"An exact Terminal bridge helper PID is required."};
    __block MacBridgeRefresh r;
    __block int staged = 0, committed = 0;
    dispatch_sync(dispatch_get_main_queue(), ^{ staged = mac_bridge_refresh_stage(number.intValue, &r); });
    if (!staged) return @{@"ok": @NO, @"error": @"That helper is not an idle, live Terminal bridge; original tab retained."};
    pid_t replacement = r.pid;
    if (mac_bridge_refresh_wait(&r))
        dispatch_sync(dispatch_get_main_queue(), ^{ committed = mac_bridge_refresh_commit(&r); });
    mac_bridge_refresh_discard(&r);
    return committed ? @{@"ok": @YES, @"oldHelperPid": number, @"helperPid": @(replacement)} :
        @{@"ok": @NO, @"error": @"Replacement did not become ready or the original tab changed; original tab retained."};
}
