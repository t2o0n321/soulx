/*
 * SoulX Android Emulator - unit tests for the macOS infrastructure: paths.c, proc.c (and log.c)
 *
 * Standalone program: returns 0 when every check passes and prints each failure. It never touches
 * the real ~/.soulx-android: SOULX_HOME points to a fresh mkdtemp() directory under /private/tmp.
 * No emulator, network or Android SDK is needed. The binary re-executes a copy of itself (placed
 * under $SOULX_HOME/sdk/emulator/) as a stand-in "engine" for the soulx_pid_is_engine() test.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <libproc.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "soulx_mac.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do {                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                       \
    } while (0)

static char g_home[PATH_MAX];       // the temporary SOULX_HOME
static char g_outside[PATH_MAX];    // a directory outside SOULX_HOME that must survive

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
    if (path && path[0] == '/' && strncmp(path, "/private/tmp/", 13) == 0) {
        nftw(path, rm_cb, 32, FTW_DEPTH | FTW_PHYS);
    }
}

static void join(char* buf, size_t cap, const char* a, const char* b) {
    int n = snprintf(buf, cap, "%s/%s", a, b);
    if (n < 0 || (size_t)n >= cap) {
        fprintf(stderr, "path too long: %s/%s\n", a, b);
        exit(2);
    }
}

static bool write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fputs(text, f);
    return fclose(f) == 0;
}

static bool env_has(char** env, const char* entry) {
    for (char** e = env; e && *e; e++) {
        if (strcmp(*e, entry) == 0) return true;
    }
    return false;
}

static int env_count_name(char** env, const char* name) {
    size_t n = strlen(name);
    int count = 0;
    for (char** e = env; e && *e; e++) {
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') count++;
    }
    return count;
}

static bool contains(const char* hay, const char* needle) {
    return hay && strstr(hay, needle) != NULL;
}

// ---------------------------------------------------------------------------
// paths.c
// ---------------------------------------------------------------------------

static void test_home_and_paths(void) {
    CHECK(strcmp(soulx_home(), g_home) == 0);

    char buf[PATH_MAX];
    char want[PATH_MAX];
    CHECK(soulx_path(buf, sizeof(buf), "sdk/%s", "emulator") == 0);
    join(want, sizeof(want), g_home, "sdk/emulator");
    CHECK(strcmp(buf, want) == 0);

    CHECK(soulx_path(buf, sizeof(buf), "%s", "") == 0);
    CHECK(strcmp(buf, g_home) == 0);

    char small[8];
    CHECK(soulx_path(small, sizeof(small), "sdk") == -1);
    CHECK(small[0] == '\0');
    CHECK(soulx_path(buf, sizeof(buf), "%s", "../escape") == -1);
    CHECK(soulx_path(buf, sizeof(buf), "%s", "a/../b") == -1);
    CHECK(soulx_path(buf, sizeof(buf), "%s", "a/..b") == 0);

    // Normalisation and validation of $SOULX_HOME
    char variant[PATH_MAX];
    snprintf(variant, sizeof(variant), "%s//", g_home);
    setenv("SOULX_HOME", variant, 1);
    CHECK(strcmp(soulx_home(), g_home) == 0);

    char cwd[PATH_MAX];
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
    CHECK(chdir(g_home) == 0);
    setenv("SOULX_HOME", "rel-home", 1);
    join(want, sizeof(want), g_home, "rel-home");
    CHECK(strcmp(soulx_home(), want) == 0);
    CHECK(chdir(cwd) == 0);

    setenv("SOULX_HOME", "/", 1);
    CHECK(strcmp(soulx_home(), "/") != 0);       // "/" is never accepted (falls back to the default)

    setenv("SOULX_HOME", g_home, 1);
    CHECK(strcmp(soulx_home(), g_home) == 0);
}

static void test_layout(void) {
    CHECK(soulx_ensure_layout() == 0);
    const char* dirs[] = { "sdk", "sdk/.soulx", "sdk/platforms", "downloads", "licenses", "avd", "home",
                           "home/.android", "instances", "run", "logs", "tmp" };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char p[PATH_MAX];
        join(p, sizeof(p), g_home, dirs[i]);
        CHECK(soulx_is_dir(p));
    }
    char home_dir[PATH_MAX];
    join(home_dir, sizeof(home_dir), g_home, "home");
    struct stat st;
    CHECK(stat(home_dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    CHECK(soulx_ensure_layout() == 0);           // idempotent
}

static void test_mkdirs(void) {
    char p[PATH_MAX];
    join(p, sizeof(p), g_home, "tmp/a/b//c/");
    CHECK(soulx_mkdirs(p) == 0);
    join(p, sizeof(p), g_home, "tmp/a/b/c");
    CHECK(soulx_is_dir(p));
    CHECK(soulx_mkdirs(p) == 0);

    char file[PATH_MAX];
    join(file, sizeof(file), g_home, "tmp/plain-file");
    CHECK(write_text(file, "x"));
    join(p, sizeof(p), g_home, "tmp/plain-file/sub");
    CHECK(soulx_mkdirs(p) == -1);
    CHECK(soulx_path_exists(file));
    CHECK(!soulx_is_dir(file));
    CHECK(!soulx_path_exists(p));
    CHECK(soulx_mkdirs(NULL) == -1);
    CHECK(soulx_mkdirs("") == -1);
}

static void test_atomic_rw(void) {
    char path[PATH_MAX];
    join(path, sizeof(path), g_home, "instances/rw.conf");

    static const char bin[] = { 'a', 'b', 'c', '\0', 'd', 'e', 'f', '\n' };
    CHECK(soulx_write_file_atomic(path, bin, sizeof(bin)) == 0);
    char* data = NULL;
    size_t len = 0;
    CHECK(soulx_read_file(path, &data, &len) == 0);
    CHECK(data && len == sizeof(bin) && memcmp(data, bin, sizeof(bin)) == 0 && data[len] == '\0');
    free(data);

    CHECK(soulx_write_file_atomic(path, "second\n", 7) == 0);
    data = NULL;
    CHECK(soulx_read_file(path, &data, &len) == 0);
    CHECK(data && len == 7 && strcmp(data, "second\n") == 0);
    free(data);

    // keeps the existing permission bits
    CHECK(chmod(path, 0640) == 0);
    CHECK(soulx_write_file_atomic(path, "third", 5) == 0);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0640);

    // empty file, NULL data
    char empty[PATH_MAX];
    join(empty, sizeof(empty), g_home, "instances/empty");
    CHECK(soulx_write_file_atomic(empty, NULL, 0) == 0);
    data = NULL;
    CHECK(soulx_read_file(empty, &data, &len) == 0);
    CHECK(data && len == 0 && data[0] == '\0');
    free(data);
    CHECK(soulx_write_file_atomic(empty, NULL, 3) == -1);

    // larger than the initial buffer, binary
    size_t big_len = 3u * 1024u * 1024u + 17u;
    char* big = malloc(big_len);
    CHECK(big != NULL);
    if (big) {
        for (size_t i = 0; i < big_len; i++) big[i] = (char)(i * 31u + 7u);
        char bigp[PATH_MAX];
        join(bigp, sizeof(bigp), g_home, "tmp/big.bin");
        CHECK(soulx_write_file_atomic(bigp, big, big_len) == 0);
        data = NULL;
        CHECK(soulx_read_file(bigp, &data, &len) == 0);
        CHECK(data && len == big_len && memcmp(data, big, big_len) == 0);
        free(data);
        free(big);
    }

    // missing file / directory
    char missing[PATH_MAX];
    join(missing, sizeof(missing), g_home, "instances/missing.conf");
    data = (char*)1;
    CHECK(soulx_read_file(missing, &data, &len) == -1 && data == NULL);
    CHECK(soulx_read_file(g_home, &data, &len) == -1);

    // no temporary files left behind
    char dir[PATH_MAX];
    join(dir, sizeof(dir), g_home, "instances");
    DIR* d = opendir(dir);
    CHECK(d != NULL);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) CHECK(strstr(e->d_name, ".tmp.") == NULL);
        closedir(d);
    }
}

static void test_rmtree(void) {
    // refusals
    CHECK(soulx_rmtree(NULL) == -1);
    CHECK(soulx_rmtree("") == -1);
    CHECK(soulx_rmtree("/") == -1);
    CHECK(soulx_rmtree("/private/tmp") == -1);
    CHECK(soulx_rmtree(g_home) == -1);
    CHECK(soulx_rmtree("tmp") == -1);
    CHECK(soulx_rmtree(g_outside) == -1);
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/", g_home);
    CHECK(soulx_rmtree(p) == -1);
    snprintf(p, sizeof(p), "%s/.", g_home);
    CHECK(soulx_rmtree(p) == -1);
    snprintf(p, sizeof(p), "%s/tmp/..", g_home);
    CHECK(soulx_rmtree(p) == -1);

    char keep[PATH_MAX];
    join(keep, sizeof(keep), g_outside, "keep.txt");
    CHECK(write_text(keep, "keep"));
    snprintf(p, sizeof(p), "%s/../%s/keep.txt", g_home, strrchr(g_outside, '/') + 1);
    CHECK(soulx_rmtree(p) == -1);
    CHECK(soulx_path_exists(keep));

    // same prefix, different directory ("<home>-sibling")
    char sib[PATH_MAX];
    snprintf(sib, sizeof(sib), "%s-sibling", g_home);
    CHECK(mkdir(sib, 0755) == 0);
    char sib_file[PATH_MAX];
    join(sib_file, sizeof(sib_file), sib, "f");
    CHECK(write_text(sib_file, "x"));
    CHECK(soulx_rmtree(sib_file) == -1);
    CHECK(soulx_path_exists(sib_file));
    rm_rf(sib);

    // a tree with a symlink pointing outside: only the link goes away
    char tree[PATH_MAX], deep[PATH_MAX], f[PATH_MAX], link[PATH_MAX];
    join(tree, sizeof(tree), g_home, "tmp/tree");
    join(deep, sizeof(deep), tree, "a/b/c");
    CHECK(soulx_mkdirs(deep) == 0);
    join(f, sizeof(f), deep, "file.txt");
    CHECK(write_text(f, "data"));
    join(link, sizeof(link), tree, "a/outside-link");
    CHECK(symlink(g_outside, link) == 0);
    CHECK(soulx_rmtree(tree) == 0);
    CHECK(!soulx_path_exists(tree));
    CHECK(soulx_path_exists(keep));

    // a symlink in the middle of the path is refused; the link itself can be removed
    char mid[PATH_MAX], through[PATH_MAX];
    join(mid, sizeof(mid), g_home, "tmp/mid");
    CHECK(symlink(g_outside, mid) == 0);
    join(through, sizeof(through), mid, "keep.txt");
    CHECK(soulx_rmtree(through) == -1);
    CHECK(soulx_path_exists(keep));
    CHECK(soulx_rmtree(mid) == 0);
    struct stat st;
    CHECK(lstat(mid, &st) != 0);
    CHECK(soulx_path_exists(keep));

    // read-only directories are emptied too
    char ro[PATH_MAX], ro_sub[PATH_MAX], ro_file[PATH_MAX];
    join(ro, sizeof(ro), g_home, "tmp/ro");
    join(ro_sub, sizeof(ro_sub), ro, "sub");
    CHECK(soulx_mkdirs(ro_sub) == 0);
    join(ro_file, sizeof(ro_file), ro_sub, "f");
    CHECK(write_text(ro_file, "x"));
    CHECK(chmod(ro_sub, 0555) == 0);
    CHECK(soulx_rmtree(ro) == 0);
    CHECK(!soulx_path_exists(ro));

    // single file, missing path
    char single[PATH_MAX];
    join(single, sizeof(single), g_home, "tmp/single");
    CHECK(write_text(single, "x"));
    CHECK(soulx_rmtree(single) == 0);
    CHECK(!soulx_path_exists(single));
    join(single, sizeof(single), g_home, "tmp/never/existed");
    CHECK(soulx_rmtree(single) == 0);
}

static void test_sizes(void) {
    char dir[PATH_MAX];
    join(dir, sizeof(dir), g_home, "tmp/sized");
    CHECK(soulx_mkdirs(dir) == 0);
    char block[10000];
    memset(block, 'z', sizeof(block));
    for (int i = 0; i < 3; i++) {
        char f[PATH_MAX];
        char name[32];
        snprintf(name, sizeof(name), "f%d", i);
        join(f, sizeof(f), dir, name);
        CHECK(soulx_write_file_atomic(f, block, sizeof(block)) == 0);
    }
    CHECK(soulx_dir_size(dir) >= 30000);
    char missing[PATH_MAX];
    join(missing, sizeof(missing), g_home, "tmp/no-such-dir");
    CHECK(soulx_dir_size(missing) == 0);
    CHECK(soulx_free_disk_bytes(g_home) > 0);
    join(missing, sizeof(missing), g_home, "does/not/exist/yet");
    CHECK(soulx_free_disk_bytes(missing) > 0);
}

// ---------------------------------------------------------------------------
// proc.c
// ---------------------------------------------------------------------------

static void test_run_basic(void) {
    const char* argv[] = { "/bin/echo", "hello", NULL };
    SoulXCmd cmd = { .argv = argv };
    SoulXCmdResult res;
    CHECK(soulx_run(&cmd, &res) == 0);
    CHECK(res.status == 0 && !res.timed_out);
    CHECK(res.out && res.out_len == 6 && strcmp(res.out, "hello\n") == 0);
    CHECK(res.err && res.err_len == 0);
    soulx_cmd_result_free(&res);
    CHECK(res.out == NULL && res.err == NULL);

    const char* f[] = { "/usr/bin/false", NULL };
    cmd.argv = f;
    CHECK(soulx_run(&cmd, &res) == 1);
    soulx_cmd_result_free(&res);
    CHECK(soulx_run(&cmd, NULL) == 1);

    const char* ls[] = { "/bin/ls", "/nonexistent-soulx-test-path", NULL };
    cmd.argv = ls;
    CHECK(soulx_run(&cmd, &res) != 0);
    CHECK(contains(res.err, "No such file"));
    soulx_cmd_result_free(&res);

    const char* pwd[] = { "/bin/pwd", "-P", NULL };
    cmd.argv = pwd;
    cmd.cwd = g_home;
    CHECK(soulx_run(&cmd, &res) == 0);
    char want[PATH_MAX + 2];
    snprintf(want, sizeof(want), "%s\n", g_home);
    CHECK(res.out && strcmp(res.out, want) == 0);
    soulx_cmd_result_free(&res);
}

static void test_run_errors(void) {
    const char* missing[] = { "/nonexistent/soulx-test-binary", NULL };
    SoulXCmd cmd = { .argv = missing };
    SoulXCmdResult res;
    CHECK(soulx_run(&cmd, &res) == -1);
    CHECK(res.status == -1);
    CHECK(res.err && res.err_len > 0);
    soulx_cmd_result_free(&res);

    const char* relative[] = { "echo", "x", NULL };
    cmd.argv = relative;
    CHECK(soulx_run(&cmd, &res) == -1);
    soulx_cmd_result_free(&res);

    cmd.argv = NULL;
    CHECK(soulx_run(&cmd, &res) == -1);
    soulx_cmd_result_free(&res);
    CHECK(soulx_run(NULL, NULL) == -1);
}

static void test_run_timeout(void) {
    const char* argv[] = { "/bin/sleep", "5", NULL };
    SoulXCmd cmd = { .argv = argv, .timeout_ms = 300 };
    SoulXCmdResult res;
    double t0 = soulx_now_ms();
    int status = soulx_run(&cmd, &res);
    double took = soulx_now_ms() - t0;
    CHECK(res.timed_out);
    CHECK(status == 128 + SIGKILL && res.status == 128 + SIGKILL);
    CHECK(took >= 250 && took < 2000);
    soulx_cmd_result_free(&res);
}

static void test_run_stdin(void) {
    static const char bin[] = { 'a', 'b', 'c', '\0', 'd', 'e', 'f' };
    const char* cat[] = { "/bin/cat", NULL };
    SoulXCmd cmd = { .argv = cat, .stdin_data = bin, .stdin_len = sizeof(bin) };
    SoulXCmdResult res;
    CHECK(soulx_run(&cmd, &res) == 0);
    CHECK(res.out_len == sizeof(bin) && memcmp(res.out, bin, sizeof(bin)) == 0);
    soulx_cmd_result_free(&res);

    // 2 MB in both directions at once: must not deadlock
    size_t n = 2u * 1024u * 1024u;
    char* big = malloc(n);
    CHECK(big != NULL);
    if (big) {
        for (size_t i = 0; i < n; i++) big[i] = (char)('a' + i % 26);
        cmd.stdin_data = big;
        cmd.stdin_len = n;
        cmd.timeout_ms = 20000;
        CHECK(soulx_run(&cmd, &res) == 0);
        CHECK(res.out_len == n && memcmp(res.out, big, n) == 0);
        soulx_cmd_result_free(&res);

        // max_output caps what is kept, the child still completes
        cmd.max_output = 1000;
        CHECK(soulx_run(&cmd, &res) == 0);
        CHECK(res.out_len == 1000 && memcmp(res.out, big, 1000) == 0 && res.out[1000] == '\0');
        soulx_cmd_result_free(&res);
        cmd.max_output = 0;

        // a child that never reads its stdin: EPIPE is handled (no SIGPIPE for us)
        const char* tru[] = { "/usr/bin/true", NULL };
        cmd.argv = tru;
        CHECK(soulx_run(&cmd, &res) == 0);
        soulx_cmd_result_free(&res);
        CHECK(soulx_run_passthrough(&cmd) == 0);
        free(big);
    }
}

// Every variable soulx_build_env() sets itself: each must appear exactly once in the child environment.
static const char* const k_managed_env[] = {
    "ANDROID_HOME", "ANDROID_SDK_ROOT", "ANDROID_AVD_HOME", "ANDROID_EMULATOR_HOME", "ANDROID_TMP",
    "ANDROID_ADB_SERVER_PORT", "ANDROID_EMU_ENABLE_CRASH_REPORTING", "HOME", "TMPDIR",
    "ANDROID_EMULATOR_WAIT_TIME_BEFORE_KILL", "ADB_LOCAL_TRANSPORT_MAX_PORT",
};

static void test_env(void) {
    // this process's own TMPDIR is put back at the end
    const char* tmpdir_now = getenv("TMPDIR");
    char saved_tmpdir[PATH_MAX];
    bool had_tmpdir = tmpdir_now != NULL && strlen(tmpdir_now) < sizeof(saved_tmpdir);
    if (had_tmpdir) snprintf(saved_tmpdir, sizeof(saved_tmpdir), "%s", tmpdir_now);

    setenv("ANDROID_SDK_HOME", "/bogus/sdk-home", 1);
    setenv("ANDROID_SERIAL", "emulator-9999", 1);
    setenv("ANDROID_HOME", "/bogus/sdk", 1);
    setenv("ANDROID_ADB_SERVER_PORT", "5037", 1);
    setenv("ANDROID_USER_HOME", "/bogus/user-home", 1);
    setenv("TMPDIR", "/bogus/tmpdir/", 1);
    setenv("ANDROID_EMULATOR_WAIT_TIME_BEFORE_KILL", "5", 1);
    setenv("SOULX_TEST_KEEP", "kept", 1);
    // adb must only ever talk to SoulX's private server: every ADB_* / ANDROID_ADB_* variable the
    // user may export (remote server address, vendor keys, kill-server refusal, log path) is dropped,
    // and SoulX caps adb's emulator scan so it never finds Android Studio's.
    setenv("ANDROID_ADB_SERVER_ADDRESS", "adb-remote.invalid", 1);
    setenv("ADB_VENDOR_KEYS", "/bogus/keys", 1);
    setenv("ADB_REJECT_KILL_SERVER", "1", 1);
    setenv("ANDROID_ADB_LOG_PATH", "/bogus/adb.log", 1);
    setenv("ADB_LOCAL_TRANSPORT_MAX_PORT", "5700", 1);

    char** env = soulx_build_env();
    CHECK(env != NULL);
    if (env) {
        char e[PATH_MAX + 64];
        snprintf(e, sizeof(e), "HOME=%s/home", g_home);
        CHECK(env_has(env, e));
        snprintf(e, sizeof(e), "ANDROID_HOME=%s/sdk", g_home);
        CHECK(env_has(env, e));
        snprintf(e, sizeof(e), "ANDROID_SDK_ROOT=%s/sdk", g_home);
        CHECK(env_has(env, e));
        snprintf(e, sizeof(e), "ANDROID_AVD_HOME=%s/avd", g_home);
        CHECK(env_has(env, e));
        snprintf(e, sizeof(e), "ANDROID_EMULATOR_HOME=%s/home/.android", g_home);
        CHECK(env_has(env, e));
        snprintf(e, sizeof(e), "ANDROID_TMP=%s/tmp", g_home);
        CHECK(env_has(env, e));
        CHECK(env_has(env, "ANDROID_ADB_SERVER_PORT=5137"));
        CHECK(env_has(env, "ANDROID_EMU_ENABLE_CRASH_REPORTING=0"));
        // netsimd's netsim.ini and the adb server log must land in $SOULX_HOME/tmp, not the user's $TMPDIR;
        // the engine's kill watchdog gets 90 s so a long-running instance can finish its snapshot save
        snprintf(e, sizeof(e), "TMPDIR=%s/tmp/", g_home);
        CHECK(env_has(env, e));
        CHECK(env_count_name(env, "TMPDIR") == 1);
        CHECK(!env_has(env, "TMPDIR=/bogus/tmpdir/"));
        CHECK(env_has(env, "ANDROID_EMULATOR_WAIT_TIME_BEFORE_KILL=90"));
        CHECK(env_count_name(env, "ANDROID_EMULATOR_WAIT_TIME_BEFORE_KILL") == 1);
        CHECK(!env_has(env, "ANDROID_EMULATOR_WAIT_TIME_BEFORE_KILL=5"));
        for (size_t i = 0; i < sizeof(k_managed_env) / sizeof(k_managed_env[0]); i++) {
            CHECK(env_count_name(env, k_managed_env[i]) == 1);
        }
        CHECK(env_has(env, "SOULX_TEST_KEEP=kept"));
        CHECK(env_count_name(env, "HOME") == 1);
        CHECK(env_count_name(env, "ANDROID_HOME") == 1);
        CHECK(env_count_name(env, "ANDROID_ADB_SERVER_PORT") == 1);
        CHECK(env_count_name(env, "ANDROID_SDK_HOME") == 0);
        CHECK(env_count_name(env, "ANDROID_SERIAL") == 0);
        CHECK(env_count_name(env, "ANDROID_USER_HOME") == 0);
        CHECK(env_count_name(env, "ANDROID_PREFS_ROOT") == 0);
        CHECK(env_count_name(env, "ADB_SERVER_SOCKET") == 0);
        // every stray adb variable scrubbed; the scan is capped at 5554
        CHECK(env_count_name(env, "ANDROID_ADB_SERVER_ADDRESS") == 0);
        CHECK(env_count_name(env, "ADB_VENDOR_KEYS") == 0);
        CHECK(env_count_name(env, "ADB_REJECT_KILL_SERVER") == 0);
        CHECK(env_count_name(env, "ANDROID_ADB_LOG_PATH") == 0);
        CHECK(env_has(env, "ADB_LOCAL_TRANSPORT_MAX_PORT=5554"));
        CHECK(env_count_name(env, "ADB_LOCAL_TRANSPORT_MAX_PORT") == 1);
        CHECK(!env_has(env, "ADB_LOCAL_TRANSPORT_MAX_PORT=5700"));
        soulx_free_env(env);
    }

    // what a child really sees
    const char* argv[] = { "/usr/bin/env", NULL };
    SoulXCmd cmd = { .argv = argv, .soulx_env = true };
    SoulXCmdResult res;
    CHECK(soulx_run(&cmd, &res) == 0);
    char line[PATH_MAX + 64];
    snprintf(line, sizeof(line), "\nHOME=%s/home\n", g_home);
    CHECK(contains(res.out, line));
    CHECK(contains(res.out, "ANDROID_ADB_SERVER_PORT=5137\n"));
    snprintf(line, sizeof(line), "\nTMPDIR=%s/tmp/\n", g_home);
    CHECK(contains(res.out, line));
    CHECK(contains(res.out, "\nANDROID_EMULATOR_WAIT_TIME_BEFORE_KILL=90\n"));
    CHECK(!contains(res.out, "/bogus/tmpdir"));
    CHECK(!contains(res.out, "ANDROID_EMULATOR_WAIT_TIME_BEFORE_KILL=5"));
    CHECK(!contains(res.out, "ANDROID_SDK_HOME"));
    CHECK(!contains(res.out, "ANDROID_SERIAL"));
    soulx_cmd_result_free(&res);

    // extra variables for one child: added once, replacing an inherited one of the same name
    setenv("SOULX_TEST_EXTRA", "inherited", 1);
    const char* const extra[] = { "SOULX_TEST_EXTRA=mine", "SOULX_TEST_EXTRA2=two", NULL };
    cmd.env = extra;
    CHECK(soulx_run(&cmd, &res) == 0);
    CHECK(contains(res.out, "\nSOULX_TEST_EXTRA=mine\n") && contains(res.out, "\nSOULX_TEST_EXTRA2=two\n"));
    CHECK(!contains(res.out, "SOULX_TEST_EXTRA=inherited"));
    CHECK(contains(res.out, "ANDROID_ADB_SERVER_PORT=5137\n"));             // the SoulX environment as before
    soulx_cmd_result_free(&res);
    cmd.env = NULL;
    unsetenv("SOULX_TEST_EXTRA");

    cmd.soulx_env = false;
    CHECK(soulx_run(&cmd, &res) == 0);
    CHECK(contains(res.out, "ANDROID_SDK_HOME=/bogus/sdk-home"));
    CHECK(contains(res.out, "TMPDIR=/bogus/tmpdir/"));
    CHECK(contains(res.out, "ANDROID_EMULATOR_WAIT_TIME_BEFORE_KILL=5"));
    soulx_cmd_result_free(&res);

    unsetenv("ANDROID_SDK_HOME");
    unsetenv("ANDROID_SERIAL");
    unsetenv("ANDROID_HOME");
    unsetenv("ANDROID_ADB_SERVER_PORT");
    unsetenv("ANDROID_USER_HOME");
    unsetenv("ANDROID_EMULATOR_WAIT_TIME_BEFORE_KILL");
    unsetenv("SOULX_TEST_KEEP");
    if (had_tmpdir) setenv("TMPDIR", saved_tmpdir, 1);
    else unsetenv("TMPDIR");
}

static void test_passthrough(void) {
    const char* tru[] = { "/usr/bin/true", NULL };
    const char* fal[] = { "/usr/bin/false", NULL };
    SoulXCmd cmd = { .argv = tru };
    CHECK(soulx_run_passthrough(&cmd) == 0);
    cmd.argv = fal;
    CHECK(soulx_run_passthrough(&cmd) == 1);

    const char* slp[] = { "/bin/sleep", "5", NULL };
    cmd.argv = slp;
    cmd.timeout_ms = 300;
    double t0 = soulx_now_ms();
    CHECK(soulx_run_passthrough(&cmd) == 128 + SIGKILL);
    CHECK(soulx_now_ms() - t0 < 2000);

    const char* missing[] = { "/nonexistent/soulx-test-binary", NULL };
    cmd.argv = missing;
    cmd.timeout_ms = 0;
    CHECK(soulx_run_passthrough(&cmd) == -1);
}

static void test_detached(const char* self_exe) {
    char log[PATH_MAX];
    join(log, sizeof(log), g_home, "logs/detached.log");
    const char* echo[] = { "/bin/echo", "detached-ok", NULL };
    SoulXCmd cmd = { .argv = echo };
    pid_t pid = soulx_spawn_detached(&cmd, log, false);
    CHECK(pid > 0);
    CHECK(soulx_wait_pid_exit(pid, 5000) == 0);
    CHECK(!soulx_pid_alive(pid));
    pid = soulx_spawn_detached(&cmd, log, true);     // appends; QoS class requested
    CHECK(pid > 0);
    CHECK(soulx_wait_pid_exit(pid, 5000) == 0);
    char* data = NULL;
    size_t len = 0;
    CHECK(soulx_read_file(log, &data, &len) == 0);
    CHECK(data && strcmp(data, "detached-ok\ndetached-ok\n") == 0);
    free(data);

    // A parent whose stdin is closed (daemon-like): fd 0 must not swallow the child's output.
    char log2[PATH_MAX];
    join(log2, sizeof(log2), g_home, "logs/closed-stdin.log");
    int saved_in = dup(STDIN_FILENO);           // -1 when this test already runs with stdin closed
    if (saved_in >= 0) close(STDIN_FILENO);
    pid = soulx_spawn_detached(&cmd, log2, false);
    const char* tru[] = { "/usr/bin/true", NULL };
    SoulXCmd pass = { .argv = tru };
    int prc = soulx_run_passthrough(&pass);
    if (saved_in >= 0) {
        dup2(saved_in, STDIN_FILENO);
        close(saved_in);
    }
    CHECK(prc == 0);
    CHECK(pid > 0);
    CHECK(soulx_wait_pid_exit(pid, 5000) == 0);
    data = NULL;
    CHECK(soulx_read_file(log2, &data, &len) == 0);
    CHECK(data && strcmp(data, "detached-ok\n") == 0);
    free(data);

    // new session, alive until killed, not an engine
    const char* slp[] = { "/bin/sleep", "30", NULL };
    cmd.argv = slp;
    pid = soulx_spawn_detached(&cmd, log, true);
    CHECK(pid > 0);
    if (pid > 0) {
        CHECK(getsid(pid) == pid);
        CHECK(getpgid(pid) == pid);
        CHECK(soulx_pid_alive(pid));
        CHECK(!soulx_pid_is_engine(pid));
        CHECK(soulx_wait_pid_exit(pid, 200) == -1);
        CHECK(kill(pid, SIGTERM) == 0);
        CHECK(soulx_wait_pid_exit(pid, 5000) == 0);
        CHECK(!soulx_pid_alive(pid));
    }

    // bad input
    const char* relative[] = { "sleep", "1", NULL };
    cmd.argv = relative;
    CHECK(soulx_spawn_detached(&cmd, log, false) == -1);
    char bad_log[PATH_MAX];
    join(bad_log, sizeof(bad_log), g_home, "no-such-dir/x.log");
    cmd.argv = slp;
    CHECK(soulx_spawn_detached(&cmd, bad_log, false) == -1);

    // soulx_pid_is_engine: a copy of this test binary under sdk/emulator/ plays the engine
    char engine_dir[PATH_MAX];
    char engine[PATH_MAX];
    join(engine_dir, sizeof(engine_dir), g_home, "sdk/emulator");
    join(engine, sizeof(engine), engine_dir, "fake-engine");
    CHECK(soulx_mkdirs(engine_dir) == 0);
    data = NULL;
    CHECK(self_exe && soulx_read_file(self_exe, &data, &len) == 0);
    if (data) {
        CHECK(soulx_write_file_atomic(engine, data, len) == 0);
        CHECK(chmod(engine, 0755) == 0);
        free(data);
        const char* fake[] = { engine, "--sleep-child", "30", NULL };
        cmd.argv = fake;
        pid = soulx_spawn_detached(&cmd, log, true);
        CHECK(pid > 0);
        if (pid > 0) {
            bool seen = false;
            for (int i = 0; i < 100 && !seen; i++) {    // wait for the exec to complete
                seen = soulx_pid_is_engine(pid);
                if (!seen) soulx_sleep_ms(20);
            }
            CHECK(seen);
            kill(pid, SIGKILL);
            CHECK(soulx_wait_pid_exit(pid, 5000) == 0);
            CHECK(!soulx_pid_is_engine(pid));
        }
    }
    CHECK(!soulx_pid_is_engine(getpid()));
    CHECK(!soulx_pid_is_engine(0));
    CHECK(!soulx_pid_is_engine(-1));
}

static void test_time_and_signals(void) {
    CHECK(soulx_pid_alive(getpid()));
    CHECK(!soulx_pid_alive(0));
    CHECK(!soulx_pid_alive(-5));

    double a = soulx_now_ms();
    CHECK(soulx_sleep_ms(50) == 0);
    double b = soulx_now_ms();
    CHECK(b - a >= 45 && b - a < 1000);
    CHECK(soulx_sleep_ms(0) == 0);

    CHECK(!soulx_interrupted());
    g_soulx_interrupted = 1;
    CHECK(soulx_interrupted());
    double c = soulx_now_ms();
    CHECK(soulx_sleep_ms(1000) == -1);
    CHECK(soulx_now_ms() - c < 100);
    g_soulx_interrupted = 0;
}

static void test_log(void) {
    soulx_set_verbose(true);
    CHECK(soulx_is_verbose());
    soulx_log(SOULX_LOG_DEBUG, "debug line from test_paths_proc (verbose on)");
    soulx_set_verbose(false);
    if (!getenv("SOULX_VERBOSE")) CHECK(!soulx_is_verbose());
    soulx_log(SOULX_LOG_DEBUG, "this debug line must not be printed");
    soulx_log(SOULX_LOG_INFO, "info line from test_paths_proc %d", 1);
    soulx_log(SOULX_LOG_WARN, "warning line from test_paths_proc\n");
    char long_msg[3000];
    memset(long_msg, 'x', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';
    soulx_log(SOULX_LOG_ERROR, "long message (%zu chars) %.20s...", strlen(long_msg), long_msg);
    soulx_status_line("status %d%%", 50);   // no output unless stderr is a terminal
    soulx_status_done();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "--sleep-child") == 0) {   // stand-in engine process
        sleep((unsigned)atoi(argv[2]));
        return 0;
    }

    char self_exe[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(getpid(), self_exe, sizeof(self_exe)) <= 0) self_exe[0] = '\0';

    char tmpl[] = "/private/tmp/soulx-test-pp-XXXXXX";
    char tmpl2[] = "/private/tmp/soulx-test-pp-out-XXXXXX";
    if (!mkdtemp(tmpl) || !mkdtemp(tmpl2)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(g_home, sizeof(g_home), "%s", tmpl);
    snprintf(g_outside, sizeof(g_outside), "%s", tmpl2);
    setenv("SOULX_HOME", g_home, 1);

    test_home_and_paths();
    test_layout();
    test_mkdirs();
    test_atomic_rw();
    test_rmtree();
    test_sizes();
    test_run_basic();
    test_run_errors();
    test_run_timeout();
    test_run_stdin();
    test_env();
    test_passthrough();
    test_detached(self_exe[0] ? self_exe : NULL);
    test_time_and_signals();
    test_log();

    if (g_failures == 0) {
        rm_rf(g_home);
        rm_rf(g_outside);
        printf("test_paths_proc: all %d checks passed\n", g_checks);
        return 0;
    }
    fprintf(stderr, "test_paths_proc: %d of %d checks FAILED (kept %s)\n", g_failures, g_checks, g_home);
    return 1;
}
