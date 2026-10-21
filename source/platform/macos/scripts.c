/*
 * SoulX Android Emulator - each instance's Lua scripts
 *
 * Scripts live where the user can see them: ~/Documents/SoulX Scripts/<instance>/ ($SOULX_SCRIPTS replaces the
 * root, for tests and other setups). A script is a .lua file at the top of that folder, or a folder (a script
 * package: its .lua files plus whatever they require, e.g. lib/) one level down. A script's display name is its
 * "-- name: ..." line near the top.
 *
 * Before SoulX had one folder per instance, every script sat directly in the root; soulx_scripts_migrate() moves
 * such entries into default/ once (they were made for the default instance).
 */
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "soulx_mac.h"

#define SCRIPTS_DIRNAME     "SoulX Scripts"
#define SCRIPTS_MARKER      ".soulx-per-instance"   // root layout: one folder per instance
#define NAME_SCAN_LINES     40

static bool user_dir(char* out, size_t cap) {
    struct passwd pw;
    struct passwd* res = NULL;
    char pwbuf[4096];
    const char* home = NULL;
    if (getpwuid_r(getuid(), &pw, pwbuf, sizeof(pwbuf), &res) == 0 && res && res->pw_dir && res->pw_dir[0] == '/') {
        home = res->pw_dir;
    } else {
        home = getenv("HOME");
    }
    if (!home || home[0] != '/') return false;
    int n = snprintf(out, cap, "%s", home);
    return n > 0 && (size_t)n < cap;
}

int soulx_scripts_root(char* out, size_t cap) {
    const char* env = getenv("SOULX_SCRIPTS");
    int n;
    if (env && env[0] == '/') {
        n = snprintf(out, cap, "%s", env);
    } else {
        char home[PATH_MAX];
        if (!user_dir(home, sizeof(home))) return -1;
        n = snprintf(out, cap, "%s/Documents/%s", home, SCRIPTS_DIRNAME);
    }
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static void write_marker(const char* root) {
    char m[PATH_MAX];
    if (snprintf(m, sizeof(m), "%s/%s", root, SCRIPTS_MARKER) >= (int)sizeof(m)) return;
    if (!soulx_path_exists(m)) {
        static const char note[] = "One folder per SoulX instance: <instance>/ holds that instance's scripts.\n";
        (void)soulx_write_file_atomic(m, note, sizeof(note) - 1);
    }
}

int soulx_scripts_dir(const char* instance, char* out, size_t cap, bool create) {
    if (!soulx_name_valid(instance)) return -1;
    char root[PATH_MAX];
    if (soulx_scripts_root(root, sizeof(root)) != 0) return -1;
    int n = snprintf(out, cap, "%s/%s", root, instance);
    if (n < 0 || (size_t)n >= cap) return -1;
    if (create) {
        bool fresh_root = !soulx_is_dir(root);
        if (soulx_mkdirs(out) != 0) return -1;
        if (fresh_root) write_marker(root);         // a new root needs no migration
    }
    return 0;
}

int soulx_scripts_migrate(void) {
    char root[PATH_MAX], marker[PATH_MAX];
    if (soulx_scripts_root(root, sizeof(root)) != 0 || !soulx_is_dir(root)) return 0;
    if (snprintf(marker, sizeof(marker), "%s/%s", root, SCRIPTS_MARKER) >= (int)sizeof(marker)) return -1;
    if (soulx_path_exists(marker)) return 0;
    DIR* d = opendir(root);
    if (!d) return -1;
    char dst_dir[PATH_MAX];
    if (snprintf(dst_dir, sizeof(dst_dir), "%s/default", root) >= (int)sizeof(dst_dir)) { closedir(d); return -1; }
    int moved = 0, failed = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;                       // ., .., .DS_Store, the marker
        SoulXInstanceConf c;
        if (soulx_name_valid(e->d_name) && soulx_reg_load(e->d_name, &c) == 0) continue;   // already per-instance
        if (soulx_mkdirs(dst_dir) != 0) { failed++; break; }
        char from[PATH_MAX], to[PATH_MAX];
        if (snprintf(from, sizeof(from), "%s/%s", root, e->d_name) >= (int)sizeof(from) ||
            snprintf(to, sizeof(to), "%s/%s", dst_dir, e->d_name) >= (int)sizeof(to)) {
            failed++;
            continue;
        }
        if (soulx_path_exists(to) || rename(from, to) != 0) {
            soulx_log(SOULX_LOG_WARN, "scripts: could not move %s into %s", from, dst_dir);
            failed++;
            continue;
        }
        moved++;
    }
    closedir(d);
    if (moved) soulx_log(SOULX_LOG_INFO, "scripts: moved %d item(s) into %s (scripts are now kept per instance)",
                         moved, dst_dir);
    if (failed == 0) write_marker(root);            // try again next time if anything is left behind
    return failed ? -1 : 0;
}

int soulx_script_name(const char* path, char* out, size_t cap) {
    if (cap) out[0] = '\0';
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int found = -1;
    for (int i = 0; i < NAME_SCAN_LINES && fgets(line, sizeof(line), f); i++) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "--", 2) != 0) continue;
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '@') { p++; while (*p == ' ' || *p == '\t') p++; }
        if (strncasecmp(p, "name", 4) != 0) continue;
        char sep = p[4];
        if (sep != ':' && sep != '=' && sep != ' ' && sep != '\t') continue;
        p += 4;
        while (*p == ':' || *p == '=' || *p == ' ' || *p == '\t') p++;
        size_t n = strcspn(p, "\r\n");
        while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
        if (n == 0) continue;
        if (n >= cap) n = cap - 1;
        memcpy(out, p, n);
        out[n] = '\0';
        found = 0;
        break;
    }
    fclose(f);
    return found;
}

static bool is_lua(const char* name) {
    size_t n = strlen(name);
    return n > 4 && name[0] != '.' && strcasecmp(name + n - 4, ".lua") == 0;
}

static int cmp_scripts(const void* a, const void* b) {
    return strcasecmp(((const SoulXScript*)a)->file, ((const SoulXScript*)b)->file);
}

static void add_script(SoulXScript* out, int max, int* count, const char* dir, const char* rel, const char* top,
                       bool package) {
    if (*count >= max) { (*count)++; return; }
    SoulXScript* s = &out[*count];
    memset(s, 0, sizeof(*s));
    snprintf(s->file, sizeof(s->file), "%s", rel);
    snprintf(s->top, sizeof(s->top), "%s", top);
    snprintf(s->path, sizeof(s->path), "%s/%s", dir, rel);
    s->package = package;
    struct stat st;
    if (stat(s->path, &st) == 0) { s->size = (long long)st.st_size; s->modified = st.st_mtime; }
    (void)soulx_script_name(s->path, s->name, sizeof(s->name));
    (*count)++;
}

int soulx_scripts_list(const char* instance, SoulXScript* out, int max) {
    char dir[PATH_MAX];
    if (soulx_scripts_dir(instance, dir, sizeof(dir), false) != 0) return -1;
    DIR* d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -1;
    int count = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char full[PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", dir, e->d_name) >= (int)sizeof(full)) continue;
        if (soulx_is_dir(full)) {
            if (strcasecmp(e->d_name, "lib") == 0) continue;     // shared helpers, not scripts
            DIR* sd = opendir(full);
            if (!sd) continue;
            struct dirent* se;
            while ((se = readdir(sd)) != NULL) {
                if (!is_lua(se->d_name)) continue;
                char rel[PATH_MAX];
                if (snprintf(rel, sizeof(rel), "%s/%s", e->d_name, se->d_name) >= (int)sizeof(rel)) continue;
                add_script(out, max, &count, dir, rel, e->d_name, true);
            }
            closedir(sd);
        } else if (is_lua(e->d_name)) {
            add_script(out, max, &count, dir, e->d_name, e->d_name, false);
        }
    }
    closedir(d);
    int n = count < max ? count : max;
    if (out && n > 1) qsort(out, (size_t)n, sizeof(SoulXScript), cmp_scripts);
    return count;
}

// A name the user gave for an entry inside the instance's folder: one path component, no tricks.
static bool entry_ok(const char* s) {
    if (!s || !s[0] || s[0] == '.' || strchr(s, '/') || strlen(s) > 200) return false;
    return true;
}

int soulx_scripts_import(const char* instance, const char* src, char* dest_out, size_t cap) {
    struct stat st;
    if (!src || stat(src, &st) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot read %s: %s", src ? src : "(null)", strerror(errno));
        return -1;
    }
    bool dir = S_ISDIR(st.st_mode);
    char whole[PATH_MAX];                                    // ".../pkg/" -> ".../pkg" before taking the last part
    snprintf(whole, sizeof(whole), "%s", src);
    size_t wl = strlen(whole);
    while (wl > 1 && whole[wl - 1] == '/') whole[--wl] = '\0';
    const char* base = strrchr(whole, '/');
    base = base ? base + 1 : whole;
    char trimmed[PATH_MAX];
    snprintf(trimmed, sizeof(trimmed), "%s", base);
    if (!entry_ok(trimmed)) {
        soulx_log(SOULX_LOG_ERROR, "cannot import '%s': give a file or folder name", src);
        return -1;
    }
    if (!dir && !is_lua(trimmed)) {
        soulx_log(SOULX_LOG_ERROR, "'%s' is not a .lua script (import a .lua file or a folder that contains one)", src);
        return -1;
    }
    char folder[PATH_MAX], dest[PATH_MAX];
    if (soulx_scripts_dir(instance, folder, sizeof(folder), true) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create the scripts folder of '%s'", instance);
        return -1;
    }
    if (snprintf(dest, sizeof(dest), "%s/%s", folder, trimmed) >= (int)sizeof(dest)) return -1;
    char rsrc[PATH_MAX], rdest[PATH_MAX];
    if (realpath(src, rsrc) && realpath(dest, rdest) && strcmp(rsrc, rdest) == 0) {
        if (dest_out) snprintf(dest_out, cap, "%s", dest);        // already there
        return 0;
    }
    if (soulx_path_exists(dest) && soulx_scripts_trash(instance, trimmed) != 0) return -1;   // replace, keeping the old
    copyfile_flags_t flags = COPYFILE_ALL | (dir ? COPYFILE_RECURSIVE : 0);
    if (copyfile(src, dest, NULL, flags) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot copy %s into %s: %s", src, folder, strerror(errno));
        return -1;
    }
    if (dest_out) snprintf(dest_out, cap, "%s", dest);
    return 0;
}

// Moves path into the Trash as `name` (made unique the way the Trash does it). $SOULX_TRASH_DIR, when set,
// stands in for the Trash (tests never touch the user's real one). 0 ok.
int soulx_move_to_trash(const char* path, const char* name) {
    char home[PATH_MAX], trash[PATH_MAX], to[PATH_MAX];
    const char* tenv = getenv("SOULX_TRASH_DIR");
    if (tenv && tenv[0] == '/') {
        if (snprintf(trash, sizeof(trash), "%s", tenv) >= (int)sizeof(trash)) return -1;
    } else if (!user_dir(home, sizeof(home)) || snprintf(trash, sizeof(trash), "%s/.Trash", home) >= (int)sizeof(trash)) {
        return -1;
    }
    (void)soulx_mkdirs(trash);
    // the Trash keeps names unique: "farm.lua", then "farm.lua 2026-10-17 12.30.05", then with a counter
    snprintf(to, sizeof(to), "%s/%s", trash, name);
    if (soulx_path_exists(to)) {
        char stamp[32];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M.%S", &tm);
        snprintf(to, sizeof(to), "%s/%s %s", trash, name, stamp);
        for (int i = 2; soulx_path_exists(to) && i < 100; i++) snprintf(to, sizeof(to), "%s/%s %s %d", trash, name, stamp, i);
    }
    if (rename(path, to) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot move %s to the Trash: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int soulx_scripts_trash(const char* instance, const char* entry) {
    if (!entry_ok(entry)) {
        soulx_log(SOULX_LOG_ERROR, "'%s' is not an entry of the scripts folder", entry ? entry : "");
        return -1;
    }
    char folder[PATH_MAX], path[PATH_MAX];
    if (soulx_scripts_dir(instance, folder, sizeof(folder), false) != 0 ||
        snprintf(path, sizeof(path), "%s/%s", folder, entry) >= (int)sizeof(path)) return -1;
    if (!soulx_path_exists(path)) {
        soulx_log(SOULX_LOG_ERROR, "no script '%s' in %s", entry, folder);
        return -1;
    }
    return soulx_move_to_trash(path, entry);
}

// The folders in ~/Documents that belong to one instance: its scripts, its network captures, and the captures of
// SoulX versions before network_capture (SoulX Captures/<name>). Deleting the instance moves them to the Trash.
int soulx_instance_trash_user_files(const char* instance, int* moved) {
    if (moved) *moved = 0;
    if (!soulx_name_valid(instance)) return -1;
    char home[PATH_MAX], dir[PATH_MAX], label[160];
    int rc = 0;
    struct { const char* env; const char* docs; const char* what; } spots[] = {
        { NULL, NULL, "scripts" },
        { "SOULX_NETWORK_CAPTURES", "SoulX Network Captures", "network captures" },
        { NULL, "SoulX Captures", "captures" },
    };
    bool have_home = user_dir(home, sizeof(home));
    for (size_t i = 0; i < sizeof(spots) / sizeof(spots[0]); i++) {
        int n = -1;
        if (i == 0) {
            n = soulx_scripts_dir(instance, dir, sizeof(dir), false) == 0 ? 0 : -1;
        } else {
            const char* base = spots[i].env ? getenv(spots[i].env) : NULL;
            if (base && base[0] == '/') {
                n = snprintf(dir, sizeof(dir), "%s/%s", base, instance);
            } else if (have_home) {
                n = snprintf(dir, sizeof(dir), "%s/Documents/%s/%s", home, spots[i].docs, instance);
            }
            if (n >= (int)sizeof(dir)) n = -1;
        }
        if (n < 0 || !soulx_is_dir(dir)) continue;
        snprintf(label, sizeof(label), "%s %s", instance, spots[i].what);      // "farm scripts" in the Trash
        if (soulx_move_to_trash(dir, label) == 0) {
            if (moved) (*moved)++;
            soulx_log(SOULX_LOG_INFO, "moved %s to the Trash (as \"%s\")", dir, label);
        } else {
            rc = -1;
        }
    }
    return rc;
}

int soulx_scripts_set_name(const char* instance, const char* file, const char* name) {
    if (!name || !name[0] || strpbrk(name, "\r\n") || strlen(name) > 120) {
        soulx_log(SOULX_LOG_ERROR, "a script name is one line of up to 120 characters");
        return -1;
    }
    if (!file || !file[0] || file[0] == '/' || strstr(file, "..") || !is_lua(strrchr(file, '/') ? strrchr(file, '/') + 1 : file)) {
        soulx_log(SOULX_LOG_ERROR, "'%s' is not a script of '%s' (see: soulx scripts -i %s)", file ? file : "",
                  instance, instance);
        return -1;
    }
    char folder[PATH_MAX], path[PATH_MAX];
    if (soulx_scripts_dir(instance, folder, sizeof(folder), false) != 0 ||
        snprintf(path, sizeof(path), "%s/%s", folder, file) >= (int)sizeof(path)) return -1;
    char* src = NULL;
    size_t len = 0;
    if (soulx_read_file(path, &src, &len) != 0) {
        soulx_log(SOULX_LOG_ERROR, "no script '%s' in %s", file, folder);
        return -1;
    }
    // replace the first "-- name:" line within the first lines, else add one at the top
    char header[160];
    snprintf(header, sizeof(header), "-- name: %s\n", name);
    size_t out_cap = len + strlen(header) + 1;
    char* out = malloc(out_cap);
    if (!out) { free(src); return -1; }
    size_t o = 0, i = 0;
    bool done = false;
    for (int lineno = 0; i < len; lineno++) {
        size_t end = i;
        while (end < len && src[end] != '\n') end++;
        size_t next = end < len ? end + 1 : end;
        bool is_name = false;
        if (!done && lineno < NAME_SCAN_LINES) {
            char tmp[512];
            size_t n = next - i < sizeof(tmp) - 1 ? next - i : sizeof(tmp) - 1;
            memcpy(tmp, src + i, n);
            tmp[n] = '\0';
            // reuse the parser on this single line
            char* p = tmp;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "--", 2) == 0) {
                p += 2;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '@') { p++; while (*p == ' ' || *p == '\t') p++; }
                is_name = strncasecmp(p, "name", 4) == 0 && (p[4] == ':' || p[4] == '=' || p[4] == ' ' || p[4] == '\t');
            }
        }
        if (is_name) {
            memcpy(out + o, header, strlen(header));
            o += strlen(header);
            done = true;
        } else {
            memcpy(out + o, src + i, next - i);
            o += next - i;
        }
        i = next;
    }
    if (!done) {                                    // no name line: put one first
        memmove(out + strlen(header), out, o);
        memcpy(out, header, strlen(header));
        o += strlen(header);
    }
    int rc = soulx_write_file_atomic(path, out, o);
    free(out);
    free(src);
    return rc;
}

int soulx_scripts_find(const char* instance, const char* query, char* out, size_t cap) {
    SoulXScript list[256];
    int n = soulx_scripts_list(instance, list, 256);
    if (n <= 0) return -1;
    if (n > 256) n = 256;
    int hit = -1, hits = 0;
    for (int pass = 0; pass < 2 && hits == 0; pass++) {      // exact display name first, then the file name
        for (int i = 0; i < n; i++) {
            const char* f = strrchr(list[i].file, '/');
            f = f ? f + 1 : list[i].file;
            char stem[PATH_MAX];
            snprintf(stem, sizeof(stem), "%s", f);
            size_t sl = strlen(stem);
            if (sl > 4) stem[sl - 4] = '\0';
            bool match = pass == 0 ? (list[i].name[0] && strcmp(list[i].name, query) == 0)
                                   : (strcmp(list[i].file, query) == 0 || strcmp(f, query) == 0 || strcmp(stem, query) == 0);
            if (match) { hit = i; hits++; }
        }
    }
    if (hits != 1) return hits == 0 ? -1 : -2;
    snprintf(out, cap, "%s", list[hit].path);
    return 0;
}
