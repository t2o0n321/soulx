/*
 * SoulX Android Emulator - Lua 5.4 automation scripts (soulx run <script.lua>)
 *
 * soulx_script_run() runs one script against a running instance. A script is plain Lua 5.4 plus
 * a small set of global functions (reference: docs/SCRIPTING.md), e.g.
 *
 *     launch("com.example.game")
 *     wait("img/start.png", 30)
 *     tap("img/start.png")
 *     for i = 1, 50 do
 *         swipe(300, 1500, 800, 1500, 200)
 *         tap(540, 1650)
 *         sleep(800 + math.random(200))
 *         if color(1000, 80) == 0xFF2D2D then tap(980, 1700) end
 *     end
 *
 * Coordinates are logical screen pixels: the pixels of a screenshot in the current orientation.
 * Relative paths (templates, screenshots, install, require) are relative to the script's folder;
 * the process never changes its working directory.
 *
 * Design
 *  - Per-run state lives in a ScriptCtx stored in the lua_State's extra space, so scripts on
 *    different instances may run at the same time in different threads (device_api.c).
 *  - The device is reached only through a SoulXLuaDeviceOps table (input.c, apps.c, adb.c);
 *    tests/macos/test_lua.c installs a fake device with soulx_lua_set_device_ops().
 *  - Screen cache: color, is_color, find, find_all, exists and tap(image) reuse the last capture
 *    while it is younger than 100 ms and no input was sent since; capture() and screenshot()
 *    always capture. wait() and wait_gone() poll in rounds of at least 200 ms.
 *    Rule for the code below: no Lua allocation while a pointer into the cached screen is held
 *    (an allocation may run a script's __gc finalizer, which may capture again and free it).
 *  - Images are 'soulx.Image' userdata owning malloc'd RGBA pixels (freed by __gc / __close);
 *    the collector is told about that memory so that dropped captures are released promptly.
 *    Template files are loaded once per run (cached by absolute path, reloaded when the file
 *    changes on disk).
 *  - Ctrl+C: g_soulx_interrupted is checked by a count hook (every 1000 VM instructions) and by
 *    every API function; the script then stops with "interrupted" and exit code 130. pcall,
 *    xpcall and coroutine.resume re-raise the interruption, so a script cannot swallow it, and
 *    os.exit() ends the script, not the process. A blocking shell() is ended by Ctrl+C too.
 *    Closing the device releases held touches.
 *  - Scripts never start host processes or load native code: os.execute, io.popen,
 *    package.loadlib and the C module searchers (package.cpath) are removed (shell() runs a
 *    command inside Android through adb). They are trusted code otherwise: io and os can read
 *    and write the user's files.
 */

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "image.h"
#include "soulx_mac.h"

// ---------------------------------------------------------------------------
// Limits and defaults
// ---------------------------------------------------------------------------

#define SCRIPT_CACHE_MAX_AGE_MS     100.0           // reuse a capture younger than this
#define SCRIPT_POLL_MS              200.0           // wait()/wait_gone(): shortest polling round
#define SCRIPT_HOOK_COUNT           1000            // VM instructions between two Ctrl+C checks
#define SCRIPT_FIND_ALL_MAX         256             // most matches returned by find_all()
#define SCRIPT_TEMPLATES_MAX        128             // template files kept loaded per run
#define SCRIPT_TEMPLATES_BYTES      ((size_t)256 * 1024 * 1024)
#define SCRIPT_LONG_PRESS_MS        800
#define SCRIPT_SWIPE_MS             300
#define SCRIPT_GESTURE_MAX_MS       60000
#define SCRIPT_SLEEP_MAX_MS         86400000        // 24 h
#define SCRIPT_WAIT_DEFAULT_S       10.0
#define SCRIPT_WAIT_MAX_S           86400.0
#define SCRIPT_IS_COLOR_TOLERANCE   16
#define SCRIPT_SHELL_TIMEOUT_S      60.0
#define SCRIPT_SHELL_MAX_S          3600.0
#define SCRIPT_SHELL_CMD_MAX        8192
#define SCRIPT_CONSOLE_CMD_MAX      4000
#define SCRIPT_CONSOLE_REPLY        16384
#define SCRIPT_TEXT_MAX             1000
#define SCRIPT_KEY_MAX              64
#define SCRIPT_KEYCODE_MAX          999
#define SCRIPT_INSTANCE_REF_MAX     64
#define SCRIPT_ARGS_MAX             4096
#define SCRIPT_PACKAGE_MAX          255
#define SCRIPT_PM_TIMEOUT_MS        30000           // pm list packages (stop_app's installed check)
#define SCRIPT_COORD_LIMIT          1e6             // |coordinate| accepted before the screen check
#define SCRIPT_MSG_MAX              1024

#define IMAGE_MT                    "soulx.Image"
#define MEMORY_MT                   "soulx.Memory"
#define EM_DASH                     "\xE2\x80\x94"     // UTF-8 em dash

// ---------------------------------------------------------------------------
// Device layer (swappable for the unit tests)
// ---------------------------------------------------------------------------

// Everything a script does to the instance goes through this table. Functions return 0 on
// success. The layout is mirrored in tests/macos/test_lua.c: keep both in sync
// (soulx_lua_set_device_ops() rejects a table of a different size).
typedef struct {
    // Opens an instance (NULL or "" = $SOULX_INSTANCE or "default"): returns a device handle and
    // writes the resolved instance name to name, or writes a message to err and returns NULL.
    void* (*open)(const char* instance, char* name, size_t name_cap, char* err, size_t err_cap);
    void  (*close)(void* dev);                                       // releases held touches too
    int   (*screen_size)(void* dev, int* w, int* h);                 // logical (rotated) size
    int   (*capture)(void* dev, SoulXImage* img);                     // logical orientation, malloc'd
    int   (*touch)(void* dev, int finger, int action, int x, int y); // SOULX_TOUCH_DOWN/UP/MOVE
    int   (*tap)(void* dev, int x, int y);
    int   (*long_press)(void* dev, int x, int y, int ms);
    int   (*swipe)(void* dev, int x1, int y1, int x2, int y2, int ms);
    int   (*key)(void* dev, const char* name_or_code);
    int   (*text)(void* dev, const char* text);
    int   (*console)(void* dev, const char* cmd, char* reply, size_t cap);   // 0 OK, 1 KO, -1 I/O
    // 0 = the command ran (stdout in *out, malloc'd; exit status in *status),
    // -1 = it could not run (reason in err), -2 = timed out
    int   (*shell)(void* dev, const char* cmd, int timeout_ms, char** out, size_t* out_len, int* status,
                   char* err, size_t err_cap);
    int   (*launch)(void* dev, const char* package);
    int   (*stop_app)(void* dev, const char* package);               // 1 = the app is not installed
    int   (*install)(void* dev, const char* path, bool reinstall, char* package, size_t cap);
    int   (*current_app)(void* dev, char* package, size_t cap);      // -1 = unknown
} SoulXLuaDeviceOps;

// Exported for the unit tests (tests/macos/test_lua.c); not part of soulx_mac.h.
// ops = NULL restores the real device. Returns -1 when the table is incomplete or has another size.
int soulx_lua_set_device_ops(const SoulXLuaDeviceOps* ops, size_t ops_size);

// input.c and adb.c (exported there for this file; not part of soulx_mac.h)
int  soulx_input_keycode(const char* name_or_code);
void soulx_input_watch_rotation(SoulXInput* in);
int  soulx_adb_interruptible(const char* serial, const char* const* args, SoulXCmdResult* res, int timeout_ms);

typedef struct {
    SoulXInput* in;
    char serial[64];
} RealDevice;

static void* real_open(const char* instance, char* name, size_t name_cap, char* err, size_t err_cap) {
    SoulXInstanceConf conf;
    SoulXRunState st;
    if (soulx_resolve_instance(instance, &conf) != 0) {
        const char* shown = instance;
        if (!shown || !*shown) {
            const char* env = getenv("SOULX_INSTANCE");
            shown = (env && *env) ? env : SOULX_DEFAULT_INSTANCE;
        }
        snprintf(err, err_cap, "no such instance '%.64s' (see: soulx list)", shown);
        return NULL;
    }
    if (!soulx_instance_running(conf.name, &st)) {
        snprintf(err, err_cap, "instance '%s' is not running " EM_DASH " start it with: soulx start %s",
                 conf.name, conf.name);
        return NULL;
    }
    RealDevice* d = calloc(1, sizeof(*d));
    if (!d) {
        snprintf(err, err_cap, "out of memory");
        return NULL;
    }
    d->in = soulx_input_open(conf.name);
    if (!d->in) {
        free(d);
        snprintf(err, err_cap, "cannot connect to the emulator console of '%s'", conf.name);
        return NULL;
    }
    const char* serial = soulx_input_serial(d->in);
    int n = (serial && *serial) ? snprintf(d->serial, sizeof(d->serial), "%s", serial) : 0;
    if (!serial || !*serial) soulx_serial(st.port, d->serial, sizeof(d->serial));
    if (n < 0 || (size_t)n >= sizeof(d->serial) || !d->serial[0]) {
        soulx_input_close(d->in);
        free(d);
        snprintf(err, err_cap, "invalid device serial for '%s'", conf.name);
        return NULL;
    }
    snprintf(name, name_cap, "%s", conf.name);
    return d;
}

static void real_close(void* dev) {
    RealDevice* d = dev;
    if (!d) return;
    soulx_input_close(d->in);
    free(d);
}

static int real_screen_size(void* dev, int* w, int* h) {
    return soulx_input_screen_size(((RealDevice*)dev)->in, w, h);
}

static int real_capture(void* dev, SoulXImage* img) {
    return soulx_input_capture(((RealDevice*)dev)->in, img);
}

static int real_touch(void* dev, int finger, int action, int x, int y) {
    return soulx_input_touch(((RealDevice*)dev)->in, finger, action, x, y);
}

static int real_tap(void* dev, int x, int y) {
    return soulx_input_tap(((RealDevice*)dev)->in, x, y);
}

static int real_long_press(void* dev, int x, int y, int ms) {
    return soulx_input_long_press(((RealDevice*)dev)->in, x, y, ms);
}

static int real_swipe(void* dev, int x1, int y1, int x2, int y2, int ms) {
    return soulx_input_swipe(((RealDevice*)dev)->in, x1, y1, x2, y2, ms);
}

static int real_key(void* dev, const char* name_or_code) {
    return soulx_input_key(((RealDevice*)dev)->in, name_or_code);
}

static int real_text(void* dev, const char* text) {
    return soulx_input_text(((RealDevice*)dev)->in, text);
}

static int real_console(void* dev, const char* cmd, char* reply, size_t cap) {
    return soulx_input_console(((RealDevice*)dev)->in, cmd, reply, cap);
}

// First line of s (without CR/LF) into buf.
static void first_line(const char* s, char* buf, size_t cap) {
    if (cap == 0) return;
    buf[0] = '\0';
    if (!s) return;
    size_t n = strcspn(s, "\r\n");
    if (n >= cap) n = cap - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
}

// adb's own complaints, as opposed to output of the command that ran inside Android.
static bool adb_complaint(const char* line) {
    static const char* const prefixes[] = {
        "error: device", "error: no devices", "error: closed", "error: protocol fault",
        "error: cannot connect", "adb: device", "adb: error",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (strncmp(line, prefixes[i], strlen(prefixes[i])) == 0) return true;
    }
    return false;
}

static int real_shell(void* dev, const char* cmd, int timeout_ms, char** out, size_t* out_len, int* status,
                      char* err, size_t err_cap) {
    RealDevice* d = dev;
    const char* args[4];
    int k = 0;
    args[k++] = "shell";
    if (cmd[0] == '-') args[k++] = "--";       // not an adb option
    args[k++] = cmd;
    args[k] = NULL;

    SoulXCmdResult res;
    memset(&res, 0, sizeof(res));
    int rc = soulx_adb_interruptible(d->serial, args, &res, timeout_ms);  // Ctrl+C ends the wait
    soulx_input_watch_rotation(d->in);           // e.g. `settings put system user_rotation 1`
    if (res.timed_out) {
        soulx_cmd_result_free(&res);
        return -2;
    }
    first_line(res.err, err, err_cap);
    if (rc < 0 || res.status < 0 || (res.status != 0 && adb_complaint(err))) {
        if (!err[0]) snprintf(err, err_cap, "adb could not run");
        soulx_cmd_result_free(&res);
        return -1;
    }
    *out = res.out;
    *out_len = res.out_len;
    *status = res.status;
    res.out = NULL;
    res.out_len = 0;
    soulx_cmd_result_free(&res);
    return 0;
}

// The orientation may change after these (a landscape game starts or ends): input.c then re-reads it
// often for a few seconds, so coordinates follow the new orientation at once.
static int real_launch(void* dev, const char* package) {
    RealDevice* d = dev;
    int rc = soulx_launch_package(d->serial, package);
    soulx_input_watch_rotation(d->in);
    return rc;
}

// 1 = installed, 0 = not installed, -1 = cannot tell. `pm list packages <filter>` lists every
// package whose name contains the filter ("package:<name>" lines), like the CLI's check.
static int real_installed(const char* serial, const char* package) {
    char cmd[SCRIPT_PACKAGE_MAX + 32];
    int n = snprintf(cmd, sizeof(cmd), "pm list packages %s", package);
    if (n < 0 || (size_t)n >= sizeof(cmd)) return -1;
    char* out = NULL;
    if (soulx_adb_shell(serial, cmd, &out, SCRIPT_PM_TIMEOUT_MS) != 0 || !out) {
        free(out);
        return -1;
    }
    int found = 0;
    bool unexpected = false;
    size_t plen = strlen(package);
    for (const char* line = out; *line; ) {
        size_t len = strcspn(line, "\r\n");
        if (len >= 8 && strncmp(line, "package:", 8) == 0) {
            if (len == 8 + plen && strncmp(line + 8, package, plen) == 0) found = 1;
        } else if (len > 0) {
            unexpected = true;                  // e.g. an error message from pm
        }
        line += len;
        while (*line == '\r' || *line == '\n') line++;
    }
    free(out);
    return (!found && unexpected) ? -1 : found;
}

static int real_stop_app(void* dev, const char* package) {
    RealDevice* d = dev;
    int installed = real_installed(d->serial, package);
    if (installed == 0) return 1;
    int rc = soulx_force_stop(d->serial, package);
    soulx_input_watch_rotation(d->in);
    return rc;
}

static int real_install(void* dev, const char* path, bool reinstall, char* package, size_t cap) {
    RealDevice* d = dev;
    SoulXApkInfo info;
    int rc = soulx_install_path(d->serial, path, &info, reinstall);
    soulx_input_watch_rotation(d->in);
    if (rc != 0) return -1;
    snprintf(package, cap, "%s", info.package);
    return 0;
}

static int real_current_app(void* dev, char* package, size_t cap) {
    return soulx_foreground_package(((RealDevice*)dev)->serial, package, cap);
}

static const SoulXLuaDeviceOps k_real_ops = {
    .open = real_open,
    .close = real_close,
    .screen_size = real_screen_size,
    .capture = real_capture,
    .touch = real_touch,
    .tap = real_tap,
    .long_press = real_long_press,
    .swipe = real_swipe,
    .key = real_key,
    .text = real_text,
    .console = real_console,
    .shell = real_shell,
    .launch = real_launch,
    .stop_app = real_stop_app,
    .install = real_install,
    .current_app = real_current_app,
};

static _Atomic(const SoulXLuaDeviceOps*) g_device_ops;     // NULL = k_real_ops

static bool ops_complete(const SoulXLuaDeviceOps* o) {
    return o->open && o->close && o->screen_size && o->capture && o->touch && o->tap && o->long_press &&
           o->swipe && o->key && o->text && o->console && o->shell && o->launch && o->stop_app &&
           o->install && o->current_app;
}

int soulx_lua_set_device_ops(const SoulXLuaDeviceOps* ops, size_t ops_size) {
    if (ops && (ops_size != sizeof(*ops) || !ops_complete(ops))) return -1;
    atomic_store(&g_device_ops, ops);
    return 0;
}

// ---------------------------------------------------------------------------
// Per-run state
// ---------------------------------------------------------------------------

typedef enum {
    STOP_NONE = 0,
    STOP_INTERRUPT,                     // Ctrl+C (or soulx_macro_stop)
    STOP_EXIT                           // os.exit()
} StopReason;

typedef struct {
    const SoulXLuaDeviceOps* ops;
    void* dev;                          // NULL once the script has ended
    char instance[SOULX_NAME_MAX + 1];
    const char* script;                 // path as given: chunk name and args[0]
    char dir[PATH_MAX];                 // absolute folder of the script: base of relative paths
    int argc;
    char** argv;
    double t0;                          // soulx_now_ms() at start
    double speed;                       // playback speed: sleep()/replay pauses are divided by this (1.0 = normal)
    int pass;                           // current repeat pass, 1-based
    int passes;                         // total passes requested (0 = loop until stopped)

    SoulXImage screen;                   // screen cache (owned)
    double screen_ms;                   // when it was captured
    unsigned long captures;

    bool down[SOULX_MAX_FINGERS];        // touch_down() state per finger
    int fx[SOULX_MAX_FINGERS];
    int fy[SOULX_MAX_FINGERS];

    int templates;                      // entries of the template cache table
    size_t template_bytes;

    StopReason stop;
    int exit_code;                      // os.exit() code
    int status;                         // Lua status of the run (LUA_OK, LUA_ERRRUN, ...)
    char* message;                      // error message + traceback (malloc'd)
} ScriptCtx;

// 'soulx.Image' userdata
typedef struct {
    SoulXImage img;                      // malloc'd pixels; NULL once released
    bool file;                          // loaded from a file (template cache entry): identity below
    dev_t dev;
    ino_t ino;
    off_t size;
    struct timespec mtime;
    size_t bytes;                       // pixel bytes counted in the template cache
    bool warned;                        // "larger than the search area" warning shown
} LuaImage;

static const char k_templates = 0;     // registry key (by address) of the template cache table

static ScriptCtx* ctx_of(lua_State* L) {
    return *(ScriptCtx**)lua_getextraspace(L);
}

// ---------------------------------------------------------------------------
// Errors, Ctrl+C and reports
// ---------------------------------------------------------------------------

// The raising helpers return int only so that API functions can 'return script_error(...)'.
static _Noreturn int script_error(lua_State* L, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void script_warn(lua_State* L, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void report(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// luaL_error with printf formatting: raises "<chunk>:<line>: <message>".
static _Noreturn int script_error(lua_State* L, const char* fmt, ...) {
    char msg[SCRIPT_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) snprintf(msg, sizeof(msg), "error");
    luaL_where(L, 1);
    lua_pushstring(L, msg);
    lua_concat(L, 2);
    lua_error(L);
    abort();                                            // not reached: lua_error() does not return
}

// Warning with the script position; allocates nothing in Lua (safe while the screen is borrowed).
static void script_warn(lua_State* L, const char* fmt, ...) {
    char msg[SCRIPT_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    lua_Debug ar;
    if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar) && ar.currentline > 0) {
        soulx_log(SOULX_LOG_WARN, "%s:%d: %s", ar.short_src, ar.currentline, msg);
    } else {
        soulx_log(SOULX_LOG_WARN, "%s", msg);
    }
}

// Stops the script: "interrupted" (Ctrl+C) or "exit" (os.exit). The first reason wins.
static _Noreturn int raise_stop(lua_State* L, ScriptCtx* ctx, StopReason why) {
    if (ctx->stop == STOP_NONE) ctx->stop = why;
    lua_pushstring(L, ctx->stop == STOP_EXIT ? "exit" : "interrupted");
    lua_error(L);
    abort();                                            // not reached: lua_error() does not return
}

// Honours a pending Ctrl+C; first thing of every API function.
static ScriptCtx* api_check(lua_State* L) {
    ScriptCtx* ctx = ctx_of(L);
    if (ctx->stop != STOP_NONE || soulx_interrupted()) raise_stop(L, ctx, STOP_INTERRUPT);
    return ctx;
}

// api_check + the device must still be open (a finalizer may run after the script ended).
static ScriptCtx* api_enter(lua_State* L) {
    ScriptCtx* ctx = api_check(L);
    if (!ctx->dev) script_error(L, "the script has ended");
    return ctx;
}

// After a blocking device call: Ctrl+C wins over the call's own result.
static void after_device(lua_State* L, ScriptCtx* ctx) {
    if (soulx_interrupted()) raise_stop(L, ctx, STOP_INTERRUPT);
}

// Count hook: lets Ctrl+C stop pure Lua loops.
static void api_hook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    ScriptCtx* ctx = ctx_of(L);
    if (ctx->stop != STOP_NONE || soulx_interrupted()) raise_stop(L, ctx, STOP_INTERRUPT);
}

// Final messages go to stderr (stdout belongs to the script's print) and to a LogCallback.
static void report(int level, const char* fmt, ...) {
    char small[512];
    char* heap = NULL;
    const char* msg = small;
    va_list ap;
    va_list ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0) {
        small[0] = '\0';
    } else if ((size_t)n >= sizeof(small)) {
        heap = malloc((size_t)n + 1);
        if (heap) {
            vsnprintf(heap, (size_t)n + 1, fmt, ap2);
            msg = heap;
        }
    }
    va_end(ap2);
    soulx_status_done();
    flockfile(stderr);
    fputs(msg, stderr);
    fputc('\n', stderr);
    fflush(stderr);
    funlockfile(stderr);
    soulx_mac_log_dispatch(level, msg);
    free(heap);
}

static char* message_dup(const char* s) {
    if (!s) s = "unknown error";
    size_t n = strlen(s);
    char* d = malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

// ---------------------------------------------------------------------------
// Argument helpers
// ---------------------------------------------------------------------------

// A number in [lo, hi] (numeric strings accepted, as everywhere in Lua), rounded to an int.
static int int_arg(lua_State* L, int idx, double lo, double hi, const char* what) {
    lua_Number v = luaL_checknumber(L, idx);
    if (!(v >= lo && v <= hi)) {                         // also rejects NaN
        char msg[160];
        snprintf(msg, sizeof(msg), "%s must be between %.0f and %.0f", what, lo, hi);
        luaL_argerror(L, idx, msg);
    }
    return (int)floor(v + 0.5);
}

static int opt_int_arg(lua_State* L, int idx, int def, double lo, double hi, const char* what) {
    return lua_isnoneornil(L, idx) ? def : int_arg(L, idx, lo, hi, what);
}

// A screen coordinate, rounded to the nearest pixel (the screen check comes later).
static int coord_arg(lua_State* L, int idx) {
    lua_Number v = luaL_checknumber(L, idx);
    if (!(v > -SCRIPT_COORD_LIMIT && v < SCRIPT_COORD_LIMIT)) luaL_argerror(L, idx, "invalid coordinate");
    return (int)floor(v + 0.5);
}

// A string of 1..max bytes without NUL bytes.
static const char* text_arg(lua_State* L, int idx, size_t max, const char* what) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, idx, &len);
    char msg[160];
    if (len == 0) {
        snprintf(msg, sizeof(msg), "%s is empty", what);
        luaL_argerror(L, idx, msg);
    }
    if (len > max) {
        snprintf(msg, sizeof(msg), "%s is too long (at most %zu bytes)", what, max);
        luaL_argerror(L, idx, msg);
    }
    if (memchr(s, '\0', len)) {
        snprintf(msg, sizeof(msg), "%s contains a NUL byte", what);
        luaL_argerror(L, idx, msg);
    }
    return s;
}

static const char* package_arg(lua_State* L, int idx) {
    const char* p = text_arg(L, idx, SCRIPT_PACKAGE_MAX, "package name");
    if (!soulx_package_name_valid(p)) luaL_argerror(L, idx, "invalid package name (e.g. \"com.example.game\")");
    return p;
}

// Sandbox (code an AI sent through `soulx mcp`):  names the only folder whose files the
// script may read or write (the instance's scripts folder); relative paths start there. Set only by the MCP server.
static const char* sandbox_dir(void) {
    const char* d = getenv("SOULX_LUA_SANDBOX_DIR");
    return (d && d[0] == '/') ? d : NULL;
}

static void sandbox_check(lua_State* L, int idx, const char* path) {
    const char* box = sandbox_dir();
    if (!box) return;
    char rbox[PATH_MAX], rp[PATH_MAX];
    if (!realpath(box, rbox)) luaL_argerror(L, idx, "sandbox: the scripts folder does not exist");
    if (!realpath(path, rp)) {                       // a file to be written: check its folder
        char parent[PATH_MAX];
        snprintf(parent, sizeof(parent), "%s", path);
        char* slash = strrchr(parent, '/');
        if (!slash) luaL_argerror(L, idx, "sandbox: path outside the scripts folder");
        *slash = '\0';
        char rparent[PATH_MAX];
        if (!realpath(parent[0] ? parent : "/", rparent)) luaL_argerror(L, idx, "sandbox: path outside the scripts folder");
        snprintf(rp, sizeof(rp), "%s/%s", rparent, slash + 1);
        if (strchr(slash + 1, '/') || !strcmp(slash + 1, "..") || !strcmp(slash + 1, ".")) {
            luaL_argerror(L, idx, "sandbox: path outside the scripts folder");
        }
    }
    size_t bl = strlen(rbox);
    if (strncmp(rp, rbox, bl) != 0 || (rp[bl] != '/' && rp[bl] != '\0')) {
        luaL_argerror(L, idx, "sandbox: only files in the instance's scripts folder can be used");
    }
}

// A file path: absolute, or relative to the script's folder.
static void path_arg(lua_State* L, ScriptCtx* ctx, int idx, char* out, size_t cap, const char* what) {
    const char* p = text_arg(L, idx, PATH_MAX - 1, what);
    int n;
    const char* box = sandbox_dir();
    if (box && p[0] != '/') {
        n = snprintf(out, cap, "%s/%s", box, p);
        if (n < 0 || (size_t)n >= cap) luaL_argerror(L, idx, "path too long");
        sandbox_check(L, idx, out);
        return;
    }
    if (p[0] == '/') {
        n = snprintf(out, cap, "%s", p);
    } else if (strcmp(ctx->dir, "/") == 0) {
        n = snprintf(out, cap, "/%s", p);
    } else {
        n = snprintf(out, cap, "%s/%s", ctx->dir, p);
    }
    if (n < 0 || (size_t)n >= cap) luaL_argerror(L, idx, "path too long");
    sandbox_check(L, idx, out);
}

// mkdir -p of the folder that will hold path.
static int ensure_parent_dir(const char* path) {
    const char* slash = strrchr(path, '/');
    if (!slash || slash == path) return 0;
    char dir[PATH_MAX];
    size_t n = (size_t)(slash - path);
    if (n >= sizeof(dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(dir, path, n);
    dir[n] = '\0';
    return soulx_is_dir(dir) ? 0 : soulx_mkdirs(dir);
}

static void check_inside(lua_State* L, const char* what, const char* area, int x, int y, int w, int h) {
    if (x < 0 || y < 0 || x >= w || y >= h) {
        script_error(L, "%s: point (%d, %d) is outside the %s (%dx%d)", what, x, y, area, w, h);
    }
}

static void screen_dims(lua_State* L, ScriptCtx* ctx, const char* what, int* w, int* h) {
    *w = 0;
    *h = 0;
    int rc = ctx->ops->screen_size(ctx->dev, w, h);
    after_device(L, ctx);
    if (rc != 0 || *w <= 0 || *h <= 0) {
        script_error(L, "%s: cannot read the screen size of '%s' (is it still running?)", what, ctx->instance);
    }
}

// x, y at idx and idx + 1: rounded and inside the screen.
static void point_args(lua_State* L, ScriptCtx* ctx, int idx, const char* what, int* x, int* y) {
    *x = coord_arg(L, idx);
    *y = coord_arg(L, idx + 1);
    int w, h;
    screen_dims(L, ctx, what, &w, &h);
    check_inside(L, what, "screen", *x, *y, w, h);
}

// ---------------------------------------------------------------------------
// Screen cache
// ---------------------------------------------------------------------------

static void screen_drop(ScriptCtx* ctx) {
    free(ctx->screen.rgba);
    memset(&ctx->screen, 0, sizeof(ctx->screen));
    ctx->screen_ms = 0.0;
}

// The current screen: the cached capture while younger than max_age_ms, else a new capture.
// The result stays valid until the next screen_get/screen_drop (see the rule in the header).
static const SoulXImage* screen_get(lua_State* L, ScriptCtx* ctx, double max_age_ms, const char* what) {
    if (ctx->screen.rgba && max_age_ms > 0.0 && soulx_now_ms() - ctx->screen_ms < max_age_ms) {
        return &ctx->screen;
    }
    SoulXImage img = { 0, 0, NULL };
    int rc = ctx->ops->capture(ctx->dev, &img);
    if (soulx_interrupted()) {
        free(img.rgba);
        raise_stop(L, ctx, STOP_INTERRUPT);
    }
    if (rc != 0 || !img.rgba || img.w <= 0 || img.h <= 0 || img.w > SOULX_IMAGE_MAX_DIM ||
        img.h > SOULX_IMAGE_MAX_DIM) {
        free(img.rgba);
        script_error(L, "%s: cannot read the screen of '%s' (is it still running?)", what, ctx->instance);
    }
    screen_drop(ctx);
    ctx->screen = img;
    ctx->screen_ms = soulx_now_ms();
    ctx->captures++;
    return &ctx->screen;
}

// Common tail of every input action: Ctrl+C, failure; the cached screen is outdated now.
static void after_input(lua_State* L, ScriptCtx* ctx, int rc, const char* what) {
    screen_drop(ctx);
    after_device(L, ctx);
    if (rc != 0) {
        script_error(L, "%s: the emulator '%s' did not accept the input (is it still running?)", what,
                     ctx->instance);
    }
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

static LuaImage* image_new(lua_State* L) {
    LuaImage* li = lua_newuserdatauv(L, sizeof(LuaImage), 0);
    memset(li, 0, sizeof(*li));
    luaL_setmetatable(L, IMAGE_MT);
    return li;
}

// Pixels live outside Lua's heap: tell the collector about them so that images the script no
// longer uses are freed promptly (a capture is about 10 MB).
static void gc_account(lua_State* L, const SoulXImage* img) {
    size_t kb = (size_t)img->w * (size_t)img->h * 4 / 1024;
    if (kb > 0) lua_gc(L, LUA_GCSTEP, kb > INT_MAX ? INT_MAX : (int)kb);
}

static int image_copy(SoulXImage* dst, const SoulXImage* src) {
    size_t bytes = (size_t)src->w * (size_t)src->h * 4;
    dst->rgba = malloc(bytes);
    if (!dst->rgba) return -1;
    memcpy(dst->rgba, src->rgba, bytes);
    dst->w = src->w;
    dst->h = src->h;
    return 0;
}

static void stamp_set(LuaImage* li, const struct stat* st) {
    li->file = true;
    li->dev = st->st_dev;
    li->ino = st->st_ino;
    li->size = st->st_size;
    li->mtime = st->st_mtimespec;
}

static bool stamp_same(const LuaImage* li, const struct stat* st) {
    return li->file && li->dev == st->st_dev && li->ino == st->st_ino && li->size == st->st_size &&
           li->mtime.tv_sec == st->st_mtimespec.tv_sec && li->mtime.tv_nsec == st->st_mtimespec.tv_nsec;
}

// Image argument: an Image, or an image file (PNG/JPEG) relative to the script's folder, loaded
// once per run and cached by absolute path (reloaded when the file changes). The resolved Image
// replaces the argument on the stack, which keeps it alive while the caller uses it.
static LuaImage* image_arg(lua_State* L, int idx, const char* what) {
    ScriptCtx* ctx = ctx_of(L);
    idx = lua_absindex(L, idx);
    LuaImage* li = luaL_testudata(L, idx, IMAGE_MT);
    if (li) {
        if (!li->img.rgba) luaL_argerror(L, idx, "the image has been released");
        return li;
    }
    if (lua_type(L, idx) != LUA_TSTRING) luaL_typeerror(L, idx, "image file name or Image");

    char path[PATH_MAX];
    char abs[PATH_MAX];
    struct stat st;
    path_arg(L, ctx, idx, path, sizeof(path), "image file name");
    if (!realpath(path, abs) || stat(abs, &st) != 0) {
        int e = errno;
        script_error(L, "%s: cannot open image '%s' (%s): %s", what, lua_tostring(L, idx), path, strerror(e));
    }
    if (!S_ISREG(st.st_mode)) script_error(L, "%s: '%s' is not a file", what, path);

    lua_rawgetp(L, LUA_REGISTRYINDEX, &k_templates);
    int cache = lua_gettop(L);
    lua_getfield(L, cache, abs);
    LuaImage* hit = luaL_testudata(L, -1, IMAGE_MT);
    if (hit && hit->img.rgba && stamp_same(hit, &st)) {
        lua_replace(L, idx);
        lua_pop(L, 1);                                  // cache
        return hit;
    }
    if (hit) {                                          // the file changed: forget the old copy
        ctx->templates--;
        ctx->template_bytes -= hit->bytes < ctx->template_bytes ? hit->bytes : ctx->template_bytes;
        lua_pushnil(L);
        lua_setfield(L, cache, abs);
    }
    lua_pop(L, 1);                                      // old entry or nil
    if (ctx->templates >= SCRIPT_TEMPLATES_MAX || ctx->template_bytes >= SCRIPT_TEMPLATES_BYTES) {
        lua_newtable(L);                                // start over with an empty cache
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &k_templates);
        lua_replace(L, cache);
        ctx->templates = 0;
        ctx->template_bytes = 0;
    }

    LuaImage* fresh = image_new(L);
    if (soulx_image_load_png(abs, &fresh->img) != 0) script_error(L, "%s: %s", what, soulx_image_error());
    stamp_set(fresh, &st);
    fresh->bytes = (size_t)fresh->img.w * (size_t)fresh->img.h * 4;
    lua_pushvalue(L, -1);
    lua_setfield(L, cache, abs);
    ctx->templates++;
    ctx->template_bytes += fresh->bytes;
    lua_replace(L, idx);
    lua_pop(L, 1);                                      // cache
    gc_account(L, &fresh->img);
    return fresh;
}

// A template larger than the search area can never be found: say so once per image.
// Allocates nothing in Lua.
static void warn_template(lua_State* L, LuaImage* tm, const SoulXImage* scr, const SoulXMatchOpts* o,
                          const char* what) {
    if (tm->warned) return;
    long long aw = scr->w;
    long long ah = scr->h;
    bool region = o->region_w > 0 && o->region_h > 0;
    if (region) {
        long long x0 = o->region_x > 0 ? o->region_x : 0;
        long long y0 = o->region_y > 0 ? o->region_y : 0;
        long long x1 = (long long)o->region_x + o->region_w;
        long long y1 = (long long)o->region_y + o->region_h;
        if (x1 > scr->w) x1 = scr->w;
        if (y1 > scr->h) y1 = scr->h;
        aw = x1 > x0 ? x1 - x0 : 0;
        ah = y1 > y0 ? y1 - y0 : 0;
    }
    if (tm->img.w > aw || tm->img.h > ah) {
        tm->warned = true;
        script_warn(L, "%s: the image (%dx%d) is larger than the %s (%lldx%lld), so it can never be found",
                    what, tm->img.w, tm->img.h, region ? "search region" : "screen", aw, ah);
    }
}

// ---------------------------------------------------------------------------
// Matching options: {threshold = 0.88, region = {x, y, w, h}, color_tolerance = 40}
// ---------------------------------------------------------------------------

static void region_opt(lua_State* L, int opts_idx, int r, SoulXMatchOpts* o) {
    static const char* const names[4] = { "x", "y", "w", "h" };
    if (!lua_istable(L, r)) luaL_argerror(L, opts_idx, "region must be a table {x, y, w, h}");
    int v[4];
    for (int i = 0; i < 4; i++) {
        lua_rawgeti(L, r, i + 1);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, r, names[i]);
        }
        if (lua_type(L, -1) != LUA_TNUMBER) {
            luaL_argerror(L, opts_idx, "region must be {x, y, w, h}: four numbers in screen pixels");
        }
        lua_Number d = lua_tonumber(L, -1);
        if (!(d >= 0 && d <= SOULX_IMAGE_MAX_DIM)) {
            luaL_argerror(L, opts_idx, "region values must be between 0 and 16384");
        }
        v[i] = (int)floor(d + 0.5);
        lua_pop(L, 1);
    }
    if (v[2] <= 0 || v[3] <= 0) luaL_argerror(L, opts_idx, "region width and height must be at least 1");
    o->region_x = v[0];
    o->region_y = v[1];
    o->region_w = v[2];
    o->region_h = v[3];
}

static void opts_arg(lua_State* L, int idx, SoulXMatchOpts* o) {
    soulx_match_opts_init(o);
    if (lua_isnoneornil(L, idx)) return;
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) luaL_typeerror(L, idx, "options table");
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_argerror(L, idx, "option names must be strings, e.g. {threshold = 0.9}");
        }
        const char* k = lua_tostring(L, -2);
        if (strcmp(k, "threshold") == 0) {
            lua_Number t = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : -1.0;
            if (!(t > 0.0 && t <= 1.0)) {
                luaL_argerror(L, idx, "threshold must be a number above 0 and at most 1 (default 0.88)");
            }
            o->threshold = t;
        } else if (strcmp(k, "color_tolerance") == 0) {
            lua_Number t = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : -1.0;
            if (!(t >= 0.0 && t <= 255.0)) {
                luaL_argerror(L, idx, "color_tolerance must be a number from 0 (no colour check) to 255");
            }
            o->color_tolerance = (int)floor(t + 0.5);
        } else if (strcmp(k, "region") == 0) {
            region_opt(L, idx, lua_gettop(L), o);
        } else if (strcmp(k, "scale") == 0) {
            lua_Number t = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : -1.0;
            if (!(t >= 0.1 && t <= 10.0)) {
                luaL_argerror(L, idx, "scale must be a number from 0.1 to 10 (1.0 = no rescale)");
            }
            o->scale = t;
        } else {
            char msg[160];
            snprintf(msg, sizeof(msg), "unknown option '%.40s' (valid: threshold, region, color_tolerance, scale)", k);
            luaL_argerror(L, idx, msg);
        }
        lua_pop(L, 1);
    }
}

// ---------------------------------------------------------------------------
// Matching results
// ---------------------------------------------------------------------------

// find: pushes x, y, score (3 results) or nil (1 result). Allocates nothing before matching.
static int push_find(lua_State* L, const SoulXImage* scr, LuaImage* tm, const SoulXMatchOpts* o, const char* what) {
    warn_template(L, tm, scr, o, what);
    SoulXMatch m;
    int rc = soulx_image_find(scr, &tm->img, o, &m);
    if (rc < 0) return script_error(L, "%s: %s", what, soulx_image_error());
    if (rc == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, m.x);
    lua_pushinteger(L, m.y);
    lua_pushnumber(L, m.score);
    return 3;
}

// Scratch space for find_all, allocated before the screen is borrowed.
static SoulXMatch* matches_new(lua_State* L) {
    return lua_newuserdatauv(L, sizeof(SoulXMatch) * SCRIPT_FIND_ALL_MAX, 0);
}

// find_all: pushes an array of {x = , y = , score = }, best first.
static int push_find_all(lua_State* L, const SoulXImage* scr, LuaImage* tm, const SoulXMatchOpts* o,
                         SoulXMatch* buf, const char* what) {
    warn_template(L, tm, scr, o, what);
    int n = soulx_image_find_all(scr, &tm->img, o, buf, SCRIPT_FIND_ALL_MAX);
    if (n < 0) return script_error(L, "%s: %s", what, soulx_image_error());
    // the screen is not used below: allocating is fine again
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, buf[i].x);
        lua_setfield(L, -2, "x");
        lua_pushinteger(L, buf[i].y);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, buf[i].score);
        lua_setfield(L, -2, "score");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Image methods
// ---------------------------------------------------------------------------

static LuaImage* img_self(lua_State* L) {
    LuaImage* li = luaL_checkudata(L, 1, IMAGE_MT);
    if (!li->img.rgba) luaL_argerror(L, 1, "the image has been released");
    return li;
}

// __gc and __close: frees the pixels
static int img_gc(lua_State* L) {
    LuaImage* li = luaL_checkudata(L, 1, IMAGE_MT);
    free(li->img.rgba);
    li->img.rgba = NULL;
    return 0;
}

static int img_tostring(lua_State* L) {
    LuaImage* li = luaL_checkudata(L, 1, IMAGE_MT);
    if (li->img.rgba) {
        lua_pushfstring(L, "Image(%dx%d)", li->img.w, li->img.h);
    } else {
        lua_pushliteral(L, "Image(released)");
    }
    return 1;
}

// img:size() -> w, h
static int img_size(lua_State* L) {
    LuaImage* li = img_self(L);
    lua_pushinteger(L, li->img.w);
    lua_pushinteger(L, li->img.h);
    return 2;
}

// img:color(x, y) -> 0xRRGGBB
static int img_color(lua_State* L) {
    api_check(L);
    LuaImage* li = img_self(L);
    int x = coord_arg(L, 2);
    int y = coord_arg(L, 3);
    check_inside(L, "color", "image", x, y, li->img.w, li->img.h);
    lua_pushinteger(L, soulx_image_pixel(&li->img, x, y));
    return 1;
}

// img:find(image [, opts]) -> x, y, score | nil
static int img_find(lua_State* L) {
    api_check(L);
    LuaImage* li = img_self(L);
    LuaImage* tm = image_arg(L, 2, "find");
    SoulXMatchOpts o;
    opts_arg(L, 3, &o);
    return push_find(L, &li->img, tm, &o, "find");
}

// img:find_all(image [, opts]) -> {{x =, y =, score =}, ...}
static int img_find_all(lua_State* L) {
    api_check(L);
    LuaImage* li = img_self(L);
    LuaImage* tm = image_arg(L, 2, "find_all");
    SoulXMatchOpts o;
    opts_arg(L, 3, &o);
    SoulXMatch* buf = matches_new(L);
    return push_find_all(L, &li->img, tm, &o, buf, "find_all");
}

// img:crop(x, y, w, h) -> Image
static int img_crop(lua_State* L) {
    api_check(L);
    LuaImage* li = img_self(L);
    int x = coord_arg(L, 2);
    int y = coord_arg(L, 3);
    int w = int_arg(L, 4, 1, SOULX_IMAGE_MAX_DIM, "width");
    int h = int_arg(L, 5, 1, SOULX_IMAGE_MAX_DIM, "height");
    LuaImage* out = image_new(L);
    if (soulx_image_crop(&li->img, x, y, w, h, &out->img) != 0) {
        return script_error(L, "crop: %s", soulx_image_error());
    }
    gc_account(L, &out->img);
    return 1;
}

// img:save(path) -> absolute path (PNG; missing folders are created)
static int img_save(lua_State* L) {
    ScriptCtx* ctx = api_check(L);
    LuaImage* li = img_self(L);
    char path[PATH_MAX];
    path_arg(L, ctx, 2, path, sizeof(path), "file name");
    if (ensure_parent_dir(path) != 0) {
        return script_error(L, "save: cannot create the folder for %s: %s", path, strerror(errno));
    }
    if (soulx_image_save_png(path, &li->img) != 0) return script_error(L, "save: %s", soulx_image_error());
    lua_pushstring(L, path);
    return 1;
}

// ---------------------------------------------------------------------------
// Script API: apps
// ---------------------------------------------------------------------------

// launch(package)
static int api_launch(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    const char* pkg = package_arg(L, 1);
    int rc = ctx->ops->launch(ctx->dev, pkg);
    screen_drop(ctx);
    after_device(L, ctx);
    if (rc != 0) return script_error(L, "launch: cannot start %s (is it installed?)", pkg);
    return 0;
}

// stop_app(package): force-stops an installed app. Stopping an app that is not running is fine,
// but stopping one that is not installed is an error, like the CLI's `soulx stop-app`.
static int api_stop_app(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    const char* pkg = package_arg(L, 1);
    int rc = ctx->ops->stop_app(ctx->dev, pkg);        // 1 = not installed
    screen_drop(ctx);
    after_device(L, ctx);
    if (rc == 1) return script_error(L, "stop_app: %s is not installed on '%s' (see: soulx apps)", pkg, ctx->instance);
    if (rc != 0) return script_error(L, "stop_app: cannot stop %s", pkg);
    return 0;
}

// install(path [, reinstall = false]) -> package name
static int api_install(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    char path[PATH_MAX];
    path_arg(L, ctx, 1, path, sizeof(path), "file name");
    if (!lua_isnoneornil(L, 2)) luaL_checktype(L, 2, LUA_TBOOLEAN);
    bool reinstall = lua_toboolean(L, 2);
    struct stat st;
    if (stat(path, &st) != 0) return script_error(L, "install: %s: %s", path, strerror(errno));
    char pkg[256] = "";
    int rc = ctx->ops->install(ctx->dev, path, reinstall, pkg, sizeof(pkg));
    after_device(L, ctx);
    if (rc != 0 || !pkg[0]) {
        return script_error(L, "install: installing %s failed (the reason is printed above)", path);
    }
    lua_pushstring(L, pkg);
    return 1;
}

// current_app() -> package name of the foreground app, or nil
static int api_current_app(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    char pkg[256] = "";
    int rc = ctx->ops->current_app(ctx->dev, pkg, sizeof(pkg));
    after_device(L, ctx);
    if (rc != 0 || !pkg[0]) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, pkg);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Script API: input
// ---------------------------------------------------------------------------

// tap(x, y) -> true | tap(image [, opts]) -> true when found and tapped, else false
static int api_tap(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    int t2 = lua_type(L, 2);
    if (lua_type(L, 1) == LUA_TNUMBER || t2 == LUA_TNUMBER || (t2 == LUA_TSTRING && lua_isnumber(L, 2))) {
        int x, y;
        point_args(L, ctx, 1, "tap", &x, &y);
        int rc = ctx->ops->tap(ctx->dev, x, y);
        after_input(L, ctx, rc, "tap");
        lua_pushboolean(L, 1);
        return 1;
    }
    LuaImage* tm = image_arg(L, 1, "tap");
    SoulXMatchOpts o;
    opts_arg(L, 2, &o);
    const SoulXImage* scr = screen_get(L, ctx, SCRIPT_CACHE_MAX_AGE_MS, "tap");
    warn_template(L, tm, scr, &o, "tap");
    SoulXMatch m;
    int found = soulx_image_find(scr, &tm->img, &o, &m);
    if (found < 0) return script_error(L, "tap: %s", soulx_image_error());
    if (found == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    int rc = ctx->ops->tap(ctx->dev, m.x, m.y);
    after_input(L, ctx, rc, "tap");
    lua_pushboolean(L, 1);
    return 1;
}

// long_press(x, y [, ms = 800])
static int api_long_press(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    int ms = opt_int_arg(L, 3, SCRIPT_LONG_PRESS_MS, 1, SCRIPT_GESTURE_MAX_MS, "ms");
    int x, y;
    point_args(L, ctx, 1, "long_press", &x, &y);
    int rc = ctx->ops->long_press(ctx->dev, x, y, ms);
    after_input(L, ctx, rc, "long_press");
    return 0;
}

// swipe(x1, y1, x2, y2 [, ms = 300])
static int api_swipe(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    int x1 = coord_arg(L, 1);
    int y1 = coord_arg(L, 2);
    int x2 = coord_arg(L, 3);
    int y2 = coord_arg(L, 4);
    int ms = opt_int_arg(L, 5, SCRIPT_SWIPE_MS, 1, SCRIPT_GESTURE_MAX_MS, "ms");
    int w, h;
    screen_dims(L, ctx, "swipe", &w, &h);
    check_inside(L, "swipe", "screen", x1, y1, w, h);
    check_inside(L, "swipe", "screen", x2, y2, w, h);
    int rc = ctx->ops->swipe(ctx->dev, x1, y1, x2, y2, ms);
    after_input(L, ctx, rc, "swipe");
    return 0;
}

static int finger_arg(lua_State* L, int idx) {
    return opt_int_arg(L, idx, 0, 0, SOULX_MAX_FINGERS - 1, "finger");
}

// touch_down(x, y [, finger = 0])
static int api_touch_down(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    int f = finger_arg(L, 3);
    int x, y;
    point_args(L, ctx, 1, "touch_down", &x, &y);
    if (ctx->down[f]) {
        return script_error(L, "touch_down: finger %d is already down (call touch_up(%d) first)", f, f);
    }
    int rc = ctx->ops->touch(ctx->dev, f, SOULX_TOUCH_DOWN, x, y);
    ctx->down[f] = rc == 0;
    ctx->fx[f] = x;
    ctx->fy[f] = y;
    after_input(L, ctx, rc, "touch_down");
    return 0;
}

// touch_move(x, y [, finger = 0])
static int api_touch_move(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    int f = finger_arg(L, 3);
    int x, y;
    point_args(L, ctx, 1, "touch_move", &x, &y);
    if (!ctx->down[f]) return script_error(L, "touch_move: finger %d is not down (call touch_down first)", f);
    int rc = ctx->ops->touch(ctx->dev, f, SOULX_TOUCH_MOVE, x, y);
    ctx->fx[f] = x;
    ctx->fy[f] = y;
    after_input(L, ctx, rc, "touch_move");
    return 0;
}

// touch_up([finger = 0]) -> true, or false when that finger was not down
static int api_touch_up(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    int f = finger_arg(L, 1);
    if (!ctx->down[f]) {
        lua_pushboolean(L, 0);
        return 1;
    }
    ctx->down[f] = false;
    int rc = ctx->ops->touch(ctx->dev, f, SOULX_TOUCH_UP, ctx->fx[f], ctx->fy[f]);
    after_input(L, ctx, rc, "touch_up");
    lua_pushboolean(L, 1);
    return 1;
}

// key(name_or_code): "BACK", "HOME", "ENTER", "KEYCODE_VOLUME_UP", 66, ...
static int api_key(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    char key[SCRIPT_KEY_MAX + 1];
    if (lua_type(L, 1) == LUA_TNUMBER) {
        int isint = 0;
        lua_Integer code = lua_tointegerx(L, 1, &isint);
        if (!isint || code < 1 || code > SCRIPT_KEYCODE_MAX) {
            luaL_argerror(L, 1, "a key code is a whole number from 1 to 999 (e.g. 4 = BACK)");
        }
        snprintf(key, sizeof(key), "%d", (int)code);
    } else {
        const char* s = text_arg(L, 1, SCRIPT_KEY_MAX, "key name");
        for (const char* p = s; *p; p++) {
            bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_';
            if (!ok) luaL_argerror(L, 1, "a key name uses letters, digits and '_' (e.g. \"BACK\", \"ENTER\")");
        }
        snprintf(key, sizeof(key), "%s", s);
    }
    // Reject an unknown key here, before the device is touched, so there is one clear error (the
    // device layer would otherwise also log its own "unknown key" line).
    if (soulx_input_keycode(key) < 0) {
        return script_error(L, "key: unknown key '%s' (use a name like BACK, HOME, ENTER, A, F1, "
                               "or an Android key code from 1 to 999)", key);
    }
    int rc = ctx->ops->key(ctx->dev, key);
    screen_drop(ctx);
    after_device(L, ctx);
    if (rc != 0) {
        return script_error(L, "key: the emulator '%s' did not accept the key '%s' (is it still running?)",
                            ctx->instance, key);
    }
    return 0;
}

// text(str): types ASCII text into the focused field ("\n" = Enter, "\t" = Tab)
static int api_text(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    const char* s = text_arg(L, 1, SCRIPT_TEXT_MAX, "text");
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p >= 0x80) luaL_argerror(L, 1, "only ASCII text can be typed");
        if ((*p < 0x20 && *p != '\n' && *p != '\t' && *p != '\r') || *p == 0x7F) {
            luaL_argerror(L, 1, "control characters cannot be typed (\"\\n\" = Enter and \"\\t\" = Tab work)");
        }
    }
    int rc = ctx->ops->text(ctx->dev, s);
    after_input(L, ctx, rc, "text");
    return 0;
}

// ---------------------------------------------------------------------------
// Script API: time and logging
// ---------------------------------------------------------------------------

// Scale a pause by the run's playback speed (1/speed), clamped to the sleep range.
static int scale_pause(const ScriptCtx* ctx, int ms) {
    if (ctx->speed > 0.0 && ctx->speed != 1.0) {
        double scaled = (double)ms / ctx->speed;
        if (scaled > (double)SCRIPT_SLEEP_MAX_MS) scaled = (double)SCRIPT_SLEEP_MAX_MS;
        ms = (int)(scaled + 0.5);
    }
    return ms;
}

// sleep(ms)
static int api_sleep(lua_State* L) {
    ScriptCtx* ctx = api_check(L);
    int ms = int_arg(L, 1, 0, SCRIPT_SLEEP_MAX_MS, "ms");
    if (soulx_sleep_ms(scale_pause(ctx, ms)) != 0) return raise_stop(L, ctx, STOP_INTERRUPT);
    return 0;
}

// speed([x]) -> current playback speed; with an argument, sets it (0.1..10) and returns the new value
static int api_speed(lua_State* L) {
    ScriptCtx* ctx = api_check(L);
    if (!lua_isnoneornil(L, 1)) {
        double x = luaL_checknumber(L, 1);
        luaL_argcheck(L, x >= 0.1 && x <= 10.0, 1, "speed must be between 0.1 and 10");
        ctx->speed = x;
    }
    lua_pushnumber(L, ctx->speed);
    return 1;
}

// iteration() -> current pass (1-based), total passes (0 = looping until stopped)
static int api_iteration(lua_State* L) {
    ScriptCtx* ctx = api_check(L);
    lua_pushinteger(L, ctx->pass);
    lua_pushinteger(L, ctx->passes);
    return 2;
}

// millis() -> milliseconds since the script started
static int api_millis(lua_State* L) {
    ScriptCtx* ctx = api_check(L);
    lua_pushinteger(L, (lua_Integer)(soulx_now_ms() - ctx->t0));
    return 1;
}

// log(...): "[12.3s] message" on stderr (values are separated by spaces)
static int api_log(lua_State* L) {
    ScriptCtx* ctx = ctx_of(L);
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, ' ');
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    size_t len = 0;
    const char* msg = lua_tolstring(L, -1, &len);
    char stamp[48];
    snprintf(stamp, sizeof(stamp), "[%.1fs] ", (soulx_now_ms() - ctx->t0) / 1000.0);
    soulx_status_done();
    flockfile(stderr);
    fputs(stamp, stderr);
    fwrite(msg, 1, len, stderr);
    fputc('\n', stderr);
    fflush(stderr);
    funlockfile(stderr);
    lua_pushfstring(L, "%s%s", stamp, msg);
    soulx_mac_log_dispatch(SOULX_LOG_INFO, lua_tostring(L, -1));
    return 0;
}

// ---------------------------------------------------------------------------
// Script API: screen
// ---------------------------------------------------------------------------

// load_image(path) -> Image. Loads a PNG or JPEG file (reference images, templates, or a saved screenshot to test
// recognition on). Pixels are RGBA, like capture(); use img:color / img:find / img:crop on it. The path is relative to
// the script folder like every other file path (templates, screenshot, img:save, install), whatever the working folder
// is (the app does not start scripts from it); under the AI sandbox it must lie in the scripts folder.
static int api_load_image(lua_State* L) {
    ScriptCtx* ctx = api_check(L);
    char path[PATH_MAX];
    path_arg(L, ctx, 1, path, sizeof(path), "file name");
    LuaImage* li = image_new(L);
    if (soulx_image_load_png(path, &li->img) != 0) return script_error(L, "load_image: %s", soulx_image_error());
    gc_account(L, &li->img);
    return 1;
}

// capture() -> Image of the current screen (always a new capture)
static int api_capture(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    LuaImage* li = image_new(L);                        // allocated before the screen is borrowed
    const SoulXImage* scr = screen_get(L, ctx, 0.0, "capture");
    if (image_copy(&li->img, scr) != 0) return script_error(L, "capture: out of memory");
    gc_account(L, &li->img);
    return 1;
}

// color(x, y) -> 0xRRGGBB
static int api_color(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    int x = coord_arg(L, 1);
    int y = coord_arg(L, 2);
    const SoulXImage* scr = screen_get(L, ctx, SCRIPT_CACHE_MAX_AGE_MS, "color");
    check_inside(L, "color", "screen", x, y, scr->w, scr->h);
    lua_pushinteger(L, soulx_image_pixel(scr, x, y));
    return 1;
}

// is_color(x, y, rgb [, tolerance = 16]) -> true when every channel differs by at most tolerance
static int api_is_color(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    int x = coord_arg(L, 1);
    int y = coord_arg(L, 2);
    lua_Integer rgb = luaL_checkinteger(L, 3);
    if (rgb < 0 || rgb > 0xFFFFFF) luaL_argerror(L, 3, "colour must be 0xRRGGBB (0 to 0xFFFFFF)");
    int tol = opt_int_arg(L, 4, SCRIPT_IS_COLOR_TOLERANCE, 0, 255, "tolerance");
    const SoulXImage* scr = screen_get(L, ctx, SCRIPT_CACHE_MAX_AGE_MS, "is_color");
    check_inside(L, "is_color", "screen", x, y, scr->w, scr->h);
    int32_t c = soulx_image_pixel(scr, x, y);
    int dr = abs((int)((c >> 16) & 0xFF) - (int)((rgb >> 16) & 0xFF));
    int dg = abs((int)((c >> 8) & 0xFF) - (int)((rgb >> 8) & 0xFF));
    int db = abs((int)(c & 0xFF) - (int)(rgb & 0xFF));
    lua_pushboolean(L, dr <= tol && dg <= tol && db <= tol);
    return 1;
}

// read_text([region]) -> string | nil. OCR (macOS Vision, on-device) of the screen or a {x, y, w, h} region.
static int api_read_text(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    bool has_region = !lua_isnoneornil(L, 1);
    SoulXMatchOpts o;
    soulx_match_opts_init(&o);
    if (has_region) region_opt(L, 1, 1, &o);
    const SoulXImage* scr = screen_get(L, ctx, SCRIPT_CACHE_MAX_AGE_MS, "read_text");
    SoulXImage crop = { 0, 0, NULL };
    const SoulXImage* src = scr;
    if (has_region) {
        int x = o.region_x, y = o.region_y, w = o.region_w, h = o.region_h;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > scr->w) w = scr->w - x;
        if (y + h > scr->h) h = scr->h - y;
        if (w <= 0 || h <= 0) { lua_pushnil(L); return 1; }
        if (soulx_image_crop(scr, x, y, w, h, &crop) != 0) return script_error(L, "read_text: %s", soulx_image_error());
        src = &crop;
    }
    char buf[4096];
    int rc = soulx_ocr_text(src, buf, sizeof(buf), false);
    if (crop.rgba) soulx_image_free(&crop);
    if (rc != 0) { lua_pushnil(L); return 1; }
    lua_pushstring(L, buf);
    return 1;
}

// find(image [, opts]) -> x, y, score | nil
static int api_find(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    LuaImage* tm = image_arg(L, 1, "find");
    SoulXMatchOpts o;
    opts_arg(L, 2, &o);
    const SoulXImage* scr = screen_get(L, ctx, SCRIPT_CACHE_MAX_AGE_MS, "find");
    return push_find(L, scr, tm, &o, "find");
}

// find_all(image [, opts]) -> {{x =, y =, score =}, ...} (best first; empty when none)
static int api_find_all(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    LuaImage* tm = image_arg(L, 1, "find_all");
    SoulXMatchOpts o;
    opts_arg(L, 2, &o);
    SoulXMatch* buf = matches_new(L);
    const SoulXImage* scr = screen_get(L, ctx, SCRIPT_CACHE_MAX_AGE_MS, "find_all");
    return push_find_all(L, scr, tm, &o, buf, "find_all");
}

// exists(image [, opts]) -> bool
static int api_exists(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    LuaImage* tm = image_arg(L, 1, "exists");
    SoulXMatchOpts o;
    opts_arg(L, 2, &o);
    const SoulXImage* scr = screen_get(L, ctx, SCRIPT_CACHE_MAX_AGE_MS, "exists");
    warn_template(L, tm, scr, &o, "exists");
    SoulXMatch m;
    int rc = soulx_image_find(scr, &tm->img, &o, &m);
    if (rc < 0) return script_error(L, "exists: %s", soulx_image_error());
    lua_pushboolean(L, rc == 1);
    return 1;
}

// wait(image [, timeout_s = 10 [, opts]]) -> x, y | nil
// wait_gone(image [, timeout_s = 10 [, opts]]) -> true when the image is no longer visible, else false
static int wait_common(lua_State* L, bool gone) {
    const char* what = gone ? "wait_gone" : "wait";
    ScriptCtx* ctx = api_enter(L);
    LuaImage* tm = image_arg(L, 1, what);
    lua_Number timeout = luaL_optnumber(L, 2, SCRIPT_WAIT_DEFAULT_S);
    if (!(timeout >= 0.0 && timeout <= SCRIPT_WAIT_MAX_S)) {
        luaL_argerror(L, 2, "timeout must be between 0 and 86400 seconds");
    }
    SoulXMatchOpts o;
    opts_arg(L, 3, &o);

    const double deadline = soulx_now_ms() + timeout * 1000.0;
    for (int attempt = 0;; attempt++) {
        double started = soulx_now_ms();
        // the first look may reuse a fresh capture; later rounds always capture
        const SoulXImage* scr = screen_get(L, ctx, attempt == 0 ? SCRIPT_CACHE_MAX_AGE_MS : 0.0, what);
        warn_template(L, tm, scr, &o, what);
        SoulXMatch m;
        int rc = soulx_image_find(scr, &tm->img, &o, &m);
        if (rc < 0) return script_error(L, "%s: %s", what, soulx_image_error());
        if (!gone && rc == 1) {
            lua_pushinteger(L, m.x);
            lua_pushinteger(L, m.y);
            return 2;
        }
        if (gone && rc == 0) {
            lua_pushboolean(L, 1);
            return 1;
        }
        double now = soulx_now_ms();
        if (now >= deadline) break;
        double pause = SCRIPT_POLL_MS - (now - started);
        if (pause > deadline - now) pause = deadline - now;
        if (pause > 0.0 && soulx_sleep_ms((int)ceil(pause)) != 0) return raise_stop(L, ctx, STOP_INTERRUPT);
        api_check(L);
    }
    if (gone) {
        lua_pushboolean(L, 0);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int api_wait(lua_State* L) {
    return wait_common(L, false);
}

static int api_wait_gone(lua_State* L) {
    return wait_common(L, true);
}

// screenshot(path [, x, y, w, h]) -> absolute path of the PNG written (always a new capture)
static int api_screenshot(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    char path[PATH_MAX];
    path_arg(L, ctx, 1, path, sizeof(path), "file name");
    bool part = !lua_isnoneornil(L, 2);
    int rx = 0, ry = 0, rw = 0, rh = 0;
    LuaImage* crop = NULL;
    if (part) {
        rx = int_arg(L, 2, 0, SOULX_IMAGE_MAX_DIM, "x");
        ry = int_arg(L, 3, 0, SOULX_IMAGE_MAX_DIM, "y");
        rw = int_arg(L, 4, 1, SOULX_IMAGE_MAX_DIM, "width");
        rh = int_arg(L, 5, 1, SOULX_IMAGE_MAX_DIM, "height");
        crop = image_new(L);                            // allocated before the screen is borrowed
    }
    if (ensure_parent_dir(path) != 0) {
        return script_error(L, "screenshot: cannot create the folder for %s: %s", path, strerror(errno));
    }
    const SoulXImage* scr = screen_get(L, ctx, 0.0, "screenshot");
    const SoulXImage* out = scr;
    if (part) {
        if (rx + rw > scr->w || ry + rh > scr->h) {
            return script_error(L, "screenshot: the rectangle (%d, %d, %d, %d) is not inside the screen (%dx%d)",
                                rx, ry, rw, rh, scr->w, scr->h);
        }
        if (soulx_image_crop(scr, rx, ry, rw, rh, &crop->img) != 0) {
            return script_error(L, "screenshot: %s", soulx_image_error());
        }
        out = &crop->img;
    }
    if (soulx_image_save_png(path, out) != 0) return script_error(L, "screenshot: %s", soulx_image_error());
    lua_pushstring(L, path);
    return 1;
}

// screen_size() -> w, h (logical: follows the current orientation)
static int api_screen_size(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    int w, h;
    screen_dims(L, ctx, "screen_size", &w, &h);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

// ---------------------------------------------------------------------------
// Script API: instance, console, shell
// ---------------------------------------------------------------------------

static void release_touches(ScriptCtx* ctx) {
    for (int f = 0; f < SOULX_MAX_FINGERS; f++) {
        if (ctx->down[f] && ctx->dev) (void)ctx->ops->touch(ctx->dev, f, SOULX_TOUCH_UP, ctx->fx[f], ctx->fy[f]);
        ctx->down[f] = false;
    }
}

// use(name) -> instance name: later calls go to that (running) instance
static int api_use(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    const char* ref = text_arg(L, 1, SCRIPT_INSTANCE_REF_MAX, "instance name");
    char name[SOULX_NAME_MAX + 1] = "";
    char err[256] = "";
    void* dev = ctx->ops->open(ref, name, sizeof(name), err, sizeof(err));
    if (!dev) {
        after_device(L, ctx);
        return script_error(L, "use: %s", err[0] ? err : "cannot open the instance");
    }
    release_touches(ctx);
    ctx->ops->close(ctx->dev);
    ctx->dev = dev;
    snprintf(ctx->instance, sizeof(ctx->instance), "%s", name[0] ? name : ref);
    screen_drop(ctx);
    after_device(L, ctx);
    lua_pushstring(L, ctx->instance);
    return 1;
}

// console(cmd) -> reply text (raises when the console answers KO)
static int api_console(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    const char* cmd = text_arg(L, 1, SCRIPT_CONSOLE_CMD_MAX, "console command");
    if (strpbrk(cmd, "\r\n")) luaL_argerror(L, 1, "a console command is a single line");
    luaL_Buffer b;
    char* reply = luaL_buffinitsize(L, &b, SCRIPT_CONSOLE_REPLY);
    reply[0] = '\0';
    int rc = ctx->ops->console(ctx->dev, cmd, reply, SCRIPT_CONSOLE_REPLY);
    reply[SCRIPT_CONSOLE_REPLY - 1] = '\0';
    luaL_pushresultsize(&b, strlen(reply));
    screen_drop(ctx);                                   // e.g. "rotate" changes the screen
    after_device(L, ctx);
    if (rc == 1) return script_error(L, "console(\"%.200s\"): KO: %s", cmd, lua_tostring(L, -1));
    if (rc != 0) {
        return script_error(L, "console: no answer from the emulator console of '%s' (is it still running?)",
                            ctx->instance);
    }
    return 1;
}

// Memory box: malloc'd memory freed by __gc unless the owner frees it first (no leak on errors).
static void** memory_box(lua_State* L) {
    void** box = lua_newuserdatauv(L, sizeof(void*), 0);
    *box = NULL;
    luaL_setmetatable(L, MEMORY_MT);
    return box;
}

static int memory_gc(lua_State* L) {
    void** box = luaL_checkudata(L, 1, MEMORY_MT);
    free(*box);
    *box = NULL;
    return 0;
}

// shell(cmd [, timeout_s = 60]) -> output (trailing newlines removed), exit status
static int api_shell(lua_State* L) {
    ScriptCtx* ctx = api_enter(L);
    const char* cmd = text_arg(L, 1, SCRIPT_SHELL_CMD_MAX, "command");
    lua_Number timeout = luaL_optnumber(L, 2, SCRIPT_SHELL_TIMEOUT_S);
    if (!(timeout > 0.0 && timeout <= SCRIPT_SHELL_MAX_S)) {
        luaL_argerror(L, 2, "timeout must be above 0 and at most 3600 seconds");
    }
    void** box = memory_box(L);
    char* out = NULL;
    size_t out_len = 0;
    int status = -1;
    char err[256] = "";
    int rc = ctx->ops->shell(ctx->dev, cmd, (int)ceil(timeout * 1000.0), &out, &out_len, &status, err, sizeof(err));
    *box = out;
    screen_drop(ctx);
    after_device(L, ctx);
    if (rc == -2) return script_error(L, "shell: '%.60s' did not finish within %g s", cmd, timeout);
    if (rc != 0) return script_error(L, "shell: cannot run '%.60s': %s", cmd, err[0] ? err : "adb failed");
    while (out && out_len > 0 && (out[out_len - 1] == '\n' || out[out_len - 1] == '\r')) out_len--;
    lua_pushlstring(L, out ? out : "", out ? out_len : 0);
    free(out);
    *box = NULL;
    lua_pushinteger(L, status);
    return 2;
}

// ---------------------------------------------------------------------------
// Standard library adjustments
// ---------------------------------------------------------------------------

// os.exit([code]): ends the script with that exit code (true = 0, false = 1); never the process
static int api_os_exit(lua_State* L) {
    ScriptCtx* ctx = ctx_of(L);
    int code = 0;
    if (lua_isboolean(L, 1)) {
        code = lua_toboolean(L, 1) ? 0 : 1;
    } else if (!lua_isnoneornil(L, 1)) {
        code = int_arg(L, 1, 0, 255, "exit code");
    }
    if (ctx->stop == STOP_NONE) ctx->exit_code = code;
    return raise_stop(L, ctx, STOP_EXIT);
}

// os.execute / io.popen: scripts never run host commands
static int api_os_execute(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {                        // "is a shell available?"
        lua_pushboolean(L, 0);
        return 1;
    }
    return script_error(L, "os.execute is not available in SoulX scripts; use shell(\"...\") to run a "
                           "command inside Android");
}

static int api_io_popen(lua_State* L) {
    return script_error(L, "io.popen is not available in SoulX scripts; use shell(\"...\") to run a "
                           "command inside Android");
}

// Called with the results of pcall/xpcall/coroutine.resume: re-raises a pending stop request.
static int api_stop_check(lua_State* L) {
    ScriptCtx* ctx = ctx_of(L);
    if (ctx->stop != STOP_NONE || soulx_interrupted()) return raise_stop(L, ctx, STOP_INTERRUPT);
    return lua_gettop(L);
}

static const char k_prelude[] =
    "local check = ...\n"
    "local pcall, xpcall, resume = pcall, xpcall, coroutine.resume\n"
    "_G.pcall = function(...) return check(pcall(...)) end\n"
    "_G.xpcall = function(...) return check(xpcall(...)) end\n"
    "coroutine.resume = function(...) return check(resume(...)) end\n";

// Helper library: pure Lua on top of the API (flow control, colour/grid vision, drag paths). Loaded as
// globals after the API is registered, so a script can use these without require(). Looks up find/exists/
// tap/color/capture/touch_* as globals at call time, so a script may override them (handy for testing).
static const char k_helpers[] =
    "local function first_match(imgs, opts)\n"
    "  for k, img in pairs(imgs) do local x, y = find(img, opts); if x then return k, x, y end end\n"
    "end\n"
    "function find_any(imgs, opts) return first_match(imgs, opts) end\n"            // -> key, x, y or nil
    "function wait_any(imgs, t, opts)\n"                                            // wait for the first of several
    "  local dl = millis() + (t or 10) * 1000\n"
    "  while true do\n"
    "    local k, x, y = first_match(imgs, opts)\n"
    "    if k ~= nil then return k, x, y end\n"
    "    if millis() >= dl then return nil end\n"
    "    sleep(200)\n"
    "  end\n"
    "end\n"
    "function tap_until(image, done, t, opts)\n"                                    // tap until a condition holds
    "  local dl = millis() + (t or 15) * 1000\n"
    "  while true do\n"
    "    if type(done) == 'function' then if done() then return true end\n"
    "    elseif done ~= nil then if exists(done, opts) then return true end\n"
    "    else if not exists(image, opts) then return true end end\n"
    "    tap(image, opts)\n"
    "    if millis() >= dl then return false end\n"
    "    sleep(500)\n"
    "  end\n"
    "end\n"
    "function retry(fn, times, pause)\n"                                            // run fn until truthy, up to N
    "  times = times or 3\n"
    "  for i = 1, times do local r = fn(i); if r then return r end; if i < times then sleep(pause or 500) end end\n"
    "end\n"
    "function assert_screen(image, opts, msg)\n"
    "  if not exists(image, opts) then error(msg or ('assert_screen: ' .. tostring(image) .. ' not on screen'), 2) end\n"
    "end\n"
    "function color_dist(a, b)\n"                                                   // squared, green-weighted
    "  local dr = ((a >> 16) & 255) - ((b >> 16) & 255)\n"
    "  local dg = ((a >> 8) & 255) - ((b >> 8) & 255)\n"
    "  local db = (a & 255) - (b & 255)\n"
    "  return dr*dr*2 + dg*dg*3 + db*db\n"
    "end\n"
    "function classify(rgb, palette, maxd)\n"                                       // nearest palette colour -> key
    "  local best, bd\n"
    "  for name, ref in pairs(palette) do\n"
    "    local d = color_dist(rgb, ref)\n"
    "    if not bd or d < bd then best, bd = name, d end\n"
    "  end\n"
    "  if maxd and bd and bd > maxd then return nil, bd end\n"
    "  return best, bd\n"
    "end\n"
    "function read_grid(x0, y0, dx, dy, cols, rows, sample)\n"                      // one capture -> board[r][c]=rgb
    "  local img = capture()\n"                                                     // sample>0: average a +-sample
    "  local iw, ih = img:size()\n"                                                 // px box (robust to orb symbols)
    "  local function at(x, y) return img:color(math.min(iw-1, math.max(0, x)), math.min(ih-1, math.max(0, y))) end\n"
    "  local b = {}\n"
    "  for r = 1, rows do\n"
    "    b[r] = {}\n"
    "    for c = 1, cols do\n"
    "      local cx, cy = x0 + (c-1)*dx, y0 + (r-1)*dy\n"
    "      if sample and sample > 0 then\n"
    "        local R, G, B, W = 0, 0, 0, 0\n"                                        // saturation-weighted average:
    "        local pr, pg, pb, pn = 0, 0, 0, 0\n"                                    // colourful pixels dominate so a
    "        local step = math.max(1, sample // 3)\n"                               // white glow/symbol on a tile is
    "        for yy = -sample, sample, step do for xx = -sample, sample, step do\n" // ignored (robust to variants)
    "          local p = at(math.floor(cx+xx+0.5), math.floor(cy+yy+0.5))\n"
    "          local rr, gg, bb = (p>>16)&255, (p>>8)&255, p&255\n"
    "          local sat = math.max(rr, gg, bb) - math.min(rr, gg, bb)\n"
    "          R = R + rr*sat; G = G + gg*sat; B = B + bb*sat; W = W + sat\n"
    "          pr = pr + rr; pg = pg + gg; pb = pb + bb; pn = pn + 1\n"
    "        end end\n"
    "        if W > 0 then\n"
    "          b[r][c] = ((R//W)<<16) | ((G//W)<<8) | (B//W)\n"
    "        else\n"
    "          b[r][c] = ((pr//pn)<<16) | ((pg//pn)<<8) | (pb//pn)\n"                // all grey: plain average
    "        end\n"
    "      else\n"
    "        b[r][c] = at(math.floor(cx+0.5), math.floor(cy+0.5))\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  return b\n"
    "end\n"
    "function read_board(x0, y0, dx, dy, cols, rows, palette, maxd, sample)\n"      // read_grid + classify each cell
    "  local g = read_grid(x0, y0, dx, dy, cols, rows, sample)\n"
    "  local b = {}\n"
    "  for r = 1, #g do b[r] = {}; for c = 1, #g[r] do b[r][c] = classify(g[r][c], palette, maxd) end end\n"
    "  return b\n"
    "end\n";

// Helper library, part 2 (split so neither embedded-Lua literal exceeds the C99 4095-char limit).
static const char k_helpers2[] =
    "function hue(rgb)\n"                                                           // 0..360 hue, or nil for grey
    "  local r, g, b = (rgb>>16)&255, (rgb>>8)&255, rgb&255\n"
    "  local mx, mn = math.max(r, g, b), math.min(r, g, b)\n"
    "  local d = mx - mn\n"
    "  if d == 0 then return nil end\n"
    "  local h\n"
    "  if mx == r then h = ((g - b) / d) % 6\n"
    "  elseif mx == g then h = (b - r) / d + 2\n"
    "  else h = (r - g) / d + 4 end\n"
    "  return (h * 60) % 360\n"
    "end\n"
    "function classify_hue(rgb, palette, max_deg)\n"                               // nearest hue -> key (robust to
    "  local h = hue(rgb)\n"                                                        // brightness/saturation, so a
    "  if not h then return nil end\n"                                              // glowing or framed tile still
    "  local best, bd\n"                                                            // matches its base colour)
    "  for name, href in pairs(palette) do\n"
    "    local diff = math.abs(h - href) % 360\n"
    "    if diff > 180 then diff = 360 - diff end\n"
    "    if not bd or diff < bd then best, bd = name, diff end\n"
    "  end\n"
    "  if max_deg and bd and bd > max_deg then return nil end\n"
    "  return best\n"
    "end\n"
    "function classify_cell(region, templates, opts)\n"                            // best template match in a region
    "  local o = {}\n"
    "  if opts then for k, v in pairs(opts) do o[k] = v end end\n"
    "  o.region = region\n"
    "  local best, bs\n"
    "  for name, img in pairs(templates) do\n"
    "    local x, y, score = find(img, o)\n"
    "    if x and (not bs or score > bs) then best, bs = name, score end\n"
    "  end\n"
    "  return best, bs\n"
    "end\n"
    "function read_number(region)\n"                                     // first number from read_text (OCR)
    "  local s = read_text(region)\n"
    "  if not s then return nil end\n"
    "  s = s:gsub(',', '')\n"
    "  local num = s:match('%d+%.?%d*')\n"
    "  return num and tonumber(num) or nil\n"
    "end\n"
    "function find_scaled(image, scales, opts)\n"                        // best match across template scales
    "  scales = scales or {0.8, 0.9, 1.0, 1.1, 1.2}\n"
    "  local bx, by, bs, bsc\n"
    "  for _, sc in ipairs(scales) do\n"
    "    local o = {}\n"
    "    if opts then for k, v in pairs(opts) do o[k] = v end end\n"
    "    o.scale = sc\n"
    "    local x, y, score = find(image, o)\n"
    "    if x and (not bs or score > bs) then bx, by, bs, bsc = x, y, score, sc end\n"
    "  end\n"
    "  if bx then return bx, by, bs, bsc end\n"
    "  return nil\n"
    "end\n"
    "function drag_path(points, opts)\n"                                            // drag finger 0 through points
    "  opts = opts or {}\n"
    "  if #points < 1 then return false end\n"
    "  local step = opts.step_ms or 16\n"
    "  local maxpx = opts.max_step_px\n"                                            // subdivide long hops so a
    "  touch_down(points[1][1], points[1][2])\n"                                    // dragged tile keeps up (nil=off)
    "  if (opts.hold_ms or 60) > 0 then sleep(opts.hold_ms or 60) end\n"
    "  local cx, cy = points[1][1], points[1][2]\n"
    "  for i = 2, #points do\n"
    "    local tx, ty = points[i][1], points[i][2]\n"
    "    if maxpx and maxpx > 0 then\n"
    "      local dx, dy = tx - cx, ty - cy\n"
    "      local n = math.max(1, math.ceil(math.sqrt(dx*dx + dy*dy) / maxpx))\n"
    "      for k = 1, n do\n"
    "        touch_move(math.floor(cx + dx*k/n + 0.5), math.floor(cy + dy*k/n + 0.5))\n"
    "        if step > 0 then sleep(step) end\n"
    "      end\n"
    "    else\n"
    "      touch_move(tx, ty)\n"
    "      if step > 0 then sleep(step) end\n"
    "    end\n"
    "    cx, cy = tx, ty\n"
    "  end\n"
    "  touch_up()\n"
    "  return true\n"
    "end\n";

// Live parameter panel: a script declares controls (param_slider/toggle/choice/number/button); when it runs
// inside the SoulX window a small panel appears and the user tweaks them live; run headless (no window) they just
// return their defaults. Rendezvous is the directory in $SOULX_UI_DIR (the window sets it, creates it, and removes
// it when the script ends): the script writes 'params' (declarations, atomic via rename) and reads 'values' (the
// window writes them). All reads are defensive — a missing or malformed file yields defaults, never an error, so a
// broken panel can never crash the script, and a broken script can never crash the window.
static const char k_ui[] =
    "local _dir = os.getenv('SOULX_UI_DIR')\n"
    "local _order, _line = {}, {}\n"
    "local function _declare(key, parts)\n"
    "  if not _dir then return end\n"
    "  if _line[key] == nil then _order[#_order+1] = key end\n"
    "  _line[key] = table.concat(parts, '\\t')\n"
    "  local f = io.open(_dir .. '/params.tmp', 'w')\n"
    "  if not f then return end\n"
    "  for _, k in ipairs(_order) do f:write(_line[k], '\\n') end\n"
    "  f:close()\n"
    "  os.rename(_dir .. '/params.tmp', _dir .. '/params')\n"
    "end\n"
    "local _vals, _vt = {}, -1\n"
    "local function _values()\n"
    "  if not _dir then return _vals end\n"
    "  local now = millis()\n"
    "  if _vt >= 0 and now - _vt < 100 then return _vals end\n"           // re-read at most every 100 ms
    "  _vt = now\n"
    "  local f = io.open(_dir .. '/values', 'r')\n"
    "  if not f then return _vals end\n"
    "  local t = {}\n"
    "  for line in f:lines() do local k, v = line:match('^(.-)\\t(.*)$'); if k then t[k] = v end end\n"
    "  f:close()\n"
    "  _vals = t\n"
    "  return t\n"
    "end\n"
    "local function _raw(name) return _values()[name] end\n"
    "function param_slider(label, lo, hi, default, step)\n"
    "  default = default or lo\n"
    "  _declare(label, {'slider', label, label, default, lo, hi, step or 1})\n"
    "  return { name = label, kind = 'slider', default = default,\n"
    "    value = function(self) return tonumber(_raw(self.name)) or self.default end }\n"
    "end\n"
    "function param_number(label, lo, hi, default, step)\n"
    "  default = default or lo or 0\n"
    "  _declare(label, {'number', label, label, default, lo or '', hi or '', step or 1})\n"
    "  return { name = label, kind = 'number', default = default,\n"
    "    value = function(self) return tonumber(_raw(self.name)) or self.default end }\n"
    "end\n"
    "function param_toggle(label, default)\n"
    "  local d = default and true or false\n"
    "  _declare(label, {'toggle', label, label, d and 1 or 0})\n"
    "  local function val(self)\n"
    "    local r = _raw(self.name)\n"
    "    if r == '1' or r == 'true' then return true elseif r == '0' or r == 'false' then return false end\n"
    "    return self.default\n"
    "  end\n"
    "  return { name = label, kind = 'toggle', default = d, value = val, on = val }\n"
    "end\n"
    "function param_choice(label, options, default)\n"
    "  options = options or {}\n"
    "  default = default or options[1]\n"
    "  local parts = {'choice', label, label, default}\n"
    "  for _, o in ipairs(options) do parts[#parts+1] = tostring(o) end\n"
    "  _declare(label, parts)\n"
    "  return { name = label, kind = 'choice', default = default,\n"
    "    value = function(self) local r = _raw(self.name); if r == nil then return self.default end; return r end }\n"
    "end\n"
    "function param_button(label)\n"                                      // :pressed() is true once per click
    "  _declare(label, {'button', label, label})\n"
    "  return { name = label, kind = 'button', seen = 0,\n"
    "    pressed = function(self)\n"
    "      local n = tonumber(_raw(self.name)) or 0\n"
    "      if n > self.seen then self.seen = n; return true end\n"
    "      return false\n"
    "    end }\n"
    "end\n";

// ---------------------------------------------------------------------------
// Interpreter setup
// ---------------------------------------------------------------------------

static const luaL_Reg k_api[] = {
    { "launch", api_launch },
    { "stop_app", api_stop_app },
    { "install", api_install },
    { "current_app", api_current_app },
    { "tap", api_tap },
    { "long_press", api_long_press },
    { "swipe", api_swipe },
    { "touch_down", api_touch_down },
    { "touch_move", api_touch_move },
    { "touch_up", api_touch_up },
    { "key", api_key },
    { "text", api_text },
    { "sleep", api_sleep },
    { "speed", api_speed },
    { "iteration", api_iteration },
    { "millis", api_millis },
    { "log", api_log },
    { "capture", api_capture },
    { "load_image", api_load_image },
    { "color", api_color },
    { "is_color", api_is_color },
    { "find", api_find },
    { "find_all", api_find_all },
    { "exists", api_exists },
    { "read_text", api_read_text },
    { "wait", api_wait },
    { "wait_gone", api_wait_gone },
    { "screenshot", api_screenshot },
    { "screen_size", api_screen_size },
    { "use", api_use },
    { "console", api_console },
    { "shell", api_shell },
    { NULL, NULL }
};

static const luaL_Reg k_image_meta[] = {
    { "__gc", img_gc },
    { "__close", img_gc },
    { "__tostring", img_tostring },
    { NULL, NULL }
};

static const luaL_Reg k_image_methods[] = {
    { "color", img_color },
    { "find", img_find },
    { "find_all", img_find_all },
    { "crop", img_crop },
    { "save", img_save },
    { "size", img_size },
    { NULL, NULL }
};

static void api_register(lua_State* L, ScriptCtx* ctx) {
    // Image and memory box types
    luaL_newmetatable(L, IMAGE_MT);
    luaL_setfuncs(L, k_image_meta, 0);
    luaL_newlib(L, k_image_methods);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, IMAGE_MT);
    lua_setfield(L, -2, "__metatable");                 // keeps __gc out of the script's reach
    lua_pop(L, 1);
    luaL_newmetatable(L, MEMORY_MT);
    lua_pushcfunction(L, memory_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, MEMORY_MT);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &k_templates);

    // The API as globals, and as soulx.* (handy when a local variable shadows a global)
    lua_pushglobaltable(L);
    luaL_setfuncs(L, k_api, 0);
    lua_pop(L, 1);
    luaL_newlib(L, k_api);
    lua_pushliteral(L, SOULX_VERSION_STRING);
    lua_setfield(L, -2, "version");
    lua_pushvalue(L, -1);
    lua_setglobal(L, "soul");                   // the name before the SoulX rename: older scripts keep working
    lua_setglobal(L, "soulx");

    // No host commands; os.exit ends the script only
    lua_getglobal(L, "os");
    lua_pushcfunction(L, api_os_exit);
    lua_setfield(L, -2, "exit");
    lua_pushcfunction(L, api_os_execute);
    lua_setfield(L, -2, "execute");
    lua_pop(L, 1);
    lua_getglobal(L, "io");
    lua_pushcfunction(L, api_io_popen);
    lua_setfield(L, -2, "popen");
    lua_pop(L, 1);

    // No native code: remove package.loadlib, package.cpath and the C module searchers, so that
    // require() can only find .lua files next to the script and a script can never load a .so into
    // the soulx process. This matches docs/SCRIPTING.md ("scripts never run programs on your Mac")
    // and holds even if Lua was built with LUA_USE_DLOPEN (the C searchers are entries 3 and 4 of
    // package.searchers; a nil at 3 also stops require() before 4).
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, "loadlib");
        lua_pushliteral(L, "");
        lua_setfield(L, -2, "cpath");
        lua_getfield(L, -1, "searchers");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            lua_rawseti(L, -2, 3);                      // the C-path (package.cpath) searcher
            lua_pushnil(L);
            lua_rawseti(L, -2, 4);                      // the all-in-one / C-root searcher
        }
        lua_pop(L, 1);                                  // searchers
    }
    lua_pop(L, 1);                                      // package

    // pcall, xpcall and coroutine.resume cannot swallow Ctrl+C or os.exit
    if (luaL_loadbufferx(L, k_prelude, sizeof(k_prelude) - 1, "=soulx", "t") != LUA_OK) lua_error(L);
    lua_pushcfunction(L, api_stop_check);
    lua_call(L, 1, 0);

    // Helper library (flow / vision / grid / drag) as globals, in two parts
    if (luaL_loadbufferx(L, k_helpers, sizeof(k_helpers) - 1, "=soulx-helpers", "t") != LUA_OK) lua_error(L);
    lua_call(L, 0, 0);
    if (luaL_loadbufferx(L, k_helpers2, sizeof(k_helpers2) - 1, "=soulx-helpers2", "t") != LUA_OK) lua_error(L);
    lua_call(L, 0, 0);

    // Live parameter panel API (param_slider/toggle/choice/number/button)
    if (luaL_loadbufferx(L, k_ui, sizeof(k_ui) - 1, "=soulx-ui", "t") != LUA_OK) lua_error(L);
    lua_call(L, 0, 0);

    // args[0] = script path, args[1..n] = arguments (also as the standard name 'arg')
    lua_createtable(L, ctx->argc, 1);
    lua_pushstring(L, ctx->script);
    lua_rawseti(L, -2, 0);
    for (int i = 0; i < ctx->argc; i++) {
        lua_pushstring(L, ctx->argv[i] ? ctx->argv[i] : "");
        lua_rawseti(L, -2, i + 1);
    }
    lua_pushvalue(L, -1);
    lua_setglobal(L, "arg");
    lua_setglobal(L, "args");

    // math.random: a different sequence on every run
    lua_getglobal(L, "math");
    lua_getfield(L, -1, "randomseed");
    lua_Integer seed = (lua_Integer)time(NULL) ^ ((lua_Integer)getpid() << 16);
    seed ^= (lua_Integer)(soulx_now_ms() * 1000.0);
    lua_pushinteger(L, seed);
    lua_call(L, 1, 0);
    lua_pop(L, 1);

    // require() finds modules next to the script
    if (!strpbrk(ctx->dir, ";?")) {
        lua_getglobal(L, "package");
        lua_getfield(L, -1, "path");
        const char* old = lua_tostring(L, -1);
        lua_pushfstring(L, "%s/?.lua;%s/?/init.lua;%s", ctx->dir, ctx->dir, old ? old : "");
        lua_setfield(L, -3, "path");
        lua_pop(L, 2);
    }

    // Sandbox (AI code from `soulx mcp`): nothing that reaches the Mac beyond the scripts folder (path_arg) - no
    // io, no os file/env/process functions, no loading of other code (load could take binary chunks), no
    // emulator console (it can write files on the Mac). Taps, swipes, keys, text, screen and shell() (inside
    // Android) stay.
    if (sandbox_dir()) {
        static const char* const k_removed[] = { "io", "dofile", "loadfile", "load", "require", "package", NULL };
        for (int i = 0; k_removed[i]; i++) {
            lua_pushnil(L);
            lua_setglobal(L, k_removed[i]);
        }
        lua_getglobal(L, "os");
        lua_newtable(L);                                // keep only the clock functions
        static const char* const k_kept[] = { "clock", "time", "date", "difftime", "exit", NULL };
        for (int i = 0; k_kept[i]; i++) {
            lua_getfield(L, -2, k_kept[i]);
            lua_setfield(L, -2, k_kept[i]);
        }
        lua_setglobal(L, "os");
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_setglobal(L, "console");
        lua_getglobal(L, "soulx");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            lua_setfield(L, -2, "console");
        }
        lua_pop(L, 1);
    }
}

// Message handler: error message + traceback
static int api_msgh(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (!msg) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) {
            msg = lua_tostring(L, -1);
        } else {
            msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
        }
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

// Runs in protected mode: sets up the interpreter, loads and runs the script. The outcome is
// left in the ScriptCtx.
static int script_main(lua_State* L) {
    ScriptCtx* ctx = lua_touserdata(L, 1);
    lua_settop(L, 0);
    luaL_openlibs(L);
    api_register(L, ctx);

    int st = luaL_loadfilex(L, ctx->script, "t");
    if (st != LUA_OK) {
        ctx->status = st;
        ctx->message = message_dup(lua_tostring(L, -1));
        return 0;
    }
    lua_pushcfunction(L, api_msgh);
    lua_insert(L, -2);                                  // handler below the chunk
    int handler = lua_gettop(L) - 1;
    luaL_checkstack(L, ctx->argc + 1, "too many script arguments");
    for (int i = 0; i < ctx->argc; i++) {               // the arguments are also the chunk's '...'
        lua_pushstring(L, ctx->argv[i] ? ctx->argv[i] : "");
    }

    lua_sethook(L, api_hook, LUA_MASKCOUNT, SCRIPT_HOOK_COUNT);
    st = lua_pcall(L, ctx->argc, 0, handler);
    lua_sethook(L, NULL, 0, 0);
    ctx->status = st;
    if (st != LUA_OK) ctx->message = message_dup(lua_tostring(L, -1));
    return 0;
}

// Absolute folder of the script (symbolic links resolved).
static int script_folder(const char* script, char* dir, size_t cap) {
    char real[PATH_MAX];
    if (!realpath(script, real)) return -1;
    char* slash = strrchr(real, '/');
    if (!slash) {
        errno = EINVAL;
        return -1;
    }
    if (slash == real) slash++;                         // the script is in "/"
    *slash = '\0';
    int n = snprintf(dir, cap, "%s", real);
    if (n < 0 || (size_t)n >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int soulx_script_run_ex(const char* instance, const char* script_path, int argc, char** argv, int repeat, double speed) {
    if (!script_path || !script_path[0]) {
        soulx_log(SOULX_LOG_ERROR, "no script given");
        return 1;
    }
    if (argc < 0 || !argv) argc = 0;
    if (argc > SCRIPT_ARGS_MAX) {
        soulx_log(SOULX_LOG_ERROR, "too many script arguments (at most %d)", SCRIPT_ARGS_MAX);
        return 1;
    }

    ScriptCtx* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        soulx_log(SOULX_LOG_ERROR, "out of memory");
        return 1;
    }
    ctx->t0 = soulx_now_ms();
    ctx->script = script_path;
    ctx->argc = argc;
    ctx->argv = argv;
    ctx->status = LUA_OK;
    ctx->speed = (speed >= 0.1 && speed <= 10.0) ? speed : 1.0;
    bool loop = (repeat <= 0);                          // repeat <= 0 means "until Ctrl+C / stop"
    ctx->passes = loop ? 0 : repeat;
    if (script_folder(script_path, ctx->dir, sizeof(ctx->dir)) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot open %s: %s", script_path, strerror(errno));
        free(ctx);
        return 1;
    }

    const SoulXLuaDeviceOps* ops = atomic_load(&g_device_ops);
    ctx->ops = ops ? ops : &k_real_ops;
    char err[256] = "";
    ctx->dev = ctx->ops->open(instance, ctx->instance, sizeof(ctx->instance), err, sizeof(err));
    if (!ctx->dev) {
        soulx_log(SOULX_LOG_ERROR, "%s", err[0] ? err : "cannot open the instance");
        free(ctx);
        return 1;
    }
    if (!ctx->instance[0]) {
        snprintf(ctx->instance, sizeof(ctx->instance), "%s", (instance && *instance) ? instance : SOULX_DEFAULT_INSTANCE);
    }

    // The device is opened once; each pass gets a fresh Lua state (so a repeated run starts clean).
    int code = 0;
    int pass = 0;
    while (loop || pass < repeat) {
        pass++;
        ctx->pass = pass;
        ctx->stop = STOP_NONE;
        ctx->status = LUA_OK;
        ctx->exit_code = 0;
        free(ctx->message);
        ctx->message = NULL;

        lua_State* L = luaL_newstate();
        if (!L) {
            soulx_log(SOULX_LOG_ERROR, "cannot start the Lua interpreter: out of memory");
            code = 1;
            break;
        }
        *(ScriptCtx**)lua_getextraspace(L) = ctx;

        if (ctx->passes == 0) {
            soulx_log(SOULX_LOG_INFO, "pass %d: running %s on %s (Ctrl+C stops it)", pass, script_path, ctx->instance);
        } else if (ctx->passes > 1) {
            soulx_log(SOULX_LOG_INFO, "pass %d/%d: running %s on %s (Ctrl+C stops it)",
                     pass, ctx->passes, script_path, ctx->instance);
        } else {
            soulx_log(SOULX_LOG_INFO, "running %s on %s (Ctrl+C stops it)", script_path, ctx->instance);
        }

        lua_pushcfunction(L, script_main);
        lua_pushlightuserdata(L, ctx);
        int st = lua_pcall(L, 1, 0, 0);
        if (st != LUA_OK) {                             // the interpreter setup failed (out of memory)
            ctx->status = st;
            if (!ctx->message) ctx->message = message_dup(lua_tostring(L, -1));
        }

        // Release held touches, then end this pass.
        release_touches(ctx);
        lua_close(L);                                   // runs the remaining finalizers (device still open)
        screen_drop(ctx);

        if (ctx->status == LUA_OK && ctx->stop == STOP_NONE) {   // the script ran to the end: do the next pass
            code = 0;
            if (loop || repeat > 1) soulx_log(SOULX_LOG_DEBUG, "pass %d done", pass);
            continue;
        }
        // os.exit(), an error or an interrupt ends the whole run.
        if (ctx->stop == STOP_EXIT) {
            code = ctx->exit_code;
        } else if (ctx->stop == STOP_INTERRUPT || soulx_interrupted()) {
            code = 130;
        } else {
            code = 1;
        }
        break;
    }

    ctx->ops->close(ctx->dev);
    ctx->dev = NULL;
    double secs = (soulx_now_ms() - ctx->t0) / 1000.0;

    if (code == 0) {
        if (ctx->passes != 1) report(SOULX_LOG_INFO, "script finished %d pass%s in %.1f s",
                                      pass, pass == 1 ? "" : "es", secs);
        else report(SOULX_LOG_INFO, "script finished in %.1f s", secs);
    } else if (ctx->stop == STOP_EXIT) {
        report(SOULX_LOG_WARN, "script exited with code %d after %.1f s", code, secs);
    } else if (code == 130) {
        if (ctx->message) soulx_log(SOULX_LOG_DEBUG, "%s", ctx->message);
        report(SOULX_LOG_WARN, "script stopped (Ctrl+C) after %.1f s", secs);
    } else if (ctx->passes == 1) {
        report(SOULX_LOG_ERROR, "script error: %s", ctx->message ? ctx->message : "unknown error");
    } else {
        report(SOULX_LOG_ERROR, "script error (pass %d): %s", pass, ctx->message ? ctx->message : "unknown error");
    }
    soulx_log(SOULX_LOG_DEBUG, "%lu screen capture%s", ctx->captures, ctx->captures == 1 ? "" : "s");

    free(ctx->message);
    free(ctx);
    return code;
}

int soulx_script_run(const char* instance, const char* script_path, int argc, char** argv) {
    return soulx_script_run_ex(instance, script_path, argc, argv, 1, 1.0);
}
