/*
 * SoulX Android Emulator - macOS $SOULX_HOME layout and file helpers
 *
 * soulx_home() is $SOULX_HOME (made absolute) or <passwd home>/.soulx-android. $HOME is deliberately
 * not used: SoulX runs its children with HOME=$SOULX_HOME/home and the CLI may be run that way too.
 * Without a passwd entry an absolute $HOME is used, then the per-user private directory
 * (confstr _CS_DARWIN_USER_DIR); never a shared, world-writable place such as /tmp.
 * soulx_rmtree() only deletes paths strictly inside soulx_home() and never follows symbolic links.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "soulx_mac.h"

#define SOULX_HOME_DIRNAME       ".soulx-android"
#define SOULX_HOME_UNUSABLE      "/var/empty/.soulx-android"  // no home at all: every write fails cleanly
#define SOULX_READ_MAX           (512u * 1024u * 1024u)  // soulx_read_file refuses larger files
#define SOULX_TREE_MAX_DEPTH     128                     // rmtree / dir_size recursion limit

// Every directory of the layout documented in soulx_mac.h (relative to soulx_home()).
static const char* const k_layout[] = {
    "sdk",
    "sdk/.soulx",
    "sdk/platforms",        // empty; keeps the engine's SDK-root sanity checks happy
    "downloads",
    "licenses",
    "avd",
    "home",
    "home/.android",
    "instances",
    "run",
    "logs",
    "tmp",
};

// ---------------------------------------------------------------------------
// soulx_home
// ---------------------------------------------------------------------------

static char g_home[PATH_MAX];
static char g_home_src[PATH_MAX];       // the $SOULX_HOME value g_home was computed from ("" = default)
static bool g_home_valid = false;
static bool g_home_warned = false;

// Collapses "//", drops "/./" components and trailing slashes. Input must be absolute.
static void normalize_abs_path(char* p) {
    char* w = p;
    const char* r = p;
    while (*r) {
        if (*r == '/') {
            while (r[1] == '/') r++;
            if (r[1] == '.' && (r[2] == '/' || r[2] == '\0')) {
                r += 2;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
    size_t len = strlen(p);
    while (len > 1 && p[len - 1] == '/') p[--len] = '\0';
    if (len == 0) {
        p[0] = '/';
        p[1] = '\0';
    }
}

static int default_home(char* out, size_t cap) {
    const char* dir = NULL;
    struct passwd pw;
    struct passwd* res = NULL;
    char pwbuf[4096];
    char user_dir[PATH_MAX];
    if (getpwuid_r(getuid(), &pw, pwbuf, sizeof(pwbuf), &res) == 0 && res && res->pw_dir && res->pw_dir[0] == '/') {
        dir = res->pw_dir;
    } else {
        dir = getenv("HOME");   // no passwd entry: an absolute $HOME
        if (!dir || dir[0] != '/') {
            // else the per-user private directory (/var/folders/../0/, mode 0700, owned by the user)
            size_t n = confstr(_CS_DARWIN_USER_DIR, user_dir, sizeof(user_dir));
            if (n == 0 || n > sizeof(user_dir) || user_dir[0] != '/') return -1;
            dir = user_dir;
        }
    }
    int n = snprintf(out, cap, "%s/%s", dir, SOULX_HOME_DIRNAME);
    if (n < 0 || (size_t)n >= cap) return -1;
    normalize_abs_path(out);
    return 0;
}

// No usable home at all: a path under the root-owned, read-only /var/empty, so every attempt to
// create SoulX's files fails with an error instead of landing in a shared directory.
static void unusable_home(char* out, size_t cap, const char* env) {
    snprintf(out, cap, "%s", SOULX_HOME_UNUSABLE);
    if (!g_home_warned) {
        g_home_warned = true;
        if (env) soulx_log(SOULX_LOG_ERROR, "invalid SOULX_HOME '%s' and no home directory to fall back to; set "
                          "SOULX_HOME to an absolute path", env);
        else soulx_log(SOULX_LOG_ERROR, "cannot determine a home directory for SoulX; set SOULX_HOME to an absolute path");
    }
}

static int home_from_env(const char* env, char* out, size_t cap) {
    int n;
    if (env[0] == '/') {
        n = snprintf(out, cap, "%s", env);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return -1;
        n = snprintf(out, cap, "%s/%s", cwd, env);
    }
    if (n < 0 || (size_t)n >= cap) return -1;
    normalize_abs_path(out);
    if (strcmp(out, "/") == 0) return -1;     // never let "/" be the SoulX home
    return 0;
}

const char* soulx_home(void) {
    const char* env = getenv("SOULX_HOME");
    if (env && !env[0]) env = NULL;
    const char* src = env ? env : "";

    if (g_home_valid && strcmp(g_home_src, src) == 0) return g_home;

    char computed[PATH_MAX];
    if (env) {
        if (home_from_env(env, computed, sizeof(computed)) != 0) {
            if (default_home(computed, sizeof(computed)) != 0) {
                unusable_home(computed, sizeof(computed), env);
            } else if (!g_home_warned) {
                g_home_warned = true;
                soulx_log(SOULX_LOG_ERROR, "ignoring invalid SOULX_HOME '%s'; using %s", env, computed);
            }
        } else if (env[0] != '/' && !g_home_warned) {
            g_home_warned = true;
            soulx_log(SOULX_LOG_WARN, "SOULX_HOME '%s' is not absolute; using %s", env, computed);
        }
    } else if (default_home(computed, sizeof(computed)) != 0) {
        unusable_home(computed, sizeof(computed), NULL);
    }

    memcpy(g_home, computed, sizeof(g_home));
    if (strlen(src) < sizeof(g_home_src)) {
        memcpy(g_home_src, src, strlen(src) + 1);
        g_home_valid = true;
    } else {
        g_home_valid = false;           // absurdly long SOULX_HOME: recompute (it was rejected anyway)
    }
    return g_home;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// True when any '/'-separated component of rel is "..".
static bool has_dotdot(const char* rel) {
    const char* p = rel;
    while (*p) {
        while (*p == '/') p++;
        const char* start = p;
        while (*p && *p != '/') p++;
        if (p - start == 2 && start[0] == '.' && start[1] == '.') return true;
    }
    return false;
}

int soulx_path(char* buf, size_t cap, const char* rel_fmt, ...) {
    if (!buf || cap == 0) return -1;
    buf[0] = '\0';
    if (!rel_fmt) return -1;

    char rel[PATH_MAX];
    va_list ap;
    va_start(ap, rel_fmt);
    int n = vsnprintf(rel, sizeof(rel), rel_fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(rel)) return -1;
    if (has_dotdot(rel)) return -1;     // a SoulX path never escapes soulx_home()

    const char* r = rel;
    while (*r == '/') r++;
    const char* home = soulx_home();
    int m = r[0] ? snprintf(buf, cap, "%s/%s", home, r) : snprintf(buf, cap, "%s", home);
    if (m < 0 || (size_t)m >= cap) {
        buf[0] = '\0';                  // never hand out a truncated path
        return -1;
    }
    return 0;
}

bool soulx_path_exists(const char* path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0;
}

bool soulx_is_dir(const char* path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int soulx_mkdirs(const char* path) {
    if (!path || !path[0]) {
        errno = EINVAL;
        return -1;
    }
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(tmp, path, len + 1);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (char* p = tmp + 1; ; p++) {
        bool last = (*p == '\0');
        if (*p != '/' && !last) continue;
        if (!last && p[-1] == '/') continue;    // "//"
        char saved = *p;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0) {
            int err = errno;
            if (!soulx_is_dir(tmp)) {            // EEXIST on a file, EACCES, EROFS, ...
                errno = (err == EEXIST) ? ENOTDIR : err;
                return -1;
            }
        }
        *p = saved;
        if (last) break;
    }
    return 0;
}

int soulx_ensure_layout(void) {
    const char* home = soulx_home();
    if (soulx_mkdirs(home) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", home, strerror(errno));
        return -1;
    }
    // The home holds adb keys and console tokens, and SoulX deletes files inside it: it must belong to
    // this user (a directory someone else created, or a link to one, is refused).
    struct stat hst;
    if (stat(home, &hst) != 0 || !S_ISDIR(hst.st_mode)) {
        soulx_log(SOULX_LOG_ERROR, "%s is not a directory", home);
        return -1;
    }
    if (hst.st_uid != geteuid()) {
        soulx_log(SOULX_LOG_ERROR, "%s belongs to another user (uid %d); set SOULX_HOME to a directory of your own",
                 home, (int)hst.st_uid);
        return -1;
    }
    static bool warned_writable = false;
    if ((hst.st_mode & S_IWOTH) && !(hst.st_mode & S_ISVTX) && !warned_writable) {
        warned_writable = true;
        soulx_log(SOULX_LOG_WARN, "%s is writable by every user of this Mac; restrict it with: chmod o-w '%s'", home, home);
    }
    for (size_t i = 0; i < sizeof(k_layout) / sizeof(k_layout[0]); i++) {
        char dir[PATH_MAX];
        if (soulx_path(dir, sizeof(dir), "%s", k_layout[i]) != 0) {
            soulx_log(SOULX_LOG_ERROR, "SoulX home path too long: %s", home);
            return -1;
        }
        if (soulx_mkdirs(dir) != 0) {
            soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", dir, strerror(errno));
            return -1;
        }
    }
    // home/ holds the adb private key and the emulator console token: keep it private.
    char priv[PATH_MAX];
    if (soulx_path(priv, sizeof(priv), "home") == 0) (void)chmod(priv, 0700);
    if (soulx_path(priv, sizeof(priv), "home/.android") == 0) (void)chmod(priv, 0700);
    return 0;
}

// ---------------------------------------------------------------------------
// soulx_rmtree - fd-relative walk, never follows symlinks
// ---------------------------------------------------------------------------

static int rmtree_at(int parent_fd, const char* name, int depth);

static int remove_dir_contents(int dfd, int depth) {
    int fd2 = dup(dfd);
    if (fd2 < 0) return -1;
    DIR* d = fdopendir(fd2);
    if (!d) {
        close(fd2);
        return -1;
    }

    // Collect the names first: deleting while iterating may skip entries on some filesystems.
    char** names = NULL;
    size_t count = 0;
    size_t cap = 0;
    int rc = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            char** grown = realloc(names, ncap * sizeof(char*));
            if (!grown) {
                rc = -1;
                break;
            }
            names = grown;
            cap = ncap;
        }
        names[count] = strdup(e->d_name);
        if (!names[count]) {
            rc = -1;
            break;
        }
        count++;
    }
    closedir(d);

    for (size_t i = 0; i < count; i++) {
        if (rmtree_at(dfd, names[i], depth + 1) != 0) rc = -1;
        free(names[i]);
    }
    free(names);
    return rc;
}

static int rmtree_at(int parent_fd, const char* name, int depth) {
    struct stat st;
    if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT ? 0 : -1;

    if (!S_ISDIR(st.st_mode)) {         // files, symlinks (the link itself), fifos, ...
        if (unlinkat(parent_fd, name, 0) != 0 && errno != ENOENT) return -1;
        return 0;
    }
    if (depth > SOULX_TREE_MAX_DEPTH) {
        errno = ELOOP;
        return -1;
    }
    if ((st.st_mode & S_IRWXU) != S_IRWXU) {   // read-only directories cannot be emptied
        (void)fchmodat(parent_fd, name, (st.st_mode & 07777) | S_IRWXU, AT_SYMLINK_NOFOLLOW);
    }
    int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = remove_dir_contents(fd, depth);
    close(fd);
    if (unlinkat(parent_fd, name, AT_REMOVEDIR) != 0 && errno != ENOENT) rc = -1;
    return rc;
}

// Checks that path is absolute, strictly inside soulx_home(), has no "." / ".." components and
// that no directory between soulx_home() and the final component is a symbolic link.
static bool rmtree_path_allowed(const char* path, const char* home) {
    if (!path || path[0] != '/') return false;
    size_t hl = strlen(home);
    if (strncmp(path, home, hl) != 0 || path[hl] != '/') return false;

    const char* rel = path + hl;
    while (*rel == '/') rel++;
    if (!*rel) return false;            // soulx_home() itself

    char walk[PATH_MAX];
    if (strlen(path) >= sizeof(walk)) return false;
    memcpy(walk, home, hl + 1);
    size_t wl = hl;

    const char* p = rel;
    while (*p) {
        const char* start = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - start);
        while (*p == '/') p++;
        if ((clen == 1 && start[0] == '.') || (clen == 2 && start[0] == '.' && start[1] == '.')) return false;
        if (!*p) break;                 // final component: may be anything (it is not followed)
        if (wl + 1 + clen >= sizeof(walk)) return false;
        walk[wl++] = '/';
        memcpy(walk + wl, start, clen);
        wl += clen;
        walk[wl] = '\0';
        struct stat st;
        if (lstat(walk, &st) != 0) {
            if (errno == ENOENT) return true;   // nothing below can exist either
            return false;
        }
        if (!S_ISDIR(st.st_mode)) return false; // symlink (or file) in the middle of the path
    }
    return true;
}

int soulx_rmtree(const char* path) {
    const char* home = soulx_home();
    if (!rmtree_path_allowed(path, home)) {
        soulx_log(SOULX_LOG_ERROR, "refusing to delete '%s': not strictly inside %s (or reached through a symbolic link)",
                 path ? path : "(null)", home);
        errno = EPERM;
        return -1;
    }

    char parent[PATH_MAX];
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    if (len >= sizeof(parent)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(parent, path, len);
    parent[len] = '\0';
    char* slash = strrchr(parent, '/');
    if (!slash || slash == parent) {
        errno = EPERM;
        return -1;
    }
    *slash = '\0';
    const char* name = slash + 1;

    int pfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (pfd < 0) return errno == ENOENT ? 0 : -1;
    int rc = rmtree_at(pfd, name, 0);
    int saved = errno;
    close(pfd);
    errno = saved;
    return rc;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

int soulx_read_file(const char* path, char** data, size_t* len) {
    if (data) *data = NULL;
    if (len) *len = 0;
    if (!path || !data) {
        errno = EINVAL;
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        errno = EISDIR;
        return -1;
    }
    if (S_ISREG(st.st_mode) && (uint64_t)st.st_size > SOULX_READ_MAX) {
        close(fd);
        errno = EFBIG;
        return -1;
    }

    size_t cap = (S_ISREG(st.st_mode) && st.st_size > 0) ? (size_t)st.st_size + 1 : 4096;
    char* buf = malloc(cap);
    if (!buf) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    size_t n = 0;
    for (;;) {
        if (n + 1 >= cap) {
            if (cap >= SOULX_READ_MAX) {
                free(buf);
                close(fd);
                errno = EFBIG;
                return -1;
            }
            size_t ncap = cap * 2;
            if (ncap > SOULX_READ_MAX) ncap = SOULX_READ_MAX;
            char* grown = realloc(buf, ncap);
            if (!grown) {
                free(buf);
                close(fd);
                errno = ENOMEM;
                return -1;
            }
            buf = grown;
            cap = ncap;
        }
        ssize_t r = read(fd, buf + n, cap - n - 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            int saved = errno;
            free(buf);
            close(fd);
            errno = saved;
            return -1;
        }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = '\0';
    *data = buf;
    if (len) *len = n;
    return 0;
}

int soulx_write_file_atomic(const char* path, const char* data, size_t len) {
    if (!path || !path[0] || (!data && len > 0)) {
        errno = EINVAL;
        return -1;
    }
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = mkostemp(tmp, O_CLOEXEC);
    if (fd < 0) return -1;

    mode_t mode = 0644;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) mode = st.st_mode & 07777;   // keep existing permissions

    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            goto fail;
        }
        off += (size_t)w;
    }
    if (fchmod(fd, mode) != 0) goto fail;
    if (fsync(fd) != 0) goto fail;
    if (close(fd) != 0) {
        fd = -1;
        goto fail;
    }
    fd = -1;
    if (rename(tmp, path) != 0) goto fail;
    return 0;

fail:
    {
        int saved = errno;
        if (fd >= 0) close(fd);
        unlink(tmp);
        errno = saved;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Disk usage
// ---------------------------------------------------------------------------

static uint64_t dir_size_fd(int dfd, int depth) {
    if (depth > SOULX_TREE_MAX_DEPTH) return 0;
    int fd2 = dup(dfd);
    if (fd2 < 0) return 0;
    DIR* d = fdopendir(fd2);
    if (!d) {
        close(fd2);
        return 0;
    }
    uint64_t total = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        struct stat st;
        if (fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        total += (uint64_t)st.st_blocks * 512u;    // allocated bytes, like du (sparse disk images)
        if (S_ISDIR(st.st_mode)) {
            int cfd = openat(dfd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cfd >= 0) {
                total += dir_size_fd(cfd, depth + 1);
                close(cfd);
            }
        }
    }
    closedir(d);
    return total;
}

uint64_t soulx_dir_size(const char* path) {
    if (!path || !path[0]) return 0;
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    uint64_t total = (uint64_t)st.st_blocks * 512u;
    if (!S_ISDIR(st.st_mode)) return total;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return total;
    total += dir_size_fd(fd, 0);
    close(fd);
    return total;
}

uint64_t soulx_free_disk_bytes(const char* path) {
    if (!path || !path[0]) return 0;
    char probe[PATH_MAX];
    int n = snprintf(probe, sizeof(probe), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(probe)) return 0;

    // The directory may not exist yet (first run): measure its closest existing ancestor.
    for (;;) {
        struct statfs sf;
        if (statfs(probe, &sf) == 0) return (uint64_t)sf.f_bavail * (uint64_t)sf.f_bsize;
        if (errno != ENOENT && errno != ENOTDIR) return 0;
        char* slash = strrchr(probe, '/');
        if (!slash) return 0;
        if (slash == probe) {
            if (probe[1] == '\0') return 0;
            probe[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
}

// ---------------------------------------------------------------------------
// Migration from Soul (before the SoulX rename)
// ---------------------------------------------------------------------------

#include <libproc.h>

#define LEGACY_HOME_DIRNAME     ".soul-android"
#define LEGACY_AVD_PREFIX       "soul_"

// An engine runs from |home|/sdk/emulator (its executable lies there).
static bool engine_runs_from(const char* home) {
    char prefix[PATH_MAX];
    int n = snprintf(prefix, sizeof(prefix), "%s/sdk/emulator/", home);
    if (n < 0 || (size_t)n >= sizeof(prefix)) return false;
    int count = proc_listallpids(NULL, 0);
    if (count <= 0) return false;
    pid_t* pids = calloc((size_t)count + 64, sizeof(pid_t));
    if (!pids) return false;
    count = proc_listallpids(pids, (int)(((size_t)count + 64) * sizeof(pid_t)));
    bool found = false;
    for (int i = 0; i < count && !found; i++) {
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (pids[i] > 0 && proc_pidpath(pids[i], path, sizeof(path)) > 0 && strncmp(path, prefix, (size_t)n) == 0) {
            found = true;
        }
    }
    free(pids);
    return found;
}

// avd/soul_<name>.avd + .ini -> avd/soulx_<name>.avd + .ini; the .ini's path= line follows the new directory.
static void migrate_avds(const char* home) {
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s/avd", home) >= (int)sizeof(dir)) return;
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    char names[64][NAME_MAX + 1];
    int count = 0;
    while (count < 64 && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, LEGACY_AVD_PREFIX, strlen(LEGACY_AVD_PREFIX)) != 0) continue;
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".avd") != 0) continue;
        memcpy(names[count++], e->d_name, len + 1);
    }
    closedir(d);
    for (int i = 0; i < count; i++) {
        const char* rest = names[i] + strlen(LEGACY_AVD_PREFIX);                 // "<name>.avd"
        char from[PATH_MAX], to[PATH_MAX], ini_from[PATH_MAX], ini_to[PATH_MAX];
        int base = (int)strlen(rest) - 4;
        if (snprintf(from, sizeof(from), "%s/%s", dir, names[i]) >= (int)sizeof(from) ||
            snprintf(to, sizeof(to), "%s/soulx_%s", dir, rest) >= (int)sizeof(to) ||
            snprintf(ini_from, sizeof(ini_from), "%s/" LEGACY_AVD_PREFIX "%.*s.ini", dir, base, rest) >=
                (int)sizeof(ini_from) ||
            snprintf(ini_to, sizeof(ini_to), "%s/soulx_%.*s.ini", dir, base, rest) >= (int)sizeof(ini_to)) {
            continue;
        }
        if (soulx_path_exists(to) || rename(from, to) != 0) continue;
        (void)unlink(ini_from);                 // soulx_avd_write writes the .ini again at the next start
        char ini[PATH_MAX + 64];
        int n = snprintf(ini, sizeof(ini), "avd.ini.encoding=UTF-8\npath=%s\n", to);
        if (n > 0 && (size_t)n < sizeof(ini)) (void)soulx_write_file_atomic(ini_to, ini, (size_t)n);
    }
}

// The window's folders in the user's home (scripts, screenshots, videos): Soul's names -> SoulX's, when only the
// old one exists.
static void migrate_user_folders(void) {
    static const char* const pairs[][2] = {
        { "Documents/Soul Scripts", "Documents/SoulX Scripts" },
        { "Pictures/Soul", "Pictures/SoulX" },
        { "Movies/Soul", "Movies/SoulX" },
    };
    struct passwd pw;
    struct passwd* res = NULL;
    char pwbuf[4096];
    if (getpwuid_r(getuid(), &pw, pwbuf, sizeof(pwbuf), &res) != 0 || !res || !res->pw_dir) return;
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        char from[PATH_MAX], to[PATH_MAX];
        if (snprintf(from, sizeof(from), "%s/%s", res->pw_dir, pairs[i][0]) >= (int)sizeof(from) ||
            snprintf(to, sizeof(to), "%s/%s", res->pw_dir, pairs[i][1]) >= (int)sizeof(to)) {
            continue;
        }
        if (soulx_is_dir(from) && !soulx_path_exists(to) && rename(from, to) == 0) {
            soulx_log(SOULX_LOG_INFO, "renamed %s to %s", from, to);
        }
    }
}

int soulx_migrate_legacy_home(void) {
    migrate_user_folders();
    const char* env = getenv("SOULX_HOME");
    if (env && env[0]) return 0;                // an explicit home is never migrated into
    char home[PATH_MAX], legacy[PATH_MAX];
    if (default_home(home, sizeof(home)) != 0) return 0;
    size_t cut = strlen(home) - strlen(SOULX_HOME_DIRNAME);
    if (snprintf(legacy, sizeof(legacy), "%.*s%s", (int)cut, home, LEGACY_HOME_DIRNAME) >= (int)sizeof(legacy)) {
        return 0;
    }
    if (soulx_path_exists(home) || !soulx_is_dir(legacy)) return 0;
    if (engine_runs_from(legacy)) {
        soulx_log(SOULX_LOG_ERROR, "an instance started by Soul still runs from %s; stop it first (soul stop), then run "
                  "soulx again to move Soul's data to %s", legacy, home);
        return -1;
    }
    if (rename(legacy, home) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot move %s to %s: %s", legacy, home, strerror(errno));
        return -1;
    }
    char from[PATH_MAX], to[PATH_MAX];
    if (snprintf(from, sizeof(from), "%s/sdk/.soul", home) < (int)sizeof(from) &&
        snprintf(to, sizeof(to), "%s/sdk/.soulx", home) < (int)sizeof(to) && soulx_is_dir(from) &&
        !soulx_path_exists(to)) {
        (void)rename(from, to);                 // the install markers of the runtime
    }
    migrate_avds(home);
    soulx_log(SOULX_LOG_INFO, "moved Soul's runtime, instances and keymaps from %s to %s", legacy, home);
    return 0;
}

// ---------------------------------------------------------------------------
// UI language of SoulX's window and launcher: $SOULX_LANG, else $SOULX_HOME/ui_language, else the system's
// ---------------------------------------------------------------------------

static const char* lang_canonical(const char* s) {
    if (!s) return NULL;
    if (!strcasecmp(s, "system")) return "system";
    if (!strcasecmp(s, "en") || !strncasecmp(s, "en-", 3)) return "en";
    if (!strcasecmp(s, "zh-hant") || !strcasecmp(s, "zh-tw") || !strcasecmp(s, "zh-hk")) return "zh-hant";
    if (!strcasecmp(s, "zh-hans") || !strcasecmp(s, "zh-cn") || !strcasecmp(s, "zh")) return "zh-hans";
    return NULL;
}

const char* soulx_lang_canonical(const char* s) { return lang_canonical(s); }

const char* soulx_ui_language(void) {
    const char* env = lang_canonical(getenv("SOULX_LANG"));
    if (env) return env;
    char path[PATH_MAX];
    char* data = NULL;
    size_t len = 0;
    const char* v = "system";
    if (soulx_path(path, sizeof(path), "ui_language") == 0 && soulx_read_file(path, &data, &len) == 0) {
        data[strcspn(data, " \t\r\n")] = '\0';
        const char* c = lang_canonical(data);
        if (c) v = c;
    }
    free(data);
    return v;
}

int soulx_set_ui_language(const char* lang) {
    const char* c = lang_canonical(lang);
    if (!c) return -1;
    if (soulx_ensure_layout() != 0) return -1;
    char path[PATH_MAX];
    if (soulx_path(path, sizeof(path), "ui_language") != 0) return -1;
    char body[16];
    int n = snprintf(body, sizeof(body), "%s\n", c);
    return soulx_write_file_atomic(path, body, (size_t)n);
}
