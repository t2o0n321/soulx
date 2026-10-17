/*
 * SoulX Android Emulator - macOS runtime provisioning
 *
 * SoulX owns its engine under $SOULX_HOME/sdk: Google's open-source Android Emulator, platform-tools
 * (adb) and Android 17 system images, pinned below by URL, size and SHA-1 (Google publishes SHA-1
 * only). `soulx setup` shows and records the Android SDK license, downloads with /usr/bin/curl
 * (resumable), verifies size + SHA-1 with CommonCrypto, unpacks with /usr/bin/unzip into a staging
 * directory, renames it into place and writes an install marker sdk/.soulx/<key>.installed.
 * Nothing here reads or writes Android Studio's SDK or ~/.android.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/proc_info.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <sys/xattr.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#include <CommonCrypto/CommonDigest.h>

#include "soulx_mac.h"

// Exposed for tests/macos/test_runtime.c (not part of soulx_mac.h).
int soulx_runtime_extract_license(const char* xml, const char* id, char** out);
int soulx_runtime_write_marker(const SoulXPackage* p);

#define CURL_BIN            "/usr/bin/curl"
#define UNZIP_BIN           "/usr/bin/unzip"
#define XATTR_BIN           "/usr/bin/xattr"
#define CODESIGN_BIN        "/usr/bin/codesign"
#define QEMU_REL            "sdk/emulator/qemu/darwin-aarch64/qemu-system-aarch64"
#define TERMS_URL           "https://developer.android.com/studio/terms"

#define GB                  1000000000ULL
#define DISK_MARGIN         (1ULL * GB)     // setup aborts below need + margin
#define DISK_COMFORT        (8ULL * GB)     // setup warns below need + comfort (instance data, snapshots)
#define HASH_BUF_SIZE       (1u << 20)      // streaming SHA-1 read size
#define MAX_CANDIDATES      8               // same-size files in downloads/ worth hashing
#define MAX_REG_LIST        64
#define LICENSE_PREVIEW     25              // lines of license text shown before the prompt

// Google's repository documents that carry the <license> texts (searched in this order).
static const char* const k_license_docs[] = {
    "https://dl.google.com/android/repository/repository2-3.xml",
    "https://dl.google.com/android/repository/sys-img/google_apis_playstore/sys-img2-4.xml",
};
#define LICENSE_DOC_COUNT   (sizeof(k_license_docs) / sizeof(k_license_docs[0]))

// ---------------------------------------------------------------------------
// Pinned packages (sizes and SHA-1 from Google's repository2-3.xml / sys-img2-4.xml)
// ---------------------------------------------------------------------------

static const SoulXPackage k_packages[] = {
    {
        .key = "emulator",
        .kind = SOULX_PKG_ENGINE,
        .sdk_id = "emulator",
        .version = "37.2.12",
        .url = "https://dl.google.com/android/repository/emulator-darwin_aarch64-16428233.zip",
        .size = 416112708ULL,
        .sha1 = "3af4fe44ce82b3d88ae5678a53735f27ad729c15",
        .license_id = "android-sdk-license",
        .zip_root = "emulator",
        .install_rel = "emulator",
        .description = "Android Emulator engine 37.2.12 (QEMU, Apple Silicon)",
    },
    {
        .key = "platform-tools",
        .kind = SOULX_PKG_PLATFORM_TOOLS,
        .sdk_id = "platform-tools",
        .version = "37.0.1",
        .url = "https://dl.google.com/android/repository/platform-tools_r37.0.1-darwin.zip",
        .size = 16110554ULL,
        .sha1 = "6ae73f4de6452dc57e62ec02b68eed92a4c21661",
        .license_id = "android-sdk-license",
        .zip_root = "platform-tools",
        .install_rel = "platform-tools",
        .description = "Android SDK Platform-Tools 37.0.1 (adb)",
    },
    {
        .key = "play",
        .kind = SOULX_PKG_IMAGE,
        .sdk_id = "system-images;android-37.0;google_apis_playstore;arm64-v8a",
        .version = "r06",
        .url = "https://dl.google.com/android/repository/sys-img/google_apis_playstore/arm64-v8a-37.0_r06.zip",
        .size = 2257751510ULL,
        .sha1 = "642cf494464b4d1d4d37fa50630fe153599be3fb",
        .license_id = "android-sdk-arm-dbt-license",
        .zip_root = "arm64-v8a",
        .install_rel = "system-images/android-37.0/google_apis_playstore/arm64-v8a",
        .target = "android-37.0",
        .tag_id = "google_apis_playstore",
        .tag_display = "Google Play",
        .android_name = "Android 17",
        .playstore = true,
        .page16k = false,
        .description = "Android 17 (API 37) Google Play r06, arm64-v8a, 4 KB pages (default)",
    },
    {
        .key = "google",
        .kind = SOULX_PKG_IMAGE,
        .sdk_id = "system-images;android-37.0;google_apis;arm64-v8a",
        .version = "r06",
        .url = "https://dl.google.com/android/repository/sys-img/google_apis/arm64-v8a-37.0_r06.zip",
        .size = 2194282378ULL,
        .sha1 = "e007fb23024c59bffdf944b95c61143bc38101ce",
        .license_id = "android-sdk-arm-dbt-license",
        .zip_root = "arm64-v8a",
        .install_rel = "system-images/android-37.0/google_apis/arm64-v8a",
        .target = "android-37.0",
        .tag_id = "google_apis",
        .tag_display = "Google APIs",
        .android_name = "Android 17",
        .playstore = false,
        .page16k = false,
        .description = "Android 17 (API 37) Google APIs r06, arm64-v8a (userdebug, adb root)",
    },
    {
        .key = "play16k",
        .kind = SOULX_PKG_IMAGE,
        .sdk_id = "system-images;android-37.1;google_apis_playstore_ps16k;arm64-v8a",
        .version = "r09",
        .url = "https://dl.google.com/android/repository/sys-img/google_apis_playstore/arm64-v8a-playstore-ps16k-37.1_r09.zip",
        .size = 2127264536ULL,
        .sha1 = "26be7d5274e6f5455b0028cdaa98b950d19c3077",
        .license_id = "android-sdk-arm-dbt-license",
        .zip_root = "arm64-v8a",
        .install_rel = "system-images/android-37.1/google_apis_playstore_ps16k/arm64-v8a",
        .target = "android-37.1",
        .tag_id = "google_apis_playstore_ps16k",
        .tag_display = "Google Play (16 KB)",
        .android_name = "Android 17 QPR1",
        .playstore = true,
        .page16k = true,
        .description = "Android 17 QPR1 (API 37.1) Google Play r09, arm64-v8a, 16 KB pages",
    },
};

#define PACKAGE_COUNT (sizeof(k_packages) / sizeof(k_packages[0]))

// Bytes on disk after extraction: the sum of each pinned zip's uncompressed entries, read from the
// zips' central directories (system images differ a lot: google 1.6x and play16k 4.1x their zip).
static uint64_t unpacked_size(const SoulXPackage* p) {
    static const struct {
        const char* key;
        uint64_t bytes;
    } k_unpacked[] = {
        { "emulator", 1309740864ULL },
        { "platform-tools", 38814256ULL },
        { "play", 2887305349ULL },
        { "google", 4632457058ULL },
        { "play16k", 8762254637ULL },
    };
    for (size_t i = 0; i < sizeof(k_unpacked) / sizeof(k_unpacked[0]); i++) {
        if (strcmp(p->key, k_unpacked[i].key) == 0) return k_unpacked[i].bytes;
    }
    return p->size * 5;                     // not measured: a generous estimate
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static bool streq(const char* a, const char* b) {
    return a && b && strcmp(a, b) == 0;
}

// Display only: buffers are sized for the longest value ("1234.5 GB").
static void format_bytes(uint64_t b, char* buf, size_t cap) {
    if (b >= GB) snprintf(buf, cap, "%.1f GB", (double)b / 1e9);
    else if (b >= 1000000ULL) snprintf(buf, cap, "%.0f MB", (double)b / 1e6);
    else if (b >= 1000ULL) snprintf(buf, cap, "%.0f KB", (double)b / 1e3);
    else snprintf(buf, cap, "%llu B", (unsigned long long)b);
}

// "Android 17 (API 37) Google Play" (display only; 160-byte buffers are ample).
static void image_label(const SoulXPackage* p, char* buf, size_t cap) {
    const char* api = p->target ? p->target : "?";
    if (strncmp(api, "android-", 8) == 0) api += 8;
    char v[16];
    snprintf(v, sizeof(v), "%s", api);
    size_t n = strlen(v);
    if (n > 2 && strcmp(v + n - 2, ".0") == 0) v[n - 2] = '\0';
    snprintf(buf, cap, "%s (API %s) %s", p->android_name ? p->android_name : "Android", v,
             p->tag_display ? p->tag_display : p->key);
}

static const SoulXPackage* find_kind(SoulXPackageKind kind) {
    for (size_t i = 0; i < PACKAGE_COUNT; i++) {
        if (k_packages[i].kind == kind) return &k_packages[i];
    }
    return NULL;
}

static void image_keys(char* buf, size_t cap) {
    size_t off = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < PACKAGE_COUNT; i++) {
        if (k_packages[i].kind != SOULX_PKG_IMAGE) continue;
        int n = snprintf(buf + off, cap - off, "%s%s", off ? ", " : "", k_packages[i].key);
        if (n < 0 || (size_t)n >= cap - off) break;
        off += (size_t)n;
    }
}

static void iso_now(char* buf, size_t cap) {
    time_t t = time(NULL);
    struct tm tm;
    if (!gmtime_r(&t, &tm) || strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        snprintf(buf, cap, "unknown");
    }
}

static int sysctl_i64(const char* name, int64_t* out) {
    union { int32_t i32; int64_t i64; } v;
    size_t len = sizeof(v);
    memset(&v, 0, sizeof(v));
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0) return -1;
    if (len == sizeof(int32_t)) *out = v.i32;
    else if (len == sizeof(int64_t)) *out = v.i64;
    else return -1;
    return 0;
}

static bool host_is_arm64(void) {
    int64_t v = 0;
    if (sysctl_i64("hw.optional.arm64", &v) == 0 && v == 1) return true;
    struct utsname u;
    return uname(&u) == 0 && strcmp(u.machine, "arm64") == 0;
}

// Last part of a captured error stream, for one-line error messages.
static const char* tail_of(const char* s, size_t max) {
    if (!s) return "";
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) n--;
    return n > max ? s + n - max : s;
}

// ---------------------------------------------------------------------------
// SHA-1 (CommonCrypto; Google only publishes SHA-1 for SDK packages)
// ---------------------------------------------------------------------------

static void hex_digest(const unsigned char* md, size_t n, char* hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        hex[2 * i] = digits[md[i] >> 4];
        hex[2 * i + 1] = digits[md[i] & 0x0f];
    }
    hex[2 * n] = '\0';
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static void sha1_hex_mem(const void* data, size_t len, char hex[2 * CC_SHA1_DIGEST_LENGTH + 1]) {
    CC_SHA1_CTX ctx;
    CC_SHA1_Init(&ctx);
    const unsigned char* p = data;
    while (len > 0) {
        CC_LONG chunk = len > 0x40000000u ? 0x40000000u : (CC_LONG)len;
        CC_SHA1_Update(&ctx, p, chunk);
        p += chunk;
        len -= chunk;
    }
    unsigned char md[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1_Final(md, &ctx);
    hex_digest(md, sizeof(md), hex);
}

// Streams a file through SHA-1 with a progress line; 0 ok, -1 on error or Ctrl+C.
static int sha1_hex_file(const char* path, const char* label, char hex[2 * CC_SHA1_DIGEST_LENGTH + 1]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    uint64_t total = (fstat(fd, &st) == 0 && st.st_size > 0) ? (uint64_t)st.st_size : 0;
    unsigned char* buf = malloc(HASH_BUF_SIZE);
    if (!buf) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }

    CC_SHA1_CTX ctx;
    CC_SHA1_Init(&ctx);
    uint64_t done = 0;
    double last = 0;
    int rc = 0;
    for (;;) {
        if (g_soulx_interrupted) {
            rc = -1;
            errno = EINTR;
            break;
        }
        ssize_t r = read(fd, buf, HASH_BUF_SIZE);
        if (r < 0) {
            if (errno == EINTR) continue;
            rc = -1;
            break;
        }
        if (r == 0) break;
        CC_SHA1_Update(&ctx, buf, (CC_LONG)r);
        done += (uint64_t)r;
        double now = soulx_now_ms();
        if (label && total > 0 && now - last >= 250) {
            last = now;
            soulx_status_line("Verifying %s... %d%%", label, (int)(done * 100 / total));
        }
    }
    if (label) soulx_status_done();
    int saved = errno;
    free(buf);
    close(fd);
    if (rc != 0) {
        errno = saved;
        return -1;
    }
    unsigned char md[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1_Final(md, &ctx);
    hex_digest(md, sizeof(md), hex);
    return 0;
}

#pragma clang diagnostic pop

// ---------------------------------------------------------------------------
// Package table
// ---------------------------------------------------------------------------

const SoulXPackage* soulx_runtime_packages(size_t* count) {
    if (count) *count = PACKAGE_COUNT;
    return k_packages;
}

const SoulXPackage* soulx_runtime_find(const char* key) {
    if (!key) return NULL;
    for (size_t i = 0; i < PACKAGE_COUNT; i++) {
        if (strcmp(k_packages[i].key, key) == 0) return &k_packages[i];
    }
    return NULL;
}

const SoulXPackage* soulx_runtime_default_image(void) {
    return soulx_runtime_find("play");
}

int soulx_tool_path(const char* tool, char* buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    buf[0] = '\0';
    if (streq(tool, "adb")) return soulx_path(buf, cap, "sdk/platform-tools/adb");
    if (streq(tool, "emulator")) return soulx_path(buf, cap, "sdk/emulator/emulator");
    return -1;
}

// ---------------------------------------------------------------------------
// The Metal renderer: the engine's own ANGLE, pointed at Metal
// ---------------------------------------------------------------------------
//
// The engine ships ANGLE (sdk/emulator/lib64/gles_angle) with its Metal backend built in and, in its
// "swangle" GPU mode, runs the guest's OpenGL ES on it: GLES straight to Metal, one translation instead
// of the default GLES -> desktop OpenGL -> Metal through Apple's deprecated OpenGL (which also leaked
// tens of GB in an hour of a game). But the engine overwrites ANGLE_DEFAULT_PLATFORM with "vulkan" (on
// SwiftShader, the CPU). So SoulX renames the variable this ANGLE reads to SOULX_ANGLE_PLATFORM_ENV (same
// length, 4 bytes change), re-signs the library ad hoc (the engine is signed with
// disable-library-validation) and sets the variable itself when it starts an engine. The library as
// Google ships it stays next to it as libGLESv2.dylib.google; `soulx setup` replaces both with a fresh
// engine, and the next start prepares that one again.

#define ANGLE_LIB_REL       "sdk/emulator/lib64/gles_angle/libGLESv2.dylib"
#define ANGLE_ENV_GOOGLE    "ANGLE_DEFAULT_PLATFORM"

static size_t count_bytes(const char* hay, size_t len, const char* needle, size_t n) {
    size_t count = 0;
    for (const char* p = hay; (p = memmem(p, len - (size_t)(p - hay), needle, n)) != NULL; p += n) count++;
    return count;
}

int soulx_runtime_prepare_angle(void) {
    // both names with their NUL, so a longer name containing either one never matches
    static const char google[] = ANGLE_ENV_GOOGLE;
    static const char soulx[] = SOULX_ANGLE_PLATFORM_ENV;
    _Static_assert(sizeof(google) == sizeof(soulx), "the renamed variable must keep the length of the original");

    char lib[PATH_MAX], orig[PATH_MAX], tmp[PATH_MAX];
    if (soulx_path(lib, sizeof(lib), "%s", ANGLE_LIB_REL) != 0 ||
        soulx_path(orig, sizeof(orig), "%s.google", ANGLE_LIB_REL) != 0 ||
        soulx_path(tmp, sizeof(tmp), "%s.soulx-%d", ANGLE_LIB_REL, (int)getpid()) != 0) {
        soulx_log(SOULX_LOG_ERROR, "SoulX home path too long: %s", soulx_home());
        return -1;
    }
    char* data = NULL;
    size_t len = 0;
    if (soulx_read_file(lib, &data, &len) != 0) {
        soulx_log(SOULX_LOG_WARN, "cannot read the engine's ANGLE (%s): %s", lib, strerror(errno));
        return -1;
    }
    // the name Soul used before the SoulX rename; such a library is prepared again under the new name
    static const char legacy[] = "SOULA_DEFAULT_PLATFORM";
    _Static_assert(sizeof(legacy) == sizeof(soulx), "the legacy name has the same length");
    size_t ours = count_bytes(data, len, soulx, sizeof(soulx));
    size_t theirs = count_bytes(data, len, google, sizeof(google));
    const char* from = google;
    if (theirs == 0 && ours == 0 && count_bytes(data, len, legacy, sizeof(legacy)) == 1) {
        theirs = 1;
        from = legacy;
    }
    if (ours == 1 && theirs == 0) {
        free(data);
        return 0;                                    // prepared by an earlier start
    }
    if (ours != 0 || theirs != 1) {
        free(data);
        soulx_log(SOULX_LOG_WARN, "the engine's ANGLE (%s) is not the build SoulX knows (%zu/%zu platform variables)",
                 lib, theirs, ours);
        return -1;
    }

    int rc = -1;
    if (!soulx_path_exists(orig) && soulx_write_file_atomic(orig, data, len) != 0) {
        soulx_log(SOULX_LOG_WARN, "cannot keep the original ANGLE as %s: %s", orig, strerror(errno));
        goto out;
    }
    memcpy(memmem(data, len, from, sizeof(google)), soulx, sizeof(soulx));
    if (soulx_write_file_atomic(tmp, data, len) != 0) {
        soulx_log(SOULX_LOG_WARN, "cannot write %s: %s", tmp, strerror(errno));
        goto out;
    }
    const char* argv[] = { CODESIGN_BIN, "--force", "--sign", "-", tmp, NULL };
    SoulXCmd cmd = { .argv = argv, .timeout_ms = 60000 };
    SoulXCmdResult res;
    if (soulx_run(&cmd, &res) != 0) {
        soulx_log(SOULX_LOG_WARN, "cannot sign the Metal renderer (%s): %s", tmp,
                 res.err && res.err[0] ? res.err : "codesign failed");
        soulx_cmd_result_free(&res);
        (void)unlink(tmp);
        goto out;
    }
    soulx_cmd_result_free(&res);
    if (rename(tmp, lib) != 0) {
        soulx_log(SOULX_LOG_WARN, "cannot install the Metal renderer as %s: %s", lib, strerror(errno));
        (void)unlink(tmp);
        goto out;
    }
    soulx_log(SOULX_LOG_DEBUG, "prepared the Metal renderer: %s (original kept as %s)", lib, orig);
    rc = 0;

out:
    free(data);
    return rc;
}

// ---------------------------------------------------------------------------
// Install markers: sdk/.soulx/<key>.installed = "sha1=<sha1>\nversion=<version>\n"
// ---------------------------------------------------------------------------

static int marker_path(const SoulXPackage* p, char* buf, size_t cap) {
    return soulx_path(buf, cap, "sdk/.soulx/%s.installed", p->key);
}

static int install_dir(const SoulXPackage* p, char* buf, size_t cap) {
    return soulx_path(buf, cap, "sdk/%s", p->install_rel);
}

int soulx_runtime_write_marker(const SoulXPackage* p) {
    if (!p) return -1;
    char path[PATH_MAX];
    char dir[PATH_MAX];
    char body[256];
    if (marker_path(p, path, sizeof(path)) != 0 || soulx_path(dir, sizeof(dir), "sdk/.soulx") != 0) return -1;
    int n = snprintf(body, sizeof(body), "sha1=%s\nversion=%s\n", p->sha1, p->version);
    if (n < 0 || (size_t)n >= sizeof(body)) return -1;
    if (soulx_mkdirs(dir) != 0) return -1;
    return soulx_write_file_atomic(path, body, (size_t)n);
}

// Value of the first "key=value" line (trimmed) in text; 0 when found.
static int kv_get(const char* text, const char* key, char* out, size_t cap) {
    size_t kl = strlen(key);
    const char* line = text;
    while (line && *line) {
        const char* eol = strchr(line, '\n');
        size_t ll = eol ? (size_t)(eol - line) : strlen(line);
        if (ll > kl && strncmp(line, key, kl) == 0 && line[kl] == '=') {
            const char* v = line + kl + 1;
            size_t vl = ll - kl - 1;
            while (vl > 0 && (*v == ' ' || *v == '\t')) {
                v++;
                vl--;
            }
            while (vl > 0 && (v[vl - 1] == '\r' || v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
            if (vl >= cap) return -1;
            memcpy(out, v, vl);
            out[vl] = '\0';
            return 0;
        }
        line = eol ? eol + 1 : NULL;
    }
    return -1;
}

static bool marker_matches(const SoulXPackage* p) {
    char path[PATH_MAX];
    if (marker_path(p, path, sizeof(path)) != 0) return false;
    char* text = NULL;
    if (soulx_read_file(path, &text, NULL) != 0) return false;
    char sha[64];
    bool ok = kv_get(text, "sha1", sha, sizeof(sha)) == 0 && strcasecmp(sha, p->sha1) == 0;
    free(text);
    return ok;
}

static bool is_executable_file(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

// Files every pinned system image contains (checked in the central directory of each zip): an image
// directory without them cannot boot, whatever its install marker says.
static const char* const k_image_files[] = {
    "system.img", "vendor.img", "ramdisk.img", "kernel-ranchu", "source.properties",
};

static bool image_files_present(const char* dir) {
    for (size_t i = 0; i < sizeof(k_image_files) / sizeof(k_image_files[0]); i++) {
        char path[PATH_MAX];
        struct stat st;
        int n = snprintf(path, sizeof(path), "%s/%s", dir, k_image_files[i]);
        if (n < 0 || (size_t)n >= sizeof(path) || stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            return false;
        }
    }
    return true;
}

bool soulx_runtime_installed(const SoulXPackage* p) {
    if (!p || !marker_matches(p)) return false;
    char dir[PATH_MAX];
    if (install_dir(p, dir, sizeof(dir)) != 0 || !soulx_is_dir(dir)) return false;
    char exe[PATH_MAX];
    switch (p->kind) {
    case SOULX_PKG_ENGINE:
        // the launcher and the qemu binaries it execs (-no-window uses the headless one)
        return soulx_tool_path("emulator", exe, sizeof(exe)) == 0 && is_executable_file(exe) &&
               soulx_path(exe, sizeof(exe), "%s", QEMU_REL) == 0 && is_executable_file(exe) &&
               soulx_path(exe, sizeof(exe), "%s-headless", QEMU_REL) == 0 && is_executable_file(exe);
    case SOULX_PKG_PLATFORM_TOOLS:
        return soulx_tool_path("adb", exe, sizeof(exe)) == 0 && is_executable_file(exe);
    case SOULX_PKG_IMAGE:
        return image_files_present(dir);
    }
    return false;
}

bool soulx_runtime_ready(const char* image_key) {
    const SoulXPackage* image = (image_key && image_key[0]) ? soulx_runtime_find(image_key) : soulx_runtime_default_image();
    if (!image || image->kind != SOULX_PKG_IMAGE) return false;
    return soulx_runtime_installed(find_kind(SOULX_PKG_ENGINE)) &&
           soulx_runtime_installed(find_kind(SOULX_PKG_PLATFORM_TOOLS)) &&
           soulx_runtime_installed(image);
}

typedef enum { PKG_MISSING, PKG_OUTDATED, PKG_INCOMPLETE, PKG_OK } PkgState;

static PkgState package_state(const SoulXPackage* p) {
    char path[PATH_MAX];
    if (marker_path(p, path, sizeof(path)) != 0 || !soulx_path_exists(path)) return PKG_MISSING;
    if (!marker_matches(p)) return PKG_OUTDATED;
    return soulx_runtime_installed(p) ? PKG_OK : PKG_INCOMPLETE;
}

static const char* state_name(PkgState s) {
    switch (s) {
    case PKG_OK:         return "installed";
    case PKG_OUTDATED:   return "outdated";
    case PKG_INCOMPLETE: return "incomplete";
    case PKG_MISSING:    return "not installed";
    }
    return "not installed";
}

// ---------------------------------------------------------------------------
// License text extraction (plain string search; exposed for unit tests)
// ---------------------------------------------------------------------------

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// Decodes XML character data: the five predefined entities, &#NN; / &#xHH;, CDATA sections and
// CR/CRLF line ends. Unknown or malformed references are kept literally. *out is malloc'd.
static int xml_unescape(const char* src, size_t len, char** out) {
    char* dst = malloc(len + 1);        // decoding never makes the text longer
    if (!dst) return -1;
    size_t i = 0;
    size_t o = 0;
    while (i < len) {
        char c = src[i];
        if (c == '<' && len - i >= 9 && memcmp(src + i, "<![CDATA[", 9) == 0) {
            size_t j = i + 9;
            while (j + 3 <= len && memcmp(src + j, "]]>", 3) != 0) j++;
            size_t end = (j + 3 <= len) ? j : len;
            memcpy(dst + o, src + i + 9, end - (i + 9));
            o += end - (i + 9);
            i = (j + 3 <= len) ? j + 3 : len;
            continue;
        }
        if (c == '\r') {
            dst[o++] = '\n';
            i += (i + 1 < len && src[i + 1] == '\n') ? 2 : 1;
            continue;
        }
        if (c == '&') {
            size_t semi = i + 1;
            while (semi < len && semi - i <= 12 && src[semi] != ';') semi++;
            if (semi < len && src[semi] == ';') {
                const char* ent = src + i + 1;
                size_t el = semi - i - 1;
                char rep[4];
                int rl = 0;
                if (el == 4 && memcmp(ent, "quot", 4) == 0) { rep[0] = '"'; rl = 1; }
                else if (el == 4 && memcmp(ent, "apos", 4) == 0) { rep[0] = '\''; rl = 1; }
                else if (el == 2 && memcmp(ent, "lt", 2) == 0) { rep[0] = '<'; rl = 1; }
                else if (el == 2 && memcmp(ent, "gt", 2) == 0) { rep[0] = '>'; rl = 1; }
                else if (el == 3 && memcmp(ent, "amp", 3) == 0) { rep[0] = '&'; rl = 1; }
                else if (el >= 2 && ent[0] == '#') {
                    bool hex = (ent[1] == 'x' || ent[1] == 'X');
                    size_t k = hex ? 2 : 1;
                    bool valid = k < el;
                    uint32_t cp = 0;
                    for (; k < el && valid; k++) {
                        int d = hex ? hex_value(ent[k]) : ((ent[k] >= '0' && ent[k] <= '9') ? ent[k] - '0' : -1);
                        if (d < 0) {
                            valid = false;
                        } else {
                            cp = cp * (hex ? 16u : 10u) + (uint32_t)d;
                            if (cp > 0x10FFFF) valid = false;
                        }
                    }
                    if (valid && cp != 0 && !(cp >= 0xD800 && cp <= 0xDFFF)) rl = utf8_encode(cp, rep);
                }
                if (rl > 0) {
                    memcpy(dst + o, rep, (size_t)rl);
                    o += (size_t)rl;
                    i = semi + 1;
                    continue;
                }
            }
        }
        dst[o++] = c;
        i++;
    }
    dst[o] = '\0';
    *out = dst;
    return 0;
}

// True when the start-tag attributes in [s, end) contain name="want" (or name='want').
static bool tag_attr_equals(const char* s, const char* end, const char* name, const char* want) {
    size_t nl = strlen(name);
    size_t wl = strlen(want);
    const char* p = s;
    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) p++;
        const char* an = p;
        while (p < end && !isspace((unsigned char)*p) && *p != '=' && *p != '/' && *p != '>') p++;
        size_t al = (size_t)(p - an);
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) break;
        if (*p != '=') {
            if (al == 0) p++;           // stray character: skip it
            continue;                   // attribute without a value
        }
        p++;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || (*p != '"' && *p != '\'')) return false;
        char quote = *p++;
        const char* v = p;
        while (p < end && *p != quote) p++;
        if (p >= end) return false;
        size_t vl = (size_t)(p - v);
        p++;
        if (al == nl && memcmp(an, name, nl) == 0) return vl == wl && memcmp(v, want, wl) == 0;
    }
    return false;
}

int soulx_runtime_extract_license(const char* xml, const char* id, char** out) {
    if (!out) return -1;
    *out = NULL;
    if (!xml || !id || !id[0]) return -1;

    const char* p = xml;
    while ((p = strstr(p, "<license")) != NULL) {
        const char* attrs = p + 8;
        if (!isspace((unsigned char)*attrs) && *attrs != '>' && *attrs != '/') {
            p = attrs;                  // <licenses>, <license-foo>, ...
            continue;
        }
        const char* q = attrs;          // end of the start tag ('>' inside quotes does not count)
        char quote = 0;
        while (*q && (quote || *q != '>')) {
            if (quote) {
                if (*q == quote) quote = 0;
            } else if (*q == '"' || *q == '\'') {
                quote = *q;
            }
            q++;
        }
        if (!*q) return -1;
        bool self_closing = q > attrs && q[-1] == '/';
        if (tag_attr_equals(attrs, self_closing ? q - 1 : q, "id", id)) {
            if (self_closing) {
                *out = calloc(1, 1);
                return *out ? 0 : -1;
            }
            const char* body = q + 1;
            const char* end = strstr(body, "</license>");
            if (!end) return -1;
            return xml_unescape(body, (size_t)(end - body), out);
        }
        p = q + 1;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Licenses: licenses/<id>.txt (text) + licenses/<id>.accepted (acceptance record)
// ---------------------------------------------------------------------------

typedef struct {
    char* body[LICENSE_DOC_COUNT];
    bool tried[LICENSE_DOC_COUNT];
} LicenseDocs;

static bool license_id_valid(const char* id) {
    size_t n = id ? strlen(id) : 0;
    if (n == 0 || n > 64) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

// The SHA-1 recorded when license <id> was accepted ("unavailable" when its text could not be
// shown then); false when it was never accepted.
static bool license_accepted_sha1(const char* id, char* sha, size_t cap) {
    char path[PATH_MAX];
    char* record = NULL;
    sha[0] = '\0';
    if (soulx_path(path, sizeof(path), "licenses/%s.accepted", id) != 0 || soulx_read_file(path, &record, NULL) != 0) {
        return false;
    }
    bool accepted = record[0] != '\0';
    if (accepted && kv_get(record, "sha1", sha, cap) != 0) snprintf(sha, cap, "unavailable");
    free(record);
    return accepted;
}

// Downloads url into $S/tmp/ with curl and returns its content (malloc'd).
static int fetch_url(const char* url, char** body) {
    static unsigned counter = 0;
    *body = NULL;
    char tmp[PATH_MAX];
    if (soulx_path(tmp, sizeof(tmp), "tmp/license-%d-%u.xml", (int)getpid(), counter++) != 0) return -1;
    const char* argv[] = {
        CURL_BIN, "-q", "-fsSL", "--proto", "=https", "--proto-redir", "=https",
        "--retry", "2", "--retry-delay", "1", "--connect-timeout", "15", "--max-time", "120",
        "-o", tmp, url, NULL
    };
    SoulXCmd cmd = { .argv = argv, .timeout_ms = 150000 };
    SoulXCmdResult res;
    int rc = soulx_run(&cmd, &res);
    if (rc != 0) {
        soulx_log(SOULX_LOG_DEBUG, "fetching %s failed (curl exit %d): %s", url, rc, tail_of(res.err, 300));
        soulx_cmd_result_free(&res);
        unlink(tmp);
        return -1;
    }
    soulx_cmd_result_free(&res);
    int r = soulx_read_file(tmp, body, NULL);
    unlink(tmp);
    return r;
}

// The current license text: extracted from Google's repository documents (each fetched at most once
// per setup run) and saved as licenses/<id>.txt, so changed terms are noticed; the saved copy when the
// documents cannot be fetched. source receives where it came from.
static int license_text(const char* id, LicenseDocs* docs, char** text, char* source, size_t source_cap) {
    *text = NULL;
    char path[PATH_MAX];
    if (soulx_path(path, sizeof(path), "licenses/%s.txt", id) != 0) return -1;

    for (size_t i = 0; i < LICENSE_DOC_COUNT; i++) {
        if (!docs->tried[i]) {
            docs->tried[i] = true;
            soulx_log(SOULX_LOG_INFO, "Fetching license texts from %s", k_license_docs[i]);
            if (fetch_url(k_license_docs[i], &docs->body[i]) != 0) docs->body[i] = NULL;
        }
        if (!docs->body[i]) continue;
        if (soulx_runtime_extract_license(docs->body[i], id, text) == 0 && (*text)[0]) {
            if (soulx_write_file_atomic(path, *text, strlen(*text)) != 0) {
                soulx_log(SOULX_LOG_WARN, "cannot save %s: %s", path, strerror(errno));
            }
            snprintf(source, source_cap, "%s", k_license_docs[i]);
            return 0;
        }
        free(*text);
        *text = NULL;
    }

    size_t len = 0;                     // offline: the copy saved by an earlier setup
    if (soulx_read_file(path, text, &len) == 0 && len > 0) {
        soulx_log(SOULX_LOG_DEBUG, "using the saved text of license %s (%s)", id, path);
        snprintf(source, source_cap, "%s", path);
        return 0;
    }
    free(*text);
    *text = NULL;
    return -1;
}

static void show_license(const char* id, const char* text, const char* saved_path, const char* required_by,
                         bool brief) {
    static const char rule[] = "------------------------------------------------------------------------";
    soulx_status_done();
    if (brief) {                        // already accepted (--accept-licenses) by someone who saw it elsewhere
        fprintf(stderr, "License: %s (required by: %s). Full text: %s\n", id, required_by, text ? saved_path : TERMS_URL);
        fflush(stderr);
        return;
    }
    fprintf(stderr, "\n%s\nLicense: %s (required by: %s)\n%s\n", rule, id, required_by, rule);
    if (text) {
        const char* p = text;
        int shown = 0;
        while (*p && shown < LICENSE_PREVIEW) {
            const char* eol = strchr(p, '\n');
            size_t n = eol ? (size_t)(eol - p) : strlen(p);
            fprintf(stderr, "%.*s\n", (int)n, p);
            shown++;
            p = eol ? eol + 1 : p + n;
        }
        int more = 0;
        for (const char* q = p; *q; q++) {
            if (*q == '\n') more++;
        }
        if (*p && (p[strlen(p) - 1] != '\n')) more++;
        if (more > 0) fprintf(stderr, "[... %d more lines ...]\n", more);
        fprintf(stderr, "%s\nFull text: %s\n", rule, saved_path);
    } else {
        fprintf(stderr, "The license text could not be downloaded. Read the Android Software Development\n"
                        "Kit License Agreement at %s before accepting.\n%s\n", TERMS_URL, rule);
    }
    fflush(stderr);
}

// Asks on the terminal; false on "no", EOF or Ctrl+C. Reads raw bytes so Ctrl+C is noticed even
// when the CLI's SIGINT handler restarts system calls.
static bool prompt_yes(const char* question) {
    fprintf(stderr, "%s [y/N] ", question);
    fflush(stderr);
    char line[64];
    size_t n = 0;
    bool overflow = false;
    for (;;) {
        if (g_soulx_interrupted) {
            fputc('\n', stderr);
            return false;
        }
        struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (pr == 0) continue;
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return false;
        }
        if (r == 0) {
            fputc('\n', stderr);
            return false;
        }
        if (c == '\n') break;
        if (n + 1 < sizeof(line)) line[n++] = c;
        else overflow = true;
    }
    line[n] = '\0';
    if (overflow) return false;
    char* s = line;
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) s[--len] = '\0';
    return strcasecmp(s, "y") == 0 || strcasecmp(s, "yes") == 0;
}

static int record_acceptance(const char* id, const char* text, const char* source, const char* how) {
    char sha[2 * CC_SHA1_DIGEST_LENGTH + 1] = "unavailable";
    if (text) sha1_hex_mem(text, strlen(text), sha);
    char when[32];
    iso_now(when, sizeof(when));
    char body[PATH_MAX + 512];
    int n = snprintf(body, sizeof(body), "license=%s\nsha1=%s\naccepted=%s\nsource=%s\nvia=%s\n",
                     id, sha, when, source, how);
    if (n < 0 || (size_t)n >= sizeof(body)) return -1;
    char dir[PATH_MAX];
    char path[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "licenses") != 0 || soulx_path(path, sizeof(path), "licenses/%s.accepted", id) != 0) return -1;
    if (soulx_mkdirs(dir) != 0) return -1;
    return soulx_write_file_atomic(path, body, (size_t)n);
}

static bool env_accepts_license(void) {
    const char* v = getenv("SOULX_ACCEPT_ANDROID_SDK_LICENSE");
    return v && strcmp(v, "1") == 0;
}

static int ensure_licenses(const SoulXPackage* const* pkgs, size_t count, const SoulXSetupOpts* opts) {
    const char* ids[PACKAGE_COUNT];
    size_t nid = 0;
    for (size_t i = 0; i < count; i++) {
        const char* id = pkgs[i]->license_id;
        if (!id || !id[0]) continue;
        bool seen = false;
        for (size_t j = 0; j < nid; j++) {
            if (strcmp(ids[j], id) == 0) seen = true;
        }
        if (!seen && nid < PACKAGE_COUNT) ids[nid++] = id;
    }

    LicenseDocs docs;
    memset(&docs, 0, sizeof(docs));
    int rc = 0;
    for (size_t k = 0; k < nid && rc == 0; k++) {
        const char* id = ids[k];
        if (!license_id_valid(id)) {
            soulx_log(SOULX_LOG_ERROR, "invalid license id '%s'", id);
            rc = -1;
            break;
        }
        // An acceptance covers exactly the text that was shown: compare the recorded SHA-1 with the
        // current terms and ask again when they changed (or could not be shown back then).
        char recorded[2 * CC_SHA1_DIGEST_LENGTH + 16];
        bool accepted_before = license_accepted_sha1(id, recorded, sizeof(recorded));

        char required_by[128] = "";
        size_t off = 0;
        for (size_t i = 0; i < count; i++) {
            if (!streq(pkgs[i]->license_id, id)) continue;
            int n = snprintf(required_by + off, sizeof(required_by) - off, "%s%s", off ? ", " : "", pkgs[i]->key);
            if (n < 0 || (size_t)n >= sizeof(required_by) - off) break;
            off += (size_t)n;
        }

        char saved[PATH_MAX];
        char source[PATH_MAX];
        if (soulx_path(saved, sizeof(saved), "licenses/%s.txt", id) != 0) {
            rc = -1;
            break;
        }
        snprintf(source, sizeof(source), "%s", TERMS_URL);
        char* text = NULL;
        if (license_text(id, &docs, &text, source, sizeof(source)) != 0) {
            if (g_soulx_interrupted) {
                soulx_log(SOULX_LOG_ERROR, "setup interrupted");
                rc = -1;
                break;
            }
            if (accepted_before) {
                soulx_log(SOULX_LOG_DEBUG, "license %s was accepted earlier (its current text is not available to "
                         "compare)", id);
                continue;
            }
            soulx_log(SOULX_LOG_WARN, "could not download the text of license %s", id);
            snprintf(source, sizeof(source), "%s", TERMS_URL);
        }
        if (accepted_before && text) {
            char sha[2 * CC_SHA1_DIGEST_LENGTH + 1];
            sha1_hex_mem(text, strlen(text), sha);
            if (strcasecmp(sha, recorded) == 0) {
                soulx_log(SOULX_LOG_DEBUG, "license %s already accepted", id);
                free(text);
                continue;
            }
            if (strcmp(recorded, "unavailable") == 0) {
                soulx_log(SOULX_LOG_INFO, "License %s was accepted without its text; please review it now.", id);
            } else {
                soulx_log(SOULX_LOG_WARN, "the terms of license %s changed since they were accepted; please review them",
                         id);
            }
        }
        // the launcher asks for the agreement on its own page (with a link to the text): no need to print it
        show_license(id, text, saved, required_by, opts->accept_licenses && !isatty(STDERR_FILENO));

        const char* how = NULL;
        if (opts->accept_licenses) {
            how = "--accept-licenses";
        } else if (env_accepts_license()) {
            how = "SOULX_ACCEPT_ANDROID_SDK_LICENSE=1";
        } else if (isatty(STDIN_FILENO)) {
            if (prompt_yes("Do you accept these license terms?")) {
                how = "prompt";
            } else {
                if (g_soulx_interrupted) soulx_log(SOULX_LOG_ERROR, "setup interrupted");
                else soulx_log(SOULX_LOG_ERROR, "license %s was not accepted; setup cancelled", id);
                rc = -1;
            }
        } else {
            soulx_log(SOULX_LOG_ERROR, "license %s must be accepted: read %s, then run `soulx setup --accept-licenses`",
                     id, text ? saved : TERMS_URL);
            rc = -1;
        }
        if (how) {
            if (record_acceptance(id, text, source, how) != 0) {
                soulx_log(SOULX_LOG_ERROR, "cannot record the acceptance of %s: %s", id, strerror(errno));
                rc = -1;
            } else {
                soulx_log(SOULX_LOG_INFO, "License %s accepted (%s).", id, how);
            }
        }
        free(text);
    }
    for (size_t i = 0; i < LICENSE_DOC_COUNT; i++) free(docs.body[i]);
    return rc;
}

// ---------------------------------------------------------------------------
// Downloads
// ---------------------------------------------------------------------------

static const char* url_basename(const char* url) {
    const char* s = strrchr(url, '/');
    return s ? s + 1 : url;
}

// downloads/ file name: the URL's basename; images get a key prefix (all images share basenames).
static int download_name(const SoulXPackage* p, char* buf, size_t cap) {
    int n = (p->kind == SOULX_PKG_IMAGE)
        ? snprintf(buf, cap, "sysimg-%s-%s", p->key, url_basename(p->url))
        : snprintf(buf, cap, "%s", url_basename(p->url));
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

static bool regular_file_of_size(const char* path, uint64_t size) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size == size;
}

// Files in downloads/ with exactly p's size (canonical name first), e.g. a pre-fetched
// sysimg-google_apis_playstore-arm64-v8a-37.0_r06.zip or a finished .part.
static int cached_candidates(const SoulXPackage* p, char out[][PATH_MAX], int max) {
    int n = 0;
    char name[NAME_MAX + 1];
    char canon[PATH_MAX];
    char dir[PATH_MAX];
    if (download_name(p, name, sizeof(name)) != 0 ||
        soulx_path(canon, sizeof(canon), "downloads/%s", name) != 0 ||
        soulx_path(dir, sizeof(dir), "downloads") != 0) {
        return 0;
    }
    if (max > 0 && regular_file_of_size(canon, p->size)) {
        memcpy(out[n++], canon, strlen(canon) + 1);
    }
    DIR* d = opendir(dir);
    if (!d) return n;
    struct dirent* e;
    while (n < max && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[PATH_MAX];
        int k = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (k < 0 || (size_t)k >= sizeof(path)) continue;
        if (strcmp(path, canon) == 0 || !regular_file_of_size(path, p->size)) continue;
        memcpy(out[n++], path, (size_t)k + 1);
    }
    closedir(d);
    return n;
}

// 0: zip_out names a verified copy; -1: none found; -2: interrupted.
static int find_verified_zip(const SoulXPackage* p, char* zip_out, size_t cap) {
    char cands[MAX_CANDIDATES][PATH_MAX];
    int n = cached_candidates(p, cands, MAX_CANDIDATES);
    for (int i = 0; i < n; i++) {
        const char* base = url_basename(cands[i]);
        char hex[2 * CC_SHA1_DIGEST_LENGTH + 1];
        if (sha1_hex_file(cands[i], base, hex) != 0) {
            if (g_soulx_interrupted) return -2;
            continue;
        }
        if (strcmp(hex, p->sha1) == 0) {
            size_t len = strlen(cands[i]);
            if (len >= cap) return -1;
            memcpy(zip_out, cands[i], len + 1);
            soulx_log(SOULX_LOG_INFO, "Using verified download %s", base);
            return 0;
        }
        soulx_log(SOULX_LOG_DEBUG, "%s has the size of %s but a different SHA-1; ignored", base, p->key);
    }
    return -1;
}

typedef struct {
    const char* part;
    const char* key;
    uint64_t total;
    volatile bool done;
} DownloadWatch;

// Without a terminal curl shows nothing: report the size of the growing .part file instead (the launcher shows it).
static void* watch_download(void* arg) {
    DownloadWatch* w = arg;
    char total_s[32];
    format_bytes(w->total, total_s, sizeof(total_s));
    while (!w->done) {
        struct stat st;
        if (stat(w->part, &st) == 0 && w->total > 0) {
            char done_s[32];
            format_bytes((uint64_t)st.st_size, done_s, sizeof(done_s));
            soulx_status_line("Downloading %s... %d%% (%s / %s)", w->key, (int)((uint64_t)st.st_size * 100 / w->total),
                              done_s, total_s);
        }
        usleep(250 * 1000);
    }
    return NULL;
}

static int run_curl_download(const char* part, const char* url, const char* key, uint64_t total) {
    bool tty = isatty(STDERR_FILENO);
    const char* progress = tty ? "--progress-bar" : "-sS";
    const char* argv[] = {
        CURL_BIN, "-q", "-fL", "--proto", "=https", "--proto-redir", "=https",
        "--retry", "5", "--retry-delay", "2", "-C", "-", progress, "-o", part, url, NULL
    };
    SoulXCmd cmd = { .argv = argv };
    DownloadWatch w = { .part = part, .key = key, .total = total, .done = false };
    pthread_t th;
    bool watching = !tty && pthread_create(&th, NULL, watch_download, &w) == 0;
    int rc = soulx_run_passthrough(&cmd);
    if (watching) {
        w.done = true;
        pthread_join(th, NULL);
    }
    return rc;
}

static int download_package(const SoulXPackage* p, char* zip_out, size_t cap) {
    char name[NAME_MAX + 1];
    char dest[PATH_MAX];
    char part[PATH_MAX];
    if (download_name(p, name, sizeof(name)) != 0 || soulx_path(dest, sizeof(dest), "downloads/%s", name) != 0) {
        soulx_log(SOULX_LOG_ERROR, "download path too long under %s", soulx_home());
        return -1;
    }
    int n = snprintf(part, sizeof(part), "%s.part", dest);
    if (n < 0 || (size_t)n >= sizeof(part)) {
        soulx_log(SOULX_LOG_ERROR, "download path too long under %s", soulx_home());
        return -1;
    }

    struct stat st;
    if (lstat(part, &st) == 0 && (!S_ISREG(st.st_mode) || (uint64_t)st.st_size > p->size)) unlink(part);
    bool complete = lstat(part, &st) == 0 && (uint64_t)st.st_size == p->size;
    if (!complete) {
        char size_s[32];
        format_bytes(p->size, size_s, sizeof(size_s));
        soulx_log(SOULX_LOG_INFO, "Downloading %s %s (%s) from %s", p->key, p->version, size_s, p->url);
        int rc = run_curl_download(part, p->url, p->key, p->size);
        if (rc == 33 && !g_soulx_interrupted) {     // server refused to resume: start over once
            unlink(part);
            rc = run_curl_download(part, p->url, p->key, p->size);
        }
        if (g_soulx_interrupted) {
            soulx_log(SOULX_LOG_ERROR, "download interrupted; run `soulx setup` again to resume");
            return -1;
        }
        if (rc != 0) {
            soulx_log(SOULX_LOG_ERROR, "download of %s failed (curl exit status %d); run `soulx setup` again to resume",
                     p->key, rc);
            return -1;
        }
    }

    if (lstat(part, &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != p->size) {
        soulx_log(SOULX_LOG_ERROR, "%s: downloaded %lld bytes, expected %llu; the file was deleted",
                 p->key, (long long)st.st_size, (unsigned long long)p->size);
        unlink(part);
        return -1;
    }
    char hex[2 * CC_SHA1_DIGEST_LENGTH + 1];
    if (sha1_hex_file(part, p->key, hex) != 0) {
        if (g_soulx_interrupted) soulx_log(SOULX_LOG_ERROR, "verification interrupted; run `soulx setup` again");
        else soulx_log(SOULX_LOG_ERROR, "cannot read %s: %s", part, strerror(errno));
        return -1;
    }
    if (strcmp(hex, p->sha1) != 0) {
        soulx_log(SOULX_LOG_ERROR, "%s: SHA-1 mismatch (expected %s, got %s); the download was deleted", p->key, p->sha1, hex);
        unlink(part);
        return -1;
    }
    if (rename(part, dest) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot rename %s: %s", part, strerror(errno));
        return -1;
    }
    size_t len = strlen(dest);
    if (len >= cap) return -1;
    memcpy(zip_out, dest, len + 1);
    return 0;
}

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------

static bool has_quarantine(const char* path) {
    return getxattr(path, "com.apple.quarantine", NULL, 0, 0, XATTR_NOFOLLOW) >= 0;
}

static bool running_instance_blocks(const SoulXPackage* const* todo, size_t n, char* who, size_t cap);

// Moves an unpacked package (src) into place (dst). Runs under the registry lock, which `soulx start`
// holds while it launches an engine, after checking again that no running instance uses the files:
// one may have been started during the download. The previous copy is renamed aside in one step and
// only deleted once the new copy and its install marker are in place, so an interrupted or failed
// setup never leaves a half-deleted package that still counts as installed.
static int commit_package(const SoulXPackage* p, const char* src, const char* dst, const char* parent) {
    char old[PATH_MAX];
    if (soulx_path(old, sizeof(old), "tmp/old-%s-%d", p->key, (int)getpid()) != 0) {
        soulx_log(SOULX_LOG_ERROR, "install path too long under %s", soulx_home());
        return -1;
    }
    int lock = soulx_lock_acquire();
    if (lock < 0) return -1;

    int rc = -1;
    char who[SOULX_NAME_MAX + 1];
    const SoulXPackage* const one[1] = { p };
    struct stat st;
    bool had_old = lstat(dst, &st) == 0;
    if (running_instance_blocks(one, 1, who, sizeof(who))) {
        soulx_log(SOULX_LOG_ERROR, "instance '%s' was started during setup and uses the files of %s; stop it, then run "
                 "`soulx setup` again: soulx stop %s", who, p->key, who);
        goto out;
    }
    if (soulx_mkdirs(parent) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", parent, strerror(errno));
        goto out;
    }
    if (had_old) {
        (void)soulx_rmtree(old);
        if (rename(dst, old) != 0) {
            soulx_log(SOULX_LOG_ERROR, "cannot move the previous %s aside: %s", dst, strerror(errno));
            goto out;
        }
    }
    if (rename(src, dst) != 0) {
        int err = errno;
        if (had_old && rename(old, dst) != 0) {
            soulx_log(SOULX_LOG_ERROR, "cannot install %s (%s) and cannot restore the previous version (%s); run "
                     "`soulx setup --force`", dst, strerror(err), strerror(errno));
        } else {
            soulx_log(SOULX_LOG_ERROR, "cannot install %s: %s", dst, strerror(err));
        }
        goto out;
    }
    if (soulx_runtime_write_marker(p) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot write the install marker for %s: %s", p->key, strerror(errno));
        goto out;
    }
    rc = 0;

out:
    soulx_lock_release(lock);
    if (rc == 0 && had_old && soulx_rmtree(old) != 0) {
        soulx_log(SOULX_LOG_WARN, "could not delete the previous %s (left in %s)", p->key, old);
    }
    return rc;
}

// Deletes what an interrupted setup left in tmp/: unpacked stages and previous versions moved aside.
// The caller holds the setup lock, so no other setup is using them.
static void sweep_setup_leftovers(void) {
    char dir[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "tmp") != 0) return;
    DIR* d = opendir(dir);
    if (!d) return;
    char found[16][NAME_MAX + 1];
    int n = 0;
    struct dirent* e;
    while (n < 16 && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "stage-", 6) != 0 && strncmp(e->d_name, "old-", 4) != 0) continue;
        if (strlen(e->d_name) >= sizeof(found[0])) continue;
        memcpy(found[n++], e->d_name, strlen(e->d_name) + 1);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        if (soulx_path(path, sizeof(path), "tmp/%s", found[i]) != 0) continue;
        if (soulx_rmtree(path) == 0) soulx_log(SOULX_LOG_DEBUG, "removed %s (left by an interrupted setup)", path);
        else soulx_log(SOULX_LOG_WARN, "could not remove %s (left by an interrupted setup)", path);
    }
}

static int extract_package(const SoulXPackage* p, const char* zip) {
    char stage[PATH_MAX];
    char src[PATH_MAX];
    char dst[PATH_MAX];
    char parent[PATH_MAX];
    if (soulx_path(stage, sizeof(stage), "tmp/stage-%s", p->key) != 0 || install_dir(p, dst, sizeof(dst)) != 0) {
        soulx_log(SOULX_LOG_ERROR, "install path too long under %s", soulx_home());
        return -1;
    }
    int n = snprintf(src, sizeof(src), "%s/%s", stage, p->zip_root);
    int m = snprintf(parent, sizeof(parent), "%s", dst);
    if (n < 0 || (size_t)n >= sizeof(src) || m < 0 || (size_t)m >= sizeof(parent)) {
        soulx_log(SOULX_LOG_ERROR, "install path too long under %s", soulx_home());
        return -1;
    }
    char* slash = strrchr(parent, '/');
    if (slash) *slash = '\0';

    (void)soulx_rmtree(stage);           // leftover of an interrupted run
    if (soulx_mkdirs(stage) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", stage, strerror(errno));
        return -1;
    }

    char size_s[32];
    format_bytes(unpacked_size(p), size_s, sizeof(size_s));
    soulx_log(SOULX_LOG_INFO, "Extracting %s (about %s)...", p->key, size_s);
    const char* argv[] = { UNZIP_BIN, "-q", "-o", zip, "-d", stage, NULL };
    SoulXCmd cmd = { .argv = argv, .timeout_ms = 60 * 60 * 1000 };
    SoulXCmdResult res;
    int rc = soulx_run(&cmd, &res);
    if (rc != 0 && rc != 1) {           // 1 = warnings only (the layout is checked below)
        if (g_soulx_interrupted) soulx_log(SOULX_LOG_ERROR, "extraction interrupted");
        else soulx_log(SOULX_LOG_ERROR, "unzip failed for %s (exit %d): %s", zip, rc, tail_of(res.err, 400));
        soulx_cmd_result_free(&res);
        (void)soulx_rmtree(stage);
        return -1;
    }
    if (rc == 1) soulx_log(SOULX_LOG_WARN, "unzip reported warnings for %s: %s", zip, tail_of(res.err, 400));
    soulx_cmd_result_free(&res);

    if (!soulx_is_dir(src)) {
        soulx_log(SOULX_LOG_ERROR, "unexpected archive layout: %s/ not found in %s", p->zip_root, zip);
        (void)soulx_rmtree(stage);
        return -1;
    }
    int committed = commit_package(p, src, dst, parent);   // also writes the install marker
    (void)soulx_rmtree(stage);
    if (committed != 0) return -1;

    // Binaries are used exactly as Google signed them; only a quarantine flag is cleared.
    char exe[PATH_MAX];
    bool quarantined = has_quarantine(dst);
    if (!quarantined && p->kind == SOULX_PKG_ENGINE && soulx_tool_path("emulator", exe, sizeof(exe)) == 0) {
        quarantined = has_quarantine(exe);
    }
    if (quarantined) {
        const char* xargv[] = { XATTR_BIN, "-dr", "com.apple.quarantine", dst, NULL };
        SoulXCmd xcmd = { .argv = xargv, .timeout_ms = 120000 };
        if (soulx_run(&xcmd, NULL) != 0) soulx_log(SOULX_LOG_WARN, "could not clear com.apple.quarantine on %s", dst);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

static int setup_lock(void) {
    char path[PATH_MAX];
    if (soulx_path(path, sizeof(path), "downloads/.setup.lock") != 0) return -1;
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) soulx_log(SOULX_LOG_ERROR, "another `soulx setup` is already running");
        else soulx_log(SOULX_LOG_ERROR, "cannot lock %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

// Packages are installed one after the other: download the zip (unless a copy is cached), unpack it
// into tmp/, move it into place, delete the zip (unless --keep-downloads). The disk use peaks while a
// package is unpacked: everything installed before it, plus its zip, plus its unpacked files. The old
// copy of a replaced package is only deleted after the new one is in place, so it is not credited.
static int check_disk(const SoulXPackage* const* todo, size_t n, bool keep_downloads) {
    uint64_t need = 0;
    uint64_t installed = 0;             // bytes added by the packages before the current one
    for (size_t i = 0; i < n; i++) {
        char cands[MAX_CANDIDATES][PATH_MAX];
        uint64_t zip = cached_candidates(todo[i], cands, MAX_CANDIDATES) == 0 ? todo[i]->size : 0;
        uint64_t peak = installed + zip + unpacked_size(todo[i]);
        if (peak > need) need = peak;
        installed += unpacked_size(todo[i]) + (keep_downloads ? zip : 0);
    }
    uint64_t avail = soulx_free_disk_bytes(soulx_home());
    if (avail == 0) return 0;           // unknown: do not block
    char need_s[32];
    char avail_s[32];
    format_bytes(need, need_s, sizeof(need_s));
    format_bytes(avail, avail_s, sizeof(avail_s));
    if (avail < need + DISK_MARGIN) {
        soulx_log(SOULX_LOG_ERROR, "not enough free disk space in %s: about %s needed, %s free", soulx_home(), need_s, avail_s);
        return -1;
    }
    if (avail < need + DISK_COMFORT) {
        soulx_log(SOULX_LOG_WARN, "free disk space is low (%s free, about %s needed); each instance also needs "
                 "several GB for its data and quick-boot snapshots", avail_s, need_s);
    }
    return 0;
}

// Name of a running instance whose engine or system image would be replaced (NULL = none).
static bool running_instance_blocks(const SoulXPackage* const* todo, size_t n, char* who, size_t cap) {
    bool engine = false;
    for (size_t i = 0; i < n; i++) {
        if (todo[i]->kind == SOULX_PKG_ENGINE) engine = true;
    }
    SoulXInstanceConf* list = calloc(MAX_REG_LIST, sizeof(*list));
    if (!list) return false;
    int count = soulx_reg_list(list, MAX_REG_LIST);
    bool blocked = false;
    for (int i = 0; i < count && !blocked; i++) {
        bool affected = engine;
        for (size_t k = 0; k < n && !affected; k++) {
            if (todo[k]->kind == SOULX_PKG_IMAGE && strcmp(todo[k]->key, list[i].image) == 0) affected = true;
        }
        if (affected && soulx_instance_running(list[i].name, NULL)) {
            snprintf(who, cap, "%s", list[i].name);
            blocked = true;
        }
    }
    free(list);
    return blocked;
}

int soulx_runtime_setup(const char* image_key, const SoulXSetupOpts* opts) {
    SoulXSetupOpts defaults = { false, false, false };
    if (!opts) opts = &defaults;

    const SoulXPackage* image = (image_key && image_key[0]) ? soulx_runtime_find(image_key) : soulx_runtime_default_image();
    if (!image || image->kind != SOULX_PKG_IMAGE) {
        char keys[64];
        image_keys(keys, sizeof(keys));
        soulx_log(SOULX_LOG_ERROR, "unknown system image '%s' (available: %s)", image_key ? image_key : "", keys);
        return -1;
    }
    if (!host_is_arm64()) {
        soulx_log(SOULX_LOG_ERROR, "SoulX requires a Mac with Apple Silicon (arm64)");
        return -1;
    }
    if (soulx_ensure_layout() != 0) return -1;
    int lock = setup_lock();
    if (lock < 0) return -1;
    sweep_setup_leftovers();

    const SoulXPackage* wanted[3] = { find_kind(SOULX_PKG_ENGINE), find_kind(SOULX_PKG_PLATFORM_TOOLS), image };
    const SoulXPackage* todo[3];
    size_t ntodo = 0;
    for (size_t i = 0; i < 3; i++) {
        if (opts->force || !soulx_runtime_installed(wanted[i])) todo[ntodo++] = wanted[i];
    }

    int rc = 0;
    if (ntodo > 0) {
        char who[SOULX_NAME_MAX + 1];
        if (running_instance_blocks(todo, ntodo, who, sizeof(who))) {
            soulx_log(SOULX_LOG_ERROR, "instance '%s' is running on the files to be replaced; stop it first: soulx stop %s", who, who);
            rc = -1;
            goto done;
        }
        if (check_disk(todo, ntodo, opts->keep_downloads) != 0 || ensure_licenses(todo, ntodo, opts) != 0) {
            rc = -1;
            goto done;
        }
        for (size_t i = 0; i < ntodo && rc == 0; i++) {
            const SoulXPackage* p = todo[i];
            if (g_soulx_interrupted) {
                rc = -1;
                break;
            }
            soulx_log(SOULX_LOG_INFO, "[%zu/%zu] %s", i + 1, ntodo, p->description);
            char zip[PATH_MAX];
            int found = find_verified_zip(p, zip, sizeof(zip));
            if (found == -2) {
                soulx_log(SOULX_LOG_ERROR, "verification interrupted");
                rc = -1;
                break;
            }
            if (found != 0 && download_package(p, zip, sizeof(zip)) != 0) {
                rc = -1;
                break;
            }
            if (extract_package(p, zip) != 0) {
                rc = -1;
                break;
            }
            if (!soulx_runtime_installed(p)) {
                soulx_log(SOULX_LOG_ERROR, "%s did not install correctly (missing files); run `soulx setup --force`", p->key);
                rc = -1;
                break;
            }
            if (!opts->keep_downloads && unlink(zip) != 0) {
                soulx_log(SOULX_LOG_WARN, "cannot delete %s: %s", zip, strerror(errno));
            }
            soulx_log(SOULX_LOG_INFO, "Installed %s %s", p->key, p->version);
        }
        if (rc != 0) goto done;
    } else {
        soulx_log(SOULX_LOG_INFO, "Runtime already installed (use --force to reinstall)");
    }

    char platforms[PATH_MAX];
    if (soulx_path(platforms, sizeof(platforms), "sdk/platforms") == 0) (void)soulx_mkdirs(platforms);
    if (soulx_ensure_adb_key() != 0) soulx_log(SOULX_LOG_WARN, "adb key not created yet; adb creates one on first use");

    {
        char label[160];
        image_label(image, label, sizeof(label));
        printf("Runtime ready: %s · engine %s · adb %s\n", label,
               find_kind(SOULX_PKG_ENGINE)->version, find_kind(SOULX_PKG_PLATFORM_TOOLS)->version);
        fflush(stdout);
    }

done:
    close(lock);                        // releases the flock
    return rc;
}

// ---------------------------------------------------------------------------
// adb key
// ---------------------------------------------------------------------------

int soulx_ensure_adb_key(void) {
    char key[PATH_MAX];
    char dir[PATH_MAX];
    char adb[PATH_MAX];
    if (soulx_path(key, sizeof(key), "home/.android/adbkey") != 0 ||
        soulx_path(dir, sizeof(dir), "home/.android") != 0 ||
        soulx_tool_path("adb", adb, sizeof(adb)) != 0) {
        return -1;
    }
    if (soulx_path_exists(key)) return 0;
    if (soulx_mkdirs(dir) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", dir, strerror(errno));
        return -1;
    }
    if (!is_executable_file(adb)) {
        soulx_log(SOULX_LOG_ERROR, "adb is not installed; run: soulx setup");
        return -1;
    }
    const char* argv[] = { adb, "keygen", key, NULL };
    SoulXCmd cmd = { .argv = argv, .soulx_env = true, .timeout_ms = 30000 };
    SoulXCmdResult res;
    int rc = soulx_run(&cmd, &res);
    if (rc != 0 || !soulx_path_exists(key)) {
        soulx_log(SOULX_LOG_ERROR, "adb keygen failed (exit %d): %s", rc, tail_of(res.err, 300));
        soulx_cmd_result_free(&res);
        return -1;
    }
    soulx_cmd_result_free(&res);
    soulx_log(SOULX_LOG_DEBUG, "created %s", key);
    return 0;
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

int soulx_runtime_print_status(void) {
    const SoulXPackage* def = soulx_runtime_default_image();
    printf("SoulX runtime: %s\n", soulx_home());
    printf("  %-15s %-9s %-14s %10s  %s\n", "PACKAGE", "VERSION", "STATUS", "SIZE", "DESCRIPTION");
    for (size_t i = 0; i < PACKAGE_COUNT; i++) {
        const SoulXPackage* p = &k_packages[i];
        PkgState s = package_state(p);
        char dir[PATH_MAX];
        char size_s[32] = "-";
        if (install_dir(p, dir, sizeof(dir)) == 0 && soulx_is_dir(dir)) format_bytes(soulx_dir_size(dir), size_s, sizeof(size_s));
        char desc[200];
        if (p->kind == SOULX_PKG_IMAGE) {
            char label[160];
            image_label(p, label, sizeof(label));
            snprintf(desc, sizeof(desc), "%s%s", label, p == def ? " [default]" : "");
        } else {
            snprintf(desc, sizeof(desc), "%s", p->description);
        }
        printf("  %-15s %-9s %-14s %10s  %s\n", p->key, p->version, state_name(s), size_s, desc);
    }
    char dl[PATH_MAX];
    if (soulx_path(dl, sizeof(dl), "downloads") == 0) {
        char size_s[32];
        format_bytes(soulx_is_dir(dl) ? soulx_dir_size(dl) : 0, size_s, sizeof(size_s));
        printf("  Download cache: %s (%s)\n", size_s, dl);
    }
    if (!soulx_runtime_ready(NULL)) printf("Run `soulx setup` to install the runtime.\n");
    fflush(stdout);
    return 0;
}

// ---------------------------------------------------------------------------
// Doctor
// ---------------------------------------------------------------------------

typedef struct {
    int blockers;
    int warnings;
} DoctorTally;

// kind: 'o' ok, '!' warning, 'x' blocker.
static void check_line(DoctorTally* t, char kind, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

static void check_line(DoctorTally* t, char kind, const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) msg[0] = '\0';           // (a truncated message is still printed)
    const char* mark = "✓";        // check mark
    if (kind == 'x') {
        mark = "✗";                // ballot x
        t->blockers++;
    } else if (kind == '!') {
        mark = "!";
        t->warnings++;
    }
    printf("  %s %s\n", mark, msg);
}

static bool tcp_port_listening(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = false;
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        struct pollfd p = { fd, POLLOUT, 0 };
        if (poll(&p, 1, 300) == 1) {
            int err = 0;
            socklen_t len = sizeof(err);
            ok = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
        }
    }
    close(fd);
    return ok;
}

// A process of this user that listens on TCP <port> (any local address); 0 when none is visible
// (no listener, or one that belongs to another user).
static pid_t tcp_listener_pid(int port) {
    int count = proc_listallpids(NULL, 0);
    if (count <= 0) return 0;
    size_t cap = (size_t)count + 64;
    pid_t* pids = calloc(cap, sizeof(*pids));
    if (!pids) return 0;
    int n = proc_listallpids(pids, (int)(cap * sizeof(*pids)));
    pid_t found = 0;
    for (int i = 0; i < n && (size_t)i < cap && !found; i++) {
        if (pids[i] <= 0) continue;
        int bytes = proc_pidinfo(pids[i], PROC_PIDLISTFDS, 0, NULL, 0);
        if (bytes <= 0) continue;
        bytes += 16 * (int)sizeof(struct proc_fdinfo);     // descriptors opened meanwhile
        struct proc_fdinfo* fds = malloc((size_t)bytes);
        if (!fds) continue;
        bytes = proc_pidinfo(pids[i], PROC_PIDLISTFDS, 0, fds, bytes);
        int nfd = bytes > 0 ? bytes / (int)sizeof(struct proc_fdinfo) : 0;
        for (int k = 0; k < nfd && !found; k++) {
            if (fds[k].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
            struct socket_fdinfo si;
            if (proc_pidfdinfo(pids[i], fds[k].proc_fd, PROC_PIDFDSOCKETINFO, &si, (int)sizeof(si)) != (int)sizeof(si) ||
                si.psi.soi_kind != SOCKINFO_TCP) {
                continue;
            }
            const struct tcp_sockinfo* t = &si.psi.soi_proto.pri_tcp;
            if (t->tcpsi_state == TSI_S_LISTEN && ntohs((uint16_t)t->tcpsi_ini.insi_lport) == port) found = pids[i];
        }
        free(fds);
    }
    free(pids);
    return found;
}

// Doctor line for port 5137: SoulX's own adb server, nothing, or something else (another SOULX_HOME's
// adb, any other program), which would answer SoulX's adb commands in its place.
static void check_adb_server(DoctorTally* t) {
    pid_t pid = tcp_listener_pid(SOULX_ADB_SERVER_PORT);
    if (pid <= 0) {
        if (tcp_port_listening(SOULX_ADB_SERVER_PORT)) {
            check_line(t, '!', "something is listening on 127.0.0.1:%d (SoulX's adb server port) and it could not be "
                       "identified as SoulX's adb", SOULX_ADB_SERVER_PORT);
        } else {
            check_line(t, 'o', "SoulX adb server not running (starts on demand on port %d)", SOULX_ADB_SERVER_PORT);
        }
        return;
    }
    char exe[PROC_PIDPATHINFO_MAXSIZE];
    char adb[PATH_MAX];
    char adb_real[PATH_MAX];
    bool have_exe = proc_pidpath(pid, exe, sizeof(exe)) > 0;
    if (have_exe && soulx_tool_path("adb", adb, sizeof(adb)) == 0 && realpath(adb, adb_real) &&
        strcmp(exe, adb_real) == 0) {
        check_line(t, 'o', "SoulX adb server listening on 127.0.0.1:%d (pid %d)", SOULX_ADB_SERVER_PORT, (int)pid);
    } else {
        check_line(t, '!', "port %d is used by %s (pid %d), not by SoulX's adb; stop it so SoulX's adb server can "
                   "start", SOULX_ADB_SERVER_PORT, have_exe ? exe : "another program", (int)pid);
    }
}

int soulx_doctor(void) {
    DoctorTally t = { 0, 0 };
    printf("SoulX doctor (SOULX_HOME=%s)\n", soulx_home());

    // Host
    struct utsname u;
    const char* machine = uname(&u) == 0 ? u.machine : "unknown";
    int64_t v = 0;
    bool arm_hw = host_is_arm64();
    if (strcmp(machine, "arm64") == 0) {
        check_line(&t, 'o', "Apple Silicon (arm64)");
    } else if (arm_hw) {
        check_line(&t, '!', "soulx is running under Rosetta (%s); use the native arm64 build", machine);
    } else {
        check_line(&t, 'x', "This Mac is %s; SoulX needs Apple Silicon (arm64)", machine);
    }

    char osver[64] = "";
    size_t oslen = sizeof(osver);
    if (sysctlbyname("kern.osproductversion", osver, &oslen, NULL, 0) == 0 && osver[0]) {
        int major = atoi(osver);
        if (major > 0 && major < 12) check_line(&t, '!', "macOS %s is older than the supported minimum (macOS 12)", osver);
        else check_line(&t, 'o', "macOS %s", osver);
    } else {
        check_line(&t, '!', "macOS version unknown");
    }

    if (sysctl_i64("kern.hv_support", &v) == 0 && v == 1) {
        check_line(&t, 'o', "Hypervisor.framework available");
    } else {
        check_line(&t, 'x', "Hypervisor.framework not available (virtualization unsupported here)");
    }

    if (sysctl_i64("hw.memsize", &v) == 0 && v > 0) {
        double gb = (double)v / (1024.0 * 1024.0 * 1024.0);
        if (gb < 8.5) check_line(&t, '!', "Memory: %.0f GB; prefer the lite profile (soulx create --profile lite)", gb);
        else check_line(&t, 'o', "Memory: %.0f GB", gb);
    }

    uint64_t avail = soulx_free_disk_bytes(soulx_home());
    char avail_s[32];
    format_bytes(avail, avail_s, sizeof(avail_s));
    if (avail > 0 && avail < 10ULL * GB) check_line(&t, '!', "Free disk: %s in %s (low)", avail_s, soulx_home());
    else if (avail > 0) check_line(&t, 'o', "Free disk: %s in %s", avail_s, soulx_home());

    // Runtime
    const SoulXPackage* eng = find_kind(SOULX_PKG_ENGINE);
    const SoulXPackage* pt = find_kind(SOULX_PKG_PLATFORM_TOOLS);
    bool eng_ok = soulx_runtime_installed(eng);
    bool pt_ok = soulx_runtime_installed(pt);
    int images = 0;
    for (size_t i = 0; i < PACKAGE_COUNT; i++) {
        if (k_packages[i].kind == SOULX_PKG_IMAGE && soulx_runtime_installed(&k_packages[i])) images++;
    }
    if (!eng_ok && !pt_ok && images == 0) {
        check_line(&t, 'x', "SoulX runtime not installed — run: soulx setup");
    } else {
        if (eng_ok) check_line(&t, 'o', "Engine %s installed", eng->version);
        else check_line(&t, 'x', "Engine %s not installed (%s) — run: soulx setup", eng->version, state_name(package_state(eng)));
        if (pt_ok) check_line(&t, 'o', "Platform-tools %s installed (adb)", pt->version);
        else check_line(&t, 'x', "Platform-tools %s not installed (%s) — run: soulx setup", pt->version, state_name(package_state(pt)));
        for (size_t i = 0; i < PACKAGE_COUNT; i++) {
            const SoulXPackage* p = &k_packages[i];
            if (p->kind != SOULX_PKG_IMAGE) continue;
            char label[160];
            image_label(p, label, sizeof(label));
            PkgState s = package_state(p);
            if (s == PKG_OK) check_line(&t, 'o', "System image %s: %s", p->key, label);
            else if (s != PKG_MISSING) check_line(&t, '!', "System image %s is %s — run: soulx setup --image %s", p->key, state_name(s), p->key);
        }
        if (images == 0) check_line(&t, 'x', "No system image installed — run: soulx setup");
    }

    if (eng_ok) {
        char qemu[PATH_MAX];
        if (soulx_path(qemu, sizeof(qemu), "%s", QEMU_REL) == 0) {
            const char* argv[] = { CODESIGN_BIN, "-v", qemu, NULL };
            SoulXCmd cmd = { .argv = argv, .timeout_ms = 60000 };
            if (soulx_run(&cmd, NULL) == 0) check_line(&t, 'o', "Engine code signature valid");
            else check_line(&t, 'x', "Engine code signature invalid or engine binary missing — run: soulx setup --force");
        }
    }

    char key[PATH_MAX];
    if (pt_ok && soulx_path(key, sizeof(key), "home/.android/adbkey") == 0) {
        if (soulx_path_exists(key)) check_line(&t, 'o', "adb key present");
        else check_line(&t, '!', "adb key missing — run: soulx setup");
    }

    char avd[PATH_MAX];
    if (soulx_path(avd, sizeof(avd), "avd") == 0) {
        if (soulx_is_dir(avd)) check_line(&t, 'o', "Device directory %s", avd);
        else check_line(&t, '!', "%s is missing: the engine would fall back to ~/.android/avd — run: soulx setup", avd);
    }

    check_adb_server(&t);

    // Instances
    SoulXInstanceConf* list = calloc(MAX_REG_LIST, sizeof(*list));
    if (list) {
        int count = soulx_reg_list(list, MAX_REG_LIST);
        if (count <= 0) check_line(&t, 'o', "No instances yet (create one: soulx create)");
        for (int i = 0; i < count; i++) {
            SoulXRunState st;
            memset(&st, 0, sizeof(st));
            const SoulXPackage* img = soulx_runtime_find(list[i].image);
            if (soulx_instance_running(list[i].name, &st)) {
                check_line(&t, 'o', "Instance #%d %s: running (emulator-%d, pid %d)", list[i].id, list[i].name, st.port, (int)st.pid);
            } else if (!img || img->kind != SOULX_PKG_IMAGE) {
                check_line(&t, '!', "Instance #%d %s: unknown image '%s'", list[i].id, list[i].name, list[i].image);
            } else if (!soulx_runtime_installed(img)) {
                check_line(&t, '!', "Instance #%d %s: stopped; its image is not installed — run: soulx setup --image %s",
                           list[i].id, list[i].name, img->key);
            } else {
                check_line(&t, 'o', "Instance #%d %s: stopped", list[i].id, list[i].name);
            }
        }
        free(list);
    }

    if (t.blockers > 0) printf("%d problem(s) must be fixed before SoulX can run.\n", t.blockers);
    else if (t.warnings > 0) printf("Ready, with %d warning(s).\n", t.warnings);
    else printf("All checks passed.\n");
    fflush(stdout);
    return t.blockers;
}
