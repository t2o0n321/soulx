/*
 * SoulX Android Emulator - MCP server (`soulx mcp`): lets an AI app (any MCP client) see and
 * operate SoulX's instances through the Model Context Protocol.
 *
 * Security model (see docs/USAGE.md "AI control (MCP)"):
 *  - Off by default. The switch and every permission live in $SOULX_HOME/mcp.conf, set from the launcher or
 *    `soulx mcp config`. The server re-reads the file on every tool call, so turning it off takes effect at once.
 *  - stdio only: JSON-RPC on stdin/stdout of the process the AI client started. No network port is opened.
 *  - Only the instances allowed in the config are visible or usable.
 *  - Permissions: starting/stopping instances, installing apps, running the user's scripts, running Lua the AI
 *    writes. AI-written Lua runs in a sandbox (soulx_lua.c: no io/os file functions, no loading code, no console;
 *    files only inside the instance's scripts folder). The user's scripts run only from that folder.
 *  - Never exposed: creating, deleting or reconfiguring instances, setup.
 *  - Every tool call is appended to $SOULX_HOME/logs/mcp.log. Every tool has a time limit.
 *
 * The tools run SoulX's own CLI (this executable) for the actual work, so they share its checks and behaviour.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "soulx_mac.h"
#include "../../script/image.h"

#define MCP_PROTOCOL_DEFAULT  "2025-06-18"
#define MCP_LOG_MAX           (2u * 1024u * 1024u)
#define MCP_TEXT_MAX          20000          // text returned to the AI from scripts / OCR
#define MCP_LINE_MAX          (16u * 1024u * 1024u)
#define MCP_JOBS_MAX          16
#define MCP_MARK              "@@SOULX_MCP@@"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void soulx_mcp_config_defaults(SoulXMcpConfig* c) {
    memset(c, 0, sizeof(*c));
    c->enabled = false;
    snprintf(c->instances, sizeof(c->instances), "*");
    c->allow_start_stop = true;
    c->allow_install = false;
    c->allow_scripts = true;
    c->allow_lua = true;
}

static bool parse_flag(const char* v) { return v && (!strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes")); }

int soulx_mcp_config_load(SoulXMcpConfig* c) {
    soulx_mcp_config_defaults(c);
    char path[PATH_MAX];
    if (soulx_path(path, sizeof(path), "mcp.conf") != 0) return -1;
    char* data = NULL;
    size_t len = 0;
    if (soulx_read_file(path, &data, &len) != 0) return 0;          // no file: the defaults (off)
    char* save = NULL;
    for (char* line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '#') continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* k = line;
        char* v = eq + 1;
        v[strcspn(v, "\r")] = '\0';
        if (!strcmp(k, "enabled")) c->enabled = parse_flag(v);
        else if (!strcmp(k, "instances")) snprintf(c->instances, sizeof(c->instances), "%s", v);
        else if (!strcmp(k, "allow_start_stop")) c->allow_start_stop = parse_flag(v);
        else if (!strcmp(k, "allow_install")) c->allow_install = parse_flag(v);
        else if (!strcmp(k, "allow_scripts")) c->allow_scripts = parse_flag(v);
        else if (!strcmp(k, "allow_lua")) c->allow_lua = parse_flag(v);
    }
    free(data);
    return 0;
}

int soulx_mcp_config_save(const SoulXMcpConfig* c) {
    if (soulx_ensure_layout() != 0) return -1;
    char path[PATH_MAX];
    if (soulx_path(path, sizeof(path), "mcp.conf") != 0) return -1;
    char body[1024];
    int n = snprintf(body, sizeof(body),
        "# SoulX AI control (MCP) - managed by soulx (launcher or `soulx mcp config`)\n"
        "enabled=%d\ninstances=%s\nallow_start_stop=%d\nallow_install=%d\nallow_scripts=%d\nallow_lua=%d\n",
        c->enabled, c->instances, c->allow_start_stop, c->allow_install, c->allow_scripts, c->allow_lua);
    if (n < 0 || (size_t)n >= sizeof(body)) return -1;
    return soulx_write_file_atomic(path, body, (size_t)n);
}

bool soulx_mcp_instance_allowed(const SoulXMcpConfig* c, const char* name) {
    if (!name || !soulx_name_valid(name)) return false;
    if (!strcmp(c->instances, "*")) return true;
    size_t nl = strlen(name);
    for (const char* p = c->instances; *p; ) {
        while (*p == ',' || *p == ' ') p++;
        const char* e = p;
        while (*e && *e != ',') e++;
        size_t len = (size_t)(e - p);
        while (len && p[len - 1] == ' ') len--;
        if (len == nl && !strncmp(p, name, nl)) return true;
        p = e;
    }
    return false;
}

// A deleted instance leaves the AI's list of allowed instances (else a new one with its name would inherit it).
int soulx_mcp_forget_instance(const char* name) {
    SoulXMcpConfig c;
    if (!name || soulx_mcp_config_load(&c) != 0 || !strcmp(c.instances, "*")) return 0;
    if (!soulx_mcp_instance_allowed(&c, name)) return 0;
    char out[sizeof(c.instances)] = "";
    size_t nl = strlen(name), used = 0;
    for (const char* p = c.instances; *p; ) {
        while (*p == ',' || *p == ' ') p++;
        const char* e = p;
        while (*e && *e != ',') e++;
        size_t len = (size_t)(e - p);
        while (len && p[len - 1] == ' ') len--;
        if (len && !(len == nl && !strncmp(p, name, nl)) && used + len + 2 < sizeof(out)) {
            if (used) out[used++] = ',';
            memcpy(out + used, p, len);
            used += len;
            out[used] = 0;
        }
        p = e;
    }
    snprintf(c.instances, sizeof(c.instances), "%s", out);
    return soulx_mcp_config_save(&c);
}

// ---------------------------------------------------------------------------
// Audit log
// ---------------------------------------------------------------------------

void soulx_mcp_audit(const char* tool, const char* instance, const char* detail, bool ok) {
    char path[PATH_MAX];
    if (soulx_path(path, sizeof(path), "logs/mcp.log") != 0) return;
    struct stat st;
    if (stat(path, &st) == 0 && (size_t)st.st_size > MCP_LOG_MAX) {
        char old[PATH_MAX + 4];
        snprintf(old, sizeof(old), "%s.1", path);
        rename(path, old);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    char line[700];
    char d[400];
    snprintf(d, sizeof(d), "%s", detail ? detail : "");
    for (char* p = d; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    int n = snprintf(line, sizeof(line), "%s\t%s\t%s\t%s\t%s\n", stamp, ok ? "ok" : "error", tool ? tool : "-",
                     instance && instance[0] ? instance : "-", d);
    if (n > 0) (void)!write(fd, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
    close(fd);
}

// ---------------------------------------------------------------------------
// JSON (just what the protocol needs)
// ---------------------------------------------------------------------------

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct Json {
    JType type;
    bool b;
    double num;
    char* str;                 // J_STR value
    char* key;                 // member name when inside an object
    struct Json* child;        // first element / member
    struct Json* next;
} Json;

static void jfree(Json* j) {
    while (j) {
        Json* n = j->next;
        jfree(j->child);
        free(j->str);
        free(j->key);
        free(j);
        j = n;
    }
}

static void skip_ws(const char** p) { while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++; }

static void utf8_put(char** o, unsigned cp) {
    char* s = *o;
    if (cp < 0x80) *s++ = (char)cp;
    else if (cp < 0x800) { *s++ = (char)(0xC0 | (cp >> 6)); *s++ = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { *s++ = (char)(0xE0 | (cp >> 12)); *s++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *s++ = (char)(0x80 | (cp & 0x3F)); }
    else { *s++ = (char)(0xF0 | (cp >> 18)); *s++ = (char)(0x80 | ((cp >> 12) & 0x3F)); *s++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *s++ = (char)(0x80 | (cp & 0x3F)); }
    *o = s;
}

static int hex4(const char* p, unsigned* v) {
    *v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        unsigned d = (c >= '0' && c <= '9') ? (unsigned)(c - '0') : (c >= 'a' && c <= 'f') ? (unsigned)(c - 'a' + 10)
                   : (c >= 'A' && c <= 'F') ? (unsigned)(c - 'A' + 10) : 99;
        if (d > 15) return -1;
        *v = *v * 16 + d;
    }
    return 0;
}

static char* parse_string(const char** p) {
    if (**p != '"') return NULL;
    (*p)++;
    const char* s = *p;
    size_t cap = 16;
    for (const char* q = s; *q && *q != '"'; q++) { cap++; if (*q == '\\' && q[1]) q++; }
    char* out = malloc(cap * 2 + 8);
    if (!out) return NULL;
    char* o = out;
    while (**p && **p != '"') {
        char c = *(*p)++;
        if (c != '\\') { *o++ = c; continue; }
        char e = *(*p)++;
        switch (e) {
        case '"': *o++ = '"'; break;
        case '\\': *o++ = '\\'; break;
        case '/': *o++ = '/'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            unsigned cp;
            if (hex4(*p, &cp) != 0) { free(out); return NULL; }
            *p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && (*p)[0] == '\\' && (*p)[1] == 'u') {
                unsigned lo;
                if (hex4(*p + 2, &lo) == 0 && lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    *p += 6;
                }
            }
            utf8_put(&o, cp);
            break;
        }
        default: free(out); return NULL;
        }
    }
    if (**p != '"') { free(out); return NULL; }
    (*p)++;
    *o = '\0';
    return out;
}

static Json* parse_value(const char** p, int depth);

static Json* jnew(JType t) {
    Json* j = calloc(1, sizeof(Json));
    if (j) j->type = t;
    return j;
}

static Json* parse_value(const char** p, int depth) {
    if (depth > 64) return NULL;
    skip_ws(p);
    char c = **p;
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        Json* j = jnew(obj ? J_OBJ : J_ARR);
        if (!j) return NULL;
        (*p)++;
        skip_ws(p);
        Json** tail = &j->child;
        if (**p == (obj ? '}' : ']')) { (*p)++; return j; }
        for (;;) {
            char* key = NULL;
            if (obj) {
                skip_ws(p);
                key = parse_string(p);
                skip_ws(p);
                if (!key || **p != ':') { free(key); jfree(j); return NULL; }
                (*p)++;
            }
            Json* v = parse_value(p, depth + 1);
            if (!v) { free(key); jfree(j); return NULL; }
            v->key = key;
            *tail = v;
            tail = &v->next;
            skip_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == (obj ? '}' : ']')) { (*p)++; return j; }
            jfree(j);
            return NULL;
        }
    }
    if (c == '"') {
        Json* j = jnew(J_STR);
        if (!j) return NULL;
        j->str = parse_string(p);
        if (!j->str) { free(j); return NULL; }
        return j;
    }
    if (!strncmp(*p, "true", 4)) { *p += 4; Json* j = jnew(J_BOOL); if (j) j->b = true; return j; }
    if (!strncmp(*p, "false", 5)) { *p += 5; return jnew(J_BOOL); }
    if (!strncmp(*p, "null", 4)) { *p += 4; return jnew(J_NULL); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char* end = NULL;
        double d = strtod(*p, &end);
        if (end == *p) return NULL;
        *p = end;
        Json* j = jnew(J_NUM);
        if (j) j->num = d;
        return j;
    }
    return NULL;
}

static Json* jparse(const char* s) {
    const char* p = s;
    Json* j = parse_value(&p, 0);
    if (!j) return NULL;
    skip_ws(&p);
    if (*p) { jfree(j); return NULL; }
    return j;
}

static Json* jget(const Json* o, const char* key) {
    if (!o || o->type != J_OBJ) return NULL;
    for (Json* m = o->child; m; m = m->next) if (m->key && !strcmp(m->key, key)) return m;
    return NULL;
}

static const char* jstr(const Json* o, const char* key) {
    Json* v = jget(o, key);
    return v && v->type == J_STR ? v->str : NULL;
}

static bool jnum(const Json* o, const char* key, double* out) {
    Json* v = jget(o, key);
    if (v && v->type == J_NUM) { *out = v->num; return true; }
    if (v && v->type == J_STR && v->str[0]) {          // "100" is accepted too
        char* end = NULL;
        double d = strtod(v->str, &end);
        if (end && !*end) { *out = d; return true; }
    }
    return false;
}

static bool jbool(const Json* o, const char* key, bool dflt) {
    Json* v = jget(o, key);
    if (!v) return dflt;
    if (v->type == J_BOOL) return v->b;
    if (v->type == J_STR) return parse_flag(v->str);
    return dflt;
}

// ---------------------------------------------------------------------------
// Output buffer
// ---------------------------------------------------------------------------

typedef struct { char* s; size_t len, cap; } Buf;

static void bput(Buf* b, const char* s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char* ns = realloc(b->s, cap);
        if (!ns) return;
        b->s = ns;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void bs(Buf* b, const char* s) { bput(b, s, strlen(s)); }

static void bf(Buf* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void bf(Buf* b, const char* fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) bput(b, tmp, (size_t)(n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1));
}

static void bjs(Buf* b, const char* s) {               // a JSON string literal
    bs(b, "\"");
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"': bs(b, "\\\""); break;
        case '\\': bs(b, "\\\\"); break;
        case '\n': bs(b, "\\n"); break;
        case '\r': bs(b, "\\r"); break;
        case '\t': bs(b, "\\t"); break;
        default:
            if (*p < 0x20) bf(b, "\\u%04x", *p);
            else bput(b, (const char*)p, 1);
        }
    }
    bs(b, "\"");
}

static void bjson_raw(Buf* b, const Json* j) {           // re-serialise a parsed value (the request id)
    if (!j) { bs(b, "null"); return; }
    switch (j->type) {
    case J_NULL: bs(b, "null"); break;
    case J_BOOL: bs(b, j->b ? "true" : "false"); break;
    case J_NUM:
        if (j->num == floor(j->num) && fabs(j->num) < 1e15) bf(b, "%.0f", j->num);
        else bf(b, "%.17g", j->num);
        break;
    case J_STR: bjs(b, j->str); break;
    default: bs(b, "null");
    }
}

static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void bbase64(Buf* b, const unsigned char* d, size_t n) {
    char q[4];
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16 | (i + 1 < n ? (unsigned)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        q[0] = k_b64[(v >> 18) & 63];
        q[1] = k_b64[(v >> 12) & 63];
        q[2] = i + 1 < n ? k_b64[(v >> 6) & 63] : '=';
        q[3] = i + 2 < n ? k_b64[v & 63] : '=';
        bput(b, q, 4);
    }
}

// ---------------------------------------------------------------------------
// Running soulx (this executable) for the tools
// ---------------------------------------------------------------------------

static char g_self[PATH_MAX];

typedef struct {
    int status;
    char* out;      // stdout + stderr, without SoulX's own "running ..." chatter
} RunOut;

static void runout_free(RunOut* r) { free(r->out); r->out = NULL; }

// Runs `soulx args...` with a time limit. sandbox_dir != NULL runs Lua in the sandbox (see soulx_lua.c).
static int run_self(const char* const* args, int timeout_ms, const char* sandbox_dir, RunOut* ro) {
    memset(ro, 0, sizeof(*ro));
    const char* argv[48];
    int n = 0;
    argv[n++] = g_self;
    for (int i = 0; args[i] && n < 46; i++) argv[n++] = args[i];
    argv[n] = NULL;
    SoulXCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.argv = argv;
    cmd.timeout_ms = timeout_ms;
    cmd.max_output = 64u * 1024u * 1024u;
    if (sandbox_dir) setenv("SOULX_LUA_SANDBOX_DIR", sandbox_dir, 1);
    else unsetenv("SOULX_LUA_SANDBOX_DIR");
    setenv("SOULX_FROM_MCP", "1", 1);
    SoulXCmdResult res;
    memset(&res, 0, sizeof(res));
    int st = soulx_run(&cmd, &res);
    unsetenv("SOULX_LUA_SANDBOX_DIR");
    Buf b = { 0 };
    if (res.out) bput(&b, res.out, strlen(res.out));
    if (res.err) {
        // keep the messages, drop SoulX's progress lines that mean nothing to the caller
        char* save = NULL;
        for (char* line = strtok_r(res.err, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (!strncmp(line, "soulx: running ", 15) || !strncmp(line, "script finished in ", 19)) continue;
            bs(&b, line);
            bs(&b, "\n");
        }
    }
    ro->status = res.timed_out ? -2 : st;
    ro->out = b.s ? b.s : strdup("");
    soulx_cmd_result_free(&res);
    return ro->status;
}

// ---------------------------------------------------------------------------
// Tool results
// ---------------------------------------------------------------------------

typedef struct {
    Buf content;        // JSON array items
    bool error;
    char audit[300];
} Result;

static void res_text(Result* r, const char* text) {
    if (r->content.len) bs(&r->content, ",");
    bs(&r->content, "{\"type\":\"text\",\"text\":");
    char* t = NULL;
    if (text && strlen(text) > MCP_TEXT_MAX) {        // keep the end: the latest output matters most
        size_t n = strlen(text);
        t = malloc(MCP_TEXT_MAX + 64);
        if (t) snprintf(t, MCP_TEXT_MAX + 64, "[... %zu earlier characters cut]\n%s", n - MCP_TEXT_MAX, text + n - MCP_TEXT_MAX);
    }
    bjs(&r->content, t ? t : text);
    free(t);
    bs(&r->content, "}");
}

static void res_textf(Result* r, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void res_textf(Result* r, const char* fmt, ...) {
    char tmp[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    res_text(r, tmp);
}

static void res_error(Result* r, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void res_error(Result* r, const char* fmt, ...) {
    char tmp[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    r->error = true;
    res_text(r, tmp);
}

// The outcome of a CLI call as the tool's result.
static void res_run(Result* r, const RunOut* ro, const char* ok_text) {
    if (ro->status == -2) { res_error(r, "timed out\n%s", ro->out ? ro->out : ""); return; }
    if (ro->status != 0) {
        res_error(r, "%s", ro->out && ro->out[0] ? ro->out : "failed");
        return;
    }
    res_text(r, ro->out && ro->out[0] ? ro->out : ok_text);
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

typedef struct {
    const SoulXMcpConfig* cfg;
    const Json* args;
    char instance[SOULX_NAME_MAX + 1];
} Call;

typedef struct {
    pid_t pid;
    char instance[SOULX_NAME_MAX + 1];
    char script[256];
    char log[PATH_MAX];
    int status;          // -1 while running
    time_t started;
} Job;

static Job g_jobs[MCP_JOBS_MAX];

static void reap_jobs(void) {
    for (int i = 0; i < MCP_JOBS_MAX; i++) {
        if (g_jobs[i].pid <= 0 || g_jobs[i].status != -1) continue;
        int st = 0;
        pid_t w = waitpid(g_jobs[i].pid, &st, WNOHANG);
        if (w == g_jobs[i].pid) g_jobs[i].status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        else if (w < 0 && kill(g_jobs[i].pid, 0) != 0) g_jobs[i].status = 255;
    }
}

static char* read_tail(const char* path, size_t max) {
    char* data = NULL;
    size_t len = 0;
    if (soulx_read_file(path, &data, &len) != 0) return strdup("");
    if (len > max) {
        char* t = strdup(data + len - max);
        free(data);
        return t;
    }
    return data;
}

// Resolves and checks the target instance ("instance" argument, else $SOULX_INSTANCE, else "default").
static bool pick_instance(Call* c, Result* r) {
    const char* name = jstr(c->args, "instance");
    if (!name || !name[0]) name = getenv("SOULX_INSTANCE");
    if (!name || !name[0]) name = "default";
    SoulXInstanceConf conf;
    if (!soulx_name_valid(name) || soulx_reg_load(name, &conf) != 0) {
        res_error(r, "no instance '%s' (see list_instances)", name);
        return false;
    }
    if (!soulx_mcp_instance_allowed(c->cfg, conf.name)) {
        res_error(r, "instance '%s' is not allowed for AI control (SoulX launcher > AI control)", conf.name);
        return false;
    }
    snprintf(c->instance, sizeof(c->instance), "%s", conf.name);
    return true;
}

static bool num_arg(const Call* c, Result* r, const char* key, double lo, double hi, double dflt, bool required,
                    double* out) {
    double v;
    if (!jnum(c->args, key, &v)) {
        if (required) { res_error(r, "missing number '%s'", key); return false; }
        *out = dflt;
        return true;
    }
    if (!(v >= lo && v <= hi)) { res_error(r, "'%s' must be between %g and %g", key, lo, hi); return false; }
    *out = v;
    return true;
}

static bool scripts_folder(const Call* c, Result* r, char* out, size_t cap) {
    if (soulx_scripts_dir(c->instance, out, cap, true) != 0) {
        res_error(r, "cannot prepare the scripts folder of '%s'", c->instance);
        return false;
    }
    return true;
}

// Runs a Lua snippet on the instance in the sandbox; its "@@SOULX_MCP@@ ..." line is the tool's data.
static int run_sandboxed(Call* c, Result* r, const char* code, const char* const* extra_args, int timeout_ms,
                         RunOut* ro) {
    char box[PATH_MAX], dir[PATH_MAX], file[PATH_MAX];
    if (!scripts_folder(c, r, box, sizeof(box))) return -1;
    if (soulx_path(dir, sizeof(dir), "tmp/mcp") != 0 || soulx_mkdirs(dir) != 0) {
        res_error(r, "cannot prepare a temporary folder");
        return -1;
    }
    chmod(dir, 0700);
    snprintf(file, sizeof(file), "%s/snippet-%d-%ld.lua", dir, (int)getpid(), (long)(soulx_now_ms() * 1000.0));
    if (soulx_write_file_atomic(file, code, strlen(code)) != 0) {
        res_error(r, "cannot write the snippet");
        return -1;
    }
    const char* args[40] = { "run", "-i", c->instance, file };
    int n = 4;
    for (int i = 0; extra_args && extra_args[i] && n < 38; i++) args[n++] = extra_args[i];
    args[n] = NULL;
    int st = run_self(args, timeout_ms, box, ro);
    unlink(file);
    return st;
}

// The data line a snippet printed (after the marker), or NULL.
static const char* marker_data(const char* out) {
    const char* m = out ? strstr(out, MCP_MARK) : NULL;
    if (!m) return NULL;
    m += strlen(MCP_MARK);
    while (*m == ' ') m++;
    return m;
}

static void t_list_instances(Call* c, Result* r) {
    SoulXInstanceConf list[64];
    int n = soulx_reg_list(list, 64);
    Buf b = { 0 };
    int shown = 0;
    for (int i = 0; i < n && i < 64; i++) {
        if (!soulx_mcp_instance_allowed(c->cfg, list[i].name)) continue;
        SoulXRunState st;
        bool running = soulx_instance_running(list[i].name, &st);
        bf(&b, "%s: %s, %dx%d%s, %d CPUs, %d MB, gpu %s, fps %d%s\n", list[i].name, running ? "running" : "stopped",
           list[i].width, list[i].height, list[i].landscape ? " (landscape)" : "", list[i].cores, list[i].ram_mb,
           list[i].gpu, list[i].fps, running && st.headless ? ", no window" : "");
        shown++;
    }
    if (!shown) bs(&b, "No instances are available for AI control.");
    res_text(r, b.s);
    free(b.s);
}

static void t_start_instance(Call* c, Result* r) {
    if (!c->cfg->allow_start_stop) { res_error(r, "starting instances is not allowed (SoulX launcher > AI control)"); return; }
    if (!pick_instance(c, r)) return;
    bool window = jbool(c->args, "window", true);
    const char* args[] = { "start", c->instance, window ? NULL : "--headless", NULL };
    RunOut ro;
    run_self(args, 420000, NULL, &ro);
    res_run(r, &ro, "started");
    runout_free(&ro);
}

static void t_stop_instance(Call* c, Result* r) {
    if (!c->cfg->allow_start_stop) { res_error(r, "stopping instances is not allowed (SoulX launcher > AI control)"); return; }
    if (!pick_instance(c, r)) return;
    const char* args[] = { "stop", c->instance, NULL };
    RunOut ro;
    run_self(args, 120000, NULL, &ro);
    res_run(r, &ro, "stopped");
    runout_free(&ro);
}

static void t_instance_status(Call* c, Result* r) {
    if (!pick_instance(c, r)) return;
    const char* a1[] = { "status", c->instance, NULL };
    const char* a2[] = { "stats", c->instance, "--interval", "1000", NULL };
    RunOut s1, s2;
    run_self(a1, 30000, NULL, &s1);
    run_self(a2, 30000, NULL, &s2);
    Buf b = { 0 };
    bs(&b, s1.out ? s1.out : "");
    if (s2.status == 0 && s2.out) { bs(&b, "\n"); bs(&b, s2.out); }
    if (s1.status != 0) { r->error = true; }
    res_text(r, b.s ? b.s : "");
    free(b.s);
    runout_free(&s1);
    runout_free(&s2);
}

static void t_screenshot(Call* c, Result* r) {
    if (!pick_instance(c, r)) return;
    double max_size, quality;
    if (!num_arg(c, r, "max_size", 200, 4000, 1024, false, &max_size)) return;
    if (!num_arg(c, r, "quality", 30, 95, 75, false, &quality)) return;
    const char* format = jstr(c->args, "format");
    bool png = format && !strcasecmp(format, "png");
    char dir[PATH_MAX], file[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "tmp/mcp") != 0 || soulx_mkdirs(dir) != 0) { res_error(r, "no temporary folder"); return; }
    chmod(dir, 0700);
    snprintf(file, sizeof(file), "%s/shot-%d.png", dir, (int)getpid());
    const char* args[] = { "screenshot", "-i", c->instance, file, NULL };
    RunOut ro;
    run_self(args, 30000, NULL, &ro);
    if (ro.status != 0) { res_run(r, &ro, ""); runout_free(&ro); unlink(file); return; }
    runout_free(&ro);
    SoulXImage img = { 0 }, crop = { 0 }, scaled = { 0 };
    if (soulx_image_load_png(file, &img) != 0) { res_error(r, "cannot read the screenshot: %s", soulx_image_error()); unlink(file); return; }
    unlink(file);
    int sw = img.w, sh = img.h;
    int rx = 0, ry = 0, rw = sw, rh = sh;
    Json* reg = jget(c->args, "region");
    if (reg && reg->type == J_OBJ) {
        double x, y, w, h;
        if (!jnum(reg, "x", &x) || !jnum(reg, "y", &y) || !jnum(reg, "w", &w) || !jnum(reg, "h", &h)) {
            res_error(r, "region needs x, y, w and h (screen pixels)");
            free(img.rgba);
            return;
        }
        rx = (int)fmax(0, fmin(x, sw - 1));
        ry = (int)fmax(0, fmin(y, sh - 1));
        rw = (int)fmax(1, fmin(w, sw - rx));
        rh = (int)fmax(1, fmin(h, sh - ry));
    }
    const SoulXImage* cur = &img;
    if (rx || ry || rw != sw || rh != sh) {
        if (soulx_image_crop(&img, rx, ry, rw, rh, &crop) != 0) { res_error(r, "cannot crop: %s", soulx_image_error()); free(img.rgba); return; }
        cur = &crop;
    }
    double scale = 1.0;
    int longest = cur->w > cur->h ? cur->w : cur->h;
    if (longest > max_size) {
        scale = max_size / longest;
        if (soulx_image_scale(cur, scale, &scaled) != 0) { res_error(r, "cannot scale: %s", soulx_image_error()); free(img.rgba); free(crop.rgba); return; }
        cur = &scaled;
    }
    unsigned char* enc = NULL;
    size_t enc_len = 0;
    if (soulx_image_encode(cur, !png, (int)quality, &enc, &enc_len) != 0) {
        res_error(r, "cannot encode: %s", soulx_image_error());
    } else {
        if (r->content.len) bs(&r->content, ",");
        bs(&r->content, "{\"type\":\"image\",\"mimeType\":");
        bjs(&r->content, png ? "image/png" : "image/jpeg");
        bs(&r->content, ",\"data\":\"");
        bbase64(&r->content, enc, enc_len);
        bs(&r->content, "\"}");
        if (scale == 1.0 && rx == 0 && ry == 0) {
            res_textf(r, "Screen of '%s': %dx%d pixels. Image pixels are screen pixels (use them for tap, swipe ...).",
                      c->instance, sw, sh);
        } else {
            res_textf(r, "Screen of '%s': %dx%d pixels. This image shows the region x=%d y=%d w=%d h=%d at scale %.4f: "
                         "screen_x = %d + image_x / %.4f, screen_y = %d + image_y / %.4f. Tap, swipe and the other "
                         "tools take screen pixels.", c->instance, sw, sh, rx, ry, rw, rh, scale, rx, scale, ry, scale);
        }
    }
    free(enc);
    free(img.rgba);
    free(crop.rgba);
    free(scaled.rgba);
}

static bool region_args(const Call* c, Result* r, char out[4][16]) {
    Json* reg = jget(c->args, "region");
    for (int i = 0; i < 4; i++) snprintf(out[i], 16, "-1");
    if (!reg) return true;
    double v[4];
    if (reg->type != J_OBJ || !jnum(reg, "x", &v[0]) || !jnum(reg, "y", &v[1]) || !jnum(reg, "w", &v[2]) ||
        !jnum(reg, "h", &v[3]) || v[2] <= 0 || v[3] <= 0) {
        res_error(r, "region needs x, y, w and h (screen pixels)");
        return false;
    }
    for (int i = 0; i < 4; i++) snprintf(out[i], 16, "%d", (int)v[i]);
    return true;
}

static void t_read_text(Call* c, Result* r) {
    if (!pick_instance(c, r)) return;
    char reg[4][16];
    if (!region_args(c, r, reg)) return;
    static const char code[] =
        "local x, y, w, h = tonumber(args[1]), tonumber(args[2]), tonumber(args[3]), tonumber(args[4])\n"
        "local t = (w > 0) and read_text({x, y, w, h}) or read_text()\n"
        "log('" MCP_MARK " ' .. (t or ''):gsub('\\\\', '\\\\\\\\'):gsub('\\n', '\\\\n'))\n";
    const char* a[] = { reg[0], reg[1], reg[2], reg[3], NULL };
    RunOut ro;
    if (run_sandboxed(c, r, code, a, 60000, &ro) < 0 && !ro.out) return;
    const char* d = marker_data(ro.out);
    if (ro.status != 0 || !d) { res_run(r, &ro, ""); runout_free(&ro); return; }
    Buf b = { 0 };
    for (const char* p = d; *p && *p != '\n'; p++) {
        if (p[0] == '\\' && p[1] == 'n') { bs(&b, "\n"); p++; }
        else if (p[0] == '\\' && p[1] == '\\') { bs(&b, "\\"); p++; }
        else bput(&b, p, 1);
    }
    res_text(r, b.len ? b.s : "(no text found)");
    free(b.s);
    runout_free(&ro);
}

static void t_find_image(Call* c, Result* r) {
    if (!pick_instance(c, r)) return;
    const char* tmpl = jstr(c->args, "template");
    if (!tmpl || !tmpl[0]) { res_error(r, "missing 'template' (an image in the instance's scripts folder)"); return; }
    double threshold;
    if (!num_arg(c, r, "threshold", 0.1, 1.0, 0.88, false, &threshold)) return;
    char reg[4][16];
    if (!region_args(c, r, reg)) return;
    char th[32];
    snprintf(th, sizeof(th), "%.3f", threshold);
    bool all = jbool(c->args, "all", false);
    static const char code[] =
        "local o = { threshold = tonumber(args[2]) }\n"
        "if tonumber(args[5]) > 0 then o.region = { tonumber(args[3]), tonumber(args[4]), tonumber(args[5]), tonumber(args[6]) } end\n"
        "local out = {}\n"
        "if args[7] == '1' then\n"
        "  for _, m in ipairs(find_all(args[1], o)) do out[#out + 1] = string.format('%d,%d,%.3f', m.x, m.y, m.score) end\n"
        "else\n"
        "  local x, y, s = find(args[1], o)\n"
        "  if x then out[1] = string.format('%d,%d,%.3f', x, y, s) end\n"
        "end\n"
        "log('" MCP_MARK " ' .. table.concat(out, ';'))\n";
    const char* a[] = { tmpl, th, reg[0], reg[1], reg[2], reg[3], all ? "1" : "0", NULL };
    RunOut ro;
    if (run_sandboxed(c, r, code, a, 60000, &ro) < 0 && !ro.out) return;
    const char* d = marker_data(ro.out);
    if (ro.status != 0 || !d) { res_run(r, &ro, ""); runout_free(&ro); return; }
    Buf b = { 0 };
    int count = 0;
    char line[8192];
    snprintf(line, sizeof(line), "%.*s", (int)strcspn(d, "\n"), d);
    char* save = NULL;
    for (char* m = strtok_r(line, ";", &save); m; m = strtok_r(NULL, ";", &save)) {
        int x, y;
        double s;
        if (sscanf(m, "%d,%d,%lf", &x, &y, &s) == 3) { bf(&b, "match at x=%d y=%d (score %.3f)\n", x, y, s); count++; }
    }
    res_text(r, count ? b.s : "not found");
    free(b.s);
    runout_free(&ro);
}

static void t_pixel_color(Call* c, Result* r) {
    if (!pick_instance(c, r)) return;
    Json* pts = jget(c->args, "points");
    if (!pts || pts->type != J_ARR || !pts->child) { res_error(r, "'points' must be a list like [[x, y], ...]"); return; }
    Buf list = { 0 };
    int n = 0;
    for (Json* p = pts->child; p && n < 64; p = p->next, n++) {
        if (p->type != J_ARR || !p->child || !p->child->next || p->child->type != J_NUM || p->child->next->type != J_NUM) {
            res_error(r, "every point is [x, y]");
            free(list.s);
            return;
        }
        bf(&list, "%s%d,%d", n ? ";" : "", (int)p->child->num, (int)p->child->next->num);
    }
    static const char code[] =
        "local out = {}\n"
        "for x, y in args[1]:gmatch('(%-?%d+),(%-?%d+)') do\n"
        "  out[#out + 1] = string.format('%s,%s,%06X', x, y, color(tonumber(x), tonumber(y)))\n"
        "end\n"
        "log('" MCP_MARK " ' .. table.concat(out, ';'))\n";
    const char* a[] = { list.s, NULL };
    RunOut ro;
    int st = run_sandboxed(c, r, code, a, 30000, &ro);
    free(list.s);
    if (st < 0 && !ro.out) return;
    const char* d = marker_data(ro.out);
    if (ro.status != 0 || !d) { res_run(r, &ro, ""); runout_free(&ro); return; }
    Buf b = { 0 };
    char line[8192];
    snprintf(line, sizeof(line), "%.*s", (int)strcspn(d, "\n"), d);
    char* save = NULL;
    for (char* m = strtok_r(line, ";", &save); m; m = strtok_r(NULL, ";", &save)) {
        int x, y;
        char hex[16];
        if (sscanf(m, "%d,%d,%15s", &x, &y, hex) == 3) bf(&b, "(%d, %d): #%s\n", x, y, hex);
    }
    res_text(r, b.s ? b.s : "");
    free(b.s);
    runout_free(&ro);
}

static void t_tap(Call* c, Result* r) {
    if (!pick_instance(c, r)) return;
    double x, y;
    if (!num_arg(c, r, "x", 0, 65535, 0, true, &x) || !num_arg(c, r, "y", 0, 65535, 0, true, &y)) return;
    char xs[16], ys[16];
    snprintf(xs, sizeof(xs), "%d", (int)x);
    snprintf(ys, sizeof(ys), "%d", (int)y);
    const char* args[] = { "tap", "-i", c->instance, xs, ys, NULL };
    RunOut ro;
    run_self(args, 30000, NULL, &ro);
    res_run(r, &ro, "tapped");
    runout_free(&ro);
}

static void t_swipe(Call* c, Result* r) {
    if (!pick_instance(c, r)) return;
    double v[5];
    const char* keys[] = { "x1", "y1", "x2", "y2" };
    for (int i = 0; i < 4; i++) if (!num_arg(c, r, keys[i], 0, 65535, 0, true, &v[i])) return;
    if (!num_arg(c, r, "ms", 1, 60000, 300, false, &v[4])) return;
    char s[5][16];
    for (int i = 0; i < 5; i++) snprintf(s[i], 16, "%d", (int)v[i]);
    const char* args[] = { "swipe", "-i", c->instance, s[0], s[1], s[2], s[3], s[4], NULL };
    RunOut ro;
    run_self(args, 90000, NULL, &ro);
    res_run(r, &ro, "swiped");
    runout_free(&ro);
}

static void t_long_press(Call* c, Result* r) {
    if (!pick_instance(c, r)) return;
    double x, y, ms;
    if (!num_arg(c, r, "x", 0, 65535, 0, true, &x) || !num_arg(c, r, "y", 0, 65535, 0, true, &y) ||
        !num_arg(c, r, "ms", 50, 30000, 800, false, &ms)) return;
    char s[3][16];
    snprintf(s[0], 16, "%d", (int)x);
    snprintf(s[1], 16, "%d", (int)y);
    snprintf(s[2], 16, "%d", (int)ms);
    static const char code[] = "long_press(tonumber(args[1]), tonumber(args[2]), tonumber(args[3]))\n";
    const char* a[] = { s[0], s[1], s[2], NULL };
    RunOut ro;
    if (run_sandboxed(c, r, code, a, 60000, &ro) < 0 && !ro.out) return;
    res_run(r, &ro, "long-pressed");
    runout_free(&ro);
}

static void t_drag_path(Call* c, Result* r) {
    if (!pick_instance(c, r)) return;
    Json* pts = jget(c->args, "points");
    if (!pts || pts->type != J_ARR || !pts->child || !pts->child->next) { res_error(r, "'points' needs at least 2 points [[x, y], ...]"); return; }
    double step, hold, maxpx;
    if (!num_arg(c, r, "step_ms", 1, 1000, 20, false, &step) || !num_arg(c, r, "hold_ms", 0, 5000, 150, false, &hold) ||
        !num_arg(c, r, "max_step_px", 0, 2000, 45, false, &maxpx)) return;
    Buf list = { 0 };
    int n = 0;
    for (Json* p = pts->child; p && n < 500; p = p->next, n++) {
        if (p->type != J_ARR || !p->child || !p->child->next || p->child->type != J_NUM || p->child->next->type != J_NUM) {
            res_error(r, "every point is [x, y]");
            free(list.s);
            return;
        }
        bf(&list, "%s%d,%d", n ? ";" : "", (int)p->child->num, (int)p->child->next->num);
    }
    char s[3][16];
    snprintf(s[0], 16, "%d", (int)step);
    snprintf(s[1], 16, "%d", (int)hold);
    snprintf(s[2], 16, "%d", (int)maxpx);
    static const char code[] =
        "local pts = {}\n"
        "for x, y in args[1]:gmatch('(%-?%d+),(%-?%d+)') do pts[#pts + 1] = { tonumber(x), tonumber(y) } end\n"
        "local mp = tonumber(args[4])\n"
        "drag_path(pts, { step_ms = tonumber(args[2]), hold_ms = tonumber(args[3]), max_step_px = mp > 0 and mp or nil })\n";
    const char* a[] = { list.s, s[0], s[1], s[2], NULL };
    RunOut ro;
    int st = run_sandboxed(c, r, code, a, 300000, &ro);
    free(list.s);
    if (st < 0 && !ro.out) return;
    res_run(r, &ro, "dragged");
    runout_free(&ro);
}

static void t_press_key(Call* c, Result* r) {
    if (!pick_instance(c, r)) return;
    const char* key = jstr(c->args, "key");
    if (!key || !key[0]) { res_error(r, "missing 'key' (e.g. BACK, HOME, ENTER)"); return; }
    const char* args[] = { "key", "-i", c->instance, key, NULL };
    RunOut ro;
    run_self(args, 30000, NULL, &ro);
    res_run(r, &ro, "pressed");
    runout_free(&ro);
}

static void t_type_text(Call* c, Result* r) {
    if (!pick_instance(c, r)) return;
    const char* text = jstr(c->args, "text");
    if (!text || !text[0]) { res_error(r, "missing 'text'"); return; }
    const char* args[] = { "text", "-i", c->instance, "--", text, NULL };
    RunOut ro;
    run_self(args, 120000, NULL, &ro);
    res_run(r, &ro, "typed");
    runout_free(&ro);
}

static void t_list_apps(Call* c, Result* r) {
    if (!pick_instance(c, r)) return;
    const char* args[] = { "apps", "-i", c->instance, jbool(c->args, "all", false) ? "--all" : NULL, NULL };
    RunOut ro;
    run_self(args, 60000, NULL, &ro);
    res_run(r, &ro, "(no apps)");
    runout_free(&ro);
}

static void t_install_app(Call* c, Result* r) {
    if (!c->cfg->allow_install) { res_error(r, "installing apps is not allowed (SoulX launcher > AI control)"); return; }
    if (!pick_instance(c, r)) return;
    const char* path = jstr(c->args, "path");
    if (!path || path[0] != '/') { res_error(r, "'path' must be an absolute path to an .apk, .xapk, .apks or .apkm file"); return; }
    const char* args[] = { "install", "-i", c->instance, path, jbool(c->args, "reinstall", false) ? "--reinstall" : NULL, NULL };
    RunOut ro;
    run_self(args, 600000, NULL, &ro);
    res_run(r, &ro, "installed");
    runout_free(&ro);
}

static void t_app_cmd(Call* c, Result* r, const char* verb) {
    if (!pick_instance(c, r)) return;
    const char* pkg = jstr(c->args, "package");
    if (!pkg || !soulx_package_name_valid(pkg)) { res_error(r, "missing or invalid 'package' (e.g. com.example.game)"); return; }
    const char* args[] = { verb, "-i", c->instance, pkg, NULL };
    RunOut ro;
    run_self(args, 60000, NULL, &ro);
    res_run(r, &ro, "done");
    runout_free(&ro);
}

static void t_list_scripts(Call* c, Result* r) {
    if (!pick_instance(c, r)) return;
    SoulXScript list[128];
    int n = soulx_scripts_list(c->instance, list, 128);
    Buf b = { 0 };
    bf(&b, "Scripts of '%s':\n", c->instance);
    if (n <= 0) bs(&b, "  (none)\n");
    for (int i = 0; i < n && i < 128; i++) bf(&b, "  %s  (%s)\n", list[i].name[0] ? list[i].name : "(no name)", list[i].file);
    reap_jobs();
    bool any = false;
    for (int i = 0; i < MCP_JOBS_MAX; i++) {
        if (g_jobs[i].pid <= 0 || strcmp(g_jobs[i].instance, c->instance)) continue;
        if (!any) { bs(&b, "Background runs:\n"); any = true; }
        if (g_jobs[i].status == -1) bf(&b, "  job %d: %s, running for %lds\n", (int)g_jobs[i].pid, g_jobs[i].script, (long)(time(NULL) - g_jobs[i].started));
        else bf(&b, "  job %d: %s, finished (exit %d)\n", (int)g_jobs[i].pid, g_jobs[i].script, g_jobs[i].status);
    }
    res_text(r, b.s);
    free(b.s);
}

// A script of the instance only: a name/file known to its folder, or a path inside that folder.
static bool resolve_script(Call* c, Result* r, const char* script, char* out, size_t cap) {
    char box[PATH_MAX];
    if (!scripts_folder(c, r, box, sizeof(box))) return false;
    if (strchr(script, '/')) {
        char full[PATH_MAX], rbox[PATH_MAX], rp[PATH_MAX];
        if (script[0] == '/') snprintf(full, sizeof(full), "%s", script);
        else snprintf(full, sizeof(full), "%s/%s", box, script);
        if (!realpath(box, rbox) || !realpath(full, rp) || strncmp(rp, rbox, strlen(rbox)) || rp[strlen(rbox)] != '/') {
            res_error(r, "only the instance's own scripts can be run (see list_scripts)");
            return false;
        }
        snprintf(out, cap, "%s", rp);
        return true;
    }
    int fr = soulx_scripts_find(c->instance, script, out, cap);
    if (fr == 0) return true;
    res_error(r, fr == -2 ? "more than one script is called '%s': use its file name (see list_scripts)"
                          : "no script '%s' (see list_scripts)", script);
    return false;
}

static void t_run_script(Call* c, Result* r) {
    if (!c->cfg->allow_scripts) { res_error(r, "running scripts is not allowed (SoulX launcher > AI control)"); return; }
    if (!pick_instance(c, r)) return;
    const char* script = jstr(c->args, "script");
    if (!script || !script[0]) { res_error(r, "missing 'script' (a name from list_scripts)"); return; }
    char path[PATH_MAX];
    if (!resolve_script(c, r, script, path, sizeof(path))) return;
    double repeat, speed, timeout;
    if (!num_arg(c, r, "repeat", 1, 100000, 1, false, &repeat) || !num_arg(c, r, "speed", 0.1, 10, 1, false, &speed) ||
        !num_arg(c, r, "timeout_s", 1, 3600, 300, false, &timeout)) return;
    bool loop = jbool(c->args, "loop", false);
    bool background = jbool(c->args, "background", false) || loop;
    char rs[16], ss[16];
    snprintf(rs, sizeof(rs), "%d", (int)repeat);
    snprintf(ss, sizeof(ss), "%g", speed);
    const char* argv[40] = { "run", "-i", c->instance };
    int n = 3;
    if (loop) argv[n++] = "--loop";
    else { argv[n++] = "--repeat"; argv[n++] = rs; }
    argv[n++] = "--speed";
    argv[n++] = ss;
    argv[n++] = path;
    Json* a = jget(c->args, "args");
    for (Json* x = a && a->type == J_ARR ? a->child : NULL; x && n < 38; x = x->next) if (x->type == J_STR) argv[n++] = x->str;
    argv[n] = NULL;
    if (!background) {
        RunOut ro;
        run_self(argv, (int)timeout * 1000, NULL, &ro);
        res_run(r, &ro, "finished");
        runout_free(&ro);
        return;
    }
    reap_jobs();
    int slot = -1;
    for (int i = 0; i < MCP_JOBS_MAX; i++) if (g_jobs[i].pid <= 0 || g_jobs[i].status != -1) { slot = i; break; }
    if (slot < 0) { res_error(r, "too many background runs: stop one first (stop_script)"); return; }
    char dir[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "tmp/mcp") != 0 || soulx_mkdirs(dir) != 0) { res_error(r, "no temporary folder"); return; }
    Job* j = &g_jobs[slot];
    memset(j, 0, sizeof(*j));
    snprintf(j->log, sizeof(j->log), "%s/job-%d-%d.log", dir, (int)getpid(), slot);
    unlink(j->log);
    const char* full[48];
    full[0] = g_self;
    for (int i = 0; i <= n; i++) full[i + 1] = argv[i];
    SoulXCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.argv = full;
    unsetenv("SOULX_LUA_SANDBOX_DIR");
    pid_t pid = soulx_spawn_detached(&cmd, j->log, false);
    if (pid <= 0) { res_error(r, "could not start the script"); return; }
    j->pid = pid;
    j->status = -1;
    j->started = time(NULL);
    snprintf(j->instance, sizeof(j->instance), "%s", c->instance);
    snprintf(j->script, sizeof(j->script), "%s", script);
    res_textf(r, "started in the background as job %d (stop it with stop_script, see it in list_scripts)", (int)pid);
}

static void t_run_lua(Call* c, Result* r) {
    if (!c->cfg->allow_lua) { res_error(r, "running Lua code is not allowed (SoulX launcher > AI control)"); return; }
    if (!pick_instance(c, r)) return;
    const char* code = jstr(c->args, "code");
    if (!code || !code[0]) { res_error(r, "missing 'code'"); return; }
    if (strlen(code) > 200000) { res_error(r, "code is too long"); return; }
    double timeout;
    if (!num_arg(c, r, "timeout_s", 1, 600, 60, false, &timeout)) return;
    RunOut ro;
    if (run_sandboxed(c, r, code, NULL, (int)timeout * 1000, &ro) < 0 && !ro.out) return;
    res_run(r, &ro, "finished (no output: use log() to return values)");
    runout_free(&ro);
}

static void t_stop_script(Call* c, Result* r) {
    double job;
    if (!num_arg(c, r, "job", 1, 1e9, 0, true, &job)) return;
    reap_jobs();
    Job* j = NULL;
    for (int i = 0; i < MCP_JOBS_MAX; i++) if (g_jobs[i].pid == (pid_t)job) j = &g_jobs[i];
    if (!j) { res_error(r, "no job %d (see list_scripts)", (int)job); return; }
    if (!soulx_mcp_instance_allowed(c->cfg, j->instance)) { res_error(r, "instance '%s' is not allowed", j->instance); return; }
    snprintf(c->instance, sizeof(c->instance), "%s", j->instance);
    if (j->status == -1) {
        kill(j->pid, SIGINT);                       // soulx run stops cleanly: lifts fingers, prints the end
        for (int i = 0; i < 50 && j->status == -1; i++) { usleep(100000); reap_jobs(); }
        if (j->status == -1) { kill(j->pid, SIGKILL); usleep(200000); reap_jobs(); }
    }
    char* tail = read_tail(j->log, 8000);
    res_textf(r, "job %d stopped (exit %d). Last output:\n%s", (int)j->pid, j->status, tail ? tail : "");
    free(tail);
    unlink(j->log);
    j->pid = 0;
}

// ---------------------------------------------------------------------------
// Tool table
// ---------------------------------------------------------------------------

typedef void (*ToolFn)(Call*, Result*);

typedef struct {
    const char* name;
    ToolFn fn;
    const char* description;
    const char* schema;        // JSON Schema of the arguments (properties + required)
    bool read_only;
} Tool;

static void t_launch_app(Call* c, Result* r) { t_app_cmd(c, r, "launch"); }
static void t_stop_app(Call* c, Result* r) { t_app_cmd(c, r, "stop-app"); }

#define INST "\"instance\":{\"type\":\"string\",\"description\":\"Instance name (default: \\\"default\\\"); see list_instances\"}"
#define XY "\"x\":{\"type\":\"integer\",\"description\":\"Screen pixels from the left\"},\"y\":{\"type\":\"integer\",\"description\":\"Screen pixels from the top\"}"
#define REGION "\"region\":{\"type\":\"object\",\"description\":\"Part of the screen in screen pixels\",\"properties\":{\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"w\":{\"type\":\"integer\"},\"h\":{\"type\":\"integer\"}},\"required\":[\"x\",\"y\",\"w\",\"h\"]}"
#define POINTS "{\"type\":\"array\",\"items\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"},\"minItems\":2,\"maxItems\":2}}"

static const Tool k_tools[] = {
    { "list_instances", t_list_instances, "List the Android instances available for AI control and whether they run.",
      "{\"type\":\"object\",\"properties\":{}}", true },
    { "start_instance", t_start_instance, "Start an instance and wait until Android has booted (about 15-30 s).",
      "{\"type\":\"object\",\"properties\":{" INST ",\"window\":{\"type\":\"boolean\",\"description\":\"Open SoulX's window (default true)\"}}}", false },
    { "stop_instance", t_stop_instance, "Shut an instance down. Apps and data stay.",
      "{\"type\":\"object\",\"properties\":{" INST "}}", false },
    { "instance_status", t_instance_status, "Details of an instance: state, screen size, CPU and memory use.",
      "{\"type\":\"object\",\"properties\":{" INST "}}", true },
    { "screenshot", t_screenshot,
      "Look at the screen. Returns an image and the screen size. Coordinates for tap, swipe and the other tools are "
      "screen pixels; when the image is scaled or cropped, the text says how to convert.",
      "{\"type\":\"object\",\"properties\":{" INST "," REGION ",\"max_size\":{\"type\":\"integer\",\"description\":\"Longest side of the image (default 1024)\"},"
      "\"format\":{\"type\":\"string\",\"enum\":[\"jpeg\",\"png\"]},\"quality\":{\"type\":\"integer\",\"description\":\"JPEG quality 30-95 (default 75)\"}}}", true },
    { "read_text", t_read_text, "Read the text on the screen (on-device OCR), optionally only in a region.",
      "{\"type\":\"object\",\"properties\":{" INST "," REGION "}}", true },
    { "find_image", t_find_image,
      "Find a template image on the screen. The template is a PNG in the instance's scripts folder (a path relative to it).",
      "{\"type\":\"object\",\"properties\":{" INST ",\"template\":{\"type\":\"string\"},\"threshold\":{\"type\":\"number\",\"description\":\"0.1-1 (default 0.88)\"},"
      "\"all\":{\"type\":\"boolean\",\"description\":\"Every match instead of the best one\"}," REGION "},\"required\":[\"template\"]}", true },
    { "pixel_color", t_pixel_color, "Read the colour of screen pixels (#RRGGBB).",
      "{\"type\":\"object\",\"properties\":{" INST ",\"points\":" POINTS "},\"required\":[\"points\"]}", true },
    { "tap", t_tap, "Tap the screen at (x, y) in screen pixels.",
      "{\"type\":\"object\",\"properties\":{" INST "," XY "},\"required\":[\"x\",\"y\"]}", false },
    { "long_press", t_long_press, "Press and hold at (x, y).",
      "{\"type\":\"object\",\"properties\":{" INST "," XY ",\"ms\":{\"type\":\"integer\",\"description\":\"Hold time (default 800)\"}},\"required\":[\"x\",\"y\"]}", false },
    { "swipe", t_swipe, "Swipe from (x1, y1) to (x2, y2).",
      "{\"type\":\"object\",\"properties\":{" INST ",\"x1\":{\"type\":\"integer\"},\"y1\":{\"type\":\"integer\"},\"x2\":{\"type\":\"integer\"},\"y2\":{\"type\":\"integer\"},"
      "\"ms\":{\"type\":\"integer\",\"description\":\"Duration (default 300)\"}},\"required\":[\"x1\",\"y1\",\"x2\",\"y2\"]}", false },
    { "drag_path", t_drag_path,
      "Hold a finger down and drag it through a list of points (e.g. moving a tile along a path), then lift it.",
      "{\"type\":\"object\",\"properties\":{" INST ",\"points\":" POINTS ",\"step_ms\":{\"type\":\"integer\",\"description\":\"Pause between moves (default 20)\"},"
      "\"hold_ms\":{\"type\":\"integer\",\"description\":\"Hold before the first move (default 150)\"},\"max_step_px\":{\"type\":\"integer\",\"description\":\"Split longer moves (default 45)\"}},\"required\":[\"points\"]}", false },
    { "press_key", t_press_key, "Press a key: BACK, HOME, APP_SWITCH, ENTER, DEL, TAB, SPACE, ESCAPE, DPAD_*, VOLUME_UP/DOWN, A-Z, KEYCODE_*.",
      "{\"type\":\"object\",\"properties\":{" INST ",\"key\":{\"type\":\"string\"}},\"required\":[\"key\"]}", false },
    { "type_text", t_type_text, "Type text into the focused input field.",
      "{\"type\":\"object\",\"properties\":{" INST ",\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}", false },
    { "list_apps", t_list_apps, "List the installed apps (package names).",
      "{\"type\":\"object\",\"properties\":{" INST ",\"all\":{\"type\":\"boolean\",\"description\":\"Include system apps\"}}}", true },
    { "install_app", t_install_app, "Install an .apk, .xapk, .apks or .apkm file from the Mac (if allowed).",
      "{\"type\":\"object\",\"properties\":{" INST ",\"path\":{\"type\":\"string\",\"description\":\"Absolute path on the Mac\"},\"reinstall\":{\"type\":\"boolean\"}},\"required\":[\"path\"]}", false },
    { "launch_app", t_launch_app, "Start an installed app.",
      "{\"type\":\"object\",\"properties\":{" INST ",\"package\":{\"type\":\"string\"}},\"required\":[\"package\"]}", false },
    { "stop_app", t_stop_app, "Force-stop an app.",
      "{\"type\":\"object\",\"properties\":{" INST ",\"package\":{\"type\":\"string\"}},\"required\":[\"package\"]}", false },
    { "list_scripts", t_list_scripts, "List the instance's Lua scripts and the background runs started here.",
      "{\"type\":\"object\",\"properties\":{" INST "}}", true },
    { "run_script", t_run_script,
      "Run one of the instance's scripts (by name or file name from list_scripts). Waits for it, or runs it in the "
      "background (background or loop) and returns a job number for stop_script.",
      "{\"type\":\"object\",\"properties\":{" INST ",\"script\":{\"type\":\"string\"},\"args\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"repeat\":{\"type\":\"integer\"},\"loop\":{\"type\":\"boolean\"},\"speed\":{\"type\":\"number\",\"description\":\"0.1-10 (default 1)\"},"
      "\"background\":{\"type\":\"boolean\"},\"timeout_s\":{\"type\":\"integer\",\"description\":\"When waiting (default 300)\"}},\"required\":[\"script\"]}", false },
    { "run_lua", t_run_lua,
      "Run Lua code on the instance (SoulX's script API: tap, swipe, long_press, drag_path, key, text, sleep, find, "
      "wait, color, capture, read_text, launch, shell ...). Runs in a sandbox: no access to the Mac's files except "
      "the instance's scripts folder. Use log(...) to return values.",
      "{\"type\":\"object\",\"properties\":{" INST ",\"code\":{\"type\":\"string\"},\"timeout_s\":{\"type\":\"integer\",\"description\":\"1-600 (default 60)\"}},\"required\":[\"code\"]}", false },
    { "stop_script", t_stop_script, "Stop a background script run and return its last output.",
      "{\"type\":\"object\",\"properties\":{\"job\":{\"type\":\"integer\"}},\"required\":[\"job\"]}", false },
};

#define NTOOLS ((int)(sizeof(k_tools) / sizeof(k_tools[0])))

// ---------------------------------------------------------------------------
// Protocol
// ---------------------------------------------------------------------------

static void send_line(Buf* b) {
    bs(b, "\n");
    fwrite(b->s, 1, b->len, stdout);
    fflush(stdout);
}

static void reply_error(const Json* id, int code, const char* msg) {
    Buf b = { 0 };
    bs(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    bjson_raw(&b, id);
    bf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
    bjs(&b, msg);
    bs(&b, "}}");
    send_line(&b);
    free(b.s);
}

static void reply_result(const Json* id, const char* result_json) {
    Buf b = { 0 };
    bs(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    bjson_raw(&b, id);
    bs(&b, ",\"result\":");
    bs(&b, result_json);
    bs(&b, "}");
    send_line(&b);
    free(b.s);
}

static void handle_initialize(const Json* id, const Json* params) {
    const char* want = params ? jstr(params, "protocolVersion") : NULL;
    static const char* const known[] = { "2024-11-05", "2025-03-26", "2025-06-18", NULL };
    const char* ver = MCP_PROTOCOL_DEFAULT;
    for (int i = 0; want && known[i]; i++) if (!strcmp(want, known[i])) ver = known[i];
    Buf b = { 0 };
    bs(&b, "{\"protocolVersion\":");
    bjs(&b, ver);
    bs(&b, ",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"soulx\",\"version\":");
    bjs(&b, SOULX_VERSION_STRING);
    bs(&b, "},\"instructions\":");
    bjs(&b, "SoulX runs Android on this Mac. Look with screenshot (or read_text), act with tap / swipe / drag_path / "
            "press_key / type_text, and use run_lua for several steps at once. Coordinates are screen pixels. "
            "Only the instances and actions the user allowed in SoulX are available.");
    bs(&b, "}");
    reply_result(id, b.s);
    free(b.s);
}

static void handle_tools_list(const Json* id) {
    Buf b = { 0 };
    bs(&b, "{\"tools\":[");
    for (int i = 0; i < NTOOLS; i++) {
        if (i) bs(&b, ",");
        bs(&b, "{\"name\":");
        bjs(&b, k_tools[i].name);
        bs(&b, ",\"description\":");
        bjs(&b, k_tools[i].description);
        bs(&b, ",\"inputSchema\":");
        bs(&b, k_tools[i].schema);
        bf(&b, ",\"annotations\":{\"readOnlyHint\":%s}}", k_tools[i].read_only ? "true" : "false");
    }
    bs(&b, "]}");
    reply_result(id, b.s);
    free(b.s);
}

static void call_parsed(const char* name, const Json* args, char** result_json, bool* is_error);

static void handle_tools_call(const Json* id, const Json* params) {
    const char* name = params ? jstr(params, "name") : NULL;
    if (!name) { reply_error(id, -32602, "missing tool name"); return; }
    char* result = NULL;
    bool is_error = false;
    call_parsed(name, params ? jget(params, "arguments") : NULL, &result, &is_error);
    if (!result) { reply_error(id, -32603, "internal error"); return; }
    reply_result(id, result);
    free(result);
}

static void call_parsed(const char* name, const Json* args, char** result_json, bool* is_error) {
    *result_json = NULL;
    *is_error = false;
    SoulXMcpConfig cfg;
    soulx_mcp_config_load(&cfg);                    // every call: switching it off works at once
    Result r;
    memset(&r, 0, sizeof(r));
    Json empty = { 0 };
    empty.type = J_OBJ;
    Call c = { .cfg = &cfg, .args = (args && args->type == J_OBJ) ? args : &empty };
    const Tool* tool = NULL;
    for (int i = 0; i < NTOOLS; i++) if (!strcmp(k_tools[i].name, name)) tool = &k_tools[i];
    if (!cfg.enabled) {
        res_error(&r, "AI control is turned off in SoulX (launcher > AI control, or: soulx mcp config --enable)");
    } else if (!tool) {
        res_error(&r, "unknown tool '%s'", name);
    } else {
        tool->fn(&c, &r);
    }
    // audit: the tool, the instance and the arguments (text shortened; never images)
    Buf d = { 0 };
    for (const Json* m = c.args->child; m; m = m->next) {
        if (d.len > 250) break;
        bf(&d, "%s%s=", d.len ? " " : "", m->key ? m->key : "?");
        if (m->type == J_STR) bf(&d, "%.80s", m->str);
        else if (m->type == J_NUM) bf(&d, "%g", m->num);
        else if (m->type == J_BOOL) bs(&d, m->b ? "true" : "false");
        else bs(&d, "...");
    }
    soulx_mcp_audit(name, c.instance, d.s ? d.s : "", !r.error);
    free(d.s);
    Buf b = { 0 };
    bs(&b, "{\"content\":[");
    if (r.content.s) bs(&b, r.content.s);
    bf(&b, "],\"isError\":%s}", r.error ? "true" : "false");
    free(r.content.s);
    *result_json = b.s;
    *is_error = r.error;
}

void soulx_mcp_call_tool(const char* name, const char* args_json, char** result_json, bool* is_error) {
    Json* args = args_json ? jparse(args_json) : NULL;
    call_parsed(name, args, result_json, is_error);
    jfree(args);
}

static void handle_message(const char* line) {
    Json* msg = jparse(line);
    if (!msg || msg->type != J_OBJ) {
        Json nul = { 0 };
        reply_error(&nul, -32700, "parse error");
        jfree(msg);
        return;
    }
    const char* method = jstr(msg, "method");
    Json* id = jget(msg, "id");
    Json* params = jget(msg, "params");
    if (!method) { jfree(msg); return; }             // a response to us: we send no requests
    if (!id) { jfree(msg); return; }                 // notifications (initialized, cancelled ...): nothing to do
    if (!strcmp(method, "initialize")) handle_initialize(id, params);
    else if (!strcmp(method, "ping")) reply_result(id, "{}");
    else if (!strcmp(method, "tools/list")) handle_tools_list(id);
    else if (!strcmp(method, "tools/call")) handle_tools_call(id, params);
    else if (!strcmp(method, "resources/list")) reply_result(id, "{\"resources\":[]}");
    else if (!strcmp(method, "prompts/list")) reply_result(id, "{\"prompts\":[]}");
    else reply_error(id, -32601, "method not found");
    jfree(msg);
}

int soulx_mcp_serve(const char* self_exe) {
    snprintf(g_self, sizeof(g_self), "%s", self_exe ? self_exe : "");
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    char* line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) > 0) {
        if ((size_t)n > MCP_LINE_MAX) continue;
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (!n) continue;
        handle_message(line);
        reap_jobs();
    }
    free(line);
    // the client went away: stop background runs this server started
    for (int i = 0; i < MCP_JOBS_MAX; i++) if (g_jobs[i].pid > 0 && g_jobs[i].status == -1) kill(g_jobs[i].pid, SIGINT);
    return 0;
}

void soulx_mcp_set_self(const char* self_exe) { snprintf(g_self, sizeof(g_self), "%s", self_exe ? self_exe : ""); }

const char* soulx_mcp_tool_names(int i) { return i >= 0 && i < NTOOLS ? k_tools[i].name : NULL; }

// ---------------------------------------------------------------------------
// One-click connection of AI apps: write SoulX into each app's own MCP configuration
// ---------------------------------------------------------------------------

// Full JSON output (configuration files are rewritten with every other setting kept).
static void jwrite(Buf* b, const Json* j, int indent) {
    char pad[80];
    int n = indent * 2 < 78 ? indent * 2 : 78;
    memset(pad, ' ', (size_t)n);
    pad[n] = '\0';
    switch (j->type) {
    case J_NULL: bs(b, "null"); break;
    case J_BOOL: bs(b, j->b ? "true" : "false"); break;
    case J_NUM:
        if (j->num == floor(j->num) && fabs(j->num) < 1e15) bf(b, "%.0f", j->num);
        else bf(b, "%.17g", j->num);
        break;
    case J_STR: bjs(b, j->str); break;
    case J_ARR:
    case J_OBJ: {
        bool obj = j->type == J_OBJ;
        if (!j->child) { bs(b, obj ? "{}" : "[]"); break; }
        bs(b, obj ? "{\n" : "[\n");
        for (Json* c = j->child; c; c = c->next) {
            bs(b, pad);
            bs(b, "  ");
            if (obj) { bjs(b, c->key ? c->key : ""); bs(b, ": "); }
            jwrite(b, c, indent + 1);
            bs(b, c->next ? ",\n" : "\n");
        }
        bs(b, pad);
        bs(b, obj ? "}" : "]");
        break;
    }
    }
}

static Json* jstr_new(const char* key, const char* s) {
    Json* j = jnew(J_STR);
    if (!j) return NULL;
    j->key = key ? strdup(key) : NULL;
    j->str = strdup(s);
    return j;
}

// Removes member |key| of object |o|; true when it was there.
static bool jremove(Json* o, const char* key) {
    for (Json** pp = &o->child; *pp; pp = &(*pp)->next) {
        if ((*pp)->key && !strcmp((*pp)->key, key)) {
            Json* gone = *pp;
            *pp = gone->next;
            gone->next = NULL;
            jfree(gone);
            return true;
        }
    }
    return false;
}

// Object member |key| of |o|, created (empty object) when missing; NULL when it exists but is not an object.
static Json* jobject_member(Json* o, const char* key) {
    Json* m = jget(o, key);
    if (m) return m->type == J_OBJ ? m : NULL;
    m = jnew(J_OBJ);
    if (!m) return NULL;
    m->key = strdup(key);
    Json** tail = &o->child;
    while (*tail) tail = &(*tail)->next;
    *tail = m;
    return m;
}

typedef enum { CFG_JSON, CFG_CLI } ClientKind;

typedef struct {
    const char* id;
    const char* name;
    ClientKind kind;
    const char* config;        // CFG_JSON: path below $HOME
    const char* servers_key;   // "mcpServers" or "servers" (VS Code)
    bool vscode_style;         // entries carry "type": "stdio"
    const char* const* apps;   // presence of any of these (below / or $HOME) means installed
    const char* restart_hint;
} McpClient;

static const char* const k_app_desktop[] = { "/Applications/Claude.app", "~/Applications/Claude.app", NULL };
static const char* const k_app_cursor[] = { "/Applications/Cursor.app", "~/.cursor", NULL };
static const char* const k_app_vscode[] = { "/Applications/Visual Studio Code.app", "~/Applications/Visual Studio Code.app", NULL };
static const char* const k_app_windsurf[] = { "/Applications/Windsurf.app", "~/.codeium/windsurf", NULL };
static const char* const k_app_lmstudio[] = { "/Applications/LM Studio.app", "~/.lmstudio", NULL };
static const char* const k_app_gemini[] = { "~/.gemini", NULL };
static const char* const k_app_code_cli[] = { NULL };

static const McpClient k_clients[] = {
    { "claude-desktop", "Claude Desktop", CFG_JSON, "Library/Application Support/Claude/claude_desktop_config.json",
      "mcpServers", false, k_app_desktop, "Quit and reopen Claude Desktop." },
    { "claude-code", "Claude Code", CFG_CLI, ".claude.json", "mcpServers", false, k_app_code_cli,
      "Start a new Claude Code session." },
    { "cursor", "Cursor", CFG_JSON, ".cursor/mcp.json", "mcpServers", false, k_app_cursor, "Restart Cursor." },
    { "vscode", "VS Code", CFG_JSON, "Library/Application Support/Code/User/mcp.json", "servers", true, k_app_vscode,
      "Reload VS Code (its MCP servers list shows SoulX)." },
    { "windsurf", "Windsurf", CFG_JSON, ".codeium/windsurf/mcp_config.json", "mcpServers", false, k_app_windsurf,
      "Restart Windsurf." },
    { "lmstudio", "LM Studio", CFG_JSON, ".lmstudio/mcp.json", "mcpServers", false, k_app_lmstudio, "Restart LM Studio." },
    { "gemini", "Gemini CLI", CFG_JSON, ".gemini/settings.json", "mcpServers", false, k_app_gemini,
      "Start a new Gemini CLI session." },
};
#define NCLIENTS ((int)(sizeof(k_clients) / sizeof(k_clients[0])))

// $HOME first (tests point it elsewhere), else the account's home.
static bool home_dir(char* out, size_t cap) {
    const char* h = getenv("HOME");
    if (h && h[0] == '/') { snprintf(out, cap, "%s", h); return true; }
    return false;
}

static void expand(const char* p, char* out, size_t cap) {
    char home[PATH_MAX] = "";
    if (p[0] == '~' && home_dir(home, sizeof(home))) snprintf(out, cap, "%s%s", home, p + 1);
    else snprintf(out, cap, "%s", p);
}

// The client's command-line tool (Claude Code): GUI apps do not get the shell's PATH, so look in the usual places.
static bool find_cli(char* out, size_t cap) {
    static const char* const where[] = { "~/.local/bin/claude", "~/.claude/local/claude", "/opt/homebrew/bin/claude",
                                         "/usr/local/bin/claude", "~/.npm-global/bin/claude", "~/.bun/bin/claude", NULL };
    const char* forced = getenv("SOULX_MCP_CLIENT_CLI");     // tests
    if (forced) {
        snprintf(out, cap, "%s", forced);
        return forced[0] && access(out, X_OK) == 0;
    }
    for (int i = 0; where[i]; i++) {
        expand(where[i], out, cap);
        if (access(out, X_OK) == 0) return true;
    }
    return false;
}

static bool client_installed(const McpClient* c) {
    char p[PATH_MAX];
    if (c->kind == CFG_CLI) return find_cli(p, sizeof(p));
    for (int i = 0; c->apps[i]; i++) {
        expand(c->apps[i], p, sizeof(p));
        if (soulx_path_exists(p)) return true;
    }
    char home[PATH_MAX];
    if (home_dir(home, sizeof(home))) {
        snprintf(p, sizeof(p), "%s/%s", home, c->config);
        if (soulx_path_exists(p)) return true;
    }
    return false;
}

static int config_path(const McpClient* c, char* out, size_t cap) {
    char home[PATH_MAX];
    if (!home_dir(home, sizeof(home))) return -1;
    int n = snprintf(out, cap, "%s/%s", home, c->config);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

// "connected" (points at this soulx), "other" (a soulx entry pointing elsewhere), "no", or "unreadable".
static const char* client_state(const McpClient* c, const char* exe) {
    char path[PATH_MAX];
    if (config_path(c, path, sizeof(path)) != 0) return "no";
    char* data = NULL;
    size_t len = 0;
    if (soulx_read_file(path, &data, &len) != 0) return "no";
    Json* root = jparse(data);
    free(data);
    if (!root || root->type != J_OBJ) { jfree(root); return "unreadable"; }
    Json* servers = jget(root, c->servers_key);
    Json* entry = servers && servers->type == J_OBJ ? jget(servers, "soulx") : NULL;
    const char* state = "no";
    if (entry && entry->type == J_OBJ) {
        const char* cmd = jstr(entry, "command");
        state = cmd && !strcmp(cmd, exe) ? "connected" : "other";
    }
    jfree(root);
    return state;
}

static const McpClient* find_client(const char* id) {
    for (int i = 0; i < NCLIENTS; i++) if (!strcmp(k_clients[i].id, id)) return &k_clients[i];
    return NULL;
}

// Writes or removes the "soulx" entry in a JSON configuration, keeping everything else.
static int edit_json_config(const McpClient* c, const char* exe, bool add, char* msg, size_t cap) {
    char path[PATH_MAX];
    if (config_path(c, path, sizeof(path)) != 0) { snprintf(msg, cap, "no home folder"); return -1; }
    char* data = NULL;
    size_t len = 0;
    Json* root = NULL;
    bool existed = soulx_read_file(path, &data, &len) == 0;
    if (existed) {
        const char* p = data;
        skip_ws(&p);
        root = *p ? jparse(data) : jnew(J_OBJ);          // an empty file counts as {}
        if (!root || root->type != J_OBJ) {
            snprintf(msg, cap, "%s is not valid JSON; SoulX leaves it alone (fix it or add SoulX by hand)", path);
            jfree(root);
            free(data);
            return -1;
        }
    } else {
        if (!add) { snprintf(msg, cap, "not connected"); return 0; }
        root = jnew(J_OBJ);
    }
    Json* servers = jobject_member(root, c->servers_key);
    if (!servers) {
        snprintf(msg, cap, "\"%s\" in %s is not an object; SoulX leaves the file alone", c->servers_key, path);
        jfree(root);
        free(data);
        return -1;
    }
    jremove(servers, "soulx");
    if (add) {
        Json* e = jnew(J_OBJ);
        e->key = strdup("soulx");
        Json** t = &e->child;
        if (c->vscode_style) { *t = jstr_new("type", "stdio"); t = &(*t)->next; }
        *t = jstr_new("command", exe);
        t = &(*t)->next;
        Json* args = jnew(J_ARR);
        args->key = strdup("args");
        args->child = jstr_new(NULL, "mcp");
        *t = args;
        Json** tail = &servers->child;
        while (*tail) tail = &(*tail)->next;
        *tail = e;
    }
    // keep a copy of the user's file before SoulX first changes it
    if (existed) {
        char backup[PATH_MAX + 16];
        snprintf(backup, sizeof(backup), "%s.soulx-backup", path);
        if (!soulx_path_exists(backup)) (void)soulx_write_file_atomic(backup, data, len);
    }
    free(data);
    Buf b = { 0 };
    jwrite(&b, root, 0);
    bs(&b, "\n");
    jfree(root);
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char* slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; soulx_mkdirs(dir); }
    struct stat st;
    mode_t mode = stat(path, &st) == 0 ? (st.st_mode & 0777) : 0644;
    int rc = soulx_write_file_atomic(path, b.s, b.len);
    if (rc == 0) chmod(path, mode);
    free(b.s);
    if (rc != 0) { snprintf(msg, cap, "cannot write %s", path); return -1; }
    snprintf(msg, cap, "%s", add ? c->restart_hint : "removed");
    return 0;
}

static int edit_cli_config(const McpClient* c, const char* exe, bool add, char* msg, size_t cap) {
    (void)c;
    char cli[PATH_MAX];
    if (!find_cli(cli, sizeof(cli))) { snprintf(msg, cap, "its command-line tool was not found"); return -1; }
    // remove first so that "add" also repairs an entry that points elsewhere
    const char* rm[] = { cli, "mcp", "remove", "-s", "user", "soulx", NULL };
    SoulXCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.argv = rm;
    cmd.timeout_ms = 30000;
    SoulXCmdResult res;
    memset(&res, 0, sizeof(res));
    soulx_run(&cmd, &res);
    soulx_cmd_result_free(&res);
    if (!add) { snprintf(msg, cap, "removed"); return 0; }
    const char* ad[] = { cli, "mcp", "add", "-s", "user", "soulx", "--", exe, "mcp", NULL };
    cmd.argv = ad;
    memset(&res, 0, sizeof(res));
    int st = soulx_run(&cmd, &res);
    if (st != 0) snprintf(msg, cap, "it refused: %.200s", res.err && res.err[0] ? res.err : (res.out ? res.out : ""));
    else snprintf(msg, cap, "%s", c->restart_hint);
    soulx_cmd_result_free(&res);
    return st == 0 ? 0 : -1;
}

int soulx_mcp_client_count(void) { return NCLIENTS; }

int soulx_mcp_client_info(int i, const char* exe, SoulXMcpClientInfo* out) {
    if (i < 0 || i >= NCLIENTS || !out) return -1;
    const McpClient* c = &k_clients[i];
    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%s", c->id);
    snprintf(out->name, sizeof(out->name), "%s", c->name);
    out->installed = client_installed(c);
    snprintf(out->state, sizeof(out->state), "%s", client_state(c, exe));
    config_path(c, out->config, sizeof(out->config));
    return 0;
}

int soulx_mcp_client_connect(const char* id, const char* exe, bool add, char* msg, size_t cap) {
    const McpClient* c = find_client(id ? id : "");
    if (!c) { snprintf(msg, cap, "unknown app '%s'", id ? id : ""); return -1; }
    int rc = c->kind == CFG_CLI ? edit_cli_config(c, exe, add, msg, cap) : edit_json_config(c, exe, add, msg, cap);
    char detail[300];
    snprintf(detail, sizeof(detail), "%s %s: %s", add ? "connect" : "disconnect", c->id, msg);
    soulx_mcp_audit("app", NULL, detail, rc == 0);
    return rc;
}
