/*
 * SoulX Android Emulator - macOS backend: application packages
 * APK / XAPK / APKS / APKM inspection, installation, launch, stop and listing
 *
 * Archives are read with /usr/bin/zipinfo and /usr/bin/unzip (argv only, never a shell) and the
 * binary AndroidManifest.xml (AXML) is decoded by the bounds-checked parser below. Everything that
 * comes out of an archive is untrusted: offsets, lengths, counts and names are validated before use
 * and nothing read from it reaches a host shell.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "soulx_mac.h"

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

#define AXML_MAX_BYTES       (16u * 1024u * 1024u)  // AndroidManifest.xml size cap
#define AXML_MAX_STRINGS     (1u << 20)             // string pool entries
#define AXML_MAX_DEPTH       512                    // element nesting
#define XAPK_JSON_MAX_BYTES  (1024u * 1024u)        // manifest.json size cap
#define XAPK_NAME_MAX        256                    // split_apks[].file (including the NUL)
#define JSON_MAX_DEPTH       64
#define LISTING_MAX_BYTES    (64u * 1024u * 1024u)  // zipinfo output cap
#define APK_SET_MAX          512                    // APKs installed together (bundletool's all-config
                                                    // sets of big apps exceed 128 splits)
#define OBB_MAX              32                     // OBB files per package
#define SPLIT_NAME_MAX       128
#define ZIP_TIMEOUT_MS       (2 * 60 * 1000)
#define EXTRACT_TIMEOUT_MS   (15 * 60 * 1000)
#define INSTALL_TIMEOUT_MS   (10 * 60 * 1000)
#define OBB_TIMEOUT_MS       (30 * 60 * 1000)
#define SHELL_TIMEOUT_MS     (30 * 1000)
#define LAUNCH_TIMEOUT_MS    (60 * 1000)
#define EXTRACT_MARGIN_BYTES (512ull * 1024 * 1024)
#define UNZIP_BAD_PASSWORD   82                     // unzip: encrypted entries could not be decrypted
#define SIZE_UNKNOWN         UINT64_MAX

static const char k_unzip[] = "/usr/bin/unzip";
static const char k_zipinfo[] = "/usr/bin/zipinfo";

// Exported for the unit tests (tests/macos/test_apps.c); not part of soulx_mac.h.
int soulx_axml_parse(const uint8_t* data, size_t len, SoulXApkInfo* info);
int soulx_xapk_json_parse(const char* json, size_t len, char* package, size_t package_cap,
                         long long* version_code, char files[][XAPK_NAME_MAX], int max_files, int* file_count);
                                                    // 0 ok, -1 malformed, -2 more than max_files split APKs

// ---------------------------------------------------------------------------
// Binary XML constants (frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h)
// ---------------------------------------------------------------------------

#define RES_STRING_POOL_TYPE     0x0001
#define RES_XML_TYPE             0x0003
#define RES_XML_START_ELEMENT    0x0102
#define RES_XML_END_ELEMENT      0x0103
#define RES_XML_RESOURCE_MAP     0x0180
#define RES_UTF8_FLAG            0x0100u
#define RES_TYPE_STRING          0x03
#define RES_TYPE_FIRST_INT       0x10
#define RES_TYPE_LAST_INT        0x1f
#define AXML_NONE                0xFFFFFFFFu

#define ATTR_NAME                0x01010003u   // android:name
#define ATTR_SCREEN_ORIENTATION  0x0101001eu   // android:screenOrientation
#define ATTR_VERSION_CODE        0x0101021bu   // android:versionCode
#define ATTR_VERSION_NAME        0x0101021cu   // android:versionName
#define ATTR_TARGET_ACTIVITY     0x01010202u   // android:targetActivity
#define ATTR_VERSION_CODE_MAJOR  0x01010576u   // android:versionCodeMajor

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static uint16_t rd16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

static bool fmt_ok(int n, size_t cap)
{
    return n >= 0 && (size_t)n < cap;
}

// Copies src into dst; returns false (dst emptied) when it does not fit.
static bool copy_str(char* dst, size_t cap, const char* src)
{
    if (cap == 0) return false;
    size_t n = strlen(src);
    if (n >= cap) {
        dst[0] = '\0';
        return false;
    }
    memcpy(dst, src, n + 1);
    return true;
}

static bool ends_with_ci(const char* s, const char* suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcasecmp(s + n - m, suffix) == 0;
}

static const char* path_base(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Encodes one code point as UTF-8 (invalid code points become U+FFFD); returns the byte count.
static size_t utf8_put(uint32_t cp, char out[4])
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// Squashes tool output into one printable line for an error message.
static const char* one_line(const char* src, size_t len, char* buf, size_t cap)
{
    size_t o = 0;
    bool space = false;
    if (cap == 0) return "";
    for (size_t i = 0; src && i < len && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = o > 0;
            continue;
        }
        if (c < 0x20 || c == 0x7F) continue;
        if (space) {
            if (o + 2 >= cap) break;
            buf[o++] = ' ';
            space = false;
        }
        buf[o++] = (char)c;
    }
    buf[o] = '\0';
    return buf;
}

// Printable-ASCII copy of an untrusted name (an archive entry, a manifest.json field) for a message:
// anything else becomes '?' and long names are cut, so no terminal escape, line break or
// bidirectional-override character ever reaches the log.
static const char* shown(const char* src, char* buf, size_t cap)
{
    if (cap == 0) return "";
    const unsigned char* s = (const unsigned char*)(src ? src : "");
    size_t o = 0;
    while (*s && o + 1 < cap) {
        unsigned char c = *s++;
        if (c >= 0x80) {
            while ((*s & 0xC0) == 0x80) s++;            // one '?' per UTF-8 sequence
            c = '?';
        } else if (c < 0x20 || c == 0x7F) {
            c = '?';
        }
        buf[o++] = (char)c;
    }
    if (*s && cap >= 4) {                               // cut: mark it
        memcpy(buf + cap - 4, "...", 3);
        o = cap - 1;
    }
    buf[o] = '\0';
    return buf;
}

// Best message of a finished tool: stderr when present, else stdout.
static const char* tool_msg(const SoulXCmdResult* r, char* buf, size_t cap)
{
    one_line(r->err, r->err ? r->err_len : 0, buf, cap);
    if (buf[0] == '\0') one_line(r->out, r->out ? r->out_len : 0, buf, cap);
    if (buf[0] == '\0') copy_str(buf, cap, r->timed_out ? "timed out" : "no output");
    return buf;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool soulx_package_name_valid(const char* package)
{
    if (!package || !is_alpha(package[0])) return false;
    size_t len = strlen(package);
    if (len > 255) return false;
    int segments = 1;
    bool seg_empty = false;
    for (size_t i = 1; i < len; i++) {
        char c = package[i];
        if (c == '.') {
            if (seg_empty) return false;
            segments++;
            seg_empty = true;
            continue;
        }
        if (!is_alnum(c) && c != '_') return false;
        seg_empty = false;
    }
    return segments >= 2 && !seg_empty;
}

static bool serial_valid(const char* s)
{
    if (!s || !s[0] || s[0] == '-') return false;
    for (size_t n = 0; s[n]; n++) {
        char c = s[n];
        if (n >= 63) return false;
        if (!is_alnum(c) && c != '.' && c != '_' && c != ':' && c != '-') return false;
    }
    return true;
}

// Archive entry names handed to unzip: unzip treats them as patterns and they become host paths,
// so only plain relative names made of a conservative character set are accepted.
static bool entry_name_safe(const char* e)
{
    size_t n = strlen(e);
    if (n == 0 || n > 255) return false;
    const char* comp = e;
    for (size_t i = 0; i <= n; i++) {
        char c = e[i];
        if (c == '/' || c == '\0') {
            if (e + i == comp) return false;                       // "/x", "a//b", "a/"
            if (comp[0] == '.' || comp[0] == '-' || comp[0] == ' ') return false;   // ".", "..", hidden, option-like
            comp = e + i + 1;
            continue;
        }
        if (!is_alnum(c) && c != '.' && c != '_' && c != '-' && c != '+' && c != ' ' &&
            c != '(' && c != ')' && c != ',') return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// AXML: string pool
// ---------------------------------------------------------------------------

typedef struct {
    const uint8_t* chunk;        // string pool chunk (NULL until seen)
    size_t size;                 // validated chunk size
    size_t header;               // chunk header size (string offsets follow it)
    uint32_t count;
    size_t strings_start;
    bool utf8;
    const uint8_t* resmap;       // attribute resource ids, indexed like the string pool
    uint32_t resmap_count;
} AxmlPool;

typedef struct {
    const uint8_t* p;
    const uint8_t* end;
    bool utf8;
} AxmlStr;

static int axml_pool_load(AxmlPool* pool, const uint8_t* chunk, size_t header, size_t size)
{
    if (header < 28) return -1;
    uint32_t count = rd32(chunk + 8);
    uint32_t styles = rd32(chunk + 12);
    uint32_t flags = rd32(chunk + 16);
    uint32_t start = rd32(chunk + 20);
    if (count > AXML_MAX_STRINGS || styles > AXML_MAX_STRINGS) return -1;
    if ((uint64_t)header + ((uint64_t)count + styles) * 4 > size) return -1;
    if (count > 0 && start >= size) return -1;
    pool->chunk = chunk;
    pool->size = size;
    pool->header = header;
    pool->count = count;
    pool->strings_start = start;
    pool->utf8 = (flags & RES_UTF8_FLAG) != 0;
    return 0;
}

// UTF-8 pools prefix each string with two lengths of 1 or 2 bytes each.
static int axml_u8_len(const uint8_t** pp, const uint8_t* end, size_t* out)
{
    const uint8_t* p = *pp;
    if (p >= end) return -1;
    size_t n = p[0];
    p++;
    if (n & 0x80) {
        if (p >= end) return -1;
        n = ((n & 0x7F) << 8) | p[0];
        p++;
    }
    *pp = p;
    *out = n;
    return 0;
}

static int axml_str_open(const AxmlPool* pool, uint32_t idx, AxmlStr* s)
{
    if (!pool->chunk || idx >= pool->count) return -1;
    uint32_t off = rd32(pool->chunk + pool->header + (size_t)idx * 4);
    uint64_t pos = (uint64_t)pool->strings_start + off;
    if (pos >= pool->size) return -1;
    const uint8_t* p = pool->chunk + pos;
    const uint8_t* end = pool->chunk + pool->size;
    if (pool->utf8) {
        size_t units, bytes;
        if (axml_u8_len(&p, end, &units) != 0 || axml_u8_len(&p, end, &bytes) != 0) return -1;
        if (bytes > (size_t)(end - p)) return -1;
        s->p = p;
        s->end = p + bytes;
        s->utf8 = true;
        return 0;
    }
    if (end - p < 2) return -1;
    uint32_t units = rd16(p);
    p += 2;
    if (units & 0x8000) {
        if (end - p < 2) return -1;
        units = ((units & 0x7FFF) << 16) | rd16(p);
        p += 2;
    }
    if ((uint64_t)units * 2 > (uint64_t)(end - p)) return -1;
    s->p = p;
    s->end = p + (size_t)units * 2;
    s->utf8 = false;
    return 0;
}

// Next code point of the string, -1 at its end. Malformed sequences yield U+FFFD.
static int32_t axml_str_next(AxmlStr* s)
{
    if (s->p >= s->end) return -1;
    if (!s->utf8) {
        uint32_t u = rd16(s->p);
        s->p += 2;
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (s->end - s->p >= 2) {
                uint32_t lo = rd16(s->p);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    s->p += 2;
                    return (int32_t)(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                }
            }
            return 0xFFFD;
        }
        if (u >= 0xDC00 && u <= 0xDFFF) return 0xFFFD;
        return (int32_t)u;
    }
    const uint8_t* p = s->p;
    size_t avail = (size_t)(s->end - p);
    uint32_t c = p[0];
    if (c < 0x80) {
        s->p++;
        return (int32_t)c;
    }
    size_t n;
    uint32_t min;
    if ((c & 0xE0) == 0xC0) { n = 2; c &= 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 3; c &= 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 4; c &= 0x07; min = 0x10000; }
    else { s->p++; return 0xFFFD; }
    if (avail < n) { s->p++; return 0xFFFD; }
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { s->p++; return 0xFFFD; }
        c = (c << 6) | (p[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) { s->p++; return 0xFFFD; }
    s->p += n;
    return (int32_t)c;
}

// Decodes string idx as UTF-8 into out; control characters (and NUL) become '?'.
// Returns 0 = complete, 1 = truncated (out holds a prefix), -1 = invalid index or data.
static int axml_str_copy(const AxmlPool* pool, uint32_t idx, char* out, size_t cap)
{
    if (cap == 0) return -1;
    out[0] = '\0';
    AxmlStr s;
    if (axml_str_open(pool, idx, &s) != 0) return -1;
    size_t n = 0;
    int32_t cp;
    while ((cp = axml_str_next(&s)) >= 0) {
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) cp = '?';
        char enc[4];
        size_t k = utf8_put((uint32_t)cp, enc);
        if (n + k >= cap) {
            out[n] = '\0';
            return 1;
        }
        memcpy(out + n, enc, k);
        n += k;
    }
    out[n] = '\0';
    return 0;
}

// Compares string idx with an ASCII literal, stopping at the first difference.
static bool axml_str_eq(const AxmlPool* pool, uint32_t idx, const char* lit)
{
    AxmlStr s;
    if (axml_str_open(pool, idx, &s) != 0) return false;
    const unsigned char* l = (const unsigned char*)lit;
    int32_t cp;
    while ((cp = axml_str_next(&s)) >= 0) {
        if (*l == '\0' || (uint32_t)cp != *l) return false;
        l++;
    }
    return *l == '\0';
}

// ---------------------------------------------------------------------------
// AXML: chunk walker
// ---------------------------------------------------------------------------

typedef struct {
    bool start;                  // start (true) or end (false) of an element
    int depth;                   // depth of the element (root = 1)
    uint32_t name;               // element name (string index)
    const uint8_t* attrs;        // start elements: validated attribute array (NULL when empty)
    uint32_t attr_count;
    uint32_t attr_size;
} AxmlEvent;

typedef struct {
    uint32_t ns;
    uint32_t name;
    uint32_t raw;
    uint8_t type;
    uint32_t data;
} AxmlAttr;

// Visitor: 0 = continue, 1 = stop (success), -1 = reject the document.
typedef int (*AxmlVisitFn)(const AxmlPool* pool, const AxmlEvent* ev, void* ctx);

static int axml_walk(const uint8_t* data, size_t len, AxmlVisitFn visit, void* ctx)
{
    if (!data || len < 8 || len > AXML_MAX_BYTES) return -1;
    if (rd16(data) != RES_XML_TYPE) return -1;
    size_t header = rd16(data + 2);
    size_t total = rd32(data + 4);
    if (header < 8 || total < header || total > len) return -1;

    AxmlPool pool;
    memset(&pool, 0, sizeof(pool));
    int depth = 0;
    size_t off = header;
    while (off < total) {
        if (total - off < 8) return -1;
        const uint8_t* c = data + off;
        uint16_t type = rd16(c);
        size_t ch = rd16(c + 2);
        size_t cs = rd32(c + 4);
        if (ch < 8 || cs < ch || cs > total - off) return -1;

        if (type == RES_STRING_POOL_TYPE) {
            if (!pool.chunk && axml_pool_load(&pool, c, ch, cs) != 0) return -1;
        } else if (type == RES_XML_RESOURCE_MAP) {
            if (!pool.resmap) {
                pool.resmap = c + ch;
                pool.resmap_count = (uint32_t)((cs - ch) / 4);
            }
        } else if (type == RES_XML_START_ELEMENT) {
            // ResXMLTree_node (16 bytes) + ResXMLTree_attrExt (20 bytes) + attributes
            if (!pool.chunk || ch < 16 || cs - ch < 20) return -1;
            const uint8_t* ext = c + ch;
            size_t astart = rd16(ext + 8);
            size_t asize = rd16(ext + 10);
            size_t acount = rd16(ext + 12);
            if (acount > 0 && (asize < 20 || (uint64_t)astart + (uint64_t)asize * acount > cs - ch)) return -1;
            if (++depth > AXML_MAX_DEPTH) return -1;
            AxmlEvent ev = { true, depth, rd32(ext + 4), acount ? ext + astart : NULL,
                             (uint32_t)acount, (uint32_t)asize };
            int rc = visit(&pool, &ev, ctx);
            if (rc != 0) return rc > 0 ? 0 : -1;
        } else if (type == RES_XML_END_ELEMENT) {
            if (!pool.chunk || ch < 16 || cs - ch < 8 || depth == 0) return -1;
            AxmlEvent ev = { false, depth, rd32(c + ch + 4), NULL, 0, 0 };
            depth--;
            int rc = visit(&pool, &ev, ctx);
            if (rc != 0) return rc > 0 ? 0 : -1;
        }
        // namespaces, CDATA and unknown chunks are skipped
        off += cs;
    }
    return (pool.chunk && depth == 0) ? 0 : -1;
}

static void axml_attr_at(const AxmlEvent* ev, uint32_t i, AxmlAttr* a)
{
    const uint8_t* p = ev->attrs + (size_t)i * ev->attr_size;
    a->ns = rd32(p);
    a->name = rd32(p + 4);
    a->raw = rd32(p + 8);
    a->type = p[15];
    a->data = rd32(p + 16);
}

// Resource id of an attribute name (0 when the resource map does not cover it).
static uint32_t axml_attr_rid(const AxmlPool* pool, const AxmlAttr* a)
{
    if (pool->resmap && a->name < pool->resmap_count) return rd32(pool->resmap + (size_t)a->name * 4);
    return 0;
}

// android:<name>: matched by resource id like PackageManager (names may be obfuscated); the name is
// only a fallback for files without a resource map.
static bool axml_attr_is(const AxmlPool* pool, const AxmlAttr* a, uint32_t rid, const char* name)
{
    uint32_t r = axml_attr_rid(pool, a);
    if (r != 0) return r == rid;
    return a->ns != AXML_NONE && axml_str_eq(pool, a->name, name);
}

// Attribute without namespace ("package", "split").
static bool axml_attr_plain(const AxmlPool* pool, const AxmlAttr* a, const char* name)
{
    return a->ns == AXML_NONE && axml_str_eq(pool, a->name, name);
}

static uint32_t axml_attr_str(const AxmlAttr* a)
{
    return a->type == RES_TYPE_STRING ? a->data : a->raw;
}

static bool axml_attr_int(const AxmlAttr* a, uint32_t* v)
{
    if (a->type < RES_TYPE_FIRST_INT || a->type > RES_TYPE_LAST_INT) return false;
    *v = a->data;
    return true;
}

// ---------------------------------------------------------------------------
// AXML: AndroidManifest.xml scan
// ---------------------------------------------------------------------------

typedef struct {
    SoulXApkInfo* info;
    char split[SPLIT_NAME_MAX];      // manifest split="..." ("" = base APK)
    bool root_seen;
    bool have_package;
    uint32_t vc_low;                 // android:versionCode
    uint32_t vc_high;                // android:versionCodeMajor
    int app_depth;                   // <application> currently open at this depth (0 = no)
    int comp_depth;                  // <activity>/<activity-alias> currently open (0 = no)
    bool comp_alias;
    int comp_orientation;            // -1 = unspecified
    uint32_t comp_target;            // android:targetActivity of an alias (string index)
    bool comp_launcher;
    int filter_depth;                // <intent-filter> currently open (0 = no)
    bool f_main;
    bool f_launcher;
    bool found_launcher;             // first MAIN/LAUNCHER component
    bool launcher_alias;
    int launcher_orientation;
    char launcher_target[256];
} ManifestScan;

typedef struct {
    const char* package;
    const char* target;              // fully qualified class name of the alias target
    int app_depth;
    bool found;
    int orientation;
} AliasScan;

static bool orientation_landscape(int o)
{
    // landscape, sensorLandscape, reverseLandscape, userLandscape
    return o == 0 || o == 6 || o == 8 || o == 11;
}

static int orientation_value(const AxmlAttr* a)
{
    uint32_t v;
    return (axml_attr_int(a, &v) && v <= 1000) ? (int)v : -1;
}

// Expands ".Main" / "Main" class names the way PackageManager does.
static int class_qualify(const char* package, const char* cls, char* out, size_t cap)
{
    int n;
    if (cls[0] == '.') n = snprintf(out, cap, "%s%s", package, cls);
    else if (!strchr(cls, '.')) n = snprintf(out, cap, "%s.%s", package, cls);
    else n = snprintf(out, cap, "%s", cls);
    return fmt_ok(n, cap) ? 0 : -1;
}

static int scan_manifest_attrs(const AxmlPool* pool, const AxmlEvent* ev, ManifestScan* m)
{
    SoulXApkInfo* info = m->info;
    for (uint32_t i = 0; i < ev->attr_count; i++) {
        AxmlAttr a;
        uint32_t v;
        axml_attr_at(ev, i, &a);
        if (axml_attr_plain(pool, &a, "package")) {
            if (axml_str_copy(pool, axml_attr_str(&a), info->package, sizeof(info->package)) != 0) return -1;
            m->have_package = true;
        } else if (axml_attr_plain(pool, &a, "split")) {
            if (axml_str_copy(pool, axml_attr_str(&a), m->split, sizeof(m->split)) != 0) return -1;
        } else if (axml_attr_is(pool, &a, ATTR_VERSION_CODE, "versionCode")) {
            if (axml_attr_int(&a, &v)) m->vc_low = v;
        } else if (axml_attr_is(pool, &a, ATTR_VERSION_CODE_MAJOR, "versionCodeMajor")) {
            if (axml_attr_int(&a, &v)) m->vc_high = v;
        } else if (axml_attr_is(pool, &a, ATTR_VERSION_NAME, "versionName")) {
            // a resource reference (@string/...) cannot be resolved without resources.arsc
            if (axml_str_copy(pool, axml_attr_str(&a), info->version_name, sizeof(info->version_name)) < 0) {
                info->version_name[0] = '\0';
            }
        }
    }
    return 0;
}

static void scan_component_attrs(const AxmlPool* pool, const AxmlEvent* ev, ManifestScan* m)
{
    for (uint32_t i = 0; i < ev->attr_count; i++) {
        AxmlAttr a;
        axml_attr_at(ev, i, &a);
        if (axml_attr_is(pool, &a, ATTR_SCREEN_ORIENTATION, "screenOrientation")) {
            m->comp_orientation = orientation_value(&a);
        } else if (axml_attr_is(pool, &a, ATTR_TARGET_ACTIVITY, "targetActivity")) {
            m->comp_target = axml_attr_str(&a);
        }
    }
}

static void scan_filter_child(const AxmlPool* pool, const AxmlEvent* ev, ManifestScan* m, bool action)
{
    for (uint32_t i = 0; i < ev->attr_count; i++) {
        AxmlAttr a;
        axml_attr_at(ev, i, &a);
        if (!axml_attr_is(pool, &a, ATTR_NAME, "name")) continue;
        uint32_t s = axml_attr_str(&a);
        if (action && axml_str_eq(pool, s, "android.intent.action.MAIN")) m->f_main = true;
        if (!action && axml_str_eq(pool, s, "android.intent.category.LAUNCHER")) m->f_launcher = true;
    }
}

static int manifest_visit(const AxmlPool* pool, const AxmlEvent* ev, void* ctx)
{
    ManifestScan* m = ctx;
    if (!ev->start) {
        if (m->filter_depth && ev->depth == m->filter_depth) {
            if (m->f_main && m->f_launcher) m->comp_launcher = true;
            m->filter_depth = 0;
        } else if (m->comp_depth && ev->depth == m->comp_depth) {
            if (m->comp_launcher && !m->found_launcher) {
                m->found_launcher = true;
                m->launcher_alias = m->comp_alias;
                m->launcher_orientation = m->comp_orientation;
                m->launcher_target[0] = '\0';
                if (m->comp_alias && m->comp_target != AXML_NONE &&
                    axml_str_copy(pool, m->comp_target, m->launcher_target, sizeof(m->launcher_target)) != 0) {
                    m->launcher_target[0] = '\0';
                }
            }
            m->comp_depth = 0;
        } else if (m->app_depth && ev->depth == m->app_depth) {
            m->app_depth = 0;
        }
        return 0;
    }

    if (ev->depth == 1) {
        if (m->root_seen || !axml_str_eq(pool, ev->name, "manifest")) return -1;
        m->root_seen = true;
        return scan_manifest_attrs(pool, ev, m);
    }
    if (ev->depth == 2) {
        if (axml_str_eq(pool, ev->name, "application")) m->app_depth = 2;
        return 0;
    }
    if (m->app_depth && ev->depth == m->app_depth + 1) {
        bool activity = axml_str_eq(pool, ev->name, "activity");
        bool alias = !activity && axml_str_eq(pool, ev->name, "activity-alias");
        if (activity || alias) {
            m->comp_depth = ev->depth;
            m->comp_alias = alias;
            m->comp_orientation = -1;
            m->comp_target = AXML_NONE;
            m->comp_launcher = false;
            scan_component_attrs(pool, ev, m);
        }
        return 0;
    }
    if (m->comp_depth && ev->depth == m->comp_depth + 1) {
        if (axml_str_eq(pool, ev->name, "intent-filter")) {
            m->filter_depth = ev->depth;
            m->f_main = false;
            m->f_launcher = false;
        }
        return 0;
    }
    if (m->filter_depth && ev->depth == m->filter_depth + 1) {
        if (axml_str_eq(pool, ev->name, "action")) scan_filter_child(pool, ev, m, true);
        else if (axml_str_eq(pool, ev->name, "category")) scan_filter_child(pool, ev, m, false);
    }
    return 0;
}

// Second pass for an <activity-alias> launcher: the orientation comes from its target activity.
static int alias_visit(const AxmlPool* pool, const AxmlEvent* ev, void* ctx)
{
    AliasScan* s = ctx;
    if (!ev->start) {
        if (s->app_depth && ev->depth == s->app_depth) s->app_depth = 0;
        return 0;
    }
    if (ev->depth == 2 && axml_str_eq(pool, ev->name, "application")) {
        s->app_depth = 2;
        return 0;
    }
    if (!s->app_depth || ev->depth != s->app_depth + 1 || !axml_str_eq(pool, ev->name, "activity")) return 0;

    bool match = false;
    int orientation = -1;
    for (uint32_t i = 0; i < ev->attr_count; i++) {
        AxmlAttr a;
        axml_attr_at(ev, i, &a);
        if (axml_attr_is(pool, &a, ATTR_NAME, "name")) {
            char name[256], full[512];
            if (axml_str_copy(pool, axml_attr_str(&a), name, sizeof(name)) == 0 &&
                class_qualify(s->package, name, full, sizeof(full)) == 0) {
                match = strcmp(full, s->target) == 0;
            }
        } else if (axml_attr_is(pool, &a, ATTR_SCREEN_ORIENTATION, "screenOrientation")) {
            orientation = orientation_value(&a);
        }
    }
    if (!match) return 0;
    s->found = true;
    s->orientation = orientation;
    return 1;
}

static int axml_parse_manifest(const uint8_t* data, size_t len, SoulXApkInfo* info, char* split, size_t split_cap)
{
    info->package[0] = '\0';
    info->version_name[0] = '\0';
    info->version_code = 0;
    info->wants_landscape = false;
    if (split && split_cap) split[0] = '\0';

    ManifestScan m;
    memset(&m, 0, sizeof(m));
    m.info = info;
    m.comp_orientation = -1;
    m.comp_target = AXML_NONE;
    m.launcher_orientation = -1;
    if (axml_walk(data, len, manifest_visit, &m) != 0 || !m.root_seen || !m.have_package ||
        !soulx_package_name_valid(info->package)) {
        goto fail;
    }
    if (split && split_cap && !copy_str(split, split_cap, m.split)) goto fail;

    info->version_code = (long long)(((uint64_t)(m.vc_high & 0x7FFFFFFFu) << 32) | m.vc_low);
    int orientation = m.launcher_orientation;
    if (m.found_launcher && m.launcher_alias && m.launcher_target[0]) {
        char target[512];
        if (class_qualify(info->package, m.launcher_target, target, sizeof(target)) == 0) {
            AliasScan s = { info->package, target, 0, false, -1 };
            if (axml_walk(data, len, alias_visit, &s) == 0 && s.found) orientation = s.orientation;
        }
    }
    info->wants_landscape = m.found_launcher && orientation_landscape(orientation);
    return 0;

fail:
    info->package[0] = '\0';
    info->version_name[0] = '\0';
    info->version_code = 0;
    info->wants_landscape = false;
    if (split && split_cap) split[0] = '\0';
    return -1;
}

int soulx_axml_parse(const uint8_t* data, size_t len, SoulXApkInfo* info)
{
    if (!info) return -1;
    return axml_parse_manifest(data, len, info, NULL, 0);
}

// ---------------------------------------------------------------------------
// XAPK manifest.json: a small JSON scanner (package_name, version_code, split_apks[].file)
// ---------------------------------------------------------------------------

typedef struct {
    const char* p;
    const char* end;
    int depth;
} JsonCur;

static void json_ws(JsonCur* j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static bool json_peek(JsonCur* j, char c)
{
    json_ws(j);
    return j->p < j->end && *j->p == c;
}

static int json_hex4(JsonCur* j, uint32_t* v)
{
    if (j->end - j->p < 4) return -1;
    uint32_t x = 0;
    for (int i = 0; i < 4; i++) {
        char c = j->p[i];
        x <<= 4;
        if (c >= '0' && c <= '9') x |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') x |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') x |= (uint32_t)(c - 'A' + 10);
        else return -1;
    }
    j->p += 4;
    *v = x;
    return 0;
}

// Reads a string value (the cursor must be at its opening quote); out may be NULL.
// Returns 0, 1 when out was too small (it holds a prefix), -1 when malformed.
static int json_string(JsonCur* j, char* out, size_t cap)
{
    json_ws(j);
    if (j->p >= j->end || *j->p != '"') return -1;
    j->p++;
    size_t n = 0;
    bool truncated = false;
    if (out && cap) out[0] = '\0';
    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p++;
        char enc[4];
        size_t k = 1;
        if (c == '"') {
            if (out && cap) out[n] = '\0';
            return truncated ? 1 : 0;
        }
        if (c < 0x20) return -1;
        if (c == '\\') {
            if (j->p >= j->end) return -1;
            char e = *j->p++;
            uint32_t cp;
            switch (e) {
            case '"': cp = '"'; break;
            case '\\': cp = '\\'; break;
            case '/': cp = '/'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'n': cp = '\n'; break;
            case 'r': cp = '\r'; break;
            case 't': cp = '\t'; break;
            case 'u':
                if (json_hex4(j, &cp) != 0) return -1;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    JsonCur save = *j;
                    uint32_t lo = 0;
                    if (j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                        j->p += 2;
                        if (json_hex4(j, &lo) != 0 || lo < 0xDC00 || lo > 0xDFFF) {
                            *j = save;
                            lo = 0;
                        }
                    }
                    cp = lo ? 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00) : 0xFFFD;
                }
                break;
            default:
                return -1;
            }
            k = utf8_put(cp, enc);
        } else {
            enc[0] = (char)c;
        }
        if (out && cap) {
            if (!truncated && n + k < cap) {
                memcpy(out + n, enc, k);
                n += k;
            } else {
                truncated = true;
            }
        }
    }
    return -1;
}

// Copies a number / literal token; returns 0, 1 when too long, -1 when empty.
static int json_token(JsonCur* j, char* out, size_t cap)
{
    json_ws(j);
    size_t n = 0;
    bool truncated = false;
    while (j->p < j->end && (is_alnum(*j->p) || *j->p == '-' || *j->p == '+' || *j->p == '.')) {
        if (n + 1 < cap) out[n++] = *j->p;
        else truncated = true;
        j->p++;
    }
    if (cap) out[n] = '\0';
    if (n == 0 && !truncated) return -1;
    return truncated ? 1 : 0;
}

static int json_skip(JsonCur* j)
{
    json_ws(j);
    if (j->p >= j->end) return -1;
    char c = *j->p;
    if (c == '"') return json_string(j, NULL, 0) < 0 ? -1 : 0;
    if (c == '{' || c == '[') {
        char close = (c == '{') ? '}' : ']';
        if (++j->depth > JSON_MAX_DEPTH) return -1;
        j->p++;
        if (json_peek(j, close)) {
            j->p++;
            j->depth--;
            return 0;
        }
        for (;;) {
            if (c == '{') {
                if (json_string(j, NULL, 0) < 0 || !json_peek(j, ':')) return -1;
                j->p++;
            }
            if (json_skip(j) != 0) return -1;
            json_ws(j);
            if (j->p >= j->end) return -1;
            if (*j->p == ',') {
                j->p++;
                continue;
            }
            if (*j->p != close) return -1;
            j->p++;
            j->depth--;
            return 0;
        }
    }
    char tok[64];
    return json_token(j, tok, sizeof(tok)) < 0 ? -1 : 0;
}

static int json_parse_count(const char* s, long long* v)
{
    if (!is_digit(s[0]) || strlen(s) > 19) return -1;
    for (const char* p = s; *p; p++) {
        if (!is_digit(*p)) return -1;
    }
    errno = 0;
    long long x = strtoll(s, NULL, 10);
    if (errno != 0 || x < 0) return -1;
    *v = x;
    return 0;
}

// One split_apks[] object: only "file" is used.
static int json_split_object(JsonCur* j, char* file, size_t cap, bool* have)
{
    *have = false;
    file[0] = '\0';
    j->p++;                                  // '{'
    if (++j->depth > JSON_MAX_DEPTH) return -1;
    if (json_peek(j, '}')) {
        j->p++;
        j->depth--;
        return 0;
    }
    for (;;) {
        char key[16];
        int kr = json_string(j, key, sizeof(key));
        if (kr < 0 || !json_peek(j, ':')) return -1;
        j->p++;
        if (kr == 0 && strcmp(key, "file") == 0 && json_peek(j, '"')) {
            int r = json_string(j, file, cap);
            if (r < 0) return -1;
            *have = (r == 0 && file[0] != '\0');
        } else if (json_skip(j) != 0) {
            return -1;
        }
        json_ws(j);
        if (j->p >= j->end) return -1;
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        if (*j->p != '}') return -1;
        j->p++;
        j->depth--;
        return 0;
    }
}

// 0 ok, -1 malformed, -2 more than max_files entries.
static int json_split_apks(JsonCur* j, char files[][XAPK_NAME_MAX], int max_files, int* count)
{
    j->p++;                                  // '['
    if (++j->depth > JSON_MAX_DEPTH) return -1;
    if (json_peek(j, ']')) {
        j->p++;
        j->depth--;
        return 0;
    }
    for (;;) {
        if (json_peek(j, '{')) {
            char file[XAPK_NAME_MAX];
            bool have;
            if (json_split_object(j, file, sizeof(file), &have) != 0) return -1;
            if (have && files) {
                if (*count >= max_files) return -2;          // more APKs than SoulX installs together
                memcpy(files[*count], file, sizeof(file));
                (*count)++;
            }
        } else if (json_skip(j) != 0) {
            return -1;
        }
        json_ws(j);
        if (j->p >= j->end) return -1;
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        if (*j->p != ']') return -1;
        j->p++;
        j->depth--;
        return 0;
    }
}

int soulx_xapk_json_parse(const char* json, size_t len, char* package, size_t package_cap,
                         long long* version_code, char files[][XAPK_NAME_MAX], int max_files, int* file_count)
{
    int local_count = 0;
    if (!file_count) file_count = &local_count;
    *file_count = 0;
    if (package && package_cap) package[0] = '\0';
    if (version_code) *version_code = -1;
    if (!json || len > XAPK_JSON_MAX_BYTES) return -1;

    JsonCur j = { json, json + len, 0 };
    if (len >= 3 && (unsigned char)json[0] == 0xEF && (unsigned char)json[1] == 0xBB &&
        (unsigned char)json[2] == 0xBF) {
        j.p += 3;                            // UTF-8 BOM
    }
    if (!json_peek(&j, '{')) return -1;
    j.p++;
    j.depth = 1;
    if (json_peek(&j, '}')) return 0;
    for (;;) {
        char key[32];
        int kr = json_string(&j, key, sizeof(key));
        if (kr < 0 || !json_peek(&j, ':')) return -1;
        j.p++;
        json_ws(&j);
        if (j.p >= j.end) return -1;
        char c = *j.p;
        if (kr == 0 && strcmp(key, "package_name") == 0 && c == '"') {
            // untrusted: only a valid package name is kept (it ends up in messages)
            char tmp[256];
            int r = json_string(&j, tmp, sizeof(tmp));
            if (r < 0) return -1;
            if (package && package_cap &&
                (r != 0 || !soulx_package_name_valid(tmp) || !copy_str(package, package_cap, tmp))) {
                package[0] = '\0';
            }
        } else if (kr == 0 && strcmp(key, "version_code") == 0 && (c == '"' || c == '-' || is_digit(c))) {
            char tmp[32];
            int r = (c == '"') ? json_string(&j, tmp, sizeof(tmp)) : json_token(&j, tmp, sizeof(tmp));
            long long v;
            if (r < 0) return -1;
            if (r == 0 && version_code && json_parse_count(tmp, &v) == 0) *version_code = v;
        } else if (kr == 0 && strcmp(key, "split_apks") == 0 && c == '[') {
            int r = json_split_apks(&j, files, max_files, file_count);
            if (r != 0) return r;
        } else if (json_skip(&j) != 0) {
            return -1;
        }
        json_ws(&j);
        if (j.p >= j.end) return -1;
        if (*j.p == ',') {
            j.p++;
            continue;
        }
        if (*j.p != '}') return -1;
        return 0;
    }
}

// ---------------------------------------------------------------------------
// APK sets: the APK files (base + splits) and OBB files of one app on the host
// ---------------------------------------------------------------------------

static const char* const k_abis[] = {
    "arm64-v8a", "armeabi-v7a", "armeabi", "x86", "x86_64", "riscv64", "mips", "mips64"
};
#define ABI_COUNT     ((int)(sizeof(k_abis) / sizeof(k_abis[0])))
#define ABI_ARM64_BIT 1u                    // k_abis[0]

typedef struct {
    char path[PATH_MAX];                    // APK on the host (the input file or an extracted copy)
    char split[SPLIT_NAME_MAX];             // split name from its manifest ("" = base APK)
    char package[256];
    bool skip;                              // config split for an ABI the arm64 images cannot run
} ApkFile;

typedef struct {
    char name[XAPK_NAME_MAX];               // "Android/obb/<package>/<file>.obb"
    uint64_t size;                          // uncompressed size from the listing (SIZE_UNKNOWN if not listed)
} ObbEntry;

typedef struct {
    ApkFile apks[APK_SET_MAX];
    int apk_count;
    char obbs[OBB_MAX][PATH_MAX];           // OBB files on the host: directory input, or unpacked entries
    int obb_count;
    ObbEntry obb_entries[OBB_MAX];          // archive input: OBB entries, unpacked only when they are copied
    int obb_entry_count;
    char zip[PATH_MAX];                     // archive input: the archive ("" = none)
    char dir[PATH_MAX];                     // directory input: the directory ("" = none)
    int base;                               // index of the base APK
    unsigned abi_mask;                      // bit i = k_abis[i]
    SoulXApkInfo info;
    char json_package[256];                 // XAPK manifest.json package_name ("" = none)
    char tmpdir[PATH_MAX];                  // private scratch dir under $SOULX_HOME/tmp ("" = none yet)
    char zip_warning[256];                  // first warning of zipinfo/unzip (exit status 1), "" = none
    int log_level;                          // level of the "Unpacking ..." message (quiet for inspection)
    int links;
} ApkSet;

// Matches an ABI directory name ("arm64-v8a") or a split suffix ("arm64_v8a"); -1 if unknown.
static int abi_lookup(const char* s, size_t n, bool split_form)
{
    for (int i = 0; i < ABI_COUNT; i++) {
        const char* a = k_abis[i];
        if (strlen(a) != n) continue;
        bool eq = true;
        for (size_t k = 0; k < n && eq; k++) {
            char c = a[k];
            if (split_form && c == '-') c = '_';
            eq = (s[k] == c);
        }
        if (eq) return i;
    }
    return -1;
}

// "config.arm64_v8a" -> index of arm64-v8a; -1 for any other split.
static int split_abi(const char* split)
{
    if (strncmp(split, "config.", 7) != 0) return -1;
    return abi_lookup(split + 7, strlen(split + 7), true);
}

// ABIs with native libraries in a zip listing (lib/<abi>/<name>.so).
static unsigned abis_from_listing(const char* listing, size_t len)
{
    unsigned mask = 0;
    const char* p = listing;
    const char* end = listing + len;
    while (p < end) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        const char* le = nl ? nl : end;
        while (le > p && le[-1] == '\r') le--;
        if (le - p > 4 && memcmp(p, "lib/", 4) == 0) {
            const char* a = p + 4;
            const char* slash = memchr(a, '/', (size_t)(le - a));
            if (slash && le - (slash + 1) > 3 && memcmp(le - 3, ".so", 3) == 0) {
                int idx = abi_lookup(a, (size_t)(slash - a), false);
                if (idx >= 0) mask |= 1u << idx;
            }
        }
        p = nl ? nl + 1 : end;
    }
    return mask;
}

static void abis_format(unsigned mask, char* out, size_t cap)
{
    size_t o = 0;
    out[0] = '\0';
    for (int i = 0; i < ABI_COUNT; i++) {
        if (!(mask & (1u << i))) continue;
        size_t n = strlen(k_abis[i]);
        if (o + n + 2 > cap) break;
        if (o) out[o++] = ',';
        memcpy(out + o, k_abis[i], n);
        o += n;
        out[o] = '\0';
    }
}

// A process with this pid exists (zombies and processes of other users included).
static bool pid_exists(pid_t pid)
{
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

// tmp/apk-<pid>-XXXXXX: the scratch directory of one SoulX process. Returns its pid, 0 if not one.
static pid_t scratch_dir_pid(const char* name)
{
    if (strncmp(name, "apk-", 4) != 0 || !is_digit(name[4])) return 0;
    char* end = NULL;
    errno = 0;
    long pid = strtol(name + 4, &end, 10);
    if (errno != 0 || pid <= 0 || pid > INT_MAX || *end != '-' || strlen(end + 1) != 6) return 0;
    for (const char* p = end + 1; *p; p++) {
        if (!is_alnum(*p)) return 0;
    }
    return (pid_t)pid;
}

// Removes the scratch directories of SoulX processes that no longer exist: an install that was killed
// (SIGTERM, a closed terminal, a second Ctrl+C) or crashed leaves its unpacked APKs and OBB data
// behind, possibly gigabytes. Runs once per process.
static void sweep_stale_scratch(void)
{
    static atomic_flag swept = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set(&swept)) return;
    char dir[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "tmp") != 0) return;
    DIR* d = opendir(dir);
    if (!d) return;
    char stale[32][NAME_MAX + 1];
    int n = 0;
    struct dirent* e;
    while (n < 32 && (e = readdir(d)) != NULL) {
        pid_t pid = scratch_dir_pid(e->d_name);
        if (pid <= 0 || pid == getpid() || pid_exists(pid) || !copy_str(stale[n], sizeof(stale[n]), e->d_name)) continue;
        n++;
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        if (soulx_path(path, sizeof(path), "tmp/%s", stale[i]) != 0) continue;
        if (soulx_rmtree(path) == 0) soulx_log(SOULX_LOG_DEBUG, "removed %s (left by an interrupted install)", path);
        else soulx_log(SOULX_LOG_WARN, "could not remove %s (left by an interrupted install)", path);
    }
}

static int apkset_tmp(ApkSet* set)
{
    if (set->tmpdir[0]) return 0;
    char dir[PATH_MAX], tmpl[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "tmp") != 0 || soulx_mkdirs(dir) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s/tmp", soulx_home());
        return -1;
    }
    sweep_stale_scratch();
    if (soulx_path(tmpl, sizeof(tmpl), "tmp/apk-%d-XXXXXX", (int)getpid()) != 0 || !mkdtemp(tmpl)) {
        soulx_log(SOULX_LOG_ERROR, "cannot create a temporary directory in %s: %s", dir, strerror(errno));
        return -1;
    }
    return copy_str(set->tmpdir, sizeof(set->tmpdir), tmpl) ? 0 : -1;
}

static void apkset_free(ApkSet* set)
{
    if (!set) return;
    if (set->tmpdir[0] && soulx_rmtree(set->tmpdir) != 0) {
        soulx_log(SOULX_LOG_WARN, "could not remove %s", set->tmpdir);
    }
    free(set);
}

// unzip/zipinfo expand wildcards in their archive argument: hand them a pattern-free "./name"
// relative to dir (through a private symlink when the real file name contains * ? [ ] or \).
static int apkset_tool_path(ApkSet* set, const char* abs, char* dir, size_t dcap, char* name, size_t ncap)
{
    const char* base = path_base(abs);
    if (abs[0] != '/' || base[0] == '\0') return -1;
    if (!strpbrk(base, "*?[]\\")) {
        size_t dlen = (size_t)(base - abs) - 1;
        if (dlen == 0) dlen = 1;            // a file directly in "/"
        if (dlen >= dcap) return -1;
        memcpy(dir, abs, dlen);
        dir[dlen] = '\0';
        return fmt_ok(snprintf(name, ncap, "./%s", base), ncap) ? 0 : -1;
    }
    if (apkset_tmp(set) != 0) return -1;
    char link[PATH_MAX];
    int n = set->links++;
    if (!fmt_ok(snprintf(link, sizeof(link), "%s/in-%d.zip", set->tmpdir, n), sizeof(link)) ||
        !fmt_ok(snprintf(name, ncap, "./in-%d.zip", n), ncap) || !copy_str(dir, dcap, set->tmpdir)) {
        return -1;
    }
    if (symlink(abs, link) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot link %s: %s", abs, strerror(errno));
        return -1;
    }
    return 0;
}

static int run_tool(const char* const* argv, const char* cwd, size_t max_output, int timeout_ms, SoulXCmdResult* res)
{
    SoulXCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.argv = argv;
    cmd.cwd = cwd;
    cmd.timeout_ms = timeout_ms;
    cmd.max_output = max_output;
    memset(res, 0, sizeof(*res));
    return soulx_run(&cmd, res);
}

// unzip/zipinfo exit codes: 0 = ok, 1 = warnings only (e.g. bytes before the first entry).
static bool zip_status_ok(int rc)
{
    return rc == 0 || rc == 1;
}

// Keeps the first warning of an archive tool (exit status 1, e.g. "41 extra bytes at beginning or
// within zipfile" for a truncated download): shown when the archive then turns out to be unusable.
static void note_zip_warning(ApkSet* set, const char* zip, const SoulXCmdResult* res)
{
    if (set->zip_warning[0] || !res->err || res->err_len == 0) return;
    char line[200];
    one_line(res->err, res->err_len, line, sizeof(line));
    // display text: a cut message is acceptable
    if (line[0]) (void)snprintf(set->zip_warning, sizeof(set->zip_warning), "%s: %s", path_base(zip), line);
}

static void password_error(const char* zip)
{
    soulx_log(SOULX_LOG_ERROR, "%s is password-protected; SoulX cannot unpack encrypted archives", path_base(zip));
}

// Entry names of a zip (zipinfo -1), newline separated.
static int zip_listing(ApkSet* set, const char* zip, char** out, size_t* out_len)
{
    char dir[PATH_MAX], name[PATH_MAX], msg[256];
    if (apkset_tool_path(set, zip, dir, sizeof(dir), name, sizeof(name)) != 0) return -1;
    const char* argv[] = { k_zipinfo, "-1", name, NULL };
    SoulXCmdResult res;
    int rc = run_tool(argv, dir, LISTING_MAX_BYTES, ZIP_TIMEOUT_MS, &res);
    if (!zip_status_ok(rc) || !res.out || res.out_len >= LISTING_MAX_BYTES) {
        soulx_log(SOULX_LOG_ERROR, "cannot read the archive %s: %s", path_base(zip), tool_msg(&res, msg, sizeof(msg)));
        soulx_cmd_result_free(&res);
        return -1;
    }
    if (rc == 1) note_zip_warning(set, zip, &res);
    *out = res.out;
    *out_len = res.out_len;
    res.out = NULL;
    soulx_cmd_result_free(&res);
    return 0;
}

// Reads one entry into memory (unzip -p): 0 ok, -1 when it is missing, empty, larger than cap or
// unreadable, -2 when it is encrypted. "-P ''" makes unzip fail at once on an encrypted entry
// instead of prompting for the password on the terminal (invisibly, as its output is captured).
static int zip_read_entry(ApkSet* set, const char* zip, const char* entry, size_t cap, uint8_t** data, size_t* len)
{
    char dir[PATH_MAX], name[PATH_MAX];
    *data = NULL;
    *len = 0;
    if (apkset_tool_path(set, zip, dir, sizeof(dir), name, sizeof(name)) != 0) return -1;
    const char* argv[] = { k_unzip, "-P", "", "-p", name, entry, NULL };
    SoulXCmdResult res;
    int rc = run_tool(argv, dir, cap + 1, ZIP_TIMEOUT_MS, &res);
    if (rc == UNZIP_BAD_PASSWORD) {
        soulx_cmd_result_free(&res);
        return -2;
    }
    if (!zip_status_ok(rc) || !res.out || res.out_len == 0 || res.out_len > cap) {
        soulx_cmd_result_free(&res);
        return -1;
    }
    if (rc == 1) note_zip_warning(set, zip, &res);
    *data = (uint8_t*)res.out;
    *len = res.out_len;
    res.out = NULL;
    soulx_cmd_result_free(&res);
    return 0;
}

// Total uncompressed size of an archive ("N files, M bytes uncompressed, ..." from zipinfo -t).
static int zip_total_size(ApkSet* set, const char* zip, uint64_t* total)
{
    char dir[PATH_MAX], name[PATH_MAX];
    if (apkset_tool_path(set, zip, dir, sizeof(dir), name, sizeof(name)) != 0) return -1;
    const char* argv[] = { k_zipinfo, "-t", name, NULL };
    SoulXCmdResult res;
    int rc = run_tool(argv, dir, 64 * 1024, ZIP_TIMEOUT_MS, &res);
    int ret = -1;
    const char* key = res.out ? strstr(res.out, " bytes uncompressed") : NULL;
    if (zip_status_ok(rc) && key) {
        const char* p = key;
        while (p > res.out && is_digit(p[-1])) p--;
        if (p < key) {
            errno = 0;
            unsigned long long v = strtoull(p, NULL, 10);
            if (errno == 0) {
                *total = v;
                ret = 0;
            }
        }
    }
    soulx_cmd_result_free(&res);
    return ret;
}

// Per-entry sizes and encryption flags of an archive (zipinfo -s -T).
typedef struct {
    const char* name;                       // points into ZipInfo.text
    uint64_t size;                          // uncompressed bytes
    bool encrypted;
} ZipEntryInfo;

typedef struct {
    char* text;
    ZipEntryInfo* entries;
    size_t count;
} ZipInfo;

static void zip_info_free(ZipInfo* zi)
{
    free(zi->text);
    free(zi->entries);
    memset(zi, 0, sizeof(*zi));
}

static bool digits_only(const char* s, size_t from, size_t to)
{
    for (size_t i = from; i < to; i++) {
        if (!is_digit(s[i])) return false;
    }
    return true;
}

// One entry line: "<mode> <version> <os> <size> <type> <method> <yyyymmdd.hhmmss> <name>". The name
// follows the timestamp after exactly one space; the first letter of <type> (t = text, b = binary) is
// upper case for an encrypted entry. Header and totals lines do not have this shape. Names with
// control characters are printed in ^X notation, so an entry name never spans lines.
static bool zip_info_line(char* line, ZipEntryInfo* e)
{
    char* tok[7];
    char* p = line;
    for (int i = 0; i < 7; i++) {
        while (*p == ' ') p++;
        tok[i] = p;
        while (*p && *p != ' ') p++;
        if (*p != ' ' || p == tok[i]) return false;
        *p++ = '\0';
    }
    const char* type = tok[4];
    const char* when = tok[6];
    size_t digits = strlen(tok[3]);
    if (!*p || digits == 0 || digits > 20 || !digits_only(tok[3], 0, digits) || strlen(type) != 2 ||
        !strchr("bBtT", type[0]) || strlen(when) != 15 || when[8] != '.' || !digits_only(when, 0, 8) ||
        !digits_only(when, 9, 15)) {
        return false;
    }
    errno = 0;
    unsigned long long v = strtoull(tok[3], NULL, 10);
    if (errno != 0) return false;
    e->name = p;
    e->size = v;
    e->encrypted = type[0] == 'B' || type[0] == 'T';
    return true;
}

static int zip_info(ApkSet* set, const char* zip, ZipInfo* zi)
{
    char dir[PATH_MAX], name[PATH_MAX];
    memset(zi, 0, sizeof(*zi));
    if (apkset_tool_path(set, zip, dir, sizeof(dir), name, sizeof(name)) != 0) return -1;
    const char* argv[] = { k_zipinfo, "-s", "-T", name, NULL };
    SoulXCmdResult res;
    int rc = run_tool(argv, dir, LISTING_MAX_BYTES, ZIP_TIMEOUT_MS, &res);
    if (!zip_status_ok(rc) || !res.out || res.out_len >= LISTING_MAX_BYTES) {
        soulx_cmd_result_free(&res);
        return -1;
    }
    size_t len = res.out_len, lines = 1;
    for (size_t i = 0; i < len; i++) {
        if (res.out[i] == '\n') lines++;
    }
    zi->entries = calloc(lines, sizeof(*zi->entries));
    if (!zi->entries) {
        soulx_cmd_result_free(&res);
        return -1;
    }
    zi->text = res.out;                     // NUL-terminated by soulx_run
    res.out = NULL;
    soulx_cmd_result_free(&res);
    for (char* p = zi->text; p < zi->text + len;) {
        char* nl = memchr(p, '\n', (size_t)(zi->text + len - p));
        char* le = nl ? nl : zi->text + len;
        *le = '\0';
        if (le > p && le[-1] == '\r') le[-1] = '\0';
        if (zi->count < lines && zip_info_line(p, &zi->entries[zi->count])) zi->count++;
        p = le + 1;
    }
    return 0;
}

static const ZipEntryInfo* zip_info_find(const ZipInfo* zi, const char* name)
{
    for (size_t i = 0; i < zi->count; i++) {
        if (strcmp(zi->entries[i].name, name) == 0) return &zi->entries[i];
    }
    return NULL;
}

// Refuses to unpack need bytes when the scratch volume has less free space (plus a margin).
static int check_free_space(const ApkSet* set, const char* zip, uint64_t need)
{
    uint64_t free_bytes = soulx_free_disk_bytes(set->tmpdir);
    if (free_bytes > 0 && (need > free_bytes || free_bytes - need < EXTRACT_MARGIN_BYTES)) {
        soulx_log(SOULX_LOG_ERROR, "not enough free disk space to unpack %s (needs %.1f GB, %.1f GB free)",
                 path_base(zip), (double)need / 1e9, (double)free_bytes / 1e9);
        return -1;
    }
    return 0;
}

static int apkset_add_apk(ApkSet* set, const char* path)
{
    if (set->apk_count >= APK_SET_MAX) {
        soulx_log(SOULX_LOG_ERROR, "too many APK files (SoulX installs at most %d together)", APK_SET_MAX);
        return -1;
    }
    ApkFile* f = &set->apks[set->apk_count];
    memset(f, 0, sizeof(*f));
    if (!copy_str(f->path, sizeof(f->path), path)) {
        soulx_log(SOULX_LOG_ERROR, "path too long: %s", path);
        return -1;
    }
    set->apk_count++;
    return 0;
}

static int apkset_add_obb(ApkSet* set, const char* path)
{
    if (set->obb_count >= OBB_MAX) {
        soulx_log(SOULX_LOG_WARN, "more than %d OBB files; ignoring %s", OBB_MAX, path_base(path));
        return 0;
    }
    if (!copy_str(set->obbs[set->obb_count], sizeof(set->obbs[0]), path)) return -1;
    set->obb_count++;
    return 0;
}

static int cmp_apkfile(const void* a, const void* b)
{
    return strcmp(((const ApkFile*)a)->path, ((const ApkFile*)b)->path);
}

// A folder of split APKs. Only regular files are taken: a folder unpacked from a downloaded archive
// may contain symbolic links to anything on the Mac.
static int apkset_from_dir(ApkSet* set, const char* dir)
{
    if (!copy_str(set->dir, sizeof(set->dir), dir)) {
        soulx_log(SOULX_LOG_ERROR, "path too long: %s", dir);
        return -1;
    }
    DIR* d = opendir(dir);
    if (!d) {
        soulx_log(SOULX_LOG_ERROR, "%s: %s", dir, strerror(errno));
        return -1;
    }
    int rc = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        char p[PATH_MAX];
        struct stat sb;
        if (e->d_name[0] == '.' || !ends_with_ci(e->d_name, ".apk")) continue;
        if (!fmt_ok(snprintf(p, sizeof(p), "%s/%s", dir, e->d_name), sizeof(p)) || lstat(p, &sb) != 0) continue;
        if (S_ISLNK(sb.st_mode)) {
            soulx_log(SOULX_LOG_WARN, "ignoring %s: it is a symbolic link (put the APK file itself in the folder)",
                     e->d_name);
            continue;
        }
        if (!S_ISREG(sb.st_mode)) continue;
        if (apkset_add_apk(set, p) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(d);
    if (rc != 0) return -1;
    if (set->apk_count == 0) {
        soulx_log(SOULX_LOG_ERROR, "%s contains no .apk files", dir);
        return -1;
    }
    qsort(set->apks, (size_t)set->apk_count, sizeof(set->apks[0]), cmp_apkfile);
    return 0;
}

// "dir/<name>" directly inside dir (no deeper path).
static bool entry_in_dir(const char* e, const char* dir)
{
    size_t n = strlen(dir);
    return strncmp(e, dir, n) == 0 && e[n] != '\0' && !strchr(e + n, '/');
}

// Android/obb/<package>/<file>.obb
static bool entry_is_obb(const char* e)
{
    static const char prefix[] = "Android/obb/";
    if (strncmp(e, prefix, sizeof(prefix) - 1) != 0) return false;
    const char* owner = e + sizeof(prefix) - 1;
    const char* slash = strchr(owner, '/');
    return slash && slash > owner && !strchr(slash + 1, '/') && ends_with_ci(slash + 1, ".obb") &&
           entry_name_safe(e);
}

static bool sel_has(const char* const* sel, int n, const char* e)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(sel[i], e) == 0) return true;
    }
    return false;
}

// Adds e unless it is already selected; false when the selection is full.
static bool sel_add(const char** sel, int* n, int max, const char* e)
{
    if (sel_has(sel, *n, e)) return true;
    if (*n >= max) return false;
    sel[(*n)++] = e;
    return true;
}

// APK entries of an archive without a usable manifest.json: bundletool splits/ (+ asset-slices/),
// else APKs at the root (XAPK, APKM, universal .apks), else the arm64 bundletool standalone APK.
// Returns their number, -1 when there are more than max.
static int select_apk_entries(char** lines, size_t n, const char** sel, int max)
{
    int k = 0;
    for (size_t i = 0; i < n; i++) {
        const char* e = lines[i];
        if ((entry_in_dir(e, "splits/") || entry_in_dir(e, "asset-slices/")) && ends_with_ci(e, ".apk") &&
            entry_name_safe(e) && !sel_add(sel, &k, max, e)) {
            return -1;
        }
    }
    if (k) return k;
    for (size_t i = 0; i < n; i++) {
        const char* e = lines[i];
        if (!strchr(e, '/') && ends_with_ci(e, ".apk") && entry_name_safe(e) && !sel_add(sel, &k, max, e)) return -1;
    }
    if (k) return k;
    const char* pick = NULL;
    int standalones = 0;
    for (size_t i = 0; i < n; i++) {
        const char* e = lines[i];
        if (!entry_in_dir(e, "standalones/") || !ends_with_ci(e, ".apk") || !entry_name_safe(e)) continue;
        standalones++;
        if (!pick && strstr(e, "arm64_v8a")) pick = e;
    }
    if (!pick && standalones == 1) {
        for (size_t i = 0; i < n && !pick; i++) {
            const char* e = lines[i];
            if (entry_in_dir(e, "standalones/") && ends_with_ci(e, ".apk") && entry_name_safe(e)) pick = e;
        }
    }
    if (pick && max > 0) sel[k++] = pick;
    return k;
}

// Extracts the given entries into <tmpdir>/x and checks each one is a plain file.
// 0 ok, -1 error, -2 the archive is password-protected (reported).
static int apkset_extract(ApkSet* set, const char* zip, const char* const* entries, int n, char* outdir, size_t cap)
{
    char dir[PATH_MAX], name[PATH_MAX], msg[256];
    if (apkset_tmp(set) != 0) return -1;
    if (!fmt_ok(snprintf(outdir, cap, "%s/x", set->tmpdir), cap) || soulx_mkdirs(outdir) != 0) return -1;
    if (apkset_tool_path(set, zip, dir, sizeof(dir), name, sizeof(name)) != 0) return -1;

    const char** argv = calloc((size_t)n + 10, sizeof(*argv));
    if (!argv) {
        soulx_log(SOULX_LOG_ERROR, "out of memory");
        return -1;
    }
    int k = 0;
    argv[k++] = k_unzip;
    argv[k++] = "-q";
    argv[k++] = "-o";
    argv[k++] = "-P";                       // an encrypted entry fails at once instead of prompting
    argv[k++] = "";
    argv[k++] = name;
    for (int i = 0; i < n; i++) argv[k++] = entries[i];
    argv[k++] = "-d";
    argv[k++] = outdir;
    argv[k] = NULL;

    SoulXCmdResult res;
    int rc = run_tool(argv, dir, 1024 * 1024, EXTRACT_TIMEOUT_MS, &res);
    free(argv);
    if (rc == UNZIP_BAD_PASSWORD) {
        password_error(zip);
        soulx_cmd_result_free(&res);
        return -2;
    }
    if (!zip_status_ok(rc)) {
        soulx_log(SOULX_LOG_ERROR, "cannot unpack %s: %s", path_base(zip), tool_msg(&res, msg, sizeof(msg)));
        soulx_cmd_result_free(&res);
        return -1;
    }
    if (rc == 1) note_zip_warning(set, zip, &res);
    soulx_cmd_result_free(&res);

    for (int i = 0; i < n; i++) {
        char p[PATH_MAX];
        struct stat sb;
        if (!fmt_ok(snprintf(p, sizeof(p), "%s/%s", outdir, entries[i]), sizeof(p)) ||
            lstat(p, &sb) != 0 || !S_ISREG(sb.st_mode)) {
            soulx_log(SOULX_LOG_ERROR, "%s: entry %s is missing or not a regular file", path_base(zip), entries[i]);
            return -1;
        }
    }
    return 0;
}

// Selects and unpacks the APKs of an archive. Its OBB entries (want_obb) are only listed here:
// soulx_install_path unpacks them later, and only those the device does not have yet.
static int apkset_from_archive(ApkSet* set, const char* zip, bool want_obb)
{
    int ret = -1;
    char* listing = NULL;
    size_t llen = 0;
    char** lines = NULL;
    size_t nlines = 0;
    char (*json_files)[XAPK_NAME_MAX] = NULL;
    const char* sel[APK_SET_MAX];
    int napk = 0;
    char buf[300];
    ZipInfo zi;
    memset(&zi, 0, sizeof(zi));

    if (!copy_str(set->zip, sizeof(set->zip), zip) || apkset_tmp(set) != 0) return -1;
    if (zip_listing(set, zip, &listing, &llen) != 0) return -1;
    if (zip_info(set, zip, &zi) != 0) soulx_log(SOULX_LOG_DEBUG, "cannot read the entry sizes of %s", path_base(zip));

    // one entry name per line (names with embedded NULs/newlines simply never validate)
    for (size_t i = 0; i < llen; i++) {
        if (listing[i] == '\n') nlines++;
    }
    lines = calloc(nlines + 1, sizeof(*lines));
    if (!lines) {
        soulx_log(SOULX_LOG_ERROR, "out of memory");
        goto out;
    }
    nlines = 0;
    for (char* p = listing; p < listing + llen;) {
        char* nl = memchr(p, '\n', (size_t)(listing + llen - p));
        char* le = nl ? nl : listing + llen;
        *le = '\0';                                  // listing is NUL-terminated by soulx_run
        if (le > p && le[-1] == '\r') le[-1] = '\0';
        if (*p) lines[nlines++] = p;
        p = le + 1;
    }

    // XAPK: manifest.json names the APKs to install
    for (size_t i = 0; i < nlines; i++) {
        if (strcmp(lines[i], "manifest.json") != 0) continue;
        uint8_t* js = NULL;
        size_t jlen = 0;
        int nfiles = 0;
        long long vc = -1;
        json_files = calloc(APK_SET_MAX, sizeof(*json_files));
        if (!json_files) {
            soulx_log(SOULX_LOG_ERROR, "out of memory");
            goto out;
        }
        int jr = zip_read_entry(set, zip, "manifest.json", XAPK_JSON_MAX_BYTES, &js, &jlen);
        int pr = jr == 0 ? soulx_xapk_json_parse((const char*)js, jlen, set->json_package, sizeof(set->json_package),
                                                &vc, json_files, APK_SET_MAX, &nfiles)
                         : -1;
        free(js);
        if (jr == -2) {
            password_error(zip);
            goto out;
        }
        if (pr == -2) {
            soulx_log(SOULX_LOG_ERROR, "the manifest.json of %s lists more than %d APKs (SoulX installs at most %d "
                     "together)", path_base(zip), APK_SET_MAX, APK_SET_MAX);
            goto out;
        }
        if (pr != 0) {
            soulx_log(SOULX_LOG_WARN, "%s: ignoring an unreadable manifest.json", path_base(zip));
            nfiles = 0;
        }
        for (int f = 0; f < nfiles; f++) {
            const char* name = json_files[f];
            const char* found = NULL;
            if (!ends_with_ci(name, ".apk") || !entry_name_safe(name)) {
                soulx_log(SOULX_LOG_WARN, "manifest.json: ignoring split entry '%s'", shown(name, buf, sizeof(buf)));
                continue;
            }
            for (size_t l = 0; l < nlines && !found; l++) {
                if (strcmp(lines[l], name) == 0) found = lines[l];
            }
            if (!found) {
                soulx_log(SOULX_LOG_WARN, "manifest.json lists %s but the archive does not contain it", name);
                continue;
            }
            (void)sel_add(sel, &napk, APK_SET_MAX, found);      // at most APK_SET_MAX names were parsed
        }
        break;
    }
    if (napk == 0) napk = select_apk_entries(lines, nlines, sel, APK_SET_MAX);
    if (napk < 0) {
        soulx_log(SOULX_LOG_ERROR, "%s holds more than %d APKs (SoulX installs at most %d together)", path_base(zip),
                 APK_SET_MAX, APK_SET_MAX);
        goto out;
    }
    if (napk == 0) {
        soulx_log(SOULX_LOG_ERROR, "%s contains no APK", path_base(zip));
        goto out;
    }

    // only the APKs are unpacked now: room for them, and no password prompt
    uint64_t need = 0;
    bool unknown = false;
    for (int i = 0; i < napk; i++) {
        const ZipEntryInfo* ze = zip_info_find(&zi, sel[i]);
        if (!ze) {
            unknown = true;
        } else if (ze->encrypted) {
            password_error(zip);
            goto out;
        } else {
            need += ze->size;
        }
    }
    uint64_t total = 0;
    if (unknown && zip_total_size(set, zip, &total) == 0) need = total;    // the whole archive at most
    if (check_free_space(set, zip, need) != 0) goto out;

    if (want_obb) {
        for (size_t i = 0; i < nlines; i++) {
            bool dup = false;
            if (!entry_is_obb(lines[i])) continue;
            for (int k = 0; k < set->obb_entry_count && !dup; k++) dup = strcmp(set->obb_entries[k].name, lines[i]) == 0;
            if (dup) continue;
            if (set->obb_entry_count >= OBB_MAX) {
                soulx_log(SOULX_LOG_WARN, "more than %d OBB files; ignoring %s", OBB_MAX, path_base(lines[i]));
                continue;
            }
            ObbEntry* o = &set->obb_entries[set->obb_entry_count];
            if (!copy_str(o->name, sizeof(o->name), lines[i])) continue;
            const ZipEntryInfo* ze = zip_info_find(&zi, lines[i]);
            o->size = ze ? ze->size : SIZE_UNKNOWN;
            set->obb_entry_count++;
        }
    }

    soulx_log(set->log_level, "Unpacking %s...", path_base(zip));
    char outdir[PATH_MAX];
    if (apkset_extract(set, zip, sel, napk, outdir, sizeof(outdir)) != 0) goto out;
    for (int i = 0; i < napk; i++) {
        char p[PATH_MAX];
        if (!fmt_ok(snprintf(p, sizeof(p), "%s/%s", outdir, sel[i]), sizeof(p)) || apkset_add_apk(set, p) != 0) goto out;
    }
    ret = 0;

out:
    zip_info_free(&zi);
    free(json_files);
    free(lines);
    free(listing);
    return ret;
}

// Parent directory name of an OBB equals the package (".../Android/obb/<package>/<file>.obb").
static bool obb_owned_by(const char* path, const char* package)
{
    const char* base = path_base(path);
    if (base == path) return false;
    const char* end = base - 1;
    const char* start = end;
    while (start > path && start[-1] != '/') start--;
    size_t n = (size_t)(end - start);
    return n == strlen(package) && memcmp(start, package, n) == 0;
}

// Directory input: opens <dir>/Android/obb/<package> without following a symbolic link in any of
// those components. Whatever is copied there can be read by the (untrusted) game, so a link to,
// say, ~/.ssh must never be followed. -1 when the folder does not exist or is reached through a link.
static int open_obb_dir(const char* dir, const char* package, bool quiet)
{
    const char* parts[3] = { "Android", "obb", package };
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (int i = 0; i < 3 && fd >= 0; i++) {
        int next = openat(fd, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && (errno == ELOOP || errno == ENOTDIR) && !quiet) {
            struct stat sb;
            if (fstatat(fd, parts[i], &sb, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(sb.st_mode)) {
                soulx_log(SOULX_LOG_WARN, "ignoring the OBB files of %s: %s/%s is a symbolic link", package,
                         i == 0 ? dir : (i == 1 ? "Android" : "Android/obb"), parts[i]);
            }
        }
        close(fd);
        fd = next;
    }
    return fd;
}

// OBB data of the app: archive entries under Android/obb/<package>/ (listed, not unpacked yet), or
// for a directory input the regular files in <dir>/Android/obb/<package>/ (symbolic links refused).
static int apkset_collect_obbs(ApkSet* set, bool want_obb)
{
    const char* package = set->info.package;
    char buf[300];
    int kept = 0;
    for (int i = 0; i < set->obb_entry_count; i++) {
        if (obb_owned_by(set->obb_entries[i].name, package)) {
            if (kept != i) set->obb_entries[kept] = set->obb_entries[i];
            kept++;
        } else {
            soulx_log(SOULX_LOG_WARN, "ignoring %s (OBB of another package)",
                     shown(path_base(set->obb_entries[i].name), buf, sizeof(buf)));
        }
    }
    set->obb_entry_count = kept;
    if (!want_obb || !set->dir[0]) return 0;

    int dfd = open_obb_dir(set->dir, package, false);
    if (dfd < 0) return 0;
    int lfd = dup(dfd);
    DIR* d = lfd >= 0 ? fdopendir(lfd) : NULL;
    if (!d) {
        if (lfd >= 0) close(lfd);
        close(dfd);
        return 0;
    }
    int rc = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && rc == 0) {
        char p[PATH_MAX];
        struct stat sb;
        if (e->d_name[0] == '.' || !ends_with_ci(e->d_name, ".obb")) continue;
        if (fstatat(dfd, e->d_name, &sb, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISLNK(sb.st_mode)) {
            soulx_log(SOULX_LOG_WARN, "ignoring %s: an OBB file must be a regular file, not a symbolic link", e->d_name);
            continue;
        }
        if (!S_ISREG(sb.st_mode)) continue;
        if (!fmt_ok(snprintf(p, sizeof(p), "%s/Android/obb/%s/%s", set->dir, package, e->d_name), sizeof(p))) continue;
        rc = apkset_add_obb(set, p);
    }
    closedir(d);                            // closes lfd
    close(dfd);
    return rc;
}

// Reads every APK's manifest and listing: base APK, package consistency, splits, ABIs.
static int apkset_analyze(ApkSet* set, bool want_obb)
{
    int bases = 0;
    unsigned mask = 0;
    for (int i = 0; i < set->apk_count; i++) {
        ApkFile* f = &set->apks[i];
        const char* name = path_base(f->path);
        uint8_t* xml = NULL;
        size_t xlen = 0;
        if (soulx_interrupted()) return -1;
        if (zip_read_entry(set, f->path, "AndroidManifest.xml", AXML_MAX_BYTES, &xml, &xlen) != 0) {
            soulx_log(SOULX_LOG_ERROR, "%s is not a readable APK (no AndroidManifest.xml)", name);
            return -1;
        }
        SoulXApkInfo inf;
        memset(&inf, 0, sizeof(inf));
        int rc = axml_parse_manifest(xml, xlen, &inf, f->split, sizeof(f->split));
        free(xml);
        if (rc != 0) {
            soulx_log(SOULX_LOG_ERROR, "%s: malformed or unsupported AndroidManifest.xml", name);
            return -1;
        }
        copy_str(f->package, sizeof(f->package), inf.package);
        if (f->split[0] == '\0') {
            bases++;
            set->base = i;
            set->info = inf;
        } else {
            int abi = split_abi(f->split);
            if (abi >= 0) mask |= 1u << abi;
        }
        char* listing = NULL;
        size_t llen = 0;
        if (zip_listing(set, f->path, &listing, &llen) != 0) return -1;
        mask |= abis_from_listing(listing, llen);
        free(listing);
    }
    if (bases == 0) {
        soulx_log(SOULX_LOG_ERROR, "only split APKs found: install the complete set (.apks/.xapk/.apkm, or a "
                 "directory that also holds the base APK)");
        return -1;
    }
    if (bases > 1) {
        soulx_log(SOULX_LOG_ERROR, "found %d base APKs: the input mixes several apps", bases);
        return -1;
    }
    const char* package = set->info.package;
    for (int i = 0; i < set->apk_count; i++) {
        if (strcmp(set->apks[i].package, package) != 0) {
            soulx_log(SOULX_LOG_ERROR, "%s belongs to %s, not %s", path_base(set->apks[i].path),
                     set->apks[i].package, package);
            return -1;
        }
        set->apks[i].skip = split_abi(set->apks[i].split) > 0;     // config.<abi> other than arm64_v8a
    }
    if (set->json_package[0] && strcmp(set->json_package, package) != 0) {
        soulx_log(SOULX_LOG_WARN, "manifest.json names %s but the APK is %s; using the APK", set->json_package, package);
    }
    set->abi_mask = mask;
    abis_format(mask, set->info.abis, sizeof(set->info.abis));
    return apkset_collect_obbs(set, want_obb);
}

static int apkset_load(ApkSet* set, const char* input, bool want_obb)
{
    char abs[PATH_MAX];
    struct stat sb;
    int rc;
    if (!realpath(input, abs) || stat(abs, &sb) != 0) {
        soulx_log(SOULX_LOG_ERROR, "%s: %s", input, strerror(errno));
        return -1;
    }
    if (S_ISDIR(sb.st_mode)) {
        rc = apkset_from_dir(set, abs);
    } else if (!S_ISREG(sb.st_mode)) {
        soulx_log(SOULX_LOG_ERROR, "%s is not a regular file", input);
        return -1;
    } else if (ends_with_ci(abs, ".apk")) {
        rc = apkset_add_apk(set, abs);
    } else if (ends_with_ci(abs, ".xapk") || ends_with_ci(abs, ".apks") || ends_with_ci(abs, ".apkm") ||
               ends_with_ci(abs, ".zip")) {
        rc = apkset_from_archive(set, abs, want_obb);
    } else {
        soulx_log(SOULX_LOG_ERROR, "%s: unsupported file type (expected .apk, .xapk, .apks, .apkm or a directory of "
                 "APKs)", input);
        return -1;
    }
    if (rc == 0) rc = apkset_analyze(set, want_obb);
    if (rc != 0 && set->zip_warning[0] && !soulx_interrupted()) {
        soulx_log(SOULX_LOG_WARN, "the archive may be damaged or incomplete (an interrupted download?): %s",
                 set->zip_warning);
    }
    return rc;
}

int soulx_apk_inspect(const char* path, SoulXApkInfo* info)
{
    if (!info) return -1;
    memset(info, 0, sizeof(*info));
    if (!path || !path[0]) {
        soulx_log(SOULX_LOG_ERROR, "no APK file given");
        return -1;
    }
    ApkSet* set = calloc(1, sizeof(*set));
    if (!set) {
        soulx_log(SOULX_LOG_ERROR, "out of memory");
        return -1;
    }
    set->log_level = SOULX_LOG_DEBUG;        // the install that usually follows says "Unpacking"
    int rc = apkset_load(set, path, false);
    if (rc == 0) *info = set->info;
    apkset_free(set);
    return rc;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

// versionCode of an installed package; -1 when it is not installed or unknown.
static int installed_version(const char* serial, const char* package, long long* version)
{
    char cmd[512], *out = NULL;
    *version = -1;
    if (!fmt_ok(snprintf(cmd, sizeof(cmd), "dumpsys package %s | grep -m 1 versionCode=", package), sizeof(cmd))) {
        return -1;
    }
    soulx_adb_shell(serial, cmd, &out, SHELL_TIMEOUT_MS);       // grep exits 1 when not installed
    const char* p = out ? strstr(out, "versionCode=") : NULL;
    int ret = -1;
    if (p) {
        char* end;
        p += strlen("versionCode=");
        errno = 0;
        long long v = strtoll(p, &end, 10);
        if (end != p && errno == 0) {
            *version = v;
            ret = 0;
        }
    }
    free(out);
    return ret;
}

// "2.7.3 (73)", or "(version 73)" without a version name: the wording of the CLI.
static void version_label(const SoulXApkInfo* info, char* buf, size_t cap)
{
    int n = info->version_name[0] ? snprintf(buf, cap, "%s (%lld)", info->version_name, info->version_code)
                                  : snprintf(buf, cap, "(version %lld)", info->version_code);
    if (!fmt_ok(n, cap)) (void)snprintf(buf, cap, "(version %lld)", info->version_code);    // display only
}

static void install_hint(const char* msg, const char* package)
{
    if (strstr(msg, "INSTALL_FAILED_UPDATE_INCOMPATIBLE")) {
        soulx_log(SOULX_LOG_ERROR, "the installed %s is signed with a different key; remove it first: soulx uninstall %s",
                 package, package);
    } else if (strstr(msg, "INSTALL_FAILED_VERSION_DOWNGRADE")) {
        soulx_log(SOULX_LOG_ERROR, "a newer %s is installed; remove it first: soulx uninstall %s", package, package);
    } else if (strstr(msg, "INSTALL_FAILED_NO_MATCHING_ABIS")) {
        soulx_log(SOULX_LOG_ERROR, "%s has no arm64-v8a native code; SoulX's Android 17 images run 64-bit ARM apps only",
                 package);
    } else if (strstr(msg, "INSTALL_FAILED_INSUFFICIENT_STORAGE")) {
        soulx_log(SOULX_LOG_ERROR, "the instance's data partition is full; create one with more storage (soulx create --data GB)");
    } else if (strstr(msg, "INSTALL_FAILED_OLDER_SDK")) {
        soulx_log(SOULX_LOG_ERROR, "%s needs a newer Android version than this image", package);
    }
}

// True when the `stat -c '%s %n'` listing has <name> with this size.
static bool remote_has(const char* listing, const char* name, uint64_t size)
{
    const char* p = listing;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        char line[PATH_MAX];
        if (ll < sizeof(line)) {
            memcpy(line, p, ll);
            line[ll] = '\0';
            if (ll && line[ll - 1] == '\r') line[ll - 1] = '\0';
            char* end;
            errno = 0;
            unsigned long long v = strtoull(line, &end, 10);
            if (end != line && errno == 0 && *end == ' ' && v == size && strcmp(path_base(end + 1), name) == 0) {
                return true;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

// An OBB file about to be copied is still a regular file; its size. For a directory input it must
// still be reached without symbolic links (checked again: the folder may have changed since).
static bool host_obb_ok(const ApkSet* set, const char* package, const char* path, uint64_t* size)
{
    struct stat sb;
    bool ok;
    if (set->dir[0]) {
        int dfd = open_obb_dir(set->dir, package, true);
        ok = dfd >= 0 && fstatat(dfd, path_base(path), &sb, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(sb.st_mode);
        if (dfd >= 0) close(dfd);
    } else {
        ok = lstat(path, &sb) == 0 && S_ISREG(sb.st_mode);
    }
    if (ok) *size = (uint64_t)sb.st_size;
    return ok;
}

// Unpacks OBB entries of the archive into the scratch directory and adds them to set->obbs.
static int obb_unpack(ApkSet* set, const char* const* entries, int n, uint64_t bytes, bool sizes_known)
{
    if (n == 0) return 0;
    uint64_t total = 0;
    if (!sizes_known && zip_total_size(set, set->zip, &total) == 0 && total > bytes) bytes = total;
    if (check_free_space(set, set->zip, bytes) != 0) return -1;
    soulx_log(SOULX_LOG_INFO, "Unpacking %d OBB file%s (%.0f MB) from %s...", n, n == 1 ? "" : "s",
             (double)bytes / (1024.0 * 1024.0), path_base(set->zip));
    char outdir[PATH_MAX];
    if (apkset_extract(set, set->zip, entries, n, outdir, sizeof(outdir)) != 0) return -1;
    for (int i = 0; i < n; i++) {
        char p[PATH_MAX];
        if (!fmt_ok(snprintf(p, sizeof(p), "%s/%s", outdir, entries[i]), sizeof(p)) || apkset_add_obb(set, p) != 0) {
            return -1;
        }
    }
    return 0;
}

// Copies the app's OBB files to /sdcard/Android/obb/<package>/. Files the device already has with
// the same name and size are kept, unless force (an explicit reinstall of the installed version);
// OBB entries of an archive are only unpacked when they are actually copied.
static int obb_sync(const char* serial, const char* package, ApkSet* set, bool force)
{
    char remote[320], dest[330], cmd[800], msg[512];
    if (!fmt_ok(snprintf(remote, sizeof(remote), "/sdcard/Android/obb/%s", package), sizeof(remote)) ||
        !fmt_ok(snprintf(dest, sizeof(dest), "%s/", remote), sizeof(dest)) ||
        !fmt_ok(snprintf(cmd, sizeof(cmd), "mkdir -p %s && stat -c '%%s %%n' %s/* 2>/dev/null", remote, remote),
                sizeof(cmd))) {
        return -1;
    }
    char* existing = NULL;
    soulx_adb_shell(serial, cmd, &existing, SHELL_TIMEOUT_MS);  // stat fails on an empty directory: fine

    const char* want[OBB_MAX];
    int nwant = 0;
    uint64_t want_bytes = 0;
    bool sizes_known = true;
    for (int i = 0; i < set->obb_entry_count; i++) {
        const ObbEntry* o = &set->obb_entries[i];
        if (!force && o->size != SIZE_UNKNOWN && existing && remote_has(existing, path_base(o->name), o->size)) {
            soulx_log(SOULX_LOG_DEBUG, "%s is already on the device", path_base(o->name));
            continue;
        }
        want[nwant++] = o->name;
        if (o->size == SIZE_UNKNOWN) sizes_known = false;
        else want_bytes += o->size;
    }
    if (obb_unpack(set, want, nwant, want_bytes, sizes_known) != 0) {
        free(existing);
        return -1;
    }

    const char* args[OBB_MAX + 3];
    int k = 0, n = 0;
    uint64_t bytes = 0;
    args[k++] = "push";
    for (int i = 0; i < set->obb_count; i++) {
        uint64_t size = 0;
        if (!host_obb_ok(set, package, set->obbs[i], &size)) {
            soulx_log(SOULX_LOG_ERROR, "%s is no longer a regular file; not copying it", set->obbs[i]);
            free(existing);
            return -1;
        }
        if (!force && existing && remote_has(existing, path_base(set->obbs[i]), size)) {
            soulx_log(SOULX_LOG_DEBUG, "%s is already on the device", path_base(set->obbs[i]));
            continue;
        }
        args[k++] = set->obbs[i];
        bytes += size;
        n++;
    }
    free(existing);
    if (n == 0) {
        soulx_log(SOULX_LOG_INFO, "OBB data of %s is already on the device", package);
        return 0;
    }
    args[k++] = dest;
    args[k] = NULL;
    soulx_log(SOULX_LOG_INFO, "Copying %d OBB file%s (%.0f MB) to %s...", n, n == 1 ? "" : "s",
             (double)bytes / (1024.0 * 1024.0), dest);
    SoulXCmdResult res;
    memset(&res, 0, sizeof(res));
    int rc = soulx_adb(serial, args, &res, OBB_TIMEOUT_MS);
    if (rc != 0) {
        soulx_log(SOULX_LOG_ERROR, "copying OBB files failed: %s", tool_msg(&res, msg, sizeof(msg)));
    }
    soulx_cmd_result_free(&res);
    return rc == 0 ? 0 : -1;
}

int soulx_install_path(const char* serial, const char* path, SoulXApkInfo* info_out, bool reinstall)
{
    if (info_out) memset(info_out, 0, sizeof(*info_out));
    if (!serial_valid(serial)) {
        soulx_log(SOULX_LOG_ERROR, "install: invalid device serial");
        return -1;
    }
    if (!path || !path[0]) {
        soulx_log(SOULX_LOG_ERROR, "install: no file given");
        return -1;
    }
    ApkSet* set = calloc(1, sizeof(*set));
    if (!set) {
        soulx_log(SOULX_LOG_ERROR, "out of memory");
        return -1;
    }
    int ret = -1;
    set->log_level = SOULX_LOG_INFO;
    if (apkset_load(set, path, true) != 0) goto out;

    const SoulXApkInfo* info = &set->info;
    if (info_out) *info_out = *info;
    if (info->abis[0] && !(set->abi_mask & ABI_ARM64_BIT)) {
        soulx_log(SOULX_LOG_ERROR, "%s only ships native code for %s; SoulX's Android 17 images run 64-bit arm64-v8a "
                 "apps only (INSTALL_FAILED_NO_MATCHING_ABIS)", info->package, info->abis);
        goto out;
    }

    // Is this version installed already? Asked before installing: it decides whether the install is
    // skipped and whether an explicit reinstall also refreshes the OBB data.
    char label[96];
    version_label(info, label, sizeof(label));
    bool has_obb = set->obb_count > 0 || set->obb_entry_count > 0;
    long long installed = -1;
    bool same_version = (!reinstall || has_obb) && installed_version(serial, info->package, &installed) == 0 &&
                        installed == info->version_code;
    bool skip = !reinstall && same_version;
    if (skip) {
        soulx_log(SOULX_LOG_INFO, "%s %s is already installed", info->package, label);
    } else {
        const char* args[APK_SET_MAX + 5];
        char msg[512];
        int k = 0, n = 0;
        uint64_t bytes = 0;
        args[k++] = NULL;                                      // verb, set below
        args[k++] = "-r";
        args[k++] = "-g";
        for (int pass = 0; pass < 2; pass++) {                 // base APK first, then the splits
            for (int i = 0; i < set->apk_count; i++) {
                const ApkFile* f = &set->apks[i];
                struct stat sb;
                if ((pass == 0) != (i == set->base)) continue;
                if (f->skip) {
                    soulx_log(SOULX_LOG_DEBUG, "skipping %s (split for another ABI)", path_base(f->path));
                    continue;
                }
                if (stat(f->path, &sb) == 0) bytes += (uint64_t)sb.st_size;
                args[k++] = f->path;
                n++;
            }
        }
        args[0] = (n == 1) ? "install" : "install-multiple";
        args[k] = NULL;
        soulx_log(SOULX_LOG_INFO, "Installing %s %s (%d APK%s, %.0f MB)...", info->package, info->version_name, n,
                 n == 1 ? "" : "s", (double)bytes / (1024.0 * 1024.0));

        SoulXCmdResult res;
        memset(&res, 0, sizeof(res));
        int rc = soulx_adb(serial, args, &res, INSTALL_TIMEOUT_MS);
        bool failed = rc != 0 || (res.out && strstr(res.out, "Failure")) || (res.err && strstr(res.err, "Failure ["));
        if (failed) {
            tool_msg(&res, msg, sizeof(msg));
            soulx_log(SOULX_LOG_ERROR, "installing %s failed: %s", info->package, msg);
            install_hint(msg, info->package);
            soulx_cmd_result_free(&res);
            goto out;
        }
        soulx_cmd_result_free(&res);
        // the caller reports the result (the CLI prints "Installed ..." itself)
        soulx_log(SOULX_LOG_DEBUG, "installed %s %s", info->package, label);
    }
    // An explicit reinstall of the installed version also refreshes the OBB data (a patched OBB may
    // keep its name and size); otherwise only missing or resized OBB files are copied.
    if (has_obb && obb_sync(serial, info->package, set, reinstall && same_version) != 0) goto out;
    ret = 0;

out:
    apkset_free(set);
    return ret;
}

// ---------------------------------------------------------------------------
// Launch / stop / uninstall / foreground / list
// ---------------------------------------------------------------------------

static bool has_line_prefix(const char* text, const char* prefix)
{
    size_t n = strlen(prefix);
    for (const char* p = text; p && *p;) {
        if (strncmp(p, prefix, n) == 0) return true;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return false;
}

static bool component_valid(const char* c)
{
    const char* slash = strchr(c, '/');
    if (!slash || slash == c || !slash[1] || strlen(c) > 511) return false;
    for (const char* p = c; *p; p++) {
        if (!is_alnum(*p) && *p != '.' && *p != '_' && *p != '$' && *p != '/') return false;
    }
    return strchr(slash + 1, '/') == NULL;
}

// Last non-empty line containing '/' (cmd package resolve-activity --brief).
static void last_component(const char* out, char* comp, size_t cap)
{
    comp[0] = '\0';
    for (const char* p = out; p && *p;) {
        const char* nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        while (ll > 0 && (p[ll - 1] == '\r' || p[ll - 1] == ' ')) ll--;
        while (ll > 0 && *p == ' ') {
            p++;
            ll--;
        }
        if (ll > 0 && ll < cap && memchr(p, '/', ll)) {
            memcpy(comp, p, ll);
            comp[ll] = '\0';
        }
        p = nl ? nl + 1 : NULL;
    }
}

int soulx_launch_package(const char* serial, const char* package)
{
    if (!serial_valid(serial) || !soulx_package_name_valid(package)) {
        soulx_log(SOULX_LOG_ERROR, "launch: invalid package name '%s'", package ? package : "");
        return -1;
    }
    char cmd[1024], comp[512], *out = NULL;
    if (!fmt_ok(snprintf(cmd, sizeof(cmd), "cmd package resolve-activity --brief -a android.intent.action.MAIN "
                         "-c android.intent.category.LAUNCHER %s", package), sizeof(cmd))) {
        return -1;
    }
    comp[0] = '\0';
    if (soulx_adb_shell(serial, cmd, &out, SHELL_TIMEOUT_MS) == 0 && out) last_component(out, comp, sizeof(comp));
    free(out);
    out = NULL;

    // The launcher's own intent (MAIN + LAUNCHER, NEW_TASK | RESET_TASK_IF_NEEDED): when the app is
    // already running its task is brought to the front. An intent with only the component would
    // not match the task's root intent, and a standard-launchMode activity would be started a
    // second time on top of the running one.
    if (comp[0] && component_valid(comp) &&
        fmt_ok(snprintf(cmd, sizeof(cmd), "am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER "
                        "-f 0x10200000 -n '%s'", comp), sizeof(cmd))) {
        int rc = soulx_adb_shell(serial, cmd, &out, LAUNCH_TIMEOUT_MS);
        bool ok = rc == 0 && out && !has_line_prefix(out, "Error");
        if (!ok) soulx_log(SOULX_LOG_DEBUG, "am start %s failed; trying monkey", comp);
        free(out);
        out = NULL;
        if (ok) return 0;
    }

    if (!fmt_ok(snprintf(cmd, sizeof(cmd), "monkey -p %s -c android.intent.category.LAUNCHER 1", package),
                sizeof(cmd))) {
        return -1;
    }
    soulx_adb_shell(serial, cmd, &out, LAUNCH_TIMEOUT_MS);
    bool ok = out && strstr(out, "Events injected: 1");
    free(out);
    if (!ok) {
        soulx_log(SOULX_LOG_ERROR, "cannot launch %s: it is not installed or has no launcher activity", package);
        return -1;
    }
    return 0;
}

int soulx_force_stop(const char* serial, const char* package)
{
    if (!serial_valid(serial) || !soulx_package_name_valid(package)) {
        soulx_log(SOULX_LOG_ERROR, "stop: invalid package name '%s'", package ? package : "");
        return -1;
    }
    char cmd[512], *out = NULL;
    if (!fmt_ok(snprintf(cmd, sizeof(cmd), "am force-stop %s", package), sizeof(cmd))) return -1;
    int rc = soulx_adb_shell(serial, cmd, &out, SHELL_TIMEOUT_MS);
    free(out);
    if (rc != 0) soulx_log(SOULX_LOG_ERROR, "cannot stop %s", package);
    return rc == 0 ? 0 : -1;
}

int soulx_uninstall_package(const char* serial, const char* package)
{
    if (!serial_valid(serial) || !soulx_package_name_valid(package)) {
        soulx_log(SOULX_LOG_ERROR, "uninstall: invalid package name '%s'", package ? package : "");
        return -1;
    }
    const char* args[] = { "uninstall", package, NULL };
    SoulXCmdResult res;
    char msg[512];
    memset(&res, 0, sizeof(res));
    int rc = soulx_adb(serial, args, &res, 2 * 60 * 1000);
    bool ok = rc == 0 && res.out && strstr(res.out, "Success");
    if (!ok) {
        tool_msg(&res, msg, sizeof(msg));
        // Android answers DELETE_FAILED_INTERNAL_ERROR both for an unknown package and for a
        // preinstalled system app: ask which one it is.
        long long version = -1;
        if (strstr(msg, "DELETE_FAILED_INTERNAL_ERROR") || strstr(msg, "not installed")) {
            if (installed_version(serial, package, &version) != 0) {
                soulx_log(SOULX_LOG_ERROR, "%s is not installed", package);
            } else {
                soulx_log(SOULX_LOG_ERROR, "%s cannot be uninstalled (it is part of the system image?)", package);
            }
        } else {
            soulx_log(SOULX_LOG_ERROR, "uninstalling %s failed: %s", package, msg);
        }
    }
    soulx_cmd_result_free(&res);
    return ok ? 0 : -1;
}

// Package of "...{<hash> u0 <package>/<activity> t12}" in a dumpsys line.
static int record_package(const char* line, size_t len, char* package, size_t cap)
{
    const char* end = line + len;
    const char* p = memchr(line, '{', len);
    if (!p) return -1;
    p++;
    for (;;) {
        while (p < end && *p == ' ') p++;
        const char* tok = p;
        while (p < end && *p != ' ' && *p != '}') p++;
        if (p == tok) return -1;
        const char* slash = memchr(tok, '/', (size_t)(p - tok));
        if (slash) {
            size_t n = (size_t)(slash - tok);
            if (n == 0 || n >= cap) return -1;
            memcpy(package, tok, n);
            package[n] = '\0';
            return soulx_package_name_valid(package) ? 0 : -1;
        }
    }
}

int soulx_foreground_package(const char* serial, char* package, size_t cap)
{
    static const char* const queries[] = {
        "dumpsys activity activities | grep -E 'ResumedActivity'",
        "dumpsys window | grep -E 'mCurrentFocus|mFocusedApp'",
    };
    if (!package || cap == 0) return -1;
    package[0] = '\0';
    if (!serial_valid(serial)) return -1;
    for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++) {
        char* out = NULL;
        soulx_adb_shell(serial, queries[q], &out, SHELL_TIMEOUT_MS);
        for (const char* p = out; p && *p;) {
            const char* nl = strchr(p, '\n');
            size_t ll = nl ? (size_t)(nl - p) : strlen(p);
            if (record_package(p, ll, package, cap) == 0) {
                free(out);
                return 0;
            }
            p = nl ? nl + 1 : NULL;
        }
        free(out);
    }
    package[0] = '\0';
    return -1;
}

static int cmp_str(const void* a, const void* b)
{
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

int soulx_list_packages(const char* serial, bool third_party_only, char** out)
{
    if (!out) return -1;
    *out = NULL;
    if (!serial_valid(serial)) return -1;
    char* raw = NULL;
    int rc = soulx_adb_shell(serial, third_party_only ? "pm list packages -3" : "pm list packages", &raw,
                            SHELL_TIMEOUT_MS);
    if (rc != 0 || !raw) {
        soulx_log(SOULX_LOG_ERROR, "cannot list packages (is the instance fully booted?)");
        free(raw);
        return -1;
    }
    size_t lines = 1, total = 0;
    for (const char* p = raw; *p; p++) {
        if (*p == '\n') lines++;
    }
    char** names = calloc(lines, sizeof(*names));
    if (!names) {
        free(raw);
        return -1;
    }
    size_t n = 0;
    for (char* p = raw; p && *p;) {
        char* nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        size_t ll = strlen(p);
        while (ll > 0 && (p[ll - 1] == '\r' || p[ll - 1] == ' ')) p[--ll] = '\0';
        if (strncmp(p, "package:", 8) == 0 && soulx_package_name_valid(p + 8) && n < lines) {
            names[n++] = p + 8;
            total += ll - 8 + 1;
        }
        p = nl ? nl + 1 : NULL;
    }
    qsort(names, n, sizeof(*names), cmp_str);
    char* joined = malloc(total + 1);
    if (!joined) {
        free(names);
        free(raw);
        return -1;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        size_t l = strlen(names[i]);
        memcpy(joined + o, names[i], l);
        o += l;
        joined[o++] = '\n';
    }
    joined[o] = '\0';
    free(names);
    free(raw);
    *out = joined;
    return 0;
}
