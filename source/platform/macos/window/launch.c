/*
 * SoulX Android Emulator - starting and finding SoulX's window process (macOS)
 *
 * soulx_window_spawn() starts '<realpath of this executable> window <name>' in a new session (stdin /dev/null,
 * stdout + stderr appended to logs/<name>.window.log, cwd "/") with the caller's environment: the window is a
 * normal app, only its adb / engine children get the SoulX isolation environment. It does not check for a running
 * window first: a second 'soulx window <name>' activates the existing one and exits (window/app.m).
 *
 * soulx_window_running() reads the pid file run/<name>.window, which the window process writes itself (decimal
 * pid, "pid=<n>" also accepted). The pid counts only while it is alive, runs a soulx executable (this one, or
 * another copy such as SoulX.app's helper) and started before the pid file was written: a recycled pid that now
 * belongs to another soulx process (soulx run, soulx record...) fails the last check. The function never
 * modifies the pid file (the window process owns it), so it cannot race with a window that is starting.
 */

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "soulx_mac.h"

#define WINDOW_LOG_ROTATE_BYTES (4LL * 1024 * 1024)    // logs/<name>.window.log -> .window.log.1 above this
#define WINDOW_PID_FILE_MAX     256
#define WINDOW_PID_SLACK_S      2                       // pid file mtime vs process start (clock granularity)

// Canonical path of the running executable. 0 ok.
static int self_exe(char* out, size_t cap) {
    char raw[PATH_MAX];
    uint32_t size = sizeof(raw);
    char* dyn = NULL;
    const char* path = raw;
    if (_NSGetExecutablePath(raw, &size) != 0) {        // size now holds the length needed
        dyn = malloc(size);
        if (!dyn || _NSGetExecutablePath(dyn, &size) != 0) {
            free(dyn);
            return -1;
        }
        path = dyn;
    }
    char real[PATH_MAX];
    bool ok = realpath(path, real) != NULL;
    free(dyn);
    if (!ok) return -1;
    size_t len = strlen(real);
    if (len >= cap) return -1;
    memcpy(out, real, len + 1);
    return 0;
}

static void rotate_log(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < WINDOW_LOG_ROTATE_BYTES) return;
    char old[PATH_MAX];
    int n = snprintf(old, sizeof(old), "%s.1", path);
    if (n < 0 || (size_t)n >= sizeof(old)) return;
    if (rename(path, old) != 0) soulx_log(SOULX_LOG_DEBUG, "cannot rotate %s: %s", path, strerror(errno));
}

int soulx_window_spawn(const char* instance) {
    if (!soulx_name_valid(instance)) {
        soulx_log(SOULX_LOG_ERROR, "invalid instance name '%.64s'", instance ? instance : "");
        return -1;
    }
    char exe[PATH_MAX];
    if (self_exe(exe, sizeof(exe)) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot open SoulX's window: the path of this program is unknown");
        return -1;
    }
    char logs[PATH_MAX];
    char log_path[PATH_MAX];
    if (soulx_path(logs, sizeof(logs), "logs") != 0 ||
        soulx_path(log_path, sizeof(log_path), "logs/%s.window.log", instance) != 0) {
        soulx_log(SOULX_LOG_ERROR, "SoulX home path too long: %s", soulx_home());
        return -1;
    }
    if (soulx_mkdirs(logs) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", logs, strerror(errno));
        return -1;
    }
    rotate_log(log_path);

    const char* argv[] = { exe, "window", instance, NULL };
    SoulXCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.argv = argv;
    cmd.cwd = "/";                  // never keep the caller's directory (a removable volume) busy
    cmd.soulx_env = false;           // inherited environment
    // New session, stdin /dev/null, output appended to the log; the window's main thread renders at
    // user-interactive QoS.
    pid_t pid = soulx_spawn_detached(&cmd, log_path, true);
    if (pid <= 0) return -1;
    soulx_log(SOULX_LOG_DEBUG, "window for '%s' started (pid %d, log %s)", instance, (int)pid, log_path);
    return 0;
}

// Pid from the pid file and the file's modification time; 0 when missing or malformed.
static pid_t read_pid_file(const char* path, struct timespec* mtime) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > WINDOW_PID_FILE_MAX) {
        close(fd);
        return 0;
    }
    char buf[WINDOW_PID_FILE_MAX + 1];
    ssize_t n;
    do {
        n = read(fd, buf, sizeof(buf) - 1);
    } while (n < 0 && errno == EINTR);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    const char* p = buf;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (strncmp(p, "pid=", 4) == 0) p += 4;
    long v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        if (++digits > 9) return 0;
        v = v * 10 + (*p - '0');
        p++;
    }
    if (digits == 0 || v <= 1 || (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')) return 0;
    *mtime = st.st_mtimespec;
    return (pid_t)v;
}

// proc_pidpath(pid) names the same file as |self| (string compare, then realpath, then device + inode), or another
// copy of soulx (same file name): a window opened by SoulX.app's helper still belongs to the instance when a
// development build or a second install asks. started_before() keeps a recycled pid out either way.
static bool runs_executable(pid_t pid, const char* self) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) return false;
    if (strcmp(path, self) == 0) return true;
    const char* pn = strrchr(path, '/');
    const char* sn = strrchr(self, '/');
    if (pn && sn && strcmp(pn, sn) == 0) return true;
    char real[PATH_MAX];
    if (realpath(path, real) && strcmp(real, self) == 0) return true;
    struct stat a, b;
    return stat(path, &a) == 0 && stat(self, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

// The process existed when the pid file was written (a pid recycled later started after it).
static bool started_before(pid_t pid, const struct timespec* written) {
    struct proc_bsdinfo bi;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, (int)sizeof(bi)) != (int)sizeof(bi)) return true;  // unknown
    return (long long)bi.pbi_start_tvsec <= (long long)written->tv_sec + WINDOW_PID_SLACK_S;
}

bool soulx_window_running(const char* instance, pid_t* pid_out) {
    if (pid_out) *pid_out = 0;
    if (!soulx_name_valid(instance)) return false;
    char path[PATH_MAX];
    if (soulx_path(path, sizeof(path), "run/%s.window", instance) != 0) return false;
    struct timespec written;
    pid_t pid = read_pid_file(path, &written);
    if (pid <= 0 || !soulx_pid_alive(pid)) return false;
    char self[PATH_MAX];
    if (self_exe(self, sizeof(self)) != 0 || !runs_executable(pid, self) || !started_before(pid, &written)) return false;
    if (pid_out) *pid_out = pid;
    return true;
}

// run/<name>.script ("pid=<n>\npath=<...>\n"), written by the window while it runs a script job
// (window_controller.m startJob:/jobExited:). Informational only: no identity cross-check like
// soulx_window_running above, since the launcher only uses this to show which script is running and offer to
// stop it, never to decide who owns anything. A stale file (the window crashed without cleaning up) is caught
// by the pid liveness check.
bool soulx_running_script(const char* instance, char* path_out, size_t cap) {
    if (path_out && cap) path_out[0] = '\0';
    if (!soulx_name_valid(instance)) return false;
    char path[PATH_MAX];
    if (soulx_path(path, sizeof(path), "run/%s.script", instance) != 0) return false;
    char* data = NULL;
    size_t len = 0;
    if (soulx_read_file(path, &data, &len) != 0 || !data) return false;

    pid_t pid = 0;
    char script[PATH_MAX] = "";
    char* save = NULL;
    for (char* line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "pid=", 4) == 0) {
            char* end = NULL;
            long v = strtol(line + 4, &end, 10);
            if (end != line + 4 && v > 1 && v < INT32_MAX) pid = (pid_t)v;
        } else if (strncmp(line, "path=", 5) == 0 && strlen(line + 5) < sizeof(script)) {
            snprintf(script, sizeof(script), "%s", line + 5);
        }
    }
    free(data);

    if (pid <= 0 || script[0] == '\0' || !soulx_pid_alive(pid)) return false;
    if (path_out) snprintf(path_out, cap, "%s", script);
    return true;
}
