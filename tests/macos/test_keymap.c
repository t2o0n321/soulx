/*
 * SoulX Android Emulator - unit tests for source/platform/macos/window/keymap.c
 *
 * Key names, kVK_* virtual key codes (checked against Carbon's own constants; only the header is used, nothing is
 * linked) and HID usages; keymap parse / format round trips and line-numbered errors; load / save under a
 * temporary SOULX_HOME; the runtime: tap hold, joystick in 8 directions + release, swipe, key repeat, shared keys,
 * release_all and the finger limits.
 */

#include <Carbon/Carbon.h>

#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "soulx_mac.h"
#include "window/keymap.h"

// keymap.c (exported there, not in keymap.h yet)
void soulx_keymap_runtime_set_screen(SoulXKeymapRuntime* rt, int width, int height);

static int g_checks = 0;
static int g_failures = 0;
static char g_home[PATH_MAX];

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

#define ACT_MAX 16

static int key(const char* name) {
    int k = soulx_key_from_name(name);
    if (k < 0) {
        fprintf(stderr, "FAIL: test key '%s' unknown\n", name);
        g_failures++;
    }
    return k;
}

static bool near(double a, double b) {
    return fabs(a - b) < 1e-9;
}

// ---------------------------------------------------------------------------
// Key table
// ---------------------------------------------------------------------------

static const char* const k_header_names[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U",
    "V", "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "F1", "F2", "F3", "F4", "F5",
    "F6", "F7", "F8", "F9", "F10", "F11", "F12", "SPACE", "ENTER", "TAB", "ESC", "BACKSPACE", "UP", "DOWN",
    "LEFT", "RIGHT", "LSHIFT", "RSHIFT", "LCTRL", "RCTRL", "LALT", "RALT", "MINUS", "EQUAL", "LBRACKET",
    "RBRACKET", "BACKSLASH", "SEMICOLON", "QUOTE", "COMMA", "PERIOD", "SLASH", "GRAVE",
};

static int key_count(void) {
    int n = 0;
    while (soulx_key_name(n + 1)) n++;
    return n;
}

static void lower(const char* in, char* out, size_t cap) {
    size_t i = 0;
    for (; in[i] && i + 1 < cap; i++) out[i] = (in[i] >= 'A' && in[i] <= 'Z') ? (char)(in[i] - 'A' + 'a') : in[i];
    out[i] = '\0';
}

static void test_key_names(void) {
    for (size_t i = 0; i < sizeof(k_header_names) / sizeof(k_header_names[0]); i++) {
        int k = soulx_key_from_name(k_header_names[i]);
        CHECK(k > 0);
        if (k > 0) CHECK(strcmp(soulx_key_name(k), k_header_names[i]) == 0);
        char lc[32];
        lower(k_header_names[i], lc, sizeof(lc));
        CHECK(soulx_key_from_name(lc) == k);
    }
    int n = key_count();
    CHECK(n >= 74);
    for (int k = 1; k <= n; k++) {
        const char* name = soulx_key_name(k);
        CHECK(name != NULL && name[0] != '\0');
        if (!name) continue;
        CHECK(soulx_key_from_name(name) == k);
        for (const char* p = name; *p; p++) CHECK((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'));
        for (int j = 1; j < k; j++) CHECK(strcmp(soulx_key_name(j), name) != 0);
    }
    CHECK(soulx_key_name(0) == NULL);
    CHECK(soulx_key_name(-1) == NULL);
    CHECK(soulx_key_name(n + 1) == NULL);
    CHECK(soulx_key_name(INT_MAX) == NULL);
    CHECK(soulx_key_name(INT_MIN) == NULL);

    const char* const bad[] = { "", "SPCE", "F0", "F21", "F120", "KP10", " A", "A ", "AA", "ESC\n", "A\tB", "+",
                                "ABCDEFGHIJKLMNOPQR", "SPACESPACESPACESPACE", "\xC3\xA9", "#", "0x04" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(soulx_key_from_name(bad[i]) == -1);
    CHECK(soulx_key_from_name(NULL) == -1);

    // aliases parse, but the canonical name is what is written
    CHECK(soulx_key_from_name("escape") == key("ESC"));
    CHECK(soulx_key_from_name("Return") == key("ENTER"));
    CHECK(soulx_key_from_name("lcmd") == key("LMETA"));
    CHECK(soulx_key_from_name("RCMD") == key("RMETA"));
    CHECK(soulx_key_from_name("LOption") == key("LALT"));
    CHECK(soulx_key_from_name("roption") == key("RALT"));
    CHECK(strcmp(soulx_key_name(soulx_key_from_name("escape")), "ESC") == 0);
    // extensions
    const char* const ext[] = { "DELETE", "HOME", "END", "PAGEUP", "PAGEDOWN", "INSERT", "CAPSLOCK", "LMETA",
                                "RMETA", "MENU", "SECTION", "F13", "F20", "NUMLOCK", "KP0", "KP9", "KPDOT",
                                "KPENTER", "KPPLUS", "KPMINUS", "KPMULTIPLY", "KPDIVIDE", "KPEQUAL" };
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) CHECK(soulx_key_from_name(ext[i]) > 0);
}

static void test_mac_keycodes(void) {
    static const struct { int vk; const char* name; } map[] = {
        { kVK_ANSI_A, "A" }, { kVK_ANSI_B, "B" }, { kVK_ANSI_C, "C" }, { kVK_ANSI_D, "D" }, { kVK_ANSI_E, "E" },
        { kVK_ANSI_F, "F" }, { kVK_ANSI_G, "G" }, { kVK_ANSI_H, "H" }, { kVK_ANSI_I, "I" }, { kVK_ANSI_J, "J" },
        { kVK_ANSI_K, "K" }, { kVK_ANSI_L, "L" }, { kVK_ANSI_M, "M" }, { kVK_ANSI_N, "N" }, { kVK_ANSI_O, "O" },
        { kVK_ANSI_P, "P" }, { kVK_ANSI_Q, "Q" }, { kVK_ANSI_R, "R" }, { kVK_ANSI_S, "S" }, { kVK_ANSI_T, "T" },
        { kVK_ANSI_U, "U" }, { kVK_ANSI_V, "V" }, { kVK_ANSI_W, "W" }, { kVK_ANSI_X, "X" }, { kVK_ANSI_Y, "Y" },
        { kVK_ANSI_Z, "Z" },
        { kVK_ANSI_0, "0" }, { kVK_ANSI_1, "1" }, { kVK_ANSI_2, "2" }, { kVK_ANSI_3, "3" }, { kVK_ANSI_4, "4" },
        { kVK_ANSI_5, "5" }, { kVK_ANSI_6, "6" }, { kVK_ANSI_7, "7" }, { kVK_ANSI_8, "8" }, { kVK_ANSI_9, "9" },
        { kVK_F1, "F1" }, { kVK_F2, "F2" }, { kVK_F3, "F3" }, { kVK_F4, "F4" }, { kVK_F5, "F5" }, { kVK_F6, "F6" },
        { kVK_F7, "F7" }, { kVK_F8, "F8" }, { kVK_F9, "F9" }, { kVK_F10, "F10" }, { kVK_F11, "F11" },
        { kVK_F12, "F12" }, { kVK_F13, "F13" }, { kVK_F14, "F14" }, { kVK_F15, "F15" }, { kVK_F16, "F16" },
        { kVK_F17, "F17" }, { kVK_F18, "F18" }, { kVK_F19, "F19" }, { kVK_F20, "F20" },
        { kVK_Space, "SPACE" }, { kVK_Return, "ENTER" }, { kVK_Tab, "TAB" }, { kVK_Escape, "ESC" },
        { kVK_Delete, "BACKSPACE" }, { kVK_ForwardDelete, "DELETE" },
        { kVK_UpArrow, "UP" }, { kVK_DownArrow, "DOWN" }, { kVK_LeftArrow, "LEFT" }, { kVK_RightArrow, "RIGHT" },
        { kVK_Home, "HOME" }, { kVK_End, "END" }, { kVK_PageUp, "PAGEUP" }, { kVK_PageDown, "PAGEDOWN" },
        { kVK_Help, "INSERT" },
        { kVK_Shift, "LSHIFT" }, { kVK_RightShift, "RSHIFT" }, { kVK_Control, "LCTRL" },
        { kVK_RightControl, "RCTRL" }, { kVK_Option, "LALT" }, { kVK_RightOption, "RALT" },
        { kVK_Command, "LMETA" }, { kVK_RightCommand, "RMETA" }, { kVK_CapsLock, "CAPSLOCK" },
        { kVK_ANSI_Minus, "MINUS" }, { kVK_ANSI_Equal, "EQUAL" }, { kVK_ANSI_LeftBracket, "LBRACKET" },
        { kVK_ANSI_RightBracket, "RBRACKET" }, { kVK_ANSI_Backslash, "BACKSLASH" },
        { kVK_ANSI_Semicolon, "SEMICOLON" }, { kVK_ANSI_Quote, "QUOTE" }, { kVK_ANSI_Comma, "COMMA" },
        { kVK_ANSI_Period, "PERIOD" }, { kVK_ANSI_Slash, "SLASH" }, { kVK_ANSI_Grave, "GRAVE" },
        { kVK_ISO_Section, "SECTION" }, { kVK_ContextualMenu, "MENU" },
        { kVK_ANSI_Keypad0, "KP0" }, { kVK_ANSI_Keypad1, "KP1" }, { kVK_ANSI_Keypad2, "KP2" },
        { kVK_ANSI_Keypad3, "KP3" }, { kVK_ANSI_Keypad4, "KP4" }, { kVK_ANSI_Keypad5, "KP5" },
        { kVK_ANSI_Keypad6, "KP6" }, { kVK_ANSI_Keypad7, "KP7" }, { kVK_ANSI_Keypad8, "KP8" },
        { kVK_ANSI_Keypad9, "KP9" }, { kVK_ANSI_KeypadDecimal, "KPDOT" }, { kVK_ANSI_KeypadEnter, "KPENTER" },
        { kVK_ANSI_KeypadPlus, "KPPLUS" }, { kVK_ANSI_KeypadMinus, "KPMINUS" },
        { kVK_ANSI_KeypadMultiply, "KPMULTIPLY" }, { kVK_ANSI_KeypadDivide, "KPDIVIDE" },
        { kVK_ANSI_KeypadEquals, "KPEQUAL" }, { kVK_ANSI_KeypadClear, "NUMLOCK" },
    };
    int mapped = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        int k = soulx_key_from_mac_keycode((unsigned short)map[i].vk);
        CHECK(k > 0 && k == soulx_key_from_name(map[i].name));
        if (k <= 0 || k != soulx_key_from_name(map[i].name)) {
            fprintf(stderr, "  kVK 0x%02X -> %d, expected %s\n", map[i].vk, k, map[i].name);
        }
    }
    // keys without a sensible Android key, and unused codes
    const int unmapped[] = { kVK_Function, kVK_VolumeUp, kVK_VolumeDown, kVK_Mute, kVK_JIS_Yen, kVK_JIS_Underscore,
                             kVK_JIS_KeypadComma, kVK_JIS_Eisu, kVK_JIS_Kana, 0x34, 0x42, 0x44, 0x46, 0x4D, 0x6C,
                             0x70, 0x7F, 0x80, 0xFF, 0xFFFF };
    for (size_t i = 0; i < sizeof(unmapped) / sizeof(unmapped[0]); i++) {
        CHECK(soulx_key_from_mac_keycode((unsigned short)unmapped[i]) == -1);
    }
    // the mapping is one to one
    int seen[256] = { 0 };
    for (int vk = 0; vk < 0x10000; vk++) {
        int k = soulx_key_from_mac_keycode((unsigned short)vk);
        if (k < 0) continue;
        mapped++;
        CHECK(vk < 0x80);
        CHECK(k > 0 && k < 256);
        if (k > 0 && k < 256) {
            CHECK(seen[k] == 0);
            seen[k] = 1;
        }
    }
    CHECK(mapped == (int)(sizeof(map) / sizeof(map[0])));
    CHECK(mapped == key_count());               // every key can be typed on a Mac keyboard
}

static void test_hid_usages(void) {
    for (char c = 'A'; c <= 'Z'; c++) {
        char name[2] = { c, '\0' };
        CHECK(soulx_key_hid_usage(key(name)) == 0x04 + (c - 'A'));
    }
    for (char c = '1'; c <= '9'; c++) {
        char name[2] = { c, '\0' };
        CHECK(soulx_key_hid_usage(key(name)) == 0x1E + (c - '1'));
    }
    static const struct { const char* name; int usage; } map[] = {
        { "0", 0x27 }, { "ENTER", 0x28 }, { "ESC", 0x29 }, { "BACKSPACE", 0x2A }, { "TAB", 0x2B }, { "SPACE", 0x2C },
        { "MINUS", 0x2D }, { "EQUAL", 0x2E }, { "LBRACKET", 0x2F }, { "RBRACKET", 0x30 }, { "BACKSLASH", 0x31 },
        { "SEMICOLON", 0x33 }, { "QUOTE", 0x34 }, { "GRAVE", 0x35 }, { "COMMA", 0x36 }, { "PERIOD", 0x37 },
        { "SLASH", 0x38 }, { "CAPSLOCK", 0x39 }, { "F1", 0x3A }, { "F12", 0x45 }, { "INSERT", 0x49 },
        { "HOME", 0x4A }, { "PAGEUP", 0x4B }, { "DELETE", 0x4C }, { "END", 0x4D }, { "PAGEDOWN", 0x4E },
        { "RIGHT", 0x4F }, { "LEFT", 0x50 }, { "DOWN", 0x51 }, { "UP", 0x52 }, { "NUMLOCK", 0x53 },
        { "KPDIVIDE", 0x54 }, { "KPMULTIPLY", 0x55 }, { "KPMINUS", 0x56 }, { "KPPLUS", 0x57 }, { "KPENTER", 0x58 },
        { "KP1", 0x59 }, { "KP9", 0x61 }, { "KP0", 0x62 }, { "KPDOT", 0x63 }, { "SECTION", 0x64 }, { "MENU", 0x65 },
        { "KPEQUAL", 0x67 }, { "F13", 0x68 }, { "F20", 0x6F },
        { "LCTRL", 0xE0 }, { "LSHIFT", 0xE1 }, { "LALT", 0xE2 }, { "LMETA", 0xE3 }, { "RCTRL", 0xE4 },
        { "RSHIFT", 0xE5 }, { "RALT", 0xE6 }, { "RMETA", 0xE7 },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) CHECK(soulx_key_hid_usage(key(map[i].name)) == map[i].usage);
    for (int k = 1; k <= key_count(); k++) {
        int u = soulx_key_hid_usage(k);
        CHECK((u >= 0x04 && u <= 0x6F) || (u >= 0xE0 && u <= 0xE7));
        for (int j = 1; j < k; j++) CHECK(soulx_key_hid_usage(j) != u);
    }
    CHECK(soulx_key_hid_usage(0) == -1);
    CHECK(soulx_key_hid_usage(-5) == -1);
    CHECK(soulx_key_hid_usage(key_count() + 1) == -1);

    // same usages as hidkbd's character table (the unshifted character of each key)
    static const struct { char c; const char* name; } chars[] = {
        { 'a', "A" }, { 'z', "Z" }, { '0', "0" }, { '5', "5" }, { ' ', "SPACE" }, { '-', "MINUS" }, { '=', "EQUAL" },
        { '[', "LBRACKET" }, { ']', "RBRACKET" }, { '\\', "BACKSLASH" }, { ';', "SEMICOLON" }, { '\'', "QUOTE" },
        { ',', "COMMA" }, { '.', "PERIOD" }, { '/', "SLASH" }, { '`', "GRAVE" }, { '\n', "ENTER" }, { '\t', "TAB" },
    };
    for (size_t i = 0; i < sizeof(chars) / sizeof(chars[0]); i++) {
        int usage = -1;
        bool shift = true;
        CHECK(soulx_hidkbd_usage_for_char(chars[i].c, &usage, &shift) == 0);
        CHECK(!shift && usage == soulx_key_hid_usage(key(chars[i].name)));
    }
}

// ---------------------------------------------------------------------------
// Parse / format
// ---------------------------------------------------------------------------

static bool same_binding(const SoulXKeyBinding* a, const SoulXKeyBinding* b) {
    if (a->kind != b->kind || a->x != b->x || a->y != b->y) return false;
    int nkeys = a->kind == SOULX_KM_JOYSTICK ? 4 : 1;
    for (int i = 0; i < nkeys; i++) {
        if (a->keys[i] != b->keys[i]) return false;
    }
    if (a->kind == SOULX_KM_JOYSTICK && a->radius != b->radius) return false;
    if (a->kind == SOULX_KM_SWIPE && (a->x2 != b->x2 || a->y2 != b->y2 || a->ms != b->ms)) return false;
    return true;
}

static void test_parse_ok(void) {
    const char* text =
        "\xEF\xBB\xBF# SoulX keymap\n"
        "tap Space 0.85 0.80\n"
        "  TAP   q   .5   1   # inline comment\n"
        "\n"
        "joystick W A S D 0.18 0.75 0.10\n"
        "Swipe E 0.5 0.8 0.5 0.3 150\r\n"
        "\t \t\n"
        "tap kp5 0 0.\r\n"
        "# trailing comment without newline: tap A 0 0";
    SoulXKeymap km;
    memset(&km, 0, sizeof(km));
    snprintf(km.package, sizeof(km.package), "com.example.game");
    char err[256] = "x";
    CHECK(soulx_keymap_parse(text, &km, err, sizeof(err)) == 0);
    CHECK(err[0] == '\0');
    CHECK(strcmp(km.package, "com.example.game") == 0);    // parse leaves the package alone
    CHECK(km.count == 5);
    if (km.count == 5) {
        CHECK(km.b[0].kind == SOULX_KM_TAP && km.b[0].keys[0] == key("SPACE") && km.b[0].x == 0.85 && km.b[0].y == 0.80);
        CHECK(km.b[1].kind == SOULX_KM_TAP && km.b[1].keys[0] == key("Q") && km.b[1].x == 0.5 && km.b[1].y == 1.0);
        CHECK(km.b[2].kind == SOULX_KM_JOYSTICK && km.b[2].keys[0] == key("W") && km.b[2].keys[1] == key("A") &&
              km.b[2].keys[2] == key("S") && km.b[2].keys[3] == key("D") && km.b[2].x == 0.18 &&
              km.b[2].y == 0.75 && km.b[2].radius == 0.10);
        CHECK(km.b[3].kind == SOULX_KM_SWIPE && km.b[3].keys[0] == key("E") && km.b[3].x == 0.5 &&
              km.b[3].y == 0.8 && km.b[3].x2 == 0.5 && km.b[3].y2 == 0.3 && km.b[3].ms == 150);
        CHECK(km.b[4].kind == SOULX_KM_TAP && km.b[4].keys[0] == key("KP5") && km.b[4].x == 0.0 && km.b[4].y == 0.0);
    }

    // empty, comments only, numbers in every accepted spelling, CR-only blank lines
    CHECK(soulx_keymap_parse("", &km, err, sizeof(err)) == 0 && km.count == 0);
    CHECK(soulx_keymap_parse("# nothing\n\n   # more\n", &km, err, sizeof(err)) == 0 && km.count == 0);
    CHECK(soulx_keymap_parse("\r\n\r\n", &km, err, sizeof(err)) == 0 && km.count == 0);
    CHECK(soulx_keymap_parse("tap a 0 1\ntap b 1.0 0.0\ntap c 00.5000 .25\ntap d 0.123456789012345 1.\n", &km, err,
                            sizeof(err)) == 0);
    CHECK(km.count == 4);
    if (km.count == 4) {
        CHECK(km.b[2].x == 0.5 && km.b[2].y == 0.25);
        CHECK(near(km.b[3].x, 0.123456789012345) && km.b[3].y == 1.0);
    }
    // a joystick radius of exactly 0.5, the longest and shortest swipes
    CHECK(soulx_keymap_parse("joystick up left down right 0.5 0.5 0.5\nswipe x 0 0 1 1 1\nswipe y 1 1 0 0 5000\n", &km,
                            err, sizeof(err)) == 0 && km.count == 3);
    // two bindings may share a key
    CHECK(soulx_keymap_parse("tap space 0.1 0.1\ntap space 0.9 0.9\njoystick w a s d 0.5 0.5 0.1\ntap w 0.2 0.2\n", &km,
                            err, sizeof(err)) == 0 && km.count == 4);
    // a long comment is fine
    char* long_comment = malloc(5000);
    if (long_comment) {
        memset(long_comment, 'c', 4998);
        long_comment[0] = '#';
        long_comment[4998] = '\n';
        long_comment[4999] = '\0';
        CHECK(soulx_keymap_parse(long_comment, &km, err, sizeof(err)) == 0 && km.count == 0);
        free(long_comment);
    }
    // exactly SOULX_KEYMAP_MAX bindings
    char* many = malloc(SOULX_KEYMAP_MAX * 32 + 64);
    if (many) {
        size_t off = 0;
        for (int i = 0; i < SOULX_KEYMAP_MAX; i++) off += (size_t)sprintf(many + off, "tap a 0.%02d 0.5\n", i);
        CHECK(soulx_keymap_parse(many, &km, err, sizeof(err)) == 0 && km.count == SOULX_KEYMAP_MAX);
        sprintf(many + off, "tap b 0.5 0.5\n");
        CHECK(soulx_keymap_parse(many, &km, err, sizeof(err)) == -1 && km.count == 0);
        CHECK(strstr(err, "line 65:") == err && strstr(err, "too many bindings") != NULL);
        free(many);
    }
}

typedef struct {
    const char* line;
    const char* expect;         // substring of the message
} BadLine;

static void test_parse_errors(void) {
    const BadLine bad[] = {
        { "tapp A 0.1 0.1", "unknown binding type 'tapp'" },
        { "0.5 A 0.1 0.1", "unknown binding type" },
        { "tap XYZ 0.1 0.1", "unknown key 'XYZ'" },
        { "tap A 0.1", "needs more values" },
        { "tap A", "needs more values" },
        { "tap", "needs more values" },
        { "tap A 0.1 0.2 0.3", "too many values" },
        { "tap A 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9", "too many values" },
        { "tap A 1.5 0.2", "x must be a number between 0 and 1, not '1.5'" },
        { "tap A 0.5 -0.1", "y must be" },
        { "tap A 0.5 abc", "y must be" },
        { "tap A 0.5 0.5.5", "y must be" },
        { "tap A 1e-1 0.5", "x must be" },
        { "tap A 0,5 0.5", "x must be" },
        { "tap A . 0.5", "x must be" },
        { "tap A +0.5 0.5", "x must be" },
        { "tap A 1.00001 0.5", "x must be" },
        { "tap A 0.1234567890123456 0.5", "x must be" },
        { "tap A nan 0.5", "x must be" },
        { "joystick W A S W 0.5 0.5 0.1", "different" },
        { "joystick W A S D 0.5 0.5 0", "radius" },
        { "joystick W A S D 0.5 0.5 0.6", "radius" },
        { "joystick W A S D 0.5 0.5 -1", "radius" },
        { "joystick W A S D 1.5 0.5 0.1", "cx must be" },
        { "joystick W A S D 0.5 7 0.1", "cy must be" },
        { "joystick W A S D 0.5 0.5", "needs more values" },
        { "joystick W A S 0.5 0.5 0.1", "needs more values" },
        { "joystick W A S 0.5 0.5 0.1 0.2", "unknown key '0.5'" },
        { "swipe Q 0.5 0.5 0.5 0.5 0", "duration" },
        { "swipe Q 0.5 0.5 0.5 0.5 5001", "duration" },
        { "swipe Q 0.5 0.5 0.5 0.5 1.5", "duration" },
        { "swipe Q 0.5 0.5 0.5 0.5 -3", "duration" },
        { "swipe Q 0.5 0.5 0.5 0.5 9999999999", "duration" },
        { "swipe Q 0.5 0.5 0.5 2 100", "y2 must be" },
        { "swipe Q 0.5 0.5 x 0.5 100", "x2 must be" },
        { "swipe Q 0.5 0.5 0.5 0.5", "needs more values" },
        { "tap A 0.1 0.1 \xC3\xA9", "unexpected character" },
        { "tap A 0.1\x01 0.1", "unexpected character" },
        { "tap \x7F 0.1 0.1", "unexpected character" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char text[512];
        snprintf(text, sizeof(text), "tap SPACE 0.5 0.5\n\n# comment\n%s\ntap B 0.5 0.5\n", bad[i].line);
        SoulXKeymap km;
        memset(&km, 0, sizeof(km));
        char err[256] = "";
        int rc = soulx_keymap_parse(text, &km, err, sizeof(err));
        CHECK(rc == -1);
        CHECK(km.count == 0);
        CHECK(strncmp(err, "line 4: ", 8) == 0);
        CHECK(strstr(err, bad[i].expect) != NULL);
        if (rc != -1 || strncmp(err, "line 4: ", 8) != 0 || !strstr(err, bad[i].expect)) {
            fprintf(stderr, "  line '%s' -> rc %d, err '%s'\n", bad[i].line, rc, err);
        }
    }

    // a binding line that is too long (comments may be any length)
    char* text = malloc(2048);
    if (text) {
        size_t off = (size_t)sprintf(text, "tap A 0.5 0.5\ntap B 0.5 0.5");
        memset(text + off, ' ', 600);
        sprintf(text + off + 600, "\n");
        SoulXKeymap km;
        char err[128];
        CHECK(soulx_keymap_parse(text, &km, err, sizeof(err)) == -1);
        CHECK(strstr(err, "line 2: line too long") == err);
        free(text);
    }
    // an error message never overflows, NULL / tiny buffers are fine
    SoulXKeymap km;
    CHECK(soulx_keymap_parse("tap XYZ 0 0", &km, NULL, 0) == -1);
    char tiny[6] = "zzzzz";
    CHECK(soulx_keymap_parse("tap XYZ 0 0", &km, tiny, sizeof(tiny)) == -1);
    CHECK(strcmp(tiny, "line ") == 0);
    char one[1] = { 'q' };
    CHECK(soulx_keymap_parse("tap XYZ 0 0", &km, one, sizeof(one)) == -1 && one[0] == '\0');
    char err[200];
    CHECK(soulx_keymap_parse(NULL, &km, err, sizeof(err)) == -1 && km.count == 0);
    CHECK(soulx_keymap_parse("tap A 0 0", NULL, err, sizeof(err)) == -1);
    // a long garbage token is shortened in the message
    CHECK(soulx_keymap_parse("tap AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA 0 0", &km, err, sizeof(err)) == -1);
    CHECK(strstr(err, "AAAAAAAAAAAAAAAAAAAAAAAA...'") != NULL);
    // the error is on the right line with CRLF files too
    CHECK(soulx_keymap_parse("tap A 0 0\r\n\r\ntap A 0 2\r\n", &km, err, sizeof(err)) == -1);
    CHECK(strncmp(err, "line 3: ", 8) == 0);
}

static void make_keymap(SoulXKeymap* km) {
    memset(km, 0, sizeof(*km));
    snprintf(km->package, sizeof(km->package), "com.example.game");
    km->b[0].kind = SOULX_KM_TAP;
    km->b[0].keys[0] = key("SPACE");
    km->b[0].x = 0.85;
    km->b[0].y = 0.80;
    km->b[1].kind = SOULX_KM_JOYSTICK;
    km->b[1].keys[0] = key("W");
    km->b[1].keys[1] = key("A");
    km->b[1].keys[2] = key("S");
    km->b[1].keys[3] = key("D");
    km->b[1].x = 0.18;
    km->b[1].y = 0.75;
    km->b[1].radius = 0.10;
    km->b[2].kind = SOULX_KM_SWIPE;
    km->b[2].keys[0] = key("Q");
    km->b[2].x = 0.5;
    km->b[2].y = 0.8;
    km->b[2].x2 = 0.5;
    km->b[2].y2 = 0.3;
    km->b[2].ms = 150;
    km->b[3].kind = SOULX_KM_TAP;
    km->b[3].keys[0] = key("KPENTER");
    km->b[3].x = 1.0;
    km->b[3].y = 0.0;
    km->b[4].kind = SOULX_KM_TAP;
    km->b[4].keys[0] = key("F1");
    km->b[4].x = 0.125;
    km->b[4].y = 0.0001;
    km->count = 5;
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint64_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static double rnd_fraction(void) {
    return (double)(rnd() % 10001) / 10000.0;   // 4 decimals: what format writes
}

static void test_format(void) {
    SoulXKeymap km;
    make_keymap(&km);
    char buf[4096];
    CHECK(soulx_keymap_format(&km, buf, sizeof(buf)) == 0);
    const char* golden =
        "# SoulX keymap for com.example.game\n"
        "# tap <key> <x> <y> | joystick <up> <left> <down> <right> <cx> <cy> <radius> | swipe <key> <x1> <y1> <x2> "
        "<y2> <ms>\n"
        "# x, y: fractions (0..1) of the upright screen; radius: fraction of its shorter side\n"
        "tap      SPACE 0.85 0.80\n"
        "joystick W A S D 0.18 0.75 0.10\n"
        "swipe    Q 0.50 0.80 0.50 0.30 150\n"
        "tap      KPENTER 1.00 0.00\n"
        "tap      F1 0.125 0.0001\n";
    CHECK(strcmp(buf, golden) == 0);
    if (strcmp(buf, golden) != 0) fprintf(stderr, "  got:\n%s", buf);

    // round trip: parse(format(km)) == km and format is stable
    SoulXKeymap back;
    memset(&back, 0, sizeof(back));
    char err[256];
    CHECK(soulx_keymap_parse(buf, &back, err, sizeof(err)) == 0);
    CHECK(back.count == km.count);
    for (int i = 0; i < km.count && i < back.count; i++) CHECK(same_binding(&km.b[i], &back.b[i]));
    char again[4096];
    snprintf(back.package, sizeof(back.package), "%s", km.package);
    CHECK(soulx_keymap_format(&back, again, sizeof(again)) == 0 && strcmp(again, buf) == 0);

    // default keymap header
    km.package[0] = '\0';
    CHECK(soulx_keymap_format(&km, buf, sizeof(buf)) == 0);
    CHECK(strncmp(buf, "# SoulX keymap: default", 22) == 0);

    // values are rounded to 4 decimals
    km.b[0].x = 0.123456;
    km.b[0].y = 0.99996;
    CHECK(soulx_keymap_format(&km, buf, sizeof(buf)) == 0);
    CHECK(strstr(buf, "tap      SPACE 0.1235 1.00\n") != NULL);

    // random keymaps survive the round trip exactly
    int nkeys = key_count();
    for (int iter = 0; iter < 300; iter++) {
        SoulXKeymap r;
        memset(&r, 0, sizeof(r));
        r.count = (int)(rnd() % (SOULX_KEYMAP_MAX + 1));
        for (int i = 0; i < r.count; i++) {
            SoulXKeyBinding* b = &r.b[i];
            b->kind = (SoulXKeymapKind)(rnd() % 3);
            b->x = rnd_fraction();
            b->y = rnd_fraction();
            if (b->kind == SOULX_KM_JOYSTICK) {
                int first = 1 + (int)(rnd() % (uint64_t)(nkeys - 3));
                for (int k = 0; k < 4; k++) b->keys[k] = first + k;
                b->radius = (double)(1 + rnd() % 5000) / 10000.0;
            } else {
                b->keys[0] = 1 + (int)(rnd() % (uint64_t)nkeys);
            }
            if (b->kind == SOULX_KM_SWIPE) {
                b->x2 = rnd_fraction();
                b->y2 = rnd_fraction();
                b->ms = 1 + (int)(rnd() % 5000);
            }
        }
        char* text = malloc(32768);
        if (!text) break;
        CHECK(soulx_keymap_format(&r, text, 32768) == 0);
        SoulXKeymap p;
        memset(&p, 0, sizeof(p));
        CHECK(soulx_keymap_parse(text, &p, err, sizeof(err)) == 0);
        CHECK(p.count == r.count);
        bool same = p.count == r.count;
        for (int i = 0; i < r.count && same; i++) same = same_binding(&r.b[i], &p.b[i]);
        CHECK(same);
        free(text);
    }

    // too small a buffer, invalid input
    make_keymap(&km);
    char small[64] = "x";
    CHECK(soulx_keymap_format(&km, small, sizeof(small)) == -1 && small[0] == '\0');
    CHECK(soulx_keymap_format(&km, NULL, 100) == -1);
    CHECK(soulx_keymap_format(&km, buf, 0) == -1);
    CHECK(soulx_keymap_format(NULL, buf, sizeof(buf)) == -1);
    SoulXKeymap bad = km;
    bad.b[1].x = 1.5;
    CHECK(soulx_keymap_format(&bad, buf, sizeof(buf)) == -1 && buf[0] == '\0');
    bad = km;
    bad.b[1].y = NAN;
    CHECK(soulx_keymap_format(&bad, buf, sizeof(buf)) == -1);
    bad = km;
    bad.b[0].keys[0] = 0;
    CHECK(soulx_keymap_format(&bad, buf, sizeof(buf)) == -1);
    bad = km;
    bad.b[1].keys[3] = bad.b[1].keys[0];
    CHECK(soulx_keymap_format(&bad, buf, sizeof(buf)) == -1);
    bad = km;
    bad.b[1].radius = 0;
    CHECK(soulx_keymap_format(&bad, buf, sizeof(buf)) == -1);
    bad = km;
    bad.b[2].ms = 0;
    CHECK(soulx_keymap_format(&bad, buf, sizeof(buf)) == -1);
    bad = km;
    bad.b[0].kind = (SoulXKeymapKind)7;
    CHECK(soulx_keymap_format(&bad, buf, sizeof(buf)) == -1);
    bad = km;
    bad.count = SOULX_KEYMAP_MAX + 1;
    CHECK(soulx_keymap_format(&bad, buf, sizeof(buf)) == -1);
    bad = km;
    bad.count = -1;
    CHECK(soulx_keymap_format(&bad, buf, sizeof(buf)) == -1);
    bad = km;
    snprintf(bad.package, sizeof(bad.package), "bad\npackage");
    CHECK(soulx_keymap_format(&bad, buf, sizeof(buf)) == -1);
    bad = km;
    memset(bad.package, 'a', sizeof(bad.package));      // not NUL-terminated
    CHECK(soulx_keymap_format(&bad, buf, sizeof(buf)) == -1);
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

static void home_path(char* buf, size_t cap, const char* rel) {
    snprintf(buf, cap, "%s/%s", g_home, rel);
}

static bool write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fputs(text, f);
    return fclose(f) == 0;
}

static void test_files(void) {
    SoulXKeymap km;
    char path[PATH_MAX];

    // nothing yet (not even the keymaps directory)
    CHECK(soulx_keymap_load("com.example.game", &km) == 2 && km.count == 0 && km.package[0] == '\0');
    CHECK(soulx_keymap_load(NULL, &km) == 2);
    CHECK(soulx_keymap_load("com.example.game", NULL) == -1);

    // default keymap
    SoulXKeymap def;
    memset(&def, 0, sizeof(def));
    def.count = 1;
    def.b[0].kind = SOULX_KM_TAP;
    def.b[0].keys[0] = key("ENTER");
    def.b[0].x = 0.5;
    def.b[0].y = 0.9;
    CHECK(soulx_keymap_save(&def) == 0);
    home_path(path, sizeof(path), "keymaps/default.keymap");
    struct stat st;
    CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode));
    CHECK(soulx_keymap_load("com.example.game", &km) == 1);
    CHECK(km.count == 1 && km.package[0] == '\0' && same_binding(&km.b[0], &def.b[0]));
    CHECK(soulx_keymap_load("", &km) == 1);
    CHECK(soulx_keymap_load(NULL, &km) == 1);
    // invalid package names never reach the file system; they get the default keymap
    const char* const bad_names[] = { "../default", "a/b.c", "game", "com..x", "com.x.", ".com.x", "com x.y",
                                      "com.example.game/../../x" };
    for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++) {
        CHECK(soulx_keymap_load(bad_names[i], &km) == 1 && km.package[0] == '\0');
    }

    // per-game keymap
    SoulXKeymap game;
    make_keymap(&game);
    CHECK(soulx_keymap_save(&game) == 0);
    home_path(path, sizeof(path), "keymaps/com.example.game.keymap");
    CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode));
    memset(&km, 0x5A, sizeof(km));
    CHECK(soulx_keymap_load("com.example.game", &km) == 0);
    CHECK(strcmp(km.package, "com.example.game") == 0 && km.count == game.count);
    for (int i = 0; i < game.count && i < km.count; i++) CHECK(same_binding(&km.b[i], &game.b[i]));
    // saving again replaces the file atomically
    game.count = 2;
    CHECK(soulx_keymap_save(&game) == 0);
    CHECK(soulx_keymap_load("com.example.game", &km) == 0 && km.count == 2);
    // an empty per-game keymap still overrides the default one
    game.count = 0;
    CHECK(soulx_keymap_save(&game) == 0);
    CHECK(soulx_keymap_load("com.example.game", &km) == 0 && km.count == 0);

    // a keymap shared through a symbolic link
    char link_path[PATH_MAX];
    home_path(link_path, sizeof(link_path), "keymaps/com.other.game.keymap");
    CHECK(symlink(path, link_path) == 0);
    CHECK(soulx_keymap_load("com.other.game", &km) == 0 && strcmp(km.package, "com.other.game") == 0);

    // a broken per-game file is an error (the warning is logged once)
    home_path(path, sizeof(path), "keymaps/com.broken.game.keymap");
    CHECK(write_text(path, "tap A 0.5 0.5\ntap XYZ 0.5 0.5\n"));
    CHECK(soulx_keymap_load("com.broken.game", &km) == -1 && km.count == 0);
    CHECK(soulx_keymap_load("com.broken.game", &km) == -1);
    // a directory, an over-long file, a file with a NUL byte
    home_path(path, sizeof(path), "keymaps/com.dir.game.keymap");
    CHECK(mkdir(path, 0755) == 0);
    CHECK(soulx_keymap_load("com.dir.game", &km) == -1);
    home_path(path, sizeof(path), "keymaps/com.huge.game.keymap");
    FILE* f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        for (int i = 0; i < 20000; i++) fputs("# padding padding\n", f);
        fclose(f);
    }
    CHECK(soulx_keymap_load("com.huge.game", &km) == -1);
    home_path(path, sizeof(path), "keymaps/com.nul.game.keymap");
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fwrite("tap A 0 0\n\0tap B 0 0\n", 1, 21, f);
        fclose(f);
    }
    CHECK(soulx_keymap_load("com.nul.game", &km) == -1);

    // save refuses invalid names and keymaps
    SoulXKeymap bad;
    make_keymap(&bad);
    snprintf(bad.package, sizeof(bad.package), "../evil");
    CHECK(soulx_keymap_save(&bad) == -1);
    snprintf(bad.package, sizeof(bad.package), "com.example.game");
    bad.b[0].x = 2.0;
    CHECK(soulx_keymap_save(&bad) == -1);
    CHECK(soulx_keymap_save(NULL) == -1);
    home_path(path, sizeof(path), "evil.keymap");
    CHECK(stat(path, &st) != 0);
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

static void check_touch(const SoulXKeymapAction* a, int action, double x, double y) {
    CHECK(a->kind == SOULX_KM_ACT_TOUCH);
    CHECK(a->action == action);
    CHECK(a->finger >= SOULX_KEYMAP_FIRST_FINGER && a->finger <= SOULX_KEYMAP_LAST_FINGER);
    CHECK(near(a->x, x) && near(a->y, y));
    if (!near(a->x, x) || !near(a->y, y)) fprintf(stderr, "  touch at (%f, %f), expected (%f, %f)\n", a->x, a->y, x, y);
}

static void test_runtime_basic(void) {
    CHECK(soulx_keymap_runtime_new(NULL) == NULL);
    soulx_keymap_runtime_free(NULL);
    SoulXKeymapAction out[ACT_MAX];
    CHECK(soulx_keymap_key_event(NULL, key("A"), true, out, ACT_MAX) == 0);
    CHECK(soulx_keymap_release_all(NULL, out, ACT_MAX) == 0);
    CHECK(!soulx_keymap_binds(NULL, key("A")));
    soulx_keymap_runtime_set_screen(NULL, 100, 100);

    SoulXKeymap km;
    make_keymap(&km);
    SoulXKeymapRuntime* rt = soulx_keymap_runtime_new(&km);
    CHECK(rt != NULL);
    if (!rt) return;
    memset(&km, 0, sizeof(km));                 // the runtime keeps its own copy

    CHECK(soulx_keymap_binds(rt, key("SPACE")));
    CHECK(soulx_keymap_binds(rt, key("W")) && soulx_keymap_binds(rt, key("A")) && soulx_keymap_binds(rt, key("S")) &&
          soulx_keymap_binds(rt, key("D")));
    CHECK(soulx_keymap_binds(rt, key("Q")));
    CHECK(!soulx_keymap_binds(rt, key("E")) && !soulx_keymap_binds(rt, key("ESC")));
    CHECK(!soulx_keymap_binds(rt, 0) && !soulx_keymap_binds(rt, -1) && !soulx_keymap_binds(rt, 100000));
    CHECK(soulx_keymap_key_event(rt, 0, true, out, ACT_MAX) == 0);
    CHECK(soulx_keymap_key_event(rt, 9999, true, out, ACT_MAX) == 0);
    CHECK(soulx_keymap_key_event(rt, key("E"), true, out, ACT_MAX) == 0);
    CHECK(soulx_keymap_key_event(rt, key("SPACE"), true, NULL, ACT_MAX) == 0);
    CHECK(soulx_keymap_key_event(rt, key("SPACE"), true, out, 0) == 0);

    // tap: hold the key = hold the finger; repeat ignored
    CHECK(soulx_keymap_key_event(rt, key("SPACE"), true, out, ACT_MAX) == 1);
    check_touch(&out[0], SOULX_TOUCH_DOWN, 0.85, 0.80);
    int tap_finger = out[0].finger;
    CHECK(soulx_keymap_key_event(rt, key("SPACE"), true, out, ACT_MAX) == 0);
    CHECK(soulx_keymap_key_event(rt, key("SPACE"), true, out, ACT_MAX) == 0);
    CHECK(soulx_keymap_key_event(rt, key("SPACE"), false, out, ACT_MAX) == 1);
    check_touch(&out[0], SOULX_TOUCH_UP, 0.85, 0.80);
    CHECK(out[0].finger == tap_finger);
    CHECK(soulx_keymap_key_event(rt, key("SPACE"), false, out, ACT_MAX) == 0);

    // swipe: one swipe per press; the release only frees its finger
    CHECK(soulx_keymap_key_event(rt, key("Q"), true, out, ACT_MAX) == 1);
    CHECK(out[0].kind == SOULX_KM_ACT_SWIPE);
    CHECK(out[0].finger >= SOULX_KEYMAP_FIRST_FINGER && out[0].finger <= SOULX_KEYMAP_LAST_FINGER);
    CHECK(out[0].x == 0.5 && out[0].y == 0.8 && out[0].x2 == 0.5 && out[0].y2 == 0.3 && out[0].ms == 150);
    int swipe_finger = out[0].finger;
    CHECK(soulx_keymap_key_event(rt, key("Q"), true, out, ACT_MAX) == 0);
    CHECK(soulx_keymap_key_event(rt, key("SPACE"), true, out, ACT_MAX) == 1);
    CHECK(out[0].finger != swipe_finger);       // the swipe still owns its finger
    CHECK(soulx_keymap_key_event(rt, key("SPACE"), false, out, ACT_MAX) == 1);
    CHECK(soulx_keymap_key_event(rt, key("Q"), false, out, ACT_MAX) == 0);
    CHECK(soulx_keymap_key_event(rt, key("Q"), false, out, ACT_MAX) == 0);
    CHECK(soulx_keymap_key_event(rt, key("Q"), true, out, ACT_MAX) == 1 && out[0].kind == SOULX_KM_ACT_SWIPE);
    CHECK(out[0].finger != swipe_finger);       // least recently released finger first
    CHECK(soulx_keymap_key_event(rt, key("Q"), false, out, ACT_MAX) == 0);

    // releasing a key that was never pressed does nothing
    CHECK(soulx_keymap_key_event(rt, key("W"), false, out, ACT_MAX) == 0);
    CHECK(soulx_keymap_release_all(rt, out, ACT_MAX) == 0);
    soulx_keymap_runtime_free(rt);
}

// Presses keys (in order), returns where the joystick finger is, then releases everything.
static void joystick_case(SoulXKeymapRuntime* rt, const char* const* keys, int nkeys, double ex, double ey) {
    SoulXKeymapAction out[ACT_MAX];
    int finger = 0;
    double x = -1, y = -1;
    for (int i = 0; i < nkeys; i++) {
        int n = soulx_keymap_key_event(rt, key(keys[i]), true, out, ACT_MAX);
        if (i == 0) {
            CHECK(n == 2);
            if (n != 2) return;
            check_touch(&out[0], SOULX_TOUCH_DOWN, 0.18, 0.75);
            CHECK(out[1].action == SOULX_TOUCH_MOVE && out[1].finger == out[0].finger);
            finger = out[0].finger;
            x = out[1].x;
            y = out[1].y;
        } else {
            CHECK(n == 1);
            if (n != 1) return;
            CHECK(out[0].kind == SOULX_KM_ACT_TOUCH && out[0].action == SOULX_TOUCH_MOVE && out[0].finger == finger);
            x = out[0].x;
            y = out[0].y;
        }
        CHECK(soulx_keymap_key_event(rt, key(keys[i]), true, out, ACT_MAX) == 0);   // repeat
    }
    CHECK(near(x, ex) && near(y, ey));
    if (!near(x, ex) || !near(y, ey)) fprintf(stderr, "  joystick %s%s -> (%f, %f), expected (%f, %f)\n", keys[0],
                                              nkeys > 1 ? keys[1] : "", x, y, ex, ey);
    for (int i = 0; i < nkeys; i++) {
        int n = soulx_keymap_key_event(rt, key(keys[i]), false, out, ACT_MAX);
        CHECK(n == 1);
        if (n != 1) continue;
        CHECK(out[0].finger == finger);
        if (i + 1 < nkeys) {
            CHECK(out[0].action == SOULX_TOUCH_MOVE);
        } else {
            CHECK(out[0].action == SOULX_TOUCH_UP);
        }
        x = out[0].x;
        y = out[0].y;
    }
    CHECK(soulx_keymap_key_event(rt, key(keys[nkeys - 1]), false, out, ACT_MAX) == 0);
}

static void test_runtime_joystick(void) {
    SoulXKeymap km;
    make_keymap(&km);
    SoulXKeymapRuntime* rt = soulx_keymap_runtime_new(&km);
    CHECK(rt != NULL);
    if (!rt) return;
    const double cx = 0.18, cy = 0.75, r = 0.10, d = 0.10 * 0.70710678118654752440;
    struct { const char* keys[2]; int n; double x, y; } dirs[] = {
        { { "W", NULL }, 1, cx, cy - r },       { { "W", "D" }, 2, cx + d, cy - d },
        { { "D", NULL }, 1, cx + r, cy },       { { "S", "D" }, 2, cx + d, cy + d },
        { { "S", NULL }, 1, cx, cy + r },       { { "S", "A" }, 2, cx - d, cy + d },
        { { "A", NULL }, 1, cx - r, cy },       { { "A", "W" }, 2, cx - d, cy - d },
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) joystick_case(rt, dirs[i].keys, dirs[i].n, dirs[i].x, dirs[i].y);

    // opposite keys cancel (the finger stays down at the centre)
    SoulXKeymapAction out[ACT_MAX];
    CHECK(soulx_keymap_key_event(rt, key("W"), true, out, ACT_MAX) == 2);
    CHECK(soulx_keymap_key_event(rt, key("S"), true, out, ACT_MAX) == 1);
    check_touch(&out[0], SOULX_TOUCH_MOVE, cx, cy);
    CHECK(soulx_keymap_key_event(rt, key("A"), true, out, ACT_MAX) == 1);
    check_touch(&out[0], SOULX_TOUCH_MOVE, cx - r, cy);
    CHECK(soulx_keymap_key_event(rt, key("D"), true, out, ACT_MAX) == 1);      // all four: centre
    check_touch(&out[0], SOULX_TOUCH_MOVE, cx, cy);
    CHECK(soulx_keymap_key_event(rt, key("W"), false, out, ACT_MAX) == 1);
    check_touch(&out[0], SOULX_TOUCH_MOVE, cx, cy + r);
    CHECK(soulx_keymap_key_event(rt, key("A"), false, out, ACT_MAX) == 1);
    check_touch(&out[0], SOULX_TOUCH_MOVE, cx + d, cy + d);
    CHECK(soulx_keymap_key_event(rt, key("S"), false, out, ACT_MAX) == 1);
    check_touch(&out[0], SOULX_TOUCH_MOVE, cx + r, cy);
    CHECK(soulx_keymap_key_event(rt, key("D"), false, out, ACT_MAX) == 1);
    check_touch(&out[0], SOULX_TOUCH_UP, cx + r, cy);

    // left, then left+right (centre), + up, + down (centre again); release_all lifts the finger where it is
    CHECK(soulx_keymap_key_event(rt, key("A"), true, out, ACT_MAX) == 2);
    CHECK(soulx_keymap_key_event(rt, key("D"), true, out, ACT_MAX) == 1);      // centre
    CHECK(soulx_keymap_key_event(rt, key("W"), true, out, ACT_MAX) == 1);      // up
    CHECK(soulx_keymap_key_event(rt, key("S"), true, out, ACT_MAX) == 1);      // centre again
    CHECK(soulx_keymap_release_all(rt, out, ACT_MAX) == 1);
    check_touch(&out[0], SOULX_TOUCH_UP, cx, cy);
    for (const char* k = "WASD"; *k; k++) {
        char name[2] = { *k, '\0' };
        CHECK(soulx_keymap_key_event(rt, key(name), false, out, ACT_MAX) == 0);
    }

    // not enough room for DOWN + MOVE: nothing happens and nothing is remembered
    CHECK(soulx_keymap_key_event(rt, key("W"), true, out, 1) == 0);
    CHECK(soulx_keymap_key_event(rt, key("W"), false, out, ACT_MAX) == 0);
    CHECK(soulx_keymap_key_event(rt, key("W"), true, out, 2) == 2);
    CHECK(soulx_keymap_key_event(rt, key("W"), false, out, 1) == 1);

    // the screen size makes the radius a circle: portrait 1080x2400 and landscape 2400x1080
    soulx_keymap_runtime_set_screen(rt, 1080, 2400);
    const char* up[] = { "W" };
    const char* right[] = { "D" };
    const char* up_right[] = { "W", "D" };
    joystick_case(rt, up, 1, cx, cy - r * 1080.0 / 2400.0);
    joystick_case(rt, right, 1, cx + r, cy);
    joystick_case(rt, up_right, 2, cx + d, cy - d * 1080.0 / 2400.0);
    soulx_keymap_runtime_set_screen(rt, 2400, 1080);
    joystick_case(rt, up, 1, cx, cy - r);
    joystick_case(rt, right, 1, cx + r * 1080.0 / 2400.0, cy);
    soulx_keymap_runtime_set_screen(rt, 0, 1080);                                // unknown: fractions as is
    joystick_case(rt, right, 1, cx + r, cy);
    soulx_keymap_runtime_set_screen(rt, -5, -5);
    joystick_case(rt, up, 1, cx, cy - r);
    soulx_keymap_runtime_free(rt);

    // targets are clamped to the screen
    SoulXKeymap edge;
    memset(&edge, 0, sizeof(edge));
    edge.count = 1;
    edge.b[0].kind = SOULX_KM_JOYSTICK;
    edge.b[0].keys[0] = key("UP");
    edge.b[0].keys[1] = key("LEFT");
    edge.b[0].keys[2] = key("DOWN");
    edge.b[0].keys[3] = key("RIGHT");
    edge.b[0].x = 0.02;
    edge.b[0].y = 0.98;
    edge.b[0].radius = 0.5;
    rt = soulx_keymap_runtime_new(&edge);
    CHECK(rt != NULL);
    if (!rt) return;
    CHECK(soulx_keymap_key_event(rt, key("LEFT"), true, out, ACT_MAX) == 2);
    check_touch(&out[1], SOULX_TOUCH_MOVE, 0.0, 0.98);
    CHECK(soulx_keymap_key_event(rt, key("DOWN"), true, out, ACT_MAX) == 1);
    check_touch(&out[0], SOULX_TOUCH_MOVE, 0.0, 1.0);
    CHECK(soulx_keymap_release_all(rt, out, ACT_MAX) == 1);
    soulx_keymap_runtime_free(rt);
}

static void test_runtime_fingers(void) {
    // nine tap bindings, keys 1..9; fingers 1..8 only
    SoulXKeymap km;
    memset(&km, 0, sizeof(km));
    for (int i = 0; i < 9; i++) {
        char name[2] = { (char)('1' + i), '\0' };
        km.b[i].kind = SOULX_KM_TAP;
        km.b[i].keys[0] = key(name);
        km.b[i].x = 0.1 * (i + 1);
        km.b[i].y = 0.5;
    }
    km.count = 9;
    SoulXKeymapRuntime* rt = soulx_keymap_runtime_new(&km);
    CHECK(rt != NULL);
    if (!rt) return;
    SoulXKeymapAction out[ACT_MAX];
    bool used[SOULX_MAX_FINGERS] = { false };
    for (int i = 0; i < 8; i++) {
        char name[2] = { (char)('1' + i), '\0' };
        CHECK(soulx_keymap_key_event(rt, key(name), true, out, ACT_MAX) == 1);
        int f = out[0].finger;
        CHECK(f >= SOULX_KEYMAP_FIRST_FINGER && f <= SOULX_KEYMAP_LAST_FINGER);
        if (f >= 0 && f < SOULX_MAX_FINGERS) {
            CHECK(!used[f]);
            used[f] = true;
        }
    }
    CHECK(soulx_keymap_key_event(rt, key("9"), true, out, ACT_MAX) == 0);       // no finger left
    CHECK(soulx_keymap_key_event(rt, key("9"), false, out, ACT_MAX) == 0);      // and no phantom release
    CHECK(soulx_keymap_key_event(rt, key("3"), false, out, ACT_MAX) == 1);
    int freed = out[0].finger;
    CHECK(soulx_keymap_key_event(rt, key("9"), true, out, ACT_MAX) == 1 && out[0].finger == freed);
    // release_all lifts every held finger; with a small buffer over several calls
    CHECK(soulx_keymap_release_all(rt, out, 3) == 3);
    CHECK(soulx_keymap_release_all(rt, out, 3) == 3);
    CHECK(soulx_keymap_release_all(rt, out, 3) == 2);
    CHECK(soulx_keymap_release_all(rt, out, 3) == 0);
    CHECK(soulx_keymap_release_all(rt, NULL, 3) == 0);
    for (int i = 0; i < 9; i++) {
        char name[2] = { (char)('1' + i), '\0' };
        CHECK(soulx_keymap_key_event(rt, key(name), false, out, ACT_MAX) == 0);
    }
    // all fingers are free again
    for (int i = 0; i < 8; i++) {
        char name[2] = { (char)('1' + i), '\0' };
        CHECK(soulx_keymap_key_event(rt, key(name), true, out, ACT_MAX) == 1);
    }
    CHECK(soulx_keymap_release_all(rt, out, ACT_MAX) == 8);
    for (int i = 0; i < 8; i++) {
        CHECK(out[i].kind == SOULX_KM_ACT_TOUCH && out[i].action == SOULX_TOUCH_UP);
        CHECK(near(out[i].x, 0.1 * (i + 1)) && near(out[i].y, 0.5));
    }
    soulx_keymap_runtime_free(rt);

    // the least recently released finger is used first
    memset(&km, 0, sizeof(km));
    km.count = 2;
    km.b[0].kind = SOULX_KM_TAP;
    km.b[0].keys[0] = key("A");
    km.b[0].x = 0.1;
    km.b[0].y = 0.1;
    km.b[1].kind = SOULX_KM_TAP;
    km.b[1].keys[0] = key("B");
    km.b[1].x = 0.2;
    km.b[1].y = 0.2;
    rt = soulx_keymap_runtime_new(&km);
    CHECK(rt != NULL);
    if (!rt) return;
    int seen[SOULX_MAX_FINGERS] = { 0 };
    for (int i = 0; i < 16; i++) {
        CHECK(soulx_keymap_key_event(rt, key(i % 2 ? "B" : "A"), true, out, ACT_MAX) == 1);
        int f = out[0].finger;
        if (f >= 0 && f < SOULX_MAX_FINGERS) seen[f]++;
        CHECK(soulx_keymap_key_event(rt, key(i % 2 ? "B" : "A"), false, out, ACT_MAX) == 1 && out[0].finger == f);
    }
    for (int f = SOULX_KEYMAP_FIRST_FINGER; f <= SOULX_KEYMAP_LAST_FINGER; f++) CHECK(seen[f] == 2);  // round robin
    CHECK(seen[0] == 0 && seen[9] == 0);
    soulx_keymap_runtime_free(rt);

    // shared keys: every binding of the key reacts, each with its own finger
    memset(&km, 0, sizeof(km));
    km.count = 3;
    km.b[0].kind = SOULX_KM_TAP;
    km.b[0].keys[0] = key("SPACE");
    km.b[0].x = 0.1;
    km.b[0].y = 0.1;
    km.b[1].kind = SOULX_KM_TAP;
    km.b[1].keys[0] = key("SPACE");
    km.b[1].x = 0.9;
    km.b[1].y = 0.9;
    km.b[2].kind = SOULX_KM_JOYSTICK;
    km.b[2].keys[0] = key("SPACE");
    km.b[2].keys[1] = key("LEFT");
    km.b[2].keys[2] = key("DOWN");
    km.b[2].keys[3] = key("RIGHT");
    km.b[2].x = 0.5;
    km.b[2].y = 0.5;
    km.b[2].radius = 0.2;
    rt = soulx_keymap_runtime_new(&km);
    CHECK(rt != NULL);
    if (!rt) return;
    CHECK(soulx_keymap_key_event(rt, key("SPACE"), true, out, ACT_MAX) == 4);
    CHECK(out[0].action == SOULX_TOUCH_DOWN && out[1].action == SOULX_TOUCH_DOWN && out[2].action == SOULX_TOUCH_DOWN &&
          out[3].action == SOULX_TOUCH_MOVE);
    CHECK(out[0].finger != out[1].finger && out[1].finger != out[2].finger && out[0].finger != out[2].finger);
    CHECK(near(out[3].y, 0.3));
    // a buffer too small for all of them: the ones that fit happen, the others keep their state
    CHECK(soulx_keymap_key_event(rt, key("SPACE"), false, out, 2) == 2);
    CHECK(soulx_keymap_key_event(rt, key("SPACE"), false, out, ACT_MAX) == 1 && out[0].action == SOULX_TOUCH_UP);
    CHECK(soulx_keymap_key_event(rt, key("SPACE"), false, out, ACT_MAX) == 0);
    soulx_keymap_runtime_free(rt);

    // invalid bindings are dropped by runtime_new; the count is clamped
    memset(&km, 0, sizeof(km));
    km.count = 3;
    km.b[0].kind = SOULX_KM_TAP;
    km.b[0].keys[0] = key("A");
    km.b[0].x = 2.0;
    km.b[1].kind = SOULX_KM_TAP;
    km.b[1].keys[0] = key("B");
    km.b[1].x = 0.5;
    km.b[1].y = 0.5;
    km.b[2].kind = (SoulXKeymapKind)9;
    km.b[2].keys[0] = key("C");
    rt = soulx_keymap_runtime_new(&km);
    CHECK(rt != NULL);
    if (rt) {
        CHECK(!soulx_keymap_binds(rt, key("A")) && soulx_keymap_binds(rt, key("B")) && !soulx_keymap_binds(rt, key("C")));
        soulx_keymap_runtime_free(rt);
    }
    km.count = 1000;
    rt = soulx_keymap_runtime_new(&km);
    CHECK(rt != NULL);
    if (rt) {
        CHECK(soulx_keymap_binds(rt, key("B")));
        soulx_keymap_runtime_free(rt);
    }
    km.count = -4;
    rt = soulx_keymap_runtime_new(&km);
    CHECK(rt != NULL);
    if (rt) {
        CHECK(!soulx_keymap_binds(rt, key("B")));
        soulx_keymap_runtime_free(rt);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static int rm_entry(const char* path, const struct stat* st, int flag, struct FTW* ftw) {
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

int main(void) {
    snprintf(g_home, sizeof(g_home), "/private/tmp/soulx-test-keymap-XXXXXX");
    if (!mkdtemp(g_home)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("SOULX_HOME", g_home, 1);             // never touch the real ~/.soulx-android

    test_key_names();
    test_mac_keycodes();
    test_hid_usages();
    test_parse_ok();
    test_parse_errors();
    test_format();
    test_files();
    test_runtime_basic();
    test_runtime_joystick();
    test_runtime_fingers();

    nftw(g_home, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
    if (g_failures) {
        fprintf(stderr, "test_keymap: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_keymap: all %d checks passed\n", g_checks);
    return 0;
}
