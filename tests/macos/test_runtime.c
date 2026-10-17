/*
 * SoulX Android Emulator - unit tests for the macOS runtime provisioning (runtime.c)
 *
 * Standalone program: returns 0 when every check passes and prints each failure. It never touches
 * the real ~/.soulx-android (SOULX_HOME = fresh mkdtemp() directory under /private/tmp), never
 * downloads anything and never starts an emulator. Installed packages are faked on disk; a copy
 * of this binary stands in for `adb` to exercise soulx_ensure_adb_key() and the SoulX environment.
 */

#include <errno.h>
#include <ftw.h>
#include <libproc.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "soulx_mac.h"

// Non-static helpers of runtime.c that are not part of soulx_mac.h.
extern int soulx_runtime_extract_license(const char* xml, const char* id, char** out);
extern int soulx_runtime_write_marker(const SoulXPackage* p);

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do {                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                       \
    } while (0)

static char g_home[PATH_MAX];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int rm_cb(const char* path, const struct stat* st, int flag, struct FTW* ftw) {
    (void)st;
    (void)ftw;
    if (flag == FTW_DP || flag == FTW_D || flag == FTW_DNR) {
        chmod(path, 0700);
        rmdir(path);
        return 0;
    }
    unlink(path);
    return 0;
}

static void rm_rf(const char* path) {
    if (path && strncmp(path, "/private/tmp/", 13) == 0) nftw(path, rm_cb, 32, FTW_DEPTH | FTW_PHYS);
}

static void home_path(char* buf, size_t cap, const char* rel) {
    int n = snprintf(buf, cap, "%s/%s", g_home, rel);
    if (n < 0 || (size_t)n >= cap) {
        fprintf(stderr, "path too long\n");
        exit(2);
    }
}

static bool is_lower_hex40(const char* s) {
    if (!s || strlen(s) != 40) return false;
    for (const char* p = s; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    }
    return true;
}

static bool make_executable(const char* path, const char* content) {
    if (soulx_write_file_atomic(path, content, strlen(content)) != 0) return false;
    return chmod(path, 0755) == 0;
}

// The files soulx_runtime_installed() now requires inside a system-image directory:
// an image dir that is missing them cannot boot, whatever its install marker says.
static void make_image_files(const char* dir) {
    static const char* const files[] = { "system.img", "vendor.img", "ramdisk.img", "kernel-ranchu",
                                          "source.properties" };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char path[PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        if (n < 0 || (size_t)n >= (int)sizeof(path)) {
            CHECK(false);
            return;
        }
        CHECK(soulx_write_file_atomic(path, "img\n", 4) == 0);
    }
}

// ---------------------------------------------------------------------------
// Package table
// ---------------------------------------------------------------------------

static void test_table(void) {
    size_t n = 0;
    const SoulXPackage* pk = soulx_runtime_packages(&n);
    CHECK(pk != NULL);
    CHECK(n == 5);
    if (!pk || n != 5) return;

    static const char* const keys[] = { "emulator", "platform-tools", "play", "google", "play16k" };
    static const SoulXPackageKind kinds[] = { SOULX_PKG_ENGINE, SOULX_PKG_PLATFORM_TOOLS, SOULX_PKG_IMAGE,
                                             SOULX_PKG_IMAGE, SOULX_PKG_IMAGE };
    static const char* const prefix = "https://dl.google.com/android/repository/";
    for (size_t i = 0; i < n; i++) {
        const SoulXPackage* p = &pk[i];
        CHECK(strcmp(p->key, keys[i]) == 0);
        CHECK(p->kind == kinds[i]);
        CHECK(is_lower_hex40(p->sha1));
        CHECK(p->size > 0);
        CHECK(p->url && strncmp(p->url, prefix, strlen(prefix)) == 0);
        CHECK(p->url && strlen(p->url) > 4 && strcmp(p->url + strlen(p->url) - 4, ".zip") == 0);
        CHECK(p->version && p->version[0]);
        CHECK(p->sdk_id && p->sdk_id[0]);
        CHECK(p->license_id && p->license_id[0]);
        CHECK(p->zip_root && p->zip_root[0]);
        CHECK(p->install_rel && p->install_rel[0] && p->install_rel[0] != '/' && !strstr(p->install_rel, ".."));
        CHECK(p->description && p->description[0]);
        if (p->kind == SOULX_PKG_IMAGE) {
            CHECK(p->target && strncmp(p->target, "android-37.", 11) == 0);
            CHECK(p->tag_id && p->tag_display && p->android_name);
            CHECK(strcmp(p->zip_root, "arm64-v8a") == 0);
            CHECK(strcmp(p->license_id, "android-sdk-arm-dbt-license") == 0);
        } else {
            CHECK(strcmp(p->license_id, "android-sdk-license") == 0);
        }
        CHECK(soulx_runtime_find(p->key) == p);
    }

    // The exact pins (spec section 1)
    const SoulXPackage* e = soulx_runtime_find("emulator");
    CHECK(e && e->size == 416112708ULL && strcmp(e->sha1, "3af4fe44ce82b3d88ae5678a53735f27ad729c15") == 0);
    CHECK(e && strcmp(e->url, "https://dl.google.com/android/repository/emulator-darwin_aarch64-16428233.zip") == 0);
    CHECK(e && strcmp(e->version, "37.2.12") == 0 && strcmp(e->install_rel, "emulator") == 0);
    const SoulXPackage* t = soulx_runtime_find("platform-tools");
    CHECK(t && t->size == 16110554ULL && strcmp(t->sha1, "6ae73f4de6452dc57e62ec02b68eed92a4c21661") == 0);
    CHECK(t && strcmp(t->url, "https://dl.google.com/android/repository/platform-tools_r37.0.1-darwin.zip") == 0);
    CHECK(t && strcmp(t->version, "37.0.1") == 0);
    const SoulXPackage* play = soulx_runtime_find("play");
    CHECK(play && play->size == 2257751510ULL && strcmp(play->sha1, "642cf494464b4d1d4d37fa50630fe153599be3fb") == 0);
    CHECK(play && strcmp(play->url, "https://dl.google.com/android/repository/sys-img/google_apis_playstore/arm64-v8a-37.0_r06.zip") == 0);
    CHECK(play && strcmp(play->install_rel, "system-images/android-37.0/google_apis_playstore/arm64-v8a") == 0);
    CHECK(play && strcmp(play->target, "android-37.0") == 0 && strcmp(play->tag_id, "google_apis_playstore") == 0);
    CHECK(play && strcmp(play->tag_display, "Google Play") == 0 && strcmp(play->android_name, "Android 17") == 0);
    CHECK(play && play->playstore && !play->page16k);
    const SoulXPackage* g = soulx_runtime_find("google");
    CHECK(g && g->size == 2194282378ULL && strcmp(g->sha1, "e007fb23024c59bffdf944b95c61143bc38101ce") == 0);
    CHECK(g && strcmp(g->url, "https://dl.google.com/android/repository/sys-img/google_apis/arm64-v8a-37.0_r06.zip") == 0);
    CHECK(g && strcmp(g->install_rel, "system-images/android-37.0/google_apis/arm64-v8a") == 0);
    CHECK(g && strcmp(g->tag_id, "google_apis") == 0 && strcmp(g->tag_display, "Google APIs") == 0);
    CHECK(g && !g->playstore && !g->page16k);
    const SoulXPackage* k16 = soulx_runtime_find("play16k");
    CHECK(k16 && k16->size == 2127264536ULL && strcmp(k16->sha1, "26be7d5274e6f5455b0028cdaa98b950d19c3077") == 0);
    CHECK(k16 && strcmp(k16->url, "https://dl.google.com/android/repository/sys-img/google_apis_playstore/arm64-v8a-playstore-ps16k-37.1_r09.zip") == 0);
    CHECK(k16 && strcmp(k16->install_rel, "system-images/android-37.1/google_apis_playstore_ps16k/arm64-v8a") == 0);
    CHECK(k16 && strcmp(k16->target, "android-37.1") == 0 && strcmp(k16->tag_id, "google_apis_playstore_ps16k") == 0);
    CHECK(k16 && strcmp(k16->tag_display, "Google Play (16 KB)") == 0 && strcmp(k16->android_name, "Android 17 QPR1") == 0);
    CHECK(k16 && k16->playstore && k16->page16k);

    const SoulXPackage* def = soulx_runtime_default_image();
    CHECK(def && strcmp(def->key, "play") == 0);
    CHECK(soulx_runtime_find("nope") == NULL);
    CHECK(soulx_runtime_find(NULL) == NULL);
    CHECK(soulx_runtime_find("") == NULL);
}

static void test_tool_path(void) {
    char buf[PATH_MAX];
    char want[PATH_MAX];
    CHECK(soulx_tool_path("adb", buf, sizeof(buf)) == 0);
    home_path(want, sizeof(want), "sdk/platform-tools/adb");
    CHECK(strcmp(buf, want) == 0);
    CHECK(soulx_tool_path("emulator", buf, sizeof(buf)) == 0);
    home_path(want, sizeof(want), "sdk/emulator/emulator");
    CHECK(strcmp(buf, want) == 0);
    CHECK(soulx_tool_path("qemu", buf, sizeof(buf)) == -1);
    CHECK(soulx_tool_path(NULL, buf, sizeof(buf)) == -1);
    char tiny[8];
    CHECK(soulx_tool_path("adb", tiny, sizeof(tiny)) == -1);
}

// ---------------------------------------------------------------------------
// Markers
// ---------------------------------------------------------------------------

static void test_markers(void) {
    CHECK(soulx_ensure_layout() == 0);
    const SoulXPackage* eng = soulx_runtime_find("emulator");
    const SoulXPackage* pt = soulx_runtime_find("platform-tools");
    const SoulXPackage* play = soulx_runtime_find("play");
    const SoulXPackage* google = soulx_runtime_find("google");
    if (!eng || !pt || !play || !google) {
        CHECK(false);
        return;
    }
    CHECK(!soulx_runtime_installed(eng));
    CHECK(!soulx_runtime_installed(NULL));
    CHECK(!soulx_runtime_ready(NULL));

    // engine: executable + marker
    char dir[PATH_MAX], exe[PATH_MAX], marker[PATH_MAX];
    home_path(dir, sizeof(dir), "sdk/emulator");
    CHECK(soulx_mkdirs(dir) == 0);
    home_path(exe, sizeof(exe), "sdk/emulator/emulator");
    CHECK(make_executable(exe, "#!fake engine\n"));
    CHECK(soulx_runtime_write_marker(eng) == 0);
    CHECK(!soulx_runtime_installed(eng));            // launcher present, but the qemu binaries are missing
    char qemu[PATH_MAX];
    home_path(qemu, sizeof(qemu), "sdk/emulator/qemu/darwin-aarch64");
    CHECK(soulx_mkdirs(qemu) == 0);
    home_path(qemu, sizeof(qemu), "sdk/emulator/qemu/darwin-aarch64/qemu-system-aarch64");
    CHECK(make_executable(qemu, "#!fake qemu\n"));
    home_path(qemu, sizeof(qemu), "sdk/emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless");
    CHECK(make_executable(qemu, "#!fake qemu headless\n"));
    CHECK(soulx_runtime_installed(eng));

    home_path(marker, sizeof(marker), "sdk/.soulx/emulator.installed");
    char* text = NULL;
    size_t len = 0;
    CHECK(soulx_read_file(marker, &text, &len) == 0);
    CHECK(text && strcmp(text, "sha1=3af4fe44ce82b3d88ae5678a53735f27ad729c15\nversion=37.2.12\n") == 0);
    free(text);

    const char* stale = "sha1=0000000000000000000000000000000000000000\nversion=37.2.11\n";
    CHECK(soulx_write_file_atomic(marker, stale, strlen(stale)) == 0);
    CHECK(!soulx_runtime_installed(eng));
    const char* upper = "version=37.2.12\nsha1=3AF4FE44CE82B3D88AE5678A53735F27AD729C15\r\n";
    CHECK(soulx_write_file_atomic(marker, upper, strlen(upper)) == 0);
    CHECK(soulx_runtime_installed(eng));             // case-insensitive, any line order, CRLF
    CHECK(soulx_runtime_write_marker(eng) == 0);
    CHECK(chmod(exe, 0644) == 0);
    CHECK(!soulx_runtime_installed(eng));            // launcher must be executable
    CHECK(chmod(exe, 0755) == 0);
    CHECK(soulx_runtime_installed(eng));

    // platform-tools
    home_path(dir, sizeof(dir), "sdk/platform-tools");
    CHECK(soulx_mkdirs(dir) == 0);
    CHECK(soulx_runtime_write_marker(pt) == 0);
    CHECK(!soulx_runtime_installed(pt));             // adb missing
    home_path(exe, sizeof(exe), "sdk/platform-tools/adb");
    CHECK(make_executable(exe, "#!fake adb\n"));
    CHECK(soulx_runtime_installed(pt));

    // default image
    CHECK(!soulx_runtime_ready("play"));
    home_path(dir, sizeof(dir), "sdk/system-images/android-37.0/google_apis_playstore/arm64-v8a");
    CHECK(soulx_runtime_write_marker(play) == 0);
    CHECK(!soulx_runtime_installed(play));           // install dir missing
    CHECK(soulx_mkdirs(dir) == 0);
    CHECK(!soulx_runtime_installed(play));           // dir present, but the image files are missing
    make_image_files(dir);
    CHECK(soulx_runtime_installed(play));
    CHECK(soulx_runtime_ready("play"));
    CHECK(soulx_runtime_ready(NULL));
    CHECK(soulx_runtime_ready(""));
    CHECK(!soulx_runtime_ready("google"));
    CHECK(!soulx_runtime_ready("emulator"));
    CHECK(!soulx_runtime_ready("bogus"));
    CHECK(!soulx_runtime_installed(google));

    CHECK(soulx_runtime_print_status() == 0);

    CHECK(soulx_rmtree(dir) == 0);
    CHECK(!soulx_runtime_installed(play));
    CHECK(!soulx_runtime_ready("play"));
}

// ---------------------------------------------------------------------------
// License extraction
// ---------------------------------------------------------------------------

static const char k_xml[] =
    "<?xml version='1.0' encoding='utf-8'?>\n"
    "<sdk:sdk-repository xmlns:sdk=\"http://schemas.android.com/sdk/android/repo/repository2/03\">\n"
    "  <licenses><note>not a license element</note></licenses>\n"
    "  <license id=\"android-sdk-preview-license\" type=\"text\">Preview terms</license>\n"
    "  <license id=\"android-sdk-license\" type=\"text\">Terms and Conditions\n"
    "\n"
    "1. &quot;SDK&quot; &amp; &lt;tools&gt; aren&apos;t &#65;&#x42;&#X43; &#169; &#x1F600; &bogus; &#xD800; &#0; &amp\n"
    "Line 3</license>\n"
    "  <license type=\"text\" id='android-sdk-arm-dbt-license'>ARM DBT\r\nterms\r</license>\n"
    "  <license id=\"empty-license\"/>\n"
    "  <license id=\"cdata-license\"><![CDATA[a < b && c &amp;]]> tail</license>\n"
    "  <license note=\"a>b\" id=\"gt-in-attr\">gt ok</license>\n"
    "  <license\n      id=\"multiline-tag\"\n      type=\"text\">multi</license>\n"
    "</sdk:sdk-repository>\n";

static void expect_license(const char* id, const char* want) {
    char* out = NULL;
    int rc = soulx_runtime_extract_license(k_xml, id, &out);
    CHECK(rc == 0);
    CHECK(out && strcmp(out, want) == 0);
    if (rc != 0 || !out || strcmp(out, want) != 0) {
        fprintf(stderr, "  license %s: got [%s]\n", id, out ? out : "(null)");
    }
    free(out);
}

static void test_license_extraction(void) {
    expect_license("android-sdk-license",
                   "Terms and Conditions\n\n"
                   "1. \"SDK\" & <tools> aren't ABC \xC2\xA9 \xF0\x9F\x98\x80 &bogus; &#xD800; &#0; &amp\n"
                   "Line 3");
    expect_license("android-sdk-preview-license", "Preview terms");
    expect_license("android-sdk-arm-dbt-license", "ARM DBT\nterms\n");
    expect_license("empty-license", "");
    expect_license("cdata-license", "a < b && c &amp; tail");
    expect_license("gt-in-attr", "gt ok");
    expect_license("multiline-tag", "multi");

    char* out = (char*)1;
    CHECK(soulx_runtime_extract_license(k_xml, "missing-license", &out) == -1 && out == NULL);
    CHECK(soulx_runtime_extract_license(k_xml, "android-sdk", &out) == -1 && out == NULL);   // no prefix match
    CHECK(soulx_runtime_extract_license(NULL, "x", &out) == -1);
    CHECK(soulx_runtime_extract_license(k_xml, NULL, &out) == -1);
    CHECK(soulx_runtime_extract_license(k_xml, "", &out) == -1);
    CHECK(soulx_runtime_extract_license(k_xml, "x", NULL) == -1);
    CHECK(soulx_runtime_extract_license("<license id=\"x\">no end", "x", &out) == -1 && out == NULL);
    CHECK(soulx_runtime_extract_license("<license id=\"x\"", "x", &out) == -1 && out == NULL);
    CHECK(soulx_runtime_extract_license("<license id=\"x>", "x", &out) == -1 && out == NULL);
    CHECK(soulx_runtime_extract_license("", "x", &out) == -1 && out == NULL);
}

// ---------------------------------------------------------------------------
// Setup argument validation, adb key, doctor
// ---------------------------------------------------------------------------

static void test_setup_validation(void) {
    SoulXSetupOpts opts = { .accept_licenses = true, .keep_downloads = true, .force = false };
    CHECK(soulx_runtime_setup("bogus", &opts) == -1);
    CHECK(soulx_runtime_setup("emulator", &opts) == -1);       // not a system image
    CHECK(soulx_runtime_setup("platform-tools", NULL) == -1);
}

static void test_adb_key(const char* self_exe) {
    char key[PATH_MAX], adb[PATH_MAX];
    home_path(key, sizeof(key), "home/.android/adbkey");
    home_path(adb, sizeof(adb), "sdk/platform-tools/adb");
    unlink(adb);
    CHECK(soulx_ensure_adb_key() == -1);             // no adb installed
    CHECK(!soulx_path_exists(key));

    // A copy of this binary acts as adb: "adb keygen <file>" records what it saw.
    char* data = NULL;
    size_t len = 0;
    CHECK(self_exe && soulx_read_file(self_exe, &data, &len) == 0);
    if (!data) return;
    CHECK(soulx_write_file_atomic(adb, data, len) == 0);
    CHECK(chmod(adb, 0755) == 0);
    free(data);

    CHECK(soulx_ensure_adb_key() == 0);
    char* got = NULL;
    CHECK(soulx_read_file(key, &got, NULL) == 0);
    char want[PATH_MAX * 2];
    snprintf(want, sizeof(want), "fake-adbkey\nHOME=%s/home\nANDROID_ADB_SERVER_PORT=5137\nANDROID_SDK_HOME=\n", g_home);
    CHECK(got && strcmp(got, want) == 0);
    if (got && strcmp(got, want) != 0) fprintf(stderr, "  adbkey content: [%s]\n", got);
    free(got);

    CHECK(soulx_write_file_atomic(key, "existing", 8) == 0);
    CHECK(soulx_ensure_adb_key() == 0);              // present: adb is not run again
    got = NULL;
    CHECK(soulx_read_file(key, &got, NULL) == 0 && got && strcmp(got, "existing") == 0);
    free(got);
}

static void test_doctor(void) {
    // Everything faked above is removed: the runtime counts as missing -> at least one blocker.
    char sdk[PATH_MAX];
    home_path(sdk, sizeof(sdk), "sdk");
    CHECK(soulx_rmtree(sdk) == 0);
    CHECK(soulx_ensure_layout() == 0);
    CHECK(soulx_doctor() >= 1);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

// "adb keygen <file>" mode used by test_adb_key (runs with the SoulX environment).
static int fake_adb(int argc, char** argv) {
    if (argc < 3) return 2;
    const char* home = getenv("HOME");
    const char* port = getenv("ANDROID_ADB_SERVER_PORT");
    const char* sdk_home = getenv("ANDROID_SDK_HOME");
    FILE* f = fopen(argv[2], "w");
    if (!f) return 1;
    fprintf(f, "fake-adbkey\nHOME=%s\nANDROID_ADB_SERVER_PORT=%s\nANDROID_SDK_HOME=%s\n",
            home ? home : "", port ? port : "", sdk_home ? sdk_home : "");
    return fclose(f) == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    const char* base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (strcmp(base, "adb") == 0) {
        return (argc >= 2 && strcmp(argv[1], "keygen") == 0) ? fake_adb(argc, argv) : 1;
    }

    char self_exe[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(getpid(), self_exe, sizeof(self_exe)) <= 0) self_exe[0] = '\0';

    char tmpl[] = "/private/tmp/soulx-test-rt-XXXXXX";
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(g_home, sizeof(g_home), "%s", tmpl);
    setenv("SOULX_HOME", g_home, 1);
    setenv("ANDROID_SDK_HOME", "/bogus/android-sdk-home", 1);  // must not reach SoulX's children

    CHECK(strcmp(soulx_home(), g_home) == 0);
    test_table();
    test_tool_path();
    test_markers();
    test_license_extraction();
    test_setup_validation();
    test_adb_key(self_exe[0] ? self_exe : NULL);
    test_doctor();

    if (g_failures == 0) {
        rm_rf(g_home);
        printf("test_runtime: all %d checks passed\n", g_checks);
        return 0;
    }
    fprintf(stderr, "test_runtime: %d of %d checks FAILED (kept %s)\n", g_failures, g_checks, g_home);
    return 1;
}
