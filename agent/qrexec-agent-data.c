/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2013  Marek Marczykowski-Górecki  <marmarek@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE 1
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <stdbool.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <libvchan.h>

#include "qrexec.h"
#include "libqrexec-utils.h"
#include "qrexec-agent.h"

#define VCHAN_BUFFER_SIZE 65536

#define QREXEC_DATA_MIN_VERSION QREXEC_PROTOCOL_V2

static volatile int child_exited;
static volatile sig_atomic_t stdio_socket_requested;
int stdout_msg_type = MSG_DATA_STDOUT;
pid_t child_process_pid;
int remote_process_status = 0;

/* whether qrexec-client should replace problematic bytes with _ before printing the output;
 * positive value will enable the feature
 */
int replace_chars_stdout = -1;
int replace_chars_stderr = -1;

static void sigchld_handler(int __attribute__((__unused__))x)
{
    child_exited = 1;
}

static void sigusr1_handler(int __attribute__((__unused__))x)
{
    stdio_socket_requested = 1;
    signal(SIGUSR1, SIG_IGN);
}

void prepare_child_env() {
    char pid_s[10];

    signal(SIGCHLD, sigchld_handler);
    signal(SIGUSR1, sigusr1_handler);
    int res = snprintf(pid_s, sizeof(pid_s), "%d", getpid());
    if (res < 0) abort();
    if (res >= (int)sizeof(pid_s)) abort();
    if (setenv("QREXEC_AGENT_PID", pid_s, 1)) abort();
}

int handle_handshake(libvchan_t *ctrl)
{
    struct msg_header hdr;
    struct peer_info info;
    int actual_version;

    /* send own HELLO */
    hdr.type = MSG_HELLO;
    hdr.len = sizeof(info);
    info.version = QREXEC_PROTOCOL_VERSION;

    if (libvchan_send(ctrl, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        fprintf(stderr, "Failed to send HELLO hdr to agent\n");
        return -1;
    }

    if (libvchan_send(ctrl, &info, sizeof(info)) != sizeof(info)) {
        fprintf(stderr, "Failed to send HELLO hdr to agent\n");
        return -1;
    }

    /* receive MSG_HELLO from remote */
    if (libvchan_recv(ctrl, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        fprintf(stderr, "Failed to read agent HELLO hdr\n");
        return -1;
    }

    if (hdr.type != MSG_HELLO || hdr.len != sizeof(info)) {
        fprintf(stderr, "Invalid HELLO packet received: type %d, len %d\n", hdr.type, hdr.len);
        return -1;
    }

    if (libvchan_recv(ctrl, &info, sizeof(info)) != sizeof(info)) {
        fprintf(stderr, "Failed to read agent HELLO body\n");
        return -1;
    }

    actual_version = info.version < QREXEC_PROTOCOL_VERSION ? info.version : QREXEC_PROTOCOL_VERSION;

    if (actual_version < QREXEC_DATA_MIN_VERSION) {
        fprintf(stderr, "Incompatible agent protocol version (remote %d, local %d)\n", info.version, QREXEC_PROTOCOL_VERSION);
        return -1;
    }

    return actual_version;
}


static int handle_just_exec(char *cmdline)
{
    int fdn, pid;

    char *end_username = strchr(cmdline, ':');
    if (!end_username) {
        fprintf(stderr, "No colon in command from dom0\n");
        return -1;
    }
    *end_username++ = '\0';
    switch (pid = fork()) {
        case -1:
            perror("fork");
            return -1;
        case 0:
            fdn = open("/dev/null", O_RDWR);
            fix_fds(fdn, fdn, fdn);
            do_exec(end_username, cmdline);
        default:;
    }
    fprintf(stderr, "executed (nowait) %s pid %d\n", cmdline, pid);
    return 0;
}

static void send_exit_code(libvchan_t *data_vchan, int status)
{
    struct msg_header hdr;
    hdr.type = MSG_DATA_EXIT_CODE;
    hdr.len = sizeof(status);
    if (libvchan_send(data_vchan, &hdr, sizeof(hdr)) < 0)
        handle_vchan_error("write hdr");
    if (libvchan_send(data_vchan, &status, sizeof(status)) < 0)
        handle_vchan_error("write status");
    fprintf(stderr, "send exit code %d\n", status);
}

static int process_child_io(libvchan_t *data_vchan,
        int stdin_fd, int stdout_fd, int stderr_fd,
        int data_protocol_version, struct buffer *stdin_buf)
{
    fd_set rdset, wrset;
    int vchan_fd;
    sigset_t selectmask;
    int child_process_status = child_process_pid > 0 ? -1 : 0;
    int remote_process_status = -1;
    int ret, max_fd;
    struct timespec zero_timeout = { 0, 0 };
    struct timespec normal_timeout = { 10, 0 };

    sigemptyset(&selectmask);
    sigaddset(&selectmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &selectmask, NULL);
    sigemptyset(&selectmask);

    set_nonblock(stdin_fd);
    if (stdout_fd != stdin_fd)
        set_nonblock(stdout_fd);
    else if ((stdout_fd = fcntl(stdin_fd, F_DUPFD_CLOEXEC, 3)) < 0)
        abort(); // not worth handling running out of file descriptors
    if (stderr_fd >= 0)
        set_nonblock(stderr_fd);

    while (1) {
        if (child_exited) {
            int status;
            if (child_process_pid > 0 &&
                    waitpid(child_process_pid, &status, WNOHANG) > 0) {
                if (WIFSIGNALED(status))
                    child_process_status = 128 + WTERMSIG(status);
                else
                    child_process_status = WEXITSTATUS(status);
                if (stdin_fd >= 0) {
                    /* restore flags */
                    set_block(stdin_fd);
                    if (!child_process_pid || stdin_fd == 1 ||
                            (shutdown(stdin_fd, SHUT_WR) == -1 &&
                             errno == ENOTSOCK)) {
                        close(stdin_fd);
                    }
                    stdin_fd = -1;
                }
            }
            child_exited = 0;
        }

        /* if all done, exit the loop */
        if ((!child_process_pid || child_process_status > -1) &&
                (child_process_pid || remote_process_status > -1) &&
                stdin_fd == -1 && stdout_fd == -1 && stderr_fd == -1) {
            if (child_process_status > -1) {
                send_exit_code(data_vchan, child_process_status);
            }
            break;
        }
        /* also if vchan is disconnected (and we processed all the data), there
         * is no sense of processing further data */
        if (!libvchan_data_ready(data_vchan) &&
                !libvchan_is_open(data_vchan) &&
                !buffer_len(stdin_buf)) {
            break;
        }
        /* child signaled desire to use single socket for both stdin and stdout */
        if (stdio_socket_requested == 1) {
            if (stdout_fd != -1) {
                do
                    errno = 0;
                while (dup3(stdin_fd, stdout_fd, O_CLOEXEC) &&
                       (errno == EINTR || errno == EBUSY));
                // other errors are fatal
                if (errno) {
                    fputs("Fatal error from dup3()\n", stderr);
                    abort();
                }
            } else {
                stdout_fd = fcntl(stdin_fd, F_DUPFD_CLOEXEC, 3);
                // all errors are fatal
                if (stdout_fd < 3)
                    abort();
            }
            stdio_socket_requested = 2;
        }
        /* otherwise handle the events */

        FD_ZERO(&rdset);
        FD_ZERO(&wrset);
        max_fd = -1;
        vchan_fd = libvchan_fd_for_select(data_vchan);
        if (libvchan_buffer_space(data_vchan) > (int)sizeof(struct msg_header)) {
            if (stdout_fd >= 0) {
                FD_SET(stdout_fd, &rdset);
                if (stdout_fd > max_fd)
                    max_fd = stdout_fd;
            }
            if (stderr_fd >= 0) {
                FD_SET(stderr_fd, &rdset);
                if (stderr_fd > max_fd)
                    max_fd = stderr_fd;
            }
        }
        FD_SET(vchan_fd, &rdset);
        if (vchan_fd > max_fd)
            max_fd = vchan_fd;
        /* if we have something buffered for the child process, wake also on
         * writable stdin */
        if (stdin_fd > -1 && buffer_len(stdin_buf)) {
            FD_SET(stdin_fd, &wrset);
            if (stdin_fd > max_fd)
                max_fd = stdin_fd;
        }

        if (!buffer_len(stdin_buf) && libvchan_data_ready(data_vchan) > 0) {
            /* check for other FDs, but exit immediately */
            ret = pselect(max_fd + 1, &rdset, &wrset, NULL, &zero_timeout, &selectmask);
        } else
            ret = pselect(max_fd + 1, &rdset, &wrset, NULL, &normal_timeout, &selectmask);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            else {
                perror("pselect");
                /* TODO */
                break;
            }
        }

        /* clear event pending flag */
        if (FD_ISSET(vchan_fd, &rdset)) {
            if (libvchan_wait(data_vchan) < 0)
                handle_vchan_error("wait");
        }

        /* handle_remote_data will check if any data is available */
        switch (handle_remote_data(
                    data_vchan, stdin_fd,
                    &remote_process_status,
                    stdin_buf,
                    data_protocol_version,
                    child_process_pid && stdin_fd != 1,
                    replace_chars_stdout,
                    replace_chars_stderr)) {
            case REMOTE_ERROR:
                handle_vchan_error("read");
                break;
            case REMOTE_EOF:
                stdin_fd = -1;
                break;
            case REMOTE_EXITED:
                /* remote process exited, no sense in sending more data to it;
                 * be careful to not shutdown socket inherited from parent */
                if (!child_process_pid || stdout_fd == 0 ||
                        (shutdown(stdout_fd, SHUT_RD) == -1 &&
                         errno == ENOTSOCK)) {
                    close(stdout_fd);
                }
                stdout_fd = -1;
                close(stderr_fd);
                stderr_fd = -1;
                /* if we do not care for any local process, return remote process code */
                if (child_process_pid == 0)
                    return remote_process_status;
                break;
        }
        if (stdout_fd >= 0 && FD_ISSET(stdout_fd, &rdset)) {
            switch (handle_input(
                        data_vchan, stdout_fd, stdout_msg_type,
                        data_protocol_version,
                        stdio_socket_requested < 2)) {
                case REMOTE_ERROR:
                    handle_vchan_error("send");
                    break;
                case REMOTE_EOF:
                    stdout_fd = -1;
                    break;
            }
        }
        if (stderr_fd >= 0 && FD_ISSET(stderr_fd, &rdset)) {
            switch (handle_input(
                        data_vchan, stderr_fd, MSG_DATA_STDERR,
                        data_protocol_version,
                        stdio_socket_requested < 2)) {
                case REMOTE_ERROR:
                    handle_vchan_error("send");
                    break;
                case REMOTE_EOF:
                    stderr_fd = -1;
                    break;
            }
        }
    }
    /* make sure that all the pipes/sockets are closed, so the child process
     * (if any) will know that the connection is terminated */
    if (stdout_fd != -1) {
        /* restore flags */
        set_block(stdout_fd);
        /* be careful to not shutdown socket inherited from parent */
        if (!child_process_pid || stdout_fd == 0 ||
                (shutdown(stdout_fd, SHUT_RD) == -1 && errno == ENOTSOCK)) {
            close(stdout_fd);
        }
        stdout_fd = -1;
    }
    if (stdin_fd != -1) {
        /* restore flags */
        set_block(stdin_fd);
        /* be careful to not shutdown socket inherited from parent */
        if (!child_process_pid || stdin_fd == 1 ||
                (shutdown(stdin_fd, SHUT_WR) == -1 && errno == ENOTSOCK)) {
            close(stdin_fd);
        }
        stdin_fd = -1;
    }
    if (stderr_fd != -1) {
        /* restore flags */
        set_block(stderr_fd);
        close(stderr_fd);
        stderr_fd = -1;
    }
    if (child_process_pid == 0)
        return remote_process_status;
    return child_process_status;
}

/* Behaviour depends on type parameter:
 *  MSG_SERVICE_CONNECT - create vchan server, pass the data to/from given FDs
 *    (stdin_fd, stdout_fd, stderr_fd), then return remote process exit code
 *  MSG_JUST_EXEC - connect to vchan server, fork+exec process given by cmdline
 *    parameter, send artificial exit code "0" (local process can still be
 *    running), then return 0
 *  MSG_EXEC_CMDLINE - connect to vchan server, fork+exec process given by
 *    cmdline parameter, pass the data to/from that process, then return local
 *    process exit code
 *
 *  buffer_size is about vchan buffer allocated (only for vchan server cases),
 *  use 0 to use built-in default (64k); needs to be power of 2
 */
static int handle_new_process_common(int type, int connect_domain, int connect_port,
                char *cmdline, size_t cmdline_len, /* MSG_JUST_EXEC and MSG_EXEC_CMDLINE */
                int stdin_fd, int stdout_fd, int stderr_fd /* MSG_SERVICE_CONNECT */,
                int buffer_size)
{
    libvchan_t *data_vchan;
    int exit_code = 0;
    pid_t pid;
    int data_protocol_version;

    if (buffer_size == 0)
        buffer_size = VCHAN_BUFFER_SIZE;

    if (type == MSG_SERVICE_CONNECT) {
        data_vchan = libvchan_server_init(connect_domain, connect_port,
                buffer_size, buffer_size);
        if (data_vchan)
            libvchan_wait(data_vchan);
    } else {
        if (cmdline == NULL) {
            fputs("internal qrexec error: NULL cmdline passed to a non-MSG_SERVICE_CONNECT call\n", stderr);
            abort();
        } else if (cmdline_len == 0) {
            fputs("internal qrexec error: zero-length command line passed to a non-MSG_SERVICE_CONNECT call\n", stderr);
            abort();
        } else if (cmdline_len > MAX_QREXEC_CMD_LEN) {
            /* This is arbitrary, but it helps reduce the risk of overflows in other code */
            fprintf(stderr, "Bad command from dom0: command line too long: length %zu\n", cmdline_len);
            abort();
        }
        cmdline[cmdline_len-1] = 0;
        data_vchan = libvchan_client_init(connect_domain, connect_port);
    }
    if (!data_vchan) {
        fprintf(stderr, "Data vchan connection failed\n");
        exit(1);
    }
    data_protocol_version = handle_handshake(data_vchan);

    prepare_child_env();
    /* TODO: use setresuid to allow child process to actually send the signal? */

    switch (type) {
        case MSG_JUST_EXEC:
            send_exit_code(data_vchan, handle_just_exec(cmdline));
            break;
        case MSG_EXEC_CMDLINE: {
            struct buffer stdin_buf;
            buffer_init(&stdin_buf);
            if (execute_qubes_rpc_command(cmdline, &pid, &stdin_fd, &stdout_fd, &stderr_fd, !qrexec_is_fork_server, &stdin_buf) < 0) {
                fputs("failed to spawn process\n", stderr);
            }
            fprintf(stderr, "executed %s pid %d\n", cmdline, pid);
            child_process_pid = pid;
            exit_code = process_child_io(
                    data_vchan, stdin_fd, stdout_fd, stderr_fd,
                    data_protocol_version, &stdin_buf);
            fprintf(stderr, "pid %d exited with %d\n", pid, exit_code);
            break;
        }
        case MSG_SERVICE_CONNECT: {
            struct buffer stdin_buf;
            buffer_init(&stdin_buf);
            child_process_pid = 0;
            stdout_msg_type = MSG_DATA_STDIN;
            exit_code = process_child_io(
                    data_vchan, stdin_fd, stdout_fd, stderr_fd,
                    data_protocol_version, &stdin_buf);
            break;
        }
    }
    libvchan_close(data_vchan);
    return exit_code;
}

/* Returns PID of data processing process */
pid_t handle_new_process(int type, int connect_domain, int connect_port,
        char *cmdline, size_t cmdline_len)
{
    int exit_code;
    pid_t pid;
    assert(type != MSG_SERVICE_CONNECT);

    switch (pid=fork()){
        case -1:
            perror("fork");
            return -1;
        case 0:
            break;
        default:
            return pid;
    }

    /* child process */
    exit_code = handle_new_process_common(type, connect_domain, connect_port,
            cmdline, cmdline_len,
            -1, -1, -1, 0);

    exit(exit_code);
}

/* Returns exit code of remote process */
int handle_data_client(int type, int connect_domain, int connect_port,
                int stdin_fd, int stdout_fd, int stderr_fd, int buffer_size)
{
    int exit_code;

    assert(type == MSG_SERVICE_CONNECT);

    exit_code = handle_new_process_common(type, connect_domain, connect_port,
            NULL, 0, stdin_fd, stdout_fd, stderr_fd, buffer_size);
    return exit_code;
}

/* Local Variables: */
/* mode: c */
/* indent-tabs-mode: nil */
/* c-basic-offset: 4 */
/* coding: utf-8-unix */
/* End: */
