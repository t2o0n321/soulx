/*
 * SoulX Android Emulator - macro recorder (`soulx record`)
 *
 * Streams `adb shell getevent -lt` (every input device, labelled, timestamped) and turns what
 * happens on the touchscreen and the keyboards into a Lua script for `soulx run`:
 *   one finger    tap(x, y)                   shorter than 300 ms, moved less than 20 px
 *                 long_press(x, y, ms)        300 ms or longer, moved less than 20 px
 *                 swipe(x1, y1, x2, y2, ms)   start -> end
 *   2+ fingers    touch_down / touch_move / touch_up lines (moves sampled every >= 30 ms);
 *                 also used for one finger held longer than swipe()/long_press() accept (60 s)
 *   keys          key("NAME"); printable keys typed within 1.5 s of each other -> text("...")
 *                 (at most 500 characters per call; Shift and Caps Lock give capitals); a key
 *                 pressed with Ctrl, Alt or Meta held becomes a comment (key() cannot replay chords)
 *   pauses        sleep(ms) for gaps >= 30 ms, rounded to 10 ms (guest event timestamps, so adb
 *                 latency does not distort them; rounding errors are carried, not accumulated).
 *                 Each action is charged what its replay takes (a tap 44 ms however long the finger
 *                 stayed down, a key 40 ms, text its typing time, a swipe its duration), so the
 *                 replayed actions start when the recorded ones did.
 * The touchscreen reports raw positions (0..32767) in the display's natural orientation; they are
 * converted to logical (screenshot) pixels with the display rotation current when each gesture
 * starts. The kernel only reports values that changed, so the recorder keeps per-slot state; at
 * the start the positions of the (unused) touch slots are primed through the console so that a
 * first touch at an unchanged position still reports its coordinates.
 * The recording goes to a temporary file next to the output (flushed after every action) that
 * replaces the output only when the recording succeeded; Ctrl+C stops the recording.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "soulx_mac.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define REC_MAX_DEVICES     64
#define REC_DEV_MAX         96          // device path / name buffers (getevent names are < 80)
#define REC_MAX_SLOTS       16          // ABS_MT_SLOT values tracked (the emulator has 0..10)
#define REC_MAX_FINGERS     SOULX_MAX_FINGERS
#define REC_RAW_SCALE       32768       // natural = raw * dim / 32768
#define REC_RAW_MAX         32767
#define REC_PRIME_SLOTS     10
#define REC_PRIME_RAW       32767       // a click never produces it (x * 32767 / dim < 32767)
#define REC_TAP_MS          300.0
#define REC_STILL_PX        20
#define REC_SAMPLE_MS       30.0        // multi-touch moves: at most one line per finger per 30 ms
#define REC_SLEEP_MIN_MS    30.0
#define REC_TEXT_GAP_MS     1500.0
// What replaying an action takes (input.c: a tap holds the finger 40 ms; measured: Lua tap 44 ms, key 31 ms
// median, swipe/long_press their duration + 2-6 ms). The timeline is charged these, not the recorded
// durations, so that sleep() makes up the difference.
#define REC_TAP_COST_MS     44.0
#define REC_KEY_COST_MS     40.0
#define REC_GESTURE_EXTRA_MS 3.0
#define REC_TMP_TRIES       16          // temporary output names tried
// Limits of the script API (soulx_lua.c): swipe()/long_press() take 1..60000 ms, sleep() at most
// 24 h, text() at most 1000 characters. Longer gestures are written as touch_* lines instead.
#define REC_GESTURE_MAX_MS  60000.0
#define REC_SLEEP_MAX_MS    86400000L
#define REC_TEXT_RUN_MAX    500
#define REC_TEXT_MAX        (REC_TEXT_RUN_MAX + 1)
#define REC_MAX_SAMPLES     200000      // per gesture; further moves are dropped
#define REC_ACTION_MAX      (2 * REC_TEXT_MAX + 32)
#define REC_LINE_MAX        4096        // longest getevent line accepted
#define REC_POLL_MS         100
#define REC_DRAIN_MS        300         // after Ctrl+C: events still in the pipe
#define REC_STOP_WAIT_MS    1000
#define REC_ERR_MAX         2048

#define EM_DASH             "\xE2\x80\x94"

// soulx_getevent_parse() results
#define SOULX_GE_NONE        0           // blank or unrecognised line
#define SOULX_GE_DEVICE      1           // "add device N: <path>"            -> dev = path
#define SOULX_GE_NAME        2           // '  name:     "<name>"'             -> dev = name
#define SOULX_GE_EVENT       3           // "[ <sec>.<usec>] [<path>: ]TYPE CODE VALUE" -> ts_ms (guest
                                        // clock in ms), dev (path, "" in the single-device form),
                                        // type, code, value (DOWN 1 / UP 0 / REPEAT 2, hex as int32)

typedef struct SoulXRecorder SoulXRecorder;
typedef int (*SoulXRecRotationFn)(void* ctx);    // display rotation 0..3 (< 0: unknown -> 0)

// Exported for tests/macos/test_input.c (not part of soulx_mac.h).
int  soulx_getevent_parse(const char* line, double* ts_ms, char* dev, size_t dev_cap, char* type, size_t type_cap,
                         char* code, size_t code_cap, int* value);
SoulXRecorder* soulx_recorder_new(FILE* out, int w_nat, int h_nat, SoulXRecRotationFn rotation_fn, void* ctx, bool echo);
void soulx_recorder_prime(SoulXRecorder* r, int slots, int raw_x, int raw_y);
int  soulx_recorder_feed(SoulXRecorder* r, const char* line);
int  soulx_recorder_idle(SoulXRecorder* r, double host_now_ms);
int  soulx_recorder_finish(SoulXRecorder* r);
int  soulx_recorder_actions(const SoulXRecorder* r);
void soulx_recorder_free(SoulXRecorder* r);

// input.c (exported there for this file and the tests)
int  soulx_input_text_cost_ms(const char* s, size_t n);
int  soulx_input_display_size(SoulXInput* in, int* w_nat, int* h_nat);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static bool fmt_ok(int n, size_t cap) {
    return n >= 0 && (size_t)n < cap;
}

static bool is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static const char* skip_blanks(const char* p) {
    while (is_blank(*p)) p++;
    return p;
}

static size_t token_len(const char* p) {
    size_t n = 0;
    while (p[n] && !is_blank(p[n])) n++;
    return n;
}

static bool copy_span(char* dst, size_t cap, const char* src, size_t n) {
    if (!dst || cap == 0 || n >= cap) return false;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

static bool contains_nocase(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i]) {
            char a = hay[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (i == n) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// getevent -lt line parser
// ---------------------------------------------------------------------------

// EV_KEY values are words (DOWN / UP / REPEAT), other values 8 hex digits (ffffffff = -1).
// Other labels (e.g. MT_TOOL_FINGER) give 0.
static int parse_value(const char* p, size_t n) {
    if (n == 4 && strncmp(p, "DOWN", 4) == 0) return 1;
    if (n == 2 && strncmp(p, "UP", 2) == 0) return 0;
    if (n == 6 && strncmp(p, "REPEAT", 6) == 0) return 2;
    if (n == 0 || n > 8) return 0;
    unsigned long v = 0;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = (v << 4) | (unsigned long)d;
    }
    return v <= 0x7fffffffUL ? (int)v : (int)((long long)v - 0x100000000LL);
}

int soulx_getevent_parse(const char* line, double* ts_ms, char* dev, size_t dev_cap, char* type, size_t type_cap,
                        char* code, size_t code_cap, int* value) {
    if (ts_ms) *ts_ms = 0;
    if (dev && dev_cap) dev[0] = '\0';
    if (type && type_cap) type[0] = '\0';
    if (code && code_cap) code[0] = '\0';
    if (value) *value = 0;
    if (!line) return SOULX_GE_NONE;

    // add device 12: /dev/input/event1
    if (strncmp(line, "add device ", 11) == 0) {
        const char* p = line + 11;
        if (!is_digit(*p)) return SOULX_GE_NONE;
        while (is_digit(*p)) p++;
        if (*p != ':') return SOULX_GE_NONE;
        p = skip_blanks(p + 1);
        size_t n = token_len(p);
        if (n == 0 || !copy_span(dev, dev_cap, p, n)) return SOULX_GE_NONE;
        return SOULX_GE_DEVICE;
    }

    const char* p = skip_blanks(line);
    //   name:     "virtio_input_multi_touch_1"
    if (strncmp(p, "name:", 5) == 0) {
        p = skip_blanks(p + 5);
        if (*p != '"') return SOULX_GE_NONE;
        const char* end = strrchr(p + 1, '"');
        if (!end || !copy_span(dev, dev_cap, p + 1, (size_t)(end - (p + 1)))) return SOULX_GE_NONE;
        return SOULX_GE_NAME;
    }

    // [     302.729329] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   00000000
    // [    1115.534456] EV_ABS       ABS_MT_SLOT          00000000          (single-device form)
    if (*p != '[') return SOULX_GE_NONE;
    p = skip_blanks(p + 1);
    if (!is_digit(*p)) return SOULX_GE_NONE;
    long long sec = 0, frac_ns = 0;             // integers: "1.030000" must give exactly 1030 ms
    int digits = 0;
    while (is_digit(*p)) {
        if (++digits > 12) return SOULX_GE_NONE;
        sec = sec * 10 + (*p - '0');
        p++;
    }
    if (*p == '.') {
        p++;
        int frac = 0;
        while (is_digit(*p)) {
            if (++frac > 9) return SOULX_GE_NONE;
            frac_ns = frac_ns * 10 + (*p - '0');
            p++;
        }
        for (; frac < 9; frac++) frac_ns *= 10;
    }
    p = skip_blanks(p);
    if (*p != ']') return SOULX_GE_NONE;
    p = skip_blanks(p + 1);

    size_t n = token_len(p);
    if (n == 0) return SOULX_GE_NONE;
    if (p[n - 1] == ':') {                                  // device path
        if (!copy_span(dev, dev_cap, p, n - 1)) return SOULX_GE_NONE;
        p = skip_blanks(p + n);
        n = token_len(p);
    }
    if (n == 0 || !copy_span(type, type_cap, p, n)) return SOULX_GE_NONE;
    p = skip_blanks(p + n);
    n = token_len(p);
    if (n == 0 || !copy_span(code, code_cap, p, n)) return SOULX_GE_NONE;
    p = skip_blanks(p + n);
    n = token_len(p);
    if (n == 0) return SOULX_GE_NONE;
    if (value) *value = parse_value(p, n);
    if (ts_ms) *ts_ms = (double)sec * 1000.0 + (double)frac_ns / 1e6;
    return SOULX_GE_EVENT;
}

// ---------------------------------------------------------------------------
// Recorder state
// ---------------------------------------------------------------------------

typedef struct {
    int  tracking;          // ABS_MT_TRACKING_ID (-1 = no contact)
    int  x, y;              // raw ABS_MT_POSITION_X/Y (-1 = not known yet)
    int  pressure;          // ABS_MT_PRESSURE (-1 = not known yet, counts as touching; 0 = hovering)
    bool active;            // a contact as of the last SYN_REPORT
    int  active_id;         // its tracking id
    int  finger;            // its finger number in the current gesture (-1 = not recorded)
    int  lx, ly;            // its last logical position
} RecSlot;

typedef enum { REC_DOWN, REC_MOVE, REC_UP } RecKind;

typedef struct {
    double  t;              // guest time, ms
    RecKind kind;
    int     finger;
    int     x, y;           // logical pixels
} RecSample;

typedef struct {
    char path[REC_DEV_MAX];
    char name[REC_DEV_MAX];
} RecDevice;

struct SoulXRecorder {
    FILE* out;
    int   w_nat, h_nat;                 // panel in its natural orientation
    SoulXRecRotationFn rotation_fn;
    void* rotation_ctx;
    bool  echo;                         // print every action to stderr
    double host_start;

    RecDevice dev[REC_MAX_DEVICES];
    int   ndev;
    int   last_dev;                     // the device the next "name:" line belongs to
    char  touch[REC_DEV_MAX];           // touchscreen device path ("" = not chosen yet)

    int     slot_cur;                   // current ABS_MT_SLOT (-1 = out of range)
    RecSlot slot[REC_MAX_SLOTS];

    bool  g_open;                       // a gesture (first finger down .. last finger up)
    int   g_rot;                        // rotation at its start
    int   g_contacts;                   // touches in it
    int   g_active;                     // touches currently down
    bool  g_unknown;                    // a touch started at an unknown position
    bool  finger_busy[REC_MAX_FINGERS];
    RecSample* samples;
    size_t nsamples;
    size_t cap_samples;

    bool   shift_l, shift_r;
    bool   ctrl_l, ctrl_r, alt_l, alt_r, meta_l, meta_r;
    bool   caps;                        // Caps Lock is on
    char   text[REC_TEXT_MAX];          // printable keys not written yet
    size_t text_len;
    bool   text_open;
    double text_start, text_last;       // guest ms
    double text_last_host;              // host ms of the last character

    bool   clock_set;
    double clock;                       // recording time the replay has reached (guest ms)
    int    actions;
    bool   failed;                      // output error / out of memory
};

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

static void rec_write(SoulXRecorder* r, const char* line) {
    if (r->failed) return;
    if (fputs(line, r->out) == EOF || fputc('\n', r->out) == EOF || fflush(r->out) != 0) r->failed = true;
}

// sleep() up to time t; the first action starts the clock (the time before it is not replayed).
static void rec_sleep_until(SoulXRecorder* r, double t) {
    if (!r->clock_set) {
        r->clock = t;
        r->clock_set = true;
        return;
    }
    double gap = t - r->clock;
    if (gap < REC_SLEEP_MIN_MS) return;                 // shorter gaps carry over to the next one
    long ms = (long)((gap + 5.0) / 10.0) * 10;
    r->clock += (double)ms;
    while (ms > 0) {
        long part = ms > REC_SLEEP_MAX_MS ? REC_SLEEP_MAX_MS : ms;
        char line[48];
        snprintf(line, sizeof(line), "sleep(%ld)", part);
        rec_write(r, line);
        ms -= part;
    }
}

static void rec_echo(SoulXRecorder* r, const char* what) {
    if (r->echo) soulx_log(SOULX_LOG_INFO, "%6.1fs  %s", (soulx_now_ms() - r->host_start) / 1000.0, what);
}

static void rec_line(SoulXRecorder* r, double t, const char* line) {
    rec_sleep_until(r, t);
    rec_write(r, line);
}

// One action starting at t that takes dur ms to replay.
static void rec_action(SoulXRecorder* r, double t, double dur, const char* line) {
    rec_line(r, t, line);
    r->clock += dur;
    r->actions++;
    rec_echo(r, line);
}

static long round10(double ms) {
    return ms <= 0 ? 0 : (long)((ms + 5.0) / 10.0) * 10;
}

// ---------------------------------------------------------------------------
// Keys and text
// ---------------------------------------------------------------------------

typedef struct {
    const char* code;
    char normal;
    char shifted;
} RecCharKey;

static const RecCharKey k_char_keys[] = {
    { "KEY_SPACE", ' ', ' ' }, { "KEY_MINUS", '-', '_' }, { "KEY_EQUAL", '=', '+' },
    { "KEY_LEFTBRACE", '[', '{' }, { "KEY_RIGHTBRACE", ']', '}' }, { "KEY_SEMICOLON", ';', ':' },
    { "KEY_APOSTROPHE", '\'', '"' }, { "KEY_GRAVE", '`', '~' }, { "KEY_BACKSLASH", '\\', '|' },
    { "KEY_COMMA", ',', '<' }, { "KEY_DOT", '.', '>' }, { "KEY_SLASH", '/', '?' },
    { "KEY_KPASTERISK", '*', '*' }, { "KEY_KPMINUS", '-', '-' }, { "KEY_KPPLUS", '+', '+' },
    { "KEY_KPDOT", '.', '.' }, { "KEY_KPSLASH", '/', '/' }, { "KEY_KPEQUAL", '=', '=' }, { "KEY_KPCOMMA", ',', ',' },
};

// Linux key -> Android key name accepted by key() (soulx_input_keycode)
static const struct {
    const char* code;
    const char* name;
} k_key_names[] = {
    { "KEY_BACK", "BACK" }, { "KEY_HOMEPAGE", "HOME" }, { "KEY_HOME", "MOVE_HOME" }, { "KEY_END", "MOVE_END" },
    { "KEY_APPSELECT", "APP_SWITCH" }, { "KEY_MENU", "MENU" }, { "KEY_ENTER", "ENTER" },
    { "KEY_KPENTER", "NUMPAD_ENTER" }, { "KEY_BACKSPACE", "DEL" }, { "KEY_DELETE", "FORWARD_DEL" },
    { "KEY_TAB", "TAB" }, { "KEY_ESC", "ESCAPE" }, { "KEY_UP", "DPAD_UP" }, { "KEY_DOWN", "DPAD_DOWN" },
    { "KEY_LEFT", "DPAD_LEFT" }, { "KEY_RIGHT", "DPAD_RIGHT" }, { "KEY_SELECT", "DPAD_CENTER" },
    { "KEY_VOLUMEUP", "VOLUME_UP" }, { "KEY_VOLUMEDOWN", "VOLUME_DOWN" }, { "KEY_MUTE", "VOLUME_MUTE" },
    { "KEY_POWER", "POWER" }, { "KEY_SLEEP", "SLEEP" }, { "KEY_WAKEUP", "WAKEUP" }, { "KEY_CAMERA", "CAMERA" },
    { "KEY_SEARCH", "SEARCH" }, { "KEY_PAGEUP", "PAGE_UP" }, { "KEY_PAGEDOWN", "PAGE_DOWN" },
    { "KEY_INSERT", "INSERT" }, { "KEY_PLAYPAUSE", "MEDIA_PLAY_PAUSE" }, { "KEY_PLAYCD", "MEDIA_PLAY" },
    { "KEY_PAUSECD", "MEDIA_PAUSE" }, { "KEY_STOPCD", "MEDIA_STOP" }, { "KEY_NEXTSONG", "MEDIA_NEXT" },
    { "KEY_PREVIOUSSONG", "MEDIA_PREVIOUS" }, { "KEY_REWIND", "MEDIA_REWIND" },
    { "KEY_FASTFORWARD", "MEDIA_FAST_FORWARD" }, { "KEY_BRIGHTNESSDOWN", "BRIGHTNESS_DOWN" },
    { "KEY_BRIGHTNESSUP", "BRIGHTNESS_UP" }, { "KEY_ASSISTANT", "ASSIST" }, { "KEY_VOICECOMMAND", "VOICE_ASSIST" },
    { "KEY_CUT", "CUT" }, { "KEY_COPY", "COPY" }, { "KEY_PASTE", "PASTE" }, { "KEY_SYSRQ", "SYSRQ" },
    { "KEY_FORWARD", "FORWARD" }, { "KEY_REFRESH", "REFRESH" }, { "KEY_HELP", "HELP" },
    { "KEY_CALC", "CALCULATOR" }, { "KEY_ALL_APPLICATIONS", "ALL_APPS" },
};

// The character a key types (0 when it types none); letters are capitals with Shift or Caps Lock
// (not both).
static char rec_key_char(const char* code, bool shift, bool caps) {
    const char* k = code + 4;                           // after "KEY_"
    if (k[0] >= 'A' && k[0] <= 'Z' && k[1] == '\0') return (shift != caps) ? k[0] : (char)(k[0] - 'A' + 'a');
    if (is_digit(k[0]) && k[1] == '\0') return shift ? ")!@#$%^&*("[k[0] - '0'] : k[0];
    if (k[0] == 'K' && k[1] == 'P' && is_digit(k[2]) && k[3] == '\0') return k[2];     // keypad digits
    for (size_t i = 0; i < sizeof(k_char_keys) / sizeof(k_char_keys[0]); i++) {
        if (strcmp(code, k_char_keys[i].code) == 0) return shift ? k_char_keys[i].shifted : k_char_keys[i].normal;
    }
    return 0;
}

static const char* rec_key_name(const char* code) {
    const char* k = code + 4;
    if (k[0] == 'F' && is_digit(k[1]) && (k[2] == '\0' || (is_digit(k[2]) && k[3] == '\0'))) {
        int n = k[2] ? (k[1] - '0') * 10 + (k[2] - '0') : k[1] - '0';
        if (n >= 1 && n <= 12) return k;                // "F1".."F12"
    }
    for (size_t i = 0; i < sizeof(k_key_names) / sizeof(k_key_names[0]); i++) {
        if (strcmp(code, k_key_names[i].code) == 0) return k_key_names[i].name;
    }
    return NULL;
}

static void rec_text_flush(SoulXRecorder* r) {
    if (!r->text_open) return;
    r->text_open = false;
    if (r->text_len == 0) return;
    char line[REC_ACTION_MAX];
    size_t n = 0;
    memcpy(line, "text(\"", 6);
    n = 6;
    for (size_t i = 0; i < r->text_len; i++) {
        char c = r->text[i];
        if (c == '"' || c == '\\') line[n++] = '\\';
        line[n++] = c;
    }
    memcpy(line + n, "\")", 3);
    double cost = soulx_input_text_cost_ms(r->text, r->text_len);
    r->text_len = 0;
    rec_action(r, r->text_start, cost, line);
}

static void rec_text_add(SoulXRecorder* r, double t, char c) {
    if (r->text_open && (t - r->text_last > REC_TEXT_GAP_MS || r->text_len + 1 >= sizeof(r->text))) {
        rec_text_flush(r);
    }
    if (!r->text_open) {
        r->text_open = true;
        r->text_start = t;
        r->text_len = 0;
    }
    r->text[r->text_len++] = c;
    r->text_last = t;
    r->text_last_host = soulx_now_ms();
}

// The held-state flag of a modifier key, NULL for any other key.
static bool* rec_modifier(SoulXRecorder* r, const char* code) {
    static const char* const names[] = { "KEY_LEFTSHIFT", "KEY_RIGHTSHIFT", "KEY_LEFTCTRL", "KEY_RIGHTCTRL",
                                         "KEY_LEFTALT", "KEY_RIGHTALT", "KEY_LEFTMETA", "KEY_RIGHTMETA" };
    bool* flags[] = { &r->shift_l, &r->shift_r, &r->ctrl_l, &r->ctrl_r, &r->alt_l, &r->alt_r, &r->meta_l,
                      &r->meta_r };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(code, names[i]) == 0) return flags[i];
    }
    return NULL;
}

// A key pressed while Ctrl, Alt or Meta is held: key() cannot replay a combination, so it is written
// as a comment (and never typed as a letter).
static void rec_chord(SoulXRecorder* r, double t, const char* code) {
    rec_text_flush(r);
    char line[128];
    snprintf(line, sizeof(line), "-- %s%s%s%.40s pressed here (key combinations are not recorded)",
             (r->ctrl_l || r->ctrl_r) ? "Ctrl+" : "", (r->alt_l || r->alt_r) ? "Alt+" : "",
             (r->meta_l || r->meta_r) ? "Meta+" : "", code + 4);
    rec_line(r, t, line);
    rec_echo(r, line);
}

static void rec_key(SoulXRecorder* r, double t, const char* code, int value) {
    if (value == 2) return;                                         // auto-repeat
    bool* mod = rec_modifier(r, code);
    if (mod) {
        *mod = value != 0;
        return;
    }
    if (strcmp(code, "KEY_CAPSLOCK") == 0) {
        if (value == 1) r->caps = !r->caps;
        return;
    }
    if (value != 1) return;                                         // key up

    if (r->ctrl_l || r->ctrl_r || r->alt_l || r->alt_r || r->meta_l || r->meta_r) {
        rec_chord(r, t, code);
        return;
    }
    char c = rec_key_char(code, r->shift_l || r->shift_r, r->caps);
    if (c) {
        rec_text_add(r, t, c);
        return;
    }
    if (strcmp(code, "KEY_BACKSPACE") == 0 && r->text_open && r->text_len > 0 && t - r->text_last <= REC_TEXT_GAP_MS) {
        r->text_len--;                                              // a typo fixed while typing
        r->text_last = t;
        r->text_last_host = soulx_now_ms();
        return;
    }
    const char* name = rec_key_name(code);
    if (!name) {
        soulx_log(SOULX_LOG_DEBUG, "record: ignoring %s", code);
        return;
    }
    rec_text_flush(r);
    char line[64];
    snprintf(line, sizeof(line), "key(\"%s\")", name);
    rec_action(r, t, REC_KEY_COST_MS, line);
}

// ---------------------------------------------------------------------------
// Touch gestures
// ---------------------------------------------------------------------------

// Raw touchscreen value -> natural pixel. The guest sees position raw * dim / 32768; SoulX aims at pixel
// centres (input.c), the console's `event mouse` (and the emulator's own window) at pixel edges scaled
// by 32767, just below the edge. A quarter-pixel bias - downwards on an axis the rotation mirrors -
// gives the pixel either kind of touch was aimed at, so a replay touches the recorded pixel again.
static int raw_to_pixel(int raw, int dim, bool mirrored) {
    raw = clampi(raw, 0, REC_RAW_MAX);
    long long num = (long long)raw * dim + (mirrored ? -(REC_RAW_SCALE / 4) : REC_RAW_SCALE / 4);
    int px = num < 0 ? 0 : (int)(num / REC_RAW_SCALE);
    return clampi(px, 0, dim - 1);
}

// Natural pixel -> logical pixel for the gesture's rotation (inverse of input.c's soulx_map_logical_pixel).
static void rec_to_logical(const SoulXRecorder* r, int raw_x, int raw_y, int* lx, int* ly) {
    const int w = r->w_nat, h = r->h_nat, rot = r->g_rot;
    int nx = raw_to_pixel(raw_x, w, rot == 1 || rot == 2);
    int ny = raw_to_pixel(raw_y, h, rot == 2 || rot == 3);
    switch (rot) {
    case 1:  *lx = ny;         *ly = w - 1 - nx; break;
    case 2:  *lx = w - 1 - nx; *ly = h - 1 - ny; break;
    case 3:  *lx = h - 1 - ny; *ly = nx;         break;
    default: *lx = nx;         *ly = ny;         break;
    }
}

static void rec_sample(SoulXRecorder* r, double t, RecKind kind, int finger, int x, int y) {
    if (r->nsamples == r->cap_samples) {
        if (kind == REC_MOVE && r->nsamples >= REC_MAX_SAMPLES) return;     // endless gesture: keep downs/ups
        size_t ncap = r->cap_samples ? r->cap_samples * 2 : 256;
        RecSample* grown = realloc(r->samples, ncap * sizeof(*grown));
        if (!grown) {
            if (kind != REC_MOVE) {
                soulx_log(SOULX_LOG_ERROR, "record: out of memory");
                r->failed = true;
            }
            return;
        }
        r->samples = grown;
        r->cap_samples = ncap;
    }
    RecSample* s = &r->samples[r->nsamples++];
    s->t = t;
    s->kind = kind;
    s->finger = finger;
    s->x = x;
    s->y = y;
}

static void rec_gesture_begin(SoulXRecorder* r) {
    rec_text_flush(r);                              // text typed before the touch comes first
    int rot = r->rotation_fn ? r->rotation_fn(r->rotation_ctx) : 0;
    r->g_rot = (rot >= 0 && rot <= 3) ? rot : 0;
    r->g_open = true;
    r->g_contacts = 0;
    r->g_active = 0;
    r->g_unknown = false;
    r->nsamples = 0;
    memset(r->finger_busy, 0, sizeof(r->finger_busy));
}

// touch_down / touch_move / touch_up lines with the original timing: gestures with two or more
// fingers, and one-finger gestures too long for swipe() / long_press().
static void rec_emit_touches(SoulXRecorder* r) {
    double written_t[REC_MAX_FINGERS];
    int wx[REC_MAX_FINGERS], wy[REC_MAX_FINGERS];          // last written position
    bool pending[REC_MAX_FINGERS];
    double pt[REC_MAX_FINGERS];
    int px[REC_MAX_FINGERS], py[REC_MAX_FINGERS];          // newest position not written yet
    memset(pending, 0, sizeof(pending));
    for (int f = 0; f < REC_MAX_FINGERS; f++) {
        written_t[f] = pt[f] = 0;
        wx[f] = wy[f] = px[f] = py[f] = 0;
    }
    int touches = 0;
    char line[96];
    for (size_t i = 0; i < r->nsamples; i++) {
        const RecSample* s = &r->samples[i];
        int f = s->finger;
        if (f < 0 || f >= REC_MAX_FINGERS) continue;
        switch (s->kind) {
        case REC_DOWN:
            snprintf(line, sizeof(line), "touch_down(%d, %d, %d)", s->x, s->y, f);
            rec_line(r, s->t, line);
            written_t[f] = s->t;
            wx[f] = s->x;
            wy[f] = s->y;
            pending[f] = false;
            touches++;
            break;
        case REC_MOVE:
            pending[f] = true;
            pt[f] = s->t;
            px[f] = s->x;
            py[f] = s->y;
            if (s->t - written_t[f] >= REC_SAMPLE_MS) {
                if (px[f] != wx[f] || py[f] != wy[f]) {
                    snprintf(line, sizeof(line), "touch_move(%d, %d, %d)", px[f], py[f], f);
                    rec_line(r, s->t, line);
                    written_t[f] = s->t;
                    wx[f] = px[f];
                    wy[f] = py[f];
                }
                pending[f] = false;
            }
            break;
        case REC_UP:
            if (pending[f] && (px[f] != wx[f] || py[f] != wy[f])) {     // end exactly where the finger left
                snprintf(line, sizeof(line), "touch_move(%d, %d, %d)", px[f], py[f], f);
                rec_line(r, pt[f], line);
            }
            pending[f] = false;
            snprintf(line, sizeof(line), "touch_up(%d)", f);
            rec_line(r, s->t, line);
            break;
        }
    }
    r->actions++;
    snprintf(line, sizeof(line), "touch gesture (%d finger%s, touch_* lines)", touches, touches == 1 ? "" : "s");
    rec_echo(r, line);
}

// One finger: tap / long_press / swipe.
static void rec_emit_single(SoulXRecorder* r) {
    const RecSample* first = &r->samples[0];
    const RecSample* last = &r->samples[r->nsamples - 1];
    long long max_d2 = 0;
    for (size_t i = 0; i < r->nsamples; i++) {
        long long dx = r->samples[i].x - first->x;
        long long dy = r->samples[i].y - first->y;
        if (dx * dx + dy * dy > max_d2) max_d2 = dx * dx + dy * dy;
    }
    double dur = last->t - first->t;
    if (dur < 0) dur = 0;
    if (dur > REC_GESTURE_MAX_MS) {
        rec_emit_touches(r);
        return;
    }
    char line[128];
    double cost;                                        // what the replay of the line takes
    if (max_d2 < (long long)REC_STILL_PX * REC_STILL_PX) {
        if (dur < REC_TAP_MS) {
            snprintf(line, sizeof(line), "tap(%d, %d)", first->x, first->y);
            cost = REC_TAP_COST_MS;
        } else {
            long ms = round10(dur);
            snprintf(line, sizeof(line), "long_press(%d, %d, %ld)", first->x, first->y, ms);
            cost = (double)ms + REC_GESTURE_EXTRA_MS;
        }
    } else {
        long ms = round10(dur);
        if (ms < 10) ms = 10;
        snprintf(line, sizeof(line), "swipe(%d, %d, %d, %d, %ld)", first->x, first->y, last->x, last->y, ms);
        cost = (double)ms + REC_GESTURE_EXTRA_MS;
    }
    rec_action(r, first->t, cost, line);
}

static void rec_gesture_end(SoulXRecorder* r) {
    r->g_open = false;
    if (r->nsamples == 0) return;
    if (r->g_unknown) {
        rec_write(r, "-- (a touch here was not recorded: the touchscreen did not report its position)");
        soulx_log(SOULX_LOG_WARN, "a touch was not recorded: the touchscreen did not report its position");
    } else if (r->g_contacts == 1) {
        rec_emit_single(r);
    } else {
        rec_emit_touches(r);
    }
    r->nsamples = 0;
}

static void rec_contact_move(SoulXRecorder* r, RecSlot* s, double t) {
    if (s->finger < 0 || s->x < 0 || s->y < 0) return;
    int lx, ly;
    rec_to_logical(r, s->x, s->y, &lx, &ly);
    if (lx == s->lx && ly == s->ly) return;
    s->lx = lx;
    s->ly = ly;
    rec_sample(r, t, REC_MOVE, s->finger, lx, ly);
}

static void rec_contact_start(SoulXRecorder* r, RecSlot* s, double t) {
    if (!r->g_open) rec_gesture_begin(r);
    s->active = true;
    s->active_id = s->tracking;
    r->g_active++;
    r->g_contacts++;
    s->finger = -1;
    for (int f = 0; f < REC_MAX_FINGERS; f++) {
        if (!r->finger_busy[f]) {
            s->finger = f;
            break;
        }
    }
    if (s->finger < 0) return;                      // an eleventh finger is not recorded
    r->finger_busy[s->finger] = true;
    if (s->x < 0 || s->y < 0) {
        r->g_unknown = true;
        s->lx = 0;
        s->ly = 0;
    } else {
        rec_to_logical(r, s->x, s->y, &s->lx, &s->ly);
    }
    rec_sample(r, t, REC_DOWN, s->finger, s->lx, s->ly);
}

static void rec_contact_end(SoulXRecorder* r, RecSlot* s, double t, bool lifted) {
    s->active = false;
    if (r->g_active > 0) r->g_active--;
    if (s->finger < 0) return;
    if (lifted) rec_contact_move(r, s, t);          // a release frame may carry a last position
    rec_sample(r, t, REC_UP, s->finger, s->lx, s->ly);
    r->finger_busy[s->finger] = false;
    s->finger = -1;
}

// SYN_REPORT of the touchscreen: compares every slot with the previous frame.
static void rec_frame(SoulXRecorder* r, double t) {
    for (int i = 0; i < REC_MAX_SLOTS; i++) {
        RecSlot* s = &r->slot[i];
        bool touching = s->tracking >= 0 && s->pressure != 0;
        if (s->active && !touching) rec_contact_end(r, s, t, true);
        else if (s->active && s->tracking != s->active_id) rec_contact_end(r, s, t, false);   // replaced
        if (touching && !s->active) rec_contact_start(r, s, t);
        else if (touching) rec_contact_move(r, s, t);
    }
    if (r->g_open && r->g_active == 0) rec_gesture_end(r);
}

// ---------------------------------------------------------------------------
// Devices and events
// ---------------------------------------------------------------------------

static void rec_device_add(SoulXRecorder* r, const char* path) {
    for (int i = 0; i < r->ndev; i++) {
        if (strcmp(r->dev[i].path, path) == 0) {
            r->dev[i].name[0] = '\0';
            r->last_dev = i;
            return;
        }
    }
    if (r->ndev >= REC_MAX_DEVICES) {
        r->last_dev = -1;
        return;
    }
    RecDevice* d = &r->dev[r->ndev];
    if (!copy_span(d->path, sizeof(d->path), path, strlen(path))) {
        r->last_dev = -1;
        return;
    }
    d->name[0] = '\0';
    r->last_dev = r->ndev++;
}

static void rec_device_name(SoulXRecorder* r, const char* name) {
    if (r->last_dev < 0) return;
    RecDevice* d = &r->dev[r->last_dev];
    if (!copy_span(d->name, sizeof(d->name), name, strlen(name))) return;
    // the touchscreen of display 0 (multi_touch_2.. belong to secondary displays)
    if (strcmp(name, "virtio_input_multi_touch_1") == 0) memcpy(r->touch, d->path, sizeof(r->touch));
}

// Multi-touch events count only from the touchscreen. Without the emulator's own name it is
// the first device named "...touch..." (or, when none is, any device) that sends them.
static bool rec_is_touch(SoulXRecorder* r, const char* dev) {
    if (!dev[0]) return true;                       // single-device form
    if (r->touch[0]) return strcmp(dev, r->touch) == 0;
    bool any_named = false;
    int idx = -1;
    for (int i = 0; i < r->ndev; i++) {
        if (contains_nocase(r->dev[i].name, "touch")) any_named = true;
        if (strcmp(r->dev[i].path, dev) == 0) idx = i;
    }
    bool ok = idx >= 0 ? (!any_named || contains_nocase(r->dev[idx].name, "touch")) : !any_named;
    if (ok && copy_span(r->touch, sizeof(r->touch), dev, strlen(dev))) {
        soulx_log(SOULX_LOG_DEBUG, "record: touchscreen %s (%s)", dev, idx >= 0 ? r->dev[idx].name : "?");
    }
    return ok;
}

static void rec_event(SoulXRecorder* r, double t, const char* dev, const char* type, const char* code, int value) {
    if (strcmp(type, "EV_ABS") == 0 && strncmp(code, "ABS_MT_", 7) == 0) {
        if (!rec_is_touch(r, dev)) return;
        if (strcmp(code, "ABS_MT_SLOT") == 0) {
            r->slot_cur = (value >= 0 && value < REC_MAX_SLOTS) ? value : -1;
            return;
        }
        if (r->slot_cur < 0) return;
        RecSlot* s = &r->slot[r->slot_cur];
        if (strcmp(code, "ABS_MT_TRACKING_ID") == 0) s->tracking = value < 0 ? -1 : value;
        else if (strcmp(code, "ABS_MT_POSITION_X") == 0) s->x = value < 0 ? 0 : value;
        else if (strcmp(code, "ABS_MT_POSITION_Y") == 0) s->y = value < 0 ? 0 : value;
        else if (strcmp(code, "ABS_MT_PRESSURE") == 0) s->pressure = value;
    } else if (strcmp(type, "EV_SYN") == 0) {
        if (strcmp(code, "SYN_REPORT") == 0 && (!dev[0] || (r->touch[0] && strcmp(dev, r->touch) == 0))) {
            rec_frame(r, t);
        }
    } else if (strcmp(type, "EV_KEY") == 0 && strncmp(code, "KEY_", 4) == 0) {
        rec_key(r, t, code, value);                 // BTN_* (touch / stylus buttons) are not keys
    }
}

// ---------------------------------------------------------------------------
// Recorder API
// ---------------------------------------------------------------------------

SoulXRecorder* soulx_recorder_new(FILE* out, int w_nat, int h_nat, SoulXRecRotationFn rotation_fn, void* ctx, bool echo) {
    if (!out || w_nat <= 0 || h_nat <= 0 || w_nat > 16384 || h_nat > 16384) return NULL;
    SoulXRecorder* r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->out = out;
    r->w_nat = w_nat;
    r->h_nat = h_nat;
    r->rotation_fn = rotation_fn;
    r->rotation_ctx = ctx;
    r->echo = echo;
    r->host_start = soulx_now_ms();
    r->last_dev = -1;
    for (int i = 0; i < REC_MAX_SLOTS; i++) {
        r->slot[i].tracking = -1;
        r->slot[i].x = -1;
        r->slot[i].y = -1;
        r->slot[i].pressure = -1;
        r->slot[i].finger = -1;
    }
    return r;
}

// The first |slots| slots were set to (raw_x, raw_y) through the console and slot 0 selected.
void soulx_recorder_prime(SoulXRecorder* r, int slots, int raw_x, int raw_y) {
    if (!r) return;
    for (int i = 0; i < slots && i < REC_MAX_SLOTS; i++) {
        r->slot[i].x = raw_x;
        r->slot[i].y = raw_y;
    }
    r->slot_cur = 0;
}

int soulx_recorder_feed(SoulXRecorder* r, const char* line) {
    if (!r || r->failed) return -1;
    double ts = 0;
    char dev[REC_DEV_MAX], type[32], code[48];
    int value = 0;
    switch (soulx_getevent_parse(line, &ts, dev, sizeof(dev), type, sizeof(type), code, sizeof(code), &value)) {
    case SOULX_GE_DEVICE:
        rec_device_add(r, dev);
        break;
    case SOULX_GE_NAME:
        rec_device_name(r, dev);
        break;
    case SOULX_GE_EVENT:
        rec_event(r, ts, dev, type, code, value);
        break;
    default:
        break;
    }
    return r->failed ? -1 : 0;
}

// Writes the text being typed once nothing was typed for 1.5 s (host clock).
int soulx_recorder_idle(SoulXRecorder* r, double host_now_ms) {
    if (!r) return -1;
    if (r->text_open && host_now_ms - r->text_last_host > REC_TEXT_GAP_MS) rec_text_flush(r);
    return r->failed ? -1 : 0;
}

int soulx_recorder_finish(SoulXRecorder* r) {
    if (!r) return -1;
    rec_text_flush(r);
    if (r->g_open) {
        soulx_log(SOULX_LOG_INFO, "the touch still going on at the end was not recorded");
        r->g_open = false;
        r->nsamples = 0;
    }
    return r->failed ? -1 : 0;
}

int soulx_recorder_actions(const SoulXRecorder* r) {
    return r ? r->actions : 0;
}

void soulx_recorder_free(SoulXRecorder* r) {
    if (!r) return;
    free(r->samples);
    free(r);
}

// ---------------------------------------------------------------------------
// adb getevent child (posix_spawn, SoulX environment)
// ---------------------------------------------------------------------------

typedef struct {
    pid_t pid;
    int   out_fd;
    int   err_fd;
} RecChild;

static void close_fd(int* fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

// Pipe with close-on-exec on both ends, never using descriptors 0..2. 0 or an errno value.
static int rec_pipe(int fds[2]) {
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

// Starts `adb -s <serial> shell getevent -lt`; stdout and stderr come back through pipes.
// The child stays in our process group, so a Ctrl+C in the terminal also ends it.
static int rec_spawn(const char* serial, RecChild* ch) {
    ch->pid = -1;
    ch->out_fd = -1;
    ch->err_fd = -1;

    char adb[PATH_MAX];
    if (soulx_tool_path("adb", adb, sizeof(adb)) != 0 || access(adb, X_OK) != 0) {
        soulx_log(SOULX_LOG_ERROR, "SoulX's adb is not installed " EM_DASH " run: soulx setup");
        return -1;
    }
    const char* argv[] = { adb, "-s", serial, "shell", "getevent", "-lt", NULL };

    int out_p[2] = { -1, -1 };
    int err_p[2] = { -1, -1 };
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    bool fa_ok = false, attr_ok = false;
    char** env = NULL;
    pid_t pid = -1;
    int rc = 0;

    if ((rc = rec_pipe(out_p)) != 0 || (rc = rec_pipe(err_p)) != 0) goto done;
    if ((rc = posix_spawn_file_actions_init(&fa)) != 0) goto done;
    fa_ok = true;
    if ((rc = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0)) != 0 ||
        (rc = posix_spawn_file_actions_adddup2(&fa, out_p[1], STDOUT_FILENO)) != 0 ||
        (rc = posix_spawn_file_actions_adddup2(&fa, err_p[1], STDERR_FILENO)) != 0) {
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
    close_fd(&out_p[1]);
    close_fd(&err_p[1]);
    if (rc != 0) {
        close_fd(&out_p[0]);
        close_fd(&err_p[0]);
        soulx_log(SOULX_LOG_ERROR, "cannot run %s: %s", adb, strerror(rc));
        return -1;
    }
    ch->pid = pid;
    ch->out_fd = out_p[0];
    ch->err_fd = err_p[0];
    for (int i = 0; i < 2; i++) {
        int fd = i == 0 ? ch->out_fd : ch->err_fd;
        int fl = fcntl(fd, F_GETFL);
        if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    return 0;
}

// Ends the child (SIGTERM, then SIGKILL) and reaps it. Exit status, 128 + signal, or -1.
static int rec_stop_child(RecChild* ch) {
    if (ch->pid <= 0) return -1;
    int status = 0;
    pid_t w;
    while ((w = waitpid(ch->pid, &status, WNOHANG)) < 0 && errno == EINTR) {}
    if (w == 0) {
        kill(ch->pid, SIGTERM);
        double deadline = soulx_now_ms() + REC_STOP_WAIT_MS;
        for (;;) {
            w = waitpid(ch->pid, &status, WNOHANG);
            if (w != 0 && !(w < 0 && errno == EINTR)) break;
            if (soulx_now_ms() >= deadline) {
                kill(ch->pid, SIGKILL);
                while ((w = waitpid(ch->pid, &status, 0)) < 0 && errno == EINTR) {}
                break;
            }
            struct timespec ts = { 0, 10 * 1000000L };      // not soulx_sleep_ms: we may be interrupted
            nanosleep(&ts, NULL);
        }
    }
    ch->pid = -1;
    if (w <= 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

// Reads what is available. 1 = data, 0 = nothing right now, -1 = end of stream / error.
static int rec_read(int fd, char* buf, size_t cap, size_t* got) {
    *got = 0;
    for (;;) {
        ssize_t r = read(fd, buf, cap);
        if (r > 0) {
            *got = (size_t)r;
            return 1;
        }
        if (r == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

typedef struct {
    char   buf[REC_LINE_MAX];
    size_t len;
    bool   skip;                // the current line is too long: dropped
    size_t lines;               // complete lines seen
} RecLines;

static int rec_feed_data(SoulXRecorder* rec, RecLines* lb, const char* data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n') {
            if (!lb->skip) {
                lb->buf[lb->len] = '\0';
                lb->lines++;
                if (soulx_recorder_feed(rec, lb->buf) != 0) return -1;
            }
            lb->len = 0;
            lb->skip = false;
        } else if (!lb->skip) {
            if (lb->len + 1 < sizeof(lb->buf)) lb->buf[lb->len++] = c;
            else lb->skip = true;
        }
    }
    return 0;
}

static void rec_collect_err(int* fd, char* err, size_t cap, size_t* len) {
    if (*fd < 0) return;
    char chunk[512];
    size_t got = 0;
    int st;
    while ((st = rec_read(*fd, chunk, sizeof(chunk), &got)) > 0) {
        size_t room = cap - 1 - *len;
        size_t n = got < room ? got : room;
        memcpy(err + *len, chunk, n);
        *len += n;
        err[*len] = '\0';
    }
    if (st < 0) close_fd(fd);
}

// ---------------------------------------------------------------------------
// soulx_record
// ---------------------------------------------------------------------------

static int rec_rotation(void* ctx) {
    return soulx_input_rotation((SoulXInput*)ctx);
}

// Sets the position of every unused touch slot to a value no click produces, so the kernel
// reports the coordinates of the next touch in each slot even if they did not change since
// before the recording (it only reports changed values). Slot 0 is selected again at the end.
static bool rec_prime(SoulXInput* in) {
    char cmd[1024];
    int n = snprintf(cmd, sizeof(cmd), "event send");
    if (!fmt_ok(n, sizeof(cmd))) return false;
    size_t len = (size_t)n;
    for (int s = 0; s < REC_PRIME_SLOTS; s++) {
        n = snprintf(cmd + len, sizeof(cmd) - len,
                     " EV_ABS:ABS_MT_SLOT:%d EV_ABS:ABS_MT_POSITION_X:%d EV_ABS:ABS_MT_POSITION_Y:%d",
                     s, REC_PRIME_RAW, REC_PRIME_RAW);
        if (!fmt_ok(n, sizeof(cmd) - len)) return false;
        len += (size_t)n;
    }
    n = snprintf(cmd + len, sizeof(cmd) - len, " EV_ABS:ABS_MT_SLOT:0 EV_SYN:0:0");
    if (!fmt_ok(n, sizeof(cmd) - len)) return false;
    char reply[256];
    int rc = soulx_input_console(in, cmd, reply, sizeof(reply));
    if (rc != 0) {
        soulx_log(SOULX_LOG_DEBUG, "record: could not prime the touch slots (%s)", rc == 1 ? reply : "console error");
    }
    return rc == 0;
}

// Control characters would end the Lua comment line; quotes the path for the shell when needed.
static void rec_shell_word(const char* in, char* out, size_t cap) {
    bool plain = in[0] != '\0';
    for (const char* p = in; *p; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '/' || c == '.' || c == '_' || c == '-' || c == '+' || c == ',' || c == ':' || c == '@';
        if (!ok) plain = false;
    }
    size_t n = 0;
    if (!plain && n + 1 < cap) out[n++] = '\'';
    for (const char* p = in; *p && n + 2 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        out[n++] = (c < 0x20 || c == 0x7f || (!plain && c == '\'')) ? '?' : (char)c;
    }
    if (!plain && n + 1 < cap) out[n++] = '\'';
    out[n] = '\0';
}

static int rec_header(FILE* out, const char* name, int lw, int lh, int rot, int w_nat, int h_nat,
                      const char* path) {
    char when[64] = "";
    time_t now = time(NULL);
    struct tm lt;
    if (now != (time_t)-1 && localtime_r(&now, &lt)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &lt);
    char file[PATH_MAX + 8];
    rec_shell_word(path, file, sizeof(file));
    fprintf(out,
            "-- SoulX macro recorded with `soulx record` from instance '%s' on %s\n"
            "-- Screen: %dx%d logical pixels (rotation %d degrees; panel %dx%d)\n"
            "-- Replay: soulx run %s   (on this instance: soulx run -i %s %s)\n"
            "-- Coordinates are logical screen pixels, the same as in screenshots.\n"
            "\n",
            name, when, lw, lh, rot * 90, w_nat, h_nat, file, name, file);
    return (fflush(out) == 0 && !ferror(out)) ? 0 : -1;
}

// Creates "<path>.rec-<pid>-<n>.tmp" next to path (same file system, so rename() can replace path
// atomically at the end; 0644 minus the umask, like the output itself). Returns the descriptor or -1.
static int rec_open_temp(const char* path, char* tmp, size_t cap) {
    static unsigned counter;
    for (int i = 0; i < REC_TMP_TRIES; i++) {
        int n = snprintf(tmp, cap, "%s.rec-%d-%u.tmp", path, (int)getpid(), counter++);
        if (!fmt_ok(n, cap)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST;
    return -1;
}

int soulx_record(const char* instance, const char* out_path) {
    if (!out_path || !out_path[0]) {
        soulx_log(SOULX_LOG_ERROR, "record: no output file given");
        return 1;
    }
    SoulXInstanceConf conf;
    SoulXRunState st;
    char serial[64];
    if (soulx_mac_require_running(instance, &conf, &st, serial, sizeof(serial)) != 0) return 1;
    if (!serial[0]) return 1;

    SoulXInput* in = soulx_input_open(conf.name);
    if (!in) {
        soulx_log(SOULX_LOG_ERROR, "cannot connect to the emulator console of '%s'", conf.name);
        return 1;
    }
    int lw = 0, lh = 0, w_nat = 0, h_nat = 0;
    (void)soulx_input_screen_size(in, &lw, &lh);
    int rot = soulx_input_rotation(in);
    if (rot < 0) rot = 0;
    // the display the guest really has (the registry may describe the next start already)
    if (soulx_input_display_size(in, &w_nat, &h_nat) != 0 || w_nat <= 0 || h_nat <= 0) {
        w_nat = conf.width;
        h_nat = conf.height;
    }

    // An existing file (soulx record --force) is replaced only by a recording that worked.
    struct stat old_st;
    bool replacing = stat(out_path, &old_st) == 0;
    char tmp_path[PATH_MAX];
    int fd = rec_open_temp(out_path, tmp_path, sizeof(tmp_path));
    FILE* out = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!out) {
        soulx_log(SOULX_LOG_ERROR, "cannot write %s: %s", out_path, strerror(errno));
        if (fd >= 0) {
            close(fd);
            unlink(tmp_path);
        }
        soulx_input_close(in);
        return 1;
    }
    if (rec_header(out, conf.name, lw, lh, rot, w_nat, h_nat, out_path) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot write %s: %s", out_path, strerror(errno));
        fclose(out);
        unlink(tmp_path);
        soulx_input_close(in);
        return 1;
    }

    bool primed = rec_prime(in);
    RecChild ch;
    SoulXRecorder* rec = NULL;
    if (rec_spawn(serial, &ch) != 0 || !(rec = soulx_recorder_new(out, w_nat, h_nat, rec_rotation, in, true))) {
        if (ch.pid > 0) {
            (void)rec_stop_child(&ch);
            close_fd(&ch.out_fd);
            close_fd(&ch.err_fd);
            soulx_log(SOULX_LOG_ERROR, "record: out of memory");
        }
        fclose(out);
        unlink(tmp_path);
        soulx_input_close(in);
        return 1;
    }
    if (primed) soulx_recorder_prime(rec, REC_PRIME_SLOTS, REC_PRIME_RAW, REC_PRIME_RAW);

    soulx_log(SOULX_LOG_INFO, "Recording '%s' (%dx%d) to %s " EM_DASH " press Ctrl+C to stop", conf.name, lw, lh,
             out_path);

    RecLines lines;
    memset(&lines, 0, sizeof(lines));
    char chunk[8192];
    char err[REC_ERR_MAX];
    size_t err_len = 0;
    err[0] = '\0';
    bool eof = false;
    bool write_failed = false;
    double started = soulx_now_ms();

    while (!eof && !g_soulx_interrupted) {
        struct pollfd pfd[2];
        nfds_t np = 0;
        pfd[np].fd = ch.out_fd;
        pfd[np].events = POLLIN;
        pfd[np].revents = 0;
        np++;
        if (ch.err_fd >= 0) {
            pfd[np].fd = ch.err_fd;
            pfd[np].events = POLLIN;
            pfd[np].revents = 0;
            np++;
        }
        int pr = poll(pfd, np, REC_POLL_MS);
        if (pr < 0 && errno != EINTR) {
            soulx_log(SOULX_LOG_ERROR, "record: poll failed: %s", strerror(errno));
            eof = true;
            break;
        }
        if (pr > 0 && pfd[0].revents) {
            size_t got = 0;
            int r = rec_read(ch.out_fd, chunk, sizeof(chunk), &got);
            if (r > 0 && rec_feed_data(rec, &lines, chunk, got) != 0) {
                write_failed = true;
                break;
            }
            if (r < 0) eof = true;
        }
        if (pr > 0 && np > 1 && pfd[1].revents) rec_collect_err(&ch.err_fd, err, sizeof(err), &err_len);
        if (soulx_recorder_idle(rec, soulx_now_ms()) != 0) {
            write_failed = true;
            break;
        }
    }

    // Ctrl+C: what the device sent before is still in the pipe (adb itself ends on the same Ctrl+C)
    if (!eof && !write_failed) {
        double until = soulx_now_ms() + REC_DRAIN_MS;
        while (soulx_now_ms() < until) {
            struct pollfd p = { ch.out_fd, POLLIN, 0 };
            int pr = poll(&p, 1, 20);
            if (pr < 0 && errno != EINTR) break;
            if (pr <= 0) continue;
            size_t got = 0;
            int r = rec_read(ch.out_fd, chunk, sizeof(chunk), &got);
            if (r > 0 && rec_feed_data(rec, &lines, chunk, got) != 0) {
                write_failed = true;
                break;
            }
            if (r < 0) break;
        }
    }
    int status = rec_stop_child(&ch);
    rec_collect_err(&ch.err_fd, err, sizeof(err), &err_len);
    close_fd(&ch.out_fd);
    close_fd(&ch.err_fd);

    bool interrupted = g_soulx_interrupted != 0;
    if (soulx_recorder_finish(rec) != 0) write_failed = true;
    int actions = soulx_recorder_actions(rec);
    double secs = (soulx_now_ms() - started) / 1000.0;
    soulx_recorder_free(rec);
    if (!write_failed) {
        fprintf(out, "\n-- end of recording: %d action%s in %.1f s\n", actions, actions == 1 ? "" : "s", secs);
    }
    if (fflush(out) != 0 || ferror(out)) write_failed = true;
    if (fclose(out) != 0) write_failed = true;
    soulx_input_close(in);

    // first line of adb's error output, for the messages below
    size_t el = strcspn(err, "\r\n");
    err[el] = '\0';

    if (write_failed) {
        soulx_log(SOULX_LOG_ERROR, "cannot write the recording (disk full?); what was recorded is in %s%s%s", tmp_path,
                 replacing ? " and " : "", replacing ? "the old file was kept" : "");
        return 1;
    }
    if (!interrupted && lines.lines == 0) {
        soulx_log(SOULX_LOG_ERROR, "adb getevent failed on '%s' (exit status %d)%s%s", conf.name, status,
                 err[0] ? ": " : "", err);
        unlink(tmp_path);
        return 1;
    }
    if (actions == 0 && replacing) {
        unlink(tmp_path);
        soulx_log(SOULX_LOG_WARN, "nothing was recorded; %s was left as it was", out_path);
    } else if (rename(tmp_path, out_path) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot save the recording as %s: %s (it is in %s)", out_path, strerror(errno),
                 tmp_path);
        return 1;
    } else {
        soulx_log(SOULX_LOG_INFO, "Saved %d action%s (%.1fs) to %s", actions, actions == 1 ? "" : "s", secs, out_path);
    }
    if (!interrupted) {
        soulx_log(SOULX_LOG_ERROR, "the input event stream of '%s' ended unexpectedly (adb exit status %d)%s%s",
                 conf.name, status, err[0] ? ": " : "", err);
        return 1;
    }
    return 0;
}
