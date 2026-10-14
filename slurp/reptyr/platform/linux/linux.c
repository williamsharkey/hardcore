/*
 * Copyright (C) 2011 by Nelson Elhage
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifdef __linux__

#include "linux.h"
#include "../platform.h"
#include "../../reptyr.h"
#include "../../ptrace.h"
#include <stdint.h>

int parse_proc_stat(int statfd, struct proc_stat *out) {
    char buf[4096], *end;
    ssize_t n;
    unsigned dev;
    lseek(statfd, 0, SEEK_SET);
    n = read(statfd, buf, sizeof(buf) - 1);
    if (n <= 0) return n < 0 ? errno : EINVAL;
    buf[n] = 0;
    /* comm may contain spaces, newlines and closing parentheses. */
    end = strrchr(buf, ')');
    if (!end || sscanf(buf, "%d", &out->pid) != 1 ||
        sscanf(end + 1, " %c %d %d %d %u", &out->state, &out->ppid,
               &out->pgid, &out->sid, &dev) != 5) return EINVAL;
    out->comm[0] = 0;
    out->ctty = dev;
    return 0;
}

int read_proc_stat(pid_t pid, struct proc_stat *out) {
    char stat_path[PATH_MAX];
    int statfd;
    int err;

    snprintf(stat_path, sizeof stat_path, "/proc/%d/stat", pid);
    statfd = open(stat_path, O_RDONLY);
    if (statfd < 0) {
        error("Unable to open %s: %s", stat_path, strerror(errno));
        return errno;
    }
    err = parse_proc_stat(statfd, out);


    close(statfd);
    return err;
}

int read_uid(pid_t pid, uid_t *out) {
    char stat_path[PATH_MAX];
    char buf[1024];
    int statfd;
    int err = 0;
    int n;
    char *p = buf;

    snprintf(stat_path, sizeof stat_path, "/proc/%d/status", pid);
    statfd = open(stat_path, O_RDONLY);
    if (statfd < 0) {
        error("Unable to open %s: %s", stat_path, strerror(errno));
        return errno;
    }

    if ((n = read(statfd, buf, sizeof(buf))) < 0) {
        err = assert_nonzero(errno);
        goto out;
    }
    while (p < buf + n) {
        if (strncmp(p, "Uid:\t", strlen("Uid:\t")) == 0)
            break;
        p = memchr(p, '\n', buf+n-p);
        if (p == NULL)
            break;
        p++;
        continue;
    }
    if (p == NULL || p >= buf + n) {
        debug("Unable to parse emulator uid: no Uid line found");
        *out = -1;
        goto out;
    }
    if (sscanf(p, "Uid:\t%d", out) < 0) {
        debug("Unable to parse emulator uid: unparseable Uid line");
    }

 out:
    close(statfd);
    return err;
}

// Find the PID of the terminal emulator for `target's terminal.
//
// We assume that the terminal emulator is the parent of the session
// leader. This is true in most cases, although in principle you can
// construct situations where it is false. We should fail safe later
// on if this turns out to be wrong, however.
int find_terminal_emulator(struct steal_pty_state *steal) {
    debug("session leader of pid %d = %d",
          (int)steal->target_stat.pid,
          (int)steal->target_stat.sid);
    struct proc_stat leader_st;
    int err;
    if ((err = read_proc_stat(steal->target_stat.sid, &leader_st)))
        return err;
    debug("found terminal emulator process: %d", (int) leader_st.ppid);
    steal->emulator_pid = leader_st.ppid;
    return 0;
}

int get_terminal_state(struct steal_pty_state *steal, pid_t target) {
    int err;

    if ((err = read_proc_stat(target, &steal->target_stat)))
        return err;

    if (major(steal->target_stat.ctty) < 136 || major(steal->target_stat.ctty) > 143) {
        error("Child is not connected to a pseudo-TTY. Unable to steal TTY.");
        return EINVAL;
    }

    if ((err = find_terminal_emulator(steal)))
        return err;

    if ((err = read_uid(steal->emulator_pid, &steal->emulator_uid)))
        return err;

    return 0;
}

// ptmx(4) and Linux Documentation/devices.txt document
// /dev/ptmx has having major 5 and minor 2. I can't find any
// constants in headers after a brief glance that I should be
// using here.
#define PTMX_DEVICE (makedev(5, 2))

// Find the fd(s) in the terminal emulator process that corresponds to
// the master side of the target's pty. Store the result in
// steal->master_fds.
int find_master_fd(struct steal_pty_state *steal) {
    DIR *dir;
    struct dirent *d;
    struct stat st;
    int err;
    char buf[PATH_MAX];

    snprintf(buf, sizeof buf, "/proc/%d/fd/", steal->child.pid);
    if ((dir = opendir(buf)) == NULL)
        return errno;
    while ((d = readdir(dir)) != NULL) {
        if (d->d_name[0] == '.') continue;
        snprintf(buf, sizeof buf, "/proc/%d/fd/%s", steal->child.pid, d->d_name);
        if (stat(buf, &st) < 0)
            continue;

        debug("Checking fd: %s: st_dev=%x", d->d_name, (int)st.st_rdev);

        if (st.st_rdev != PTMX_DEVICE)
            continue;

        debug("found a ptmx fd: %s", d->d_name);
        err = do_syscall(&steal->child, ioctl,
                         atoi(d->d_name),
                         TIOCGPTN,
                         steal->child_scratch,
                         0, 0, 0);
        if (err < 0) {
            debug(" error doing TIOCGPTN: %s", strerror(-err));
            continue;
        }
        int ptn;
        err = ptrace_memcpy_from_child(&steal->child, &ptn,
                                       steal->child_scratch, sizeof(ptn));
        if (err < 0) {
            debug(" error getting ptn: %s", strerror(steal->child.error));
            continue;
        }
        if (ptn == (int)((major(steal->target_stat.ctty) - 136) * 256 + minor(steal->target_stat.ctty))) {
            debug("found a master fd: %d", atoi(d->d_name));
            if (fd_array_push(&steal->master_fds, atoi(d->d_name)) != 0) {
                error("unable to allocate memory for fd array!");
                closedir(dir);
                return ENOMEM;
            }
        }
    }

    closedir(dir);
    if (steal->master_fds.n == 0) {
        return ESRCH;
    }
    return 0;
}

/* Homebrew posix_openpt() */
unsigned long ptrace_socketcall(struct ptrace_child *child,
                                unsigned long scratch,
                                unsigned long socketcall,
                                unsigned long p0, unsigned long p1,
                                unsigned long p2, unsigned long p3,
                                unsigned long p4)
{
    // We assume that socketcall is only used on 32-bit
    // architectures. If there are any 64-bit architectures that do
    // socketcall, and we port to them, this will need to change.
    uint32_t args[] = {p0, p1, p2, p3, p4};
    int err;

    err = ptrace_memcpy_to_child(child, scratch, &args, sizeof args);
    if (err < 0)
        return (unsigned long)err;
    return do_syscall(child, socketcall, socketcall, scratch, 0, 0, 0, 0);
}


#endif
