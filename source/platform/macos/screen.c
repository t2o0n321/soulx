/*
 * SoulX Android Emulator - macOS backend: screen capture
 * Raw framebuffer capture through `adb exec-out screencap` (decoded to tightly packed RGBA8888)
 * and PNG screenshots (`screencap -p`).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soulx_mac.h"

// ---------------------------------------------------------------------------
// Limits and pixel formats (android/hardware PixelFormat values used by screencap)
// ---------------------------------------------------------------------------

#define SCREEN_MAX_DIM          16384u
#define SCREEN_MAX_RGBA_BYTES   (256u * 1024u * 1024u)
#define SCREEN_TIMEOUT_MS       15000

#define PF_RGBA_8888            1
#define PF_RGBX_8888            2
#define PF_RGB_888              3
#define PF_RGB_565              4
#define PF_BGRA_8888            5
#define PF_RGBA_1010102         0x2B

// Exported for the unit tests (tests/macos/test_screen.c); not part of soulx_mac.h.
int soulx_screencap_decode(const uint8_t* data, size_t len, SoulXImage* img);

static uint32_t rd32le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static size_t bytes_per_pixel(uint32_t format)
{
    switch (format) {
    case PF_RGBA_8888:
    case PF_RGBX_8888:
    case PF_BGRA_8888:
    case PF_RGBA_1010102:
        return 4;
    case PF_RGB_888:
        return 3;
    case PF_RGB_565:
        return 2;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Raw screencap decoding
// ---------------------------------------------------------------------------

// Raw `screencap` output: little-endian u32 width, height, format (+ u32 dataspace since Android 12)
// followed by tightly packed rows. Returns 0 and a malloc'd RGBA image, or -1 on malformed input.
int soulx_screencap_decode(const uint8_t* data, size_t len, SoulXImage* img)
{
    if (!img) return -1;
    img->w = 0;
    img->h = 0;
    img->rgba = NULL;
    if (!data || len < 12) return -1;

    uint32_t w = rd32le(data), h = rd32le(data + 4), format = rd32le(data + 8);
    size_t bpp = bytes_per_pixel(format);
    if (w == 0 || h == 0 || w > SCREEN_MAX_DIM || h > SCREEN_MAX_DIM || bpp == 0) return -1;
    uint64_t pixels = (uint64_t)w * h;
    if (pixels * 4 > SCREEN_MAX_RGBA_BYTES) return -1;
    uint64_t body = pixels * bpp;
    if (body > len) return -1;
    size_t header = len - (size_t)body;
    if (header != 12 && header != 16) return -1;

    uint8_t* out = malloc((size_t)pixels * 4);
    if (!out) return -1;
    const uint8_t* src = data + header;
    uint8_t* dst = out;
    for (uint64_t i = 0; i < pixels; i++, dst += 4, src += bpp) {
        switch (format) {
        case PF_RGBA_8888:
            memcpy(dst, src, 4);
            break;
        case PF_RGBX_8888:
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255;
            break;
        case PF_BGRA_8888:
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
            break;
        case PF_RGB_888:
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255;
            break;
        case PF_RGB_565: {
            uint32_t v = (uint32_t)src[0] | ((uint32_t)src[1] << 8);
            uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
            dst[0] = (uint8_t)((r << 3) | (r >> 2));
            dst[1] = (uint8_t)((g << 2) | (g >> 4));
            dst[2] = (uint8_t)((b << 3) | (b >> 2));
            dst[3] = 255;
            break;
        }
        default: {                          // PF_RGBA_1010102
            uint32_t v = rd32le(src);
            dst[0] = (uint8_t)((v & 0x3FF) >> 2);
            dst[1] = (uint8_t)(((v >> 10) & 0x3FF) >> 2);
            dst[2] = (uint8_t)(((v >> 20) & 0x3FF) >> 2);
            dst[3] = (uint8_t)(((v >> 30) & 0x3) * 85);
            break;
        }
        }
    }
    img->w = (int)w;
    img->h = (int)h;
    img->rgba = out;
    return 0;
}

void soulx_image_free(SoulXImage* img)
{
    if (!img) return;
    free(img->rgba);
    img->rgba = NULL;
    img->w = 0;
    img->h = 0;
}

// ---------------------------------------------------------------------------
// Capture through adb
// ---------------------------------------------------------------------------

int soulx_screen_capture(const char* serial, SoulXImage* img)
{
    if (!img) return -1;
    img->w = 0;
    img->h = 0;
    img->rgba = NULL;
    if (!serial) return -1;

    const char* args[] = { "screencap", NULL };
    unsigned char* data = NULL;
    size_t len = 0;
    if (soulx_adb_exec_out(serial, args, &data, &len, SCREEN_TIMEOUT_MS) != 0) {
        soulx_log(SOULX_LOG_ERROR, "screen capture failed (adb exec-out screencap)");
        free(data);
        return -1;
    }
    int rc = soulx_screencap_decode(data, len, img);
    if (rc != 0) {
        if (len >= 12) {
            soulx_log(SOULX_LOG_ERROR, "unexpected screencap data: %u x %u, pixel format %u, %zu bytes",
                     rd32le(data), rd32le(data + 4), rd32le(data + 8), len);
        } else {
            soulx_log(SOULX_LOG_ERROR, "unexpected screencap data (%zu bytes)", len);
        }
    }
    free(data);
    return rc;
}

int soulx_screen_save_png(const char* serial, const char* path)
{
    static const uint8_t png_magic[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (!serial || !path || !path[0]) {
        soulx_log(SOULX_LOG_ERROR, "screenshot: no output path");
        return -1;
    }
    const char* args[] = { "screencap", "-p", NULL };
    unsigned char* data = NULL;
    size_t len = 0;
    if (soulx_adb_exec_out(serial, args, &data, &len, SCREEN_TIMEOUT_MS) != 0) {
        soulx_log(SOULX_LOG_ERROR, "screenshot failed (adb exec-out screencap -p)");
        free(data);
        return -1;
    }
    if (len < sizeof(png_magic) + 12 || memcmp(data, png_magic, sizeof(png_magic)) != 0) {
        soulx_log(SOULX_LOG_ERROR, "screenshot failed: the device did not return a PNG image");
        free(data);
        return -1;
    }
    int rc = soulx_write_file_atomic(path, (const char*)data, len);
    free(data);
    if (rc != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot write %s", path);
        return -1;
    }
    return 0;
}
