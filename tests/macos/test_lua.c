/*
 * SoulX Android Emulator - unit tests for source/script/soulx_lua.c
 *
 * Runs real Lua scripts through soulx_script_run() against a fake device installed with
 * soulx_lua_set_device_ops(): a synthetic 1080x2400 screen with a "button", and a log of every
 * action the script sends. Needs no emulator. SOULX_HOME points to a fresh temporary directory.
 *
 * Covered: the input API (tap, swipe, long_press, touch_down/move/up, key, text) and its checks,
 * coordinates outside the screen, args/varargs, error reporting (script error + traceback,
 * syntax errors, non-string errors) and exit codes (os.exit, Ctrl+C = 130), the screen cache
 * (100 ms reuse, dropped after input, capture() always fresh), template matching with files
 * relative to the script folder (find/find_all/exists/tap(image), option validation, regions,
 * reload of a changed file), wait()/wait_gone() pacing and timeouts, Image methods
 * (size/color/crop/save/find/find_all/tostring/<close>), screenshot(), console/shell/launch/
 * stop_app/install/current_app/use with fake replies, Ctrl+C (count hook, sleep, wait, pcall and
 * coroutines cannot swallow it) and touch release, disabled host commands, require() next to the
 * script, log/print output, the real device layer refusing an unknown instance, and that dropped
 * captures are freed promptly (GC accounting of the pixel memory).
 */

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "image.h"
#include "soulx_mac.h"

// ---------------------------------------------------------------------------
// Test hook of soulx_lua.c (mirror of its SoulXLuaDeviceOps: keep both in sync)
// ---------------------------------------------------------------------------

typedef struct {
    void* (*open)(const char* instance, char* name, size_t name_cap, char* err, size_t err_cap);
    void  (*close)(void* dev);
    int   (*screen_size)(void* dev, int* w, int* h);
    int   (*capture)(void* dev, SoulXImage* img);
    int   (*touch)(void* dev, int finger, int action, int x, int y);
    int   (*tap)(void* dev, int x, int y);
    int   (*long_press)(void* dev, int x, int y, int ms);
    int   (*swipe)(void* dev, int x1, int y1, int x2, int y2, int ms);
    int   (*key)(void* dev, const char* name_or_code);
    int   (*text)(void* dev, const char* text);
    int   (*console)(void* dev, const char* cmd, char* reply, size_t cap);
    int   (*shell)(void* dev, const char* cmd, int timeout_ms, char** out, size_t* out_len, int* status,
                   char* err, size_t err_cap);
    int   (*launch)(void* dev, const char* package);
    int   (*stop_app)(void* dev, const char* package);
    int   (*install)(void* dev, const char* path, bool reinstall, char* package, size_t cap);
    int   (*current_app)(void* dev, char* package, size_t cap);
} SoulXLuaDeviceOps;

extern int soulx_lua_set_device_ops(const SoulXLuaDeviceOps* ops, size_t ops_size);

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

static int g_failures = 0;
static int g_checks = 0;
static char g_dir[PATH_MAX];            // scripts, templates and outputs

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// Fake device
// ---------------------------------------------------------------------------

#define FAKE_W      1080
#define FAKE_H      2400
#define BTN_X       440                 // button rectangle; centre = (540, 1650)
#define BTN_Y       1610
#define BTN_W       200
#define BTN_H       80
#define LOG_MAX     256

typedef struct {
    char name[40];
} FakeDev;

static struct {
    FakeDev devs[16];
    int ndevs;
    int opens;
    int closes;
    SoulXImage screen;                   // background (noise)
    SoulXImage button;                   // pasted at BTN_X, BTN_Y while visible
    bool button_visible;
    int button_after;                   // > 0: the button appears with this capture number
    bool tap_hides_button;
    int captures;
    char log[LOG_MAX][96];
    int nlog;
    char foreground[64];
    bool sample_memory;
    size_t peak_in_use;                 // malloc bytes in use, sampled at every capture
    int scene;                          // SCENE_*: a tiny simulated game for the example scripts
    int missed_ok_taps;                 // SCENE_ROUNDS: taps on "ok" the game ignores (a missed tap)
} g_fake;

// ---------------------------------------------------------------------------
// Simulated game for the example scripts: overlays (img/<name>.png) that appear and disappear
// when the script taps them
// ---------------------------------------------------------------------------

enum { SCENE_NONE = 0, SCENE_ROUNDS, SCENE_DAILY };

typedef struct {
    const char* name;
    int x, y, w, h;
    bool visible;
    int show_at;                        // > 0: becomes visible with this capture number
    SoulXImage img;
} Overlay;

static Overlay g_overlays[] = {
    { "start", 440, 1610, 200, 80, false, 0, { 0, 0, NULL } },        // image_loop.lua
    { "ok", 440, 2000, 200, 80, false, 0, { 0, 0, NULL } },
    { "title", 340, 800, 400, 120, false, 0, { 0, 0, NULL } },        // daily_routine.lua
    { "close", 900, 300, 80, 80, false, 0, { 0, 0, NULL } },
    { "home", 0, 2200, 1080, 100, false, 0, { 0, 0, NULL } },
    { "battle", 440, 1610, 200, 80, false, 0, { 0, 0, NULL } },
    { "auto", 900, 150, 120, 60, false, 0, { 0, 0, NULL } },
    { "victory", 240, 900, 600, 150, false, 0, { 0, 0, NULL } },
    { "collect", 440, 1900, 200, 80, false, 0, { 0, 0, NULL } },
};
#define OVERLAYS ((int)(sizeof(g_overlays) / sizeof(g_overlays[0])))

static Overlay* overlay(const char* name) {
    for (int i = 0; i < OVERLAYS; i++) {
        if (strcmp(g_overlays[i].name, name) == 0) return &g_overlays[i];
    }
    fprintf(stderr, "test_lua: no overlay '%s'\n", name);
    exit(2);
}

static void scene_set(int scene) {
    g_fake.scene = scene;
    for (int i = 0; i < OVERLAYS; i++) {
        g_overlays[i].visible = false;
        g_overlays[i].show_at = 0;
    }
}

// Shows an overlay now (after = 0) or with the after-th capture from now.
static void scene_show(const char* name, int after) {
    Overlay* o = overlay(name);
    if (after <= 0) {
        o->visible = true;
        o->show_at = 0;
    } else {
        o->show_at = g_fake.captures + after;
    }
}

static void scene_hide(const char* name) {
    Overlay* o = overlay(name);
    o->visible = false;
    o->show_at = 0;
}

static bool scene_hit(const char* name, int x, int y) {
    const Overlay* o = overlay(name);
    return o->visible && x >= o->x && x < o->x + o->w && y >= o->y && y < o->y + o->h;
}

// What the game does when the script taps (x, y).
static void scene_tap(int x, int y) {
    if (g_fake.scene == SCENE_ROUNDS) {
        if (scene_hit("start", x, y)) {
            scene_hide("start");
            scene_show("ok", 2);                        // the round takes two captures
        } else if (scene_hit("ok", x, y)) {
            if (g_fake.missed_ok_taps > 0) {
                g_fake.missed_ok_taps--;
                return;
            }
            scene_hide("ok");
            scene_show("start", 1);
        }
    } else if (g_fake.scene == SCENE_DAILY) {
        if (overlay("title")->visible) {                // "tap anywhere to start"
            scene_hide("title");
            scene_show("home", 0);
            scene_show("battle", 0);
            scene_show("close", 0);                     // a daily pop-up
        } else if (scene_hit("close", x, y)) {
            scene_hide("close");
        } else if (scene_hit("battle", x, y)) {
            scene_hide("home");
            scene_hide("battle");
            scene_show("auto", 0);
            scene_show("victory", 3);                   // the battle takes three captures
            scene_show("collect", 3);
        } else if (scene_hit("auto", x, y)) {
            scene_hide("auto");
        } else if (scene_hit("collect", x, y)) {
            scene_hide("victory");
            scene_hide("collect");
            scene_show("home", 0);
            scene_show("battle", 0);
            scene_show("close", 1);                     // a reward pop-up
        }
    }
}

static void scene_draw(SoulXImage* img) {
    for (int i = 0; i < OVERLAYS; i++) {
        Overlay* o = &g_overlays[i];
        if (o->show_at > 0 && g_fake.captures >= o->show_at) {
            o->visible = true;
            o->show_at = 0;
        }
        if (!o->visible || !o->img.rgba) continue;
        for (int y = 0; y < o->h; y++) {
            memcpy(img->rgba + ((size_t)(o->y + y) * FAKE_W + (size_t)o->x) * 4,
                   o->img.rgba + (size_t)y * (size_t)o->w * 4, (size_t)o->w * 4);
        }
    }
}

static void fake_reset(void) {
    g_fake.ndevs = 0;
    g_fake.opens = 0;
    g_fake.closes = 0;
    g_fake.button_visible = true;
    g_fake.button_after = 0;
    g_fake.tap_hides_button = false;
    g_fake.captures = 0;
    g_fake.nlog = 0;
    g_fake.foreground[0] = '\0';
    g_fake.sample_memory = false;
    g_fake.peak_in_use = 0;
    g_fake.missed_ok_taps = 0;
    scene_set(SCENE_NONE);
}

static void fake_log(void* dev, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

static void fake_log(void* dev, const char* fmt, ...) {
    if (g_fake.nlog >= LOG_MAX) return;
    char action[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(action, sizeof(action), fmt, ap);
    va_end(ap);
    snprintf(g_fake.log[g_fake.nlog++], sizeof(g_fake.log[0]), "%s: %s", ((FakeDev*)dev)->name, action);
}

static bool log_has(const char* line) {
    for (int i = 0; i < g_fake.nlog; i++) {
        if (strcmp(g_fake.log[i], line) == 0) return true;
    }
    return false;
}

static void* fake_open(const char* instance, char* name, size_t name_cap, char* err, size_t err_cap) {
    const char* n = (instance && *instance) ? instance : "default";
    if (strcmp(n, "missing") == 0 || g_fake.ndevs >= 16) {
        snprintf(err, err_cap, "no such instance '%s' (see: soulx list)", n);
        return NULL;
    }
    FakeDev* d = &g_fake.devs[g_fake.ndevs++];
    snprintf(d->name, sizeof(d->name), "%s", n);
    snprintf(name, name_cap, "%s", n);
    g_fake.opens++;
    return d;
}

static void fake_close(void* dev) {
    (void)dev;
    g_fake.closes++;
}

static int fake_screen_size(void* dev, int* w, int* h) {
    (void)dev;
    *w = FAKE_W;
    *h = FAKE_H;
    return 0;
}

static size_t malloc_in_use(void) {
    malloc_statistics_t st;
    memset(&st, 0, sizeof(st));
    malloc_zone_statistics(NULL, &st);
    return st.size_in_use;
}

static int fake_capture(void* dev, SoulXImage* img) {
    (void)dev;
    g_fake.captures++;
    if (g_fake.button_after > 0 && g_fake.captures >= g_fake.button_after) g_fake.button_visible = true;
    size_t bytes = (size_t)FAKE_W * FAKE_H * 4;
    img->rgba = malloc(bytes);
    if (!img->rgba) return -1;
    memcpy(img->rgba, g_fake.screen.rgba, bytes);
    img->w = FAKE_W;
    img->h = FAKE_H;
    if (g_fake.scene != SCENE_NONE) {
        scene_draw(img);
    } else if (g_fake.button_visible) {
        for (int y = 0; y < BTN_H; y++) {
            memcpy(img->rgba + ((size_t)(BTN_Y + y) * FAKE_W + BTN_X) * 4,
                   g_fake.button.rgba + (size_t)y * BTN_W * 4, (size_t)BTN_W * 4);
        }
    }
    if (g_fake.sample_memory) {
        size_t in_use = malloc_in_use();
        if (in_use > g_fake.peak_in_use) g_fake.peak_in_use = in_use;
    }
    return 0;
}

static int fake_touch(void* dev, int finger, int action, int x, int y) {
    const char* a = action == SOULX_TOUCH_DOWN ? "down" : action == SOULX_TOUCH_UP ? "up" : "move";
    fake_log(dev, "touch %d %s %d %d", finger, a, x, y);
    return 0;
}

static int fake_tap(void* dev, int x, int y) {
    fake_log(dev, "tap %d %d", x, y);
    if (g_fake.tap_hides_button && x >= BTN_X && x < BTN_X + BTN_W && y >= BTN_Y && y < BTN_Y + BTN_H) {
        g_fake.button_visible = false;
    }
    scene_tap(x, y);
    return 0;
}

static int fake_long_press(void* dev, int x, int y, int ms) {
    fake_log(dev, "long_press %d %d %d", x, y, ms);
    return 0;
}

static int fake_swipe(void* dev, int x1, int y1, int x2, int y2, int ms) {
    fake_log(dev, "swipe %d %d %d %d %d", x1, y1, x2, y2, ms);
    return 0;
}

static int fake_key(void* dev, const char* name_or_code) {
    fake_log(dev, "key %s", name_or_code);
    return strcmp(name_or_code, "NO_SUCH_KEY") == 0 ? -1 : 0;
}

static int fake_text(void* dev, const char* text) {
    fake_log(dev, "text %s", text);
    return 0;
}

static int fake_console(void* dev, const char* cmd, char* reply, size_t cap) {
    fake_log(dev, "console %s", cmd);
    if (strcmp(cmd, "ping") == 0) {
        snprintf(reply, cap, "pong");
        return 0;
    }
    if (strcmp(cmd, "bad") == 0) {
        snprintf(reply, cap, "unknown command");
        return 1;
    }
    return -1;
}

static int fake_shell(void* dev, const char* cmd, int timeout_ms, char** out, size_t* out_len, int* status,
                      char* err, size_t err_cap) {
    fake_log(dev, "shell %s (%d ms)", cmd, timeout_ms);
    const char* text = NULL;
    int st = 0;
    if (strcmp(cmd, "echo hi") == 0) {
        text = "hi\n";
    } else if (strcmp(cmd, "exit 3") == 0) {
        text = "";
        st = 3;
    } else if (strcmp(cmd, "sleep 99") == 0) {
        return -2;
    } else if (strcmp(cmd, "offline") == 0) {
        snprintf(err, err_cap, "error: device offline");
        return -1;
    } else {
        text = "line1\r\nline2\r\n\n";
    }
    *out = strdup(text);
    if (!*out) return -1;
    *out_len = strlen(text);
    *status = st;
    return 0;
}

static int fake_launch(void* dev, const char* package) {
    fake_log(dev, "launch %s", package);
    if (strcmp(package, "com.example.game") != 0) return -1;
    snprintf(g_fake.foreground, sizeof(g_fake.foreground), "%s", package);
    if (g_fake.scene == SCENE_DAILY) scene_show("title", 0);
    return 0;
}

static int fake_stop_app(void* dev, const char* package) {
    fake_log(dev, "stop_app %s", package);
    if (strcmp(g_fake.foreground, package) == 0) g_fake.foreground[0] = '\0';
    if (g_fake.scene != SCENE_NONE) {
        for (int i = 0; i < OVERLAYS; i++) {
            g_overlays[i].visible = false;
            g_overlays[i].show_at = 0;
        }
    }
    return 0;
}

static int fake_install(void* dev, const char* path, bool reinstall, char* package, size_t cap) {
    const char* base = strrchr(path, '/');
    fake_log(dev, "install %s%s", base ? base + 1 : path, reinstall ? " (reinstall)" : "");
    size_t n = strlen(path);
    if (n < 4 || strcmp(path + n - 4, ".apk") != 0) return -1;
    snprintf(package, cap, "com.example.game");
    return 0;
}

static int fake_current_app(void* dev, char* package, size_t cap) {
    (void)dev;
    if (!g_fake.foreground[0]) return -1;
    snprintf(package, cap, "%s", g_fake.foreground);
    return 0;
}

static const SoulXLuaDeviceOps k_fake_ops = {
    .open = fake_open,
    .close = fake_close,
    .screen_size = fake_screen_size,
    .capture = fake_capture,
    .touch = fake_touch,
    .tap = fake_tap,
    .long_press = fake_long_press,
    .swipe = fake_swipe,
    .key = fake_key,
    .text = fake_text,
    .console = fake_console,
    .shell = fake_shell,
    .launch = fake_launch,
    .stop_app = fake_stop_app,
    .install = fake_install,
    .current_app = fake_current_app,
};

// ---------------------------------------------------------------------------
// Synthetic images
// ---------------------------------------------------------------------------

static uint32_t g_rng = 0x2545F491u;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void set_px(SoulXImage* img, int x, int y, int r, int g, int b) {
    uint8_t* p = img->rgba + ((size_t)y * (size_t)img->w + (size_t)x) * 4;
    p[0] = (uint8_t)r;
    p[1] = (uint8_t)g;
    p[2] = (uint8_t)b;
    p[3] = 255;
}

static int32_t px_rgb(const SoulXImage* img, int x, int y) {
    const uint8_t* p = img->rgba + ((size_t)y * (size_t)img->w + (size_t)x) * 4;
    return (int32_t)(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]);
}

// Background: 6x6 blocks of random colours plus a little per-pixel noise (structure at every
// scale, so that any crop of it is unique). Button: red with a white pattern.
static int make_images(void) {
    if (soulx_image_alloc(&g_fake.screen, FAKE_W, FAKE_H) != 0) return -1;
    if (soulx_image_alloc(&g_fake.button, BTN_W, BTN_H) != 0) return -1;
    for (int by = 0; by < FAKE_H; by += 6) {
        for (int bx = 0; bx < FAKE_W; bx += 6) {
            uint32_t c = rnd();
            for (int y = by; y < by + 6 && y < FAKE_H; y++) {
                for (int x = bx; x < bx + 6 && x < FAKE_W; x++) {
                    int n = (int)(rnd() % 9) - 4;
                    int r = (int)(c & 0xFF) + n, g = (int)((c >> 8) & 0xFF) + n, b = (int)((c >> 16) & 0xFF) + n;
                    set_px(&g_fake.screen, x, y, r < 0 ? 0 : r > 255 ? 255 : r, g < 0 ? 0 : g > 255 ? 255 : g,
                           b < 0 ? 0 : b > 255 ? 255 : b);
                }
            }
        }
    }
    for (int y = 0; y < BTN_H; y++) {
        for (int x = 0; x < BTN_W; x++) {
            bool white = ((y / 10) % 2 == 0 && (x / 25) % 2 == 1) || (x > 80 && x < 120 && y > 30 && y < 50);
            if (white) {
                set_px(&g_fake.button, x, y, 250, 250, 250);
            } else {
                set_px(&g_fake.button, x, y, 220, 40 + (x % 7), 40);
            }
        }
    }
    return 0;
}

// One distinct striped pattern per overlay of the simulated game, saved as img/<name>.png.
static int make_overlays(const char* dir) {
    for (int i = 0; i < OVERLAYS; i++) {
        Overlay* o = &g_overlays[i];
        if (soulx_image_alloc(&o->img, o->w, o->h) != 0) return -1;
        int period = 6 + 3 * i;
        for (int y = 0; y < o->h; y++) {
            for (int x = 0; x < o->w; x++) {
                bool stripe = ((x + y * (i % 3 + 1)) / period) % 2 == 0;
                int n = (int)(rnd() % 24);
                if (stripe) {
                    set_px(&o->img, x, y, 30 + 25 * i + n, 200 - 15 * i + n, 90 + n);
                } else {
                    set_px(&o->img, x, y, 230 - 20 * i + n, 40 + 20 * i + n, 200 - 10 * i + n);
                }
            }
        }
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/img/%s.png", dir, o->name);
        if (soulx_image_save_png(path, &o->img) != 0) return -1;
    }
    return 0;
}

// A green/blue checkerboard that never appears on the fake screen.
static int make_other_template(const char* path) {
    SoulXImage img;
    if (soulx_image_alloc(&img, 120, 60) != 0) return -1;
    for (int y = 0; y < 60; y++) {
        for (int x = 0; x < 120; x++) {
            if (((x / 8) + (y / 8)) % 2) {
                set_px(&img, x, y, 10, 200, 30);
            } else {
                set_px(&img, x, y, 20, 30, 210);
            }
        }
    }
    int rc = soulx_image_save_png(path, &img);
    free(img.rgba);
    return rc;
}

// ---------------------------------------------------------------------------
// Running scripts
// ---------------------------------------------------------------------------

typedef struct {
    int code;
    double ms;
    char out[32768];
    char err[32768];
} Run;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void path_in_dir(char* buf, size_t cap, const char* name) {
    snprintf(buf, cap, "%s/%s", g_dir, name);
}

static int write_text(const char* name, const char* text) {
    char path[PATH_MAX];
    path_in_dir(path, sizeof(path), name);
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    fputs(text, f);
    return fclose(f);
}

static void read_text(const char* path, char* buf, size_t cap) {
    buf[0] = '\0';
    FILE* f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
}

static bool file_exists(const char* name) {
    char path[PATH_MAX];
    struct stat st;
    path_in_dir(path, sizeof(path), name);
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// Writes the script into g_dir and runs it on the fake instance "default" with stdout and
// stderr captured.
static void run_on(Run* r, const char* instance, const char* file, const char* code, int argc, char** argv) {
    char script[PATH_MAX], out_path[PATH_MAX], err_path[PATH_MAX];
    path_in_dir(script, sizeof(script), file);
    path_in_dir(out_path, sizeof(out_path), "stdout.txt");
    path_in_dir(err_path, sizeof(err_path), "stderr.txt");
    if (code && write_text(file, code) != 0) {
        fprintf(stderr, "cannot write %s\n", script);
        exit(2);
    }
    g_fake.ndevs = 0;                   // device handles live for one run
    fflush(stdout);
    fflush(stderr);
    int saved_out = dup(STDOUT_FILENO), saved_err = dup(STDERR_FILENO);
    int fo = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int fe = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (saved_out < 0 || saved_err < 0 || fo < 0 || fe < 0) {
        perror("redirect");
        exit(2);
    }
    dup2(fo, STDOUT_FILENO);
    dup2(fe, STDERR_FILENO);
    close(fo);
    close(fe);

    double t0 = now_ms();
    r->code = soulx_script_run(instance, script, argc, argv);
    r->ms = now_ms() - t0;

    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);
    read_text(out_path, r->out, sizeof(r->out));
    read_text(err_path, r->err, sizeof(r->err));
}

static void run(Run* r, const char* file, const char* code) {
    run_on(r, "default", file, code, 0, NULL);
}

// Like run(), but through soulx_script_run_ex: runs the script `repeat` times (0 = loop) at `speed`.
static void run_ex(Run* r, const char* file, const char* code, int repeat, double speed) {
    char script[PATH_MAX], out_path[PATH_MAX], err_path[PATH_MAX];
    path_in_dir(script, sizeof(script), file);
    path_in_dir(out_path, sizeof(out_path), "stdout.txt");
    path_in_dir(err_path, sizeof(err_path), "stderr.txt");
    if (code && write_text(file, code) != 0) {
        fprintf(stderr, "cannot write %s\n", script);
        exit(2);
    }
    g_fake.ndevs = 0;
    fflush(stdout);
    fflush(stderr);
    int saved_out = dup(STDOUT_FILENO), saved_err = dup(STDERR_FILENO);
    int fo = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int fe = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (saved_out < 0 || saved_err < 0 || fo < 0 || fe < 0) {
        perror("redirect");
        exit(2);
    }
    dup2(fo, STDOUT_FILENO);
    dup2(fe, STDERR_FILENO);
    close(fo);
    close(fe);

    r->code = soulx_script_run_ex("default", script, 0, NULL, repeat, speed);

    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);
    read_text(out_path, r->out, sizeof(r->out));
    read_text(err_path, r->err, sizeof(r->err));
}

// The script must succeed; prints its stderr otherwise.
static void expect_ok(const Run* r, const char* what) {
    CHECK(r->code == 0 && strstr(r->err, "script finished in"), "%s: exit code %d, stderr:\n%s", what, r->code,
          r->err);
}

// The script must fail with exit code 1 and this text in its error message.
static void expect_error(const char* what, const char* code, const char* text) {
    static Run r;
    run(&r, "fails.lua", code);
    CHECK(r.code == 1 && strstr(r.err, "script error: ") && strstr(r.err, text),
          "%s: expected an error containing '%s', got exit code %d, stderr:\n%s", what, text, r.code, r.err);
}

static Run g_run;                       // large: keep it off the stack

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_device_ops_hook(void) {
    SoulXLuaDeviceOps partial = k_fake_ops;
    partial.capture = NULL;
    CHECK(soulx_lua_set_device_ops(&k_fake_ops, sizeof(k_fake_ops) - 1) == -1, "size mismatch accepted");
    CHECK(soulx_lua_set_device_ops(&partial, sizeof(partial)) == -1, "incomplete table accepted");
    CHECK(soulx_lua_set_device_ops(NULL, 0) == 0, "cannot restore the real device");

    // The real device layer: no instance exists in the temporary SOULX_HOME
    fake_reset();
    run_on(&g_run, "nosuch", "real.lua", "print('never')\n", 0, NULL);
    CHECK(g_run.code == 1 && strstr(g_run.err, "no such instance 'nosuch'") && !strstr(g_run.out, "never"),
          "real device: code %d, stderr:\n%s", g_run.code, g_run.err);

    CHECK(soulx_lua_set_device_ops(&k_fake_ops, sizeof(k_fake_ops)) == 0, "cannot install the fake device");
}

static void test_input_api(void) {
    fake_reset();
    run(&g_run, "input.lua",
        "assert(tap(540, 1650) == true)\n"
        "swipe(300, 1500, 800, 1500, 200)\n"
        "swipe(10, 20, 30, 40)\n"
        "long_press(100, 200)\n"
        "long_press(100, 200, 1500)\n"
        "key('BACK')\n"
        "key(66)\n"
        "key('KEYCODE_HOME')\n"
        "text('hello world!')\n"
        "text('a\\tb\\n')\n"
        "touch_down(100, 200)\n"
        "touch_move(150, 250)\n"
        "assert(touch_up() == true)\n"
        "assert(touch_up() == false)\n"
        "touch_down(1, 2, 3)\n"
        "touch_up(3)\n"
        "tap(10.4, 20.6)\n"
        "tap('7', '8')\n"
        "local w, h = screen_size()\n"
        "assert(w == 1080 and h == 2400, 'screen_size')\n"
        "print('done')\n");
    expect_ok(&g_run, "input api");
    static const char* const want[] = {
        "default: tap 540 1650",
        "default: swipe 300 1500 800 1500 200",
        "default: swipe 10 20 30 40 300",
        "default: long_press 100 200 800",
        "default: long_press 100 200 1500",
        "default: key BACK",
        "default: key 66",
        "default: key KEYCODE_HOME",
        "default: text hello world!",
        "default: text a\tb\n",
        "default: touch 0 down 100 200",
        "default: touch 0 move 150 250",
        "default: touch 0 up 150 250",
        "default: touch 3 down 1 2",
        "default: touch 3 up 1 2",
        "default: tap 10 21",
        "default: tap 7 8",
    };
    int n = (int)(sizeof(want) / sizeof(want[0]));
    CHECK(g_fake.nlog == n, "input api: %d actions logged, want %d", g_fake.nlog, n);
    for (int i = 0; i < n && i < g_fake.nlog; i++) {
        CHECK(strcmp(g_fake.log[i], want[i]) == 0, "action %d: got '%s', want '%s'", i, g_fake.log[i], want[i]);
    }
    CHECK(strcmp(g_run.out, "done\n") == 0, "stdout: '%s'", g_run.out);
    CHECK(g_fake.opens == 1 && g_fake.closes == 1, "opens %d closes %d", g_fake.opens, g_fake.closes);
    CHECK(g_fake.captures == 0, "input needs no capture (%d)", g_fake.captures);
}

static void test_input_errors(void) {
    fake_reset();
    expect_error("x outside", "tap(1080, 5)\n", "tap: point (1080, 5) is outside the screen (1080x2400)");
    expect_error("y outside", "\nlong_press(5, -1)\n", "fails.lua:2: long_press: point (5, -1) is outside the screen");
    expect_error("swipe end", "swipe(1, 1, 5, 2400)\n", "swipe: point (5, 2400) is outside the screen (1080x2400)");
    expect_error("swipe args", "swipe(1, 2, 3)\n", "bad argument #4 to 'swipe'");
    expect_error("nan", "tap(0/0, 5)\n", "invalid coordinate");
    expect_error("huge", "tap(1e300, 5)\n", "invalid coordinate");
    expect_error("swipe ms", "swipe(1, 1, 2, 2, 0)\n", "ms must be between 1 and 60000");
    expect_error("key name", "key('BA CK')\n", "a key name uses letters");
    expect_error("key code", "key(12.5)\n", "a key code is a whole number");
    expect_error("key code 0", "key(0)\n", "a key code is a whole number from 1 to 999");
    expect_error("unknown key", "key('NO_SUCH_KEY')\n", "key: unknown key 'NO_SUCH_KEY'");
    expect_error("text control", "text('a\\1b')\n", "control characters cannot be typed");
    expect_error("text non-ascii", "text('caf\\u{E9}')\n", "only ASCII text can be typed");
    expect_error("text empty", "text('')\n", "text is empty");
    expect_error("finger", "touch_down(1, 1, 10)\n", "finger must be between 0 and 9");
    expect_error("move without down", "touch_move(5, 5)\n", "finger 0 is not down");
    expect_error("double down", "touch_down(5, 5)\ntouch_down(6, 6)\n", "finger 0 is already down");
    expect_error("sleep", "sleep(-1)\n", "ms must be between 0 and 86400000");
    expect_error("is_color rgb", "is_color(1, 1, 0x1000000)\n", "colour must be 0xRRGGBB");
    expect_error("package", "launch('not a package')\n", "invalid package name");
    expect_error("console newline", "console('a\\nb')\n", "a console command is a single line");
}

static void test_args_and_output(void) {
    fake_reset();
    char a1[] = "alpha", a2[] = "two words";
    char* argv[] = { a1, a2, NULL };
    run_on(&g_run, "default", "args.lua",
           "local a, b = ...\n"
           "assert(args[0]:match('args%.lua$'), args[0])\n"
           "assert(#args == 2 and args[1] == 'alpha' and args[2] == 'two words')\n"
           "assert(a == 'alpha' and b == 'two words')\n"
           "assert(arg == args)\n"
           "assert(math.type(millis()) == 'integer')\n"
           "print('n', #args)\n"
           "log('hello', 42, true, nil)\n",
           2, argv);
    expect_ok(&g_run, "args");
    CHECK(strcmp(g_run.out, "n\t2\n") == 0, "print output: '%s'", g_run.out);
    CHECK(strstr(g_run.err, "s] hello 42 true nil\n") && strstr(g_run.err, "[0."), "log output:\n%s", g_run.err);
    CHECK(strstr(g_run.err, "running ") && strstr(g_run.err, "args.lua on default"), "start line:\n%s", g_run.err);
}

static void test_errors_and_exit(void) {
    fake_reset();
    run(&g_run, "boom.lua", "local x = 1\n\nerror('boom')\n");
    CHECK(g_run.code == 1 && strstr(g_run.err, "script error: ") && strstr(g_run.err, "boom.lua:3: boom") &&
          strstr(g_run.err, "stack traceback:"), "error: code %d, stderr:\n%s", g_run.code, g_run.err);
    CHECK(g_fake.closes == g_fake.opens, "device not closed after an error");

    run(&g_run, "syntax.lua", "tap(1, 2\n");
    CHECK(g_run.code == 1 && strstr(g_run.err, "script error: ") && strstr(g_run.err, "syntax.lua:"),
          "syntax error: code %d, stderr:\n%s", g_run.code, g_run.err);

    expect_error("table error", "error({})\n", "(error object is a table value)");
    expect_error("no such script dir item", "dofile('/nonexistent/x.lua')\n", "cannot open /nonexistent/x.lua");

    run(&g_run, "exit3.lua", "os.exit(3)\nprint('not reached')\n");
    CHECK(g_run.code == 3 && strstr(g_run.err, "script exited with code 3") && !strstr(g_run.out, "not reached"),
          "os.exit(3): code %d, stderr:\n%s", g_run.code, g_run.err);
    run(&g_run, "exit0.lua", "os.exit()\n");
    expect_ok(&g_run, "os.exit()");
    run(&g_run, "exitf.lua", "os.exit(false)\n");
    CHECK(g_run.code == 1, "os.exit(false): code %d", g_run.code);
    run(&g_run, "exitp.lua", "pcall(os.exit, 5)\nprint('not reached')\n");
    CHECK(g_run.code == 5 && !strstr(g_run.out, "not reached"), "pcall(os.exit): code %d, out '%s'", g_run.code,
          g_run.out);
    run(&g_run, "exitc.lua",
        "local co = coroutine.create(function() os.exit(7) end)\n"
        "coroutine.resume(co)\n"
        "print('not reached')\n");
    CHECK(g_run.code == 7 && !strstr(g_run.out, "not reached"), "os.exit in a coroutine: code %d", g_run.code);

    expect_error("os.execute", "os.execute('ls')\n", "os.execute is not available");
    expect_error("io.popen", "io.popen('ls')\n", "io.popen is not available");
    run(&g_run, "shellq.lua", "assert(os.execute() == false)\n");
    expect_ok(&g_run, "os.execute()");

    // No native code: package.loadlib, package.cpath and the C module searchers are removed, so a
    // script cannot load a .so into the soulx process.
    run(&g_run, "noloadlib.lua",
        "assert(package.loadlib == nil, 'loadlib')\n"
        "assert(package.cpath == '', 'cpath')\n"
        "assert(package.searchers[3] == nil, 'C searcher')\n"
        "assert(package.searchers[4] == nil, 'C root searcher')\n"
        "assert(type(package.searchers[1]) == 'function' and type(package.searchers[2]) == 'function')\n");
    expect_ok(&g_run, "native loaders removed");

    // a missing script and an unknown instance
    char missing[PATH_MAX];
    path_in_dir(missing, sizeof(missing), "missing.lua");
    unlink(missing);
    run_on(&g_run, "default", "missing.lua", NULL, 0, NULL);
    CHECK(g_run.code == 1 && strstr(g_run.err, "cannot open"), "missing script: %d\n%s", g_run.code, g_run.err);
    run_on(&g_run, "missing", "inst.lua", "print('x')\n", 0, NULL);
    CHECK(g_run.code == 1 && strstr(g_run.err, "no such instance 'missing'"), "unknown instance: %d\n%s",
          g_run.code, g_run.err);
}

static void test_screen_cache(void) {
    fake_reset();
    run(&g_run, "cache1.lua", "color(1, 1)\ncolor(2, 2)\nassert(is_color(3, 3, color(3, 3), 0))\n");
    expect_ok(&g_run, "cache1");
    CHECK(g_fake.captures == 1, "three reads within 100 ms: %d captures", g_fake.captures);

    fake_reset();
    run(&g_run, "cache2.lua", "color(1, 1)\nsleep(150)\ncolor(1, 1)\n");
    expect_ok(&g_run, "cache2");
    CHECK(g_fake.captures == 2, "read after 150 ms: %d captures", g_fake.captures);

    fake_reset();
    run(&g_run, "cache3.lua", "color(1, 1)\ntap(5, 5)\ncolor(1, 1)\nkey('BACK')\nexists('img/button.png')\n");
    expect_ok(&g_run, "cache3");
    CHECK(g_fake.captures == 3, "reads after input: %d captures", g_fake.captures);

    fake_reset();
    run(&g_run, "cache4.lua", "capture()\ncapture()\ncolor(1, 1)\n");
    expect_ok(&g_run, "cache4");
    CHECK(g_fake.captures == 2, "capture() always captures, color reuses it: %d captures", g_fake.captures);

    // (a small region keeps each search far below 100 ms, also in unoptimised builds)
    fake_reset();
    run(&g_run, "cache5.lua",
        "local near = {region = {420, 1590, 240, 120}}\n"
        "assert(find('img/button.png', near))\n"
        "assert(exists('img/button.png', near))\n"
        "assert(tap('img/button.png', near))\n");
    expect_ok(&g_run, "cache5");
    CHECK(g_fake.captures == 1 && log_has("default: tap 540 1650"), "find+exists+tap: %d captures",
          g_fake.captures);
}

static void test_matching(void) {
    fake_reset();
    char abs_tmpl[PATH_MAX], center[32], corner[32];
    path_in_dir(abs_tmpl, sizeof(abs_tmpl), "img/button.png");
    snprintf(center, sizeof(center), "%d", (int)px_rgb(&g_fake.button, BTN_W / 2, BTN_H / 2));
    snprintf(corner, sizeof(corner), "%d", (int)px_rgb(&g_fake.screen, 0, 0));
    char* argv[] = { abs_tmpl, center, corner, NULL };
    run_on(&g_run, "default", "match.lua",
           "local abs, center, corner = args[1], tonumber(args[2]), tonumber(args[3])\n"
           "local x, y, s = find('img/button.png')\n"
           "assert(x == 540 and y == 1650, ('find %s %s'):format(x, y))\n"
           "assert(s > 0.95, s)\n"
           "assert(color(540, 1650) == center and color(0, 0) == corner)\n"
           "assert(is_color(0, 0, corner) and not is_color(540, 1650, 0x000000))\n"
           "assert(is_color(540, 1650, 0xF0F0F0, 10) and not is_color(540, 1650, 0xF0F0F0, 9))\n"
           "assert(exists('img/button.png') and not exists('img/other.png'))\n"
           "assert(find('img/other.png') == nil)\n"
           "local all = find_all('img/button.png')\n"
           "assert(#all == 1 and all[1].x == 540 and all[1].y == 1650 and all[1].score > 0.95)\n"
           "assert(#find_all('img/other.png') == 0)\n"
           "assert(find('img/button.png', {region = {400, 1500, 400, 300}}))\n"
           "assert(find('img/button.png', {region = {x = 400, y = 1500, w = 400, h = 300}}))\n"
           "assert(not find('img/button.png', {region = {0, 0, 500, 500}}))\n"
           "assert(find('img/button.png', {threshold = 0.99, color_tolerance = 0}))\n"
           "assert(find('./img/../img/button.png'))\n"
           "assert(find(abs))\n"
           "assert(tap('img/button.png') == true)\n"
           "assert(tap('img/other.png', {threshold = 0.95}) == false)\n"
           "local img = capture()\n"
           "assert(find(img:crop(100, 100, 120, 60)) == 160)\n",
           3, argv);
    expect_ok(&g_run, "matching");
    CHECK(log_has("default: tap 540 1650") && g_fake.nlog == 1, "tap(image) taps the centre once (%d actions)",
          g_fake.nlog);

    // a template bigger than the search area is reported once
    fake_reset();
    run(&g_run, "big.lua",
        "local img = capture()\n"
        "local big = img:crop(0, 0, 600, 600)\n"
        "assert(find(big, {region = {0, 0, 500, 500}}) == nil)\n"
        "assert(find(big, {region = {0, 0, 500, 500}}) == nil)\n");
    expect_ok(&g_run, "big template");
    const char* w1 = strstr(g_run.err, "is larger than the search region (500x500)");
    CHECK(w1 && !strstr(w1 + 1, "is larger than"), "warning once:\n%s", g_run.err);

    expect_error("unknown option", "find('img/button.png', {treshold = 0.9})\n", "unknown option 'treshold'");
    expect_error("threshold range", "find('img/button.png', {threshold = 2})\n", "threshold must be a number");
    expect_error("threshold type", "find('img/button.png', {threshold = 'high'})\n", "threshold must be a number");
    expect_error("tolerance", "find('img/button.png', {color_tolerance = 300})\n", "color_tolerance must be");
    expect_error("region size", "find('img/button.png', {region = {0, 0, 0, 10}})\n", "region width and height");
    expect_error("region type", "find('img/button.png', {region = 'all'})\n", "region must be a table");
    expect_error("region short", "find('img/button.png', {region = {1, 2, 3}})\n", "four numbers");
    expect_error("opts type", "find('img/button.png', 5)\n", "options table expected");
    expect_error("opts key", "find('img/button.png', {0.9})\n", "option names must be strings");
    expect_error("missing file", "find('img/missing.png')\n", "find: cannot open image 'img/missing.png'");
    expect_error("not an image", "tap('notes.txt')\n", "not a valid PNG/JPEG image");
    expect_error("image type", "find(42)\n", "image file name or Image expected");
    expect_error("folder", "find('img')\n", "is not a file");
}

static void test_template_reload(void) {
    fake_reset();
    run(&g_run, "reload.lua",
        "local img = capture()\n"
        "img:crop(100, 100, 120, 60):save('img/dyn.png')\n"
        "local x, y = find('img/dyn.png')\n"
        "assert(x == 160 and y == 130, ('first %s %s'):format(x, y))\n"
        "img:crop(700, 300, 120, 60):save('img/dyn.png')\n"
        "x, y = find('img/dyn.png')\n"
        "assert(x == 760 and y == 330, ('reloaded %s %s'):format(x, y))\n");
    expect_ok(&g_run, "template reload");
}

static void test_wait(void) {
    fake_reset();
    g_fake.button_visible = false;
    g_fake.button_after = 3;
    run(&g_run, "wait1.lua",
        "local x, y = wait('img/button.png', 5)\n"
        "assert(x == 540 and y == 1650)\n");
    expect_ok(&g_run, "wait appears");
    CHECK(g_fake.captures == 3, "wait: %d captures, want 3", g_fake.captures);
    CHECK(g_run.ms >= 390.0, "wait: rounds of >= 200 ms (took %.0f ms)", g_run.ms);

    fake_reset();
    run(&g_run, "wait2.lua",
        "local t = millis()\n"
        "assert(wait('img/other.png', 0.5) == nil)\n"
        "local dt = millis() - t\n"
        "assert(dt >= 490 and dt < 1500, dt)\n");
    expect_ok(&g_run, "wait timeout");
    CHECK(g_fake.captures >= 3 && g_fake.captures <= 5, "wait timeout: %d captures", g_fake.captures);

    fake_reset();
    run(&g_run, "wait3.lua", "assert(wait('img/other.png', 0) == nil)\n");
    expect_ok(&g_run, "wait 0");
    CHECK(g_fake.captures == 1, "wait(0): %d captures", g_fake.captures);

    fake_reset();
    g_fake.tap_hides_button = true;
    run(&g_run, "wait4.lua",
        "assert(wait_gone('img/button.png', 0.3) == false)\n"
        "assert(tap('img/button.png'))\n"
        "assert(wait_gone('img/button.png', 2) == true)\n"
        "assert(not tap('img/button.png'))\n");
    expect_ok(&g_run, "wait_gone");

    expect_error("wait timeout arg", "wait('img/button.png', -1)\n", "timeout must be between 0 and 86400");
}


static void test_image_objects(void) {
    fake_reset();
    char center[32];
    snprintf(center, sizeof(center), "%d", (int)px_rgb(&g_fake.button, BTN_W / 2, BTN_H / 2));
    char* argv[] = { center, NULL };
    run_on(&g_run, "default", "image.lua",
           "local center = tonumber(args[1])\n"
           "local img = capture()\n"
           "local w, h = img:size()\n"
           "assert(w == 1080 and h == 2400)\n"
           "assert(tostring(img) == 'Image(1080x2400)')\n"
           "assert(img:color(540, 1650) == center)\n"
           "local part = img:crop(440, 1610, 200, 80)\n"
           "local pw, ph = part:size()\n"
           "assert(pw == 200 and ph == 80)\n"
           "local saved = part:save('out/part.png')\n"
           "assert(saved:sub(1, 1) == '/' and saved:match('out/part%.png$'), saved)\n"
           "local x, y, s = img:find('out/part.png')\n"
           "assert(x == 540 and y == 1650 and s > 0.95)\n"
           "assert(#img:find_all(part) == 1)\n"
           "assert(img:find('img/other.png') == nil)\n"
           "local ok, err = pcall(img.crop, img, 1000, 0, 200, 10)\n"
           "assert(not ok and err:find('not inside'), err)\n"
           "ok, err = pcall(img.color, img, 5000, 1)\n"
           "assert(not ok and err:find('outside the image'), err)\n"
           "ok, err = pcall(img.size, 42)\n"
           "assert(not ok and err:find('soulx.Image expected'), err)\n"
           "do\n"
           "  local tmp <close> = capture()\n"
           "  assert(tmp:size() == 1080)\n"
           "end\n"
           "local held\n"
           "do\n"
           "  local t2 <close> = capture()\n"
           "  held = t2\n"
           "end\n"
           "ok, err = pcall(held.size, held)\n"
           "assert(not ok and err:find('released'), err)\n"
           "assert(tostring(held) == 'Image(released)')\n"
           "assert(getmetatable(img) == 'soulx.Image')\n"
           "print('images ok')\n",
           1, argv);
    expect_ok(&g_run, "image objects");
    CHECK(strcmp(g_run.out, "images ok\n") == 0, "image objects: '%s'", g_run.out);
    CHECK(file_exists("out/part.png"), "out/part.png was not written next to the script");
}

static void test_screenshot(void) {
    fake_reset();
    run(&g_run, "shot.lua",
        "local p = screenshot('shots/full.png')\n"
        "assert(p:sub(1, 1) == '/' and p:match('shots/full%.png$'), p)\n"
        "screenshot('shots/part.png', 440, 1610, 200, 80)\n"
        "local ok, err = pcall(screenshot, 'shots/bad.png', 1000, 0, 200, 10)\n"
        "assert(not ok and err:find('is not inside the screen %(1080x2400%)'), err)\n");
    expect_ok(&g_run, "screenshot");
    CHECK(g_fake.captures == 3, "screenshot always captures (%d)", g_fake.captures);
    char path[PATH_MAX];
    SoulXImage img;
    path_in_dir(path, sizeof(path), "shots/full.png");
    CHECK(soulx_image_load_png(path, &img) == 0 && img.w == FAKE_W && img.h == FAKE_H, "full screenshot");
    free(img.rgba);
    path_in_dir(path, sizeof(path), "shots/part.png");
    bool part_ok = soulx_image_load_png(path, &img) == 0 && img.w == BTN_W && img.h == BTN_H &&
                   memcmp(img.rgba, g_fake.button.rgba, (size_t)BTN_W * BTN_H * 4) == 0;
    CHECK(part_ok, "partial screenshot holds the button");
    free(img.rgba);
    CHECK(!file_exists("shots/bad.png"), "a failed screenshot left a file");
}

static void test_device_misc(void) {
    fake_reset();
    run(&g_run, "misc.lua",
        "assert(console('ping') == 'pong')\n"
        "local ok, e = pcall(console, 'bad')\n"
        "assert(not ok and e:find('KO: unknown command'), e)\n"
        "ok, e = pcall(console, 'dead')\n"
        "assert(not ok and e:find('no answer'), e)\n"
        "local out, st = shell('echo hi')\n"
        "assert(out == 'hi' and st == 0)\n"
        "out, st = shell('exit 3')\n"
        "assert(out == '' and st == 3)\n"
        "out = shell('other')\n"
        "assert(out == 'line1\\r\\nline2', out)\n"
        "ok, e = pcall(shell, 'sleep 99', 2)\n"
        "assert(not ok and e:find('did not finish within 2 s'), e)\n"
        "ok, e = pcall(shell, 'offline')\n"
        "assert(not ok and e:find('device offline'), e)\n"
        "ok, e = pcall(shell, 'x', 0)\n"
        "assert(not ok and e:find('timeout must be'), e)\n"
        "assert(current_app() == nil)\n"
        "launch('com.example.game')\n"
        "assert(current_app() == 'com.example.game')\n"
        "stop_app('com.example.game')\n"
        "assert(current_app() == nil)\n"
        "assert(install('game.apk') == 'com.example.game')\n"
        "assert(install('game.apk', true) == 'com.example.game')\n"
        "ok, e = pcall(install, 'game.xapk')\n"
        "assert(not ok and e:find('No such file'), e)\n"
        "ok, e = pcall(install, 'notes.txt')\n"
        "assert(not ok and e:find('installing'), e)\n"
        "ok, e = pcall(install, 'game.apk', 'yes')\n"
        "assert(not ok and e:find('boolean expected'), e)\n"
        "ok, e = pcall(launch, 'com.missing.app')\n"
        "assert(not ok and e:find('cannot start com.missing.app'), e)\n"
        "assert(use('second') == 'second')\n"
        "tap(1, 1)\n"
        "ok, e = pcall(use, 'missing')\n"
        "assert(not ok and e:find(\"no such instance 'missing'\"), e)\n"
        "tap(2, 2)\n"
        "assert(soulx.version and soulx.tap)\n"
        "soulx.tap(3, 3)\n");
    expect_ok(&g_run, "device misc");
    static const char* const want[] = {
        "default: console ping", "default: shell echo hi (60000 ms)", "default: shell sleep 99 (2000 ms)",
        "default: launch com.example.game", "default: stop_app com.example.game", "default: install game.apk",
        "default: install game.apk (reinstall)", "default: launch com.missing.app", "second: tap 1 1",
        "second: tap 2 2", "second: tap 3 3",
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        CHECK(log_has(want[i]), "device misc: missing action '%s'", want[i]);
    }
    CHECK(g_fake.opens == 2 && g_fake.closes == 2, "use(): opens %d closes %d", g_fake.opens, g_fake.closes);
}

// ---------------------------------------------------------------------------
// Ctrl+C
// ---------------------------------------------------------------------------

static void* interrupter(void* arg) {
    int ms = *(const int*)arg;
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
    g_soulx_interrupted = 1;
    return NULL;
}

// Runs a script and "presses Ctrl+C" after_ms later; expects exit code 130 within 2 s.
static void expect_interrupted(const char* what, const char* code, int after_ms) {
    static int delay;
    pthread_t th;
    delay = after_ms;
    g_soulx_interrupted = 0;
    if (pthread_create(&th, NULL, interrupter, &delay) != 0) {
        CHECK(false, "%s: pthread_create failed", what);
        return;
    }
    run(&g_run, "stop.lua", code);
    pthread_join(th, NULL);
    g_soulx_interrupted = 0;
    CHECK(g_run.code == 130 && strstr(g_run.err, "script stopped (Ctrl+C) after "),
          "%s: exit code %d, stderr:\n%s", what, g_run.code, g_run.err);
    CHECK(g_run.ms < 2000.0, "%s: took %.0f ms to stop", what, g_run.ms);
    CHECK(!strstr(g_run.out, "swallowed"), "%s: the script went on after Ctrl+C", what);
    CHECK(g_fake.closes == g_fake.opens, "%s: device not closed", what);
}

static void test_interrupt(void) {
    fake_reset();
    expect_interrupted("busy loop", "while true do end\n", 150);
    expect_interrupted("pcall loop", "while true do pcall(function() while true do end end) end\n", 150);
    expect_interrupted("pcall sleep", "while true do local ok = pcall(sleep, 50) end\n", 150);
    expect_interrupted("xpcall",
                       "xpcall(function() while true do end end, function(m) return m end)\nprint('swallowed')\n",
                       150);
    expect_interrupted("coroutine",
                       "local co = coroutine.create(function() while true do end end)\n"
                       "while true do coroutine.resume(co) end\n", 150);
    expect_interrupted("wrapped coroutine",
                       "local f = coroutine.wrap(function() while true do end end)\n"
                       "pcall(f)\nprint('swallowed')\n", 150);
    expect_interrupted("sleep", "sleep(100000)\nprint('swallowed')\n", 150);
    expect_interrupted("wait", "wait('img/other.png', 100)\nprint('swallowed')\n", 150);
    expect_interrupted("string callback", "string.gsub(string.rep('a', 100), 'a', function() while true do end end)\n",
                       150);

    fake_reset();
    expect_interrupted("touches", "touch_down(10, 10)\ntouch_down(20, 20, 1)\nwhile true do end\n", 150);
    CHECK(log_has("default: touch 0 up 10 10") && log_has("default: touch 1 up 20 20"),
          "held touches were not released after Ctrl+C");
}

// ---------------------------------------------------------------------------
// Modules and memory
// ---------------------------------------------------------------------------

static void test_require(void) {
    fake_reset();
    run(&g_run, "req.lua",
        "local h = require('helpers')\n"
        "assert(h.answer == 42)\n"
        "assert(require('lib').name == 'lib')\n");
    expect_ok(&g_run, "require next to the script");
}

static void test_gc_accounting(void) {
    // 60 captures of 10 MB each, dropped right away: the collector must free them on the way
    fake_reset();
    g_fake.sample_memory = true;
    size_t base = malloc_in_use();
    run(&g_run, "gc.lua", "for i = 1, 60 do local img = capture() end\n");
    expect_ok(&g_run, "gc");
    size_t grown = g_fake.peak_in_use > base ? g_fake.peak_in_use - base : 0;
    CHECK(grown < (size_t)200 * 1024 * 1024, "dropped captures were not freed: %zu MB in use",
          grown / (1024 * 1024));

    // ... while images the script keeps stay valid
    fake_reset();
    run(&g_run, "gc2.lua",
        "local keep = {}\n"
        "for i = 1, 8 do keep[i] = capture() end\n"
        "collectgarbage()\n"
        "for i = 1, 8 do assert(keep[i]:size() == 1080 and keep[i]:color(0, 0) == keep[1]:color(0, 0)) end\n");
    expect_ok(&g_run, "kept images");
}

// ---------------------------------------------------------------------------
// The example scripts, unmodified, against the simulated game
// ---------------------------------------------------------------------------

// <repository>/examples/scripts, found from this file's path (CMake compiles absolute paths).
static bool examples_dir(char* out, size_t cap) {
    char buf[PATH_MAX];
    if (__FILE__[0] != '/' || snprintf(buf, sizeof(buf), "%s", __FILE__) >= (int)sizeof(buf)) return false;
    for (int i = 0; i < 3; i++) {                       // strip test_lua.c, macos, tests
        char* slash = strrchr(buf, '/');
        if (!slash) return false;
        *slash = '\0';
    }
    struct stat st;
    return snprintf(out, cap, "%s/examples/scripts", buf) < (int)cap && stat(out, &st) == 0 && S_ISDIR(st.st_mode);
}

static int copy_into_dir(const char* src_dir, const char* name) {
    char src[PATH_MAX];
    static char text[65536];
    snprintf(src, sizeof(src), "%s/%s", src_dir, name);
    read_text(src, text, sizeof(text));
    return text[0] ? write_text(name, text) : -1;
}

// Runs examples/scripts/<name> through a wrapper that makes every sleep and timeout 50x shorter.
static void run_example(const char* name, int argc, char** argv) {
    char wrapper[96];
    char code[1024];
    snprintf(wrapper, sizeof(wrapper), "run_%s", name);
    snprintf(code, sizeof(code),
             "local real_sleep, real_wait, real_wait_gone = sleep, wait, wait_gone\n"
             "sleep = function(ms) real_sleep(ms // 50) end\n"
             "wait = function(img, t, opts) return real_wait(img, (t or 10) / 50, opts) end\n"
             "wait_gone = function(img, t, opts) return real_wait_gone(img, (t or 10) / 50, opts) end\n"
             "dofile(args[0]:match('^(.*)/') .. '/%s')\n",
             name);
    run_on(&g_run, "default", wrapper, code, argc, argv);
}

static void test_examples(void) {
    char dir[PATH_MAX];
    if (!examples_dir(dir, sizeof(dir))) {
        printf("test_lua: examples/scripts not found from %s; example scripts not tested\n", __FILE__);
        return;
    }
    CHECK(copy_into_dir(dir, "image_loop.lua") == 0 && copy_into_dir(dir, "daily_routine.lua") == 0 &&
          copy_into_dir(dir, "tap_loop.lua") == 0 && copy_into_dir(dir, "recorded_macro.lua") == 0,
          "cannot copy the example scripts from %s", dir);

    fake_reset();
    scene_set(SCENE_ROUNDS);
    scene_show("start", 0);
    g_fake.missed_ok_taps = 1;                          // the script must notice and tap again
    char rounds[] = "2";
    char* argv1[] = { rounds, NULL };
    run_example("image_loop.lua", 1, argv1);
    CHECK(g_run.code == 0 && strstr(g_run.err, "2 of 2 rounds played"), "image_loop.lua: exit code %d, stderr:\n%s",
          g_run.code, g_run.err);
    CHECK(g_fake.missed_ok_taps == 0, "image_loop.lua: the missed tap was not retried");

    fake_reset();
    scene_set(SCENE_DAILY);
    char pkg[] = "com.example.game", battles[] = "2";
    char* argv2[] = { pkg, battles, NULL };
    run_example("daily_routine.lua", 2, argv2);
    CHECK(g_run.code == 0 && strstr(g_run.err, "2 battles won") && strstr(g_run.err, "on the home screen"),
          "daily_routine.lua: exit code %d, stderr:\n%s", g_run.code, g_run.err);
    CHECK(log_has("default: launch com.example.game") && log_has("default: key HOME"),
          "daily_routine.lua did not launch the game and leave it");

    fake_reset();                                       // stays on the noise screen: no badge
    char three[] = "3";
    char* argv3[] = { three, NULL };
    run_example("tap_loop.lua", 1, argv3);
    CHECK(g_run.code == 0 && strstr(g_run.err, "done") && log_has("default: swipe 300 1500 800 1500 200"),
          "tap_loop.lua: exit code %d, stderr:\n%s", g_run.code, g_run.err);

    fake_reset();
    run_example("recorded_macro.lua", 0, NULL);
    CHECK(g_run.code == 0 && log_has("default: long_press 200 400 900") && log_has("default: text player1") &&
          log_has("default: key ENTER"), "recorded_macro.lua: exit code %d, stderr:\n%s", g_run.code, g_run.err);
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

static int mkdir_in(const char* name) {
    char path[PATH_MAX];
    path_in_dir(path, sizeof(path), name);
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int setup_files(void) {
    char path[PATH_MAX];
    if (mkdir_in("home") != 0 || mkdir_in("img") != 0 || mkdir_in("lib") != 0) return -1;
    path_in_dir(path, sizeof(path), "img/button.png");
    if (soulx_image_save_png(path, &g_fake.button) != 0) return -1;
    path_in_dir(path, sizeof(path), "img/other.png");
    if (make_other_template(path) != 0) return -1;
    if (make_overlays(g_dir) != 0) return -1;
    if (write_text("notes.txt", "not an image\n") != 0) return -1;
    if (write_text("game.apk", "PK fake\n") != 0) return -1;
    if (write_text("helpers.lua", "return { answer = 42 }\n") != 0) return -1;
    if (write_text("lib/init.lua", "return { name = 'lib' }\n") != 0) return -1;
    return 0;
}

static int remove_entry(const char* path, const struct stat* sb, int flag, struct FTW* ftw) {
    (void)sb;
    (void)flag;
    (void)ftw;
    return remove(path);
}

// --repeat / --loop / --speed (soulx_script_run_ex), iteration() and speed().
static void test_repeat_and_speed(void) {
    fake_reset();
    // --repeat N runs the body N times; iteration() reports pass and total.
    run_ex(&g_run, "rep.lua", "local i, n = iteration(); print('pass ' .. i .. '/' .. n)\n", 3, 1.0);
    CHECK(g_run.code == 0, "repeat 3: exit code %d\n%s", g_run.code, g_run.err);
    CHECK(strstr(g_run.out, "pass 1/3") && strstr(g_run.out, "pass 2/3") && strstr(g_run.out, "pass 3/3"),
          "repeat 3: output missing a pass:\n%s", g_run.out);
    CHECK(strstr(g_run.err, "finished 3 passes"), "repeat 3: wrong summary:\n%s", g_run.err);

    // --loop (repeat <= 0): total is 0; Ctrl+C-style stop ends it. Here os.exit() ends it.
    fake_reset();
    run_ex(&g_run, "loop.lua",
           "local i, n = iteration(); print('pass ' .. i .. ' of ' .. n); if i == 2 then os.exit(0) end\n", 0, 1.0);
    CHECK(g_run.code == 0, "loop: exit code %d\n%s", g_run.code, g_run.err);
    CHECK(strstr(g_run.out, "pass 1 of 0") && strstr(g_run.out, "pass 2 of 0") && !strstr(g_run.out, "pass 3"),
          "loop: os.exit() should stop at pass 2 (total 0):\n%s", g_run.out);

    // os.exit() ends the whole run mid-repeat.
    fake_reset();
    run_ex(&g_run, "exit.lua", "print('pass ' .. (iteration())); if iteration() == 2 then os.exit(0) end\n", 5, 1.0);
    CHECK(g_run.code == 0 && strstr(g_run.out, "pass 2") && !strstr(g_run.out, "pass 3"),
          "exit mid-repeat: should stop at pass 2:\n%s%s", g_run.out, g_run.err);

    // An error in a later pass reports which pass and stops the loop.
    fake_reset();
    run_ex(&g_run, "err.lua", "if iteration() == 2 then error('boom') end\n", 5, 1.0);
    CHECK(g_run.code == 1 && strstr(g_run.err, "pass 2") && strstr(g_run.err, "boom"),
          "repeat error: expected a pass-2 error, got code %d\n%s", g_run.code, g_run.err);

    // speed() reads the run's speed and sets it (0.1..10); out of range is an argument error.
    fake_reset();
    run_ex(&g_run, "spd.lua",
           "assert(speed() == 2.0); assert(speed(0.5) == 0.5); assert(speed() == 0.5); print('ok')\n", 1, 2.0);
    CHECK(g_run.code == 0 && strstr(g_run.out, "ok"), "speed get/set: code %d\n%s", g_run.code, g_run.err);
    fake_reset();
    run_ex(&g_run, "spdbad.lua", "speed(0)\n", 1, 1.0);
    CHECK(g_run.code == 1 && strstr(g_run.err, "between 0.1 and 10"),
          "speed(0) should be rejected:\n%s", g_run.err);

    // --speed scales sleep(): at 10x a 1000 ms sleep finishes in well under a second.
    fake_reset();
    double t0 = now_ms();
    run_ex(&g_run, "slp.lua", "sleep(1000)\n", 1, 10.0);
    double dt = now_ms() - t0;
    CHECK(g_run.code == 0, "speed sleep: exit code %d\n%s", g_run.code, g_run.err);
    CHECK(dt < 600.0, "speed 10x: sleep(1000) took %.0f ms, expected well under 1000", dt);
}

// The helper library (flow / vision / grid / drag), tested by overriding the API globals it calls.
static void test_helper_library(void) {
    fake_reset();
    static const char* code =
        "local t = 0\n"
        "millis = function() return t end\n"
        "sleep = function(ms) t = t + (ms or 0) end\n"
        "find = function(img) if img == 'b' then return 10, 20, 0.9 end end\n"
        "exists = function(img) return find(img) ~= nil end\n"
        "local k, x, y = find_any({'a','b','c'})\n"
        "assert(k == 2 and x == 10 and y == 20, 'find_any')\n"
        "assert(find_any({'a'}) == nil, 'find_any none')\n"
        "assert(color_dist(0x112233, 0x112233) == 0, 'color_dist')\n"
        "local pal = { red = 0xFF0000, green = 0x00FF00, blue = 0x0000FF }\n"
        "assert(classify(0xFE0101, pal) == 'red', 'classify')\n"
        "assert(classify(0x808080, pal, 1) == nil, 'classify maxd')\n"
        "local colors = { {0xFF0000, 0x00FF00}, {0x0000FF, 0xFF0000} }\n"
        "capture = function() return { size = function() return 1000, 1000 end,\n"
        "  color = function(self, px, py) return colors[math.floor(py/10)+1][math.floor(px/10)+1] end } end\n"
        "local g = read_grid(0, 0, 10, 10, 2, 2)\n"
        "assert(g[1][1] == 0xFF0000 and g[2][1] == 0x0000FF, 'read_grid')\n"
        "assert(read_grid(0, 0, 10, 10, 2, 2, 5)[1][1] ~= nil, 'read_grid sample')\n"  // averaging path
        "local b = read_board(0, 0, 10, 10, 2, 2, pal)\n"
        "assert(b[1][1] == 'red' and b[1][2] == 'green' and b[2][1] == 'blue', 'read_board')\n"
        "local moves = {}\n"
        "touch_down = function(a, cc) moves[#moves+1] = 'd' end\n"
        "touch_move = function(a, cc) moves[#moves+1] = 'm' end\n"
        "touch_up = function() moves[#moves+1] = 'u' end\n"
        "drag_path({{1,2},{3,4},{5,6}}, {step_ms=0, hold_ms=0})\n"
        "assert(#moves == 4 and moves[1]=='d' and moves[4]=='u', 'drag_path')\n"
        "find = function(img) if img=='fire' then return 1,1,0.95 elseif img=='water' then return 1,1,0.8 end end\n"
        "local nm, sc = classify_cell({0,0,9,9}, { fire='fire', water='water' })\n"
        "assert(nm == 'fire' and sc == 0.95, 'classify_cell')\n"
        "local n = 0\n"
        "local r = retry(function() n = n + 1; return n >= 3 and 'ok' or nil end, 5)\n"
        "assert(r == 'ok' and n == 3, 'retry')\n"
        "exists = function() return false end\n"
        "assert(not pcall(function() assert_screen('x') end), 'assert_screen')\n"
        "print('helpers ok')\n";
    run(&g_run, "helpers.lua", code);
    CHECK(g_run.code == 0 && strstr(g_run.out, "helpers ok"),
          "helper library: code %d\nout:%s\nerr:%s", g_run.code, g_run.out, g_run.err);
}

// The live parameter panel API (param_slider/toggle/choice/number/button), via its SOULX_UI_DIR file protocol.
static void test_parameter_panel(void) {
    fake_reset();
    char uidir[] = "/private/tmp/soulx-ui-XXXXXX";
    if (!mkdtemp(uidir)) { CHECK(0, "parameter panel: mkdtemp failed"); return; }
    setenv("SOULX_UI_DIR", uidir, 1);
    static const char* code =
        // the window writes 'values' before/while the script runs; the script reads them live
        "local vf = assert(io.open(os.getenv('SOULX_UI_DIR') .. '/values', 'w'))\n"
        "vf:write('Speed\\t8\\nAuto\\t0\\nTarget\\twood\\nGo\\t1\\n'); vf:close()\n"
        "local s = param_slider('Speed', 1, 10, 5)\n"
        "local t = param_toggle('Auto', true)\n"
        "local c = param_choice('Target', {'fire','water','wood'}, 'water')\n"
        "local b = param_button('Go')\n"
        "local other = param_slider('Other', 1, 10, 3)\n"           // absent from values -> default
        "assert(s:value() == 8, 'slider live')\n"
        "assert(t:on() == false, 'toggle live')\n"
        "assert(c:value() == 'wood', 'choice live')\n"
        "assert(other:value() == 3, 'slider default')\n"
        "assert(b:pressed() == true, 'button pressed')\n"
        "assert(b:pressed() == false, 'button once')\n"
        "local f = assert(io.open(os.getenv('SOULX_UI_DIR') .. '/params', 'r'))\n"
        "local txt = f:read('a'); f:close()\n"
        "assert(txt:find('slider\\tSpeed\\tSpeed\\t5\\t1\\t10\\t1', 1, true), 'decl slider')\n"
        "assert(txt:find('toggle\\tAuto\\tAuto\\t1', 1, true), 'decl toggle')\n"
        "assert(txt:find('choice\\tTarget\\tTarget\\twater\\tfire\\twater\\twood', 1, true), 'decl choice')\n"
        "assert(txt:find('button\\tGo\\tGo', 1, true), 'decl button')\n"
        "print('params ok')\n";
    run(&g_run, "params.lua", code);
    unsetenv("SOULX_UI_DIR");
    CHECK(g_run.code == 0 && strstr(g_run.out, "params ok"),
          "parameter panel: code %d\nout:%s\nerr:%s", g_run.code, g_run.out, g_run.err);
}

// read_text/read_number (Vision OCR). The fake screen has no real glyphs, so this is a non-crash + type smoke test
// (the end-to-end OCR is verified live); it exercises the whole-screen and region-crop paths and the nil handling.
static void test_ocr(void) {
    fake_reset();
    scene_set(SCENE_ROUNDS);
    scene_show("start", 0);
    static const char* code =
        "local t = read_text()\n"
        "assert(t == nil or type(t) == 'string', 'read_text type')\n"
        "local n = read_number()\n"
        "assert(n == nil or type(n) == 'number', 'read_number type')\n"
        "local r = read_text({0, 0, 50, 50})\n"             // region crop path
        "assert(r == nil or type(r) == 'string', 'region type')\n"
        "print('ocr ok')\n";
    run(&g_run, "ocr.lua", code);
    CHECK(g_run.code == 0 && strstr(g_run.out, "ocr ok"),
          "ocr: code %d\nout:%s\nerr:%s", g_run.code, g_run.out, g_run.err);
}

// Multi-scale matching: the `scale` option resizes the template, and find_scaled picks the best scale.
// Sandbox for AI code (soulx mcp run_lua): no host access beyond the scripts folder, device operations still work.
static void test_sandbox(void) {
    char box[PATH_MAX], link[PATH_MAX], outside[PATH_MAX];
    snprintf(box, sizeof(box), "%s/box", g_dir);
    snprintf(link, sizeof(link), "%s/box/escape", g_dir);
    snprintf(outside, sizeof(outside), "%s/outside.png", g_dir);
    mkdir(box, 0755);
    unlink(link);
    CHECK(symlink(g_dir, link) == 0, "symlink");
    setenv("SOULX_LUA_SANDBOX_DIR", box, 1);

    run(&g_run, "sb_globals.lua",
        "assert(io == nil and load == nil and dofile == nil and loadfile == nil)\n"
        "assert(require == nil and package == nil and console == nil and soulx.console == nil)\n"
        "assert(os.getenv == nil and os.remove == nil and os.rename == nil and os.tmpname == nil)\n"
        "assert(os.execute == nil and type(os.time) == 'function' and type(os.clock) == 'function')\n"
        "tap(10, 20)\n"
        "assert(screen_size() > 0)\n"
        "screenshot('inside.png')\n");
    expect_ok(&g_run, "sandbox: device operations and a file inside the folder");
    char inside[PATH_MAX];
    snprintf(inside, sizeof(inside), "%s/inside.png", box);
    CHECK(access(inside, F_OK) == 0, "sandbox: relative paths start in the scripts folder");

    expect_error("sandbox: absolute path outside", "screenshot('/private/tmp/soulx-sandbox-x.png')\n", "sandbox");
    expect_error("sandbox: ..", "screenshot('../outside.png')\n", "sandbox");
    expect_error("sandbox: symlink out", "screenshot('escape/outside.png')\n", "sandbox");
    expect_error("sandbox: template outside", "find('/etc/hosts')\n", "sandbox");
    expect_error("sandbox: io", "io.open('/etc/hosts')\n", "io");
    CHECK(access(outside, F_OK) != 0 && access("/private/tmp/soulx-sandbox-x.png", F_OK) != 0,
          "sandbox: nothing was written outside");

    unsetenv("SOULX_LUA_SANDBOX_DIR");
    run(&g_run, "sb_off.lua", "assert(io ~= nil and require ~= nil and load ~= nil)\n");
    expect_ok(&g_run, "no sandbox: the user's own scripts keep io and require");
    unlink(link);
}

static void test_scale_matching(void) {
    fake_reset();
    scene_set(SCENE_ROUNDS);
    scene_show("start", 0);
    static const char* code =
        "local scr = capture()\n"
        "local w, h = scr:size()\n"
        "assert(w >= 200 and h >= 200, 'scene too small')\n"
        "local tmpl = scr:crop(40, 40, 64, 64)\n"
        "local _, _, s1 = scr:find(tmpl, {scale = 1.0, threshold = 0.3})\n"
        "assert(s1 and s1 > 0.9, 'scale 1.0 self-match score ' .. tostring(s1))\n"
        "local _, _, s3 = scr:find(tmpl, {scale = 3.0, threshold = 0.3})\n"
        "assert(s3 == nil or s3 < s1, 'scaling the template changes the match')\n"
        "local bx, by, bscore, bs = find_scaled(tmpl, {1.0, 1.5}, {threshold = 0.3})\n"
        "assert(bx ~= nil and bs ~= nil, 'find_scaled returns a match and scale')\n"
        "print('scale ok')\n";
    run(&g_run, "scale.lua", code);
    CHECK(g_run.code == 0 && strstr(g_run.out, "scale ok"),
          "scale matching: code %d\nout:%s\nerr:%s", g_run.code, g_run.out, g_run.err);
}

int main(void) {
    char tmpl[] = "/private/tmp/soulx-test-lua-XXXXXX";
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(g_dir, sizeof(g_dir), "%s", tmpl);
    char home[PATH_MAX];
    path_in_dir(home, sizeof(home), "home");
    setenv("SOULX_HOME", home, 1);               // never touch the real ~/.soulx-android
    unsetenv("SOULX_INSTANCE");

    if (make_images() != 0 || setup_files() != 0) {
        fprintf(stderr, "test_lua: cannot prepare %s\n", g_dir);
        return 1;
    }

    test_device_ops_hook();
    test_input_api();
    test_input_errors();
    test_args_and_output();
    test_errors_and_exit();
    test_screen_cache();
    test_matching();
    test_template_reload();
    test_wait();
    test_image_objects();
    test_screenshot();
    test_device_misc();
    test_interrupt();
    test_require();
    test_gc_accounting();
    test_repeat_and_speed();
    test_helper_library();
    test_parameter_panel();
    test_ocr();
    test_scale_matching();
    test_sandbox();
    test_examples();

    soulx_lua_set_device_ops(NULL, 0);
    free(g_fake.screen.rgba);
    free(g_fake.button.rgba);
    for (int i = 0; i < OVERLAYS; i++) free(g_overlays[i].img.rgba);
    if (g_failures == 0) nftw(g_dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);

    if (g_failures) {
        fprintf(stderr, "test_lua: %d of %d check(s) failed (files kept in %s)\n", g_failures, g_checks, g_dir);
        return 1;
    }
    printf("test_lua: all %d checks passed\n", g_checks);
    return 0;
}
