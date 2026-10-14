/* Hardcore's Linux half. Built from source into hardcore.exe; see README.md. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if !defined(__x86_64__)
#error "Hardcore WSL attachment currently requires x86-64 Linux."
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include "reptyr/reptyr.h"
#include "../build/slurp-license.h"

void debug(const char *fmt, ...) { (void)fmt; }
void error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); fputs("hardcore slurp: ", stderr);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}
void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); exit(1);
}

typedef struct {
    int pid, ppid, sid;
    unsigned tty;
    unsigned long long start;
    uid_t uid;
    char comm[256], cwd[PATH_MAX];
} Session;

static int proc_info(int pid, Session *s) {
    char path[64], buf[4096], *p, *end, *save;
    struct stat st;
    ssize_t n;
    int fd, field;
    memset(s, 0, sizeof(*s));
    snprintf(path, sizeof(path), "/proc/%d", pid);
    if (stat(path, &st)) return 0;
    s->uid = st.st_uid; s->pid = pid;
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof(buf) - 1); close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    p = strchr(buf, '('); end = strrchr(buf, ')');
    if (!p || !end || end <= p) return 0;
    snprintf(s->comm, sizeof(s->comm), "%.*s", (int)(end - p - 1), p + 1);
    p = strtok_r(end + 2, " ", &save);
    for (field = 3; p; field++, p = strtok_r(NULL, " ", &save)) {
        if (field == 4) s->ppid = atoi(p);
        if (field == 6) s->sid = atoi(p);
        if (field == 7) s->tty = (unsigned)strtoul(p, NULL, 10);
        if (field == 22) { s->start = strtoull(p, NULL, 10); break; }
    }
    snprintf(path, sizeof(path), "/proc/%d/cwd", pid);
    n = readlink(path, s->cwd, sizeof(s->cwd) - 1);
    if (n > 0) s->cwd[n] = 0;
    return s->start != 0;
}

static int hardcore_session(int pid) {
    char path[64], buf[65536];
    ssize_t n;
    int fd;
    snprintf(path, sizeof(path), "/proc/%d/environ", pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof(buf)); close(fd);
    for (ssize_t i = 0; i < n;) {
        size_t len = strnlen(buf + i, (size_t)(n - i));
        if (len == 21 && !memcmp(buf + i, "TERM_PROGRAM=hardcore", 21)) return 1;
        /* HARDCORE_RC is in the initial exec environment, unlike exports in .bashrc. */
        if (len >= 12 && !memcmp(buf + i, "HARDCORE_RC=", 12)) return 1;
        i += (ssize_t)len + 1;
    }
    return 0;
}

static void print_hex(const char *p) {
    if (!*p) { putchar('-'); return; }
    for (; *p; p++) printf("%02x", (unsigned char)*p);
}

static int list_sessions(void) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    if (!d) return 1;
    puts("HARDCORE-SLURP 1");
    while ((e = readdir(d))) {
        Session s, parent;
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 1 || pid > INT_MAX || !proc_info((int)pid, &s)) continue;
        if (s.uid != getuid() || s.pid != s.sid || !s.tty || hardcore_session(s.pid)) continue;
        /* Only direct WSL terminals: don't take SSH, tmux, IDE tasks or services. */
        if (!proc_info(s.ppid, &parent) || strncmp(parent.comm, "Relay(", 6)) continue;
        printf("%d %llu %u ", s.pid, s.start, (unsigned)s.uid);
        print_hex(s.cwd); putchar(' '); print_hex(s.comm); putchar('\n');
    }
    closedir(d);
    return ferror(stdout) ? 1 : 0;
}

static volatile sig_atomic_t resized = 1;
static void on_winch(int sig) { (void)sig; resized = 1; }

typedef struct { unsigned char data[65536]; size_t off, len; } Queue;
static int pump_read(int fd, Queue *q) {
    ssize_t n = read(fd, q->data, sizeof(q->data));
    if (n > 0) { q->off = 0; q->len = (size_t)n; return 1; }
    return n < 0 && (errno == EINTR || errno == EAGAIN);
}
static int pump_write(int fd, Queue *q) {
    ssize_t n = write(fd, q->data + q->off, q->len);
    if (n > 0) { q->off += (size_t)n; q->len -= (size_t)n; return 1; }
    return n < 0 && (errno == EINTR || errno == EAGAIN);
}

static int relay(int master) {
    struct termios saved, raw;
    struct sigaction sa = {0};
    Queue to_pty = {0}, to_screen = {0};
    int in_flags = fcntl(0, F_GETFL), out_flags = fcntl(1, F_GETFL);
    int have_termios = tcgetattr(0, &saved) == 0;
    if (have_termios) { raw = saved; cfmakeraw(&raw); tcsetattr(0, TCSANOW, &raw); }
    sa.sa_handler = on_winch; sigemptyset(&sa.sa_mask); sigaction(SIGWINCH, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    fcntl(0, F_SETFL, in_flags | O_NONBLOCK);
    fcntl(1, F_SETFL, out_flags | O_NONBLOCK);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    for (;;) {
        struct pollfd p[3] = {{0, to_pty.len ? 0 : POLLIN, 0},
            {master, (short)((to_screen.len ? 0 : POLLIN) | (to_pty.len ? POLLOUT : 0)), 0},
            {1, to_screen.len ? POLLOUT : 0, 0}};
        if (resized) {
            struct winsize ws;
            resized = 0;
            if (!ioctl(0, TIOCGWINSZ, &ws)) ioctl(master, TIOCSWINSZ, &ws);
        }
        if (poll(p, 3, 100) < 0) { if (errno == EINTR) continue; break; }
        if ((p[2].revents & POLLOUT) && !pump_write(1, &to_screen)) break;
        if ((p[1].revents & POLLOUT) && !pump_write(master, &to_pty)) break;
        if ((p[0].revents & POLLIN) && !pump_read(0, &to_pty)) break;
        if ((p[1].revents & POLLIN) && !pump_read(master, &to_screen)) break;
        if (p[0].revents & (POLLHUP | POLLERR | POLLNVAL)) break;
        if ((p[1].revents & (POLLHUP | POLLERR | POLLNVAL)) && !to_screen.len && !(p[1].revents & POLLIN)) break;
        if (p[2].revents & (POLLHUP | POLLERR | POLLNVAL)) break;
    }
    fcntl(0, F_SETFL, in_flags); fcntl(1, F_SETFL, out_flags);
    if (have_termios) tcsetattr(0, TCSANOW, &saved);
    close(master);
    return 0;
}

int main(int argc, char **argv) {
    Session s;
    int master = -1, err;
    char *end;
    long pid;
    if (argc == 2 && !strcmp(argv[1], "--license")) { puts(slurp_license); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--list")) return list_sessions();
    if (argc != 5 || strcmp(argv[1], "--attach")) {
        error("internal helper: --list or --attach PID START UID"); return 2;
    }
    pid = strtol(argv[2], &end, 10);
    if (*end || pid <= 1 || pid > INT_MAX || !proc_info((int)pid, &s) ||
        s.start != strtoull(argv[3], NULL, 10) || s.uid != strtoul(argv[4], NULL, 10) ||
        s.pid != s.sid || !s.tty || hardcore_session(s.pid)) {
        error("session changed or is not eligible; original terminal left alone"); return 1;
    }
    for (int p = getpid(); p > 1;) {
        Session ancestor;
        if (p == s.pid) { error("cannot attach our own terminal"); return 1; }
        if (!proc_info(p, &ancestor) || ancestor.ppid == p) break;
        p = ancestor.ppid;
    }
    if (!isatty(0) || !isatty(1)) { error("attachment requires a terminal"); return 1; }
    /* Lock the original proc inode: no lock files, stale PIDs or symlink races. */
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", s.pid);
    int lock = open(path, O_RDONLY | O_CLOEXEC);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        error("session already being attached"); return 1;
    }
    err = steal_pty(s.pid, &master);
    if (err || master < 0) {
        error("could not attach PID %d: %s. No shell was restarted.", s.pid, strerror(err ? err : EIO));
        return 1;
    }
    /* No shell command is injected. Let full-screen programs redraw themselves. */
    printf("\r\nhardcore: attached live WSL session %d\r\n", s.pid); fflush(stdout);
    pid_t fg = tcgetpgrp(master);
    if (fg > 0) kill(-fg, SIGWINCH);
    return relay(master);
}
