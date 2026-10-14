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
#include <sys/types.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ptrace.h"
#include "reptyr.h"
#include "reallocarray.h"
#include "platform/platform.h"

int fd_array_push(struct fd_array *fda, int fd) {
    int *tmp;

    if (fda->n == fda->allocated) {
        fda->allocated = fda->allocated ? 2 * fda->allocated : 2;
        tmp = xreallocarray(fda->fds, fda->allocated, sizeof *tmp);
        if (tmp == NULL) {
            free(fda->fds);
            fda->fds = NULL;
            fda->allocated = 0;
            return -1;
        }
        fda->fds = tmp;
    }
    fda->fds[fda->n++] = fd;
    return 0;
}

static void do_unmap(struct ptrace_child *child, child_addr_t addr, unsigned long len) {
    if (addr == (child_addr_t) - 1)
        return;
    do_syscall(child, munmap, (unsigned long)addr, len, 0, 0, 0, 0);
}

static int do_dup2(struct ptrace_child *child, int oldfd, int newfd) {
    if (oldfd == newfd) {
        return 0;
    }
    if (ptrace_syscall_numbers(child)->nr_dup2 != -1) {
        return do_syscall(child, dup2, oldfd, newfd, 0, 0, 0, 0);
    } else {
        return do_syscall(child, dup3, oldfd, newfd, 0, 0, 0, 0);
    }
}

int ignore_hup(struct ptrace_child *child, child_addr_t scratch_page) {
    int err;

    struct sigaction act = {
        .sa_handler = SIG_IGN,
    };
    err = ptrace_memcpy_to_child(child, scratch_page,
                                 &act, sizeof act);
    if (err < 0)
        return err;
    err = do_syscall(child, rt_sigaction,
                     SIGHUP, (unsigned long)scratch_page,
                     0, 8, 0, 0);

    return err;
}

/*
 * Wait for the specific pid to enter state 'T', or stopped. We have to pull the
 * /proc file rather than attaching with ptrace() and doing a wait() because
 * half the point of this exercise is for the process's real parent (the shell)
 * to see the TSTP.
 *
 * In case the process is masking or ignoring SIGTSTP, we time out after a
 * second and continue with the attach -- it'll still work mostly right, you
 * just won't get the old shell back.
 */
int mmap_scratch(struct ptrace_child *child, child_addr_t *addr) {
    long mmap_syscall;
    child_addr_t scratch_page;

    mmap_syscall = ptrace_syscall_numbers(child)->nr_mmap2;
    if (mmap_syscall == -1)
        mmap_syscall = ptrace_syscall_numbers(child)->nr_mmap;
    scratch_page = ptrace_remote_syscall(child, mmap_syscall, 0,
                                         sysconf(_SC_PAGE_SIZE), PROT_READ | PROT_WRITE,
                                         MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    //MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);

    if (scratch_page > (unsigned long) - 1000) {
        return -(signed long)scratch_page;
    }

    *addr = scratch_page;
    debug("Allocated scratch page: %lx", scratch_page);

    return 0;
}

int grab_pid(pid_t pid, struct ptrace_child *child, child_addr_t *scratch) {
    int err;

    if (ptrace_attach_child(child, pid)) {
        err = child->error;
        goto out;
    }
    if (ptrace_advance_to_state(child, ptrace_at_syscall)) {
        err = child->error;
        goto out;
    }
    if (ptrace_save_regs(child)) {
        err = child->error;
        goto out;
    }

    if ((err = mmap_scratch(child, scratch)))
        goto out_restore_regs;

    return 0;

out_restore_regs:
    ptrace_restore_regs(child);

out:
    ptrace_detach_child(child);

    return err;
}

int setup_steal_socket(struct steal_pty_state *steal) {
    strcpy(steal->tmpdir, "/tmp/reptyr.XXXXXX");
    if (mkdtemp(steal->tmpdir) == NULL)
        return errno;

    steal->addr_un.sun_family = AF_UNIX;
    if (snprintf(steal->addr_un.sun_path, sizeof(steal->addr_un.sun_path),
                 "%s/reptyr.sock", steal->tmpdir) >= (int)sizeof(steal->addr_un.sun_path)) {
        error("tmpdir path too long!");
        return ENAMETOOLONG;
    }

    if ((steal->sockfd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0)
        return errno;

    if (bind(steal->sockfd, &steal->addr, sizeof(steal->addr_un)) < 0)
        return errno;

    if (chown(steal->addr_un.sun_path, steal->emulator_uid, -1) < 0)
        debug("chown %s: %s", steal->addr_un.sun_path, strerror(errno));
    if (chown(steal->tmpdir, steal->emulator_uid, -1) < 0)
        debug("chown %s: %s", steal->tmpdir, strerror(errno));

    return 0;
}

int setup_steal_socket_child(struct steal_pty_state *steal) {
    int err;
    err = do_socketcall(&steal->child,
                        steal->child_scratch + sysconf(_SC_PAGE_SIZE)/2,
                        socket, AF_UNIX, SOCK_DGRAM, 0, 0, 0);
    if (err < 0)
        return -err;
    steal->child_fd = err;
    debug("Opened fd %d in the child.", steal->child_fd);
    err = ptrace_memcpy_to_child(&steal->child, steal->child_scratch,
                                 &steal->addr_un, sizeof(steal->addr_un));
    if (err < 0)
        return steal->child.error;
    err = do_socketcall(&steal->child,
                        steal->child_scratch + sysconf(_SC_PAGE_SIZE)/2,
                        connect, steal->child_fd, steal->child_scratch,
                        sizeof(steal->addr_un), 0, 0);
    if (err < 0)
        return -err;
    debug("Connected to the shared socket.");
    return 0;
}

int steal_child_pty(struct steal_pty_state *steal) {
    struct {
        struct msghdr msg;
        unsigned char buf[CMSG_SPACE(sizeof(int))];
    } buf = {};
    struct cmsghdr *cm;
    int err;

    buf.msg.msg_control = buf.buf;
    buf.msg.msg_controllen = CMSG_SPACE(sizeof(int));
    cm = CMSG_FIRSTHDR(&buf.msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &steal->master_fds.fds[0], sizeof(int));
    buf.msg.msg_controllen = cm->cmsg_len;

    // Relocate for the child
    buf.msg.msg_control = (void*)(steal->child_scratch +
                                  ((uint8_t*)buf.msg.msg_control - (uint8_t*)&buf));

    if (ptrace_memcpy_to_child(&steal->child,
                               steal->child_scratch,
                               &buf, sizeof(buf))) {
        return steal->child.error;
    }

    steal->child.error = 0;
    err = do_socketcall(&steal->child,
                        steal->child_scratch + sysconf(_SC_PAGE_SIZE)/2,
                        sendmsg,
                        steal->child_fd,
                        steal->child_scratch,
                        MSG_DONTWAIT, 0, 0);
    if (err < 0) {
        return steal->child.error ? steal->child.error : -err;
    }

    debug("Sent the pty fd, going to receive it.");

    buf.msg.msg_control = buf.buf;
    buf.msg.msg_controllen = CMSG_SPACE(sizeof(int));

    err = recvmsg(steal->sockfd, &buf.msg, MSG_DONTWAIT);
    if (err < 0) {
        error("Error receiving message.");
        return errno;
    }

    debug("Got a message: %d bytes, %ld control",
          err, (long)buf.msg.msg_controllen);

    if (buf.msg.msg_controllen < CMSG_LEN(sizeof(int))) {
        error("No fd received?");
        return EINVAL;
    }

    memcpy(&steal->ptyfd, CMSG_DATA(cm), sizeof(steal->ptyfd));

    debug("Got tty fd: %d", steal->ptyfd);

    return 0;
}

// Attach to the session leader of the stolen session, and block
// SIGHUP so that if and when the terminal emulator tries to HUP it,
// it doesn't die.
int steal_block_hup(struct steal_pty_state *steal) {
    struct ptrace_child leader;
    child_addr_t scratch = 0;
    int err = 0;

    if ((err = grab_pid(steal->target_stat.sid, &leader, &scratch)))
        return err;

    err = ignore_hup(&leader, scratch);

    do_unmap(&leader, scratch, sysconf(_SC_PAGE_SIZE));
    ptrace_restore_regs(&leader);
    ptrace_detach_child(&leader);
    if (steal->target_stat.state == 'T') kill(leader.pid, SIGSTOP);

    return err;
}

int steal_cleanup_child(struct steal_pty_state *steal) {
    if (ptrace_memcpy_to_child(&steal->child,
                               steal->child_scratch,
                               "/dev/null", sizeof("/dev/null"))) {
        return steal->child.error;
    }

    int nullfd = do_syscall(&steal->child, openat, -1, steal->child_scratch, O_RDWR, 0, 0, 0);
    if (nullfd < 0) {
        return steal->child.error ? steal->child.error : -nullfd;
    }

    int i;
    for (i = 0; i < steal->master_fds.n; ++i) {
        int rv = do_dup2(&steal->child, nullfd, steal->master_fds.fds[i]);
        if (rv < 0) {
            /* The final descriptor still holds the original PTY until commit. */
            int backup = steal->master_fds.fds[steal->master_fds.n - 1];
            for (int j = 0; j < i; j++) do_dup2(&steal->child, backup, steal->master_fds.fds[j]);
            do_syscall(&steal->child, close, nullfd, 0, 0, 0, 0, 0);
            return -rv;
        }
    }

    do_syscall(&steal->child, close, nullfd, 0, 0, 0, 0, 0);
    do_syscall(&steal->child, close, steal->child_fd, 0, 0, 0, 0, 0);

    steal->child_fd = 0;
    do_unmap(&steal->child, steal->child_scratch, sysconf(_SC_PAGE_SIZE));
    steal->child_scratch = 0;

    ptrace_restore_regs(&steal->child);

    ptrace_detach_child(&steal->child);
    ptrace_wait(&steal->child);
    return 0;
}

int steal_pty(pid_t pid, int *pty) {
    int err = 0;
    struct steal_pty_state steal = {};
    long page_size = sysconf(_SC_PAGE_SIZE);

    if ((err = get_terminal_state(&steal, pid)))
        goto out;

    if ((err = setup_steal_socket(&steal)))
        goto out;

    debug("Listening on socket: %s", steal.addr_un.sun_path);
    debug("Attaching terminal emulator pid=%d", steal.emulator_pid);

    if ((err = grab_pid(steal.emulator_pid, &steal.child, &steal.child_scratch)))
        goto out;

    debug("Attached to terminal emulator (pid %d)",
          (int)steal.emulator_pid);

    /* SCM_RIGHTS structures below use the native ABI. */
    if (steal.child.personality != 0) { err = EOPNOTSUPP; goto out; }

    if ((err = find_master_fd(&steal))) {
        error("Unable to find the fd for the pty!");
        goto out;
    }

    if ((err = setup_steal_socket_child(&steal)))
        goto out;

    if ((err = steal_child_pty(&steal)))
        goto out;

    if ((err = steal_block_hup(&steal)))
        goto out;

    if ((err = steal_cleanup_child(&steal)))
        goto out;

    goto out_no_child;

out:
    if (steal.ptyfd) {
        close(steal.ptyfd);
        steal.ptyfd = 0;
    }

    if (steal.child_fd > 0)
        do_syscall(&steal.child, close, steal.child_fd, 0, 0, 0, 0, 0);

    if (steal.child_scratch > 0)
        do_unmap(&steal.child, steal.child_scratch, page_size);

    if (steal.child.state != ptrace_detached) {
        ptrace_restore_regs(&steal.child);
        ptrace_detach_child(&steal.child);
    }

out_no_child:

    if (steal.sockfd > 0) {
        close(steal.sockfd);
        unlink(steal.addr_un.sun_path);
    }

    if (steal.tmpdir[0]) {
        rmdir(steal.tmpdir);
    }

    if (steal.ptyfd)
        *pty = steal.ptyfd;

    free(steal.master_fds.fds);

    return err;
}
