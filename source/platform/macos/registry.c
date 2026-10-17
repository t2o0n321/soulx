/*
 * SoulX Android Emulator - macOS instance registry
 *
 * Persistent instances live in $SOULX_HOME/instances/<name>.conf (key=value
 * lines), the runtime state of a started instance in $SOULX_HOME/run/<name>.state
 * and cross-process serialisation uses an flock() on $SOULX_HOME/run/lock.
 * The highest id ever handed out is kept in instances/.last_id, so the id of a
 * deleted instance is never given to a new one.
 *
 * Every write to a state file happens under the lock. soulx_instance_running()
 * runs unlocked (list, status, scripts, API polling) but repairs or removes a
 * state file only when it can take the lock without waiting (or the calling
 * thread already holds it), and only if the file is still the one it judged.
 */

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <libproc.h>

#include "soulx_mac.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define REG_DEFAULT_IMAGE   "play"          // == soulx_runtime_default_image()->key
#define REG_FILE_MAX        (64 * 1024)     // conf / state files are a few hundred bytes
#define REG_LOCK_WAIT_MS    60000           // give up waiting for run/lock after 60 s
#define REG_MAX_ENTRIES     4096            // sanity bound for the instances/ directory
#define REG_LAST_ID_FILE    "instances/.last_id"
#define REG_WARNED_MAX      32              // conf files warned about once per process

// Per-thread state of the SoulX lock: the lock is re-entrant for the thread that holds it.
static _Thread_local int t_lock_fd = -1;    // private descriptor of the locked open file description
static _Thread_local int t_lock_depth = 0;

// Identity of a state file as it was read (a rewrite always creates a new inode: tmp + rename).
typedef struct {
    dev_t dev;
    ino_t ino;
} RegFileId;

typedef struct {
    const char* name;
    int cores;
    int ram_mb;
    int data_gb;
    int width;                              // natural (portrait) panel
    int height;
    int dpi;
} RegProfile;

// API 37 needs at least 4 GB of guest RAM, hence the same floor for "lite".
static const RegProfile k_profiles[] = {
    { "lite",        2, 4096,  6,  720, 1600, 320 },
    { "balanced",    4, 4096,  8, 1080, 2400, 420 },
    { "performance", 6, 6144, 16, 1080, 2400, 420 },
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static bool is_lower_alnum(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
}

static bool all_digits(const char* s) {
    if (!s || !*s) return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
    }
    return true;
}

// Parses a whole base-10 integer within [lo, hi].
static bool parse_long(const char* s, long lo, long hi, long* out) {
    if (!s || !*s) return false;
    errno = 0;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi) return false;
    *out = v;
    return true;
}

static bool parse_bool(const char* s, bool* out) {
    if (!strcmp(s, "1") || !strcasecmp(s, "true") || !strcasecmp(s, "yes")) {
        *out = true;
        return true;
    }
    if (!strcmp(s, "0") || !strcasecmp(s, "false") || !strcasecmp(s, "no")) {
        *out = false;
        return true;
    }
    return false;
}

// True when |s| is NUL-terminated within |cap| bytes, at most |cap - 1| long and
// every character belongs to |charset| (an empty string is accepted).
static bool field_ok(const char* s, size_t cap, const char* charset) {
    const char* nul = memchr(s, '\0', cap);
    if (!nul) return false;
    for (const char* p = s; p < nul; p++) {
        if (!strchr(charset, *p)) return false;
    }
    return true;
}

#define CHARSET_IMAGE "abcdefghijklmnopqrstuvwxyz0123456789_-"
#define CHARSET_DNS   "0123456789abcdefABCDEF.:,"
#define CHARSET_TIME  "0123456789TZ:.+-"

static bool gpu_known(const char* g) {
    return !strcmp(g, "metal") || !strcmp(g, "host") || !strcmp(g, "host-novulkan") || !strcmp(g, "software");
}

// An even console port SOULX_PORT_FIRST..SOULX_PORT_LAST (the adb port is the next one).
static bool console_port_valid(int port) {
    return port >= SOULX_PORT_FIRST && port <= SOULX_PORT_LAST && (port - SOULX_PORT_FIRST) % 2 == 0;
}

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = '\0';
    return s;
}

static int conf_path(const char* name, char* buf, size_t cap) {
    return soulx_path(buf, cap, "instances/%s.conf", name);
}

static int state_path(const char* name, char* buf, size_t cap) {
    return soulx_path(buf, cap, "run/%s.state", name);
}

// Reads a small regular file (symlinks, FIFOs and other non-regular files refused) through one
// descriptor, so *id_out describes exactly the file whose bytes were read. 0 ok, 1 missing, -1 unusable.
static int read_small_file(const char* path, char** data, RegFileId* id_out) {
    *data = NULL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct stat sb;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size < 0 || sb.st_size > REG_FILE_MAX) {
        close(fd);
        return -1;
    }
    size_t want = (size_t)sb.st_size;
    char* buf = malloc(want + 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    size_t len = 0;
    while (len < want) {
        ssize_t n = read(fd, buf + len, want - len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            free(buf);
            close(fd);
            return -1;
        }
        if (n == 0) break;                  // shrank meanwhile: use what is there
        len += (size_t)n;
    }
    close(fd);
    buf[len] = '\0';
    if (id_out) {
        id_out->dev = sb.st_dev;
        id_out->ino = sb.st_ino;
    }
    *data = buf;
    return 0;
}

// FNV-1a, for remembering paths cheaply.
static uint64_t hash_str(const char* s) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

// Problems with a conf file are reported once per process (list, status and a start scan the registry
// several times); a file that changed since (new mtime) is reported again.
static bool first_report(const char* path) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static struct { uint64_t hash; time_t mtime; } seen[REG_WARNED_MAX];
    static int next;
    struct stat sb;
    time_t mtime = lstat(path, &sb) == 0 ? sb.st_mtime : 0;
    uint64_t h = hash_str(path);
    bool first = true;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < REG_WARNED_MAX && first; i++) {
        if (seen[i].hash == h && seen[i].mtime == mtime) first = false;
    }
    if (first) {
        seen[next].hash = h;
        seen[next].mtime = mtime;
        next = (next + 1) % REG_WARNED_MAX;
    }
    pthread_mutex_unlock(&mu);
    return first;
}

// Calls |fn(key, value, ctx)| for every "key=value" line (comments and blank
// lines skipped, surrounding blanks and CR trimmed). Modifies |data|.
static void parse_kv(char* data, void (*fn)(const char* key, const char* value, void* ctx), void* ctx) {
    char* save = NULL;
    for (char* line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char* l = trim(line);
        if (*l == '\0' || *l == '#') continue;
        char* eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        fn(trim(l), trim(eq + 1), ctx);
    }
}

// ---------------------------------------------------------------------------
// Instance configuration
// ---------------------------------------------------------------------------

void soulx_conf_defaults(SoulXInstanceConf* c, const char* name) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    // A name that does not fit is left empty so later validation rejects it
    // instead of silently creating a truncated instance name.
    if (name && strlen(name) < sizeof(c->name)) {
        memcpy(c->name, name, strlen(name) + 1);
    }
    snprintf(c->image, sizeof(c->image), "%s", REG_DEFAULT_IMAGE);
    soulx_conf_apply_profile(c, "balanced");
    snprintf(c->gpu, sizeof(c->gpu), "%s", "host");
    c->dns[0] = '\0';
    c->stay_awake = true;
    c->lean = true;
    c->fps = 60;
    c->audio = true;
}

// Applies a profile; the panel keeps the orientation recorded in c->landscape.
int soulx_conf_apply_profile(SoulXInstanceConf* c, const char* profile) {
    if (!c || !profile) return -1;
    for (size_t i = 0; i < sizeof(k_profiles) / sizeof(k_profiles[0]); i++) {
        const RegProfile* p = &k_profiles[i];
        if (strcasecmp(p->name, profile) != 0) continue;
        c->cores = p->cores;
        c->ram_mb = p->ram_mb;
        c->data_gb = p->data_gb;
        c->dpi = p->dpi;
        c->width = c->landscape ? p->height : p->width;
        c->height = c->landscape ? p->width : p->height;
        return 0;
    }
    return -1;
}

const char* soulx_conf_profile_name(const SoulXInstanceConf* c) {
    if (!c) return "custom";
    for (size_t i = 0; i < sizeof(k_profiles) / sizeof(k_profiles[0]); i++) {
        const RegProfile* p = &k_profiles[i];
        int pw = c->landscape ? p->height : p->width;       // apply_profile swaps for landscape
        int ph = c->landscape ? p->width : p->height;
        if (c->cores == p->cores && c->ram_mb == p->ram_mb && c->data_gb == p->data_gb &&
            c->width == pw && c->height == ph && c->dpi == p->dpi) {
            return p->name;
        }
    }
    return "custom";
}

bool soulx_name_valid(const char* name) {
    if (!name || !is_lower_alnum(name[0])) return false;
    size_t n = strnlen(name, SOULX_NAME_MAX + 1);
    if (n > SOULX_NAME_MAX) return false;
    for (size_t i = 1; i < n; i++) {
        if (!is_lower_alnum(name[i]) && name[i] != '_' && name[i] != '-') return false;
    }
    return true;
}

typedef struct {
    SoulXInstanceConf* conf;
    bool have_id;
    const char* path;
} ConfParse;

static void conf_set_int(int* field, const char* key, const char* value, long lo, long hi, const char* path) {
    long v = 0;
    if (parse_long(value, lo, hi, &v)) {
        *field = (int)v;
    } else {
        soulx_log(SOULX_LOG_DEBUG, "%s: ignoring invalid %s=%s", path, key, value);
    }
}

static void conf_set_str(char* field, size_t cap, const char* key, const char* value,
                         const char* charset, const char* path) {
    if (strlen(value) < cap && field_ok(value, cap, charset)) {
        memcpy(field, value, strlen(value) + 1);
    } else {
        soulx_log(SOULX_LOG_DEBUG, "%s: ignoring invalid %s=%s", path, key, value);
    }
}

static void conf_kv(const char* key, const char* value, void* ctx) {
    ConfParse* cp = ctx;
    SoulXInstanceConf* c = cp->conf;
    bool b = false;

    if (!strcmp(key, "id")) {
        long v = 0;
        if (parse_long(value, 1, INT_MAX, &v)) {
            c->id = (int)v;
            cp->have_id = true;
        }
    } else if (!strcmp(key, "name")) {
        if (strcmp(value, c->name) != 0) {
            soulx_log(SOULX_LOG_DEBUG, "%s: name=%s differs from the file name; using '%s'", cp->path, value, c->name);
        }
    } else if (!strcmp(key, "image")) {
        conf_set_str(c->image, sizeof(c->image), key, value, CHARSET_IMAGE, cp->path);
    } else if (!strcmp(key, "cores")) {
        conf_set_int(&c->cores, key, value, 1, 64, cp->path);
    } else if (!strcmp(key, "ram_mb")) {
        conf_set_int(&c->ram_mb, key, value, 512, 1048576, cp->path);
    } else if (!strcmp(key, "data_gb")) {
        conf_set_int(&c->data_gb, key, value, 1, 4096, cp->path);
    } else if (!strcmp(key, "width")) {
        conf_set_int(&c->width, key, value, 1, 16384, cp->path);
    } else if (!strcmp(key, "height")) {
        conf_set_int(&c->height, key, value, 1, 16384, cp->path);
    } else if (!strcmp(key, "dpi")) {
        conf_set_int(&c->dpi, key, value, 1, 2000, cp->path);
    } else if (!strcmp(key, "landscape")) {
        if (parse_bool(value, &b)) c->landscape = b;
    } else if (!strcmp(key, "gpu")) {
        if (gpu_known(value)) {
            snprintf(c->gpu, sizeof(c->gpu), "%s", value);
        } else {
            soulx_log(SOULX_LOG_DEBUG, "%s: ignoring unknown gpu=%s", cp->path, value);
        }
    } else if (!strcmp(key, "dns")) {
        conf_set_str(c->dns, sizeof(c->dns), key, value, CHARSET_DNS, cp->path);
    } else if (!strcmp(key, "stay_awake")) {
        if (parse_bool(value, &b)) c->stay_awake = b;
    } else if (!strcmp(key, "lean")) {
        if (parse_bool(value, &b)) c->lean = b;
    } else if (!strcmp(key, "fps")) {
        conf_set_int(&c->fps, key, value, 1, 240, cp->path);
    } else if (!strcmp(key, "audio")) {
        if (parse_bool(value, &b)) c->audio = b;
    } else if (!strcmp(key, "created")) {
        conf_set_str(c->created, sizeof(c->created), key, value, CHARSET_TIME, cp->path);
    } else if (!strcmp(key, "last_port")) {
        long v = 0;
        if (parse_long(value, 0, SOULX_PORT_LAST, &v) && (v == 0 || console_port_valid((int)v))) {
            c->last_port = (int)v;
        } else {
            soulx_log(SOULX_LOG_DEBUG, "%s: ignoring invalid last_port=%s", cp->path, value);
        }
    }
    // unknown keys are ignored (forward compatibility)
}

int soulx_reg_load(const char* name, SoulXInstanceConf* out) {
    if (!out || !soulx_name_valid(name)) return -1;

    char path[PATH_MAX];
    if (conf_path(name, path, sizeof(path)) != 0) return -1;

    char* data = NULL;
    int rc = read_small_file(path, &data, NULL);
    if (rc != 0) {
        if (rc < 0) {
            soulx_log(first_report(path) ? SOULX_LOG_WARN : SOULX_LOG_DEBUG, "cannot read instance file %s", path);
        }
        return -1;
    }

    SoulXInstanceConf c;
    soulx_conf_defaults(&c, name);
    ConfParse cp = { &c, false, path };
    parse_kv(data, conf_kv, &cp);
    free(data);

    if (!cp.have_id) {
        soulx_log(first_report(path) ? SOULX_LOG_WARN : SOULX_LOG_DEBUG, "instance file %s has no valid id; ignoring it",
                 path);
        return -1;
    }
    *out = c;
    return 0;
}

// Highest id ever handed out (instances/.last_id), 0 when unknown.
static int last_id_read(void) {
    char path[PATH_MAX];
    if (soulx_path(path, sizeof(path), REG_LAST_ID_FILE) != 0) return 0;
    char* data = NULL;
    if (read_small_file(path, &data, NULL) != 0) return 0;
    long v = 0;
    char* line = data;
    if (strncmp(line, "last_id=", 8) == 0) line += 8;
    line[strcspn(line, "\r\n")] = '\0';
    if (!parse_long(trim(line), 1, INT_MAX, &v)) v = 0;
    free(data);
    return (int)v;
}

// Remembers |id| as handed out (best effort: without it ids are still unique among existing instances).
static void last_id_note(int id) {
    if (id <= last_id_read()) return;
    char path[PATH_MAX], body[32];
    int n = snprintf(body, sizeof(body), "last_id=%d\n", id);
    if (soulx_path(path, sizeof(path), REG_LAST_ID_FILE) != 0 || n < 0 || (size_t)n >= sizeof(body)) return;
    if (soulx_write_file_atomic(path, body, (size_t)n) != 0) {
        soulx_log(SOULX_LOG_DEBUG, "cannot record the last instance id in %s", path);
    }
}

int soulx_reg_save(const SoulXInstanceConf* c) {
    if (!c || c->id <= 0 || !soulx_name_valid(c->name)) return -1;
    if (!field_ok(c->image, sizeof(c->image), CHARSET_IMAGE) || !c->image[0] ||
        !field_ok(c->dns, sizeof(c->dns), CHARSET_DNS) ||
        !field_ok(c->created, sizeof(c->created), CHARSET_TIME) ||
        !memchr(c->gpu, '\0', sizeof(c->gpu)) || !gpu_known(c->gpu)) {
        soulx_log(SOULX_LOG_ERROR, "refusing to save instance '%s': invalid field value", c->name);
        return -1;
    }
    if (c->cores <= 0 || c->ram_mb <= 0 || c->data_gb <= 0 ||
        c->width <= 0 || c->height <= 0 || c->dpi <= 0) {
        soulx_log(SOULX_LOG_ERROR, "refusing to save instance '%s': invalid hardware values", c->name);
        return -1;
    }

    char dir[PATH_MAX], path[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "instances") != 0 || conf_path(c->name, path, sizeof(path)) != 0) {
        return -1;
    }
    if (soulx_mkdirs(dir) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", dir, strerror(errno));
        return -1;
    }

    char body[1024];
    int n = snprintf(body, sizeof(body),
        "# SoulX Android Emulator instance (managed by soulx)\n"
        "id=%d\n"
        "name=%s\n"
        "image=%s\n"
        "cores=%d\n"
        "ram_mb=%d\n"
        "data_gb=%d\n"
        "width=%d\n"
        "height=%d\n"
        "dpi=%d\n"
        "landscape=%d\n"
        "gpu=%s\n"
        "dns=%s\n"
        "stay_awake=%d\n"
        "lean=%d\n"
        "fps=%d\n"
        "audio=%d\n"
        "created=%s\n"
        "last_port=%d\n",
        c->id, c->name, c->image, c->cores, c->ram_mb, c->data_gb, c->width, c->height,
        c->dpi, c->landscape ? 1 : 0, c->gpu, c->dns, c->stay_awake ? 1 : 0, c->lean ? 1 : 0, c->fps,
        c->audio ? 1 : 0, c->created,
        console_port_valid(c->last_port) ? c->last_port : 0);
    if (n < 0 || (size_t)n >= sizeof(body)) return -1;

    if (soulx_write_file_atomic(path, body, (size_t)n) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot write %s", path);
        return -1;
    }
    last_id_note(c->id);
    return 0;
}

// 0 when the conf file is gone afterwards (removed or already absent).
int soulx_reg_delete(const char* name) {
    if (!soulx_name_valid(name)) return -1;
    char path[PATH_MAX];
    if (conf_path(name, path, sizeof(path)) != 0) return -1;
    if (unlink(path) != 0 && errno != ENOENT) {
        soulx_log(SOULX_LOG_ERROR, "cannot remove %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int conf_cmp(const void* a, const void* b) {
    const SoulXInstanceConf* x = a;
    const SoulXInstanceConf* y = b;
    if (x->id != y->id) return x->id < y->id ? -1 : 1;
    return strcmp(x->name, y->name);
}

// Loads every valid instance sorted by id. *items is malloc'd (NULL when empty).
static int reg_load_all(SoulXInstanceConf** items) {
    *items = NULL;
    char dir[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "instances") != 0) return -1;

    DIR* d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -1;

    SoulXInstanceConf* arr = NULL;
    size_t count = 0, cap = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && count < REG_MAX_ENTRIES) {
        size_t n = strlen(e->d_name);
        if (n <= 5 || strcmp(e->d_name + n - 5, ".conf") != 0) continue;
        size_t stem = n - 5;
        if (stem > SOULX_NAME_MAX) continue;
        char name[SOULX_NAME_MAX + 1];
        memcpy(name, e->d_name, stem);
        name[stem] = '\0';
        if (!soulx_name_valid(name)) continue;

        SoulXInstanceConf c;
        if (soulx_reg_load(name, &c) != 0) continue;
        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            SoulXInstanceConf* grown = realloc(arr, ncap * sizeof(*arr));
            if (!grown) {
                free(arr);
                closedir(d);
                return -1;
            }
            arr = grown;
            cap = ncap;
        }
        arr[count++] = c;
    }
    closedir(d);

    if (count > 1) qsort(arr, count, sizeof(*arr), conf_cmp);
    *items = arr;
    return (int)count;
}

// out == NULL: returns the total number of instances; else fills up to |max|.
int soulx_reg_list(SoulXInstanceConf* out, int max) {
    SoulXInstanceConf* all = NULL;
    int n = reg_load_all(&all);
    if (n < 0) return -1;
    if (!out) {
        free(all);
        return n;
    }
    int k = n < max ? n : (max > 0 ? max : 0);
    if (k > 0) memcpy(out, all, (size_t)k * sizeof(*out));
    free(all);
    return k;
}

int soulx_reg_find_by_id(int id, SoulXInstanceConf* out) {
    if (id <= 0) return -1;
    SoulXInstanceConf* all = NULL;
    int n = reg_load_all(&all);
    int rc = -1;
    for (int i = 0; i < n; i++) {
        if (all[i].id == id) {
            if (out) *out = all[i];
            rc = 0;
            break;
        }
    }
    free(all);
    return rc;
}

// max(existing ids, highest id ever handed out) + 1, so a deleted instance's id is not reused while
// API clients may still hold it; -1 when the registry cannot be read.
int soulx_reg_next_id(void) {
    SoulXInstanceConf* all = NULL;
    int n = reg_load_all(&all);
    if (n < 0) return -1;
    int max_id = last_id_read();
    for (int i = 0; i < n; i++) {
        if (all[i].id > max_id) max_id = all[i].id;
    }
    free(all);
    return max_id < INT_MAX ? max_id + 1 : -1;
}

int soulx_resolve_instance(const char* name_or_id, SoulXInstanceConf* out) {
    const char* key = name_or_id;
    if (!key || !*key) {
        const char* env = getenv("SOULX_INSTANCE");
        key = (env && *env) ? env : SOULX_DEFAULT_INSTANCE;
    }

    SoulXInstanceConf c;
    if (all_digits(key)) {
        long id = 0;
        if (parse_long(key, 1, INT_MAX, &id) && soulx_reg_find_by_id((int)id, &c) == 0) {
            if (out) *out = c;
            return 0;
        }
        // fall through: a hand-made instance may be named with digits only
    }
    if (soulx_name_valid(key) && soulx_reg_load(key, &c) == 0) {
        if (out) *out = c;
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

typedef struct {
    SoulXRunState* st;
    bool have_pid;
    bool have_port;
} StateParse;

static void state_kv(const char* key, const char* value, void* ctx) {
    StateParse* sp = ctx;
    long v = 0;
    bool b = false;
    if (!strcmp(key, "pid")) {
        if (parse_long(value, 1, INT_MAX, &v)) {
            sp->st->pid = (pid_t)v;
            sp->have_pid = true;
        }
    } else if (!strcmp(key, "port")) {
        if (parse_long(value, 1, 65534, &v)) {
            sp->st->port = (int)v;
            sp->have_port = true;
        }
    } else if (!strcmp(key, "headless")) {
        if (parse_bool(value, &b)) sp->st->headless = b;
    } else if (!strcmp(key, "started")) {
        if (strlen(value) < sizeof(sp->st->started) &&
            field_ok(value, sizeof(sp->st->started), CHARSET_TIME)) {
            memcpy(sp->st->started, value, strlen(value) + 1);
        }
    }
}

// 0 ok, 1 no state file, -1 unreadable or invalid; *id (optional) = the file that was read.
static int state_read(const char* name, SoulXRunState* out, RegFileId* id) {
    char path[PATH_MAX];
    if (state_path(name, path, sizeof(path)) != 0) return -1;

    char* data = NULL;
    int rc = read_small_file(path, &data, id);
    if (rc != 0) return rc;

    SoulXRunState st;
    memset(&st, 0, sizeof(st));
    StateParse sp = { &st, false, false };
    parse_kv(data, state_kv, &sp);
    free(data);

    if (!sp.have_pid || !sp.have_port) return -1;
    *out = st;
    return 0;
}

int soulx_state_load(const char* name, SoulXRunState* out) {
    if (!out || !soulx_name_valid(name)) return -1;
    return state_read(name, out, NULL) == 0 ? 0 : -1;
}

int soulx_state_save(const char* name, const SoulXRunState* st) {
    if (!st || !soulx_name_valid(name) || st->pid <= 0 || st->port <= 0 || st->port > 65534 ||
        !field_ok(st->started, sizeof(st->started), CHARSET_TIME)) {
        return -1;
    }
    char dir[PATH_MAX], path[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "run") != 0 || state_path(name, path, sizeof(path)) != 0) return -1;
    if (soulx_mkdirs(dir) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", dir, strerror(errno));
        return -1;
    }

    char body[256];
    int n = snprintf(body, sizeof(body), "pid=%d\nport=%d\nheadless=%d\nstarted=%s\n",
                     (int)st->pid, st->port, st->headless ? 1 : 0, st->started);
    if (n < 0 || (size_t)n >= sizeof(body)) return -1;
    if (soulx_write_file_atomic(path, body, (size_t)n) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot write %s", path);
        return -1;
    }
    return 0;
}

int soulx_state_clear(const char* name) {
    if (!soulx_name_valid(name)) return -1;
    char path[PATH_MAX];
    if (state_path(name, path, sizeof(path)) != 0) return -1;
    if (unlink(path) != 0 && errno != ENOENT) return -1;
    return 0;
}

void soulx_avd_name(const char* name, char* buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (snprintf(buf, cap, "soulx_%s", name ? name : "") >= (int)cap) {
        buf[0] = '\0';                      // never hand out a truncated AVD name
    }
}

// ---------------------------------------------------------------------------
// Engine process identification
//
// The recorded pid is trusted only while it is alive, lives inside
// $SOULX_HOME/sdk/emulator/ and (when its argv is readable) runs our AVD.
// When the recorded pid is gone - e.g. a launcher that handed over to a qemu
// child and exited - the engine is looked up by its command line instead
// (-avd soulx_<name> -port <P>) and the state file is repaired.
// ---------------------------------------------------------------------------

typedef enum {
    ARGS_UNKNOWN = 0,                       // argv unreadable or no -avd option
    ARGS_MATCH,
    ARGS_MISMATCH
} ArgsMatch;

static ArgsMatch engine_args_match(pid_t pid, const char* avd, int port) {
    int mib_max[2] = { CTL_KERN, KERN_ARGMAX };
    int argmax = 0;
    size_t sz = sizeof(argmax);
    if (sysctl(mib_max, 2, &argmax, &sz, NULL, 0) != 0 || argmax <= (int)sizeof(int)) return ARGS_UNKNOWN;

    char* buf = malloc((size_t)argmax);
    if (!buf) return ARGS_UNKNOWN;
    int mib[3] = { CTL_KERN, KERN_PROCARGS2, (int)pid };
    size_t size = (size_t)argmax;
    if (sysctl(mib, 3, buf, &size, NULL, 0) != 0 || size <= sizeof(int)) {
        free(buf);
        return ARGS_UNKNOWN;
    }

    // Layout: int argc, exec path, NUL padding, argv[0..argc-1], environment.
    int argc = 0;
    memcpy(&argc, buf, sizeof(argc));
    size_t off = sizeof(int);
    while (off < size && buf[off] != '\0') off++;
    while (off < size && buf[off] == '\0') off++;

    const char* prev = NULL;
    const char* avd_val = NULL;
    const char* port_val = NULL;
    for (int i = 0; i < argc && off < size; i++) {
        const char* arg = buf + off;
        const char* nul = memchr(arg, '\0', size - off);
        if (!nul) break;
        if (prev && !strcmp(prev, "-avd")) avd_val = arg;
        if (prev && !strcmp(prev, "-port")) port_val = arg;
        prev = arg;
        off = (size_t)(nul - buf) + 1;
    }

    ArgsMatch m = ARGS_UNKNOWN;
    if (avd_val) {
        m = strcmp(avd_val, avd) == 0 ? ARGS_MATCH : ARGS_MISMATCH;
        long p = 0;
        if (m == ARGS_MATCH && port > 0 && port_val &&
            (!parse_long(port_val, 1, 65535, &p) || p != port)) {
            m = ARGS_MISMATCH;
        }
    }
    free(buf);
    return m;
}

static bool pid_is_qemu(pid_t pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) return false;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strncmp(base, "qemu-system-", 12) == 0;
}

// Finds a live SoulX qemu process running |avd| on console |port|; 0 if none.
static pid_t reg_find_engine(const char* avd, int port) {
    int n = proc_listallpids(NULL, 0);
    if (n <= 0) return 0;
    size_t cap = (size_t)n + 64;
    pid_t* pids = calloc(cap, sizeof(pid_t));
    if (!pids) return 0;
    int got = proc_listallpids(pids, (int)(cap * sizeof(pid_t)));

    pid_t found = 0;
    for (int i = 0; i < got && !found; i++) {
        pid_t p = pids[i];
        if (p <= 1 || !pid_is_qemu(p) || !soulx_pid_is_engine(p)) continue;
        if (engine_args_match(p, avd, port) == ARGS_MATCH) found = p;
    }
    free(pids);
    return found;
}

static int lock_take(bool wait);

// The lock when it is free right now (or already held by this thread), else -1: soulx_instance_running()
// must neither block nor deadlock when it is called by a thread that holds the lock.
static int lock_try(void) {
    return lock_take(false);
}

// True when the state file of |name| is still the file identified by |id|. Call with the lock held:
// every writer of state files holds it, so the answer stays valid until the lock is released.
static bool state_file_is(const char* name, const RegFileId* id) {
    char path[PATH_MAX];
    struct stat sb;
    return state_path(name, path, sizeof(path)) == 0 && lstat(path, &sb) == 0 &&
           sb.st_dev == id->dev && sb.st_ino == id->ino;
}

// Removes the state file of |name| if it is still the one identified by |id| (a concurrent
// `soulx start` may have written a fresh one meanwhile). Skipped when the lock is busy.
static void state_remove_if_same(const char* name, const RegFileId* id, const char* why) {
    int lock = lock_try();
    if (lock < 0) return;                   // someone is writing states right now; a later call cleans up
    if (state_file_is(name, id)) {
        soulx_log(SOULX_LOG_DEBUG, "instance '%s': %s", name, why);
        soulx_state_clear(name);
    }
    soulx_lock_release(lock);
}

bool soulx_instance_running(const char* name, SoulXRunState* st_out) {
    if (st_out) memset(st_out, 0, sizeof(*st_out));
    if (!soulx_name_valid(name)) return false;

    SoulXRunState st;
    RegFileId id;
    memset(&id, 0, sizeof(id));
    int rc = state_read(name, &st, &id);
    if (rc == 1) return false;              // no state file: the normal "stopped" case
    if (rc != 0) {
        RegFileId bad;
        char path[PATH_MAX];
        struct stat sb;
        if (state_path(name, path, sizeof(path)) == 0 && lstat(path, &sb) == 0) {
            bad.dev = sb.st_dev;
            bad.ino = sb.st_ino;
            state_remove_if_same(name, &bad, "removing an unreadable state file");
        }
        return false;
    }

    char avd[SOULX_NAME_MAX + 8];
    soulx_avd_name(name, avd, sizeof(avd));

    if (soulx_pid_alive(st.pid) && soulx_pid_is_engine(st.pid) &&
        engine_args_match(st.pid, avd, st.port) != ARGS_MISMATCH) {
        if (st_out) *st_out = st;
        return true;
    }

    pid_t healed = reg_find_engine(avd, st.port);
    if (healed > 0) {
        SoulXRunState fixed = st;
        fixed.pid = healed;
        int lock = lock_try();              // persisting the repair is optional; the answer is not
        if (lock >= 0) {
            if (state_file_is(name, &id)) soulx_state_save(name, &fixed);
            soulx_lock_release(lock);
        }
        soulx_log(SOULX_LOG_DEBUG, "instance '%s': engine pid %d -> %d", name, (int)st.pid, (int)healed);
        if (st_out) *st_out = fixed;
        return true;
    }

    char why[64];
    snprintf(why, sizeof(why), "clearing stale state (pid %d)", (int)st.pid);
    state_remove_if_same(name, &id, why);
    return false;
}

// ---------------------------------------------------------------------------
// Global lock
// ---------------------------------------------------------------------------

// The lock is re-entrant per thread: when the calling thread already holds it, a nested acquire returns
// another descriptor of the same open file description (closing that one does not drop the flock) and
// only the release of the last outstanding descriptor unlocks. Other threads and processes still wait.
static int lock_take(bool wait) {
    if (t_lock_depth > 0) {
        int fd = fcntl(t_lock_fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (fd < 0) {
            soulx_log(SOULX_LOG_ERROR, "cannot lock the SoulX registry: %s", strerror(errno));
            return -1;
        }
        t_lock_depth++;
        return fd;
    }

    char dir[PATH_MAX], path[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "run") != 0 || soulx_path(path, sizeof(path), "run/lock") != 0) return -1;
    if (wait && soulx_mkdirs(dir) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", dir, strerror(errno));
        return -1;
    }

    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        if (wait) soulx_log(SOULX_LOG_ERROR, "cannot open lock file %s: %s", path, strerror(errno));
        return -1;
    }

    // Non-blocking attempts so Ctrl+C and the timeout are always honoured.
    double start = soulx_now_ms();
    bool told = false;
    for (;;) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            int keep = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);   // for nested acquires
            if (keep < 0) {
                soulx_log(SOULX_LOG_ERROR, "cannot lock %s: %s", path, strerror(errno));
                break;
            }
            t_lock_fd = keep;
            t_lock_depth = 1;
            return fd;
        }
        if (errno != EWOULDBLOCK && errno != EINTR) {
            soulx_log(SOULX_LOG_ERROR, "cannot lock %s: %s", path, strerror(errno));
            break;
        }
        if (!wait) break;
        double waited = soulx_now_ms() - start;
        if (!told && waited >= 1000) {
            soulx_log(SOULX_LOG_INFO, "waiting for another soulx command to finish...");
            told = true;
        }
        if (waited >= REG_LOCK_WAIT_MS) {
            soulx_log(SOULX_LOG_ERROR, "timed out waiting for the SoulX lock (%s)", path);
            break;
        }
        if (soulx_sleep_ms(50) != 0) break;      // interrupted
    }
    close(fd);                                  // also drops a lock taken just before a failed dup
    return -1;
}

int soulx_lock_acquire(void) {
    return lock_take(true);
}

void soulx_lock_release(int fd) {
    if (fd < 0) return;
    if (t_lock_depth > 1) {                     // nested: the flock stays with the outer descriptors
        t_lock_depth--;
        close(fd);
        return;
    }
    if (t_lock_depth == 1) {
        t_lock_depth = 0;
        if (t_lock_fd >= 0) close(t_lock_fd);
        t_lock_fd = -1;
    }
    flock(fd, LOCK_UN);
    close(fd);
}
