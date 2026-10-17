/*
 * SoulX Android Emulator - macOS command line interface ("soulx")
 * Apple Silicon port: sets up SoulX's own Android 17 runtime, manages instances,
 * installs and plays games and runs Lua automation scripts.
 *
 * Everything goes through the macOS backend (source/platform/macos/soulx_mac.h);
 * no process is ever started through a shell. Logs and progress go to stderr,
 * command results to stdout.
 * Exit codes: 0 ok, 1 failure, 2 usage error, 130 interrupted (Ctrl+C).
 */

#include <arpa/inet.h>
#include <errno.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include "soulx_android.h"
#include "soulx_mac.h"

// image.c: write a captured frame as a PNG (used by the fast-path 'soulx screenshot'). SoulXImage is in
// soulx_mac.h; declared here to avoid pulling the script headers into the CLI.
int soulx_image_save_png(const char* path, const SoulXImage* img);

// Exit codes
#define CLI_EXIT_OK             0
#define CLI_EXIT_FAIL           1
#define CLI_EXIT_USAGE          2
#define CLI_EXIT_INTR           130

// Instance limits (same ranges as soulx_mac_create)
#define CLI_CORES_MIN           1
#define CLI_CORES_MAX           8
#define CLI_RAM_MIN             4096        // Android 17 (API 37) floor
#define CLI_RAM_MAX             12288
#define CLI_DATA_MIN            4
#define CLI_DATA_MAX            64
#define CLI_DIM_MIN             320
#define CLI_DIM_MAX             3840
#define CLI_DPI_MIN             120
#define CLI_DPI_MAX             640
#define CLI_FPS_MIN             20
#define CLI_FPS_MAX             60
#define CLI_TIMEOUT_MIN         10
#define CLI_TIMEOUT_MAX         3600
#define CLI_BOOT_TIMEOUT        300         // seconds, same default as soulx_mac_start
#define CLI_DNS_MAX             4

// Input and misc limits
#define CLI_COORD_MAX           65535
#define CLI_SWIPE_MS_DEFAULT    300
#define CLI_SWIPE_MS_MAX        60000
#define CLI_KEY_MAX             48
#define CLI_TEXT_MAX            4096
#define CLI_CONSOLE_MAX         1024
#define CLI_CONSOLE_REPLY       (64 * 1024)
#define CLI_LIST_MAX            512
#define CLI_MAX_OPTS            20
#define CLI_SHELL_TIMEOUT_MS    30000
#define CLI_CELL                64

#define EM_DASH                 "\xE2\x80\x94"      // UTF-8 em dash (same style as the backend messages)

#if defined(__aarch64__) || defined(__arm64__)
#define CLI_HOST_ARCH "arm64"
#elif defined(__x86_64__)
#define CLI_HOST_ARCH "x86_64"
#else
#define CLI_HOST_ARCH "unknown"
#endif

#define NOPTS(table) ((int)(sizeof(table) / sizeof((table)[0])))

// ---------------------------------------------------------------------------
// Command table types
// ---------------------------------------------------------------------------

typedef struct {
    const char* name;           // long option name without "--"
    char        short_name;     // single-letter alias, 0 = none
    bool        has_value;      // "--name VALUE" or "--name=VALUE"
} CliOpt;

typedef enum {
    CLI_INTERLEAVED,            // options anywhere; "--" ends them
    CLI_STOP_AT_POSITIONAL,     // options only before the first positional (run, console)
    CLI_PASSTHROUGH             // like CLI_STOP_AT_POSITIONAL, and any option other than -i/-v/-h
                                // starts the pass-through words (adb, shell, logcat, text)
} CliParseMode;

typedef struct {
    const char* val[CLI_MAX_OPTS];  // value of each command option (flags: "1"); NULL = not given
    char**      pos;                // positional arguments (malloc'd, NULL terminated)
    int         npos;
    const char* instance;           // -i/--instance given after the command name
    bool        help;               // -h/--help
} CliArgs;

typedef struct {
    const char* instance;           // -i/--instance given before the command name
} CliGlobal;

typedef struct CliCommand CliCommand;
typedef int (*CliHandler)(const CliCommand* self, const CliArgs* a, const CliGlobal* g);

struct CliCommand {
    const char*   name;
    CliHandler    handler;
    int           group;            // section in 'soulx help'
    const char*   synopsis;         // short form shown in the overview
    const char*   summary;          // one line shown in the overview
    const char*   usage;            // full help; its first line is the usage synopsis
    const CliOpt* opts;
    int           nopts;
    CliParseMode  mode;
    bool          uses_instance;    // accepts -i NAME
};

enum { GROUP_START, GROUP_INSTANCES, GROUP_APPS, GROUP_AUTOMATION, GROUP_TOOLS, GROUP_COUNT };

typedef struct {
    SoulXInstanceConf conf;
    SoulXRunState     st;
    char             serial[64];
} CliTarget;

static const CliCommand* find_command(const char* name);
static const CliCommand* command_table(size_t* count);

// ---------------------------------------------------------------------------
// Ctrl+C
// ---------------------------------------------------------------------------

// The first stop signal (Ctrl+C, kill, timeout(1), a closed terminal) asks the running operation to
// stop cleanly: it polls g_soulx_interrupted, lifts any held touches, finishes a recording and prints
// the stop message. A second one restores the default action and re-raises, so a wedged command can
// still be killed by signalling it again.
static void on_stop_signal(int sig) {
    if (g_soulx_interrupted) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_soulx_interrupted = 1;
}

// Installs on_stop_signal for one signal. respect_ignored keeps an inherited SIG_IGN (SIGHUP under
// nohup) in place; for the others we install even over an inherited SIG_IGN.
static void install_stop_signal(int sig, bool respect_ignored) {
    struct sigaction old;
    if (respect_ignored && sigaction(sig, NULL, &old) == 0 && old.sa_handler == SIG_IGN) {
        return;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;    // no SA_RESTART: blocking calls return EINTR so loops see the flag
    if (sigaction(sig, &sa, NULL) != 0) {
        soulx_log(SOULX_LOG_WARN, "cannot install the handler for signal %d: %s", sig, strerror(errno));
    }
}

// SIGINT and SIGTERM always stop the current operation cleanly, even when a non-interactive shell
// started us with SIGINT ignored (a background `soulx run &` stopped with `kill -INT` or `kill`, a
// `timeout` supervisor, cron, CI): without this a SIGTERM would kill us with a finger still pressed in
// the guest. SIGHUP does the same, but an inherited SIG_IGN is left alone so `nohup soulx run ... &`
// keeps surviving a closed terminal. (docs/SCRIPTING.md documents `kill <pid>` as the clean stop.)
static void install_signal_handlers(void) {
    install_stop_signal(SIGINT, false);
    install_stop_signal(SIGTERM, false);
    install_stop_signal(SIGHUP, true);
}

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

static bool copy_str(char* dst, size_t cap, const char* src) {
    if (cap == 0) {
        return false;
    }
    int n = snprintf(dst, cap, "%s", src ? src : "");
    return n >= 0 && (size_t)n < cap;
}

static bool str_append(char* dst, size_t cap, const char* src) {
    size_t len = strnlen(dst, cap);
    if (len >= cap) {
        return false;
    }
    int n = snprintf(dst + len, cap - len, "%s", src);
    return n >= 0 && (size_t)n < cap - len;
}

static char* rtrim(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    return s;
}

static const char* nz(const char* s, const char* fallback) {
    return (s && *s) ? s : fallback;
}

static bool all_digits(const char* s) {
    if (!s || !*s) {
        return false;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
    }
    return true;
}

static bool has_control_chars(const char* s) {
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            return true;
        }
    }
    return false;
}

// Replaces control characters (e.g. from an APK manifest) before printing.
static void sanitize(char* s, size_t cap) {
    if (cap == 0) {
        return;
    }
    s[cap - 1] = '\0';
    for (unsigned char* p = (unsigned char*)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            *p = '?';
        }
    }
}

static bool is_option_like(const char* s) {
    return s[0] == '-' && s[1] != '\0';
}

static bool looks_negative_number(const char* s) {
    return s[0] == '-' && s[1] >= '0' && s[1] <= '9';
}

// Strict base-10 integer: optional '-', digits only, nothing else, within [min, max].
static bool parse_int(const char* s, long min, long max, int* out) {
    if (!s) {
        return false;
    }
    const char* p = (*s == '-') ? s + 1 : s;
    if (*p < '0' || *p > '9') {
        return false;
    }
    errno = 0;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max) {
        return false;
    }
    *out = (int)v;
    return true;
}

// "1080x2400" -> width, height (each CLI_DIM_MIN..CLI_DIM_MAX)
static bool parse_resolution(const char* s, int* w, int* h) {
    const char* x = strpbrk(s, "xX");
    if (!x || x == s) {
        return false;
    }
    char wbuf[16];
    size_t n = (size_t)(x - s);
    if (n >= sizeof wbuf) {
        return false;
    }
    memcpy(wbuf, s, n);
    wbuf[n] = '\0';
    return parse_int(wbuf, CLI_DIM_MIN, CLI_DIM_MAX, w) && parse_int(x + 1, CLI_DIM_MIN, CLI_DIM_MAX, h);
}

// "1.1.1.1,8.8.8.8": up to CLI_DNS_MAX IPv4/IPv6 addresses
static bool dns_list_valid(const char* s, size_t cap) {
    if (!s || !*s || strlen(s) >= cap) {
        return false;
    }
    int count = 0;
    const char* p = s;
    while (*p) {
        size_t n = strcspn(p, ",");
        char ip[INET6_ADDRSTRLEN];
        if (n == 0 || n >= sizeof ip) {
            return false;
        }
        memcpy(ip, p, n);
        ip[n] = '\0';
        unsigned char addr[sizeof(struct in6_addr)];
        if (inet_pton(AF_INET, ip, addr) != 1 && inet_pton(AF_INET6, ip, addr) != 1) {
            return false;
        }
        if (++count > CLI_DNS_MAX) {
            return false;
        }
        p += n;
        if (*p == ',') {
            p++;
            if (*p == '\0') {
                return false;
            }
        }
    }
    return count > 0;
}

static bool gpu_mode_valid(const char* s) {
    return strcmp(s, "metal") == 0 || strcmp(s, "host") == 0 || strcmp(s, "host-novulkan") == 0 ||
           strcmp(s, "software") == 0;
}

// Joins words with single spaces; false when the result does not fit.
static bool join_words(char* const* words, int n, char* buf, size_t cap) {
    if (cap == 0) {
        return false;
    }
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (i > 0 && !str_append(buf, cap, " ")) {
            return false;
        }
        if (!str_append(buf, cap, words[i])) {
            return false;
        }
    }
    return true;
}

static size_t edit_distance(const char* s, const char* t) {
    size_t n = strlen(s);
    size_t m = strlen(t);
    if (n > 32 || m > 32) {
        return SIZE_MAX;
    }
    size_t prev[33];
    size_t cur[33];
    for (size_t j = 0; j <= m; j++) {
        prev[j] = j;
    }
    for (size_t i = 1; i <= n; i++) {
        cur[0] = i;
        for (size_t j = 1; j <= m; j++) {
            size_t best = prev[j] + 1;
            if (cur[j - 1] + 1 < best) {
                best = cur[j - 1] + 1;
            }
            size_t subst = prev[j - 1] + (s[i - 1] == t[j - 1] ? 0 : 1);
            if (subst < best) {
                best = subst;
            }
            cur[j] = best;
        }
        memcpy(prev, cur, (m + 1) * sizeof prev[0]);
    }
    return prev[m];
}

static void format_duration(long long s, char* buf, size_t cap) {
    int n;
    if (s < 60) {
        n = snprintf(buf, cap, "%llds", s);
    } else if (s < 3600) {
        n = snprintf(buf, cap, "%lldm %02llds", s / 60, s % 60);
    } else if (s < 86400) {
        n = snprintf(buf, cap, "%lldh %02lldm", s / 3600, (s % 3600) / 60);
    } else {
        n = snprintf(buf, cap, "%lldd %lldh", s / 86400, (s % 86400) / 3600);
    }
    if (n < 0 || (size_t)n >= cap) {
        copy_str(buf, cap, "?");
    }
}

// Seconds elapsed since an ISO-8601 UTC time ("2026-10-12T03:04:05Z"); -1 if unparsable.
static long long seconds_since(const char* iso) {
    int year, mon, day, hour, min, sec;
    if (!iso || sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &mon, &day, &hour, &min, &sec) != 6) {
        return -1;
    }
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = year - 1900;
    tmv.tm_mon = mon - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = hour;
    tmv.tm_min = min;
    tmv.tm_sec = sec;
    time_t then = timegm(&tmv);
    time_t now = time(NULL);
    if (then == (time_t)-1 || now == (time_t)-1) {
        return -1;
    }
    return now >= then ? (long long)(now - then) : 0;
}

// Physical memory footprint of a process (what Activity Monitor shows), 0 if unknown.
static uint64_t process_footprint_mb(pid_t pid) {
    struct rusage_info_v2 ri;
    memset(&ri, 0, sizeof ri);
    if (pid <= 0 || proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t*)&ri) != 0) {
        return 0;
    }
    return ri.ri_phys_footprint / (1024 * 1024);
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

static void print_usage_line(FILE* f, const CliCommand* cmd) {
    size_t n = strcspn(cmd->usage, "\n");
    fprintf(f, "usage: %.*s\n", (int)n, cmd->usage);
}

static int usage_error(const CliCommand* cmd, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Prints "soulx: error: <msg>" plus the usage line; returns CLI_EXIT_USAGE.
static int usage_error(const CliCommand* cmd, const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (n < 0) {
        copy_str(msg, sizeof msg, "invalid arguments");
    } else if ((size_t)n >= sizeof msg) {
        memcpy(msg + sizeof msg - 4, "...", 4);
    }
    soulx_log(SOULX_LOG_ERROR, "%s", msg);
    if (cmd) {
        print_usage_line(stderr, cmd);
    }
    if (cmd && strcmp(cmd->name, "help") != 0) {
        fprintf(stderr, "Run 'soulx help %s' for details.\n", cmd->name);
    } else {
        fputs("Run 'soulx help' to see all commands.\n", stderr);
    }
    return CLI_EXIT_USAGE;
}

static int out_of_memory(void) {
    soulx_log(SOULX_LOG_ERROR, "out of memory");
    return CLI_EXIT_FAIL;
}

static void json_str(FILE* f, const char* s) {
    fputc('"', f);
    const unsigned char* p = (const unsigned char*)(s ? s : "");
    while (*p) {
        unsigned char ch = *p;
        if (ch == '"' || ch == '\\') {
            fputc('\\', f);
            fputc(ch, f);
            p++;
        } else if (ch == '\n') {
            fputs("\\n", f);
            p++;
        } else if (ch == '\r') {
            fputs("\\r", f);
            p++;
        } else if (ch == '\t') {
            fputs("\\t", f);
            p++;
        } else if (ch < 0x20 || ch == 0x7f) {
            fprintf(f, "\\u%04x", (unsigned)ch);
            p++;
        } else if (ch < 0x80) {
            fputc(ch, f);
            p++;
        } else {
            // copy a well-formed UTF-8 sequence, replace anything else with U+FFFD
            int len = 0;
            if (ch >= 0xc2 && ch <= 0xdf) {
                len = 2;
            } else if (ch >= 0xe0 && ch <= 0xef) {
                len = 3;
            } else if (ch >= 0xf0 && ch <= 0xf4) {
                len = 4;
            }
            bool ok = len > 0;
            // The lead-byte ranges above exclude C0/C1 and F5..FF; also reject overlong, surrogate and
            // out-of-range sequences, whose first continuation byte falls outside the valid range for
            // the lead byte. p[1] is in bounds: a NUL there just fails the test.
            if (ok && len >= 2) {
                unsigned char b1 = p[1];
                if ((ch == 0xe0 && b1 < 0xa0) ||    // overlong 3-byte encoding
                    (ch == 0xed && b1 > 0x9f) ||    // UTF-16 surrogate (U+D800..U+DFFF)
                    (ch == 0xf0 && b1 < 0x90) ||    // overlong 4-byte encoding
                    (ch == 0xf4 && b1 > 0x8f)) {    // past U+10FFFF
                    ok = false;
                }
            }
            for (int k = 1; ok && k < len; k++) {
                ok = (p[k] & 0xc0) == 0x80;
            }
            if (ok) {
                fwrite(p, 1, (size_t)len, f);
                p += len;
            } else {
                fputs("\\ufffd", f);
                p++;
            }
        }
    }
    fputc('"', f);
}

static void json_str_or_null(FILE* f, const char* s) {
    if (s) {
        json_str(f, s);
    } else {
        fputs("null", f);
    }
}

// ---------------------------------------------------------------------------
// Option parsing
// ---------------------------------------------------------------------------

enum { GOPT_INSTANCE, GOPT_VERBOSE, GOPT_HELP };

static const CliOpt k_global_opts[] = {
    [GOPT_INSTANCE] = { "instance", 'i', true  },
    [GOPT_VERBOSE]  = { "verbose",  'v', false },
    [GOPT_HELP]     = { "help",     'h', false },
};

typedef struct {
    const CliOpt* opt;
    int           index;
    bool          global;
} CliOptRef;

static bool find_long_opt(const CliCommand* cmd, const char* name, size_t len, CliOptRef* ref) {
    const CliOpt* tables[2] = { cmd->opts, k_global_opts };
    int counts[2] = { cmd->nopts, NOPTS(k_global_opts) };
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < counts[t]; i++) {
            const CliOpt* o = &tables[t][i];
            if (strlen(o->name) == len && strncmp(o->name, name, len) == 0) {
                ref->opt = o;
                ref->index = i;
                ref->global = (t == 1);
                return true;
            }
        }
    }
    return false;
}

static bool find_short_opt(const CliCommand* cmd, char c, CliOptRef* ref) {
    const CliOpt* tables[2] = { cmd->opts, k_global_opts };
    int counts[2] = { cmd->nopts, NOPTS(k_global_opts) };
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < counts[t]; i++) {
            const CliOpt* o = &tables[t][i];
            if (o->short_name != 0 && o->short_name == c) {
                ref->opt = o;
                ref->index = i;
                ref->global = (t == 1);
                return true;
            }
        }
    }
    return false;
}

static const char* closest_option(const CliCommand* cmd, const char* name, size_t len) {
    char word[40];
    if (len >= sizeof word) {
        return NULL;
    }
    memcpy(word, name, len);
    word[len] = '\0';
    const char* best = NULL;
    size_t best_d = 3;
    for (int i = 0; i < cmd->nopts + NOPTS(k_global_opts); i++) {
        const CliOpt* o = i < cmd->nopts ? &cmd->opts[i] : &k_global_opts[i - cmd->nopts];
        size_t d = edit_distance(word, o->name);
        if (d < best_d) {
            best_d = d;
            best = o->name;
        }
    }
    return best;
}

static void store_opt(const CliOptRef* ref, const char* value, CliArgs* a) {
    if (!ref->global) {
        a->val[ref->index] = value;
        return;
    }
    switch (ref->index) {
    case GOPT_INSTANCE:
        a->instance = value;
        break;
    case GOPT_VERBOSE:
        soulx_set_verbose(true);
        break;
    case GOPT_HELP:
        a->help = true;
        break;
    default:
        break;
    }
}

static int parse_long_opt(const CliCommand* cmd, int argc, char** argv, int* i, CliArgs* a) {
    const char* name = argv[*i] + 2;
    const char* eq = strchr(name, '=');
    size_t len = eq ? (size_t)(eq - name) : strlen(name);
    CliOptRef ref;
    if (!find_long_opt(cmd, name, len, &ref)) {
        const char* hint = closest_option(cmd, name, len);
        if (hint) {
            return usage_error(cmd, "unknown option '--%.*s' (did you mean '--%s'?)", (int)len, name, hint);
        }
        return usage_error(cmd, "unknown option '--%.*s'", (int)len, name);
    }
    const char* value = "1";
    if (ref.opt->has_value) {
        if (eq) {
            value = eq + 1;
        } else if (*i + 1 < argc && !is_option_like(argv[*i + 1])) {
            *i += 1;
            value = argv[*i];
        } else {
            return usage_error(cmd, "option '--%s' needs a value", ref.opt->name);
        }
    } else if (eq) {
        return usage_error(cmd, "option '--%s' does not take a value", ref.opt->name);
    }
    store_opt(&ref, value, a);
    return 0;
}

static int parse_short_opts(const CliCommand* cmd, int argc, char** argv, int* i, CliArgs* a) {
    const char* arg = argv[*i];
    CliOptRef long_ref;
    if (strlen(arg) > 2 && find_long_opt(cmd, arg + 1, strlen(arg + 1), &long_ref)) {
        // "-headless" is a typo for "--headless", not the cluster "-h -e -a ..."
        return usage_error(cmd, "unknown option '%s' (did you mean '-%s'?)", arg, arg);
    }
    for (const char* p = arg + 1; *p; p++) {
        CliOptRef ref;
        if (!find_short_opt(cmd, *p, &ref)) {
            return usage_error(cmd, "unknown option '-%c'", *p);
        }
        if (ref.opt->has_value) {
            const char* value;
            if (p[1] != '\0') {
                value = p + 1;
            } else if (*i + 1 < argc && !is_option_like(argv[*i + 1])) {
                *i += 1;
                value = argv[*i];
            } else {
                return usage_error(cmd, "option '-%c' needs a value", *p);
            }
            store_opt(&ref, value, a);
            return 0;
        }
        store_opt(&ref, "1", a);
    }
    return 0;
}

// In CLI_PASSTHROUGH mode (adb, shell, logcat, text) only -i NAME / --instance[=]NAME before the first
// pass-through word are SoulX's. -v, -h, --help and every other option are the pass-through command's own
// (so `soulx logcat -v threadtime` sends -v to adb, not to SoulX; `soulx adb --help` runs adb's help). Use
// 'soulx help adb' for SoulX's help, and `--` to pass any remaining SoulX-looking option through verbatim.
static bool is_passthrough_instance_option(const char* arg) {
    return strcmp(arg, "-i") == 0 || strcmp(arg, "--instance") == 0 || strncmp(arg, "--instance=", 11) == 0;
}

// Parses the arguments that follow the command name. Returns 0, or the exit code of an
// error that was already reported. a->pos must be freed by the caller in every case.
static int cli_parse(const CliCommand* cmd, int argc, char** argv, CliArgs* a) {
    memset(a, 0, sizeof *a);
    a->pos = calloc((size_t)argc + 1, sizeof *a->pos);
    if (!a->pos) {
        return out_of_memory();
    }
    if (cmd->nopts > CLI_MAX_OPTS) {
        soulx_log(SOULX_LOG_ERROR, "internal error: too many options for '%s'", cmd->name);
        return CLI_EXIT_FAIL;
    }
    bool options_done = false;
    for (int i = 0; i < argc; i++) {
        char* arg = argv[i];
        if (!options_done && strcmp(arg, "--") == 0) {
            options_done = true;
            continue;
        }
        bool option = !options_done && is_option_like(arg) && !looks_negative_number(arg);
        if (option && (cmd->mode != CLI_PASSTHROUGH || is_passthrough_instance_option(arg))) {
            int rc = (arg[1] == '-') ? parse_long_opt(cmd, argc, argv, &i, a)
                                     : parse_short_opts(cmd, argc, argv, &i, a);
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        a->pos[a->npos++] = arg;
        if (cmd->mode != CLI_INTERLEAVED) {
            // everything after the first word belongs to the command (adb args, script args, ...)
            for (int k = i + 1; k < argc; k++) {
                a->pos[a->npos++] = argv[k];
            }
            break;
        }
    }
    return 0;
}

static int no_positionals(const CliCommand* self, const CliArgs* a) {
    if (a->npos > 0) {
        return usage_error(self, "unexpected argument '%s'", a->pos[0]);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Instances and runtime
// ---------------------------------------------------------------------------

static const char* default_instance_name(void) {
    const char* env = getenv("SOULX_INSTANCE");
    return (env && *env) ? env : SOULX_DEFAULT_INSTANCE;
}

// A reference to an existing instance: its name or its numeric id (the registry resolves
// all-digit references as an id first).
static bool instance_ref_valid(const char* s) {
    return s && *s && soulx_name_valid(s);
}

// A name for a new instance (all-digit names would be read as ids).
static bool new_name_valid(const char* s) {
    return s && soulx_name_valid(s) && !all_digits(s);
}

static int bad_instance_error(const CliCommand* cmd, const char* ref) {
    const char* env = getenv("SOULX_INSTANCE");
    bool from_env = env && strcmp(env, ref) == 0;
    return usage_error(cmd, "invalid instance '%s'%s: use an instance name (lowercase letters, digits, "
                       "'-' and '_') or its id", ref, from_env ? " (from $SOULX_INSTANCE)" : "");
}

static int bad_new_name_error(const CliCommand* cmd, const char* name) {
    return usage_error(cmd, "invalid name '%s': use 1-%d lowercase letters, digits, '-' or '_', starting "
                       "with a letter or digit (not digits only)", name, SOULX_NAME_MAX);
}

// Instance given as the optional [name] argument or with -i (else $SOULX_INSTANCE or "default").
static int instance_from_args(const CliCommand* self, const CliArgs* a, const CliGlobal* g, const char** out) {
    if (a->npos > 1) {
        return usage_error(self, "too many arguments: '%s'", a->pos[1]);
    }
    const char* opt = a->instance ? a->instance : g->instance;
    const char* name = a->npos == 1 ? a->pos[0] : NULL;
    if (name && opt && strcmp(name, opt) != 0) {
        return usage_error(self, "two different instances given: '%s' and '%s'", name, opt);
    }
    if (!name) {
        name = opt ? opt : default_instance_name();
    }
    if (!instance_ref_valid(name)) {
        return bad_instance_error(self, name);
    }
    *out = name;
    return 0;
}

static void no_such_instance(const char* ref) {
    if (all_digits(ref)) {
        soulx_log(SOULX_LOG_ERROR, "no such instance '%s' (see: soulx list)", ref);
    } else if (strcmp(ref, default_instance_name()) == 0) {
        soulx_log(SOULX_LOG_ERROR, "no such instance '%s' " EM_DASH " create it with: soulx create "
                 "(or just run: soulx play game.apk)", ref);
    } else {
        soulx_log(SOULX_LOG_ERROR, "no such instance '%s' " EM_DASH " create it with: soulx create %s", ref, ref);
    }
}

// Resolves an instance name or id; prints a helpful error when there is no such instance.
static int load_instance(const char* ref, SoulXInstanceConf* conf) {
    memset(conf, 0, sizeof *conf);
    if (soulx_resolve_instance(ref, conf) == 0) {
        return 0;
    }
    no_such_instance(ref);
    return -1;
}

// The running instance selected with -i (else $SOULX_INSTANCE or "default").
static int cli_target(const CliCommand* self, const CliArgs* a, const CliGlobal* g, CliTarget* t) {
    const char* ref = a->instance ? a->instance : g->instance;
    if (!ref) {
        ref = default_instance_name();
    }
    if (!instance_ref_valid(ref)) {
        return bad_instance_error(self, ref);
    }
    memset(t, 0, sizeof *t);
    if (soulx_mac_require_running(ref, &t->conf, &t->st, t->serial, sizeof t->serial) != 0) {
        return CLI_EXIT_FAIL;
    }
    return 0;
}

static void make_landscape(SoulXInstanceConf* c) {
    if (c->width < c->height) {
        int w = c->width;
        c->width = c->height;
        c->height = w;
    }
    c->landscape = true;
}

static const SoulXPackage* find_image(const char* key) {
    const SoulXPackage* p = key ? soulx_runtime_find(key) : NULL;
    return (p && p->kind == SOULX_PKG_IMAGE) ? p : NULL;
}

static int unknown_image_error(const CliCommand* cmd, const char* key) {
    char list[256] = "";
    size_t n = 0;
    const SoulXPackage* pk = soulx_runtime_packages(&n);
    for (size_t i = 0; pk && i < n; i++) {
        if (pk[i].kind != SOULX_PKG_IMAGE) {
            continue;
        }
        if ((list[0] && !str_append(list, sizeof list, ", ")) || !str_append(list, sizeof list, pk[i].key)) {
            break;
        }
    }
    return usage_error(cmd, "unknown system image '%s' (available: %s)", key, nz(list, "none"));
}

// "Android 17, Google Play"
static void android_label(const char* image_key, char* buf, size_t cap) {
    const SoulXPackage* img = find_image(image_key);
    int n = img ? snprintf(buf, cap, "%s, %s", nz(img->android_name, "Android"), nz(img->tag_display, img->key))
                : snprintf(buf, cap, "unknown image '%s'", image_key);
    if (n < 0 || (size_t)n >= cap) {
        copy_str(buf, cap, image_key);
    }
}

// Bytes still to download for the engine, adb and the given image (all of them when !only_missing).
static uint64_t runtime_download_bytes(const char* image_key, bool only_missing) {
    const char* keys[3] = { "emulator", "platform-tools", image_key };
    uint64_t total = 0;
    for (size_t k = 0; k < 3; k++) {
        const SoulXPackage* p = keys[k] ? soulx_runtime_find(keys[k]) : NULL;
        if (p && (!only_missing || !soulx_runtime_installed(p))) {
            total += p->size;
        }
    }
    return total;
}

// True when the runtime for the instance's image is installed; otherwise explains how to get it
// (never downloads gigabytes without the user asking for it).
static bool runtime_ready_or_explain(const SoulXInstanceConf* conf) {
    const SoulXPackage* img = find_image(conf->image);
    if (!img) {
        soulx_log(SOULX_LOG_ERROR, "instance '%s' uses an unknown system image '%s'", conf->name, conf->image);
        return false;
    }
    if (soulx_runtime_ready(img->key)) {
        return true;
    }
    const SoulXPackage* def = soulx_runtime_default_image();
    bool is_default = def && strcmp(def->key, img->key) == 0;
    double gb = (double)runtime_download_bytes(img->key, true) / 1e9;
    soulx_log(SOULX_LOG_ERROR, "the %s (%s) runtime is not installed yet", nz(img->android_name, "Android"),
             nz(img->tag_display, img->key));
    fprintf(stderr, "Install it once (about %.1f GB download) with:\n  soulx setup%s%s\n"
            "then run this command again.\n", gb, is_default ? "" : " --image ", is_default ? "" : img->key);
    return false;
}

static int instance_log_path(const char* name, char* buf, size_t cap) {
    return soulx_path(buf, cap, "logs/%s.log", name);
}

// Starts the instance if needed and waits until Android has finished booting.
static int ensure_booted(const SoulXInstanceConf* conf, bool headless, SoulXRunState* st, char* serial, size_t cap) {
    bool was_running = soulx_instance_running(conf->name, st);
    if (!was_running) {
        SoulXStartOpts so = { .headless = headless, .cold = false, .wait = true, .timeout_s = 0 };
        if (soulx_mac_start(conf->name, &so, st) != 0) {
            return -1;
        }
        if (!soulx_instance_running(conf->name, st)) {
            char log_path[PATH_MAX];
            if (instance_log_path(conf->name, log_path, sizeof log_path) != 0) {
                copy_str(log_path, sizeof log_path, "its log in $SOULX_HOME/logs");
            }
            soulx_log(SOULX_LOG_ERROR, "instance '%s' stopped right after starting (see %s)", conf->name, log_path);
            return -1;
        }
    } else if (headless && !st->headless) {
        soulx_log(SOULX_LOG_INFO, "note: '%s' is already running with a window; --headless applies to its next start",
                 conf->name);
    }
    soulx_serial(st->port, serial, cap);
    char prop[16] = "";
    bool booted = soulx_adb_getprop(serial, "sys.boot_completed", prop, sizeof prop) == 0 &&
                  strcmp(rtrim(prop), "1") == 0;
    if (!booted && soulx_adb_wait_boot(serial, st->pid, CLI_BOOT_TIMEOUT) != 0) {
        if (!g_soulx_interrupted) {
            soulx_log(SOULX_LOG_ERROR, "Android on '%s' did not finish booting", conf->name);
        }
        return -1;
    }
    if (was_running) {
        // It was started without waiting (soulx start --no-wait), or was already up when we found it:
        // run the post-boot setup (stay awake, private DNS off) now. Idempotent, one adb round trip.
        // A fresh start we did above already ran it in finish_boot.
        soulx_mac_post_boot(conf->name, st);
    }
    return 0;
}

// Opens SoulX's window for the instance (unless headless), or activates it when it is already open:
// soulx_window_spawn starts a detached '<this program> window <name>', whose soulx_window_run brings an
// existing window to the front and exits instead of opening a second one. The engine itself always runs
// headless (-no-window); this is the only window the user sees. Best effort: the instance keeps running
// if the window cannot be opened.
static void open_window_unless_headless(const char* name, bool headless) {
    if (headless) {
        return;
    }
    if (soulx_window_spawn(name) != 0) {
        soulx_log(SOULX_LOG_WARN, "could not open the SoulX window for '%s'; it keeps running "
                 "(open it later with: soulx window %s)", name, name);
    }
}

// ---------------------------------------------------------------------------
// Apps
// ---------------------------------------------------------------------------

static bool abi_supported(const char* abis) {
    const char* p = abis;
    bool any = false;
    while (*p) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        size_t n = strcspn(p, ",");
        size_t m = n;
        while (m > 0 && p[m - 1] == ' ') {
            m--;
        }
        if (m > 0) {
            any = true;
            if (m == 9 && strncmp(p, "arm64-v8a", 9) == 0) {
                return true;
            }
        }
        p += n;
    }
    return !any;    // no native code at all: runs anywhere
}

static bool has_app_extension(const char* path) {
    static const char* const exts[] = { ".apk", ".xapk", ".apks", ".apkm" };
    size_t len = strlen(path);
    for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++) {
        size_t e = strlen(exts[i]);
        if (len > e && strcasecmp(path + len - e, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

// Inspects an app file and checks it can run on SoulX's arm64 Android 17 images.
static int inspect_app(const char* path, SoulXApkInfo* info) {
    memset(info, 0, sizeof *info);
    if (soulx_apk_inspect(path, info) != 0) {
        // soulx_apk_inspect already logged the specific reason. Add the list of supported formats only
        // when the file is not one SoulX recognises (a wrong extension), not for a corrupt .apk/.xapk,
        // where it would read as if the format were unsupported.
        struct stat sb;
        bool is_dir = stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
        if (!is_dir && !has_app_extension(path)) {
            soulx_log(SOULX_LOG_ERROR, "cannot install %s: SoulX installs .apk, .xapk, .apks and .apkm files and "
                     "folders of split APKs", path);
        }
        return -1;
    }
    info->package[sizeof info->package - 1] = '\0';
    sanitize(info->version_name, sizeof info->version_name);
    sanitize(info->abis, sizeof info->abis);
    if (!soulx_package_name_valid(info->package)) {
        soulx_log(SOULX_LOG_ERROR, "%s: the app manifest has no valid package name", path);
        return -1;
    }
    if (!abi_supported(info->abis)) {
        soulx_log(SOULX_LOG_ERROR, "%s only contains native code for %s; Android 17 on Apple Silicon runs 64-bit "
                 "ARM apps (arm64-v8a) only", path, info->abis);
        return -1;
    }
    return 0;
}

static void version_label(const SoulXApkInfo* info, char* buf, size_t cap) {
    int n = info->version_name[0]
        ? snprintf(buf, cap, "%s (%lld)", info->version_name, info->version_code)
        : snprintf(buf, cap, "(version %lld)", info->version_code);
    if (n < 0 || (size_t)n >= cap) {
        n = snprintf(buf, cap, "(version %lld)", info->version_code);
        if (n < 0 || (size_t)n >= cap) {
            copy_str(buf, cap, "");
        }
    }
}

// 1 = installed (*code = its versionCode, -1 if unknown), 0 = not installed, -1 = could not tell.
static int installed_version(const char* serial, const char* package, long long* code) {
    *code = -1;
    if (!soulx_package_name_valid(package)) {
        return -1;
    }
    char cmd[320];
    int n = snprintf(cmd, sizeof cmd, "pm list packages --show-versioncode %s", package);
    if (n < 0 || (size_t)n >= sizeof cmd) {
        return -1;
    }
    char* out = NULL;
    if (soulx_adb_shell(serial, cmd, &out, CLI_SHELL_TIMEOUT_MS) != 0 || !out) {
        free(out);
        return -1;
    }
    int found = 0;
    bool unexpected = false;
    size_t plen = strlen(package);
    char* line = out;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        rtrim(line);
        if (strncmp(line, "package:", 8) == 0) {
            const char* rest = line + 8;
            if (strncmp(rest, package, plen) == 0 && (rest[plen] == '\0' || rest[plen] == ' ')) {
                found = 1;
                const char* v = strstr(rest + plen, "versionCode:");
                if (v) {
                    errno = 0;
                    char* end = NULL;
                    long long x = strtoll(v + 12, &end, 10);
                    if (errno == 0 && end != v + 12) {
                        *code = x;
                    }
                }
                break;
            }
        } else if (*line) {
            unexpected = true;      // e.g. an error message from pm
        }
        line = next;
    }
    free(out);
    if (!found && unexpected) {
        return -1;
    }
    return found;
}

// Installs an inspected app unless the same version is already installed (or reinstall).
// Results go to stdout (soulx install) or to the log (soulx play). 0 = installed or up to date.
static int install_inspected(const char* serial, const char* path, const SoulXApkInfo* info, bool reinstall,
                             bool results_to_stdout) {
    char ver[96];
    version_label(info, ver, sizeof ver);
    // Ask first only to tell a downgrade apart and to word the result. The backend decides whether to
    // install or skip (same version), and it always syncs the app's OBB data, so a game whose OBB copy
    // failed before is repaired on the next install instead of being skipped outright.
    // soulx_install_path logs its own progress ("Installing ..." / "... is already installed"); the CLI
    // adds only the one-line stdout result, with no duplicated stderr log.
    long long have = -1;
    int state = installed_version(serial, info->package, &have);
    bool up_to_date = state == 1 && have == info->version_code && !reinstall;
    if (soulx_install_path(serial, path, NULL, reinstall) != 0) {
        if (!g_soulx_interrupted && state == 1 && have > info->version_code) {
            soulx_log(SOULX_LOG_INFO, "a newer version (%lld) is installed; to downgrade, uninstall it first: "
                     "soulx uninstall %s", have, info->package);
        }
        return -1;
    }
    if (results_to_stdout) {
        if (up_to_date) {
            printf("%s %s is already installed (use --reinstall to install it again)\n", info->package, ver);
        } else {
            printf("Installed %s %s\n", info->package, ver);
        }
    }
    return 0;
}

static int cmp_cstr(const void* x, const void* y) {
    const char* const* a = x;
    const char* const* b = y;
    return strcmp(*a, *b);
}

// ---------------------------------------------------------------------------
// Legacy v1.0 options (README: soulx --create NAME, --start ID, --adb ID "args", ...)
// ---------------------------------------------------------------------------

typedef struct {
    const char* flag;           // v1.0 option
    const char* command;        // command it maps to
    int         nargs;          // arguments it requires
    const char* args_help;      // shown when they are missing
} CliLegacy;

static const CliLegacy k_legacy[] = {
    { "--create",  "create",  1, "--create NAME" },
    { "--start",   "start",   1, "--start ID|NAME" },
    { "--stop",    "stop",    1, "--stop ID|NAME" },
    { "--list",    "list",    0, "--list" },
    { "--delete",  "delete",  1, "--delete ID|NAME" },
    { "--install", "install", 2, "--install ID|NAME APK" },
    { "--adb",     "adb",     2, "--adb ID|NAME \"ARGS\"" },
    { "--help",    "help",    0, "--help" },
    { "--version", "version", 0, "--version" },
};

static const CliLegacy* find_legacy(const char* flag) {
    for (size_t i = 0; i < sizeof k_legacy / sizeof k_legacy[0]; i++) {
        if (strcmp(k_legacy[i].flag, flag) == 0) {
            return &k_legacy[i];
        }
    }
    return NULL;
}

// v1.0 accepted free-form names ("Gaming Instance"); turn them into a valid name ("gaming-instance").
static bool slugify_name(const char* in, char* out, size_t cap) {
    if (cap < 2) {
        return false;
    }
    size_t max = (cap - 1 < SOULX_NAME_MAX) ? cap - 1 : SOULX_NAME_MAX;
    size_t n = 0;
    bool pending_dash = false;
    for (const unsigned char* p = (const unsigned char*)in; *p && n < max; p++) {
        unsigned char ch = *p;
        if (ch >= 'A' && ch <= 'Z') {
            ch = (unsigned char)(ch - 'A' + 'a');
        }
        bool keep = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || (ch == '_' && n > 0);
        if (!keep) {
            pending_dash = n > 0;
            continue;
        }
        if (pending_dash) {
            if (n + 1 >= max) {
                break;
            }
            out[n++] = '-';
            pending_dash = false;
        }
        out[n++] = (char)ch;
    }
    while (n > 0 && (out[n - 1] == '-' || out[n - 1] == '_')) {
        n--;
    }
    out[n] = '\0';
    return n > 0 && new_name_valid(out);
}

// Splits a v1.0 "--adb" argument string into words: blanks separate words, '...' and "..." group,
// a backslash escapes the next character (outside single quotes). Nothing is expanded or run.
// Words are written into buf (at least strlen(s) + 1 bytes). Returns the word count, -1 on error.
static int split_words(const char* s, char* buf, char** words, int max_words) {
    int count = 0;
    char* o = buf;
    const char* p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') {
            p++;
        }
        if (!*p) {
            break;
        }
        if (count >= max_words) {
            return -1;
        }
        words[count++] = o;
        char quote = 0;
        while (*p) {
            char ch = *p;
            if (quote) {
                if (ch == quote) {
                    quote = 0;
                    p++;
                } else if (ch == '\\' && quote == '"' && (p[1] == '"' || p[1] == '\\')) {
                    *o++ = p[1];
                    p += 2;
                } else {
                    *o++ = ch;
                    p++;
                }
                continue;
            }
            if (ch == ' ' || ch == '\t' || ch == '\n') {
                break;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                p++;
            } else if (ch == '\\' && p[1] != '\0') {
                *o++ = p[1];
                p += 2;
            } else {
                *o++ = ch;
                p++;
            }
        }
        if (quote) {
            return -1;
        }
        *o++ = '\0';
    }
    return count;
}

static int dispatch(const CliCommand* cmd, int argc, char** argv, const CliGlobal* g);

static int legacy_create(const CliCommand* cmd, int argc, char** argv, const CliGlobal* g) {
    char name[SOULX_NAME_MAX + 1];
    if (!slugify_name(argv[0], name, sizeof name)) {
        return usage_error(cmd, "cannot make an instance name out of '%s'; use letters and digits, "
                           "e.g.: soulx create gaming", argv[0]);
    }
    if (strcmp(name, argv[0]) != 0) {
        soulx_log(SOULX_LOG_INFO, "using the instance name '%s' for \"%s\"", name, argv[0]);
    }
    char** nargv = calloc((size_t)argc + 1, sizeof *nargv);
    if (!nargv) {
        return out_of_memory();
    }
    nargv[0] = name;
    for (int i = 1; i < argc; i++) {
        nargv[i] = argv[i];
    }
    int rc = dispatch(cmd, argc, nargv, g);
    free(nargv);
    return rc;
}

static int legacy_adb(const CliCommand* cmd, int argc, char** argv, const CliGlobal* g) {
    static char k_end_of_options[] = "--";
    const char* s = argv[1];
    size_t len = strlen(s);
    size_t max_words = len / 2 + 2;
    char* buf = malloc(len + 1);
    char** nargv = calloc(max_words + (size_t)argc + 1, sizeof *nargv);
    if (!buf || !nargv) {
        free(buf);
        free(nargv);
        return out_of_memory();
    }
    nargv[0] = k_end_of_options;        // everything after it goes to adb verbatim
    int nw = split_words(s, buf, nargv + 1, (int)max_words);
    int rc;
    if (nw < 0) {
        rc = usage_error(cmd, "unbalanced quotes in \"%s\"", s);
    } else {
        int n = 1 + nw;
        for (int i = 2; i < argc; i++) {
            nargv[n++] = argv[i];
        }
        CliGlobal lg = *g;
        lg.instance = argv[0];
        rc = dispatch(cmd, n, nargv, &lg);
    }
    free(nargv);
    free(buf);
    return rc;
}

static int run_legacy(const CliLegacy* lg, int argc, char** argv, const CliGlobal* g) {
    const CliCommand* cmd = find_command(lg->command);
    if (!cmd) {
        return CLI_EXIT_FAIL;
    }
    // the ID|NAME must not look like an option; the "ARGS" of --adb may ("-d shell ls")
    if (argc < lg->nargs || (lg->nargs > 0 && is_option_like(argv[0]))) {
        return usage_error(cmd, "%s needs more arguments (usage: soulx %s)", lg->flag, lg->args_help);
    }
    if (strcmp(lg->command, "create") == 0) {
        return legacy_create(cmd, argc, argv, g);
    }
    if (strcmp(lg->command, "adb") == 0) {
        return legacy_adb(cmd, argc, argv, g);
    }
    if (strcmp(lg->command, "install") == 0) {
        CliGlobal ig = *g;
        ig.instance = argv[0];
        return dispatch(cmd, argc - 1, argv + 1, &ig);
    }
    return dispatch(cmd, argc, argv, g);    // start/stop/delete ID|NAME, list, help, version
}

// ---------------------------------------------------------------------------
// Commands: getting started
// ---------------------------------------------------------------------------

enum { SETUP_IMAGE, SETUP_ACCEPT, SETUP_KEEP, SETUP_FORCE };
static const CliOpt k_setup_opts[] = {
    [SETUP_IMAGE]  = { "image",           0, true  },
    [SETUP_ACCEPT] = { "accept-licenses", 0, false },
    [SETUP_KEEP]   = { "keep-downloads",  0, false },
    [SETUP_FORCE]  = { "force",           0, false },
};

static int cmd_setup(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    (void)g;
    const char* key = a->val[SETUP_IMAGE];
    if (a->npos > 1) {
        return usage_error(self, "too many arguments: '%s'", a->pos[1]);
    }
    if (a->npos == 1) {         // "soulx setup google" works too
        if (key && strcmp(key, a->pos[0]) != 0) {
            return usage_error(self, "two different images given: '%s' and '%s'", key, a->pos[0]);
        }
        key = a->pos[0];
    }
    if (!key) {
        const SoulXPackage* def = soulx_runtime_default_image();
        if (!def) {
            soulx_log(SOULX_LOG_ERROR, "internal error: no default system image");
            return CLI_EXIT_FAIL;
        }
        key = def->key;
    }
    const SoulXPackage* img = find_image(key);
    if (!img) {
        return unknown_image_error(self, key);
    }
    SoulXSetupOpts so = {
        .accept_licenses = a->val[SETUP_ACCEPT] != NULL,
        .keep_downloads  = a->val[SETUP_KEEP] != NULL,
        .force           = a->val[SETUP_FORCE] != NULL,
    };
    if (soulx_runtime_setup(img->key, &so) != 0) {
        return g_soulx_interrupted ? CLI_EXIT_INTR : CLI_EXIT_FAIL;
    }
    printf("\nNext: soulx play game.apk   (creates an instance, installs your game and launches it)\n");
    return CLI_EXIT_OK;
}

enum { PLAY_HEADLESS, PLAY_REINSTALL };
static const CliOpt k_play_opts[] = {
    [PLAY_HEADLESS]  = { "headless",  0, false },
    [PLAY_REINSTALL] = { "reinstall", 0, false },
};

static int cmd_play(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos == 0) {
        return usage_error(self, "what should I play? Give an APK file or a package name, e.g.: soulx play game.apk");
    }
    if (a->npos > 1) {
        return usage_error(self, "too many arguments: '%s'", a->pos[1]);
    }
    const char* target = a->pos[0];
    bool headless = a->val[PLAY_HEADLESS] != NULL;
    bool reinstall = a->val[PLAY_REINSTALL] != NULL;

    // 1. What to play: an app file (checked now, before any slow step) or an installed package
    SoulXApkInfo info;
    memset(&info, 0, sizeof info);
    struct stat sb;
    bool is_file = stat(target, &sb) == 0;
    int stat_errno = errno;
    const char* pkg;
    if (is_file) {
        if (inspect_app(target, &info) != 0) {
            return CLI_EXIT_FAIL;
        }
        pkg = info.package;
    } else if (has_app_extension(target) || strchr(target, '/')) {
        soulx_log(SOULX_LOG_ERROR, "%s: %s", target, strerror(stat_errno));
        return CLI_EXIT_FAIL;
    } else if (soulx_package_name_valid(target)) {
        pkg = target;
    } else {
        return usage_error(self, "'%s' is neither an app file nor a package name", target);
    }

    // 2. The instance: an existing one, or a new one (landscape screen when the game asks for it)
    const char* ref = a->instance ? a->instance : g->instance;
    if (!ref) {
        ref = default_instance_name();
    }
    if (!instance_ref_valid(ref)) {
        return bad_instance_error(self, ref);
    }
    SoulXInstanceConf conf;
    memset(&conf, 0, sizeof conf);
    bool create = false;
    if (soulx_resolve_instance(ref, &conf) != 0) {
        if (all_digits(ref)) {
            no_such_instance(ref);
            return CLI_EXIT_FAIL;
        }
        if (!is_file) {
            soulx_log(SOULX_LOG_ERROR, "no such instance '%s', so %s is not installed on it " EM_DASH " pass the "
                     "game's APK file instead: soulx play game.apk", ref, pkg);
            return CLI_EXIT_FAIL;
        }
        soulx_conf_defaults(&conf, ref);
        if (info.wants_landscape) {
            make_landscape(&conf);
        }
        create = true;
    } else if (is_file && info.wants_landscape && !conf.landscape) {
        soulx_log(SOULX_LOG_INFO, "note: %s prefers landscape but '%s' has a portrait screen; for a landscape "
                 "instance: soulx create NAME --landscape", pkg, conf.name);
    }

    // 3. Runtime: never downloaded without the user asking for it. Checked before the instance
    //    is created, so a refused play leaves nothing behind.
    if (!runtime_ready_or_explain(&conf)) {
        return CLI_EXIT_FAIL;
    }
    if (create) {
        soulx_log(SOULX_LOG_INFO, "creating instance '%s' (%dx%d %s screen) for %s", conf.name, conf.width,
                 conf.height, conf.landscape ? "landscape" : "portrait", pkg);
        if (soulx_mac_create(&conf) != 0) {
            return CLI_EXIT_FAIL;
        }
    }
    if (g_soulx_interrupted) {
        return CLI_EXIT_INTR;
    }

    // 4. Start and wait for Android
    SoulXRunState st;
    char serial[64];
    memset(&st, 0, sizeof st);
    if (ensure_booted(&conf, headless, &st, serial, sizeof serial) != 0) {
        return g_soulx_interrupted ? CLI_EXIT_INTR : CLI_EXIT_FAIL;
    }

    // 5. Install (skipped when this version is already there)
    if (is_file) {
        if (install_inspected(serial, target, &info, reinstall, false) != 0) {
            return g_soulx_interrupted ? CLI_EXIT_INTR : CLI_EXIT_FAIL;
        }
    } else {
        long long have = -1;
        if (installed_version(serial, pkg, &have) == 0) {
            soulx_log(SOULX_LOG_ERROR, "%s is not installed on '%s'; pass the game's APK file instead: soulx play "
                     "game.apk", pkg, conf.name);
            return CLI_EXIT_FAIL;
        }
    }
    if (g_soulx_interrupted) {
        return CLI_EXIT_INTR;
    }

    // 6. Launch
    if (soulx_launch_package(serial, pkg) != 0) {
        soulx_log(SOULX_LOG_ERROR, "could not launch %s on '%s'", pkg, conf.name);
        return CLI_EXIT_FAIL;
    }
    printf("Playing %s on %s\n", pkg, conf.name);
    open_window_unless_headless(conf.name, headless);
    return CLI_EXIT_OK;
}

// ---------------------------------------------------------------------------
// Commands: instances
// ---------------------------------------------------------------------------

enum {
    CREATE_PROFILE, CREATE_LANDSCAPE, CREATE_CORES, CREATE_RAM, CREATE_RES, CREATE_DPI, CREATE_DATA,
    CREATE_IMAGE, CREATE_GPU, CREATE_DNS, CREATE_NO_STAY_AWAKE, CREATE_NO_LEAN, CREATE_FPS, CREATE_NO_AUDIO
};
static const CliOpt k_create_opts[] = {
    [CREATE_PROFILE]       = { "profile",       0, true  },
    [CREATE_LANDSCAPE]     = { "landscape",     0, false },
    [CREATE_CORES]         = { "cores",         0, true  },
    [CREATE_RAM]           = { "ram",           0, true  },
    [CREATE_RES]           = { "res",           0, true  },
    [CREATE_DPI]           = { "dpi",           0, true  },
    [CREATE_DATA]          = { "data",          0, true  },
    [CREATE_IMAGE]         = { "image",         0, true  },
    [CREATE_GPU]           = { "gpu",           0, true  },
    [CREATE_DNS]           = { "dns",           0, true  },
    [CREATE_NO_STAY_AWAKE] = { "no-stay-awake", 0, false },
    [CREATE_NO_LEAN]       = { "no-lean",       0, false },
    [CREATE_FPS]           = { "fps",           0, true  },
    [CREATE_NO_AUDIO]      = { "no-audio",      0, false },
};
_Static_assert(NOPTS(k_create_opts) <= CLI_MAX_OPTS, "too many create options");

static int range_error(const CliCommand* self, const char* opt, const char* value, int min, int max,
                       const char* unit) {
    return usage_error(self, "%s must be a whole number from %d to %d%s (got '%s')", opt, min, max, unit, value);
}

static int cmd_create(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos > 1) {
        return usage_error(self, "too many arguments: '%s'", a->pos[1]);
    }
    const char* opt = a->instance ? a->instance : g->instance;
    const char* name = a->npos == 1 ? a->pos[0] : NULL;
    if (name && opt && strcmp(name, opt) != 0) {
        return usage_error(self, "two different names given: '%s' and '%s'", name, opt);
    }
    if (!name) {
        name = opt ? opt : default_instance_name();
    }
    if (!new_name_valid(name)) {
        return bad_new_name_error(self, name);
    }

    SoulXInstanceConf c;
    memset(&c, 0, sizeof c);
    soulx_conf_defaults(&c, name);
    const char* v;
    // the profile first, explicit values override it
    if ((v = a->val[CREATE_PROFILE]) && soulx_conf_apply_profile(&c, v) != 0) {
        return usage_error(self, "unknown profile '%s' (use lite, balanced or performance)", v);
    }
    if ((v = a->val[CREATE_IMAGE])) {
        const SoulXPackage* img = find_image(v);
        if (!img) {
            return unknown_image_error(self, v);
        }
        if (!copy_str(c.image, sizeof c.image, img->key)) {
            return usage_error(self, "image key '%s' is too long", img->key);
        }
    }
    if ((v = a->val[CREATE_CORES]) && !parse_int(v, CLI_CORES_MIN, CLI_CORES_MAX, &c.cores)) {
        return range_error(self, "--cores", v, CLI_CORES_MIN, CLI_CORES_MAX, "");
    }
    if ((v = a->val[CREATE_RAM]) && !parse_int(v, CLI_RAM_MIN, CLI_RAM_MAX, &c.ram_mb)) {
        return range_error(self, "--ram", v, CLI_RAM_MIN, CLI_RAM_MAX, " MB");
    }
    if ((v = a->val[CREATE_DATA]) && !parse_int(v, CLI_DATA_MIN, CLI_DATA_MAX, &c.data_gb)) {
        return range_error(self, "--data", v, CLI_DATA_MIN, CLI_DATA_MAX, " GB");
    }
    if ((v = a->val[CREATE_DPI]) && !parse_int(v, CLI_DPI_MIN, CLI_DPI_MAX, &c.dpi)) {
        return range_error(self, "--dpi", v, CLI_DPI_MIN, CLI_DPI_MAX, "");
    }
    if ((v = a->val[CREATE_RES])) {
        int w, h;
        if (!parse_resolution(v, &w, &h)) {
            return usage_error(self, "--res must look like 1080x2400, each side %d-%d (got '%s')",
                               CLI_DIM_MIN, CLI_DIM_MAX, v);
        }
        c.width = w;
        c.height = h;
    }
    if ((v = a->val[CREATE_GPU])) {
        if (!gpu_mode_valid(v) || !copy_str(c.gpu, sizeof c.gpu, v)) {
            return usage_error(self, "unknown GPU mode '%s' (use metal, host, host-novulkan or software)", v);
        }
    }
    if ((v = a->val[CREATE_DNS])) {
        if (!dns_list_valid(v, sizeof c.dns) || !copy_str(c.dns, sizeof c.dns, v)) {
            return usage_error(self, "--dns expects up to %d IP addresses separated by commas, e.g. "
                               "1.1.1.1,8.8.8.8 (got '%s')", CLI_DNS_MAX, v);
        }
    }
    if (a->val[CREATE_NO_STAY_AWAKE]) {
        c.stay_awake = false;
    }
    if (a->val[CREATE_NO_LEAN]) {
        c.lean = false;
    }
    if ((v = a->val[CREATE_FPS]) && !parse_int(v, CLI_FPS_MIN, CLI_FPS_MAX, &c.fps)) {
        return range_error(self, "--fps", v, CLI_FPS_MIN, CLI_FPS_MAX, "");
    }
    if (a->val[CREATE_NO_AUDIO]) {
        c.audio = false;
    }
    if (a->val[CREATE_LANDSCAPE]) {
        make_landscape(&c);
    } else {
        c.landscape = c.width > c.height;
    }

    SoulXInstanceConf existing;
    if (soulx_reg_load(c.name, &existing) == 0) {
        soulx_log(SOULX_LOG_ERROR, "an instance named '%s' already exists (id %d); pick another name or delete it "
                 "first: soulx delete %s", c.name, existing.id, c.name);
        return CLI_EXIT_FAIL;
    }
    if (soulx_mac_create(&c) != 0) {
        return CLI_EXIT_FAIL;
    }

    char android[CLI_CELL];
    android_label(c.image, android, sizeof android);
    printf("Created instance '%s' (id %d): %s, %d CPUs, %d MB RAM, %d GB storage, %dx%d @ %d dpi%s\n",
           c.name, c.id, android, c.cores, c.ram_mb, c.data_gb, c.width, c.height, c.dpi,
           c.landscape ? " (landscape)" : "");
    // (when the runtime is missing, soulx_mac_create already said how to install it)
    if (soulx_runtime_ready(c.image)) {
        printf("Next: soulx start %s   (or: soulx play -i %s game.apk)\n", c.name, c.name);
    }
    return CLI_EXIT_OK;
}

enum { LIST_JSON };
static const CliOpt k_list_opts[] = {
    [LIST_JSON] = { "json", 0, false },
};

enum { COL_NAME, COL_PROFILE, COL_STATE, COL_SCREEN, COL_RAM, COL_SERIAL, COL_COUNT };

typedef struct {
    char cell[COL_COUNT][CLI_CELL];
    bool running;
} ListRow;

#define GLYPH_RUNNING "\xE2\x97\x8F"   // ●
#define GLYPH_STOPPED "\xE2\x97\x8B"   // ○

// Display columns of a UTF-8 string (the glyphs ● ○ and the em dash — are 1 column / 3 bytes each).
static int disp_width(const char* s) {
    int w = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ) {
        if (*p < 0x80) { w++; p++; }
        else if ((*p & 0xE0) == 0xC0) { w++; p += 2; }
        else if ((*p & 0xF0) == 0xE0) { w++; p += 3; }
        else if ((*p & 0xF8) == 0xF0) { w += 2; p += 4; }
        else { w++; p++; }
    }
    return w;
}

static void set_cell(char* cell, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

static void set_cell(char* cell, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(cell, CLI_CELL, fmt, ap);
    va_end(ap);
    if (n < 0) {
        copy_str(cell, CLI_CELL, "?");
    } else if (n >= CLI_CELL) {
        memcpy(cell + CLI_CELL - 4, "...", 4);     // display only: mark the cut
    }
}

// JSON object for one instance; extra fields when the caller has status details.
typedef struct {
    bool        boot_known;
    bool        booted;
    long long   uptime_s;
    uint64_t    memory_mb;
    const char* log_path;
} StatusExtra;

static void json_instance(FILE* f, const SoulXInstanceConf* c, bool running, const SoulXRunState* st,
                          const StatusExtra* x) {
    const SoulXPackage* img = find_image(c->image);
    fprintf(f, "{\"id\": %d, \"name\": ", c->id);
    json_str(f, c->name);
    fputs(", \"state\": ", f);
    json_str(f, running ? "running" : "stopped");
    fputs(", \"image\": ", f);
    json_str(f, c->image);
    fputs(", \"android\": ", f);
    json_str_or_null(f, img ? img->android_name : NULL);
    fputs(", \"target\": ", f);
    json_str_or_null(f, img ? img->target : NULL);
    fprintf(f, ", \"cores\": %d, \"ram_mb\": %d, \"data_gb\": %d, \"width\": %d, \"height\": %d, \"dpi\": %d, "
            "\"landscape\": %s", c->cores, c->ram_mb, c->data_gb, c->width, c->height, c->dpi,
            c->landscape ? "true" : "false");
    fputs(", \"gpu\": ", f);
    json_str(f, c->gpu);
    fputs(", \"dns\": ", f);
    json_str(f, c->dns);
    fprintf(f, ", \"stay_awake\": %s, \"lean\": %s, \"fps\": %d, \"audio\": %s, \"created\": ",
            c->stay_awake ? "true" : "false", c->lean ? "true" : "false", c->fps, c->audio ? "true" : "false");
    json_str(f, c->created);
    if (running) {
        char serial[64];
        soulx_serial(st->port, serial, sizeof serial);
        fputs(", \"serial\": ", f);
        json_str(f, serial);
        fprintf(f, ", \"console_port\": %d, \"adb_port\": %d, \"pid\": %d, \"headless\": %s, \"started\": ",
                st->port, st->port + 1, (int)st->pid, st->headless ? "true" : "false");
        json_str(f, st->started);
    } else {
        fputs(", \"serial\": null, \"console_port\": null, \"adb_port\": null, \"pid\": null, "
              "\"headless\": null, \"started\": null", f);
    }
    if (x) {
        fputs(", \"booted\": ", f);
        fputs(!running ? "false" : !x->boot_known ? "null" : x->booted ? "true" : "false", f);
        if (running && x->uptime_s >= 0) {
            fprintf(f, ", \"uptime_s\": %lld", x->uptime_s);
        } else {
            fputs(", \"uptime_s\": null", f);
        }
        if (running && x->memory_mb > 0) {
            fprintf(f, ", \"memory_mb\": %llu", (unsigned long long)x->memory_mb);
        } else {
            fputs(", \"memory_mb\": null", f);
        }
        fputs(", \"log\": ", f);
        json_str_or_null(f, x->log_path);
    }
    fputc('}', f);
}

enum {
    CONFIG_PROFILE, CONFIG_LANDSCAPE, CONFIG_PORTRAIT, CONFIG_CORES, CONFIG_RAM, CONFIG_RES, CONFIG_DPI,
    CONFIG_DATA, CONFIG_GPU, CONFIG_DNS, CONFIG_NO_DNS, CONFIG_STAY_AWAKE, CONFIG_NO_STAY_AWAKE, CONFIG_LEAN,
    CONFIG_NO_LEAN, CONFIG_FPS, CONFIG_AUDIO, CONFIG_NO_AUDIO, CONFIG_JSON
};
static const CliOpt k_config_opts[] = {
    [CONFIG_PROFILE]       = { "profile",       0, true  },
    [CONFIG_LANDSCAPE]     = { "landscape",     0, false },
    [CONFIG_PORTRAIT]      = { "portrait",      0, false },
    [CONFIG_CORES]         = { "cores",         0, true  },
    [CONFIG_RAM]           = { "ram",           0, true  },
    [CONFIG_RES]           = { "res",           0, true  },
    [CONFIG_DPI]           = { "dpi",           0, true  },
    [CONFIG_DATA]          = { "data",          0, true  },
    [CONFIG_GPU]           = { "gpu",           0, true  },
    [CONFIG_DNS]           = { "dns",           0, true  },
    [CONFIG_NO_DNS]        = { "no-dns",        0, false },
    [CONFIG_STAY_AWAKE]    = { "stay-awake",    0, false },
    [CONFIG_NO_STAY_AWAKE] = { "no-stay-awake", 0, false },
    [CONFIG_LEAN]          = { "lean",          0, false },
    [CONFIG_NO_LEAN]       = { "no-lean",       0, false },
    [CONFIG_FPS]           = { "fps",           0, true  },
    [CONFIG_AUDIO]         = { "audio",         0, false },
    [CONFIG_NO_AUDIO]      = { "no-audio",      0, false },
    [CONFIG_JSON]          = { "json",          0, false },
};
_Static_assert(NOPTS(k_config_opts) <= CLI_MAX_OPTS, "too many config options");

static void print_conf_text(const SoulXInstanceConf* c, bool running) {
    char android[CLI_CELL];
    android_label(c->image, android, sizeof android);
    printf("%s (id %d, %s)\n", c->name, c->id, running ? "running" : "stopped");
    printf("  profile     %s\n", soulx_conf_profile_name(c));
    printf("  image       %s (%s)\n", c->image, android);
    printf("  cores       %d\n", c->cores);
    printf("  ram         %d MB\n", c->ram_mb);
    printf("  data        %d GB\n", c->data_gb);
    printf("  screen      %dx%d @ %d dpi, %s\n", c->width, c->height, c->dpi, c->landscape ? "landscape" : "portrait");
    printf("  gpu         %s\n", c->gpu);
    printf("  dns         %s\n", c->dns[0] ? c->dns : "the Mac's");
    printf("  stay-awake  %s\n", c->stay_awake ? "on" : "off");
    printf("  lean        %s\n", c->lean ? "on" : "off");
    printf("  fps         %d\n", c->fps);
    printf("  audio       %s\n", c->audio ? "on" : "off");
}

// Prints one "name: old -> new" line per changed setting; returns how many changed.
static int print_conf_changes(const SoulXInstanceConf* o, const SoulXInstanceConf* n) {
    int count = 0;
#define CHANGED_INT(label, field, unit) \
    if (o->field != n->field) { printf("  %-11s %d%s -> %d%s\n", label, o->field, unit, n->field, unit); count++; }
#define CHANGED_BOOL(label, field) \
    if (o->field != n->field) { printf("  %-11s %s -> %s\n", label, o->field ? "on" : "off", n->field ? "on" : "off"); count++; }
    CHANGED_INT("cores", cores, "")
    CHANGED_INT("ram", ram_mb, " MB")
    CHANGED_INT("data", data_gb, " GB")
    if (o->width != n->width || o->height != n->height) {
        printf("  %-11s %dx%d -> %dx%d\n", "screen", o->width, o->height, n->width, n->height);
        count++;
    }
    CHANGED_INT("dpi", dpi, "")
    CHANGED_BOOL("landscape", landscape)
    if (strcmp(o->gpu, n->gpu) != 0) { printf("  %-11s %s -> %s\n", "gpu", o->gpu, n->gpu); count++; }
    if (strcmp(o->dns, n->dns) != 0) {
        printf("  %-11s %s -> %s\n", "dns", o->dns[0] ? o->dns : "the Mac's", n->dns[0] ? n->dns : "the Mac's");
        count++;
    }
    CHANGED_BOOL("stay-awake", stay_awake)
    CHANGED_BOOL("lean", lean)
    CHANGED_INT("fps", fps, "")
    CHANGED_BOOL("audio", audio)
#undef CHANGED_INT
#undef CHANGED_BOOL
    return count;
}

static int cmd_config(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    const char* ref = NULL;
    int rc = instance_from_args(self, a, g, &ref);
    if (rc != 0) {
        return rc;
    }
    SoulXInstanceConf cur;
    if (load_instance(ref, &cur) != 0) {
        return CLI_EXIT_FAIL;
    }
    SoulXRunState st;
    memset(&st, 0, sizeof st);
    bool running = soulx_instance_running(cur.name, &st);

    if (a->val[CONFIG_LANDSCAPE] && a->val[CONFIG_PORTRAIT]) {
        return usage_error(self, "--landscape and --portrait cannot be used together");
    }
    if (a->val[CONFIG_STAY_AWAKE] && a->val[CONFIG_NO_STAY_AWAKE]) {
        return usage_error(self, "--stay-awake and --no-stay-awake cannot be used together");
    }
    if (a->val[CONFIG_LEAN] && a->val[CONFIG_NO_LEAN]) {
        return usage_error(self, "--lean and --no-lean cannot be used together");
    }
    if (a->val[CONFIG_DNS] && a->val[CONFIG_NO_DNS]) {
        return usage_error(self, "--dns and --no-dns cannot be used together");
    }
    if (a->val[CONFIG_AUDIO] && a->val[CONFIG_NO_AUDIO]) {
        return usage_error(self, "--audio and --no-audio cannot be used together");
    }

    SoulXInstanceConf c = cur;
    bool changing = false;
    for (int i = 0; i < NOPTS(k_config_opts); i++) {
        if (i != CONFIG_JSON && a->val[i]) changing = true;
    }
    const char* v;
    // the profile first (it keeps the orientation and the storage), explicit values override it
    if ((v = a->val[CONFIG_PROFILE])) {
        int data_gb = c.data_gb;
        if (soulx_conf_apply_profile(&c, v) != 0) {
            return usage_error(self, "unknown profile '%s' (use lite, balanced or performance)", v);
        }
        c.data_gb = data_gb;
    }
    if ((v = a->val[CONFIG_CORES]) && !parse_int(v, CLI_CORES_MIN, CLI_CORES_MAX, &c.cores)) {
        return range_error(self, "--cores", v, CLI_CORES_MIN, CLI_CORES_MAX, "");
    }
    if ((v = a->val[CONFIG_RAM]) && !parse_int(v, CLI_RAM_MIN, CLI_RAM_MAX, &c.ram_mb)) {
        return range_error(self, "--ram", v, CLI_RAM_MIN, CLI_RAM_MAX, " MB");
    }
    if ((v = a->val[CONFIG_DATA]) && !parse_int(v, CLI_DATA_MIN, CLI_DATA_MAX, &c.data_gb)) {
        return range_error(self, "--data", v, CLI_DATA_MIN, CLI_DATA_MAX, " GB");
    }
    if ((v = a->val[CONFIG_DPI]) && !parse_int(v, CLI_DPI_MIN, CLI_DPI_MAX, &c.dpi)) {
        return range_error(self, "--dpi", v, CLI_DPI_MIN, CLI_DPI_MAX, "");
    }
    if ((v = a->val[CONFIG_RES])) {
        int w, h;
        if (!parse_resolution(v, &w, &h)) {
            return usage_error(self, "--res must look like 1080x2400, each side %d-%d (got '%s')",
                               CLI_DIM_MIN, CLI_DIM_MAX, v);
        }
        c.width = w;
        c.height = h;
        if (w != h) c.landscape = w > h;            // the shape given wins over the old orientation
    }
    // an explicit orientation wins over everything; soulx_mac_reconfigure turns the panel to match
    if (a->val[CONFIG_LANDSCAPE]) c.landscape = true;
    if (a->val[CONFIG_PORTRAIT]) c.landscape = false;
    if ((v = a->val[CONFIG_GPU])) {
        if (!gpu_mode_valid(v) || !copy_str(c.gpu, sizeof c.gpu, v)) {
            return usage_error(self, "unknown GPU mode '%s' (use host, host-novulkan, software or metal)", v);
        }
    }
    if ((v = a->val[CONFIG_DNS])) {
        if (!dns_list_valid(v, sizeof c.dns) || !copy_str(c.dns, sizeof c.dns, v)) {
            return usage_error(self, "--dns expects up to %d IP addresses separated by commas, e.g. "
                               "1.1.1.1,8.8.8.8 (got '%s')", CLI_DNS_MAX, v);
        }
    }
    if (a->val[CONFIG_NO_DNS]) c.dns[0] = '\0';
    if (a->val[CONFIG_STAY_AWAKE]) c.stay_awake = true;
    if (a->val[CONFIG_NO_STAY_AWAKE]) c.stay_awake = false;
    if (a->val[CONFIG_LEAN]) c.lean = true;
    if (a->val[CONFIG_NO_LEAN]) c.lean = false;
    if ((v = a->val[CONFIG_FPS]) && !parse_int(v, CLI_FPS_MIN, CLI_FPS_MAX, &c.fps)) {
        return range_error(self, "--fps", v, CLI_FPS_MIN, CLI_FPS_MAX, "");
    }
    if (a->val[CONFIG_AUDIO]) c.audio = true;
    if (a->val[CONFIG_NO_AUDIO]) c.audio = false;

    bool needs_restart = false;
    if (changing && soulx_mac_reconfigure(&c, &needs_restart) != 0) {
        return CLI_EXIT_FAIL;
    }

    if (a->val[CONFIG_JSON]) {
        json_instance(stdout, &c, running, &st, NULL);
        fputc('\n', stdout);
        return CLI_EXIT_OK;
    }
    if (!changing) {
        print_conf_text(&c, running);
        return CLI_EXIT_OK;
    }
    printf("Updated '%s':\n", c.name);
    if (print_conf_changes(&cur, &c) == 0) {
        printf("  (nothing changed)\n");
    } else if (needs_restart) {
        printf("'%s' is running: the new settings apply at its next start (soulx restart %s)\n", c.name, c.name);
    }
    return CLI_EXIT_OK;
}

static int cmd_list(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    (void)g;
    int rc = no_positionals(self, a);
    if (rc != 0) {
        return rc;
    }
    bool json = a->val[LIST_JSON] != NULL;
    SoulXInstanceConf* confs = calloc(CLI_LIST_MAX, sizeof *confs);
    if (!confs) {
        return out_of_memory();
    }
    int n = soulx_reg_list(confs, CLI_LIST_MAX);
    if (n < 0) {
        char dir[PATH_MAX];
        if (soulx_path(dir, sizeof dir, "instances") == 0 && !soulx_path_exists(dir)) {
            n = 0;      // nothing created yet
        } else {
            soulx_log(SOULX_LOG_ERROR, "cannot read the instance registry in %s/instances", nz(soulx_home(), "?"));
            free(confs);
            return CLI_EXIT_FAIL;
        }
    }
    if (n > CLI_LIST_MAX) {
        n = CLI_LIST_MAX;
    }

    if (json) {
        fputs("[", stdout);
        for (int i = 0; i < n; i++) {
            SoulXRunState st;
            memset(&st, 0, sizeof st);
            bool running = soulx_instance_running(confs[i].name, &st);
            fputs(i == 0 ? "\n  " : ",\n  ", stdout);
            json_instance(stdout, &confs[i], running, &st, NULL);
        }
        fputs(n > 0 ? "\n]\n" : "]\n", stdout);
        free(confs);
        return CLI_EXIT_OK;
    }

    if (n == 0) {
        free(confs);
        printf("No instances yet. Create one with: soulx create   (or just run: soulx play game.apk)\n");
        return CLI_EXIT_OK;
    }
    ListRow* rows = calloc((size_t)n, sizeof *rows);
    if (!rows) {
        free(confs);
        return out_of_memory();
    }
    static const char* const headers[COL_COUNT] = {
        "NAME", "PROFILE", "STATE", "SCREEN", "RAM", "SERIAL"
    };
    int width[COL_COUNT];
    for (int c = 0; c < COL_COUNT; c++) {
        width[c] = disp_width(headers[c]);
    }
    for (int i = 0; i < n; i++) {
        const SoulXInstanceConf* c = &confs[i];
        SoulXRunState st;
        memset(&st, 0, sizeof st);
        bool running = soulx_instance_running(c->name, &st);
        rows[i].running = running;
        set_cell(rows[i].cell[COL_NAME], "%s", c->name);
        set_cell(rows[i].cell[COL_PROFILE], "%s", soulx_conf_profile_name(c));
        set_cell(rows[i].cell[COL_STATE], "%s %s", running ? GLYPH_RUNNING : GLYPH_STOPPED,
                 running ? (st.headless ? "running (headless)" : "running") : "stopped");
        set_cell(rows[i].cell[COL_SCREEN], "%dx%d", c->width, c->height);
        set_cell(rows[i].cell[COL_RAM], "%g GB", c->ram_mb / 1024.0);
        if (running) {
            soulx_serial(st.port, rows[i].cell[COL_SERIAL], CLI_CELL);
        } else {
            set_cell(rows[i].cell[COL_SERIAL], "\xE2\x80\x94");   // em dash
        }
        for (int col = 0; col < COL_COUNT; col++) {
            int w = disp_width(rows[i].cell[col]);
            if (w > width[col]) {
                width[col] = w;
            }
        }
    }
    bool color = isatty(fileno(stdout));
    for (int col = 0; col < COL_COUNT; col++) {
        if (col == COL_COUNT - 1) {
            printf("%s\n", headers[col]);
        } else {
            printf("%s%*s  ", headers[col], width[col] - disp_width(headers[col]), "");
        }
    }
    for (int i = 0; i < n; i++) {
        for (int col = 0; col < COL_COUNT; col++) {
            const char* cell = rows[i].cell[col];
            const char* c0 = "";
            const char* c1 = "";
            if (color && col == COL_STATE) {
                c0 = rows[i].running ? "\033[32m" : "\033[2m";   // green when running, dim when stopped
                c1 = "\033[0m";
            }
            if (col == COL_COUNT - 1) {
                printf("%s%s%s\n", c0, cell, c1);
            } else {
                printf("%s%s%s%*s  ", c0, cell, c1, width[col] - disp_width(cell), "");
            }
        }
    }
    free(rows);
    free(confs);
    return CLI_EXIT_OK;
}

enum { STATUS_JSON };
static const CliOpt k_status_opts[] = {
    [STATUS_JSON] = { "json", 0, false },
};

static int cmd_status(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    const char* ref = NULL;
    int rc = instance_from_args(self, a, g, &ref);
    if (rc != 0) {
        return rc;
    }
    SoulXInstanceConf c;
    if (load_instance(ref, &c) != 0) {
        return CLI_EXIT_FAIL;
    }
    SoulXRunState st;
    memset(&st, 0, sizeof st);
    bool running = soulx_instance_running(c.name, &st);
    char serial[64] = "";
    char log_path[PATH_MAX];
    StatusExtra x = { .boot_known = false, .booted = false, .uptime_s = -1, .memory_mb = 0, .log_path = NULL };
    if (instance_log_path(c.name, log_path, sizeof log_path) == 0) {
        x.log_path = log_path;
    }
    if (running) {
        soulx_serial(st.port, serial, sizeof serial);
        char prop[16] = "";
        if (soulx_adb_getprop(serial, "sys.boot_completed", prop, sizeof prop) == 0) {
            x.boot_known = true;
            x.booted = strcmp(rtrim(prop), "1") == 0;
        }
        x.uptime_s = seconds_since(st.started);
        x.memory_mb = process_footprint_mb(st.pid);
    }

    if (a->val[STATUS_JSON]) {
        json_instance(stdout, &c, running, &st, &x);
        fputc('\n', stdout);
        return CLI_EXIT_OK;
    }

    const SoulXPackage* img = find_image(c.image);
    bool color = isatty(fileno(stdout));
    const char* sc = color ? (running ? "\033[32m" : "\033[2m") : "";   // State colour: green / dim
    const char* sr = color ? "\033[0m" : "";
    printf("Instance:   %s (id %d)\n", c.name, c.id);
    if (!running) {
        printf("State:      %sstopped%s\n", sc, sr);
    } else if (x.boot_known) {
        printf("State:      %srunning%s, %s\n", sc, sr, x.booted ? "Android is ready" : "Android is booting");
    } else {
        printf("State:      %srunning%s\n", sc, sr);
    }
    if (img) {
        printf("Android:    %s, %s (%s, image '%s')\n", nz(img->android_name, "Android"),
               nz(img->tag_display, img->key), nz(img->target, "?"), img->key);
    } else {
        printf("Android:    unknown image '%s'\n", c.image);
    }
    printf("Profile:    %s\n", soulx_conf_profile_name(&c));
    printf("Hardware:   %d CPUs, %d MB RAM, %d GB storage, GPU %s\n", c.cores, c.ram_mb, c.data_gb, c.gpu);
    printf("Screen:     %dx%d @ %d dpi (%s)\n", c.width, c.height, c.dpi, c.landscape ? "landscape" : "portrait");
    printf("DNS:        %s\n", c.dns[0] ? c.dns : "the Mac's resolvers");
    printf("Stay awake: %s\n", c.stay_awake ? "yes" : "no");
    printf("Lean:       %s\n", c.lean ? "yes (Google apps games never need are disabled)" : "no");
    if (running) {
        printf("Serial:     %s (console port %d, adb port %d)\n", serial, st.port, st.port + 1);
        printf("Engine:     pid %d, %s", (int)st.pid, st.headless ? "headless" : "window");
        if (x.uptime_s >= 0) {
            char up[32];
            format_duration(x.uptime_s, up, sizeof up);
            printf(", up %s", up);
        }
        if (x.memory_mb > 0) {
            printf(", %llu MB memory", (unsigned long long)x.memory_mb);
        }
        printf("\n");
    }
    printf("Created:    %s\n", nz(c.created, "?"));
    if (x.log_path) {
        printf("Log:        %s\n", x.log_path);
    }
    return CLI_EXIT_OK;
}

enum { START_HEADLESS, START_COLD, START_NO_WAIT, START_TIMEOUT };
static const CliOpt k_start_opts[] = {
    [START_HEADLESS] = { "headless", 0, false },
    [START_COLD]     = { "cold",     0, false },
    [START_NO_WAIT]  = { "no-wait",  0, false },
    [START_TIMEOUT]  = { "timeout",  0, true  },
};

static int start_or_restart(const CliCommand* self, const CliArgs* a, const CliGlobal* g, bool restart) {
    const char* ref = NULL;
    int rc = instance_from_args(self, a, g, &ref);
    if (rc != 0) {
        return rc;
    }
    int timeout = 0;
    const char* tv = a->val[START_TIMEOUT];
    if (tv && !parse_int(tv, CLI_TIMEOUT_MIN, CLI_TIMEOUT_MAX, &timeout)) {
        return range_error(self, "--timeout", tv, CLI_TIMEOUT_MIN, CLI_TIMEOUT_MAX, " seconds");
    }
    SoulXInstanceConf c;
    if (load_instance(ref, &c) != 0) {
        return CLI_EXIT_FAIL;
    }
    bool headless = a->val[START_HEADLESS] != NULL;
    bool cold = a->val[START_COLD] != NULL;
    SoulXRunState st;
    memset(&st, 0, sizeof st);
    bool running = soulx_instance_running(c.name, &st);
    bool was_running = running && !restart;     // 'start' on a running instance: do not relaunch the engine
    if (was_running && (cold || (headless && !st.headless))) {
        soulx_log(SOULX_LOG_INFO, "note: --headless/--cold apply to the next start; use: soulx restart %s", c.name);
    }
    if (running && restart) {
        if (!a->val[START_HEADLESS]) {
            headless = st.headless;     // a restart keeps the window mode
        }
        soulx_log(SOULX_LOG_INFO, "stopping '%s' ...", c.name);
        if (soulx_mac_stop(c.name, false) != 0) {
            return g_soulx_interrupted ? CLI_EXIT_INTR : CLI_EXIT_FAIL;
        }
    }
    if (g_soulx_interrupted) {
        return CLI_EXIT_INTR;
    }
    // soulx_mac_start handles the already-running case too: it waits for the boot to finish (so
    // `soulx start` on a still-booting instance keeps its promise to wait) and never
    // starts a second engine. A restart always starts fresh because we stopped it above.
    SoulXStartOpts so = { .headless = was_running ? st.headless : headless, .cold = cold,
                         .wait = a->val[START_NO_WAIT] == NULL, .timeout_s = timeout };
    SoulXRunState nst;
    memset(&nst, 0, sizeof nst);
    if (soulx_mac_start(c.name, &so, &nst) != 0) {
        return g_soulx_interrupted ? CLI_EXIT_INTR : CLI_EXIT_FAIL;   // the engine said what happened
    }
    if (!soulx_instance_running(c.name, &nst)) {
        soulx_log(SOULX_LOG_ERROR, "'%s' is not running after the start; see: soulx status %s", c.name, c.name);
        return CLI_EXIT_FAIL;
    }
    char serial[64];
    soulx_serial(nst.port, serial, sizeof serial);
    if (was_running) {
        printf("'%s' is already running (%s%s)\n", c.name, serial, nst.headless ? ", headless" : "");
    } else if (so.wait) {
        printf("'%s' is ready (%s%s)\n", c.name, serial, nst.headless ? ", headless" : "");
    } else {
        printf("'%s' is starting (%s); check it with: soulx status %s\n", c.name, serial, c.name);
    }
    // Open (or bring to front) SoulX's own window unless this instance runs headless. The window shows
    // the engine's shared frames; the engine never shows a window of its own.
    open_window_unless_headless(c.name, nst.headless);
    return CLI_EXIT_OK;
}

static int cmd_start(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    return start_or_restart(self, a, g, false);
}

static int cmd_restart(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    return start_or_restart(self, a, g, true);
}

static int cmd_window(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    // The instance may be a leading positional ("soulx window default") or -i NAME; any other words are
    // the test-hook options, forwarded to soulx_window_run untouched (CLI_PASSTHROUGH).
    const char* name = NULL;
    int opt_start = 0;
    if (a->npos > 0 && !is_option_like(a->pos[0])) {
        name = a->pos[0];
        opt_start = 1;
    }
    const char* opt = a->instance ? a->instance : g->instance;
    if (name && opt && strcmp(name, opt) != 0) {
        return usage_error(self, "two different instances given: '%s' and '%s'", name, opt);
    }
    if (!name) {
        name = opt ? opt : default_instance_name();
    }
    if (!instance_ref_valid(name)) {
        return bad_instance_error(self, name);
    }
    // soulx_window_run resolves the instance, requires it running, runs the AppKit event loop and
    // returns 0 ok / 1 failure / 2 usage error (the same codes the CLI uses).
    int rc = soulx_window_run(name, a->npos - opt_start, a->pos + opt_start);
    return (rc >= 0 && rc <= 255) ? rc : CLI_EXIT_FAIL;
}

enum { STOP_FORCE };
static const CliOpt k_stop_opts[] = {
    [STOP_FORCE] = { "force", 0, false },
};

static int cmd_stop(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    const char* ref = NULL;
    int rc = instance_from_args(self, a, g, &ref);
    if (rc != 0) {
        return rc;
    }
    SoulXInstanceConf c;
    if (load_instance(ref, &c) != 0) {
        return CLI_EXIT_FAIL;
    }
    SoulXRunState st;
    if (!soulx_instance_running(c.name, &st)) {
        printf("'%s' is not running\n", c.name);
        // Its engine may have exited by itself (crash, window closed, kill -9), leaving SoulX's private
        // adb server behind; take it down when nothing else is running.
        soulx_mac_cleanup_idle();
        return CLI_EXIT_OK;
    }
    bool force = a->val[STOP_FORCE] != NULL;
    if (soulx_mac_stop(c.name, force) != 0) {
        return g_soulx_interrupted ? CLI_EXIT_INTR : CLI_EXIT_FAIL;
    }
    printf("Stopped '%s'%s\n", c.name, force ? " (forced)" : "");
    return CLI_EXIT_OK;
}

enum { DELETE_YES };
static const CliOpt k_delete_opts[] = {
    [DELETE_YES] = { "yes", 'y', false },
};

static bool ask_yes_no(const char* question) {
    fprintf(stderr, "%s [y/N] ", question);
    fflush(stderr);
    char answer[32];
    if (!fgets(answer, sizeof answer, stdin)) {
        fputc('\n', stderr);
        return false;
    }
    rtrim(answer);
    return strcasecmp(answer, "y") == 0 || strcasecmp(answer, "yes") == 0;
}

static int cmd_delete(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos > 1) {
        return usage_error(self, "too many arguments: '%s'", a->pos[1]);
    }
    const char* opt = a->instance ? a->instance : g->instance;
    const char* ref = a->npos == 1 ? a->pos[0] : opt;
    if (a->npos == 1 && opt && strcmp(a->pos[0], opt) != 0) {
        return usage_error(self, "two different instances given: '%s' and '%s'", a->pos[0], opt);
    }
    if (!ref) {
        return usage_error(self, "which instance? Name it explicitly, e.g.: soulx delete default");
    }
    if (!instance_ref_valid(ref)) {
        return bad_instance_error(self, ref);
    }
    SoulXInstanceConf c;
    if (load_instance(ref, &c) != 0) {
        return CLI_EXIT_FAIL;
    }
    SoulXRunState st;
    if (soulx_instance_running(c.name, &st)) {
        soulx_log(SOULX_LOG_ERROR, "'%s' is running; stop it first: soulx stop %s", c.name, c.name);
        return CLI_EXIT_FAIL;
    }
    if (!a->val[DELETE_YES]) {
        if (!isatty(STDIN_FILENO)) {
            return usage_error(self, "not deleting '%s' without confirmation; add --yes", c.name);
        }
        char question[160];
        int n = snprintf(question, sizeof question, "Delete instance '%s' and everything on it (apps, game data, "
                         "snapshots)?", c.name);
        if (n < 0 || (size_t)n >= sizeof question) {
            copy_str(question, sizeof question, "Delete this instance and everything on it?");
        }
        if (!ask_yes_no(question)) {
            soulx_log(SOULX_LOG_INFO, "cancelled; nothing was deleted");
            return g_soulx_interrupted ? CLI_EXIT_INTR : CLI_EXIT_FAIL;
        }
    }
    if (soulx_mac_delete(c.name) != 0) {
        return CLI_EXIT_FAIL;
    }
    printf("Deleted instance '%s'\n", c.name);
    return CLI_EXIT_OK;
}

// ---------------------------------------------------------------------------
// Commands: apps
// ---------------------------------------------------------------------------

enum { INSTALL_REINSTALL };
static const CliOpt k_install_opts[] = {
    [INSTALL_REINSTALL] = { "reinstall", 0, false },
};

static int cmd_install(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos == 0) {
        return usage_error(self, "which file? e.g.: soulx install game.apk");
    }
    // check every file before touching the instance
    for (int i = 0; i < a->npos; i++) {
        struct stat sb;
        if (stat(a->pos[i], &sb) != 0) {
            soulx_log(SOULX_LOG_ERROR, "%s: %s", a->pos[i], strerror(errno));
            return CLI_EXIT_FAIL;
        }
        if (!S_ISREG(sb.st_mode) && !S_ISDIR(sb.st_mode)) {
            soulx_log(SOULX_LOG_ERROR, "%s is not a file or folder", a->pos[i]);
            return CLI_EXIT_FAIL;
        }
    }
    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    bool reinstall = a->val[INSTALL_REINSTALL] != NULL;
    int failed = 0;
    for (int i = 0; i < a->npos; i++) {
        if (g_soulx_interrupted) {
            return CLI_EXIT_INTR;
        }
        SoulXApkInfo info;
        if (inspect_app(a->pos[i], &info) != 0 ||
            install_inspected(t.serial, a->pos[i], &info, reinstall, true) != 0) {
            failed++;
        }
    }
    if (failed > 0 && a->npos > 1) {
        soulx_log(SOULX_LOG_ERROR, "%d of %d apps were not installed", failed, a->npos);
    }
    return failed > 0 ? CLI_EXIT_FAIL : CLI_EXIT_OK;
}

enum { PKG_LAUNCH, PKG_STOP, PKG_UNINSTALL };

static int package_command(const CliCommand* self, const CliArgs* a, const CliGlobal* g, int action) {
    if (a->npos == 0) {
        return usage_error(self, "which app? Give its package name, e.g. com.example.game (see: soulx apps)");
    }
    if (a->npos > 1) {
        return usage_error(self, "too many arguments: '%s'", a->pos[1]);
    }
    const char* pkg = a->pos[0];
    if (!soulx_package_name_valid(pkg)) {
        return usage_error(self, "'%s' is not a package name (like com.example.game); see: soulx apps", pkg);
    }
    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    long long have = -1;
    if (installed_version(t.serial, pkg, &have) == 0) {
        soulx_log(SOULX_LOG_ERROR, "%s is not installed on '%s' (see: soulx apps)", pkg, t.conf.name);
        return CLI_EXIT_FAIL;
    }
    switch (action) {
    case PKG_LAUNCH:
        if (soulx_launch_package(t.serial, pkg) != 0) {
            soulx_log(SOULX_LOG_ERROR, "could not launch %s on '%s'", pkg, t.conf.name);
            return CLI_EXIT_FAIL;
        }
        printf("Launched %s on %s\n", pkg, t.conf.name);
        return CLI_EXIT_OK;
    case PKG_STOP:
        if (soulx_force_stop(t.serial, pkg) != 0) {
            soulx_log(SOULX_LOG_ERROR, "could not stop %s on '%s'", pkg, t.conf.name);
            return CLI_EXIT_FAIL;
        }
        printf("Stopped %s on %s\n", pkg, t.conf.name);
        return CLI_EXIT_OK;
    default:
        if (soulx_uninstall_package(t.serial, pkg) != 0) {
            soulx_log(SOULX_LOG_ERROR, "could not uninstall %s from '%s'", pkg, t.conf.name);
            return CLI_EXIT_FAIL;
        }
        printf("Uninstalled %s from %s\n", pkg, t.conf.name);
        return CLI_EXIT_OK;
    }
}

static int cmd_launch(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    return package_command(self, a, g, PKG_LAUNCH);
}

static int cmd_stop_app(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    return package_command(self, a, g, PKG_STOP);
}

static int cmd_uninstall(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    return package_command(self, a, g, PKG_UNINSTALL);
}

enum { APPS_ALL };
static const CliOpt k_apps_opts[] = {
    [APPS_ALL] = { "all", 0, false },
};

static int cmd_apps(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    int rc = no_positionals(self, a);
    if (rc != 0) {
        return rc;
    }
    CliTarget t;
    rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    bool all = a->val[APPS_ALL] != NULL;
    char* out = NULL;
    if (soulx_list_packages(t.serial, !all, &out) != 0) {
        free(out);
        soulx_log(SOULX_LOG_ERROR, "could not list the apps on '%s'", t.conf.name);
        return CLI_EXIT_FAIL;
    }
    size_t cap = 1;
    for (const char* p = out; p && *p; p++) {
        if (*p == '\n') {
            cap++;
        }
    }
    char** lines = calloc(cap, sizeof *lines);
    if (!lines) {
        free(out);
        return out_of_memory();
    }
    size_t n = 0;
    char* line = out;
    while (line && *line && n < cap) {
        char* next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        rtrim(line);
        if (*line) {
            lines[n++] = line;
        }
        line = next;
    }
    qsort(lines, n, sizeof *lines, cmp_cstr);
    for (size_t i = 0; i < n; i++) {
        printf("%s\n", lines[i]);
    }
    if (n == 0) {
        if (all) {
            soulx_log(SOULX_LOG_INFO, "no apps found");
        } else {
            soulx_log(SOULX_LOG_INFO, "no apps installed yet; install one with: soulx install game.apk");
        }
    }
    free(lines);
    free(out);
    return CLI_EXIT_OK;
}

// ---------------------------------------------------------------------------
// Commands: automation
// ---------------------------------------------------------------------------

static bool readable_file(const char* path) {
    struct stat sb;
    if (stat(path, &sb) != 0) {
        soulx_log(SOULX_LOG_ERROR, "%s: %s", path, strerror(errno));
        return false;
    }
    if (!S_ISREG(sb.st_mode)) {
        soulx_log(SOULX_LOG_ERROR, "%s is not a file", path);
        return false;
    }
    if (access(path, R_OK) != 0) {
        soulx_log(SOULX_LOG_ERROR, "%s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

enum { RUN_REPEAT, RUN_LOOP, RUN_SPEED };
static const CliOpt k_run_opts[] = {
    [RUN_REPEAT] = { "repeat", 0, true  },
    [RUN_LOOP]   = { "loop",   0, false },
    [RUN_SPEED]  = { "speed",  0, true  },
};
_Static_assert(NOPTS(k_run_opts) <= CLI_MAX_OPTS, "too many run options");

static int cmd_run(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos == 0) {
        return usage_error(self, "which script? e.g.: soulx run farm.lua");
    }
    const char* script = a->pos[0];
    // not a file here: a script of the instance, by its name or file name (soulx run "Daily run")
    char found[PATH_MAX];
    struct stat sst;
    if (stat(script, &sst) != 0 && !strchr(script, '/')) {
        const char* inst = a->instance ? a->instance : g->instance ? g->instance : default_instance_name();
        SoulXInstanceConf ic;
        int fr = soulx_resolve_instance(inst, &ic) == 0 ? soulx_scripts_find(ic.name, script, found, sizeof found) : -1;
        if (fr == 0) {
            script = found;
        } else if (fr == -2) {
            soulx_log(SOULX_LOG_ERROR, "more than one script of '%s' is called '%s': use its file name (soulx scripts -i %s)",
                      ic.name, script, ic.name);
            return CLI_EXIT_FAIL;
        }
    }
    if (!readable_file(script)) {
        return CLI_EXIT_FAIL;
    }

    int repeat = 1;                 // how many times to run; 0 = loop until Ctrl+C
    double speed = 1.0;
    const char* v;
    if (a->val[RUN_LOOP]) {
        repeat = 0;
    }
    if ((v = a->val[RUN_REPEAT])) {
        int n;
        if (!parse_int(v, 1, 100000, &n)) {
            return usage_error(self, "--repeat must be a whole number from 1 to 100000 (use --loop to repeat forever)");
        }
        if (!a->val[RUN_LOOP]) repeat = n;      // --loop wins when both are given
    }
    if ((v = a->val[RUN_SPEED])) {
        char* end;
        errno = 0;
        double sp = strtod(v, &end);
        if (end == v || *end != '\0' || errno == ERANGE || !(sp >= 0.1 && sp <= 10.0)) {
            return usage_error(self, "--speed must be a number from 0.1 to 10 (e.g. 2 = twice as fast, 0.5 = half)");
        }
        speed = sp;
    }

    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    rc = soulx_script_run_ex(t.conf.name, script, a->npos - 1, a->pos + 1, repeat, speed);
    if (rc < 0 || rc > 255) {
        rc = CLI_EXIT_FAIL;
    }
    return rc;
}

enum { RECORD_FORCE };
static const CliOpt k_record_opts[] = {
    [RECORD_FORCE] = { "force", 0, false },
};

// The output file must be creatable and must not silently replace an existing one.
static bool output_file_ok(const char* path, bool overwrite) {
    struct stat sb;
    if (lstat(path, &sb) == 0) {
        if (S_ISDIR(sb.st_mode)) {
            soulx_log(SOULX_LOG_ERROR, "%s is a folder", path);
            return false;
        }
        if (!overwrite) {
            soulx_log(SOULX_LOG_ERROR, "%s already exists (add --force to overwrite it)", path);
            return false;
        }
    } else if (errno != ENOENT) {
        soulx_log(SOULX_LOG_ERROR, "%s: %s", path, strerror(errno));
        return false;
    }
    char dir[PATH_MAX];
    const char* slash = strrchr(path, '/');
    if (!slash) {
        copy_str(dir, sizeof dir, ".");
    } else {
        size_t n = (slash == path) ? 1 : (size_t)(slash - path);
        if (n >= sizeof dir) {
            soulx_log(SOULX_LOG_ERROR, "%s: path too long", path);
            return false;
        }
        memcpy(dir, path, n);
        dir[n] = '\0';
    }
    if (!soulx_is_dir(dir)) {
        soulx_log(SOULX_LOG_ERROR, "folder %s does not exist", dir);
        return false;
    }
    if (access(dir, W_OK) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot write to %s: %s", dir, strerror(errno));
        return false;
    }
    return true;
}

static int cmd_record(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos == 0) {
        return usage_error(self, "where should the script go? e.g.: soulx record macro.lua");
    }
    if (a->npos > 1) {
        return usage_error(self, "too many arguments: '%s'", a->pos[1]);
    }
    const char* out = a->pos[0];
    if (!output_file_ok(out, a->val[RECORD_FORCE] != NULL)) {
        return CLI_EXIT_FAIL;
    }
    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    rc = soulx_record(t.conf.name, out);
    if (rc == 0) {
        printf("Recorded %s; replay it with: soulx run %s\n", out, out);
        return CLI_EXIT_OK;
    }
    return (rc > 0 && rc <= 255) ? rc : CLI_EXIT_FAIL;
}

static SoulXInput* open_input(const CliTarget* t) {
    SoulXInput* in = soulx_input_open(t->conf.name);
    if (!in) {
        soulx_log(SOULX_LOG_ERROR, "cannot connect to the emulator console of '%s'", t->conf.name);
    }
    return in;
}

static bool point_on_screen(SoulXInput* in, int x, int y) {
    int w = 0;
    int h = 0;
    if (soulx_input_screen_size(in, &w, &h) != 0 || w <= 0 || h <= 0) {
        return true;    // size unknown: input.c validates
    }
    if (x < w && y < h) {
        return true;
    }
    soulx_log(SOULX_LOG_ERROR, "point %d,%d is outside the screen (%dx%d: x 0-%d, y 0-%d)", x, y, w, h, w - 1, h - 1);
    return false;
}

static int cmd_tap(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos != 2) {
        return usage_error(self, "expected X and Y, e.g.: soulx tap 540 1200");
    }
    int x;
    int y;
    if (!parse_int(a->pos[0], 0, CLI_COORD_MAX, &x) || !parse_int(a->pos[1], 0, CLI_COORD_MAX, &y)) {
        return usage_error(self, "X and Y must be whole numbers of screen pixels (got '%s' '%s')",
                           a->pos[0], a->pos[1]);
    }
    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    SoulXInput* in = open_input(&t);
    if (!in) {
        return CLI_EXIT_FAIL;
    }
    rc = CLI_EXIT_FAIL;
    if (point_on_screen(in, x, y)) {
        if (soulx_input_tap(in, x, y) == 0) {
            rc = CLI_EXIT_OK;
        } else {
            soulx_log(SOULX_LOG_ERROR, "tap failed on '%s'", t.conf.name);
        }
    }
    soulx_input_close(in);
    return rc;
}

static int cmd_swipe(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos != 4 && a->npos != 5) {
        return usage_error(self, "expected X1 Y1 X2 Y2 [MS], e.g.: soulx swipe 540 1800 540 600 300");
    }
    int v[4];
    for (int i = 0; i < 4; i++) {
        if (!parse_int(a->pos[i], 0, CLI_COORD_MAX, &v[i])) {
            return usage_error(self, "coordinates must be whole numbers of screen pixels (got '%s')", a->pos[i]);
        }
    }
    int ms = CLI_SWIPE_MS_DEFAULT;
    if (a->npos == 5 && !parse_int(a->pos[4], 1, CLI_SWIPE_MS_MAX, &ms)) {
        return range_error(self, "MS", a->pos[4], 1, CLI_SWIPE_MS_MAX, " milliseconds");
    }
    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    SoulXInput* in = open_input(&t);
    if (!in) {
        return CLI_EXIT_FAIL;
    }
    rc = CLI_EXIT_FAIL;
    if (point_on_screen(in, v[0], v[1]) && point_on_screen(in, v[2], v[3])) {
        if (soulx_input_swipe(in, v[0], v[1], v[2], v[3], ms) == 0) {
            rc = CLI_EXIT_OK;
        } else if (!g_soulx_interrupted) {
            soulx_log(SOULX_LOG_ERROR, "swipe failed on '%s'", t.conf.name);
        }
    }
    soulx_input_close(in);
    return rc;
}

static int cmd_key(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos != 1) {
        return usage_error(self, "expected one key, e.g.: soulx key BACK");
    }
    char key[CLI_KEY_MAX + 1];
    const char* k = a->pos[0];
    size_t n = strlen(k);
    if (n == 0 || n > CLI_KEY_MAX) {
        return usage_error(self, "invalid key '%s'", k);
    }
    for (size_t i = 0; i < n; i++) {
        char ch = k[i];
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
        if (!ok) {
            return usage_error(self, "invalid key '%s' (use names like BACK, HOME, ENTER, A or a keycode number)", k);
        }
        key[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
    }
    key[n] = '\0';
    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    SoulXInput* in = open_input(&t);
    if (!in) {
        return CLI_EXIT_FAIL;
    }
    rc = soulx_input_key(in, key) == 0 ? CLI_EXIT_OK : CLI_EXIT_FAIL;
    if (rc != CLI_EXIT_OK) {
        soulx_log(SOULX_LOG_ERROR, "could not press %s on '%s'", key, t.conf.name);
    }
    soulx_input_close(in);
    return rc;
}

static int cmd_text(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos == 0) {
        return usage_error(self, "what should I type? e.g.: soulx text \"hello world\"");
    }
    char text[CLI_TEXT_MAX + 1];
    if (!join_words(a->pos, a->npos, text, sizeof text)) {
        return usage_error(self, "text too long (at most %d bytes)", CLI_TEXT_MAX);
    }
    if (text[0] == '\0' || has_control_chars(text)) {
        return usage_error(self, "the text must be one non-empty line (use 'soulx key ENTER' for Enter)");
    }
    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    SoulXInput* in = open_input(&t);
    if (!in) {
        return CLI_EXIT_FAIL;
    }
    rc = soulx_input_text(in, text) == 0 ? CLI_EXIT_OK : CLI_EXIT_FAIL;
    if (rc != CLI_EXIT_OK && !g_soulx_interrupted) {
        soulx_log(SOULX_LOG_ERROR, "could not type the text on '%s'", t.conf.name);
    }
    soulx_input_close(in);
    return rc;
}

// ./soulx-<instance>-<YYYYmmdd-HHMMSS>[-N].png that does not exist yet
static int default_screenshot_path(const char* name, char* buf, size_t cap) {
    time_t now = time(NULL);
    struct tm lt;
    if (now == (time_t)-1 || !localtime_r(&now, &lt)) {
        return -1;
    }
    char stamp[32];
    if (strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &lt) == 0) {
        return -1;
    }
    for (int k = 1; k < 100; k++) {
        int n = (k == 1) ? snprintf(buf, cap, "./soulx-%s-%s.png", name, stamp)
                         : snprintf(buf, cap, "./soulx-%s-%s-%d.png", name, stamp, k);
        if (n < 0 || (size_t)n >= cap) {
            return -1;
        }
        struct stat sb;
        if (lstat(buf, &sb) != 0 && errno == ENOENT) {
            return 0;
        }
    }
    return -1;
}

static int default_recording_path(const char* name, char* buf, size_t cap) {
    time_t now = time(NULL);
    struct tm lt;
    if (now == (time_t)-1 || !localtime_r(&now, &lt)) {
        return -1;
    }
    char stamp[32];
    if (strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &lt) == 0) {
        return -1;
    }
    for (int k = 1; k < 100; k++) {
        int n = (k == 1) ? snprintf(buf, cap, "./soulx-%s-%s.webm", name, stamp)
                         : snprintf(buf, cap, "./soulx-%s-%s-%d.webm", name, stamp, k);
        if (n < 0 || (size_t)n >= cap) {
            return -1;
        }
        struct stat sb;
        if (lstat(buf, &sb) != 0 && errno == ENOENT) {
            return 0;
        }
    }
    return -1;
}

static int cmd_screenrecord(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos > 1) {
        return usage_error(self, "too many arguments: '%s'", a->pos[1]);
    }
    if (a->npos == 1 && !output_file_ok(a->pos[0], true)) {
        return CLI_EXIT_FAIL;
    }
    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    char path[PATH_MAX];
    if (a->npos == 1) {
        if (!copy_str(path, sizeof path, a->pos[0])) {
            soulx_log(SOULX_LOG_ERROR, "%s: path too long", a->pos[0]);
            return CLI_EXIT_FAIL;
        }
    } else if (default_recording_path(t.conf.name, path, sizeof path) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot choose a file name; give one: soulx screenrecord out.webm");
        return CLI_EXIT_FAIL;
    }
    // The engine writes the WebM itself (console 'screenrecord'); it needs a .webm path with no spaces.
    if (soulx_start_recording(t.conf.id, path) != SOULX_OK) {
        return CLI_EXIT_FAIL;
    }
    printf("soulx: recording '%s' to %s\n", t.conf.name, path);
    printf("soulx: press Ctrl+C to stop (the engine stops on its own after 3 minutes)\n");
    fflush(stdout);
    double start = soulx_now_ms();
    while (!g_soulx_interrupted && soulx_now_ms() - start < 178000.0) {   // just under the engine's 3-min cap
        soulx_sleep_ms(200);
    }
    if (soulx_stop_recording(t.conf.id) != SOULX_OK) {
        soulx_log(SOULX_LOG_WARN, "could not confirm the recording stopped; check %s", path);
    }
    struct stat sb;
    if (stat(path, &sb) != 0 || sb.st_size == 0) {
        soulx_sleep_ms(500);                                            // the engine flushes a moment after 'stop'
    }
    if (stat(path, &sb) == 0 && sb.st_size > 0) {
        printf("soulx: saved %s (%.1f MB)\n", path, (double)sb.st_size / 1e6);
    } else {
        printf("soulx: recording stopped (%s)\n", path);
    }
    return g_soulx_interrupted ? CLI_EXIT_INTR : CLI_EXIT_OK;
}

// ---------------------------------------------------------------------------
// Commands: sound and resources
// ---------------------------------------------------------------------------

#define VOLUME_STREAM           "3"         // STREAM_MUSIC: what games play on

enum { VOLUME_JSON };
static const CliOpt k_volume_opts[] = {
    [VOLUME_JSON] = { "json", 0, false },
};

// Reads "-> N" from a line of 'cmd audio' output that contains |what|.
static bool audio_reply_int(const char* out, const char* what, int* v) {
    const char* p = out ? strstr(out, what) : NULL;
    if (!p) return false;
    const char* arrow = strstr(p, "-> ");
    const char* eol = strchr(p, '\n');
    if (!arrow || (eol && arrow > eol)) return false;
    char* end = NULL;
    long n = strtol(arrow + 3, &end, 10);
    if (end == arrow + 3 || n < 0 || n > 1000) return false;
    *v = (int)n;
    return true;
}

static int cmd_volume(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos > 1) {
        return usage_error(self, "expected one value: 0-100, mute or unmute");
    }
    char set[96] = "";
    if (a->npos == 1) {
        const char* v = a->pos[0];
        int pct = 0;
        if (!strcmp(v, "mute")) {
            copy_str(set, sizeof set, "cmd audio adj-mute " VOLUME_STREAM " >/dev/null; ");
        } else if (!strcmp(v, "unmute")) {
            copy_str(set, sizeof set, "cmd audio adj-unmute " VOLUME_STREAM " >/dev/null; ");
        } else if (parse_int(v, 0, 100, &pct)) {
            set[0] = '\x01';                    // the level needs the stream's maximum first
        } else {
            return usage_error(self, "volume must be 0-100, mute or unmute (got '%s')", v);
        }
    }
    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    static const char k_query[] = "cmd audio get-stream-volume " VOLUME_STREAM "; cmd audio get-max-volume "
                                  VOLUME_STREAM "; echo muted=$(cmd audio is-stream-mute " VOLUME_STREAM ")";
    char* out = NULL;
    int level = 0, max = 0;
    if (set[0] == '\x01') {
        if (soulx_adb_shell(t.serial, k_query, &out, CLI_SHELL_TIMEOUT_MS) != 0 ||
            !audio_reply_int(out, "getStreamMaxVolume", &max) || max <= 0) {
            free(out);
            soulx_log(SOULX_LOG_ERROR, "could not read the volume range of '%s'", t.conf.name);
            return CLI_EXIT_FAIL;
        }
        free(out);
        out = NULL;
        int pct = 0;
        (void)parse_int(a->pos[0], 0, 100, &pct);
        snprintf(set, sizeof set, "cmd audio adj-unmute " VOLUME_STREAM " >/dev/null; cmd audio set-volume "
                 VOLUME_STREAM " %d >/dev/null; ", (pct * max + 50) / 100);
    }
    char cmd[512];
    snprintf(cmd, sizeof cmd, "%s%s", set, k_query);
    if (soulx_adb_shell(t.serial, cmd, &out, CLI_SHELL_TIMEOUT_MS) != 0 ||
        !audio_reply_int(out, "getStreamVolume", &level) || !audio_reply_int(out, "getStreamMaxVolume", &max) ||
        max <= 0) {
        free(out);
        soulx_log(SOULX_LOG_ERROR, "could not %s the volume of '%s'", a->npos ? "change" : "read", t.conf.name);
        return CLI_EXIT_FAIL;
    }
    bool muted = strstr(out, "muted=true") != NULL;
    free(out);
    int pct = (level * 100 + max / 2) / max;
    if (a->val[VOLUME_JSON]) {
        printf("{\"name\": ");
        json_str(stdout, t.conf.name);
        printf(", \"volume\": %d, \"level\": %d, \"max\": %d, \"muted\": %s, \"audio\": %s}\n", pct, level, max,
               muted ? "true" : "false", t.conf.audio ? "true" : "false");
    } else {
        printf("%s: volume %d%% (%d/%d)%s\n", t.conf.name, pct, level, max, muted ? ", muted" : "");
        if (!t.conf.audio) {
            printf("note: '%s' has no sound device (audio off); turn it on with: soulx config %s --audio\n",
                   t.conf.name, t.conf.name);
        }
    }
    return CLI_EXIT_OK;
}

enum { STATS_JSON, STATS_WATCH, STATS_INTERVAL };
static const CliOpt k_stats_opts[] = {
    [STATS_JSON]     = { "json",     0, false },
    [STATS_WATCH]    = { "watch",    0, false },
    [STATS_INTERVAL] = { "interval", 0, true  },
};

#define STATS_INTERVAL_DEFAULT  1000        // ms
#define STATS_INTERVAL_MIN      200
#define STATS_INTERVAL_MAX      60000
#define STATS_MAX               64

typedef struct {
    SoulXInstanceConf conf;
    SoulXRunState     st;
    uint64_t          cpu_ns;               // user + system time of the engine
    bool              ok;
} StatsRow;

static uint64_t mach_to_ns(uint64_t t) {
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    return t * tb.numer / tb.denom;
}

static uint64_t now_ns(void) {
    return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
}

static bool engine_cpu_ns(pid_t pid, uint64_t* ns, uint64_t* footprint) {
    struct rusage_info_v2 ri;
    memset(&ri, 0, sizeof ri);
    if (pid <= 0 || proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t*)&ri) != 0) return false;
    *ns = mach_to_ns(ri.ri_user_time + ri.ri_system_time);     // rusage times are mach ticks on arm64
    if (footprint) *footprint = ri.ri_phys_footprint;
    return true;
}

// Sleeps |ms| in short steps; false when Ctrl+C arrived.
static bool stats_sleep(int ms) {
    for (int left = ms; left > 0; left -= 50) {
        if (soulx_interrupted()) return false;
        if (getppid() == 1) return false;           // orphaned: the process that started us is gone
        struct timespec ts = { 0, (long)(left < 50 ? left : 50) * 1000000L };
        nanosleep(&ts, NULL);
    }
    return !soulx_interrupted();
}

static int cmd_stats(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos > 1) {
        return usage_error(self, "too many arguments: '%s'", a->pos[1]);
    }
    int interval = STATS_INTERVAL_DEFAULT;
    const char* v = a->val[STATS_INTERVAL];
    if (v && !parse_int(v, STATS_INTERVAL_MIN, STATS_INTERVAL_MAX, &interval)) {
        return range_error(self, "--interval", v, STATS_INTERVAL_MIN, STATS_INTERVAL_MAX, " ms");
    }
    const char* only = a->npos == 1 ? a->pos[0] : (a->instance ? a->instance : g->instance);
    SoulXInstanceConf one;
    if (only && load_instance(only, &one) != 0) {
        return CLI_EXIT_FAIL;
    }
    bool json = a->val[STATS_JSON] != NULL;
    bool watch = a->val[STATS_WATCH] != NULL;
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    uint64_t host_mem = 0;
    size_t len = sizeof host_mem;
    (void)sysctlbyname("hw.memsize", &host_mem, &len, NULL, 0);

    static StatsRow rows[STATS_MAX];
    static SoulXInstanceConf confs[STATS_MAX];
    do {
        int n = 0;
        if (only) {
            confs[0] = one;
            n = 1;
        } else {
            n = soulx_reg_list(confs, STATS_MAX);
            if (n < 0) n = 0;
        }
        int nrows = 0;
        for (int i = 0; i < n; i++) {
            StatsRow* r = &rows[nrows];
            memset(r, 0, sizeof *r);
            r->conf = confs[i];
            if (!soulx_instance_running(r->conf.name, &r->st)) {
                if (only) nrows++;          // a named instance is shown even when stopped
                continue;
            }
            r->ok = engine_cpu_ns(r->st.pid, &r->cpu_ns, NULL);
            nrows++;
        }
        uint64_t t0 = now_ns();
        if (!stats_sleep(interval)) break;
        uint64_t dt = now_ns() - t0;

        if (json) fputs("{\"host\": {", stdout);
        if (json) printf("\"cpus\": %d, \"memory_mb\": %llu}, \"instances\": [", ncpu,
                         (unsigned long long)(host_mem / (1024 * 1024)));
        else printf("%-16s %-8s %8s %10s %8s\n", "NAME", "STATE", "CPU", "MEMORY", "UPTIME");
        bool first = true;
        for (int i = 0; i < nrows; i++) {
            StatsRow* r = &rows[i];
            uint64_t ns = 0, fp = 0;
            bool live = r->ok && engine_cpu_ns(r->st.pid, &ns, &fp) && ns >= r->cpu_ns && dt > 0;
            double cpu = live ? (double)(ns - r->cpu_ns) * 100.0 / (double)dt : 0;   // 100 = one full core
            long long up = live ? seconds_since(r->st.started) : -1;
            if (json) {
                printf("%s{\"name\": ", first ? "" : ", ");
                json_str(stdout, r->conf.name);
                printf(", \"state\": \"%s\"", live ? "running" : "stopped");
                if (live) {
                    printf(", \"pid\": %d, \"cpu_percent\": %.1f, \"memory_mb\": %llu, \"uptime_s\": %lld, "
                           "\"cores\": %d, \"ram_mb\": %d, \"fps\": %d}", (int)r->st.pid, cpu,
                           (unsigned long long)(fp / (1024 * 1024)), up, r->conf.cores, r->conf.ram_mb, r->conf.fps);
                } else {
                    fputs(", \"pid\": null, \"cpu_percent\": null, \"memory_mb\": null, \"uptime_s\": null}", stdout);
                }
            } else if (live) {
                char upt[32];
                snprintf(upt, sizeof upt, "%lld:%02lld:%02lld", up / 3600, up / 60 % 60, up % 60);
                printf("%-16s %-8s %7.1f%% %7llu MB %8s\n", r->conf.name, "running", cpu,
                       (unsigned long long)(fp / (1024 * 1024)), upt);
            } else {
                printf("%-16s %-8s %8s %10s %8s\n", r->conf.name, "stopped", "-", "-", "-");
            }
            first = false;
        }
        if (json) fputs("]}\n", stdout);
        else if (nrows == 0) printf("(no running instances)\n");
        else printf("CPU: 100%% = one core of %d; memory: the engine's footprint\n", ncpu);
        fflush(stdout);
        if (watch && !json) putchar('\n');
    } while (watch && !soulx_interrupted() && getppid() != 1);  // ends with its parent (a launcher that quit)
    return CLI_EXIT_OK;
}

static int cmd_screenshot(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos > 1) {
        return usage_error(self, "too many arguments: '%s'", a->pos[1]);
    }
    if (a->npos == 1 && !output_file_ok(a->pos[0], true)) {
        return CLI_EXIT_FAIL;
    }
    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    char path[PATH_MAX];
    if (a->npos == 1) {
        if (!copy_str(path, sizeof path, a->pos[0])) {
            soulx_log(SOULX_LOG_ERROR, "%s: path too long", a->pos[0]);
            return CLI_EXIT_FAIL;
        }
    } else if (default_screenshot_path(t.conf.name, path, sizeof path) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot choose a file name for the screenshot; give one: soulx screenshot out.png");
        return CLI_EXIT_FAIL;
    }
    // Fast path: soulx_input_capture is the console screenshot (already rotated upright), ~5x faster
    // than the adb screencap that soulx_screen_save_png uses; fall back to that path if it is unavailable.
    rc = -1;
    SoulXInput* in = soulx_input_open(t.conf.name);
    if (in) {
        SoulXImage img;
        if (soulx_input_capture(in, &img) == 0) {
            rc = soulx_image_save_png(path, &img);
            soulx_image_free(&img);
        }
        soulx_input_close(in);
    }
    if (rc != 0 && !g_soulx_interrupted) {
        rc = soulx_screen_save_png(t.serial, path);
    }
    if (rc != 0) {
        if (!g_soulx_interrupted) {
            soulx_log(SOULX_LOG_ERROR, "could not take a screenshot of '%s'", t.conf.name);
        }
        return g_soulx_interrupted ? CLI_EXIT_INTR : CLI_EXIT_FAIL;
    }
    printf("%s\n", path);
    return CLI_EXIT_OK;
}

// ---------------------------------------------------------------------------
// Commands: tools
// ---------------------------------------------------------------------------

// adb's own exit status, or a failure when adb could not be run at all.
static int passthrough_exit(int status) {
    if (status < 0) {
        soulx_log(SOULX_LOG_ERROR, "could not run SoulX's adb (is the runtime installed? run: soulx setup)");
        return CLI_EXIT_FAIL;
    }
    return status > 255 ? CLI_EXIT_FAIL : status;
}

static bool adb_installed(void) {
    char adb[PATH_MAX];
    if (soulx_tool_path("adb", adb, sizeof adb) != 0 || access(adb, X_OK) != 0) {
        soulx_log(SOULX_LOG_ERROR, "SoulX's adb is not installed yet; run: soulx setup");
        return false;
    }
    return true;
}

// Runs adb with the given words after a fixed first word (NULL = none).
static int adb_with_words(const char* serial, const char* first, char* const* words, int n) {
    const char** args = calloc((size_t)n + 2, sizeof *args);
    if (!args) {
        return out_of_memory();
    }
    int k = 0;
    if (first) {
        args[k++] = first;
    }
    for (int i = 0; i < n; i++) {
        args[k++] = words[i];
    }
    args[k] = NULL;
    fflush(stdout);     // adb writes to the same terminal
    int status = soulx_adb_passthrough(serial, args);
    free(args);
    return passthrough_exit(status);
}

static int cmd_adb(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    // adb commands that talk to the adb server only, and work without a running instance
    static const char* const server_cmds[] = {
        "devices", "version", "start-server", "kill-server", "help", "--version", "--help"
    };
    bool server_only = a->npos == 0;
    for (size_t i = 0; !server_only && i < sizeof server_cmds / sizeof server_cmds[0]; i++) {
        server_only = strcmp(a->pos[0], server_cmds[i]) == 0;
    }
    if (!adb_installed()) {
        return CLI_EXIT_FAIL;
    }
    if (server_only) {
        const char* ref = a->instance ? a->instance : g->instance;
        if (!ref) {
            ref = default_instance_name();
        }
        char serial[64] = "";
        SoulXInstanceConf c;
        SoulXRunState st;
        if (instance_ref_valid(ref) && soulx_resolve_instance(ref, &c) == 0 && soulx_instance_running(c.name, &st)) {
            soulx_serial(st.port, serial, sizeof serial);
        }
        return adb_with_words(serial[0] ? serial : NULL, NULL, a->pos, a->npos);
    }
    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    return adb_with_words(t.serial, NULL, a->pos, a->npos);
}

static int cmd_shell(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    return adb_with_words(t.serial, "shell", a->pos, a->npos);
}

static int cmd_logcat(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    return adb_with_words(t.serial, "logcat", a->pos, a->npos);
}

static int cmd_console(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    if (a->npos == 0) {
        return usage_error(self, "which console command? e.g.: soulx console help");
    }
    char line[CLI_CONSOLE_MAX + 1];
    if (!join_words(a->pos, a->npos, line, sizeof line)) {
        return usage_error(self, "console command too long (at most %d characters)", CLI_CONSOLE_MAX);
    }
    if (has_control_chars(line)) {
        return usage_error(self, "the console command must be a single line of text");
    }
    CliTarget t;
    int rc = cli_target(self, a, g, &t);
    if (rc != 0) {
        return rc;
    }
    char* reply = malloc(CLI_CONSOLE_REPLY);
    if (!reply) {
        return out_of_memory();
    }
    reply[0] = '\0';
    int r = soulx_console_oneshot(t.st.port, line, reply, CLI_CONSOLE_REPLY);
    reply[CLI_CONSOLE_REPLY - 1] = '\0';
    if (r == 0) {
        rtrim(reply);
        if (reply[0]) {
            printf("%s\n", reply);
        }
        rc = CLI_EXIT_OK;
    } else if (r == 1) {
        rtrim(reply);
        if (strncmp(reply, "KO", 2) == 0) {
            soulx_log(SOULX_LOG_ERROR, "%s", reply);
        } else {
            soulx_log(SOULX_LOG_ERROR, "KO: %s", nz(reply, "command failed"));
        }
        rc = CLI_EXIT_FAIL;
    } else {
        soulx_log(SOULX_LOG_ERROR, "cannot talk to the emulator console of '%s' (port %d)", t.conf.name, t.st.port);
        rc = CLI_EXIT_FAIL;
    }
    free(reply);
    return rc;
}

static int cmd_doctor(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    (void)g;
    int rc = no_positionals(self, a);
    if (rc != 0) {
        return rc;
    }
    return soulx_doctor() == 0 ? CLI_EXIT_OK : CLI_EXIT_FAIL;
}

enum { SCRIPTS_JSON };
static const CliOpt k_scripts_opts[] = {
    [SCRIPTS_JSON] = { "json", 0, false },
};

#define SCRIPTS_LIST_MAX 512

static void json_scripts(const char* instance) {
    static SoulXScript list[SCRIPTS_LIST_MAX];
    char folder[PATH_MAX] = "";
    (void)soulx_scripts_dir(instance, folder, sizeof folder, false);
    int n = soulx_scripts_list(instance, list, SCRIPTS_LIST_MAX);
    if (n > SCRIPTS_LIST_MAX) n = SCRIPTS_LIST_MAX;
    printf("{\"instance\": ");
    json_str(stdout, instance);
    printf(", \"folder\": ");
    json_str(stdout, folder);
    printf(", \"scripts\": [");
    for (int i = 0; i < n; i++) {
        const SoulXScript* s = &list[i];
        printf("%s{\"file\": ", i ? ", " : "");
        json_str(stdout, s->file);
        printf(", \"entry\": ");
        json_str(stdout, s->top);
        printf(", \"path\": ");
        json_str(stdout, s->path);
        printf(", \"name\": ");
        json_str_or_null(stdout, s->name[0] ? s->name : NULL);
        printf(", \"package\": %s, \"size\": %lld, \"modified\": %lld}", s->package ? "true" : "false", s->size,
               (long long)s->modified);
    }
    printf("]}\n");
}

// soulx scripts [-i NAME] [list|folder|import PATH...|remove ENTRY|rename FILE NEW NAME...] [--json]
static int cmd_scripts(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    const char* instance = a->instance ? a->instance : g->instance ? g->instance : default_instance_name();
    SoulXInstanceConf conf;
    if (load_instance(instance, &conf) != 0) {
        return CLI_EXIT_FAIL;
    }
    instance = conf.name;
    const char* sub = a->npos ? a->pos[0] : "list";
    bool json = a->val[SCRIPTS_JSON] != NULL;
    int rc = CLI_EXIT_OK;
    if (!strcmp(sub, "list")) {
        if (a->npos > 1) return usage_error(self, "list takes no arguments");
    } else if (!strcmp(sub, "folder")) {
        char folder[PATH_MAX];
        if (soulx_scripts_dir(instance, folder, sizeof folder, true) != 0) return CLI_EXIT_FAIL;
        printf("%s\n", folder);
        return CLI_EXIT_OK;
    } else if (!strcmp(sub, "import")) {
        if (a->npos < 2) return usage_error(self, "what should I import? e.g.: soulx scripts import farm.lua");
        for (int i = 1; i < a->npos; i++) {
            char dest[PATH_MAX];
            if (soulx_scripts_import(instance, a->pos[i], dest, sizeof dest) != 0) { rc = CLI_EXIT_FAIL; continue; }
            if (!json) printf("imported %s\n", dest);
        }
    } else if (!strcmp(sub, "remove")) {
        if (a->npos != 2) return usage_error(self, "which entry? e.g.: soulx scripts remove farm.lua");
        if (soulx_scripts_trash(instance, a->pos[1]) != 0) return CLI_EXIT_FAIL;
        if (!json) printf("moved %s to the Trash\n", a->pos[1]);
    } else if (!strcmp(sub, "rename")) {
        if (a->npos < 3) return usage_error(self, "e.g.: soulx scripts rename farm.lua Daily farm");
        char name[128] = "";
        for (int i = 2; i < a->npos; i++) {
            if (i > 2 && !str_append(name, sizeof name, " ")) break;
            if (!str_append(name, sizeof name, a->pos[i])) break;
        }
        if (soulx_scripts_set_name(instance, a->pos[1], name) != 0) return CLI_EXIT_FAIL;
        if (!json) printf("%s is now named \"%s\"\n", a->pos[1], name);
    } else {
        return usage_error(self, "unknown action '%s' (use list, folder, import, remove or rename)", sub);
    }
    if (json) {
        json_scripts(instance);
    } else if (!strcmp(sub, "list")) {
        static SoulXScript list[SCRIPTS_LIST_MAX];
        char folder[PATH_MAX] = "";
        (void)soulx_scripts_dir(instance, folder, sizeof folder, false);
        int n = soulx_scripts_list(instance, list, SCRIPTS_LIST_MAX);
        if (n > SCRIPTS_LIST_MAX) n = SCRIPTS_LIST_MAX;
        printf("Scripts of '%s' (%s):\n", instance, folder);
        if (n <= 0) printf("  (none yet: soulx scripts -i %s import farm.lua)\n", instance);
        for (int i = 0; i < n; i++) {
            printf("  %-28s %s\n", list[i].name[0] ? list[i].name : "(no name)", list[i].file);
        }
    }
    return rc;
}

enum { MCP_ENABLE, MCP_DISABLE, MCP_INSTANCES, MCP_ALLOW, MCP_DENY, MCP_JSON, MCP_LINES };
static const CliOpt k_mcp_opts[] = {
    [MCP_ENABLE]    = { "enable",    0, false },
    [MCP_DISABLE]   = { "disable",   0, false },
    [MCP_INSTANCES] = { "instances", 0, true  },
    [MCP_ALLOW]     = { "allow",     0, true  },
    [MCP_DENY]      = { "deny",      0, true  },
    [MCP_JSON]      = { "json",      0, false },
    [MCP_LINES]     = { "lines",     'n', true },
};

static bool* mcp_perm(SoulXMcpConfig* c, const char* name) {
    if (!strcmp(name, "start-stop")) return &c->allow_start_stop;
    if (!strcmp(name, "install")) return &c->allow_install;
    if (!strcmp(name, "scripts")) return &c->allow_scripts;
    if (!strcmp(name, "lua")) return &c->allow_lua;
    return NULL;
}

static void self_path(char* out, size_t cap) {
    char buf[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(getpid(), buf, sizeof buf) > 0) copy_str(out, cap, buf);
    else copy_str(out, cap, "soulx");
}

static int cmd_mcp(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    (void)g;
    const char* sub = a->npos ? a->pos[0] : "serve";
    char exe[PATH_MAX];
    self_path(exe, sizeof exe);
    if (!strcmp(sub, "serve")) {
        if (a->npos > 1) return usage_error(self, "serve takes no arguments");
        if (soulx_ensure_layout() != 0) return CLI_EXIT_FAIL;
        SoulXMcpConfig c;
        soulx_mcp_config_load(&c);
        if (!c.enabled) {
            // keep serving: the client gets a clear answer on every call, and switching it on needs no restart
            soulx_log(SOULX_LOG_WARN, "AI control is turned off: tools answer with an error until it is enabled "
                     "(launcher > AI control, or: soulx mcp config --enable)");
        }
        return soulx_mcp_serve(exe) == 0 ? CLI_EXIT_OK : CLI_EXIT_FAIL;
    }
    if (!strcmp(sub, "config")) {
        if (a->npos > 1) return usage_error(self, "config takes options only");
        if (a->val[MCP_ENABLE] && a->val[MCP_DISABLE]) return usage_error(self, "--enable and --disable together");
        SoulXMcpConfig c;
        soulx_mcp_config_load(&c);
        bool changed = false;
        if (a->val[MCP_ENABLE]) { c.enabled = true; changed = true; }
        if (a->val[MCP_DISABLE]) { c.enabled = false; changed = true; }
        const char* v;
        if ((v = a->val[MCP_INSTANCES])) {
            if (strcmp(v, "*") != 0) {
                char copy[512];
                if (!copy_str(copy, sizeof copy, v)) return usage_error(self, "--instances is too long");
                char* save = NULL;
                for (char* n = strtok_r(copy, ",", &save); n; n = strtok_r(NULL, ",", &save)) {
                    while (*n == ' ') n++;
                    if (!soulx_name_valid(n)) return usage_error(self, "'%s' is not an instance name", n);
                }
            }
            copy_str(c.instances, sizeof c.instances, v);
            changed = true;
        }
        for (int k = 0; k < 2; k++) {
            const char* list = a->val[k == 0 ? MCP_ALLOW : MCP_DENY];
            if (!list) continue;
            char copy[256];
            copy_str(copy, sizeof copy, list);
            char* save = NULL;
            for (char* n = strtok_r(copy, ",", &save); n; n = strtok_r(NULL, ",", &save)) {
                bool* p = mcp_perm(&c, n);
                if (!p) return usage_error(self, "unknown permission '%s' (start-stop, install, scripts, lua)", n);
                *p = k == 0;
                changed = true;
            }
        }
        if (changed && soulx_mcp_config_save(&c) != 0) {
            soulx_log(SOULX_LOG_ERROR, "could not save %s/mcp.conf", soulx_home());
            return CLI_EXIT_FAIL;
        }
        if (changed) soulx_mcp_audit("config", NULL, c.enabled ? "settings changed (on)" : "settings changed (off)", true);
        if (a->val[MCP_JSON]) {
            printf("{\"enabled\": %s, \"instances\": ", c.enabled ? "true" : "false");
            json_str(stdout, c.instances);
            printf(", \"allow_start_stop\": %s, \"allow_install\": %s, \"allow_scripts\": %s, \"allow_lua\": %s, "
                   "\"command\": ", c.allow_start_stop ? "true" : "false", c.allow_install ? "true" : "false",
                   c.allow_scripts ? "true" : "false", c.allow_lua ? "true" : "false");
            json_str(stdout, exe);
            printf("}\n");
        } else {
            printf("AI control (MCP): %s\n", c.enabled ? "on" : "off");
            printf("  instances      %s\n", c.instances);
            printf("  start-stop     %s\n", c.allow_start_stop ? "allowed" : "not allowed");
            printf("  install        %s\n", c.allow_install ? "allowed" : "not allowed");
            printf("  scripts        %s\n", c.allow_scripts ? "allowed" : "not allowed");
            printf("  lua            %s (always sandboxed)\n", c.allow_lua ? "allowed" : "not allowed");
        }
        return CLI_EXIT_OK;
    }
    if (!strcmp(sub, "log")) {
        int lines = 50;
        const char* v = a->val[MCP_LINES];
        if (v && !parse_int(v, 1, 10000, &lines)) return range_error(self, "--lines", v, 1, 10000, "");
        char path[PATH_MAX];
        char* data = NULL;
        size_t len = 0;
        if (soulx_path(path, sizeof path, "logs/mcp.log") != 0 || soulx_read_file(path, &data, &len) != 0) {
            if (a->val[MCP_JSON]) printf("[]\n"); else printf("(no AI control activity yet)\n");
            return CLI_EXIT_OK;
        }
        // the last N lines
        const char* start = data + len;
        for (int seen = 0; start > data; ) {
            if (start[-1] == '\n' && start != data + len && ++seen > lines) break;
            start--;
        }
        if (!a->val[MCP_JSON]) {
            fputs(start, stdout);
        } else {
            printf("[");
            bool first = true;
            char* save = NULL;
            for (char* line = strtok_r((char*)start, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                char* f[5] = { 0 };
                int k = 0;
                char* tsave = NULL;
                for (char* t = strtok_r(line, "\t", &tsave); t && k < 5; t = strtok_r(NULL, "\t", &tsave)) f[k++] = t;
                if (k < 4) continue;
                printf("%s{\"time\": ", first ? "" : ", ");
                json_str(stdout, f[0]);
                printf(", \"ok\": %s, \"tool\": ", !strcmp(f[1], "ok") ? "true" : "false");
                json_str(stdout, f[2]);
                printf(", \"instance\": ");
                json_str(stdout, f[3]);
                printf(", \"detail\": ");
                json_str(stdout, f[4] ? f[4] : "");
                printf("}");
                first = false;
            }
            printf("]\n");
        }
        free(data);
        return CLI_EXIT_OK;
    }
    if (!strcmp(sub, "setup")) {
        printf("Connect an AI client to SoulX (it starts `%s mcp` and talks to it over stdin/stdout;\n"
               "no network port is opened):\n\n", exe);
        printf("  In the AI app, add an MCP server (stdio):\n    command:   %s\n    arguments: mcp\n\n", exe);
        printf("  As JSON (most apps):\n    \"mcpServers\": { \"soulx\": { \"command\": \"%s\", \"args\": [\"mcp\"] } }\n\n", exe);
        SoulXMcpConfig c;
        soulx_mcp_config_load(&c);
        printf("AI control is %s. %s\n", c.enabled ? "on" : "off",
               c.enabled ? "" : "Turn it on in the launcher (AI control) or: soulx mcp config --enable");
        return CLI_EXIT_OK;
    }
    return usage_error(self, "unknown action '%s' (use serve, config, log or setup)", sub);
}

enum { LANGUAGE_JSON };
static const CliOpt k_language_opts[] = {
    [LANGUAGE_JSON] = { "json", 0, false },
};

static int cmd_language(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    (void)g;
    if (a->npos > 1) {
        return usage_error(self, "expected one language: system, en, zh-hant or zh-hans");
    }
    if (a->npos == 1) {
        if (!soulx_lang_canonical(a->pos[0])) {
            return usage_error(self, "unknown language '%s' (use system, en, zh-hant or zh-hans)", a->pos[0]);
        }
        if (soulx_set_ui_language(a->pos[0]) != 0) {
            soulx_log(SOULX_LOG_ERROR, "could not save the language in %s", soulx_home());
            return CLI_EXIT_FAIL;
        }
    }
    const char* lang = soulx_ui_language();
    if (a->val[LANGUAGE_JSON]) {
        printf("{\"language\": ");
        json_str(stdout, lang);
        printf(", \"from_env\": %s}\n", soulx_lang_canonical(getenv("SOULX_LANG")) ? "true" : "false");
    } else {
        printf("%s\n", lang);
    }
    return CLI_EXIT_OK;
}

enum { RUNTIME_JSON };
static const CliOpt k_runtime_opts[] = {
    [RUNTIME_JSON] = { "json", 0, false },
};

static int cmd_runtime(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    (void)g;
    int rc = no_positionals(self, a);
    if (rc != 0) {
        return rc;
    }
    if (!a->val[RUNTIME_JSON]) {
        return soulx_runtime_print_status() == 0 ? CLI_EXIT_OK : CLI_EXIT_FAIL;
    }
    // for launchers: every package, whether it is installed, and whether the default image can run
    size_t n = 0;
    const SoulXPackage* pkgs = soulx_runtime_packages(&n);
    const SoulXPackage* def = soulx_runtime_default_image();
    printf("{\"home\": ");
    json_str(stdout, soulx_home());
    printf(", \"default_image\": ");
    json_str_or_null(stdout, def ? def->key : NULL);
    printf(", \"ready\": %s, \"packages\": [", def && soulx_runtime_ready(def->key) ? "true" : "false");
    for (size_t i = 0; i < n; i++) {
        const SoulXPackage* p = &pkgs[i];
        printf("%s{\"key\": ", i ? ", " : "");
        json_str(stdout, p->key);
        printf(", \"kind\": \"%s\", \"version\": ", p->kind == SOULX_PKG_ENGINE ? "engine" :
               p->kind == SOULX_PKG_PLATFORM_TOOLS ? "platform-tools" : "image");
        json_str(stdout, p->version);
        printf(", \"download_bytes\": %llu, \"installed\": %s, \"description\": ", (unsigned long long)p->size,
               soulx_runtime_installed(p) ? "true" : "false");
        json_str_or_null(stdout, p->description);
        if (p->kind == SOULX_PKG_IMAGE) {
            printf(", \"android\": ");
            json_str_or_null(stdout, p->android_name);
            printf(", \"playstore\": %s, \"page16k\": %s", p->playstore ? "true" : "false", p->page16k ? "true" : "false");
        }
        putchar('}');
    }
    printf("]}\n");
    return CLI_EXIT_OK;
}

// API level from an image target such as "android-37.0" (0 if unknown)
static int target_api(const char* target) {
    int api = 0;
    if (target && strncmp(target, "android-", 8) == 0) {
        char num[8];
        size_t n = strspn(target + 8, "0123456789");
        if (n > 0 && n < sizeof num) {
            memcpy(num, target + 8, n);
            num[n] = '\0';
            if (!parse_int(num, 1, 999, &api)) {
                api = 0;
            }
        }
    }
    return api;
}

static int cmd_version(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    (void)g;
    int rc = no_positionals(self, a);
    if (rc != 0) {
        return rc;
    }
    const SoulXPackage* engine = soulx_runtime_find("emulator");
    const SoulXPackage* img = soulx_runtime_default_image();
    int api = img ? target_api(img->target) : 0;
    printf("SoulX Android Emulator %s (macOS %s) \u2014 engine: Android Emulator %s, %s (API %d)\n",
           SOULX_VERSION_STRING, CLI_HOST_ARCH, engine ? nz(engine->version, "?") : "?",
           img ? nz(img->android_name, "Android") : "Android", api);
    return CLI_EXIT_OK;
}

static void print_overview(FILE* f) {
    static const char* const titles[GROUP_COUNT] = {
        "Getting started", "Instances", "Apps", "Automation", "Tools"
    };
    size_t n = 0;
    const CliCommand* cmds = command_table(&n);
    int width = 0;
    for (size_t i = 0; i < n; i++) {
        int w = (int)strlen(cmds[i].synopsis);
        if (w > width) {
            width = w;
        }
    }
    fprintf(f, "SoulX Android Emulator %s - play Android games on your Mac (Android 17, Apple Silicon)\n",
            SOULX_VERSION_STRING);
    for (int grp = 0; grp < GROUP_COUNT; grp++) {
        fprintf(f, "\n%s:\n", titles[grp]);
        for (size_t i = 0; i < n; i++) {
            if (cmds[i].group == grp) {
                fprintf(f, "  soulx %-*s  %s\n", width, cmds[i].synopsis, cmds[i].summary);
            }
        }
    }
    fputs("\n-i NAME picks the instance (default: $SOULX_INSTANCE, else \"default\"); -v shows debug output.\n"
          "Run 'soulx help <command>' for details. The v1.0 options (--create, --start, --list, ...) still\n"
          "work, one per command.\n",
          f);
}

static void print_command_help(FILE* f, const CliCommand* cmd) {
    fprintf(f, "usage: %s\nCommon options:\n", cmd->usage);
    if (cmd->uses_instance) {
        // create names the new instance and delete requires an explicit name: neither falls back to
        // $SOULX_INSTANCE/"default", so they do not advertise that default.
        const char* desc = "instance to use (default: $SOULX_INSTANCE, else \"default\")";
        if (strcmp(cmd->name, "delete") == 0) {
            desc = "instance to delete (no default; name it explicitly)";
        } else if (strcmp(cmd->name, "create") == 0) {
            desc = "name for the new instance (default: $SOULX_INSTANCE, else \"default\")";
        }
        fprintf(f, "  -i, --instance NAME   %s\n", desc);
    }
    fputs("  -v, --verbose         show debug output\n", f);
}

static int unknown_command(const CliCommand* ctx, const char* word);

static int cmd_help(const CliCommand* self, const CliArgs* a, const CliGlobal* g) {
    (void)g;
    if (a->npos == 0) {
        print_overview(stdout);
        return CLI_EXIT_OK;
    }
    if (a->npos > 1) {
        return usage_error(self, "too many arguments: '%s'", a->pos[1]);
    }
    const CliCommand* cmd = find_command(a->pos[0]);
    if (!cmd) {
        return unknown_command(self, a->pos[0]);
    }
    print_command_help(stdout, cmd);
    return CLI_EXIT_OK;
}

// ---------------------------------------------------------------------------
// Command table
// ---------------------------------------------------------------------------

#define NO_OPTS NULL, 0

static const CliCommand k_commands[] = {
    { "setup", cmd_setup, GROUP_START, "setup", "install SoulX's Android 17 runtime (one-time download)",
      "soulx setup [--image play|google|play16k] [--accept-licenses] [--keep-downloads] [--force]\n"
      "\n"
      "Downloads and installs SoulX's own Android runtime into $SOULX_HOME (default ~/.soulx-android):\n"
      "the Android Emulator engine, adb (platform-tools) and an Android 17 system image. Android\n"
      "Studio is not needed and is never used. Every download is checked (size and SHA-1).\n"
      "\n"
      "  --image KEY         system image to install:\n"
      "                        play     Android 17, Google Play (default)\n"
      "                        google   Android 17, Google APIs (adb root available)\n"
      "                        play16k  Android 17 QPR1, Google Play, 16 KB memory pages\n"
      "  --accept-licenses   accept the Android SDK licenses without asking\n"
      "  --keep-downloads    keep the downloaded zip files in $SOULX_HOME/downloads\n"
      "  --force             reinstall packages that are already installed\n",
      k_setup_opts, NOPTS(k_setup_opts), CLI_INTERLEAVED, false },

    { "play", cmd_play, GROUP_START, "play game.apk", "create an instance, install the game and play it",
      "soulx play [-i NAME] <game.apk|package> [--headless] [--reinstall]\n"
      "\n"
      "The one-step way to play: creates the instance on first use (with a landscape screen when the\n"
      "game asks for landscape), starts it and waits for Android, installs the game (skipped when the\n"
      "same version is already installed) and launches it. Accepts .apk, .xapk, .apks and .apkm files,\n"
      "a folder of split APKs, or the package name of an installed app. Run 'soulx setup' once first.\n"
      "\n"
      "  --headless    start the instance without a window (if it is not running yet)\n"
      "  --reinstall   install the game even if the same version is already installed\n"
      "\n"
      "Examples:\n"
      "  soulx play ~/Downloads/game.apk\n"
      "  soulx play -i racing com.example.racer\n",
      k_play_opts, NOPTS(k_play_opts), CLI_INTERLEAVED, true },

    { "create", cmd_create, GROUP_INSTANCES, "create [name]", "create an instance (--landscape, --profile ...)",
      "soulx create [name] [options]\n"
      "\n"
      "Creates an instance (an Android 17 phone). The name defaults to $SOULX_INSTANCE or \"default\";\n"
      "names use lowercase letters, digits, '-' and '_' (up to 32 characters).\n"
      "\n"
      "  --profile P       lite (2 CPUs, 4096 MB, 720x1600), balanced (default: 4 CPUs, 4096 MB,\n"
      "                    1080x2400) or performance (6 CPUs, 6144 MB, 1080x2400)\n"
      "  --landscape       landscape screen (wider than tall), for landscape games\n"
      "  --cores N         virtual CPUs, 1-8\n"
      "  --ram MB          memory in MB, 4096-12288\n"
      "  --res WxH         screen size in pixels, e.g. 1080x2400 (320-3840 per side)\n"
      "  --dpi N           screen density, 120-640\n"
      "  --data GB         storage for apps and data in GB, 4-64\n"
      "  --image KEY       system image: play (default), google or play16k (see 'soulx help setup')\n"
      "  --gpu MODE        host (default: the Mac's GPU), host-novulkan, software (slow; for games that\n"
      "                    render incorrectly) or metal (experimental: OpenGL ES through ANGLE on Metal)\n"
      "  --dns IP[,IP]     DNS servers to use instead of the Mac's (up to 4)\n"
      "  --no-stay-awake   let the screen turn off while the instance runs\n"
      "  --no-lean         keep every preinstalled Google app running (by default SoulX disables the ones\n"
      "                    games never need, such as YouTube, Photos and Gemini; about 1 GB less memory)\n"
      "  --fps N           frame rate cap, 20-60 (default 60): the Android screen refreshes N times a\n"
      "                    second, so games render at most that often (30 saves CPU and GPU for idle farming)\n"
      "  --no-audio        no sound (saves a little CPU; useful for instances that run in the background)\n"
      "\n"
      "Example: soulx create racing --profile performance --landscape\n",
      k_create_opts, NOPTS(k_create_opts), CLI_INTERLEAVED, true },

    { "config", cmd_config, GROUP_INSTANCES, "config [name]", "show or change the settings of an instance",
      "soulx config [name] [options] [--json]\n"
      "\n"
      "Without options, shows the settings of an instance (default: $SOULX_INSTANCE, else \"default\").\n"
      "With options, changes them. A stopped instance gets them at its next start; a running one keeps\n"
      "its current settings until it is restarted (soulx restart). The system image is fixed when the\n"
      "instance is created, and storage can only grow.\n"
      "\n"
      "  --profile P       lite, balanced or performance (CPUs, RAM, screen; keeps orientation and storage)\n"
      "  --landscape       landscape screen (wider than tall)\n"
      "  --portrait        portrait screen (taller than wide)\n"
      "  --cores N         virtual CPUs, 1-8\n"
      "  --ram MB          memory in MB, 4096-12288\n"
      "  --res WxH         screen size in pixels, e.g. 1080x2400 (320-3840 per side); its shape sets the\n"
      "                    orientation unless --landscape or --portrait is given\n"
      "  --dpi N           screen density, 120-640\n"
      "  --data GB         storage for apps and data in GB, up to 64 (only larger than now)\n"
      "  --gpu MODE        host, host-novulkan, software or metal (see 'soulx help create')\n"
      "  --dns IP[,IP]     DNS servers to use instead of the Mac's (up to 4)\n"
      "  --no-dns          use the Mac's DNS servers again\n"
      "  --stay-awake      keep the screen on while the instance runs\n"
      "  --no-stay-awake   let the screen turn off\n"
      "  --lean            disable the preinstalled Google apps games never need\n"
      "  --no-lean         enable them again\n"
      "  --fps N           frame rate cap, 20-60 (60 = no cap)\n"
      "  --audio           sound on\n"
      "  --no-audio        sound off (no audio device)\n"
      "  --json            print the settings (after any change) as JSON\n"
      "\n"
      "Examples:\n"
      "  soulx config\n"
      "  soulx config racing --cores 6 --ram 6144 --landscape\n"
      "  soulx config --gpu host-novulkan --json\n",
      k_config_opts, NOPTS(k_config_opts), CLI_INTERLEAVED, true },

    { "list", cmd_list, GROUP_INSTANCES, "list", "list your instances",
      "soulx list [--json]\n"
      "\n"
      "Lists your instances: id, name, state, Android version, CPUs, RAM, screen and adb serial.\n"
      "\n"
      "  --json   print JSON instead of a table\n",
      k_list_opts, NOPTS(k_list_opts), CLI_INTERLEAVED, false },

    { "status", cmd_status, GROUP_INSTANCES, "status [name]", "show the details of an instance",
      "soulx status [name] [--json]\n"
      "\n"
      "Shows one instance (default: $SOULX_INSTANCE, else \"default\"): state, Android version,\n"
      "hardware, screen, adb serial, engine pid, uptime, memory use and log file.\n"
      "\n"
      "  --json   print JSON instead of text\n",
      k_status_opts, NOPTS(k_status_opts), CLI_INTERLEAVED, true },

    { "stats", cmd_stats, GROUP_INSTANCES, "stats [name]", "CPU and memory use of running instances",
      "soulx stats [name] [--json] [--watch] [--interval MS]\n"
      "\n"
      "Measures the CPU and memory use of every running instance (or only of the named one) over\n"
      "one interval. CPU is in percent of one core (an instance using two full cores shows 200%);\n"
      "memory is the engine's footprint, the number Activity Monitor shows.\n"
      "\n"
      "  --json          print one JSON object per measurement (for launchers and scripts)\n"
      "  --watch         keep measuring until Ctrl+C\n"
      "  --interval MS   length of one measurement, 200-60000 ms (default 1000)\n"
      "\n"
      "Example: soulx stats --watch --json\n",
      k_stats_opts, NOPTS(k_stats_opts), CLI_INTERLEAVED, true },

    { "start", cmd_start, GROUP_INSTANCES, "start [name]", "start an instance (--headless: no window)",
      "soulx start [name] [--headless] [--cold] [--no-wait] [--timeout S]\n"
      "\n"
      "Starts an instance and waits until Android has booted. Every start is a fresh boot (about 15 s):\n"
      "SoulX does not use Quick Boot snapshots (restoring one made the engine leak memory).\n"
      "\n"
      "  --headless    no emulator window (for automation)\n"
      "  --cold        (kept for scripts; every start is a full boot)\n"
      "  --no-wait     return as soon as the engine is launched\n"
      "  --timeout S   stop waiting for the boot after S seconds (10-3600, default 300)\n",
      k_start_opts, NOPTS(k_start_opts), CLI_INTERLEAVED, true },

    { "stop", cmd_stop, GROUP_INSTANCES, "stop [name]", "shut an instance down",
      "soulx stop [name] [--force]\n"
      "\n"
      "Shuts an instance down (in about a second). Apps and data stay; the next start boots fresh.\n"
      "\n"
      "  --force   kill the engine immediately\n",
      k_stop_opts, NOPTS(k_stop_opts), CLI_INTERLEAVED, true },

    { "restart", cmd_restart, GROUP_INSTANCES, "restart [name]", "stop an instance and start it again",
      "soulx restart [name] [--headless] [--cold] [--no-wait] [--timeout S]\n"
      "\n"
      "Stops a running instance and starts it again, with the same options as 'soulx start'. An\n"
      "instance that runs headless stays headless.\n",
      k_start_opts, NOPTS(k_start_opts), CLI_INTERLEAVED, true },

    { "window", cmd_window, GROUP_INSTANCES, "window [name]", "open SoulX's window for a running instance",
      "soulx window [name]\n"
      "\n"
      "Opens SoulX's own window for a running instance: the Android screen, the toolbar (back, home,\n"
      "recents, rotate, record, run script, screenshot, game keymap) and the keymap editor. The engine\n"
      "always runs without a window of its own; this is the window you see. 'soulx start' and 'soulx play'\n"
      "open it automatically unless you pass --headless, so you rarely need this command.\n"
      "\n"
      "If a window for the instance is already open, it is brought to the front instead of opening a\n"
      "second one. The window closes (and the instance keeps running) with Option-Command-W; closing it\n"
      "normally (Command-W or Quit) stops the instance.\n",
      NO_OPTS, CLI_PASSTHROUGH, true },

    { "delete", cmd_delete, GROUP_INSTANCES, "delete <name>", "delete an instance and its data",
      "soulx delete <name> [--yes]\n"
      "\n"
      "Deletes a stopped instance and everything on it (installed apps, game data, snapshots).\n"
      "\n"
      "  -y, --yes   do not ask for confirmation\n",
      k_delete_opts, NOPTS(k_delete_opts), CLI_INTERLEAVED, true },

    { "install", cmd_install, GROUP_APPS, "install <file>...", "install .apk/.xapk/.apks/.apkm files",
      "soulx install [-i NAME] <file>... [--reinstall]\n"
      "\n"
      "Installs apps on a running instance: .apk files, .xapk files (including OBB data), .apks and\n"
      ".apkm bundles, or a folder of split APKs. Apps whose same version is already installed are\n"
      "skipped.\n"
      "\n"
      "  --reinstall   install even if the same version is already installed\n",
      k_install_opts, NOPTS(k_install_opts), CLI_INTERLEAVED, true },

    { "launch", cmd_launch, GROUP_APPS, "launch <package>", "start an installed app",
      "soulx launch [-i NAME] <package>\n"
      "\n"
      "Starts an installed app, e.g.: soulx launch com.example.game   (see 'soulx apps')\n",
      NO_OPTS, CLI_INTERLEAVED, true },

    { "apps", cmd_apps, GROUP_APPS, "apps", "list installed apps",
      "soulx apps [-i NAME] [--all]\n"
      "\n"
      "Lists the package names of the apps you installed.\n"
      "\n"
      "  --all   include the system apps\n",
      k_apps_opts, NOPTS(k_apps_opts), CLI_INTERLEAVED, true },

    { "stop-app", cmd_stop_app, GROUP_APPS, "stop-app <package>", "force-stop an app",
      "soulx stop-app [-i NAME] <package>\n"
      "\n"
      "Force-stops an app, e.g.: soulx stop-app com.example.game\n",
      NO_OPTS, CLI_INTERLEAVED, true },

    { "uninstall", cmd_uninstall, GROUP_APPS, "uninstall <package>", "remove an app and its data",
      "soulx uninstall [-i NAME] <package>\n"
      "\n"
      "Removes an app and its data from the instance.\n",
      NO_OPTS, CLI_INTERLEAVED, true },

    { "run", cmd_run, GROUP_AUTOMATION, "run [--repeat N|--loop] [--speed X] <script.lua> [args]",
      "run a Lua script that plays for you",
      "soulx run [-i NAME] [--repeat N | --loop] [--speed X] <script.lua> [args...]\n"
      "\n"
      "Runs a Lua 5.4 automation script on a running instance. Options go before the script name;\n"
      "words after it are passed to the script in the global table 'args'. Ctrl+C stops the script.\n"
      "\n"
      "  --repeat N   run the script N times (1-100000); default 1\n"
      "  --loop       run it over and over until you stop it (Ctrl+C)\n"
      "  --speed X    playback speed 0.1-10 (default 1): every sleep()/replay pause is divided by X,\n"
      "               so 2 is twice as fast and 0.5 is half speed\n"
      "\n"
      "In a script, speed([x]) reads or sets the speed and iteration() returns the current pass and\n"
      "the total (0 while looping). Script functions include tap, swipe, long_press, key, text, sleep,\n"
      "find, exists, wait, color, is_color, capture, screenshot, launch, stop_app, current_app, shell,\n"
      "console and log; see examples/scripts/ for complete scripts.\n",
      k_run_opts, NOPTS(k_run_opts), CLI_STOP_AT_POSITIONAL, true },

    { "record", cmd_record, GROUP_AUTOMATION, "record <out.lua>", "record your taps and swipes as a Lua script",
      "soulx record [-i NAME] <out.lua> [--force]\n"
      "\n"
      "Records what you do on the instance (taps, swipes, long presses, keys and the pauses between\n"
      "them) as a Lua script. Press Ctrl+C to stop recording, then replay it with: soulx run out.lua\n"
      "\n"
      "  --force   overwrite the file if it already exists\n",
      k_record_opts, NOPTS(k_record_opts), CLI_INTERLEAVED, true },

    { "tap", cmd_tap, GROUP_AUTOMATION, "tap X Y", "tap the screen",
      "soulx tap [-i NAME] X Y\n"
      "\n"
      "Taps the screen at X,Y in screen pixels (the same coordinates as in 'soulx screenshot').\n",
      NO_OPTS, CLI_INTERLEAVED, true },

    { "swipe", cmd_swipe, GROUP_AUTOMATION, "swipe X1 Y1 X2 Y2 [MS]", "swipe on the screen",
      "soulx swipe [-i NAME] X1 Y1 X2 Y2 [MS]\n"
      "\n"
      "Swipes from X1,Y1 to X2,Y2 in MS milliseconds (default 300).\n",
      NO_OPTS, CLI_INTERLEAVED, true },

    { "key", cmd_key, GROUP_AUTOMATION, "key NAME", "press a key (BACK, HOME, ENTER, ...)",
      "soulx key [-i NAME] KEY\n"
      "\n"
      "Presses a key. KEY is a name such as BACK, HOME, APP_SWITCH, MENU, ENTER, DEL, TAB, SPACE,\n"
      "ESCAPE, DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT, DPAD_CENTER, VOLUME_UP, VOLUME_DOWN, POWER,\n"
      "A-Z, an Android KEYCODE_* name, or a numeric Android key code (e.g. 4 = BACK). A bare number is\n"
      "a key code, not a digit key: use KEYCODE_0..KEYCODE_9 to type digits.\n",
      NO_OPTS, CLI_INTERLEAVED, true },

    { "text", cmd_text, GROUP_AUTOMATION, "text TEXT", "type text",
      "soulx text [-i NAME] TEXT...\n"
      "\n"
      "Types text into the focused input field; several words are joined with spaces.\n"
      "Example: soulx text \"hello world\"\n",
      NO_OPTS, CLI_PASSTHROUGH, true },

    { "volume", cmd_volume, GROUP_AUTOMATION, "volume [0-100|mute|unmute]", "show or set the sound volume",
      "soulx volume [-i NAME] [0-100|mute|unmute] [--json]\n"
      "\n"
      "Shows or sets the media volume of a running instance (the sound games play), right away and\n"
      "without a restart. mute silences it and unmute brings the previous level back. To turn the\n"
      "sound device off altogether (saves a little CPU), use: soulx config --no-audio\n"
      "\n"
      "  --json   print the result as JSON\n"
      "\n"
      "Examples: soulx volume 30   soulx volume -i farm2 mute\n",
      k_volume_opts, NOPTS(k_volume_opts), CLI_INTERLEAVED, true },

    { "screenshot", cmd_screenshot, GROUP_AUTOMATION, "screenshot [out.png]", "save a PNG screenshot",
      "soulx screenshot [-i NAME] [out.png]\n"
      "\n"
      "Saves a PNG screenshot and prints its path. Default file: ./soulx-<instance>-<date>-<time>.png\n",
      NO_OPTS, CLI_INTERLEAVED, true },

    { "screenrecord", cmd_screenrecord, GROUP_AUTOMATION, "screenrecord [out.webm]", "record the screen to a video",
      "soulx screenrecord [-i NAME] [out.webm]\n"
      "\n"
      "Records the screen to a WebM video. Press Ctrl+C to stop; the engine stops on its own after\n"
      "3 minutes (its built-in limit). Default file: ./soulx-<instance>-<date>-<time>.webm\n"
      "The path must end in .webm and contain no spaces.\n",
      NO_OPTS, CLI_INTERLEAVED, true },

    { "adb", cmd_adb, GROUP_TOOLS, "adb [args...]", "run SoulX's adb against the instance",
      "soulx adb [-i NAME] [args...]\n"
      "\n"
      "Runs SoulX's own adb (a private adb server on port 5137, separate from Android Studio's) against\n"
      "the instance. Only -i NAME selects the instance; every other word (including -v and -h) goes to\n"
      "adb. Put '--' first to pass a leading option through verbatim. Examples:\n"
      "  soulx adb shell getprop ro.build.version.release\n"
      "  soulx adb push save.dat /sdcard/Download/\n",
      NO_OPTS, CLI_PASSTHROUGH, true },

    { "shell", cmd_shell, GROUP_TOOLS, "shell [command...]", "open an Android shell or run one command",
      "soulx shell [-i NAME] [command...]\n"
      "\n"
      "Opens an interactive Android shell, or runs one command, e.g.: soulx shell pm list packages\n",
      NO_OPTS, CLI_PASSTHROUGH, true },

    { "logcat", cmd_logcat, GROUP_TOOLS, "logcat [args...]", "show the Android log",
      "soulx logcat [-i NAME] [args...]\n"
      "\n"
      "Streams the Android log (adb logcat); Ctrl+C stops it. Options go to adb, so 'soulx logcat -v\n"
      "threadtime' and 'soulx logcat -s Unity' work (only -i NAME selects the instance).\n",
      NO_OPTS, CLI_PASSTHROUGH, true },

    { "console", cmd_console, GROUP_TOOLS, "console <command...>", "send an emulator console command",
      "soulx console [-i NAME] <command...>\n"
      "\n"
      "Sends one command to the emulator console and prints its reply; exits with 1 when the console\n"
      "answers KO. Examples:\n"
      "  soulx console geo fix -122.084 37.422     (longitude, then latitude)\n"
      "  soulx console power capacity 15\n"
      "  soulx console help\n",
      NO_OPTS, CLI_STOP_AT_POSITIONAL, true },

    { "doctor", cmd_doctor, GROUP_TOOLS, "doctor", "check this Mac, the runtime and your instances",
      "soulx doctor\n"
      "\n"
      "Checks this Mac (Apple Silicon, Hypervisor, memory, free disk), SoulX's runtime, the adb server\n"
      "and your instances. Exits with 1 when something keeps SoulX from running.\n",
      NO_OPTS, CLI_INTERLEAVED, false },

    { "scripts", cmd_scripts, GROUP_AUTOMATION, "scripts [action]", "list or manage an instance's scripts",
      "soulx scripts [-i NAME] [list|folder|import PATH...|remove ENTRY|rename FILE NEW NAME] [--json]\n"
      "\n"
      "Each instance has its own scripts, in ~/Documents/SoulX Scripts/<instance>/: .lua files, or folders\n"
      "that hold a script with the files it needs (lib/ ...). SoulX's window lists and runs only that\n"
      "instance's scripts, and 'soulx run NAME' finds them by name.\n"
      "\n"
      "  list               the instance's scripts with their names (the default)\n"
      "  folder             print the folder (created if missing)\n"
      "  import PATH...     copy .lua files or script folders in (an entry with the same name is\n"
      "                     replaced; the old one goes to the Trash)\n"
      "  remove ENTRY       move a script or script folder to the Trash\n"
      "  rename FILE NAME   set the name a script shows (its \"-- name:\" line)\n"
      "  --json             print the instance's scripts as JSON (after any change)\n"
      "\n"
      "Examples: soulx scripts -i farm import ~/Downloads/daily.lua   soulx scripts rename daily.lua Daily run\n",
      k_scripts_opts, NOPTS(k_scripts_opts), CLI_INTERLEAVED, true },

    { "mcp", cmd_mcp, GROUP_TOOLS, "mcp [action]", "AI control: the MCP server and its settings",
      "soulx mcp [serve|config|log|setup] [options]\n"
      "\n"
      "Lets an AI app (any MCP client) see and operate your instances through the\n"
      "Model Context Protocol. Off by default; the launcher (AI control) or 'mcp config' turns it on.\n"
      "The server talks only over stdin/stdout of the process the client starts: no network port.\n"
      "\n"
      "  serve (default)        run the server (the AI client starts this)\n"
      "  config                 show or change the settings:\n"
      "    --enable | --disable   turn AI control on or off (takes effect at once)\n"
      "    --instances LIST       the instances it may use: \"*\" or names separated by commas\n"
      "    --allow P,... --deny P,...   permissions: start-stop, install, scripts (your scripts),\n"
      "                           lua (code the AI writes; always sandboxed, no access to your files)\n"
      "    --json                 print the settings as JSON\n"
      "  log [-n N] [--json]    the last N tool calls (all of them are logged)\n"
      "  setup                  how to connect an AI app\n"
      "\n"
      "Never available to the AI: creating, deleting or changing instances, and setup.\n",
      k_mcp_opts, NOPTS(k_mcp_opts), CLI_INTERLEAVED, false },

    { "language", cmd_language, GROUP_TOOLS, "language [lang]", "show or set the language of the window and launcher",
      "soulx language [system|en|zh-hant|zh-hans] [--json]\n"
      "\n"
      "Shows or sets the language of SoulX's window and of the launcher: English, Traditional Chinese\n"
      "or Simplified Chinese. \"system\" (the default) follows the Mac's language. The environment\n"
      "variable SOULX_LANG overrides the saved choice. A window that is already open keeps its\n"
      "language until it is opened again. (The soulx command itself always prints English.)\n"
      "\n"
      "  --json   print the result as JSON\n",
      k_language_opts, NOPTS(k_language_opts), CLI_INTERLEAVED, false },

    { "runtime", cmd_runtime, GROUP_TOOLS, "runtime", "show the installed runtime packages",
      "soulx runtime [--json]\n"
      "\n"
      "Shows which runtime packages (engine, adb, system images) are installed, their versions and\n"
      "disk usage.\n"
      "\n"
      "  --json   print JSON instead of text (packages, installed, and whether the default image is ready)\n",
      k_runtime_opts, NOPTS(k_runtime_opts), CLI_INTERLEAVED, false },

    { "version", cmd_version, GROUP_TOOLS, "version", "show the version",
      "soulx version\n"
      "\n"
      "Prints the SoulX version and the Android engine it runs.\n",
      NO_OPTS, CLI_INTERLEAVED, false },

    { "help", cmd_help, GROUP_TOOLS, "help [command]", "show this overview or the details of a command",
      "soulx help [command]\n"
      "\n"
      "Shows the overview of all commands, or the details of one command.\n",
      NO_OPTS, CLI_INTERLEAVED, false },
};

static const CliCommand* command_table(size_t* count) {
    *count = sizeof k_commands / sizeof k_commands[0];
    return k_commands;
}

static const CliCommand* find_command(const char* name) {
    for (size_t i = 0; i < sizeof k_commands / sizeof k_commands[0]; i++) {
        if (strcmp(k_commands[i].name, name) == 0) {
            return &k_commands[i];
        }
    }
    return NULL;
}

// ctx: the command whose usage to show with the error (NULL = general hint)
static int unknown_command(const CliCommand* ctx, const char* word) {
    const char* best = NULL;
    size_t best_d = 3;
    for (size_t i = 0; i < sizeof k_commands / sizeof k_commands[0]; i++) {
        size_t d = edit_distance(word, k_commands[i].name);
        if (d < best_d) {
            best_d = d;
            best = k_commands[i].name;
        }
    }
    if (best) {
        return usage_error(ctx, "unknown command '%s' (did you mean '%s'?)", word, best);
    }
    return usage_error(ctx, "unknown command '%s'", word);
}

// ---------------------------------------------------------------------------
// Dispatch and main
// ---------------------------------------------------------------------------

static int check_environment(void) {
    const char* home = getenv("SOULX_HOME");
    if (home && *home && home[0] != '/') {
        soulx_log(SOULX_LOG_ERROR, "SOULX_HOME must be an absolute path (it is '%s')", home);
        return CLI_EXIT_USAGE;
    }
    if (!soulx_home()) {
        soulx_log(SOULX_LOG_ERROR, "cannot determine SoulX's home folder (set SOULX_HOME)");
        return CLI_EXIT_FAIL;
    }
    return 0;
}

static int dispatch(const CliCommand* cmd, int argc, char** argv, const CliGlobal* g) {
    CliArgs a;
    int rc = cli_parse(cmd, argc, argv, &a);
    if (rc == 0) {
        if (a.help) {
            print_command_help(stdout, cmd);
        } else if (a.instance && !cmd->uses_instance) {
            rc = usage_error(cmd, "-i/--instance does not apply to 'soulx %s'", cmd->name);
        } else {
            bool needs_home = cmd->handler != cmd_help && cmd->handler != cmd_version;
            rc = needs_home ? check_environment() : 0;
            if (rc == 0) {
                rc = cmd->handler(cmd, &a, g);
            }
        }
    }
    free(a.pos);
    return rc;
}

int main(int argc, char** argv) {
    install_signal_handlers();
    const char* env_verbose = getenv("SOULX_VERBOSE");
    if (env_verbose && strcmp(env_verbose, "1") == 0) {
        soulx_set_verbose(true);
    }
    if (soulx_migrate_legacy_home() != 0) return 1;    // Soul's data, once, before anything reads the home
    (void)soulx_scripts_migrate();                     // once: scripts move into per-instance folders

    // global options before the command: -v, -i NAME
    CliGlobal g = { NULL };
    int i = 1;
    int rc = -1;
    while (i < argc) {
        const char* arg = argv[i];
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            soulx_set_verbose(true);
            i++;
        } else if (strcmp(arg, "-i") == 0 || strcmp(arg, "--instance") == 0) {
            if (i + 1 >= argc || is_option_like(argv[i + 1])) {
                rc = usage_error(NULL, "option '%s' needs an instance name", arg);
                break;
            }
            g.instance = argv[i + 1];
            i += 2;
        } else if (strncmp(arg, "--instance=", 11) == 0) {
            g.instance = arg + 11;
            i++;
        } else if (strcmp(arg, "-instance") == 0) {
            rc = usage_error(NULL, "unknown option '-instance' (did you mean '--instance'?)");
            break;
        } else if (strncmp(arg, "-i", 2) == 0 && arg[2] != '\0') {
            g.instance = arg + 2;       // -iNAME
            i++;
        } else if (strcmp(arg, "--") == 0) {
            i++;                        // end of global options: the command follows
            break;
        } else {
            break;
        }
    }

    if (rc < 0) {
        if (i >= argc) {
            print_overview(stdout);
            rc = CLI_EXIT_OK;
        } else {
            const char* word = argv[i];
            const CliLegacy* legacy = find_legacy(word);
            const CliCommand* cmd = NULL;
            if (legacy) {
                rc = run_legacy(legacy, argc - i - 1, argv + i + 1, &g);
            } else if (strcmp(word, "-h") == 0) {
                rc = dispatch(find_command("help"), argc - i - 1, argv + i + 1, &g);
            } else if (strcmp(word, "-V") == 0) {
                rc = dispatch(find_command("version"), argc - i - 1, argv + i + 1, &g);
            } else if (word[0] == '-') {
                rc = usage_error(NULL, "unknown option '%s'", word);
            } else if ((cmd = find_command(word)) != NULL) {
                rc = dispatch(cmd, argc - i - 1, argv + i + 1, &g);
            } else {
                rc = unknown_command(NULL, word);
            }
        }
    }

    if (g_soulx_interrupted && rc != CLI_EXIT_OK) {
        rc = CLI_EXIT_INTR;
    }
    if (fflush(stdout) != 0 && rc == CLI_EXIT_OK) {
        soulx_log(SOULX_LOG_ERROR, "cannot write the output: %s", strerror(errno));
        rc = CLI_EXIT_FAIL;
    }
    return rc;
}
