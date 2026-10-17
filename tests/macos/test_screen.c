/*
 * SoulX Android Emulator - unit tests for source/platform/macos/screen.c
 * Raw `screencap` decoding on synthetic buffers: 12- and 16-byte headers, pixel formats
 * 1 (RGBA), 2 (RGBX), 3 (RGB), 4 (RGB565), 5 (BGRA), 0x2B (RGBA1010102), malformed sizes.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "soulx_mac.h"

extern int soulx_screencap_decode(const uint8_t* data, size_t len, SoulXImage* img);

static int g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

static void wr32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Header (12 or 16 bytes) + w*h*bpp bytes of pixels, pixel i byte k = (i * 7 + k * 31 + 1) & 0xFF.
static uint8_t* make_cap(uint32_t w, uint32_t h, uint32_t format, size_t bpp, size_t header, size_t* len)
{
    *len = header + (size_t)w * h * bpp;
    uint8_t* d = malloc(*len ? *len : 1);
    if (!d) exit(2);
    memset(d, 0, header);
    wr32(d, w);
    wr32(d + 4, h);
    wr32(d + 8, format);
    if (header == 16) wr32(d + 12, 0x08810000);   // a dataspace value
    for (size_t i = 0; i < (size_t)w * h; i++) {
        for (size_t k = 0; k < bpp; k++) d[header + i * bpp + k] = (uint8_t)((i * 7 + k * 31 + 1) & 0xFF);
    }
    return d;
}

static uint8_t src_byte(size_t i, size_t k)
{
    return (uint8_t)((i * 7 + k * 31 + 1) & 0xFF);
}

static void test_rgba_family(void)
{
    const uint32_t w = 5, h = 3;
    for (size_t header = 12; header <= 16; header += 4) {
        for (int f = 0; f < 3; f++) {
            uint32_t format = f == 0 ? 1 : f == 1 ? 2 : 5;
            size_t len;
            uint8_t* d = make_cap(w, h, format, 4, header, &len);
            SoulXImage img = { -1, -1, NULL };
            CHECK(soulx_screencap_decode(d, len, &img) == 0);
            CHECK(img.w == (int)w && img.h == (int)h && img.rgba != NULL);
            if (img.rgba) {
                bool ok = true;
                for (size_t i = 0; i < (size_t)w * h; i++) {
                    const uint8_t* px = img.rgba + i * 4;
                    uint8_t r = src_byte(i, 0), g = src_byte(i, 1), b = src_byte(i, 2), a = src_byte(i, 3);
                    if (format == 1) ok = ok && px[0] == r && px[1] == g && px[2] == b && px[3] == a;
                    if (format == 2) ok = ok && px[0] == r && px[1] == g && px[2] == b && px[3] == 255;
                    if (format == 5) ok = ok && px[0] == b && px[1] == g && px[2] == r && px[3] == a;
                }
                CHECK(ok);
            }
            soulx_image_free(&img);
            CHECK(img.rgba == NULL && img.w == 0 && img.h == 0);
            free(d);
        }
    }
}

static void test_other_formats(void)
{
    size_t len;
    SoulXImage img = { 0, 0, NULL };

    // RGB_888 (3 bytes per pixel)
    uint8_t* d = make_cap(4, 2, 3, 3, 16, &len);
    CHECK(soulx_screencap_decode(d, len, &img) == 0);
    if (img.rgba) {
        CHECK(img.rgba[4] == src_byte(1, 0) && img.rgba[5] == src_byte(1, 1) && img.rgba[6] == src_byte(1, 2));
        CHECK(img.rgba[7] == 255);
    }
    soulx_image_free(&img);
    free(d);

    // RGB_565: pure red, green, blue, white
    d = make_cap(4, 1, 4, 2, 12, &len);
    const uint16_t px565[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    for (int i = 0; i < 4; i++) {
        d[12 + i * 2] = (uint8_t)px565[i];
        d[12 + i * 2 + 1] = (uint8_t)(px565[i] >> 8);
    }
    CHECK(soulx_screencap_decode(d, len, &img) == 0);
    if (img.rgba) {
        const uint8_t want[16] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
        CHECK(memcmp(img.rgba, want, sizeof(want)) == 0);
    }
    soulx_image_free(&img);
    free(d);

    // RGBA_1010102: max red, half green, zero blue, opaque
    d = make_cap(1, 1, 0x2B, 4, 16, &len);
    wr32(d + 16, 0x3FFu | (0x200u << 10) | (0u << 20) | (3u << 30));
    CHECK(soulx_screencap_decode(d, len, &img) == 0);
    if (img.rgba) CHECK(img.rgba[0] == 255 && img.rgba[1] == 128 && img.rgba[2] == 0 && img.rgba[3] == 255);
    soulx_image_free(&img);
    free(d);
}

static void test_malformed(void)
{
    size_t len;
    SoulXImage img = { 7, 7, NULL };
    uint8_t* d = make_cap(5, 3, 1, 4, 16, &len);

    // too short for a header
    for (size_t l = 0; l < 12; l++) CHECK(soulx_screencap_decode(d, l, &img) == -1);
    // every other wrong length (only 12- or 16-byte headers are valid)
    for (size_t l = 12; l < len + 32; l++) {
        uint8_t* copy = malloc(l);
        if (!copy) exit(2);
        memcpy(copy, d, l < len ? l : len);
        if (l > len) memset(copy + len, 0, l - len);
        int rc = soulx_screencap_decode(copy, l, &img);
        CHECK(rc == ((l == len || l == len - 4) ? 0 : -1));
        CHECK(rc == 0 || (img.rgba == NULL && img.w == 0 && img.h == 0));
        soulx_image_free(&img);
        free(copy);
    }
    // zero / huge dimensions, unsupported formats
    wr32(d, 0);
    CHECK(soulx_screencap_decode(d, len, &img) == -1);
    wr32(d, 5);
    wr32(d + 4, 0);
    CHECK(soulx_screencap_decode(d, len, &img) == -1);
    wr32(d, 0x7FFFFFFF);
    wr32(d + 4, 0x7FFFFFFF);
    CHECK(soulx_screencap_decode(d, len, &img) == -1);
    wr32(d, 0xFFFFFFFF);
    wr32(d + 4, 0xFFFFFFFF);
    CHECK(soulx_screencap_decode(d, len, &img) == -1);
    wr32(d, 5);
    wr32(d + 4, 3);
    const uint32_t formats[] = { 0, 6, 7, 0x16, 0x100, 0xFFFFFFFF };
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        wr32(d + 8, formats[i]);
        CHECK(soulx_screencap_decode(d, len, &img) == -1);
    }
    wr32(d + 8, 1);
    CHECK(soulx_screencap_decode(d, len, &img) == 0);
    soulx_image_free(&img);
    CHECK(soulx_screencap_decode(NULL, len, &img) == -1);
    CHECK(soulx_screencap_decode(d, len, NULL) == -1);
    soulx_image_free(NULL);
    free(d);

    // random headers never crash and never succeed with a size that does not match
    uint64_t x = 0x1234567887654321ull;
    for (int iter = 0; iter < 20000; iter++) {
        uint8_t buf[64];
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        size_t l = 12 + (size_t)(x % 52);
        for (size_t i = 0; i < l; i++) buf[i] = (uint8_t)(x >> ((i % 8) * 8)) ^ (uint8_t)i;
        wr32(buf, (uint32_t)(x % 4));
        wr32(buf + 4, (uint32_t)((x >> 8) % 4));
        uint8_t* copy = malloc(l);
        if (!copy) exit(2);
        memcpy(copy, buf, l);
        if (soulx_screencap_decode(copy, l, &img) == 0) {
            CHECK(img.w > 0 && img.h > 0 && img.w * img.h * 2 <= (int)l);
        }
        soulx_image_free(&img);
        free(copy);
    }
}

int main(void)
{
    char tmpl[] = "/private/tmp/soulx-test-screen-XXXXXX";
    char* home = mkdtemp(tmpl);
    if (!home) {
        perror("mkdtemp");
        return 1;
    }
    setenv("SOULX_HOME", home, 1);               // never touch the real ~/.soulx-android

    test_rgba_family();
    test_other_formats();
    test_malformed();

    rmdir(home);
    if (g_failures) {
        fprintf(stderr, "test_screen: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_screen: all tests passed\n");
    return 0;
}
