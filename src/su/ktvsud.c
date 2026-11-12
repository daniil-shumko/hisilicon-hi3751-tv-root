/*
 * ktvsud v2 -- root command daemon with per-app approval
 *
 * Sockets (both abstract namespace):
 *   "ktvsu"     client socket. Peer uid is read with SO_PEERCRED and checked
 *               against a policy file. Unknown uids are held pending until the
 *               Superuser app answers, or the request times out.
 *   "ktvsuadm"  admin socket. Only the uid in admin_uid may connect. The
 *               Superuser app uses this to list/answer requests and manage
 *               rules.
 *
 * State lives in /data/misc/ktvsu (0700 root):
 *   policy      lines of "<uid> allow|deny"
 *   mode        ask | allow-all | deny-all
 *   admin_uid   uid allowed to use the admin socket
 *   pending/    "<id>" files: line1 uid, line2 package, line3 command
 *   answer/     "<id>" files written by the daemon: "allow|deny [remember]"
 *
 * Client wire protocol is unchanged from v1:
 *   -> uint32 LE length, then the command bytes
 *   <- raw stdout/stderr, then "KTVEXIT\0" + uint16 LE exit code + 2 pad
 *      exit code 77 means "denied by policy"
 *
 * Build: see Makefile (armv7a-linux-androideabi21)
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <poll.h>
#include <pty.h>
#include <utmp.h>

#define SOCK_REQ      "ktvsu"
#define SOCK_ADM      "ktvsuadm"
#define STATE_DIR     "/data/misc/ktvsu"
#define POLICY_FILE   STATE_DIR "/policy"
#define MODE_FILE     STATE_DIR "/mode"
#define ADMIN_FILE    STATE_DIR "/admin_uid"
#define PEND_DIR      STATE_DIR "/pending"
#define ANS_DIR       STATE_DIR "/answer"
#define CALL_LOG      STATE_DIR "/calls.log"

#define TRAILER       "KTVEXIT"
#define TRAILER_LEN   12
#define CMD_MAX       (1 << 20)
#define KTV_LINE_MAX  8192
#define WAIT_SECONDS  120
#define CODE_DENIED   77

/* ------------------------------------------------------------------ utils */

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

static uid_t peer_uid(int fd) {
    struct ucred uc;
    socklen_t len = sizeof(uc);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &len) != 0) return (uid_t)-1;
    return uc.uid;
}

/* read one line, strip trailing newline. returns length or -1 */
static int read_line(int fd, char *buf, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) {
            if (i == 0) return -1;
            break;
        }
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = 0;
    return (int)i;
}

/* append a JSON-escaped copy of s to out */
static void json_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; s && *s && o + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20) { o += snprintf(out + o, cap - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

/* pull a string field out of a flat JSON object: "key":"value" */
static int json_str(const char *json, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == ':')) p++;
    if (*p != '"') return 0;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[o++] = '\n'; break;
                case 'r': out[o++] = '\r'; break;
                case 't': out[o++] = '\t'; break;
                default:  out[o++] = *p;   break;
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return 1;
}

static int json_bool(const char *json, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == ':')) p++;
    return strncmp(p, "true", 4) == 0;
}

/* ------------------------------------------------------------------ state */

static void ensure_state(void) {
    mkdir(STATE_DIR, 0700);
    chmod(STATE_DIR, 0700);
    mkdir(PEND_DIR, 0700);
    mkdir(ANS_DIR, 0700);
}

static void read_mode(char *out, size_t cap) {
    FILE *f = fopen(MODE_FILE, "r");
    if (!f) { snprintf(out, cap, "ask"); return; }
    if (!fgets(out, (int)cap, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
    if (!strcmp(out, "allow-all") || !strcmp(out, "deny-all")) return;
    snprintf(out, cap, "ask");
}

static void set_mode(const char *m) {
    FILE *f = fopen(MODE_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\n", m);
    fclose(f);
    chmod(MODE_FILE, 0600);
}

static uid_t read_admin_uid(void) {
    FILE *f = fopen(ADMIN_FILE, "r");
    if (!f) return (uid_t)-1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return (uid_t)v;
}

/* returns "allow", "deny" or "" (no rule) */
static void policy_for(uid_t uid, char *out, size_t cap) {
    out[0] = 0;
    FILE *f = fopen(POLICY_FILE, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        unsigned long u = 0;
        char val[16];
        if (sscanf(line, "%lu %15s", &u, val) == 2 && (uid_t)u == uid) {
            snprintf(out, cap, "%s", val);
            break;
        }
    }
    fclose(f);
}

static void policy_set(uid_t uid, const char *val) {
    /* rewrite the file without this uid, then append */
    char tmp[64];
    snprintf(tmp, sizeof(tmp), POLICY_FILE ".tmp");
    FILE *in = fopen(POLICY_FILE, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return; }
    if (in) {
        char line[128];
        while (fgets(line, sizeof(line), in)) {
            unsigned long u = 0;
            if (sscanf(line, "%lu", &u) == 1 && (uid_t)u == uid) continue;
            fputs(line, out);
        }
        fclose(in);
    }
    fprintf(out, "%lu %s\n", (unsigned long)uid, val);
    fclose(out);
    chmod(tmp, 0600);
    rename(tmp, POLICY_FILE);
}

static void policy_remove(uid_t uid) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), POLICY_FILE ".tmp");
    FILE *in = fopen(POLICY_FILE, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return; }
    if (in) {
        char line[128];
        while (fgets(line, sizeof(line), in)) {
            unsigned long u = 0;
            if (sscanf(line, "%lu", &u) == 1 && (uid_t)u == uid) continue;
            fputs(line, out);
        }
        fclose(in);
    }
    fclose(out);
    chmod(tmp, 0600);
    rename(tmp, POLICY_FILE);
}

/* uid -> package name, from the package database */
static void pkg_for_uid(uid_t uid, char *out, size_t cap) {
    snprintf(out, cap, "uid %u", (unsigned)uid);
    FILE *f = fopen("/data/system/packages.list", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char name[256];
        unsigned long u = 0;
        if (sscanf(line, "%255s %lu", name, &u) == 2 && (uid_t)u == uid) {
            snprintf(out, cap, "%s", name);
            break;
        }
    }
    fclose(f);
}

/* ---------------------------------------------------------------- request */

static void send_trailer(int fd, int code) {
    unsigned char tail[TRAILER_LEN];
    memset(tail, 0, sizeof(tail));
    memcpy(tail, TRAILER, 7);
    tail[8] = code & 0xff;
    tail[9] = (code >> 8) & 0xff;
    write_full(fd, tail, sizeof(tail));
}

static void child_env(void) {
    setenv("PATH",
           "/sbin:/system/sbin:/system/bin:/system/xbin:/odm/bin:/vendor/bin:/vendor/xbin", 1);
    setenv("USER", "root", 1);
    setenv("HOME", "/data/local/tmp", 1);
    setenv("TERM", "dumb", 1);
}

/* one command: output only */
static void run_and_stream(int fd, const char *cmd) {
    int pfd[2];
    if (pipe(pfd) != 0) return;

    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        child_env();
        execl("/system/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    if (pid < 0) { close(pfd[0]); return; }

    char buf[8192];
    ssize_t n;
    while ((n = read(pfd[0], buf, sizeof(buf))) > 0) {
        if (write_full(fd, buf, (size_t)n) != 0) break;
    }
    close(pfd[0]);

    int st = 0, code = 1;
    if (pid > 0 && waitpid(pid, &st, 0) == pid) {
        if (WIFEXITED(st))        code = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) code = 128 + WTERMSIG(st);
    }
    send_trailer(fd, code);
}

/* Interactive root shell, on a pty.
 *
 * A pty is not a luxury here. Root-aware apps spawn `su` and wait for the
 * shell to announce itself; with plain pipes there is no prompt, no job
 * control and no line discipline, and a 5 s read timeout turns into
 * "could not obtain root". FX File Explorer is exactly that case:
 *
 *   W nextapp.cat: Error querying /proc/mounts.
 *   W nextapp.cat: ze.i$b: Interactive Shell Error
 */
static void run_interactive(int fd) {
    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, NULL);
    if (pid < 0) { send_trailer(fd, 1); return; }

    if (pid == 0) {
        child_env();
        setenv("PS1", "# ", 1);
        setenv("PS2", "> ", 1);
        execl("/system/bin/sh", "sh", "-i", (char *)NULL);
        execl("/system/bin/sh", "sh", (char *)NULL);
        _exit(127);
    }

    char buf[8192];
    int stdin_open = 1;

    for (;;) {
        struct pollfd fds[2];
        int nf = 0;
        fds[nf].fd = master;
        fds[nf].events = POLLIN;
        fds[nf].revents = 0;
        nf++;
        if (stdin_open) {
            fds[nf].fd = fd;
            fds[nf].events = POLLIN;
            fds[nf].revents = 0;
            nf++;
        }

        int rc = poll(fds, (nfds_t)nf, 500);
        if (rc < 0 && errno != EINTR) break;

        if (rc > 0) {
            if (fds[0].revents & (POLLIN | POLLHUP)) {
                ssize_t n = read(master, buf, sizeof(buf));
                if (n > 0) {
                    if (write_full(fd, buf, (size_t)n) != 0) break;
                } else if (n == 0) {
                    break;
                } else if (errno == EIO) {
                    break;              /* child has gone */
                }
            }
            if (nf > 1 && (fds[1].revents & (POLLIN | POLLHUP))) {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n > 0) {
                    if (write_full(master, buf, (size_t)n) != 0) { stdin_open = 0; }
                } else {
                    stdin_open = 0;
                }
            }
        }

        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            ssize_t n;
            while ((n = read(master, buf, sizeof(buf))) > 0) {
                if (write_full(fd, buf, (size_t)n) != 0) break;
            }
            int code = WIFEXITED(st) ? WEXITSTATUS(st)
                     : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1);
            send_trailer(fd, code);
            close(master);
            return;
        }
    }

    close(master);
    send_trailer(fd, 1);
}

static void deny(int fd, const char *why) {
    dprintf(fd, "su: %s\n", why);
    unsigned char tail[TRAILER_LEN];
    memset(tail, 0, sizeof(tail));
    memcpy(tail, TRAILER, 7);
    tail[8] = CODE_DENIED & 0xff;
    tail[9] = (CODE_DENIED >> 8) & 0xff;
    write_full(fd, tail, sizeof(tail));
}

/* wait for the Superuser app to answer. returns 1 allow, 0 deny */
static int wait_for_answer(const char *id, int *remember) {
    char path[256];
    snprintf(path, sizeof(path), ANS_DIR "/%s", id);
    for (int i = 0; i < WAIT_SECONDS * 5; i++) {
        FILE *f = fopen(path, "r");
        if (f) {
            char val[32] = {0};
            if (!fgets(val, sizeof(val), f)) val[0] = 0;
            fclose(f);
            unlink(path);
            *remember = strstr(val, "remember") != NULL;
            return strncmp(val, "allow", 5) == 0;
        }
        usleep(200000);
    }
    return 0;
}

/* Append one line per request. Root-only, rotated at 128 KB.
 *
 * This is the difference between guessing why an app "cannot get root" and
 * knowing: it records the uid, the command (or "interactive shell") and the
 * verdict. */
static void log_call(uid_t uid, const char *cmd, const char *verdict) {
    struct stat st;
    if (stat(CALL_LOG, &st) == 0 && st.st_size > 128 * 1024) {
        char keep[80];
        snprintf(keep, sizeof(keep), "%s.old", CALL_LOG);
        rename(CALL_LOG, keep);
    }
    FILE *f = fopen(CALL_LOG, "a");
    if (!f) return;
    char head[160];
    size_t i = 0;
    if (cmd) {
        for (; cmd[i] && i < sizeof(head) - 1; i++) {
            char c = cmd[i];
            head[i] = (c == '\n' || c == '\r') ? ' ' : c;
        }
    }
    head[i] = 0;
    fprintf(f, "%ld uid=%u %s :: %s\n", (long)time(NULL), (unsigned)uid,
            cmd ? (head[0] ? head : "(empty)") : "(interactive shell)", verdict);
    fclose(f);
    chmod(CALL_LOG, 0600);
}

static void serve_client(int fd) {
    uid_t uid = peer_uid(fd);
    if (uid == (uid_t)-1) { close(fd); return; }

    unsigned char hdr[4];
    if (read_full(fd, hdr, 4) != 0) { close(fd); return; }
    unsigned int len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((unsigned)hdr[3] << 24);
    if (len > CMD_MAX) { close(fd); return; }

    /* len == 0 means "give me an interactive root shell" */
    char *cmd = NULL;
    if (len > 0) {
        cmd = calloc(1, len + 1);
        if (!cmd || read_full(fd, cmd, len) != 0) { free(cmd); close(fd); return; }
    }

    char rule[16], mode[32];
    policy_for(uid, rule, sizeof(rule));
    read_mode(mode, sizeof(mode));

    if (!strcmp(mode, "deny-all") && strcmp(rule, "allow")) {
        log_call(uid, cmd, "deny (deny-all mode)");
        deny(fd, "root access disabled (deny-all mode)");
        free(cmd); close(fd); return;
    }
    if (!strcmp(rule, "deny")) {
        log_call(uid, cmd, "deny (policy)");
        deny(fd, "denied by policy");
        free(cmd); close(fd); return;
    }
    if (strcmp(rule, "allow") && strcmp(mode, "allow-all")) {
        /* no rule and we are in ask mode: queue a request */
        char id[32];
        snprintf(id, sizeof(id), "%ld%u", (long)time(NULL), (unsigned)(getpid() % 100000));
        char pkg[256];
        pkg_for_uid(uid, pkg, sizeof(pkg));

        char path[256];
        snprintf(path, sizeof(path), PEND_DIR "/%s", id);
        FILE *f = fopen(path, "w");
        if (!f) { deny(fd, "cannot queue request"); free(cmd); close(fd); return; }
        fprintf(f, "%u\n%s\n%s\n", (unsigned)uid, pkg, cmd ? cmd : "(interactive shell)");
        fclose(f);
        chmod(path, 0600);

        /* Raise the prompt from *here*, not from the app.
         *
         * Android 14 blocks background activity starts (BAL_BLOCK) from a
         * foreground service, and SYSTEM_ALERT_WINDOW is not enough after a
         * reboot. A start issued by uid 0 is allowlisted, so the daemon --
         * which is root -- asks the system to show the dialog. The app then
         * only has to read the request and answer it. */
        pid_t launcher = fork();
        if (launcher == 0) {
            execl("/system/bin/am", "am", "start", "-n",
                  "lan.ktvsu.manager/.PromptActivity",
                  "--es", "id", id,
                  (char *)NULL);
            _exit(127);
        }
        if (launcher > 0) {
            int st;
            waitpid(launcher, &st, 0);
        }

        int remember = 0;
        int allowed = wait_for_answer(id, &remember);

        unlink(path);
        if (allowed) {
            if (remember && strcmp(mode, "allow-all")) policy_set(uid, "allow");
            log_call(uid, cmd, "allow (approved)");
        } else {
            log_call(uid, cmd, "deny (rejected/timeout)");
            deny(fd, "request denied");
            free(cmd); close(fd); return;
        }
    }

    if (strcmp(rule, "allow") == 0 || !strcmp(mode, "allow-all")) {
        log_call(uid, cmd, "allow (rule)");
    }

    if (cmd) {
        run_and_stream(fd, cmd);
    } else {
        run_interactive(fd);
    }
    free(cmd);
    close(fd);
}

/* ------------------------------------------------------------------ admin */

static void admin_reply(int fd, const char *json) {
    write_full(fd, json, strlen(json));
    write_full(fd, "\n", 1);
}

static void serve_admin(int fd) {
    uid_t admin = read_admin_uid();
    if (admin == (uid_t)-1 || peer_uid(fd) != admin) {
        admin_reply(fd, "{\"ok\":false,\"error\":\"not the superuser app\"}");
        close(fd);
        return;
    }

    char req[KTV_LINE_MAX];
    if (read_line(fd, req, sizeof(req)) <= 0) { close(fd); return; }

    char op[32] = {0};
    json_str(req, "op", op, sizeof(op));

    if (!strcmp(op, "hello") || !strcmp(op, "ping")) {
        char mode[32];
        read_mode(mode, sizeof(mode));
        char out[160];
        snprintf(out, sizeof(out), "{\"ok\":true,\"mode\":\"%s\",\"version\":2}", mode);
        admin_reply(fd, out);
    } else if (!strcmp(op, "pending")) {
        DIR *d = opendir(PEND_DIR);
        char body[KTV_LINE_MAX];
        size_t o = 0;
        o += snprintf(body + o, sizeof(body) - o, "{\"ok\":true,\"pending\":[");
        int first = 1;
        struct dirent *e;
        if (d) {
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                char path[300];
                snprintf(path, sizeof(path), PEND_DIR "/%s", e->d_name);
                FILE *f = fopen(path, "r");
                if (!f) continue;
                char suid[32] = {0}, spkg[256] = {0}, scmd[1024] = {0};
                if (!fgets(suid, sizeof(suid), f)) { fclose(f); continue; }
                if (!fgets(spkg, sizeof(spkg), f)) spkg[0] = 0;
                if (!fgets(scmd, sizeof(scmd), f)) scmd[0] = 0;
                fclose(f);
                suid[strcspn(suid, "\n")] = 0;
                spkg[strcspn(spkg, "\n")] = 0;
                scmd[strcspn(scmd, "\n")] = 0;
                char epkg[600], ecmd[2200], eid[128];
                json_escape(spkg, epkg, sizeof(epkg));
                json_escape(scmd, ecmd, sizeof(ecmd));
                json_escape(e->d_name, eid, sizeof(eid));
                o += snprintf(body + o, sizeof(body) - o,
                              "%s{\"id\":\"%s\",\"uid\":%s,\"pkg\":\"%s\",\"cmd\":\"%s\"}",
                              first ? "" : ",", eid, suid[0] ? suid : "0", epkg, ecmd);
                first = 0;
                if (o > sizeof(body) - 512) break;
            }
            closedir(d);
        }
        snprintf(body + o, sizeof(body) - o, "]}");
        admin_reply(fd, body);
    } else if (!strcmp(op, "answer")) {
        char id[64] = {0};
        json_str(req, "id", id, sizeof(id));
        int allow = json_bool(req, "allow");
        int remember = json_bool(req, "remember");
        if (!id[0]) { admin_reply(fd, "{\"ok\":false,\"error\":\"no id\"}"); close(fd); return; }
        char path[256];
        snprintf(path, sizeof(path), ANS_DIR "/%s", id);
        FILE *f = fopen(path, "w");
        if (!f) { admin_reply(fd, "{\"ok\":false,\"error\":\"cannot write answer\"}"); close(fd); return; }
        fprintf(f, "%s%s\n", allow ? "allow" : "deny", remember ? " remember" : "");
        fclose(f);
        chmod(path, 0600);
        admin_reply(fd, "{\"ok\":true}");
    } else if (!strcmp(op, "rules")) {
        FILE *f = fopen(POLICY_FILE, "r");
        char body[KTV_LINE_MAX];
        size_t o = 0;
        o += snprintf(body + o, sizeof(body) - o, "{\"ok\":true,\"rules\":[");
        int first = 1;
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                unsigned long u = 0;
                char val[16] = {0};
                if (sscanf(line, "%lu %15s", &u, val) != 2) continue;
                char pkg[256];
                pkg_for_uid((uid_t)u, pkg, sizeof(pkg));
                char epkg[600];
                json_escape(pkg, epkg, sizeof(epkg));
                o += snprintf(body + o, sizeof(body) - o,
                              "%s{\"uid\":%lu,\"pkg\":\"%s\",\"policy\":\"%s\"}",
                              first ? "" : ",", u, epkg, val);
                first = 0;
                if (o > sizeof(body) - 400) break;
            }
            fclose(f);
        }
        snprintf(body + o, sizeof(body) - o, "]}");
        admin_reply(fd, body);
    } else if (!strcmp(op, "revoke")) {
        char suid[32] = {0};
        json_str(req, "uid", suid, sizeof(suid));
        unsigned long u = strtoul(suid, NULL, 10);
        policy_remove((uid_t)u);
        admin_reply(fd, "{\"ok\":true}");
    } else if (!strcmp(op, "mode")) {
        char val[32] = {0};
        json_str(req, "value", val, sizeof(val));
        if (strcmp(val, "ask") && strcmp(val, "allow-all") && strcmp(val, "deny-all")) {
            admin_reply(fd, "{\"ok\":false,\"error\":\"bad mode\"}");
        } else {
            set_mode(val);
            admin_reply(fd, "{\"ok\":true}");
        }
    } else {
        admin_reply(fd, "{\"ok\":false,\"error\":\"unknown op\"}");
    }
    close(fd);
}

/* ------------------------------------------------------------------- main */

static int bind_abstract(const char *name) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    sa.sun_path[0] = 0;
    snprintf(sa.sun_path + 1, sizeof(sa.sun_path) - 1, "%s", name);
    /* The length MUST be offsetof(sun_path) + 1 + strlen(name).
     * Binding with sizeof(sa) records the name with all the trailing NULs
     * included, which no standard client can match -- android.net.LocalSocket
     * connects with the exact length and gets ECONNREFUSED. */
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name));
    if (bind(s, (struct sockaddr *)&sa, len) < 0) { close(s); return -1; }
    if (listen(s, 16) < 0) { close(s); return -1; }
    return s;
}

static void reap(int sig) {
    (void)sig;
    int st;
    while (waitpid(-1, &st, WNOHANG) > 0) { }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, reap);

    ensure_state();
    if (access(MODE_FILE, F_OK) != 0) set_mode("ask");

    int sreq = bind_abstract(SOCK_REQ);
    if (sreq < 0) { perror("bind " SOCK_REQ); return 1; }
    int sadm = bind_abstract(SOCK_ADM);
    if (sadm < 0) { perror("bind " SOCK_ADM); return 1; }

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sreq, &rfds);
        FD_SET(sadm, &rfds);
        int maxfd = sreq > sadm ? sreq : sadm;
        int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) continue;
            usleep(100000);
            continue;
        }

        for (int which = 0; which < 2; which++) {
            int lsn = which ? sadm : sreq;
            if (!FD_ISSET(lsn, &rfds)) continue;
            int c = accept(lsn, NULL, NULL);
            if (c < 0) continue;
            pid_t p = fork();
            if (p == 0) {
                signal(SIGCHLD, SIG_DFL);   /* this child must reap its own shell */
                close(sreq);
                close(sadm);
                if (which) serve_admin(c);
                else       serve_client(c);
                _exit(0);
            }
            close(c);
        }
    }
    return 0;
}
