/* Disposable integration-test job: extra tty fd, raw/canonical input, signals. */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
static volatile sig_atomic_t interrupted, resized;
static void onint(int sig) { (void)sig; interrupted = 1; }
static void onwinch(int sig) { (void)sig; resized = 1; }
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    int raw = atoi(argv[3]);
    FILE *log = fopen(argv[2], "w");
    if (!log) return 3;
    setbuf(log, NULL); setbuf(stdout, NULL);
    int extra = open("/dev/tty", O_RDWR | O_NONBLOCK);
    if (extra < 0) return 4;
    struct termios mode;
    if (tcgetattr(0, &mode)) return 5;
    if (raw) { cfmakeraw(&mode); tcsetattr(0, TCSANOW, &mode); }
    signal(SIGINT, onint); signal(SIGWINCH, onwinch);
    fcntl(0, F_SETFL, O_NONBLOCK);
    FILE *ready = fopen(argv[1], "w");
    if (!ready) return 6;
    fprintf(ready, "%d %d\n", getpid(), getpgid(getppid())); fclose(ready);
    puts("Disposable Hardcore takeover worker");
    for (;;) {
        char b[256]; int n = read(raw ? extra : 0, b, sizeof b);
        if (n > 0) {
            int t = open("/dev/tty", O_RDWR);
            fprintf(log, "pid=%d sid=%d fg=%d flags=%d input=", getpid(), getsid(0),
                    t < 0 ? -1 : tcgetpgrp(t), !!(fcntl(0,F_GETFL) & O_NONBLOCK));
            fwrite(b, 1, n, log); fputc('\n', log);
            if (t >= 0) { write(t, "CTTY-OK\r\n", 9); close(t); }
            write(extra, "EXTRA-FD-OK\r\n", 13);
            write(1, "\033[32mTRANSFERRED: ", 18); write(1, b, n); write(1, "\033[0m", 4);
        }
        if (interrupted) { fprintf(log, "SIGINT\n"); interrupted = 0; }
        if (resized) {
            struct winsize ws = {0}; ioctl(0, TIOCGWINSZ, &ws);
            fprintf(log, "WINCH %d %d\n", ws.ws_col, ws.ws_row); resized = 0;
        }
        usleep(10000);
    }
}
