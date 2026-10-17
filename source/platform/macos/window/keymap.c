/*
 * SoulX Android Emulator - game key mapping for SoulX's window (keyboard -> touches)
 *
 * Pure C (no AppKit): the key table (names, macOS kVK_* virtual key codes, HID usages), the keymap file format
 * (parse / format / load / save) and the runtime state machine that turns key events into touch actions.
 * API and file format: keymap.h.
 *
 * Key names: every name listed in keymap.h, plus DELETE (forward delete) HOME END PAGEUP PAGEDOWN INSERT
 * CAPSLOCK LMETA RMETA (Command) MENU SECTION (ISO section key) F13-F20 NUMLOCK (keypad Clear) KP0-KP9 KPDOT
 * KPENTER KPPLUS KPMINUS KPMULTIPLY KPDIVIDE KPEQUAL, so the window can map every key of a Mac keyboard to a HID
 * usage. Parsing also accepts the aliases ESCAPE RETURN LCMD RCMD LOPTION ROPTION; formatting writes the
 * canonical names. Numbers are parsed and written without the C library's locale (always '.' as the decimal
 * separator).
 *
 * Runtime: every binding that uses a key reacts to it (two bindings may share a key). Each held tap, each active
 * joystick and each held swipe key owns one finger of 1..8; fingers are handed out least-recently-released
 * first, so a finger that just went up (or whose swipe may still be running) is reused last. A key event needs
 * at most 2 * 8 = 16 actions; with a smaller buffer a binding that does not fit is skipped and keeps its state.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "soulx_mac.h"
#include "keymap.h"

#define KM_LINE_MAX         512             // longest binding line (comments may be longer)
#define KM_TOKENS_MAX       8               // "joystick" + 7 values
#define KM_FILE_MAX         (256 * 1024)    // keymap files are a few KB
#define KM_TEXT_MAX         (24 * 1024)     // formatted keymap (64 bindings) always fits
#define KM_NAME_MAX         16              // longest key name or alias accepted
#define KM_NUM_DIGITS_MAX   15              // digits of a decimal number (keeps the mantissa exact)
#define KM_SWIPE_MS_MIN     1
#define KM_SWIPE_MS_MAX     5000
#define KM_RADIUS_MAX       0.5
#define KM_FRACTION_SCALE   10000           // written with 4 decimals
#define KM_SQRT1_2          0.70710678118654752440

// Joystick key slots in SoulXKeyBinding.keys and the runtime's pressed bit mask
#define JOY_UP              0
#define JOY_LEFT            1
#define JOY_DOWN            2
#define JOY_RIGHT           3

// ---------------------------------------------------------------------------
// Key table. SoulXKey code = index + 1 (0 is never a key, so a zeroed binding has no key).
// ---------------------------------------------------------------------------

typedef struct {
    const char* name;           // canonical upper-case name
    unsigned char hid;          // HID usage, Keyboard/Keypad page 0x07
    short mac;                  // kVK_* virtual key code (Carbon HIToolbox/Events.h), -1 = none
} KeyDef;

static const KeyDef k_keys[] = {
    // letters
    { "A", 0x04, 0x00 }, { "B", 0x05, 0x0B }, { "C", 0x06, 0x08 }, { "D", 0x07, 0x02 },
    { "E", 0x08, 0x0E }, { "F", 0x09, 0x03 }, { "G", 0x0A, 0x05 }, { "H", 0x0B, 0x04 },
    { "I", 0x0C, 0x22 }, { "J", 0x0D, 0x26 }, { "K", 0x0E, 0x28 }, { "L", 0x0F, 0x25 },
    { "M", 0x10, 0x2E }, { "N", 0x11, 0x2D }, { "O", 0x12, 0x1F }, { "P", 0x13, 0x23 },
    { "Q", 0x14, 0x0C }, { "R", 0x15, 0x0F }, { "S", 0x16, 0x01 }, { "T", 0x17, 0x11 },
    { "U", 0x18, 0x20 }, { "V", 0x19, 0x09 }, { "W", 0x1A, 0x0D }, { "X", 0x1B, 0x07 },
    { "Y", 0x1C, 0x10 }, { "Z", 0x1D, 0x06 },
    // digit row
    { "0", 0x27, 0x1D }, { "1", 0x1E, 0x12 }, { "2", 0x1F, 0x13 }, { "3", 0x20, 0x14 },
    { "4", 0x21, 0x15 }, { "5", 0x22, 0x17 }, { "6", 0x23, 0x16 }, { "7", 0x24, 0x1A },
    { "8", 0x25, 0x1C }, { "9", 0x26, 0x19 },
    // function keys
    { "F1", 0x3A, 0x7A }, { "F2", 0x3B, 0x78 }, { "F3", 0x3C, 0x63 }, { "F4", 0x3D, 0x76 },
    { "F5", 0x3E, 0x60 }, { "F6", 0x3F, 0x61 }, { "F7", 0x40, 0x62 }, { "F8", 0x41, 0x64 },
    { "F9", 0x42, 0x65 }, { "F10", 0x43, 0x6D }, { "F11", 0x44, 0x67 }, { "F12", 0x45, 0x6F },
    // editing and navigation
    { "SPACE", 0x2C, 0x31 }, { "ENTER", 0x28, 0x24 }, { "TAB", 0x2B, 0x30 }, { "ESC", 0x29, 0x35 },
    { "BACKSPACE", 0x2A, 0x33 },
    { "UP", 0x52, 0x7E }, { "DOWN", 0x51, 0x7D }, { "LEFT", 0x50, 0x7B }, { "RIGHT", 0x4F, 0x7C },
    // modifiers
    { "LSHIFT", 0xE1, 0x38 }, { "RSHIFT", 0xE5, 0x3C }, { "LCTRL", 0xE0, 0x3B }, { "RCTRL", 0xE4, 0x3E },
    { "LALT", 0xE2, 0x3A }, { "RALT", 0xE6, 0x3D },
    // punctuation
    { "MINUS", 0x2D, 0x1B }, { "EQUAL", 0x2E, 0x18 }, { "LBRACKET", 0x2F, 0x21 }, { "RBRACKET", 0x30, 0x1E },
    { "BACKSLASH", 0x31, 0x2A }, { "SEMICOLON", 0x33, 0x29 }, { "QUOTE", 0x34, 0x27 }, { "COMMA", 0x36, 0x2B },
    { "PERIOD", 0x37, 0x2F }, { "SLASH", 0x38, 0x2C }, { "GRAVE", 0x35, 0x32 },
    // --- extensions (not listed in keymap.h): the rest of a Mac keyboard ---
    { "DELETE", 0x4C, 0x75 }, { "HOME", 0x4A, 0x73 }, { "END", 0x4D, 0x77 }, { "PAGEUP", 0x4B, 0x74 },
    { "PAGEDOWN", 0x4E, 0x79 },
    { "INSERT", 0x49, 0x72 },           // kVK_Help sits where Insert is on a PC keyboard
    { "CAPSLOCK", 0x39, 0x39 },
    { "LMETA", 0xE3, 0x37 }, { "RMETA", 0xE7, 0x36 },   // Command keys
    { "MENU", 0x65, 0x6E },             // kVK_ContextualMenu -> Keyboard Application
    { "SECTION", 0x64, 0x0A },          // kVK_ISO_Section -> Keyboard Non-US \ and |
    { "F13", 0x68, 0x69 }, { "F14", 0x69, 0x6B }, { "F15", 0x6A, 0x71 }, { "F16", 0x6B, 0x6A },
    { "F17", 0x6C, 0x40 }, { "F18", 0x6D, 0x4F }, { "F19", 0x6E, 0x50 }, { "F20", 0x6F, 0x5A },
    { "NUMLOCK", 0x53, 0x47 },          // keypad Clear -> Keypad Num Lock and Clear
    { "KP0", 0x62, 0x52 }, { "KP1", 0x59, 0x53 }, { "KP2", 0x5A, 0x54 }, { "KP3", 0x5B, 0x55 },
    { "KP4", 0x5C, 0x56 }, { "KP5", 0x5D, 0x57 }, { "KP6", 0x5E, 0x58 }, { "KP7", 0x5F, 0x59 },
    { "KP8", 0x60, 0x5B }, { "KP9", 0x61, 0x5C },
    { "KPDOT", 0x63, 0x41 }, { "KPENTER", 0x58, 0x4C }, { "KPPLUS", 0x57, 0x45 }, { "KPMINUS", 0x56, 0x4E },
    { "KPMULTIPLY", 0x55, 0x43 }, { "KPDIVIDE", 0x54, 0x4B }, { "KPEQUAL", 0x67, 0x51 },
};

#define KEY_COUNT ((int)(sizeof(k_keys) / sizeof(k_keys[0])))

static const struct {
    const char* alias;
    const char* name;
} k_aliases[] = {
    { "ESCAPE", "ESC" }, { "RETURN", "ENTER" }, { "LCMD", "LMETA" }, { "RCMD", "RMETA" },
    { "LOPTION", "LALT" }, { "ROPTION", "RALT" },
};

static bool key_valid(int key) {
    return key >= 1 && key <= KEY_COUNT;
}

static char ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

// ASCII-only case-insensitive comparison (strcasecmp follows the locale).
static bool name_equal(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        if (ascii_upper(*a) != ascii_upper(*b)) return false;
    }
    return *a == *b;
}

int soulx_key_from_name(const char* name) {
    if (!name || !name[0] || strnlen(name, KM_NAME_MAX + 1) > KM_NAME_MAX) return -1;
    for (size_t i = 0; i < sizeof(k_aliases) / sizeof(k_aliases[0]); i++) {
        if (name_equal(name, k_aliases[i].alias)) {
            name = k_aliases[i].name;
            break;
        }
    }
    for (int i = 0; i < KEY_COUNT; i++) {
        if (name_equal(name, k_keys[i].name)) return i + 1;
    }
    return -1;
}

const char* soulx_key_name(int key) {
    return key_valid(key) ? k_keys[key - 1].name : NULL;
}

int soulx_key_from_mac_keycode(unsigned short keycode) {
    for (int i = 0; i < KEY_COUNT; i++) {
        if (k_keys[i].mac >= 0 && (unsigned short)k_keys[i].mac == keycode) return i + 1;
    }
    return -1;
}

int soulx_key_hid_usage(int key) {
    return key_valid(key) ? k_keys[key - 1].hid : -1;
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

static const char* kind_name(SoulXKeymapKind kind) {
    switch (kind) {
    case SOULX_KM_TAP:      return "tap";
    case SOULX_KM_JOYSTICK: return "joystick";
    case SOULX_KM_SWIPE:    return "swipe";
    }
    return NULL;
}

static bool fraction_ok(double v) {
    return v >= 0.0 && v <= 1.0;                // false for NaN
}

// NULL when the binding is usable, else what is wrong with it.
static const char* binding_problem(const SoulXKeyBinding* b) {
    switch (b->kind) {
    case SOULX_KM_TAP:
        if (!key_valid(b->keys[0])) return "invalid key";
        if (!fraction_ok(b->x) || !fraction_ok(b->y)) return "x and y must be between 0 and 1";
        return NULL;
    case SOULX_KM_SWIPE:
        if (!key_valid(b->keys[0])) return "invalid key";
        if (!fraction_ok(b->x) || !fraction_ok(b->y) || !fraction_ok(b->x2) || !fraction_ok(b->y2)) {
            return "x1, y1, x2 and y2 must be between 0 and 1";
        }
        if (b->ms < KM_SWIPE_MS_MIN || b->ms > KM_SWIPE_MS_MAX) return "the duration must be 1..5000 ms";
        return NULL;
    case SOULX_KM_JOYSTICK:
        for (int i = 0; i < 4; i++) {
            if (!key_valid(b->keys[i])) return "invalid key";
            for (int j = 0; j < i; j++) {
                if (b->keys[i] == b->keys[j]) return "the four joystick keys must be different";
            }
        }
        if (!fraction_ok(b->x) || !fraction_ok(b->y)) return "cx and cy must be between 0 and 1";
        if (!(b->radius > 0.0 && b->radius <= KM_RADIUS_MAX)) return "the radius must be greater than 0 and at most 0.5";
        return NULL;
    }
    return "unknown binding type";
}

// Number of SoulXKeyBinding.keys entries the binding uses.
static int binding_key_count(const SoulXKeyBinding* b) {
    return b->kind == SOULX_KM_JOYSTICK ? 4 : 1;
}

// ---------------------------------------------------------------------------
// Numbers (no locale: '.' is always the decimal separator)
// ---------------------------------------------------------------------------

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// Plain decimal: "0.85", "1", ".5", "0." - no sign, exponent or thousands separator. At most 15 significant
// digits and 15 decimals, so the mantissa and the power of ten are exact.
static bool parse_decimal(const char* s, double* out) {
    static const double k_pow10[KM_NUM_DIGITS_MAX + 1] = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    };
    uint64_t mant = 0;
    int digits = 0;
    int significant = 0;
    int frac = 0;
    bool point = false;
    for (const char* p = s; *p; p++) {
        if (*p == '.') {
            if (point) return false;
            point = true;
            continue;
        }
        if (!is_digit(*p)) return false;
        digits++;
        if (point && ++frac > KM_NUM_DIGITS_MAX) return false;
        if (mant == 0 && *p == '0') continue;   // leading zero
        if (++significant > KM_NUM_DIGITS_MAX) return false;
        mant = mant * 10u + (uint64_t)(*p - '0');
    }
    if (digits == 0) return false;
    *out = (double)mant / k_pow10[frac];        // one correctly rounded division: "0.85" == 0.85
    return true;
}

static bool parse_int(const char* s, int lo, int hi, int* out) {
    long v = 0;
    int digits = 0;
    for (const char* p = s; *p; p++) {
        if (!is_digit(*p) || ++digits > 9) return false;
        v = v * 10 + (*p - '0');
    }
    if (digits == 0 || v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

// Fraction 0..1 with 4 decimals, trailing zeros trimmed down to 2: 0.5 -> "0.50", 0.125 -> "0.125".
static void format_fraction(double v, char out[16]) {
    if (!(v >= 0.0)) v = 0.0;
    if (v > 1.0) v = 1.0;
    long n = lround(v * KM_FRACTION_SCALE);
    char frac[8];
    snprintf(frac, sizeof(frac), "%04ld", n % KM_FRACTION_SCALE);
    int len = 4;
    while (len > 2 && frac[len - 1] == '0') len--;
    snprintf(out, 16, "%ld.%.*s", n / KM_FRACTION_SCALE, len, frac);
}

// ---------------------------------------------------------------------------
// Parse / format
// ---------------------------------------------------------------------------

static void set_err(char* err, size_t errcap, int line, const char* fmt, ...) __attribute__((format(printf, 4, 5)));

static void set_err(char* err, size_t errcap, int line, const char* fmt, ...) {
    if (!err || errcap == 0) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (line > 0) snprintf(err, errcap, "line %d: %s", line, msg);
    else snprintf(err, errcap, "%s", msg);
}

// Token for an error message: printable, at most 24 characters.
static const char* shown(const char* tok, char buf[32]) {
    size_t n = 0;
    for (const char* p = tok; *p && n < 24; p++) buf[n++] = (*p >= 0x20 && *p < 0x7F) ? *p : '?';
    if (strlen(tok) > 24) {
        buf[n++] = '.';
        buf[n++] = '.';
        buf[n++] = '.';
    }
    buf[n] = '\0';
    return buf;
}

static bool parse_fraction_tok(const char* tok, double* out) {
    return parse_decimal(tok, out) && fraction_ok(*out);
}

// Parses the tokens of one binding line into b. 0 ok, -1 with err set.
static int parse_binding(char** tok, int ntok, int line, SoulXKeyBinding* b, char* err, size_t errcap) {
    char show[32];
    memset(b, 0, sizeof(*b));
    const char* kind = tok[0];
    int want;
    if (name_equal(kind, "tap")) {
        b->kind = SOULX_KM_TAP;
        want = 4;
    } else if (name_equal(kind, "joystick")) {
        b->kind = SOULX_KM_JOYSTICK;
        want = 8;
    } else if (name_equal(kind, "swipe")) {
        b->kind = SOULX_KM_SWIPE;
        want = 7;
    } else {
        set_err(err, errcap, line, "unknown binding type '%s' (expected tap, joystick or swipe)", shown(kind, show));
        return -1;
    }
    if (ntok != want) {
        static const char* const usage[] = {
            "tap <key> <x> <y>",
            "joystick <up> <left> <down> <right> <cx> <cy> <radius>",
            "swipe <key> <x1> <y1> <x2> <y2> <ms>",
        };
        set_err(err, errcap, line, "%s %s values: %s", kind_name(b->kind), ntok < want ? "needs more" : "has too many",
                usage[b->kind]);
        return -1;
    }

    int nkeys = binding_key_count(b);
    for (int i = 0; i < nkeys; i++) {
        b->keys[i] = soulx_key_from_name(tok[1 + i]);
        if (b->keys[i] < 0) {
            set_err(err, errcap, line, "unknown key '%s'", shown(tok[1 + i], show));
            return -1;
        }
    }
    const char* const* num = (const char* const*)tok + 1 + nkeys;
    switch (b->kind) {
    case SOULX_KM_TAP:
    case SOULX_KM_JOYSTICK: {
        double* v[2] = { &b->x, &b->y };
        static const char* const tap_names[2] = { "x", "y" };
        static const char* const joy_names[2] = { "cx", "cy" };
        for (int i = 0; i < 2; i++) {
            if (!parse_fraction_tok(num[i], v[i])) {
                set_err(err, errcap, line, "%s must be a number between 0 and 1, not '%s'",
                        b->kind == SOULX_KM_TAP ? tap_names[i] : joy_names[i], shown(num[i], show));
                return -1;
            }
        }
        if (b->kind == SOULX_KM_JOYSTICK) {
            if (!parse_decimal(num[2], &b->radius) || !(b->radius > 0.0 && b->radius <= KM_RADIUS_MAX)) {
                set_err(err, errcap, line, "the radius must be a number greater than 0 and at most 0.5, not '%s'",
                        shown(num[2], show));
                return -1;
            }
        }
        break;
    }
    case SOULX_KM_SWIPE: {
        double* v[4] = { &b->x, &b->y, &b->x2, &b->y2 };
        static const char* const names[4] = { "x1", "y1", "x2", "y2" };
        for (int i = 0; i < 4; i++) {
            if (!parse_fraction_tok(num[i], v[i])) {
                set_err(err, errcap, line, "%s must be a number between 0 and 1, not '%s'", names[i], shown(num[i], show));
                return -1;
            }
        }
        if (!parse_int(num[4], KM_SWIPE_MS_MIN, KM_SWIPE_MS_MAX, &b->ms)) {
            set_err(err, errcap, line, "the duration must be a whole number of milliseconds 1..5000, not '%s'",
                    shown(num[4], show));
            return -1;
        }
        break;
    }
    }
    const char* problem = binding_problem(b);
    if (problem) {
        set_err(err, errcap, line, "%s", problem);
        return -1;
    }
    return 0;
}

int soulx_keymap_parse(const char* text, SoulXKeymap* km, char* err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!km) {
        set_err(err, errcap, 0, "no keymap");
        return -1;
    }
    km->count = 0;                              // the package name is left as it is
    if (!text) {
        set_err(err, errcap, 0, "no text");
        return -1;
    }

    SoulXKeyBinding* parsed = calloc(SOULX_KEYMAP_MAX, sizeof(*parsed));
    if (!parsed) {
        set_err(err, errcap, 0, "out of memory");
        return -1;
    }
    int count = 0;
    int line = 0;
    const char* p = text;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;   // BOM

    while (*p) {
        line++;
        const char* end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        const char* hash = memchr(p, '#', len);
        size_t content = hash ? (size_t)(hash - p) : len;
        if (content > 0 && p[content - 1] == '\r') content--;   // CRLF without a comment
        if (content >= KM_LINE_MAX) {
            set_err(err, errcap, line, "line too long (more than %d characters)", KM_LINE_MAX - 1);
            goto fail;
        }
        char buf[KM_LINE_MAX];
        memcpy(buf, p, content);
        buf[content] = '\0';
        p = end ? end + 1 : p + len;

        char* tok[KM_TOKENS_MAX + 1];
        int ntok = 0;
        bool bad_char = false;
        for (char* q = buf; *q;) {
            unsigned char c = (unsigned char)*q;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
                *q++ = '\0';
                continue;
            }
            if (c < 0x20 || c >= 0x7F) {
                bad_char = true;
                break;
            }
            if (ntok == KM_TOKENS_MAX + 1) break;           // too many: reported below
            tok[ntok++] = q;
            while (*q && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\v' && *q != '\f') {
                c = (unsigned char)*q;
                if (c < 0x20 || c >= 0x7F) {
                    bad_char = true;
                    break;
                }
                q++;
            }
            if (bad_char) break;
        }
        if (bad_char) {
            set_err(err, errcap, line, "unexpected character (only ASCII text is allowed)");
            goto fail;
        }
        if (ntok == 0) continue;                            // blank or comment-only line
        if (count == SOULX_KEYMAP_MAX) {
            set_err(err, errcap, line, "too many bindings (at most %d)", SOULX_KEYMAP_MAX);
            goto fail;
        }
        if (parse_binding(tok, ntok, line, &parsed[count], err, errcap) != 0) goto fail;
        count++;
    }

    memcpy(km->b, parsed, (size_t)count * sizeof(*parsed));
    km->count = count;
    free(parsed);
    return 0;

fail:
    free(parsed);
    return -1;
}

static bool package_ok(const char* package) {
    return package[0] == '\0' || soulx_package_name_valid(package);
}

// Appends with truncation tracking.
typedef struct {
    char* buf;
    size_t cap;
    size_t len;
    bool overflow;
} OutBuf;

static void out_add(OutBuf* o, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

static void out_add(OutBuf* o, const char* fmt, ...) {
    if (o->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->len) {
        o->overflow = true;
        return;
    }
    o->len += (size_t)n;
}

int soulx_keymap_format(const SoulXKeymap* km, char* buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    buf[0] = '\0';
    if (!km || km->count < 0 || km->count > SOULX_KEYMAP_MAX || !memchr(km->package, '\0', sizeof(km->package)) ||
        !package_ok(km->package)) {
        return -1;
    }
    OutBuf o = { buf, cap, 0, false };
    if (km->package[0]) out_add(&o, "# SoulX keymap for %s\n", km->package);
    else out_add(&o, "# SoulX keymap: default (games without their own keymap)\n");
    out_add(&o, "# tap <key> <x> <y> | joystick <up> <left> <down> <right> <cx> <cy> <radius> | "
                "swipe <key> <x1> <y1> <x2> <y2> <ms>\n"
                "# x, y: fractions (0..1) of the upright screen; radius: fraction of its shorter side\n");
    for (int i = 0; i < km->count; i++) {
        const SoulXKeyBinding* b = &km->b[i];
        if (binding_problem(b)) {
            buf[0] = '\0';
            return -1;
        }
        char x[16], y[16];
        format_fraction(b->x, x);
        format_fraction(b->y, y);
        switch (b->kind) {
        case SOULX_KM_TAP:
            out_add(&o, "tap      %s %s %s\n", soulx_key_name(b->keys[0]), x, y);
            break;
        case SOULX_KM_JOYSTICK: {
            char r[16];
            format_fraction(b->radius, r);
            out_add(&o, "joystick %s %s %s %s %s %s %s\n", soulx_key_name(b->keys[JOY_UP]),
                    soulx_key_name(b->keys[JOY_LEFT]), soulx_key_name(b->keys[JOY_DOWN]),
                    soulx_key_name(b->keys[JOY_RIGHT]), x, y, r);
            break;
        }
        case SOULX_KM_SWIPE: {
            char x2[16], y2[16];
            format_fraction(b->x2, x2);
            format_fraction(b->y2, y2);
            out_add(&o, "swipe    %s %s %s %s %s %d\n", soulx_key_name(b->keys[0]), x, y, x2, y2, b->ms);
            break;
        }
        }
    }
    if (o.overflow) {
        buf[0] = '\0';
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Files: $SOULX_HOME/keymaps/<package>.keymap and default.keymap
// ---------------------------------------------------------------------------

static pthread_mutex_t g_warn_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_last_warning[PATH_MAX + 320];

// The window reloads the keymap every 2 s: warn about a broken file once, not every time.
static void warn_once(const char* msg) {
    pthread_mutex_lock(&g_warn_lock);
    bool fresh = strcmp(msg, g_last_warning) != 0;
    if (fresh) snprintf(g_last_warning, sizeof(g_last_warning), "%s", msg);
    pthread_mutex_unlock(&g_warn_lock);
    if (fresh) soulx_log(SOULX_LOG_WARN, "%s", msg);
}

// Reads a small text file (symlinks are followed: a keymap may be shared between games). 0 ok, 1 missing,
// -1 unusable.
static int read_text(const char* path, char** text) {
    *text = NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > KM_FILE_MAX) {
        close(fd);
        return -1;
    }
    size_t cap = (size_t)st.st_size + 1;
    char* buf = malloc(cap);
    if (!buf) {
        close(fd);
        return -1;
    }
    size_t n = 0;
    for (;;) {
        if (n + 1 >= cap) break;                // the file grew: keep what fits
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) {
            free(buf);
            close(fd);
            return -1;
        }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = '\0';
    if (strlen(buf) != n) {                     // embedded NUL: not a text file
        free(buf);
        return -1;
    }
    *text = buf;
    return 0;
}

// 0 loaded, 1 missing, -1 unreadable or invalid (warned).
static int load_file(const char* path, SoulXKeymap* km) {
    char* text = NULL;
    int rc = read_text(path, &text);
    if (rc == 1) return 1;
    if (rc != 0) {
        char msg[PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "cannot read the keymap %s", path);
        warn_once(msg);
        return -1;
    }
    char err[256];
    rc = soulx_keymap_parse(text, km, err, sizeof(err));
    free(text);
    if (rc != 0) {
        char msg[PATH_MAX + 320];
        snprintf(msg, sizeof(msg), "keymap %s: %s", path, err);
        warn_once(msg);
        km->count = 0;
        return -1;
    }
    return 0;
}

int soulx_keymap_load(const char* package, SoulXKeymap* km) {
    if (!km) return -1;
    memset(km, 0, sizeof(*km));
    char path[PATH_MAX];
    if (package && package[0] && soulx_package_name_valid(package)) {
        if (soulx_path(path, sizeof(path), "keymaps/%s.keymap", package) != 0) return -1;
        int rc = load_file(path, km);
        if (rc < 0) return -1;
        if (rc == 0) {
            snprintf(km->package, sizeof(km->package), "%s", package);
            return 0;
        }
    } else if (package && package[0]) {
        soulx_log(SOULX_LOG_DEBUG, "keymap: ignoring the invalid package name '%.64s'", package);
    }
    if (soulx_path(path, sizeof(path), "keymaps/default.keymap") != 0) return -1;
    int rc = load_file(path, km);
    if (rc < 0) return -1;
    return rc == 0 ? 1 : 2;
}

int soulx_keymap_save(const SoulXKeymap* km) {
    if (!km || !memchr(km->package, '\0', sizeof(km->package)) || !package_ok(km->package)) return -1;
    char dir[PATH_MAX];
    char path[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "keymaps") != 0 ||
        soulx_path(path, sizeof(path), "keymaps/%s.keymap", km->package[0] ? km->package : "default") != 0) {
        return -1;
    }
    char* text = malloc(KM_TEXT_MAX);
    if (!text) return -1;
    int rc = soulx_keymap_format(km, text, KM_TEXT_MAX);
    if (rc == 0 && soulx_mkdirs(dir) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", dir, strerror(errno));
        rc = -1;
    }
    if (rc == 0 && soulx_write_file_atomic(path, text, strlen(text)) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot write %s: %s", path, strerror(errno));
        rc = -1;
    }
    free(text);
    return rc;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

typedef struct {
    unsigned pressed;           // bit i = keys[i] held (tap / swipe: bit 0)
    int finger;                 // 0 = none
    double x, y;                // joystick: where its finger is now
} BindState;

struct SoulXKeymapRuntime {
    SoulXKeymap km;              // valid bindings only
    BindState st[SOULX_KEYMAP_MAX];
    bool busy[SOULX_KEYMAP_LAST_FINGER + 1];
    unsigned long released[SOULX_KEYMAP_LAST_FINGER + 1];     // release order (0 = never used)
    unsigned long clock;
    int screen_w;               // logical screen size for the joystick radius (0 = unknown)
    int screen_h;
};

// Exported, not in keymap.h yet (contract gap, see the W1 report): the logical screen size in pixels. A joystick
// radius is a fraction of the shorter side; with the size known the finger moves on a circle, without it the
// radius is applied to both fractions as is (exact on the shorter axis only).
void soulx_keymap_runtime_set_screen(SoulXKeymapRuntime* rt, int width, int height);

SoulXKeymapRuntime* soulx_keymap_runtime_new(const SoulXKeymap* km) {
    if (!km) return NULL;
    SoulXKeymapRuntime* rt = calloc(1, sizeof(*rt));
    if (!rt) return NULL;
    if (memchr(km->package, '\0', sizeof(km->package))) memcpy(rt->km.package, km->package, sizeof(km->package));
    int count = km->count < 0 ? 0 : km->count > SOULX_KEYMAP_MAX ? SOULX_KEYMAP_MAX : km->count;
    for (int i = 0; i < count; i++) {
        if (binding_problem(&km->b[i])) {
            soulx_log(SOULX_LOG_DEBUG, "keymap: skipping invalid binding %d", i + 1);
            continue;
        }
        rt->km.b[rt->km.count++] = km->b[i];
    }
    return rt;
}

void soulx_keymap_runtime_free(SoulXKeymapRuntime* rt) {
    free(rt);
}

void soulx_keymap_runtime_set_screen(SoulXKeymapRuntime* rt, int width, int height) {
    if (!rt) return;
    bool ok = width > 0 && height > 0 && width <= 65535 && height <= 65535;
    rt->screen_w = ok ? width : 0;
    rt->screen_h = ok ? height : 0;
}

// Bit of |key| in the binding's pressed mask, 0 when the binding does not use the key.
static unsigned key_bit(const SoulXKeyBinding* b, int key) {
    unsigned bits = 0;
    for (int i = 0; i < binding_key_count(b); i++) {
        if (b->keys[i] == key) bits |= 1u << i;
    }
    return bits;
}

bool soulx_keymap_binds(const SoulXKeymapRuntime* rt, int key) {
    if (!rt || !key_valid(key)) return false;
    for (int i = 0; i < rt->km.count; i++) {
        if (key_bit(&rt->km.b[i], key)) return true;
    }
    return false;
}

static int finger_alloc(SoulXKeymapRuntime* rt) {
    int best = 0;
    for (int f = SOULX_KEYMAP_FIRST_FINGER; f <= SOULX_KEYMAP_LAST_FINGER; f++) {
        if (rt->busy[f]) continue;
        if (!best || rt->released[f] < rt->released[best]) best = f;
    }
    if (best) rt->busy[best] = true;
    return best;
}

static void finger_free(SoulXKeymapRuntime* rt, int f) {
    if (f < SOULX_KEYMAP_FIRST_FINGER || f > SOULX_KEYMAP_LAST_FINGER) return;
    rt->busy[f] = false;
    rt->released[f] = ++rt->clock;
}

static double clamp01(double v) {
    return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
}

// Where the joystick finger goes for a pressed mask: the centre plus radius * unit direction (diagonals
// normalised, opposite keys cancel).
static void joystick_target(const SoulXKeymapRuntime* rt, const SoulXKeyBinding* b, unsigned pressed, double* x, double* y) {
    int dx = 0;
    int dy = 0;
    if (pressed & (1u << JOY_UP)) dy -= 1;
    if (pressed & (1u << JOY_DOWN)) dy += 1;
    if (pressed & (1u << JOY_LEFT)) dx -= 1;
    if (pressed & (1u << JOY_RIGHT)) dx += 1;
    double ux = dx;
    double uy = dy;
    if (dx && dy) {
        ux *= KM_SQRT1_2;
        uy *= KM_SQRT1_2;
    }
    double rx = b->radius;
    double ry = b->radius;
    if (rt->screen_w > 0 && rt->screen_h > 0) {
        double shorter = rt->screen_w < rt->screen_h ? rt->screen_w : rt->screen_h;
        rx = b->radius * shorter / rt->screen_w;
        ry = b->radius * shorter / rt->screen_h;
    }
    *x = clamp01(b->x + ux * rx);
    *y = clamp01(b->y + uy * ry);
}

static void emit_touch(SoulXKeymapAction* a, int finger, int action, double x, double y) {
    memset(a, 0, sizeof(*a));
    a->kind = SOULX_KM_ACT_TOUCH;
    a->finger = finger;
    a->action = action;
    a->x = x;
    a->y = y;
}

int soulx_keymap_key_event(SoulXKeymapRuntime* rt, int key, bool down, SoulXKeymapAction* out, int max) {
    if (!rt || !out || max <= 0 || !key_valid(key)) return 0;
    int n = 0;
    for (int i = 0; i < rt->km.count; i++) {
        const SoulXKeyBinding* b = &rt->km.b[i];
        BindState* s = &rt->st[i];
        unsigned bit = key_bit(b, key);
        if (!bit) continue;

        switch (b->kind) {
        case SOULX_KM_TAP:
            if (down) {
                if (s->pressed || n + 1 > max) break;          // key repeat / no room
                int f = finger_alloc(rt);
                if (!f) break;                                  // all keymap fingers are in use
                s->pressed = 1;
                s->finger = f;
                emit_touch(&out[n++], f, SOULX_TOUCH_DOWN, b->x, b->y);
            } else {
                if (!s->pressed || n + 1 > max) break;
                emit_touch(&out[n++], s->finger, SOULX_TOUCH_UP, b->x, b->y);
                finger_free(rt, s->finger);
                s->pressed = 0;
                s->finger = 0;
            }
            break;

        case SOULX_KM_SWIPE:
            if (down) {
                if (s->pressed || n + 1 > max) break;
                int f = finger_alloc(rt);
                if (!f) break;
                s->pressed = 1;
                s->finger = f;
                SoulXKeymapAction* a = &out[n++];
                memset(a, 0, sizeof(*a));
                a->kind = SOULX_KM_ACT_SWIPE;
                a->finger = f;
                a->x = b->x;
                a->y = b->y;
                a->x2 = b->x2;
                a->y2 = b->y2;
                a->ms = b->ms;
            } else if (s->pressed) {                            // the swipe ends by itself: free the finger
                finger_free(rt, s->finger);
                s->pressed = 0;
                s->finger = 0;
            }
            break;

        case SOULX_KM_JOYSTICK: {
            unsigned now = down ? (s->pressed | bit) : (s->pressed & ~bit);
            if (now == s->pressed) break;                       // key repeat, or a key that was not held
            double tx, ty;
            if (s->pressed == 0) {                              // first key: touch the centre, then push
                if (n + 2 > max) break;
                int f = finger_alloc(rt);
                if (!f) break;
                s->finger = f;
                s->pressed = now;
                emit_touch(&out[n++], f, SOULX_TOUCH_DOWN, b->x, b->y);
                joystick_target(rt, b, now, &tx, &ty);
                s->x = b->x;
                s->y = b->y;
                if (tx != s->x || ty != s->y) {
                    emit_touch(&out[n++], f, SOULX_TOUCH_MOVE, tx, ty);
                    s->x = tx;
                    s->y = ty;
                }
            } else if (now == 0) {                              // last key released: lift the finger
                if (n + 1 > max) break;
                emit_touch(&out[n++], s->finger, SOULX_TOUCH_UP, s->x, s->y);
                finger_free(rt, s->finger);
                s->pressed = 0;
                s->finger = 0;
            } else {                                            // direction change
                joystick_target(rt, b, now, &tx, &ty);
                if (tx != s->x || ty != s->y) {
                    if (n + 1 > max) break;
                    emit_touch(&out[n++], s->finger, SOULX_TOUCH_MOVE, tx, ty);
                    s->x = tx;
                    s->y = ty;
                }
                s->pressed = now;
            }
            break;
        }
        }
    }
    return n;
}

int soulx_keymap_release_all(SoulXKeymapRuntime* rt, SoulXKeymapAction* out, int max) {
    if (!rt) return 0;
    int n = 0;
    for (int i = 0; i < rt->km.count; i++) {
        const SoulXKeyBinding* b = &rt->km.b[i];
        BindState* s = &rt->st[i];
        if (!s->pressed) continue;
        if (b->kind != SOULX_KM_SWIPE) {
            if (!out || n >= max) continue;                     // no room: still held, a later call lifts it
            if (b->kind == SOULX_KM_TAP) emit_touch(&out[n++], s->finger, SOULX_TOUCH_UP, b->x, b->y);
            else emit_touch(&out[n++], s->finger, SOULX_TOUCH_UP, s->x, s->y);
        }
        finger_free(rt, s->finger);
        s->pressed = 0;
        s->finger = 0;
    }
    return n;
}
