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

#define SOCK_REQ      "ktvsu"
#define SOCK_ADM      "ktvsuadm"
#define STATE_DIR     "/data/misc/ktvsu"
#define POLICY_FILE   STATE_DIR "/policy"
#define MODE_FILE     STATE_DIR "/mode"
#define ADMIN_FILE    STATE_DIR "/admin_uid"
#define PEND_DIR      STATE_DIR "/pending"
#define ANS_DIR       STATE_DIR "/answer"

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

static void run_and_stream(int fd, const char *cmd) {
    int pfd[2];
    if (pipe(pfd) != 0) return;

    pid_t pid = fork();
    if (pid == 0) {
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

    unsigned char tail[TRAILER_LEN];
    memset(tail, 0, sizeof(tail));
    memcpy(tail, TRAILER, 7);
    tail[8] = code & 0xff;
    tail[9] = (code >> 8) & 0xff;
    write_full(fd, tail, sizeof(tail));
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

static void serve_client(int fd) {
    uid_t uid = peer_uid(fd);
    if (uid == (uid_t)-1) { close(fd); return; }

    unsigned char hdr[4];
    if (read_full(fd, hdr, 4) != 0) { close(fd); return; }
    unsigned int len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((unsigned)hdr[3] << 24);
    if (len == 0 || len > CMD_MAX) { close(fd); return; }

    char *cmd = calloc(1, len + 1);
    if (!cmd || read_full(fd, cmd, len) != 0) { free(cmd); close(fd); return; }

    char rule[16], mode[32];
    policy_for(uid, rule, sizeof(rule));
    read_mode(mode, sizeof(mode));

    if (!strcmp(mode, "deny-all") && strcmp(rule, "allow")) {
        deny(fd, "root access disabled (deny-all mode)");
        free(cmd); close(fd); return;
    }
    if (!strcmp(rule, "deny")) {
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
        fprintf(f, "%u\n%s\n%s\n", (unsigned)uid, pkg, cmd);
        fclose(f);
        chmod(path, 0600);

        int remember = 0;
        int allowed = wait_for_answer(id, &remember);

        unlink(path);
        if (allowed) {
            if (remember && strcmp(mode, "allow-all")) policy_set(uid, "allow");
        } else {
            deny(fd, "request denied");
            free(cmd); close(fd); return;
        }
    }

    run_and_stream(fd, cmd);
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
