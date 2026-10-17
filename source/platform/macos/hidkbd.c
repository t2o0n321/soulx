/*
 * SoulX Android Emulator - virtual USB HID keyboard inside the guest (macOS host)
 *
 * One long-lived 'adb -s <serial> shell hid -' child per keyboard, spawned with posix_spawn in the SoulX
 * environment. Android's `hid` tool (frameworks/base/cmds/hid) reads JSON objects from stdin:
 *   {"id":1,"command":"register","name":...,"vid":...,"pid":...,"bus":"usb","descriptor":[...]}
 *       creates a uhid device (allowed for the adb shell user, also on 'user' / Play builds)
 *   {"id":1,"command":"report","report":[...]}     one input report (report id first)
 *   {"id":1,"command":"delay","duration":<ms>}     later reports wait that long, in guest time
 * The tool holds reports back until the input system has opened the new device, and on stdin EOF it sends the
 * queued reports, removes the device and exits.
 *
 * Reports: id 1 = boot keyboard (modifier bits, reserved, 6 key usages 0x00..0x65; usages above 0x65 are
 * outside the descriptor and skipped), id 2 = consumer control (one 16-bit usage 0x001..0x3FF).
 * soulx_hidkbd_open() waits until 'getevent -lp' lists a new "SoulX Virtual Keyboard" (3 s at most).
 * soulx_hidkbd_type() paces characters on the host (one per HK_TYPE_CHAR_MS) and holds each key HK_TYPE_HOLD_MS
 * in guest time, so the guest's 64-event evdev buffer never overflows (an overflow drops keys: SYN_DROPPED).
 * tap / consumer / type start from and end with every key released.
 * Calls on one keyboard are serialised by a mutex; soulx_hidkbd_close() must not race with other calls.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "soulx_mac.h"

#define HK_NAME                 "SoulX Virtual Keyboard"
#define HK_VID                  0x18d1          // Google
#define HK_PID                  0x4e11
#define HK_DEVICE_ID            1               // id of the device inside this hid process

#define HK_OPEN_TIMEOUT_MS      3000            // the guest must list the new device within this time
#define HK_POLL_MS              100
#define HK_PROBE_TIMEOUT_MS     2500            // one 'getevent -lp'
#define HK_SETTLE_MS            150             // let the input system open the new node before the first report
#define HK_WRITE_TIMEOUT_MS     2000            // a stuck pipe fails the call instead of blocking the caller
#define HK_TYPE_CHAR_MS         16              // host pacing of soulx_hidkbd_type: about 60 characters per second
#define HK_TYPE_HOLD_MS         6               // guest-side key hold while typing
#define HK_TAP_HOLD_MS          30              // guest-side key hold of tap / consumer (games poll key state)
#define HK_CLOSE_WAIT_MS        1500            // hid drains its queue and exits after stdin EOF
#define HK_KILL_WAIT_MS         1000

#define HK_KEYS                 6               // key slots of the boot keyboard report
#define HK_KEY_USAGE_MIN        0x04
#define HK_KEY_USAGE_MAX        0x65            // logical maximum of the descriptor's key array
#define HK_CONSUMER_MAX         0x3FF
#define HK_REPORT_LEN           (3 + HK_KEYS)   // id, modifiers, reserved, keys
#define HK_LINE_MAX             256             // one JSON report / delay line
#define HK_TAIL                 512             // last output of the child, for error messages
#define HK_NODES_MAX            1024            // newline-separated event node list

// Boot-protocol keyboard (report id 1: 8 modifier bits, reserved byte, 5 LED outputs, 6 key slots 0x00..0x65)
// + consumer control (report id 2: one 16-bit usage 0..0x3FF). Same bytes as the prototype.
static const unsigned char k_descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

struct SoulXHidKbd {
    pthread_mutex_t lock;
    pid_t pid;                  // adb client; -1 once reaped
    int in_fd;                  // write end of its stdin
    int out_fd;                 // read end of its stdout + stderr
    bool broken;                // the child exited or a write failed: every call fails
    unsigned char last[HK_REPORT_LEN];      // last keyboard report sent
    bool have_last;
    char serial[65];
    char tail[HK_TAIL];         // most recent output of the child
    size_t tail_len;
};

// Exported for tests/macos/test_hidkbd.c (not part of soulx_mac.h).
int soulx_hidkbd_kbd_report(int modifiers, const int* usages, int count, unsigned char report[HK_REPORT_LEN]);
int soulx_hidkbd_json_register(char* buf, size_t cap);
int soulx_hidkbd_json_report(const unsigned char* report, size_t len, char* buf, size_t cap);
int soulx_hidkbd_json_delay(int ms, char* buf, size_t cap);
int soulx_hidkbd_parse_nodes(const char* getevent_out, const char* name, char* nodes, size_t cap);

// ---------------------------------------------------------------------------
// Characters and reports (pure)
// ---------------------------------------------------------------------------

int soulx_hidkbd_usage_for_char(char c, int* usage, bool* shift) {
    static const char k_plain[] = " -=[]\\;'`,./\n\t";
    static const unsigned char k_plain_usage[] = { 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36,
                                                   0x37, 0x38, 0x28, 0x2B };
    static const char k_shifted[] = "!@#$%^&*()_+{}|:\"~<>?";
    static const unsigned char k_shifted_usage[] = { 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                                                     0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37,
                                                     0x38 };
    int u = -1;
    bool sh = false;
    if (c >= 'a' && c <= 'z') {
        u = 0x04 + (c - 'a');
    } else if (c >= 'A' && c <= 'Z') {
        u = 0x04 + (c - 'A');
        sh = true;
    } else if (c >= '1' && c <= '9') {
        u = 0x1E + (c - '1');
    } else if (c == '0') {
        u = 0x27;
    } else if (c != '\0') {
        const char* p = strchr(k_plain, c);
        if (p) {
            u = k_plain_usage[p - k_plain];
        } else if ((p = strchr(k_shifted, c)) != NULL) {
            u = k_shifted_usage[p - k_shifted];
            sh = true;
        }
    }
    if (u < 0) return -1;
    if (usage) *usage = u;
    if (shift) *shift = sh;
    return 0;
}

// Keyboard report from a full state. Usages 0xE0..0xE7 become modifier bits, 0 and usages outside the
// descriptor (1..3, > 0x65) are skipped, duplicates count once. -1 for more than 6 keys or bad arguments.
int soulx_hidkbd_kbd_report(int modifiers, const int* usages, int count, unsigned char report[HK_REPORT_LEN]) {
    if (!report || modifiers < 0 || modifiers > 0xFF || count < 0 || (count > 0 && !usages)) return -1;
    memset(report, 0, HK_REPORT_LEN);
    report[0] = 1;
    unsigned mods = (unsigned)modifiers;
    int n = 0;
    for (int i = 0; i < count; i++) {
        int u = usages[i];
        if (u >= 0xE0 && u <= 0xE7) {
            mods |= 1u << (u - 0xE0);
            continue;
        }
        if (u < HK_KEY_USAGE_MIN || u > HK_KEY_USAGE_MAX) continue;
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (report[3 + j] == (unsigned char)u) dup = true;
        }
        if (dup) continue;
        if (n == HK_KEYS) return -1;
        report[3 + n++] = (unsigned char)u;
    }
    report[1] = (unsigned char)mods;
    return 0;
}

typedef struct {
    char* buf;
    size_t cap;
    size_t len;
    bool overflow;
} JsonBuf;

static void jb_add(JsonBuf* j, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

static void jb_add(JsonBuf* j, const char* fmt, ...) {
    if (j->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->len, j->cap - j->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= j->cap - j->len) {
        j->overflow = true;
        return;
    }
    j->len += (size_t)n;
}

static int jb_bytes(JsonBuf* j, const unsigned char* data, size_t len) {
    jb_add(j, "[");
    for (size_t i = 0; i < len; i++) jb_add(j, "%s%u", i ? "," : "", (unsigned)data[i]);
    jb_add(j, "]");
    return j->overflow ? -1 : 0;
}

static int jb_finish(JsonBuf* j) {
    if (j->overflow) {
        if (j->cap) j->buf[0] = '\0';
        return -1;
    }
    return (int)j->len;
}

// The "register" line (newline-terminated). Returns its length, -1 when cap is too small.
int soulx_hidkbd_json_register(char* buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    JsonBuf j = { buf, cap, 0, false };
    jb_add(&j, "{\"id\":%d,\"command\":\"register\",\"name\":\"%s\",\"vid\":%d,\"pid\":%d,\"bus\":\"usb\",\"descriptor\":",
           HK_DEVICE_ID, HK_NAME, HK_VID, HK_PID);
    jb_bytes(&j, k_descriptor, sizeof(k_descriptor));
    jb_add(&j, "}\n");
    return jb_finish(&j);
}

int soulx_hidkbd_json_report(const unsigned char* report, size_t len, char* buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    buf[0] = '\0';
    if (!report || len == 0 || len > 64) return -1;
    JsonBuf j = { buf, cap, 0, false };
    jb_add(&j, "{\"id\":%d,\"command\":\"report\",\"report\":", HK_DEVICE_ID);
    jb_bytes(&j, report, len);
    jb_add(&j, "}\n");
    return jb_finish(&j);
}

int soulx_hidkbd_json_delay(int ms, char* buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    buf[0] = '\0';
    if (ms < 0 || ms > 60000) return -1;
    JsonBuf j = { buf, cap, 0, false };
    jb_add(&j, "{\"id\":%d,\"command\":\"delay\",\"duration\":%d}\n", HK_DEVICE_ID, ms);
    return jb_finish(&j);
}

// Event nodes of the devices called |name| in 'getevent -lp' output ("add device N: <path>" followed by
// '  name:     "<name>"'), newline-separated into nodes. Returns how many, -1 when nodes is too small.
int soulx_hidkbd_parse_nodes(const char* out, const char* name, char* nodes, size_t cap) {
    if (!nodes || cap == 0 || !name) return -1;
    nodes[0] = '\0';
    if (!out) return 0;
    size_t name_len = strlen(name);
    size_t used = 0;
    int count = 0;
    char node[128] = "";
    const char* p = out;
    while (*p) {
        const char* end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ')) len--;
        const char* s = p;
        size_t sl = len;
        while (sl > 0 && (*s == ' ' || *s == '\t')) {
            s++;
            sl--;
        }
        static const char k_add[] = "add device ";
        static const char k_name[] = "name:";
        if (sl > sizeof(k_add) - 1 && memcmp(s, k_add, sizeof(k_add) - 1) == 0) {
            const char* colon = memchr(s, ':', sl);
            node[0] = '\0';
            if (colon) {
                const char* q = colon + 1;
                while (q < s + sl && *q == ' ') q++;
                size_t nl = (size_t)(s + sl - q);
                if (nl > 0 && nl < sizeof(node) && !memchr(q, ' ', nl)) {
                    memcpy(node, q, nl);
                    node[nl] = '\0';
                }
            }
        } else if (sl > sizeof(k_name) - 1 && memcmp(s, k_name, sizeof(k_name) - 1) == 0 && node[0]) {
            const char* q = s + sizeof(k_name) - 1;
            while (q < s + sl && (*q == ' ' || *q == '\t')) q++;
            size_t rest = (size_t)(s + sl - q);
            if (rest == name_len + 2 && q[0] == '"' && q[rest - 1] == '"' && memcmp(q + 1, name, name_len) == 0) {
                size_t nl = strlen(node);
                if (used + nl + 2 > cap) return -1;
                memcpy(nodes + used, node, nl);
                used += nl;
                nodes[used++] = '\n';
                nodes[used] = '\0';
                count++;
            }
            node[0] = '\0';                     // one name per device
        }
        p = end ? end + 1 : p + len;
        if (!end) break;
    }
    return count;
}

// True when |now| lists a node that |before| does not, or more of them.
static bool has_new_node(const char* before, int before_count, const char* now, int now_count) {
    if (now_count > before_count) return true;
    for (const char* p = now; *p;) {
        const char* end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        bool seen = false;
        for (const char* q = before; *q;) {
            const char* qe = strchr(q, '\n');
            size_t ql = qe ? (size_t)(qe - q) : strlen(q);
            if (ql == len && memcmp(p, q, len) == 0) seen = true;
            q = qe ? qe + 1 : q + ql;
        }
        if (!seen && len > 0) return true;
        p = end ? end + 1 : p + len;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The adb child
// ---------------------------------------------------------------------------

// Serials SoulX hands to adb: "emulator-5600", "127.0.0.1:5555", ... (same rule as adb.c)
static bool serial_ok(const char* s) {
    if (!s || !*s) return false;
    size_t n = strnlen(s, 65);
    if (n > 64 || s[0] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                  ch == '-' || ch == '_' || ch == '.' || ch == ':';
        if (!ok) return false;
    }
    return true;
}

static void close_fd(int* fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

// Pipe with close-on-exec on both ends, never using descriptors 0..2. 0 or an errno value.
static int hk_pipe(int fds[2]) {
    int p[2];
    if (pipe(p) != 0) return errno ? errno : EMFILE;
    for (int i = 0; i < 2; i++) {
        if (p[i] <= STDERR_FILENO) {
            int moved = fcntl(p[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (moved < 0) {
                int e = errno ? errno : EMFILE;
                close(p[0]);
                close(p[1]);
                return e;
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

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// Starts 'adb -s <serial> shell hid -' with a stdin pipe and its stdout + stderr on one pipe. It stays in our
// process group (a Ctrl+C in the terminal ends it too; the guest then removes the device, releasing its keys).
static int hk_spawn(SoulXHidKbd* k, const char* adb) {
    const char* argv[] = { adb, "-s", k->serial, "shell", "hid", "-", NULL };
    int in_p[2] = { -1, -1 };
    int out_p[2] = { -1, -1 };
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    bool fa_ok = false;
    bool attr_ok = false;
    char** env = NULL;
    pid_t pid = -1;
    int rc;

    if ((rc = hk_pipe(in_p)) != 0 || (rc = hk_pipe(out_p)) != 0) goto done;
    if ((rc = posix_spawn_file_actions_init(&fa)) != 0) goto done;
    fa_ok = true;
    if ((rc = posix_spawn_file_actions_adddup2(&fa, in_p[0], STDIN_FILENO)) != 0 ||
        (rc = posix_spawn_file_actions_adddup2(&fa, out_p[1], STDOUT_FILENO)) != 0 ||
        (rc = posix_spawn_file_actions_adddup2(&fa, out_p[1], STDERR_FILENO)) != 0) {
        goto done;
    }
    if ((rc = posix_spawnattr_init(&attr)) != 0) goto done;
    attr_ok = true;
    {
        sigset_t defaults, mask;
        sigemptyset(&mask);
        sigemptyset(&defaults);
        const int sigs[] = { SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGALRM, SIGTERM, SIGCHLD, SIGTSTP,
                             SIGTTIN, SIGTTOU, SIGUSR1, SIGUSR2, SIGWINCH };
        for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaddset(&defaults, sigs[i]);
        short flags = (short)(POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT);
        if ((rc = posix_spawnattr_setsigdefault(&attr, &defaults)) != 0 ||
            (rc = posix_spawnattr_setsigmask(&attr, &mask)) != 0 ||
            (rc = posix_spawnattr_setflags(&attr, flags)) != 0) {
            goto done;
        }
    }
    env = soulx_build_env();
    if (!env) {
        rc = ENOMEM;
        goto done;
    }
    rc = posix_spawn(&pid, adb, &fa, &attr, (char* const*)argv, env);

done:
    soulx_free_env(env);
    if (fa_ok) posix_spawn_file_actions_destroy(&fa);
    if (attr_ok) posix_spawnattr_destroy(&attr);
    close_fd(&in_p[0]);
    close_fd(&out_p[1]);
    if (rc != 0) {
        close_fd(&in_p[1]);
        close_fd(&out_p[0]);
        soulx_log(SOULX_LOG_ERROR, "cannot run %s: %s", adb, strerror(rc));
        return -1;
    }
    k->pid = pid;
    k->in_fd = in_p[1];
    k->out_fd = out_p[0];
    set_nonblock(k->in_fd);
    set_nonblock(k->out_fd);
    (void)fcntl(k->in_fd, F_SETNOSIGPIPE, 1);   // EPIPE instead of SIGPIPE when the child is gone
    return 0;
}

// Reads whatever the child printed (it must never block on a full pipe) and keeps the tail.
static void hk_drain(SoulXHidKbd* k) {
    if (k->out_fd < 0) return;
    char chunk[1024];
    for (int i = 0; i < 64; i++) {
        ssize_t r = read(k->out_fd, chunk, sizeof(chunk));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            if (r == 0) close_fd(&k->out_fd);   // EOF: the child closed its output
            return;
        }
        size_t n = (size_t)r;
        size_t shown = n;
        while (shown > 0 && (chunk[shown - 1] == '\n' || chunk[shown - 1] == '\r')) shown--;
        if (shown > 0) soulx_log(SOULX_LOG_DEBUG, "virtual keyboard (hid): %.*s", (int)shown, chunk);
        if (n >= sizeof(k->tail) - 1) {
            memcpy(k->tail, chunk + n - (sizeof(k->tail) - 1), sizeof(k->tail) - 1);
            k->tail_len = sizeof(k->tail) - 1;
        } else {
            size_t keep = sizeof(k->tail) - 1 - n;
            if (k->tail_len > keep) {
                memmove(k->tail, k->tail + (k->tail_len - keep), keep);
                k->tail_len = keep;
            }
            memcpy(k->tail + k->tail_len, chunk, n);
            k->tail_len += n;
        }
        k->tail[k->tail_len] = '\0';
    }
}

// Reaps the child if it exited. True while it runs.
static bool hk_alive(SoulXHidKbd* k) {
    if (k->pid <= 0) return false;
    int status = 0;
    pid_t w;
    while ((w = waitpid(k->pid, &status, WNOHANG)) < 0 && errno == EINTR) {}
    if (w == 0) return true;
    if (w == k->pid) {
        if (WIFEXITED(status)) soulx_log(SOULX_LOG_DEBUG, "virtual keyboard: adb exited with status %d", WEXITSTATUS(status));
        else if (WIFSIGNALED(status)) soulx_log(SOULX_LOG_DEBUG, "virtual keyboard: adb killed by signal %d", WTERMSIG(status));
    }
    k->pid = -1;
    k->broken = true;
    return false;
}

// Last non-empty line of the child's output, for error messages.
static const char* hk_last_line(SoulXHidKbd* k, char* buf, size_t cap) {
    size_t end = k->tail_len;
    while (end > 0 && (k->tail[end - 1] == '\n' || k->tail[end - 1] == '\r' || k->tail[end - 1] == ' ')) end--;
    size_t start = end;
    while (start > 0 && k->tail[start - 1] != '\n') start--;
    size_t n = end - start;
    if (n >= cap) n = cap - 1;
    memcpy(buf, k->tail + start, n);
    buf[n] = '\0';
    return buf;
}

static bool hk_wait_exit(SoulXHidKbd* k, int timeout_ms) {
    double deadline = soulx_now_ms() + timeout_ms;
    for (;;) {
        hk_drain(k);
        if (!hk_alive(k)) return true;
        if (soulx_now_ms() >= deadline) return false;
        struct timespec ts = { 0, 10 * 1000000L };
        nanosleep(&ts, NULL);
    }
}

// Closes stdin (hid sends what it queued, removes the device and exits), then escalates.
static void hk_stop_child(SoulXHidKbd* k) {
    close_fd(&k->in_fd);
    if (k->pid > 0 && !hk_wait_exit(k, HK_CLOSE_WAIT_MS)) {
        kill(k->pid, SIGTERM);
        if (!hk_wait_exit(k, HK_KILL_WAIT_MS) && k->pid > 0) {
            kill(k->pid, SIGKILL);
            int status;
            while (waitpid(k->pid, &status, 0) < 0 && errno == EINTR) {}
            k->pid = -1;
        }
    }
    close_fd(&k->out_fd);
    k->broken = true;
}

// Writes all of data to the child's stdin (2 s at most). -1 marks the keyboard broken.
static int hk_write(SoulXHidKbd* k, const char* data, size_t len) {
    if (k->broken || k->in_fd < 0) return -1;
    size_t off = 0;
    double deadline = soulx_now_ms() + HK_WRITE_TIMEOUT_MS;
    while (off < len) {
        ssize_t w = write(k->in_fd, data + off, len - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            double left = deadline - soulx_now_ms();
            if (left <= 0) {
                soulx_log(SOULX_LOG_WARN, "virtual keyboard on %s does not accept input any more", k->serial);
                k->broken = true;
                return -1;
            }
            hk_drain(k);                        // the child may be waiting for us to read its output
            struct pollfd pfd = { k->in_fd, POLLOUT, 0 };
            (void)poll(&pfd, 1, left < 50 ? (int)left + 1 : 50);
            continue;
        }
        hk_drain(k);
        (void)hk_alive(k);
        soulx_log(SOULX_LOG_DEBUG, "virtual keyboard on %s: write failed: %s", k->serial, strerror(errno));
        k->broken = true;
        return -1;
    }
    return 0;
}

// Appends one report line to a batch.
static int add_report(JsonBuf* j, const unsigned char* report, size_t len) {
    char line[HK_LINE_MAX];
    if (soulx_hidkbd_json_report(report, len, line, sizeof(line)) < 0) return -1;
    jb_add(j, "%s", line);
    return j->overflow ? -1 : 0;
}

static int add_delay(JsonBuf* j, int ms) {
    char line[HK_LINE_MAX];
    if (soulx_hidkbd_json_delay(ms, line, sizeof(line)) < 0) return -1;
    jb_add(j, "%s", line);
    return j->overflow ? -1 : 0;
}

static void remember(SoulXHidKbd* k, const unsigned char* report) {
    memcpy(k->last, report, HK_REPORT_LEN);
    k->have_last = true;
}

// Lines of one key press: [modifiers], modifiers + key, delay, [modifiers], all released.
static int add_press(JsonBuf* j, int modifiers, int usage, int hold_ms) {
    unsigned char mods_only[HK_REPORT_LEN];
    unsigned char pressed[HK_REPORT_LEN];
    unsigned char none[HK_REPORT_LEN];
    if (soulx_hidkbd_kbd_report(modifiers, NULL, 0, mods_only) != 0 ||
        soulx_hidkbd_kbd_report(modifiers, &usage, 1, pressed) != 0 ||
        soulx_hidkbd_kbd_report(0, NULL, 0, none) != 0) {
        return -1;
    }
    bool mods = mods_only[1] != 0 && memcmp(mods_only, pressed, HK_REPORT_LEN) != 0;
    if (mods && add_report(j, mods_only, HK_REPORT_LEN) != 0) return -1;
    if (add_report(j, pressed, HK_REPORT_LEN) != 0) return -1;
    if (hold_ms > 0 && add_delay(j, hold_ms) != 0) return -1;
    if (mods && add_report(j, mods_only, HK_REPORT_LEN) != 0) return -1;
    return add_report(j, none, HK_REPORT_LEN);
}

// ---------------------------------------------------------------------------
// Device detection
// ---------------------------------------------------------------------------

// Nodes of our keyboards listed by 'adb shell getevent -lp'. Count, or -1 when adb failed.
static int probe_nodes(const char* adb, const char* serial, char* nodes, size_t cap, int timeout_ms) {
    const char* argv[] = { adb, "-s", serial, "shell", "getevent", "-lp", NULL };
    SoulXCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.argv = argv;
    cmd.soulx_env = true;
    cmd.timeout_ms = timeout_ms;
    cmd.max_output = 1024 * 1024;
    SoulXCmdResult res;
    int status = soulx_run(&cmd, &res);
    int count = -1;
    if (status == 0 && !res.timed_out) count = soulx_hidkbd_parse_nodes(res.out, HK_NAME, nodes, cap);
    else soulx_log(SOULX_LOG_DEBUG, "virtual keyboard: getevent on %s failed (status %d)", serial, status);
    soulx_cmd_result_free(&res);
    return count;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

SoulXHidKbd* soulx_hidkbd_open(const char* serial) {
    if (!serial_ok(serial)) {
        soulx_log(SOULX_LOG_DEBUG, "virtual keyboard: invalid device serial");
        return NULL;
    }
    char adb[PATH_MAX];
    if (soulx_tool_path("adb", adb, sizeof(adb)) != 0 || access(adb, X_OK) != 0) {
        soulx_log(SOULX_LOG_DEBUG, "virtual keyboard: SoulX's adb is not installed");
        return NULL;
    }

    // Keyboards that already carry our name (another SoulX process, or one that is still being removed)
    char before[HK_NODES_MAX];
    int before_count = probe_nodes(adb, serial, before, sizeof(before), HK_PROBE_TIMEOUT_MS);
    if (before_count < 0) {
        soulx_log(SOULX_LOG_WARN, "virtual keyboard: cannot list the input devices of %s", serial);
        return NULL;
    }

    SoulXHidKbd* k = calloc(1, sizeof(*k));
    if (!k) return NULL;
    k->pid = -1;
    k->in_fd = -1;
    k->out_fd = -1;
    memcpy(k->serial, serial, strlen(serial) + 1);
    if (pthread_mutex_init(&k->lock, NULL) != 0) {
        free(k);
        return NULL;
    }
    if (hk_spawn(k, adb) != 0) {
        pthread_mutex_destroy(&k->lock);
        free(k);
        return NULL;
    }

    char line[1024];
    bool found = false;
    bool ambiguous = false;
    bool interrupted = false;
    int len = soulx_hidkbd_json_register(line, sizeof(line));
    if (len > 0 && hk_write(k, line, (size_t)len) == 0) {
        double deadline = soulx_now_ms() + HK_OPEN_TIMEOUT_MS;
        for (;;) {
            if (soulx_sleep_ms(HK_POLL_MS) != 0) {
                interrupted = true;
                break;
            }
            hk_drain(k);
            if (!hk_alive(k)) break;            // adb or hid gave up (no hid tool, no uhid access, offline...)
            int left = (int)(deadline - soulx_now_ms());
            if (left <= 0) break;
            char now[HK_NODES_MAX];
            int count = probe_nodes(adb, serial, now, sizeof(now), left < HK_PROBE_TIMEOUT_MS ? left : HK_PROBE_TIMEOUT_MS);
            if (count > 0 && has_new_node(before, before_count, now, count)) {
                found = true;
                break;
            }
            if (count > 0) ambiguous = true;
            if (soulx_now_ms() >= deadline) break;
        }
    }
    if (!found && ambiguous && !interrupted && hk_alive(k)) {
        // Our name is listed but no node is new: an old keyboard went away while ours appeared on its node.
        soulx_log(SOULX_LOG_DEBUG, "virtual keyboard: could not tell the new device apart; assuming it is ours");
        found = true;
    }
    if (!found) {
        char last[160];
        hk_drain(k);
        hk_last_line(k, last, sizeof(last));
        if (!interrupted) {
            soulx_log(SOULX_LOG_WARN, "virtual keyboard: the device did not appear on %s within %d s%s%s", serial,
                     HK_OPEN_TIMEOUT_MS / 1000, last[0] ? ": " : "", last);
        }
        hk_stop_child(k);
        pthread_mutex_destroy(&k->lock);
        free(k);
        return NULL;
    }
    struct timespec ts = { 0, HK_SETTLE_MS * 1000000L };
    nanosleep(&ts, NULL);
    hk_drain(k);
    soulx_log(SOULX_LOG_DEBUG, "virtual keyboard ready on %s", serial);
    return k;
}

int soulx_hidkbd_set(SoulXHidKbd* k, int modifiers, const int* usages, int count) {
    if (!k) return -1;
    unsigned char report[HK_REPORT_LEN];
    if (soulx_hidkbd_kbd_report(modifiers, usages, count, report) != 0) return -1;
    pthread_mutex_lock(&k->lock);
    int rc = 0;
    hk_drain(k);
    if (k->broken) {
        rc = -1;
    } else if (!k->have_last || memcmp(k->last, report, HK_REPORT_LEN) != 0) {
        char line[HK_LINE_MAX];
        int len = soulx_hidkbd_json_report(report, HK_REPORT_LEN, line, sizeof(line));
        rc = (len > 0 && hk_write(k, line, (size_t)len) == 0) ? 0 : -1;
        if (rc == 0) remember(k, report);
    }
    pthread_mutex_unlock(&k->lock);
    return rc;
}

int soulx_hidkbd_tap(SoulXHidKbd* k, int modifiers, int usage) {
    if (!k || modifiers < 0 || modifiers > 0xFF) return -1;
    bool is_mod = usage >= 0xE0 && usage <= 0xE7;
    if (!is_mod && (usage < HK_KEY_USAGE_MIN || usage > HK_KEY_USAGE_MAX)) return -1;
    char batch[2048];
    JsonBuf j = { batch, sizeof(batch), 0, false };
    if (add_press(&j, modifiers, usage, HK_TAP_HOLD_MS) != 0) return -1;
    unsigned char none[HK_REPORT_LEN];
    (void)soulx_hidkbd_kbd_report(0, NULL, 0, none);
    pthread_mutex_lock(&k->lock);
    hk_drain(k);
    int rc = hk_write(k, batch, j.len);
    if (rc == 0) remember(k, none);
    pthread_mutex_unlock(&k->lock);
    return rc;
}

int soulx_hidkbd_consumer(SoulXHidKbd* k, int usage) {
    if (!k || usage <= 0 || usage > HK_CONSUMER_MAX) return -1;
    const unsigned char down[3] = { 2, (unsigned char)(usage & 0xFF), (unsigned char)(usage >> 8) };
    const unsigned char up[3] = { 2, 0, 0 };
    char batch[512];
    JsonBuf j = { batch, sizeof(batch), 0, false };
    if (add_report(&j, down, sizeof(down)) != 0 || add_delay(&j, HK_TAP_HOLD_MS) != 0 ||
        add_report(&j, up, sizeof(up)) != 0) {
        return -1;
    }
    pthread_mutex_lock(&k->lock);
    hk_drain(k);
    int rc = hk_write(k, batch, j.len);
    pthread_mutex_unlock(&k->lock);
    return rc;
}

int soulx_hidkbd_type(SoulXHidKbd* k, const char* ascii) {
    if (!k || !ascii) return -1;
    for (const char* p = ascii; *p; p++) {
        if (soulx_hidkbd_usage_for_char(*p, NULL, NULL) != 0) return -1;    // nothing typed: non-ASCII or control
    }
    unsigned char none[HK_REPORT_LEN];
    (void)soulx_hidkbd_kbd_report(0, NULL, 0, none);
    pthread_mutex_lock(&k->lock);
    int rc = k->broken ? -1 : 0;
    if (rc == 0 && k->have_last && memcmp(k->last, none, HK_REPORT_LEN) != 0) {   // start with nothing held
        char line[HK_LINE_MAX];
        int len = soulx_hidkbd_json_report(none, HK_REPORT_LEN, line, sizeof(line));
        rc = (len > 0 && hk_write(k, line, (size_t)len) == 0) ? 0 : -1;
    }
    double start = soulx_now_ms();
    size_t i = 0;
    for (const char* p = ascii; *p && rc == 0; p++, i++) {
        double due = start + (double)i * HK_TYPE_CHAR_MS;     // host pacing (no drift)
        for (;;) {
            double left = due - soulx_now_ms();
            if (left <= 0) break;
            if (soulx_sleep_ms(left < 20 ? (int)left + 1 : 20) != 0) break;
        }
        if (soulx_interrupted()) {               // Ctrl+C in the CLI: stop typing, keys are released below
            rc = -1;
            break;
        }
        int usage = 0;
        bool shift = false;
        (void)soulx_hidkbd_usage_for_char(*p, &usage, &shift);
        char batch[1024];
        JsonBuf j = { batch, sizeof(batch), 0, false };
        // Shift travels in the same report as the key (verified with the prototype): 2 reports per character.
        unsigned char pressed[HK_REPORT_LEN];
        if (soulx_hidkbd_kbd_report(shift ? SOULX_HID_MOD_LSHIFT : 0, &usage, 1, pressed) != 0 ||
            add_report(&j, pressed, HK_REPORT_LEN) != 0 || add_delay(&j, HK_TYPE_HOLD_MS) != 0 ||
            add_report(&j, none, HK_REPORT_LEN) != 0) {
            rc = -1;
            break;
        }
        hk_drain(k);
        rc = hk_write(k, batch, j.len);
    }
    if (!k->broken) {
        if (rc != 0) {                          // never leave a key down
            char line[HK_LINE_MAX];
            int len = soulx_hidkbd_json_report(none, HK_REPORT_LEN, line, sizeof(line));
            if (len > 0) (void)hk_write(k, line, (size_t)len);
        }
        remember(k, none);
    }
    pthread_mutex_unlock(&k->lock);
    return rc;
}

void soulx_hidkbd_close(SoulXHidKbd* k) {
    if (!k) return;
    pthread_mutex_lock(&k->lock);
    hk_drain(k);
    if (!k->broken && hk_alive(k)) {
        unsigned char none[HK_REPORT_LEN];
        const unsigned char consumer_up[3] = { 2, 0, 0 };
        char batch[512];
        JsonBuf j = { batch, sizeof(batch), 0, false };
        (void)soulx_hidkbd_kbd_report(0, NULL, 0, none);
        if (add_report(&j, none, HK_REPORT_LEN) == 0 && add_report(&j, consumer_up, sizeof(consumer_up)) == 0) {
            (void)hk_write(k, batch, j.len);
        }
    }
    hk_stop_child(k);
    pthread_mutex_unlock(&k->lock);
    pthread_mutex_destroy(&k->lock);
    free(k);
}
