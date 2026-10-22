/*
 * SoulX Android Emulator - Magisk root support
 *
 * Downloads Magisk v31.0, extracts arm64 tools, and patches an instance's ramdisk
 * so it boots with Magisk root.  Tools are cached under $SOULX_HOME/sdk/magisk/
 * and verified before each use.
 */

#include "soulx_mac.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CURL_BIN       "/usr/bin/curl"
#define UNZIP_BIN      "/usr/bin/unzip"

#define MAGISK_APK_URL "https://github.com/topjohnwu/Magisk/releases/download/v" SOULX_MAGISK_VERSION "/Magisk-v" SOULX_MAGISK_VERSION ".apk"
#define MAGISK_APK     "Magisk-v" SOULX_MAGISK_VERSION ".apk"

#define DEVICE_WORK    "/data/local/tmp/magisk-soulx"
#define ADB_TIMEOUT    120000
#define BOOT_TIMEOUT   300

static const char* k_arm64_libs[] = {
    "lib/arm64-v8a/libbusybox.so",
    "lib/arm64-v8a/libinit-ld.so",
    "lib/arm64-v8a/libmagisk.so",
    "lib/arm64-v8a/libmagiskboot.so",
    "lib/arm64-v8a/libmagiskinit.so",
    "lib/arm64-v8a/libmagiskpolicy.so",
};
#define N_LIBS  (sizeof(k_arm64_libs) / sizeof(k_arm64_libs[0]))

static const char* k_assets[] = {
    "assets/boot_patch.sh",
    "assets/main.jar",
    "assets/stub.apk",
    "assets/util_functions.sh",
};
#define N_ASSETS  (sizeof(k_assets) / sizeof(k_assets[0]))

// On-device binary names: the "lib" prefix and ".so" suffix are stripped by the patch script,
// but we push them with the names magiskboot uses.
static const struct { const char* zip_entry; const char* device_name; } k_device_tools[] = {
    { "lib/arm64-v8a/libbusybox.so",      "busybox"      },
    { "lib/arm64-v8a/libinit-ld.so",      "init-ld"      },
    { "lib/arm64-v8a/libmagisk.so",        "magisk"       },
    { "lib/arm64-v8a/libmagiskboot.so",    "magiskboot"   },
    { "lib/arm64-v8a/libmagiskinit.so",    "magiskinit"   },
    { "lib/arm64-v8a/libmagiskpolicy.so",  "magiskpolicy" },
    { "assets/stub.apk",                   "stub.apk"     },
    { "assets/main.jar",                   "main.jar"     },
};
#define N_DEVICE_TOOLS  (sizeof(k_device_tools) / sizeof(k_device_tools[0]))

// Binary patch: replace the su policy SQL queries in libmagisk.so so that
// su always returns ALLOW (policy=2), unconditionally for any uid.
// This is intentional for headless emulator operation: there is no Magisk
// Manager UI to approve su requests, so the binary must auto-allow.
//
// Two queries are patched:
//   1. 84-byte single-column (su/db.rs:123 — uid_granted_root, zygisk path,
//      reads by column index 0): "SELECT policy FROM policies WHERE uid=?..."
//   2. 107-byte three-column (su/db.rs:56 — get_root_settings, the su daemon,
//      reads by column NAME via `match column.as_str()`): the one that actually
//      controls `su` authorization.  AS aliases are REQUIRED here.
//
// logging=0 and notification=0 skip the futile get_manager() call that would
// otherwise fail silently on every su (no Magisk Manager installed).
// The '?' parameter binding is preserved via the tautology ?*0=0.

static const char k_su_patch_orig1[] =
    "SELECT policy FROM policies WHERE uid=? "
    "AND (until=0 OR until>strftime('%s', 'now'))";
static const char k_su_patch_repl1[] =
    "SELECT 2 AS policy WHERE ?*0=0";

static const char k_su_patch_orig2[] =
    "SELECT policy, logging, notification FROM policies WHERE uid=? "
    "AND (until=0 OR until>strftime('%s', 'now'))";
static const char k_su_patch_repl2[] =
    "SELECT 2 AS policy, 0 AS logging, 0 AS notification WHERE ?*0=0";

// Pre-built magisk.db: hex dump of a minimal SQLite database that grants shell root.
// policies: uid=2000 (shell), policy=ALLOW(2), until=0, logging=1, notification=1
// settings: bootloop=0
// Generated offline; contents verified by PoC (2026-10-22).
// 36864 bytes, but the actual meaningful content is the first page (4096 bytes);
// the rest is zero-padded by SQLite.  We embed only the hex of the non-zero prefix.
// For production we write the file from the embedded binary blob below.

#include "magisk_db.inc"   // defines: magisk_db_init(), k_magisk_db_len, MAGISK_DB_SIZE

// Apply the su-auto-allow binary patch to a copy of libmagisk.so.
// Searches for each original SQL string and overwrites it in-place,
// padding the replacement with spaces to match the original length.
// Returns 0 on success, -1 if either pattern is not found.
static int patch_magisk_su_policy(const char* src, const char* dst) {
    FILE* f = fopen(src, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);

    unsigned char* buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
    fclose(f);

    struct { const char* orig; size_t orig_len; const char* repl; size_t repl_len; } patches[] = {
        { k_su_patch_orig1, strlen(k_su_patch_orig1), k_su_patch_repl1, strlen(k_su_patch_repl1) },
        { k_su_patch_orig2, strlen(k_su_patch_orig2), k_su_patch_repl2, strlen(k_su_patch_repl2) },
    };

    for (int p = 0; p < 2; p++) {
        const unsigned char* needle = (const unsigned char*)patches[p].orig;
        size_t nlen = patches[p].orig_len;
        unsigned char* found = NULL;
        for (long i = 0; i <= sz - (long)nlen; i++) {
            if (memcmp(buf + i, needle, nlen) == 0) { found = buf + i; break; }
        }
        if (!found) {
            soulx_log(SOULX_LOG_ERROR, "su policy patch %d: pattern not found in magisk binary", p + 1);
            free(buf);
            return -1;
        }
        memcpy(found, patches[p].repl, patches[p].repl_len);
        memset(found + patches[p].repl_len, ' ', nlen - patches[p].repl_len);
    }

    f = fopen(dst, "wb");
    if (!f) { free(buf); return -1; }
    size_t written = fwrite(buf, 1, (size_t)sz, f);
    fclose(f);
    free(buf);
    return (written == (size_t)sz) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------

static int magisk_dir(char* buf, size_t cap) {
    return soulx_path(buf, cap, "%s", SOULX_MAGISK_DIR);
}

static int magisk_apk_path(char* buf, size_t cap) {
    return soulx_path(buf, cap, "%s/%s", SOULX_MAGISK_DIR, MAGISK_APK);
}

static int magisk_tool_path(char* buf, size_t cap, const char* name) {
    return soulx_path(buf, cap, "%s/%s", SOULX_MAGISK_DIR, name);
}

// ---------------------------------------------------------------------------
// integrity check
// ---------------------------------------------------------------------------

bool soulx_magisk_tools_ok(void) {
    char path[PATH_MAX];

    // APK must exist
    if (magisk_apk_path(path, sizeof path) != 0 || access(path, R_OK) != 0)
        return false;

    // Every extracted tool must exist and be non-empty
    for (size_t i = 0; i < N_LIBS; i++) {
        const char* base = strrchr(k_arm64_libs[i], '/');
        if (!base) return false;
        base++;
        if (magisk_tool_path(path, sizeof path, base) != 0) return false;
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size == 0) return false;
    }
    for (size_t i = 0; i < N_ASSETS; i++) {
        const char* base = strrchr(k_assets[i], '/');
        if (!base) return false;
        base++;
        if (magisk_tool_path(path, sizeof path, base) != 0) return false;
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size == 0) return false;
    }

    // magisk_prebuilt.db
    if (magisk_tool_path(path, sizeof path, "magisk_prebuilt.db") != 0) return false;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size == 0) return false;

    // Patched magisk binary (auto-allow su)
    if (magisk_tool_path(path, sizeof path, "libmagisk_patched.so") != 0) return false;
    if (stat(path, &st) != 0 || st.st_size == 0) return false;

    return true;
}

// ---------------------------------------------------------------------------
// download + extract
// ---------------------------------------------------------------------------

static int download_apk(const char* dest) {
    char part[PATH_MAX];
    int n = snprintf(part, sizeof part, "%s.part", dest);
    if (n < 0 || (size_t)n >= sizeof part) return -1;

    // Resume-capable download
    soulx_log(SOULX_LOG_INFO, "Downloading Magisk v%s...", SOULX_MAGISK_VERSION);
    const char* argv[] = {
        CURL_BIN, "-q", "-fL", "--proto", "=https", "--proto-redir", "=https",
        "--retry", "5", "--retry-delay", "2", "-C", "-",
        isatty(STDERR_FILENO) ? "--progress-bar" : "-sS",
        "-o", part, MAGISK_APK_URL, NULL
    };
    SoulXCmd cmd = { .argv = argv, .interruptible = true };
    int rc = soulx_run_passthrough(&cmd);
    if (rc != 0) {
        if (g_soulx_interrupted)
            soulx_log(SOULX_LOG_ERROR, "download interrupted; run 'soulx root' again to resume");
        else
            soulx_log(SOULX_LOG_ERROR, "failed to download Magisk (curl exit %d)", rc);
        return -1;
    }
    if (rename(part, dest) != 0) {
        soulx_log(SOULX_LOG_ERROR, "rename %s -> %s: %s", part, dest, strerror(errno));
        return -1;
    }
    return 0;
}

static int extract_tools(const char* apk, const char* dir) {
    soulx_log(SOULX_LOG_INFO, "Extracting Magisk tools...");

    // Build unzip args: extract arm64 libs + assets to the magisk dir
    // unzip -jo APK entry1 entry2 ... -d DIR
    const char* argv[4 + N_LIBS + N_ASSETS + 3];
    int k = 0;
    argv[k++] = UNZIP_BIN;
    argv[k++] = "-jo";     // junk paths, overwrite
    argv[k++] = apk;
    for (size_t i = 0; i < N_LIBS; i++) argv[k++] = k_arm64_libs[i];
    for (size_t i = 0; i < N_ASSETS; i++) argv[k++] = k_assets[i];
    argv[k++] = "-d";
    argv[k++] = dir;
    argv[k] = NULL;

    SoulXCmdResult res;
    memset(&res, 0, sizeof res);
    SoulXCmd cmd = { .argv = argv, .timeout_ms = 60000 };
    int rc = soulx_run(&cmd, &res);
    soulx_cmd_result_free(&res);
    if (rc != 0) {
        soulx_log(SOULX_LOG_ERROR, "failed to extract Magisk tools from %s", apk);
        return -1;
    }
    return 0;
}

static int write_static_files(const char* dir) {
    // magisk_prebuilt.db
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/magisk_prebuilt.db", dir);
    if (n < 0 || (size_t)n >= sizeof path) return -1;
    unsigned char db_buf[MAGISK_DB_SIZE];
    magisk_db_init(db_buf);
    if (soulx_write_file_atomic(path, (const char*)db_buf, k_magisk_db_len) != 0) {
        soulx_log(SOULX_LOG_ERROR, "failed to write %s", path);
        return -1;
    }

    // Patch libmagisk.so: auto-allow su for headless operation.
    // overlay.d rc injection is broken on GKI/Android 17, so the prebuilt db
    // cannot be seeded via init.rc.  Instead we patch the magisk binary to
    // always return ALLOW for su policy queries.
    char src[PATH_MAX], dst[PATH_MAX];
    n = snprintf(src, sizeof src, "%s/libmagisk.so", dir);
    if (n < 0 || (size_t)n >= sizeof src) return -1;
    n = snprintf(dst, sizeof dst, "%s/libmagisk_patched.so", dir);
    if (n < 0 || (size_t)n >= sizeof dst) return -1;
    if (patch_magisk_su_policy(src, dst) != 0) {
        soulx_log(SOULX_LOG_ERROR, "failed to patch magisk su policy");
        return -1;
    }
    return 0;
}

int soulx_magisk_ensure_tools(void) {
    if (soulx_magisk_tools_ok()) return 0;

    char dir[PATH_MAX];
    if (magisk_dir(dir, sizeof dir) != 0) return -1;
    if (soulx_mkdirs(dir) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", dir, strerror(errno));
        return -1;
    }

    char apk[PATH_MAX];
    if (magisk_apk_path(apk, sizeof apk) != 0) return -1;

    if (access(apk, R_OK) != 0) {
        if (download_apk(apk) != 0) return -1;
    }

    if (extract_tools(apk, dir) != 0) return -1;
    if (write_static_files(dir) != 0) return -1;

    if (!soulx_magisk_tools_ok()) {
        soulx_log(SOULX_LOG_ERROR, "Magisk tools extracted but integrity check failed");
        return -1;
    }

    soulx_log(SOULX_LOG_INFO, "Magisk v%s tools ready", SOULX_MAGISK_VERSION);
    return 0;
}

// ---------------------------------------------------------------------------
// adb helpers
// ---------------------------------------------------------------------------

static int adb_push(const char* serial, const char* local, const char* remote) {
    const char* args[] = { "push", local, remote, NULL };
    SoulXCmdResult res;
    memset(&res, 0, sizeof res);
    int rc = soulx_adb(serial, args, &res, ADB_TIMEOUT);
    soulx_cmd_result_free(&res);
    return rc;
}

static int adb_pull(const char* serial, const char* remote, const char* local) {
    const char* args[] = { "pull", remote, local, NULL };
    SoulXCmdResult res;
    memset(&res, 0, sizeof res);
    int rc = soulx_adb(serial, args, &res, ADB_TIMEOUT);
    soulx_cmd_result_free(&res);
    return rc;
}

static int adb_shell_check(const char* serial, const char* cmd) {
    char* out = NULL;
    int rc = soulx_adb_shell(serial, cmd, &out, ADB_TIMEOUT);
    free(out);
    return rc;
}

// ---------------------------------------------------------------------------
// ramdisk patching (on-device)
// ---------------------------------------------------------------------------

static int find_stock_ramdisk(const SoulXInstanceConf* c, char* buf, size_t cap) {
    const SoulXPackage* img = soulx_runtime_find(c->image);
    if (!img) {
        soulx_log(SOULX_LOG_ERROR, "unknown system image '%s' for instance '%s'", c->image, c->name);
        return -1;
    }
    char img_dir[PATH_MAX];
    if (soulx_path(img_dir, sizeof img_dir, "sdk/%s", img->install_rel) != 0) return -1;
    int n = snprintf(buf, cap, "%s/ramdisk.img", img_dir);
    if (n < 0 || (size_t)n >= cap) return -1;
    if (access(buf, R_OK) != 0) {
        soulx_log(SOULX_LOG_ERROR, "stock ramdisk not found at %s (is the runtime installed?)", buf);
        return -1;
    }
    return 0;
}

static int push_tools_to_device(const char* serial) {
    char dir[PATH_MAX];
    if (magisk_dir(dir, sizeof dir) != 0) return -1;

    // Create the working directory on device
    if (adb_shell_check(serial, "rm -rf " DEVICE_WORK " && mkdir -p " DEVICE_WORK) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s on the device", DEVICE_WORK);
        return -1;
    }

    soulx_log(SOULX_LOG_INFO, "Pushing Magisk tools to the device...");
    for (size_t i = 0; i < N_DEVICE_TOOLS; i++) {
        char local[PATH_MAX];
        const char* base = strrchr(k_device_tools[i].zip_entry, '/');
        if (!base) return -1;
        base++;

        // Use the patched binary for the magisk daemon
        const char* src_name = base;
        if (strcmp(base, "libmagisk.so") == 0) src_name = "libmagisk_patched.so";

        int n = snprintf(local, sizeof local, "%s/%s", dir, src_name);
        if (n < 0 || (size_t)n >= sizeof local) return -1;

        char remote[PATH_MAX];
        n = snprintf(remote, sizeof remote, "%s/%s", DEVICE_WORK, k_device_tools[i].device_name);
        if (n < 0 || (size_t)n >= sizeof remote) return -1;

        if (adb_push(serial, local, remote) != 0) {
            soulx_log(SOULX_LOG_ERROR, "failed to push %s", k_device_tools[i].device_name);
            return -1;
        }
    }

    // Push overlay.d support files
    char path[PATH_MAX];
    if (magisk_tool_path(path, sizeof path, "magisk_prebuilt.db") != 0) return -1;
    char remote[256];
    snprintf(remote, sizeof remote, "%s/magisk_prebuilt.db", DEVICE_WORK);
    if (adb_push(serial, path, remote) != 0) return -1;

    // Make tools executable
    if (adb_shell_check(serial, "chmod 755 " DEVICE_WORK "/magiskboot " DEVICE_WORK "/magisk "
                                 DEVICE_WORK "/magiskinit " DEVICE_WORK "/magiskpolicy "
                                 DEVICE_WORK "/busybox") != 0) {
        soulx_log(SOULX_LOG_ERROR, "chmod on device tools failed");
        return -1;
    }
    return 0;
}

static int patch_ramdisk_on_device(const char* serial, const char* stock_ramdisk) {
    // Push the stock ramdisk
    char remote_ramdisk[256];
    snprintf(remote_ramdisk, sizeof remote_ramdisk, "%s/ramdisk.img", DEVICE_WORK);
    soulx_log(SOULX_LOG_INFO, "Pushing stock ramdisk to the device...");
    if (adb_push(serial, stock_ramdisk, remote_ramdisk) != 0) {
        soulx_log(SOULX_LOG_ERROR, "failed to push stock ramdisk");
        return -1;
    }

    // Step 1: decompress the stock ramdisk and save a copy for the backup step
    soulx_log(SOULX_LOG_INFO, "Patching ramdisk with Magisk v%s...", SOULX_MAGISK_VERSION);
    char cmd[2048];

    snprintf(cmd, sizeof cmd,
        "cd %s && "
        "./magiskboot decompress ramdisk.img ramdisk.cpio 2>&1 && "
        "cp ramdisk.cpio ramdisk.cpio.orig",
        DEVICE_WORK);
    if (adb_shell_check(serial, cmd) != 0) {
        soulx_log(SOULX_LOG_ERROR, "ramdisk decompress failed");
        return -1;
    }

    // Step 2: compress Magisk binaries for overlay
    snprintf(cmd, sizeof cmd,
        "cd %s && "
        "./magiskboot compress=xz magisk magisk.xz 2>&1 && "
        "./magiskboot compress=xz stub.apk stub.xz 2>&1 && "
        "./magiskboot compress=xz init-ld init-ld.xz 2>&1",
        DEVICE_WORK);
    if (adb_shell_check(serial, cmd) != 0) {
        soulx_log(SOULX_LOG_ERROR, "tool compression failed");
        return -1;
    }

    // Step 3: write Magisk config
    snprintf(cmd, sizeof cmd,
        "cd %s && "
        "echo 'KEEPVERITY=true' > config && "
        "echo 'KEEPFORCEENCRYPT=true' >> config && "
        "echo 'RECOVERYMODE=false' >> config && "
        "echo 'VENDORBOOT=false' >> config && "
        "echo 'PREINITDEVICE=vdd1' >> config",
        DEVICE_WORK);
    if (adb_shell_check(serial, cmd) != 0) {
        soulx_log(SOULX_LOG_ERROR, "failed to write Magisk config");
        return -1;
    }

    // Step 4: patch the cpio — inject Magisk into the ramdisk.
    // 'backup' must run AFTER 'add init' so .backup/init.xz has the original init.
    // busybox and magiskpolicy go UNCOMPRESSED into overlay.d/sbin/ — magiskinit only
    // auto-decompresses its own known .xz files (magisk, stub, init-ld); custom .xz
    // files are copied verbatim.  Uncompressed files land directly in /debug_ramdisk/.
    snprintf(cmd, sizeof cmd,
        "cd %s && "
        "./magiskboot cpio ramdisk.cpio "
        "\"add 0750 init magiskinit\" "
        "\"mkdir 0750 overlay.d\" "
        "\"mkdir 0750 overlay.d/sbin\" "
        "\"add 0644 overlay.d/sbin/magisk.xz magisk.xz\" "
        "\"add 0644 overlay.d/sbin/stub.xz stub.xz\" "
        "\"add 0644 overlay.d/sbin/init-ld.xz init-ld.xz\" "
        "\"add 0755 overlay.d/sbin/busybox busybox\" "
        "\"add 0755 overlay.d/sbin/magiskpolicy magiskpolicy\" "
        "\"add 0644 overlay.d/sbin/magisk_prebuilt.db magisk_prebuilt.db\" "
        "\"backup ramdisk.cpio.orig\" "
        "\"patch\" "
        "\"mkdir 000 .backup\" "
        "\"add 000 .backup/.magisk config\"",
        DEVICE_WORK);
    if (adb_shell_check(serial, cmd) != 0) {
        soulx_log(SOULX_LOG_ERROR, "ramdisk cpio patching failed");
        return -1;
    }

    // Step 5: recompress
    snprintf(cmd, sizeof cmd,
        "cd %s && "
        "./magiskboot compress=lz4_lg ramdisk.cpio ramdisk_patched.img 2>&1",
        DEVICE_WORK);
    if (adb_shell_check(serial, cmd) != 0) {
        soulx_log(SOULX_LOG_ERROR, "ramdisk recompress failed");
        return -1;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

#define ROOT_STEPS 7
#define ROOT_PROGRESS(step, msg) do { \
    printf("[root] %d/%d %s\n", (step), ROOT_STEPS, (msg)); fflush(stdout); \
} while (0)

int soulx_magisk_patch(const char* name) {
    SoulXInstanceConf c;
    if (soulx_reg_load(name, &c) != 0) {
        soulx_log(SOULX_LOG_ERROR, "instance '%s' not found", name);
        return -1;
    }

    SoulXRunState st;
    if (soulx_instance_running(c.name, &st)) {
        soulx_log(SOULX_LOG_ERROR, "'%s' is running; stop it first (soulx stop %s)", c.name, c.name);
        return -1;
    }

    if (!soulx_runtime_ready(c.image)) {
        soulx_log(SOULX_LOG_ERROR, "the runtime for image '%s' is not installed; run: soulx setup", c.image);
        return -1;
    }

    char stock_ramdisk[PATH_MAX];
    if (find_stock_ramdisk(&c, stock_ramdisk, sizeof stock_ramdisk) != 0) return -1;

    ROOT_PROGRESS(1, "Downloading Magisk tools");
    if (soulx_magisk_ensure_tools() != 0) return -1;

    ROOT_PROGRESS(2, "Starting instance");
    SoulXStartOpts so = { .headless = true, .cold = true, .wait = true, .timeout_s = BOOT_TIMEOUT };
    if (soulx_mac_start(c.name, &so, &st) != 0) {
        soulx_log(SOULX_LOG_ERROR, "failed to start '%s'", c.name);
        return -1;
    }

    char serial[32];
    soulx_serial(st.port, serial, sizeof serial);

    ROOT_PROGRESS(3, "Waiting for boot");
    if (soulx_adb_wait_boot(serial, st.pid, BOOT_TIMEOUT) != 0) {
        soulx_log(SOULX_LOG_ERROR, "Android did not finish booting");
        soulx_mac_stop(c.name, false);
        return -1;
    }

    ROOT_PROGRESS(4, "Pushing tools to device");
    int rc = push_tools_to_device(serial);

    if (rc == 0) {
        ROOT_PROGRESS(5, "Patching ramdisk");
        rc = patch_ramdisk_on_device(serial, stock_ramdisk);
    }

    char avd_dir[PATH_MAX];
    char dest[PATH_MAX];
    if (rc == 0) {
        ROOT_PROGRESS(6, "Pulling patched ramdisk");
        if (soulx_path(avd_dir, sizeof avd_dir, "avd/soulx_%s.avd", c.name) != 0 ||
            snprintf(dest, sizeof dest, "%s/%s", avd_dir, SOULX_MAGISK_RAMDISK) < 0) {
            soulx_log(SOULX_LOG_ERROR, "path too long");
            rc = -1;
        }
    }

    char remote_patched[256];
    snprintf(remote_patched, sizeof remote_patched, "%s/ramdisk_patched.img", DEVICE_WORK);
    if (rc == 0) {
        rc = adb_pull(serial, remote_patched, dest);
        if (rc != 0) soulx_log(SOULX_LOG_ERROR, "failed to pull patched ramdisk");
    }

    adb_shell_check(serial, "rm -rf " DEVICE_WORK);

    ROOT_PROGRESS(7, "Stopping instance");
    soulx_mac_stop(c.name, false);

    if (rc != 0) return -1;

    struct stat fst;
    if (stat(dest, &fst) != 0 || fst.st_size < 1024) {
        soulx_log(SOULX_LOG_ERROR, "patched ramdisk is missing or too small: %s", dest);
        return -1;
    }

    if (!c.rooted) {
        c.rooted = true;
        bool restart = false;
        if (soulx_mac_reconfigure(&c, &restart) != 0) {
            soulx_log(SOULX_LOG_WARN, "ramdisk patched but failed to set rooted=1 in the config; "
                      "enable it manually: soulx config %s --root", c.name);
        }
    }

    // The Magisk environment (/data/adb/magisk/ + db seed) is set up automatically
    // by soulx_mac_post_boot on the first rooted boot — no second boot needed here.
    soulx_log(SOULX_LOG_INFO, "Root is ready. Start '%s' and Magisk root is active from the first boot.", c.name);
    return 0;
}

int soulx_magisk_unpatch(const char* name) {
    SoulXInstanceConf c;
    if (soulx_reg_load(name, &c) != 0) {
        soulx_log(SOULX_LOG_ERROR, "instance '%s' not found", name);
        return -1;
    }

    char path[PATH_MAX];
    if (soulx_path(path, sizeof path, "avd/soulx_%s.avd/%s", c.name, SOULX_MAGISK_RAMDISK) != 0)
        return -1;

    if (access(path, F_OK) == 0) {
        if (unlink(path) != 0) {
            soulx_log(SOULX_LOG_ERROR, "cannot remove %s: %s", path, strerror(errno));
            return -1;
        }
        soulx_log(SOULX_LOG_INFO, "removed patched ramdisk: %s", path);
    }

    if (c.rooted) {
        c.rooted = false;
        bool restart = false;
        soulx_mac_reconfigure(&c, &restart);
    }
    return 0;
}

int soulx_magisk_verify(const char* name) {
    SoulXInstanceConf c;
    SoulXRunState st;
    char serial[64];
    if (soulx_mac_require_running(name, &c, &st, serial, sizeof serial) != 0)
        return -1;

    if (!c.rooted) {
        soulx_log(SOULX_LOG_WARN, "'%s' does not have root enabled in its configuration", c.name);
        return -1;
    }

    // check Magisk binary in debug_ramdisk (placed there by magiskinit at boot)
    char* out = NULL;
    if (soulx_adb_shell(serial, "test -f /debug_ramdisk/magisk && echo ok", &out, 5000) != 0
        || !out || strncmp(out, "ok", 2) != 0) {
        free(out);
        soulx_log(SOULX_LOG_ERROR, "Magisk binary not found on '%s' — root is not active", c.name);
        return -1;
    }
    free(out);

    // verify su actually grants uid 0
    out = NULL;
    if (soulx_adb_shell(serial, "su -c id", &out, 5000) != 0 || !out || !strstr(out, "uid=0")) {
        free(out);
        soulx_log(SOULX_LOG_ERROR, "su test failed on '%s' — Magisk is present but su does not work", c.name);
        return -1;
    }
    free(out);

    soulx_log(SOULX_LOG_INFO, "root verified on '%s': Magisk daemon running, su grants uid=0", c.name);
    return 0;
}
