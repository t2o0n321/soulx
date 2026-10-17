/*
 * SoulX Android Emulator - unit tests for source/platform/macos/apps.c
 * Binary AndroidManifest.xml (AXML) parser on manifests generated in-test (plus truncation and
 * random-mutation fuzzing), the XAPK manifest.json scanner, package name validation, and
 * soulx_apk_inspect on real .apk/.xapk/.apks/.apkm archives and directories built with /usr/bin/zip
 * inside a temporary SOULX_HOME (no emulator needed).
 * soulx_install_path / soulx_launch_package / soulx_uninstall_package run against a fake adb: a copy of
 * this binary installed as $SOULX_HOME/sdk/platform-tools/adb, which logs its calls and what `push`
 * reads, so the tests can prove that no file outside an input folder is ever copied to the guest.
 * Build with -fsanitize=address,undefined to turn memory errors into failures.
 */

#include <dirent.h>
#include <errno.h>
#include <libproc.h>
#include <limits.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "soulx_mac.h"

extern int soulx_axml_parse(const uint8_t* data, size_t len, SoulXApkInfo* info);
extern int soulx_xapk_json_parse(const char* json, size_t len, char* package, size_t package_cap,
                                long long* version_code, char files[][256], int max_files, int* file_count);

#ifndef AXML_FUZZ_ITERS
#define AXML_FUZZ_ITERS 6000            // per manifest variant; raise with -DAXML_FUZZ_ITERS=... for long runs
#endif

static int g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

// ---------------------------------------------------------------------------
// Deterministic PRNG (xorshift64*)
// ---------------------------------------------------------------------------

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return (uint32_t)((g_rng * 0x2545F4914F6CDD1Dull) >> 32);
}

// ---------------------------------------------------------------------------
// Byte buffer
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t* d;
    size_t n;
    size_t cap;
} Buf;

static void put(Buf* b, const void* p, size_t n)
{
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->n + n) cap *= 2;
        uint8_t* d = realloc(b->d, cap);
        if (!d) {
            perror("realloc");
            exit(2);
        }
        b->d = d;
        b->cap = cap;
    }
    if (n) memcpy(b->d + b->n, p, n);
    b->n += n;
}

static void put8(Buf* b, uint32_t v)
{
    uint8_t x = (uint8_t)v;
    put(b, &x, 1);
}

static void put16(Buf* b, uint32_t v)
{
    put8(b, v & 0xFF);
    put8(b, (v >> 8) & 0xFF);
}

static void put32(Buf* b, uint32_t v)
{
    put16(b, v & 0xFFFF);
    put16(b, v >> 16);
}

static void set32(Buf* b, size_t off, uint32_t v)
{
    b->d[off] = (uint8_t)v;
    b->d[off + 1] = (uint8_t)(v >> 8);
    b->d[off + 2] = (uint8_t)(v >> 16);
    b->d[off + 3] = (uint8_t)(v >> 24);
}

static uint8_t* dup_bytes(const uint8_t* p, size_t n)
{
    uint8_t* d = malloc(n ? n : 1);           // exact size: ASan catches any read past the end
    if (!d) {
        perror("malloc");
        exit(2);
    }
    if (n) memcpy(d, p, n);
    return d;
}

// ---------------------------------------------------------------------------
// AXML builder
// ---------------------------------------------------------------------------

#define NONE        0xFFFFFFFFu
#define T_STRING    0x03
#define T_INT_DEC   0x10

enum {
    // attribute names covered by the resource map (must come first)
    S_VERSION_CODE, S_VERSION_NAME, S_SCREEN_ORIENTATION, S_NAME, S_TARGET_ACTIVITY, S_VERSION_CODE_MAJOR,
    // everything else
    S_PACKAGE, S_MANIFEST, S_APPLICATION, S_ACTIVITY, S_INTENT_FILTER, S_ACTION, S_CATEGORY, S_ANDROID,
    S_ANDROID_URI, S_PKG_VALUE, S_VN_VALUE, S_MAIN, S_LAUNCHER, S_MAIN_ACTIVITY, S_ACTIVITY_ALIAS,
    S_LAUNCHER_ALIAS, S_SPLIT, S_SPLIT_VALUE, S_VIEW, S_COUNT
};

static const uint32_t k_resids[] = { 0x0101021b, 0x0101021c, 0x0101001e, 0x01010003, 0x01010202, 0x01010576 };

typedef struct {
    bool utf8;                  // UTF-8 string pool (else UTF-16)
    bool obfuscated;            // android: attribute names emptied (resource ids only)
    bool no_resmap;             // no resource map chunk (names only)
    int orientation;            // screenOrientation of .MainActivity, -1 = none
    bool via_alias;             // launcher is an <activity-alias> targeting .MainActivity
    bool main_action;           // intent-filter has action MAIN (else VIEW)
    bool launcher_category;     // intent-filter has category LAUNCHER
    uint32_t version_code;
    int32_t version_major;      // -1 = none
    const char* package;
    const char* version_name;
    const char* split;          // NULL = base APK
    const char* root;           // root element name, NULL = "manifest"
} Spec;

typedef struct {
    uint32_t ns, name, raw;
    uint8_t type;
    uint32_t data;
} TAttr;

static Spec spec_default(void)
{
    Spec s = { false, false, false, 6, false, true, true, 42, -1, "com.example.game", "1.2.3", NULL, NULL };
    return s;
}

static TAttr str_attr(uint32_t ns, uint32_t name, uint32_t value)
{
    TAttr a = { ns, name, value, T_STRING, value };
    return a;
}

static TAttr int_attr(uint32_t ns, uint32_t name, uint32_t value)
{
    TAttr a = { ns, name, NONE, T_INT_DEC, value };
    return a;
}

static void pool(Buf* b, const char* const* strs, int n, bool utf8)
{
    size_t start = b->n;
    put16(b, 0x0001);
    put16(b, 28);
    put32(b, 0);                                // chunk size, patched below
    put32(b, (uint32_t)n);
    put32(b, 0);                                // styles
    put32(b, utf8 ? 0x100u : 0u);
    put32(b, (uint32_t)(28 + 4 * n));           // stringsStart
    put32(b, 0);                                // stylesStart
    size_t offs = b->n;
    for (int i = 0; i < n; i++) put32(b, 0);
    size_t data = b->n;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(strs[i]);           // ASCII, < 128 characters
        set32(b, offs + 4 * (size_t)i, (uint32_t)(b->n - data));
        if (utf8) {
            put8(b, (uint32_t)len);
            put8(b, (uint32_t)len);
            put(b, strs[i], len);
            put8(b, 0);
        } else {
            put16(b, (uint32_t)len);
            for (size_t k = 0; k < len; k++) put16(b, (unsigned char)strs[i][k]);
            put16(b, 0);
        }
    }
    while ((b->n - start) % 4) put8(b, 0);
    set32(b, start + 4, (uint32_t)(b->n - start));
}

static void start_el(Buf* b, uint32_t name, const TAttr* attrs, int n)
{
    put16(b, 0x0102);
    put16(b, 16);
    put32(b, (uint32_t)(16 + 20 + 20 * n));
    put32(b, 1);                                // line
    put32(b, NONE);                             // comment
    put32(b, NONE);                             // ns
    put32(b, name);
    put16(b, 20);                               // attributeStart
    put16(b, 20);                               // attributeSize
    put16(b, (uint32_t)n);
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    for (int i = 0; i < n; i++) {
        put32(b, attrs[i].ns);
        put32(b, attrs[i].name);
        put32(b, attrs[i].raw);
        put16(b, 8);
        put8(b, 0);
        put8(b, attrs[i].type);
        put32(b, attrs[i].data);
    }
}

static void end_el(Buf* b, uint32_t name)
{
    put16(b, 0x0103);
    put16(b, 16);
    put32(b, 24);
    put32(b, 1);
    put32(b, NONE);
    put32(b, NONE);
    put32(b, name);
}

static void namespace_chunk(Buf* b, uint32_t type)
{
    put16(b, type);
    put16(b, 16);
    put32(b, 24);
    put32(b, 1);
    put32(b, NONE);
    put32(b, S_ANDROID);
    put32(b, S_ANDROID_URI);
}

static void launcher_filter(Buf* b, const Spec* s)
{
    TAttr action = str_attr(S_ANDROID_URI, S_NAME, s->main_action ? S_MAIN : S_VIEW);
    TAttr category = str_attr(S_ANDROID_URI, S_NAME, S_LAUNCHER);
    start_el(b, S_INTENT_FILTER, NULL, 0);
    start_el(b, S_ACTION, &action, 1);
    end_el(b, S_ACTION);
    if (s->launcher_category) {
        start_el(b, S_CATEGORY, &category, 1);
        end_el(b, S_CATEGORY);
    }
    end_el(b, S_INTENT_FILTER);
}

// <manifest package versionCode versionName [versionCodeMajor] [split]>
//   <application>
//     <activity name=".MainActivity" [screenOrientation]> [launcher filter] </activity>
//     [<activity-alias name=".Launcher" targetActivity=".MainActivity"> launcher filter </activity-alias>]
static Buf build(const Spec* s)
{
    const char* strs[S_COUNT] = {
        "versionCode", "versionName", "screenOrientation", "name", "targetActivity", "versionCodeMajor",
        "package", s->root ? s->root : "manifest", "application", "activity", "intent-filter", "action",
        "category", "android", "http://schemas.android.com/apk/res/android", s->package, s->version_name,
        "android.intent.action.MAIN", "android.intent.category.LAUNCHER", ".MainActivity", "activity-alias",
        ".Launcher", "split", s->split ? s->split : "", "android.intent.action.VIEW"
    };
    if (s->obfuscated) {
        for (int i = 0; i <= S_VERSION_CODE_MAJOR; i++) strs[i] = "";
    }
    const uint32_t A = S_ANDROID_URI;
    Buf b = { NULL, 0, 0 };
    put16(&b, 0x0003);
    put16(&b, 8);
    put32(&b, 0);                               // document size, patched below
    pool(&b, strs, S_COUNT, s->utf8);
    if (!s->no_resmap) {
        put16(&b, 0x0180);
        put16(&b, 8);
        put32(&b, (uint32_t)(8 + sizeof(k_resids)));
        for (size_t i = 0; i < sizeof(k_resids) / sizeof(k_resids[0]); i++) put32(&b, k_resids[i]);
    }
    namespace_chunk(&b, 0x0100);

    TAttr ma[5];
    int n = 0;
    ma[n++] = int_attr(A, S_VERSION_CODE, s->version_code);
    ma[n++] = str_attr(A, S_VERSION_NAME, S_VN_VALUE);
    ma[n++] = str_attr(NONE, S_PACKAGE, S_PKG_VALUE);
    if (s->version_major >= 0) ma[n++] = int_attr(A, S_VERSION_CODE_MAJOR, (uint32_t)s->version_major);
    if (s->split) ma[n++] = str_attr(NONE, S_SPLIT, S_SPLIT_VALUE);
    start_el(&b, S_MANIFEST, ma, n);
    start_el(&b, S_APPLICATION, NULL, 0);

    TAttr aa[2];
    n = 0;
    aa[n++] = str_attr(A, S_NAME, S_MAIN_ACTIVITY);
    if (s->orientation >= 0) aa[n++] = int_attr(A, S_SCREEN_ORIENTATION, (uint32_t)s->orientation);
    start_el(&b, S_ACTIVITY, aa, n);
    if (!s->via_alias) launcher_filter(&b, s);
    end_el(&b, S_ACTIVITY);
    if (s->via_alias) {
        TAttr la[2] = { str_attr(A, S_NAME, S_LAUNCHER_ALIAS), str_attr(A, S_TARGET_ACTIVITY, S_MAIN_ACTIVITY) };
        start_el(&b, S_ACTIVITY_ALIAS, la, 2);
        launcher_filter(&b, s);
        end_el(&b, S_ACTIVITY_ALIAS);
    }
    end_el(&b, S_APPLICATION);
    end_el(&b, S_MANIFEST);
    namespace_chunk(&b, 0x0101);
    set32(&b, 4, (uint32_t)b.n);
    return b;
}

static int parse_spec(const Spec* s, SoulXApkInfo* info)
{
    Buf b = build(s);
    uint8_t* d = dup_bytes(b.d, b.n);
    memset(info, 0, sizeof(*info));
    int rc = soulx_axml_parse(d, b.n, info);
    free(d);
    free(b.d);
    return rc;
}

// ---------------------------------------------------------------------------
// AXML tests
// ---------------------------------------------------------------------------

static void test_axml_basic(void)
{
    SoulXApkInfo info;
    Spec s = spec_default();
    for (int utf8 = 0; utf8 <= 1; utf8++) {
        s.utf8 = utf8 != 0;
        CHECK(parse_spec(&s, &info) == 0);
        CHECK(strcmp(info.package, "com.example.game") == 0);
        CHECK(strcmp(info.version_name, "1.2.3") == 0);
        CHECK(info.version_code == 42);
        CHECK(info.wants_landscape);
        CHECK(info.abis[0] == '\0');
    }
}

static void test_axml_orientation(void)
{
    static const struct { int value; bool landscape; } cases[] = {
        { 0, true }, { 6, true }, { 8, true }, { 11, true },
        { 1, false }, { 4, false }, { 7, false }, { 10, false }, { -1, false },
    };
    SoulXApkInfo info;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Spec s = spec_default();
        s.orientation = cases[i].value;
        CHECK(parse_spec(&s, &info) == 0);
        CHECK(info.wants_landscape == cases[i].landscape);
    }

    // not a launcher: action VIEW, or no LAUNCHER category
    Spec s = spec_default();
    s.orientation = 0;
    s.main_action = false;
    CHECK(parse_spec(&s, &info) == 0 && !info.wants_landscape);
    s = spec_default();
    s.orientation = 0;
    s.launcher_category = false;
    CHECK(parse_spec(&s, &info) == 0 && !info.wants_landscape);
}

static void test_axml_alias(void)
{
    SoulXApkInfo info;
    Spec s = spec_default();
    s.via_alias = true;
    s.orientation = 0;                          // on the alias target
    CHECK(parse_spec(&s, &info) == 0 && info.wants_landscape);
    s.orientation = 1;
    CHECK(parse_spec(&s, &info) == 0 && !info.wants_landscape);
    s.orientation = 11;
    s.utf8 = true;
    CHECK(parse_spec(&s, &info) == 0 && info.wants_landscape);
}

static void test_axml_variants(void)
{
    SoulXApkInfo info;

    // attribute names stripped by an obfuscator: resource ids still identify them
    Spec s = spec_default();
    s.obfuscated = true;
    CHECK(parse_spec(&s, &info) == 0);
    CHECK(info.version_code == 42 && strcmp(info.version_name, "1.2.3") == 0 && info.wants_landscape);

    // no resource map: names are the fallback
    s = spec_default();
    s.no_resmap = true;
    CHECK(parse_spec(&s, &info) == 0);
    CHECK(info.version_code == 42 && info.wants_landscape);

    // versionCodeMajor forms the upper 32 bits
    s = spec_default();
    s.version_code = 5;
    s.version_major = 1;
    CHECK(parse_spec(&s, &info) == 0 && info.version_code == 4294967301LL);

    // full 32-bit versionCode is unsigned
    s = spec_default();
    s.version_code = 0xFFFFFFFFu;
    CHECK(parse_spec(&s, &info) == 0 && info.version_code == 4294967295LL);

    // split APK manifests parse too
    s = spec_default();
    s.split = "config.arm64_v8a";
    CHECK(parse_spec(&s, &info) == 0 && strcmp(info.package, "com.example.game") == 0);

    // control characters in strings are neutralised (no terminal escapes)
    s = spec_default();
    s.version_name = "1.0\x1b[31m\x07";
    CHECK(parse_spec(&s, &info) == 0 && strcmp(info.version_name, "1.0?[31m?") == 0);

    // invalid package names are rejected (they end up in adb shell command lines)
    const char* bad[] = { "com.example;reboot", "nodots", "com..x", ".com.x", "com.x.", "com.x y", "1com.x", "" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        s = spec_default();
        s.package = bad[i];
        CHECK(parse_spec(&s, &info) == -1);
        CHECK(info.package[0] == '\0');
    }

    // root element must be <manifest>
    s = spec_default();
    s.root = "application";
    CHECK(parse_spec(&s, &info) == -1);

    // not AXML at all
    const uint8_t text[] = "<?xml version=\"1.0\"?><manifest/>";
    CHECK(soulx_axml_parse(text, sizeof(text), &info) == -1);
    CHECK(soulx_axml_parse(NULL, 0, &info) == -1);
    CHECK(soulx_axml_parse(text, sizeof(text), NULL) == -1);
}

static void test_axml_fuzz(void)
{
    SoulXApkInfo info;
    int mutants = 0, accepted = 0;
    for (int variant = 0; variant < 4; variant++) {
        Spec s = spec_default();
        s.utf8 = (variant & 1) != 0;
        s.via_alias = (variant & 2) != 0;
        s.version_major = 3;
        Buf b = build(&s);

        // every truncation: the declared size no longer fits -> rejected
        for (size_t len = 0; len < b.n; len++) {
            uint8_t* d = dup_bytes(b.d, len);
            CHECK(soulx_axml_parse(d, len, &info) == -1);
            free(d);
        }
        // truncations with the declared size patched to match (chunks cut mid-way)
        for (size_t len = 8; len < b.n; len++) {
            uint8_t* d = dup_bytes(b.d, len);
            d[4] = (uint8_t)len;
            d[5] = (uint8_t)(len >> 8);
            d[6] = (uint8_t)(len >> 16);
            d[7] = (uint8_t)(len >> 24);
            if (soulx_axml_parse(d, len, &info) == 0) CHECK(soulx_package_name_valid(info.package));
            free(d);
        }
        // random mutations
        for (int iter = 0; iter < AXML_FUZZ_ITERS; iter++) {
            uint8_t* d = dup_bytes(b.d, b.n);
            int flips = 1 + (int)(rnd() % 8);
            for (int f = 0; f < flips; f++) {
                size_t pos = rnd() % b.n;
                switch (rnd() % 4) {
                case 0: d[pos] ^= (uint8_t)(1u << (rnd() % 8)); break;
                case 1: d[pos] = 0xFF; break;
                case 2: d[pos] = 0x00; break;
                default: d[pos] = (uint8_t)rnd(); break;
                }
            }
            mutants++;
            if (soulx_axml_parse(d, b.n, &info) == 0) {
                accepted++;
                CHECK(soulx_package_name_valid(info.package));
            }
            free(d);
        }
        free(b.d);
    }

    // random garbage behind a plausible XML header
    for (int iter = 0; iter < 3000; iter++) {
        size_t len = 8 + rnd() % 512;
        uint8_t* d = malloc(len);
        if (!d) exit(2);
        for (size_t i = 0; i < len; i++) d[i] = (uint8_t)rnd();
        d[0] = 0x03;
        d[1] = 0x00;
        d[2] = 0x08;
        d[3] = 0x00;
        d[4] = (uint8_t)len;
        d[5] = (uint8_t)(len >> 8);
        d[6] = 0;
        d[7] = 0;
        if (soulx_axml_parse(d, len, &info) == 0) CHECK(soulx_package_name_valid(info.package));
        free(d);
    }
    printf("axml fuzz: %d of %d mutated manifests still parsed (all others rejected cleanly)\n", accepted, mutants);
    CHECK(accepted > 0 && accepted < mutants);
}

// ---------------------------------------------------------------------------
// XAPK manifest.json
// ---------------------------------------------------------------------------

static int json_parse(const char* text, char* package, long long* vc, char files[][256], int max, int* count)
{
    size_t len = strlen(text);
    char* d = (char*)dup_bytes((const uint8_t*)text, len);
    int rc = soulx_xapk_json_parse(d, len, package, 256, vc, files, max, count);
    free(d);
    return rc;
}

static void test_json(void)
{
    static char files[8][256];
    char package[256];
    long long vc;
    int count;

    const char* xapk =
        "\xEF\xBB\xBF{\n"
        "  \"xapk_version\": 2,\n"
        "  \"package_name\": \"com.example.game\",\n"
        "  \"name\": \"Game \\\"Deluxe\\\" \\u00e9\\ud83d\\ude00\",\n"
        "  \"version_code\": \"1234\",\n"
        "  \"version_name\": \"1.2.3\",\n"
        "  \"min_sdk_version\": \"24\",\n"
        "  \"permissions\": [\"android.permission.INTERNET\", {\"x\": [1, 2.5e3, -3, true, false, null]}],\n"
        "  \"split_configs\": [\"config.arm64_v8a\", \"config.xxhdpi\"],\n"
        "  \"split_apks\": [\n"
        "    {\"file\": \"com.example.game.apk\", \"id\": \"base\"},\n"
        "    {\"id\": \"config.arm64_v8a\", \"file\": \"config.arm64_v8a.apk\"},\n"
        "    {\"file\": \"config\\u002exxhdpi.apk\", \"id\": \"config.xxhdpi\", \"extra\": {\"a\": [[]]}},\n"
        "    {\"id\": \"no-file\"}\n"
        "  ],\n"
        "  \"expansions\": [{\"file\": \"Android/obb/com.example.game/main.1234.com.example.game.obb\"}],\n"
        "  \"total_size\": 123456789\n"
        "}\n";
    CHECK(json_parse(xapk, package, &vc, files, 8, &count) == 0);
    CHECK(strcmp(package, "com.example.game") == 0);
    CHECK(vc == 1234);
    CHECK(count == 3);
    CHECK(count == 3 && strcmp(files[0], "com.example.game.apk") == 0);
    CHECK(count == 3 && strcmp(files[1], "config.arm64_v8a.apk") == 0);
    CHECK(count == 3 && strcmp(files[2], "config.xxhdpi.apk") == 0);

    // numeric version_code, no split_apks (XAPK v1)
    CHECK(json_parse("{\"version_code\": 77, \"package_name\": \"a.b\"}", package, &vc, files, 8, &count) == 0);
    CHECK(vc == 77 && count == 0 && strcmp(package, "a.b") == 0);

    // absent / non-numeric fields
    CHECK(json_parse("{}", package, &vc, files, 8, &count) == 0 && vc == -1 && package[0] == '\0');
    CHECK(json_parse("{\"version_code\": \"12x\", \"package_name\": 5}", package, &vc, files, 8, &count) == 0);
    CHECK(vc == -1 && package[0] == '\0');

    // more split APKs than the caller accepts: told apart from a malformed document
    CHECK(json_parse("{\"split_apks\": [{\"file\": \"a.apk\"}, {\"file\": \"b.apk\"}, {\"file\": \"c.apk\"}]}",
                     package, &vc, files, 2, &count) == -2);

    // package_name is untrusted: anything but a valid package name is dropped (it ends up in messages)
    CHECK(json_parse("{\"package_name\": \"\\u001b]0;PWNED\\u0007\\u001b[2J\\nsoulx: Installed com.good.app\"}",
                     package, &vc, files, 8, &count) == 0 && package[0] == '\0');
    CHECK(json_parse("{\"package_name\": \"com.good.app; reboot\"}", package, &vc, files, 8, &count) == 0 &&
          package[0] == '\0');

    // malformed documents
    const char* bad[] = {
        "", "[]", "{", "{\"a\"}", "{\"a\": }", "{\"a\": 1,}", "{\"a\": \"x}", "{\"a\": \"\\q\"}",
        "{\"a\": [1, 2}", "{\"a\": {\"b\": 1]}", "{\"a\": \"\x01\"}", "{\"split_apks\": [{\"file\": }]}",
        "{\"a\": \"\\u12\"}", "{\"package_name\": \"a.b\" \"x\": 1}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(json_parse(bad[i], package, &vc, files, 8, &count) == -1);
    }

    // deep nesting is bounded, not a stack overflow
    size_t depth = 100000;
    char* deep = malloc(depth * 2 + 16);
    if (!deep) exit(2);
    size_t o = strlen("{\"a\": ");
    memcpy(deep, "{\"a\": ", o);
    for (size_t i = 0; i < depth; i++) deep[o++] = '[';
    for (size_t i = 0; i < depth; i++) deep[o++] = ']';
    deep[o++] = '}';
    deep[o] = '\0';
    CHECK(json_parse(deep, package, &vc, files, 8, &count) == -1);
    free(deep);

    // every prefix of a valid document: no crash, only the complete one succeeds
    size_t len = strlen(xapk);
    for (size_t l = 0; l < len; l++) {
        char* d = (char*)dup_bytes((const uint8_t*)xapk, l);
        int rc = soulx_xapk_json_parse(d, l, package, sizeof(package), &vc, files, 8, &count);
        CHECK(rc == (l == len - 1 ? 0 : -1));   // only "...}" without the trailing "\n" is complete
        free(d);
    }
    // random mutations
    for (int iter = 0; iter < 20000; iter++) {
        char* d = (char*)dup_bytes((const uint8_t*)xapk, len);
        int flips = 1 + (int)(rnd() % 4);
        for (int f = 0; f < flips; f++) d[rnd() % len] = (char)(rnd() % 128);
        soulx_xapk_json_parse(d, len, package, sizeof(package), &vc, files, 8, &count);
        CHECK(count >= 0 && count <= 8);
        free(d);
    }
}

// ---------------------------------------------------------------------------
// Package names
// ---------------------------------------------------------------------------

static void test_package_names(void)
{
    const char* good[] = { "com.example.game", "a.b", "com.Example_1.x2", "a.0", "com.unity3d.player",
                           "jp.co.app_name.v2" };
    const char* bad[] = { NULL, "", "a", "a.", ".a", "a..b", "1a.b", "_a.b", "a.b-c", "a.b c", "a.b;c", "a.b/c",
                          "com.example.game\n", "a.b$c", "a.b'c" };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) CHECK(soulx_package_name_valid(good[i]));
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(!soulx_package_name_valid(bad[i]));

    char longest[300];
    memset(longest, 'a', sizeof(longest));
    longest[1] = '.';
    longest[255] = '\0';
    CHECK(soulx_package_name_valid(longest));     // 255 characters
    longest[255] = 'a';
    longest[256] = '\0';
    CHECK(!soulx_package_name_valid(longest));    // 256 characters
}

// ---------------------------------------------------------------------------
// soulx_apk_inspect on real archives (/usr/bin/zip to build, zipinfo/unzip to read)
// ---------------------------------------------------------------------------

static char g_work[PATH_MAX];
static int g_stage;

static void join(char* out, const char* dir, const char* name)
{
    int n = snprintf(out, PATH_MAX, "%s/%s", dir, name);
    if (n < 0 || n >= PATH_MAX) {
        fprintf(stderr, "path too long\n");
        exit(2);
    }
}

static void write_bytes(const char* path, const void* data, size_t len)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char* slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (soulx_mkdirs(dir) != 0) {
            fprintf(stderr, "mkdir %s failed\n", dir);
            exit(2);
        }
    }
    FILE* f = fopen(path, "wb");
    if (!f || fwrite(data, 1, len, f) != len || fclose(f) != 0) {
        perror(path);
        exit(2);
    }
}

static void copy_file(const char* from, const char* to)
{
    char* data = NULL;
    size_t len = 0;
    if (soulx_read_file(from, &data, &len) != 0) {
        fprintf(stderr, "cannot read %s\n", from);
        exit(2);
    }
    write_bytes(to, data, len);
    free(data);
}

static void new_stage(char* stage)
{
    char name[32];
    snprintf(name, sizeof(name), "stage-%d", g_stage++);
    join(stage, g_work, name);
    if (soulx_mkdirs(stage) != 0) exit(2);
}

static void zip_dir(const char* stage, const char* out)
{
    const char* argv[] = { "/usr/bin/zip", "-q", "-r", "-y", "-X", out, ".", NULL };
    SoulXCmd cmd;
    SoulXCmdResult res;
    memset(&cmd, 0, sizeof(cmd));
    cmd.argv = argv;
    cmd.cwd = stage;
    cmd.timeout_ms = 60000;
    int rc = soulx_run(&cmd, &res);
    soulx_cmd_result_free(&res);
    if (rc != 0) {
        fprintf(stderr, "zip %s failed (%d)\n", out, rc);
        exit(2);
    }
}

// An APK: binary manifest + classes.dex + lib/<abi>/libgame.so for each abi.
static void make_apk(const char* out, const Spec* s, const char* const* abis, int nabis)
{
    char stage[PATH_MAX], p[PATH_MAX];
    new_stage(stage);
    Buf b = build(s);
    join(p, stage, "AndroidManifest.xml");
    write_bytes(p, b.d, b.n);
    free(b.d);
    join(p, stage, "classes.dex");
    write_bytes(p, "dex\n035", 7);
    for (int i = 0; i < nabis; i++) {
        char rel[128];
        snprintf(rel, sizeof(rel), "lib/%s/libgame.so", abis[i]);
        join(p, stage, rel);
        write_bytes(p, "\177ELF", 4);
    }
    zip_dir(stage, out);
}

static bool dir_empty(const char* path)
{
    DIR* d = opendir(path);
    if (!d) return true;
    struct dirent* e;
    bool empty = true;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) empty = false;
    }
    closedir(d);
    return empty;
}

static void test_inspect_archives(void)
{
    static const char* const both[] = { "arm64-v8a", "armeabi-v7a" };
    static const char* const arm64[] = { "arm64-v8a" };
    static const char* const v7[] = { "armeabi-v7a" };
    SoulXApkInfo info;
    char p[PATH_MAX], stage[PATH_MAX], base[PATH_MAX], base_noabi[PATH_MAX], split64[PATH_MAX], split32[PATH_MAX];

    join(g_work, soulx_home(), "work");
    if (soulx_mkdirs(g_work) != 0) exit(2);

    // single APK with native code for two ABIs
    Spec s = spec_default();
    join(base, g_work, "base.apk");
    make_apk(base, &s, both, 2);
    CHECK(soulx_apk_inspect(base, &info) == 0);
    CHECK(strcmp(info.package, "com.example.game") == 0 && info.version_code == 42 && info.wants_landscape);
    CHECK(strcmp(info.version_name, "1.2.3") == 0);
    CHECK(strcmp(info.abis, "arm64-v8a,armeabi-v7a") == 0);

    // split set: base without native code + config splits
    s.orientation = 1;
    join(base_noabi, g_work, "base-noabi.apk");
    make_apk(base_noabi, &s, NULL, 0);
    Spec sp = spec_default();
    sp.split = "config.arm64_v8a";
    join(split64, g_work, "config.arm64_v8a.apk");
    make_apk(split64, &sp, arm64, 1);
    sp.split = "config.armeabi_v7a";
    join(split32, g_work, "config.armeabi_v7a.apk");
    make_apk(split32, &sp, v7, 1);

    // XAPK: manifest.json + splits + OBB, with wildcard characters and spaces in the file name
    new_stage(stage);
    const char* manifest_json =
        "{\"package_name\": \"com.example.game\", \"version_code\": \"42\", \"split_apks\": ["
        "{\"file\": \"com.example.game.apk\", \"id\": \"base\"}, {\"file\": \"config.arm64_v8a.apk\"}, "
        "{\"file\": \"config.armeabi_v7a.apk\"}, {\"file\": \"../../evil.apk\"}]}";
    join(p, stage, "manifest.json");
    write_bytes(p, manifest_json, strlen(manifest_json));
    join(p, stage, "com.example.game.apk");
    copy_file(base_noabi, p);
    join(p, stage, "config.arm64_v8a.apk");
    copy_file(split64, p);
    join(p, stage, "config.armeabi_v7a.apk");
    copy_file(split32, p);
    join(p, stage, "Android/obb/com.example.game/main.42.com.example.game.obb");
    write_bytes(p, "OBB", 3);
    char xapk[PATH_MAX];
    join(xapk, g_work, "My Game [mod] (1).xapk");
    zip_dir(stage, xapk);
    memset(&info, 0x55, sizeof(info));
    CHECK(soulx_apk_inspect(xapk, &info) == 0);
    CHECK(strcmp(info.package, "com.example.game") == 0 && info.version_code == 42 && !info.wants_landscape);
    CHECK(strcmp(info.abis, "arm64-v8a,armeabi-v7a") == 0);

    // bundletool .apks: splits/ + toc.pb
    new_stage(stage);
    join(p, stage, "splits/base-master.apk");
    copy_file(base_noabi, p);
    join(p, stage, "splits/base-arm64_v8a.apk");
    copy_file(split64, p);
    join(p, stage, "toc.pb");
    write_bytes(p, "\x0a\x00", 2);
    char apks[PATH_MAX];
    join(apks, g_work, "game.apks");
    zip_dir(stage, apks);
    CHECK(soulx_apk_inspect(apks, &info) == 0);
    CHECK(strcmp(info.package, "com.example.game") == 0 && strcmp(info.abis, "arm64-v8a") == 0);

    // APKMirror-style .apkm: APKs at the root, no manifest.json
    new_stage(stage);
    join(p, stage, "base.apk");
    copy_file(base, p);
    join(p, stage, "split_config.arm64_v8a.apk");
    copy_file(split64, p);
    join(p, stage, "info.json");
    write_bytes(p, "{}", 2);
    char apkm[PATH_MAX];
    join(apkm, g_work, "game.apkm");
    zip_dir(stage, apkm);
    CHECK(soulx_apk_inspect(apkm, &info) == 0);
    CHECK(info.wants_landscape && strcmp(info.abis, "arm64-v8a,armeabi-v7a") == 0);

    // directory of APKs
    new_stage(stage);
    join(p, stage, "base.apk");
    copy_file(base_noabi, p);
    join(p, stage, "config.arm64_v8a.apk");
    copy_file(split64, p);
    CHECK(soulx_apk_inspect(stage, &info) == 0);
    CHECK(strcmp(info.package, "com.example.game") == 0 && strcmp(info.abis, "arm64-v8a") == 0);

    fprintf(stderr, "test_apps: the following errors are expected\n");

    // only splits
    new_stage(stage);
    join(p, stage, "config.arm64_v8a.apk");
    copy_file(split64, p);
    CHECK(soulx_apk_inspect(stage, &info) == -1);

    // two different apps in one directory
    Spec other = spec_default();
    other.package = "org.other.app";
    join(p, stage, "base.apk");
    copy_file(base_noabi, p);
    join(p, stage, "other.apk");
    make_apk(p, &other, NULL, 0);
    CHECK(soulx_apk_inspect(stage, &info) == -1);

    // a symlink stored in the archive is never followed
    new_stage(stage);
    join(p, stage, "base.apk");
    if (symlink("/etc/hosts", p) != 0) exit(2);
    char evil[PATH_MAX];
    join(evil, g_work, "evil.apkm");
    zip_dir(stage, evil);
    CHECK(soulx_apk_inspect(evil, &info) == -1);

    // not a zip / unsupported type / missing file / bad arguments
    join(p, g_work, "fake.apk");
    write_bytes(p, "not a zip", 9);
    CHECK(soulx_apk_inspect(p, &info) == -1);
    join(p, g_work, "game.txt");
    write_bytes(p, "x", 1);
    CHECK(soulx_apk_inspect(p, &info) == -1);
    join(p, g_work, "missing.apk");
    CHECK(soulx_apk_inspect(p, &info) == -1);
    CHECK(soulx_apk_inspect("", &info) == -1);
    CHECK(soulx_apk_inspect(base, NULL) == -1);

    // every temporary directory was removed
    join(p, soulx_home(), "tmp");
    CHECK(dir_empty(p));
    CHECK(soulx_rmtree(g_work) == 0);
    CHECK(soulx_rmtree(p) == 0 || !soulx_is_dir(p));
}

// ---------------------------------------------------------------------------
// Install against a fake adb (OBB symlink security)
// ---------------------------------------------------------------------------

// A copy of this binary stands in for SoulX's adb at $SOULX_HOME/sdk/platform-tools/adb. It answers
// just enough for soulx_install_path (install -> "Success"; the installed-version and existing-OBB
// shell queries -> nothing) and records every source handed to `adb push` in
// $SOULX_HOME/tmp/fake-adb-push.log, so a test can prove exactly which host files reach the guest.
static int fake_adb(int argc, char** argv)
{
    const char* sub = NULL;
    int sub_i = 0;
    for (int i = 1; i < argc; i++) {              // skip "-s <serial>" and any other leading options
        if (strcmp(argv[i], "-s") == 0) {
            i++;
            continue;
        }
        if (argv[i][0] == '-') continue;
        sub = argv[i];
        sub_i = i;
        break;
    }
    if (!sub) return 0;
    if (strcmp(sub, "install") == 0 || strcmp(sub, "install-multiple") == 0 || strcmp(sub, "uninstall") == 0) {
        printf("Success\n");
        return 0;
    }
    if (strcmp(sub, "shell") == 0) return 0;      // dumpsys package / mkdir+stat: report nothing
    if (strcmp(sub, "push") == 0) {
        const char* home = getenv("SOULX_HOME");
        char path[PATH_MAX];
        if (home && snprintf(path, sizeof(path), "%s/tmp/fake-adb-push.log", home) < (int)sizeof(path)) {
            FILE* f = fopen(path, "a");
            if (f) {
                for (int i = sub_i + 1; i < argc - 1; i++) fprintf(f, "%s\n", argv[i]);   // last arg = destination
                fclose(f);
            }
        }
        return 0;
    }
    return 0;
}

// File contents as a malloc'd NUL-terminated string ("" when the file does not exist).
static char* slurp(const char* path)
{
    char* data = NULL;
    size_t len = 0;
    if (soulx_read_file(path, &data, &len) != 0) {
        data = malloc(1);
        if (data) data[0] = '\0';
    }
    return data;
}

static void install_fake_adb(void)
{
    char self[PROC_PIDPATHINFO_MAXSIZE], adb[PATH_MAX];
    CHECK(proc_pidpath(getpid(), self, sizeof(self)) > 0);
    join(adb, soulx_home(), "sdk/platform-tools/adb");
    copy_file(self, adb);                         // write_bytes creates the parent directory
    CHECK(chmod(adb, 0755) == 0);
}

// A directory input must never follow a symbolic link to copy an arbitrary host file into
// the guest's OBB directory. A symlinked *.obb, and a symlinked Android/obb/<package> directory, are
// both refused; a real OBB file beside them is still copied.
static void test_install_obb_symlink(void)
{
    static const char* const arm64[] = { "arm64-v8a" };
    const char* serial = "emulator-5600";
    char stage[PATH_MAX], obbdir[PATH_MAX], p[PATH_MAX], secret[PATH_MAX], pushlog[PATH_MAX];
    SoulXApkInfo info;

    join(g_work, soulx_home(), "work");
    if (soulx_mkdirs(g_work) != 0) exit(2);
    install_fake_adb();
    join(pushlog, soulx_home(), "tmp");
    if (soulx_mkdirs(pushlog) != 0) exit(2);
    join(pushlog, soulx_home(), "tmp/fake-adb-push.log");

    // A host file the attacker wants pushed into the guest (outside the installed folder).
    join(secret, g_work, "secret.txt");
    write_bytes(secret, "TOPSECRET", 9);

    fprintf(stderr, "test_apps: the following OBB warnings are expected\n");

    // Scenario 1: a legit OBB and a symlinked OBB side by side in Android/obb/<package>.
    new_stage(stage);
    Spec s = spec_default();
    join(p, stage, "base.apk");
    make_apk(p, &s, arm64, 1);                    // package com.example.game, arm64 native code
    join(obbdir, stage, "Android/obb/com.example.game");
    if (soulx_mkdirs(obbdir) != 0) exit(2);
    join(p, obbdir, "good.main.obb");
    write_bytes(p, "GOODOBB", 7);
    join(p, obbdir, "evil.main.obb");
    if (symlink(secret, p) != 0) exit(2);         // symlink to the host file

    unlink(pushlog);
    CHECK(soulx_install_path(serial, stage, &info, false) == 0);
    CHECK(strcmp(info.package, "com.example.game") == 0);
    char* log = slurp(pushlog);
    CHECK(strstr(log, "good.main.obb") != NULL);  // the real OBB was copied
    CHECK(strstr(log, "evil.main.obb") == NULL);  // the symlink was not
    CHECK(strstr(log, "secret.txt") == NULL);     // and its target never reached `adb push`
    free(log);

    // Scenario 2: the whole Android/obb/<package> directory is a symlink to a folder of OBBs.
    char outside[PATH_MAX];
    new_stage(stage);
    join(p, stage, "base.apk");
    make_apk(p, &s, arm64, 1);
    join(outside, g_work, "outside_obb");
    if (soulx_mkdirs(outside) != 0) exit(2);
    join(p, outside, "sneak.main.obb");
    write_bytes(p, "SNEAK", 5);
    join(obbdir, stage, "Android/obb");
    if (soulx_mkdirs(obbdir) != 0) exit(2);
    join(p, obbdir, "com.example.game");
    if (symlink(outside, p) != 0) exit(2);        // symlinked package directory

    unlink(pushlog);
    CHECK(soulx_install_path(serial, stage, &info, false) == 0);
    log = slurp(pushlog);
    CHECK(strstr(log, "sneak.main.obb") == NULL); // a symlinked component is never descended into
    free(log);

    CHECK(soulx_rmtree(g_work) == 0);
    join(p, soulx_home(), "tmp");
    (void)soulx_rmtree(p);
}

int main(int argc, char** argv)
{
    const char* base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (strcmp(base, "adb") == 0) return fake_adb(argc, argv);

    char tmpl[] = "/private/tmp/soulx-test-apps-XXXXXX";
    char* home = mkdtemp(tmpl);
    if (!home) {
        perror("mkdtemp");
        return 1;
    }
    setenv("SOULX_HOME", home, 1);               // never touch the real ~/.soulx-android

    test_package_names();
    test_axml_basic();
    test_axml_orientation();
    test_axml_alias();
    test_axml_variants();
    test_axml_fuzz();
    test_json();
    test_inspect_archives();
    test_install_obb_symlink();

    rmdir(home);
    if (g_failures) {
        fprintf(stderr, "test_apps: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_apps: all tests passed\n");
    return 0;
}
