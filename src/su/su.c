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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SOCK_NAME "ktvsu"
#define TRAILER_LEN 12
#define CMD_MAX 65536

static int connect_daemon(void) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    sa.sun_path[0] = 0; /* abstract namespace */
    memcpy(sa.sun_path + 1, SOCK_NAME, strlen(SOCK_NAME));

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) return s;

    /* fall back to the filesystem socket the daemon writes if abstract fails */
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, "/dev/socket/" SOCK_NAME);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) return s;

    close(s);
    return -1;
}

int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("su (ktvsu) 1.0\n");
        return 0;
    }

    if (getuid() == 0) {
        /* already root: skip the daemon entirely */
        int start = 1;
        if (argc > 1 && !strcmp(argv[1], "-c")) start = 2;
        if (start >= argc) {
            execl("/system/bin/sh", "sh", (char *)NULL);
            return 127;
        }
        size_t n = 0;
        for (int i = start; i < argc; i++) n += strlen(argv[i]) + 1;
        char *cmd = calloc(1, n + 1);
        if (!cmd) return 1;
        for (int i = start; i < argc; i++) {
            if (i > start) strcat(cmd, " ");
            strcat(cmd, argv[i]);
        }
        execl("/system/bin/sh", "sh", "-c", cmd, (char *)NULL);
        return 127;
    }

    int start = 1;
    if (argc > 1 && (!strcmp(argv[1], "-c") || !strcmp(argv[1], "0"))) start = 2;

    static char cmd[CMD_MAX];
    cmd[0] = 0;
    if (start >= argc) {
        strcpy(cmd, "exec /system/bin/sh");
    } else {
        for (int i = start; i < argc; i++) {
            size_t cur = strlen(cmd);
            if (i > start) strncat(cmd, " ", sizeof(cmd) - cur - 1);
            strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
        }
    }
    size_t len = strlen(cmd);

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
    if (write(s, hdr, 4) != 4 || write(s, cmd, len) != (ssize_t)len) {
        fprintf(stderr, "su: send failed\n");
        close(s);
        return 1;
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
