/*
 * vm_archive.c - export an instance to one file (.soulxvm) and import it again, on this Mac or another.
 *
 * The file is an Apple Archive (aa, built into macOS) without compression: the disks hold mostly compressed game
 * data that does not shrink, and storing it plain makes packing fast and its progress exact. Inside:
 *
 *   manifest.ini      format, SoulX version, instance name, system image key/version, export time, total bytes
 *   checksums.txt     "<sha256>  <size>  <path>" for every file below: import refuses an archive that does not match
 *   instance.conf     the instance's settings (its id, created time and last port are not used on import)
 *   device/           the AVD folder: disks (userdata, encryption key, cache), config, modem state. Files that
 *                     belong to one engine run or name this Mac's paths are left out; the engine writes them anew.
 *   scripts/          the instance's Lua scripts (~/Documents/SoulX Scripts/<name>/): .lua files, script packages
 *                     and lib/, as the script list shows them; other things left in that folder are not taken
 *   keymaps/          every game keymap ($SOULX_HOME/keymaps); on import only the missing ones are added
 *
 * Game progress lives in the disks, so both directions protect them: export runs only while the instance is
 * stopped, under the instance lock, from copy-on-write clones (the instance itself is never written); import
 * verifies every checksum before the instance exists, never replaces an existing instance, and removes
 * everything it made when any step fails.
 */

#include "soulx_mac.h"

#include <CommonCrypto/CommonDigest.h>
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

#define VM_FORMAT        1
#define EM_DASH          "\xE2\x80\x94"     // UTF-8 em dash
#define AA_TOOL          "/usr/bin/aa"

// Files of one engine run, or files naming this Mac's paths: never archived (the engine recreates them).
static const char* const k_skip[] = {
    ".DS_Store", "multiinstance.lock", "hardware-qemu.ini", "emu-launch-params.txt", "bootcompleted.ini",
    "read-snapshot.txt", "tmpAdbCmds", "console_out", "snapshots", NULL,
};

static bool skipped(const char* name) {
    for (int i = 0; k_skip[i]; i++) {
        if (!strcmp(name, k_skip[i])) return true;
    }
    size_t n = strlen(name);
    return n > 5 && !strcmp(name + n - 5, ".lock");
}

static void report(SoulXVmProgress cb, void* ctx, const char* phase, double frac) {
    if (cb) cb(phase, frac < 0 ? 0 : (frac > 1 ? 1 : frac), ctx);
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

// Copies src (file or folder) to dst as a copy-on-write clone where the volume allows, else a plain copy.
// skip_host: leave out k_skip entries (the device folder). 0 ok.
static int clone_tree(const char* src, const char* dst, bool skip_host) {
    struct stat sb;
    if (lstat(src, &sb) != 0) return -1;
    if (S_ISLNK(sb.st_mode)) return 0;                     // nothing SoulX keeps is a link: never follow one
    if (S_ISREG(sb.st_mode)) {
        if (copyfile(src, dst, NULL, COPYFILE_ALL | COPYFILE_CLONE) != 0) {
            soulx_log(SOULX_LOG_ERROR, "cannot copy %s: %s", src, strerror(errno));
            return -1;
        }
        return 0;
    }
    if (!S_ISDIR(sb.st_mode)) return 0;
    if (mkdir(dst, 0755) != 0 && errno != EEXIST) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", dst, strerror(errno));
        return -1;
    }
    DIR* d = opendir(src);
    if (!d) return -1;
    int rc = 0;
    struct dirent* e;
    while (rc == 0 && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (skip_host && skipped(e->d_name)) continue;
        char s[PATH_MAX], t[PATH_MAX];
        if (snprintf(s, sizeof(s), "%s/%s", src, e->d_name) >= (int)sizeof(s) ||
            snprintf(t, sizeof(t), "%s/%s", dst, e->d_name) >= (int)sizeof(t)) {
            rc = -1;
            break;
        }
        rc = clone_tree(s, t, skip_host);
    }
    closedir(d);
    return rc;
}

// Adds the entries of src that dst lacks (never replaces a file there). 0 ok.
static int merge_missing(const char* src, const char* dst) {
    if (!soulx_is_dir(src)) return 0;
    if (soulx_mkdirs(dst) != 0) return -1;
    DIR* d = opendir(src);
    if (!d) return -1;
    int rc = 0;
    struct dirent* e;
    while (rc == 0 && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".DS_Store")) continue;
        char s[PATH_MAX], t[PATH_MAX];
        if (snprintf(s, sizeof(s), "%s/%s", src, e->d_name) >= (int)sizeof(s) ||
            snprintf(t, sizeof(t), "%s/%s", dst, e->d_name) >= (int)sizeof(t)) {
            rc = -1;
            break;
        }
        if (soulx_is_dir(s)) {
            rc = merge_missing(s, t);
        } else if (!soulx_path_exists(t)) {
            rc = clone_tree(s, t, false);
        }
    }
    closedir(d);
    return rc;
}

static bool is_lua(const char* name) {
    size_t n = strlen(name);
    return n > 4 && !strcasecmp(name + n - 4, ".lua");
}

static bool dir_has_lua(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return false;
    bool found = false;
    struct dirent* e;
    while (!found && (e = readdir(d))) found = e->d_name[0] != '.' && is_lua(e->d_name);
    closedir(d);
    return found;
}

// The instance's scripts as SoulX sees them (soulx_scripts_list): top-level .lua files, folders holding a .lua
// (script packages) and the shared lib/ folder. Anything else a user left in the folder stays out. 0 ok.
static int clone_scripts(const char* src, const char* dst) {
    DIR* d = opendir(src);
    if (!d) return errno == ENOENT ? 0 : -1;
    int rc = 0;
    bool made = false;
    struct dirent* e;
    while (rc == 0 && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char s[PATH_MAX], t[PATH_MAX];
        if (snprintf(s, sizeof(s), "%s/%s", src, e->d_name) >= (int)sizeof(s) ||
            snprintf(t, sizeof(t), "%s/%s", dst, e->d_name) >= (int)sizeof(t)) continue;
        bool dir = soulx_is_dir(s);
        bool take = dir ? (!strcasecmp(e->d_name, "lib") || dir_has_lua(s)) : is_lua(e->d_name);
        if (!take) continue;
        if (!made) {
            if (mkdir(dst, 0755) != 0 && errno != EEXIST) { rc = -1; break; }
            made = true;
        }
        rc = clone_tree(s, t, false);
    }
    closedir(d);
    return rc;
}

// The regular files under root, as paths relative to it.
typedef struct {
    char** rel;
    uint64_t* size;
    size_t n, cap;
    uint64_t total;
} FileList;

static void list_free(FileList* l) {
    for (size_t i = 0; i < l->n; i++) free(l->rel[i]);
    free(l->rel);
    free(l->size);
    memset(l, 0, sizeof(*l));
}

static int list_walk(const char* root, const char* rel, FileList* l) {
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s%s%s", root, rel[0] ? "/" : "", rel) >= (int)sizeof(dir)) return -1;
    DIR* d = opendir(dir);
    if (!d) return -1;
    int rc = 0;
    struct dirent* e;
    while (rc == 0 && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char r[PATH_MAX], full[PATH_MAX];
        if (snprintf(r, sizeof(r), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name) >= (int)sizeof(r) ||
            snprintf(full, sizeof(full), "%s/%s", root, r) >= (int)sizeof(full)) {
            rc = -1;
            break;
        }
        struct stat sb;
        if (lstat(full, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) {
            rc = list_walk(root, r, l);
        } else if (S_ISREG(sb.st_mode)) {
            if (l->n == l->cap) {
                size_t cap = l->cap ? l->cap * 2 : 64;
                char** nr = realloc(l->rel, cap * sizeof(char*));
                if (!nr) { rc = -1; break; }
                l->rel = nr;
                uint64_t* ns = realloc(l->size, cap * sizeof(uint64_t));
                if (!ns) { rc = -1; break; }
                l->size = ns;
                l->cap = cap;
            }
            l->rel[l->n] = strdup(r);
            if (!l->rel[l->n]) { rc = -1; break; }
            l->size[l->n] = (uint64_t)sb.st_size;
            l->total += (uint64_t)sb.st_size;
            l->n++;
        }
    }
    closedir(d);
    return rc;
}

// SHA-256 of a file as lower-case hex; *done grows by the bytes read (for progress). 0 ok.
static int sha256_file(const char* path, char hex[65], uint64_t* done, uint64_t total, SoulXVmProgress cb,
                       void* ctx, const char* phase) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    (void)fcntl(fd, F_NOCACHE, 1);                         // a one-pass read of gigabytes: keep the page cache
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    size_t cap = 4 << 20;
    unsigned char* buf = malloc(cap);
    if (!buf) { close(fd); return -1; }
    int rc = 0;
    double last = 0;
    for (;;) {
        if (soulx_interrupted()) { rc = -1; break; }
        ssize_t r = read(fd, buf, cap);
        if (r < 0) {
            if (errno == EINTR) continue;
            rc = -1;
            break;
        }
        if (r == 0) break;
        CC_SHA256_Update(&c, buf, (CC_LONG)r);
        *done += (uint64_t)r;
        double now = soulx_now_ms();
        if (now - last > 250) {
            report(cb, ctx, phase, total ? (double)*done / (double)total : 0);
            last = now;
        }
    }
    free(buf);
    close(fd);
    if (rc != 0) return rc;
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(md, &c);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + i * 2, 3, "%02x", md[i]);
    return 0;
}

// Runs aa with argv, reporting progress while it works: grow_path's size (a file, or a folder's total) against
// expect bytes. 0 when aa exited 0.
static int run_aa(const char* const* argv, const char* grow_path, bool grow_is_dir, uint64_t expect,
                  SoulXVmProgress cb, void* ctx, const char* phase) {
    pid_t pid;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    int err = posix_spawn(&pid, argv[0], &fa, NULL, (char* const*)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (err != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot run %s: %s", argv[0], strerror(err));
        return -1;
    }
    int status = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno != EINTR) return -1;
        if (soulx_interrupted()) {
            kill(pid, SIGTERM);
            (void)waitpid(pid, &status, 0);
            return -1;
        }
        uint64_t have = 0;
        if (grow_is_dir) {
            have = soulx_dir_size(grow_path);
        } else {
            struct stat sb;
            if (stat(grow_path, &sb) == 0) have = (uint64_t)sb.st_size;
        }
        report(cb, ctx, phase, expect ? (double)have / (double)expect : 0);
        (void)soulx_sleep_ms(400);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        soulx_log(SOULX_LOG_ERROR, "%s failed (status %d)", argv[0], WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return -1;
    }
    return 0;
}

static int write_text(const char* path, const char* text) {
    return soulx_write_file_atomic(path, text, strlen(text));
}

static void iso_utc_now(char* buf, size_t cap) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

// A fresh work folder under $SOULX_HOME/tmp (so moves into avd/ are renames on the same volume).
static int work_dir(const char* kind, char* out, size_t cap) {
    if (soulx_path(out, cap, "tmp/%s-%d-%lld", kind, (int)getpid(), (long long)time(NULL)) != 0) return -1;
    if (soulx_mkdirs(out) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", out, strerror(errno));
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

static void manifest_kv(char* line, SoulXVmManifest* m) {
    char* eq = strchr(line, '=');
    if (!eq) return;
    *eq = 0;
    const char* k = line;
    const char* v = eq + 1;
    if (!strcmp(k, "format")) m->format = atoi(v);
    else if (!strcmp(k, "name")) snprintf(m->name, sizeof(m->name), "%s", v);
    else if (!strcmp(k, "image")) snprintf(m->image, sizeof(m->image), "%s", v);
    else if (!strcmp(k, "image_version")) snprintf(m->image_version, sizeof(m->image_version), "%s", v);
    else if (!strcmp(k, "android")) snprintf(m->android, sizeof(m->android), "%s", v);
    else if (!strcmp(k, "soulx_version")) snprintf(m->soulx_version, sizeof(m->soulx_version), "%s", v);
    else if (!strcmp(k, "exported")) snprintf(m->exported, sizeof(m->exported), "%s", v);
    else if (!strcmp(k, "bytes")) m->bytes = strtoull(v, NULL, 10);
    else if (!strcmp(k, "scripts")) m->scripts = atoi(v);
}

static int manifest_read(const char* path, SoulXVmManifest* m) {
    char* data = NULL;
    size_t len = 0;
    if (soulx_read_file(path, &data, &len) != 0) return -1;
    memset(m, 0, sizeof(*m));
    char* save = NULL;
    for (char* line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) manifest_kv(line, m);
    free(data);
    if (m->format < 1 || !m->name[0] || !m->image[0]) return -1;
    return 0;
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

int soulx_vm_export(const char* name, const char* out_path, SoulXVmProgress cb, void* ctx) {
    SoulXInstanceConf c;
    if (!name || !out_path || soulx_reg_load(name, &c) != 0) {
        soulx_log(SOULX_LOG_ERROR, "no such instance '%s' (see: soulx list)", name ? name : "");
        return -1;
    }
    char avd[SOULX_NAME_MAX + 8], dev[PATH_MAX], conf_file[PATH_MAX], scripts[PATH_MAX], keymaps[PATH_MAX];
    soulx_avd_name(c.name, avd, sizeof(avd));
    if (soulx_path(dev, sizeof(dev), "avd/%s.avd", avd) != 0 ||
        soulx_path(conf_file, sizeof(conf_file), "instances/%s.conf", c.name) != 0 ||
        soulx_path(keymaps, sizeof(keymaps), "keymaps") != 0) return -1;
    if (!soulx_is_dir(dev)) {
        soulx_log(SOULX_LOG_ERROR, "the device data of '%s' is missing (%s)", c.name, dev);
        return -1;
    }

    // Stopped, and kept stopped while the disks are cloned: the instance lock keeps start/delete out.
    int lock = soulx_lock_acquire();
    if (lock < 0) return -1;
    if (soulx_instance_running(c.name, NULL)) {
        soulx_lock_release(lock);
        soulx_log(SOULX_LOG_ERROR, "instance '%s' is running " EM_DASH " stop it first so its disks are complete: "
                 "soulx stop %s", c.name, c.name);
        return -1;
    }
    char work[PATH_MAX], part[PATH_MAX + 8];
    if (work_dir("export", work, sizeof(work)) != 0) {
        soulx_lock_release(lock);
        return -1;
    }
    int rc = -1;
    char p[PATH_MAX];
    FileList files = { 0 };
    report(cb, ctx, "prepare", 0);
    snprintf(p, sizeof(p), "%s/device", work);
    if (clone_tree(dev, p, true) != 0) goto unlock;
    snprintf(p, sizeof(p), "%s/instance.conf", work);
    if (clone_tree(conf_file, p, false) != 0) goto unlock;
    soulx_lock_release(lock);                              // the clones are this moment's disks
    lock = -1;

    int nscripts = 0;
    if (soulx_scripts_dir(c.name, scripts, sizeof(scripts), false) == 0 && soulx_is_dir(scripts)) {
        snprintf(p, sizeof(p), "%s/scripts", work);
        if (clone_scripts(scripts, p) != 0) goto out;
        nscripts = soulx_scripts_list(c.name, NULL, 0);
        if (nscripts < 0) nscripts = 0;
    }
    if (soulx_is_dir(keymaps)) {
        snprintf(p, sizeof(p), "%s/keymaps", work);
        if (clone_tree(keymaps, p, false) != 0) goto out;
    }

    // checksums of everything, then the manifest
    if (list_walk(work, "", &files) != 0) goto out;
    {
        size_t cap = files.n * (64 + 2 + 24 + 2 + PATH_MAX / 8) + 64;
        char* sums = malloc(cap);
        if (!sums) goto out;
        size_t len = 0;
        uint64_t done = 0;
        for (size_t i = 0; i < files.n; i++) {
            char hex[65];
            snprintf(p, sizeof(p), "%s/%s", work, files.rel[i]);
            if (sha256_file(p, hex, &done, files.total, cb, ctx, "checksum") != 0) {
                free(sums);
                if (!soulx_interrupted()) soulx_log(SOULX_LOG_ERROR, "cannot read %s", p);
                goto out;
            }
            int n = snprintf(sums + len, cap - len, "%s  %llu  %s\n", hex, (unsigned long long)files.size[i],
                             files.rel[i]);
            if (n < 0 || (size_t)n >= cap - len) { free(sums); goto out; }
            len += (size_t)n;
        }
        snprintf(p, sizeof(p), "%s/checksums.txt", work);
        int w = soulx_write_file_atomic(p, sums, len);
        free(sums);
        if (w != 0) goto out;
    }
    {
        const SoulXPackage* img = soulx_runtime_find(c.image);
        char now[32], man[1024];
        iso_utc_now(now, sizeof(now));
        snprintf(man, sizeof(man),
                 "# SoulX instance export\nformat=%d\nsoulx_version=%s\nname=%s\nimage=%s\nimage_version=%s\n"
                 "android=%s\nexported=%s\nbytes=%llu\nscripts=%d\n",
                 VM_FORMAT, SOULX_VERSION_STRING, c.name, c.image, img && img->version ? img->version : "",
                 img && img->android_name ? img->android_name : "", now, (unsigned long long)files.total, nscripts);
        snprintf(p, sizeof(p), "%s/manifest.ini", work);
        if (write_text(p, man) != 0) goto out;
    }

    // pack: written next to the target as .part, renamed when complete
    if (snprintf(part, sizeof(part), "%s.part", out_path) >= (int)sizeof(part)) goto out;
    (void)unlink(part);
    {
        const char* argv[] = { AA_TOOL, "archive", "-a", "raw", "-d", work, "-o", part, NULL };
        if (run_aa(argv, part, false, files.total, cb, ctx, "pack") != 0) {
            (void)unlink(part);
            goto out;
        }
    }
    if (rename(part, out_path) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot write %s: %s", out_path, strerror(errno));
        (void)unlink(part);
        goto out;
    }
    report(cb, ctx, "done", 1);
    rc = 0;
    goto out;

unlock:
    if (lock >= 0) soulx_lock_release(lock);
out:
    list_free(&files);
    (void)soulx_rmtree(work);
    return rc;
}

// ---------------------------------------------------------------------------
// Inspect and import
// ---------------------------------------------------------------------------

int soulx_vm_inspect(const char* archive, SoulXVmManifest* m) {
    if (!archive || !m || !soulx_path_exists(archive)) {
        soulx_log(SOULX_LOG_ERROR, "cannot open %s", archive ? archive : "");
        return -1;
    }
    char work[PATH_MAX], p[PATH_MAX];
    if (work_dir("inspect", work, sizeof(work)) != 0) return -1;
    const char* argv[] = { AA_TOOL, "extract", "-i", archive, "-d", work, "-include-path", "manifest.ini", NULL };
    int rc = run_aa(argv, work, true, 0, NULL, NULL, NULL);
    snprintf(p, sizeof(p), "%s/manifest.ini", work);
    if (rc == 0 && manifest_read(p, m) != 0) {
        soulx_log(SOULX_LOG_ERROR, "%s is not a SoulX instance export", archive);
        rc = -1;
    }
    if (rc == 0 && m->format > VM_FORMAT) {
        soulx_log(SOULX_LOG_ERROR, "%s was exported by a newer SoulX (%s); update SoulX to import it", archive,
                 m->soulx_version);
        rc = -1;
    }
    struct stat sb;
    if (rc == 0 && stat(archive, &sb) == 0) m->archive_bytes = (uint64_t)sb.st_size;
    (void)soulx_rmtree(work);
    return rc;
}

// Every line of checksums.txt matches the extracted file. 0 ok.
static int verify(const char* work, SoulXVmProgress cb, void* ctx) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/checksums.txt", work);
    char* data = NULL;
    size_t len = 0;
    if (soulx_read_file(p, &data, &len) != 0) {
        soulx_log(SOULX_LOG_ERROR, "the export has no checksums; it is damaged or not a SoulX export");
        return -1;
    }
    uint64_t total = 0;
    for (char* s = data; *s;) {                            // pass 1: total bytes, for progress
        char hex[65];
        unsigned long long size = 0;
        if (sscanf(s, "%64s %llu", hex, &size) == 2) total += size;
        char* nl = strchr(s, '\n');
        if (!nl) break;
        s = nl + 1;
    }
    int rc = 0;
    uint64_t done = 0;
    char* save = NULL;
    for (char* line = strtok_r(data, "\n", &save); line && rc == 0; line = strtok_r(NULL, "\n", &save)) {
        char want[65], got[65];
        unsigned long long size = 0;
        int off = 0;
        if (sscanf(line, "%64s %llu %n", want, &size, &off) != 2 || off <= 0) continue;
        const char* rel = line + off;
        if (strstr(rel, "..") || rel[0] == '/') { rc = -1; break; }
        snprintf(p, sizeof(p), "%s/%s", work, rel);
        struct stat sb;
        if (stat(p, &sb) != 0 || (unsigned long long)sb.st_size != size) {
            soulx_log(SOULX_LOG_ERROR, "the export is incomplete: %s is missing or has the wrong size", rel);
            rc = -1;
            break;
        }
        if (sha256_file(p, got, &done, total, cb, ctx, "verify") != 0) { rc = -1; break; }
        if (strcmp(want, got) != 0) {
            soulx_log(SOULX_LOG_ERROR, "the export is damaged: %s does not match its checksum", rel);
            rc = -1;
            break;
        }
    }
    free(data);
    return rc;
}

int soulx_vm_import(const char* archive, const char* as_name, char* name_out, size_t cap, SoulXVmProgress cb,
                    void* ctx) {
    SoulXVmManifest m;
    if (soulx_vm_inspect(archive, &m) != 0) return -1;
    const char* name = as_name && as_name[0] ? as_name : m.name;
    if (!soulx_name_valid(name)) {
        soulx_log(SOULX_LOG_ERROR, "'%s' is not a valid instance name (lowercase letters, digits, - and _)", name);
        return -1;
    }
    SoulXInstanceConf existing;
    if (soulx_reg_load(name, &existing) == 0) {
        soulx_log(SOULX_LOG_ERROR, "an instance named '%s' already exists; import it under another name: "
                 "soulx import FILE --name NEW", name);
        return -1;
    }
    const SoulXPackage* img = soulx_runtime_find(m.image);
    if (!img || img->kind != SOULX_PKG_IMAGE) {
        soulx_log(SOULX_LOG_ERROR, "the export uses system image '%s', which this SoulX does not know", m.image);
        return -1;
    }
    if (m.image_version[0] && img->version && strcmp(m.image_version, img->version) != 0) {
        soulx_log(SOULX_LOG_WARN, "the export was made with %s %s; this SoulX has %s %s (Android updates the "
                 "instance on its first start)", m.image, m.image_version, m.image, img->version);
    }
    uint64_t free_b = soulx_free_disk_bytes(soulx_home());
    if (free_b && free_b < m.archive_bytes + (m.archive_bytes / 10)) {
        soulx_log(SOULX_LOG_ERROR, "not enough free disk space: the import needs about %.1f GB",
                 (double)m.archive_bytes * 1.1 / 1e9);
        return -1;
    }

    char work[PATH_MAX], p[PATH_MAX];
    if (work_dir("import", work, sizeof(work)) != 0) return -1;
    int rc = -1;
    bool created = false;
    SoulXInstanceConf c;
    {
        const char* argv[] = { AA_TOOL, "extract", "-i", archive, "-d", work, NULL };
        if (run_aa(argv, work, true, m.bytes, cb, ctx, "unpack") != 0) goto out;
    }
    if (verify(work, cb, ctx) != 0) goto out;

    {
        char* text = NULL;
        size_t len = 0;
        snprintf(p, sizeof(p), "%s/instance.conf", work);
        if (soulx_read_file(p, &text, &len) != 0 || soulx_conf_parse(text, name, &c) != 0) {
            free(text);
            soulx_log(SOULX_LOG_ERROR, "the export has no readable instance settings");
            goto out;
        }
        free(text);
    }
    c.last_port = 0;
    report(cb, ctx, "install", 0);
    if (soulx_mac_create(&c) != 0) goto out;               // a new id, its own config.ini and .ini
    created = true;

    // the disks and device state go in beside the config.ini SoulX just wrote for this name and Mac
    {
        char avd[SOULX_NAME_MAX + 8], dev[PATH_MAX], src[PATH_MAX];
        soulx_avd_name(c.name, avd, sizeof(avd));
        if (soulx_path(dev, sizeof(dev), "avd/%s.avd", avd) != 0) goto out;
        snprintf(src, sizeof(src), "%s/device", work);
        DIR* d = opendir(src);
        if (!d) {
            soulx_log(SOULX_LOG_ERROR, "the export has no device data");
            goto out;
        }
        struct dirent* e;
        int mv = 0;
        while (mv == 0 && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, "config.ini") ||
                skipped(e->d_name)) continue;
            char s[PATH_MAX], t[PATH_MAX];
            snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
            snprintf(t, sizeof(t), "%s/%s", dev, e->d_name);
            if (rename(s, t) != 0) {
                soulx_log(SOULX_LOG_ERROR, "cannot move %s into place: %s", e->d_name, strerror(errno));
                mv = -1;
            }
        }
        closedir(d);
        if (mv != 0) goto out;
    }

    // scripts into the new instance's folder, keymaps the user does not have yet
    snprintf(p, sizeof(p), "%s/scripts", work);
    if (soulx_is_dir(p)) {
        char dst[PATH_MAX];
        if (soulx_scripts_dir(c.name, dst, sizeof(dst), true) != 0 || merge_missing(p, dst) != 0) {
            soulx_log(SOULX_LOG_WARN, "the scripts could not all be copied to the instance's scripts folder");
        }
    }
    snprintf(p, sizeof(p), "%s/keymaps", work);
    if (soulx_is_dir(p)) {
        char dst[PATH_MAX];
        if (soulx_path(dst, sizeof(dst), "keymaps") != 0 || merge_missing(p, dst) != 0) {
            soulx_log(SOULX_LOG_WARN, "the game keymaps could not all be copied");
        }
    }
    report(cb, ctx, "done", 1);
    if (name_out && cap) snprintf(name_out, cap, "%s", c.name);
    rc = 0;

out:
    if (rc != 0 && created) (void)soulx_mac_delete(c.name);   // nothing half-imported stays behind
    (void)soulx_rmtree(work);
    return rc;
}
