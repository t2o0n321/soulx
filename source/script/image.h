/*
 * SoulX Android Emulator - image helpers and template matching (macOS script engine)
 *
 * SoulXImage (soulx_mac.h) is a tightly packed RGBA8888 buffer. Every buffer produced here is
 * plain malloc() memory: release it with soulx_image_free() (screen.c) or free(img->rgba).
 *
 * Template matching: zero-mean normalized cross-correlation (ZNCC) on grayscale. A coarse,
 * box-filtered level finds candidate positions, which are then refined at full resolution.
 * Template pixels with alpha < 128 are ignored; an optional colour check rejects matches whose
 * colours differ from the template (ZNCC alone only compares brightness structure).
 */

#ifndef SOULX_IMAGE_H
#define SOULX_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#include "soulx_mac.h"

#define SOULX_IMAGE_MAX_DIM                  16384   // largest accepted width or height
#define SOULX_IMAGE_MAX_FILE                 (64u * 1024u * 1024u)   // largest accepted image file
#define SOULX_MATCH_DEFAULT_THRESHOLD        0.88
#define SOULX_MATCH_DEFAULT_COLOR_TOLERANCE  40

typedef struct {
    double threshold;            // minimum score in (0, 1]; <= 0 selects the default (0.88)
    int region_x, region_y;      // search area in screen pixels; region_w <= 0 or region_h <= 0
    int region_w, region_h;      // means the whole screen. The area is clipped to the screen and
                                 // a match must lie entirely inside it.
    int color_tolerance;         // 1..255: reject a match when, for any of R/G/B, the mean absolute
                                 // difference over opaque template pixels exceeds it; 0 disables
    double scale;                // template rescale factor before matching; <= 0 or 1.0 = no rescale
} SoulXMatchOpts;

typedef struct {
    int x, y;                    // centre of the match: top-left + (template w / 2, h / 2)
    double score;                // ZNCC score, 1.0 = identical up to brightness/contrast
} SoulXMatch;

void soulx_match_opts_init(SoulXMatchOpts* o);           // defaults: 0.88, whole screen, tolerance 40

// Image helpers. All return 0 on success and -1 on error (see soulx_image_error()).
int  soulx_image_alloc(SoulXImage* img, int w, int h);   // zero-filled (transparent black)
int  soulx_image_load_png(const char* path, SoulXImage* out);   // PNG (or JPEG), converted to RGBA
int  soulx_image_load_mem(const unsigned char* data, size_t len, SoulXImage* out);
int  soulx_image_save_png(const char* path, const SoulXImage* img);   // atomic (tmp + rename)
int  soulx_image_crop(const SoulXImage* src, int x, int y, int w, int h, SoulXImage* out);
                                                       // rectangle must lie inside src
int  soulx_image_scale(const SoulXImage* src, double factor, SoulXImage* out);   // bilinear, alpha preserved
int32_t soulx_image_pixel(const SoulXImage* img, int x, int y);   // 0xRRGGBB, -1 when outside
const char* soulx_image_error(void);                    // last error message of this thread

// Template matching. o may be NULL (defaults).
// soulx_image_find: 1 = found (*best = best accepted match), 0 = not found (*best = the best
// rejected candidate, or x = y = -1 and score 0 when there was none), -1 = invalid arguments
// or out of memory.
int soulx_image_find(const SoulXImage* screen, const SoulXImage* tmpl, const SoulXMatchOpts* o,
                    SoulXMatch* best);
// soulx_image_find_all: number of matches written to out (sorted by score, best first;
// overlapping detections suppressed), or -1 on error.
int soulx_image_find_all(const SoulXImage* screen, const SoulXImage* tmpl, const SoulXMatchOpts* o,
                        SoulXMatch* out, int max);

#endif // SOULX_IMAGE_H
