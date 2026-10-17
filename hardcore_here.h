/* Terminal-hosted backend for the same Term/Cell engine as the GPU app.
 * The persistent owner and its terminal attachment run this same executable.
 * Included once, after the shared parser and ANSI snapshot helpers. */
#ifndef HARDCORE_HERE_H
#define HARDCORE_HERE_H

#ifndef _WIN32
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <dirent.h>
#include <limits.h>
#ifdef __APPLE__
#include <util.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#else
#include <pty.h>
#endif

#define HERE_MAX_TABS 32
#define HERE_PENDING_MAX (4 * 1024 * 1024)
enum { HERE_DETACH = 1, HERE_RELOAD, HERE_STOP };
typedef struct HereUI HereUI;
typedef struct {
    Term t;
    int id, fd, exited, status, unread;
    pid_t pid;
    char cwd[1024], program[64];
    SBuf pending;
    size_t pending_off;
} HereTab;
typedef struct {
    HereTab tabs[HERE_MAX_TABS];
    int ntabs, active, cols, rows, child_cols, child_rows, dirty;
    int action, attached, next_id;
    int font_request, font_allowed, font_size, font_available, font_active;
    char font_name[128], font_error[256];
    pid_t caf_pid;
    char session[49], exe[4096], shell[1024], cwd[1024];
    char **command;
    HereUI *ui;
} Here;

static uint64_t here_now_ms(void);
static int here_new(Here *h, const char *cwd, char *const *argv);
static void here_close(Here *h, int index);
static int here_write(Here *h, int index, const char *data, size_t n);
static void here_resize_tabs(Here *h, int cols, int rows);
static void here_caf(Here *h, int enabled);
static void here_font(Here *h, const char *name, int size, int allowed);
static void here_ui_message(Here *h, const char *message);
static void here_ui_init(Here *h);
static void here_ui_free(Here *h);
static void here_ui_resize(Here *h, int cols, int rows);
static void here_ui_input(Here *h, const char *data, size_t n);
static void here_ui_render(Here *h, SBuf *out);
static void here_ui_tick(Here *h, uint64_t now);
static void here_ui_focus(Here *h, int focused);

#include "hardcore_here_helpers.h"
#include "hardcore_here_ui.h"
#include "hardcore_here_transport.h"
#endif
#endif
