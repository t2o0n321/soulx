/*
 * SoulX Android Emulator - macOS adb client
 *
 * Always runs SoulX's own adb ($SOULX_HOME/sdk/platform-tools/adb) with the SoulX
 * isolation environment, so it talks to the private adb server on port 5137
 * and uses the keys in $SOULX_HOME/home/.android. Devices are addressed by
 * their emulator serial ("emulator-<console port>").
 *
 * When adb reports that it cannot see a device ("device 'emulator-5600' not found", "device
 * offline") although its SoulX instance is running - e.g. right after the adb server restarted -
 * the command waits for the device (`adb wait-for-device`, at most 15 s) and runs once more.
 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "soulx_mac.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define ADB_MAX_ARGS            4096
#define ADB_EXEC_OUT_MAX        (128u * 1024u * 1024u)   // binary-safe capture cap
#define ADB_GETPROP_TIMEOUT_MS  5000
#define ADB_BOOT_POLL_TIMEOUT_MS 2000                    // per getprop while booting
#define ADB_SERVER_TIMEOUT_MS   30000
#define ADB_BOOT_DEFAULT_S      300
#define ADB_WAIT_DEVICE_MS      15000                    // device lost while its instance runs: wait so long
#define ADB_LOST_HOLDOFF_MS     15000                    // ... but not again this soon after a wait failed
#define ADB_STATE_TIMEOUT_MS    5000                     // adb get-state before a passthrough command

// adb_run() flags
#define ADB_INTERRUPTIBLE       0x1u                     // Ctrl+C ends adb (SoulXCmd.interruptible)
#define ADB_NO_WAIT             0x2u                     // never wait for a device adb cannot see

#define EM_DASH                 "\xE2\x80\x94"     // UTF-8 em dash
#define ELLIPSIS                "\xE2\x80\xA6"     // UTF-8 horizontal ellipsis

// Exported for soulx_lua.c and input.c (not part of soulx_mac.h).
int soulx_adb_interruptible(const char* serial, const char* const* args, SoulXCmdResult* res, int timeout_ms);
                                                // soulx_adb() that Ctrl+C ends (res->status 130)
int soulx_adb_shell_nowait(const char* serial, const char* shell_cmd, char** out, int timeout_ms);
                                                // soulx_adb_shell() that fails at once when adb cannot
                                                // see the device (for frequent background polls)

static pthread_mutex_t g_lost_lock = PTHREAD_MUTEX_INITIALIZER;
static char   g_lost_serial[65];                // the last serial a wait-for-device gave up on
static double g_lost_ms;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Serials SoulX hands to adb: "emulator-5554", "127.0.0.1:5555", ...
static bool serial_valid(const char* s) {
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

static bool prop_valid(const char* p) {
    if (!p || !*p) return false;
    size_t n = strnlen(p, 129);
    if (n > 128) return false;
    for (size_t i = 0; i < n; i++) {
        char ch = p[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                  ch == '.' || ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

// Trims trailing whitespace in place.
static void rtrim(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
}

// Logs adb's stderr (first line only) at debug level.
static void log_adb_error(const char* what, const SoulXCmdResult* res) {
    if (!res->err || res->err_len == 0) return;
    char line[256];
    size_t n = strcspn(res->err, "\r\n");
    if (n >= sizeof(line)) n = sizeof(line) - 1;
    memcpy(line, res->err, n);
    line[n] = '\0';
    soulx_log(SOULX_LOG_DEBUG, "adb %s: %s", what, line);
}

// Runs: adb [-s serial] [sub] args... (once)
static int adb_exec(const char* serial, const char* sub, const char* const* args,
                    SoulXCmdResult* res, int timeout_ms, size_t max_output, unsigned flags) {
    memset(res, 0, sizeof(*res));
    res->status = -1;

    if (serial && !serial_valid(serial)) {
        soulx_log(SOULX_LOG_ERROR, "invalid device serial");
        return -1;
    }

    char adb[PATH_MAX];
    if (soulx_tool_path("adb", adb, sizeof(adb)) != 0 || access(adb, X_OK) != 0) {
        soulx_log(SOULX_LOG_ERROR, "SoulX's adb is not installed " EM_DASH " run: soulx setup");
        return -1;
    }

    size_t nargs = 0;
    while (args && args[nargs]) {
        if (++nargs > ADB_MAX_ARGS) {
            soulx_log(SOULX_LOG_ERROR, "too many adb arguments");
            return -1;
        }
    }

    size_t total = 1 + (serial ? 2 : 0) + (sub ? 1 : 0) + nargs + 1;
    const char** argv = calloc(total, sizeof(*argv));
    if (!argv) return -1;
    size_t k = 0;
    argv[k++] = adb;
    if (serial) {
        argv[k++] = "-s";
        argv[k++] = serial;
    }
    if (sub) argv[k++] = sub;
    for (size_t i = 0; i < nargs; i++) argv[k++] = args[i];
    argv[k] = NULL;

    SoulXCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.argv = argv;
    cmd.soulx_env = true;
    cmd.timeout_ms = timeout_ms;
    cmd.max_output = max_output;
    cmd.interruptible = (flags & ADB_INTERRUPTIBLE) != 0;

    int rc = soulx_run(&cmd, res);
    free(argv);
    if (res->timed_out) soulx_log(SOULX_LOG_DEBUG, "adb %s timed out after %d ms", sub ? sub : (nargs ? args[0] : ""), timeout_ms);
    return rc;
}

// adb's own complaint that it cannot see the device, e.g. "adb: device 'emulator-5600' not found",
// "error: device offline" (a line of its stderr; the command's own output is told apart later).
static bool device_missing(const SoulXCmdResult* res) {
    for (const char* line = res->err; line && *line; ) {
        size_t n = strcspn(line, "\r\n");
        char l[256];
        size_t k = n < sizeof(l) - 1 ? n : sizeof(l) - 1;
        memcpy(l, line, k);
        l[k] = '\0';
        if ((strncmp(l, "adb: ", 5) == 0 || strncmp(l, "error: ", 7) == 0) &&
            ((strstr(l, "device") && (strstr(l, "not found") || strstr(l, "offline"))) ||
             strstr(l, "no devices/emulators found"))) {
            return true;
        }
        line += n;
        while (*line == '\r' || *line == '\n') line++;
    }
    return false;
}

// Port of an "emulator-<port>" serial, 0 for any other serial.
static int serial_port(const char* serial) {
    const char* prefix = "emulator-";
    if (!serial || strncmp(serial, prefix, strlen(prefix)) != 0) return 0;
    char* end = NULL;
    long port = strtol(serial + strlen(prefix), &end, 10);
    if (!end || *end != '\0' || port <= 0 || port > 65535) return 0;
    return (int)port;
}

// The started SoulX instance whose engine has |serial| (its run state names that port): its name and
// its configuration in |conf_out| (may be NULL). false when there is none.
static bool serial_instance(const char* serial, char* inst, size_t cap, SoulXInstanceConf* conf_out) {
    if (cap) inst[0] = '\0';
    int port = serial_port(serial);
    if (port <= 0) return false;
    int n = soulx_reg_list(NULL, 0);
    if (n <= 0) return false;
    SoulXInstanceConf* all = calloc((size_t)n, sizeof(*all));
    if (!all) return false;
    n = soulx_reg_list(all, n);
    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        SoulXRunState st;
        if (soulx_state_load(all[i].name, &st) != 0 || st.port != port) continue;
        snprintf(inst, cap, "%s", all[i].name);
        if (conf_out) *conf_out = all[i];
        found = true;
    }
    free(all);
    return found;
}

// adb does not see |serial| although its instance is running: true (and the instance's name in
// |inst|) when the instance runs on that port and `adb get-state` does not say "device".
static bool device_lost(const char* serial, char* inst, size_t cap) {
    SoulXRunState st;
    if (!serial_instance(serial, inst, cap, NULL) || !soulx_instance_running(inst, &st) ||
        st.port != serial_port(serial)) {
        return false;
    }
    const char* args[] = { "get-state", NULL };
    SoulXCmdResult res;
    int rc = adb_exec(serial, NULL, args, &res, ADB_STATE_TIMEOUT_MS, 4096, 0);
    bool visible = rc == 0 && res.out && strncmp(res.out, "device", 6) == 0 &&
                   (res.out[6] == '\0' || res.out[6] == '\n' || res.out[6] == '\r');
    soulx_cmd_result_free(&res);
    return !visible && !soulx_interrupted();
}

// Waits (at most ADB_WAIT_DEVICE_MS) until adb sees |serial| again. Not again within
// ADB_LOST_HOLDOFF_MS after a wait for the same device gave up, so that a series of commands fails fast.
static bool wait_for_lost_device(const char* serial, const char* inst) {
    pthread_mutex_lock(&g_lost_lock);
    bool holdoff = strcmp(g_lost_serial, serial) == 0 && soulx_now_ms() - g_lost_ms < ADB_LOST_HOLDOFF_MS;
    pthread_mutex_unlock(&g_lost_lock);
    if (holdoff) return false;

    soulx_log(SOULX_LOG_DEBUG, "adb does not see %s although '%s' is running; waiting for it (up to %d s)",
             serial, inst, ADB_WAIT_DEVICE_MS / 1000);
    const char* args[] = { "wait-for-device", NULL };
    SoulXCmdResult res;
    int rc = adb_exec(serial, NULL, args, &res, ADB_WAIT_DEVICE_MS, 4096, ADB_INTERRUPTIBLE);
    bool back = rc == 0 && !res.timed_out;
    soulx_cmd_result_free(&res);

    pthread_mutex_lock(&g_lost_lock);
    if (back) {
        if (strcmp(g_lost_serial, serial) == 0) g_lost_serial[0] = '\0';
    } else {
        snprintf(g_lost_serial, sizeof(g_lost_serial), "%s", serial);
        g_lost_ms = soulx_now_ms();
    }
    pthread_mutex_unlock(&g_lost_lock);
    if (!back && !soulx_interrupted()) {
        soulx_log(SOULX_LOG_ERROR, "SoulX's adb cannot see '%s' (%s) although it is running (was the adb server "
                 "restarted?) " EM_DASH " try again in a few seconds, or restart it: soulx restart %s",
                 inst, serial, inst);
    }
    return back;
}

// adb_exec(); when adb cannot see the device of a running instance, waits for it and runs the
// command once more (unless ADB_NO_WAIT). A command that failed that way never reached the device.
static int adb_run(const char* serial, const char* sub, const char* const* args,
                   SoulXCmdResult* res, int timeout_ms, size_t max_output, unsigned flags) {
    int rc = adb_exec(serial, sub, args, res, timeout_ms, max_output, flags);
    if (!serial || (flags & ADB_NO_WAIT) || rc <= 0 || res->timed_out || soulx_interrupted() ||
        !device_missing(res)) {
        return rc;
    }
    char inst[SOULX_NAME_MAX + 1];
    if (!device_lost(serial, inst, sizeof(inst)) || !wait_for_lost_device(serial, inst)) return rc;
    soulx_log(SOULX_LOG_DEBUG, "adb sees %s again; running the command once more", serial);
    soulx_cmd_result_free(res);
    return adb_exec(serial, sub, args, res, timeout_ms, max_output, flags);
}

static int adb_getprop_timeout(const char* serial, const char* prop, char* buf, size_t cap, int timeout_ms,
                               unsigned flags) {
    if (!buf || cap == 0) return -1;
    buf[0] = '\0';
    if (!serial || !prop_valid(prop)) return -1;

    const char* args[] = { "getprop", prop, NULL };
    SoulXCmdResult res;
    int rc = adb_run(serial, "shell", args, &res, timeout_ms, 4096, flags);
    if (rc == 0 && res.out) {
        rtrim(res.out);
        snprintf(buf, cap, "%s", res.out);
    } else {
        log_adb_error("getprop", &res);
    }
    soulx_cmd_result_free(&res);
    return rc == 0 ? 0 : -1;
}

// "<name> (Android 17)" when |serial| belongs to a started SoulX instance (and
// that instance's name in |inst|), else the serial itself.
static void boot_label(const char* serial, char* label, size_t cap, char* inst, size_t inst_cap) {
    snprintf(label, cap, "%s", serial);
    SoulXInstanceConf conf;
    if (!serial_instance(serial, inst, inst_cap, &conf)) return;
    const SoulXPackage* img = soulx_runtime_find(conf.image);
    snprintf(label, cap, "%s (%s)", conf.name, img && img->android_name ? img->android_name : "Android");
}

// Reaps |pid| if it is our exited child, then checks that it is still an engine.
static bool engine_alive(pid_t pid) {
    int status = 0;
    (void)waitpid(pid, &status, WNOHANG);
    return soulx_pid_alive(pid) && soulx_pid_is_engine(pid);
}

// ---------------------------------------------------------------------------
// Public functions
// ---------------------------------------------------------------------------

void soulx_serial(int port, char* buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (snprintf(buf, cap, "emulator-%d", port) >= (int)cap) buf[0] = '\0';
}

int soulx_adb_start_server(void) {
    const char* args[] = { "start-server", NULL };
    SoulXCmdResult res;
    int rc = adb_run(NULL, NULL, args, &res, ADB_SERVER_TIMEOUT_MS, 0, 0);
    if (rc != 0) {
        char line[256] = "";
        if (res.err && res.err_len) {
            size_t n = strcspn(res.err, "\r\n");
            if (n >= sizeof(line)) n = sizeof(line) - 1;
            memcpy(line, res.err, n);
            line[n] = '\0';
        }
        soulx_log(SOULX_LOG_ERROR, "could not start SoulX's adb server on port %d%s%s",
                 SOULX_ADB_SERVER_PORT, line[0] ? ": " : "", line);
    }
    soulx_cmd_result_free(&res);
    return rc == 0 ? 0 : -1;
}

// Returns adb's exit status (-1 when it could not run). |res| may be NULL.
int soulx_adb(const char* serial, const char* const* args, SoulXCmdResult* res, int timeout_ms) {
    SoulXCmdResult local;
    SoulXCmdResult* r = res ? res : &local;
    int rc = adb_run(serial, NULL, args, r, timeout_ms, 0, 0);
    if (!res) soulx_cmd_result_free(&local);
    return rc;
}

int soulx_adb_interruptible(const char* serial, const char* const* args, SoulXCmdResult* res, int timeout_ms) {
    SoulXCmdResult local;
    SoulXCmdResult* r = res ? res : &local;
    int rc = adb_run(serial, NULL, args, r, timeout_ms, 0, ADB_INTERRUPTIBLE);
    if (!res) soulx_cmd_result_free(&local);
    return rc;
}

int soulx_adb_passthrough(const char* serial, const char* const* args) {
    if (serial && !serial_valid(serial)) {
        soulx_log(SOULX_LOG_ERROR, "invalid device serial");
        return -1;
    }
    char adb[PATH_MAX];
    if (soulx_tool_path("adb", adb, sizeof(adb)) != 0 || access(adb, X_OK) != 0) {
        soulx_log(SOULX_LOG_ERROR, "SoulX's adb is not installed " EM_DASH " run: soulx setup");
        return -1;
    }
    size_t nargs = 0;
    while (args && args[nargs]) {
        if (++nargs > ADB_MAX_ARGS) return -1;
    }
    const char** argv = calloc(nargs + 4, sizeof(*argv));
    if (!argv) return -1;
    size_t k = 0;
    argv[k++] = adb;
    if (serial) {
        argv[k++] = "-s";
        argv[k++] = serial;
    }
    for (size_t i = 0; i < nargs; i++) argv[k++] = args[i];
    argv[k] = NULL;

    // adb's output goes straight to the terminal, so a lost device is checked first: when adb cannot
    // see the device of a running instance (adb server restarted, guest still booting), wait for it.
    char inst[SOULX_NAME_MAX + 1];
    if (serial && serial_port(serial) > 0 && device_lost(serial, inst, sizeof(inst))) {
        soulx_status_line("Waiting for adb to see '%s'" ELLIPSIS, inst);
        bool back = wait_for_lost_device(serial, inst);
        soulx_status_done();
        if (!back && soulx_interrupted()) {
            free(argv);
            return 130;
        }
    }

    SoulXCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.argv = argv;
    cmd.soulx_env = true;
    int rc = soulx_run_passthrough(&cmd);
    free(argv);
    return rc;
}

static int adb_shell_flags(const char* serial, const char* shell_cmd, char** out, int timeout_ms, unsigned flags) {
    if (out) *out = NULL;
    if (!serial || !shell_cmd || !*shell_cmd) return -1;

    // "--" keeps a command that starts with '-' from being parsed as an adb shell option.
    const char* args[] = { shell_cmd[0] == '-' ? "--" : shell_cmd, shell_cmd[0] == '-' ? shell_cmd : NULL, NULL };
    SoulXCmdResult res;
    int rc = adb_run(serial, "shell", args, &res, timeout_ms, 0, flags);
    if (rc != 0) log_adb_error("shell", &res);
    if (out) {
        *out = res.out;
        res.out = NULL;
        res.out_len = 0;
    }
    soulx_cmd_result_free(&res);
    return rc;
}

// adb -s <serial> shell <shell_cmd>: the command string is ONE argv element and
// is interpreted by the guest's shell. Returns the remote exit status.
int soulx_adb_shell(const char* serial, const char* shell_cmd, char** out, int timeout_ms) {
    return adb_shell_flags(serial, shell_cmd, out, timeout_ms, 0);
}

int soulx_adb_shell_nowait(const char* serial, const char* shell_cmd, char** out, int timeout_ms) {
    return adb_shell_flags(serial, shell_cmd, out, timeout_ms, ADB_NO_WAIT);
}

int soulx_adb_exec_out(const char* serial, const char* const* args, unsigned char** data, size_t* len, int timeout_ms) {
    if (data) *data = NULL;
    if (len) *len = 0;
    if (!serial || !data || !len) return -1;

    SoulXCmdResult res;
    int rc = adb_run(serial, "exec-out", args, &res, timeout_ms, ADB_EXEC_OUT_MAX, 0);
    if (rc == 0) {
        if (res.out_len >= ADB_EXEC_OUT_MAX) {
            soulx_log(SOULX_LOG_ERROR, "adb exec-out output exceeds %u MB", ADB_EXEC_OUT_MAX / (1024u * 1024u));
            rc = -1;
        } else {
            *data = (unsigned char*)res.out;
            *len = res.out_len;
            res.out = NULL;
            res.out_len = 0;
        }
    } else {
        log_adb_error("exec-out", &res);
    }
    soulx_cmd_result_free(&res);
    return rc;
}

int soulx_adb_getprop(const char* serial, const char* prop, char* buf, size_t cap) {
    return adb_getprop_timeout(serial, prop, buf, cap, ADB_GETPROP_TIMEOUT_MS, 0);
}

int soulx_adb_wait_boot(const char* serial, pid_t pid, int timeout_s) {
    if (!serial_valid(serial)) return -1;
    if (timeout_s <= 0) timeout_s = ADB_BOOT_DEFAULT_S;

    char label[128], inst[SOULX_NAME_MAX + 1];
    boot_label(serial, label, sizeof(label), inst, sizeof(inst));

    double start = soulx_now_ms();
    for (;;) {
        int secs = (int)((soulx_now_ms() - start) / 1000.0);

        if (pid > 0 && !engine_alive(pid)) {
            // A launcher may have handed over to a child: ask the registry.
            SoulXRunState st;
            if (inst[0] && soulx_instance_running(inst, &st) && st.pid != pid) {
                pid = st.pid;
                continue;
            }
            soulx_status_done();
            soulx_log(SOULX_LOG_ERROR, "the engine of %s exited while booting", label);
            return -1;
        }

        char val[16];
        // "device offline" is normal while booting: never wait for the device here
        if (adb_getprop_timeout(serial, "sys.boot_completed", val, sizeof(val), ADB_BOOT_POLL_TIMEOUT_MS,
                                ADB_NO_WAIT) == 0 &&
            strcmp(val, "1") == 0) {
            soulx_status_done();
            soulx_log(SOULX_LOG_DEBUG, "%s booted in %d s", label, secs);
            return 0;
        }

        if (soulx_now_ms() - start >= timeout_s * 1000.0) {
            soulx_status_done();
            soulx_log(SOULX_LOG_ERROR, "timed out after %d s waiting for %s to boot", timeout_s, label);
            return -1;
        }
        soulx_status_line("Booting %s" ELLIPSIS " %ds", label, secs);
        if (soulx_sleep_ms(1000) != 0) {
            soulx_status_done();
            soulx_log(SOULX_LOG_WARN, "stopped waiting for %s to boot (interrupted)", label);
            return -1;
        }
    }
}
