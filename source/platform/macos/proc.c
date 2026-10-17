/*
 * SoulX Android Emulator - macOS process helpers
 *
 * Every child is started with posix_spawn() and an argv array (never a shell). Children only
 * inherit the descriptors set up explicitly (POSIX_SPAWN_CLOEXEC_DEFAULT), start with default
 * signal dispositions and an empty signal mask, and - when SoulXCmd.soulx_env is set - run in the
 * SoulX isolation environment (soulx_build_env) so that adb and the engine only ever see $SOULX_HOME.
 */

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <pthread/spawn.h>      // posix_spawnattr_set_qos_class_np
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "soulx_mac.h"

#ifndef SZOMB
#define SZOMB 5                 // <sys/proc.h>: process is a zombie
#endif

#define RUN_DEFAULT_MAX_OUT     (256u * 1024u * 1024u)  // SoulXCmd.max_output == 0
#define RUN_MAX_ERR             (8u * 1024u * 1024u)    // captured stderr cap
#define RUN_IO_CHUNK            65536
#define RUN_EXIT_DRAIN_MS       500     // pipes still open after the child exited (daemonized grandchild)
#define RUN_KILL_DRAIN_MS       2000    // pipes still open after a timeout SIGKILL
#define RUN_INTERRUPT_KILL_MS   2000    // SoulXCmd.interruptible: SIGTERM, then SIGKILL this much later
#define RUN_INTERRUPTED_STATUS  130     // res->status of a child ended because of Ctrl+C (128 + SIGINT)

extern char** environ;

volatile sig_atomic_t g_soulx_interrupted = 0;

// Variables removed from the inherited environment (some are set again below).
static const char* const k_env_remove[] = {
    "ANDROID_SDK_HOME", "ANDROID_PREFS_ROOT", "ANDROID_USER_HOME", "ANDROID_SERIAL",
    "ANDROID_HOME", "ANDROID_SDK_ROOT", "ANDROID_AVD_HOME", "ANDROID_EMULATOR_HOME", "ANDROID_TMP",
    "HOME", "ANDROID_EMU_ENABLE_CRASH_REPORTING", "TMPDIR", "ANDROID_EMULATOR_WAIT_TIME_BEFORE_KILL",
};

// Every inherited variable with one of these prefixes is removed too: they configure adb (server address
// and socket, vendor keys, log path, kill-server refusal, USB / mDNS discovery, install defaults, ...).
// SoulX's adb must only ever talk to its private server on 127.0.0.1:SOULX_ADB_SERVER_PORT.
static const char* const k_env_remove_prefix[] = { "ADB_", "ANDROID_ADB_" };

// Highest console port the adb server scans for emulators. Below adb's first scan port (5555) it
// scans nothing, so SoulX's private server never connects to other emulators (e.g. Android Studio's
// on 5554..5584, whose guests would ask "Allow USB debugging?"). SoulX's engines (5600+, outside
// the scan range anyway) register themselves with the server they were started for.
#define SOULX_ADB_SCAN_MAX_PORT  "5554"

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

static bool env_entry_is(const char* entry, const char* name) {
    size_t n = strlen(name);
    return strncmp(entry, name, n) == 0 && entry[n] == '=';
}

static bool env_entry_dropped(const char* entry) {
    for (size_t i = 0; i < sizeof(k_env_remove) / sizeof(k_env_remove[0]); i++) {
        if (env_entry_is(entry, k_env_remove[i])) return true;
    }
    for (size_t i = 0; i < sizeof(k_env_remove_prefix) / sizeof(k_env_remove_prefix[0]); i++) {
        if (strncmp(entry, k_env_remove_prefix[i], strlen(k_env_remove_prefix[i])) == 0) return true;
    }
    return false;
}

static char* env_pair(const char* name, const char* home, const char* suffix) {
    size_t len = strlen(name) + 1 + (home ? strlen(home) : 0) + strlen(suffix) + 1;
    char* s = malloc(len);
    if (!s) return NULL;
    int n = snprintf(s, len, "%s=%s%s", name, home ? home : "", suffix);
    if (n < 0 || (size_t)n >= len) {
        free(s);
        return NULL;
    }
    return s;
}

// True when |entry| ("NAME=value") sets a variable that one of |extra| sets too.
static bool env_entry_overridden(const char* entry, const char* const* extra) {
    const char* eq = strchr(entry, '=');
    if (!eq) return false;
    size_t n = (size_t)(eq - entry);
    for (const char* const* x = extra; x && *x; x++) {
        if (strncmp(*x, entry, n) == 0 && (*x)[n] == '=') return true;
    }
    return false;
}

static char** build_env(const char* const* extra) {
    const char* home = soulx_home();
    char port[16];
    snprintf(port, sizeof(port), "%d", SOULX_ADB_SERVER_PORT);
    const struct { const char* name; const char* base; const char* suffix; } set[] = {
        { "ANDROID_HOME",                       home, "/sdk" },
        { "ANDROID_SDK_ROOT",                   home, "/sdk" },
        { "ANDROID_AVD_HOME",                   home, "/avd" },
        { "ANDROID_EMULATOR_HOME",              home, "/home/.android" },
        { "ANDROID_TMP",                        home, "/tmp" },
        { "ANDROID_ADB_SERVER_PORT",            NULL, port },
        { "ADB_LOCAL_TRANSPORT_MAX_PORT",       NULL, SOULX_ADB_SCAN_MAX_PORT },
        { "ANDROID_EMU_ENABLE_CRASH_REPORTING", NULL, "0" },
        { "HOME",                               home, "/home" },
        // netsimd's netsim.ini and the adb server log otherwise land in the user's $TMPDIR
        { "TMPDIR",                             home, "/tmp/" },
        // after a console "kill" the engine's watchdog SIGKILLs it after 20 s by default, which can cut a
        // quick-boot snapshot save short on a long-running instance (measured > 20 s after 3 h uptime)
        { "ANDROID_EMULATOR_WAIT_TIME_BEFORE_KILL", NULL, "90" },
    };
    const size_t added = sizeof(set) / sizeof(set[0]);

    size_t count = 0, extras = 0;
    for (char** e = environ; e && *e; e++) count++;
    for (const char* const* x = extra; x && *x; x++) extras++;
    char** env = calloc(count + added + extras + 1, sizeof(char*));
    if (!env) return NULL;

    size_t n = 0;
    for (char** e = environ; e && *e; e++) {
        if (!strchr(*e, '=') || env_entry_dropped(*e) || env_entry_overridden(*e, extra)) continue;
        env[n] = strdup(*e);
        if (!env[n]) goto fail;
        n++;
    }

    for (size_t i = 0; i < added; i++) {
        env[n] = env_pair(set[i].name, set[i].base, set[i].suffix);
        if (!env[n]) goto fail;
        n++;
    }
    for (size_t i = 0; i < extras; i++) {
        if (!strchr(extra[i], '=')) continue;
        env[n] = strdup(extra[i]);
        if (!env[n]) goto fail;
        n++;
    }
    env[n] = NULL;
    return env;

fail:
    soulx_free_env(env);
    return NULL;
}

char** soulx_build_env(void) {
    return build_env(NULL);
}

void soulx_free_env(char** envp) {
    if (!envp) return;
    for (char** e = envp; *e; e++) free(*e);
    free(envp);
}

// ---------------------------------------------------------------------------
// Spawn helpers
// ---------------------------------------------------------------------------

static bool cmd_valid(const SoulXCmd* cmd) {
    return cmd && cmd->argv && cmd->argv[0] && cmd->argv[0][0] == '/';
}

// Pipe with close-on-exec on both ends, never using descriptors 0..2.
static int make_pipe(int fds[2]) {
    int p[2];
    if (pipe(p) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        if (p[i] <= STDERR_FILENO) {
            int moved = fcntl(p[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (moved < 0) {
                close(p[0]);
                close(p[1]);
                return -1;
            }
            close(p[i]);
            p[i] = moved;
        } else {
            (void)fcntl(p[i], F_SETFD, FD_CLOEXEC);
        }
    }
    fds[0] = p[0];
    fds[1] = p[1];
    return 0;
}

static void close_fd(int* fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int init_attr(posix_spawnattr_t* attr, short extra_flags) {
    int rc = posix_spawnattr_init(attr);
    if (rc != 0) return rc;

    sigset_t defaults;
    sigset_t mask;
    sigemptyset(&mask);
    sigemptyset(&defaults);
    const int sigs[] = { SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGALRM, SIGTERM, SIGCHLD, SIGTSTP,
                         SIGTTIN, SIGTTOU, SIGUSR1, SIGUSR2, SIGWINCH };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaddset(&defaults, sigs[i]);

    short flags = (short)(POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT | extra_flags);
    if ((rc = posix_spawnattr_setsigdefault(attr, &defaults)) != 0 ||
        (rc = posix_spawnattr_setsigmask(attr, &mask)) != 0 ||
        (rc = posix_spawnattr_setflags(attr, flags)) != 0) {
        posix_spawnattr_destroy(attr);
        return rc;
    }
    return 0;
}

static int add_chdir(posix_spawn_file_actions_t* fa, const char* cwd) {
    if (!cwd) return 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return posix_spawn_file_actions_addchdir_np(fa, cwd);   // still available on macOS 26
#pragma clang diagnostic pop
}

// Lets the child inherit a standard descriptor, or gives it /dev/null when ours is closed.
static int inherit_or_null(posix_spawn_file_actions_t* fa, int fd) {
    if (fcntl(fd, F_GETFD) != -1) return posix_spawn_file_actions_addinherit_np(fa, fd);
    return posix_spawn_file_actions_addopen(fa, fd, "/dev/null", fd == STDIN_FILENO ? O_RDONLY : O_WRONLY, 0);
}

static int do_spawn(const SoulXCmd* cmd, const posix_spawn_file_actions_t* fa, const posix_spawnattr_t* attr, pid_t* pid) {
    char** soulx_env = NULL;
    char* const* envp = environ;
    if (cmd->soulx_env) {
        soulx_env = build_env(cmd->env);
        if (!soulx_env) return ENOMEM;
        envp = soulx_env;
    }
    int rc = posix_spawn(pid, cmd->argv[0], fa, attr, (char* const*)cmd->argv, envp);
    soulx_free_env(soulx_env);
    return rc;
}

// kqueue that becomes readable when pid exits (-1 if unavailable; callers fall back to polling).
static int exit_watch(pid_t pid) {
    int kq = kqueue();
    if (kq < 0) return -1;
    struct kevent kev;
    EV_SET(&kev, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) {
        close(kq);
        return -1;
    }
    return kq;
}

static int decode_status(int wstatus) {
    if (WIFEXITED(wstatus)) return WEXITSTATUS(wstatus);
    if (WIFSIGNALED(wstatus)) return 128 + WTERMSIG(wstatus);
    return -1;
}

// ---------------------------------------------------------------------------
// soulx_run - captured stdout/stderr, optional stdin bytes, timeout
// ---------------------------------------------------------------------------

typedef struct {
    char* data;
    size_t len;
    size_t cap;
    size_t max;
} CaptureBuf;

static void capture_append(CaptureBuf* b, const char* src, size_t n) {
    if (b->len >= b->max) return;                       // over the cap: discard, keep draining
    if (n > b->max - b->len) n = b->max - b->len;
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap : 4096;
        while (ncap < b->len + n + 1) {
            if (ncap > SIZE_MAX / 2) return;
            ncap *= 2;
        }
        char* grown = realloc(b->data, ncap);
        if (!grown) {
            b->max = b->len;                            // out of memory: stop capturing
            return;
        }
        b->data = grown;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

static char* capture_finish(CaptureBuf* b, size_t* len_out) {
    if (!b->data) {
        b->data = malloc(1);
        b->len = 0;
        if (!b->data) {
            *len_out = 0;
            return NULL;
        }
    }
    b->data[b->len] = '\0';
    *len_out = b->len;
    char* out = b->data;
    b->data = NULL;
    return out;
}

// Reads what is available; returns false at EOF / error (the caller closes the fd).
static bool drain_fd(int fd, CaptureBuf* b) {
    char chunk[RUN_IO_CHUNK];
    for (int i = 0; i < 16; i++) {
        ssize_t r = read(fd, chunk, sizeof(chunk));
        if (r > 0) {
            capture_append(b, chunk, (size_t)r);
            continue;
        }
        if (r == 0) return false;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }
    return true;
}

// SoulXCmd.interruptible: the child gets SIGTERM once g_soulx_interrupted is set and SIGKILL
// RUN_INTERRUPT_KILL_MS later if it is still there.
typedef struct {
    bool   armed;               // interruptible, and no interrupt was pending when the child started
    double term_ms;             // soulx_now_ms() of the SIGTERM (0 = not sent)
    bool   signalled;           // the child was told to stop because of an interrupt
} InterruptStop;

// Returns true when it signalled the child just now.
static bool interrupt_step(InterruptStop* s, pid_t pid, double now) {
    if (!s->armed) return false;
    if (s->term_ms <= 0) {
        if (!g_soulx_interrupted) return false;
        (void)kill(pid, SIGTERM);
        s->term_ms = now > 0 ? now : 1;
        s->signalled = true;
        return true;
    }
    if (now - s->term_ms < RUN_INTERRUPT_KILL_MS) return false;
    (void)kill(pid, SIGKILL);
    s->armed = false;
    return true;
}

static void result_error(SoulXCmdResult* r, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

static void result_error(SoulXCmdResult* r, const char* fmt, ...) {
    char msg[PATH_MAX + 128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(msg)) n = (int)sizeof(msg) - 1;
    free(r->err);
    r->err = malloc((size_t)n + 1);
    if (r->err) {
        memcpy(r->err, msg, (size_t)n);
        r->err[n] = '\0';
        r->err_len = (size_t)n;
    } else {
        r->err_len = 0;
    }
    if (!r->out) r->out = calloc(1, 1);
    r->out_len = 0;
}

int soulx_run(const SoulXCmd* cmd, SoulXCmdResult* res) {
    SoulXCmdResult local;
    SoulXCmdResult* r = res ? res : &local;
    memset(r, 0, sizeof(*r));
    r->status = -1;

    if (!cmd_valid(cmd)) {
        result_error(r, "invalid command '%s': argv[0] must be an absolute path",
                     (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "(null)");
        if (!res) soulx_cmd_result_free(r);
        return -1;
    }

    int in_p[2] = { -1, -1 };
    int out_p[2] = { -1, -1 };
    int err_p[2] = { -1, -1 };
    bool feed = cmd->stdin_data && cmd->stdin_len > 0;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    bool fa_ok = false;
    bool attr_ok = false;
    int rc = 0;
    pid_t pid = -1;

    if (make_pipe(out_p) != 0 || make_pipe(err_p) != 0 || (feed && make_pipe(in_p) != 0)) {
        rc = errno ? errno : EMFILE;
        goto spawn_failed;
    }
    if ((rc = posix_spawn_file_actions_init(&fa)) != 0) goto spawn_failed;
    fa_ok = true;
    if (feed) rc = posix_spawn_file_actions_adddup2(&fa, in_p[0], STDIN_FILENO);
    else rc = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, out_p[1], STDOUT_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, err_p[1], STDERR_FILENO);
    if (rc == 0) rc = add_chdir(&fa, cmd->cwd);
    if (rc != 0) goto spawn_failed;
    if ((rc = init_attr(&attr, 0)) != 0) goto spawn_failed;
    attr_ok = true;

    rc = do_spawn(cmd, &fa, &attr, &pid);
    if (rc != 0) goto spawn_failed;

    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close_fd(&in_p[0]);
    close_fd(&out_p[1]);
    close_fd(&err_p[1]);

    int in_fd = in_p[1];
    int out_fd = out_p[0];
    int err_fd = err_p[0];
    set_nonblock(out_fd);
    set_nonblock(err_fd);
    if (in_fd >= 0) {
        set_nonblock(in_fd);
        (void)fcntl(in_fd, F_SETNOSIGPIPE, 1);     // EPIPE instead of SIGPIPE if the child stops reading
    }

    CaptureBuf out = { NULL, 0, 0, cmd->max_output ? cmd->max_output : RUN_DEFAULT_MAX_OUT };
    CaptureBuf err = { NULL, 0, 0, RUN_MAX_ERR };
    size_t in_off = 0;
    int kq = exit_watch(pid);
    bool exited = false;
    int wstatus = 0;
    double start = soulx_now_ms();
    double deadline = cmd->timeout_ms > 0 ? start + cmd->timeout_ms : 0;
    double drain_deadline = 0;
    double last_reap = start;
    // Only an interrupt that arrives while the child runs ends it: a cleanup command started after
    // an earlier Ctrl+C must still run.
    InterruptStop stop = { cmd->interruptible && !g_soulx_interrupted, 0.0, false };

    for (;;) {
        double now = soulx_now_ms();
        if (deadline > 0 && !r->timed_out && !exited && now >= deadline) {
            kill(pid, SIGKILL);
            r->timed_out = true;
            drain_deadline = now + RUN_KILL_DRAIN_MS;
        }
        if (!exited && interrupt_step(&stop, pid, now)) {
            double d = now + RUN_INTERRUPT_KILL_MS + RUN_KILL_DRAIN_MS;
            if (drain_deadline <= 0 || d < drain_deadline) drain_deadline = d;
        }
        if (!exited && (kq < 0 || now - last_reap >= 100)) {
            last_reap = now;
            pid_t w = waitpid(pid, &wstatus, WNOHANG);
            if (w == pid || (w < 0 && errno == ECHILD)) {
                exited = true;
                if (w != pid) wstatus = -1;
                double d = now + RUN_EXIT_DRAIN_MS;
                if (drain_deadline <= 0 || d < drain_deadline) drain_deadline = d;
            }
        }
        if (out_fd < 0 && err_fd < 0) break;
        if (drain_deadline > 0 && now >= drain_deadline) break;

        struct pollfd pfd[4];
        int np = 0;
        int i_out = -1, i_err = -1, i_in = -1, i_kq = -1;
        if (out_fd >= 0) { pfd[np].fd = out_fd; pfd[np].events = POLLIN; i_out = np++; }
        if (err_fd >= 0) { pfd[np].fd = err_fd; pfd[np].events = POLLIN; i_err = np++; }
        if (in_fd >= 0)  { pfd[np].fd = in_fd;  pfd[np].events = POLLOUT; i_in = np++; }
        if (kq >= 0 && !exited) { pfd[np].fd = kq; pfd[np].events = POLLIN; i_kq = np++; }
        for (int k = 0; k < np; k++) pfd[k].revents = 0;

        double wait_for = stop.armed ? 20 : 100;             // Ctrl+C is noticed within 20 ms
        if (deadline > 0 && !r->timed_out && deadline - now < wait_for) wait_for = deadline - now;
        if (drain_deadline > 0 && drain_deadline - now < wait_for) wait_for = drain_deadline - now;
        int wait_ms = wait_for < 1 ? 1 : (int)wait_for;

        int pr = poll(pfd, (nfds_t)np, wait_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        if (i_kq >= 0 && pfd[i_kq].revents) {
            struct kevent ev;
            struct timespec zero = { 0, 0 };
            (void)kevent(kq, NULL, 0, &ev, 1, &zero);
            last_reap = 0;                              // reap on the next iteration
        }
        if (i_in >= 0 && pfd[i_in].revents) {
            if (pfd[i_in].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close_fd(&in_fd);
            } else {
                size_t left = cmd->stdin_len - in_off;
                ssize_t w = write(in_fd, cmd->stdin_data + in_off, left > RUN_IO_CHUNK ? RUN_IO_CHUNK : left);
                if (w > 0) {
                    in_off += (size_t)w;
                    if (in_off >= cmd->stdin_len) close_fd(&in_fd);
                } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
                    close_fd(&in_fd);                   // EPIPE: the child does not want more input
                }
            }
        }
        if (i_out >= 0 && pfd[i_out].revents) {
            if (!drain_fd(out_fd, &out)) close_fd(&out_fd);
        }
        if (i_err >= 0 && pfd[i_err].revents) {
            if (!drain_fd(err_fd, &err)) close_fd(&err_fd);
        }
    }

    close_fd(&in_fd);
    close_fd(&out_fd);
    close_fd(&err_fd);
    if (kq >= 0) close(kq);

    // The pipes are closed; wait for the child itself (still honouring the timeout).
    int sleep_ms = 1;
    while (!exited) {
        pid_t w = waitpid(pid, &wstatus, WNOHANG);
        if (w == pid) {
            exited = true;
            break;
        }
        if (w < 0 && errno != EINTR) {
            wstatus = -1;
            break;
        }
        double now = soulx_now_ms();
        if (deadline > 0 && !r->timed_out && now >= deadline) {
            kill(pid, SIGKILL);
            r->timed_out = true;
        }
        (void)interrupt_step(&stop, pid, now);
        struct timespec ts = { 0, (long)sleep_ms * 1000000L };
        nanosleep(&ts, NULL);
        if (sleep_ms < 20) sleep_ms *= 2;
    }

    r->status = (exited && wstatus != -1) ? decode_status(wstatus) : -1;
    if (stop.signalled && !r->timed_out) r->status = RUN_INTERRUPTED_STATUS;
    r->out = capture_finish(&out, &r->out_len);
    r->err = capture_finish(&err, &r->err_len);
    int status = r->status;
    if (!res) soulx_cmd_result_free(r);
    return status;

spawn_failed:
    if (fa_ok) posix_spawn_file_actions_destroy(&fa);
    if (attr_ok) posix_spawnattr_destroy(&attr);
    close_fd(&in_p[0]);
    close_fd(&in_p[1]);
    close_fd(&out_p[0]);
    close_fd(&out_p[1]);
    close_fd(&err_p[0]);
    close_fd(&err_p[1]);
    result_error(r, "cannot run %s: %s", cmd->argv[0], strerror(rc));
    soulx_log(SOULX_LOG_DEBUG, "posix_spawn %s failed: %s", cmd->argv[0], strerror(rc));
    if (!res) soulx_cmd_result_free(r);
    return -1;
}

void soulx_cmd_result_free(SoulXCmdResult* res) {
    if (!res) return;
    free(res->out);
    free(res->err);
    res->out = NULL;
    res->err = NULL;
    res->out_len = 0;
    res->err_len = 0;
}

// ---------------------------------------------------------------------------
// soulx_run_passthrough - child shares our terminal
// ---------------------------------------------------------------------------

int soulx_run_passthrough(const SoulXCmd* cmd) {
    if (!cmd_valid(cmd)) {
        soulx_log(SOULX_LOG_ERROR, "invalid command (argv[0] must be an absolute path)");
        return -1;
    }

    int in_p[2] = { -1, -1 };
    bool feed = cmd->stdin_data && cmd->stdin_len > 0;
    if (feed && make_pipe(in_p) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create a pipe: %s", strerror(errno));
        return -1;
    }

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    int rc = posix_spawn_file_actions_init(&fa);
    if (rc != 0) {
        close_fd(&in_p[0]);
        close_fd(&in_p[1]);
        soulx_log(SOULX_LOG_ERROR, "cannot run %s: %s", cmd->argv[0], strerror(rc));
        return -1;
    }
    if (feed) rc = posix_spawn_file_actions_adddup2(&fa, in_p[0], STDIN_FILENO);
    else rc = inherit_or_null(&fa, STDIN_FILENO);
    if (rc == 0) rc = inherit_or_null(&fa, STDOUT_FILENO);
    if (rc == 0) rc = inherit_or_null(&fa, STDERR_FILENO);
    if (rc == 0) rc = add_chdir(&fa, cmd->cwd);
    bool attr_ok = false;
    if (rc == 0) {
        rc = init_attr(&attr, 0);
        attr_ok = (rc == 0);
    }
    pid_t pid = -1;
    if (rc == 0) rc = do_spawn(cmd, &fa, &attr, &pid);
    posix_spawn_file_actions_destroy(&fa);
    if (attr_ok) posix_spawnattr_destroy(&attr);
    close_fd(&in_p[0]);
    if (rc != 0) {
        close_fd(&in_p[1]);
        soulx_log(SOULX_LOG_ERROR, "cannot run %s: %s", cmd->argv[0], strerror(rc));
        return -1;
    }

    int in_fd = in_p[1];
    if (in_fd >= 0) {
        set_nonblock(in_fd);
        (void)fcntl(in_fd, F_SETNOSIGPIPE, 1);
    }
    size_t in_off = 0;
    int kq = exit_watch(pid);
    double deadline = cmd->timeout_ms > 0 ? soulx_now_ms() + cmd->timeout_ms : 0;
    // Forward only an interrupt that happens while this child runs (a cleanup command started
    // after an earlier Ctrl+C must not be interrupted immediately).
    bool sent_int = g_soulx_interrupted != 0;
    bool killed = false;
    int wstatus = 0;

    for (;;) {
        pid_t w = waitpid(pid, &wstatus, WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno != EINTR) {
            wstatus = -1;
            break;
        }
        if (g_soulx_interrupted && !sent_int) {
            kill(pid, SIGINT);          // usually already delivered by the terminal; harmless twice
            sent_int = true;
        }
        if (deadline > 0 && !killed && soulx_now_ms() >= deadline) {
            kill(pid, SIGKILL);
            killed = true;
        }

        struct pollfd pfd[2];
        int np = 0;
        int i_in = -1;
        if (kq >= 0) { pfd[np].fd = kq; pfd[np].events = POLLIN; pfd[np].revents = 0; np++; }
        if (in_fd >= 0) { pfd[np].fd = in_fd; pfd[np].events = POLLOUT; pfd[np].revents = 0; i_in = np++; }
        int pr = poll(pfd, (nfds_t)np, kq >= 0 ? 200 : 50);
        if (pr > 0 && i_in >= 0 && pfd[i_in].revents) {
            if (pfd[i_in].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close_fd(&in_fd);
            } else {
                size_t left = cmd->stdin_len - in_off;
                ssize_t n = write(in_fd, cmd->stdin_data + in_off, left > RUN_IO_CHUNK ? RUN_IO_CHUNK : left);
                if (n > 0) {
                    in_off += (size_t)n;
                    if (in_off >= cmd->stdin_len) close_fd(&in_fd);
                } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                    close_fd(&in_fd);
                }
            }
        }
        if (pr > 0 && kq >= 0 && pfd[0].revents) {
            struct kevent ev;
            struct timespec zero = { 0, 0 };
            (void)kevent(kq, NULL, 0, &ev, 1, &zero);
        }
    }
    close_fd(&in_fd);
    if (kq >= 0) close(kq);
    return wstatus == -1 ? -1 : decode_status(wstatus);
}

// ---------------------------------------------------------------------------
// soulx_spawn_detached - long-running engine
// ---------------------------------------------------------------------------

pid_t soulx_spawn_detached(const SoulXCmd* cmd, const char* log_path, bool interactive_qos) {
    if (!cmd_valid(cmd)) {
        soulx_log(SOULX_LOG_ERROR, "invalid command (argv[0] must be an absolute path)");
        return -1;
    }

    const char* out_path = (log_path && log_path[0]) ? log_path : "/dev/null";
    int log_fd = open(out_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log_fd < 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot open %s: %s", out_path, strerror(errno));
        return -1;
    }
    if (log_fd <= STDERR_FILENO) {      // our stdio is closed: keep the log out of the child's 0..2 slots
        int moved = fcntl(log_fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        close(log_fd);
        if (moved < 0) {
            soulx_log(SOULX_LOG_ERROR, "cannot open %s: %s", out_path, strerror(errno));
            return -1;
        }
        log_fd = moved;
    }

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    int rc = posix_spawn_file_actions_init(&fa);
    if (rc != 0) {
        close(log_fd);
        soulx_log(SOULX_LOG_ERROR, "cannot run %s: %s", cmd->argv[0], strerror(rc));
        return -1;
    }
    rc = posix_spawn_file_actions_adddup2(&fa, log_fd, STDOUT_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, log_fd, STDERR_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (rc == 0) rc = add_chdir(&fa, cmd->cwd);

    bool attr_ok = false;
    if (rc == 0) {
        // New session: no controlling terminal, so Ctrl+C in the CLI never reaches the engine.
        rc = init_attr(&attr, POSIX_SPAWN_SETSID);
        attr_ok = (rc == 0);
    }
    if (rc == 0 && interactive_qos) {
        int qrc = posix_spawnattr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE);
        if (qrc != 0) soulx_log(SOULX_LOG_DEBUG, "QoS class not applied: %s", strerror(qrc));
    }

    pid_t pid = -1;
    if (rc == 0) rc = do_spawn(cmd, &fa, &attr, &pid);
    posix_spawn_file_actions_destroy(&fa);
    if (attr_ok) posix_spawnattr_destroy(&attr);
    close(log_fd);
    if (rc != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot start %s: %s", cmd->argv[0], strerror(rc));
        return -1;
    }
    return pid;
}

// ---------------------------------------------------------------------------
// Process state
// ---------------------------------------------------------------------------

bool soulx_pid_alive(pid_t pid) {
    if (pid <= 0) return false;
    int st;
    if (waitpid(pid, &st, WNOHANG) == pid) return false;    // our child, now reaped
    if (kill(pid, 0) != 0 && errno != EPERM) return false;  // ESRCH
    struct proc_bsdinfo bi;
    int n = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, (int)sizeof(bi));
    if (n == (int)sizeof(bi) && bi.pbi_status == SZOMB) return false;
    return true;
}

bool soulx_pid_is_engine(pid_t pid) {
    if (pid <= 0) return false;
    char exe[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, exe, sizeof(exe)) <= 0) return false;

    char dir[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "sdk/emulator") != 0) return false;
    char real[PATH_MAX];
    const char* candidates[2] = { dir, realpath(dir, real) ? real : NULL };  // proc_pidpath is canonical
    for (int i = 0; i < 2; i++) {
        if (!candidates[i]) continue;
        size_t len = strlen(candidates[i]);
        if (strncmp(exe, candidates[i], len) == 0 && exe[len] == '/') return true;
    }
    return false;
}

int soulx_wait_pid_exit(pid_t pid, int timeout_ms) {
    if (!soulx_pid_alive(pid)) return 0;
    double deadline = soulx_now_ms() + (timeout_ms > 0 ? timeout_ms : 0);
    int kq = exit_watch(pid);
    for (;;) {
        double left = deadline - soulx_now_ms();
        if (left <= 0) break;
        int slice = left < 100 ? (int)left + 1 : 100;
        if (kq >= 0) {
            struct kevent ev;
            struct timespec ts = { slice / 1000, (long)(slice % 1000) * 1000000L };
            (void)kevent(kq, NULL, 0, &ev, 1, &ts);
        } else {
            struct timespec ts = { 0, (long)slice * 1000000L };
            nanosleep(&ts, NULL);
        }
        if (!soulx_pid_alive(pid)) {
            if (kq >= 0) close(kq);
            return 0;
        }
    }
    if (kq >= 0) close(kq);
    return soulx_pid_alive(pid) ? -1 : 0;
}

double soulx_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

bool soulx_interrupted(void) {
    return g_soulx_interrupted != 0;
}

int soulx_sleep_ms(int ms) {
    if (g_soulx_interrupted) return -1;
    if (ms <= 0) return 0;
    double end = soulx_now_ms() + ms;
    for (;;) {
        double left = end - soulx_now_ms();
        if (left <= 0) return 0;
        double slice = left < 20 ? left : 20;
        struct timespec ts = { 0, (long)(slice * 1e6) };
        nanosleep(&ts, NULL);
        if (g_soulx_interrupted) return -1;
    }
}
