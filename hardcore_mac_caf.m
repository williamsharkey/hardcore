/* Window-owned sleep inhibition. Include from the macOS platform layer.
 *
 * The child is the system caffeinate utility, never an existing caf process.
 * -w also releases assertions if Hardcore crashes or is killed. The private
 * descriptor survives exec-based updates; PID, parent, executable and start
 * time must all match before adoption or signaling. Normal exit runs atexit;
 * exec deliberately does not, so an update does not interrupt the assertion.
 * All public helpers run on the main thread. Power queries run separately.
 */
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef HARDCORE_CAF_CHANGED
#define HARDCORE_CAF_CHANGED() ((void)0)
#endif
#define MAC_CAF_ENV "HARDCORE_CAF_CHILD"

typedef struct {
    pid_t owner, pid;
    uint64_t start_sec, start_usec, began;
    dispatch_source_t watch;
} MacCaf;
static MacCaf g_mac_caf;
static int g_mac_caf_initialized, g_mac_caf_exiting;
static void mac_caf_stop(void);
static int mac_caf_active(void);

static uint64_t mac_caf_now(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec*1000000000ULL + (uint64_t)now.tv_nsec;
}

static int mac_caf_identity(pid_t pid, uint64_t *sec, uint64_t *usec) {
    struct proc_bsdinfo info;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (pid <= 1 || proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info) ||
        info.pbi_ppid != (uint32_t)getpid() || info.pbi_uid != getuid() ||
        proc_pidpath(pid, path, sizeof(path)) <= 0 || strcmp(path, "/usr/bin/caffeinate")) return 0;
    *sec = info.pbi_start_tvsec; *usec = info.pbi_start_tvusec;
    return 1;
}

static int mac_caf_matches(void) {
    uint64_t sec, usec;
    return g_mac_caf.owner == getpid() &&
           mac_caf_identity(g_mac_caf.pid, &sec, &usec) &&
           sec == g_mac_caf.start_sec && usec == g_mac_caf.start_usec;
}

static void mac_caf_forget(void) {
    if (g_mac_caf.watch) {
        dispatch_source_cancel(g_mac_caf.watch);
        dispatch_release(g_mac_caf.watch);
        g_mac_caf.watch = NULL;
    }
    g_mac_caf.pid = 0;
    g_mac_caf.start_sec = g_mac_caf.start_usec = g_mac_caf.began = 0;
    unsetenv(MAC_CAF_ENV);
    if (!g_mac_caf_exiting) HARDCORE_CAF_CHANGED();
}

static void mac_caf_watch(void) {
    pid_t pid = g_mac_caf.pid;
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC,
        (uintptr_t)pid, DISPATCH_PROC_EXIT, dispatch_get_main_queue());
    if (!source) return; /* mac_caf_active still checks and reaps on every poll. */
    g_mac_caf.watch = source;
    dispatch_source_set_event_handler(source, ^{
        if (g_mac_caf.pid == pid) (void)mac_caf_active();
    });
    dispatch_resume(source);
}

static void mac_caf_cleanup(void) {
    /* A forked shell inherits atexit handlers until exec. It must never stop
       its parent's assertion if its own shell exec fails. */
    if (g_mac_caf.owner == getpid()) { g_mac_caf_exiting = 1; mac_caf_stop(); }
}

static int mac_caf_descriptor(const char *text, uint64_t fields[5]) {
    if (!text || !*text) return 0;
    for (int i = 0; i < 5; i++) {
        if (*text < '0' || *text > '9') return 0;
        errno = 0;
        char *end;
        unsigned long long value = strtoull(text, &end, 10);
        if (errno || end == text || *end != (i == 4 ? '\0' : ':')) return 0;
        fields[i] = (uint64_t)value;
        text = end + (i != 4);
    }
    return fields[0] == (uint64_t)getpid() && fields[1] > 1 && fields[1] <= INT_MAX &&
           fields[2] > 0 && fields[3] < 1000000 && fields[4] > 0 && fields[4] <= mac_caf_now();
}

static void mac_caf_init(void) {
    if (g_mac_caf_initialized) return;
    g_mac_caf_initialized = 1;
    g_mac_caf.owner = getpid();
    atexit(mac_caf_cleanup);
    uint64_t fields[5];
    if (mac_caf_descriptor(getenv(MAC_CAF_ENV), fields)) {
        g_mac_caf.pid = (pid_t)fields[1];
        g_mac_caf.start_sec = fields[2]; g_mac_caf.start_usec = fields[3];
        g_mac_caf.began = fields[4];
        if (mac_caf_matches()) { mac_caf_watch(); return; }
    }
    mac_caf_forget();
}

static int mac_caf_active(void) {
    mac_caf_init();
    if (!g_mac_caf.pid || g_mac_caf.owner != getpid()) return 0;
    pid_t waited;
    do { waited = waitpid(g_mac_caf.pid, NULL, WNOHANG); } while (waited < 0 && errno == EINTR);
    if (waited == g_mac_caf.pid || (waited < 0 && errno == ECHILD) || !mac_caf_matches()) {
        mac_caf_forget(); return 0;
    }
    return 1;
}

/* CLOEXEC_DEFAULT prevents PTYs, transfer sockets and credentials on unrelated
   descriptors from leaking into either system utility. Both use a minimal
   environment; in particular neither receives the adoption descriptor. */
static int mac_caf_spawn(pid_t *pid, const char *path, char *const argv[], int output) {
    posix_spawn_file_actions_t files;
    posix_spawnattr_t attrs;
    int error = posix_spawn_file_actions_init(&files);
    if (error) return error;
    error = posix_spawnattr_init(&attrs);
    if (error) { posix_spawn_file_actions_destroy(&files); return error; }
    if (!(error = posix_spawn_file_actions_addopen(&files, STDIN_FILENO, "/dev/null", O_RDONLY, 0)) &&
        !(error = posix_spawn_file_actions_addopen(&files, STDERR_FILENO, "/dev/null", O_WRONLY, 0))) {
        error = output >= 0 ? posix_spawn_file_actions_adddup2(&files, output, STDOUT_FILENO) :
            posix_spawn_file_actions_addopen(&files, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    sigset_t mask, defaults;
    sigemptyset(&mask); sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM); sigaddset(&defaults, SIGINT); sigaddset(&defaults, SIGHUP);
    if (!error) error = posix_spawnattr_setsigmask(&attrs, &mask);
    if (!error) error = posix_spawnattr_setsigdefault(&attrs, &defaults);
    if (!error) error = posix_spawnattr_setflags(&attrs, POSIX_SPAWN_CLOEXEC_DEFAULT |
        POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    if (!error) error = posix_spawnattr_setpgroup(&attrs, 0);
    char *environment[] = {"PATH=/usr/bin:/bin", "LC_ALL=C", NULL};
    if (!error) error = posix_spawn(pid, path, &files, &attrs, argv, environment);
    posix_spawnattr_destroy(&attrs);
    posix_spawn_file_actions_destroy(&files);
    return error;
}

static void mac_caf_stop(void) {
    if (!mac_caf_active()) return;
    pid_t pid = g_mac_caf.pid;
    /* A matched, unreaped child cannot have its PID reused by another process.
       Do not signal a descriptor that no longer matches that owned child. */
    if (mac_caf_matches()) kill(pid, SIGTERM);
    uint64_t deadline = mac_caf_now() + 100000000ULL;
    int reaped = 0, owned = 1;
    do {
        pid_t result = waitpid(pid, NULL, WNOHANG);
        if (result == pid) { reaped = 1; break; }
        if (result < 0 && errno != EINTR) {
            if (errno == ECHILD) owned = 0;
            break;
        }
        if (result == 0) usleep(2000);
    } while (mac_caf_now() < deadline);
    if (!reaped && owned && mac_caf_matches()) {
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    } else if (!reaped && owned) {
        while (waitpid(pid, NULL, WNOHANG) < 0 && errno == EINTR) {}
    }
    mac_caf_forget();
}

/* Return the resulting enabled state, or -1 if the system utility could not
   start. No external caf/caffeinate instance is toggled or terminated. */
static int mac_caf_toggle(void) {
    if (mac_caf_active()) { mac_caf_stop(); return 0; }
    char owner[32];
    snprintf(owner, sizeof(owner), "%ld", (long)getpid());
    char *argv[] = {"/usr/bin/caffeinate", "-di", "-w", owner, NULL};
    pid_t child;
    int error = mac_caf_spawn(&child, argv[0], argv, -1);
    if (error) { errno = error; return -1; }
    g_mac_caf.pid = child;
    g_mac_caf.began = mac_caf_now();
    if (!mac_caf_identity(child, &g_mac_caf.start_sec, &g_mac_caf.start_usec)) {
        /* posix_spawn created this exact unreaped child, so startup failure
           cleanup needs no descriptor or identity supplied by the caller. */
        kill(child, SIGTERM);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        mac_caf_forget(); return -1;
    }
    char descriptor[160];
    snprintf(descriptor, sizeof(descriptor), "%ld:%ld:%llu:%llu:%llu", (long)getpid(), (long)child,
        (unsigned long long)g_mac_caf.start_sec, (unsigned long long)g_mac_caf.start_usec,
        (unsigned long long)g_mac_caf.began);
    if (setenv(MAC_CAF_ENV, descriptor, 1)) { mac_caf_stop(); return -1; }
    mac_caf_watch();
    HARDCORE_CAF_CHANGED();
    return mac_caf_active() ? 1 : -1;
}

static double mac_caf_elapsed(void) {
    if (!mac_caf_active()) return 0;
    uint64_t now = mac_caf_now();
    return now > g_mac_caf.began ? (double)(now-g_mac_caf.began)/1e9 : 0;
}

static void mac_caf_parse_power(const char *output, char *text, size_t capacity) {
    const char *supply = strstr(output, "'AC Power'") ? "Plugged in" :
                         strstr(output, "'Battery Power'") ? "On battery" : NULL;
    if (!supply) { snprintf(text, capacity, "Power status unavailable"); return; }
    int percent = -1;
    for (const char *p = output; *p; p++) {
        if (*p != '%') continue;
        const char *start = p;
        while (start > output && start[-1] >= '0' && start[-1] <= '9') start--;
        if (start < p && p-start <= 3) {
            int n = atoi(start);
            if (n >= 0 && n <= 100) { percent = n; break; }
        }
    }
    if (percent < 0) snprintf(text, capacity, "%s", supply);
    else snprintf(text, capacity, "%s · %d%%%s", supply, percent,
                  strstr(output, "; charging;") ? " · charging" : "");
}

static pthread_mutex_t g_mac_caf_power_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_mac_caf_power_text[96] = "Power status unavailable";
static uint64_t g_mac_caf_power_checked;
static int g_mac_caf_power_pending;

/* Bounded, asynchronous pmset probe. A hung or malformed probe cannot block a
   terminal frame, keep inherited PTYs open, or accumulate concurrent children. */
static void mac_caf_query_power(char text[96]) {
    strcpy(text, "Power status unavailable");
    int pipes[2];
    if (pipe(pipes)) return;
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC); fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    char *argv[] = {"/usr/bin/pmset", "-g", "batt", NULL};
    pid_t child;
    int error = mac_caf_spawn(&child, argv[0], argv, pipes[1]);
    close(pipes[1]);
    if (error) { close(pipes[0]); return; }
    char output[4096]; size_t used = 0;
    uint64_t deadline = mac_caf_now() + 2000000000ULL;
    int reaped = 0, status = 0, overflow = 0, owned = 1;
    while (mac_caf_now() < deadline) {
        ssize_t got = read(pipes[0], output+used, sizeof(output)-1-used);
        int read_error = got < 0 ? errno : 0;
        if (got > 0) used += (size_t)got;
        if (used == sizeof(output)-1) { overflow = 1; break; }
        if (!reaped) {
            pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child) reaped = 1;
            else if (waited < 0 && errno != EINTR) {
                if (errno == ECHILD) owned = 0;
                break;
            }
        }
        if (reaped && got == 0) break;
        if (got < 0 && read_error != EAGAIN && read_error != EINTR) break;
        struct pollfd p = {pipes[0], POLLIN, 0};
        poll(&p, 1, 25);
    }
    close(pipes[0]);
    if (!reaped && owned) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        return;
    }
    if (!reaped) return;
    output[used] = 0;
    if (!overflow && WIFEXITED(status) && !WEXITSTATUS(status)) mac_caf_parse_power(output, text, 96);
}

static const char *mac_caf_power(void) {
    static char result[96];
    uint64_t now = mac_caf_now();
    pthread_mutex_lock(&g_mac_caf_power_lock);
    memcpy(result, g_mac_caf_power_text, sizeof(result));
    int query = !g_mac_caf_power_pending &&
        (!g_mac_caf_power_checked || now-g_mac_caf_power_checked >= 30000000000ULL);
    if (query) g_mac_caf_power_pending = 1;
    pthread_mutex_unlock(&g_mac_caf_power_lock);
    if (query) dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        char text[96]; mac_caf_query_power(text);
        pthread_mutex_lock(&g_mac_caf_power_lock);
        memcpy(g_mac_caf_power_text, text, sizeof(text));
        g_mac_caf_power_checked = mac_caf_now(); g_mac_caf_power_pending = 0;
        pthread_mutex_unlock(&g_mac_caf_power_lock);
        dispatch_async(dispatch_get_main_queue(), ^{ HARDCORE_CAF_CHANGED(); });
    });
    return result;
}
