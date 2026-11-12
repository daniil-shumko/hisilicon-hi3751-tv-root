/*
 * ktvsud -- root command daemon
 *
 * Started by init from /system/etc/init/ktvsu.rc with user root and
 * seclabel u:r:su:s0. Listens on the abstract unix socket "ktvsu",
 * runs each received command through /system/bin/sh as root, and
 * streams stdout+stderr back to the client.
 *
 * This exists because Android's application sandbox sets
 * PR_SET_NO_NEW_PRIVS, which silently discards the setuid bit on
 * exec. A setuid su binary returns EPERM on setuid(0). A daemon that
 * init starts as root does not have that problem.
 *
 * There is no authentication. See docs/05-app-level-root.md.
 *
 * Build: see Makefile (armv7a-linux-androideabi21)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#define SOCK_NAME "ktvsu"
#define TRAILER "KTVEXIT"
#define TRAILER_LEN 12
#define CMD_MAX (1 << 20)

static int read_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, (const char *)buf + off, n - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static void serve(int fd) {
    unsigned char hdr[4];
    if (read_full(fd, hdr, 4) != 0) { close(fd); return; }

    unsigned int len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((unsigned)hdr[3] << 24);
    if (len == 0 || len > CMD_MAX) { close(fd); return; }

    char *cmd = calloc(1, len + 1);
    if (!cmd || read_full(fd, cmd, len) != 0) {
        free(cmd);
        close(fd);
        return;
    }

    int pfd[2];
    if (pipe(pfd) != 0) { free(cmd); close(fd); return; }

    pid_t pid = fork();
    if (pid == 0) {
        /* child: root shell, stdout+stderr into the pipe */
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        setenv("PATH",
               "/sbin:/system/sbin:/system/bin:/system/xbin:/odm/bin:/vendor/bin:/vendor/xbin",
               1);
        setenv("USER", "root", 1);
        setenv("HOME", "/data/local/tmp", 1);
        execl("/system/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    if (pid < 0) {
        free(cmd);
        close(fd);
        return;
    }

    char buf[8192];
    ssize_t n;
    while ((n = read(pfd[0], buf, sizeof(buf))) > 0) {
        if (write_full(fd, buf, (size_t)n) != 0) break;
    }
    close(pfd[0]);

    int st = 0;
    waitpid(pid, &st, 0);
    int code;
    if (WIFEXITED(st))         code = WEXITSTATUS(st);
    else if (WIFSIGNALED(st))  code = 128 + WTERMSIG(st);
    else                       code = 1;

    unsigned char tail[TRAILER_LEN];
    memset(tail, 0, sizeof(tail));
    memcpy(tail, TRAILER, 7);
    tail[8] = code & 0xff;
    tail[9] = (code >> 8) & 0xff;
    write_full(fd, tail, sizeof(tail));

    free(cmd);
    close(fd);
}

int main(void) {
    signal(SIGCHLD, SIG_IGN);  /* children are reaped via waitpid in serve() */
    signal(SIGPIPE, SIG_IGN);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    sa.sun_path[0] = 0;
    memcpy(sa.sun_path + 1, SOCK_NAME, strlen(SOCK_NAME));

    unlink("/dev/socket/" SOCK_NAME);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        /* abstract namespace unavailable: use a filesystem socket */
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, "/dev/socket/" SOCK_NAME);
        if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            perror("bind");
            return 1;
        }
        chmod("/dev/socket/" SOCK_NAME, 0666);
    }

    if (listen(s, 16) < 0) {
        perror("listen");
        return 1;
    }

    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            usleep(200000);
            continue;
        }
        pid_t p = fork();
        if (p == 0) {
            close(s);
            serve(c);
            _exit(0);
        }
        close(c);
    }
    return 0;
}
