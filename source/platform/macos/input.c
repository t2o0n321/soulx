/*
 * SoulX Android Emulator - macOS input injection (touch, keys, text) and fast screen capture
 *
 * Callers pass LOGICAL pixels: the guest's current (rotated) screen, i.e. screenshot space.
 * The emulator takes touch positions in the display's NATURAL orientation (W x H: the panel
 * hw.lcd.width x hw.lcd.height, or a `wm size` override) whatever the guest rotation R is
 * (`dumpsys window displays`: mCurrentRotation=ROTATION_<0|90|180|270>, cached for 2 s and
 * re-read every 250 ms for 3 s after actions that may rotate the screen). A logical pixel
 * (lx, ly) is the natural pixel
 *     R=0: (lx, ly)              R=2: (W-1-lx, H-1-ly)
 *     R=1: (W-1-ly, lx)          R=3: (ly, H-1-lx)
 * and a touch aims at the centre of that pixel: raw = (2n + 1) * 32768 / (2 * dim), 0..32767.
 * W x H come from the running guest (`wm size`), else from the AVD the engine was started with,
 * else from the registry.
 *
 * Touch = multi-touch (protocol B) frames on one persistent emulator console connection
 * (reopened once when it breaks), one slot per finger 0..9:
 *     down  event send EV_ABS:ABS_MT_SLOT:<f> EV_ABS:ABS_MT_TRACKING_ID:<id> EV_ABS:ABS_MT_POSITION_X:<rx>
 *                      EV_ABS:ABS_MT_POSITION_Y:<ry> EV_ABS:ABS_MT_PRESSURE:1024 EV_SYN:0:0
 *     move  event send EV_ABS:ABS_MT_SLOT:<f> EV_ABS:ABS_MT_POSITION_X:<rx> EV_ABS:ABS_MT_POSITION_Y:<ry> EV_SYN:0:0
 *     up    event send EV_ABS:ABS_MT_SLOT:<f> EV_ABS:ABS_MT_PRESSURE:0 EV_ABS:ABS_MT_TRACKING_ID:-1 EV_SYN:0:0
 * A finger that went down is always lifted again: after errors, after Ctrl+C and in
 * soulx_input_close(). A command is sent again on a new connection only when it never reached
 * the emulator, or when sending it twice is harmless (touch frames, screenshots): `event text`
 * after a lost reply is not repeated.
 * Keys: `adb shell input keyevent <code>` (console EV_KEY events never reach a key device).
 * Text: console `event text` (typed by the emulator's qwerty2 keyboard) in chunks of at most 14 key
 * strokes - a shifted character (capital, !, ?, ...) takes two - with a pause after each chunk: the
 * emulator silently drops a whole `event text` of more than about 15 key strokes and still answers
 * OK. `adb shell input text` is the fallback when the console refuses.
 * Capture: the frame the engine shares with SoulX's window (-share-vid, panel-native B,G,R,A in shared
 * memory: a copy, no PNG), else console `screenrecord screenshot` (the same framebuffer as a PNG the engine
 * writes to a folder of this process), turned into the logical orientation; else `adb exec-out screencap`.
 * A SoulXInput is not thread-safe.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "soulx_mac.h"
#include "image.h"
#include "window/frames.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define INPUT_RAW_SCALE         32768       // raw = (pixel + 0.5) * 32768 / dim
#define INPUT_RAW_MAX           32767       // ABS_MT_POSITION_X/Y range 0..32767
#define INPUT_PRESSURE          1024        // pressure of a touching finger (0 = hovering, no touch)
#define INPUT_TRACKING_MASK     0xFFFF      // ABS_MT_TRACKING_ID range 0..65535
#define INPUT_TAP_MS            40          // finger down time of a tap
#define INPUT_FRAME_MS          16          // swipe frame interval
#define INPUT_GESTURE_MAX_MS    600000      // longest long press / swipe (10 minutes)
#define INPUT_ROTATION_TTL_MS   2000.0      // the cached rotation is re-read when older than this
#define INPUT_ROTATION_FAST_MS  250.0       // ... or than this while the screen may be rotating
#define INPUT_ROTATION_WATCH_MS 3000.0      // how long after such an action (soulx_input_watch_rotation)
#define INPUT_ROTATION_MAX_MS   60000.0     // longest re-read interval while reads keep failing
#define INPUT_ADB_TIMEOUT_MS    5000        // dumpsys window displays / wm size
#define INPUT_KEY_TIMEOUT_MS    10000       // input keyevent / input text
#define INPUT_KEY_COST_MS       40          // typical time of one `input keyevent` (measured: 31 ms median)
#define INPUT_TEXT_CHUNK_UNITS  14          // key strokes per console `event text` (shifted characters
                                            // count 2): the emulator drops a whole command above ~15
#define INPUT_TEXT_GAP_MS       30          // pause after every `event text` chunk ...
#define INPUT_TEXT_PACE_MS      1           // ... plus this per key stroke
#define INPUT_CMD_MAX           1024        // console / shell command buffer
#define INPUT_KEY_NAME_MAX      48
#define INPUT_KEYCODE_MAX       999
#define INPUT_SHOT_WAIT_MS      500.0       // console screenshot: wait this long for the PNG
#define INPUT_SHOT_POLL_MS      4
#define INPUT_FRESH_MS          150         // longest wait for a complete shared frame (one or two publishes)
#define INPUT_SHOT_MAX_FAILS    3           // then only `adb exec-out screencap` is used
#define INPUT_SHOT_PREFIX       "Screenshot_"
#define INPUT_SHOT_MAX_NAMES    32
#define INPUT_ROT_BLOCK         64          // tile size of the 90/270 degree pixel rotation
#define INPUT_AVD_CONFIG_MAX    65536       // largest AVD config.ini read
// the guest filters: the full `dumpsys window displays` is about 18 KB
#define INPUT_DISPLAY_CMD       "wm size; dumpsys window displays | grep -m1 mCurrentRotation"
#define INPUT_ROTATION_CMD      "dumpsys window displays | grep -m1 mCurrentRotation"

#define ANDROID_KEYCODE_TAB     61
#define ANDROID_KEYCODE_ENTER   66

// console.c: how the last soulx_console_cmd() failed (mirror of its CONSOLE_FAIL_* values)
#define CONSOLE_FAIL_NO_ANSWER  3           // sent, but no (complete) reply: it may have run
#define CONSOLE_FAIL_INVALID    4           // refused before sending

typedef struct {
    bool down;
    int  id;                     // ABS_MT_TRACKING_ID of the current touch
    int  rx, ry;                 // last raw position sent
} InputFinger;

struct SoulXInput {
    char  name[SOULX_NAME_MAX + 1];
    char  serial[64];
    int   port;                  // console port (serial = emulator-<port>)
    pid_t pid;                   // engine the connection belongs to
    SoulXConsole* con;            // persistent console connection (NULL = reconnect on next use)
    int   w_nat, h_nat;          // display in its natural orientation: what touches and logical
                                 // coordinates span (the `wm size` override, else the panel)
    int   panel_w, panel_h;      // physical panel (natural orientation): the size of console screenshots
    bool  overridden;            // a `wm size` override is active
    bool  sizes_stale;           // re-read the sizes from the guest at the next refresh
    int   rotation;              // guest display rotation 0..3 (ROTATION_0/90/180/270)
    double rotation_ms;          // soulx_now_ms() of the last read attempt (0 = never)
    double rotation_ttl;         // re-read interval (grows while reads fail)
    double watch_until;          // until then the rotation is re-read every INPUT_ROTATION_FAST_MS
    InputFinger fingers[SOULX_MAX_FINGERS];
    int   next_id;               // next tracking id (increasing, wraps at 65536)
    char  shots[PATH_MAX];       // $SOULX_HOME/tmp/shots-<name>-<pid>: console screenshots land here
    int   shot_fails;            // consecutive failed console screenshots
    bool  shot_disabled;
    SoulXFrames* frames;          // the engine's shared frames (NULL = not mapped yet / gone)
    bool  display_pushed;        // the owner keeps rotation and sizes current (soulx_input_apply_display):
                                 // nothing here runs adb for them
};

// Exported for record.c, soulx_lua.c and tests/macos/test_input.c (not part of soulx_mac.h).
void soulx_map_logical_to_natural(int r, int w_nat, int h_nat, int lx, int ly, int* nx, int* ny);
void soulx_map_natural_to_logical(int r, int w_nat, int h_nat, int nx, int ny, int* lx, int* ly);
void soulx_map_logical_pixel(int r, int w_nat, int h_nat, int lx, int ly, int* nx, int* ny);
int  soulx_input_to_raw(int px, int dim);
int  soulx_image_rotate_to_logical(const SoulXImage* fb, int rotation, SoulXImage* out);
int  soulx_input_parse_rotation(const char* dumpsys);
int  soulx_input_parse_wm_size(const char* text, int* pw, int* ph, int* ow, int* oh);
int  soulx_input_keycode(const char* name_or_code);
int  soulx_input_console_text_cmd(const char* s, size_t n, char* cmd, size_t cap);
int  soulx_input_shell_text_cmd(const char* s, size_t n, char* cmd, size_t cap);
int  soulx_input_key_units(char c);
size_t soulx_input_text_chunk(const char* s, size_t n);
int  soulx_input_text_cost_ms(const char* s, size_t n);
int  soulx_input_display_size(SoulXInput* in, int* w_nat, int* h_nat);
void soulx_input_watch_rotation(SoulXInput* in);

// console.c and adb.c (exported there for this file)
int soulx_console_failure(const SoulXConsole* c);
int soulx_adb_shell_nowait(const char* serial, const char* shell_cmd, char** out, int timeout_ms);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static bool fmt_ok(int n, size_t cap) {
    return n >= 0 && (size_t)n < cap;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int norm_rotation(int r) {
    return (r >= 0 && r <= 3) ? r : 0;
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool is_printable(char c) {
    return c >= 0x20 && c <= 0x7e;
}

static bool dim_ok(int v) {
    return v > 0 && v <= SOULX_IMAGE_MAX_DIM;
}

// The string itself when it is short printable ASCII, else a placeholder (for log messages).
static const char* shown(const char* s) {
    if (!s) return "(null)";
    size_t n = strnlen(s, INPUT_KEY_NAME_MAX + 1);
    if (n == 0 || n > INPUT_KEY_NAME_MAX) return "(invalid)";
    for (size_t i = 0; i < n; i++) {
        if (!is_printable(s[i])) return "(invalid)";
    }
    return s;
}

static bool any_finger_down(const SoulXInput* in) {
    for (int f = 0; f < SOULX_MAX_FINGERS; f++) {
        if (in->fingers[f].down) return true;
    }
    return false;
}

static void logical_size(const SoulXInput* in, int* w, int* h) {
    bool quarter = (in->rotation & 1) != 0;
    *w = quarter ? in->h_nat : in->w_nat;
    *h = quarter ? in->w_nat : in->h_nat;
}

// ---------------------------------------------------------------------------
// Coordinate mapping (verified on the emulator)
// ---------------------------------------------------------------------------

// Points (pixel edges): as measured on the emulator.
void soulx_map_logical_to_natural(int r, int w_nat, int h_nat, int lx, int ly, int* nx, int* ny) {
    int x, y;
    switch (norm_rotation(r)) {
    case 1:  x = w_nat - ly; y = lx;         break;     // ROTATION_90
    case 2:  x = w_nat - lx; y = h_nat - ly; break;     // ROTATION_180
    case 3:  x = ly;         y = h_nat - lx; break;     // ROTATION_270
    default: x = lx;         y = ly;         break;
    }
    if (nx) *nx = x;
    if (ny) *ny = y;
}

void soulx_map_natural_to_logical(int r, int w_nat, int h_nat, int nx, int ny, int* lx, int* ly) {
    int x, y;
    switch (norm_rotation(r)) {
    case 1:  x = ny;         y = w_nat - nx; break;
    case 2:  x = w_nat - nx; y = h_nat - ny; break;
    case 3:  x = h_nat - ny; y = nx;         break;
    default: x = nx;         y = ny;         break;
    }
    if (lx) *lx = x;
    if (ly) *ly = y;
}

// Pixels: logical pixel (lx, ly) -> the natural pixel that shows it (same as the framebuffer rotation).
void soulx_map_logical_pixel(int r, int w_nat, int h_nat, int lx, int ly, int* nx, int* ny) {
    int x, y;
    switch (norm_rotation(r)) {
    case 1:  x = w_nat - 1 - ly; y = lx;             break;
    case 2:  x = w_nat - 1 - lx; y = h_nat - 1 - ly; break;
    case 3:  x = ly;             y = h_nat - 1 - lx; break;
    default: x = lx;             y = ly;             break;
    }
    if (nx) *nx = x;
    if (ny) *ny = y;
}

// Raw touchscreen value of the centre of natural pixel px (clamped to 0..dim-1). The guest maps a raw
// value back with raw * dim / 32768, which lands inside that pixel (also the last one).
int soulx_input_to_raw(int px, int dim) {
    if (dim <= 0) return 0;
    px = clampi(px, 0, dim - 1);
    long long r = ((2LL * px + 1) * INPUT_RAW_SCALE) / (2LL * dim);
    if (r < 0) return 0;
    if (r > INPUT_RAW_MAX) return INPUT_RAW_MAX;
    return (int)r;
}

// Validates a logical point and converts it to raw touchscreen units. strict: the point must
// lie on the screen (the far edge itself is clamped onto the last pixel); otherwise the point
// is clamped (a finger dragged past the edge stays on the edge).
static int point_to_raw(SoulXInput* in, int x, int y, bool strict, int* rx, int* ry) {
    int lw, lh;
    logical_size(in, &lw, &lh);
    if (strict && (x < 0 || y < 0 || x > lw || y > lh)) {
        soulx_log(SOULX_LOG_ERROR, "point %d,%d is outside the screen of '%s' (%dx%d: x 0-%d, y 0-%d)",
                 x, y, in->name, lw, lh, lw - 1, lh - 1);
        return -1;
    }
    x = clampi(x, 0, lw - 1);
    y = clampi(y, 0, lh - 1);
    int nx, ny;
    soulx_map_logical_pixel(in->rotation, in->w_nat, in->h_nat, x, y, &nx, &ny);
    *rx = soulx_input_to_raw(nx, in->w_nat);
    *ry = soulx_input_to_raw(ny, in->h_nat);
    return 0;
}

// ---------------------------------------------------------------------------
// Framebuffer rotation: natural W x H pixels -> logical orientation
// logical pixel (lx, ly) = natural pixel
//     R=1: (W-1-ly, lx)    R=2: (W-1-lx, H-1-ly)    R=3: (ly, H-1-lx)
// ---------------------------------------------------------------------------

// 90/270 degrees: destination rows are written sequentially, the source is read down/up a
// column; 64x64 tiles keep both sides in the cache.
static void rotate_quarter(const uint8_t* src, int w, int h, int r, uint8_t* dst, int lw, int lh) {
    for (int by = 0; by < lh; by += INPUT_ROT_BLOCK) {
        int yend = by + INPUT_ROT_BLOCK < lh ? by + INPUT_ROT_BLOCK : lh;
        for (int bx = 0; bx < lw; bx += INPUT_ROT_BLOCK) {
            int xend = bx + INPUT_ROT_BLOCK < lw ? bx + INPUT_ROT_BLOCK : lw;
            for (int ly = by; ly < yend; ly++) {
                uint8_t* d = dst + ((size_t)ly * (size_t)lw + (size_t)bx) * 4;
                if (r == 1) {
                    size_t si = (size_t)bx * (size_t)w + (size_t)(w - 1 - ly);
                    for (int lx = bx; lx < xend; lx++, d += 4, si += (size_t)w) memcpy(d, src + si * 4, 4);
                } else {
                    // unsigned wrap-around of si after the last pixel is never dereferenced
                    size_t si = (size_t)(h - 1 - bx) * (size_t)w + (size_t)ly;
                    for (int lx = bx; lx < xend; lx++, d += 4, si -= (size_t)w) memcpy(d, src + si * 4, 4);
                }
            }
        }
    }
}

int soulx_image_rotate_to_logical(const SoulXImage* fb, int rotation, SoulXImage* out) {
    if (!out) return -1;
    if (!fb || !fb->rgba || fb->w <= 0 || fb->h <= 0 || fb->w > SOULX_IMAGE_MAX_DIM ||
        fb->h > SOULX_IMAGE_MAX_DIM || rotation < 0 || rotation > 3) {
        if (out != fb) {
            out->w = 0;
            out->h = 0;
            out->rgba = NULL;
        }
        return -1;
    }
    const int w = fb->w, h = fb->h;
    const int lw = (rotation & 1) ? h : w;
    const int lh = (rotation & 1) ? w : h;
    const size_t bytes = (size_t)w * (size_t)h * 4;
    uint8_t* dst = malloc(bytes);
    if (!dst) return -1;
    const uint8_t* src = fb->rgba;

    if (rotation == 0) {
        memcpy(dst, src, bytes);
    } else if (rotation == 2) {
        for (int ly = 0; ly < lh; ly++) {
            const uint8_t* s = src + ((size_t)(h - 1 - ly) * (size_t)w + (size_t)(w - 1)) * 4;
            uint8_t* d = dst + (size_t)ly * (size_t)lw * 4;
            for (int lx = 0; lx < lw; lx++, d += 4) memcpy(d, s - (size_t)lx * 4, 4);
        }
    } else {
        rotate_quarter(src, w, h, rotation, dst, lw, lh);
    }

    if (out == fb) free(out->rgba);     // in place: release the old pixels
    out->w = lw;
    out->h = lh;
    out->rgba = dst;
    return 0;
}

// ---------------------------------------------------------------------------
// Display rotation and size
// ---------------------------------------------------------------------------

// First "mCurrentRotation=ROTATION_<deg>" (or "=<0..3>") in `dumpsys window displays`.
int soulx_input_parse_rotation(const char* dumpsys) {
    static const char key[] = "mCurrentRotation=";
    if (!dumpsys) return -1;
    for (const char* p = strstr(dumpsys, key); p; p = strstr(p + 1, key)) {
        const char* v = p + sizeof(key) - 1;
        if (strncmp(v, "ROTATION_", 9) == 0) {
            v += 9;
            int deg = 0, digits = 0;
            while (is_digit(*v) && digits < 4) {
                deg = deg * 10 + (*v - '0');
                v++;
                digits++;
            }
            if (digits == 0 || is_digit(*v)) continue;
            if (deg == 0) return 0;
            if (deg == 90) return 1;
            if (deg == 180) return 2;
            if (deg == 270) return 3;
        } else if (*v >= '0' && *v <= '3' && !is_digit(v[1])) {
            return *v - '0';
        }
    }
    return -1;
}

// Up to 6 decimal digits at *pp (advanced past them); -1 when there are none or more.
static long parse_dim(const char** pp) {
    const char* p = *pp;
    long v = 0;
    int digits = 0;
    while (is_digit(*p)) {
        if (++digits > 6) return -1;
        v = v * 10 + (*p - '0');
        p++;
    }
    *pp = p;
    return digits > 0 ? v : -1;
}

// "<label> <w>x<h>" at the start of a line of text: 0 and the size, -1 when absent or invalid.
static int parse_size_line(const char* text, const char* label, int* w, int* h) {
    size_t ln = strlen(label);
    for (const char* p = strstr(text, label); p; p = strstr(p + 1, label)) {
        if (p != text && p[-1] != '\n') continue;
        const char* v = p + ln;
        while (*v == ' ') v++;
        long a = parse_dim(&v);
        if (a < 0 || *v != 'x') return -1;
        v++;
        long b = parse_dim(&v);
        if (b < 0 || !dim_ok((int)a) || !dim_ok((int)b)) return -1;
        *w = (int)a;
        *h = (int)b;
        return 0;
    }
    return -1;
}

// `wm size`: "Physical size: 1080x2400" and, when the display is overridden, "Override size: 720x1600"
// (both in the natural orientation). 0 when the physical size was found (*ow = *oh = 0 without an
// override), -1 otherwise.
int soulx_input_parse_wm_size(const char* text, int* pw, int* ph, int* ow, int* oh) {
    if (ow) *ow = 0;
    if (oh) *oh = 0;
    if (!text || !pw || !ph) return -1;
    int w = 0, h = 0;
    if (parse_size_line(text, "Physical size:", &w, &h) != 0) return -1;
    *pw = w;
    *ph = h;
    int a = 0, b = 0;
    if (ow && oh && parse_size_line(text, "Override size:", &a, &b) == 0) {
        *ow = a;
        *oh = b;
    }
    return 0;
}

// hw.lcd.width / hw.lcd.height of the AVD the engine was started with (rewritten only while the
// instance is stopped, so it describes the running engine even when the registry changed since).
static int avd_panel_size(const char* name, int* w, int* h) {
    char path[PATH_MAX];
    if (soulx_path(path, sizeof(path), "avd/soulx_%s.avd/config.ini", name) != 0) return -1;
    struct stat sb;
    if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size > INPUT_AVD_CONFIG_MAX) return -1;
    char* data = NULL;
    size_t len = 0;
    if (soulx_read_file(path, &data, &len) != 0) return -1;
    int pw = 0, ph = 0;
    for (char* line = data; line && *line; ) {
        char* next = strchr(line, '\n');
        if (next) *next++ = '\0';
        long v = 0;
        char* end = NULL;
        if (strncmp(line, "hw.lcd.width=", 13) == 0) {
            v = strtol(line + 13, &end, 10);
            if (end && (*end == '\0' || *end == '\r') && dim_ok((int)v)) pw = (int)v;
        } else if (strncmp(line, "hw.lcd.height=", 14) == 0) {
            v = strtol(line + 14, &end, 10);
            if (end && (*end == '\0' || *end == '\r') && dim_ok((int)v)) ph = (int)v;
        }
        line = next;
    }
    free(data);
    if (pw <= 0 || ph <= 0) return -1;
    *w = pw;
    *h = ph;
    return 0;
}

// Display sizes: the physical panel and an optional override (natural orientation).
static void set_sizes(SoulXInput* in, int pw, int ph, int ow, int oh) {
    bool over = dim_ok(ow) && dim_ok(oh) && (ow != pw || oh != ph);
    int w = over ? ow : pw, h = over ? oh : ph;
    if (w != in->w_nat || h != in->h_nat || pw != in->panel_w || ph != in->panel_h) {
        soulx_log(SOULX_LOG_DEBUG, "'%s': panel %dx%d, display %dx%d%s", in->name, pw, ph, w, h,
                 over ? " (wm size override)" : "");
    }
    in->panel_w = pw;
    in->panel_h = ph;
    in->overridden = over;
    in->w_nat = w;
    in->h_nat = h;
}

// One adb call for the guest's rotation, with sizes = true also its display size (`wm size`). Uses no
// SoulXInput, so any thread may run it. 0 when the rotation was read.
int soulx_input_query_display(const char* serial, bool sizes, SoulXDisplayInfo* d) {
    if (!d) return -1;
    memset(d, 0, sizeof(*d));
    d->rotation = -1;
    if (!serial) return -1;
    char* out = NULL;
    d->adb_status = soulx_adb_shell_nowait(serial, sizes ? INPUT_DISPLAY_CMD : INPUT_ROTATION_CMD, &out,
                                          INPUT_ADB_TIMEOUT_MS);
    if (out) {
        d->rotation = soulx_input_parse_rotation(out);
        d->has_sizes = sizes && soulx_input_parse_wm_size(out, &d->panel_w, &d->panel_h, &d->over_w, &d->over_h) == 0;
    }
    d->sizes_tried = sizes;
    free(out);
    return d->rotation >= 0 ? 0 : -1;
}

// Takes what soulx_input_query_display read (cheap: the SoulXInput's own thread may call it any time).
void soulx_input_apply_display(SoulXInput* in, const SoulXDisplayInfo* d) {
    if (!in || !d) return;
    if (d->sizes_tried) {
        if (d->has_sizes) {
            set_sizes(in, d->panel_w, d->panel_h, d->over_w, d->over_h);
        } else {
            soulx_log(SOULX_LOG_DEBUG, "cannot read the display size of '%s'; keeping %dx%d", in->name, in->w_nat,
                     in->h_nat);
        }
        in->sizes_stale = false;                        // tried: the next refresh only reads the rotation
    }
    in->rotation_ms = soulx_now_ms();
    if (d->rotation < 0) {
        // keep the last known rotation; retry later, less often while it keeps failing
        in->rotation_ttl = in->rotation_ttl * 2 > INPUT_ROTATION_MAX_MS ? INPUT_ROTATION_MAX_MS : in->rotation_ttl * 2;
        soulx_log(SOULX_LOG_DEBUG, "cannot read the display rotation of '%s' (adb status %d); keeping %d degrees",
                 in->name, d->adb_status, in->rotation * 90);
        return;
    }
    in->rotation_ttl = INPUT_ROTATION_TTL_MS;
    if (d->rotation != in->rotation) {
        soulx_log(SOULX_LOG_DEBUG, "'%s': display rotation %d -> %d degrees", in->name, in->rotation * 90,
                 d->rotation * 90);
    }
    in->rotation = d->rotation;
}

void soulx_input_set_display_pushed(SoulXInput* in, bool pushed) {
    if (in) in->display_pushed = pushed;
}

// Reads the guest's rotation, with full = true also its display size (`wm size`).
static int read_display(SoulXInput* in, bool full) {
    SoulXDisplayInfo d;
    int rc = soulx_input_query_display(in->serial, full, &d);
    soulx_input_apply_display(in, &d);
    return rc;
}

// Re-reads a stale rotation (and the sizes after an engine restart); never while a finger is down
// (a gesture keeps its mapping).
static void refresh_if_stale(SoulXInput* in) {
    if (in->display_pushed || any_finger_down(in)) return;
    double now = soulx_now_ms();
    double ttl = in->rotation_ttl;
    if (now < in->watch_until && ttl > INPUT_ROTATION_FAST_MS) ttl = INPUT_ROTATION_FAST_MS;
    if (!in->sizes_stale && in->rotation_ms > 0 && now - in->rotation_ms < ttl) return;
    (void)read_display(in, in->sizes_stale);
}

// The orientation may change soon (an app was launched or stopped, a key or a console command was
// sent): re-read it often for a while.
void soulx_input_watch_rotation(SoulXInput* in) {
    if (in) in->watch_until = soulx_now_ms() + INPUT_ROTATION_WATCH_MS;
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

// (Re)connects to the console; the instance may have been restarted meanwhile.
static int input_connect(SoulXInput* in) {
    if (in->con) {
        soulx_console_close(in->con);
        in->con = NULL;
    }
    SoulXRunState st;
    if (!soulx_instance_running(in->name, &st)) {
        soulx_log(SOULX_LOG_ERROR, "instance '%s' is not running", in->name);
        return -1;
    }
    if (st.port != in->port || st.pid != in->pid) {
        // Finger states are kept: a command may be in flight (lifting a finger that is not down
        // is harmless, forgetting one that is would leave a stuck touch).
        soulx_log(SOULX_LOG_DEBUG, "instance '%s' was restarted (console port %d -> %d)", in->name, in->port, st.port);
        in->port = st.port;
        in->pid = st.pid;
        soulx_serial(st.port, in->serial, sizeof(in->serial));
        in->rotation_ms = 0;
        in->sizes_stale = true;
    }
    in->con = soulx_console_open(in->port);
    if (!in->con) {
        if (!soulx_interrupted()) {
            soulx_log(SOULX_LOG_ERROR, "cannot connect to the emulator console of '%s' (port %d)", in->name, in->port);
        }
        return -1;
    }
    return 0;
}

// Commands that may reach the emulator twice without harm: touch frames (the same frame again
// changes nothing) and screenshots. `event text`, rotate, sms, ... must never run twice.
static bool cmd_retry_safe(const char* cmd) {
    return strncmp(cmd, "event send ", 11) == 0 || strncmp(cmd, "screenrecord screenshot ", 24) == 0;
}

// One console command; when the connection broke it is reopened once and the command sent again,
// unless it may already have run and is not safe to repeat.
// 0 = OK, 1 = KO (reply holds the message), -1 = failure.
static int input_console(SoulXInput* in, const char* cmd, char* reply, size_t cap) {
    if (reply && cap) reply[0] = '\0';
    if (in->con) {
        int rc = soulx_console_cmd(in->con, cmd, reply, cap);
        if (rc >= 0) return rc;
        int why = soulx_console_failure(in->con);
        soulx_console_close(in->con);
        in->con = NULL;
        if (soulx_interrupted() || why == CONSOLE_FAIL_INVALID) return -1;  // Ctrl+C cut the reply short
        if (why == CONSOLE_FAIL_NO_ANSWER && !cmd_retry_safe(cmd)) {
            soulx_log(SOULX_LOG_ERROR, "the emulator console of '%s' did not answer in time; the command is not "
                     "sent again because it may already have run", in->name);
            return -1;
        }
        soulx_log(SOULX_LOG_DEBUG, "console connection of '%s' lost; reconnecting", in->name);
    }
    if (input_connect(in) != 0) return -1;
    int rc = soulx_console_cmd(in->con, cmd, reply, cap);
    if (rc < 0) {
        soulx_console_close(in->con);
        in->con = NULL;
        if (!soulx_interrupted()) soulx_log(SOULX_LOG_ERROR, "the emulator console of '%s' is not responding", in->name);
    }
    return rc;
}

// Lifting fingers must also work after Ctrl+C, but console.c stops waiting for replies once
// g_soulx_interrupted is set: the flag is cleared for this one command and restored afterwards
// (a second Ctrl+C meanwhile sets it again and still aborts the wait).
static int input_console_always(SoulXInput* in, const char* cmd, char* reply, size_t cap) {
    sig_atomic_t was = g_soulx_interrupted;
    if (was) g_soulx_interrupted = 0;
    int rc = input_console(in, cmd, reply, cap);
    if (was) g_soulx_interrupted = was;
    return rc;
}

// ---------------------------------------------------------------------------
// Touch frames
// ---------------------------------------------------------------------------

static int send_touch_frame(SoulXInput* in, const char* cmd, bool always) {
    char reply[256];
    int rc = always ? input_console_always(in, cmd, reply, sizeof(reply))
                    : input_console(in, cmd, reply, sizeof(reply));
    if (rc == 1) soulx_log(SOULX_LOG_ERROR, "the emulator rejected a touch event: %s", reply);
    return rc == 0 ? 0 : -1;
}

// Lifts every finger of |mask| that is down, in one frame.
static int release_fingers(SoulXInput* in, unsigned mask) {
    char cmd[INPUT_CMD_MAX];
    int n = snprintf(cmd, sizeof(cmd), "event send");
    if (!fmt_ok(n, sizeof(cmd))) return -1;
    size_t len = (size_t)n;
    unsigned lifted = 0;
    for (int f = 0; f < SOULX_MAX_FINGERS; f++) {
        if (!(mask & (1u << f)) || !in->fingers[f].down) continue;
        n = snprintf(cmd + len, sizeof(cmd) - len,
                     " EV_ABS:ABS_MT_SLOT:%d EV_ABS:ABS_MT_PRESSURE:0 EV_ABS:ABS_MT_TRACKING_ID:-1", f);
        if (!fmt_ok(n, sizeof(cmd) - len)) return -1;
        len += (size_t)n;
        lifted |= 1u << f;
    }
    if (!lifted) return 0;
    n = snprintf(cmd + len, sizeof(cmd) - len, " EV_SYN:0:0");
    if (!fmt_ok(n, sizeof(cmd) - len)) return -1;
    if (send_touch_frame(in, cmd, true) != 0) {
        soulx_log(SOULX_LOG_ERROR, "could not lift the finger(s) on '%s'", in->name);
        return -1;
    }
    for (int f = 0; f < SOULX_MAX_FINGERS; f++) {
        if (lifted & (1u << f)) in->fingers[f].down = false;
    }
    return 0;
}

static int finger_down(SoulXInput* in, int f, int rx, int ry) {
    InputFinger* fi = &in->fingers[f];
    if (fi->down && release_fingers(in, 1u << f) != 0) return -1;   // a new touch, not a move
    int id = in->next_id;
    in->next_id = (in->next_id + 1) & INPUT_TRACKING_MASK;
    char cmd[INPUT_CMD_MAX];
    if (!fmt_ok(snprintf(cmd, sizeof(cmd),
                         "event send EV_ABS:ABS_MT_SLOT:%d EV_ABS:ABS_MT_TRACKING_ID:%d EV_ABS:ABS_MT_POSITION_X:%d "
                         "EV_ABS:ABS_MT_POSITION_Y:%d EV_ABS:ABS_MT_PRESSURE:%d EV_SYN:0:0",
                         f, id, rx, ry, INPUT_PRESSURE), sizeof(cmd))) {
        return -1;
    }
    // Marked down before sending: when the reply is lost the finger may be down and must be lifted.
    fi->down = true;
    fi->id = id;
    fi->rx = rx;
    fi->ry = ry;
    return send_touch_frame(in, cmd, false);
}

static int finger_move(SoulXInput* in, int f, int rx, int ry) {
    InputFinger* fi = &in->fingers[f];
    if (fi->rx == rx && fi->ry == ry) return 0;                     // the kernel would drop it anyway
    char cmd[INPUT_CMD_MAX];
    if (!fmt_ok(snprintf(cmd, sizeof(cmd),
                         "event send EV_ABS:ABS_MT_SLOT:%d EV_ABS:ABS_MT_POSITION_X:%d EV_ABS:ABS_MT_POSITION_Y:%d "
                         "EV_SYN:0:0", f, rx, ry), sizeof(cmd))) {
        return -1;
    }
    if (send_touch_frame(in, cmd, false) != 0) return -1;
    fi->rx = rx;
    fi->ry = ry;
    return 0;
}

static int free_finger(const SoulXInput* in) {
    for (int f = 0; f < SOULX_MAX_FINGERS; f++) {
        if (!in->fingers[f].down) return f;
    }
    soulx_log(SOULX_LOG_ERROR, "all %d fingers are already down on '%s'", SOULX_MAX_FINGERS, in->name);
    return -1;
}

static bool duration_ok(const char* what, int ms) {
    if (ms >= 0 && ms <= INPUT_GESTURE_MAX_MS) return true;
    soulx_log(SOULX_LOG_ERROR, "%s duration %d ms is out of range (0-%d)", what, ms, INPUT_GESTURE_MAX_MS);
    return false;
}

static int begin_gesture(SoulXInput* in) {
    if (soulx_interrupted()) return -1;
    refresh_if_stale(in);
    return 0;
}

// Puts a free finger down at (x, y), holds it for hold_ms and lifts it again.
static int press(SoulXInput* in, int x, int y, int hold_ms) {
    if (begin_gesture(in) != 0) return -1;
    int rx, ry;
    if (point_to_raw(in, x, y, true, &rx, &ry) != 0) return -1;
    int f = free_finger(in);
    if (f < 0) return -1;
    int rc = finger_down(in, f, rx, ry);
    if (rc == 0) rc = soulx_sleep_ms(hold_ms);
    if (release_fingers(in, 1u << f) != 0) rc = -1;
    return rc == 0 ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Keys and text
// ---------------------------------------------------------------------------

typedef struct {
    const char* name;            // Android KeyEvent name without "KEYCODE_"
    int code;
} InputKey;

// A-Z, F1-F12 and NUMPAD_0-9 are computed in soulx_input_keycode().
static const InputKey k_keys[] = {
    { "SOFT_LEFT", 1 }, { "SOFT_RIGHT", 2 }, { "HOME", 3 }, { "BACK", 4 }, { "CALL", 5 }, { "ENDCALL", 6 },
    { "0", 7 }, { "1", 8 }, { "2", 9 }, { "3", 10 }, { "4", 11 }, { "5", 12 }, { "6", 13 }, { "7", 14 },
    { "8", 15 }, { "9", 16 }, { "STAR", 17 }, { "POUND", 18 },
    { "DPAD_UP", 19 }, { "DPAD_DOWN", 20 }, { "DPAD_LEFT", 21 }, { "DPAD_RIGHT", 22 }, { "DPAD_CENTER", 23 },
    { "VOLUME_UP", 24 }, { "VOLUME_DOWN", 25 }, { "POWER", 26 }, { "CAMERA", 27 }, { "CLEAR", 28 },
    { "COMMA", 55 }, { "PERIOD", 56 }, { "ALT_LEFT", 57 }, { "ALT_RIGHT", 58 }, { "SHIFT_LEFT", 59 },
    { "SHIFT_RIGHT", 60 }, { "TAB", 61 }, { "SPACE", 62 }, { "SYM", 63 }, { "EXPLORER", 64 }, { "ENVELOPE", 65 },
    { "ENTER", 66 }, { "DEL", 67 }, { "GRAVE", 68 }, { "MINUS", 69 }, { "EQUALS", 70 }, { "LEFT_BRACKET", 71 },
    { "RIGHT_BRACKET", 72 }, { "BACKSLASH", 73 }, { "SEMICOLON", 74 }, { "APOSTROPHE", 75 }, { "SLASH", 76 },
    { "AT", 77 }, { "NUM", 78 }, { "HEADSETHOOK", 79 }, { "FOCUS", 80 }, { "PLUS", 81 }, { "MENU", 82 },
    { "NOTIFICATION", 83 }, { "SEARCH", 84 }, { "MEDIA_PLAY_PAUSE", 85 }, { "MEDIA_STOP", 86 },
    { "MEDIA_NEXT", 87 }, { "MEDIA_PREVIOUS", 88 }, { "MEDIA_REWIND", 89 }, { "MEDIA_FAST_FORWARD", 90 },
    { "MUTE", 91 }, { "PAGE_UP", 92 }, { "PAGE_DOWN", 93 },
    { "BUTTON_A", 96 }, { "BUTTON_B", 97 }, { "BUTTON_C", 98 }, { "BUTTON_X", 99 }, { "BUTTON_Y", 100 },
    { "BUTTON_Z", 101 }, { "BUTTON_L1", 102 }, { "BUTTON_R1", 103 }, { "BUTTON_L2", 104 }, { "BUTTON_R2", 105 },
    { "BUTTON_THUMBL", 106 }, { "BUTTON_THUMBR", 107 }, { "BUTTON_START", 108 }, { "BUTTON_SELECT", 109 },
    { "BUTTON_MODE", 110 }, { "ESCAPE", 111 }, { "FORWARD_DEL", 112 }, { "CTRL_LEFT", 113 }, { "CTRL_RIGHT", 114 },
    { "CAPS_LOCK", 115 }, { "SCROLL_LOCK", 116 }, { "META_LEFT", 117 }, { "META_RIGHT", 118 }, { "FUNCTION", 119 },
    { "SYSRQ", 120 }, { "BREAK", 121 }, { "MOVE_HOME", 122 }, { "MOVE_END", 123 }, { "INSERT", 124 },
    { "FORWARD", 125 }, { "MEDIA_PLAY", 126 }, { "MEDIA_PAUSE", 127 }, { "MEDIA_CLOSE", 128 },
    { "MEDIA_EJECT", 129 }, { "MEDIA_RECORD", 130 }, { "NUM_LOCK", 143 },
    { "NUMPAD_DIVIDE", 154 }, { "NUMPAD_MULTIPLY", 155 }, { "NUMPAD_SUBTRACT", 156 }, { "NUMPAD_ADD", 157 },
    { "NUMPAD_DOT", 158 }, { "NUMPAD_COMMA", 159 }, { "NUMPAD_ENTER", 160 }, { "NUMPAD_EQUALS", 161 },
    { "NUMPAD_LEFT_PAREN", 162 }, { "NUMPAD_RIGHT_PAREN", 163 }, { "VOLUME_MUTE", 164 }, { "INFO", 165 },
    { "CHANNEL_UP", 166 }, { "CHANNEL_DOWN", 167 }, { "ZOOM_IN", 168 }, { "ZOOM_OUT", 169 }, { "SETTINGS", 176 },
    { "APP_SWITCH", 187 }, { "LANGUAGE_SWITCH", 204 }, { "CONTACTS", 207 }, { "CALENDAR", 208 }, { "MUSIC", 209 },
    { "CALCULATOR", 210 }, { "ASSIST", 219 }, { "BRIGHTNESS_DOWN", 220 }, { "BRIGHTNESS_UP", 221 },
    { "SLEEP", 223 }, { "WAKEUP", 224 }, { "VOICE_ASSIST", 231 }, { "HELP", 259 }, { "CUT", 277 }, { "COPY", 278 },
    { "PASTE", 279 }, { "ALL_APPS", 284 }, { "REFRESH", 285 },
    // aliases
    { "RECENTS", 187 }, { "BACKSPACE", 67 }, { "ESC", 111 }, { "RETURN", 66 }, { "UP", 19 }, { "DOWN", 20 },
    { "LEFT", 21 }, { "RIGHT", 22 }, { "CENTER", 23 }, { "PLAY_PAUSE", 85 },
};

// "BACK", "back", "KEYCODE_BACK", "F5", "A" or a decimal key code ("4") -> Android key code; -1 if unknown.
// Bare digits are key codes; the digit keys are "KEYCODE_0".."KEYCODE_9".
int soulx_input_keycode(const char* name_or_code) {
    if (!name_or_code) return -1;
    size_t n = strnlen(name_or_code, INPUT_KEY_NAME_MAX + 1);
    if (n == 0 || n > INPUT_KEY_NAME_MAX) return -1;
    bool digits = true;
    for (size_t i = 0; i < n; i++) {
        char c = name_or_code[i];
        if (!is_alpha(c) && !is_digit(c) && c != '_') return -1;
        if (!is_digit(c)) digits = false;
    }
    if (digits) {
        int v = 0;
        for (size_t i = 0; i < n; i++) {
            v = v * 10 + (name_or_code[i] - '0');
            if (v > INPUT_KEYCODE_MAX) return -1;
        }
        return v >= 1 ? v : -1;
    }

    const char* k = name_or_code;
    if (n > 8 && strncasecmp(k, "KEYCODE_", 8) == 0) k += 8;
    size_t kn = strlen(k);
    if (kn == 1 && is_alpha(k[0])) {
        char up = (k[0] >= 'a') ? (char)(k[0] - 'a' + 'A') : k[0];
        return 29 + (up - 'A');                                     // KEYCODE_A = 29
    }
    if ((k[0] == 'F' || k[0] == 'f') && (kn == 2 || kn == 3) && is_digit(k[1]) && (kn == 2 || is_digit(k[2]))) {
        int fn = kn == 2 ? k[1] - '0' : (k[1] - '0') * 10 + (k[2] - '0');
        if (fn >= 1 && fn <= 12) return 130 + fn;                   // KEYCODE_F1 = 131
    }
    if (kn == 8 && strncasecmp(k, "NUMPAD_", 7) == 0 && is_digit(k[7])) return 144 + (k[7] - '0');
    for (size_t i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]); i++) {
        if (strcasecmp(k, k_keys[i].name) == 0) return k_keys[i].code;
    }
    return -1;
}

static int adb_keyevent(SoulXInput* in, int code) {
    char cmd[64];
    if (!fmt_ok(snprintf(cmd, sizeof(cmd), "input keyevent %d", code), sizeof(cmd))) return -1;
    char* out = NULL;
    int rc = soulx_adb_shell(in->serial, cmd, &out, INPUT_KEY_TIMEOUT_MS);
    free(out);
    if (rc != 0) {
        soulx_log(SOULX_LOG_ERROR, "could not send key code %d to '%s' (adb exit status %d)", code, in->name, rc);
        return -1;
    }
    return 0;
}

// Key strokes the emulator's qwerty2 keyboard needs for a printable ASCII character: 1, or 2 for the
// characters typed with Shift (capitals and !"#$%&()*+:<>?@^_{|}~). Every other printable ASCII
// character is on a key of its own.
int soulx_input_key_units(char c) {
    if (c >= 'A' && c <= 'Z') return 2;
    return (c != '\0' && strchr("!\"#$%&()*+:<>?@^_{|}~", c)) ? 2 : 1;
}

// Length of the longest prefix of s[0..n) that is printable and fits into one `event text`
// (INPUT_TEXT_CHUNK_UNITS key strokes); 0 when s starts with a non-printable character.
size_t soulx_input_text_chunk(const char* s, size_t n) {
    if (!s) return 0;
    size_t i = 0;
    int units = 0;
    while (i < n && is_printable(s[i])) {
        int u = soulx_input_key_units(s[i]);
        if (units + u > INPUT_TEXT_CHUNK_UNITS) break;
        units += u;
        i++;
    }
    return i;
}

static int text_units(const char* s, size_t n) {
    int units = 0;
    for (size_t i = 0; i < n; i++) units += soulx_input_key_units(s[i]);
    return units;
}

// About how long soulx_input_text() takes for s[0..n) (for the macro recorder's timeline).
int soulx_input_text_cost_ms(const char* s, size_t n) {
    if (!s) return 0;
    long long ms = 0;
    size_t i = 0;
    while (i < n) {
        if (s[i] == '\n' || s[i] == '\t') {
            ms += INPUT_KEY_COST_MS;
            i++;
            continue;
        }
        size_t k = soulx_input_text_chunk(s + i, n - i);
        if (k == 0) {
            i++;
            continue;
        }
        ms += INPUT_TEXT_GAP_MS + (long long)text_units(s + i, k) * INPUT_TEXT_PACE_MS;
        i += k;
    }
    return ms > INT_MAX ? INT_MAX : (int)ms;
}

// "event text <arg>" for n printable ASCII characters: the console drops the spaces that start
// <arg> and un-escapes \\ \n \xNN \uNNNN (anything else after a backslash is an error), so
// backslashes are doubled and leading spaces are sent as \x20. 0 ok, -1 invalid / too long.
int soulx_input_console_text_cmd(const char* s, size_t n, char* cmd, size_t cap) {
    if (!s || !cmd || cap == 0) return -1;
    int k = snprintf(cmd, cap, "event text ");
    if (!fmt_ok(k, cap)) return -1;
    size_t len = (size_t)k;
    bool leading = true;
    for (size_t i = 0; i < n; i++) {
        if (!is_printable(s[i])) return -1;
        char one[2] = { s[i], '\0' };
        const char* piece = one;
        if (s[i] == ' ' && leading) piece = "\\x20";
        else if (s[i] == '\\') piece = "\\\\";
        if (s[i] != ' ') leading = false;
        size_t pl = strlen(piece);
        if (len + pl >= cap) return -1;
        memcpy(cmd + len, piece, pl);
        len += pl;
    }
    cmd[len] = '\0';
    return 0;
}

// "input text <arg>" for the guest shell (n printable ASCII characters): a space becomes "%s"
// (input's own escape for it) and every other character except letters and digits gets a
// backslash (sh passes it on literally). 0 ok, -1 invalid / too long.
int soulx_input_shell_text_cmd(const char* s, size_t n, char* cmd, size_t cap) {
    if (!s || !cmd || cap == 0) return -1;
    int k = snprintf(cmd, cap, "input text ");
    if (!fmt_ok(k, cap)) return -1;
    size_t len = (size_t)k;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!is_printable(c)) return -1;
        char piece[3] = { c, '\0', '\0' };
        if (c == ' ') {
            piece[0] = '%';
            piece[1] = 's';
        } else if (!is_alpha(c) && !is_digit(c)) {
            piece[0] = '\\';
            piece[1] = c;
        }
        size_t pl = strlen(piece);
        if (len + pl >= cap) return -1;
        memcpy(cmd + len, piece, pl);
        len += pl;
    }
    cmd[len] = '\0';
    return 0;
}

// Types printable ASCII with `adb shell input text`. input turns "%s" into a space, so a literal
// '%' followed by 's' ends one call and the 's' starts the next one.
static int type_with_adb(SoulXInput* in, const char* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t end = i;
        while (end < n) {
            end++;
            if (s[end - 1] == '%' && end < n && s[end] == 's') break;
        }
        char cmd[INPUT_CMD_MAX];
        if (soulx_input_shell_text_cmd(s + i, end - i, cmd, sizeof(cmd)) != 0) return -1;
        char* out = NULL;
        int rc = soulx_adb_shell(in->serial, cmd, &out, INPUT_KEY_TIMEOUT_MS);
        free(out);
        if (rc != 0) {
            soulx_log(SOULX_LOG_ERROR, "could not type text on '%s' (adb exit status %d)", in->name, rc);
            return -1;
        }
        i = end;
    }
    return 0;
}

// One `event text` of at most INPUT_TEXT_CHUNK_UNITS key strokes (soulx_input_text_chunk). The
// emulator answers OK even when it drops a longer one, so the limit is never exceeded; after a lost
// reply the chunk is not sent again (it may have been typed).
static int type_chunk(SoulXInput* in, const char* s, size_t n) {
    char cmd[INPUT_CMD_MAX];
    char reply[256];
    if (soulx_input_console_text_cmd(s, n, cmd, sizeof(cmd)) != 0) return -1;
    int rc = input_console(in, cmd, reply, sizeof(reply));
    if (rc == 0) {
        // The emulator only queues the key strokes: give it and the guest time to type them before the
        // next chunk (or anything else, e.g. key("ENTER") through adb) follows.
        int pause = INPUT_TEXT_GAP_MS + text_units(s, n) * INPUT_TEXT_PACE_MS;
        return soulx_sleep_ms(pause) == 0 ? 0 : -1;
    }
    if (rc < 0) return -1;
    soulx_log(SOULX_LOG_DEBUG, "console 'event text' refused (%s); using adb input text", reply);
    return type_with_adb(in, s, n);
}

// ---------------------------------------------------------------------------
// Console screenshot (fast capture)
// ---------------------------------------------------------------------------

static bool shot_name(const char* name) {
    size_t n = strlen(name);
    size_t pl = strlen(INPUT_SHOT_PREFIX);
    return n > pl + 4 && strncmp(name, INPUT_SHOT_PREFIX, pl) == 0 && strcmp(name + n - 4, ".png") == 0;
}

static void shot_failed(SoulXInput* in, const char* why) {
    soulx_log(SOULX_LOG_DEBUG, "console screenshot of '%s' not usable (%s); using adb screencap", in->name, why);
    if (++in->shot_fails >= INPUT_SHOT_MAX_FAILS && !in->shot_disabled) {
        in->shot_disabled = true;
        soulx_log(SOULX_LOG_DEBUG, "console screenshots disabled for '%s'", in->name);
    }
}

// The screenshot folder must be a real directory of ours whose path has no blanks (the console
// splits its arguments at spaces). It is (re)created before every screenshot: the engine cannot
// write into a folder that does not exist (and still answers OK).
static bool shots_dir_ready(SoulXInput* in) {
    if (in->shot_disabled) return false;
    if (in->shots[0] != '/' || strpbrk(in->shots, " \t\r\n")) {
        in->shot_disabled = true;
        soulx_log(SOULX_LOG_DEBUG, "console screenshots disabled: unusable folder %s", in->shots);
        return false;
    }
    if (soulx_mkdirs(in->shots) != 0) {
        shot_failed(in, "cannot create the screenshot folder");
        return false;
    }
    struct stat sb;
    if (lstat(in->shots, &sb) != 0 || !S_ISDIR(sb.st_mode) || sb.st_uid != getuid()) {
        in->shot_disabled = true;
        soulx_log(SOULX_LOG_DEBUG, "console screenshots disabled: %s is not a private folder", in->shots);
        return false;
    }
    return true;
}

// Deletes old Screenshot_*.png files (a stale file must never be taken for a new one).
static void shots_clear(const char* dir) {
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) return;
    int dfd2 = dup(dfd);
    DIR* d = dfd2 >= 0 ? fdopendir(dfd2) : NULL;
    if (!d) {
        if (dfd2 >= 0) close(dfd2);
        close(dfd);
        return;
    }
    char names[INPUT_SHOT_MAX_NAMES][NAME_MAX + 1];
    int count = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && count < INPUT_SHOT_MAX_NAMES) {
        if (!shot_name(e->d_name) || strlen(e->d_name) > NAME_MAX) continue;
        memcpy(names[count], e->d_name, strlen(e->d_name) + 1);
        count++;
    }
    closedir(d);
    for (int i = 0; i < count; i++) (void)unlinkat(dfd, names[i], 0);
    close(dfd);
}

// Removes the screenshot folders of processes that are gone ($SOULX_HOME/tmp/shots-<name>-<pid>, and
// the empty shots-<name> of older versions). Each process has a folder of its own, so a script, the
// window and a `soulx screenshot` never take or delete each other's files.
static void shots_sweep(const char* name) {
    char tmp[PATH_MAX], prefix[SOULX_NAME_MAX + 16];
    if (soulx_path(tmp, sizeof(tmp), "tmp") != 0 || !fmt_ok(snprintf(prefix, sizeof(prefix), "shots-%s", name),
                                                             sizeof(prefix))) {
        return;
    }
    DIR* d = opendir(tmp);
    if (!d) return;
    char stale[8][NAME_MAX + 1];
    int count = 0;
    size_t pl = strlen(prefix);
    struct dirent* e;
    while ((e = readdir(d)) != NULL && count < 8) {
        if (strncmp(e->d_name, prefix, pl) != 0 || strlen(e->d_name) > NAME_MAX) continue;
        const char* rest = e->d_name + pl;
        if (*rest == '\0') {                                        // shots-<name>: remove when empty
            memcpy(stale[count++], e->d_name, strlen(e->d_name) + 1);
            continue;
        }
        if (*rest != '-' || !is_digit(rest[1])) continue;
        char* end = NULL;
        long pid = strtol(rest + 1, &end, 10);
        if (!end || *end != '\0' || pid <= 0 || pid > INT_MAX || pid == (long)getpid()) continue;
        if (kill((pid_t)pid, 0) == 0 || errno == EPERM) continue;  // still running
        memcpy(stale[count++], e->d_name, strlen(e->d_name) + 1);
    }
    closedir(d);
    for (int i = 0; i < count; i++) {
        char path[PATH_MAX];
        if (!fmt_ok(snprintf(path, sizeof(path), "%s/%s", tmp, stale[i]), sizeof(path))) continue;
        if (strchr(stale[i] + pl, '-')) {
            shots_clear(path);
            (void)rmdir(path);
        } else {
            (void)rmdir(path);                                      // fails unless empty: that is fine
        }
    }
}

static bool png_complete(const unsigned char* d, size_t n) {
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    static const unsigned char iend[12] = { 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82 };
    return n >= sizeof(sig) + sizeof(iend) && memcmp(d, sig, sizeof(sig)) == 0 &&
           memcmp(d + n - sizeof(iend), iend, sizeof(iend)) == 0;
}

// Reads the newest screenshot once it is complete (the emulator writes it after replying).
// 1 = *data holds a complete PNG (file deleted), 0 = not there / not complete yet, -1 = error.
static int shots_take(const char* dir, unsigned char** data, size_t* len) {
    *data = NULL;
    *len = 0;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) return -1;
    int dfd2 = dup(dfd);
    DIR* d = dfd2 >= 0 ? fdopendir(dfd2) : NULL;
    if (!d) {
        if (dfd2 >= 0) close(dfd2);
        close(dfd);
        return -1;
    }
    char name[NAME_MAX + 1] = "";
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (shot_name(e->d_name) && strlen(e->d_name) <= NAME_MAX && strcmp(e->d_name, name) > 0) {
            memcpy(name, e->d_name, strlen(e->d_name) + 1);          // names sort by time
        }
    }
    closedir(d);
    if (!name[0]) {
        close(dfd);
        return 0;
    }

    int rc = 0;
    int fd = openat(dfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    struct stat sb;
    if (fd < 0) {
        rc = errno == ENOENT ? 0 : -1;
    } else if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || (uint64_t)sb.st_size > SOULX_IMAGE_MAX_FILE) {
        rc = -1;
    } else if (sb.st_size > 0) {
        size_t want = (size_t)sb.st_size;
        unsigned char* buf = malloc(want);
        if (!buf) {
            rc = -1;
        } else {
            size_t got = 0;
            while (got < want) {
                ssize_t r = read(fd, buf + got, want - got);
                if (r < 0 && errno == EINTR) continue;
                if (r <= 0) break;
                got += (size_t)r;
            }
            if (png_complete(buf, got)) {
                *data = buf;
                *len = got;
                rc = 1;
            } else {
                free(buf);
            }
        }
    }
    if (fd >= 0) close(fd);
    if (rc != 0) (void)unlinkat(dfd, name, 0);
    close(dfd);
    return rc;
}

// Panel-native framebuffer -> logical orientation.
static int framebuffer_to_logical(const SoulXInput* in, SoulXImage* fb, SoulXImage* out) {
    int lw, lh;
    logical_size(in, &lw, &lh);
    if (fb->w == in->w_nat && fb->h == in->h_nat) {
        if (in->rotation == 0) {                // as is
            *out = *fb;
            fb->rgba = NULL;
            return 0;
        }
        return soulx_image_rotate_to_logical(fb, in->rotation, out);
    }
    if (fb->w == lw && fb->h == lh) {           // the emulator itself rotated to match the guest
        *out = *fb;
        fb->rgba = NULL;
        return 0;
    }
    return -1;
}

// The frame the engine shares with SoulX's window: the framebuffer a console screenshot would return, read
// straight from memory (about 1 ms for 1080x2400 on a still screen, at most one frame period more while the
// screen moves: a complete copy needs the moment right after a publish), against a PNG the engine encodes and
// writes, and SoulX reads back and decodes. Remapped when the engine replaced the region (a restart).
static int capture_shared(SoulXInput* in, SoulXImage* img) {
    if (in->overridden) return -1;              // the panel cannot show a `wm size` override's geometry
    if (in->frames && !soulx_frames_valid(in->frames)) {
        soulx_frames_close(in->frames);
        in->frames = NULL;
    }
    if (!in->frames) in->frames = soulx_frames_open(in->port);
    if (!in->frames) return -1;
    SoulXFrameView v;
    if (soulx_frames_peek(in->frames, &v) < 0) return -1;
    size_t bytes = (size_t)v.width * (size_t)v.height * 4u;
    SoulXImage fb = { v.width, v.height, malloc(bytes) };
    if (!fb.rgba) return -1;
    if (soulx_frames_copy_fresh(in->frames, fb.rgba, bytes, &v, true, INPUT_FRESH_MS) != 0 || v.width != fb.w ||
        v.height != fb.h) {
        soulx_image_free(&fb);                   // resized meanwhile, or no complete copy in time
        return -1;
    }
    int rc = framebuffer_to_logical(in, &fb, img);
    soulx_image_free(&fb);
    return rc;
}

static int capture_console(SoulXInput* in, SoulXImage* img) {
    // The framebuffer is always the panel: it cannot show a `wm size` override's geometry
    if (in->overridden || !shots_dir_ready(in)) return -1;
    shots_clear(in->shots);

    char cmd[PATH_MAX + 32];
    if (!fmt_ok(snprintf(cmd, sizeof(cmd), "screenrecord screenshot %s", in->shots), sizeof(cmd))) {
        in->shot_disabled = true;
        return -1;
    }
    char reply[256];
    int rc = input_console(in, cmd, reply, sizeof(reply));
    if (rc != 0) {
        if (rc == 1) shot_failed(in, reply);
        return -1;
    }

    unsigned char* data = NULL;
    size_t len = 0;
    double deadline = soulx_now_ms() + INPUT_SHOT_WAIT_MS;
    for (;;) {
        int t = shots_take(in->shots, &data, &len);
        if (t == 1) break;
        if (t < 0 || soulx_now_ms() >= deadline) {
            shots_clear(in->shots);
            shot_failed(in, t < 0 ? "cannot read the screenshot file" : "no screenshot file appeared");
            return -1;
        }
        if (soulx_sleep_ms(INPUT_SHOT_POLL_MS) != 0) {
            shots_clear(in->shots);
            return -1;
        }
    }

    SoulXImage fb;
    if (soulx_image_load_mem(data, len, &fb) != 0) {
        free(data);
        shot_failed(in, soulx_image_error());
        return -1;
    }
    free(data);
    rc = framebuffer_to_logical(in, &fb, img);
    if (rc != 0) {
        char why[96];
        snprintf(why, sizeof(why), "framebuffer %dx%d, panel %dx%d, rotation %d", fb.w, fb.h, in->w_nat, in->h_nat,
                 in->rotation * 90);
        shot_failed(in, why);
    } else {
        in->shot_fails = 0;
    }
    soulx_image_free(&fb);
    return rc;
}

// ---------------------------------------------------------------------------
// Public functions
// ---------------------------------------------------------------------------

SoulXInput* soulx_input_open(const char* name) {
    SoulXInstanceConf conf;
    if (soulx_resolve_instance(name, &conf) != 0) {
        soulx_log(SOULX_LOG_DEBUG, "input: no instance '%s'", name && name[0] ? shown(name) : "(default)");
        return NULL;
    }
    SoulXRunState st;
    if (!soulx_instance_running(conf.name, &st)) {
        soulx_log(SOULX_LOG_DEBUG, "input: instance '%s' is not running", conf.name);
        return NULL;
    }
    int pw = conf.width, ph = conf.height;
    int aw = 0, ah = 0;
    if (avd_panel_size(conf.name, &aw, &ah) == 0) {     // what the engine was started with
        pw = aw;
        ph = ah;
    }
    if (!dim_ok(pw) || !dim_ok(ph)) {
        soulx_log(SOULX_LOG_ERROR, "instance '%s' has an invalid screen size %dx%d", conf.name, pw, ph);
        return NULL;
    }

    SoulXInput* in = calloc(1, sizeof(*in));
    if (!in) return NULL;
    memcpy(in->name, conf.name, strnlen(conf.name, SOULX_NAME_MAX));
    in->port = st.port;
    in->pid = st.pid;
    soulx_serial(st.port, in->serial, sizeof(in->serial));
    set_sizes(in, pw, ph, 0, 0);
    in->rotation_ttl = INPUT_ROTATION_TTL_MS;
    in->next_id = (int)arc4random_uniform(INPUT_TRACKING_MASK / 2);    // fresh ids across processes too
    if (soulx_path(in->shots, sizeof(in->shots), "tmp/shots-%s-%d", in->name, (int)getpid()) != 0) {
        in->shot_disabled = true;
    }

    in->con = soulx_console_open(in->port);
    if (!in->con) {
        soulx_log(SOULX_LOG_DEBUG, "input: cannot connect to the console of '%s' (port %d)", in->name, in->port);
        free(in);
        return NULL;
    }
    shots_sweep(in->name);
    (void)read_display(in, true);                       // rotation, and the size the guest really has
    return in;
}

void soulx_input_close(SoulXInput* in) {
    if (!in) return;
    unsigned down = 0;
    for (int f = 0; f < SOULX_MAX_FINGERS; f++) {
        if (in->fingers[f].down) down |= 1u << f;
    }
    if (down) (void)release_fingers(in, down);      // never leave a stuck touch behind
    if (in->con) soulx_console_close(in->con);
    soulx_frames_close(in->frames);
    if (in->shots[0] == '/' && soulx_is_dir(in->shots)) {
        shots_clear(in->shots);
        (void)rmdir(in->shots);
    }
    free(in);
}

int soulx_input_refresh(SoulXInput* in) {
    if (!in) return -1;
    return read_display(in, true);
}

int soulx_input_screen_size(SoulXInput* in, int* w, int* h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (!in) return -1;
    refresh_if_stale(in);
    int lw, lh;
    logical_size(in, &lw, &lh);
    if (w) *w = lw;
    if (h) *h = lh;
    return 0;
}

// The display in its natural orientation (what touches and the macro recorder map onto).
int soulx_input_display_size(SoulXInput* in, int* w_nat, int* h_nat) {
    if (w_nat) *w_nat = 0;
    if (h_nat) *h_nat = 0;
    if (!in) return -1;
    refresh_if_stale(in);
    if (w_nat) *w_nat = in->w_nat;
    if (h_nat) *h_nat = in->h_nat;
    return 0;
}

int soulx_input_rotation(SoulXInput* in) {
    if (!in) return -1;
    refresh_if_stale(in);
    return in->rotation;
}

int soulx_input_touch(SoulXInput* in, int finger, int action, int x, int y) {
    if (!in) return -1;
    if (finger < 0 || finger >= SOULX_MAX_FINGERS) {
        soulx_log(SOULX_LOG_ERROR, "finger %d is out of range (0-%d)", finger, SOULX_MAX_FINGERS - 1);
        return -1;
    }
    int rx, ry;
    switch (action) {
    case SOULX_TOUCH_UP:
        return release_fingers(in, 1u << finger);   // lifts at the last position; no-op when not down
    case SOULX_TOUCH_DOWN:
        if (begin_gesture(in) != 0) return -1;
        if (point_to_raw(in, x, y, true, &rx, &ry) != 0) return -1;
        return finger_down(in, finger, rx, ry);
    case SOULX_TOUCH_MOVE:
        if (soulx_interrupted()) return -1;
        if (!in->fingers[finger].down) {
            soulx_log(SOULX_LOG_ERROR, "finger %d is not down (touch it down first)", finger);
            return -1;
        }
        if (point_to_raw(in, x, y, false, &rx, &ry) != 0) return -1;
        return finger_move(in, finger, rx, ry);
    default:
        soulx_log(SOULX_LOG_ERROR, "unknown touch action %d", action);
        return -1;
    }
}

int soulx_input_tap(SoulXInput* in, int x, int y) {
    if (!in) return -1;
    return press(in, x, y, INPUT_TAP_MS);
}

int soulx_input_long_press(SoulXInput* in, int x, int y, int ms) {
    if (!in || !duration_ok("long press", ms)) return -1;
    return press(in, x, y, ms);
}

int soulx_input_swipe(SoulXInput* in, int x1, int y1, int x2, int y2, int ms) {
    if (!in || !duration_ok("swipe", ms)) return -1;
    if (begin_gesture(in) != 0) return -1;
    int rx1, ry1, rx2, ry2;
    if (point_to_raw(in, x1, y1, true, &rx1, &ry1) != 0 || point_to_raw(in, x2, y2, true, &rx2, &ry2) != 0) {
        return -1;
    }
    int f = free_finger(in);
    if (f < 0) return -1;

    int rc = finger_down(in, f, rx1, ry1);
    int steps = ms / INPUT_FRAME_MS;
    if (steps < 2) steps = 2;
    double t0 = soulx_now_ms();
    for (int i = 1; i <= steps && rc == 0; i++) {
        // frames on an absolute schedule: console latency and sleep granularity do not add up
        double wait = t0 + (double)ms * i / steps - soulx_now_ms();
        if (soulx_sleep_ms(wait > 0 ? (int)(wait + 0.5) : 0) != 0) {
            rc = -1;
            break;
        }
        int rx = rx1 + (int)((long long)(rx2 - rx1) * i / steps);
        int ry = ry1 + (int)((long long)(ry2 - ry1) * i / steps);
        rc = finger_move(in, f, rx, ry);
    }
    if (release_fingers(in, 1u << f) != 0) rc = -1;
    return rc == 0 ? 0 : -1;
}

int soulx_input_key(SoulXInput* in, const char* name_or_code) {
    if (!in) return -1;
    int code = soulx_input_keycode(name_or_code);
    if (code < 0) {
        soulx_log(SOULX_LOG_ERROR,
                 "unknown key '%s' (use a name like BACK, HOME, ENTER, A, F1 or an Android key code number)",
                 shown(name_or_code));
        return -1;
    }
    if (soulx_interrupted()) return -1;
    int rc = adb_keyevent(in, code);
    soulx_input_watch_rotation(in);                  // BACK / HOME may leave a landscape app
    return rc;
}

int soulx_input_text(SoulXInput* in, const char* utf8) {
    if (!in || !utf8) return -1;
    size_t len = strlen(utf8);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)utf8[i];
        if (c >= 0x80) {
            soulx_log(SOULX_LOG_ERROR, "only ASCII text is supported (the text contains a non-ASCII character)");
            return -1;
        }
        if ((c < 0x20 && c != '\n' && c != '\t' && c != '\r') || c == 0x7f) {
            soulx_log(SOULX_LOG_ERROR, "the text contains an unsupported control character (0x%02x)", c);
            return -1;
        }
    }
    size_t i = 0;
    while (i < len) {
        if (soulx_interrupted()) return -1;
        char c = utf8[i];
        if (c == '\r') {
            i++;
            continue;
        }
        if (c == '\n' || c == '\t') {                // typed as the Enter / Tab key
            if (adb_keyevent(in, c == '\n' ? ANDROID_KEYCODE_ENTER : ANDROID_KEYCODE_TAB) != 0) return -1;
            i++;
            continue;
        }
        size_t n = soulx_input_text_chunk(utf8 + i, len - i);
        if (n == 0 || type_chunk(in, utf8 + i, n) != 0) return -1;
        i += n;
    }
    return 0;
}

const char* soulx_input_serial(SoulXInput* in) {
    return in ? in->serial : NULL;
}

int soulx_input_console(SoulXInput* in, const char* cmd, char* reply, size_t cap) {
    if (reply && cap) reply[0] = '\0';
    if (!in || !cmd || !cmd[0]) return -1;
    int rc = input_console(in, cmd, reply, cap);
    if (strncmp(cmd, "event ", 6) != 0) soulx_input_watch_rotation(in);   // e.g. "rotate"
    return rc;
}

int soulx_input_capture(SoulXInput* in, SoulXImage* img) {
    if (!img) return -1;
    img->w = 0;
    img->h = 0;
    img->rgba = NULL;
    if (!in || soulx_interrupted()) return -1;
    refresh_if_stale(in);
    if (capture_shared(in, img) == 0) return 0;
    if (capture_console(in, img) == 0) return 0;
    if (soulx_interrupted()) return -1;
    return soulx_screen_capture(in->serial, img);
}
