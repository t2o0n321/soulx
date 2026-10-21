/*
 * SoulX Android Emulator - macOS engine control
 *
 * Instance lifecycle (create / start / stop / delete / post-boot) on top of the
 * SoulX-owned Android Emulator runtime, plus the public soulx_* lifecycle API.
 * The public API is backed by the persistent registry: API instance ids are
 * registry ids and every state is derived from the live engine processes, so
 * the CLI and API users always agree.
 *
 * The engine always runs without a window of its own (-no-window) and publishes
 * its frames in shared memory (-share-vid) for SoulX's window; opening that
 * window is the CLI's job (soulx_window_spawn).
 *
 * Stopping an instance asks the console to "kill" it (the engine saves its
 * Quick Boot snapshot first) and, once no SoulX instance is left running, also
 * stops SoulX's private adb server, which the engine starts itself and which
 * would otherwise outlive it. A stop returns only when the engine process is
 * gone, so its console port can be used again at once: an instance keeps its
 * console port (and adb serial) from one run to the next (conf last_port).
 *
 * Per-run markers in run/ (each holds the engine pid it belongs to):
 *   <name>.stopping  the console accepted "kill": the engine is saving its snapshot and exits by itself
 *                    (mtime = when). Other stops and starts wait for it instead of signalling it.
 *   <name>.postboot  the post-boot setup (stay awake, private DNS off, lean) was applied to this run.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/resource.h>

#ifndef SZOMB
#define SZOMB 5                 // <sys/proc.h>: process is a zombie
#endif

#include "soulx_mac.h"

// ---------------------------------------------------------------------------
// Limits and timings
// ---------------------------------------------------------------------------

#define ENGINE_CORES_MIN        1
#define ENGINE_CORES_MAX        8
#define ENGINE_RAM_MIN          4096        // Android 17 (API 37) floor
#define ENGINE_RAM_MAX          12288
#define ENGINE_DATA_MIN         4
#define ENGINE_DATA_MAX         64
#define ENGINE_DIM_MIN          320
#define ENGINE_DIM_MAX          3840
#define ENGINE_DPI_MIN          120
#define ENGINE_DPI_MAX          640
#define ENGINE_FPS_MIN          20
#define ENGINE_FPS_MAX          60          // above the host display's refresh the engine is undefined

#define BOOT_TIMEOUT_DEFAULT_S  300
#define PID_HANDOVER_MS         3000        // launcher -> qemu hand-over window
#define STOP_CONSOLE_WAIT_MS    100000      // after console "kill": the engine saves its Quick Boot snapshot first;
                                            // proc.c lets the engine's own watchdog allow up to 90 s for that
#define STOP_MIN_WAIT_MS        15000       // a stop that joins a shutdown under way waits at least this long
#define STOP_NO_CONSOLE_WAIT_MS 10000       // console unreachable and no sign of a shutdown: grace before SIGTERM
#define STOP_TERM_WAIT_MS       10000
#define STOP_KILL_WAIT_MS       5000
#define STOP_EXITING_MS         20000       // an engine that is tearing down gets this long to disappear
#define STOP_PORTS_MS           3000        // after the exit: its console / adb ports should close within this
#define STOP_SLOW_MS            30000       // slower stops name the engine log
#define START_PORT_GRACE_MS     3000        // the previous port may still be held by an engine that just died
#define PORT_PROBE_MS           300         // connect() probes of loopback ports
#define ADB_KILL_SERVER_MS      10000       // `adb kill-server` on SoulX's private server
#define POST_BOOT_TIMEOUT_MS    20000
#define LOG_KEEP                3           // rotated logs kept per instance: <name>.log.1 .. <name>.log.3
#define LOG_TAIL_LINES          20
#define LOG_TAIL_BYTES          16384
#define ENGINE_TRACK_MAX        64
#define MARKER_STOPPING         "stopping"
#define MARKER_POSTBOOT         "postboot"

#define EM_DASH                 "\xE2\x80\x94"     // UTF-8 em dash
#define ELLIPSIS                "\xE2\x80\xA6"     // UTF-8 horizontal ellipsis

// Console ports are the even numbers SOULX_PORT_FIRST..SOULX_PORT_LAST (adb = port + 1); the slot arithmetic
// in alloc_port() relies on that, whatever range soulx_mac.h defines.
_Static_assert(SOULX_PORT_FIRST > 0 && SOULX_PORT_FIRST % 2 == 0 && SOULX_PORT_LAST > SOULX_PORT_FIRST &&
               (SOULX_PORT_LAST - SOULX_PORT_FIRST) % 2 == 0 && SOULX_PORT_LAST + 1 <= 65534,
               "SOULX_PORT_FIRST..SOULX_PORT_LAST must be even console ports (adb = console port + 1)");

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static _Atomic(LogCallback) g_log_cb;
static _Atomic(InstanceStateCallback) g_state_cb;
static _Atomic(PerformanceCallback) g_perf_cb;
static _Thread_local bool t_in_log_cb;      // a callback that logs must not recurse

// Process-local knowledge used for InstanceInfo.state (keyed by name + engine pid).
typedef struct {
    char name[SOULX_NAME_MAX + 1];
    pid_t pid;
} TrackedEngine;

static pthread_mutex_t g_track_mu = PTHREAD_MUTEX_INITIALIZER;
static TrackedEngine g_booted[ENGINE_TRACK_MAX];   // seen with sys.boot_completed == 1
static TrackedEngine g_paused[ENGINE_TRACK_MAX];   // paused through soulx_pause_instance

static bool track_has(const TrackedEngine* set, const char* name, pid_t pid) {
    bool found = false;
    pthread_mutex_lock(&g_track_mu);
    for (int i = 0; i < ENGINE_TRACK_MAX && !found; i++) {
        found = set[i].pid == pid && pid > 0 && !strcmp(set[i].name, name);
    }
    pthread_mutex_unlock(&g_track_mu);
    return found;
}

static void track_remove_locked(TrackedEngine* set, const char* name) {
    for (int i = 0; i < ENGINE_TRACK_MAX; i++) {
        if (set[i].pid > 0 && !strcmp(set[i].name, name)) memset(&set[i], 0, sizeof(set[i]));
    }
}

static void track_remove(TrackedEngine* set, const char* name) {
    pthread_mutex_lock(&g_track_mu);
    track_remove_locked(set, name);
    pthread_mutex_unlock(&g_track_mu);
}

static void track_add(TrackedEngine* set, const char* name, pid_t pid) {
    pthread_mutex_lock(&g_track_mu);
    track_remove_locked(set, name);
    int slot = -1;
    for (int i = 0; i < ENGINE_TRACK_MAX && slot < 0; i++) {
        if (set[i].pid <= 0) slot = i;
    }
    if (slot < 0) slot = (int)((unsigned)pid % ENGINE_TRACK_MAX);   // full: recycle a slot
    snprintf(set[slot].name, sizeof(set[slot].name), "%s", name);
    set[slot].pid = pid;
    pthread_mutex_unlock(&g_track_mu);
}

static void notify_state(int id, InstanceState state) {
    InstanceStateCallback cb = atomic_load(&g_state_cb);
    if (cb) cb(id, state);
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static bool all_digits(const char* s) {
    if (!s || !*s) return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
    }
    return true;
}

static void iso_now(char* buf, size_t cap) {
    time_t now = time(NULL);
    struct tm tmv;
    if (!gmtime_r(&now, &tmv) || strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &tmv) == 0) {
        if (cap) buf[0] = '\0';
    }
}

static bool iso_parse(const char* s, time_t* out) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    const char* end = strptime(s, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    if (!end || *end != '\0') return false;
    *out = timegm(&tmv);
    return true;
}

// "My Game #1" -> "my-game-1"; always produces a valid SoulX name. Names with non-ASCII characters
// (which are dropped) get "-<6 hex digits>" of a hash of the whole input, so "游戏" and "你好" become
// two different instances while the same input always maps to the same name (soulx_find_instance).
static void slugify(const char* in, char* out, size_t cap) {
    char base[SOULX_NAME_MAX + 1];
    size_t n = 0;
    bool sep = false;
    bool lossy = false;
    uint32_t hash = 2166136261u;                         // FNV-1a over the original bytes
    for (const unsigned char* p = (const unsigned char*)(in ? in : ""); *p; p++) {
        hash = (hash ^ *p) * 16777619u;
        if (*p >= 0x80) lossy = true;
    }
    size_t limit = lossy ? SOULX_NAME_MAX - 7 : SOULX_NAME_MAX;   // room for "-xxxxxx"
    for (const unsigned char* p = (const unsigned char*)(in ? in : ""); *p && n < limit; p++) {
        unsigned char ch = *p;
        if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch - 'A' + 'a');
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            base[n++] = (char)ch;
            sep = false;
        } else if (n > 0 && !sep) {
            base[n++] = ch == '_' ? '_' : '-';
            sep = true;
        }
    }
    while (n > 0 && (base[n - 1] == '-' || base[n - 1] == '_')) n--;
    base[n] = '\0';
    if (lossy) {
        snprintf(out, cap, "%s-%06x", n > 0 ? base : "instance", (unsigned)(hash & 0xffffffu));
    } else if (n == 0) {
        snprintf(out, cap, "instance");
    } else if (all_digits(base)) {                       // digits alone would read as an id
        snprintf(out, cap, "instance-%.23s", base);
    } else {
        snprintf(out, cap, "%s", base);
    }
}

// Comma separated IPv4/IPv6 resolvers, at most 4.
static bool dns_valid(const char* dns) {
    size_t len = strnlen(dns, 128);
    if (len == 0) return true;
    if (len >= 128 || dns[0] == ',' || dns[len - 1] == ',' || strstr(dns, ",,")) return false;
    char tmp[128];
    memcpy(tmp, dns, len + 1);
    int count = 0;
    char* save = NULL;
    for (char* tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        struct in_addr a4;
        struct in6_addr a6;
        if (inet_pton(AF_INET, tok, &a4) != 1 && inet_pton(AF_INET6, tok, &a6) != 1) return false;
        if (++count > 4) return false;
    }
    return count > 0;
}

static void image_keys(char* buf, size_t cap) {
    size_t count = 0, used = 0;
    const SoulXPackage* pk = soulx_runtime_packages(&count);
    buf[0] = '\0';
    for (size_t i = 0; pk && i < count; i++) {
        if (pk[i].kind != SOULX_PKG_IMAGE) continue;
        int n = snprintf(buf + used, cap - used, "%s%s", used ? ", " : "", pk[i].key);
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
    }
}

static const SoulXPackage* image_of(const SoulXInstanceConf* c) {
    const SoulXPackage* img = soulx_runtime_find(c->image);
    return img && img->kind == SOULX_PKG_IMAGE ? img : NULL;
}

static const char* android_name(const SoulXInstanceConf* c) {
    const SoulXPackage* img = image_of(c);
    return img && img->android_name ? img->android_name : "Android";
}

// Strict validation used when an instance is created (messages for the CLI).
static int conf_validate(const SoulXInstanceConf* c) {
    if (!soulx_name_valid(c->name)) {
        soulx_log(SOULX_LOG_ERROR, "invalid instance name '%.32s': use 1-32 characters a-z, 0-9, '-' or '_', "
                 "starting with a letter or digit", c->name);
        return -1;
    }
    if (all_digits(c->name)) {
        soulx_log(SOULX_LOG_ERROR, "instance name '%s' is all digits, which is reserved for instance ids", c->name);
        return -1;
    }
    if (!memchr(c->image, '\0', sizeof(c->image)) || !image_of(c)) {
        char keys[128];
        image_keys(keys, sizeof(keys));
        soulx_log(SOULX_LOG_ERROR, "unknown system image '%.15s' (available: %s)", c->image, keys);
        return -1;
    }
    if (c->cores < ENGINE_CORES_MIN || c->cores > ENGINE_CORES_MAX) {
        soulx_log(SOULX_LOG_ERROR, "cores must be between %d and %d (got %d)", ENGINE_CORES_MIN, ENGINE_CORES_MAX, c->cores);
        return -1;
    }
    if (c->ram_mb < ENGINE_RAM_MIN || c->ram_mb > ENGINE_RAM_MAX) {
        soulx_log(SOULX_LOG_ERROR, "RAM must be between %d and %d MB (got %d; Android 17 needs at least %d MB)",
                 ENGINE_RAM_MIN, ENGINE_RAM_MAX, c->ram_mb, ENGINE_RAM_MIN);
        return -1;
    }
    if (c->data_gb < ENGINE_DATA_MIN || c->data_gb > ENGINE_DATA_MAX) {
        soulx_log(SOULX_LOG_ERROR, "data partition must be between %d and %d GB (got %d)",
                 ENGINE_DATA_MIN, ENGINE_DATA_MAX, c->data_gb);
        return -1;
    }
    if (c->width < ENGINE_DIM_MIN || c->width > ENGINE_DIM_MAX ||
        c->height < ENGINE_DIM_MIN || c->height > ENGINE_DIM_MAX) {
        soulx_log(SOULX_LOG_ERROR, "screen size must be between %d and %d pixels per side (got %dx%d)",
                 ENGINE_DIM_MIN, ENGINE_DIM_MAX, c->width, c->height);
        return -1;
    }
    if (c->dpi < ENGINE_DPI_MIN || c->dpi > ENGINE_DPI_MAX) {
        soulx_log(SOULX_LOG_ERROR, "density must be between %d and %d dpi (got %d)", ENGINE_DPI_MIN, ENGINE_DPI_MAX, c->dpi);
        return -1;
    }
    if (c->fps < ENGINE_FPS_MIN || c->fps > ENGINE_FPS_MAX) {
        soulx_log(SOULX_LOG_ERROR, "fps must be between %d and %d (got %d)", ENGINE_FPS_MIN, ENGINE_FPS_MAX, c->fps);
        return -1;
    }
    if (!memchr(c->gpu, '\0', sizeof(c->gpu)) || (strcmp(c->gpu, "metal") && strcmp(c->gpu, "host") &&
                                                   strcmp(c->gpu, "host-novulkan") && strcmp(c->gpu, "software"))) {
        soulx_log(SOULX_LOG_ERROR, "gpu must be metal, host, host-novulkan or software");
        return -1;
    }
    if (!memchr(c->dns, '\0', sizeof(c->dns)) || !dns_valid(c->dns)) {
        soulx_log(SOULX_LOG_ERROR, "invalid DNS server list (expected IP[,IP], at most 4)");
        return -1;
    }
    return 0;
}

static int load_conf(const char* name, SoulXInstanceConf* c) {
    if (!name || !*name) {
        soulx_log(SOULX_LOG_ERROR, "no instance name given");
        return -1;
    }
    if (soulx_resolve_instance(name, c) != 0) {
        soulx_log(SOULX_LOG_ERROR, "no such instance '%.64s' (see: soulx list)", name);
        return -1;
    }
    return 0;
}

static int log_path_for(const char* name, char* buf, size_t cap) {
    return soulx_path(buf, cap, "logs/%s.log", name);
}

static int window_log_path_for(const char* name, char* buf, size_t cap) {
    return soulx_path(buf, cap, "logs/%s.window.log", name);
}

// <path>.2 -> .3, <path>.1 -> .2, <path> -> .1: the logs of the last LOG_KEEP runs survive.
static void rotate_log(const char* path) {
    char from[PATH_MAX], to[PATH_MAX];
    for (int i = LOG_KEEP; i >= 1; i--) {
        int a = i > 1 ? snprintf(from, sizeof(from), "%s.%d", path, i - 1) : snprintf(from, sizeof(from), "%s", path);
        int b = snprintf(to, sizeof(to), "%s.%d", path, i);
        if (a < 0 || (size_t)a >= sizeof(from) || b < 0 || (size_t)b >= sizeof(to)) return;
        if (rename(from, to) != 0 && errno != ENOENT) {
            soulx_log(SOULX_LOG_DEBUG, "cannot rotate %s: %s", from, strerror(errno));
        }
    }
}

// Removes <path> and its rotated copies.
static void remove_logs(const char* path) {
    (void)unlink(path);
    char old[PATH_MAX];
    for (int i = 1; i <= LOG_KEEP + 1; i++) {        // + 1: a log.N left by a build that kept more
        int n = snprintf(old, sizeof(old), "%s.%d", path, i);
        if (n > 0 && (size_t)n < sizeof(old)) (void)unlink(old);
    }
}

// The last bytes of a log (at most cap - 1), control characters masked; returns their length.
static size_t log_tail(const char* path, char* buf, size_t cap) {
    if (cap == 0) return 0;
    buf[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat sb;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode)) {
        close(fd);
        return 0;
    }
    off_t want = (off_t)(cap - 1);
    off_t from = sb.st_size > want ? sb.st_size - want : 0;
    ssize_t n = pread(fd, buf, (size_t)want, from);
    close(fd);
    if (n <= 0) return 0;
    for (ssize_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if ((ch < 0x20 && ch != '\n' && ch != '\t') || ch == 0x7f || ch == '\0') buf[i] = ' ';
    }
    buf[n] = '\0';
    return (size_t)n;
}

// Prints the last |max_lines| lines of the engine log (control characters masked).
static void print_log_tail(const char* path, int max_lines) {
    char buf[LOG_TAIL_BYTES + 1];
    size_t end = log_tail(path, buf, sizeof(buf));
    while (end > 0 && (buf[end - 1] == '\n' || buf[end - 1] == ' ')) end--;
    buf[end] = '\0';
    if (end == 0) return;

    size_t pos = end;
    int lines = 0;
    while (pos > 0) {
        if (buf[pos - 1] == '\n' && ++lines >= max_lines) break;
        pos--;
    }
    soulx_log(SOULX_LOG_ERROR, "last lines of %s:", path);
    char* save = NULL;
    for (char* line = strtok_r(buf + pos, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        soulx_log(SOULX_LOG_INFO, "  | %s", line);
    }
}

// The engine log of the current run shows that the engine is shutting down: the emulator prints these
// lines once it accepted a console "kill" (its watchdog announcement) and while it saves the snapshot.
static bool log_shows_shutdown(const char* name) {
    char path[PATH_MAX], buf[LOG_TAIL_BYTES + 1];
    if (log_path_for(name, path, sizeof(path)) != 0 || log_tail(path, buf, sizeof(buf)) == 0) return false;
    return strstr(buf, "to shutdown gracefully") || strstr(buf, "Saving with gfxstream") ||
           strstr(buf, "Saving snapshot '");
}

// The engine log shows it could not load the Quick Boot snapshot (incompatible format after an update, or a
// snapshot left half-written by a force-stop/crash). The emulator treats this as fatal instead of cold-booting.
static bool log_bad_snapshot(const char* name) {
    char path[PATH_MAX], buf[LOG_TAIL_BYTES + 1];
    if (log_path_for(name, path, sizeof(path)) != 0 || log_tail(path, buf, sizeof(buf)) == 0) return false;
    return strstr(buf, "Snapshot file version is not compatible") ||
           strstr(buf, "napshot is not compatible") ||
           strstr(buf, "Failed to load snapshot") ||
           strstr(buf, "Could not load snapshot") ||
           (strstr(buf, "Loading snapshot") && strstr(buf, "FATAL"));
}

// Delete the instance's Quick Boot snapshot so the next start cold-boots (|why| is logged as a warning; NULL
// logs quietly). soulx_rmtree refuses anything outside soulx_home(), and the path is inside $S/avd, so this is safe.
static void clear_quickboot_snapshot(const char* name, const char* why) {
    char avd[SOULX_NAME_MAX + 8], dir[PATH_MAX];
    soulx_avd_name(name, avd, sizeof(avd));
    if (soulx_path(dir, sizeof(dir), "avd/%s.avd/snapshots", avd) != 0 || !soulx_is_dir(dir)) return;
    if (soulx_rmtree(dir) == 0) {
        if (why) soulx_log(SOULX_LOG_WARN, "removed '%s' Quick Boot snapshot (%s); the next start boots fresh", name, why);
        else soulx_log(SOULX_LOG_DEBUG, "removed '%s' Quick Boot snapshot", name);
    }
}

// ---------------------------------------------------------------------------
// Per-run markers (run/<name>.stopping, run/<name>.postboot)
// ---------------------------------------------------------------------------

static int marker_path(const char* name, const char* kind, char* buf, size_t cap) {
    return soulx_path(buf, cap, "run/%s.%s", name, kind);
}

// True when the marker exists for engine |pid|; *when = the time it was written.
static bool marker_has(const char* name, const char* kind, pid_t pid, time_t* when) {
    char path[PATH_MAX], buf[32];
    if (pid <= 0 || marker_path(name, kind, path, sizeof(path)) != 0) return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat sb;
    ssize_t n = -1;
    if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode)) n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    char* end = NULL;
    errno = 0;
    long v = strtol(buf, &end, 10);
    if (errno != 0 || end == buf || (*end != '\n' && *end != '\0') || v != (long)pid) return false;
    if (when) *when = sb.st_mtime;
    return true;
}

static void marker_set(const char* name, const char* kind, pid_t pid) {
    char path[PATH_MAX], body[32];
    int n = snprintf(body, sizeof(body), "%d\n", (int)pid);
    if (pid <= 0 || n < 0 || (size_t)n >= sizeof(body) || marker_path(name, kind, path, sizeof(path)) != 0) return;
    if (soulx_write_file_atomic(path, body, (size_t)n) != 0) soulx_log(SOULX_LOG_DEBUG, "cannot write %s", path);
}

static void markers_clear(const char* name) {
    char path[PATH_MAX];
    if (marker_path(name, MARKER_STOPPING, path, sizeof(path)) == 0) (void)unlink(path);
    if (marker_path(name, MARKER_POSTBOOT, path, sizeof(path)) == 0) (void)unlink(path);
}

// Time left of the snapshot-save allowance for a shutdown that began at |since| (never below the minimum).
static int stop_remaining_ms(time_t since) {
    time_t now = time(NULL);
    double elapsed = (now > since) ? (double)(now - since) * 1000.0 : 0.0;
    double left = STOP_CONSOLE_WAIT_MS - elapsed;
    return left < STOP_MIN_WAIT_MS ? STOP_MIN_WAIT_MS : (int)left;
}

// ---------------------------------------------------------------------------
// Engine processes
// ---------------------------------------------------------------------------

static bool pid_is_qemu(pid_t pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) return false;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strncmp(base, "qemu-system-", 12) == 0;      // also matches qemu-system-aarch64-headless
}

static pid_t find_qemu_child(pid_t parent) {
    int n = proc_listallpids(NULL, 0);
    if (n <= 0) return 0;
    size_t cap = (size_t)n + 64;
    pid_t* pids = calloc(cap, sizeof(pid_t));
    if (!pids) return 0;
    int got = proc_listallpids(pids, (int)(cap * sizeof(pid_t)));
    pid_t found = 0;
    for (int i = 0; i < got && !found; i++) {
        struct proc_bsdinfo bi;
        if (pids[i] <= 1) continue;
        if (proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) != (int)sizeof(bi)) continue;
        if ((pid_t)bi.pbi_ppid == parent && pid_is_qemu(pids[i]) && soulx_pid_is_engine(pids[i])) found = pids[i];
    }
    free(pids);
    return found;
}

// Which pid to track. The 'emulator' launcher normally execv()s
// qemu/darwin-aarch64/qemu-system-aarch64[-headless] in place (same pid; see
// main-emulator.cpp safe_execv). To also cope with a launcher that spawns qemu
// as a child, poll for up to PID_HANDOVER_MS until the launcher pid *is* qemu.
// If it is not by then, track a qemu child of the launcher when there is one,
// else the launcher itself. If the launcher already exited, *exited is set and
// the caller lets the registry find the engine by its command line
// (-avd soulx_<name> -port <P>, see soulx_instance_running), which also repairs
// the state file whenever a recorded pid later disappears.
static pid_t resolve_engine_pid(pid_t launcher, bool* exited) {
    *exited = false;
    double start = soulx_now_ms();
    for (;;) {
        int status = 0;
        pid_t w = waitpid(launcher, &status, WNOHANG);
        if (w == launcher || (w < 0 && errno == ECHILD && !soulx_pid_alive(launcher))) {
            if (w == launcher && WIFEXITED(status)) {
                soulx_log(SOULX_LOG_DEBUG, "emulator launcher exited with status %d", WEXITSTATUS(status));
            }
            *exited = true;
            return 0;
        }
        if (pid_is_qemu(launcher)) return launcher;
        if (soulx_now_ms() - start >= PID_HANDOVER_MS) break;
        if (soulx_sleep_ms(100) != 0) break;
    }
    pid_t child = find_qemu_child(launcher);
    return child > 0 ? child : launcher;
}

// An engine process as seen when a stop or start began: the start time tells a recycled pid apart.
typedef struct {
    pid_t pid;
    uint64_t start_us;          // 0 = unknown
} EngineId;

static EngineId engine_id(pid_t pid) {
    EngineId id = { pid, 0 };
    struct proc_bsdinfo bi;
    if (pid > 0 && proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, (int)sizeof(bi)) == (int)sizeof(bi)) {
        id.start_us = (uint64_t)bi.pbi_start_tvsec * 1000000u + (uint64_t)bi.pbi_start_tvusec;
    }
    return id;
}

// The process still exists, also while it exits (it may then hold its sockets and memory for a few
// seconds); false once it is gone, a zombie, or its pid belongs to another process now.
static bool engine_present(const EngineId* id) {
    int status = 0;
    (void)waitpid(id->pid, &status, WNOHANG);          // reap it when it is our own child
    if (id->pid <= 0 || (kill(id->pid, 0) != 0 && errno == ESRCH)) return false;
    struct proc_bsdinfo bi;
    if (proc_pidinfo(id->pid, PROC_PIDTBSDINFO, 0, &bi, (int)sizeof(bi)) == (int)sizeof(bi)) {
        if (bi.pbi_status == SZOMB) return false;
        uint64_t start = (uint64_t)bi.pbi_start_tvsec * 1000000u + (uint64_t)bi.pbi_start_tvusec;
        if (id->start_us && start != id->start_us) return false;
    }
    return true;
}

static void signal_engine(pid_t pid, int sig) {
    if (pid > 1 && soulx_pid_is_engine(pid)) (void)kill(pid, sig);   // never signal a recycled pid
}

// connect() to a loopback port: 1 = something accepts connections, 0 = refused, -1 = unknown.
static int port_connect_probe(int family, int port) {
    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    struct sockaddr_storage ss;
    socklen_t len;
    memset(&ss, 0, sizeof(ss));
    if (family == AF_INET) {
        struct sockaddr_in* a = (struct sockaddr_in*)&ss;
        a->sin_family = AF_INET;
        a->sin_port = htons((uint16_t)port);
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        len = sizeof(*a);
    } else {
        struct sockaddr_in6* a6 = (struct sockaddr_in6*)&ss;
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons((uint16_t)port);
        a6->sin6_addr = in6addr_loopback;
        len = sizeof(*a6);
    }
    int result = -1;
    if (connect(fd, (struct sockaddr*)&ss, len) == 0) {
        result = 1;
    } else if (errno == ECONNREFUSED) {
        result = 0;
    } else if (errno == EINPROGRESS) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        if (poll(&pfd, 1, PORT_PROBE_MS) > 0) {
            int err = 0;
            socklen_t el = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0) result = err == 0 ? 1 : (err == ECONNREFUSED ? 0 : -1);
        }
    }
    close(fd);
    return result;
}

// Waits until the engine is gone: first while it still runs as the engine (up to |timeout_ms|), then
// while the process tears down (up to STOP_EXITING_MS more; it cannot be signalled any more then), and
// finally until its console and adb ports stop accepting connections (up to STOP_PORTS_MS), so the
// next start can use the same port. 0 = gone, -1 = still running at the timeout, -2 = interrupted.
// With a |label| the status line "<label>… 12s" counts the seconds.
static int wait_engine_gone(const EngineId* id, int port, int timeout_ms, const char* label) {
    double start = soulx_now_ms();
    double exiting_since = 0;
    int shown = -1;
    int rc;
    for (;;) {
        double now = soulx_now_ms();
        if (!engine_present(id)) {
            rc = 0;
            break;
        }
        if (!soulx_pid_is_engine(id->pid)) {             // exiting: its executable is no longer mapped
            if (exiting_since == 0) exiting_since = now;
            if (now - exiting_since >= STOP_EXITING_MS) {
                soulx_log(SOULX_LOG_DEBUG, "engine pid %d is still exiting after %d s; going on", (int)id->pid,
                         STOP_EXITING_MS / 1000);
                rc = 0;
                break;
            }
        } else if (now - start >= timeout_ms) {
            rc = -1;
            break;
        }
        int secs = (int)((now - start) / 1000.0);
        if (label && secs != shown) {
            soulx_status_line("%s" ELLIPSIS " %ds", label, secs);
            shown = secs;
        }
        if (soulx_sleep_ms(100) != 0) {
            rc = -2;
            break;
        }
    }
    if (rc == 0 && port > 0) {
        double until = soulx_now_ms() + STOP_PORTS_MS;
        while ((port_connect_probe(AF_INET, port) == 1 || port_connect_probe(AF_INET, port + 1) == 1) &&
               soulx_now_ms() < until) {
            struct timespec ts = { 0, 50 * 1000000L };
            nanosleep(&ts, NULL);
        }
    }
    if (label) soulx_status_done();
    return rc;
}

// Clears the state file (and the run's markers) under the lock, unless it now describes another engine.
static void clear_state_if_pid(const char* name, pid_t pid) {
    int lock = soulx_lock_acquire();
    SoulXRunState now;
    if (soulx_state_load(name, &now) != 0 || now.pid == pid) {
        soulx_state_clear(name);
        markers_clear(name);
    }
    soulx_lock_release(lock);
}

// Registered instances whose engine is alive; -1 when the registry cannot be read.
static int running_instance_count(void) {
    int n = soulx_reg_list(NULL, 0);
    if (n <= 0) return n;
    SoulXInstanceConf* all = calloc((size_t)n, sizeof(*all));
    if (!all) return -1;
    n = soulx_reg_list(all, n);
    int running = n < 0 ? -1 : 0;
    for (int i = 0; i < n; i++) {
        if (soulx_instance_running(all[i].name, NULL)) running++;
    }
    free(all);
    return running;
}

// The engine starts SoulX's private adb server itself (-adb-path, port SOULX_ADB_SERVER_PORT) and that
// server outlives it, so the last instance to go takes it down: `adb kill-server` through soulx_adb(),
// i.e. SoulX's own adb in the SoulX environment - no other adb server is ever contacted. Runs under the
// SoulX lock, the same lock `soulx start` holds while it starts the server and records its engine, so a
// start that is under way is either seen as running here or restarts the server afterwards.
// Best effort: failures only warn. Called after every stop and by everything that may have seen the
// last engine go (a stop of an engine that had already exited, a failed start, the CLI).
void soulx_mac_cleanup_idle(void) {
    char adb[PATH_MAX];
    if (soulx_tool_path("adb", adb, sizeof(adb)) != 0 || access(adb, X_OK) != 0) return;   // no adb, no server

    int lock = soulx_lock_acquire();
    if (lock < 0) return;
    if (running_instance_count() == 0) {
        const char* args[] = { "kill-server", NULL };
        SoulXCmdResult res;
        int rc = soulx_adb(NULL, args, &res, ADB_KILL_SERVER_MS);
        if (rc == 0) {
            soulx_log(SOULX_LOG_DEBUG, "stopped SoulX's adb server (port %d): no instance is running", SOULX_ADB_SERVER_PORT);
        } else {
            char why[160] = "";
            if (res.err && res.err_len) {
                size_t len = strcspn(res.err, "\r\n");
                if (len >= sizeof(why)) len = sizeof(why) - 1;
                memcpy(why, res.err, len);
                why[len] = '\0';
            }
            soulx_log(SOULX_LOG_WARN, "could not stop SoulX's adb server (port %d): adb status %d%s%s",
                     SOULX_ADB_SERVER_PORT, rc, why[0] ? ": " : "", why);
        }
        soulx_cmd_result_free(&res);
    }
    soulx_lock_release(lock);
}

// Tells SoulX's adb server about a running engine with the smart-socket request the engine itself sends
// once at startup ("host:emulator:<adb port>"). SoulX's console ports are outside adb's scan range, so a
// server that was restarted meanwhile (soulx adb kill-server, a crash) would otherwise never see the
// engine again. The server ignores the request for an engine it already knows. When no server runs, one
// is started first. Best effort.
static void adb_register_engine(int console_port) {
    if (port_connect_probe(AF_INET, SOULX_ADB_SERVER_PORT) != 1 && soulx_adb_start_server() != 0) return;
    char req[64];
    char service[48];
    int n = snprintf(service, sizeof(service), "host:emulator:%d", console_port + 1);
    if (n <= 0 || (size_t)n >= sizeof(service)) return;
    int m = snprintf(req, sizeof(req), "%04x%s", (unsigned)n, service);
    if (m <= 0 || (size_t)m >= sizeof(req)) return;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    struct timeval tv = { 1, 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)SOULX_ADB_SERVER_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0 && send(fd, req, (size_t)m, 0) == (ssize_t)m) {
        soulx_log(SOULX_LOG_DEBUG, "registered emulator-%d with SoulX's adb server", console_port);
    }
    close(fd);
}

// ---------------------------------------------------------------------------
// Console port allocation
// ---------------------------------------------------------------------------

// One bind() of the loopback address; true when it worked (*err = errno otherwise).
static bool bind_probe(int family, int port, bool reuse, int* err) {
    *err = 0;
    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0) {
        *err = errno;
        return family == AF_INET6;                      // no IPv6 stack: nothing listens there
    }
    int one = 1;
    if (reuse) (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    bool ok;
    if (family == AF_INET) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ok = bind(fd, (struct sockaddr*)&a, sizeof(a)) == 0;
    } else {
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
        struct sockaddr_in6 a6;
        memset(&a6, 0, sizeof(a6));
        a6.sin6_family = AF_INET6;
        a6.sin6_port = htons((uint16_t)port);
        a6.sin6_addr = in6addr_loopback;
        ok = bind(fd, (struct sockaddr*)&a6, sizeof(a6)) == 0;
    }
    if (!ok) *err = errno;
    close(fd);
    return ok;
}

// Whether a new engine can use |port| on |family|'s loopback address. A strict bind (no SO_REUSEADDR)
// settles most cases. It also fails for the TIME_WAIT remnants an engine leaves behind for ~30 s after
// it exits, although the emulator (which binds with SO_REUSEADDR) can reuse such a port; two more probes
// tell them apart: a SO_REUSEADDR bind fails when a live socket owns exactly this address, and a
// connect() succeeds when something listens (e.g. on the wildcard address, which a SO_REUSEADDR bind on
// loopback does not detect).
static bool port_usable(int family, int port) {
    int err = 0;
    if (bind_probe(family, port, false, &err)) return true;
    if (err != EADDRINUSE) return family == AF_INET6;    // e.g. no ::1 configured: nothing can listen there
    if (!bind_probe(family, port, true, &err)) return false;
    return port_connect_probe(family, port) == 0;
}

static bool slot_usable(int port) {
    return port_usable(AF_INET, port) && port_usable(AF_INET, port + 1) &&
           port_usable(AF_INET6, port) && port_usable(AF_INET6, port + 1);
}

static bool console_port_valid(int port) {
    return port >= SOULX_PORT_FIRST && port <= SOULX_PORT_LAST && (port - SOULX_PORT_FIRST) % 2 == 0;
}

// Console port P (P + 1 = adb) for |self|: |preferred| (the port of its previous run, so its adb serial
// stays the same) when it is usable - waiting up to START_PORT_GRACE_MS for an engine that just died to
// let go of it - else the first usable even port that no other running SoulX instance has recorded.
// -1 if none.
static int alloc_port(const char* self, int preferred) {
    enum { SLOTS = (SOULX_PORT_LAST - SOULX_PORT_FIRST) / 2 + 1 };
    bool used[SLOTS] = { false };
    int n = soulx_reg_list(NULL, 0);
    if (n > 0) {
        SoulXInstanceConf* all = calloc((size_t)n, sizeof(*all));
        if (!all) return -1;
        n = soulx_reg_list(all, n);
        for (int i = 0; i < n; i++) {
            SoulXRunState st;
            if (!strcmp(all[i].name, self) || !soulx_instance_running(all[i].name, &st)) continue;
            int slot = (st.port - SOULX_PORT_FIRST) / 2;     // console port P (or its adb port P + 1)
            if (st.port >= SOULX_PORT_FIRST && slot < SLOTS) used[slot] = true;
        }
        free(all);
    }
    if (console_port_valid(preferred) && !used[(preferred - SOULX_PORT_FIRST) / 2]) {
        double until = soulx_now_ms() + START_PORT_GRACE_MS;
        for (;;) {
            if (slot_usable(preferred)) return preferred;
            if (soulx_now_ms() >= until || soulx_sleep_ms(250) != 0) break;
        }
    }
    for (int p = SOULX_PORT_FIRST; p <= SOULX_PORT_LAST; p += 2) {
        if (p == preferred || used[(p - SOULX_PORT_FIRST) / 2]) continue;
        if (slot_usable(p)) return p;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Instance lifecycle
// ---------------------------------------------------------------------------

// Renames avd/soulx_<name>.avd to avd/soulx_<name>.avd.orphan-<UTC time> (the engine only looks at
// directories named in an .ini file, so it ignores it) and drops the stale .ini. 0 ok.
static int move_aside_device_data(const char* name, const char* avd_dir) {
    char stamp[32], aside[PATH_MAX], ini[PATH_MAX], avd[SOULX_NAME_MAX + 8];
    time_t now = time(NULL);
    struct tm tmv;
    if (!gmtime_r(&now, &tmv) || strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv) == 0) {
        snprintf(stamp, sizeof(stamp), "%lld", (long long)now);
    }
    soulx_avd_name(name, avd, sizeof(avd));
    int n = snprintf(aside, sizeof(aside), "%s.orphan-%s", avd_dir, stamp);
    if (n < 0 || (size_t)n >= sizeof(aside) || soulx_path(ini, sizeof(ini), "avd/%s.ini", avd) != 0) {
        soulx_log(SOULX_LOG_ERROR, "SoulX home path too long: %s", soulx_home());
        return -1;
    }
    if (rename(avd_dir, aside) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot move the old device data of '%s' out of the way (%s): %s", name, avd_dir,
                 strerror(errno));
        return -1;
    }
    (void)unlink(ini);
    double gb = (double)soulx_dir_size(aside) / 1e9;
    soulx_log(SOULX_LOG_WARN, "found device data of an earlier '%s' without its instance file; kept it in %s "
             "(%.1f GB) " EM_DASH " delete that folder when you no longer need it", name, aside, gb);
    return 0;
}

int soulx_mac_create(SoulXInstanceConf* c) {
    if (!c) return -1;

    // landscape <=> width > height panel
    if (c->landscape && c->width < c->height) {
        int t = c->width;
        c->width = c->height;
        c->height = t;
    } else if (!c->landscape && c->width > c->height) {
        c->landscape = true;
    }
    if (conf_validate(c) != 0) return -1;
    if (soulx_ensure_layout() != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create the SoulX directories under %s", soulx_home());
        return -1;
    }

    char conf_file[PATH_MAX], avd_dir[PATH_MAX], avd[SOULX_NAME_MAX + 8];
    soulx_avd_name(c->name, avd, sizeof(avd));
    if (soulx_path(conf_file, sizeof(conf_file), "instances/%s.conf", c->name) != 0 ||
        soulx_path(avd_dir, sizeof(avd_dir), "avd/%s.avd", avd) != 0) {
        soulx_log(SOULX_LOG_ERROR, "SoulX home path too long: %s", soulx_home());
        return -1;
    }

    int lock = soulx_lock_acquire();
    if (lock < 0) return -1;
    int rc = -1;
    int id = 0;

    if (soulx_path_exists(conf_file)) {
        soulx_log(SOULX_LOG_ERROR, "instance '%s' already exists", c->name);
        goto out;
    }
    if (soulx_instance_running(c->name, NULL)) {
        soulx_log(SOULX_LOG_ERROR, "an engine for '%s' is still running; stop it first", c->name);
        goto out;
    }
    // Device data without an instance file (the file was removed by hand, a restore was partial, a
    // delete failed half-way): it may hold game progress, so it is moved aside, never deleted.
    if (soulx_path_exists(avd_dir) && move_aside_device_data(c->name, avd_dir) != 0) goto out;

    id = soulx_reg_next_id();
    if (id <= 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot read the instance registry");
        goto out;
    }
    c->id = id;
    iso_now(c->created, sizeof(c->created));
    if (soulx_reg_save(c) != 0) goto out;
    if (soulx_avd_write(c) != 0) {
        soulx_reg_delete(c->name);
        goto out;
    }
    rc = 0;

out:
    soulx_lock_release(lock);
    if (rc == 0 && !soulx_runtime_ready(c->image)) {
        const SoulXPackage* def = soulx_runtime_default_image();
        bool is_default = def && !strcmp(def->key, c->image);
        soulx_log(SOULX_LOG_WARN, "the Android runtime for '%s' is not installed yet " EM_DASH " run: soulx setup%s%s",
                 c->image, is_default ? "" : " --image ", is_default ? "" : c->image);
    }
    return rc;
}

int soulx_mac_reconfigure(SoulXInstanceConf* next, bool* needs_restart) {
    if (!next) return -1;
    if (needs_restart) *needs_restart = false;

    // landscape <=> width > height panel (the caller sets landscape; the panel follows it)
    if ((next->landscape && next->width < next->height) || (!next->landscape && next->width > next->height)) {
        int t = next->width;
        next->width = next->height;
        next->height = t;
    }
    if (conf_validate(next) != 0) return -1;

    int lock = soulx_lock_acquire();
    if (lock < 0) return -1;
    int rc = -1;
    SoulXInstanceConf cur;
    if (soulx_reg_load(next->name, &cur) != 0 || cur.id != next->id) {
        soulx_log(SOULX_LOG_ERROR, "no such instance '%s' (see: soulx list)", next->name);
        goto out;
    }
    // The userdata disk was formatted for this image and size: another image does not boot from it reliably,
    // and the engine grows the disk but never shrinks it.
    if (strcmp(cur.image, next->image) != 0) {
        soulx_log(SOULX_LOG_ERROR, "the system image of '%s' is fixed when it is created (it is '%s'); create a new "
                 "instance for '%s'", next->name, cur.image, next->image);
        goto out;
    }
    if (next->data_gb < cur.data_gb) {
        soulx_log(SOULX_LOG_ERROR, "storage of '%s' can only grow (it is %d GB)", next->name, cur.data_gb);
        goto out;
    }
    next->id = cur.id;
    memcpy(next->created, cur.created, sizeof(next->created));
    next->last_port = cur.last_port;
    bool running = soulx_instance_running(next->name, NULL);
    if (soulx_reg_save(next) != 0) goto out;
    // A running engine keeps the config.ini it started with; soulx_mac_start rewrites it from the registry.
    if (!running && soulx_avd_write(next) != 0) goto out;
    if (needs_restart) *needs_restart = running;
    rc = 0;

out:
    soulx_lock_release(lock);
    return rc;
}

int soulx_mac_delete(const char* name) {
    SoulXInstanceConf c;
    if (load_conf(name, &c) != 0) return -1;
    if (soulx_instance_running(c.name, NULL)) {
        soulx_log(SOULX_LOG_ERROR, "instance '%s' is running " EM_DASH " stop it first: soulx stop %s", c.name, c.name);
        return -1;
    }

    int lock = soulx_lock_acquire();
    if (lock < 0) return -1;
    int rc = -1;
    SoulXInstanceConf fresh;
    if (soulx_reg_load(c.name, &fresh) != 0) {            // deleted by another soulx meanwhile
        soulx_log(SOULX_LOG_ERROR, "no such instance '%s' (see: soulx list)", c.name);
        goto out;
    }
    c = fresh;
    if (soulx_instance_running(c.name, NULL)) {           // started meanwhile
        soulx_log(SOULX_LOG_ERROR, "instance '%s' is running " EM_DASH " stop it first: soulx stop %s", c.name, c.name);
        goto out;
    }
    // The instance file goes last: when the device data cannot be removed completely, the instance stays
    // listed so that the delete can be retried (and its disk use stays visible).
    if (soulx_avd_remove(&c) != 0) {
        soulx_log(SOULX_LOG_ERROR, "'%s' was not deleted because some of its device data could not be removed; "
                 "fix the problem above and run: soulx delete %s", c.name, c.name);
        goto out;
    }
    if (soulx_reg_delete(c.name) != 0) goto out;
    rc = 0;
    char path[PATH_MAX];
    if (log_path_for(c.name, path, sizeof(path)) == 0) remove_logs(path);
    if (window_log_path_for(c.name, path, sizeof(path)) == 0) remove_logs(path);
    soulx_state_clear(c.name);
    markers_clear(c.name);
    track_remove(g_booted, c.name);
    track_remove(g_paused, c.name);

out:
    soulx_lock_release(lock);
    return rc;
}

// The post-boot setup of this engine run, unless it was applied already (by whoever saw the boot finish
// first: a waiting start, soulx_wait_boot, soulx play or any later command). 0 when done.
static int post_boot_once(const char* name, const SoulXRunState* st) {
    if (marker_has(name, MARKER_POSTBOOT, st->pid, NULL)) return 0;
    if (soulx_mac_post_boot(name, st) != 0) return -1;
    marker_set(name, MARKER_POSTBOOT, st->pid);
    return 0;
}

// Waits for boot, runs the post-boot tweaks and reports failures with the log tail.
static int finish_boot(const SoulXInstanceConf* c, SoulXRunState* st, int timeout_s, SoulXRunState* st_out) {
    char serial[32], log_path[PATH_MAX];
    soulx_serial(st->port, serial, sizeof(serial));
    int rc = soulx_adb_wait_boot(serial, st->pid, timeout_s > 0 ? timeout_s : BOOT_TIMEOUT_DEFAULT_S);

    SoulXRunState now;
    bool running = soulx_instance_running(c->name, &now);   // the pid may have been repaired meanwhile
    if (running) *st = now;
    if (st_out) {
        if (running) *st_out = *st;
        else memset(st_out, 0, sizeof(*st_out));
    }

    if (rc != 0) {
        if (soulx_interrupted()) {
            if (running) soulx_log(SOULX_LOG_WARN, "'%s' keeps booting in the background (check: soulx status %s)", c->name, c->name);
        } else {
            if (log_path_for(c->name, log_path, sizeof(log_path)) == 0) print_log_tail(log_path, LOG_TAIL_LINES);
            if (running) soulx_log(SOULX_LOG_INFO, "the engine is still running; stop it with: soulx stop %s", c->name);
        }
        if (!running) {
            soulx_mac_cleanup_idle();                     // the engine died while booting; its adb server did not
            if (!soulx_interrupted() && log_bad_snapshot(c->name)) {
                clear_quickboot_snapshot(c->name, "it was incompatible");   // self-heal: corrupt/incompatible
                soulx_log(SOULX_LOG_ERROR, "run 'soulx start %s' again to boot fresh", c->name);
            }
        }
        return -1;
    }
    track_add(g_booted, c->name, st->pid);
    post_boot_once(c->name, st);                         // warns on failure; the device is usable anyway
    return 0;
}

static void log_argv(const char* const* argv) {
    if (!soulx_is_verbose()) return;
    char line[2048];
    size_t used = 0;
    line[0] = '\0';
    for (size_t i = 0; argv[i]; i++) {
        int n = snprintf(line + used, sizeof(line) - used, "%s%s", i ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(line) - used) break;
        used += (size_t)n;
    }
    soulx_log(SOULX_LOG_DEBUG, "exec: %s", line);
}

// soulx_mac_start() for an instance whose engine already runs: make sure SoulX's adb server knows it,
// then (o->wait) wait for the boot like a fresh start would.
static int start_already_running(const SoulXInstanceConf* c, SoulXRunState* st, const SoulXStartOpts* o,
                                 SoulXRunState* st_out) {
    char serial[32];
    soulx_serial(st->port, serial, sizeof(serial));
    soulx_log(SOULX_LOG_DEBUG, "'%s' is already running (%s)", c->name, serial);
    adb_register_engine(st->port);
    if (st_out) *st_out = *st;
    return o->wait ? finish_boot(c, st, o->timeout_s, st_out) : 0;
}

// An earlier stop got the console to accept "kill": that engine saves its Quick Boot snapshot and exits
// by itself. A start waits for it instead of treating the dying engine as running. 0 = it is gone.
static int wait_for_earlier_stop(const char* name, const SoulXRunState* st, time_t since) {
    soulx_log(SOULX_LOG_INFO, "'%s' is still shutting down; waiting for it to exit first", name);
    EngineId id = engine_id(st->pid);
    int w = wait_engine_gone(&id, st->port, stop_remaining_ms(since), "Shutting down");
    if (w == -2) return -1;
    if (w != 0) {
        soulx_log(SOULX_LOG_ERROR, "'%s' is still shutting down; try again later, or force it: soulx stop %s --force",
                 name, name);
        return -1;
    }
    clear_state_if_pid(name, st->pid);
    return 0;
}

int soulx_mac_start(const char* name, const SoulXStartOpts* opts, SoulXRunState* st_out) {
    SoulXStartOpts o;
    memset(&o, 0, sizeof(o));
    if (opts) o = *opts;
    if (st_out) memset(st_out, 0, sizeof(*st_out));

    SoulXInstanceConf c;
    if (load_conf(name, &c) != 0) return -1;
    const SoulXPackage* img = image_of(&c);
    if (!img) {
        soulx_log(SOULX_LOG_ERROR, "instance '%s' uses an unknown system image '%s'", c.name, c.image);
        return -1;
    }
    if (!soulx_runtime_ready(c.image)) {
        soulx_log(SOULX_LOG_ERROR, "the Android runtime for '%s' (%s) is not installed " EM_DASH " run: soulx setup --image %s",
                 c.name, android_name(&c), c.image);
        return -1;
    }

    SoulXRunState st;
    char serial[32];
    if (soulx_instance_running(c.name, &st)) {
        time_t since = 0;
        if (!marker_has(c.name, MARKER_STOPPING, st.pid, &since)) return start_already_running(&c, &st, &o, st_out);
        if (wait_for_earlier_stop(c.name, &st, since) != 0) return -1;
    }

    if (soulx_ensure_layout() != 0) {                     // $S/avd must exist or the engine falls back to ~/.android
        soulx_log(SOULX_LOG_ERROR, "cannot create the SoulX directories under %s", soulx_home());
        return -1;
    }
    // cwd under $SOULX_HOME: some console commands (e.g. `screenrecord screenshot` without
    // a directory) write files into the engine's working directory.
    char emulator[PATH_MAX], adb[PATH_MAX], log_path[PATH_MAX], window_log[PATH_MAX], work_dir[PATH_MAX];
    if (soulx_tool_path("emulator", emulator, sizeof(emulator)) != 0 || soulx_tool_path("adb", adb, sizeof(adb)) != 0 ||
        log_path_for(c.name, log_path, sizeof(log_path)) != 0 ||
        window_log_path_for(c.name, window_log, sizeof(window_log)) != 0 ||
        soulx_path(work_dir, sizeof(work_dir), "tmp") != 0) {
        soulx_log(SOULX_LOG_ERROR, "SoulX home path too long: %s", soulx_home());
        return -1;
    }

    int lock = soulx_lock_acquire();
    if (lock < 0) return -1;

    // The registry as it is now (a delete or a configuration change may have happened while this start
    // waited for the lock): never start an engine for an instance that no longer exists.
    SoulXInstanceConf fresh;
    if (soulx_reg_load(c.name, &fresh) != 0) {
        soulx_lock_release(lock);
        soulx_log(SOULX_LOG_ERROR, "no such instance '%s' (it was deleted meanwhile)", c.name);
        return -1;
    }
    c = fresh;
    if (soulx_instance_running(c.name, &st)) {            // started by another soulx meanwhile
        soulx_lock_release(lock);
        return start_already_running(&c, &st, &o, st_out);
    }
    if (soulx_avd_write(&c) != 0) {                       // keep config.ini in sync with the registry
        soulx_lock_release(lock);
        return -1;
    }
    int port = alloc_port(c.name, c.last_port);
    if (port < 0 || soulx_interrupted()) {
        soulx_lock_release(lock);
        if (port < 0) {
            soulx_log(SOULX_LOG_ERROR, "no free console port between %d and %d (too many emulators running?)",
                     SOULX_PORT_FIRST, SOULX_PORT_LAST);
        }
        return -1;
    }
    // The engine hands home/.android/adbkey.pub to the guest at boot; without it a
    // Google Play (user build) image would keep adb unauthorized.
    if (soulx_ensure_adb_key() != 0) {
        soulx_log(SOULX_LOG_WARN, "no adb key in %s/home/.android; adb may stay unauthorized", soulx_home());
    }
    if (soulx_adb_start_server() != 0) {
        soulx_lock_release(lock);
        return -1;
    }

    // GPU modes (engine -gpu, guest Vulkan, ANGLE platform):
    //   metal          swangle, no Vulkan, metal   OpenGL ES on the engine's ANGLE, straight on Metal
    //   host           host, Vulkan                Vulkan through MoltenVK, OpenGL ES through Apple's OpenGL
    //   host-novulkan  host, no Vulkan             OpenGL ES through Apple's OpenGL
    //   software       swiftshader_indirect        ANGLE on SwiftShader (CPU), as the engine picks it
    // metal leaves Vulkan off: the swangle mode would run it on SwiftShader, and games then use OpenGL ES. It is
    // experimental: ANGLE on Metal corrupts textures (icons and text with streaks, missing glyphs).
    // No mode uses Quick Boot. Restoring a snapshot's GPU state broke the guest's sync objects (thousands of
    // glClientWaitSync errors), and in that state the engine leaked without bound (30 GB in 11 minutes of a game
    // with gpu=host; after a cold boot the same game with auto play stayed flat at 3.5 GB); onto ANGLE it also
    // lost color buffers. Every start boots fresh (about 15 s), a stop saves nothing, no 4 GB snapshot is written.
    const char* gpu = c.gpu;
    if (!strcmp(gpu, "metal") && soulx_runtime_prepare_angle() != 0) {
        soulx_log(SOULX_LOG_WARN, "the Metal renderer is not available; '%s' uses gpu=host for this run", c.name);
        gpu = "host";
    }
    const char* engine_gpu = !strcmp(gpu, "metal") ? "swangle" : !strcmp(gpu, "software") ? "swiftshader_indirect"
                                                                                          : "host";
    bool no_vulkan = !strcmp(gpu, "metal") || !strcmp(gpu, "host-novulkan");
    bool quick_boot = false;
    if (!quick_boot) clear_quickboot_snapshot(c.name, NULL);   // never loaded again; frees its ~4 GB
    const char* const env[] = {
        !strcmp(gpu, "metal") ? SOULX_ANGLE_PLATFORM_ENV "=metal" : SOULX_ANGLE_PLATFORM_ENV "=vulkan", NULL };

    // The engine never opens a window of its own: SoulX's window shows the frames it shares
    // (-share-vid: shared memory "videmulator<port>") and sends the input.
    char avd[SOULX_NAME_MAX + 8], port_s[16];
    soulx_avd_name(c.name, avd, sizeof(avd));
    snprintf(port_s, sizeof(port_s), "%d", port);
    const char* argv[40];
    int n = 0;
    argv[n++] = emulator;
    argv[n++] = "-avd";
    argv[n++] = avd;
    argv[n++] = "-port";
    argv[n++] = port_s;
    argv[n++] = "-no-metrics";
    argv[n++] = "-gpu";
    argv[n++] = engine_gpu;
    argv[n++] = "-no-boot-anim";
    argv[n++] = "-adb-path";
    argv[n++] = adb;
    argv[n++] = "-no-window";
    argv[n++] = "-share-vid";
    if (c.dns[0] && dns_valid(c.dns)) {
        argv[n++] = "-dns-server";
        argv[n++] = c.dns;
    }
    if (!quick_boot || o.cold) argv[n++] = "-no-snapshot-load";
    if (!quick_boot) argv[n++] = "-no-snapshot-save";       // not -no-snapshot: with it the screen stays black
    if (no_vulkan) {
        argv[n++] = "-feature";
        argv[n++] = "-Vulkan";
    }
    if (!c.audio) argv[n++] = "-no-audio";                 // no audio device at all, not just a silent one
    // wlan0 runs on the engine's own WiFi, whose traffic goes through QEMU's "virtio-wifi" network backend: that is
    // where network_capture.c attaches a recording on demand. The default (netsim) WiFi lives in netsimd, which serves all
    // instances at once and cannot record a single radio while it runs. The guest sees the same "AndroidWifi" either
    // way. Listed before the diagnostic features below so that SOULX_ENGINE_FEATURES can still switch it back.
    argv[n++] = "-feature";
    argv[n++] = "-WiFiPacketStream";
    // Diagnostics: SOULX_ENGINE_FEATURES="-GLAsyncSwap,Foo" adds engine feature switches for this start.
    char features[256] = "";
    const char* fe = getenv("SOULX_ENGINE_FEATURES");
    if (fe && strlen(fe) < sizeof(features)) {
        memcpy(features, fe, strlen(fe) + 1);
        for (char* tok = strtok(features, ","); tok && n < 28; tok = strtok(NULL, ",")) {
            argv[n++] = "-feature";
            argv[n++] = tok;
        }
    }
    // QMP socket for network_capture.c. Last, because everything after -qemu goes to QEMU itself.
    char qmp[PATH_MAX + 32];
    if (soulx_mac_network_capture_qmp_arg(c.name, qmp, sizeof(qmp)) == 0) {
        argv[n++] = "-qemu";
        argv[n++] = "-qmp";
        argv[n++] = qmp;
    } else {
        soulx_log(SOULX_LOG_DEBUG, "'%s' gets no capture channel (its socket path is too long)", c.name);
    }
    argv[n] = NULL;
    log_argv(argv);

    rotate_log(log_path);
    rotate_log(window_log);                              // the window of this run logs into a fresh file
    markers_clear(c.name);                               // left by an earlier run
    SoulXCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.argv = argv;
    cmd.cwd = work_dir;
    cmd.soulx_env = true;
    cmd.env = env;
    pid_t launcher = soulx_spawn_detached(&cmd, log_path, true);
    if (launcher <= 0) {
        soulx_lock_release(lock);
        soulx_log(SOULX_LOG_ERROR, "could not launch %s", emulator);
        soulx_mac_cleanup_idle();                         // the adb server started above would outlive us
        return -1;
    }

    bool exited = false;
    pid_t pid = resolve_engine_pid(launcher, &exited);
    memset(&st, 0, sizeof(st));
    st.pid = exited ? launcher : pid;
    st.port = port;
    st.headless = o.headless;
    iso_now(st.started, sizeof(st.started));
    if (soulx_state_save(c.name, &st) != 0) {
        if (!exited) signal_engine(st.pid, SIGKILL);     // never leave an untracked engine behind
        soulx_lock_release(lock);
        soulx_mac_cleanup_idle();
        return -1;
    }
    if (exited && !soulx_instance_running(c.name, &st)) {
        soulx_lock_release(lock);
        soulx_log(SOULX_LOG_ERROR, "the emulator exited right after launch");
        print_log_tail(log_path, LOG_TAIL_LINES);
        soulx_mac_cleanup_idle();                         // the adb server started above would outlive the engine
        return -1;
    }
    int previous = c.last_port;
    if (previous != port) {                              // the next start tries this port first
        c.last_port = port;
        if (soulx_reg_save(&c) != 0) soulx_log(SOULX_LOG_DEBUG, "could not record the console port of '%s'", c.name);
    }
    soulx_lock_release(lock);

    soulx_serial(st.port, serial, sizeof(serial));
    if (console_port_valid(previous) && previous != port) {
        soulx_log(SOULX_LOG_INFO, "note: emulator-%d (the serial of its last run) is taken; '%s' uses %s this time",
                 previous, c.name, serial);
    }
    soulx_log(SOULX_LOG_INFO, "starting '%s' (%s) on %s%s", c.name,
             android_name(&c), serial, o.headless ? ", headless" : "");
    track_remove(g_paused, c.name);
    if (st_out) *st_out = st;
    if (!o.wait) return 0;
    return finish_boot(&c, &st, o.timeout_s, st_out);
}

// The console did not take "kill" and nothing shows a shutdown under way (an engine that is still
// starting has no console yet; a hung one accepts connections but never answers): give it
// STOP_NO_CONSOLE_WAIT_MS, retrying a console that refused connections and watching the log for a
// shutdown that started meanwhile (another stop got through, the window closed it). *saving is set when
// the engine turns out to be saving its snapshot; returns like wait_engine_gone().
static int wait_unresponsive(const char* name, const EngineId* id, int port, bool console_hung, bool* saving) {
    double start = soulx_now_ms();
    for (;;) {
        int w = wait_engine_gone(id, port, 1000, NULL);
        if (w != -1) return w;
        bool shutting_down = log_shows_shutdown(name);
        if (!shutting_down && !console_hung && port_connect_probe(AF_INET, port) == 1) {
            char reply[256];
            double t = soulx_now_ms();
            if (soulx_console_oneshot(port, "kill", reply, sizeof(reply)) == 0) {
                marker_set(name, MARKER_STOPPING, id->pid);
                shutting_down = true;
            } else if (soulx_now_ms() - t > 2000) {
                return -1;                              // accepts connections but never answers: hung
            }
        }
        if (shutting_down) {
            *saving = true;
            soulx_log(SOULX_LOG_INFO, "'%s' is shutting down (up to %d s)", name,
                     STOP_CONSOLE_WAIT_MS / 1000);
            return wait_engine_gone(id, port, STOP_CONSOLE_WAIT_MS, "Shutting down");
        }
        if (soulx_now_ms() - start >= STOP_NO_CONSOLE_WAIT_MS) return -1;
    }
}

int soulx_mac_stop(const char* name, bool force) {
    SoulXInstanceConf c;
    if (load_conf(name, &c) != 0) return -1;
    SoulXRunState st;
    if (!soulx_instance_running(c.name, &st)) {
        soulx_log(SOULX_LOG_INFO, "'%s' is not running", c.name);
        soulx_mac_cleanup_idle();                         // its engine may have exited by itself; adb did not
        return 0;
    }
    if (st.network_capture_path[0]) {                                // close the capture file properly first
        soulx_mac_network_capture_stop(c.name, NULL, 0);
    }

    EngineId id = engine_id(st.pid);
    char label[96];
    snprintf(label, sizeof(label), "Stopping %s", c.name);
    double t0 = soulx_now_ms();
    bool saving = false;                                 // the engine saves its snapshot and exits by itself
    int w;
    if (force) {
        soulx_log(SOULX_LOG_WARN, "force-stopping '%s': its quick-boot snapshot will not be saved", c.name);
        signal_engine(st.pid, SIGKILL);
        w = wait_engine_gone(&id, st.port, STOP_KILL_WAIT_MS, NULL);
    } else {
        time_t since = 0;
        bool console_hung = false;
        if (marker_has(c.name, MARKER_STOPPING, st.pid, &since)) {
            // An earlier stop (interrupted, from the window, a restart) got the console to accept "kill":
            // signalling the engine now would cut its snapshot save short, so only wait for it.
            soulx_log(SOULX_LOG_INFO, "'%s' is already shutting down; waiting for it to exit", c.name);
            saving = true;
        } else {
            // Console "kill" (reply "OK: killing emulator, bye bye"): the engine now saves its Quick Boot
            // snapshot, which takes 6-12 s and much longer after a long uptime, and exits by itself.
            char reply[256];
            double asked = soulx_now_ms();
            int rc = soulx_console_oneshot(st.port, "kill", reply, sizeof(reply));
            console_hung = rc != 0 && soulx_now_ms() - asked > 2000;   // connected, but no answer in time
            if (rc == 0) {
                marker_set(c.name, MARKER_STOPPING, st.pid);
                since = time(NULL);
                saving = true;
                soulx_log(SOULX_LOG_INFO, "'%s' is shutting down (up to %d s)",
                         c.name, STOP_CONSOLE_WAIT_MS / 1000);
            } else {
                soulx_log(SOULX_LOG_DEBUG, "console 'kill' on port %d failed%s%s", st.port,
                         rc == 1 ? ": " : "", rc == 1 ? reply : "");
                if (log_shows_shutdown(c.name)) {         // its console is busy because it saves already
                    since = time(NULL);
                    saving = true;
                    soulx_log(SOULX_LOG_INFO, "'%s' is already shutting down; waiting for it to exit", c.name);
                }
            }
        }
        if (saving) {
            w = wait_engine_gone(&id, st.port, stop_remaining_ms(since), "Shutting down");
        } else {
            w = wait_unresponsive(c.name, &id, st.port, console_hung, &saving);
        }
        if (w == -1) {
            if (saving) {
                soulx_log(SOULX_LOG_WARN, "'%s' did not finish its shutdown within %d s; sending SIGTERM", c.name,
                         STOP_CONSOLE_WAIT_MS / 1000);
            } else {
                soulx_log(SOULX_LOG_WARN, "'%s' does not answer on its console; sending SIGTERM", c.name);
            }
            signal_engine(st.pid, SIGTERM);
            w = wait_engine_gone(&id, st.port, STOP_TERM_WAIT_MS, label);
            if (w == -1 && !saving && log_shows_shutdown(c.name)) {
                // SIGTERM made it shut down properly: let it finish saving instead of killing it
                saving = true;
                w = wait_engine_gone(&id, st.port, STOP_CONSOLE_WAIT_MS, "Shutting down");
            }
        }
        if (w == -1) {
            soulx_log(SOULX_LOG_WARN, "'%s' ignored SIGTERM; killing it (no quick-boot snapshot)", c.name);
            signal_engine(st.pid, SIGKILL);
            w = wait_engine_gone(&id, st.port, STOP_KILL_WAIT_MS, NULL);
        }
    }
    if (w == -2) {
        if (saving) {
            soulx_log(SOULX_LOG_WARN, "interrupted: '%s' keeps saving its Quick Boot snapshot in the background "
                     "(soulx stop %s waits for it)", c.name, c.name);
        } else {
            soulx_log(SOULX_LOG_WARN, "interrupted: '%s' may still be running (check: soulx status %s)", c.name, c.name);
        }
        return -1;
    }
    if (w != 0) {
        soulx_log(SOULX_LOG_ERROR, "could not stop '%s' (engine pid %d)", c.name, (int)st.pid);
        return -1;
    }
    clear_state_if_pid(c.name, st.pid);
    track_remove(g_booted, c.name);
    track_remove(g_paused, c.name);

    // What happened to the snapshot, from the engine's own log line ("Saving snapshot '...' took N ms").
    char log_path[PATH_MAX];
    if (log_path_for(c.name, log_path, sizeof(log_path)) != 0) snprintf(log_path, sizeof(log_path), "logs/%s.log", c.name);
    // (no Quick Boot snapshot to report: SoulX starts every engine with -no-snapshot-save)
    double took = soulx_now_ms() - t0;
    if (took >= STOP_SLOW_MS) {
        soulx_log(SOULX_LOG_INFO, "stopping '%s' took %d s (engine log: %s)", c.name, (int)(took / 1000.0), log_path);
    }
    soulx_mac_cleanup_idle();                             // the last instance takes SoulX's adb server down
    return 0;
}

// lean: the preinstalled Google apps a game never needs. Together they held about 1 GB of the 4 GB guest
// (and woke up for background syncs). Disabled once per guest: the marker lives in the guest's own settings,
// so a wiped device gets it again and an app the user enables again stays enabled. Bump LEAN_VERSION when
// the list grows. Kept on purpose: Play Store, Play services, Chrome and WebView (sign-in, purchases), the
// keyboard, launcher, phone, contacts and accessibility apps.
static const char* const k_lean_packages[] = {
    "com.google.android.youtube",              "com.google.android.apps.youtube.music",
    "com.google.android.apps.photos",          "com.google.android.apps.messaging",
    "com.google.android.apps.bard",            "com.google.android.googlequicksearchbox",
    "com.google.android.apps.wellbeing",       "com.google.android.deskclock",
    "com.google.android.calendar",             "com.google.android.gm",
    "com.google.android.apps.maps",            "com.google.android.apps.docs",
    "com.google.android.tts",                  "com.google.android.as",
    "com.google.android.as.oss",               "com.google.android.apps.safetyhub",
    "com.google.android.apps.restore",         "com.google.android.apps.wallpaper",
    "com.google.android.projection.gearhead",  "com.google.android.glasses.companion",
    "com.google.android.glasses.core",
};
// Gesture navigation takes every drag that starts at a screen edge for itself (bottom: home, sides: back), and a
// mouse starts drags there all the time: the launcher got the gesture stolen halfway ("stealing input gesture"),
// so the app drawer stopped half open and the home screen flashed grey. Games keep the whole screen either way
// (they hide the system bars), so their coordinates do not change.
#define NAV_CMD                 "if [ \"$(settings get global soulx_nav)\" != 1 ]; then cmd overlay enable-exclusive " \
                                "--category com.android.internal.systemui.navbar.threebutton >/dev/null 2>&1; " \
                                "settings put global soulx_nav 1; fi"

#define LEAN_MARK               "soulx_lean"     // Settings.Global key in the guest: the LEAN_VERSION applied
#define LEAN_VERSION            "1"
#define LEAN_MAX_CACHED         "16"            // cached background processes (Android's default: 32)

// The shell commands that apply (or undo) lean, for the post-boot adb call. 0 ok, -1 does not fit.
static int lean_cmd(bool lean, char* buf, size_t cap) {
    static const char apply_head[] = "if [ \"$(settings get global " LEAN_MARK ")\" != " LEAN_VERSION " ]; then for p in";
    static const char apply_tail[] = "; do pm disable-user --user 0 $p; done >/dev/null 2>&1; settings put global "
                                     LEAN_MARK " " LEAN_VERSION "; fi; device_config put activity_manager "
                                     "max_cached_processes " LEAN_MAX_CACHED;
    static const char undo_head[] = "if [ \"$(settings get global " LEAN_MARK ")\" != null ]; then for p in";
    static const char undo_tail[] = "; do pm enable --user 0 $p; done >/dev/null 2>&1; settings delete global "
                                    LEAN_MARK " >/dev/null; device_config delete activity_manager "
                                    "max_cached_processes >/dev/null; fi";
    int k = snprintf(buf, cap, "%s", lean ? apply_head : undo_head);
    if (k < 0 || (size_t)k >= cap) return -1;
    size_t n = (size_t)k;
    for (size_t i = 0; i < sizeof(k_lean_packages) / sizeof(k_lean_packages[0]); i++) {
        k = snprintf(buf + n, cap - n, " %s", k_lean_packages[i]);
        if (k < 0 || (size_t)k >= cap - n) return -1;
        n += (size_t)k;
    }
    k = snprintf(buf + n, cap - n, "%s", lean ? apply_tail : undo_tail);
    return k < 0 || (size_t)k >= cap - n ? -1 : 0;
}

int soulx_mac_post_boot(const char* name, const SoulXRunState* st) {
    if (!st) return -1;
    SoulXInstanceConf c;
    if (load_conf(name, &c) != 0) return -1;

    // One adb round trip for every tweak. gpu=metal turns Android's window blurs off: ANGLE on Metal draws the
    // blur pass (app drawer, notification shade, recents) black, so the whole screen went black or flickered.
    // Every guest gets three-button navigation once (NAV_CMD), and keeps whatever its user chooses later.
    char lean[2048], cmd[2560];
    if (lean_cmd(c.lean, lean, sizeof(lean)) != 0) return -1;
    int n = snprintf(cmd, sizeof(cmd), "%s%s%s%s%s%s%s",
                     c.dns[0] ? "settings put global private_dns_mode off" : "",
                     c.dns[0] && c.stay_awake ? "; " : "",
                     c.stay_awake ? "svc power stayon true" : "",
                     c.dns[0] || c.stay_awake ? "; " : "",
                     !strcmp(c.gpu, "metal") ? "settings put global disable_window_blurs 1; " : "", NAV_CMD "; ", lean);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) return -1;

    char serial[32];
    soulx_serial(st->port, serial, sizeof(serial));
    char* out = NULL;
    int rc = soulx_adb_shell(serial, cmd, &out, POST_BOOT_TIMEOUT_MS);
    free(out);
    if (rc != 0) {
        soulx_log(SOULX_LOG_WARN, "post-boot setup of '%s' failed (adb status %d)", c.name, rc);
        return -1;
    }
    return 0;
}

int soulx_mac_require_running(const char* name_or_id, SoulXInstanceConf* conf, SoulXRunState* st,
                             char* serial, size_t cap) {
    SoulXInstanceConf c;
    SoulXRunState s;
    if (soulx_resolve_instance(name_or_id, &c) != 0) {
        // the same hints as the CLI gives for a missing instance
        const char* env = getenv("SOULX_INSTANCE");
        const char* fallback = (env && *env) ? env : SOULX_DEFAULT_INSTANCE;
        const char* shown = (name_or_id && *name_or_id) ? name_or_id : fallback;
        if (!soulx_name_valid(shown) || all_digits(shown)) {
            soulx_log(SOULX_LOG_ERROR, "no such instance '%.64s' (see: soulx list)", shown);
        } else if (!strcmp(shown, fallback)) {
            soulx_log(SOULX_LOG_ERROR, "no such instance '%s' " EM_DASH " create it with: soulx create "
                     "(or just run: soulx play game.apk)", shown);
        } else {
            soulx_log(SOULX_LOG_ERROR, "no such instance '%s' " EM_DASH " create it with: soulx create %s", shown, shown);
        }
        return SOULX_ERR_NOT_FOUND;
    }
    if (!soulx_instance_running(c.name, &s)) {
        soulx_log(SOULX_LOG_ERROR, "instance '%s' is not running " EM_DASH " start it with: soulx start %s", c.name, c.name);
        return SOULX_ERR_NOT_RUNNING;
    }
    if (marker_has(c.name, MARKER_STOPPING, s.pid, NULL)) {
        soulx_log(SOULX_LOG_ERROR, "instance '%s' is shutting down (saving its Quick Boot snapshot) " EM_DASH " start it "
                 "again with: soulx start %s", c.name, c.name);
        return SOULX_ERR_NOT_RUNNING;
    }
    adb_register_engine(s.port);                         // SoulX's adb server may have been restarted meanwhile
    char ser[32];
    soulx_serial(s.port, ser, sizeof(ser));
    // Started without waiting (soulx start --no-wait): the post-boot setup is applied by the first
    // command that finds the boot finished.
    if (!marker_has(c.name, MARKER_POSTBOOT, s.pid, NULL)) {
        char val[16] = "";
        if (soulx_adb_getprop(ser, "sys.boot_completed", val, sizeof(val)) == 0 && val[0] == '1' &&
            (val[1] == '\0' || val[1] == '\n' || val[1] == '\r' || val[1] == ' ')) {
            track_add(g_booted, c.name, s.pid);
            (void)post_boot_once(c.name, &s);
        }
    }
    if (conf) *conf = c;
    if (st) *st = s;
    if (serial && cap) soulx_serial(s.port, serial, cap);
    return 0;
}

// ---------------------------------------------------------------------------
// Public API helpers
// ---------------------------------------------------------------------------

static const char* profile_key(PerformanceProfile p) {
    switch (p) {
    case PERF_PROFILE_ECO:         return "lite";
    case PERF_PROFILE_BALANCED:    return "balanced";
    case PERF_PROFILE_PERFORMANCE:
    case PERF_PROFILE_GAMING:      return "performance";
    default:                       return NULL;          // PERF_PROFILE_CUSTOM
    }
}

static PerformanceProfile profile_of(const SoulXInstanceConf* c) {
    static const struct { const char* key; PerformanceProfile p; } map[] = {
        { "lite", PERF_PROFILE_ECO },
        { "balanced", PERF_PROFILE_BALANCED },
        { "performance", PERF_PROFILE_PERFORMANCE },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        SoulXInstanceConf t = *c;
        if (soulx_conf_apply_profile(&t, map[i].key) == 0 && t.cores == c->cores && t.ram_mb == c->ram_mb &&
            t.data_gb == c->data_gb && t.width == c->width && t.height == c->height && t.dpi == c->dpi) {
            return map[i].p;
        }
    }
    return PERF_PROFILE_CUSTOM;
}

static int clamp_value(const char* what, int v, int lo, int hi) {
    if (v < lo || v > hi) {
        int fixed = v < lo ? lo : hi;
        soulx_log(SOULX_LOG_WARN, "%s %d is out of range (%d-%d); using %d", what, v, lo, hi, fixed);
        return fixed;
    }
    return v;
}

// Applies a public InstanceConfig on top of |c|; out-of-range values are clamped.
// Creating (|current| NULL): the profile, the image (from enable_root + android_version) and the GPU
// are always applied; fields <= 0 keep the profile's value.
// Updating (soulx_set_config, |current| = the stored instance): zero means "keep the current value",
// so a struct that sets one field changes only that field. Since the zero values of the enums are real
// choices, they are read as "keep" too: profile ECO (0) and CUSTOM keep the hardware (use
// soulx_set_performance_profile to switch to ECO), an empty android_version keeps the system image
// (enable_root only counts together with a version, as soulx_get_config returns it), and graphics
// OPENGL (0) keeps the GPU mode (SOFTWARE selects software rendering, any other value the Mac's GPU).
static void api_apply(const InstanceConfig* cfg, SoulXInstanceConf* c, const SoulXInstanceConf* current) {
    char version[sizeof(cfg->android_version) + 1];
    memcpy(version, cfg->android_version, sizeof(cfg->android_version));
    version[sizeof(cfg->android_version)] = '\0';

    // Image: rootable (userdebug) "google" image, the 16 KB-page QPR1 image, or Google Play.
    if (!current || version[0]) {
        const char* image = "play";
        if (cfg->enable_root) {
            image = "google";
            if (!strncmp(version, "37.1", 4)) {
                soulx_log(SOULX_LOG_WARN, "no rootable Android 17 QPR1 image exists; using the Android 17 Google APIs image");
            }
        } else if (!strncmp(version, "37.1", 4)) {
            image = "play16k";
        }
        snprintf(c->image, sizeof(c->image), "%s", image);
    }

    const char* pk = profile_key(cfg->profile);
    if (current && (cfg->profile == PERF_PROFILE_ECO || cfg->profile == profile_of(current))) pk = NULL;
    if (pk) soulx_conf_apply_profile(c, pk);
    if (cfg->cpu_cores > 0) c->cores = cfg->cpu_cores;
    if (cfg->ram_mb > 0) c->ram_mb = cfg->ram_mb;
    if (cfg->storage_gb > 0) c->data_gb = cfg->storage_gb;
    if (cfg->width > 0 && cfg->height > 0) {
        c->width = cfg->width;
        c->height = cfg->height;
    }
    if (cfg->dpi > 0) c->dpi = cfg->dpi;

    c->cores = clamp_value("cpu_cores", c->cores, ENGINE_CORES_MIN, ENGINE_CORES_MAX);
    c->ram_mb = clamp_value("ram_mb", c->ram_mb, ENGINE_RAM_MIN, ENGINE_RAM_MAX);
    c->data_gb = clamp_value("storage_gb", c->data_gb, ENGINE_DATA_MIN, ENGINE_DATA_MAX);
    c->width = clamp_value("width", c->width, ENGINE_DIM_MIN, ENGINE_DIM_MAX);
    c->height = clamp_value("height", c->height, ENGINE_DIM_MIN, ENGINE_DIM_MAX);
    c->dpi = clamp_value("dpi", c->dpi, ENGINE_DPI_MIN, ENGINE_DPI_MAX);
    c->landscape = c->width > c->height;

    // Graphics: only software vs. hardware is expressible; keep host-novulkan and metal.
    if (cfg->graphics == GRAPHICS_BACKEND_SOFTWARE) {
        snprintf(c->gpu, sizeof(c->gpu), "software");
    } else if (current && cfg->graphics == GRAPHICS_BACKEND_OPENGL) {
        // keep the current mode
    } else if (strcmp(c->gpu, "host-novulkan") != 0 && strcmp(c->gpu, "metal") != 0) {
        snprintf(c->gpu, sizeof(c->gpu), "host");
    }
}

static void conf_to_api(const SoulXInstanceConf* c, InstanceConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->name, sizeof(cfg->name), "%s", c->name);
    const SoulXPackage* img = image_of(c);
    const char* target = img && img->target ? img->target : "";
    if (!strncmp(target, "android-", 8)) target += 8;
    snprintf(cfg->android_version, sizeof(cfg->android_version), "%s", target);
    cfg->cpu_cores = c->cores;
    cfg->ram_mb = c->ram_mb;
    cfg->storage_gb = c->data_gb;
    cfg->width = c->width;
    cfg->height = c->height;
    cfg->dpi = c->dpi;
    cfg->graphics = strcmp(c->gpu, "software") == 0 ? GRAPHICS_BACKEND_SOFTWARE : GRAPHICS_BACKEND_OPENGL;
    cfg->network = NETWORK_MODE_NAT;
    cfg->profile = profile_of(c);
    cfg->enable_root = strcmp(c->image, "google") == 0;
    cfg->enable_adb = true;
}

static bool instance_booted(const char* name, const SoulXRunState* st) {
    if (track_has(g_booted, name, st->pid)) return true;
    char serial[32], val[8];
    soulx_serial(st->port, serial, sizeof(serial));
    if (soulx_adb_getprop(serial, "sys.boot_completed", val, sizeof(val)) == 0 && !strcmp(val, "1")) {
        track_add(g_booted, name, st->pid);
        return true;
    }
    return false;
}

static void fill_info(const SoulXInstanceConf* c, InstanceInfo* info) {
    memset(info, 0, sizeof(*info));
    info->id = c->id;
    conf_to_api(c, &info->config);
    info->state = INSTANCE_STATE_STOPPED;

    SoulXRunState st;
    if (!soulx_instance_running(c->name, &st)) return;
    info->pid = (uint64_t)st.pid;
    info->console_port = st.port;
    info->adb_port = st.port + 1;
    soulx_serial(st.port, info->serial, sizeof(info->serial));
    snprintf(info->ip_address, sizeof(info->ip_address), "127.0.0.1");

    time_t started;
    time_t now = time(NULL);
    if (iso_parse(st.started, &started) && now > started) info->uptime_seconds = (uint64_t)(now - started);

    struct rusage_info_v2 ri;
    if (proc_pid_rusage(st.pid, RUSAGE_INFO_V2, (rusage_info_t*)&ri) == 0) {
        info->ram_usage_mb = ri.ri_phys_footprint / (1024u * 1024u);
    }

    if (marker_has(c->name, MARKER_STOPPING, st.pid, NULL)) {
        info->state = INSTANCE_STATE_STOPPING;          // saving its snapshot; exits by itself
    } else if (track_has(g_paused, c->name, st.pid)) {
        info->state = INSTANCE_STATE_PAUSED;
    } else {
        info->state = instance_booted(c->name, &st) ? INSTANCE_STATE_RUNNING : INSTANCE_STATE_STARTING;
    }
}

// Saves |next| (already validated); NEEDS_RESTART while the engine runs, since
// config.ini is only rewritten while stopped (soulx_mac_start rewrites it).
static int api_save(const SoulXInstanceConf* next) {
    if (conf_validate(next) != 0) return SOULX_ERROR;
    int lock = soulx_lock_acquire();
    if (lock < 0) return SOULX_ERROR;
    int rc = SOULX_ERROR;
    bool running = soulx_instance_running(next->name, NULL);
    if (soulx_reg_save(next) == 0) {
        if (running) rc = SOULX_ERR_NEEDS_RESTART;
        else rc = soulx_avd_write(next) == 0 ? SOULX_OK : SOULX_ERROR;
    }
    soulx_lock_release(lock);
    return rc;
}

static int console_avd_cmd(int port, const char* primary, const char* fallback) {
    char reply[256];
    int rc = soulx_console_oneshot(port, primary, reply, sizeof(reply));
    if (rc == 1 && fallback) rc = soulx_console_oneshot(port, fallback, reply, sizeof(reply));
    if (rc != 0) {
        soulx_log(SOULX_LOG_ERROR, "console command '%s' failed%s%s", primary, rc == 1 ? ": " : "", rc == 1 ? reply : "");
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Public API: core
// ---------------------------------------------------------------------------

SOULX_API int soulx_init(void) {
    // Prepares $SOULX_HOME; the adb server is started on demand by soulx_start_instance.
    if (soulx_ensure_layout() != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create the SoulX directories under %s", soulx_home());
        return SOULX_ERROR;
    }
    soulx_log(SOULX_LOG_DEBUG, "SoulX Android %s (macOS) initialized in %s", SOULX_VERSION_STRING, soulx_home());
    return SOULX_OK;
}

SOULX_API void soulx_shutdown(void) {
    // Instances are independent engine processes and keep running.
    pthread_mutex_lock(&g_track_mu);
    memset(g_booted, 0, sizeof(g_booted));
    memset(g_paused, 0, sizeof(g_paused));
    pthread_mutex_unlock(&g_track_mu);
}

SOULX_API const char* soulx_get_version(void) {
    return SOULX_VERSION_STRING;
}

// ---------------------------------------------------------------------------
// Public API: instance management
// ---------------------------------------------------------------------------

SOULX_API int soulx_create_instance(const InstanceConfig* config) {
    if (!config) return SOULX_ERROR;
    char raw[sizeof(config->name) + 1];
    memcpy(raw, config->name, sizeof(config->name));
    raw[sizeof(config->name)] = '\0';
    char name[SOULX_NAME_MAX + 1];
    slugify(raw, name, sizeof(name));

    SoulXInstanceConf c;
    soulx_conf_defaults(&c, name);
    api_apply(config, &c, NULL);
    if (soulx_mac_create(&c) != 0) return SOULX_ERROR;
    soulx_log(SOULX_LOG_DEBUG, "created instance '%s' (id %d)", c.name, c.id);
    return c.id;
}

SOULX_API int soulx_delete_instance(int instance_id) {
    SoulXInstanceConf c;
    if (soulx_reg_find_by_id(instance_id, &c) != 0) return SOULX_ERR_NOT_FOUND;
    if (soulx_instance_running(c.name, NULL)) {          // v1.0 semantics: stop, then delete
        notify_state(c.id, INSTANCE_STATE_STOPPING);
        if (soulx_mac_stop(c.name, false) != 0) {
            notify_state(c.id, INSTANCE_STATE_ERROR);
            return SOULX_ERROR;
        }
        notify_state(c.id, INSTANCE_STATE_STOPPED);
    }
    return soulx_mac_delete(c.name) == 0 ? SOULX_OK : SOULX_ERROR;
}

SOULX_API int soulx_start_instance(int instance_id) {
    SoulXInstanceConf c;
    if (soulx_reg_find_by_id(instance_id, &c) != 0) return SOULX_ERR_NOT_FOUND;
    notify_state(c.id, INSTANCE_STATE_STARTING);
    SoulXStartOpts o;
    memset(&o, 0, sizeof(o));
    o.wait = true;
    if (soulx_mac_start(c.name, &o, NULL) != 0) {
        notify_state(c.id, INSTANCE_STATE_ERROR);
        return SOULX_ERROR;
    }
    notify_state(c.id, INSTANCE_STATE_RUNNING);
    return SOULX_OK;
}

SOULX_API int soulx_stop_instance(int instance_id) {
    SoulXInstanceConf c;
    if (soulx_reg_find_by_id(instance_id, &c) != 0) return SOULX_ERR_NOT_FOUND;
    if (!soulx_instance_running(c.name, NULL)) return SOULX_OK;
    notify_state(c.id, INSTANCE_STATE_STOPPING);
    if (soulx_mac_stop(c.name, false) != 0) {
        notify_state(c.id, INSTANCE_STATE_ERROR);
        return SOULX_ERROR;
    }
    notify_state(c.id, INSTANCE_STATE_STOPPED);
    return SOULX_OK;
}

SOULX_API int soulx_pause_instance(int instance_id) {
    SoulXInstanceConf c;
    SoulXRunState st;
    if (soulx_reg_find_by_id(instance_id, &c) != 0) return SOULX_ERR_NOT_FOUND;
    if (!soulx_instance_running(c.name, &st)) return SOULX_ERR_NOT_RUNNING;
    if (console_avd_cmd(st.port, "avd pause", "avd stop") != 0) return SOULX_ERROR;
    track_add(g_paused, c.name, st.pid);
    notify_state(c.id, INSTANCE_STATE_PAUSED);
    return SOULX_OK;
}

SOULX_API int soulx_resume_instance(int instance_id) {
    SoulXInstanceConf c;
    SoulXRunState st;
    if (soulx_reg_find_by_id(instance_id, &c) != 0) return SOULX_ERR_NOT_FOUND;
    if (!soulx_instance_running(c.name, &st)) return SOULX_ERR_NOT_RUNNING;
    if (console_avd_cmd(st.port, "avd resume", "avd start") != 0) return SOULX_ERROR;
    track_remove(g_paused, c.name);
    notify_state(c.id, INSTANCE_STATE_RUNNING);
    return SOULX_OK;
}

SOULX_API int soulx_restart_instance(int instance_id) {
    int rc = soulx_stop_instance(instance_id);
    return rc == SOULX_OK ? soulx_start_instance(instance_id) : rc;
}

// Clones the configuration only: the new instance gets a fresh device (userdata,
// installed apps, accounts and snapshots are NOT copied), so the source may run.
SOULX_API int soulx_clone_instance(int source_id, const char* new_name) {
    if (!new_name || !*new_name) return SOULX_ERROR;
    SoulXInstanceConf src;
    if (soulx_reg_find_by_id(source_id, &src) != 0) return SOULX_ERR_NOT_FOUND;

    SoulXInstanceConf c = src;
    slugify(new_name, c.name, sizeof(c.name));
    c.id = 0;
    c.created[0] = '\0';
    if (soulx_mac_create(&c) != 0) return SOULX_ERROR;
    return c.id;
}

SOULX_API int soulx_get_instance_count(void) {
    int n = soulx_reg_list(NULL, 0);
    return n < 0 ? SOULX_ERROR : n;
}

SOULX_API int soulx_get_instance_info(int instance_id, InstanceInfo* info) {
    if (!info) return SOULX_ERROR;
    SoulXInstanceConf c;
    if (soulx_reg_find_by_id(instance_id, &c) != 0) return SOULX_ERR_NOT_FOUND;
    fill_info(&c, info);
    return SOULX_OK;
}

SOULX_API int soulx_list_instances(InstanceInfo* instances, int max_count) {
    if (!instances || max_count <= 0) return SOULX_ERROR;
    int total = soulx_reg_list(NULL, 0);
    if (total < 0) return SOULX_ERROR;
    int k = total < max_count ? total : max_count;
    if (k == 0) return 0;
    SoulXInstanceConf* all = calloc((size_t)k, sizeof(*all));
    if (!all) return SOULX_ERROR;
    k = soulx_reg_list(all, k);
    for (int i = 0; i < k; i++) fill_info(&all[i], &instances[i]);
    free(all);
    return k;
}

// ---------------------------------------------------------------------------
// Public API: configuration
// ---------------------------------------------------------------------------

SOULX_API int soulx_set_config(int instance_id, const InstanceConfig* config) {
    if (!config) return SOULX_ERROR;
    SoulXInstanceConf c;
    if (soulx_reg_find_by_id(instance_id, &c) != 0) return SOULX_ERR_NOT_FOUND;

    SoulXInstanceConf next = c;
    api_apply(config, &next, &c);
    if (next.data_gb < c.data_gb) {
        soulx_log(SOULX_LOG_WARN, "the data partition of '%s' cannot shrink; keeping %d GB", c.name, c.data_gb);
        next.data_gb = c.data_gb;
    }
    char raw[sizeof(config->name) + 1], slug[SOULX_NAME_MAX + 1];
    memcpy(raw, config->name, sizeof(config->name));
    raw[sizeof(config->name)] = '\0';
    if (raw[0]) {
        slugify(raw, slug, sizeof(slug));
        if (strcmp(slug, c.name) != 0) {
            soulx_log(SOULX_LOG_WARN, "instances cannot be renamed; keeping the name '%s'", c.name);
        }
    }
    return api_save(&next);
}

SOULX_API int soulx_get_config(int instance_id, InstanceConfig* config) {
    if (!config) return SOULX_ERROR;
    SoulXInstanceConf c;
    if (soulx_reg_find_by_id(instance_id, &c) != 0) return SOULX_ERR_NOT_FOUND;
    conf_to_api(&c, config);
    return SOULX_OK;
}

SOULX_API int soulx_set_performance_profile(int instance_id, PerformanceProfile profile) {
    SoulXInstanceConf c;
    if (soulx_reg_find_by_id(instance_id, &c) != 0) return SOULX_ERR_NOT_FOUND;
    if (profile == PERF_PROFILE_CUSTOM) return SOULX_OK;  // custom values stay as they are
    const char* pk = profile_key(profile);
    if (!pk) return SOULX_ERROR;

    SoulXInstanceConf next = c;
    soulx_conf_apply_profile(&next, pk);                  // keeps the panel orientation
    if (next.data_gb < c.data_gb) next.data_gb = c.data_gb;   // userdata never shrinks
    return api_save(&next);
}

// ---------------------------------------------------------------------------
// Public API: callbacks
// ---------------------------------------------------------------------------

// Stored for API compatibility (as in v1.0): the macOS backend runs no background
// sampler; use soulx_get_performance() to poll metrics.
SOULX_API void soulx_set_performance_callback(PerformanceCallback callback) {
    atomic_store(&g_perf_cb, callback);
}

// Invoked synchronously, on the calling thread, by the lifecycle functions above.
SOULX_API void soulx_set_state_callback(InstanceStateCallback callback) {
    atomic_store(&g_state_cb, callback);
}

SOULX_API void soulx_set_log_callback(LogCallback callback) {
    atomic_store(&g_log_cb, callback);
}

// Called by log.c for every message it emits (log.c also prints to stderr).
// Only forwards to the registered callback; never prints.
void soulx_mac_log_dispatch(int level, const char* message) {
    LogCallback cb = atomic_load(&g_log_cb);
    if (!cb || !message || t_in_log_cb) return;
    t_in_log_cb = true;
    cb(level, message);
    t_in_log_cb = false;
}

// ---------------------------------------------------------------------------
// Public API: extended
// ---------------------------------------------------------------------------

SOULX_API int soulx_find_instance(const char* name) {
    if (!name || !*name) return SOULX_ERR_NOT_FOUND;
    SoulXInstanceConf c;
    if (soulx_resolve_instance(name, &c) == 0) return c.id;
    char slug[SOULX_NAME_MAX + 1];                        // names given to soulx_create_instance are slugified
    slugify(name, slug, sizeof(slug));
    if (soulx_reg_load(slug, &c) == 0) return c.id;
    return SOULX_ERR_NOT_FOUND;
}

SOULX_API int soulx_wait_boot(int instance_id, int timeout_s) {
    SoulXInstanceConf c;
    SoulXRunState st;
    if (soulx_reg_find_by_id(instance_id, &c) != 0) return SOULX_ERR_NOT_FOUND;
    if (!soulx_instance_running(c.name, &st)) return SOULX_ERR_NOT_RUNNING;
    char serial[32];
    soulx_serial(st.port, serial, sizeof(serial));
    if (soulx_adb_wait_boot(serial, st.pid, timeout_s > 0 ? timeout_s : BOOT_TIMEOUT_DEFAULT_S) != 0) return SOULX_ERROR;
    if (soulx_instance_running(c.name, &st)) {
        track_add(g_booted, c.name, st.pid);
        (void)post_boot_once(c.name, &st);               // also for instances started without waiting
    }
    return SOULX_OK;
}
