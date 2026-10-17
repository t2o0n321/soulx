/*
 * SoulX Android Emulator - unit tests for console.c (macOS)
 *
 * Standalone program: returns 0 when every check passes and prints each failure.
 * A fake emulator console runs on a thread on 127.0.0.1:<ephemeral port> and
 * speaks the real protocol (CRLF lines, banner ending in "OK", "auth <token>",
 * replies ending in "OK" / "OK: ..." / "KO: ..."). SOULX_HOME points to a fresh
 * mkdtemp() directory under /private/tmp (the token lives in $SOULX_HOME/home).
 */

#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "soulx_mac.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do {                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                       \
    } while (0)

#define CHECK_STR(actual, expected) do {                                        \
        g_checks++;                                                             \
        const char* a_ = (actual);                                              \
        const char* e_ = (expected);                                            \
        if (!a_ || strcmp(a_, e_) != 0) {                                       \
            g_failures++;                                                       \
            fprintf(stderr, "FAIL %s:%d: %s == \"%s\" (got \"%s\")\n",          \
                    __FILE__, __LINE__, #actual, e_, a_ ? a_ : "(null)");       \
        }                                                                       \
    } while (0)

static char g_home[PATH_MAX];
static char g_token_path[PATH_MAX];

#define TOKEN "s3cr3t+T0ken/42"

// ---------------------------------------------------------------------------
// Fake emulator console
// ---------------------------------------------------------------------------

typedef struct {
    int listen_fd;
    int port;
    bool require_auth;
    atomic_bool stop;
    atomic_int auth_attempts;
    char last_auth[128];                    // last token received (server thread only)
    pthread_t thread;
} FakeConsole;

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void srv_send(int fd, const char* s) {
    size_t len = strlen(s);
    while (len > 0) {
        ssize_t w = send(fd, s, len, 0);
        if (w <= 0) return;
        s += w;
        len -= (size_t)w;
    }
}

// Reads one '\n'-terminated line (CR dropped). -1 on EOF.
static int srv_read_line(int fd, char* buf, size_t cap) {
    size_t n = 0;
    for (;;) {
        char ch;
        ssize_t r = recv(fd, &ch, 1, 0);
        if (r <= 0) return -1;
        if (ch == '\n') break;
        if (ch != '\r' && n + 1 < cap) buf[n++] = ch;
    }
    buf[n] = '\0';
    return (int)n;
}

static void serve_client(FakeConsole* s, int fd) {
    if (s->require_auth) {
        srv_send(fd, "Android Console: Authentication required\r\n"
                     "Android Console: type 'auth <auth_token>' to authenticate\r\n"
                     "Android Console: you can find your <auth_token> in \r\n"
                     "'/Users/nobody/.emulator_console_auth_token'\r\n"
                     "OK\r\n");
    } else {
        srv_send(fd, "Android Console: type 'help' for a list of commands\r\nOK\r\n");
    }

    bool authed = !s->require_auth;
    char line[8192];
    while (srv_read_line(fd, line, sizeof(line)) >= 0) {
        if (!strncmp(line, "auth ", 5)) {
            atomic_fetch_add(&s->auth_attempts, 1);
            snprintf(s->last_auth, sizeof(s->last_auth), "%s", line + 5);
            if (!strcmp(line + 5, TOKEN)) {
                authed = true;
                srv_send(fd, "Android Console: type 'help' for a list of commands\r\nOK\r\n");
            } else {
                srv_send(fd, "KO: authentication token does not match ~/.emulator_console_auth_token\r\n");
            }
        } else if (!authed) {
            srv_send(fd, "KO: unknown command, try 'help'\r\n");
        } else if (!strcmp(line, "avd name")) {
            srv_send(fd, "soulx_test\r\nOK\r\n");
        } else if (!strcmp(line, "multi")) {
            srv_send(fd, "line one\r\nline two\r\n  indented three\r\nOK\r\n");
        } else if (!strcmp(line, "empty")) {
            srv_send(fd, "OK\r\n");
        } else if (!strcmp(line, "big")) {
            char row[128];
            memset(row, 'x', 100);
            memcpy(row + 100, "\r\n", 3);
            for (int i = 0; i < 40; i++) srv_send(fd, row);
            srv_send(fd, "OK\r\n");
        } else if (!strcmp(line, "split")) {
            srv_send(fd, "par");
            sleep_ms(30);
            srv_send(fd, "tial\r");
            sleep_ms(30);
            srv_send(fd, "\nO");
            sleep_ms(30);
            srv_send(fd, "K\r\n");
        } else if (!strcmp(line, "help resize-display")) {              // real reply: no CRLF before OK
            srv_send(fd, "Allows you to resize the default display\r\n"
                         "As an example: 'resize-display 2' resizes the default display to the size of tabletOK\r\n");
        } else if (!strcmp(line, "okdata")) {                           // data lines that merely end in OK
            srv_send(fd, "status OK\r\n");
            sleep_ms(30);
            srv_send(fd, "all OK\r\nmore\r\nOK\r\n");
        } else if (!strcmp(line, "event send EV_KEY:KEY_BACK:1")) {
            srv_send(fd, "OK\r\n");
        } else if (!strcmp(line, "kill")) {
            srv_send(fd, "OK: killing emulator, bye bye\r\n");
            break;
        } else if (!strcmp(line, "drop")) {
            break;                                                      // connection lost mid-command
        } else {
            srv_send(fd, "KO: unknown command, try 'help'\r\n");
        }
    }
}

static void* console_thread(void* arg) {
    FakeConsole* s = arg;
    while (!atomic_load(&s->stop)) {
        struct pollfd pfd = { s->listen_fd, POLLIN, 0 };
        if (poll(&pfd, 1, 50) <= 0) continue;
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        serve_client(s, fd);
        close(fd);
    }
    return NULL;
}

static FakeConsole* console_start(bool require_auth) {
    FakeConsole* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->require_auth = require_auth;
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;                                                     // ephemeral
    socklen_t alen = sizeof(a);
    if (s->listen_fd < 0 || bind(s->listen_fd, (struct sockaddr*)&a, sizeof(a)) != 0 ||
        listen(s->listen_fd, 8) != 0 || getsockname(s->listen_fd, (struct sockaddr*)&a, &alen) != 0) {
        perror("fake console");
        if (s->listen_fd >= 0) close(s->listen_fd);
        free(s);
        return NULL;
    }
    s->port = ntohs(a.sin_port);
    if (pthread_create(&s->thread, NULL, console_thread, s) != 0) {
        close(s->listen_fd);
        free(s);
        return NULL;
    }
    return s;
}

static void console_stop(FakeConsole* s) {
    if (!s) return;
    atomic_store(&s->stop, true);
    pthread_join(s->thread, NULL);
    close(s->listen_fd);
    free(s);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int rm_cb(const char* path, const struct stat* sb, int flag, struct FTW* ftw) {
    (void)sb;
    (void)ftw;
    if (flag == FTW_DP || flag == FTW_D || flag == FTW_DNR) {
        rmdir(path);
    } else {
        unlink(path);
    }
    return 0;
}

static void rm_rf(const char* path) {
    if (path && strncmp(path, "/private/tmp/", 13) == 0) nftw(path, rm_cb, 32, FTW_DEPTH | FTW_PHYS);
}

static bool write_token(const char* text) {
    FILE* f = fopen(g_token_path, "w");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

// A loopback port with nothing listening on it.
static int closed_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t alen = sizeof(a);
    int port = 0;
    if (fd >= 0 && bind(fd, (struct sockaddr*)&a, sizeof(a)) == 0 &&
        getsockname(fd, (struct sockaddr*)&a, &alen) == 0) {
        port = ntohs(a.sin_port);
    }
    if (fd >= 0) close(fd);
    return port;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_authenticated_session(void) {
    FakeConsole* srv = console_start(true);
    CHECK(srv != NULL);
    if (!srv) return;
    CHECK(write_token("  " TOKEN "\n"));                                // trimmed like the emulator does

    SoulXConsole* c = soulx_console_open(srv->port);
    CHECK(c != NULL);
    CHECK(atomic_load(&srv->auth_attempts) == 1);
    if (c) {
        char reply[512];
        CHECK(soulx_console_cmd(c, "avd name", reply, sizeof(reply)) == 0);
        CHECK_STR(reply, "soulx_test");
        CHECK(soulx_console_cmd(c, "multi", reply, sizeof(reply)) == 0);
        CHECK_STR(reply, "line one\nline two\n  indented three");
        CHECK(soulx_console_cmd(c, "empty", reply, sizeof(reply)) == 0);
        CHECK_STR(reply, "");
        CHECK(soulx_console_cmd(c, "nope", reply, sizeof(reply)) == 1);
        CHECK_STR(reply, "unknown command, try 'help'");
        CHECK(soulx_console_cmd(c, "split", reply, sizeof(reply)) == 0);
        CHECK_STR(reply, "partial");
        CHECK(soulx_console_cmd(c, "event send EV_KEY:KEY_BACK:1", NULL, 0) == 0);

        // a final OK glued to the last data line ends the reply without the 10 s timeout
        double t0 = soulx_now_ms();
        CHECK(soulx_console_cmd(c, "help resize-display", reply, sizeof(reply)) == 0);
        CHECK(soulx_now_ms() - t0 < 2000);
        CHECK_STR(reply, "Allows you to resize the default display\n"
                         "As an example: 'resize-display 2' resizes the default display to the size of tablet");
        CHECK(soulx_console_cmd(c, "okdata", reply, sizeof(reply)) == 0);
        CHECK_STR(reply, "status OK\nall OK\nmore");
        CHECK(soulx_console_cmd(c, "avd name", reply, sizeof(reply)) == 0);
        CHECK_STR(reply, "soulx_test");

        // a reply larger than the buffer is truncated, and the stream stays in sync
        char small[64];
        CHECK(soulx_console_cmd(c, "big", small, sizeof(small)) == 0);
        CHECK(strlen(small) == sizeof(small) - 1);
        CHECK(soulx_console_cmd(c, "avd name", reply, sizeof(reply)) == 0);
        CHECK_STR(reply, "soulx_test");

        // invalid commands are rejected locally without desynchronising the session
        CHECK(soulx_console_cmd(c, "", reply, sizeof(reply)) == -1);
        CHECK(soulx_console_cmd(c, "avd name\nkill", reply, sizeof(reply)) == -1);
        CHECK(soulx_console_cmd(c, "avd name\r", reply, sizeof(reply)) == -1);
        CHECK(soulx_console_cmd(c, NULL, reply, sizeof(reply)) == -1);
        char longcmd[5000];
        memset(longcmd, 'a', sizeof(longcmd) - 1);
        longcmd[sizeof(longcmd) - 1] = '\0';
        CHECK(soulx_console_cmd(c, longcmd, reply, sizeof(reply)) == -1);
        CHECK(soulx_console_cmd(c, "avd name", reply, sizeof(reply)) == 0);
        CHECK_STR(reply, "soulx_test");

        // the peer vanishing mid-command is an I/O error; the handle stays unusable
        CHECK(soulx_console_cmd(c, "drop", reply, sizeof(reply)) == -1);
        CHECK(soulx_console_cmd(c, "avd name", reply, sizeof(reply)) == -1);
        soulx_console_close(c);
    }
    soulx_console_close(NULL);

    // one-shot helpers, including the "OK: ..." reply of kill
    char reply[256];
    CHECK(soulx_console_oneshot(srv->port, "kill", reply, sizeof(reply)) == 0);
    CHECK_STR(reply, "killing emulator, bye bye");
    CHECK(soulx_console_oneshot(srv->port, "nope", reply, sizeof(reply)) == 1);
    CHECK_STR(reply, "unknown command, try 'help'");
    CHECK(soulx_console_oneshot(srv->port, "avd name", reply, sizeof(reply)) == 0);
    CHECK_STR(reply, "soulx_test");

    // wrong, missing and empty tokens are refused
    CHECK(write_token("wrong-token\n"));
    CHECK(soulx_console_open(srv->port) == NULL);
    CHECK(strcmp(srv->last_auth, "wrong-token") == 0);
    CHECK(soulx_console_oneshot(srv->port, "avd name", reply, sizeof(reply)) == -1);
    CHECK(write_token(""));
    int before = atomic_load(&srv->auth_attempts);
    CHECK(soulx_console_open(srv->port) == NULL);
    CHECK(atomic_load(&srv->auth_attempts) == before);                  // nothing was sent
    CHECK(write_token("bad token\n"));                                  // blanks inside: refused locally
    CHECK(soulx_console_open(srv->port) == NULL);
    CHECK(atomic_load(&srv->auth_attempts) == before);
    unlink(g_token_path);
    CHECK(soulx_console_open(srv->port) == NULL);
    CHECK(atomic_load(&srv->auth_attempts) == before);

    console_stop(srv);
}

static void test_no_auth_console(void) {
    FakeConsole* srv = console_start(false);
    CHECK(srv != NULL);
    if (!srv) return;
    char reply[256];

    unlink(g_token_path);                                               // no token file: banner without auth
    SoulXConsole* c = soulx_console_open(srv->port);
    CHECK(c != NULL);
    if (c) {
        CHECK(soulx_console_cmd(c, "avd name", reply, sizeof(reply)) == 0);
        CHECK_STR(reply, "soulx_test");
        soulx_console_close(c);
    }
    CHECK(atomic_load(&srv->auth_attempts) == 0);

    CHECK(write_token(""));                                             // empty token file disables auth
    CHECK(soulx_console_oneshot(srv->port, "multi", reply, sizeof(reply)) == 0);
    CHECK_STR(reply, "line one\nline two\n  indented three");
    CHECK(atomic_load(&srv->auth_attempts) == 0);

    console_stop(srv);
}

static void test_unreachable(void) {
    int port = closed_port();
    CHECK(port > 0);
    double t0 = soulx_now_ms();
    CHECK(soulx_console_open(port) == NULL);
    char reply[16] = "junk";
    CHECK(soulx_console_oneshot(port, "avd name", reply, sizeof(reply)) == -1);
    CHECK_STR(reply, "");
    CHECK(soulx_now_ms() - t0 < 7000);                                   // connect timeout is 3 s
    CHECK(soulx_console_open(0) == NULL);
    CHECK(soulx_console_open(70000) == NULL);
    CHECK(soulx_console_cmd(NULL, "avd name", reply, sizeof(reply)) == -1);
}

// A server that greets with garbage instead of the console banner.
static void* garbage_thread(void* arg) {
    int lfd = *(int*)arg;
    struct pollfd pfd = { lfd, POLLIN, 0 };
    if (poll(&pfd, 1, 5000) > 0) {
        int fd = accept(lfd, NULL, NULL);
        if (fd >= 0) {
            srv_send(fd, "HTTP/1.1 400 Bad Request\r\n\r\n");
            close(fd);
        }
    }
    return NULL;
}

static void test_not_a_console(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t alen = sizeof(a);
    CHECK(lfd >= 0 && bind(lfd, (struct sockaddr*)&a, sizeof(a)) == 0 && listen(lfd, 1) == 0 &&
          getsockname(lfd, (struct sockaddr*)&a, &alen) == 0);
    pthread_t th;
    CHECK(pthread_create(&th, NULL, garbage_thread, &lfd) == 0);
    CHECK(soulx_console_open(ntohs(a.sin_port)) == NULL);
    pthread_join(th, NULL);
    close(lfd);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void) {
    snprintf(g_home, sizeof(g_home), "/private/tmp/soulx-test-console-XXXXXX");
    if (!mkdtemp(g_home)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("SOULX_HOME", g_home, 1);
    char home_dir[PATH_MAX];
    snprintf(home_dir, sizeof(home_dir), "%s/home", g_home);
    snprintf(g_token_path, sizeof(g_token_path), "%s/.emulator_console_auth_token", home_dir);
    if (mkdir(home_dir, 0755) != 0) {
        perror("mkdir");
        rm_rf(g_home);
        return 1;
    }

    test_authenticated_session();
    test_no_auth_console();
    test_unreachable();
    test_not_a_console();

    rm_rf(g_home);
    if (g_failures) {
        fprintf(stderr, "test_console: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_console: all %d checks passed\n", g_checks);
    return 0;
}
