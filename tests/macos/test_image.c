/*
 * SoulX Android Emulator - unit tests for source/script/image.c
 *
 * PNG round trip, crop/pixel helpers and template matching on synthetic screens
 * (exact centre, colour check, alpha mask, region restriction, find_all, speed).
 * Needs no emulator. SOULX_HOME points to a fresh temporary directory.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "image.h"

static int g_failures = 0;
static int g_checks = 0;
static char g_tmpdir[64];

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t g_rng = 0x9e3779b9u;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void img_release(SoulXImage* img) {
    free(img->rgba);
    memset(img, 0, sizeof(*img));
}

static uint8_t* px(const SoulXImage* img, int x, int y) {
    return img->rgba + ((size_t)y * (size_t)img->w + (size_t)x) * 4;
}

static void set_px(SoulXImage* img, int x, int y, int r, int g, int b, int a) {
    uint8_t* p = px(img, x, y);
    p[0] = (uint8_t)r;
    p[1] = (uint8_t)g;
    p[2] = (uint8_t)b;
    p[3] = (uint8_t)a;
}

static int clamp255(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

// Random RGB noise, opaque
static void fill_noise(SoulXImage* img) {
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            uint32_t v = rnd();
            set_px(img, x, y, (int)(v & 0xff), (int)((v >> 8) & 0xff), (int)((v >> 16) & 0xff), 255);
        }
    }
}

// Adds uniform noise in [-amp, amp] to every channel
static void add_noise(SoulXImage* img, int amp) {
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            uint8_t* p = px(img, x, y);
            for (int c = 0; c < 3; c++) {
                int d = (int)(rnd() % (uint32_t)(2 * amp + 1)) - amp;
                p[c] = (uint8_t)clamp255(p[c] + d);
            }
        }
    }
}

// A UI-like patterned template: gradient background, ring, stripes and a bar
static void fill_pattern(SoulXImage* img, int variant) {
    const int w = img->w, h = img->h;
    const double cx = w * 0.35, cy = h * 0.5, rad = (w < h ? w : h) * 0.3;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int r = 40 + 160 * x / w, g = 60 + 120 * y / h, b = 90 + variant * 30;
            double d = sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
            if (d < rad && d > rad * 0.55) {
                r = 250;
                g = 220;
                b = 30;
            }
            if (x > w * 0.6 && ((x + y) / 3) % 2 == 0) {
                r = 20;
                g = 30;
                b = 200;
            }
            if (y > h * 0.8 && x < w * 0.5) {
                r = 230;
                g = 240;
                b = 250;
            }
            set_px(img, x, y, r, g, b, 255);
        }
    }
}

// Copies the opaque (alpha >= 128) pixels of src onto dst at (x, y)
static void paste(SoulXImage* dst, const SoulXImage* src, int x, int y) {
    for (int j = 0; j < src->h; j++) {
        for (int i = 0; i < src->w; i++) {
            const uint8_t* s = px(src, i, j);
            if (s[3] < 128) continue;
            uint8_t* d = px(dst, x + i, y + j);
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
    }
}

static bool new_image(SoulXImage* img, int w, int h) {
    int rc = soulx_image_alloc(img, w, h);
    CHECK(rc == 0, "soulx_image_alloc(%d, %d) failed: %s", w, h, soulx_image_error());
    return rc == 0;
}

static void tmp_path(char* buf, size_t cap, const char* name) {
    int n = snprintf(buf, cap, "%s/%s", g_tmpdir, name);
    if (n < 0 || (size_t)n >= cap) {
        fprintf(stderr, "temporary path too long\n");
        exit(2);
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_png_and_helpers(void) {
    SoulXImage a, b, c;
    if (!new_image(&a, 37, 23)) return;
    for (int y = 0; y < a.h; y++) {
        for (int x = 0; x < a.w; x++) set_px(&a, x, y, x * 7, y * 11, (x * y) & 0xff, (x * 13 + y) & 0xff);
    }

    char path[256];
    tmp_path(path, sizeof(path), "roundtrip.png");
    CHECK(soulx_image_save_png(path, &a) == 0, "save_png: %s", soulx_image_error());
    CHECK(soulx_image_load_png(path, &b) == 0, "load_png: %s", soulx_image_error());
    CHECK(b.w == a.w && b.h == a.h, "round trip size %dx%d", b.w, b.h);
    if (b.rgba && b.w == a.w && b.h == a.h) {
        CHECK(memcmp(a.rgba, b.rgba, (size_t)a.w * a.h * 4) == 0, "round trip pixels differ");
    }
    img_release(&b);

    // crop + pixel
    CHECK(soulx_image_crop(&a, 5, 4, 10, 7, &c) == 0, "crop: %s", soulx_image_error());
    CHECK(c.w == 10 && c.h == 7, "crop size %dx%d", c.w, c.h);
    if (c.rgba) {
        uint8_t* p = px(&a, 5 + 3, 4 + 2);
        int32_t want = (int32_t)((p[0] << 16) | (p[1] << 8) | p[2]);
        CHECK(soulx_image_pixel(&c, 3, 2) == want, "pixel 0x%06x != 0x%06x", (unsigned)soulx_image_pixel(&c, 3, 2),
              (unsigned)want);
    }
    CHECK(soulx_image_pixel(&c, 10, 0) == -1, "pixel outside must be -1");
    CHECK(soulx_image_pixel(&c, -1, 0) == -1, "pixel outside must be -1");
    img_release(&c);

    SoulXImage bad;
    CHECK(soulx_image_crop(&a, 30, 0, 10, 5, &bad) == -1, "crop outside must fail");
    CHECK(soulx_image_crop(&a, 0, 0, 0, 5, &bad) == -1, "empty crop must fail");
    CHECK(soulx_image_crop(&a, -1, 0, 5, 5, &bad) == -1, "negative crop must fail");

    // in-place crop
    SoulXImage d;
    if (soulx_image_crop(&a, 0, 0, 37, 23, &d) == 0) {
        CHECK(soulx_image_crop(&d, 1, 1, 4, 3, &d) == 0 && d.w == 4 && d.h == 3, "in-place crop");
        CHECK(soulx_image_pixel(&d, 0, 0) == soulx_image_pixel(&a, 1, 1), "in-place crop content");
        img_release(&d);
    }

    // error paths
    char missing[256], garbage[256];
    tmp_path(missing, sizeof(missing), "missing.png");
    tmp_path(garbage, sizeof(garbage), "garbage.png");
    CHECK(soulx_image_load_png(missing, &b) == -1 && b.rgba == NULL, "missing file must fail");
    FILE* f = fopen(garbage, "wb");
    if (f) {
        fputs("this is not a png", f);
        fclose(f);
    }
    CHECK(soulx_image_load_png(garbage, &b) == -1, "garbage must fail");
    CHECK(strstr(soulx_image_error(), "not a valid") != NULL, "garbage error message: %s", soulx_image_error());
    CHECK(soulx_image_load_png(g_tmpdir, &b) == -1, "directory must fail");
    char nodir[256];
    tmp_path(nodir, sizeof(nodir), "no/such/dir.png");
    CHECK(soulx_image_save_png(nodir, &a) == -1, "save into a missing directory must fail");
    unlink(missing);
    unlink(garbage);
    unlink(path);
    img_release(&a);
}

static void test_find_exact(void) {
    SoulXImage scr, tm;
    if (!new_image(&scr, 1080, 2400)) return;
    if (!new_image(&tm, 40, 30)) {
        img_release(&scr);
        return;
    }
    fill_noise(&scr);
    fill_pattern(&tm, 0);
    paste(&scr, &tm, 517, 1333);
    add_noise(&scr, 6);

    SoulXMatch m;
    double t0 = now_ms();
    int rc = soulx_image_find(&scr, &tm, NULL, &m);
    double dt = now_ms() - t0;
    CHECK(rc == 1, "40x30 pattern not found (rc %d, best %.3f at %d,%d)", rc, m.score, m.x, m.y);
    CHECK(m.x == 517 + 20 && m.y == 1333 + 15, "centre (%d, %d), expected (537, 1348)", m.x, m.y);
    CHECK(m.score > 0.95, "score %.4f <= 0.95", m.score);
    printf("  find 40x30 on 1080x2400: (%d, %d) score %.4f in %.1f ms\n", m.x, m.y, m.score, dt);

    // a pure-noise template cut from the screen itself (no structure at coarse scale)
    SoulXImage cut;
    if (soulx_image_crop(&scr, 801, 211, 40, 30, &cut) == 0) {
        rc = soulx_image_find(&scr, &cut, NULL, &m);
        CHECK(rc == 1 && m.x == 821 && m.y == 226, "noise crop: rc %d at (%d, %d) score %.3f", rc, m.x, m.y, m.score);
        CHECK(m.score > 0.999, "noise crop score %.4f", m.score);
        img_release(&cut);
    }
    img_release(&tm);
    img_release(&scr);
}

static void test_color_check(void) {
    SoulXImage scr, tm, blue;
    if (!new_image(&scr, 720, 1280)) return;
    if (!new_image(&tm, 60, 40) || !new_image(&blue, 60, 40)) {
        img_release(&scr);
        img_release(&tm);
        return;
    }
    fill_noise(&scr);
    // two-tone button: red shape on dark background; the recoloured copy is blue on dark,
    // so its grayscale is an affine function of the original (identical ZNCC structure)
    for (int y = 0; y < 40; y++) {
        for (int x = 0; x < 60; x++) {
            bool fg = ((x / 6 + y / 5) % 3 == 0) || (x > 20 && x < 40 && y > 12 && y < 28);
            set_px(&tm, x, y, fg ? 220 : 30, fg ? 40 : 30, fg ? 40 : 30, 255);
            set_px(&blue, x, y, fg ? 40 : 30, fg ? 40 : 30, fg ? 220 : 30, 255);
        }
    }
    paste(&scr, &blue, 100, 700);

    SoulXMatch m;
    int rc = soulx_image_find(&scr, &tm, NULL, &m);
    CHECK(rc == 0, "recoloured copy must be rejected by the colour check (rc %d)", rc);
    CHECK(m.score > 0.95 && m.x == 130 && m.y == 720, "rejected candidate should be the copy: (%d, %d) %.3f",
          m.x, m.y, m.score);

    SoulXMatchOpts o;
    soulx_match_opts_init(&o);
    o.color_tolerance = 0;   // colour check disabled: grayscale structure matches
    rc = soulx_image_find(&scr, &tm, &o, &m);
    CHECK(rc == 1 && m.x == 130 && m.y == 720 && m.score > 0.95, "tolerance 0: rc %d (%d, %d) %.3f",
          rc, m.x, m.y, m.score);

    paste(&scr, &tm, 500, 200);   // now add the original
    rc = soulx_image_find(&scr, &tm, NULL, &m);
    CHECK(rc == 1 && m.x == 530 && m.y == 220, "original copy: rc %d (%d, %d)", rc, m.x, m.y);

    SoulXMatch all[8];
    int n = soulx_image_find_all(&scr, &tm, NULL, all, 8);
    CHECK(n == 1, "find_all with colour check: %d matches (expected 1)", n);
    img_release(&blue);
    img_release(&tm);
    img_release(&scr);
}

static void test_alpha_mask(void) {
    SoulXImage scr, tm, opaque;
    if (!new_image(&scr, 1080, 2400)) return;
    if (!new_image(&tm, 48, 48)) {
        img_release(&scr);
        return;
    }
    fill_noise(&scr);
    // round icon: opaque disc with a pattern, transparent corners
    for (int y = 0; y < 48; y++) {
        for (int x = 0; x < 48; x++) {
            double d = sqrt((x - 23.5) * (x - 23.5) + (y - 23.5) * (y - 23.5));
            bool in = d <= 20.0;
            int r = (x * 5) & 0xff, g = 255 - y * 4, b = ((x / 8 + y / 8) % 2) ? 220 : 40;
            set_px(&tm, x, y, in ? r : 0, in ? g : 0, in ? b : 0, in ? 255 : 0);
        }
    }
    paste(&scr, &tm, 300, 900);   // corners keep the noisy background
    add_noise(&scr, 4);

    SoulXMatch m;
    int rc = soulx_image_find(&scr, &tm, NULL, &m);
    CHECK(rc == 1 && m.x == 324 && m.y == 924, "masked icon: rc %d at (%d, %d)", rc, m.x, m.y);
    CHECK(m.score > 0.95, "masked icon score %.4f", m.score);
    double masked_score = m.score;

    // the same icon without its alpha channel (black corners) matches clearly worse
    if (soulx_image_crop(&tm, 0, 0, 48, 48, &opaque) == 0) {
        for (int y = 0; y < 48; y++) {
            for (int x = 0; x < 48; x++) px(&opaque, x, y)[3] = 255;
        }
        SoulXMatchOpts o;
        soulx_match_opts_init(&o);
        o.threshold = 0.01;
        o.color_tolerance = 0;
        rc = soulx_image_find(&scr, &opaque, &o, &m);
        CHECK(rc == 1 && m.score < masked_score - 0.1, "opaque icon score %.3f vs masked %.3f", m.score,
              masked_score);
        img_release(&opaque);
    }

    // fully transparent template is an error
    SoulXImage clear;
    if (new_image(&clear, 20, 20)) {
        CHECK(soulx_image_find(&scr, &clear, NULL, &m) == -1, "transparent template must be rejected");
        img_release(&clear);
    }
    img_release(&tm);
    img_release(&scr);
}

static void test_region(void) {
    SoulXImage scr, tm;
    if (!new_image(&scr, 1080, 2400)) return;
    if (!new_image(&tm, 64, 36)) {
        img_release(&scr);
        return;
    }
    fill_noise(&scr);
    fill_pattern(&tm, 1);
    paste(&scr, &tm, 100, 200);
    paste(&scr, &tm, 700, 1900);
    add_noise(&scr, 5);

    SoulXMatchOpts o;
    SoulXMatch m;
    soulx_match_opts_init(&o);
    o.region_x = 600;
    o.region_y = 1800;
    o.region_w = 300;
    o.region_h = 300;
    int rc = soulx_image_find(&scr, &tm, &o, &m);
    CHECK(rc == 1 && m.x == 732 && m.y == 1918, "region 2: rc %d at (%d, %d)", rc, m.x, m.y);

    o.region_x = -50;   // partially outside the screen: clipped
    o.region_y = 150;
    o.region_w = 300;
    o.region_h = 200;
    rc = soulx_image_find(&scr, &tm, &o, &m);
    CHECK(rc == 1 && m.x == 132 && m.y == 218, "region 1 (clipped): rc %d at (%d, %d)", rc, m.x, m.y);

    o.region_x = 0;
    o.region_y = 1000;
    o.region_w = 1080;
    o.region_h = 500;
    CHECK(soulx_image_find(&scr, &tm, &o, &m) == 0, "empty region must not match");

    o.region_x = 720;   // cuts the copy at (700, 1900): must not match
    o.region_y = 1850;
    o.region_w = 300;
    o.region_h = 300;
    CHECK(soulx_image_find(&scr, &tm, &o, &m) == 0, "partially covered copy must not match");

    o.region_x = 5000;  // entirely outside
    o.region_y = 0;
    CHECK(soulx_image_find(&scr, &tm, &o, &m) == 0, "region outside the screen must not match");

    o.region_x = 100;   // smaller than the template
    o.region_y = 200;
    o.region_w = 30;
    o.region_h = 30;
    CHECK(soulx_image_find(&scr, &tm, &o, &m) == 0, "region smaller than the template must not match");
    img_release(&tm);
    img_release(&scr);
}

static void test_find_all(void) {
    SoulXImage scr, tm;
    if (!new_image(&scr, 1080, 2400)) return;
    if (!new_image(&tm, 50, 40)) {
        img_release(&scr);
        return;
    }
    fill_noise(&scr);
    fill_pattern(&tm, 2);
    const int pos[3][2] = { { 40, 60 }, { 611, 1007 }, { 1000, 2333 } };
    for (int i = 0; i < 3; i++) paste(&scr, &tm, pos[i][0], pos[i][1]);
    add_noise(&scr, 6);

    SoulXMatch out[16];
    double t0 = now_ms();
    int n = soulx_image_find_all(&scr, &tm, NULL, out, 16);
    double dt = now_ms() - t0;
    CHECK(n == 3, "find_all: %d matches (expected 3)", n);
    for (int i = 0; i < n && i < 16; i++) {
        bool known = false;
        for (int k = 0; k < 3; k++) {
            if (out[i].x == pos[k][0] + 25 && out[i].y == pos[k][1] + 20) known = true;
        }
        CHECK(known, "unexpected match (%d, %d) score %.3f", out[i].x, out[i].y, out[i].score);
        CHECK(out[i].score > 0.95, "match %d score %.3f", i, out[i].score);
        if (i > 0) CHECK(out[i].score <= out[i - 1].score, "matches not sorted by score");
    }
    printf("  find_all 50x40 x3 on 1080x2400: %d matches in %.1f ms\n", n, dt);

    n = soulx_image_find_all(&scr, &tm, NULL, out, 2);
    CHECK(n == 2, "find_all max 2: %d", n);
    CHECK(soulx_image_find_all(&scr, &tm, NULL, out, 0) == -1, "max 0 must be rejected");
    img_release(&tm);
    img_release(&scr);
}

static void test_flat_and_edges(void) {
    SoulXImage scr, tm;
    if (!new_image(&scr, 400, 300)) return;
    if (!new_image(&tm, 30, 30)) {
        img_release(&scr);
        return;
    }
    fill_noise(&scr);
    for (int y = 0; y < 30; y++) {
        for (int x = 0; x < 30; x++) set_px(&tm, x, y, 200, 120, 40, 255);
    }
    paste(&scr, &tm, 123, 77);
    SoulXMatch m;
    int rc = soulx_image_find(&scr, &tm, NULL, &m);
    CHECK(rc == 1 && m.x == 138 && m.y == 92 && m.score > 0.95, "flat template: rc %d at (%d, %d) %.3f",
          rc, m.x, m.y, m.score);

    // template equal to the whole screen
    SoulXImage whole;
    if (soulx_image_crop(&scr, 0, 0, 400, 300, &whole) == 0) {
        rc = soulx_image_find(&scr, &whole, NULL, &m);
        CHECK(rc == 1 && m.x == 200 && m.y == 150 && m.score > 0.999, "whole-screen template: rc %d", rc);
        img_release(&whole);
    }

    // template larger than the screen: no match, no error
    SoulXImage big;
    if (new_image(&big, 401, 10)) {
        fill_noise(&big);
        CHECK(soulx_image_find(&scr, &big, NULL, &m) == 0, "larger template must not match");
        img_release(&big);
    }

    // tiny template (no coarse level)
    SoulXImage tiny;
    if (soulx_image_crop(&scr, 250, 31, 9, 7, &tiny) == 0) {
        rc = soulx_image_find(&scr, &tiny, NULL, &m);
        CHECK(rc == 1 && m.x == 254 && m.y == 34, "tiny template: rc %d at (%d, %d)", rc, m.x, m.y);
        img_release(&tiny);
    }

    CHECK(soulx_image_find(NULL, &tm, NULL, &m) == -1, "NULL screen must fail");
    CHECK(soulx_image_find(&scr, NULL, NULL, &m) == -1, "NULL template must fail");
    CHECK(soulx_image_find(&scr, &tm, NULL, NULL) == -1, "NULL result must fail");
    img_release(&tm);
    img_release(&scr);
}

static void test_speed(void) {
    SoulXImage scr, tm;
    if (!new_image(&scr, 1080, 2400)) return;
    if (!new_image(&tm, 200, 80)) {
        img_release(&scr);
        return;
    }
    fill_noise(&scr);
    fill_pattern(&tm, 3);
    paste(&scr, &tm, 433, 1777);
    add_noise(&scr, 6);

    SoulXMatch m = { 0, 0, 0.0 };
    double best = 1e9;
    int rc = 0;
    for (int run = 0; run < 5; run++) {
        double t0 = now_ms();
        rc = soulx_image_find(&scr, &tm, NULL, &m);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
    }
    CHECK(rc == 1 && m.x == 533 && m.y == 1817 && m.score > 0.95, "200x80: rc %d at (%d, %d) %.3f",
          rc, m.x, m.y, m.score);
    printf("  find 200x80 on 1080x2400: best of 5 runs = %.1f ms\n", best);
#ifdef __OPTIMIZE__
    CHECK(best < 300.0, "200x80 template search took %.1f ms (limit 300 ms)", best);
#else
    printf("  (unoptimized build: timing limit not enforced)\n");
#endif
    img_release(&tm);
    img_release(&scr);
}

int main(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/private/tmp/soulx-test-image-XXXXXX");
    if (!mkdtemp(g_tmpdir)) {
        perror("mkdtemp");
        return 2;
    }
    setenv("SOULX_HOME", g_tmpdir, 1);   // never touch the real ~/.soulx-android

    test_png_and_helpers();
    test_find_exact();
    test_color_check();
    test_alpha_mask();
    test_region();
    test_find_all();
    test_flat_and_edges();
    test_speed();

    rmdir(g_tmpdir);
    if (g_failures) {
        fprintf(stderr, "test_image: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_image: all %d checks passed\n", g_checks);
    return 0;
}
