/*
 * SoulX Android Emulator - unit tests for the pure functions of source/platform/macos/hidkbd.c
 *
 * hidkbd.c drives a virtual USB HID keyboard through a live 'adb shell hid -' child; that live path needs a guest
 * and is verified on a real instance. Here we test only the pure encoders, which have no I/O:
 *   - soulx_hidkbd_usage_for_char: the ASCII -> HID usage table (shift handling), exhaustively over 0..255,
 *   - soulx_hidkbd_kbd_report: the 9-byte boot-keyboard report (modifier folding, dedup, skips, the 6-key limit),
 *   - soulx_hidkbd_json_register / _report / _delay: the JSON lines sent to the hid tool,
 *   - soulx_hidkbd_parse_nodes: picking the device nodes of "SoulX Virtual Keyboard" out of 'getevent -lp' output.
 *
 * The encoders are checked byte for byte against an independent table.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "soulx_mac.h"   // soulx_hidkbd_usage_for_char + SOULX_HID_MOD_*

// hidkbd.c exports these for the test (not part of soulx_mac.h):
#define REPORT_LEN 9
int soulx_hidkbd_kbd_report(int modifiers, const int* usages, int count, unsigned char report[REPORT_LEN]);
int soulx_hidkbd_json_register(char* buf, size_t cap);
int soulx_hidkbd_json_report(const unsigned char* report, size_t len, char* buf, size_t cap);
int soulx_hidkbd_json_delay(int ms, char* buf, size_t cap);
int soulx_hidkbd_parse_nodes(const char* out, const char* name, char* nodes, size_t cap);

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

// ---------------------------------------------------------------------------
// char -> HID usage (an independent oracle)
// ---------------------------------------------------------------------------

typedef struct {
    char c;
    int usage;
    bool shift;
} CharUsage;

static const CharUsage k_plain[] = {
    { ' ', 0x2C, false }, { '-', 0x2D, false }, { '=', 0x2E, false }, { '[', 0x2F, false },
    { ']', 0x30, false }, { '\\', 0x31, false }, { ';', 0x33, false }, { '\'', 0x34, false },
    { '`', 0x35, false }, { ',', 0x36, false }, { '.', 0x37, false }, { '/', 0x38, false },
    { '\n', 0x28, false }, { '\t', 0x2B, false },
};
static const CharUsage k_shifted[] = {
    { '!', 0x1E, true }, { '@', 0x1F, true }, { '#', 0x20, true }, { '$', 0x21, true }, { '%', 0x22, true },
    { '^', 0x23, true }, { '&', 0x24, true }, { '*', 0x25, true }, { '(', 0x26, true }, { ')', 0x27, true },
    { '_', 0x2D, true }, { '+', 0x2E, true }, { '{', 0x2F, true }, { '}', 0x30, true }, { '|', 0x31, true },
    { ':', 0x33, true }, { '"', 0x34, true }, { '~', 0x35, true }, { '<', 0x36, true }, { '>', 0x37, true },
    { '?', 0x38, true },
};

// The expected result for one byte value, -1 usage when unsupported.
static bool oracle(int value, int* usage, bool* shift) {
    *usage = -1;
    *shift = false;
    if (value >= 'a' && value <= 'z') {
        *usage = 0x04 + (value - 'a');
        return true;
    }
    if (value >= 'A' && value <= 'Z') {
        *usage = 0x04 + (value - 'A');
        *shift = true;
        return true;
    }
    if (value >= '1' && value <= '9') {
        *usage = 0x1E + (value - '1');
        return true;
    }
    if (value == '0') {
        *usage = 0x27;
        return true;
    }
    for (size_t i = 0; i < sizeof(k_plain) / sizeof(k_plain[0]); i++) {
        if ((int)(unsigned char)k_plain[i].c == value) {
            *usage = k_plain[i].usage;
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(k_shifted) / sizeof(k_shifted[0]); i++) {
        if ((int)(unsigned char)k_shifted[i].c == value) {
            *usage = k_shifted[i].usage;
            *shift = true;
            return true;
        }
    }
    return false;
}

static void test_usage_for_char(void) {
    int supported = 0;
    for (int value = 0; value < 256; value++) {
        char c = (char)value;
        int exp_u = -1;
        bool exp_s = false;
        bool exp_ok = oracle(value, &exp_u, &exp_s);

        int usage = 0x1234;
        bool shift = true;
        int rc = soulx_hidkbd_usage_for_char(c, &usage, &shift);
        if (exp_ok) {
            supported++;
            CHECK(rc == 0);
            CHECK(usage == exp_u);
            CHECK(shift == exp_s);
            if (rc != 0 || usage != exp_u || shift != exp_s) {
                fprintf(stderr, "  value %d: rc=%d usage=0x%X shift=%d, expected usage=0x%X shift=%d\n",
                        value, rc, usage, shift, exp_u, exp_s);
            }
        } else {
            CHECK(rc == -1);
        }
        // the out-pointers are optional
        CHECK(soulx_hidkbd_usage_for_char(c, NULL, NULL) == (exp_ok ? 0 : -1));
        CHECK(soulx_hidkbd_usage_for_char(c, &usage, NULL) == (exp_ok ? 0 : -1));
    }
    CHECK(supported == 26 + 26 + 10 + (int)(sizeof(k_plain) / sizeof(k_plain[0])) +
                       (int)(sizeof(k_shifted) / sizeof(k_shifted[0])));   // 26+26+10+14+21 = 97
    // every printable ASCII except backspace/DEL is covered; NUL and other control bytes are not
    CHECK(soulx_hidkbd_usage_for_char('\0', NULL, NULL) == -1);
    CHECK(soulx_hidkbd_usage_for_char('\r', NULL, NULL) == -1);
    CHECK(soulx_hidkbd_usage_for_char((char)0x7F, NULL, NULL) == -1);
    CHECK(soulx_hidkbd_usage_for_char((char)0x1B, NULL, NULL) == -1);     // ESC is not a printable char
}

// ---------------------------------------------------------------------------
// keyboard report
// ---------------------------------------------------------------------------

static bool report_is(const unsigned char* r, unsigned char mods, const unsigned char* keys, int nkeys) {
    if (r[0] != 1 || r[1] != mods || r[2] != 0) return false;
    for (int i = 0; i < 6; i++) {
        unsigned char want = i < nkeys ? keys[i] : 0;
        if (r[3 + i] != want) return false;
    }
    return true;
}

static void test_kbd_report(void) {
    unsigned char r[REPORT_LEN];

    CHECK(soulx_hidkbd_kbd_report(0, NULL, 0, r) == 0);
    CHECK(report_is(r, 0, NULL, 0));

    int one[] = { 0x04 };
    CHECK(soulx_hidkbd_kbd_report(0, one, 1, r) == 0);
    unsigned char k1[] = { 0x04 };
    CHECK(report_is(r, 0, k1, 1));

    // explicit modifier bits
    CHECK(soulx_hidkbd_kbd_report(SOULX_HID_MOD_LSHIFT, one, 1, r) == 0);
    CHECK(report_is(r, 0x02, k1, 1));
    CHECK(soulx_hidkbd_kbd_report(0xFF, NULL, 0, r) == 0 && r[1] == 0xFF);

    // a usage in 0xE0..0xE7 folds into a modifier bit (LCtrl .. RGUI = bits 0..7)
    int mods_as_usage[] = { 0xE0, 0xE2, 0xE7 };   // LCtrl (0x01), LAlt (0x04), RGUI (0x80)
    CHECK(soulx_hidkbd_kbd_report(0, mods_as_usage, 3, r) == 0);
    CHECK(report_is(r, 0x01 | 0x04 | 0x80, NULL, 0));

    // mix: explicit modifier + a modifier-usage + two keys
    int mixed[] = { 0xE2 /*LAlt 0x04*/, 0x04 /*a*/, 0x05 /*b*/ };
    CHECK(soulx_hidkbd_kbd_report(SOULX_HID_MOD_LCTRL, mixed, 3, r) == 0);
    unsigned char ab[] = { 0x04, 0x05 };
    CHECK(report_is(r, 0x01 | 0x04, ab, 2));

    // exactly six keys
    int six[] = { 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 };
    CHECK(soulx_hidkbd_kbd_report(0, six, 6, r) == 0);
    unsigned char sixk[] = { 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 };
    CHECK(report_is(r, 0, sixk, 6));

    // seven distinct keys overflow
    int seven[] = { 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A };
    CHECK(soulx_hidkbd_kbd_report(0, seven, 7, r) == -1);

    // six keys plus a modifier usage still fit (the modifier does not use a key slot)
    int six_plus_mod[] = { 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0xE0 };
    CHECK(soulx_hidkbd_kbd_report(0, six_plus_mod, 7, r) == 0);
    CHECK(report_is(r, 0x01, sixk, 6));

    // duplicates count once
    int dup[] = { 0x04, 0x04, 0x05, 0x05, 0x05 };
    CHECK(soulx_hidkbd_kbd_report(0, dup, 5, r) == 0);
    CHECK(report_is(r, 0, ab, 2));

    // 0 and usages outside 0x04..0x65 are skipped
    int skip[] = { 0, 1, 2, 3, 0x66, 0x68, 0xFF, 0x04 };
    CHECK(soulx_hidkbd_kbd_report(0, skip, 8, r) == 0);
    CHECK(report_is(r, 0, k1, 1));
    int only_skipped[] = { 0, 1, 0x66, 0x68 };
    CHECK(soulx_hidkbd_kbd_report(0, only_skipped, 4, r) == 0);
    CHECK(report_is(r, 0, NULL, 0));
    // the logical maximum 0x65 is kept, 0x66 is not
    int edge[] = { 0x65 };
    unsigned char edgek[] = { 0x65 };
    CHECK(soulx_hidkbd_kbd_report(0, edge, 1, r) == 0 && report_is(r, 0, edgek, 1));

    // bad arguments
    CHECK(soulx_hidkbd_kbd_report(0, NULL, 0, NULL) == -1);
    CHECK(soulx_hidkbd_kbd_report(-1, NULL, 0, r) == -1);
    CHECK(soulx_hidkbd_kbd_report(0x100, NULL, 0, r) == -1);
    CHECK(soulx_hidkbd_kbd_report(0, NULL, 1, r) == -1);       // count > 0 but usages NULL
    CHECK(soulx_hidkbd_kbd_report(0, one, -1, r) == -1);
}

// ---------------------------------------------------------------------------
// JSON lines
// ---------------------------------------------------------------------------

static void test_json_register(void) {
    // the descriptor, byte for byte from the prototype
    static const int desc[] = {
        0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
        0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
        0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
        0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
        0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
        0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
        0xC0,
        0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,
        0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
        0xC0,
    };
    char want[1024];
    size_t off = 0;
    off += (size_t)snprintf(want + off, sizeof(want) - off, "[");
    for (size_t i = 0; i < sizeof(desc) / sizeof(desc[0]); i++) {
        off += (size_t)snprintf(want + off, sizeof(want) - off, "%s%d", i ? "," : "", desc[i]);
    }
    off += (size_t)snprintf(want + off, sizeof(want) - off, "]");

    char buf[1024];
    int n = soulx_hidkbd_json_register(buf, sizeof(buf));
    CHECK(n > 0 && (size_t)n == strlen(buf));
    // vid 0x18d1 = 6353, pid 0x4e11 = 19985
    const char* prefix = "{\"id\":1,\"command\":\"register\",\"name\":\"SoulX Virtual Keyboard\","
                         "\"vid\":6353,\"pid\":19985,\"bus\":\"usb\",\"descriptor\":";
    CHECK(strncmp(buf, prefix, strlen(prefix)) == 0);
    CHECK(strstr(buf, want) != NULL);              // the exact descriptor array
    CHECK(buf[n - 1] == '\n' && buf[n - 2] == '}');

    // too-small buffers fail cleanly
    char small[32];
    CHECK(soulx_hidkbd_json_register(small, sizeof(small)) == -1 && small[0] == '\0');
    CHECK(soulx_hidkbd_json_register(buf, 0) == -1);
    CHECK(soulx_hidkbd_json_register(NULL, sizeof(buf)) == -1);
}

static void test_json_report(void) {
    char buf[256];
    unsigned char kb[] = { 1, 0, 0, 0x04, 0, 0, 0, 0, 0 };
    int n = soulx_hidkbd_json_report(kb, sizeof(kb), buf, sizeof(buf));
    CHECK(n > 0 && (size_t)n == strlen(buf));
    CHECK(strcmp(buf, "{\"id\":1,\"command\":\"report\",\"report\":[1,0,0,4,0,0,0,0,0]}\n") == 0);

    // consumer report: AC Back 0x0224 -> low byte 0x24 (36), high 0x02
    unsigned char cons[] = { 2, 0x24, 0x02 };
    CHECK(soulx_hidkbd_json_report(cons, sizeof(cons), buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "{\"id\":1,\"command\":\"report\",\"report\":[2,36,2]}\n") == 0);

    unsigned char big[64];
    memset(big, 0, sizeof(big));
    CHECK(soulx_hidkbd_json_report(big, 64, buf, sizeof(buf)) > 0);     // 64 is the limit
    CHECK(soulx_hidkbd_json_report(big, 65, buf, sizeof(buf)) == -1);   // 65 is too long

    // refusals
    CHECK(soulx_hidkbd_json_report(kb, 0, buf, sizeof(buf)) == -1 && buf[0] == '\0');
    CHECK(soulx_hidkbd_json_report(NULL, 9, buf, sizeof(buf)) == -1);
    CHECK(soulx_hidkbd_json_report(kb, sizeof(kb), NULL, sizeof(buf)) == -1);
    CHECK(soulx_hidkbd_json_report(kb, sizeof(kb), buf, 0) == -1);
    char tiny[10];
    CHECK(soulx_hidkbd_json_report(kb, sizeof(kb), tiny, sizeof(tiny)) == -1 && tiny[0] == '\0');
}

static void test_json_delay(void) {
    char buf[64];
    CHECK(soulx_hidkbd_json_delay(6, buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "{\"id\":1,\"command\":\"delay\",\"duration\":6}\n") == 0);
    CHECK(soulx_hidkbd_json_delay(0, buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "{\"id\":1,\"command\":\"delay\",\"duration\":0}\n") == 0);
    CHECK(soulx_hidkbd_json_delay(60000, buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "\"duration\":60000}") != NULL);
    CHECK(soulx_hidkbd_json_delay(-1, buf, sizeof(buf)) == -1 && buf[0] == '\0');
    CHECK(soulx_hidkbd_json_delay(60001, buf, sizeof(buf)) == -1);
    CHECK(soulx_hidkbd_json_delay(6, NULL, sizeof(buf)) == -1);
    CHECK(soulx_hidkbd_json_delay(6, buf, 0) == -1);
    char tiny[8];
    CHECK(soulx_hidkbd_json_delay(6, tiny, sizeof(tiny)) == -1 && tiny[0] == '\0');
}

// ---------------------------------------------------------------------------
// getevent -lp node parsing
// ---------------------------------------------------------------------------

static void test_parse_nodes(void) {
    const char* name = "SoulX Virtual Keyboard";
    char nodes[256];

    const char* out =
        "add device 1: /dev/input/event1\n"
        "  name:     \"virtio_input_multi_touch_1\"\n"
        "add device 2: /dev/input/event12\n"
        "  name:     \"qwerty2\"\n"
        "add device 3: /dev/input/event5\n"
        "  name:     \"SoulX Virtual Keyboard\"\n";
    CHECK(soulx_hidkbd_parse_nodes(out, name, nodes, sizeof(nodes)) == 1);
    CHECK(strcmp(nodes, "/dev/input/event5\n") == 0);

    // two of ours
    const char* two =
        "add device 3: /dev/input/event5\n  name: \"SoulX Virtual Keyboard\"\n"
        "add device 4: /dev/input/event7\n  name: \"other\"\n"
        "add device 5: /dev/input/event9\n  name: \"SoulX Virtual Keyboard\"\n";
    CHECK(soulx_hidkbd_parse_nodes(two, name, nodes, sizeof(nodes)) == 2);
    CHECK(strcmp(nodes, "/dev/input/event5\n/dev/input/event9\n") == 0);

    // CRLF line endings and a trailing line without a newline
    const char* crlf =
        "add device 2: /dev/input/event5\r\n  name:     \"SoulX Virtual Keyboard\"\r\n";
    CHECK(soulx_hidkbd_parse_nodes(crlf, name, nodes, sizeof(nodes)) == 1);
    CHECK(strcmp(nodes, "/dev/input/event5\n") == 0);

    // a name that is a prefix or has extra characters must NOT match
    const char* near =
        "add device 1: /dev/input/event1\n  name: \"SoulX Virtual Keyboar\"\n"
        "add device 2: /dev/input/event2\n  name: \"SoulX Virtual Keyboardx\"\n"
        "add device 3: /dev/input/event3\n  name: \"soulx virtual keyboard\"\n";   // case-sensitive
    CHECK(soulx_hidkbd_parse_nodes(near, name, nodes, sizeof(nodes)) == 0);
    CHECK(nodes[0] == '\0');

    // a device whose name line is missing is not counted
    const char* orphan =
        "add device 1: /dev/input/event1\n"
        "add device 2: /dev/input/event5\n  name: \"SoulX Virtual Keyboard\"\n";
    CHECK(soulx_hidkbd_parse_nodes(orphan, name, nodes, sizeof(nodes)) == 1);
    CHECK(strcmp(nodes, "/dev/input/event5\n") == 0);

    // no match at all
    const char* none = "add device 1: /dev/input/event1\n  name: \"qwerty2\"\n";
    CHECK(soulx_hidkbd_parse_nodes(none, name, nodes, sizeof(nodes)) == 0);
    CHECK(nodes[0] == '\0');

    // the node list does not fit
    CHECK(soulx_hidkbd_parse_nodes(out, name, nodes, 4) == -1);

    // argument guards
    CHECK(soulx_hidkbd_parse_nodes(NULL, name, nodes, sizeof(nodes)) == 0 && nodes[0] == '\0');
    CHECK(soulx_hidkbd_parse_nodes(out, NULL, nodes, sizeof(nodes)) == -1);
    CHECK(soulx_hidkbd_parse_nodes(out, name, NULL, sizeof(nodes)) == -1);
    CHECK(soulx_hidkbd_parse_nodes(out, name, nodes, 0) == -1);
}

int main(void) {
    test_usage_for_char();
    test_kbd_report();
    test_json_register();
    test_json_report();
    test_json_delay();
    test_parse_nodes();

    if (g_failures) {
        fprintf(stderr, "test_hidkbd: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_hidkbd: all %d checks passed\n", g_checks);
    return 0;
}
