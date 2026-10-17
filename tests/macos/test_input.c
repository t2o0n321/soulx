/*
 * SoulX Android Emulator - unit tests for source/platform/macos/input.c and source/script/record.c
 * Logical <-> natural coordinate mapping for rotations 0..3 (round trips and the raw values
 * verified on the emulator), framebuffer rotation into the logical orientation, rotation parsing,
 * key names, text escaping, `getevent -lt` line parsing and the macro recorder's gesture / key
 * classification on recorded and synthetic event streams. No emulator needed.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soulx_mac.h"

// input.c (exported for the tests, not part of soulx_mac.h)
extern void soulx_map_logical_to_natural(int r, int w_nat, int h_nat, int lx, int ly, int* nx, int* ny);
extern void soulx_map_natural_to_logical(int r, int w_nat, int h_nat, int nx, int ny, int* lx, int* ly);
extern void soulx_map_logical_pixel(int r, int w_nat, int h_nat, int lx, int ly, int* nx, int* ny);
extern int  soulx_input_to_raw(int px, int dim);
extern int  soulx_image_rotate_to_logical(const SoulXImage* fb, int rotation, SoulXImage* out);
extern int  soulx_input_parse_rotation(const char* dumpsys);
extern int  soulx_input_keycode(const char* name_or_code);
extern int  soulx_input_console_text_cmd(const char* s, size_t n, char* cmd, size_t cap);
extern int  soulx_input_shell_text_cmd(const char* s, size_t n, char* cmd, size_t cap);
extern int  soulx_input_key_units(char c);
extern size_t soulx_input_text_chunk(const char* s, size_t n);

// record.c (exported for the tests)
#define SOULX_GE_NONE    0
#define SOULX_GE_DEVICE  1
#define SOULX_GE_NAME    2
#define SOULX_GE_EVENT   3
typedef struct SoulXRecorder SoulXRecorder;
typedef int (*SoulXRecRotationFn)(void* ctx);
extern int  soulx_getevent_parse(const char* line, double* ts_ms, char* dev, size_t dev_cap, char* type,
                                size_t type_cap, char* code, size_t code_cap, int* value);
extern SoulXRecorder* soulx_recorder_new(FILE* out, int w_nat, int h_nat, SoulXRecRotationFn rotation_fn, void* ctx,
                                       bool echo);
extern void soulx_recorder_prime(SoulXRecorder* r, int slots, int raw_x, int raw_y);
extern int  soulx_recorder_feed(SoulXRecorder* r, const char* line);
extern int  soulx_recorder_idle(SoulXRecorder* r, double host_now_ms);
extern int  soulx_recorder_finish(SoulXRecorder* r);
extern int  soulx_recorder_actions(const SoulXRecorder* r);
extern void soulx_recorder_free(SoulXRecorder* r);

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

static void check_str(const char* got, const char* want, const char* file, int line) {
    g_checks++;
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s:%d: output differs\n--- want ---\n%s--- got ---\n%s------------\n", file, line, want,
                got ? got : "(null)\n");
        g_failures++;
    }
}

#define CHECK_STR(got, want) check_str((got), (want), __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// Coordinate mapping
// ---------------------------------------------------------------------------

static void roundtrip_panel(int w, int h) {
    for (int r = 0; r < 4; r++) {
        int lw = (r & 1) ? h : w, lh = (r & 1) ? w : h;
        int bad = 0, out_of_range = 0, points = 0;
        const int fixed[][2] = { { 0, 0 }, { lw - 1, 0 }, { 0, lh - 1 }, { lw - 1, lh - 1 }, { lw / 2, lh / 2 },
                                 { lw, lh }, { 1, 1 }, { lw - 2, 3 }, { 7, lh - 9 } };
        for (int i = 0; i < (int)(sizeof(fixed) / sizeof(fixed[0])) + 400; i++) {
            int lx, ly;
            if (i < (int)(sizeof(fixed) / sizeof(fixed[0]))) {
                lx = fixed[i][0];
                ly = fixed[i][1];
            } else {
                int k = i - (int)(sizeof(fixed) / sizeof(fixed[0]));
                lx = (k * 37) % lw;
                ly = (k * 53) % lh;
            }
            int nx, ny, bx, by;
            soulx_map_logical_to_natural(r, w, h, lx, ly, &nx, &ny);
            if (nx < 0 || nx > w || ny < 0 || ny > h) out_of_range++;
            soulx_map_natural_to_logical(r, w, h, nx, ny, &bx, &by);
            if (bx != lx || by != ly) bad++;
            // and the other way round, starting from a natural point
            soulx_map_natural_to_logical(r, w, h, lx % (w + 1), ly % (h + 1), &bx, &by);
            soulx_map_logical_to_natural(r, w, h, bx, by, &nx, &ny);
            if (nx != lx % (w + 1) || ny != ly % (h + 1)) bad++;
            points++;
        }
        CHECK(points > 400);
        CHECK(bad == 0);
        CHECK(out_of_range == 0);
    }
}

static void test_mapping(void) {
    roundtrip_panel(1080, 2400);
    roundtrip_panel(2400, 1080);            // landscape panel (soulx create --landscape)
    roundtrip_panel(720, 1600);

    // the formula table itself (W = 1080, H = 2400)
    int nx, ny;
    soulx_map_logical_to_natural(0, 1080, 2400, 10, 20, &nx, &ny);
    CHECK(nx == 10 && ny == 20);
    soulx_map_logical_to_natural(1, 1080, 2400, 10, 20, &nx, &ny);
    CHECK(nx == 1080 - 20 && ny == 10);
    soulx_map_logical_to_natural(2, 1080, 2400, 10, 20, &nx, &ny);
    CHECK(nx == 1080 - 10 && ny == 2400 - 20);
    soulx_map_logical_to_natural(3, 1080, 2400, 10, 20, &nx, &ny);
    CHECK(nx == 20 && ny == 2400 - 10);
    soulx_map_logical_to_natural(7, 1080, 2400, 10, 20, &nx, &ny);  // invalid rotation -> 0
    CHECK(nx == 10 && ny == 20);

    // The touch path uses pixel indices, not point coordinates: soulx_map_logical_pixel maps a
    // logical pixel (lx, ly) to the natural pixel that displays it (same formulas the framebuffer
    // rotation uses), one less than the point mapping on each mirrored axis.
    soulx_map_logical_pixel(0, 1080, 2400, 10, 20, &nx, &ny);
    CHECK(nx == 10 && ny == 20);
    soulx_map_logical_pixel(1, 1080, 2400, 2200, 897, &nx, &ny);
    CHECK(nx == 1080 - 1 - 897 && ny == 2200);
    soulx_map_logical_pixel(2, 1080, 2400, 10, 20, &nx, &ny);
    CHECK(nx == 1080 - 1 - 10 && ny == 2400 - 1 - 20);
    soulx_map_logical_pixel(3, 1080, 2400, 2200, 897, &nx, &ny);
    CHECK(nx == 897 && ny == 2400 - 1 - 2200);
    soulx_map_logical_pixel(7, 1080, 2400, 10, 20, &nx, &ny);        // invalid rotation -> 0
    CHECK(nx == 10 && ny == 20);

    // verified on the emulator: taps landed on the expected UI rows. The raw value
    // aims at the centre of the natural pixel (raw = (2*px + 1) * 32768 / (2 * dim)), so the guest
    // cooks it (nat = raw * dim / 32768) back onto that pixel, the far edge included.
    soulx_map_logical_pixel(1, 1080, 2400, 2200, 897, &nx, &ny);     // nx=182, ny=2200
    CHECK(soulx_input_to_raw(nx, 1080) == 5537 && soulx_input_to_raw(ny, 2400) == 30044);
    soulx_map_logical_pixel(3, 1080, 2400, 2200, 897, &nx, &ny);     // nx=897, ny=199
    CHECK(soulx_input_to_raw(nx, 1080) == 27230 && soulx_input_to_raw(ny, 2400) == 2723);
    soulx_map_logical_pixel(2, 1080, 2400, 900, 897, &nx, &ny);      // nx=179, ny=1502
    CHECK(soulx_input_to_raw(nx, 1080) == 5446 && soulx_input_to_raw(ny, 2400) == 20514);

    // raw = (2*px + 1) * 32768 / (2 * dim), with px clamped to 0..dim-1 (the far edge reachable)
    CHECK(soulx_input_to_raw(0, 1080) == 15);
    CHECK(soulx_input_to_raw(540, 1080) == 16399);
    CHECK(soulx_input_to_raw(1079, 1080) == 32752);
    CHECK(soulx_input_to_raw(1080, 1080) == 32752);      // clamped to pixel 1079
    CHECK(soulx_input_to_raw(5000, 1080) == 32752);
    CHECK(soulx_input_to_raw(-4, 1080) == 15);           // clamped to pixel 0
    CHECK(soulx_input_to_raw(10, 0) == 0);
}

// ---------------------------------------------------------------------------
// Framebuffer rotation
// ---------------------------------------------------------------------------

// Natural framebuffer whose every pixel encodes its own coordinates.
static SoulXImage make_fb(int w, int h) {
    SoulXImage img = { w, h, malloc((size_t)w * (size_t)h * 4) };
    if (!img.rgba) exit(2);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t* p = img.rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            p[0] = (uint8_t)(x & 0xFF);
            p[1] = (uint8_t)(y & 0xFF);
            p[2] = (uint8_t)(((x >> 8) & 0x0F) | ((y >> 8) << 4));
            p[3] = 0xA5;
        }
    }
    return img;
}

// Every logical pixel must hold the natural pixel given by the mapping formulas applied to pixel
// centres (2*l+1 on a doubled grid), i.e. (W-1-ly, lx) for R=1 and so on.
static int check_rotation(const SoulXImage* fb, int r, const SoulXImage* out) {
    int w = fb->w, h = fb->h;
    int lw = (r & 1) ? h : w, lh = (r & 1) ? w : h;
    if (out->w != lw || out->h != lh || !out->rgba) return -1;
    int bad = 0;
    for (int ly = 0; ly < lh; ly++) {
        for (int lx = 0; lx < lw; lx++) {
            int a, b;
            soulx_map_logical_to_natural(r, 2 * w, 2 * h, 2 * lx + 1, 2 * ly + 1, &a, &b);
            int nx = (a - 1) / 2, ny = (b - 1) / 2;
            int ex = r == 0 ? lx : r == 1 ? w - 1 - ly : r == 2 ? w - 1 - lx : ly;     // pixel formulas
            int ey = r == 0 ? ly : r == 1 ? lx : r == 2 ? h - 1 - ly : h - 1 - lx;
            if (nx != ex || ny != ey) bad++;
            const uint8_t* got = out->rgba + ((size_t)ly * (size_t)lw + (size_t)lx) * 4;
            const uint8_t* want = fb->rgba + ((size_t)ny * (size_t)w + (size_t)nx) * 4;
            if (memcmp(got, want, 4) != 0) bad++;
        }
    }
    return bad;
}

static void test_rotate_image(void) {
    const int sizes[][2] = { { 5, 3 }, { 3, 5 }, { 4, 4 }, { 1, 7 }, { 7, 1 }, { 130, 70 }, { 64, 129 } };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        SoulXImage fb = make_fb(sizes[i][0], sizes[i][1]);
        for (int r = 0; r < 4; r++) {
            SoulXImage out = { 0, 0, NULL };
            CHECK(soulx_image_rotate_to_logical(&fb, r, &out) == 0);
            CHECK(check_rotation(&fb, r, &out) == 0);
            free(out.rgba);
        }
        free(fb.rgba);
    }

    // full-size panel: correctness of every pixel and speed of the copy
    SoulXImage fb = make_fb(1080, 2400);
    for (int r = 1; r < 4; r++) {
        SoulXImage out = { 0, 0, NULL };
        double t0 = soulx_now_ms();
        CHECK(soulx_image_rotate_to_logical(&fb, r, &out) == 0);
        double ms = soulx_now_ms() - t0;
        printf("  rotate 1080x2400 by %d degrees: %.1f ms\n", r * 90, ms);
        CHECK(ms < 250.0);
        CHECK(check_rotation(&fb, r, &out) == 0);
        free(out.rgba);
    }

    // in place (out == fb)
    SoulXImage copy = make_fb(1080, 2400);
    CHECK(soulx_image_rotate_to_logical(&copy, 1, &copy) == 0);
    CHECK(copy.w == 2400 && copy.h == 1080);
    CHECK(check_rotation(&fb, 1, &copy) == 0);
    free(copy.rgba);
    free(fb.rgba);

    // invalid arguments
    SoulXImage small = make_fb(2, 2);
    SoulXImage out = { 9, 9, NULL };
    CHECK(soulx_image_rotate_to_logical(&small, 4, &out) == -1 && out.w == 0 && out.rgba == NULL);
    CHECK(soulx_image_rotate_to_logical(&small, -1, &out) == -1);
    CHECK(soulx_image_rotate_to_logical(NULL, 1, &out) == -1);
    CHECK(soulx_image_rotate_to_logical(&small, 1, NULL) == -1);
    SoulXImage empty = { 0, 4, small.rgba };
    CHECK(soulx_image_rotate_to_logical(&empty, 1, &out) == -1);
    free(small.rgba);
}

// ---------------------------------------------------------------------------
// Rotation parsing, key names, text escaping
// ---------------------------------------------------------------------------

static void test_parse_rotation(void) {
    CHECK(soulx_input_parse_rotation("  mCurrentRotation=ROTATION_90 mDeferredRotationPauseCount=0\n") == 1);
    CHECK(soulx_input_parse_rotation("mCurrentRotation=ROTATION_0") == 0);
    CHECK(soulx_input_parse_rotation("x\n  mCurrentRotation=ROTATION_180\n") == 2);
    CHECK(soulx_input_parse_rotation("mCurrentRotation=ROTATION_270 ") == 3);
    CHECK(soulx_input_parse_rotation("mCurrentRotation=ROTATION_90\nmCurrentRotation=ROTATION_270\n") == 1);
    CHECK(soulx_input_parse_rotation("mCurrentRotation=ROTATION_45 mCurrentRotation=ROTATION_180") == 2);
    CHECK(soulx_input_parse_rotation("mCurrentRotation=ROTATION_900 mCurrentRotation=ROTATION_270") == 3);
    CHECK(soulx_input_parse_rotation("mCurrentRotation=3") == 3);
    CHECK(soulx_input_parse_rotation("mCurrentRotation=12") == -1);
    CHECK(soulx_input_parse_rotation("mRotation=1 mUserRotation=ROTATION_90") == -1);
    CHECK(soulx_input_parse_rotation("") == -1);
    CHECK(soulx_input_parse_rotation(NULL) == -1);
}

static void test_keycodes(void) {
    CHECK(soulx_input_keycode("BACK") == 4);
    CHECK(soulx_input_keycode("back") == 4);
    CHECK(soulx_input_keycode("KEYCODE_BACK") == 4);
    CHECK(soulx_input_keycode("keycode_home") == 3);
    CHECK(soulx_input_keycode("HOME") == 3);
    CHECK(soulx_input_keycode("4") == 4);
    CHECK(soulx_input_keycode("66") == 66);
    CHECK(soulx_input_keycode("KEYCODE_4") == 11);          // the digit key, not code 4
    CHECK(soulx_input_keycode("KEYCODE_0") == 7);
    CHECK(soulx_input_keycode("A") == 29);
    CHECK(soulx_input_keycode("z") == 54);
    CHECK(soulx_input_keycode("KEYCODE_Z") == 54);
    CHECK(soulx_input_keycode("F1") == 131);
    CHECK(soulx_input_keycode("f12") == 142);
    CHECK(soulx_input_keycode("F13") == -1);
    CHECK(soulx_input_keycode("NUMPAD_7") == 151);
    CHECK(soulx_input_keycode("APP_SWITCH") == 187);
    CHECK(soulx_input_keycode("RECENTS") == 187);
    CHECK(soulx_input_keycode("MENU") == 82);
    CHECK(soulx_input_keycode("ENTER") == 66);
    CHECK(soulx_input_keycode("DEL") == 67);
    CHECK(soulx_input_keycode("BACKSPACE") == 67);
    CHECK(soulx_input_keycode("FORWARD_DEL") == 112);
    CHECK(soulx_input_keycode("TAB") == 61);
    CHECK(soulx_input_keycode("SPACE") == 62);
    CHECK(soulx_input_keycode("ESCAPE") == 111);
    CHECK(soulx_input_keycode("DPAD_UP") == 19);
    CHECK(soulx_input_keycode("DPAD_DOWN") == 20);
    CHECK(soulx_input_keycode("DPAD_LEFT") == 21);
    CHECK(soulx_input_keycode("DPAD_RIGHT") == 22);
    CHECK(soulx_input_keycode("DPAD_CENTER") == 23);
    CHECK(soulx_input_keycode("VOLUME_UP") == 24);
    CHECK(soulx_input_keycode("VOLUME_DOWN") == 25);
    CHECK(soulx_input_keycode("VOLUME_MUTE") == 164);
    CHECK(soulx_input_keycode("POWER") == 26);
    CHECK(soulx_input_keycode("CAMERA") == 27);
    CHECK(soulx_input_keycode("SEARCH") == 84);
    CHECK(soulx_input_keycode("MEDIA_PLAY_PAUSE") == 85);
    CHECK(soulx_input_keycode("PAGE_UP") == 92);
    CHECK(soulx_input_keycode("PAGE_DOWN") == 93);
    CHECK(soulx_input_keycode("MOVE_HOME") == 122);
    CHECK(soulx_input_keycode("MOVE_END") == 123);
    CHECK(soulx_input_keycode("999") == 999);
    CHECK(soulx_input_keycode("1000") == -1);
    CHECK(soulx_input_keycode("0") == -1);
    CHECK(soulx_input_keycode("KEYCODE_") == -1);
    CHECK(soulx_input_keycode("NOPE") == -1);
    CHECK(soulx_input_keycode("B@CK") == -1);
    CHECK(soulx_input_keycode("") == -1);
    CHECK(soulx_input_keycode(NULL) == -1);
    CHECK(soulx_input_keycode("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") == -1);

    // every name the recorder writes must be accepted by key()
    const char* recorded[] = { "BACK", "HOME", "MOVE_HOME", "MOVE_END", "APP_SWITCH", "MENU", "ENTER", "NUMPAD_ENTER",
                               "DEL", "FORWARD_DEL", "TAB", "ESCAPE", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT",
                               "DPAD_RIGHT", "DPAD_CENTER", "VOLUME_UP", "VOLUME_DOWN", "VOLUME_MUTE", "POWER",
                               "SLEEP", "WAKEUP", "CAMERA", "SEARCH", "PAGE_UP", "PAGE_DOWN", "INSERT",
                               "MEDIA_PLAY_PAUSE", "MEDIA_PLAY", "MEDIA_PAUSE", "MEDIA_STOP", "MEDIA_NEXT",
                               "MEDIA_PREVIOUS", "MEDIA_REWIND", "MEDIA_FAST_FORWARD", "BRIGHTNESS_DOWN",
                               "BRIGHTNESS_UP", "ASSIST", "VOICE_ASSIST", "CUT", "COPY", "PASTE", "SYSRQ",
                               "FORWARD", "REFRESH", "HELP", "CALCULATOR", "ALL_APPS", "F1", "F12" };
    for (size_t i = 0; i < sizeof(recorded) / sizeof(recorded[0]); i++) {
        if (soulx_input_keycode(recorded[i]) <= 0) fprintf(stderr, "  unknown recorded key name %s\n", recorded[i]);
        CHECK(soulx_input_keycode(recorded[i]) > 0);
    }
}

// A fake emulator console that behaves like the real one for `event text`: it silently drops a
// whole command that needs more than 15 key strokes and still answers OK. "Typing"
// a chunk through it appends to |typed| only when it was not dropped.
static bool fake_console_type(char* typed, size_t* tn, const char* s, size_t k) {
    int units = 0;
    for (size_t j = 0; j < k; j++) units += soulx_input_key_units(s[j]);
    if (units > 15) return false;              // dropped
    memcpy(typed + *tn, s, k);
    *tn += k;
    return true;
}

// soulx_input_text must split text into chunks the emulator never drops.
static void test_text_chunking(void) {
    // A shifted character (a capital, or one of !"#$%&()*+:<>?@^_{|}~) needs two key strokes.
    CHECK(soulx_input_key_units('a') == 1);
    CHECK(soulx_input_key_units('Z') == 2);
    CHECK(soulx_input_key_units('!') == 2);
    CHECK(soulx_input_key_units('@') == 2);
    CHECK(soulx_input_key_units('5') == 1);
    CHECK(soulx_input_key_units(' ') == 1);
    CHECK(soulx_input_key_units('.') == 1);

    // The chunk boundary: at most 14 key strokes per chunk (the emulator drops a whole command of
    // more than about 15), a shifted character counting two.
    CHECK(soulx_input_text_chunk("abcdefghijklmn", 14) == 14);       // 14 units: one chunk
    CHECK(soulx_input_text_chunk("abcdefghijklmno", 15) == 14);      // the 15th char spills over
    CHECK(soulx_input_text_chunk("AAAAAAA", 7) == 7);                // 7 capitals = 14 units
    CHECK(soulx_input_text_chunk("AAAAAAAA", 8) == 7);               // an 8th would be 16 units
    CHECK(soulx_input_text_chunk("a!a!a!a!a!", 10) == 9);           // 1+2 repeated: 9 chars = 13 units, a 10th (!) would be 15
    CHECK(soulx_input_text_chunk("\n", 1) == 0);                     // a control byte ends a chunk at 0
    CHECK(soulx_input_text_chunk("ab\tc", 4) == 2);                  // a tab too

    // Drive the fake console with the real splitting (exactly what soulx_input_text does): no chunk
    // is ever dropped, and every character comes through, for a range of strings.
    const char* strings[] = {
        "abcdefghijklmnop",                                        // 16 lowercase -> must split
        "ABCDEFGH",                                                // 8 capitals = 16 units
        "Hello, World! 100% off!!!",                               // mixed case and symbols
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",      // 50 plain characters
        "A!A!A!A!A!A!A!A!A!A!",                                     // every character shifted
        "x",                                                       // one character
        "user@example.com password=hunter2",                       // a realistic login string
    };
    for (size_t t = 0; t < sizeof(strings) / sizeof(strings[0]); t++) {
        const char* s = strings[t];
        size_t len = strlen(s);
        char typed[128];
        size_t tn = 0, i = 0;
        int chunks = 0;
        bool dropped = false;
        while (i < len) {
            size_t k = soulx_input_text_chunk(s + i, len - i);
            CHECK(k > 0);                                          // always progresses on printable ASCII
            if (k == 0) break;
            if (!fake_console_type(typed, &tn, s + i, k)) dropped = true;
            chunks++;
            i += k;
        }
        typed[tn] = '\0';
        CHECK(!dropped);                                           // nothing the emulator would drop
        CHECK(tn == len && strcmp(typed, s) == 0);                 // every character typed, in order
        CHECK(chunks >= 1);
    }
}

static void test_text_commands(void) {
    char cmd[256];
    CHECK(soulx_input_console_text_cmd("hi", 2, cmd, sizeof(cmd)) == 0);
    CHECK_STR(cmd, "event text hi");
    CHECK(soulx_input_console_text_cmd("  a b", 5, cmd, sizeof(cmd)) == 0);       // leading blanks survive
    CHECK_STR(cmd, "event text \\x20\\x20a b");
    CHECK(soulx_input_console_text_cmd("a\\b", 3, cmd, sizeof(cmd)) == 0);        // backslash doubled
    CHECK_STR(cmd, "event text a\\\\b");
    CHECK(soulx_input_console_text_cmd("x ", 2, cmd, sizeof(cmd)) == 0);
    CHECK_STR(cmd, "event text x ");
    CHECK(soulx_input_console_text_cmd("Hello, World! 100% \"ok\"", 23, cmd, sizeof(cmd)) == 0);
    CHECK_STR(cmd, "event text Hello, World! 100% \"ok\"");
    CHECK(soulx_input_console_text_cmd("a\nb", 3, cmd, sizeof(cmd)) == -1);
    CHECK(soulx_input_console_text_cmd("abcdef", 6, cmd, 14) == -1);             // does not fit
    CHECK(soulx_input_console_text_cmd("abc", 3, cmd, 15) == 0);
    CHECK_STR(cmd, "event text abc");

    CHECK(soulx_input_shell_text_cmd("a b", 3, cmd, sizeof(cmd)) == 0);
    CHECK_STR(cmd, "input text a%sb");
    CHECK(soulx_input_shell_text_cmd("it's", 4, cmd, sizeof(cmd)) == 0);
    CHECK_STR(cmd, "input text it\\'s");
    CHECK(soulx_input_shell_text_cmd("$(rm -rf /)", 11, cmd, sizeof(cmd)) == 0);
    CHECK_STR(cmd, "input text \\$\\(rm%s\\-rf%s\\/\\)");
    CHECK(soulx_input_shell_text_cmd("50%;`x`|&", 9, cmd, sizeof(cmd)) == 0);
    CHECK_STR(cmd, "input text 50\\%\\;\\`x\\`\\|\\&");
    CHECK(soulx_input_shell_text_cmd("tab\there", 8, cmd, sizeof(cmd)) == -1);
    CHECK(soulx_input_shell_text_cmd("abc", 3, NULL, 0) == -1);
}

// ---------------------------------------------------------------------------
// getevent parsing
// ---------------------------------------------------------------------------

static void test_getevent_parse(void) {
    double ts = -1;
    char dev[64], type[32], code[48];
    int value = 99;

    CHECK(soulx_getevent_parse("[     302.729329] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   00000000            ",
                              &ts, dev, sizeof(dev), type, sizeof(type), code, sizeof(code), &value) == SOULX_GE_EVENT);
    CHECK(ts > 302729.328 && ts < 302729.330);
    CHECK(strcmp(dev, "/dev/input/event1") == 0 && strcmp(type, "EV_ABS") == 0);
    CHECK(strcmp(code, "ABS_MT_TRACKING_ID") == 0 && value == 0);

    CHECK(soulx_getevent_parse("[     302.729651] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   ffffffff            ",
                              &ts, dev, sizeof(dev), type, sizeof(type), code, sizeof(code), &value) == SOULX_GE_EVENT);
    CHECK(value == -1);

    CHECK(soulx_getevent_parse("[     302.729329] /dev/input/event1: EV_ABS       ABS_MT_POSITION_X    00003fff            ",
                              &ts, dev, sizeof(dev), type, sizeof(type), code, sizeof(code), &value) == SOULX_GE_EVENT);
    CHECK(value == 0x3fff && strcmp(code, "ABS_MT_POSITION_X") == 0);

    // single-device form (getevent -lt /dev/input/event1): no device path
    CHECK(soulx_getevent_parse("[    1115.534456] EV_ABS       ABS_MT_SLOT          00000000", &ts, dev, sizeof(dev), type,
                              sizeof(type), code, sizeof(code), &value) == SOULX_GE_EVENT);
    CHECK(dev[0] == '\0' && strcmp(type, "EV_ABS") == 0 && strcmp(code, "ABS_MT_SLOT") == 0 && value == 0);
    CHECK(ts > 1115534.455 && ts < 1115534.457);

    CHECK(soulx_getevent_parse("[    1056.444318] /dev/input/event12: EV_KEY       KEY_H                DOWN                ",
                              &ts, dev, sizeof(dev), type, sizeof(type), code, sizeof(code), &value) == SOULX_GE_EVENT);
    CHECK(strcmp(dev, "/dev/input/event12") == 0 && strcmp(code, "KEY_H") == 0 && value == 1);
    CHECK(soulx_getevent_parse("[    1056.444520] /dev/input/event12: EV_KEY       KEY_H                UP                  ",
                              &ts, dev, sizeof(dev), type, sizeof(type), code, sizeof(code), &value) == SOULX_GE_EVENT);
    CHECK(value == 0);
    CHECK(soulx_getevent_parse("[    1056.444520] /dev/input/event12: EV_KEY       KEY_H                REPEAT\r", &ts, dev,
                              sizeof(dev), type, sizeof(type), code, sizeof(code), &value) == SOULX_GE_EVENT);
    CHECK(value == 2);
    CHECK(soulx_getevent_parse("[    1056.444318] /dev/input/event12: EV_SYN       SYN_REPORT           00000000            ",
                              &ts, dev, sizeof(dev), type, sizeof(type), code, sizeof(code), &value) == SOULX_GE_EVENT);
    CHECK(strcmp(type, "EV_SYN") == 0 && strcmp(code, "SYN_REPORT") == 0);
    CHECK(soulx_getevent_parse("[       1.030000] /dev/input/event1: EV_SYN       SYN_REPORT           00000000", &ts, dev,
                              sizeof(dev), type, sizeof(type), code, sizeof(code), &value) == SOULX_GE_EVENT);
    CHECK(ts == 1030.0);                                    // exact: the 30 ms thresholds compare against it

    CHECK(soulx_getevent_parse("add device 12: /dev/input/event1", &ts, dev, sizeof(dev), type, sizeof(type), code,
                              sizeof(code), &value) == SOULX_GE_DEVICE);
    CHECK(strcmp(dev, "/dev/input/event1") == 0);
    CHECK(soulx_getevent_parse("  name:     \"virtio_input_multi_touch_1\"", &ts, dev, sizeof(dev), type, sizeof(type),
                              code, sizeof(code), &value) == SOULX_GE_NAME);
    CHECK(strcmp(dev, "virtio_input_multi_touch_1") == 0);

    const char* none[] = { "", "   ", "remove device 3: /dev/input/event2", "  events:", "could not get evdev event",
                           "[ 12.5]", "[ 12.5] EV_ABS", "[ 12.5] EV_ABS ABS_X", "[abc] EV_ABS ABS_X 0", "add device x",
                           "  name:     unquoted", "[ 1234567890123.0] EV_ABS ABS_X 00000001" };
    for (size_t i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
        CHECK(soulx_getevent_parse(none[i], &ts, dev, sizeof(dev), type, sizeof(type), code, sizeof(code), &value) ==
              SOULX_GE_NONE);
    }
    CHECK(soulx_getevent_parse(NULL, &ts, dev, sizeof(dev), type, sizeof(type), code, sizeof(code), &value) ==
          SOULX_GE_NONE);
    // a device path that does not fit the buffer is rejected, never truncated
    char small[8];
    CHECK(soulx_getevent_parse("add device 1: /dev/input/event12", &ts, small, sizeof(small), type, sizeof(type), code,
                              sizeof(code), &value) == SOULX_GE_NONE);
}

// ---------------------------------------------------------------------------
// Recorder helpers
// ---------------------------------------------------------------------------

#define MAX_LINES 2500
#define LINE_LEN  160
#define TOUCH     "/dev/input/event1"
#define TOUCH_2ND "/dev/input/event11"
#define KBD       "/dev/input/event12"

typedef struct {
    char line[MAX_LINES][LINE_LEN];
    int  n;
} Stream;

static Stream g_stream;

static int g_rot;               // rotation the callback reports
static int g_rot_seq[8];        // or one value per call (gesture) when g_rot_seq_n > 0
static int g_rot_seq_n;
static int g_rot_calls;

static int test_rotation(void* ctx) {
    (void)ctx;
    int r = g_rot;
    if (g_rot_seq_n > 0) r = g_rot_seq[g_rot_calls < g_rot_seq_n ? g_rot_calls : g_rot_seq_n - 1];
    g_rot_calls++;
    return r;
}

static void set_rotation(int r) {
    g_rot = r;
    g_rot_seq_n = 0;
    g_rot_calls = 0;
}

static void st_reset(Stream* s) {
    s->n = 0;
}

static void st_add(Stream* s, const char* text) {
    if (s->n >= MAX_LINES) {
        fprintf(stderr, "test stream too long\n");
        exit(2);
    }
    snprintf(s->line[s->n++], LINE_LEN, "%s", text);
}

// One `getevent -lt` line at |ms| (dev NULL: single-device form).
static void st_ev(Stream* s, long ms, const char* dev, const char* type, const char* code, const char* value) {
    char l[LINE_LEN];
    if (dev) {
        snprintf(l, sizeof(l), "[%8ld.%06ld] %s: %-12s %-20s %-20s", ms / 1000, (ms % 1000) * 1000, dev, type, code,
                 value);
    } else {
        snprintf(l, sizeof(l), "[%8ld.%06ld] %-12s %-20s %-20s", ms / 1000, (ms % 1000) * 1000, type, code, value);
    }
    st_add(s, l);
}

static void st_abs(Stream* s, long ms, const char* dev, const char* code, int v) {
    char val[16];
    snprintf(val, sizeof(val), "%08x", (unsigned)v);
    st_ev(s, ms, dev, "EV_ABS", code, val);
}

static void st_syn(Stream* s, long ms, const char* dev) {
    st_ev(s, ms, dev, "EV_SYN", "SYN_REPORT", "00000000");
}

static void st_down(Stream* s, long ms, const char* dev, int slot, int id, int rx, int ry) {
    st_abs(s, ms, dev, "ABS_MT_SLOT", slot);
    st_abs(s, ms, dev, "ABS_MT_TRACKING_ID", id);
    st_abs(s, ms, dev, "ABS_MT_POSITION_X", rx);
    st_abs(s, ms, dev, "ABS_MT_POSITION_Y", ry);
    st_abs(s, ms, dev, "ABS_MT_PRESSURE", 1024);
}

static void st_move(Stream* s, long ms, const char* dev, int slot, int rx, int ry) {
    st_abs(s, ms, dev, "ABS_MT_SLOT", slot);
    st_abs(s, ms, dev, "ABS_MT_POSITION_X", rx);
    st_abs(s, ms, dev, "ABS_MT_POSITION_Y", ry);
}

static void st_up(Stream* s, long ms, const char* dev, int slot) {
    st_abs(s, ms, dev, "ABS_MT_SLOT", slot);
    st_abs(s, ms, dev, "ABS_MT_PRESSURE", 0);
    st_abs(s, ms, dev, "ABS_MT_TRACKING_ID", -1);
}

// tap / press of one finger in slot 0: down, up after |hold| ms
static void st_press(Stream* s, long ms, const char* dev, int id, int rx, int ry, long hold) {
    st_down(s, ms, dev, 0, id, rx, ry);
    st_syn(s, ms, dev);
    st_up(s, ms + hold, dev, 0);
    st_syn(s, ms + hold, dev);
}

static void st_key(Stream* s, long ms, const char* code, const char* state) {
    st_ev(s, ms, KBD, "EV_KEY", code, state);
    st_syn(s, ms, KBD);
}

static void st_type(Stream* s, long ms, const char* code) {
    st_key(s, ms, code, "DOWN");
    st_key(s, ms + 5, code, "UP");
}

static void st_headers(Stream* s) {
    st_add(s, "add device 1: /dev/input/event12");
    st_add(s, "  name:     \"qwerty2\"");
    st_add(s, "add device 2: /dev/input/event11");
    st_add(s, "  name:     \"virtio_input_multi_touch_11\"");
    st_add(s, "add device 12: /dev/input/event1");
    st_add(s, "  name:     \"virtio_input_multi_touch_1\"");
    st_add(s, "add device 13: /dev/input/event0");
    st_add(s, "  name:     \"gpio-keys\"");
}

typedef struct {
    char*  text;
    size_t len;
    int    actions;
    int    errors;
} RecResult;

static RecResult run_lines(int w, int h, const char* const* lines, int n, bool prime) {
    RecResult res = { NULL, 0, 0, 0 };
    FILE* out = open_memstream(&res.text, &res.len);
    if (!out) exit(2);
    SoulXRecorder* r = soulx_recorder_new(out, w, h, test_rotation, NULL, false);
    CHECK(r != NULL);
    if (r) {
        if (prime) soulx_recorder_prime(r, 10, 32767, 32767);
        for (int i = 0; i < n; i++) {
            if (soulx_recorder_feed(r, lines[i]) != 0) res.errors++;
        }
        if (soulx_recorder_finish(r) != 0) res.errors++;
        res.actions = soulx_recorder_actions(r);
        soulx_recorder_free(r);
    }
    fclose(out);
    return res;
}

static RecResult run_stream(int w, int h, const Stream* s, bool prime) {
    static const char* ptrs[MAX_LINES];
    for (int i = 0; i < s->n; i++) ptrs[i] = s->line[i];
    return run_lines(w, h, ptrs, s->n, prime);
}

// Panel 1024 x 2048: raw = 32 * x and 16 * y map back to exactly x and y.
static int rx_(int x) {
    return x * 32;
}

static int ry_(int y) {
    return y * 16;
}

// ---------------------------------------------------------------------------
// Recorder: the recorded spike stream (verbatim `getevent -lt` output of the emulator)
// ---------------------------------------------------------------------------

static const char* const k_spike_taps[] = {
    "add device 1: /dev/input/event12",
    "  name:     \"qwerty2\"",
    "add device 2: /dev/input/event11",
    "  name:     \"virtio_input_multi_touch_11\"",
    "add device 11: /dev/input/event2",
    "  name:     \"virtio_input_multi_touch_2\"",
    "add device 12: /dev/input/event1",
    "  name:     \"virtio_input_multi_touch_1\"",
    "add device 13: /dev/input/event0",
    "  name:     \"gpio-keys\"",
    // window click at (540, 640): event mouse uses raw = px * 32767 / dim
    "[     302.729329] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   00000000            ",
    "[     302.729329] /dev/input/event1: EV_ABS       ABS_MT_POSITION_X    00003fff            ",
    "[     302.729329] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    00002221            ",
    "[     302.729329] /dev/input/event1: EV_ABS       ABS_MT_PRESSURE      00000400            ",
    "[     302.729329] /dev/input/event1: EV_ABS       ABS_MT_TOUCH_MAJOR   00000500            ",
    "[     302.729329] /dev/input/event1: EV_ABS       ABS_MT_TOUCH_MINOR   00000500            ",
    "[     302.729329] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     302.729651] /dev/input/event1: EV_ABS       ABS_MT_PRESSURE      00000000            ",
    "[     302.729651] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   ffffffff            ",
    "[     302.729651] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    // `event send` without pressure: a hover, not a touch
    "[     330.614627] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   00000001            ",
    "[     330.614627] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    0000391f            ",
    "[     330.614627] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     331.058831] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   ffffffff            ",
    "[     331.058831] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    // a touch whose position did not change: the kernel only reports id and pressure
    "[     355.327058] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   00000002            ",
    "[     355.327058] /dev/input/event1: EV_ABS       ABS_MT_PRESSURE      00000400            ",
    "[     355.327058] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     355.559985] /dev/input/event1: EV_ABS       ABS_MT_PRESSURE      00000000            ",
    "[     355.559985] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   ffffffff            ",
    "[     355.559985] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    // swipe up from y=2250 to y=800 (opened the app drawer)
    "[     400.258737] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   00000065            ",
    "[     400.258737] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    000077ff            ",
    "[     400.258737] /dev/input/event1: EV_ABS       ABS_MT_PRESSURE      00000400            ",
    "[     400.258737] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     400.280920] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    00007043            ",
    "[     400.280920] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     400.299749] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    00006887            ",
    "[     400.299749] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     400.320510] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    000060cc            ",
    "[     400.320510] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     400.343992] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    00005910            ",
    "[     400.343992] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     400.362448] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    00005154            ",
    "[     400.362448] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     400.383156] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    00004999            ",
    "[     400.383156] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     400.404180] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    000041dd            ",
    "[     400.404180] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     400.424786] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    00003a21            ",
    "[     400.424786] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     400.444339] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    00003266            ",
    "[     400.444339] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     400.461246] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    00002aaa            ",
    "[     400.461246] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     400.462129] /dev/input/event1: EV_ABS       ABS_MT_PRESSURE      00000000            ",
    "[     400.462129] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   ffffffff            ",
    "[     400.462129] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    // two taps 70 us apart
    "[     420.438115] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   00000000            ",
    "[     420.438115] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    00000fff            ",
    "[     420.438115] /dev/input/event1: EV_ABS       ABS_MT_PRESSURE      00000400            ",
    "[     420.438115] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     420.438184] /dev/input/event1: EV_ABS       ABS_MT_PRESSURE      00000000            ",
    "[     420.438184] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   ffffffff            ",
    "[     420.438184] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     420.438185] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   00000000            ",
    "[     420.438185] /dev/input/event1: EV_ABS       ABS_MT_PRESSURE      00000400            ",
    "[     420.438185] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[     420.438395] /dev/input/event1: EV_ABS       ABS_MT_PRESSURE      00000000            ",
    "[     420.438395] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   ffffffff            ",
    "[     420.438395] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
};

static const char* const k_spike_two_fingers[] = {
    "add device 12: /dev/input/event1",
    "  name:     \"virtio_input_multi_touch_1\"",
    "[    1054.517816] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   00000015            ",
    "[    1054.517816] /dev/input/event1: EV_ABS       ABS_MT_POSITION_X    00002f68            ",
    "[    1054.517816] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    00004000            ",
    "[    1054.517816] /dev/input/event1: EV_ABS       ABS_MT_PRESSURE      00000400            ",
    "[    1054.517816] /dev/input/event1: EV_ABS       ABS_MT_SLOT          00000001            ",
    "[    1054.517816] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   00000016            ",
    "[    1054.517816] /dev/input/event1: EV_ABS       ABS_MT_POSITION_X    000052f6            ",
    "[    1054.517816] /dev/input/event1: EV_ABS       ABS_MT_POSITION_Y    00004000            ",
    "[    1054.517816] /dev/input/event1: EV_ABS       ABS_MT_PRESSURE      00000400            ",
    "[    1054.517816] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
    "[    1054.572652] /dev/input/event1: EV_ABS       ABS_MT_SLOT          00000000            ",
    "[    1054.572652] /dev/input/event1: EV_ABS       ABS_MT_PRESSURE      00000000            ",
    "[    1054.572652] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   ffffffff            ",
    "[    1054.572652] /dev/input/event1: EV_ABS       ABS_MT_SLOT          00000001            ",
    "[    1054.572652] /dev/input/event1: EV_ABS       ABS_MT_PRESSURE      00000000            ",
    "[    1054.572652] /dev/input/event1: EV_ABS       ABS_MT_TRACKING_ID   ffffffff            ",
    "[    1054.572652] /dev/input/event1: EV_SYN       SYN_REPORT           00000000            ",
};

static const char* const k_spike_text[] = {
    "add device 1: /dev/input/event12",
    "  name:     \"qwerty2\"",
    "[    1056.444318] /dev/input/event12: EV_REP       REP_DELAY            00000000            ",
    "[    1056.444318] /dev/input/event12: EV_REP       REP_PERIOD           00000000            ",
    "[    1056.444318] /dev/input/event12: EV_KEY       KEY_H                DOWN                ",
    "[    1056.444318] /dev/input/event12: EV_SYN       SYN_REPORT           00000000            ",
    "[    1056.444520] /dev/input/event12: EV_KEY       KEY_H                UP                  ",
    "[    1056.444520] /dev/input/event12: EV_SYN       SYN_REPORT           00000000            ",
    "[    1056.444522] /dev/input/event12: EV_KEY       KEY_I                DOWN                ",
    "[    1056.444522] /dev/input/event12: EV_SYN       SYN_REPORT           00000000            ",
    "[    1056.444523] /dev/input/event12: EV_KEY       KEY_I                UP                  ",
    "[    1056.444523] /dev/input/event12: EV_SYN       SYN_REPORT           00000000            ",
};

#define NLINES(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void test_recorder_spike(void) {
    set_rotation(0);
    RecResult res = run_lines(1080, 2400, k_spike_taps, NLINES(k_spike_taps), false);
    CHECK_STR(res.text,
              "tap(540, 640)\n"
              "sleep(52550)\n"
              "tap(540, 1071)\n"
              "sleep(44890)\n"
              "swipe(540, 2250, 540, 800, 200)\n"
              "sleep(19980)\n"
              "tap(540, 300)\n"
              "tap(540, 300)\n");
    CHECK(res.actions == 5 && res.errors == 0);
    CHECK(g_rot_calls == 5);                    // rotation queried once per gesture
    free(res.text);

    set_rotation(0);
    res = run_lines(1080, 2400, k_spike_two_fingers, NLINES(k_spike_two_fingers), false);
    CHECK_STR(res.text,
              "touch_down(400, 1200, 0)\n"
              "touch_down(700, 1200, 1)\n"
              "sleep(50)\n"
              "touch_up(0)\n"
              "touch_up(1)\n");
    CHECK(res.actions == 1 && res.errors == 0);
    free(res.text);

    set_rotation(0);
    res = run_lines(1080, 2400, k_spike_text, NLINES(k_spike_text), false);
    CHECK_STR(res.text, "text(\"hi\")\n");
    CHECK(res.actions == 1);
    free(res.text);
}

// ---------------------------------------------------------------------------
// Recorder: gesture classification
// ---------------------------------------------------------------------------

static void test_recorder_gestures(void) {
    Stream* s = &g_stream;
    st_reset(s);
    st_headers(s);
    // tap with a little jitter
    st_down(s, 1000, TOUCH, 0, 1, rx_(500), ry_(500));
    st_syn(s, 1000, TOUCH);
    st_move(s, 1050, TOUCH, 0, rx_(505), ry_(503));
    st_syn(s, 1050, TOUCH);
    st_up(s, 1100, TOUCH, 0);
    st_syn(s, 1100, TOUCH);
    // long press, 800 ms
    st_press(s, 2000, TOUCH, 2, rx_(300), ry_(600), 800);
    // still for 400 ms: neither a tap nor >= 500 ms; replayed with its real duration
    st_press(s, 4000, TOUCH, 3, rx_(100), ry_(100), 400);
    // swipe up in 10 frames
    st_down(s, 5000, TOUCH, 0, 4, rx_(500), ry_(1800));
    st_syn(s, 5000, TOUCH);
    for (int i = 1; i <= 10; i++) {
        st_move(s, 5000 + 20 * i, TOUCH, 0, rx_(500), ry_(1800 - 120 * i));
        st_syn(s, 5000 + 20 * i, TOUCH);
    }
    st_up(s, 5210, TOUCH, 0);
    st_syn(s, 5210, TOUCH);
    // moved exactly 20 px: a swipe
    st_down(s, 6000, TOUCH, 0, 5, rx_(200), ry_(200));
    st_syn(s, 6000, TOUCH);
    st_move(s, 6010, TOUCH, 0, rx_(220), ry_(200));
    st_syn(s, 6010, TOUCH);
    st_up(s, 6050, TOUCH, 0);
    st_syn(s, 6050, TOUCH);
    // moved 19 px: still a tap
    st_down(s, 7000, TOUCH, 0, 6, rx_(200), ry_(200));
    st_syn(s, 7000, TOUCH);
    st_move(s, 7010, TOUCH, 0, rx_(219), ry_(200));
    st_syn(s, 7010, TOUCH);
    st_up(s, 7050, TOUCH, 0);
    st_syn(s, 7050, TOUCH);
    // away and back: the largest distance decides, not start -> end
    st_down(s, 8000, TOUCH, 0, 7, rx_(400), ry_(400));
    st_syn(s, 8000, TOUCH);
    st_move(s, 8100, TOUCH, 0, rx_(500), ry_(400));
    st_syn(s, 8100, TOUCH);
    st_move(s, 8190, TOUCH, 0, rx_(401), ry_(400));
    st_syn(s, 8190, TOUCH);
    st_up(s, 8200, TOUCH, 0);
    st_syn(s, 8200, TOUCH);

    set_rotation(0);
    RecResult res = run_stream(1024, 2048, s, false);
    CHECK_STR(res.text,
              "tap(500, 500)\n"
              "sleep(960)\n"
              "long_press(300, 600, 800)\n"
              "sleep(1190)\n"
              "long_press(100, 100, 400)\n"
              "sleep(600)\n"
              "swipe(500, 1800, 500, 600, 210)\n"
              "sleep(790)\n"
              "swipe(200, 200, 220, 200, 50)\n"
              "sleep(940)\n"
              "tap(200, 200)\n"
              "sleep(960)\n"
              "swipe(400, 400, 401, 400, 200)\n");
    CHECK(res.actions == 7 && res.errors == 0);
    CHECK(g_rot_calls == 7);
    free(res.text);

    // the same swipe recorded in landscape (R=1): logical = (ny, W - nx)
    st_reset(s);
    st_headers(s);
    st_down(s, 1000, TOUCH, 0, 1, rx_(500), ry_(1800));
    st_syn(s, 1000, TOUCH);
    st_move(s, 1100, TOUCH, 0, rx_(500), ry_(600));
    st_syn(s, 1100, TOUCH);
    st_up(s, 1150, TOUCH, 0);
    st_syn(s, 1150, TOUCH);
    set_rotation(1);
    res = run_stream(1024, 2048, s, false);
    CHECK_STR(res.text, "swipe(1800, 524, 600, 524, 150)\n");
    free(res.text);
}

// Raw positions verified on the emulator must give back the logical point in every rotation.
static void test_recorder_rotation(void) {
    const struct {
        int rot, rx, ry;
        const char* want;
    } cases[] = {
        { 1, 5552, 30037, "tap(2200, 897)\n" },
        { 3, 27215, 2730, "tap(2200, 897)\n" },
        { 2, 5461, 20520, "tap(900, 897)\n" },
        { 0, 16384, 8738, "tap(540, 640)\n" },
    };
    Stream* s = &g_stream;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        st_reset(s);
        st_headers(s);
        st_press(s, 1000, TOUCH, 9, cases[i].rx, cases[i].ry, 60);
        set_rotation(cases[i].rot);
        RecResult res = run_stream(1080, 2400, s, false);
        CHECK_STR(res.text, cases[i].want);
        free(res.text);
    }

    // the rotation is read at the start of every gesture
    st_reset(s);
    st_headers(s);
    st_press(s, 1000, TOUCH, 1, 5552, 30037, 60);
    st_press(s, 2000, TOUCH, 2, 5552, 30037, 60);
    set_rotation(0);
    g_rot_seq[0] = 1;
    g_rot_seq[1] = 0;
    g_rot_seq_n = 2;
    RecResult res = run_stream(1080, 2400, s, false);
    CHECK_STR(res.text, "tap(2200, 897)\nsleep(960)\ntap(183, 2200)\n");
    free(res.text);
    set_rotation(0);
}

static void test_recorder_multitouch(void) {
    Stream* s = &g_stream;
    // two fingers moving towards each other, one frame every 10 ms
    st_reset(s);
    st_headers(s);
    st_down(s, 1000, TOUCH, 0, 1, rx_(100), ry_(1000));
    st_down(s, 1000, TOUCH, 1, 2, rx_(300), ry_(1000));
    st_syn(s, 1000, TOUCH);
    for (int i = 1; i <= 10; i++) {
        st_move(s, 1000 + 10 * i, TOUCH, 0, rx_(100 + 10 * i), ry_(1000));
        st_move(s, 1000 + 10 * i, TOUCH, 1, rx_(300 - 10 * i), ry_(1000));
        st_syn(s, 1000 + 10 * i, TOUCH);
    }
    st_up(s, 1110, TOUCH, 0);
    st_up(s, 1110, TOUCH, 1);
    st_syn(s, 1110, TOUCH);
    set_rotation(0);
    RecResult res = run_stream(1024, 2048, s, false);
    CHECK_STR(res.text,
              "touch_down(100, 1000, 0)\n"
              "touch_down(300, 1000, 1)\n"
              "sleep(30)\n"
              "touch_move(130, 1000, 0)\n"
              "touch_move(270, 1000, 1)\n"
              "sleep(30)\n"
              "touch_move(160, 1000, 0)\n"
              "touch_move(240, 1000, 1)\n"
              "sleep(30)\n"
              "touch_move(190, 1000, 0)\n"
              "touch_move(210, 1000, 1)\n"
              "touch_move(200, 1000, 0)\n"
              "touch_up(0)\n"
              "touch_move(200, 1000, 1)\n"
              "touch_up(1)\n");
    CHECK(res.actions == 1 && res.errors == 0);
    CHECK(g_rot_calls == 1);
    free(res.text);

    // a second finger joins while the first one moves; the slots are not the finger numbers
    st_reset(s);
    st_headers(s);
    st_down(s, 10000, TOUCH, 3, 7, rx_(100), ry_(100));
    st_syn(s, 10000, TOUCH);
    st_move(s, 10040, TOUCH, 3, rx_(150), ry_(100));
    st_syn(s, 10040, TOUCH);
    st_down(s, 10050, TOUCH, 5, 8, rx_(300), ry_(300));
    st_syn(s, 10050, TOUCH);
    st_up(s, 10100, TOUCH, 3);
    st_up(s, 10100, TOUCH, 5);
    st_syn(s, 10100, TOUCH);
    set_rotation(0);
    res = run_stream(1024, 2048, s, false);
    CHECK_STR(res.text,
              "touch_down(100, 100, 0)\n"
              "sleep(40)\n"
              "touch_move(150, 100, 0)\n"
              "touch_down(300, 300, 1)\n"
              "sleep(60)\n"
              "touch_up(0)\n"
              "touch_up(1)\n");
    free(res.text);
}

// ---------------------------------------------------------------------------
// Recorder: keys and text
// ---------------------------------------------------------------------------

static void test_recorder_keys(void) {
    Stream* s = &g_stream;
    st_reset(s);
    st_headers(s);
    st_key(s, 1000, "KEY_LEFTSHIFT", "DOWN");
    st_type(s, 1010, "KEY_H");
    st_key(s, 1030, "KEY_LEFTSHIFT", "UP");
    st_key(s, 1100, "KEY_I", "DOWN");
    st_key(s, 1150, "KEY_I", "REPEAT");                 // auto-repeat is ignored
    st_key(s, 1160, "KEY_I", "UP");
    st_key(s, 1200, "KEY_RIGHTSHIFT", "DOWN");
    st_type(s, 1210, "KEY_1");                          // shift+1 = '!'
    st_key(s, 1230, "KEY_RIGHTSHIFT", "UP");
    st_type(s, 4000, "KEY_A");                          // more than 1.5 s later: a new text()
    st_type(s, 4100, "KEY_SPACE");
    st_type(s, 4200, "KEY_B");
    st_type(s, 4300, "KEY_BACKSPACE");                  // a typo fixed while typing
    st_type(s, 4400, "KEY_C");
    st_type(s, 4500, "KEY_ENTER");
    st_type(s, 5000, "KEY_BACK");
    st_type(s, 5100, "KEY_HOMEPAGE");
    st_type(s, 5200, "KEY_HOME");
    st_type(s, 5300, "KEY_APPSELECT");
    st_type(s, 5400, "KEY_F5");
    st_type(s, 5500, "KEY_VOLUMEUP");
    st_type(s, 5600, "KEY_UP");
    st_type(s, 5700, "KEY_BACKSPACE");                  // nothing typed to fix: the DEL key
    st_ev(s, 5800, TOUCH, "EV_KEY", "BTN_TOUCH", "DOWN");   // not a key
    st_type(s, 5900, "KEY_LEFTCTRL");                   // modifiers alone are not recorded
    st_key(s, 5990, "KEY_LEFTSHIFT", "DOWN");
    st_type(s, 6000, "KEY_APOSTROPHE");                 // '"'
    st_key(s, 6005, "KEY_LEFTSHIFT", "UP");
    st_type(s, 6010, "KEY_BACKSLASH");                  // '\'
    set_rotation(0);
    RecResult res = run_stream(1080, 2400, s, false);
    CHECK_STR(res.text,
              "text(\"Hi!\")\n"
              "sleep(2960)\n"
              "text(\"a c\")\n"
              "sleep(460)\n"
              "key(\"ENTER\")\n"
              "sleep(460)\n"
              "key(\"BACK\")\n"
              "sleep(60)\n"
              "key(\"HOME\")\n"
              "sleep(60)\n"
              "key(\"MOVE_HOME\")\n"
              "sleep(60)\n"
              "key(\"APP_SWITCH\")\n"
              "sleep(60)\n"
              "key(\"F5\")\n"
              "sleep(60)\n"
              "key(\"VOLUME_UP\")\n"
              "sleep(60)\n"
              "key(\"DPAD_UP\")\n"
              "sleep(60)\n"
              "key(\"DEL\")\n"
              "sleep(260)\n"
              "text(\"\\\"\\\\\")\n");
    CHECK(res.actions == 12 && res.errors == 0);
    CHECK(g_rot_calls == 0);
    free(res.text);

    // text typed before a touch is written before it
    st_reset(s);
    st_headers(s);
    st_type(s, 100, "KEY_A");
    st_type(s, 110, "KEY_B");
    st_press(s, 200, TOUCH, 1, 16384, 16384, 50);
    set_rotation(0);
    res = run_stream(1080, 2400, s, false);
    CHECK_STR(res.text, "text(\"ab\")\nsleep(70)\ntap(540, 1200)\n");
    free(res.text);

    // a pending text() is written once nothing was typed for 1.5 s (host clock)
    char* buf = NULL;
    size_t len = 0;
    FILE* out = open_memstream(&buf, &len);
    if (!out) exit(2);
    SoulXRecorder* r = soulx_recorder_new(out, 1080, 2400, test_rotation, NULL, false);
    CHECK(r != NULL);
    if (r) {
        st_reset(s);
        st_type(s, 100, "KEY_X");
        for (int i = 0; i < s->n; i++) CHECK(soulx_recorder_feed(r, s->line[i]) == 0);
        CHECK(soulx_recorder_idle(r, soulx_now_ms()) == 0);
        fflush(out);
        CHECK(len == 0);                                // still typing
        CHECK(soulx_recorder_idle(r, soulx_now_ms() + 2000) == 0);
        fflush(out);
        CHECK(buf && strcmp(buf, "text(\"x\")\n") == 0);
        CHECK(soulx_recorder_finish(r) == 0);
        CHECK(soulx_recorder_actions(r) == 1);
        soulx_recorder_free(r);
    }
    fclose(out);
    free(buf);
}

// ---------------------------------------------------------------------------
// Recorder: limits of the script API (swipe/long_press 1..60000 ms, sleep <= 24 h, text <= 1000 chars)
// ---------------------------------------------------------------------------

static void test_recorder_limits(void) {
    Stream* s = &g_stream;

    // a 3 ms flick still gets a valid duration
    st_reset(s);
    st_headers(s);
    st_down(s, 1000, TOUCH, 0, 1, rx_(500), ry_(1500));
    st_syn(s, 1000, TOUCH);
    st_move(s, 1002, TOUCH, 0, rx_(500), ry_(900));
    st_syn(s, 1002, TOUCH);
    st_up(s, 1003, TOUCH, 0);
    st_syn(s, 1003, TOUCH);
    set_rotation(0);
    RecResult res = run_stream(1024, 2048, s, false);
    CHECK_STR(res.text, "swipe(500, 1500, 500, 900, 10)\n");
    free(res.text);

    // one finger held for 61 s: longer than long_press() accepts -> touch_* lines
    st_reset(s);
    st_headers(s);
    st_press(s, 1000, TOUCH, 1, rx_(300), ry_(400), 61000);
    res = run_stream(1024, 2048, s, false);
    CHECK_STR(res.text, "touch_down(300, 400, 0)\nsleep(61000)\ntouch_up(0)\n");
    CHECK(res.actions == 1);
    free(res.text);

    // a 61 s drag: touch_* lines with the moves sampled
    st_reset(s);
    st_headers(s);
    st_down(s, 1000, TOUCH, 0, 1, rx_(100), ry_(100));
    st_syn(s, 1000, TOUCH);
    st_move(s, 31000, TOUCH, 0, rx_(200), ry_(100));
    st_syn(s, 31000, TOUCH);
    st_up(s, 62000, TOUCH, 0);
    st_syn(s, 62000, TOUCH);
    res = run_stream(1024, 2048, s, false);
    CHECK_STR(res.text, "touch_down(100, 100, 0)\nsleep(30000)\ntouch_move(200, 100, 0)\nsleep(31000)\ntouch_up(0)\n");
    free(res.text);

    // a pause of 25 h: sleep() takes at most 24 h at a time
    st_reset(s);
    st_headers(s);
    st_press(s, 1000, TOUCH, 1, rx_(10), ry_(10), 50);
    st_press(s, 1050L + 90000000L, TOUCH, 2, rx_(10), ry_(10), 50);
    res = run_stream(1024, 2048, s, false);
    CHECK_STR(res.text, "tap(10, 10)\nsleep(86400000)\nsleep(3600010)\ntap(10, 10)\n");
    free(res.text);

    // 501 characters typed in a row: text() calls of at most 500 characters
    st_reset(s);
    st_headers(s);
    char want[1200];
    size_t n = 0;
    n += (size_t)snprintf(want + n, sizeof(want) - n, "text(\"");
    for (int i = 0; i < 501; i++) {
        char code[8] = "KEY_A";
        code[4] = (char)('A' + i % 26);
        st_key(s, 1000 + i, code, "DOWN");
        if (i == 500) n += (size_t)snprintf(want + n, sizeof(want) - n, "\")\ntext(\"");
        want[n++] = (char)('a' + i % 26);
    }
    snprintf(want + n, sizeof(want) - n, "\")\n");
    res = run_stream(1024, 2048, s, false);
    CHECK_STR(res.text, want);
    CHECK(res.actions == 2);
    free(res.text);
}

// ---------------------------------------------------------------------------
// Recorder: devices, unknown positions, priming
// ---------------------------------------------------------------------------

static void test_recorder_devices(void) {
    Stream* s = &g_stream;

    // the secondary-display touchscreens (multi_touch_2..11) are not the screen
    st_reset(s);
    st_headers(s);
    st_press(s, 1000, TOUCH_2ND, 1, 16384, 16384, 50);
    st_press(s, 2000, TOUCH, 2, 8192, 8192, 50);
    set_rotation(0);
    RecResult res = run_stream(1080, 2400, s, false);
    CHECK_STR(res.text, "tap(270, 600)\n");
    free(res.text);

    // another image: the first device named "...touch..." that sends multi-touch events
    st_reset(s);
    st_add(s, "add device 1: /dev/input/event4");
    st_add(s, "  name:     \"gpio-keys\"");
    st_add(s, "add device 2: /dev/input/event3");
    st_add(s, "  name:     \"Some TouchScreen\"");
    st_press(s, 1000, "/dev/input/event4", 1, 16384, 16384, 50);   // not a touch device: ignored
    st_press(s, 2000, "/dev/input/event3", 2, 16384, 16384, 50);
    set_rotation(0);
    res = run_stream(1080, 2400, s, false);
    CHECK_STR(res.text, "tap(540, 1200)\n");
    free(res.text);

    // no device name says "touch": the first one that sends multi-touch events
    st_reset(s);
    st_add(s, "add device 1: /dev/input/event7");
    st_add(s, "  name:     \"virtio_input_panel\"");
    st_press(s, 1000, "/dev/input/event7", 1, 16384, 16384, 50);
    res = run_stream(1080, 2400, s, false);
    CHECK_STR(res.text, "tap(540, 1200)\n");
    free(res.text);

    // single-device form
    st_reset(s);
    st_press(s, 1000, NULL, 1, 16384, 16384, 50);
    res = run_stream(1080, 2400, s, false);
    CHECK_STR(res.text, "tap(540, 1200)\n");
    free(res.text);

    // the touchscreen never reported this slot's position: not recorded, and said so
    st_reset(s);
    st_headers(s);
    st_abs(s, 1000, TOUCH, "ABS_MT_TRACKING_ID", 5);
    st_abs(s, 1000, TOUCH, "ABS_MT_PRESSURE", 1024);
    st_syn(s, 1000, TOUCH);
    st_abs(s, 1060, TOUCH, "ABS_MT_PRESSURE", 0);
    st_abs(s, 1060, TOUCH, "ABS_MT_TRACKING_ID", -1);
    st_syn(s, 1060, TOUCH);
    res = run_stream(1080, 2400, s, false);
    CHECK(res.actions == 0);
    CHECK(res.text && strncmp(res.text, "-- ", 3) == 0 && !strstr(res.text, "tap("));
    free(res.text);

    // after priming every slot position is known
    res = run_stream(1080, 2400, s, true);
    CHECK_STR(res.text, "tap(1079, 2399)\n");
    CHECK(res.actions == 1);
    free(res.text);

    // pressure 0 is a hover, not a touch
    st_reset(s);
    st_headers(s);
    st_abs(s, 1000, TOUCH, "ABS_MT_TRACKING_ID", 5);
    st_abs(s, 1000, TOUCH, "ABS_MT_POSITION_X", 100);
    st_abs(s, 1000, TOUCH, "ABS_MT_POSITION_Y", 100);
    st_abs(s, 1000, TOUCH, "ABS_MT_PRESSURE", 0);
    st_syn(s, 1000, TOUCH);
    st_abs(s, 1060, TOUCH, "ABS_MT_TRACKING_ID", -1);
    st_syn(s, 1060, TOUCH);
    res = run_stream(1080, 2400, s, false);
    CHECK(res.actions == 0 && res.len == 0);
    free(res.text);

    // a touch still going on at the end is dropped, not half-written
    st_reset(s);
    st_headers(s);
    st_down(s, 1000, TOUCH, 0, 1, 16384, 16384);
    st_syn(s, 1000, TOUCH);
    res = run_stream(1080, 2400, s, false);
    CHECK(res.actions == 0 && res.len == 0);
    free(res.text);

    // invalid recorder arguments
    char* buf = NULL;
    size_t len = 0;
    FILE* out = open_memstream(&buf, &len);
    if (!out) exit(2);
    CHECK(soulx_recorder_new(NULL, 1080, 2400, NULL, NULL, false) == NULL);
    CHECK(soulx_recorder_new(out, 0, 2400, NULL, NULL, false) == NULL);
    CHECK(soulx_recorder_new(out, 1080, -1, NULL, NULL, false) == NULL);
    CHECK(soulx_recorder_feed(NULL, "x") == -1);
    CHECK(soulx_recorder_finish(NULL) == -1);
    CHECK(soulx_recorder_actions(NULL) == 0);
    soulx_recorder_free(NULL);
    fclose(out);
    free(buf);
}

int main(void) {
    test_mapping();
    test_rotate_image();
    test_parse_rotation();
    test_keycodes();
    test_text_chunking();
    test_text_commands();
    test_getevent_parse();
    test_recorder_spike();
    test_recorder_gestures();
    test_recorder_rotation();
    test_recorder_multitouch();
    test_recorder_keys();
    test_recorder_limits();
    test_recorder_devices();

    if (g_failures) {
        fprintf(stderr, "test_input: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_input: all %d checks passed\n", g_checks);
    return 0;
}
