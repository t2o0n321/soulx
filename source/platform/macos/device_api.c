/*
 * SoulX Android Emulator - macOS backend: device API
 * The public SOULX_APIs beyond the instance lifecycle (display, recording, input, sensors, battery,
 * network, files, snapshots, macros, performance, adb), mapped onto SoulX's registry, the emulator
 * console and SoulX's adb. Every call resolves instance id -> registry -> running state first and
 * returns SOULX_ERR_NOT_FOUND / SOULX_ERR_NOT_RUNNING / SOULX_ERR_NOT_SUPPORTED / SOULX_ERR_NEEDS_RESTART
 * where they apply.
 */

#include <errno.h>
#include <libproc.h>
#include <limits.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "soulx_mac.h"

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

#define DEV_SHELL_TIMEOUT_MS    30000
#define DEV_FILE_TIMEOUT_MS     (10 * 60 * 1000)
#define DEV_ADB_TIMEOUT_MS      (10 * 60 * 1000)
#define DEV_ADB_MAX_CMD         8192
#define DEV_ADB_MAX_ARGS        64
#define DEV_SNAPSHOT_WAIT_MS    (3 * 60 * 1000)
#define DEV_TEXT_MAX            4096
#define DEV_COORD_MAX           100000
#define DEV_SCRIPT_SLOTS        64

// Same ranges as instance creation (engine.c).
#define DEV_RES_MIN             320
#define DEV_RES_MAX             3840
#define DEV_DPI_MIN             120
#define DEV_DPI_MAX             640

#define KEY_ACTION_DOWN         0       // Android KeyEvent.ACTION_DOWN / ACTION_UP
#define KEY_ACTION_UP           1

// ---------------------------------------------------------------------------
// Target resolution
// ---------------------------------------------------------------------------

typedef struct {
    SoulXInstanceConf conf;
    SoulXRunState st;
    char serial[32];
} DevTarget;

static bool fmt_ok(int n, size_t cap)
{
    return n >= 0 && (size_t)n < cap;
}

static int dev_find(int instance_id, SoulXInstanceConf* conf)
{
    if (instance_id <= 0 || soulx_reg_find_by_id(instance_id, conf) != 0) return SOULX_ERR_NOT_FOUND;
    return SOULX_OK;
}

static int dev_running(int instance_id, DevTarget* t)
{
    memset(t, 0, sizeof(*t));
    int rc = dev_find(instance_id, &t->conf);
    if (rc != SOULX_OK) return rc;
    if (!soulx_instance_running(t->conf.name, &t->st)) return SOULX_ERR_NOT_RUNNING;
    soulx_serial(t->st.port, t->serial, sizeof(t->serial));
    return SOULX_OK;
}

// Existing instance, feature not available on this backend.
static int dev_not_supported(int instance_id, const char* what)
{
    SoulXInstanceConf c;
    int rc = dev_find(instance_id, &c);
    if (rc != SOULX_OK) return rc;
    soulx_log(SOULX_LOG_DEBUG, "%s is not supported by the macOS backend", what);
    return SOULX_ERR_NOT_SUPPORTED;
}

// Squashes tool output into one printable line.
static const char* one_line(const char* src, size_t len, char* buf, size_t cap)
{
    size_t o = 0;
    bool space = false;
    for (size_t i = 0; src && i < len && o + 2 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = o > 0;
            continue;
        }
        if (c < 0x20 || c == 0x7F) continue;
        if (space) {
            buf[o++] = ' ';
            space = false;
        }
        buf[o++] = (char)c;
    }
    if (cap) buf[o] = '\0';
    return buf;
}

// Runs console commands over one connection, stopping at the first failure.
static int dev_console(const DevTarget* t, const char* const* cmds, int n)
{
    SoulXConsole* c = soulx_console_open(t->st.port);
    if (!c) {
        soulx_log(SOULX_LOG_ERROR, "cannot reach the emulator console of '%s' (port %d)", t->conf.name, t->st.port);
        return SOULX_ERROR;
    }
    int ret = SOULX_OK;
    for (int i = 0; i < n && ret == SOULX_OK; i++) {
        char reply[512];
        int rc = soulx_console_cmd(c, cmds[i], reply, sizeof(reply));
        if (rc != 0) {
            soulx_log(SOULX_LOG_ERROR, "console command '%s' failed: %s", cmds[i],
                     rc == 1 && reply[0] ? reply : "no reply from the emulator");
            ret = SOULX_ERROR;
        }
    }
    soulx_console_close(c);
    return ret;
}

static int dev_shell(const DevTarget* t, const char* cmd)
{
    char* out = NULL;
    char msg[256];
    int rc = soulx_adb_shell(t->serial, cmd, &out, DEV_SHELL_TIMEOUT_MS);
    if (rc != 0) {
        soulx_log(SOULX_LOG_ERROR, "'%s' failed on '%s': %s", cmd, t->conf.name,
                 one_line(out, out ? strlen(out) : 0, msg, sizeof(msg)));
    }
    free(out);
    return rc == 0 ? SOULX_OK : SOULX_ERROR;
}

// Absolute host path without whitespace/control characters (the console splits on whitespace).
static int console_path(const char* in, char* out, size_t cap)
{
    char cwd[PATH_MAX];
    if (!in || !in[0]) return -1;
    if (in[0] == '/') {
        if (!fmt_ok(snprintf(out, cap, "%s", in), cap)) return -1;
    } else if (!getcwd(cwd, sizeof(cwd)) || !fmt_ok(snprintf(out, cap, "%s/%s", cwd, in), cap)) {
        return -1;
    }
    for (const char* p = out; *p; p++) {
        if ((unsigned char)*p <= 0x20 || *p == 0x7F) return -1;
    }
    return 0;
}

// Guest path for adb push/pull: absolute, printable, bounded.
static bool remote_path_valid(const char* p)
{
    if (!p || p[0] != '/' || strlen(p) > 1024) return false;
    for (; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == 0x7F) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Input: one cached SoulXInput (persistent console connection) per instance
// ---------------------------------------------------------------------------

// g_input_lock guards the slot table (id, users, used_ms; in/pid/port only while users == 0). Each
// slot's own mutex serialises the operations on its instance, so a long swipe on one instance never
// holds up input to another.
typedef struct {
    int id;                      // instance id (0 = free slot)
    int users;                   // threads using or waiting for the slot (it is never evicted then)
    double used_ms;              // last claim; the least recently used idle slot is reused
    pthread_mutex_t op;          // held while an operation runs on the slot's connection
    SoulXInput* in;
    pid_t pid;                   // engine pid / console port the connection belongs to
    int port;
    unsigned down;               // fingers put down through soulx_send_touch/multitouch and not lifted
} DevInputSlot;

#define DEV_INPUT_BAD_ARG  -2    // DevInputFn: invalid arguments; another connection would not help

// 0 ok, -1 failed (retried once on a fresh connection), DEV_INPUT_BAD_ARG (reported, not retried).
typedef int (*DevInputFn)(SoulXInput* in, unsigned* down, void* arg);

static DevInputSlot g_input_slots[MAX_INSTANCES];
static pthread_mutex_t g_input_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_input_once = PTHREAD_ONCE_INIT;

static void input_slots_init(void)
{
    for (int i = 0; i < MAX_INSTANCES; i++) pthread_mutex_init(&g_input_slots[i].op, NULL);
}

// Claims the instance's slot, else a free one, else the least recently used idle one, and marks it
// in use. *evicted receives the connection of a slot taken over from another instance: the caller
// closes it outside the lock (closing lifts fingers through that engine's console).
static DevInputSlot* input_slot_claim(int id, SoulXInput** evicted)
{
    DevInputSlot* slot = NULL;
    DevInputSlot* lru = NULL;
    *evicted = NULL;
    pthread_mutex_lock(&g_input_lock);
    for (int i = 0; i < MAX_INSTANCES && !slot; i++) {
        if (g_input_slots[i].id == id) slot = &g_input_slots[i];
    }
    for (int i = 0; i < MAX_INSTANCES && !slot; i++) {
        DevInputSlot* s = &g_input_slots[i];
        if (s->users > 0) continue;
        if (s->id == 0) slot = s;
        else if (!lru || s->used_ms < lru->used_ms) lru = s;
    }
    if (!slot && lru) slot = lru;
    if (slot && slot->id != id) {
        *evicted = slot->in;
        slot->in = NULL;
        slot->pid = 0;
        slot->port = 0;
        slot->down = 0;
        slot->id = id;
    }
    if (slot) {
        slot->users++;
        slot->used_ms = soulx_now_ms();
    }
    pthread_mutex_unlock(&g_input_lock);
    return slot;
}

static void input_slot_release(DevInputSlot* s)
{
    pthread_mutex_lock(&g_input_lock);
    s->users--;
    pthread_mutex_unlock(&g_input_lock);
}

// Caller holds s->op.
static void input_slot_drop(DevInputSlot* s)
{
    if (s->in) soulx_input_close(s->in);         // lifts the fingers still down
    s->in = NULL;
    s->down = 0;
}

// Runs fn with the instance's cached input; a failure drops the connection and retries once on a
// fresh one (the usual cause is a console connection that died with an engine restart). Invalid
// arguments are not retried: that would only fail again and lift the fingers held meanwhile.
static int dev_with_input(int instance_id, DevInputFn fn, void* arg)
{
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    pthread_once(&g_input_once, input_slots_init);

    SoulXInput* evicted = NULL;
    DevInputSlot* s = input_slot_claim(t.conf.id, &evicted);
    if (evicted) soulx_input_close(evicted);
    if (!s) {
        soulx_log(SOULX_LOG_ERROR, "input injection failed on '%s': too many instances are driven at once", t.conf.name);
        return SOULX_ERROR;
    }

    int ret = SOULX_ERROR;
    pthread_mutex_lock(&s->op);
    if (s->in && (s->pid != t.st.pid || s->port != t.st.port)) input_slot_drop(s);   // instance restarted
    for (int attempt = 0; attempt < 2 && ret != SOULX_OK; attempt++) {
        if (!s->in) {
            s->in = soulx_input_open(t.conf.name);
            if (!s->in) break;
            s->pid = t.st.pid;
            s->port = t.st.port;
            s->down = 0;
        }
        int r = fn(s->in, &s->down, arg);
        if (r == 0) ret = SOULX_OK;
        else if (r == DEV_INPUT_BAD_ARG) break;
        else input_slot_drop(s);
        if (soulx_interrupted()) break;
    }
    pthread_mutex_unlock(&s->op);
    input_slot_release(s);
    if (ret != SOULX_OK) soulx_log(SOULX_LOG_ERROR, "input injection failed on '%s'", t.conf.name);
    return ret;
}

static bool coord_valid(int v)
{
    return v >= 0 && v <= DEV_COORD_MAX;
}

static bool touch_action_valid(int a)
{
    return a == SOULX_TOUCH_DOWN || a == SOULX_TOUCH_UP || a == SOULX_TOUCH_MOVE;
}

// ---------------------------------------------------------------------------
// Stored configuration edits
// ---------------------------------------------------------------------------

// Edits the instance config: 1 = changed, 0 = unchanged, -1 = invalid.
typedef int (*ConfEditFn)(SoulXInstanceConf* c, const void* arg);

// Applies an edit under the registry lock. Stopped instance: registry + AVD files -> SOULX_OK.
// Running instance (only if allowed): registry only, used at the next start -> SOULX_ERR_NEEDS_RESTART.
static int dev_update_conf(int instance_id, bool allow_running, ConfEditFn fn, const void* arg)
{
    int fd = soulx_lock_acquire();
    if (fd < 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot lock the instance registry");
        return SOULX_ERROR;
    }
    SoulXInstanceConf c;
    int ret;
    if (dev_find(instance_id, &c) != SOULX_OK) {
        ret = SOULX_ERR_NOT_FOUND;
        goto out;
    }
    bool running = soulx_instance_running(c.name, NULL);
    if (running && !allow_running) {
        ret = SOULX_ERR_NEEDS_RESTART;
        goto out;
    }
    int changed = fn(&c, arg);
    if (changed <= 0) {
        ret = changed < 0 ? SOULX_ERROR : SOULX_OK;
        goto out;
    }
    if (soulx_reg_save(&c) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot save the configuration of '%s'", c.name);
        ret = SOULX_ERROR;
        goto out;
    }
    if (running) {
        ret = SOULX_ERR_NEEDS_RESTART;
    } else if (soulx_avd_write(&c) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot update the device definition of '%s'", c.name);
        ret = SOULX_ERROR;
    } else {
        ret = SOULX_OK;
    }
out:
    soulx_lock_release(fd);
    return ret;
}

// ---------------------------------------------------------------------------
// Display and graphics
// ---------------------------------------------------------------------------

typedef struct {
    int width, height, dpi;
} ResolutionArg;

static int edit_resolution(SoulXInstanceConf* c, const void* arg)
{
    const ResolutionArg* r = arg;
    bool landscape = r->width > r->height;
    if (c->width == r->width && c->height == r->height && c->dpi == r->dpi && c->landscape == landscape) return 0;
    c->width = r->width;
    c->height = r->height;
    c->dpi = r->dpi;
    c->landscape = landscape;
    return 1;
}

// Changes the virtual panel (registry + config.ini). Stopped: SOULX_OK. Running: saved in the registry
// and applied when the instance starts next (SOULX_ERR_NEEDS_RESTART, like soulx_set_config). The
// running guest is left as it is: a `wm size` override would make screenshots differ from the panel
// that touch input maps to, and Android would keep it across reboots, out of sync with the config.
SOULX_API int soulx_set_resolution(int instance_id, int width, int height, int dpi)
{
    if (width < DEV_RES_MIN || width > DEV_RES_MAX || height < DEV_RES_MIN || height > DEV_RES_MAX ||
        dpi < DEV_DPI_MIN || dpi > DEV_DPI_MAX) {
        soulx_log(SOULX_LOG_ERROR, "invalid resolution %dx%d @ %d dpi (%d-%d pixels, %d-%d dpi)", width, height, dpi,
                 DEV_RES_MIN, DEV_RES_MAX, DEV_DPI_MIN, DEV_DPI_MAX);
        return SOULX_ERROR;
    }
    ResolutionArg r = { width, height, dpi };
    int rc = dev_update_conf(instance_id, true, edit_resolution, &r);
    if (rc == SOULX_ERR_NEEDS_RESTART) {
        soulx_log(SOULX_LOG_INFO, "the new screen (%dx%d @ %d dpi) takes effect when the instance is started again",
                 width, height, dpi);
    }
    return rc;
}

static int edit_gpu(SoulXInstanceConf* c, const void* arg)
{
    GraphicsBackend b = *(const GraphicsBackend*)arg;
    const char* gpu = "host";                       // OpenGL ES and Vulkan on the Mac's GPU
    if (b == GRAPHICS_BACKEND_SOFTWARE) gpu = "software";
    else if (b == GRAPHICS_BACKEND_OPENGL && (!strcmp(c->gpu, "host-novulkan") || !strcmp(c->gpu, "metal"))) return 0;
    if (strcmp(c->gpu, gpu) == 0) return 0;
    if (!fmt_ok(snprintf(c->gpu, sizeof(c->gpu), "%s", gpu), sizeof(c->gpu))) return -1;
    return 1;
}

// OpenGL / Vulkan -> host (the Mac's GPU), software -> SwiftShader; DirectX does not exist on macOS.
SOULX_API int soulx_set_graphics_backend(int instance_id, GraphicsBackend backend)
{
    switch (backend) {
    case GRAPHICS_BACKEND_OPENGL:
    case GRAPHICS_BACKEND_VULKAN:
    case GRAPHICS_BACKEND_SOFTWARE:
        return dev_update_conf(instance_id, true, edit_gpu, &backend);
    case GRAPHICS_BACKEND_DIRECTX11:
    case GRAPHICS_BACKEND_DIRECTX12:
        return dev_not_supported(instance_id, "DirectX");
    default:
        return SOULX_ERROR;
    }
}

// The Android Emulator has no frame-rate cap and the instance configuration has no field for one.
SOULX_API int soulx_set_fps_limit(int instance_id, int fps)
{
    (void)fps;
    return dev_not_supported(instance_id, "an FPS limit");
}

SOULX_API int soulx_take_screenshot(int instance_id, const char* path)
{
    if (!path || !path[0]) return SOULX_ERROR;
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    return soulx_screen_save_png(t.serial, path) == 0 ? SOULX_OK : SOULX_ERROR;
}

// Console `screenrecord`: WebM on the host, written by the engine (absolute path; at most 3 minutes).
SOULX_API int soulx_start_recording(int instance_id, const char* path)
{
    char abs[PATH_MAX], cmd[PATH_MAX + 32], dir[PATH_MAX];
    struct stat sb;
    if (console_path(path, abs, sizeof(abs)) != 0 || strlen(abs) < 6 ||
        strcasecmp(abs + strlen(abs) - 5, ".webm") != 0) {
        soulx_log(SOULX_LOG_ERROR, "recording: the path must end in .webm and contain no spaces");
        return SOULX_ERROR;
    }
    memcpy(dir, abs, strlen(abs) + 1);
    *strrchr(dir, '/') = '\0';
    if (dir[0] && (stat(dir, &sb) != 0 || !S_ISDIR(sb.st_mode))) {
        soulx_log(SOULX_LOG_ERROR, "recording: directory %s does not exist", dir);
        return SOULX_ERROR;
    }
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    if (!fmt_ok(snprintf(cmd, sizeof(cmd), "screenrecord start %s", abs), sizeof(cmd))) return SOULX_ERROR;
    const char* cmds[] = { cmd };
    return dev_console(&t, cmds, 1);
}

SOULX_API int soulx_stop_recording(int instance_id)
{
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    const char* cmds[] = { "screenrecord stop" };
    return dev_console(&t, cmds, 1);
}

// ---------------------------------------------------------------------------
// Input (logical screen pixels, same as screenshots)
// ---------------------------------------------------------------------------

typedef struct {
    int finger, action, x, y;
} TouchArg;

typedef struct {
    const int* x;
    const int* y;
    const int* actions;
    int count;
} MultiTouchArg;

typedef struct {
    int x1, y1, x2, y2, ms;
} SwipeArg;

typedef enum {
    GESTURE_TAP, GESTURE_DOUBLE_TAP, GESTURE_LONG_PRESS,
    GESTURE_SWIPE_UP, GESTURE_SWIPE_DOWN, GESTURE_SWIPE_LEFT, GESTURE_SWIPE_RIGHT,
    GESTURE_PINCH_IN, GESTURE_PINCH_OUT,
    GESTURE_BACK, GESTURE_HOME, GESTURE_RECENTS
} DevGesture;

static const struct {
    const char* name;
    DevGesture gesture;
} k_gestures[] = {
    { "tap", GESTURE_TAP }, { "double_tap", GESTURE_DOUBLE_TAP }, { "long_press", GESTURE_LONG_PRESS },
    { "swipe_up", GESTURE_SWIPE_UP }, { "swipe_down", GESTURE_SWIPE_DOWN },
    { "swipe_left", GESTURE_SWIPE_LEFT }, { "swipe_right", GESTURE_SWIPE_RIGHT },
    { "pinch_in", GESTURE_PINCH_IN }, { "zoom_out", GESTURE_PINCH_IN },
    { "pinch_out", GESTURE_PINCH_OUT }, { "zoom_in", GESTURE_PINCH_OUT },
    { "back", GESTURE_BACK }, { "home", GESTURE_HOME }, { "recents", GESTURE_RECENTS },
    { "app_switch", GESTURE_RECENTS },
};

// A point where a finger can go down: on the screen (its far edge included, like input.c).
static int point_ok(SoulXInput* in, int x, int y)
{
    int w = 0, h = 0;
    if (soulx_input_screen_size(in, &w, &h) != 0 || w <= 0 || h <= 0) return -1;
    if (x < 0 || y < 0 || x > w || y > h) {
        soulx_log(SOULX_LOG_ERROR, "point %d,%d is outside the screen (%dx%d)", x, y, w, h);
        return DEV_INPUT_BAD_ARG;
    }
    return 0;
}

// One touch event; *down tracks the fingers this API put down on the connection.
static int touch_event(SoulXInput* in, unsigned* down, int finger, int action, int x, int y)
{
    unsigned bit = 1u << finger;
    if (action == SOULX_TOUCH_MOVE && !(*down & bit)) {
        soulx_log(SOULX_LOG_ERROR, "finger %d is not down (send SOULX_TOUCH_DOWN first)", finger);
        return DEV_INPUT_BAD_ARG;
    }
    if (action == SOULX_TOUCH_DOWN) {
        int rc = point_ok(in, x, y);
        if (rc != 0) return rc;
    }
    if (soulx_input_touch(in, finger, action, x, y) != 0) return -1;
    if (action == SOULX_TOUCH_DOWN) *down |= bit;
    else if (action == SOULX_TOUCH_UP) *down &= ~bit;
    return 0;
}

static int op_touch(SoulXInput* in, unsigned* down, void* arg)
{
    const TouchArg* a = arg;
    return touch_event(in, down, a->finger, a->action, a->x, a->y);
}

static int op_multitouch(SoulXInput* in, unsigned* down, void* arg)
{
    const MultiTouchArg* a = arg;
    for (int i = 0; i < a->count; i++) {        // all pointers are checked before any is sent
        if (a->actions[i] == SOULX_TOUCH_MOVE && !(*down & (1u << i))) {
            soulx_log(SOULX_LOG_ERROR, "finger %d is not down (send SOULX_TOUCH_DOWN first)", i);
            return DEV_INPUT_BAD_ARG;
        }
        int rc = a->actions[i] == SOULX_TOUCH_DOWN ? point_ok(in, a->x[i], a->y[i]) : 0;
        if (rc != 0) return rc;
    }
    for (int i = 0; i < a->count; i++) {
        int rc = touch_event(in, down, i, a->actions[i], a->x[i], a->y[i]);
        if (rc != 0) return rc;
    }
    return 0;
}

static int op_swipe(SoulXInput* in, unsigned* down, void* arg)
{
    const SwipeArg* a = arg;
    (void)down;
    int rc = point_ok(in, a->x1, a->y1);
    if (rc == 0) rc = point_ok(in, a->x2, a->y2);
    if (rc != 0) return rc;
    return soulx_input_swipe(in, a->x1, a->y1, a->x2, a->y2, a->ms);
}

static int op_key(SoulXInput* in, unsigned* down, void* arg)
{
    (void)down;
    return soulx_input_key(in, (const char*)arg);
}

static int op_text(SoulXInput* in, unsigned* down, void* arg)
{
    (void)down;
    return soulx_input_text(in, (const char*)arg);
}

// Two fingers moving apart (outward) or together around the screen centre.
static int pinch(SoulXInput* in, int w, int h, bool outward)
{
    const int steps = 20;
    int cx = w / 2, cy = h / 2;
    int span = w < h ? w : h;
    int from = outward ? span / 10 : span * 2 / 5;
    int to = outward ? span * 2 / 5 : span / 10;
    int d = from;
    if (soulx_input_touch(in, 0, SOULX_TOUCH_DOWN, cx - from, cy) != 0) return -1;
    if (soulx_input_touch(in, 1, SOULX_TOUCH_DOWN, cx + from, cy) != 0) {
        soulx_input_touch(in, 0, SOULX_TOUCH_UP, cx - from, cy);
        return -1;
    }
    int rc = 0;
    for (int i = 1; i <= steps && rc == 0; i++) {
        if (soulx_sleep_ms(16) != 0) {
            rc = -1;
            break;
        }
        d = from + (to - from) * i / steps;
        if (soulx_input_touch(in, 0, SOULX_TOUCH_MOVE, cx - d, cy) != 0 ||
            soulx_input_touch(in, 1, SOULX_TOUCH_MOVE, cx + d, cy) != 0) {
            rc = -1;
        }
    }
    soulx_input_touch(in, 1, SOULX_TOUCH_UP, cx + d, cy);     // always lift both fingers
    soulx_input_touch(in, 0, SOULX_TOUCH_UP, cx - d, cy);
    return rc;
}

static int op_gesture(SoulXInput* in, unsigned* down, void* arg)
{
    DevGesture g = *(const DevGesture*)arg;
    int w = 0, h = 0;
    if (soulx_input_screen_size(in, &w, &h) != 0 || w <= 0 || h <= 0) return -1;
    int cx = w / 2, cy = h / 2;
    if (g == GESTURE_PINCH_IN || g == GESTURE_PINCH_OUT) *down &= ~3u;     // a pinch uses and lifts fingers 0 and 1
    switch (g) {
    case GESTURE_TAP:
        return soulx_input_tap(in, cx, cy);
    case GESTURE_DOUBLE_TAP:
        if (soulx_input_tap(in, cx, cy) != 0 || soulx_sleep_ms(80) != 0) return -1;
        return soulx_input_tap(in, cx, cy);
    case GESTURE_LONG_PRESS:
        return soulx_input_long_press(in, cx, cy, 800);
    case GESTURE_SWIPE_UP:
        return soulx_input_swipe(in, cx, h * 3 / 4, cx, h / 4, 300);
    case GESTURE_SWIPE_DOWN:
        return soulx_input_swipe(in, cx, h / 4, cx, h * 3 / 4, 300);
    case GESTURE_SWIPE_LEFT:
        return soulx_input_swipe(in, w * 3 / 4, cy, w / 4, cy, 300);
    case GESTURE_SWIPE_RIGHT:
        return soulx_input_swipe(in, w / 4, cy, w * 3 / 4, cy, 300);
    case GESTURE_PINCH_IN:
        return pinch(in, w, h, false);
    case GESTURE_PINCH_OUT:
        return pinch(in, w, h, true);
    case GESTURE_BACK:
        return soulx_input_key(in, "BACK");
    case GESTURE_HOME:
        return soulx_input_key(in, "HOME");
    case GESTURE_RECENTS:
        return soulx_input_key(in, "APP_SWITCH");
    }
    return -1;
}

// Gesture names are case-insensitive; '-' and ' ' work like '_' ("swipe-up", "Pinch Out").
static int gesture_parse(const char* s, DevGesture* g)
{
    char norm[32];
    size_t n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (n + 1 >= sizeof(norm)) return -1;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '-' || c == ' ') c = '_';
        norm[n] = c;
    }
    norm[n] = '\0';
    for (size_t i = 0; i < sizeof(k_gestures) / sizeof(k_gestures[0]); i++) {
        if (strcmp(norm, k_gestures[i].name) == 0) {
            *g = k_gestures[i].gesture;
            return 0;
        }
    }
    return -1;
}

// action: SOULX_TOUCH_DOWN (0), SOULX_TOUCH_UP (1), SOULX_TOUCH_MOVE (2); finger 0.
SOULX_API int soulx_send_touch(int instance_id, int x, int y, int action)
{
    if (!coord_valid(x) || !coord_valid(y) || !touch_action_valid(action)) return SOULX_ERROR;
    TouchArg a = { 0, action, x, y };
    return dev_with_input(instance_id, op_touch, &a);
}

// Pointer i of the arrays is finger i.
SOULX_API int soulx_send_multitouch(int instance_id, int* x, int* y, int* actions, int count)
{
    if (!x || !y || !actions || count <= 0 || count > SOULX_MAX_FINGERS) return SOULX_ERROR;
    for (int i = 0; i < count; i++) {
        if (!coord_valid(x[i]) || !coord_valid(y[i]) || !touch_action_valid(actions[i])) return SOULX_ERROR;
    }
    MultiTouchArg a = { x, y, actions, count };
    return dev_with_input(instance_id, op_multitouch, &a);
}

// keycode: Android KEYCODE_* value. ACTION_DOWN (0) delivers a complete key press (down + up);
// ACTION_UP (1) is then a no-op. Holding a key down is not supported by this backend.
SOULX_API int soulx_send_key(int instance_id, int keycode, int action)
{
    if (keycode <= 0 || keycode > 999 || (action != KEY_ACTION_DOWN && action != KEY_ACTION_UP)) return SOULX_ERROR;
    if (action == KEY_ACTION_UP) {
        DevTarget t;
        return dev_running(instance_id, &t);
    }
    char key[16];
    if (!fmt_ok(snprintf(key, sizeof(key), "%d", keycode), sizeof(key))) return SOULX_ERROR;
    return dev_with_input(instance_id, op_key, key);
}

SOULX_API int soulx_send_text(int instance_id, const char* text)
{
    if (!text || !text[0] || strnlen(text, DEV_TEXT_MAX + 1) > DEV_TEXT_MAX) return SOULX_ERROR;
    return dev_with_input(instance_id, op_text, (void*)(uintptr_t)text);
}

// tap, double_tap, long_press (screen centre), swipe_up/down/left/right, pinch_in/out (zoom_out/in),
// back, home, recents.
SOULX_API int soulx_send_gesture(int instance_id, const char* gesture_type)
{
    DevGesture g;
    if (!gesture_type) return SOULX_ERROR;
    if (gesture_parse(gesture_type, &g) != 0) {
        soulx_log(SOULX_LOG_ERROR, "unknown gesture '%s'", gesture_type);
        return dev_not_supported(instance_id, "this gesture");
    }
    return dev_with_input(instance_id, op_gesture, &g);
}

// duration_ms <= 0 -> 300 ms.
SOULX_API int soulx_send_swipe(int instance_id, int x1, int y1, int x2, int y2, int duration_ms)
{
    if (!coord_valid(x1) || !coord_valid(y1) || !coord_valid(x2) || !coord_valid(y2) || duration_ms > 60000) {
        return SOULX_ERROR;
    }
    SwipeArg a = { x1, y1, x2, y2, duration_ms > 0 ? duration_ms : 300 };
    return dev_with_input(instance_id, op_swipe, &a);
}

// ---------------------------------------------------------------------------
// Sensors, location, battery
// ---------------------------------------------------------------------------

static bool vec_valid(const float v[3])
{
    for (int i = 0; i < 3; i++) {
        if (!isfinite(v[i]) || fabsf(v[i]) > 1e6f) return false;
    }
    return true;
}

// Accelerometer, gyroscope and magnetometer (the other sensors are disabled in SoulX's AVDs);
// use soulx_set_location for the GPS fields.
SOULX_API int soulx_set_sensor_data(int instance_id, const SensorData* data)
{
    if (!data || !vec_valid(data->accelerometer) || !vec_valid(data->gyroscope) || !vec_valid(data->magnetometer)) {
        return SOULX_ERROR;
    }
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    char acc[128], gyro[128], mag[128];
    const float* a = data->accelerometer;
    const float* g = data->gyroscope;
    const float* m = data->magnetometer;
    if (!fmt_ok(snprintf(acc, sizeof(acc), "sensor set acceleration %.5f:%.5f:%.5f", a[0], a[1], a[2]), sizeof(acc)) ||
        !fmt_ok(snprintf(gyro, sizeof(gyro), "sensor set gyroscope %.5f:%.5f:%.5f", g[0], g[1], g[2]), sizeof(gyro)) ||
        !fmt_ok(snprintf(mag, sizeof(mag), "sensor set magnetic-field %.5f:%.5f:%.5f", m[0], m[1], m[2]), sizeof(mag))) {
        return SOULX_ERROR;
    }
    const char* cmds[] = { acc, gyro, mag };
    return dev_console(&t, cmds, 3);
}

SOULX_API int soulx_set_location(int instance_id, double lat, double lon, double alt)
{
    if (!isfinite(lat) || !isfinite(lon) || !isfinite(alt) || lat < -90.0 || lat > 90.0 || lon < -180.0 ||
        lon > 180.0 || alt < -12000.0 || alt > 100000.0) {
        return SOULX_ERROR;
    }
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    char cmd[128];
    // the console takes longitude first
    if (!fmt_ok(snprintf(cmd, sizeof(cmd), "geo fix %.7f %.7f %.2f", lon, lat, alt), sizeof(cmd))) return SOULX_ERROR;
    const char* cmds[] = { cmd };
    return dev_console(&t, cmds, 1);
}

// level, charging (AC) and health; the console cannot set temperature or voltage.
SOULX_API int soulx_set_battery(int instance_id, const BatteryState* state)
{
    static const char* const health_names[] = { "unknown", "good", "overheat", "dead", "overvoltage", "failure" };
    if (!state || state->level < 0 || state->level > 100) return SOULX_ERROR;

    const char* health = NULL;
    size_t hlen = strnlen(state->health, sizeof(state->health));
    if (hlen > 0 && hlen < sizeof(state->health)) {
        for (size_t i = 0; i < sizeof(health_names) / sizeof(health_names[0]); i++) {
            if (strcasecmp(state->health, health_names[i]) == 0) health = health_names[i];
        }
        if (!health) {
            soulx_log(SOULX_LOG_ERROR, "battery health must be one of unknown, good, overheat, dead, overvoltage, failure");
            return SOULX_ERROR;
        }
    }
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;

    char capacity[64], status[64], hcmd[64];
    const char* st = state->is_charging ? (state->level >= 100 ? "full" : "charging") : "discharging";
    if (!fmt_ok(snprintf(capacity, sizeof(capacity), "power capacity %d", state->level), sizeof(capacity)) ||
        !fmt_ok(snprintf(status, sizeof(status), "power status %s", st), sizeof(status)) ||
        !fmt_ok(snprintf(hcmd, sizeof(hcmd), "power health %s", health ? health : "good"), sizeof(hcmd))) {
        return SOULX_ERROR;
    }
    const char* cmds[] = { state->is_charging ? "power ac on" : "power ac off", status, capacity, hcmd };
    return dev_console(&t, cmds, health ? 4 : 3);
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

// NAT = Wi-Fi and mobile data on; NONE = both off. Bridged / host-only networking does not exist here.
SOULX_API int soulx_set_network_mode(int instance_id, NetworkMode mode)
{
    const char* cmd;
    switch (mode) {
    case NETWORK_MODE_NAT:
        cmd = "svc wifi enable; svc data enable";
        break;
    case NETWORK_MODE_NONE:
        cmd = "svc wifi disable; svc data disable";
        break;
    case NETWORK_MODE_BRIDGE:
    case NETWORK_MODE_HOST_ONLY:
        return dev_not_supported(instance_id, "bridged/host-only networking");
    default:
        return SOULX_ERROR;
    }
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    return dev_shell(&t, cmd);
}

// Throttles the emulated modem in kbit/s; 0/0 removes the limit.
SOULX_API int soulx_set_network_speed(int instance_id, int download_kbps, int upload_kbps)
{
    if (download_kbps < 0 || upload_kbps < 0 || download_kbps > 10000000 || upload_kbps > 10000000 ||
        (download_kbps == 0) != (upload_kbps == 0)) {
        return SOULX_ERROR;
    }
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    char cmd[64];
    int n = download_kbps == 0 ? snprintf(cmd, sizeof(cmd), "network speed full")
                               : snprintf(cmd, sizeof(cmd), "network speed %d:%d", upload_kbps, download_kbps);
    if (!fmt_ok(n, sizeof(cmd))) return SOULX_ERROR;
    const char* cmds[] = { cmd };
    return dev_console(&t, cmds, 1);
}

// The engine's -http-proxy option is not wired into SoulX's instance configuration.
SOULX_API int soulx_set_proxy(int instance_id, const char* host, int port)
{
    (void)host;
    (void)port;
    return dev_not_supported(instance_id, "an HTTP proxy");
}

SOULX_API int soulx_enable_vpn(int instance_id, const char* config_path)
{
    (void)config_path;
    return dev_not_supported(instance_id, "VPN");
}

// ---------------------------------------------------------------------------
// Files and apps
// ---------------------------------------------------------------------------

SOULX_API int soulx_install_apk(int instance_id, const char* apk_path)
{
    if (!apk_path || !apk_path[0]) return SOULX_ERROR;
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    return soulx_install_path(t.serial, apk_path, NULL, false) == 0 ? SOULX_OK : SOULX_ERROR;
}

SOULX_API int soulx_uninstall_app(int instance_id, const char* package_name)
{
    if (!soulx_package_name_valid(package_name)) return SOULX_ERROR;
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    return soulx_uninstall_package(t.serial, package_name) == 0 ? SOULX_OK : SOULX_ERROR;
}

static int dev_adb_transfer(const DevTarget* t, const char* verb, const char* from, const char* to)
{
    const char* args[] = { verb, from, to, NULL };
    SoulXCmdResult res;
    char msg[512];
    memset(&res, 0, sizeof(res));
    int rc = soulx_adb(t->serial, args, &res, DEV_FILE_TIMEOUT_MS);
    if (rc != 0) {
        one_line(res.err, res.err_len, msg, sizeof(msg));
        if (!msg[0]) one_line(res.out, res.out_len, msg, sizeof(msg));
        soulx_log(SOULX_LOG_ERROR, "adb %s %s failed: %s", verb, from, msg[0] ? msg : "no output");
    }
    soulx_cmd_result_free(&res);
    return rc == 0 ? SOULX_OK : SOULX_ERROR;
}

SOULX_API int soulx_push_file(int instance_id, const char* local_path, const char* remote_path)
{
    char abs[PATH_MAX];
    if (!local_path || !local_path[0] || !remote_path_valid(remote_path)) return SOULX_ERROR;
    if (!realpath(local_path, abs)) {
        soulx_log(SOULX_LOG_ERROR, "%s: %s", local_path, strerror(errno));
        return SOULX_ERROR;
    }
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    return dev_adb_transfer(&t, "push", abs, remote_path);
}

SOULX_API int soulx_pull_file(int instance_id, const char* remote_path, const char* local_path)
{
    char local[PATH_MAX];
    if (!local_path || !local_path[0] || !remote_path_valid(remote_path)) return SOULX_ERROR;
    // "./-x" so a local name starting with '-' is never read as an option
    int n = local_path[0] == '-' ? snprintf(local, sizeof(local), "./%s", local_path)
                                 : snprintf(local, sizeof(local), "%s", local_path);
    if (!fmt_ok(n, sizeof(local))) return SOULX_ERROR;
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    return dev_adb_transfer(&t, "pull", remote_path, local);
}

SOULX_API int soulx_set_shared_folder(int instance_id, const char* folder_path)
{
    (void)folder_path;
    return dev_not_supported(instance_id, "shared folders");
}

SOULX_API int soulx_backup(int instance_id, const char* backup_path)
{
    (void)backup_path;
    return dev_not_supported(instance_id, "backup");
}

SOULX_API int soulx_restore(int instance_id, const char* backup_path)
{
    (void)backup_path;
    return dev_not_supported(instance_id, "restore");
}

SOULX_API int soulx_launch_app(int instance_id, const char* package_name)
{
    if (!soulx_package_name_valid(package_name)) return SOULX_ERROR;
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    return soulx_launch_package(t.serial, package_name) == 0 ? SOULX_OK : SOULX_ERROR;
}

SOULX_API int soulx_stop_app(int instance_id, const char* package_name)
{
    if (!soulx_package_name_valid(package_name)) return SOULX_ERROR;
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    return soulx_force_stop(t.serial, package_name) == 0 ? SOULX_OK : SOULX_ERROR;
}

// ---------------------------------------------------------------------------
// Snapshots (console `avd snapshot save|load|delete`)
// ---------------------------------------------------------------------------

static bool snapshot_name_valid(const char* s)
{
    if (!s || !s[0] || strlen(s) > 64) return false;
    for (const char* p = s; *p; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  (p != s && (c == '_' || c == '.' || c == '-'));
        if (!ok) return false;
    }
    return true;
}

// Is <name> a column of `avd snapshot list` (the header line excluded)?
static bool snapshot_listed(const char* list, const char* name)
{
    size_t n = strlen(name);
    for (const char* line = list; line && *line;) {
        const char* nl = strchr(line, '\n');
        size_t ll = nl ? (size_t)(nl - line) : strlen(line);
        bool header = false;
        for (size_t i = 0; i + 3 <= ll; i++) {
            if (memcmp(line + i, "TAG", 3) == 0) header = true;
        }
        for (size_t i = 0; !header && i + n <= ll; i++) {
            bool start = i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t';
            bool end = i + n == ll || line[i + n] == ' ' || line[i + n] == '\t' || line[i + n] == '\r';
            if (start && end && memcmp(line + i, name, n) == 0) return true;
        }
        line = nl ? nl + 1 : NULL;
    }
    return false;
}

// Saving/loading a large VM can outlast the console's reply timeout: wait until the console answers
// again, then check the snapshot list.
static int snapshot_settle(const DevTarget* t, const char* verb, const char* name)
{
    double deadline = soulx_now_ms() + DEV_SNAPSHOT_WAIT_MS;
    char list[8192];
    soulx_log(SOULX_LOG_INFO, "waiting for snapshot %s '%s' to finish...", verb, name);
    while (soulx_now_ms() < deadline) {
        if (!soulx_instance_running(t->conf.name, NULL)) {
            soulx_log(SOULX_LOG_ERROR, "the engine of '%s' exited during snapshot %s", t->conf.name, verb);
            return SOULX_ERROR;
        }
        if (soulx_console_oneshot(t->st.port, "avd snapshot list", list, sizeof(list)) == 0) {
            bool present = snapshot_listed(list, name);
            bool ok = strcmp(verb, "delete") == 0 ? !present : present;
            if (!ok) soulx_log(SOULX_LOG_ERROR, "snapshot %s '%s' failed", verb, name);
            return ok ? SOULX_OK : SOULX_ERROR;
        }
        if (soulx_sleep_ms(1000) != 0) return SOULX_ERROR;
    }
    soulx_log(SOULX_LOG_ERROR, "snapshot %s '%s' did not finish in time", verb, name);
    return SOULX_ERROR;
}

static int dev_snapshot(int instance_id, const char* verb, const char* name)
{
    if (!snapshot_name_valid(name)) {
        soulx_log(SOULX_LOG_ERROR, "invalid snapshot name (letters, digits, '_', '.', '-'; at most 64)");
        return SOULX_ERROR;
    }
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    char cmd[128], reply[1024];
    if (!fmt_ok(snprintf(cmd, sizeof(cmd), "avd snapshot %s %s", verb, name), sizeof(cmd))) return SOULX_ERROR;
    rc = soulx_console_oneshot(t.st.port, cmd, reply, sizeof(reply));
    if (rc == 0) return SOULX_OK;
    if (rc == 1) {
        soulx_log(SOULX_LOG_ERROR, "snapshot %s '%s' failed: %s", verb, name, reply[0] ? reply : "refused");
        return SOULX_ERROR;
    }
    return snapshot_settle(&t, verb, name);
}

SOULX_API int soulx_snapshot_create(int instance_id, const char* name)
{
    return dev_snapshot(instance_id, "save", name);
}

SOULX_API int soulx_snapshot_restore(int instance_id, const char* name)
{
    return dev_snapshot(instance_id, "load", name);
}

SOULX_API int soulx_snapshot_delete(int instance_id, const char* name)
{
    return dev_snapshot(instance_id, "delete", name);
}

// ---------------------------------------------------------------------------
// Macros and scripts (Lua, through soulx_script_run)
// ---------------------------------------------------------------------------

// The script engine's only cancellation hook is g_soulx_interrupted, so soulx_macro_stop raises it;
// the last script to finish after such a request clears it again.
static pthread_mutex_t g_script_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_script_ids[DEV_SCRIPT_SLOTS];      // instance ids of running scripts (0 = free)
static int g_script_count;
static bool g_script_stop;

static int script_begin(int instance_id)
{
    int slot = -1;
    pthread_mutex_lock(&g_script_lock);
    for (int i = 0; i < DEV_SCRIPT_SLOTS && slot < 0; i++) {
        if (g_script_ids[i] == 0) slot = i;
    }
    if (slot >= 0) g_script_ids[slot] = instance_id;
    g_script_count++;
    pthread_mutex_unlock(&g_script_lock);
    return slot;
}

// Returns true when a soulx_macro_stop request ended the script.
static bool script_end(int slot)
{
    pthread_mutex_lock(&g_script_lock);
    if (slot >= 0) g_script_ids[slot] = 0;
    g_script_count--;
    bool stopped = g_script_stop;
    if (g_script_stop && g_script_count == 0) {
        g_script_stop = false;
        g_soulx_interrupted = 0;
    }
    pthread_mutex_unlock(&g_script_lock);
    return stopped;
}

static bool script_stop_requested(void)
{
    pthread_mutex_lock(&g_script_lock);
    bool stop = g_script_stop;
    pthread_mutex_unlock(&g_script_lock);
    return stop;
}

static int dev_run_script(int instance_id, const char* path, int repeat)
{
    struct stat sb;
    if (!path || !path[0] || stat(path, &sb) != 0 || !S_ISREG(sb.st_mode)) {
        soulx_log(SOULX_LOG_ERROR, "script not found: %s", path ? path : "");
        return SOULX_ERROR;
    }
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;

    int slot = script_begin(instance_id);
    int code = 0;
    for (int i = 0; i < repeat && code == 0; i++) {
        if (script_stop_requested() || soulx_interrupted()) break;
        char* no_args[1] = { NULL };
        code = soulx_script_run(t.conf.name, path, 0, no_args);
    }
    bool stopped = script_end(slot);
    if (code == 0) return SOULX_OK;
    return (stopped && code == 130) ? SOULX_OK : SOULX_ERROR;
}

// Recording through the API is not available: use `soulx record <out.lua>`.
SOULX_API int soulx_macro_record_start(int instance_id)
{
    return dev_not_supported(instance_id, "macro recording through the API (use `soulx record`)");
}

SOULX_API int soulx_macro_record_stop(int instance_id, const char* save_path)
{
    (void)save_path;
    return dev_not_supported(instance_id, "macro recording through the API (use `soulx record`)");
}

// Plays a Lua script (e.g. one written by `soulx record`) repeat_count times (< 1 = once); blocks.
SOULX_API int soulx_macro_play(int instance_id, const char* macro_path, int repeat_count)
{
    if (repeat_count < 1) repeat_count = 1;
    if (repeat_count > 1000000) repeat_count = 1000000;
    return dev_run_script(instance_id, macro_path, repeat_count);
}

// Stops scripts playing on the instance. Because the script engine is cancelled through the shared
// interrupt flag, scripts on other instances in this process stop as well.
SOULX_API int soulx_macro_stop(int instance_id)
{
    SoulXInstanceConf c;
    int rc = dev_find(instance_id, &c);
    if (rc != SOULX_OK) return rc;
    bool active = false;
    pthread_mutex_lock(&g_script_lock);
    for (int i = 0; i < DEV_SCRIPT_SLOTS; i++) {
        if (g_script_ids[i] == instance_id) active = true;
    }
    if (active) {
        g_script_stop = true;
        g_soulx_interrupted = 1;
    }
    pthread_mutex_unlock(&g_script_lock);
    return SOULX_OK;
}

SOULX_API int soulx_run_script(int instance_id, const char* script_path)
{
    return dev_run_script(instance_id, script_path, 1);
}

// ---------------------------------------------------------------------------
// Performance (host-side view of the engine process)
// ---------------------------------------------------------------------------

typedef struct {
    int id;
    pid_t pid;
    double t_ms;
    uint64_t cpu_ns;
    uint64_t read_bytes;
    uint64_t write_bytes;
} PerfSample;

static PerfSample g_perf[MAX_INSTANCES];
static pthread_mutex_t g_perf_lock = PTHREAD_MUTEX_INITIALIZER;

static int perf_sample(pid_t pid, PerfSample* s, uint64_t* footprint)
{
    struct rusage_info_v2 ri;
    memset(&ri, 0, sizeof(ri));
    if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t*)&ri) != 0) return -1;
    // ri_*_time are mach absolute time units (not nanoseconds) on Apple Silicon
    mach_timebase_info_data_t tb;
    if (mach_timebase_info(&tb) != 0 || tb.denom == 0) {
        tb.numer = 1;
        tb.denom = 1;
    }
    s->pid = pid;
    s->t_ms = soulx_now_ms();
    s->cpu_ns = (uint64_t)((double)(ri.ri_user_time + ri.ri_system_time) * tb.numer / tb.denom);
    s->read_bytes = ri.ri_diskio_bytesread;
    s->write_bytes = ri.ri_diskio_byteswritten;
    *footprint = ri.ri_phys_footprint;
    return 0;
}

static int host_cpus(void)
{
    int n = 0;
    size_t len = sizeof(n);
    if (sysctlbyname("hw.logicalcpu", &n, &len, NULL, 0) != 0 || n <= 0) n = 1;
    return n;
}

static double rate(uint64_t now, uint64_t before, double seconds)
{
    return now > before ? (double)(now - before) / seconds : 0.0;
}

// cpu_usage_percent: share of the whole host (all cores = 100); ram_used_mb: physical footprint of
// the engine; ram_total_mb: the instance's guest RAM; disk rates in MB/s. GPU/FPS/network are not
// observable from the host and stay 0. The first call samples twice, 200 ms apart.
SOULX_API int soulx_get_performance(int instance_id, PerformanceMetrics* metrics)
{
    if (!metrics) return SOULX_ERROR;
    memset(metrics, 0, sizeof(*metrics));
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;

    PerfSample prev, cur;
    uint64_t footprint = 0;
    bool have_prev = false;
    pthread_mutex_lock(&g_perf_lock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < MAX_INSTANCES; i++) {
        if (g_perf[i].id == instance_id) slot = i;
        if (g_perf[i].t_ms < g_perf[oldest].t_ms) oldest = i;
    }
    if (slot >= 0) {
        prev = g_perf[slot];
        double age = soulx_now_ms() - prev.t_ms;
        have_prev = prev.pid == t.st.pid && age >= 50.0 && age <= 60000.0;
    }
    pthread_mutex_unlock(&g_perf_lock);

    if (!have_prev) {
        if (perf_sample(t.st.pid, &prev, &footprint) != 0) goto fail;
        soulx_sleep_ms(200);
    }
    if (perf_sample(t.st.pid, &cur, &footprint) != 0) goto fail;

    double seconds = (cur.t_ms - prev.t_ms) / 1000.0;
    if (seconds <= 0.0) seconds = 0.001;
    double cpu = rate(cur.cpu_ns, prev.cpu_ns, seconds) / 1e9 / host_cpus() * 100.0;
    metrics->cpu_usage_percent = cpu > 100.0 ? 100.0 : cpu;
    metrics->ram_used_mb = footprint / (1024 * 1024);
    metrics->ram_total_mb = t.conf.ram_mb > 0 ? (uint64_t)t.conf.ram_mb : 0;
    metrics->disk_read_mbps = rate(cur.read_bytes, prev.read_bytes, seconds) / (1024.0 * 1024.0);
    metrics->disk_write_mbps = rate(cur.write_bytes, prev.write_bytes, seconds) / (1024.0 * 1024.0);

    cur.id = instance_id;
    pthread_mutex_lock(&g_perf_lock);
    g_perf[slot >= 0 ? slot : oldest] = cur;
    pthread_mutex_unlock(&g_perf_lock);
    return SOULX_OK;

fail:
    soulx_log(SOULX_LOG_ERROR, "cannot read process statistics of the engine (pid %d)", (int)t.st.pid);
    return SOULX_ERROR;
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------

SOULX_API int soulx_clipboard_copy(int instance_id, const char* text)
{
    (void)text;
    return dev_not_supported(instance_id, "clipboard access");
}

SOULX_API int soulx_clipboard_paste(int instance_id, char* buffer, int max_len)
{
    if (buffer && max_len > 0) buffer[0] = '\0';
    return dev_not_supported(instance_id, "clipboard access");
}

SOULX_API int soulx_clipboard_sync_enable(int instance_id, bool enable)
{
    (void)enable;
    return dev_not_supported(instance_id, "clipboard sync");
}

// ---------------------------------------------------------------------------
// ADB
// ---------------------------------------------------------------------------

// Instances attach to SoulX's private adb server by themselves; this checks that adb sees the device.
SOULX_API int soulx_adb_connect(int instance_id)
{
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    if (soulx_adb_start_server() != 0) return SOULX_ERROR;
    const char* args[] = { "get-state", NULL };
    SoulXCmdResult res;
    memset(&res, 0, sizeof(res));
    rc = soulx_adb(t.serial, args, &res, 10000);
    bool ok = rc == 0 && res.out && strncmp(res.out, "device", 6) == 0;
    if (!ok) soulx_log(SOULX_LOG_ERROR, "adb does not see %s as an online device yet", t.serial);
    soulx_cmd_result_free(&res);
    return ok ? SOULX_OK : SOULX_ERROR;
}

// Splits a command line into words: blanks separate, '...' is literal, "..." honours \" and \\,
// a backslash escapes the next character. Nothing is expanded and no shell is involved.
static int split_words(const char* s, char* buf, size_t cap, char** words, int max)
{
    size_t o = 0;
    int n = 0;
    const char* p = s;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        if (n >= max) return -1;
        words[n++] = buf + o;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            if (o + 2 > cap) return -1;
            if (*p == '\'') {
                for (p++; *p && *p != '\''; p++) {
                    if (o + 2 > cap) return -1;
                    buf[o++] = *p;
                }
                if (!*p) return -1;
                p++;
            } else if (*p == '"') {
                for (p++; *p && *p != '"'; p++) {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;
                    if (o + 2 > cap) return -1;
                    buf[o++] = *p;
                }
                if (!*p) return -1;
                p++;
            } else if (*p == '\\') {
                if (!p[1]) return -1;
                buf[o++] = p[1];
                p += 2;
            } else {
                buf[o++] = *p++;
            }
        }
        buf[o++] = '\0';
    }
    words[n] = NULL;
    return n;
}

// Runs `adb -s <instance serial> <command>` (a leading "adb" is accepted). Options that retarget adb
// and server-management commands are refused. output receives stdout followed by stderr.
SOULX_API int soulx_adb_command(int instance_id, const char* command, char* output, int max_len)
{
    static const char* const denied[] = {
        "kill-server", "start-server", "server", "nodaemon", "connect", "disconnect", "pair", "reconnect"
    };
    if (output && max_len > 0) output[0] = '\0';
    size_t clen = command ? strnlen(command, DEV_ADB_MAX_CMD + 1) : 0;
    if (clen == 0 || clen > DEV_ADB_MAX_CMD) return SOULX_ERROR;
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;

    char* buf = malloc(clen + 1);
    char* words[DEV_ADB_MAX_ARGS + 1];
    if (!buf) return SOULX_ERROR;
    int n = split_words(command, buf, clen + 1, words, DEV_ADB_MAX_ARGS);
    int first = (n > 0 && strcmp(words[0], "adb") == 0) ? 1 : 0;
    if (n <= first) {
        soulx_log(SOULX_LOG_ERROR, "soulx_adb_command: cannot parse the command");
        free(buf);
        return SOULX_ERROR;
    }
    bool refused = words[first][0] == '-';
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
        if (strcmp(words[first], denied[i]) == 0) refused = true;
    }
    if (refused) {
        soulx_log(SOULX_LOG_ERROR, "soulx_adb_command: '%s' is not allowed (the command always targets this instance)",
                 words[first]);
        free(buf);
        return SOULX_ERROR;
    }

    SoulXCmdResult res;
    memset(&res, 0, sizeof(res));
    rc = soulx_adb(t.serial, (const char* const*)(words + first), &res, DEV_ADB_TIMEOUT_MS);
    if (output && max_len > 0) {
        size_t cap = (size_t)max_len, o = 0;
        const char* parts[2] = { res.out, res.err };
        size_t lens[2] = { res.out ? res.out_len : 0, res.err ? res.err_len : 0 };
        for (int i = 0; i < 2; i++) {
            size_t k = lens[i] < cap - 1 - o ? lens[i] : cap - 1 - o;
            if (k > 0) memcpy(output + o, parts[i], k);
            o += k;
        }
        output[o] = '\0';
    }
    soulx_cmd_result_free(&res);
    free(buf);
    return rc == 0 ? SOULX_OK : SOULX_ERROR;
}

// adb port of a running instance (console port + 1).
SOULX_API int soulx_get_adb_port(int instance_id)
{
    DevTarget t;
    int rc = dev_running(instance_id, &t);
    if (rc != SOULX_OK) return rc;
    return t.st.port + 1;
}
