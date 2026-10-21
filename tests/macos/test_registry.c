/*
 * SoulX Android Emulator - unit tests for registry.c, avd.c and engine.c (macOS)
 *
 * Standalone program: returns 0 when every check passes and prints each failure.
 * SOULX_HOME points to a fresh mkdtemp() directory under /private/tmp; the real
 * ~/.soulx-android, adb and the Android Emulator are never used. The lifecycle
 * test installs copies of this binary as a fake runtime under $SOULX_HOME/sdk:
 *   sdk/emulator/emulator                              -> fake launcher (execs or spawns qemu; like the
 *                                                          real one it picks the -headless engine for
 *                                                          -no-window)
 *   sdk/emulator/qemu/darwin-aarch64/qemu-system-aarch64[-headless]
 *                                                       -> fake engine (sleeps until signalled; with
 *                                                          SOULX_TEST_FAKE_CONSOLE_MS it also serves the
 *                                                          emulator console and "saves a snapshot" on kill,
 *                                                          logging the lines the real engine prints)
 *   sdk/platform-tools/adb                             -> fake adb (boot_completed = 1 unless
 *                                                          $SOULX_HOME/tmp/fake-booting exists; every call
 *                                                          is appended to $SOULX_HOME/tmp/fake-adb.log)
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <libproc.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "soulx_mac.h"

extern char** environ;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do {                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                       \
    } while (0)

#define CHECK_STR(actual, expected) do {                                        \
        g_checks++;                                                             \
        const char* a_ = (actual);                                              \
        const char* e_ = (expected);                                            \
        if (!a_ || strcmp(a_, e_) != 0) {                                       \
            g_failures++;                                                       \
            fprintf(stderr, "FAIL %s:%d: %s == \"%s\" (got \"%s\")\n",          \
                    __FILE__, __LINE__, #actual, e_, a_ ? a_ : "(null)");       \
        }                                                                       \
    } while (0)

static char g_home[PATH_MAX];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int rm_cb(const char* path, const struct stat* sb, int flag, struct FTW* ftw) {
    (void)sb;
    (void)ftw;
    if (flag == FTW_DP || flag == FTW_D || flag == FTW_DNR) {
        rmdir(path);
    } else {
        unlink(path);
    }
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

static bool write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

static char* read_text(const char* path) {
    char* data = NULL;
    size_t len = 0;
    return soulx_read_file(path, &data, &len) == 0 ? data : NULL;
}

// True when |text| contains |line| as a complete line.
static bool has_line(const char* text, const char* line) {
    size_t n = strlen(line);
    for (const char* p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (strncmp(p, line, n) == 0 && (p[n] == '\n' || p[n] == '\0')) return true;
    }
    return false;
}

// Number of complete lines of |text| equal to |line|.
static int count_lines(const char* text, const char* line) {
    int count = 0;
    size_t n = strlen(line);
    for (const char* p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (strncmp(p, line, n) == 0 && (p[n] == '\n' || p[n] == '\0')) count++;
    }
    return count;
}

static bool file_exists(const char* path) {
    struct stat sb;
    return lstat(path, &sb) == 0;
}

static bool conf_equal(const SoulXInstanceConf* a, const SoulXInstanceConf* b) {
    return a->id == b->id && !strcmp(a->name, b->name) && !strcmp(a->image, b->image) &&
           a->cores == b->cores && a->ram_mb == b->ram_mb && a->data_gb == b->data_gb &&
           a->width == b->width && a->height == b->height && a->dpi == b->dpi &&
           a->landscape == b->landscape && !strcmp(a->gpu, b->gpu) && !strcmp(a->dns, b->dns) &&
           a->stay_awake == b->stay_awake && a->lean == b->lean && a->fps == b->fps && a->audio == b->audio &&
           !strcmp(a->created, b->created) && a->last_port == b->last_port;
}

// Messages handed to the public log callback (soulx_set_log_callback), for checks on what was reported.
static pthread_mutex_t g_logs_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_logs[64 * 1024];
static size_t g_logs_len;

static void capture_log(int level, const char* message) {
    pthread_mutex_lock(&g_logs_mu);
    int n = snprintf(g_logs + g_logs_len, sizeof(g_logs) - g_logs_len, "%d %s\n", level, message);
    if (n > 0 && g_logs_len + (size_t)n < sizeof(g_logs)) g_logs_len += (size_t)n;
    pthread_mutex_unlock(&g_logs_mu);
}

static void logs_start(void) {
    pthread_mutex_lock(&g_logs_mu);
    g_logs_len = 0;
    g_logs[0] = '\0';
    pthread_mutex_unlock(&g_logs_mu);
    soulx_set_log_callback(capture_log);
}

static void logs_stop(void) {
    soulx_set_log_callback(NULL);
}

// Number of captured messages containing |text|.
static int logs_count(const char* text) {
    pthread_mutex_lock(&g_logs_mu);
    int n = 0;
    for (const char* p = strstr(g_logs, text); p; p = strstr(p + 1, text)) n++;
    pthread_mutex_unlock(&g_logs_mu);
    return n;
}

static SoulXInstanceConf make_conf(int id, const char* name) {
    SoulXInstanceConf c;
    soulx_conf_defaults(&c, name);
    c.id = id;
    snprintf(c.created, sizeof(c.created), "2026-10-12T03:04:05Z");
    return c;
}

// ---------------------------------------------------------------------------
// Names, defaults, profiles
// ---------------------------------------------------------------------------

static void test_names(void) {
    CHECK(soulx_name_valid("default"));
    CHECK(soulx_name_valid("a"));
    CHECK(soulx_name_valid("0abc"));
    CHECK(soulx_name_valid("my-game_2"));
    CHECK(soulx_name_valid("abcdefghijklmnopqrstuvwxyz012345"));          // 32 characters
    CHECK(!soulx_name_valid("abcdefghijklmnopqrstuvwxyz0123456"));        // 33 characters
    CHECK(!soulx_name_valid(""));
    CHECK(!soulx_name_valid(NULL));
    CHECK(!soulx_name_valid("-abc"));
    CHECK(!soulx_name_valid("_abc"));
    CHECK(!soulx_name_valid("Abc"));
    CHECK(!soulx_name_valid("a b"));
    CHECK(!soulx_name_valid("a.b"));
    CHECK(!soulx_name_valid("a/b"));
    CHECK(!soulx_name_valid(".."));
    CHECK(!soulx_name_valid("caf\xC3\xA9"));

    char buf[64];
    soulx_avd_name("game", buf, sizeof(buf));
    CHECK_STR(buf, "soulx_game");
    soulx_avd_name("game", buf, 6);                                       // too small: empty, never truncated
    CHECK_STR(buf, "");
}

static void test_defaults_profiles(void) {
    SoulXInstanceConf c;
    soulx_conf_defaults(&c, "default");
    CHECK(c.id == 0);
    CHECK_STR(c.name, "default");
    CHECK_STR(c.image, "play");
    CHECK(c.cores == 4 && c.ram_mb == 4096 && c.data_gb == 8);
    CHECK(c.width == 1080 && c.height == 2400 && c.dpi == 420);
    CHECK(!c.landscape);
    CHECK_STR(c.gpu, "host");
    CHECK_STR(c.dns, "");
    CHECK(c.stay_awake && c.lean);
    CHECK(c.fps == 60 && c.audio);

    soulx_conf_defaults(&c, "abcdefghijklmnopqrstuvwxyz0123456789");     // does not fit: left empty
    CHECK_STR(c.name, "");

    soulx_conf_defaults(&c, "p");
    CHECK(soulx_conf_apply_profile(&c, "lite") == 0);
    CHECK(c.cores == 2 && c.ram_mb == 4096 && c.data_gb == 6 && c.width == 720 && c.height == 1600 && c.dpi == 320);
    CHECK(soulx_conf_apply_profile(&c, "performance") == 0);
    CHECK(c.cores == 6 && c.ram_mb == 6144 && c.data_gb == 16 && c.width == 1080 && c.height == 2400 && c.dpi == 420);
    CHECK(soulx_conf_apply_profile(&c, "balanced") == 0);
    CHECK(c.cores == 4 && c.ram_mb == 4096 && c.data_gb == 8);
    CHECK(soulx_conf_apply_profile(&c, "turbo") == -1);
    CHECK(soulx_conf_apply_profile(&c, NULL) == -1);
    c.landscape = true;                                                  // profiles keep the orientation
    CHECK(soulx_conf_apply_profile(&c, "lite") == 0);
    CHECK(c.width == 1600 && c.height == 720);
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

static void test_registry(void) {
    SoulXInstanceConf out[16];
    SoulXInstanceConf c;

    unsetenv("SOULX_INSTANCE");
    CHECK(soulx_reg_list(out, 16) == 0);
    CHECK(soulx_reg_list(NULL, 0) == 0);
    CHECK(soulx_reg_next_id() == 1);
    CHECK(soulx_resolve_instance(NULL, &c) == -1);

    SoulXInstanceConf a = make_conf(1, "default");
    SoulXInstanceConf b = make_conf(2, "game");
    b.landscape = true;
    b.width = 2400;
    b.height = 1080;
    b.cores = 6;
    b.ram_mb = 6144;
    b.data_gb = 16;
    b.dpi = 400;
    b.stay_awake = false;
    b.lean = false;
    b.fps = 30;
    b.audio = false;
    snprintf(b.gpu, sizeof(b.gpu), "software");
    snprintf(b.dns, sizeof(b.dns), "8.8.8.8,2001:4860:4860::8888");
    snprintf(b.image, sizeof(b.image), "google");
    b.last_port = SOULX_PORT_FIRST + 4;
    SoulXInstanceConf z = make_conf(5, "zeta");
    CHECK(soulx_reg_save(&z) == 0);                                       // saved out of id order
    CHECK(soulx_reg_save(&a) == 0);
    CHECK(soulx_reg_save(&b) == 0);

    int n = soulx_reg_list(out, 16);
    CHECK(n == 3);
    if (n == 3) {
        CHECK(out[0].id == 1 && out[1].id == 2 && out[2].id == 5);
        CHECK_STR(out[2].name, "zeta");
    }
    CHECK(soulx_reg_list(NULL, 0) == 3);
    CHECK(soulx_reg_list(out, 1) == 1 && out[0].id == 1);                 // lowest id first
    CHECK(soulx_reg_next_id() == 6);

    // full round trip
    CHECK(soulx_reg_load("game", &c) == 0);
    CHECK(c.id == 2 && !strcmp(c.name, "game") && !strcmp(c.image, "google"));
    CHECK(c.cores == 6 && c.ram_mb == 6144 && c.data_gb == 16);
    CHECK(c.width == 2400 && c.height == 1080 && c.dpi == 400 && c.landscape);
    CHECK_STR(c.gpu, "software");
    CHECK_STR(c.dns, "8.8.8.8,2001:4860:4860::8888");
    CHECK(!c.stay_awake && !c.lean);
    CHECK(c.fps == 30 && !c.audio);
    CHECK_STR(c.created, "2026-10-12T03:04:05Z");
    CHECK(c.last_port == SOULX_PORT_FIRST + 4);
    CHECK(conf_equal(&c, &b));
    CHECK(soulx_reg_load("default", &c) == 0 && c.last_port == 0);       // never started

    CHECK(soulx_reg_find_by_id(5, &c) == 0 && !strcmp(c.name, "zeta"));
    CHECK(soulx_reg_find_by_id(3, &c) == -1);
    CHECK(soulx_reg_find_by_id(0, &c) == -1);

    // resolution: default, $SOULX_INSTANCE, ids, names
    CHECK(soulx_resolve_instance(NULL, &c) == 0 && !strcmp(c.name, "default"));
    CHECK(soulx_resolve_instance("", &c) == 0 && !strcmp(c.name, "default"));
    setenv("SOULX_INSTANCE", "game", 1);
    CHECK(soulx_resolve_instance(NULL, &c) == 0 && !strcmp(c.name, "game"));
    setenv("SOULX_INSTANCE", "5", 1);
    CHECK(soulx_resolve_instance(NULL, &c) == 0 && !strcmp(c.name, "zeta"));
    unsetenv("SOULX_INSTANCE");
    CHECK(soulx_resolve_instance("2", &c) == 0 && !strcmp(c.name, "game"));
    CHECK(soulx_resolve_instance("zeta", &c) == 0 && c.id == 5);
    CHECK(soulx_resolve_instance("nope", &c) == -1);
    CHECK(soulx_resolve_instance("99", &c) == -1);
    CHECK(soulx_resolve_instance("../instances/game", &c) == -1);
    CHECK(soulx_resolve_instance("99999999999999999999", &c) == -1);

    // invalid saves
    SoulXInstanceConf bad = make_conf(7, "Bad Name");
    CHECK(soulx_reg_save(&bad) == -1);
    bad = make_conf(0, "noid");
    CHECK(soulx_reg_save(&bad) == -1);
    bad = make_conf(7, "inject");
    snprintf(bad.dns, sizeof(bad.dns), "8.8.8.8\nid=1");
    CHECK(soulx_reg_save(&bad) == -1);
    bad = make_conf(7, "inject");
    snprintf(bad.gpu, sizeof(bad.gpu), "vulkan");
    CHECK(soulx_reg_save(&bad) == -1);
    bad = make_conf(7, "inject");
    bad.ram_mb = 0;
    CHECK(soulx_reg_save(&bad) == -1);

    // hand-written file: unknown keys ignored, missing/invalid keys -> defaults, CRLF + blanks trimmed
    char path[PATH_MAX];
    home_path(path, sizeof(path), "instances/hand.conf");
    CHECK(write_text(path, "# written by hand\r\nid=9\r\nname=other\nfoo=bar\ncores=abc\n  ram_mb = 6144  \r\n"
                           "gpu=quantum\nlandscape=yes\ndpi=99999\n\nimage=play16k\nlast_port=5601\nlean=no\n"));
    CHECK(soulx_reg_load("hand", &c) == 0);
    CHECK(c.id == 9 && !strcmp(c.name, "hand"));                         // the file name wins
    CHECK(c.cores == 4 && c.ram_mb == 6144 && c.dpi == 420);
    CHECK_STR(c.gpu, "host");
    CHECK_STR(c.image, "play16k");
    CHECK(c.landscape && c.stay_awake && !c.lean && c.data_gb == 8);
    CHECK(c.last_port == 0);                                             // an adb port, not a console port
    CHECK(soulx_reg_next_id() == 10);
    static const char* const bad_ports[] = { "last_port=5599\n", "last_port=5682\n", "last_port=x\n",
                                             "last_port=-5600\n" };
    for (size_t i = 0; i < sizeof(bad_ports) / sizeof(bad_ports[0]); i++) {
        char body[64];
        snprintf(body, sizeof(body), "id=9\n%s", bad_ports[i]);
        CHECK(write_text(path, body));
        CHECK(soulx_reg_load("hand", &c) == 0 && c.last_port == 0);
    }
    CHECK(write_text(path, "id=9\nlast_port=5680\nlandscape=yes\nimage=play16k\nram_mb=6144\n"));
    CHECK(soulx_reg_load("hand", &c) == 0 && c.last_port == SOULX_PORT_LAST);

    // files without an id, with a bad name, symlinks, temp files: never listed
    home_path(path, sizeof(path), "instances/noid.conf");
    CHECK(write_text(path, "name=noid\ncores=2\n"));
    CHECK(soulx_reg_load("noid", &c) == -1);
    home_path(path, sizeof(path), "instances/Bad.conf");
    CHECK(write_text(path, "id=11\n"));
    home_path(path, sizeof(path), "instances/game.conf.tmp.ABC123");
    CHECK(write_text(path, "id=12\n"));
    char target[PATH_MAX];
    home_path(target, sizeof(target), "instances/game.conf");
    home_path(path, sizeof(path), "instances/link.conf");
    CHECK(symlink(target, path) == 0);
    CHECK(soulx_reg_load("link", &c) == -1);
    CHECK(soulx_reg_list(NULL, 0) == 4);                                  // default, game, zeta, hand

    // delete (idempotent)
    CHECK(soulx_reg_delete("game") == 0);
    CHECK(soulx_reg_load("game", &c) == -1);
    CHECK(soulx_reg_delete("game") == 0);
    CHECK(soulx_reg_delete("../x") == -1);
    CHECK(soulx_reg_list(NULL, 0) == 3);
    const char* strays[] = { "instances/link.conf", "instances/noid.conf", "instances/Bad.conf",
                             "instances/game.conf.tmp.ABC123" };
    for (size_t i = 0; i < sizeof(strays) / sizeof(strays[0]); i++) {
        home_path(path, sizeof(path), strays[i]);
        unlink(path);
    }
    CHECK(soulx_reg_delete("hand") == 0 && soulx_reg_delete("zeta") == 0 && soulx_reg_delete("default") == 0);
    CHECK(soulx_reg_list(NULL, 0) == 0);

    // ids are never handed out twice: the highest saved id is remembered
    CHECK(soulx_reg_next_id() == 6);                                      // 5 (zeta) was the highest saved
    SoulXInstanceConf big = make_conf(41, "big");
    CHECK(soulx_reg_save(&big) == 0 && soulx_reg_delete("big") == 0);
    CHECK(soulx_reg_next_id() == 42);
    home_path(path, sizeof(path), "instances/.last_id");
    CHECK(write_text(path, "last_id=garbage\n"));                        // unreadable counter: existing ids rule
    CHECK(soulx_reg_next_id() == 1);
    unlink(path);
}

// A conf file without an id is reported once per process, not on every registry scan.
static void test_warn_once(void) {
    char path[PATH_MAX];
    home_path(path, sizeof(path), "instances/noid2.conf");
    CHECK(write_text(path, "name=noid2\n"));
    logs_start();
    for (int i = 0; i < 4; i++) CHECK(soulx_reg_list(NULL, 0) == 0);
    CHECK(logs_count("has no valid id") == 1);
    struct timespec later[2] = { { time(NULL) + 5, 0 }, { time(NULL) + 5, 0 } };
    CHECK(utimensat(AT_FDCWD, path, later, 0) == 0);                     // changed since: reported again
    CHECK(soulx_reg_list(NULL, 0) == 0);
    CHECK(logs_count("has no valid id") == 2);
    logs_stop();
    unlink(path);
}

// ---------------------------------------------------------------------------
// Runtime state + lock
// ---------------------------------------------------------------------------

static pid_t dead_pid(void) {
    pid_t pid = fork();
    if (pid == 0) _exit(0);
    int status;
    waitpid(pid, &status, 0);
    return pid;
}

static void test_state(void) {
    SoulXRunState st, back;
    memset(&st, 0, sizeof(st));
    st.pid = 4242;
    st.port = SOULX_PORT_FIRST + 2;
    st.headless = true;
    snprintf(st.started, sizeof(st.started), "2026-10-12T03:04:05Z");
    CHECK(soulx_state_save("alpha", &st) == 0);
    CHECK(soulx_state_load("alpha", &back) == 0);
    CHECK(back.pid == 4242 && back.port == SOULX_PORT_FIRST + 2 && back.headless && !strcmp(back.started, st.started));
    CHECK(back.network_capture_path[0] == '\0');                                      // not recording
    snprintf(st.network_capture_path, sizeof(st.network_capture_path), "/Users/x/Documents/SoulX Network Captures/a \"b\"/2026-10-12.pcap");
    CHECK(soulx_state_save("alpha", &st) == 0 && soulx_state_load("alpha", &back) == 0);
    CHECK_STR(back.network_capture_path, st.network_capture_path);                               // the capture in progress survives
    snprintf(st.network_capture_path, sizeof(st.network_capture_path), "/tmp/two\nlines.pcap");
    CHECK(soulx_state_save("alpha", &st) == -1);                           // would corrupt the file
    st.network_capture_path[0] = '\0';
    CHECK(soulx_state_clear("alpha") == 0);
    CHECK(soulx_state_load("alpha", &back) == -1);
    CHECK(soulx_state_clear("alpha") == 0);                               // already gone
    CHECK(soulx_state_save("Bad", &st) == -1);
    st.pid = 0;
    CHECK(soulx_state_save("alpha", &st) == -1);

    // a dead pid and a live non-engine pid are both stale: cleared
    st.pid = dead_pid();
    CHECK(soulx_state_save("alpha", &st) == 0);
    CHECK(!soulx_instance_running("alpha", &back));
    CHECK(back.pid == 0);
    CHECK(soulx_state_load("alpha", &back) == -1);
    st.pid = getpid();
    CHECK(soulx_state_save("alpha", &st) == 0);
    CHECK(!soulx_instance_running("alpha", NULL));
    CHECK(soulx_state_load("alpha", &back) == -1);

    // corrupt state file is dropped
    char path[PATH_MAX];
    home_path(path, sizeof(path), "run/alpha.state");
    CHECK(write_text(path, "garbage\n"));
    CHECK(!soulx_instance_running("alpha", NULL));
    CHECK(!file_exists(path));

    int fd = soulx_lock_acquire();
    CHECK(fd >= 0);
    soulx_lock_release(fd);
    fd = soulx_lock_acquire();                                            // re-acquirable after release
    CHECK(fd >= 0);
    soulx_lock_release(fd);
    soulx_lock_release(-1);                                               // harmless
}

// ---------------------------------------------------------------------------
// AVD files
// ---------------------------------------------------------------------------

static void test_avd(void) {
    SoulXInstanceConf c = make_conf(3, "game");
    char ini_path[PATH_MAX], cfg_path[PATH_MAX], avd_dir[PATH_MAX], line[PATH_MAX + 16];
    home_path(ini_path, sizeof(ini_path), "avd/soulx_game.ini");
    home_path(cfg_path, sizeof(cfg_path), "avd/soulx_game.avd/config.ini");
    home_path(avd_dir, sizeof(avd_dir), "avd/soulx_game.avd");

    CHECK(soulx_avd_write(&c) == 0);
    char* ini = read_text(ini_path);
    char* cfg = read_text(cfg_path);
    CHECK(ini && cfg);
    if (ini && cfg) {
        CHECK(has_line(ini, "avd.ini.encoding=UTF-8"));
        snprintf(line, sizeof(line), "path=%s", avd_dir);
        CHECK(has_line(ini, line));
        CHECK(has_line(ini, "path.rel=avd/soulx_game.avd"));
        CHECK(has_line(ini, "target=android-37.0"));

        CHECK(has_line(cfg, "AvdId=soulx_game"));
        CHECK(has_line(cfg, "avd.ini.displayname=SoulX game"));
        CHECK(has_line(cfg, "target=android-37.0"));
        CHECK(has_line(cfg, "image.sysdir.1=system-images/android-37.0/google_apis_playstore/arm64-v8a/"));
        CHECK(has_line(cfg, "tag.id=google_apis_playstore"));
        CHECK(has_line(cfg, "tag.display=Google Play"));
        CHECK(has_line(cfg, "abi.type=arm64-v8a"));
        CHECK(has_line(cfg, "hw.cpu.arch=arm64"));
        CHECK(has_line(cfg, "hw.cpu.ncore=4"));
        CHECK(has_line(cfg, "hw.ramSize=4096"));
        CHECK(has_line(cfg, "disk.dataPartition.size=8G"));
        CHECK(has_line(cfg, "PlayStore.enabled=yes"));
        CHECK(has_line(cfg, "hw.gpu.enabled=yes"));
        CHECK(has_line(cfg, "hw.gpu.mode=host"));
        CHECK(has_line(cfg, "hw.lcd.width=1080"));
        CHECK(has_line(cfg, "hw.lcd.height=2400"));
        CHECK(has_line(cfg, "hw.lcd.density=420"));
        CHECK(has_line(cfg, "hw.lcd.vsync=60"));
        CHECK(has_line(cfg, "hw.audioOutput=yes"));
        CHECK(has_line(cfg, "skin.name=1080x2400"));
        CHECK(has_line(cfg, "skin.path=1080x2400"));
        CHECK(has_line(cfg, "hw.initialOrientation=portrait"));
        CHECK(has_line(cfg, "hw.keyboard=yes"));
        CHECK(has_line(cfg, "fastboot.forceColdBoot=no"));
    }
    free(ini);
    free(cfg);

    // unchanged content is not rewritten (keeps quick-boot happy)
    struct stat before, after;
    CHECK(stat(cfg_path, &before) == 0);
    CHECK(soulx_avd_write(&c) == 0);
    CHECK(stat(cfg_path, &after) == 0);
    CHECK(before.st_ino == after.st_ino);

    // rootable image, software GPU, landscape panel
    snprintf(c.image, sizeof(c.image), "google");
    snprintf(c.gpu, sizeof(c.gpu), "software");
    c.landscape = true;
    c.width = 2400;
    c.height = 1080;
    c.cores = 6;
    c.ram_mb = 6144;
    c.data_gb = 16;
    c.fps = 45;
    c.audio = false;
    CHECK(soulx_avd_write(&c) == 0);
    cfg = read_text(cfg_path);
    CHECK(cfg != NULL);
    if (cfg) {
        CHECK(has_line(cfg, "hw.lcd.vsync=45"));
        CHECK(has_line(cfg, "hw.audioOutput=no"));
        CHECK(has_line(cfg, "image.sysdir.1=system-images/android-37.0/google_apis/arm64-v8a/"));
        CHECK(has_line(cfg, "tag.id=google_apis"));
        CHECK(has_line(cfg, "PlayStore.enabled=no"));
        CHECK(has_line(cfg, "hw.gpu.mode=swiftshader_indirect"));
        CHECK(has_line(cfg, "hw.lcd.width=2400"));
        CHECK(has_line(cfg, "hw.lcd.height=1080"));
        CHECK(has_line(cfg, "skin.name=2400x1080"));
        CHECK(has_line(cfg, "hw.initialOrientation=landscape"));
        CHECK(has_line(cfg, "hw.cpu.ncore=6"));
        CHECK(has_line(cfg, "hw.ramSize=6144"));
        CHECK(has_line(cfg, "disk.dataPartition.size=16G"));
    }
    free(cfg);

    // 16 KB-page QPR1 image
    snprintf(c.image, sizeof(c.image), "play16k");
    CHECK(soulx_avd_write(&c) == 0);
    cfg = read_text(cfg_path);
    ini = read_text(ini_path);
    CHECK(cfg && ini);
    if (cfg && ini) {
        CHECK(has_line(ini, "target=android-37.1"));
        CHECK(has_line(cfg, "target=android-37.1"));
        CHECK(has_line(cfg, "PlayStore.enabled=yes"));
    }
    free(cfg);
    free(ini);

    snprintf(c.image, sizeof(c.image), "nope");
    CHECK(soulx_avd_write(&c) == -1);
    snprintf(c.image, sizeof(c.image), "emulator");                      // not a system image
    CHECK(soulx_avd_write(&c) == -1);

    CHECK(soulx_avd_remove(&c) == 0);
    CHECK(!file_exists(ini_path) && !file_exists(avd_dir));
    CHECK(soulx_avd_remove(&c) == 0);                                     // already gone
}

// ---------------------------------------------------------------------------
// Engine: creation and the public API without a running engine
// ---------------------------------------------------------------------------

static void test_engine_offline(void) {
    SoulXInstanceConf c;
    char counter[PATH_MAX];
    home_path(counter, sizeof(counter), "instances/.last_id");
    unlink(counter);                                                     // the ids below assume a fresh registry

    // strict validation of soulx_mac_create
    soulx_conf_defaults(&c, "v");
    c.cores = 9;
    CHECK(soulx_mac_create(&c) == -1);
    soulx_conf_defaults(&c, "v");
    c.ram_mb = 2048;
    CHECK(soulx_mac_create(&c) == -1);
    soulx_conf_defaults(&c, "v");
    c.data_gb = 65;
    CHECK(soulx_mac_create(&c) == -1);
    soulx_conf_defaults(&c, "v");
    c.width = 100;
    CHECK(soulx_mac_create(&c) == -1);
    soulx_conf_defaults(&c, "v");
    c.dpi = 700;
    CHECK(soulx_mac_create(&c) == -1);
    soulx_conf_defaults(&c, "v");
    snprintf(c.image, sizeof(c.image), "android-14");
    CHECK(soulx_mac_create(&c) == -1);
    soulx_conf_defaults(&c, "v");
    snprintf(c.dns, sizeof(c.dns), "8.8.8.8;rm");
    CHECK(soulx_mac_create(&c) == -1);
    soulx_conf_defaults(&c, "v");
    snprintf(c.gpu, sizeof(c.gpu), "directx");
    CHECK(soulx_mac_create(&c) == -1);
    soulx_conf_defaults(&c, "123");                                        // reserved for ids
    CHECK(soulx_mac_create(&c) == -1);
    soulx_conf_defaults(&c, "Bad");
    CHECK(soulx_mac_create(&c) == -1);
    soulx_conf_defaults(&c, "v");
    c.fps = 61;                                                           // above the host display
    CHECK(soulx_mac_create(&c) == -1);
    soulx_conf_defaults(&c, "v");
    c.fps = 19;
    CHECK(soulx_mac_create(&c) == -1);
    CHECK(soulx_reg_list(NULL, 0) == 0);

    // a valid create: id, timestamp, registry + AVD; duplicates refused
    soulx_conf_defaults(&c, "first");
    c.landscape = true;                                                   // portrait numbers are swapped
    CHECK(soulx_mac_create(&c) == 0);
    CHECK(c.id == 1 && c.created[0] && c.width == 2400 && c.height == 1080);
    char path[PATH_MAX];
    home_path(path, sizeof(path), "avd/soulx_first.avd/config.ini");
    CHECK(file_exists(path));
    SoulXInstanceConf dup;
    soulx_conf_defaults(&dup, "first");
    CHECK(soulx_mac_create(&dup) == -1);

    // public API: slugified names, clamped values, profile mapping, image mapping
    InstanceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.name, sizeof(cfg.name), "My Game #1!");
    snprintf(cfg.android_version, sizeof(cfg.android_version), "37.1");
    cfg.cpu_cores = 32;                                                   // clamped to 8
    cfg.ram_mb = DEFAULT_RAM_MB;                                          // 2048: raised to the 4096 floor
    cfg.width = 1920;
    cfg.height = 1080;
    cfg.dpi = 240;
    cfg.profile = PERF_PROFILE_CUSTOM;
    int id = soulx_create_instance(&cfg);
    CHECK(id == 2);
    CHECK(soulx_find_instance("my-game-1") == id);
    CHECK(soulx_find_instance("My Game #1!") == id);
    CHECK(soulx_find_instance("2") == id);
    CHECK(soulx_find_instance("missing") == SOULX_ERR_NOT_FOUND);
    CHECK(soulx_find_instance(NULL) == SOULX_ERR_NOT_FOUND);
    CHECK(soulx_reg_find_by_id(id, &c) == 0);
    CHECK_STR(c.name, "my-game-1");
    CHECK_STR(c.image, "play16k");
    CHECK(c.cores == 8 && c.ram_mb == 4096 && c.width == 1920 && c.height == 1080 && c.dpi == 240 && c.landscape);
    CHECK(c.data_gb == 8);                                                // storage_gb 0 keeps the default

    InstanceConfig got;
    CHECK(soulx_get_config(id, &got) == SOULX_OK);
    CHECK_STR(got.name, "my-game-1");
    CHECK_STR(got.android_version, "37.1");
    CHECK(got.cpu_cores == 8 && got.ram_mb == 4096 && got.storage_gb == 8);
    CHECK(got.width == 1920 && got.height == 1080 && got.dpi == 240);
    CHECK(got.profile == PERF_PROFILE_CUSTOM && !got.enable_root && got.graphics == GRAPHICS_BACKEND_OPENGL);

    // set_config on a stopped instance: saved + AVD rewritten; data never shrinks
    got.enable_root = true;
    got.storage_gb = 4;
    got.graphics = GRAPHICS_BACKEND_SOFTWARE;
    CHECK(soulx_set_config(id, &got) == SOULX_OK);
    CHECK(soulx_reg_find_by_id(id, &c) == 0);
    CHECK_STR(c.image, "google");
    CHECK_STR(c.gpu, "software");
    CHECK(c.data_gb == 8);
    home_path(path, sizeof(path), "avd/soulx_my-game-1.avd/config.ini");
    char* text = read_text(path);
    CHECK(text && has_line(text, "tag.id=google_apis") && has_line(text, "hw.gpu.mode=swiftshader_indirect"));
    free(text);
    InstanceConfig again;
    CHECK(soulx_get_config(id, &again) == SOULX_OK && again.enable_root && again.graphics == GRAPHICS_BACKEND_SOFTWARE);
    CHECK(soulx_set_config(id, &again) == SOULX_OK);                         // round trip is stable
    SoulXInstanceConf same;
    CHECK(soulx_reg_find_by_id(id, &same) == 0 && conf_equal(&same, &c));

    // performance profile keeps the landscape panel
    CHECK(soulx_set_performance_profile(id, PERF_PROFILE_ECO) == SOULX_OK);
    CHECK(soulx_reg_find_by_id(id, &c) == 0);
    CHECK(c.cores == 2 && c.ram_mb == 4096 && c.width == 1600 && c.height == 720 && c.dpi == 320 && c.data_gb == 8);
    CHECK(soulx_get_config(id, &got) == SOULX_OK && got.profile == PERF_PROFILE_CUSTOM);   // data 8 != lite's 6
    CHECK(soulx_set_performance_profile(999, PERF_PROFILE_ECO) == SOULX_ERR_NOT_FOUND);

    // clone: configuration only, new id
    int clone = soulx_clone_instance(id, "Copy");
    CHECK(clone == 3);
    CHECK(soulx_reg_find_by_id(clone, &c) == 0 && !strcmp(c.name, "copy") && !strcmp(c.image, "google"));
    CHECK(soulx_clone_instance(id, "copy") == SOULX_ERROR);                  // exists
    CHECK(soulx_clone_instance(999, "x") == SOULX_ERR_NOT_FOUND);

    // listing: all stopped, ids in order
    CHECK(soulx_get_instance_count() == 3);
    InstanceInfo infos[8];
    CHECK(soulx_list_instances(infos, 8) == 3);
    CHECK(infos[0].id == 1 && infos[1].id == 2 && infos[2].id == 3);
    CHECK(infos[1].state == INSTANCE_STATE_STOPPED && infos[1].pid == 0 && infos[1].serial[0] == '\0');
    CHECK(soulx_list_instances(infos, 2) == 2);
    CHECK(soulx_list_instances(NULL, 2) == SOULX_ERROR);
    InstanceInfo info;
    CHECK(soulx_get_instance_info(2, &info) == SOULX_OK && info.state == INSTANCE_STATE_STOPPED);
    CHECK_STR(info.config.name, "my-game-1");
    CHECK(soulx_get_instance_info(42, &info) == SOULX_ERR_NOT_FOUND);

    // operations that need a running engine
    CHECK(soulx_pause_instance(2) == SOULX_ERR_NOT_RUNNING);
    CHECK(soulx_wait_boot(2, 1) == SOULX_ERR_NOT_RUNNING);
    CHECK(soulx_stop_instance(2) == SOULX_OK);                               // already stopped
    CHECK(soulx_mac_require_running("my-game-1", NULL, NULL, NULL, 0) == SOULX_ERR_NOT_RUNNING);
    CHECK(soulx_mac_require_running("ghost", NULL, NULL, NULL, 0) == SOULX_ERR_NOT_FOUND);

    // delete through both entry points
    CHECK(soulx_delete_instance(clone) == SOULX_OK);
    CHECK(soulx_delete_instance(clone) == SOULX_ERR_NOT_FOUND);
    CHECK(soulx_mac_delete("my-game-1") == 0);
    CHECK(!file_exists(path));
    CHECK(soulx_mac_delete("first") == 0);
    CHECK(soulx_mac_delete("first") == -1);
    CHECK(soulx_get_instance_count() == 0);
}

// ---------------------------------------------------------------------------
// Engine lifecycle against a fake runtime (copies of this binary)
// ---------------------------------------------------------------------------

static bool copy_self(const char* dst) {
    char self[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(getpid(), self, sizeof(self)) <= 0) return false;
    int in = open(self, O_RDONLY);
    if (in < 0) return false;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) {
        close(in);
        return false;
    }
    char buf[65536];
    ssize_t r;
    bool ok = true;
    while ((r = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)r) != r) {
            ok = false;
            break;
        }
    }
    close(in);
    return close(out) == 0 && ok && r == 0;
}

// The engine's ANGLE as far as SoulX cares: one ANGLE_DEFAULT_PLATFORM variable name (gpu=metal renames it).
#define FAKE_ANGLE_REL      "sdk/emulator/lib64/gles_angle/libGLESv2.dylib"
static const char k_fake_angle[] = "fake ANGLE\0ANGLE_DEFAULT_PLATFORM\0metal vulkan gl\0";

static bool install_fake_angle(void) {
    char path[PATH_MAX];
    home_path(path, sizeof(path), "sdk/emulator/lib64/gles_angle");
    if (soulx_mkdirs(path) != 0) return false;
    home_path(path, sizeof(path), FAKE_ANGLE_REL);
    return soulx_write_file_atomic(path, k_fake_angle, sizeof(k_fake_angle)) == 0;
}

static bool install_fake_runtime(void) {
    const char* dirs[] = {
        "sdk/.soulx", "sdk/emulator/qemu/darwin-aarch64", "sdk/platform-tools",
        "sdk/system-images/android-37.0/google_apis_playstore/arm64-v8a",
    };
    char path[PATH_MAX];
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        home_path(path, sizeof(path), dirs[i]);
        if (soulx_mkdirs(path) != 0) return false;
    }
    home_path(path, sizeof(path), "sdk/emulator/emulator");
    if (!copy_self(path)) return false;
    home_path(path, sizeof(path), "sdk/emulator/qemu/darwin-aarch64/qemu-system-aarch64");
    if (!copy_self(path)) return false;
    home_path(path, sizeof(path), "sdk/emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless");
    if (!copy_self(path)) return false;
    home_path(path, sizeof(path), "sdk/platform-tools/adb");
    if (!copy_self(path)) return false;
    if (!install_fake_angle()) return false;
    static const char* const image_files[] = { "system.img", "vendor.img", "ramdisk.img", "kernel-ranchu",
                                               "source.properties" };
    for (size_t i = 0; i < sizeof(image_files) / sizeof(image_files[0]); i++) {
        char rel[160];
        snprintf(rel, sizeof(rel), "sdk/system-images/android-37.0/google_apis_playstore/arm64-v8a/%s", image_files[i]);
        home_path(path, sizeof(path), rel);
        if (!write_text(path, "fake\n")) return false;
    }

    const char* keys[] = { "emulator", "platform-tools", "play" };
    for (size_t i = 0; i < 3; i++) {
        const SoulXPackage* p = soulx_runtime_find(keys[i]);
        if (!p) return false;
        char rel[64], body[256];
        snprintf(rel, sizeof(rel), "sdk/.soulx/%s.installed", keys[i]);
        snprintf(body, sizeof(body), "sha1=%s\nversion=%s\n", p->sha1, p->version);
        home_path(path, sizeof(path), rel);
        if (!write_text(path, body)) return false;
    }
    return soulx_runtime_ready("play");
}

static bool port_listening(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return true;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool up = connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0;
    close(fd);
    return up;
}

// Never leave a fake engine behind, even when a check failed half-way.
static void kill_leftover(pid_t pid) {
    if (pid > 0 && soulx_pid_is_engine(pid)) {
        kill(pid, SIGKILL);
        soulx_wait_pid_exit(pid, 2000);
    }
}

// How many times the fake adb ran with exactly the arguments |args_line| (e.g. "kill-server").
static int adb_calls(const char* args_line) {
    char path[PATH_MAX];
    home_path(path, sizeof(path), "tmp/fake-adb.log");
    char* text = read_text(path);
    int n = text ? count_lines(text, args_line) : 0;
    free(text);
    return n;
}

// How many times the fake adb ran with arguments that start with |prefix|.
static int adb_calls_starting(const char* prefix) {
    char path[PATH_MAX];
    home_path(path, sizeof(path), "tmp/fake-adb.log");
    char* text = read_text(path);
    int n = 0;
    size_t len = strlen(prefix);
    for (const char* p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (strncmp(p, prefix, len) == 0) n++;
    }
    free(text);
    return n;
}

static void lifecycle(const char* name, bool child_launcher) {
    if (child_launcher) setenv("SOULX_TEST_LAUNCHER_CHILD", "1", 1);
    else unsetenv("SOULX_TEST_LAUNCHER_CHILD");

    SoulXInstanceConf c;
    soulx_conf_defaults(&c, name);
    snprintf(c.dns, sizeof(c.dns), "1.1.1.1");
    c.lean = !child_launcher;                                            // the second run covers lean=0
    CHECK(soulx_mac_create(&c) == 0);

    SoulXStartOpts o;
    memset(&o, 0, sizeof(o));
    o.wait = true;
    o.headless = true;
    o.timeout_s = 20;
    SoulXRunState st;
    CHECK(soulx_mac_start(name, &o, &st) == 0);
    CHECK(st.pid > 0 && st.port >= SOULX_PORT_FIRST && st.port <= SOULX_PORT_LAST && st.port % 2 == 0 && st.headless);

    // the tracked pid is the qemu process, whatever the launcher did; with -no-window the launcher runs
    // the headless engine binary
    char exe[PROC_PIDPATHINFO_MAXSIZE] = "";
    proc_pidpath(st.pid, exe, sizeof(exe));
    const char* base = strrchr(exe, '/');
    CHECK(base && !strcmp(base + 1, "qemu-system-aarch64-headless"));
    SoulXRunState now;
    CHECK(soulx_instance_running(name, &now) && now.pid == st.pid && now.port == st.port);

    // the engine got the expected command line, SoulX's own adb, and $SOULX_HOME/tmp as working directory;
    // it never shows a window of its own and shares its frames for SoulX's window, and never uses Quick Boot
    char log_path[PATH_MAX], rel[64], want[PATH_MAX + 64];
    snprintf(rel, sizeof(rel), "logs/%s.log", name);
    home_path(log_path, sizeof(log_path), rel);
    char* log = read_text(log_path);
    CHECK(log && strstr(log, "-avd soulx_") && strstr(log, " -no-window") && strstr(log, " -dns-server 1.1.1.1") &&
          strstr(log, " -gpu host ") && !strstr(log, " -feature -Vulkan") && strstr(log, "angle: vulkan\n") &&
          strstr(log, " -no-snapshot-load -no-snapshot-save") &&
          strstr(log, " -no-metrics") && strstr(log, " -adb-path /") && strstr(log, " -share-vid"));
    snprintf(want, sizeof(want), " -avd soulx_%s -port %d ", name, st.port);
    CHECK(log && strstr(log, want));
    snprintf(want, sizeof(want), " -adb-path %s/sdk/platform-tools/adb", g_home);
    CHECK(log && strstr(log, want));
    snprintf(want, sizeof(want), "cwd: %s/tmp\n", g_home);
    CHECK(log && strstr(log, want));
    free(log);

    // post-boot ran every tweak in a single adb shell call, and that is recorded for this engine run: lean
    // disables the apps once per guest (marker in the guest's settings) and caps cached apps on every boot;
    // lean=0 enables them again only on a guest that has the marker
    char adb_log[PATH_MAX];
    home_path(adb_log, sizeof(adb_log), "tmp/fake-adb.log");
    char* calls = read_text(adb_log);
    CHECK(calls && strstr(calls, "shell settings put global private_dns_mode off; svc power stayon true; if [ "));
    CHECK(calls && !strstr(calls, "disable_window_blurs"));            // only gpu=metal turns blurs off
    CHECK(calls && strstr(calls, "if [ \"$(settings get global soulx_nav)\" != 1 ]; then cmd overlay enable-exclusive "
                                 "--category com.android.internal.systemui.navbar.threebutton >/dev/null 2>&1; "
                                 "settings put global soulx_nav 1; fi; if [ "));            // three buttons, once
    if (c.lean) {
        CHECK(calls && strstr(calls, "if [ \"$(settings get global soulx_lean)\" != 1 ]; then for p in "
                                     "com.google.android.youtube com.google.android.apps.youtube.music "));
        CHECK(calls && strstr(calls, " com.google.android.glasses.core; do pm disable-user --user 0 $p; done "
                                     ">/dev/null 2>&1; settings put global soulx_lean 1; fi; device_config put "
                                     "activity_manager max_cached_processes 16\n"));
    } else {
        CHECK(calls && strstr(calls, "if [ \"$(settings get global soulx_lean)\" != null ]; then for p in "
                                     "com.google.android.youtube "));
        CHECK(calls && strstr(calls, "; do pm enable --user 0 $p; done >/dev/null 2>&1; settings delete global "
                                     "soulx_lean >/dev/null; device_config delete activity_manager "
                                     "max_cached_processes >/dev/null; fi\n"));
    }
    free(calls);
    char marker[PATH_MAX], pid_line[32];
    snprintf(rel, sizeof(rel), "run/%s.postboot", name);
    home_path(marker, sizeof(marker), rel);
    char* mark = read_text(marker);
    snprintf(pid_line, sizeof(pid_line), "%d\n", (int)st.pid);
    CHECK(mark && !strcmp(mark, pid_line));
    free(mark);

    // a second start is a no-op; a lost pid is repaired from the command line
    SoulXRunState st2;
    o.wait = false;
    CHECK(soulx_mac_start(name, &o, &st2) == 0 && st2.pid == st.pid);
    SoulXRunState lost = st;
    lost.pid = dead_pid();
    CHECK(soulx_state_save(name, &lost) == 0);
    CHECK(soulx_instance_running(name, &now) && now.pid == st.pid);
    CHECK(soulx_state_load(name, &now) == 0 && now.pid == st.pid);

    char serial[32];
    SoulXInstanceConf rc_conf;
    CHECK(soulx_mac_require_running(name, &rc_conf, &now, serial, sizeof(serial)) == 0);
    char expect[32];
    snprintf(expect, sizeof(expect), "emulator-%d", st.port);
    CHECK_STR(serial, expect);
    CHECK(soulx_mac_delete(name) == -1);                                  // refused while running
    CHECK(soulx_avd_write(&rc_conf) == -1);                               // config.ini frozen while running

    InstanceInfo info;
    CHECK(soulx_get_instance_info(rc_conf.id, &info) == SOULX_OK);
    CHECK(info.state == INSTANCE_STATE_RUNNING && info.pid == (uint64_t)st.pid);
    CHECK(info.console_port == st.port && info.adb_port == st.port + 1 && !strcmp(info.serial, expect));
    CHECK(soulx_wait_boot(rc_conf.id, 5) == SOULX_OK);

    // stop: graceful when nothing else owns the console port (fake engine has no console:
    // SIGTERM after the short no-console wait), forced otherwise
    bool graceful = !port_listening(st.port);
    int kills = adb_calls("kill-server");
    double t0 = soulx_now_ms();
    CHECK(soulx_mac_stop(name, !graceful || child_launcher) == 0);
    CHECK(soulx_now_ms() - t0 < 15000);
    CHECK(!soulx_instance_running(name, NULL));
    CHECK(soulx_state_load(name, &now) == -1);
    CHECK(adb_calls("kill-server") == kills + 1);                        // last instance: SoulX's adb server goes too
    CHECK(!file_exists(marker));                                         // the run's markers go with its state
    CHECK(!soulx_pid_alive(st.pid));                                      // gone when the stop returns
    CHECK(soulx_mac_stop(name, false) == 0);                              // already stopped ...
    CHECK(adb_calls("kill-server") == kills + 2);                        // ... and nothing runs: adb goes
    CHECK(soulx_mac_delete(name) == 0);
    kill_leftover(st.pid);
}

// Stops |name| the hard way and makes sure no fake engine survives, whatever happened before.
static void stop_and_reap(const char* name) {
    SoulXRunState st;
    if (soulx_instance_running(name, &st)) {
        soulx_mac_stop(name, true);
        kill_leftover(st.pid);
    }
}

// The engine command line and ANGLE's platform follow the stored gpu setting; gpu=metal prepares the
// engine's ANGLE first and falls back to gpu=host when that ANGLE is not the build SoulX knows.
static void test_engine_gpu_args(void) {
    static const struct {
        const char* gpu; bool angle_ok; const char* mode; bool no_vulkan; const char* angle; bool fresh;
    } cases[] = {
        { "metal",         true,  " -gpu swangle ",              true,  "angle: metal\n",  true },
        { "host",          true,  " -gpu host ",                 false, "angle: vulkan\n", true },
        { "host-novulkan", true,  " -gpu host ",                 true,  "angle: vulkan\n", true },
        { "software",      true,  " -gpu swiftshader_indirect ", false, "angle: vulkan\n", true },
        { "metal",         false, " -gpu host ",                 false, "angle: vulkan\n", true },
    };
    char lib[PATH_MAX], orig[PATH_MAX];
    home_path(lib, sizeof(lib), FAKE_ANGLE_REL);
    home_path(orig, sizeof(orig), FAKE_ANGLE_REL ".google");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char name[16];
        snprintf(name, sizeof(name), "gpu%zu", i);
        SoulXInstanceConf c;
        soulx_conf_defaults(&c, name);
        snprintf(c.gpu, sizeof(c.gpu), "%s", cases[i].gpu);
        c.audio = i % 2 == 0;                                            // -no-audio for every other case
        CHECK(soulx_mac_create(&c) == 0);
        if (!cases[i].angle_ok) {                                        // an engine update brought another ANGLE
            static const char other[] = "another ANGLE build\0";
            CHECK(soulx_write_file_atomic(lib, other, sizeof(other)) == 0);
        }

        SoulXStartOpts o;
        memset(&o, 0, sizeof(o));
        o.headless = true;
        SoulXRunState st;
        CHECK(soulx_mac_start(name, &o, &st) == 0);

        char log_path[PATH_MAX], rel[64];
        snprintf(rel, sizeof(rel), "logs/%s.log", name);
        home_path(log_path, sizeof(log_path), rel);
        char* log = read_text(log_path);
        CHECK(log && strstr(log, cases[i].mode));                        // the one and only -gpu option
        CHECK(log && (strstr(log, " -feature -Vulkan") != NULL) == cases[i].no_vulkan);
        CHECK(log && strstr(log, cases[i].angle));
        CHECK(log && (strstr(log, " -no-snapshot-load -no-snapshot-save") != NULL) == cases[i].fresh);   // never Quick Boot
        CHECK(log && !strstr(log, " -no-snapshot "));                    // that one leaves the screen black
        CHECK(log && (strstr(log, " -no-audio") != NULL) == (i % 2 != 0));
        free(log);

        stop_and_reap(name);
        CHECK(soulx_mac_delete(name) == 0);
    }

    // the first metal start left SoulX's ANGLE in place (variable renamed, same size) and kept Google's
    CHECK(install_fake_angle() && soulx_runtime_prepare_angle() == 0);
    char* data = NULL;
    size_t len = 0;
    CHECK(soulx_read_file(lib, &data, &len) == 0 && len == sizeof(k_fake_angle));
    CHECK(data && memmem(data, len, SOULX_ANGLE_PLATFORM_ENV "\0", sizeof(SOULX_ANGLE_PLATFORM_ENV)) &&
          !memmem(data, len, "ANGLE_DEFAULT_PLATFORM", 22) && memmem(data, len, "metal vulkan gl", 15));
    free(data);
    data = NULL;
    CHECK(soulx_read_file(orig, &data, &len) == 0 && len == sizeof(k_fake_angle) &&
          memcmp(data, k_fake_angle, len) == 0);
    free(data);
    struct stat a, b;
    CHECK(stat(lib, &a) == 0 && soulx_runtime_prepare_angle() == 0 && stat(lib, &b) == 0 &&   // idempotent
          a.st_ino == b.st_ino && a.st_mtime == b.st_mtime);
    CHECK(unlink(lib) == 0 && soulx_runtime_prepare_angle() == -1);       // no ANGLE at all
    CHECK(install_fake_angle());
}

// Console "kill" is answered, then the engine needs a few seconds (the snapshot save) to exit: the stop
// has to wait for that exit instead of falling back to SIGTERM after the short no-console wait.
static void test_graceful_console_stop(void) {
    SoulXInstanceConf c;
    soulx_conf_defaults(&c, "grace");
    CHECK(soulx_mac_create(&c) == 0);

    SoulXStartOpts o;
    memset(&o, 0, sizeof(o));
    o.headless = true;
    SoulXRunState st;
    setenv("SOULX_TEST_FAKE_CONSOLE_MS", "3000", 1);                      // the fake engine serves the console
    int rc = soulx_mac_start("grace", &o, &st);
    unsetenv("SOULX_TEST_FAKE_CONSOLE_MS");
    CHECK(rc == 0);
    if (rc != 0) {
        soulx_mac_delete("grace");
        return;
    }
    bool up = false;
    for (int i = 0; i < 100 && !up; i++) {
        up = port_listening(st.port);
        if (!up) soulx_sleep_ms(30);
    }
    CHECK(up);

    int kills = adb_calls("kill-server");
    double t0 = soulx_now_ms();
    CHECK(soulx_mac_stop("grace", false) == 0);
    double took = soulx_now_ms() - t0;
    CHECK(took >= 2500 && took < 20000);                                 // waited for the exit, no early SIGTERM

    char log_path[PATH_MAX];
    home_path(log_path, sizeof(log_path), "logs/grace.log");
    char* log = read_text(log_path);
    CHECK(log && strstr(log, "fake engine: console kill received"));
    CHECK(log && strstr(log, "fake engine: exiting after the console kill"));
    CHECK(log && !strstr(log, "SIGTERM"));
    free(log);
    CHECK(!soulx_instance_running("grace", NULL));
    SoulXRunState gone;
    CHECK(soulx_state_load("grace", &gone) == -1);
    CHECK(adb_calls("kill-server") == kills + 1);

    int fd = soulx_lock_acquire();                                        // the lock is free again after the stop
    CHECK(fd >= 0);
    soulx_lock_release(fd);
    kill_leftover(st.pid);
    CHECK(soulx_mac_delete("grace") == 0);
}

// SoulX's private adb server (the engine starts it, nothing else stops it) must stay up as long as any
// SoulX instance runs and go down with the last one.
static void test_adb_server_follows_last_instance(void) {
    const char* names[2] = { "duo-a", "duo-b" };
    SoulXRunState st[2];
    SoulXStartOpts o;
    memset(&o, 0, sizeof(o));
    o.headless = true;
    for (int i = 0; i < 2; i++) {
        SoulXInstanceConf c;
        soulx_conf_defaults(&c, names[i]);
        CHECK(soulx_mac_create(&c) == 0);
        memset(&st[i], 0, sizeof(st[i]));
        CHECK(soulx_mac_start(names[i], &o, &st[i]) == 0);
        CHECK(st[i].port >= SOULX_PORT_FIRST && st[i].port <= SOULX_PORT_LAST && st[i].port % 2 == 0);
    }
    CHECK(st[0].port != st[1].port);                                     // two engines, two port slots

    int kills = adb_calls("kill-server");
    CHECK(soulx_mac_stop(names[0], true) == 0);
    CHECK(adb_calls("kill-server") == kills);                            // duo-b still needs the server
    CHECK(soulx_instance_running(names[1], NULL));
    CHECK(soulx_mac_stop(names[1], true) == 0);
    CHECK(adb_calls("kill-server") == kills + 1);                        // the last one out stops it
    for (int i = 0; i < 2; i++) {
        kill_leftover(st[i].pid);
        CHECK(soulx_mac_delete(names[i]) == 0);
    }
}

// Binds 127.0.0.1:<port> to keep that console port away from the allocator; fd or -1.
static int hold_port(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// With every console port but the last one taken, the first engine must get SOULX_PORT_LAST and a second
// one must be refused (the slot bookkeeping covers the whole range, last slot included).
static void test_port_range_end(void) {
    enum { SLOTS = (SOULX_PORT_LAST - SOULX_PORT_FIRST) / 2 + 1 };
    int held[SLOTS];
    int nheld = 0;
    for (int p = SOULX_PORT_FIRST; p < SOULX_PORT_LAST; p += 2) {
        int fd = hold_port(p);
        if (fd >= 0) held[nheld++] = fd;
    }
    int probe_console = hold_port(SOULX_PORT_LAST);
    int probe_adb = hold_port(SOULX_PORT_LAST + 1);
    bool last_free = probe_console >= 0 && probe_adb >= 0;               // this machine may use them itself
    if (probe_console >= 0) close(probe_console);
    if (probe_adb >= 0) close(probe_adb);

    // The "only SOULX_PORT_LAST is free" precondition only holds when the test itself grabbed every console
    // port below it. A console engine on this Mac (another agent's instance) or a TIME_WAIT remnant on a
    // range port - which hold_port() cannot bind but alloc_port() may legitimately reuse -
    // leaves a lower port free, so skip the check rather than report a false failure.
    bool range_held = nheld == SLOTS - 1;

    if (last_free && range_held) {
        SoulXStartOpts o;
        memset(&o, 0, sizeof(o));
        o.headless = true;
        SoulXInstanceConf c;
        SoulXRunState sa, sb;
        memset(&sa, 0, sizeof(sa));
        soulx_conf_defaults(&c, "edge-a");
        CHECK(soulx_mac_create(&c) == 0);
        soulx_conf_defaults(&c, "edge-b");
        CHECK(soulx_mac_create(&c) == 0);

        CHECK(soulx_mac_start("edge-a", &o, &sa) == 0);
        CHECK(sa.port == SOULX_PORT_LAST);
        CHECK(soulx_mac_start("edge-b", &o, &sb) == -1);                  // "no free console port"
        CHECK(!soulx_instance_running("edge-b", NULL));
        CHECK(soulx_instance_running("edge-a", &sa) && sa.port == SOULX_PORT_LAST);

        for (int i = 0; i < nheld; i++) close(held[i]);
        nheld = 0;
        stop_and_reap("edge-a");
        CHECK(soulx_mac_start("edge-b", &o, &sb) == 0);                   // ports are reusable once freed
        CHECK(sb.port >= SOULX_PORT_FIRST && sb.port <= SOULX_PORT_LAST && sb.port % 2 == 0);
        stop_and_reap("edge-b");
        CHECK(soulx_mac_delete("edge-a") == 0);
        CHECK(soulx_mac_delete("edge-b") == 0);
    } else {
        fprintf(stderr, "note: the console port range %d-%d is not fully free on this machine "
                "(held %d of %d, last slot %s); skipping the last-slot check\n",
                SOULX_PORT_FIRST, SOULX_PORT_LAST, nheld, SLOTS - 1, last_free ? "free" : "busy");
    }
    for (int i = 0; i < nheld; i++) close(held[i]);
}

// Leaves 127.0.0.1:<port> in TIME_WAIT, as the side that closes first does (the exiting engine's sockets).
// Returns false when this machine did not produce the state (the check that needs it is skipped).
static bool make_time_wait(int port) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return false;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = bind(ls, (struct sockaddr*)&a, sizeof(a)) == 0 && listen(ls, 1) == 0;
    int cl = ok ? socket(AF_INET, SOCK_STREAM, 0) : -1;
    ok = ok && cl >= 0 && connect(cl, (struct sockaddr*)&a, sizeof(a)) == 0;
    int sv = ok ? accept(ls, NULL, NULL) : -1;
    close(ls);
    if (sv >= 0) close(sv);                                             // server side first: it gets TIME_WAIT
    soulx_sleep_ms(50);
    if (cl >= 0) close(cl);
    soulx_sleep_ms(50);
    if (!ok || sv < 0) return false;
    int probe = socket(AF_INET, SOCK_STREAM, 0);                        // a strict bind must fail now
    bool in_use = probe >= 0 && bind(probe, (struct sockaddr*)&a, sizeof(a)) != 0 && errno == EADDRINUSE;
    if (probe >= 0) close(probe);
    return in_use;
}

static bool log_file_exists(const char* name, const char* suffix) {
    char rel[96], path[PATH_MAX];
    snprintf(rel, sizeof(rel), "logs/%s.log%s", name, suffix);
    home_path(path, sizeof(path), rel);
    return file_exists(path);
}

// An instance keeps its console port (adb serial) from run to run, also right after a
// stop that left TIME_WAIT remnants; the logs of the last runs are kept.
static void test_last_port(void) {
    SoulXInstanceConf c;
    soulx_conf_defaults(&c, "keep");
    CHECK(soulx_mac_create(&c) == 0);
    SoulXStartOpts o;
    memset(&o, 0, sizeof(o));
    o.headless = true;
    SoulXRunState st1, st2;
    CHECK(soulx_mac_start("keep", &o, &st1) == 0);
    CHECK(soulx_reg_load("keep", &c) == 0 && c.last_port == st1.port);   // remembered for the next start
    stop_and_reap("keep");
    CHECK(soulx_mac_start("keep", &o, &st2) == 0 && st2.port == st1.port);
    stop_and_reap("keep");

    bool tw = make_time_wait(st1.port) && make_time_wait(st1.port + 1);
    if (tw) {
        CHECK(soulx_mac_start("keep", &o, &st2) == 0 && st2.port == st1.port);
        stop_and_reap("keep");
    } else {
        fprintf(stderr, "note: could not produce TIME_WAIT on %d/%d; skipping that check\n", st1.port, st1.port + 1);
    }

    // a live listener on the old port: another port this time, with a note
    int held = hold_port(st1.port);
    if (held >= 0) {
        logs_start();
        CHECK(soulx_mac_start("keep", &o, &st2) == 0 && st2.port != st1.port);
        CHECK(logs_count("is taken;") == 1);
        logs_stop();
        CHECK(soulx_reg_load("keep", &c) == 0 && c.last_port == st2.port);
        stop_and_reap("keep");
        close(held);
    }
    CHECK(soulx_mac_start("keep", &o, &st2) == 0);                        // one more run: 4 or 5 runs so far
    stop_and_reap("keep");
    CHECK(log_file_exists("keep", "") && log_file_exists("keep", ".1") && log_file_exists("keep", ".2") &&
          log_file_exists("keep", ".3") && !log_file_exists("keep", ".4"));
    CHECK(soulx_mac_delete("keep") == 0);
    CHECK(!log_file_exists("keep", "") && !log_file_exists("keep", ".3"));
}

typedef struct {
    const char* name;
    int rc;
} StopJob;

static void* stop_job(void* arg) {
    StopJob* job = arg;
    job->rc = soulx_mac_stop(job->name, false);
    return NULL;
}

// Starts |name| with an engine that serves the console and takes |save_ms| for its "snapshot save".
static bool start_with_console(const char* name, int save_ms, SoulXRunState* st) {
    SoulXInstanceConf c;
    if (soulx_reg_load(name, &c) != 0) {
        soulx_conf_defaults(&c, name);
        if (soulx_mac_create(&c) != 0) return false;
    }
    SoulXStartOpts o;
    memset(&o, 0, sizeof(o));
    o.headless = true;
    char ms[16];
    snprintf(ms, sizeof(ms), "%d", save_ms);
    setenv("SOULX_TEST_FAKE_CONSOLE_MS", ms, 1);
    int rc = soulx_mac_start(name, &o, st);
    unsetenv("SOULX_TEST_FAKE_CONSOLE_MS");
    bool up = false;
    for (int i = 0; rc == 0 && i < 100 && !up; i++) {
        up = port_listening(st->port);
        if (!up) soulx_sleep_ms(30);
    }
    return rc == 0 && up;
}

static char* engine_log(const char* name) {
    char rel[64], path[PATH_MAX];
    snprintf(rel, sizeof(rel), "logs/%s.log", name);
    home_path(path, sizeof(path), rel);
    return read_text(path);
}

// A stop that comes while another one waits for the snapshot save only waits too; a start
// in that window waits for the exit and then starts a new engine; the API reports STOPPING meanwhile.
static void test_overlapping_stops(void) {
    SoulXRunState st;
    CHECK(start_with_console("twice", 2500, &st));
    StopJob a = { "twice", -1 };
    pthread_t ta;
    CHECK(pthread_create(&ta, NULL, stop_job, &a) == 0);
    soulx_sleep_ms(600);                                                   // the first stop's kill was accepted

    SoulXInstanceConf c;
    CHECK(soulx_reg_load("twice", &c) == 0);
    InstanceInfo info;
    CHECK(soulx_get_instance_info(c.id, &info) == SOULX_OK && info.state == INSTANCE_STATE_STOPPING);
    CHECK(soulx_mac_require_running("twice", NULL, NULL, NULL, 0) == SOULX_ERR_NOT_RUNNING);

    logs_start();
    double t0 = soulx_now_ms();
    CHECK(soulx_mac_stop("twice", false) == 0);
    CHECK(soulx_now_ms() - t0 >= 1000);                                    // waited for the save to finish
    pthread_join(ta, NULL);
    CHECK(a.rc == 0);
    CHECK(logs_count("is already shutting down") == 1);
    logs_stop();
    char* log = engine_log("twice");
    CHECK(log && strstr(log, "exiting after the console kill") && !strstr(log, "SIGTERM"));
    free(log);
    CHECK(!soulx_instance_running("twice", NULL));
    kill_leftover(st.pid);

    // a start during the save waits for that engine, then starts a fresh one
    CHECK(start_with_console("twice", 2000, &st));
    StopJob b = { "twice", -1 };
    pthread_t tb;
    CHECK(pthread_create(&tb, NULL, stop_job, &b) == 0);
    soulx_sleep_ms(600);
    SoulXStartOpts o;
    memset(&o, 0, sizeof(o));
    o.headless = true;
    SoulXRunState next;
    CHECK(soulx_mac_start("twice", &o, &next) == 0);
    pthread_join(tb, NULL);
    CHECK(b.rc == 0);
    CHECK(next.pid > 0 && next.pid != st.pid && !soulx_pid_alive(st.pid));
    CHECK(soulx_instance_running("twice", &st) && st.pid == next.pid);
    stop_and_reap("twice");
    CHECK(soulx_mac_delete("twice") == 0);
}

// A stop that cannot reach the console (busy saving after another client's "kill") reads
// the engine log and waits instead of sending SIGTERM.
static void test_stop_reads_log(void) {
    SoulXRunState st;
    setenv("SOULX_TEST_FAKE_CLOSE_ON_KILL", "1", 1);
    bool up = start_with_console("busy", 2000, &st);
    unsetenv("SOULX_TEST_FAKE_CLOSE_ON_KILL");
    CHECK(up);
    if (!up) {
        stop_and_reap("busy");
        soulx_mac_delete("busy");
        return;
    }
    char reply[128];
    CHECK(soulx_console_oneshot(st.port, "kill", reply, sizeof(reply)) == 0);   // someone else's kill
    soulx_sleep_ms(100);
    CHECK(!port_listening(st.port));                                          // its console is gone now
    double t0 = soulx_now_ms();
    CHECK(soulx_mac_stop("busy", false) == 0);
    CHECK(soulx_now_ms() - t0 >= 1000);
    char* log = engine_log("busy");
    CHECK(log && strstr(log, "exiting after the console kill") && !strstr(log, "SIGTERM"));
    free(log);
    kill_leftover(st.pid);
    CHECK(soulx_mac_delete("busy") == 0);
}

// An instance started without waiting gets its post-boot setup from the first
// command that finds the boot finished, exactly once per engine run.
static void test_lazy_post_boot(void) {
    SoulXInstanceConf c;
    soulx_conf_defaults(&c, "lazy");
    CHECK(soulx_mac_create(&c) == 0);
    char booting[PATH_MAX];
    home_path(booting, sizeof(booting), "tmp/fake-booting");
    CHECK(write_text(booting, "1\n"));
    SoulXStartOpts o;
    memset(&o, 0, sizeof(o));
    o.headless = true;                                                    // wait = false: soulx start --no-wait
    SoulXRunState st;
    CHECK(soulx_mac_start("lazy", &o, &st) == 0);
    char serial[32], cmd[96];
    snprintf(serial, sizeof(serial), "emulator-%d", st.port);
    snprintf(cmd, sizeof(cmd), "-s %s shell svc power stayon true; ", serial);   // then the lean commands
    int before = adb_calls_starting(cmd);
    CHECK(soulx_mac_require_running("lazy", NULL, NULL, NULL, 0) == 0);  // still booting: nothing yet
    CHECK(adb_calls_starting(cmd) == before);
    unlink(booting);
    CHECK(soulx_mac_require_running("lazy", NULL, NULL, NULL, 0) == 0);  // booted: applied now ...
    CHECK(adb_calls_starting(cmd) == before + 1);
    CHECK(soulx_mac_require_running("lazy", NULL, NULL, NULL, 0) == 0);  // ... and only once
    SoulXInstanceConf got;
    CHECK(soulx_reg_load("lazy", &got) == 0 && soulx_wait_boot(got.id, 5) == SOULX_OK);
    CHECK(adb_calls_starting(cmd) == before + 1);
    stop_and_reap("lazy");
    CHECK(soulx_mac_delete("lazy") == 0);
}

typedef struct {
    const char* name;
    volatile int stop;
    long polls;
} RaceReader;

static void* race_reader(void* arg) {
    RaceReader* r = arg;
    while (!r->stop) {
        (void)soulx_instance_running(r->name, NULL);
        r->polls++;
    }
    return NULL;
}

// soulx_instance_running() polled from other threads never deletes a state file that a start
// is writing (it used to take the moment between "missing" and "there again" for an unreadable file).
static void test_state_race(void) {
    SoulXInstanceConf c;
    soulx_conf_defaults(&c, "race");
    CHECK(soulx_mac_create(&c) == 0);
    SoulXStartOpts o;
    memset(&o, 0, sizeof(o));
    o.headless = true;
    SoulXRunState st;
    CHECK(soulx_mac_start("race", &o, &st) == 0);
    RaceReader readers[3];
    pthread_t threads[3];
    for (int i = 0; i < 3; i++) {
        readers[i].name = "race";
        readers[i].stop = 0;
        readers[i].polls = 0;
        CHECK(pthread_create(&threads[i], NULL, race_reader, &readers[i]) == 0);
    }
    int lost = 0;
    for (int i = 0; i < 1500; i++) {
        SoulXRunState now;
        if (soulx_state_load("race", &now) != 0 || now.pid != st.pid) lost++;
        int lock = soulx_lock_acquire();
        soulx_state_clear("race");
        CHECK(soulx_state_save("race", &st) == 0);
        soulx_lock_release(lock);
    }
    for (int i = 0; i < 3; i++) {
        readers[i].stop = 1;
        pthread_join(threads[i], NULL);
    }
    CHECK(lost == 0);
    if (lost) fprintf(stderr, "test_state_race: %d of 1500 state files were deleted\n", lost);
    CHECK(soulx_instance_running("race", NULL));
    stop_and_reap("race");
    CHECK(soulx_mac_delete("race") == 0);
}

// Data that cannot be removed keeps the instance (so the delete can be retried);
// Device data without an instance file is moved aside on create, never deleted.
static void test_delete_and_orphans(void) {
    SoulXInstanceConf c;
    soulx_conf_defaults(&c, "stuck");
    CHECK(soulx_mac_create(&c) == 0);
    char locked[PATH_MAX];
    home_path(locked, sizeof(locked), "avd/soulx_stuck.avd/locked.img");
    CHECK(write_text(locked, "x"));
    CHECK(chflags(locked, UF_IMMUTABLE) == 0);
    CHECK(soulx_mac_delete("stuck") == -1);
    CHECK(soulx_reg_load("stuck", &c) == 0);                              // still listed: retry possible
    CHECK(chflags(locked, 0) == 0);
    CHECK(soulx_mac_delete("stuck") == 0);
    CHECK(soulx_reg_load("stuck", &c) == -1);

    char dir[PATH_MAX], data[PATH_MAX];
    home_path(dir, sizeof(dir), "avd/soulx_orph.avd");
    home_path(data, sizeof(data), "avd/soulx_orph.avd/userdata-qemu.img");
    CHECK(soulx_mkdirs(dir) == 0 && write_text(data, "precious game progress\n"));
    soulx_conf_defaults(&c, "orph");
    CHECK(soulx_mac_create(&c) == 0);
    home_path(data, sizeof(data), "avd/soulx_orph.avd/config.ini");
    CHECK(file_exists(data));                                            // a fresh device ...
    home_path(data, sizeof(data), "avd/soulx_orph.avd/userdata-qemu.img");
    CHECK(!file_exists(data));
    char avd_root[PATH_MAX], kept[PATH_MAX] = "";
    home_path(avd_root, sizeof(avd_root), "avd");
    DIR* d = opendir(avd_root);
    for (struct dirent* e = d ? readdir(d) : NULL; e; e = readdir(d)) {
        if (!strncmp(e->d_name, "soulx_orph.avd.orphan-", 21)) snprintf(kept, sizeof(kept), "%s/%s", avd_root, e->d_name);
    }
    if (d) closedir(d);
    CHECK(kept[0] != '\0');                                              // ... and the old one kept aside
    if (kept[0]) {
        char path[PATH_MAX + 32];
        snprintf(path, sizeof(path), "%s/userdata-qemu.img", kept);
        char* text = read_text(path);
        CHECK(text && !strcmp(text, "precious game progress\n"));
        free(text);
        rm_rf(kept);
    }
    CHECK(soulx_mac_delete("orph") == 0);
}

// soulx config: changed settings reach the registry and config.ini; the image is fixed, storage only grows,
// and the panel turns to match the orientation.
static void test_reconfigure(void) {
    SoulXInstanceConf c, n, back;
    soulx_conf_defaults(&c, "recfg");
    CHECK(soulx_mac_create(&c) == 0);
    n = c;
    n.cores = 6;
    n.ram_mb = 6144;
    n.data_gb = 16;
    n.landscape = true;                                                   // 1080x2400 turns to 2400x1080
    n.lean = false;
    snprintf(n.dns, sizeof(n.dns), "1.1.1.1");
    bool restart = true;
    CHECK(soulx_mac_reconfigure(&n, &restart) == 0 && !restart);
    CHECK(n.width == 2400 && n.height == 1080 && n.id == c.id && !strcmp(n.created, c.created));
    CHECK(soulx_reg_load("recfg", &back) == 0);
    CHECK(back.cores == 6 && back.ram_mb == 6144 && back.data_gb == 16 && back.landscape && !back.lean);
    CHECK_STR(back.dns, "1.1.1.1");
    char path[PATH_MAX];
    home_path(path, sizeof(path), "avd/soulx_recfg.avd/config.ini");
    char* ini = read_text(path);
    CHECK(ini && strstr(ini, "hw.cpu.ncore=6\n") && strstr(ini, "hw.ramSize=6144\n") &&
          strstr(ini, "disk.dataPartition.size=16G\n") && strstr(ini, "hw.lcd.width=2400\n") &&
          strstr(ini, "hw.initialOrientation=landscape\n"));
    free(ini);

    n = back;
    n.landscape = false;                                                  // and back to portrait
    CHECK(soulx_mac_reconfigure(&n, NULL) == 0 && n.width == 1080 && n.height == 2400);
    n = back;
    n.data_gb = 8;                                                        // shrinking refused
    CHECK(soulx_mac_reconfigure(&n, NULL) == -1);
    n = back;
    snprintf(n.image, sizeof(n.image), "google");                         // the image is fixed
    CHECK(soulx_mac_reconfigure(&n, NULL) == -1);
    n = back;
    n.cores = 9;                                                          // validated like create
    CHECK(soulx_mac_reconfigure(&n, NULL) == -1);
    n = back;
    n.id = back.id + 100;                                                 // another instance's identity
    CHECK(soulx_mac_reconfigure(&n, NULL) == -1);
    soulx_conf_defaults(&n, "nosuch");
    CHECK(soulx_mac_reconfigure(&n, NULL) == -1);
    CHECK(soulx_reg_load("recfg", &back) == 0 && back.data_gb == 16 && back.cores == 6 && !back.landscape);

    // running: the registry changes now, config.ini only at the next start
    SoulXStartOpts o;
    memset(&o, 0, sizeof(o));
    o.headless = true;
    SoulXRunState st;
    CHECK(soulx_mac_start("recfg", &o, &st) == 0);
    n = back;
    n.cores = 2;
    restart = false;
    CHECK(soulx_mac_reconfigure(&n, &restart) == 0 && restart);
    CHECK(soulx_reg_load("recfg", &back) == 0 && back.cores == 2);
    ini = read_text(path);
    CHECK(ini && strstr(ini, "hw.cpu.ncore=6\n"));
    free(ini);
    stop_and_reap("recfg");
    CHECK(soulx_mac_delete("recfg") == 0);
}

// Ids are not reused, non-ASCII names stay distinct, soulx_set_config with
// a mostly empty struct changes only what it sets, and the missing-instance hints match the CLI.
static void test_api_details(void) {
    SoulXInstanceConf a, b, c;
    soulx_conf_defaults(&a, "ida");
    soulx_conf_defaults(&b, "idb");
    CHECK(soulx_mac_create(&a) == 0 && soulx_mac_create(&b) == 0 && b.id == a.id + 1);
    CHECK(soulx_mac_delete("idb") == 0);
    soulx_conf_defaults(&c, "idc");
    CHECK(soulx_mac_create(&c) == 0 && c.id == b.id + 1);                  // not b's id again
    CHECK(soulx_mac_delete("ida") == 0 && soulx_mac_delete("idc") == 0);

    InstanceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.profile = PERF_PROFILE_CUSTOM;
    snprintf(cfg.name, sizeof(cfg.name), "\xE6\xB8\xB8\xE6\x88\xB2");     // 游戏
    int g1 = soulx_create_instance(&cfg);
    snprintf(cfg.name, sizeof(cfg.name), "\xE4\xBD\xA0\xE5\xA5\xBD");     // 你好
    int g2 = soulx_create_instance(&cfg);
    snprintf(cfg.name, sizeof(cfg.name), "\xE6\xB8\xB8\xE6\x88\xB2" "1"); // 游戏1
    int g3 = soulx_create_instance(&cfg);
    CHECK(g1 > 0 && g2 > 0 && g3 > 0 && g1 != g2);
    InstanceInfo i1, i2, i3;
    CHECK(soulx_get_instance_info(g1, &i1) == SOULX_OK && soulx_get_instance_info(g2, &i2) == SOULX_OK &&
          soulx_get_instance_info(g3, &i3) == SOULX_OK);
    CHECK(strcmp(i1.config.name, i2.config.name) != 0);
    CHECK(!strncmp(i1.config.name, "instance-", 9) && !strncmp(i3.config.name, "1-", 2));
    CHECK(soulx_name_valid(i1.config.name) && soulx_name_valid(i3.config.name));
    CHECK(soulx_find_instance("\xE6\xB8\xB8\xE6\x88\xB2") == g1);
    CHECK(soulx_create_instance(&cfg) == SOULX_ERROR);                       // the same name twice
    CHECK(soulx_delete_instance(g1) == SOULX_OK && soulx_delete_instance(g2) == SOULX_OK &&
          soulx_delete_instance(g3) == SOULX_OK);

    // A struct that only sets ram_mb changes only the RAM
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.name, sizeof(cfg.name), "keeper");
    cfg.profile = PERF_PROFILE_PERFORMANCE;
    cfg.enable_root = true;
    cfg.graphics = GRAPHICS_BACKEND_SOFTWARE;
    cfg.dpi = 400;
    int id = soulx_create_instance(&cfg);
    CHECK(id > 0);
    SoulXInstanceConf before, after;
    CHECK(soulx_reg_find_by_id(id, &before) == 0);
    CHECK(!strcmp(before.image, "google") && !strcmp(before.gpu, "software") && before.cores == 6 &&
          before.dpi == 400);
    InstanceConfig u;
    memset(&u, 0, sizeof(u));
    u.ram_mb = 8192;
    CHECK(soulx_set_config(id, &u) == SOULX_OK);
    CHECK(soulx_reg_find_by_id(id, &after) == 0);
    before.ram_mb = 8192;
    CHECK(conf_equal(&before, &after));
    memset(&u, 0, sizeof(u));                                            // an explicit profile still applies
    u.profile = PERF_PROFILE_BALANCED;
    CHECK(soulx_set_config(id, &u) == SOULX_OK);
    CHECK(soulx_reg_find_by_id(id, &after) == 0 && after.cores == 4 && after.dpi == 420 &&
          !strcmp(after.image, "google") && !strcmp(after.gpu, "software"));
    memset(&u, 0, sizeof(u));                                            // and any explicit field
    u.graphics = GRAPHICS_BACKEND_VULKAN;
    snprintf(u.android_version, sizeof(u.android_version), "37.0");
    CHECK(soulx_set_config(id, &u) == SOULX_OK);
    CHECK(soulx_reg_find_by_id(id, &after) == 0 && !strcmp(after.image, "play") && !strcmp(after.gpu, "host"));
    CHECK(soulx_delete_instance(id) == SOULX_OK);

    // The same hint as the CLI for a missing instance
    unsetenv("SOULX_INSTANCE");
    logs_start();
    CHECK(soulx_mac_require_running(NULL, NULL, NULL, NULL, 0) == SOULX_ERR_NOT_FOUND);
    CHECK(soulx_mac_require_running("ghost", NULL, NULL, NULL, 0) == SOULX_ERR_NOT_FOUND);
    CHECK(logs_count("no such instance 'default' \xE2\x80\x94 create it with: soulx create (or just run: soulx play "
                     "game.apk)") == 1);
    CHECK(logs_count("no such instance 'ghost' \xE2\x80\x94 create it with: soulx create ghost") == 1);
    logs_stop();
}

// Network capture: the engine runs the built-in WiFi with a QMP socket, and network-capture-start / network-capture-stop add and
// remove the dump on it. The fake engine's QMP server (below) logs what it is asked and writes the file itself,
// like QEMU does; captures go to a folder with a space and a quote in its name to prove the JSON round trip.
static int qmp_log_count(const char* what) {
    char path[PATH_MAX];
    home_path(path, sizeof(path), "tmp/fake-qmp.log");
    char* text = read_text(path);
    int n = 0;
    for (const char* p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (strncmp(p, what, strlen(what)) == 0) n++;
    }
    free(text);
    return n;
}

static void start_headless(const char* name, SoulXRunState* st) {
    SoulXStartOpts o;
    memset(&o, 0, sizeof(o));
    o.headless = true;
    CHECK(soulx_mac_start(name, &o, st) == 0);
}

static void test_capture(void) {
    char caps[PATH_MAX], path[PATH_MAX], want[PATH_MAX + 64];
    home_path(caps, sizeof(caps), "captures \"q\"");
    setenv("SOULX_NETWORK_CAPTURES", caps, 1);
    home_path(path, sizeof(path), "tmp/fake-qmp.log");
    unlink(path);

    SoulXInstanceConf c;
    soulx_conf_defaults(&c, "cap");
    CHECK(soulx_mac_create(&c) == 0);
    SoulXRunState st;
    start_headless("cap", &st);

    // the engine got the built-in WiFi and the control socket, the -qemu part last
    char sock[PATH_MAX];
    home_path(sock, sizeof(sock), "run/cap.qmp");
    home_path(path, sizeof(path), "logs/cap.log");
    char* log = read_text(path);
    snprintf(want, sizeof(want), " -qemu -qmp unix:%s,server,nowait\n", sock);
    CHECK(log && strstr(log, " -feature -WiFiPacketStream ") && strstr(log, want));
    free(log);

    // nothing records before network-capture-start, and only one recording runs at a time
    char dest[1200], again[1200];
    SoulXRunState now;
    CHECK(soulx_mac_network_capture_stop("cap", NULL, 0) != 0);
    CHECK(qmp_log_count("object-add") == 0);
    CHECK(soulx_mac_network_capture_start("cap", dest, sizeof(dest)) == 0);
    snprintf(want, sizeof(want), "%s/cap/", caps);
    CHECK(strncmp(dest, want, strlen(want)) == 0 && strstr(dest, ".pcap") != NULL);
    CHECK(access(dest, F_OK) == 0);                                      // the engine opened it
    CHECK(soulx_instance_running("cap", &now) && !strcmp(now.network_capture_path, dest));
    CHECK(soulx_mac_network_capture_start("cap", NULL, 0) != 0);
    CHECK(soulx_mac_network_capture_stop("cap", again, sizeof(again)) == 0 && !strcmp(dest, again));
    CHECK(soulx_instance_running("cap", &now) && now.network_capture_path[0] == '\0');
    CHECK(soulx_mac_network_capture_stop("cap", NULL, 0) != 0);
    CHECK(qmp_log_count("object-add") == 1 && qmp_log_count("object-del") == 1);
    snprintf(want, sizeof(want), "object-add %s", dest);
    home_path(path, sizeof(path), "tmp/fake-qmp.log");
    char* q = read_text(path);
    CHECK(q && strstr(q, want));                                         // the path came through intact
    free(q);

    // every recording gets a file of its own, even within the same second
    CHECK(soulx_mac_network_capture_start("cap", again, sizeof(again)) == 0 && strcmp(dest, again) != 0);

    // stopping the instance closes a recording in progress first; the next run starts clean
    stop_and_reap("cap");
    CHECK(qmp_log_count("object-del") == 2);
    start_headless("cap", &st);
    CHECK(soulx_instance_running("cap", &now) && now.network_capture_path[0] == '\0');

    // an engine without the built-in WiFi card (SOULX_ENGINE_FEATURES re-enabling netsim WiFi) would record an
    // empty file: refused, and nothing is attached
    stop_and_reap("cap");
    setenv("SOULX_TEST_FAKE_NO_WIFI_NIC", "1", 1);
    start_headless("cap", &st);
    int adds = qmp_log_count("object-add");
    CHECK(soulx_mac_network_capture_start("cap", NULL, 0) != 0 && qmp_log_count("object-add") == adds);
    CHECK(soulx_instance_running("cap", &now) && now.network_capture_path[0] == '\0');
    stop_and_reap("cap");
    unsetenv("SOULX_TEST_FAKE_NO_WIFI_NIC");

    // an engine started without the control socket (an older SoulX): a clear failure, no state change
    setenv("SOULX_TEST_FAKE_NO_QMP", "1", 1);
    start_headless("cap", &st);
    CHECK(soulx_mac_network_capture_start("cap", NULL, 0) != 0);
    CHECK(soulx_instance_running("cap", &now) && now.network_capture_path[0] == '\0');
    stop_and_reap("cap");
    unsetenv("SOULX_TEST_FAKE_NO_QMP");
    CHECK(soulx_mac_network_capture_start("cap", NULL, 0) != 0);                 // not running at all
    CHECK(soulx_mac_delete("cap") == 0);

    // the socket argument: commas are doubled for QEMU, and a path too long for a unix socket is refused
    char saved[PATH_MAX];
    snprintf(saved, sizeof(saved), "%s", g_home);
    char odd[PATH_MAX], arg[PATH_MAX + 64];
    snprintf(odd, sizeof(odd), "%s/a,b", g_home);
    setenv("SOULX_HOME", odd, 1);
    CHECK(soulx_mac_network_capture_qmp_arg("x", arg, sizeof(arg)) == 0);
    snprintf(want, sizeof(want), "unix:%s/a,,b/run/x.qmp,server,nowait", g_home);
    CHECK_STR(arg, want);
    char longhome[PATH_MAX];
    snprintf(longhome, sizeof(longhome), "%s/%0120d", g_home, 0);
    setenv("SOULX_HOME", longhome, 1);
    CHECK(soulx_mac_network_capture_qmp_arg("x", arg, sizeof(arg)) == -1);
    setenv("SOULX_HOME", saved, 1);
    unsetenv("SOULX_NETWORK_CAPTURES");

    // a socket that cannot be created (here: its path is a folder) means no capture channel, not an engine that
    // refuses to start
    home_path(path, sizeof(path), "run/x.qmp");
    CHECK(mkdir(path, 0755) == 0);
    CHECK(soulx_mac_network_capture_qmp_arg("x", arg, sizeof(arg)) == -1);
    CHECK(rmdir(path) == 0);
    CHECK(soulx_mac_network_capture_qmp_arg("x", arg, sizeof(arg)) == 0 && access(path, F_OK) != 0);   // probe left nothing
}

static void test_engine_lifecycle(void) {
    if (!install_fake_runtime()) {
        CHECK(!"fake runtime could not be installed");
        return;
    }
    lifecycle("life", false);                                            // launcher exec()s qemu (real behaviour)
    lifecycle("hand", true);                                             // launcher spawns qemu and exits
    unsetenv("SOULX_TEST_LAUNCHER_CHILD");                                // the rest runs the real exec() behaviour
    test_engine_gpu_args();
    test_graceful_console_stop();
    test_adb_server_follows_last_instance();
    test_port_range_end();
    test_last_port();
    test_overlapping_stops();
    test_stop_reads_log();
    test_lazy_post_boot();
    test_state_race();
    test_delete_and_orphans();
    test_reconfigure();
    test_api_details();
    test_capture();
}

// ---------------------------------------------------------------------------
// Fake runtime binaries (selected by argv[0])
// ---------------------------------------------------------------------------

static int fake_launcher(char** argv) {
    char qemu[PATH_MAX];
    const char* slash = strrchr(argv[0], '/');
    bool no_window = false;
    for (int i = 1; argv[i]; i++) no_window = no_window || !strcmp(argv[i], "-no-window");
    int n = snprintf(qemu, sizeof(qemu), "%.*s/qemu/darwin-aarch64/qemu-system-aarch64%s",
                     (int)(slash ? slash - argv[0] : 1), slash ? argv[0] : ".", no_window ? "-headless" : "");
    if (n < 0 || (size_t)n >= sizeof(qemu)) return 1;
    for (int i = 1; argv[i]; i++) printf("%s%s", i > 1 ? " " : "args: ", argv[i]);
    printf("\n");
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) printf("cwd: %s\n", cwd);              // SoulX must launch from $SOULX_HOME/tmp
    const char* angle = getenv(SOULX_ANGLE_PLATFORM_ENV);
    printf("angle: %s\n", angle ? angle : "(unset)");                    // the platform SoulX picked for ANGLE
    fflush(stdout);
    argv[0] = qemu;
    const char* child = getenv("SOULX_TEST_LAUNCHER_CHILD");
    if (child && !strcmp(child, "1")) {
        pid_t pid;
        if (posix_spawn(&pid, qemu, NULL, NULL, argv, environ) != 0) return 1;
        struct timespec ts = { 0, 300 * 1000000L };
        nanosleep(&ts, NULL);
        return 0;                                                        // hand over and exit
    }
    execv(qemu, argv);
    return 127;
}

static volatile sig_atomic_t g_fake_term = 0;

static void fake_on_term(int sig) {
    (void)sig;
    g_fake_term = 1;
}

static void fake_say(const char* text) {
    printf("fake engine: %s\n", text);                                   // lands in $SOULX_HOME/logs/<name>.log
    fflush(stdout);
}

// Stand-in for the emulator console: banner, then "kill" -> "OK: killing emulator, bye bye" + "OK"; after
// that the engine needs |delay_ms| (the Quick Boot snapshot save) before it exits on its own, printing
// the lines the real engine prints meanwhile. With SOULX_TEST_FAKE_CLOSE_ON_KILL=1 the console goes away
// during the save (a stop that comes later can only tell from the log). A SIGTERM is noted in the log
// and ends the process at once.
static int fake_engine_console(int port, int delay_ms) {
    const char* close_env = getenv("SOULX_TEST_FAKE_CLOSE_ON_KILL");
    bool close_on_kill = close_env && !strcmp(close_env, "1");
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fake_on_term;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);                                       // no SA_RESTART: poll() sees EINTR
    signal(SIGPIPE, SIG_IGN);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return 1;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(ls, 4) != 0) {
        fake_say("cannot listen on the console port");
        return 1;
    }

    static const char banner[] = "Android Console: type 'help' for a list of commands\r\nOK\r\n";
    static const char bye[] = "OK: killing emulator, bye bye\r\nOK\r\n";
    double exit_at = 0;
    double kill_at = 0;
    for (;;) {
        if (g_fake_term) {
            fake_say("SIGTERM received");
            return 143;
        }
        if (exit_at > 0 && soulx_now_ms() >= exit_at) {
            printf("INFO         | Saving snapshot 'default_boot' took %d ms\n", (int)(soulx_now_ms() - kill_at));
            fake_say("exiting after the console kill");
            return 0;
        }
        struct pollfd pfd = { ls, POLLIN, 0 };
        if (ls < 0 || poll(&pfd, 1, 20) <= 0) {
            if (ls < 0) {
                struct timespec ts = { 0, 20 * 1000000L };
                nanosleep(&ts, NULL);
            }
            continue;
        }
        int client = accept(ls, NULL, NULL);
        if (client < 0) continue;
        send(client, banner, sizeof(banner) - 1, 0);
        char line[64] = "";
        struct pollfd cp = { client, POLLIN, 0 };
        if (poll(&cp, 1, 2000) > 0) {
            ssize_t r = recv(client, line, sizeof(line) - 1, 0);
            line[r > 0 ? r : 0] = '\0';
        }
        if (strncmp(line, "kill", 4) == 0) {
            send(client, bye, sizeof(bye) - 1, 0);
            fake_say("console kill received");
            if (exit_at == 0) {
                kill_at = soulx_now_ms();
                exit_at = kill_at + delay_ms;
                printf("INFO         | Wait for emulator (pid %d) 90 seconds to shutdown gracefully before kill\n"
                       "INFO         | Saving with gfxstream=1\n", (int)getpid());
                fflush(stdout);
                if (close_on_kill) {
                    close(ls);
                    ls = -1;
                }
            }
        }
        close(client);
    }
}

// Stand-in for QEMU's QMP socket (-qemu -qmp unix:...): one client at a time, the greeting, then a canned answer
// for the commands network_capture.c sends. What was asked lands in $SOULX_HOME/tmp/fake-qmp.log, and object-add creates
// the file like filter-dump does (a pcap with two packets). The "dump" state belongs to the engine, so it
// survives reconnects.
static void fake_qmp_log(const char* line) {
    const char* home = getenv("SOULX_HOME");
    char path[PATH_MAX];
    if (!home || snprintf(path, sizeof(path), "%s/tmp/fake-qmp.log", home) >= (int)sizeof(path)) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

static void fake_qmp_say(int fd, const char* line) {
    (void)!write(fd, line, strlen(line));
    (void)!write(fd, "\r\n", 2);
}

static void fake_qmp_answer(int fd, const char* cmd, bool* dump_on) {
    static const unsigned char pcap[] = {
        0xd4, 0xc3, 0xb2, 0xa1, 2, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 1, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 1, 2, 3, 4,
        1, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 5, 6, 7, 8,
    };
    if (strstr(cmd, "qmp_capabilities")) {
        fake_qmp_say(fd, "{\"return\": {}}");
    } else if (strstr(cmd, "human-monitor-command")) {
        const char* no_nic = getenv("SOULX_TEST_FAKE_NO_WIFI_NIC");
        if (no_nic && !strcmp(no_nic, "1")) {
            fake_qmp_say(fd, "{\"return\": \"virtio-wifi: index=0,type=user,net=10.0.2.0\\r\\n\"}");
        } else {
            fake_qmp_say(fd, "{\"return\": \"virtio-wifi-device: index=0,type=nic,\\r\\n \\\\ virtio-wifi: index=0,type=user\\r\\n\"}");
        }
    } else if (strstr(cmd, "\"object-add\"")) {
        const char* f = strstr(cmd, "\"file\":\"");
        char file[PATH_MAX];
        size_t n = 0;
        for (const char* p = f ? f + 8 : NULL; p && *p && *p != '"' && n + 1 < sizeof(file); p++) {
            if (*p == '\\' && p[1]) p++;                                 // \" and \\ in the JSON string
            file[n++] = *p;
        }
        file[n] = '\0';
        bool ok = f && strstr(cmd, "\"qom-type\":\"filter-dump\"") && strstr(cmd, "\"id\":\"soulxcap\"") &&
                  strstr(cmd, "\"netdev\":\"virtio-wifi\"");
        if (!ok || *dump_on) {
            fake_qmp_say(fd, "{\"error\": {\"class\": \"GenericError\", \"desc\": \"cannot add the object\"}}");
            return;
        }
        FILE* out = fopen(file, "wb");
        if (!out) {
            fake_qmp_say(fd, "{\"error\": {\"class\": \"GenericError\", \"desc\": \"Could not open the file\"}}");
            return;
        }
        fwrite(pcap, 1, sizeof(pcap), out);
        fclose(out);
        *dump_on = true;
        char line[PATH_MAX + 32];
        snprintf(line, sizeof(line), "object-add %s", file);
        fake_qmp_log(line);
        fake_qmp_say(fd, "{\"timestamp\": {\"seconds\": 1, \"microseconds\": 2}, \"event\": \"NIC_RX_FILTER_CHANGED\"}");
        fake_qmp_say(fd, "{\"return\": {}}");
    } else if (strstr(cmd, "\"object-del\"")) {
        if (!*dump_on) {
            fake_qmp_say(fd, "{\"error\": {\"class\": \"GenericError\", \"desc\": \"object 'soulxcap' not found\"}}");
            return;
        }
        *dump_on = false;
        fake_qmp_log("object-del");
        fake_qmp_say(fd, "{\"return\": {}}");
    } else {
        fake_qmp_say(fd, "{\"error\": {\"class\": \"CommandNotFound\", \"desc\": \"unknown command\"}}");
    }
}

static void* fake_qmp_serve(void* arg) {
    int ls = (int)(intptr_t)arg;
    bool dump_on = false;
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        fake_qmp_say(fd, "{\"QMP\": {\"version\": {\"qemu\": {\"micro\": 0, \"minor\": 12, \"major\": 2}, "
                         "\"package\": \"fake\"}, \"capabilities\": []}}");
        char buf[8192];
        size_t len = 0;
        for (;;) {
            char* nl = memchr(buf, '\n', len);
            if (!nl) {
                ssize_t got = read(fd, buf + len, sizeof(buf) - 1 - len);
                if (got <= 0) break;
                len += (size_t)got;
                continue;
            }
            *nl = '\0';
            fake_qmp_answer(fd, buf, &dump_on);
            len -= (size_t)(nl + 1 - buf);
            memmove(buf, nl + 1, len);
        }
        close(fd);
    }
    return NULL;
}

// -qemu -qmp unix:<path>,server,nowait: listens there, like the real engine (an old socket file is replaced).
static void fake_qmp_listen(int argc, char** argv) {
    const char* no_qmp = getenv("SOULX_TEST_FAKE_NO_QMP");
    if (no_qmp && !strcmp(no_qmp, "1")) return;                          // an engine from before the capture channel
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-qmp") != 0 || strncmp(argv[i + 1], "unix:", 5) != 0) continue;
        char path[PATH_MAX];
        size_t n = 0;
        const char* v = argv[i + 1] + 5;
        const char* end = strstr(v, ",server,nowait");
        for (; end && v < end && n + 1 < sizeof(path); v++) {
            if (v[0] == ',' && v[1] == ',') v++;                         // ",," is a literal comma
            path[n++] = *v;
        }
        path[n] = '\0';
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        if (!end || n == 0 || n >= sizeof(sa.sun_path)) return;
        memcpy(sa.sun_path, path, n + 1);
        int ls = socket(AF_UNIX, SOCK_STREAM, 0);
        unlink(path);
        if (ls < 0 || bind(ls, (struct sockaddr*)&sa, sizeof(sa)) != 0 || listen(ls, 1) != 0) return;
        pthread_t t;
        if (pthread_create(&t, NULL, fake_qmp_serve, (void*)(intptr_t)ls) == 0) pthread_detach(t);
        return;
    }
}

static int fake_engine(int argc, char** argv) {
    alarm(300);                                                          // never outlive an aborted test run
    fake_qmp_listen(argc, argv);
    const char* delay = getenv("SOULX_TEST_FAKE_CONSOLE_MS");             // set: serve the emulator console
    int port = 0;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "-port")) port = atoi(argv[i + 1]);
    }
    if (delay && port > 0) return fake_engine_console(port, atoi(delay));
    for (;;) pause();                                                    // until SIGTERM / SIGKILL
}

static int fake_adb(int argc, char** argv) {
    const char* home = getenv("SOULX_HOME");
    if (home) {
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/tmp/fake-adb.log", home) < (int)sizeof(path)) {
            FILE* f = fopen(path, "a");
            if (f) {
                for (int i = 1; i < argc; i++) fprintf(f, "%s%s", i > 1 ? " " : "", argv[i]);
                fprintf(f, "\n");
                fclose(f);
            }
        }
    }
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-s")) i += 2;
    if (i + 1 < argc && !strcmp(argv[i], "keygen")) {                   // adb keygen <file>
        FILE* k = fopen(argv[i + 1], "w");
        if (!k) return 1;
        fputs("fake key\n", k);
        fclose(k);
        return 0;
    }
    if (i < argc && !strcmp(argv[i], "shell") && i + 2 < argc && !strcmp(argv[i + 1], "getprop") &&
        !strcmp(argv[i + 2], "sys.boot_completed")) {
        char booting[PATH_MAX];
        bool still_booting = home && snprintf(booting, sizeof(booting), "%s/tmp/fake-booting", home) <
                                     (int)sizeof(booting) && access(booting, F_OK) == 0;
        printf("%s\n", still_booting ? "0" : "1");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    const char* base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (!strcmp(base, "emulator")) return fake_launcher(argv);
    if (!strcmp(base, "qemu-system-aarch64") || !strcmp(base, "qemu-system-aarch64-headless")) {
        return fake_engine(argc, argv);
    }
    if (!strcmp(base, "adb")) return fake_adb(argc, argv);

    snprintf(g_home, sizeof(g_home), "/private/tmp/soulx-test-registry-XXXXXX");
    if (!mkdtemp(g_home)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("SOULX_HOME", g_home, 1);
    unsetenv("SOULX_INSTANCE");
    if (soulx_ensure_layout() != 0) {
        fprintf(stderr, "cannot create the test SOULX_HOME %s\n", g_home);
        rm_rf(g_home);
        return 1;
    }

    test_names();
    test_defaults_profiles();
    test_registry();
    test_warn_once();
    test_state();
    test_avd();
    test_engine_offline();
    test_engine_lifecycle();

    rm_rf(g_home);
    if (g_failures) {
        fprintf(stderr, "test_registry: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_registry: all %d checks passed\n", g_checks);
    return 0;
}
