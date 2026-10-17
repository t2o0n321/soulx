/*
 * SoulX Android Emulator - image helpers and template matching (macOS script engine)
 *
 * PNG/JPEG decoding and PNG encoding use the vendored stb libraries, compiled only here.
 *
 * Template matching = zero-mean normalized cross-correlation (ZNCC) on grayscale:
 *   1. coarse level: screen and template are box-filtered into s x s cells with
 *      s = clamp(min(tw, th) / 12, 1, 8), raised for long thin templates (text) while the
 *      coarse search would exceed its budget. ZNCC is evaluated at every coarse position with
 *      row-wise multiply-accumulate loops; window sums come from running column sums
 *      (opaque templates) or from masked sums (templates with transparent pixels).
 *      The best positions (greedy 3x3 suppression) become candidates.
 *   2. each candidate is refined at full resolution within +-(s + 1) pixels,
 *   3. refined positions are scored exactly (double precision), checked against the
 *      threshold and the optional colour tolerance and, for find_all, de-duplicated with
 *      non-maximum suppression.
 * A template whose brightness is (almost) uniform (stddev < 2 gray levels) has no meaningful
 * ZNCC; it is scored by its RMS gray difference instead: score = 1 - rms / 32.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "image.h"

// ---------------------------------------------------------------------------
// stb (vendored, single translation unit; restricted to PNG + JPEG decoding)
// ---------------------------------------------------------------------------

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS SOULX_IMAGE_MAX_DIM
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

#define MATCH_FLAT_VAR       4.0      // template variance (gray levels^2) below this: flat template
#define MATCH_WIN_FLAT_VAR   0.25     // window variance below this: uniform window, ZNCC undefined
#define MATCH_FLAT_SCALE     32.0     // flat templates: score = 1 - rms gray difference / 32
#define MATCH_COARSE_BUDGET  400e6    // multiply-adds allowed for the coarse search
#define MATCH_REFINE_BUDGET  48e6     // multiply-adds spent on full-resolution refinement
#define MATCH_MIN_CANDS      8
#define MATCH_MAX_FIND_CANDS 2048
#define MATCH_MAX_CANDS      4096
#define MATCH_NMS_OVERLAP    0.3      // find_all: drop a match covering > 30% of a better one
#define MATCH_SCORE_EPS      1e-9

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

static _Thread_local char g_image_err[256];

static int img_fail(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

static int img_fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_image_err, sizeof(g_image_err), fmt, ap);
    va_end(ap);
    return -1;
}

const char* soulx_image_error(void) {
    return g_image_err[0] ? g_image_err : "unknown error";
}

// ---------------------------------------------------------------------------
// Basic helpers
// ---------------------------------------------------------------------------

static bool image_valid(const SoulXImage* img) {
    return img && img->rgba && img->w > 0 && img->h > 0 &&
           img->w <= SOULX_IMAGE_MAX_DIM && img->h <= SOULX_IMAGE_MAX_DIM;
}

// BT.601 luma in integer arithmetic; weights sum to 256 so the result stays in 0..255
static inline int gray_of(const uint8_t* p) {
    return (77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8;
}

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void soulx_match_opts_init(SoulXMatchOpts* o) {
    if (!o) return;
    memset(o, 0, sizeof(*o));
    o->threshold = SOULX_MATCH_DEFAULT_THRESHOLD;
    o->color_tolerance = SOULX_MATCH_DEFAULT_COLOR_TOLERANCE;
    o->scale = 1.0;
}

int soulx_image_alloc(SoulXImage* img, int w, int h) {
    if (!img) return img_fail("invalid arguments");
    memset(img, 0, sizeof(*img));
    if (w <= 0 || h <= 0 || w > SOULX_IMAGE_MAX_DIM || h > SOULX_IMAGE_MAX_DIM) {
        return img_fail("invalid image size %dx%d", w, h);
    }
    img->rgba = calloc((size_t)w * (size_t)h, 4);
    if (!img->rgba) return img_fail("out of memory (%dx%d image)", w, h);
    img->w = w;
    img->h = h;
    return 0;
}

int32_t soulx_image_pixel(const SoulXImage* img, int x, int y) {
    if (!image_valid(img) || x < 0 || y < 0 || x >= img->w || y >= img->h) return -1;
    const uint8_t* p = img->rgba + ((size_t)y * (size_t)img->w + (size_t)x) * 4;
    return (int32_t)(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2]);
}

int soulx_image_crop(const SoulXImage* src, int x, int y, int w, int h, SoulXImage* out) {
    if (!out || !image_valid(src)) return img_fail("invalid arguments");
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x > src->w - w || y > src->h - h) {
        return img_fail("crop rectangle (%d, %d, %d, %d) is not inside the %dx%d image",
                        x, y, w, h, src->w, src->h);
    }
    SoulXImage tmp;
    if (soulx_image_alloc(&tmp, w, h) != 0) return -1;
    for (int row = 0; row < h; row++) {
        memcpy(tmp.rgba + (size_t)row * (size_t)w * 4,
               src->rgba + ((size_t)(y + row) * (size_t)src->w + (size_t)x) * 4,
               (size_t)w * 4);
    }
    if (out == src) free(out->rgba);   // in-place crop: release the old pixels
    *out = tmp;
    return 0;
}

// Bilinear rescale (used for multi-scale matching). Alpha is interpolated with the colour, so a masked
// template stays masked (edge pixels become partially transparent, which the matcher's alpha >= 128 test rounds).
int soulx_image_scale(const SoulXImage* src, double factor, SoulXImage* out) {
    if (!out || !image_valid(src)) return img_fail("invalid arguments");
    if (!(factor > 0.0)) return img_fail("scale factor must be > 0");
    int dw = (int)lround(src->w * factor), dh = (int)lround(src->h * factor);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > SOULX_IMAGE_MAX_DIM || dh > SOULX_IMAGE_MAX_DIM) return img_fail("scaled image too large (%dx%d)", dw, dh);
    SoulXImage tmp;
    if (soulx_image_alloc(&tmp, dw, dh) != 0) return -1;
    const double sx = (double)src->w / dw, sy = (double)src->h / dh;
    for (int y = 0; y < dh; y++) {
        double fy = (y + 0.5) * sy - 0.5;
        int y0 = (int)floor(fy);
        double wy = fy - y0;
        int y0c = y0 < 0 ? 0 : (y0 >= src->h ? src->h - 1 : y0);
        int y1c = (y0 + 1) < 0 ? 0 : ((y0 + 1) >= src->h ? src->h - 1 : y0 + 1);
        for (int x = 0; x < dw; x++) {
            double fx = (x + 0.5) * sx - 0.5;
            int x0 = (int)floor(fx);
            double wx = fx - x0;
            int x0c = x0 < 0 ? 0 : (x0 >= src->w ? src->w - 1 : x0);
            int x1c = (x0 + 1) < 0 ? 0 : ((x0 + 1) >= src->w ? src->w - 1 : x0 + 1);
            const uint8_t* p00 = src->rgba + ((size_t)y0c * src->w + x0c) * 4;
            const uint8_t* p01 = src->rgba + ((size_t)y0c * src->w + x1c) * 4;
            const uint8_t* p10 = src->rgba + ((size_t)y1c * src->w + x0c) * 4;
            const uint8_t* p11 = src->rgba + ((size_t)y1c * src->w + x1c) * 4;
            uint8_t* d = tmp.rgba + ((size_t)y * dw + x) * 4;
            for (int c = 0; c < 4; c++) {
                double top = p00[c] * (1 - wx) + p01[c] * wx;
                double bot = p10[c] * (1 - wx) + p11[c] * wx;
                double v = top * (1 - wy) + bot * wy + 0.5;
                d[c] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        }
    }
    if (out == src) free(out->rgba);
    *out = tmp;
    return 0;
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

static int read_whole_file(const char* path, unsigned char** data, size_t* len) {
    *data = NULL;
    *len = 0;
    // O_NONBLOCK: never hang on a FIFO; regular files are checked right after
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return img_fail("cannot open '%s': %s", path, strerror(errno));

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        return img_fail("cannot stat '%s': %s", path, strerror(e));
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return img_fail("'%s' is not a regular file", path);
    }
    if (st.st_size <= 0) {
        close(fd);
        return img_fail("'%s' is empty", path);
    }
    if ((unsigned long long)st.st_size > SOULX_IMAGE_MAX_FILE) {
        close(fd);
        return img_fail("'%s' is too large (%lld bytes)", path, (long long)st.st_size);
    }

    size_t want = (size_t)st.st_size;
    unsigned char* buf = malloc(want);
    if (!buf) {
        close(fd);
        return img_fail("out of memory reading '%s'", path);
    }
    size_t got = 0;
    while (got < want) {
        ssize_t r = read(fd, buf + got, want - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            free(buf);
            close(fd);
            return img_fail("cannot read '%s': %s", path, strerror(e));
        }
        if (r == 0) break;   // file shrank while reading
        got += (size_t)r;
    }
    close(fd);
    if (got == 0) {
        free(buf);
        return img_fail("'%s' is empty", path);
    }
    *data = buf;
    *len = got;
    return 0;
}

static int write_file_atomic(const char* path, const unsigned char* data, size_t len) {
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp-XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return img_fail("path too long: %s", path);

    int fd = mkostemp(tmp, O_CLOEXEC);
    if (fd < 0) return img_fail("cannot write '%s': %s", path, strerror(errno));

    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd);
            unlink(tmp);
            return img_fail("cannot write '%s': %s", path, strerror(e));
        }
        off += (size_t)w;
    }
    if (fchmod(fd, 0644) != 0 || close(fd) != 0) {
        int e = errno;
        unlink(tmp);
        return img_fail("cannot write '%s': %s", path, strerror(e));
    }
    if (rename(tmp, path) != 0) {
        int e = errno;
        unlink(tmp);
        return img_fail("cannot write '%s': %s", path, strerror(e));
    }
    return 0;
}

int soulx_image_load_mem(const unsigned char* data, size_t len, SoulXImage* out) {
    if (!out) return img_fail("invalid arguments");
    memset(out, 0, sizeof(*out));
    if (!data || len == 0) return img_fail("empty image data");
    if (len > SOULX_IMAGE_MAX_FILE) return img_fail("image data too large (%zu bytes)", len);

    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    if (!px) {
        const char* why = stbi_failure_reason();
        return img_fail("not a valid PNG/JPEG image (%s)", why ? why : "decode error");
    }
    if (w <= 0 || h <= 0 || w > SOULX_IMAGE_MAX_DIM || h > SOULX_IMAGE_MAX_DIM) {
        stbi_image_free(px);
        return img_fail("unsupported image size %dx%d", w, h);
    }
    out->w = w;
    out->h = h;
    out->rgba = px;   // STBI_MALLOC is malloc: compatible with free() / soulx_image_free()
    return 0;
}

int soulx_image_load_png(const char* path, SoulXImage* out) {
    if (!out) return img_fail("invalid arguments");
    memset(out, 0, sizeof(*out));
    if (!path || !*path) return img_fail("no image path given");

    unsigned char* data = NULL;
    size_t len = 0;
    if (read_whole_file(path, &data, &len) != 0) return -1;
    int rc = soulx_image_load_mem(data, len, out);
    free(data);
    if (rc != 0) {
        char why[sizeof(g_image_err)];
        snprintf(why, sizeof(why), "%s", g_image_err);
        return img_fail("'%s': %s", path, why);
    }
    return 0;
}

int soulx_image_save_png(const char* path, const SoulXImage* img) {
    if (!path || !*path) return img_fail("no output path given");
    if (!image_valid(img)) return img_fail("invalid image");

    int len = 0;
    unsigned char* png = stbi_write_png_to_mem(img->rgba, img->w * 4, img->w, img->h, 4, &len);
    if (!png || len <= 0) {
        free(png);
        return img_fail("cannot encode PNG (out of memory)");
    }
    int rc = write_file_atomic(path, png, (size_t)len);
    STBIW_FREE(png);
    return rc;
}

// ---------------------------------------------------------------------------
// Matching: levels
// ---------------------------------------------------------------------------

// Template at one pyramid level. Cell values are sums of (gray - 128) over s x s pixels
// (computed as the mean of the opaque pixels times s*s), matching the screen level.
typedef struct {
    int w, h;          // cells
    int unit;          // pixels per cell (s * s)
    float* t;          // zero-mean cell values, 0 for transparent cells (w * h)
    uint8_t* m;        // 1 = opaque cell; NULL when every cell is opaque
    float* mf;         // the same mask as 0.0 / 1.0 (NULL when every cell is opaque)
    int n;             // opaque cells
    double var;        // sum of squared zero-mean values
    double mean;       // mean opaque cell value
} TLevel;

typedef struct {
    int x, y;          // top-left position (level cells for candidates, pixels once refined)
    double score;
} Cand;

static void tlevel_free(TLevel* L) {
    free(L->t);
    free(L->m);
    free(L->mf);
    memset(L, 0, sizeof(*L));
}

static int tlevel_build(const SoulXImage* tm, int s, TLevel* L) {
    memset(L, 0, sizeof(*L));
    const int cw = tm->w / s, ch = tm->h / s, unit = s * s;
    const size_t cells = (size_t)cw * (size_t)ch;
    double* val = malloc(cells * sizeof(double));
    uint8_t* m = malloc(cells);
    L->t = malloc(cells * sizeof(float));
    if (!val || !m || !L->t) {
        free(val);
        free(m);
        tlevel_free(L);
        return img_fail("out of memory");
    }

    double sum = 0.0;
    int n = 0;
    for (int by = 0; by < ch; by++) {
        for (int bx = 0; bx < cw; bx++) {
            int cnt = 0;
            long gs = 0;
            for (int yy = 0; yy < s; yy++) {
                const uint8_t* p = tm->rgba +
                    ((size_t)(by * s + yy) * (size_t)tm->w + (size_t)(bx * s)) * 4;
                for (int k = 0; k < s; k++, p += 4) {
                    if (p[3] >= 128) {
                        cnt++;
                        gs += gray_of(p);
                    }
                }
            }
            size_t idx = (size_t)by * (size_t)cw + (size_t)bx;
            if (cnt > 0 && 2 * cnt >= unit) {   // cell counts as opaque when >= half opaque
                val[idx] = ((double)gs / cnt - 128.0) * unit;
                m[idx] = 1;
                sum += val[idx];
                n++;
            } else {
                val[idx] = 0.0;
                m[idx] = 0;
            }
        }
    }

    L->w = cw;
    L->h = ch;
    L->unit = unit;
    L->n = n;
    if (n > 0) {
        L->mean = sum / n;
        double var = 0.0;
        for (size_t i = 0; i < cells; i++) {
            double t = m[i] ? val[i] - L->mean : 0.0;
            L->t[i] = (float)t;
            var += t * t;
        }
        L->var = var;
    }
    free(val);
    if ((size_t)n < cells) {
        L->m = m;
        L->mf = malloc(cells * sizeof(float));
        if (!L->mf) {
            tlevel_free(L);
            return img_fail("out of memory");
        }
        for (size_t i = 0; i < cells; i++) L->mf[i] = m[i] ? 1.0f : 0.0f;
    } else {
        free(m);
    }
    return 0;
}

// dst (cw x ch cells) = sums of (gray - 128) over s x s blocks starting at pixel (x0, y0).
// acc: scratch for cw ints (unused when s == 1).
static void level_fill(const SoulXImage* img, int x0, int y0, int cw, int ch, int s,
                       float* dst, int32_t* acc) {
    const int32_t bias = 128 * s * s;
    for (int by = 0; by < ch; by++) {
        float* row = dst + (size_t)by * (size_t)cw;
        if (s == 1) {
            const uint8_t* p = img->rgba + ((size_t)(y0 + by) * (size_t)img->w + (size_t)x0) * 4;
            for (int bx = 0; bx < cw; bx++, p += 4) row[bx] = (float)(gray_of(p) - 128);
            continue;
        }
        memset(acc, 0, (size_t)cw * sizeof(int32_t));
        for (int yy = 0; yy < s; yy++) {
            const uint8_t* p = img->rgba +
                ((size_t)(y0 + by * s + yy) * (size_t)img->w + (size_t)x0) * 4;
            for (int bx = 0; bx < cw; bx++) {
                int32_t a = 0;
                for (int k = 0; k < s; k++, p += 4) a += gray_of(p);
                acc[bx] += a;
            }
        }
        for (int bx = 0; bx < cw; bx++) row[bx] = (float)(acc[bx] - bias);
    }
}

// ---------------------------------------------------------------------------
// Matching: score maps
// ---------------------------------------------------------------------------

static void saxpy(float* restrict acc, float t, const float* restrict sp, int n) {
    for (int x = 0; x < n; x++) acc[x] += t * sp[x];
}

static void accum_sums(double* restrict ws, double* restrict wq, const float* restrict sp, int n) {
    for (int x = 0; x < n; x++) {
        double v = sp[x];
        ws[x] += v;
        wq[x] += v * v;
    }
}

// Dot products with eight independent accumulators: vectorizable without fast-math.
// With integer-valued a (|a| <= 128), a 0/1 mask and n <= 1024, the masked sums
// dot8(a, mask) and dot8_sq(a, mask) are exact.
static float dot8(const float* restrict a, const float* restrict b, int n) {
    float acc[8] = { 0 };
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        for (int k = 0; k < 8; k++) acc[k] += a[i + k] * b[i + k];
    }
    float tail = 0.0f;
    for (; i < n; i++) tail += a[i] * b[i];
    return tail + ((acc[0] + acc[4]) + (acc[1] + acc[5])) + ((acc[2] + acc[6]) + (acc[3] + acc[7]));
}

static float dot8_sq(const float* restrict a, const float* restrict w, int n) {
    float acc[8] = { 0 };
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        for (int k = 0; k < 8; k++) acc[k] += w[i + k] * a[i + k] * a[i + k];
    }
    float tail = 0.0f;
    for (; i < n; i++) tail += w[i] * a[i] * a[i];
    return tail + ((acc[0] + acc[4]) + (acc[1] + acc[5])) + ((acc[2] + acc[6]) + (acc[3] + acc[7]));
}

// Flat templates: similarity from the RMS difference (gray levels) to the template.
static double flat_score(double ssd, double n, double unit) {
    double rms = ssd > 0.0 ? sqrt(ssd / n) / unit : 0.0;
    double sc = 1.0 - rms / MATCH_FLAT_SCALE;
    return sc > 0.0 ? sc : 0.0;
}

// Similarity from window statistics: s1 = sum, s2 = sum of squares over the n opaque cells,
// num = correlation with the zero-mean template.
static double score_from_sums(const TLevel* T, bool flat, double num, double s1, double s2) {
    const double n = (double)T->n;
    const double unit = (double)T->unit;
    if (flat) {
        // sum (S - T)^2 with T = t + mean: s2 - 2 (num + mean s1) + sum t^2 + n mean^2
        double ssd = s2 - 2.0 * (num + T->mean * s1) + T->var + n * T->mean * T->mean;
        return flat_score(ssd, n, unit);
    }
    double vs = s2 - s1 * s1 / n;
    if (vs <= MATCH_WIN_FLAT_VAR * unit * unit * n || T->var <= 0.0) return 0.0;
    double sc = num / sqrt(vs * T->var);
    if (sc > 1.0) return 1.0;
    if (sc < -1.0) return -1.0;
    return sc;
}

// Score of template level T at every position of level image S (sw x sh);
// scores receives (sw - T->w + 1) * (sh - T->h + 1) values in row-major order.
static int level_scores(const float* S, int sw, int sh, const TLevel* T, bool flat, float* scores) {
    const int tw = T->w, th = T->h;
    const int nx = sw - tw + 1, ny = sh - th + 1;
    if (nx <= 0 || ny <= 0) return 0;
    const bool masked = T->m != NULL;
    // per-position dot products for narrow windows; only at full resolution, where per-row
    // float sums of integer pixel values are exact
    const bool by_position = nx < 24 && T->unit == 1 && tw <= 1024;

    float* num = malloc((size_t)nx * sizeof(float));
    double* ws = malloc((size_t)nx * sizeof(double));
    double* wq = malloc((size_t)nx * sizeof(double));
    double* cs = masked ? NULL : calloc((size_t)sw, sizeof(double));
    double* cq = masked ? NULL : calloc((size_t)sw, sizeof(double));
    if (!num || !ws || !wq || (!masked && (!cs || !cq))) {
        free(num);
        free(ws);
        free(wq);
        free(cs);
        free(cq);
        return img_fail("out of memory");
    }

    // Opaque template: column sums over the current band of th rows (exact: integer values)
    if (!masked) {
        for (int j = 0; j < th; j++) {
            const float* r = S + (size_t)j * (size_t)sw;
            for (int x = 0; x < sw; x++) {
                double v = r[x];
                cs[x] += v;
                cq[x] += v * v;
            }
        }
    }

    for (int y = 0; y < ny; y++) {
        if (!masked) {
            if (y > 0) {
                const float* old = S + (size_t)(y - 1) * (size_t)sw;
                const float* add = S + (size_t)(y + th - 1) * (size_t)sw;
                for (int x = 0; x < sw; x++) {
                    double a = add[x], o = old[x];
                    cs[x] += a - o;
                    cq[x] += a * a - o * o;
                }
            }
            double a = 0.0, b = 0.0;
            for (int i = 0; i < tw; i++) {
                a += cs[i];
                b += cq[i];
            }
            for (int x = 0; x < nx; x++) {
                ws[x] = a;
                wq[x] = b;
                if (x + 1 < nx) {
                    a += cs[x + tw] - cs[x];
                    b += cq[x + tw] - cq[x];
                }
            }
        } else {
            memset(ws, 0, (size_t)nx * sizeof(double));
            memset(wq, 0, (size_t)nx * sizeof(double));
        }
        memset(num, 0, (size_t)nx * sizeof(float));

        if (by_position) {
            // few positions per row (refinement windows): one dot product per template row
            for (int x = 0; x < nx; x++) {
                double nm = 0.0, s1 = 0.0, s2 = 0.0;
                for (int j = 0; j < th; j++) {
                    const float* srow = S + (size_t)(y + j) * (size_t)sw + (size_t)x;
                    nm += dot8(srow, T->t + (size_t)j * (size_t)tw, tw);
                    if (masked) {
                        const float* mrow = T->mf + (size_t)j * (size_t)tw;
                        s1 += dot8(srow, mrow, tw);
                        s2 += dot8_sq(srow, mrow, tw);
                    }
                }
                num[x] = (float)nm;
                if (masked) {
                    ws[x] = s1;
                    wq[x] = s2;
                }
            }
        } else {
            // many positions per row: multiply-accumulate whole rows per template cell
            for (int j = 0; j < th; j++) {
                const float* srow = S + (size_t)(y + j) * (size_t)sw;
                const float* trow = T->t + (size_t)j * (size_t)tw;
                const uint8_t* mrow = masked ? T->m + (size_t)j * (size_t)tw : NULL;
                for (int i = 0; i < tw; i++) {
                    if (mrow && !mrow[i]) continue;
                    if (trow[i] != 0.0f) saxpy(num, trow[i], srow + i, nx);
                    if (mrow) accum_sums(ws, wq, srow + i, nx);
                }
            }
        }

        float* out = scores + (size_t)y * (size_t)nx;
        for (int x = 0; x < nx; x++) {
            out[x] = (float)score_from_sums(T, flat, (double)num[x], ws[x], wq[x]);
        }
    }

    free(num);
    free(ws);
    free(wq);
    free(cs);
    free(cq);
    return 0;
}

// Exact (double precision) score of the full-resolution template at screen pixel (x, y).
static double exact_score(const SoulXImage* scr, int x, int y, const SoulXImage* tm,
                          const TLevel* F, bool flat) {
    double num = 0.0, s1 = 0.0, s2 = 0.0, ssd = 0.0;
    for (int j = 0; j < tm->h; j++) {
        const uint8_t* sp = scr->rgba + ((size_t)(y + j) * (size_t)scr->w + (size_t)x) * 4;
        const uint8_t* tp = tm->rgba + (size_t)j * (size_t)tm->w * 4;
        for (int i = 0; i < tm->w; i++, sp += 4, tp += 4) {
            if (tp[3] < 128) continue;
            double v = gray_of(sp) - 128;
            double tv = gray_of(tp) - 128;
            double t = tv - F->mean;
            num += v * t;
            s1 += v;
            s2 += v * v;
            ssd += (v - tv) * (v - tv);
        }
    }
    if (flat) return flat_score(ssd, (double)F->n, 1.0);
    return score_from_sums(F, false, num, s1, s2);
}

// Largest per-channel mean absolute RGB difference over the opaque template pixels.
static double color_distance(const SoulXImage* scr, int x, int y, const SoulXImage* tm) {
    uint64_t d[3] = { 0, 0, 0 };
    uint64_t n = 0;
    for (int j = 0; j < tm->h; j++) {
        const uint8_t* sp = scr->rgba + ((size_t)(y + j) * (size_t)scr->w + (size_t)x) * 4;
        const uint8_t* tp = tm->rgba + (size_t)j * (size_t)tm->w * 4;
        for (int i = 0; i < tm->w; i++, sp += 4, tp += 4) {
            if (tp[3] < 128) continue;
            for (int c = 0; c < 3; c++) d[c] += (uint64_t)abs((int)sp[c] - (int)tp[c]);
            n++;
        }
    }
    if (n == 0) return 0.0;
    uint64_t mx = d[0] > d[1] ? d[0] : d[1];
    if (d[2] > mx) mx = d[2];
    return (double)mx / (double)n;
}

// ---------------------------------------------------------------------------
// Matching: candidates
// ---------------------------------------------------------------------------

static int cand_cmp(const void* a, const void* b) {
    const Cand* p = a;
    const Cand* q = b;
    if (p->score != q->score) return p->score > q->score ? -1 : 1;
    if (p->y != q->y) return p->y < q->y ? -1 : 1;
    return (p->x > q->x) - (p->x < q->x);
}

// Min-heap of the best K candidates (root = weakest kept candidate)
static void heap_push(Cand* h, int* count, int cap, Cand c) {
    if (*count < cap) {
        int i = (*count)++;
        h[i] = c;
        while (i > 0) {
            int p = (i - 1) / 2;
            if (h[p].score <= h[i].score) break;
            Cand t = h[p];
            h[p] = h[i];
            h[i] = t;
            i = p;
        }
        return;
    }
    if (c.score <= h[0].score) return;
    h[0] = c;
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, sm = i;
        if (l < cap && h[l].score < h[sm].score) sm = l;
        if (r < cap && h[r].score < h[sm].score) sm = r;
        if (sm == i) break;
        Cand t = h[sm];
        h[sm] = h[i];
        h[i] = t;
        i = sm;
    }
}

// Best K positions of the score map with greedy suppression of each pick's 3x3 neighbourhood
// (the refinement window covers it). Unlike strict local maxima this keeps positions on the
// slope of a ridge, which is where the true match lies when smooth content blurs the peak.
// Returns the number of candidates written to out (sorted by score), or -1.
static int pick_candidates(const float* sc, int nx, int ny, Cand* out, int K) {
    const int cap = K * 9;   // each pick suppresses at most 8 others
    Cand* heap = malloc((size_t)cap * sizeof(Cand));
    uint8_t* taken = calloc((size_t)nx * (size_t)ny, 1);
    if (!heap || !taken) {
        free(heap);
        free(taken);
        return img_fail("out of memory");
    }
    int count = 0;
    for (int y = 0; y < ny; y++) {
        const float* row = sc + (size_t)y * (size_t)nx;
        for (int x = 0; x < nx; x++) {
            float v = row[x];
            if (!(v > 0.0f)) continue;   // also skips NaN
            if (count == cap && v <= heap[0].score) continue;
            Cand c = { x, y, v };
            heap_push(heap, &count, cap, c);
        }
    }
    qsort(heap, (size_t)count, sizeof(Cand), cand_cmp);

    int n = 0;
    for (int i = 0; i < count && n < K; i++) {
        const int x = heap[i].x, y = heap[i].y;
        if (taken[(size_t)y * (size_t)nx + (size_t)x]) continue;
        out[n++] = heap[i];
        for (int yy = y - 1; yy <= y + 1; yy++) {
            if (yy < 0 || yy >= ny) continue;
            for (int xx = x - 1; xx <= x + 1; xx++) {
                if (xx >= 0 && xx < nx) taken[(size_t)yy * (size_t)nx + (size_t)xx] = 1;
            }
        }
    }
    free(heap);
    free(taken);
    return n;
}

// Pyramid factor: the rule s = min(tw, th) / 12 (the coarse template keeps >= 12 x 12 cells),
// raised while the coarse search would cost more than MATCH_COARSE_BUDGET multiply-adds and
// the coarse template keeps >= 6 cells on its short side and >= 144 cells in total
// (long thin templates such as text lines).
static int pick_scale(int rw, int rh, int tw, int th) {
    const int mn = tw < th ? tw : th;
    int s = mn / 12;
    if (s < 1) s = 1;
    if (s > 8) s = 8;
    while (s < 8) {
        const int n = s + 1;
        if (mn / n < 6 || (tw / n) * (th / n) < 144) break;
        double cost = (double)(rw / s) * (double)(rh / s) * (double)(tw / s) * (double)(th / s);
        if (cost <= MATCH_COARSE_BUDGET) break;
        s = n;
    }
    return s;
}

static bool overlaps(const SoulXMatch* a, const SoulXMatch* b, int tw, int th) {
    int ix = tw - abs(a->x - b->x);
    int iy = th - abs(a->y - b->y);
    if (ix <= 0 || iy <= 0) return false;
    return (double)ix * (double)iy > MATCH_NMS_OVERLAP * (double)tw * (double)th;
}

// ---------------------------------------------------------------------------
// Matching: driver
// ---------------------------------------------------------------------------

// Returns the number of accepted matches (<= max) or -1. *rejected (optional) receives the
// best candidate when nothing was accepted.
static int match_run(const SoulXImage* scr, const SoulXImage* tm, const SoulXMatchOpts* opts,
                     SoulXMatch* out, int max, bool all, SoulXMatch* rejected) {
    if (rejected) {
        rejected->x = -1;
        rejected->y = -1;
        rejected->score = 0.0;
    }
    if (!image_valid(scr)) return img_fail("invalid screen image");
    if (!image_valid(tm)) return img_fail("invalid template image");
    if (!out || max <= 0) return img_fail("invalid arguments");

    SoulXMatchOpts o;
    if (opts) {
        o = *opts;
    } else {
        soulx_match_opts_init(&o);
    }
    double thr = o.threshold;
    if (!(thr > 0.0)) thr = SOULX_MATCH_DEFAULT_THRESHOLD;   // also catches NaN
    if (thr > 1.0) thr = 1.0;
    int tol = o.color_tolerance < 0 ? 0 : o.color_tolerance;

    // Optional multi-scale: match a rescaled copy of the template. Localized — the rest of the matcher is unchanged.
    SoulXImage scaled = { 0, 0, NULL };
    if (o.scale > 0.0 && o.scale != 1.0) {
        if (soulx_image_scale(tm, o.scale, &scaled) != 0) return -1;
        tm = &scaled;
    }

    // Search area: clipped region; a match must lie entirely inside it
    int rx = 0, ry = 0, rw = scr->w, rh = scr->h;
    if (o.region_w > 0 && o.region_h > 0) {
        long long x0 = o.region_x > 0 ? o.region_x : 0;
        long long y0 = o.region_y > 0 ? o.region_y : 0;
        long long x1 = (long long)o.region_x + o.region_w;
        long long y1 = (long long)o.region_y + o.region_h;
        if (x1 > scr->w) x1 = scr->w;
        if (y1 > scr->h) y1 = scr->h;
        if (x1 <= x0 || y1 <= y0) return 0;   // region outside the screen
        rx = (int)x0;
        ry = (int)y0;
        rw = (int)(x1 - x0);
        rh = (int)(y1 - y0);
    }
    const int tw = tm->w, th = tm->h;
    if (tw > rw || th > rh) return 0;

    int result = -1;
    TLevel F = { 0 }, C = { 0 };
    float* S = NULL;
    int32_t* acc = NULL;
    float* scores = NULL;
    Cand* cands = NULL;
    float* patch = NULL;
    float* pscores = NULL;

    if (tlevel_build(tm, 1, &F) != 0) goto done;
    if (F.n == 0) {
        img_fail("template has no opaque pixels (alpha >= 128)");
        goto done;
    }
    const bool flat = F.var < MATCH_FLAT_VAR * F.n;

    // Coarse level: shrink s until the coarse template keeps opaque cells and contrast
    int s = pick_scale(rw, rh, tw, th);
    while (s > 1) {
        if (tlevel_build(tm, s, &C) != 0) goto done;
        double unit2 = (double)C.unit * (double)C.unit;
        if (C.n > 0 && (flat || C.var >= MATCH_FLAT_VAR * unit2 * C.n)) break;
        tlevel_free(&C);
        s--;
    }
    const TLevel* CL = s > 1 ? &C : &F;

    const int cw = rw / s, ch = rh / s;
    S = malloc((size_t)cw * (size_t)ch * sizeof(float));
    acc = malloc((size_t)cw * sizeof(int32_t));
    if (!S || !acc) {
        img_fail("out of memory");
        goto done;
    }
    level_fill(scr, rx, ry, cw, ch, s, S, acc);

    const int nx = cw - CL->w + 1, ny = ch - CL->h + 1;
    if (nx <= 0 || ny <= 0) {
        result = 0;
        goto done;
    }
    scores = malloc((size_t)nx * (size_t)ny * sizeof(float));
    if (!scores) {
        img_fail("out of memory");
        goto done;
    }
    if (level_scores(S, cw, ch, CL, flat, scores) != 0) goto done;
    free(S);
    S = NULL;

    // Candidate budget: refinement costs (2r + 1)^2 * tw * th multiply-adds per candidate.
    // r >= 1 also at s == 1, so that suppressing a pick's neighbours loses nothing.
    const int r = s + 1;
    double cost = (double)(2 * r + 1) * (double)(2 * r + 1) * (double)tw * (double)th;
    double kd = MATCH_REFINE_BUDGET / cost;
    int K = kd > MATCH_MAX_FIND_CANDS ? MATCH_MAX_FIND_CANDS : (int)kd;
    if (K < MATCH_MIN_CANDS) K = MATCH_MIN_CANDS;
    if (all) {
        long long want = 2LL * max + 8;
        if (want > MATCH_MAX_CANDS) want = MATCH_MAX_CANDS;
        if (K < want) K = (int)want;
    }
    cands = malloc((size_t)K * sizeof(Cand));
    if (!cands) {
        img_fail("out of memory");
        goto done;
    }
    const int count = pick_candidates(scores, nx, ny, cands, K);
    if (count < 0) goto done;
    free(scores);
    scores = NULL;

    // Refine every candidate at full resolution within +-r pixels
    patch = malloc((size_t)(tw + 2 * r) * (size_t)(th + 2 * r) * sizeof(float));
    pscores = malloc((size_t)(2 * r + 1) * (size_t)(2 * r + 1) * sizeof(float));
    if (!patch || !pscores) {
        img_fail("out of memory");
        goto done;
    }
    const int max_x = rx + rw - tw, max_y = ry + rh - th;
    for (int i = 0; i < count; i++) {
        // coarse cells may lie up to s - 1 pixels past the last valid position: clamp
        int px = rx + cands[i].x * s, py = ry + cands[i].y * s;
        int x0 = clampi(px - r, rx, max_x), x1 = clampi(px + r, rx, max_x);
        int y0 = clampi(py - r, ry, max_y), y1 = clampi(py + r, ry, max_y);
        int pw = x1 - x0 + tw, ph = y1 - y0 + th;
        level_fill(scr, x0, y0, pw, ph, 1, patch, NULL);
        if (level_scores(patch, pw, ph, &F, flat, pscores) != 0) goto done;
        int pnx = x1 - x0 + 1, pny = y1 - y0 + 1;
        int best = 0;
        for (int k = 1; k < pnx * pny; k++) {
            if (pscores[k] > pscores[best]) best = k;
        }
        int bx = x0 + best % pnx, by = y0 + best / pnx;
        cands[i].x = bx;
        cands[i].y = by;
        cands[i].score = exact_score(scr, bx, by, tm, &F, flat);
    }
    qsort(cands, (size_t)count, sizeof(Cand), cand_cmp);

    // Accept in score order: threshold, colour check, suppression of overlapping matches
    int found = 0;
    for (int i = 0; i < count && found < max; i++) {
        if (cands[i].score + MATCH_SCORE_EPS < thr) break;
        SoulXMatch m = { cands[i].x + tw / 2, cands[i].y + th / 2, cands[i].score };
        bool dup = false;
        for (int k = 0; k < found && !dup; k++) dup = overlaps(&out[k], &m, tw, th);
        if (dup) continue;
        if (tol > 0 && color_distance(scr, cands[i].x, cands[i].y, tm) > (double)tol) continue;
        out[found++] = m;
        if (!all) break;
    }
    if (found == 0 && rejected && count > 0) {
        rejected->x = cands[0].x + tw / 2;
        rejected->y = cands[0].y + th / 2;
        rejected->score = cands[0].score;
    }
    result = found;

done:
    tlevel_free(&F);
    tlevel_free(&C);
    free(S);
    free(acc);
    free(scores);
    free(cands);
    free(patch);
    free(pscores);
    if (scaled.rgba) soulx_image_free(&scaled);
    return result;
}

int soulx_image_find(const SoulXImage* screen, const SoulXImage* tmpl, const SoulXMatchOpts* o,
                    SoulXMatch* best) {
    if (!best) return img_fail("invalid arguments");
    SoulXMatch m = { -1, -1, 0.0 };
    SoulXMatch rejected = { -1, -1, 0.0 };
    int n = match_run(screen, tmpl, o, &m, 1, false, &rejected);
    if (n < 0) return -1;
    *best = n > 0 ? m : rejected;
    return n > 0 ? 1 : 0;
}

int soulx_image_find_all(const SoulXImage* screen, const SoulXImage* tmpl, const SoulXMatchOpts* o,
                        SoulXMatch* out, int max) {
    if (!out || max <= 0) return img_fail("invalid arguments");
    return match_run(screen, tmpl, o, out, max, true, NULL);
}
