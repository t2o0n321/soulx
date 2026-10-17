/*
 * SoulX Android Emulator - macOS backend internal interface
 *
 * The macOS port owns its whole runtime under $SOULX_HOME (default ~/.soulx-android):
 *
 *   sdk/            SoulX-owned engine: emulator/, platform-tools/, system-images/...
 *   sdk/.soulx/      install markers (<package-key>.installed)
 *   downloads/      cached package zips
 *   licenses/       accepted license texts (<license-id>.txt)
 *   avd/            device definitions written by SoulX (soulx_<name>.ini + soulx_<name>.avd/)
 *   home/           HOME for every child process (adb keys, console token, emulator prefs)
 *   instances/      SoulX instance registry (<name>.conf)
 *   run/            runtime state of started instances (<name>.state) + lock file
 *   logs/           engine logs (<name>.log)
 *   tmp/            ANDROID_TMP
 *
 * Nothing here reads from or writes to Android Studio's SDK or ~/.android.
 * Every child process is spawned with posix_spawn and an argv array (never /bin/sh).
 */

#ifndef SOULX_MAC_H
#define SOULX_MAC_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "soulx_android.h"

#define SOULX_ADB_SERVER_PORT    5137    // private adb server, never Android Studio's 5037
#define SOULX_PORT_FIRST         5600    // console ports are even numbers 5600..5680 (adb = port + 1).
#define SOULX_PORT_LAST          5680    // Other adb servers scan 5555..5585 for emulators; staying above
                                        // that range keeps e.g. Android Studio's adb from finding SoulX's
                                        // instances (it would pop "Allow USB debugging?" in the guest).
#define SOULX_NAME_MAX           32
#define SOULX_DEFAULT_INSTANCE   "default"

// ---------------------------------------------------------------------------
// log.c - logging (stderr; respects soulx_set_log_callback)
// ---------------------------------------------------------------------------

#define SOULX_LOG_DEBUG  -1
#define SOULX_LOG_INFO    0
#define SOULX_LOG_WARN    1
#define SOULX_LOG_ERROR   2

void soulx_log(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void soulx_set_verbose(bool verbose);                 // enables SOULX_LOG_DEBUG output
bool soulx_is_verbose(void);
void soulx_status_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));  // transient line on a TTY
void soulx_status_done(void);                         // clears the transient line
void soulx_mac_log_dispatch(int level, const char* message); // used by log.c to honour LogCallback

// ---------------------------------------------------------------------------
// paths.c - $SOULX_HOME layout
// ---------------------------------------------------------------------------

const char* soulx_home(void);                         // $SOULX_HOME or ~/.soulx-android (absolute, no trailing '/')
int  soulx_path(char* buf, size_t cap, const char* rel_fmt, ...) __attribute__((format(printf, 3, 4)));
                                                     // buf = soulx_home() + "/" + formatted rel; 0 ok, -1 truncated
int  soulx_mkdirs(const char* path);                  // mkdir -p (0755); 0 ok
int  soulx_ensure_layout(void);                       // creates every directory listed above
int  soulx_migrate_legacy_home(void);                // ~/.soul-android (Soul, before the rename) -> ~/.soulx-android
                                                     // once; -1 when an engine still runs from the old place
bool soulx_path_exists(const char* path);
bool soulx_is_dir(const char* path);
int  soulx_rmtree(const char* path);                  // rm -rf, refuses anything outside soulx_home(); 0 ok
int  soulx_read_file(const char* path, char** data, size_t* len);   // malloc'd, NUL-terminated
int  soulx_write_file_atomic(const char* path, const char* data, size_t len); // tmp + rename
uint64_t soulx_dir_size(const char* path);            // bytes, recursive (best effort)
uint64_t soulx_free_disk_bytes(const char* path);

// ---------------------------------------------------------------------------
// proc.c - process spawning (posix_spawn only)
// ---------------------------------------------------------------------------

typedef struct {
    const char* const* argv;     // argv[0] must be an absolute path; NULL terminated
    const char* cwd;             // NULL = inherit
    bool soulx_env;               // apply the SoulX isolation environment (see soulx_build_env)
    const char* const* env;      // soulx_env only: extra "NAME=value" entries (NULL terminated); each replaces
                                 // an inherited variable of the same name
    const char* stdin_data;      // optional bytes written to the child's stdin, then closed
    size_t stdin_len;
    int timeout_ms;              // 0 = none; child gets SIGKILL when it expires
    size_t max_output;           // 0 = 256 MB cap for captured stdout
    bool interruptible;          // soulx_run only: when g_soulx_interrupted becomes set, terminate the child
                                 // (SIGTERM, SIGKILL 2 s later) and return with res->status = 130
} SoulXCmd;

typedef struct {
    int status;                  // exit code, 128+signal when killed, -1 when spawn failed
    bool timed_out;
    char* out;                   // captured stdout (malloc'd, NUL-terminated, may contain NULs)
    size_t out_len;
    char* err;                   // captured stderr (malloc'd, NUL-terminated)
    size_t err_len;
} SoulXCmdResult;

int   soulx_run(const SoulXCmd* cmd, SoulXCmdResult* res);   // returns res->status (or -1)
void  soulx_cmd_result_free(SoulXCmdResult* res);
int   soulx_run_passthrough(const SoulXCmd* cmd);           // child inherits stdio; returns exit status
pid_t soulx_spawn_detached(const SoulXCmd* cmd, const char* log_path, bool interactive_qos);
                                                         // new session (setsid), stdin=/dev/null, stdout+stderr
                                                         // appended to log_path; QOS_CLASS_USER_INTERACTIVE when
                                                         // interactive_qos (keeps vCPUs on performance cores)
char** soulx_build_env(void);                             // inherited env minus ANDROID_SDK_HOME, ANDROID_PREFS_ROOT,
                                                         // ANDROID_USER_HOME, ANDROID_SERIAL, ADB_SERVER_SOCKET, TMPDIR plus
                                                         // ANDROID_HOME/ANDROID_SDK_ROOT=sdk, ANDROID_AVD_HOME=avd,
                                                         // ANDROID_EMULATOR_HOME=home/.android, ANDROID_TMP=tmp,
                                                         // ANDROID_ADB_SERVER_PORT=5137, HOME=home,
                                                         // ANDROID_EMU_ENABLE_CRASH_REPORTING=0, TMPDIR=tmp/,
                                                         // ANDROID_EMULATOR_WAIT_TIME_BEFORE_KILL=90
void  soulx_free_env(char** envp);
bool  soulx_pid_alive(pid_t pid);
bool  soulx_pid_is_engine(pid_t pid);                     // proc_pidpath() is inside soulx_home()/sdk/emulator/
int   soulx_wait_pid_exit(pid_t pid, int timeout_ms);     // 0 = exited, -1 = still alive at timeout
double soulx_now_ms(void);                                // monotonic milliseconds

extern volatile sig_atomic_t g_soulx_interrupted;         // set by the CLI's SIGINT handler
bool  soulx_interrupted(void);
int   soulx_sleep_ms(int ms);                             // sleeps in small slices; -1 if interrupted

// ---------------------------------------------------------------------------
// runtime.c - SoulX runtime provisioning (engine, platform-tools, system images)
// ---------------------------------------------------------------------------

typedef enum {
    SOULX_PKG_ENGINE = 0,
    SOULX_PKG_PLATFORM_TOOLS,
    SOULX_PKG_IMAGE
} SoulXPackageKind;

typedef struct {
    const char* key;             // "emulator", "platform-tools", "play", "google", "play16k"
    SoulXPackageKind kind;
    const char* sdk_id;          // e.g. "system-images;android-37.0;google_apis_playstore;arm64-v8a"
    const char* version;         // "37.2.12", "r06", ...
    const char* url;             // absolute https://dl.google.com/... URL
    uint64_t size;               // bytes
    const char* sha1;            // lower-case hex (Google only publishes SHA-1)
    const char* license_id;      // "android-sdk-license" / "android-sdk-arm-dbt-license"
    const char* zip_root;        // single top-level directory inside the zip
    const char* install_rel;     // install directory relative to sdk/
    // system images only
    const char* target;          // "android-37.0"
    const char* tag_id;          // "google_apis_playstore"
    const char* tag_display;     // "Google Play"
    const char* android_name;    // "Android 17"
    bool playstore;
    bool page16k;
    const char* description;
} SoulXPackage;

typedef struct {
    bool accept_licenses;        // non-interactive acceptance (--accept-licenses)
    bool keep_downloads;         // keep zips in downloads/ after install
    bool force;                  // reinstall even when markers match
} SoulXSetupOpts;

const SoulXPackage* soulx_runtime_packages(size_t* count);
const SoulXPackage* soulx_runtime_find(const char* key);
const SoulXPackage* soulx_runtime_default_image(void);           // "play"
bool  soulx_runtime_installed(const SoulXPackage* p);            // marker sha1 matches and install dir exists
bool  soulx_runtime_ready(const char* image_key);               // engine + platform-tools + that image installed
int   soulx_runtime_setup(const char* image_key, const SoulXSetupOpts* opts);   // 0 ok
int   soulx_runtime_print_status(void);
int   soulx_doctor(void);                                        // prints checks, returns number of blockers
int   soulx_tool_path(const char* tool, char* buf, size_t cap); // "adb" | "emulator" -> absolute path; 0 ok
int   soulx_ensure_adb_key(void);                                // adb keygen into home/.android if missing

// gpu=metal runs the guest's OpenGL ES on the engine's own ANGLE, straight on Metal. The engine forces
// ANGLE's platform through ANGLE_DEFAULT_PLATFORM, so SoulX makes its ANGLE read this variable instead
// (same length) and sets it for every engine it starts: "metal" for gpu=metal, else "vulkan" (what the
// engine itself picks for its software renderer, which runs on the same ANGLE).
#define SOULX_ANGLE_PLATFORM_ENV  "SOULX_DEFAULT_PLATFORM"
int   soulx_runtime_prepare_angle(void);                         // 0 = SoulX's ANGLE is in place (idempotent)

// ---------------------------------------------------------------------------
// registry.c - persistent instances and their runtime state
// ---------------------------------------------------------------------------

typedef struct {
    int  id;                     // stable numeric id (1..), never reused while the instance exists
    char name[SOULX_NAME_MAX + 1];
    char image[16];              // package key of the system image ("play", ...)
    int  cores;
    int  ram_mb;
    int  data_gb;
    int  width;                  // panel width in pixels (natural orientation)
    int  height;
    int  dpi;
    bool landscape;              // hw.initialOrientation=landscape (width > height panel)
    char gpu[24];                // "metal" (default) | "host" | "host-novulkan" | "software" (see soulx_mac_start)
    char dns[128];               // "" = host resolvers, else "8.8.8.8,8.8.4.4"
    bool stay_awake;             // keep the screen on while running (svc power stayon true)
    bool lean;                   // disable the preinstalled Google apps games never need, fewer cached
                                 // background apps (post-boot; off enables those apps again)
    int  fps;                    // guest display refresh rate (hw.lcd.vsync), 20-60: games render at most this often
    bool audio;                  // sound output (hw.audioOutput); off saves the audio thread's CPU
    char created[32];            // ISO-8601 UTC
    int  last_port;              // console port of the previous run (0 = none); soulx_mac_start tries it first so
                                 // the adb serial stays stable across stop/start (persisted as last_port=)
} SoulXInstanceConf;

typedef struct {
    pid_t pid;                   // engine (qemu-system-aarch64 / emulator launcher) pid
    int   port;                  // console port; serial = emulator-<port>; adb port = port + 1
    bool  headless;              // started without SoulX's window (the engine itself ALWAYS runs with -no-window)
    char  started[32];           // ISO-8601 UTC
} SoulXRunState;

void soulx_conf_defaults(SoulXInstanceConf* c, const char* name);   // balanced profile, default image
int  soulx_conf_apply_profile(SoulXInstanceConf* c, const char* profile); // "lite" | "balanced" | "performance"
const char* soulx_conf_profile_name(const SoulXInstanceConf* c);     // matches a profile, else "custom"
bool soulx_name_valid(const char* name);                // ^[a-z0-9][a-z0-9_-]{0,31}$
int  soulx_reg_load(const char* name, SoulXInstanceConf* out);        // 0 ok, -1 not found / invalid
int  soulx_reg_save(const SoulXInstanceConf* c);                       // atomic write
int  soulx_reg_delete(const char* name);
int  soulx_reg_list(SoulXInstanceConf* out, int max);                  // returns count, sorted by id
int  soulx_reg_find_by_id(int id, SoulXInstanceConf* out);
int  soulx_reg_next_id(void);
int  soulx_resolve_instance(const char* name_or_id, SoulXInstanceConf* out);
                                                       // NULL/"" -> $SOULX_INSTANCE or "default"; digits -> id
int  soulx_state_load(const char* name, SoulXRunState* out);           // 0 ok
int  soulx_state_save(const char* name, const SoulXRunState* st);
int  soulx_state_clear(const char* name);
bool soulx_instance_running(const char* name, SoulXRunState* st_out);  // state file + live engine pid
void soulx_avd_name(const char* name, char* buf, size_t cap);         // "soulx_<name>"
int  soulx_lock_acquire(void);                          // exclusive flock on run/lock; returns fd or -1
void soulx_lock_release(int fd);

// ---------------------------------------------------------------------------
// avd.c - device definition files
// ---------------------------------------------------------------------------

int soulx_avd_write(const SoulXInstanceConf* c);         // writes avd/soulx_<name>.ini + soulx_<name>.avd/config.ini
int soulx_avd_remove(const SoulXInstanceConf* c);        // removes both

// ---------------------------------------------------------------------------
// adb.c - SoulX's own adb (private server port)
// ---------------------------------------------------------------------------

void soulx_serial(int port, char* buf, size_t cap);     // "emulator-<port>"
int  soulx_adb_start_server(void);
int  soulx_adb(const char* serial, const char* const* args, SoulXCmdResult* res, int timeout_ms);
                                                       // runs: adb -s <serial> args... (serial may be NULL)
int  soulx_adb_passthrough(const char* serial, const char* const* args);   // inherits stdio
int  soulx_adb_shell(const char* serial, const char* shell_cmd, char** out, int timeout_ms);
                                                       // adb -s serial shell <shell_cmd>; *out malloc'd (may be NULL)
int  soulx_adb_exec_out(const char* serial, const char* const* args, unsigned char** data, size_t* len, int timeout_ms);
int  soulx_adb_getprop(const char* serial, const char* prop, char* buf, size_t cap);
int  soulx_adb_wait_boot(const char* serial, pid_t pid, int timeout_s);   // 0 when sys.boot_completed == 1
                                                       // (the public soulx_wait_boot(int id, int timeout_s) lives in engine.c)

// ---------------------------------------------------------------------------
// console.c - emulator console (TCP 127.0.0.1:<port>, auth token from home/)
// ---------------------------------------------------------------------------

typedef struct SoulXConsole SoulXConsole;

SoulXConsole* soulx_console_open(int port);              // connect + read banner + auth; NULL on failure
int  soulx_console_cmd(SoulXConsole* c, const char* cmd, char* reply, size_t cap);
                                                       // sends one line, reads until "OK" / "KO: ..."
                                                       // 0 = OK, 1 = KO (reply holds message), -1 = I/O error
void soulx_console_close(SoulXConsole* c);
int  soulx_console_oneshot(int port, const char* cmd, char* reply, size_t cap);

// ---------------------------------------------------------------------------
// engine.c - instance lifecycle (also backs the public soulx_* lifecycle API)
// ---------------------------------------------------------------------------

typedef struct {
    bool headless;               // do not open SoulX's window (the engine always runs -no-window -share-vid;
                                 // opening the window is the CLI's job: soulx_window_spawn)
    bool cold;                   // -no-snapshot-load (state is still saved on exit)
    bool wait;                   // block until boot completed
    int  timeout_s;              // boot timeout (0 = default 300 s)
} SoulXStartOpts;

int soulx_mac_create(SoulXInstanceConf* c);              // assigns id/created, validates, saves registry + AVD
int soulx_mac_reconfigure(SoulXInstanceConf* next, bool* needs_restart);
                                                       // validates + saves changed settings of an existing instance
                                                       // (same id; image fixed, storage only grows); a running one
                                                       // gets them at its next start (*needs_restart = true)
int soulx_mac_delete(const char* name);                 // refuses while running
int soulx_mac_start(const char* name, const SoulXStartOpts* opts, SoulXRunState* st_out);
int soulx_mac_stop(const char* name, bool force);       // console "kill" (saves quickboot), then SIGTERM/SIGKILL
int soulx_mac_post_boot(const char* name, const SoulXRunState* st);   // DNS fix, stay-awake, ...
int soulx_mac_require_running(const char* name_or_id, SoulXInstanceConf* conf, SoulXRunState* st, char* serial, size_t cap);
                                                       // resolves + checks running; prints a helpful error otherwise
void soulx_mac_cleanup_idle(void);                      // stops SoulX's private adb server (port 5137) when no SoulX
                                                       // instance has a live engine; safe to call at any time

// ---------------------------------------------------------------------------
// apps.c - APK inspection, install, launch
// ---------------------------------------------------------------------------

typedef struct {
    char package[256];
    char version_name[64];
    long long version_code;
    char abis[128];              // comma separated lib/<abi> found in the APK set ("" = no native code)
    bool wants_landscape;        // launcher activity requests a landscape orientation
} SoulXApkInfo;

int soulx_apk_inspect(const char* path, SoulXApkInfo* info);   // .apk/.xapk/.apks/.apkm/dir; 0 ok
int soulx_install_path(const char* serial, const char* path, SoulXApkInfo* info_out, bool reinstall);
                                                             // splits -> install-multiple, OBB -> /sdcard/Android/obb
int soulx_launch_package(const char* serial, const char* package);
int soulx_force_stop(const char* serial, const char* package);
int soulx_uninstall_package(const char* serial, const char* package);
int soulx_foreground_package(const char* serial, char* package, size_t cap);
int soulx_list_packages(const char* serial, bool third_party_only, char** out);   // newline separated, malloc'd
bool soulx_package_name_valid(const char* package);

// ---------------------------------------------------------------------------
// screen.c - framebuffer capture
// ---------------------------------------------------------------------------

typedef struct {
    int w;
    int h;
    uint8_t* rgba;               // w * h * 4 bytes, tightly packed RGBA8888
} SoulXImage;

int  soulx_screen_capture(const char* serial, SoulXImage* img);   // raw `screencap` (fast path)
int  soulx_ocr_text(const SoulXImage* img, char* out, size_t cap, bool numbers);  // Vision OCR; 0 ok (out may be "")
int  soulx_screen_save_png(const char* serial, const char* path); // screenshot to a host PNG file
void soulx_image_free(SoulXImage* img);

// ---------------------------------------------------------------------------
// input.c - touch / key / text injection through the emulator console
// ---------------------------------------------------------------------------

#define SOULX_TOUCH_DOWN 0        // same values as MotionEvent / soulx_send_touch
#define SOULX_TOUCH_UP   1
#define SOULX_TOUCH_MOVE 2
#define SOULX_MAX_FINGERS 10

typedef struct SoulXInput SoulXInput;

SoulXInput* soulx_input_open(const char* name);          // instance must be running
void soulx_input_close(SoulXInput* in);
int  soulx_input_refresh(SoulXInput* in);                // re-read display size + rotation
int  soulx_input_screen_size(SoulXInput* in, int* w, int* h);   // logical (rotated) size
int  soulx_input_touch(SoulXInput* in, int finger, int action, int x, int y);   // logical coordinates
int  soulx_input_tap(SoulXInput* in, int x, int y);
int  soulx_input_long_press(SoulXInput* in, int x, int y, int ms);
int  soulx_input_swipe(SoulXInput* in, int x1, int y1, int x2, int y2, int ms);
int  soulx_input_key(SoulXInput* in, const char* name_or_code);  // "BACK", "HOME", "KEYCODE_ENTER", "66"
int  soulx_input_text(SoulXInput* in, const char* utf8);
const char* soulx_input_serial(SoulXInput* in);
int  soulx_input_console(SoulXInput* in, const char* cmd, char* reply, size_t cap);
int  soulx_input_rotation(SoulXInput* in);               // guest display rotation 0..3 (cached, refreshed when stale)
int  soulx_input_capture(SoulXInput* in, SoulXImage* img); // fastest screen capture in LOGICAL orientation
                                                       // (shared frame / console screenshot + rotation fix-up;
                                                       // falls back to soulx_screen_capture); img->rgba malloc'd

// Rotation and display size read on another thread, so that the SoulXInput's own (e.g. the window's input
// queue) never waits for adb: query anywhere, apply on the SoulXInput's thread, and set "pushed" so that its
// own reads stop refreshing them.
typedef struct {
    int  rotation;               // 0..3, -1 = not read
    bool sizes_tried;            // `wm size` was asked for
    bool has_sizes;              // ... and parsed
    int  panel_w, panel_h;       // physical panel (natural orientation)
    int  over_w, over_h;         // `wm size` override, 0 = none
    int  adb_status;
} SoulXDisplayInfo;
int  soulx_input_query_display(const char* serial, bool sizes, SoulXDisplayInfo* d);   // one adb call; 0 = read
void soulx_input_apply_display(SoulXInput* in, const SoulXDisplayInfo* d);
void soulx_input_set_display_pushed(SoulXInput* in, bool pushed);

// ---------------------------------------------------------------------------
// hidkbd.c - virtual USB HID keyboard inside the guest (real key down/up events)
// One long-lived 'adb -s <serial> shell hid -' child reads JSON lines on stdin ("register" a keyboard + consumer
// control report descriptor, then "report" packets). Works for the adb shell user on 'user' (Play) builds.
// ---------------------------------------------------------------------------

typedef struct SoulXHidKbd SoulXHidKbd;

#define SOULX_HID_MOD_LCTRL   0x01    // keyboard report modifier bits
#define SOULX_HID_MOD_LSHIFT  0x02
#define SOULX_HID_MOD_LALT    0x04
#define SOULX_HID_MOD_LMETA   0x08

SoulXHidKbd* soulx_hidkbd_open(const char* serial);      // spawns the hid process, registers the device, waits (≤ 3 s)
                                                       // until the guest shows the new input device; NULL on failure
int  soulx_hidkbd_set(SoulXHidKbd* k, int modifiers, const int* usages, int count);
                                                       // full keyboard state: modifier bits + up to 6 pressed HID
                                                       // usages (page 0x07, e.g. 0x04 = 'a'); count 0 = all released
int  soulx_hidkbd_tap(SoulXHidKbd* k, int modifiers, int usage);   // press + release one key
int  soulx_hidkbd_consumer(SoulXHidKbd* k, int usage);   // consumer page press + release (0x224 AC Back, 0x223 AC Home,
                                                       // 0xE9 volume up, 0xEA volume down)
int  soulx_hidkbd_type(SoulXHidKbd* k, const char* ascii); // types printable ASCII + \n \t (shift handled), any length,
                                                       // paced so the guest never drops keys; -1 on non-ASCII
int  soulx_hidkbd_usage_for_char(char c, int* usage, bool* shift);  // ASCII -> HID usage (0 ok, -1 unsupported)
void soulx_hidkbd_close(SoulXHidKbd* k);                 // releases all keys, unregisters (ends the hid process)

// ---------------------------------------------------------------------------
// window/ (Objective-C, AppKit + Metal) - SoulX's own window
// The engine never shows a window: frames come from the engine's -share-vid shared memory "videmulator<port>",
// input goes through input.c (touch) and hidkbd.c (keys). One window process per instance.
// ---------------------------------------------------------------------------

int  soulx_window_run(const char* instance, int argc, char** argv);
                                                       // event loop of 'soulx window <instance>'; returns the exit code
int  soulx_window_spawn(const char* instance);          // starts '<this executable> window <instance>' detached
                                                       // (new session, output to logs/<name>.window.log); 0 ok
bool soulx_window_running(const char* instance, pid_t* pid_out);   // a live window process exists for the instance
                                                       // (pid file run/<name>.window, verified with proc_pidpath)

// ---------------------------------------------------------------------------
// script (source/script) - Lua engine, image matching, recorder
// ---------------------------------------------------------------------------

int soulx_script_run(const char* instance, const char* script_path, int argc, char** argv);  // exit code
// Like soulx_script_run, but runs the script 'repeat' times (repeat <= 0 = loop until Ctrl+C/stop) and scales every
// sleep()/replay pause by 1/speed (speed in [0.1, 10], 1.0 = normal; higher = faster). A non-zero os.exit, an error or
// an interrupt ends the loop. soulx_script_run(...) == soulx_script_run_ex(..., 1, 1.0).
int soulx_script_run_ex(const char* instance, const char* script_path, int argc, char** argv, int repeat, double speed);
int soulx_record(const char* instance, const char* out_path);                               // Ctrl+C to stop

#endif // SOULX_MAC_H
