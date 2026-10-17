/*
 * SoulX Android Emulator - macOS emulator console client
 *
 * Talks to the Android Emulator console on 127.0.0.1:<console port>.
 * Protocol: a banner ending with "OK", optional "auth <token>" (token from
 * $SOULX_HOME/home/.emulator_console_auth_token, because HOME points there for
 * the engine), then one command per line; every reply ends with a line "OK"
 * (or "OK: ...") or "KO: <message>". Lines end with CRLF.
 * A SoulXConsole is not thread-safe; reconnect logic lives in callers.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "soulx_mac.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define CONSOLE_CONNECT_MS  3000
#define CONSOLE_RECV_MS     10000
#define CONSOLE_POLL_SLICE  200            // ms; keeps Ctrl+C responsive
#define CONSOLE_BUF         16384
#define CONSOLE_LINE_MAX    4096
#define CONSOLE_CMD_MAX     4000
#define CONSOLE_BANNER_MAX  64             // lines
#define CONSOLE_TOKEN_MAX   512
#define CONSOLE_GLUED_OK_MS 300            // see reply_ends_glued()

// How the last soulx_console_cmd() failed (soulx_console_failure()).
#define CONSOLE_FAIL_NONE       0           // it did not fail
#define CONSOLE_FAIL_NOT_SENT   1           // the command was not (completely) sent: it did not run
#define CONSOLE_FAIL_CLOSED     2           // sent, then the connection closed before any reply byte
                                            // (e.g. the engine restarted): it almost certainly did not run
#define CONSOLE_FAIL_NO_ANSWER  3           // sent, but the reply timed out, was cut short or Ctrl+C
                                            // stopped the wait: it may have run
#define CONSOLE_FAIL_INVALID    4           // refused before sending (empty, too long or multi-line)

struct SoulXConsole {
    int fd;
    int port;
    bool broken;                            // stream out of sync / closed: callers must reconnect
    bool eof;                               // the peer closed the connection (or reset it)
    int last_fail;                          // CONSOLE_FAIL_* of the last command
    size_t received;                        // bytes received since the last command was sent
    size_t len;                             // bytes buffered in buf
    char buf[CONSOLE_BUF];
};

// Exported for input.c (not part of soulx_mac.h): CONSOLE_FAIL_* of the last soulx_console_cmd() on c,
// so that a caller can resend a command that never reached the emulator, but not one that may have run.
int soulx_console_failure(const SoulXConsole* c);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void secure_zero(void* p, size_t n) {
    volatile unsigned char* v = p;
    while (n--) *v++ = 0;
}

static int set_nonblocking(int fd, bool on) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags);
}

// Connects to 127.0.0.1:port within CONSOLE_CONNECT_MS. Returns the fd or -1.
static int console_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));   // no SIGPIPE on a closed peer
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (set_nonblocking(fd, true) != 0) goto fail;
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        if (errno != EINPROGRESS) goto fail;
        double deadline = soulx_now_ms() + CONSOLE_CONNECT_MS;
        for (;;) {
            int left = (int)(deadline - soulx_now_ms());
            if (left <= 0 || soulx_interrupted()) goto fail;
            struct pollfd pfd = { fd, POLLOUT, 0 };
            int pr = poll(&pfd, 1, left < CONSOLE_POLL_SLICE ? left : CONSOLE_POLL_SLICE);
            if (pr < 0 && errno != EINTR) goto fail;
            if (pr > 0) break;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            errno = err ? err : errno;
            goto fail;
        }
    }
    if (set_nonblocking(fd, false) != 0) goto fail;
    struct timeval tv = { CONSOLE_RECV_MS / 1000, 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;

fail:
    {
        int saved = errno;
        close(fd);
        errno = saved;
    }
    return -1;
}

static int send_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t w = send(fd, data, len, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        data += w;
        len -= (size_t)w;
    }
    return 0;
}

// Reads one line (CR/LF and trailing blanks stripped, truncated to cap - 1
// characters; the whole line is always consumed). Returns its length or -1 on
// EOF, error, timeout or interruption (c->eof tells a closed connection apart).
static int read_line(SoulXConsole* c, char* line, size_t cap, int timeout_ms) {
    double deadline = soulx_now_ms() + timeout_ms;
    for (;;) {
        char* nl = memchr(c->buf, '\n', c->len);
        if (nl || c->len == sizeof(c->buf) || (c->eof && c->len > 0)) {
            size_t n = nl ? (size_t)(nl - c->buf) : c->len;
            size_t consumed = nl ? n + 1 : n;
            while (n > 0 && (c->buf[n - 1] == '\r' || c->buf[n - 1] == ' ' || c->buf[n - 1] == '\t')) n--;
            size_t copy = n < cap - 1 ? n : cap - 1;
            memcpy(line, c->buf, copy);
            line[copy] = '\0';
            memmove(c->buf, c->buf + consumed, c->len - consumed);
            c->len -= consumed;
            return (int)copy;
        }
        if (c->eof) return -1;

        int left = (int)(deadline - soulx_now_ms());
        if (left <= 0 || soulx_interrupted()) return -1;
        struct pollfd pfd = { c->fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, left < CONSOLE_POLL_SLICE ? left : CONSOLE_POLL_SLICE);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) continue;
        ssize_t r = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len, 0);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            c->eof = true;                                      // ECONNRESET and friends: closed
            return -1;
        }
        if (r == 0) {
            c->eof = true;
            continue;
        }
        c->len += (size_t)r;
        c->received += (size_t)r;
    }
}

static bool is_ok_line(const char* l) {
    return strcmp(l, "OK") == 0;
}

// Character tests (short-circuit) so l[2] is only read when l[0..1] matched.
static bool is_ok_message(const char* l) {                     // "OK: killing emulator, bye bye"
    return l[0] == 'O' && l[1] == 'K' && l[2] == ':';
}

static bool is_ko_line(const char* l) {                        // "KO: <message>"
    return l[0] == 'K' && l[1] == 'O' && (l[2] == '\0' || l[2] == ':' || l[2] == ' ');
}

// Some replies lack the CRLF before the final OK, e.g. `help resize-display` ends
// with "...to the size of tabletOK\r\n". A data line ending in "OK" after which
// nothing else arrives within CONSOLE_GLUED_OK_MS is taken as the end of the reply
// (the console writes a whole reply at once, so real data never pauses there).
static bool reply_ends_glued(SoulXConsole* c, const char* line) {
    size_t n = strlen(line);
    if (n <= 2 || strcmp(line + n - 2, "OK") != 0) return false;
    if (c->len > 0) return false;                               // more reply already buffered
    if (c->eof) return true;
    struct pollfd pfd = { c->fd, POLLIN, 0 };
    int pr;
    do {
        pr = poll(&pfd, 1, CONSOLE_GLUED_OK_MS);
    } while (pr < 0 && errno == EINTR);
    return pr == 0;
}

// Appends |text| as a new line of |reply| (truncating safely).
static void reply_append(char* reply, size_t cap, size_t* used, const char* text) {
    if (!reply || cap == 0) return;
    if (*used > 0 && *used < cap - 1) {
        reply[(*used)++] = '\n';
        reply[*used] = '\0';
    }
    size_t room = cap - 1 - *used;
    size_t n = strlen(text);
    if (n > room) n = room;
    memcpy(reply + *used, text, n);
    *used += n;
    reply[*used] = '\0';
}

// Reads the banner up to its "OK". Returns 0 and whether auth is required.
static int read_banner(SoulXConsole* c, bool* auth_required) {
    char line[CONSOLE_LINE_MAX];
    *auth_required = false;
    for (int i = 0; i < CONSOLE_BANNER_MAX; i++) {
        if (read_line(c, line, sizeof(line), CONSOLE_RECV_MS) < 0) return -1;
        if (is_ok_line(line)) return 0;
        if (is_ko_line(line)) return -1;
        if (strstr(line, "Authentication required") || strstr(line, "auth <auth_token>")) *auth_required = true;
    }
    return -1;
}

// Reads the console auth token. 0 ok (token may be empty = auth disabled), -1 missing/unreadable.
static int read_token(char* tok, size_t cap, char* path, size_t path_cap) {
    tok[0] = '\0';
    if (soulx_path(path, path_cap, "home/.emulator_console_auth_token") != 0) return -1;
    struct stat sb;
    if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size > CONSOLE_TOKEN_MAX) return -1;

    char* data = NULL;
    size_t len = 0;
    if (soulx_read_file(path, &data, &len) != 0) return -1;

    // trim like the emulator does (android::base::trim)
    size_t a = 0, b = strnlen(data, len);
    while (a < b && (data[a] == ' ' || data[a] == '\t' || data[a] == '\r' || data[a] == '\n')) a++;
    while (b > a && (data[b - 1] == ' ' || data[b - 1] == '\t' || data[b - 1] == '\r' || data[b - 1] == '\n')) b--;
    int rc = 0;
    if (b - a >= cap) {
        rc = -1;
    } else {
        for (size_t i = a; i < b; i++) {
            unsigned char ch = (unsigned char)data[i];
            if (ch < 0x21 || ch > 0x7e) {                    // printable, no blanks inside
                rc = -1;
                break;
            }
        }
        if (rc == 0) {
            memcpy(tok, data + a, b - a);
            tok[b - a] = '\0';
        }
    }
    secure_zero(data, len);
    free(data);
    return rc;
}

// ---------------------------------------------------------------------------
// Public functions
// ---------------------------------------------------------------------------

SoulXConsole* soulx_console_open(int port) {
    if (port <= 0 || port > 65535) return NULL;

    SoulXConsole* c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->port = port;
    c->fd = console_connect(port);
    if (c->fd < 0) {
        soulx_log(SOULX_LOG_DEBUG, "console: cannot connect to 127.0.0.1:%d: %s", port, strerror(errno));
        free(c);
        return NULL;
    }

    bool auth = false;
    if (read_banner(c, &auth) != 0) {
        soulx_log(SOULX_LOG_DEBUG, "console: no valid banner on 127.0.0.1:%d", port);
        soulx_console_close(c);
        return NULL;
    }

    if (auth) {
        // Only SoulX's own token file is read; the path printed in the banner is
        // never trusted (any local process could be listening on the port).
        char tok[CONSOLE_TOKEN_MAX + 1], path[PATH_MAX];
        if (read_token(tok, sizeof(tok), path, sizeof(path)) != 0 || !tok[0]) {
            soulx_log(SOULX_LOG_ERROR, "the emulator console on port %d requires authentication but %s is missing or empty",
                     port, path);
            soulx_console_close(c);
            return NULL;
        }
        char cmd[CONSOLE_TOKEN_MAX + 8];
        int n = snprintf(cmd, sizeof(cmd), "auth %s", tok);
        secure_zero(tok, sizeof(tok));
        char reply[256];
        int rc = (n > 0 && (size_t)n < sizeof(cmd)) ? soulx_console_cmd(c, cmd, reply, sizeof(reply)) : -1;
        secure_zero(cmd, sizeof(cmd));
        if (rc != 0) {
            soulx_log(SOULX_LOG_ERROR, "the emulator console on port %d rejected the token from %s%s%s",
                     port, path, rc == 1 ? ": " : "", rc == 1 ? reply : "");
            soulx_console_close(c);
            return NULL;
        }
    }
    return c;
}

int soulx_console_failure(const SoulXConsole* c) {
    return c ? c->last_fail : CONSOLE_FAIL_NOT_SENT;
}

int soulx_console_cmd(SoulXConsole* c, const char* cmd, char* reply, size_t cap) {
    if (reply && cap) reply[0] = '\0';
    if (!c) return -1;
    c->last_fail = CONSOLE_FAIL_NOT_SENT;
    if (c->fd < 0 || !cmd) return -1;
    if (c->broken) {
        soulx_log(SOULX_LOG_DEBUG, "console: connection to port %d is no longer usable", c->port);
        return -1;
    }

    size_t n = strlen(cmd);
    if (n == 0 || n > CONSOLE_CMD_MAX || strpbrk(cmd, "\r\n")) {
        soulx_log(SOULX_LOG_ERROR, "invalid console command (empty, too long or multi-line)");
        c->last_fail = CONSOLE_FAIL_INVALID;
        return -1;
    }

    char out[CONSOLE_CMD_MAX + 2];
    memcpy(out, cmd, n);
    out[n] = '\n';
    c->received = 0;
    int rc = send_all(c->fd, out, n + 1);
    secure_zero(out, n + 1);                                 // may hold the auth token
    if (rc != 0) {
        c->broken = true;
        return -1;                                           // the final '\n' never left: not run
    }
    c->last_fail = CONSOLE_FAIL_NONE;

    size_t used = 0;
    char line[CONSOLE_LINE_MAX];
    for (;;) {
        if (read_line(c, line, sizeof(line), CONSOLE_RECV_MS) < 0) {
            c->broken = true;
            c->last_fail = (c->eof && c->received == 0) ? CONSOLE_FAIL_CLOSED : CONSOLE_FAIL_NO_ANSWER;
            return -1;
        }
        if (is_ok_line(line)) return 0;
        if (is_ok_message(line)) {
            const char* msg = line + 3;
            while (*msg == ' ') msg++;
            reply_append(reply, cap, &used, msg);
            return 0;
        }
        if (is_ko_line(line)) {
            const char* msg = line + 2;
            if (*msg == ':') msg++;
            while (*msg == ' ') msg++;
            reply_append(reply, cap, &used, msg);
            return 1;
        }
        if (reply_ends_glued(c, line)) {
            line[strlen(line) - 2] = '\0';
            reply_append(reply, cap, &used, line);
            return 0;
        }
        reply_append(reply, cap, &used, line);
    }
}

void soulx_console_close(SoulXConsole* c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c);
}

int soulx_console_oneshot(int port, const char* cmd, char* reply, size_t cap) {
    if (reply && cap) reply[0] = '\0';
    SoulXConsole* c = soulx_console_open(port);
    if (!c) return -1;
    int rc = soulx_console_cmd(c, cmd, reply, cap);
    soulx_console_close(c);
    return rc;
}
