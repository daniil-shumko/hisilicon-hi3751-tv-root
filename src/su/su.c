/*
 * su -- client for ktvsud
 *
 * Runs inside the caller's sandbox with no elevated privileges at all.
 * It connects to the ktvsud root daemon over an abstract unix socket,
 * sends a shell command, streams the output back, and exits with the
 * daemon's exit code.
 *
 * Protocol:
 *   -> uint32 LE length
 *   -> length bytes of command
 *   <- raw stdout/stderr stream
 *   <- "KTVEXIT\0" + uint16 LE exit code + 2 pad bytes
 *
 * Build: see Makefile (armv7a-linux-androideabi21)
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <pthread.h>

#define SOCK_NAME "ktvsu"
#define TRAILER_LEN 12
#define CMD_MAX 65536

/* copy our stdin into the socket so the remote shell sees real input */
static void *pump_stdin(void *arg) {
    int fd = (int)(long)arg;
    char buf[4096];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        if (write(fd, buf, (size_t)n) != n) break;
    }
    shutdown(fd, SHUT_WR);
    return NULL;
}

static int connect_daemon(void) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    sa.sun_path[0] = 0; /* abstract namespace */
    memcpy(sa.sun_path + 1, SOCK_NAME, strlen(SOCK_NAME));

    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SOCK_NAME));
    if (connect(s, (struct sockaddr *)&sa, len) == 0) return s;

    /* fall back to the filesystem socket the daemon writes if abstract fails */
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, "/dev/socket/" SOCK_NAME);
    len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(sa.sun_path) + 1);
    if (connect(s, (struct sockaddr *)&sa, len) == 0) return s;

    close(s);
    return -1;
}

static int start_stdin_pump(int s) {
    pthread_t t;
    if (pthread_create(&t, NULL, pump_stdin, (void *)(long)s) != 0) return -1;
    pthread_detach(t);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v") || !strcmp(argv[1], "-V"))) {
        printf("su (ktvsu) 1.0\n");
        return 0;
    }

    /* Option parsing compatible with what real root apps send.
     *
     * Magisk's su accepts a pile of flags and apps probe for *it* by using
     * them. FX File Explorer runs `su -mm` and expects a shell. Treating
     * "-mm" as a command makes it conclude root is unavailable:
     *
     *   uid=10075 -mm :: allow (rule)     <- from the daemon call log
     *
     *   -c CMD                    one-shot
     *   -s, --shell, -l, --login  interactive
     *   -mm, -M, --mount-master   global mount namespace. The daemon already
     *                             lives in init's namespace, so this is a
     *                             no-op we accept instead of rejecting.
     *   -p, -u, --preserve-environment, unknown "-x"   ignored
     *   -v/--version, -h/--help
     *   "0" then words            legacy `su 0 cmd`
     * First bare word and everything after it is the command. */

    int interactive = 1;
    int start = argc;
    static char cmd[CMD_MAX];
    cmd[0] = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-c") || !strcmp(a, "--command")) {
            start = i + 1;
            interactive = 0;
            break;
        }
        if (!strcmp(a, "-s") || !strcmp(a, "--shell") || !strcmp(a, "-l") || !strcmp(a, "--login")) {
            interactive = 1;
            continue;
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: su [-c CMD] [-s] [-mm] [CMD...]\n");
            return 0;
        }
        if (!strcmp(a, "0")) {
            start = i + 1;
            interactive = 0;
            break;
        }
        if (a[0] == '-') continue;     /* -mm, -M, -p, --mount-master, ... */
        start = i;
        interactive = 0;
        break;
    }

    if (!interactive) {
        for (int i = start; i < argc; i++) {
            if (cmd[0] == 0 && i == start) strncat(cmd, argv[i], sizeof(cmd) - 1);
            else {
                strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
                strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
            }
        }
        if (cmd[0] == 0) interactive = 1;   /* e.g. a lone "-c" with no command */
    }

    if (getuid() == 0) {
        /* already root: no daemon round trip needed */
        if (interactive) {
            execl("/system/bin/sh", "sh", (char *)NULL);
        } else {
            execl("/system/bin/sh", "sh", "-c", cmd, (char *)NULL);
        }
        perror("su: exec");
        return 127;
    }

    size_t len = interactive ? 0 : strlen(cmd);

    int s = connect_daemon();
    if (s < 0) {
        fprintf(stderr, "su: root daemon not running\n");
        return 1;
    }

    unsigned char hdr[4];
    unsigned int l = (unsigned int)len;
    hdr[0] = l & 0xff;
    hdr[1] = (l >> 8) & 0xff;
    hdr[2] = (l >> 16) & 0xff;
    hdr[3] = (l >> 24) & 0xff;
    if (interactive) { }

    if (write(s, hdr, 4) != 4 || (len > 0 && write(s, cmd, len) != (ssize_t)len)) {
        fprintf(stderr, "su: send failed\n");
        close(s);
        return 1;
    }

    if (interactive) {
        start_stdin_pump(s);
    }

    /* stream everything, holding back the last TRAILER_LEN bytes so we can
     * recognise the exit-code trailer without buffering the whole output */
    char buf[8192];
    char hold[TRAILER_LEN];
    int hn = 0;
    ssize_t n;
    while ((n = read(s, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (hn == TRAILER_LEN) {
                write(STDOUT_FILENO, hold, 1);
                memmove(hold, hold + 1, TRAILER_LEN - 1);
                hn = TRAILER_LEN - 1;
            }
            hold[hn++] = buf[i];
        }
    }
    close(s);

    int code = 0;
    if (hn == TRAILER_LEN && !memcmp(hold, "KTVEXIT", 7)) {
        code = (unsigned char)hold[8] | ((unsigned char)hold[9] << 8);
    } else if (hn > 0) {
        /* no trailer: just flush what we had */
        write(STDOUT_FILENO, hold, (size_t)hn);
    }
    return code;
}
