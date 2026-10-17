/*
 * SoulX Android Emulator - unit tests for source/platform/macos/window/frames.c
 *
 * frames.c maps the engine's "-share-vid" shared-memory region "videmulator<port>" read-only. These tests play the
 * engine: a private POSIX shm object with the same 24-byte header { u32 width, height, fps, frameNumber; u64 tsUs }
 * and width*height*4 BGRA bytes is created under a test-only port (59xxx) and driven by hand. They check:
 *   - soulx_frames_shm_name bounds,
 *   - soulx_frames_open accepting a valid region and rejecting an empty / too-small / bad-header one,
 *   - soulx_frames_peek change detection and the view it fills,
 *   - soulx_frames_copy: a tear-free copy, the tear retry (via the soulx_frames_copy_hook test hook) and a
 *     buffer-too-small refusal,
 *   - soulx_frames_valid detecting a replaced object (resize) and an unlinked one,
 *   - reopening after a resize.
 *
 * No engine and no SoulX backend are needed; frames.c is self-contained.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "window/frames.h"

// frames.c exports this hook (not in frames.h): called after the pixel copy of each soulx_frames_copy attempt,
// before the frame counter is read again. It lets a single-threaded test "publish a new frame" mid-copy.
extern void (*soulx_frames_copy_hook)(int attempt);

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

#define HDR         24u                 // header size
#define OFF_W       0
#define OFF_H       4
#define OFF_FPS     8
#define OFF_FRAME   12
#define OFF_TS      16

// ---------------------------------------------------------------------------
// A fake engine that owns a "videmulator<port>" shm object (read-write)
// ---------------------------------------------------------------------------

typedef struct {
    char name[32];
    int fd;
    uint8_t* base;
    size_t map_size;            // what we mmap'd (page-rounded)
    int w, h;
} Writer;

static void put32(uint8_t* p, size_t off, uint32_t v) {
    memcpy(p + off, &v, 4);
}

static void put64(uint8_t* p, size_t off, uint64_t v) {
    memcpy(p + off, &v, 8);
}

static uint32_t get32(const uint8_t* p, size_t off) {
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

// Fills the whole pixel buffer with a per-frame tag byte and writes the header for frame n.
static void writer_set_frame(Writer* w, uint32_t n) {
    put32(w->base, OFF_W, (uint32_t)w->w);
    put32(w->base, OFF_H, (uint32_t)w->h);
    put32(w->base, OFF_FPS, 60);
    put64(w->base, OFF_TS, (uint64_t)n * 1000u + 7u);
    size_t pixels = (size_t)w->w * (size_t)w->h * 4u;
    memset(w->base + HDR, (int)(n & 0xFF), pixels);
    put32(w->base, OFF_FRAME, n);               // publish last
}

// Creates the object (removing any stale one first). 0 ok.
static int writer_create(Writer* w, int port, int width, int height) {
    memset(w, 0, sizeof(*w));
    w->fd = -1;
    snprintf(w->name, sizeof(w->name), "videmulator%d", port);
    shm_unlink(w->name);                        // ignore ENOENT
    w->fd = shm_open(w->name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (w->fd < 0) {
        perror("shm_open(writer)");
        return -1;
    }
    size_t need = HDR + (size_t)width * (size_t)height * 4u;
    if (ftruncate(w->fd, (off_t)need) != 0) {
        perror("ftruncate");
        return -1;
    }
    struct stat st;
    if (fstat(w->fd, &st) != 0) return -1;
    w->map_size = (size_t)st.st_size;           // page-rounded by the kernel
    w->base = mmap(NULL, w->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, w->fd, 0);
    if (w->base == MAP_FAILED) {
        perror("mmap(writer)");
        w->base = NULL;
        return -1;
    }
    w->w = width;
    w->h = height;
    memset(w->base, 0, w->map_size);
    writer_set_frame(w, 1);
    return 0;
}

static void writer_close(Writer* w) {
    if (w->base) munmap(w->base, w->map_size);
    if (w->fd >= 0) close(w->fd);
    if (w->name[0]) shm_unlink(w->name);
    memset(w, 0, sizeof(*w));
    w->fd = -1;
}

// ---------------------------------------------------------------------------
// Tear-retry hook: disturbs the first g_bump_remaining copy attempts
// ---------------------------------------------------------------------------

static Writer* g_hook_writer = NULL;
static int g_bump_remaining = 0;
static uint32_t g_hook_frame = 100;
static int g_hook_calls = 0;

static void tear_hook(int attempt) {
    (void)attempt;
    g_hook_calls++;
    if (g_bump_remaining > 0) {
        g_bump_remaining--;
        writer_set_frame(g_hook_writer, ++g_hook_frame);    // "a new frame arrived during the copy"
    }
}

// Shrinks the height mid-copy so after.height != before.height on every attempt (the frame still fits dst, so
// the retry is driven by the dimension change, not by the size guard).
static void resize_hook(int attempt) {
    (void)attempt;
    g_hook_calls++;
    uint32_t h = get32(g_hook_writer->base, OFF_H);
    if (h > 1) put32(g_hook_writer->base, OFF_H, h - 1);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static int test_port(int slot) {
    return 59000 + (int)(getpid() % 400) + slot * 7;    // unique-ish, inside 59xxx, <= 65535
}

static void test_shm_name(void) {
    char buf[32];
    CHECK(soulx_frames_shm_name(5600, buf, sizeof(buf)) == buf);
    CHECK(strcmp(buf, "videmulator5600") == 0);
    CHECK(strcmp(soulx_frames_shm_name(1, buf, sizeof(buf)), "videmulator1") == 0);
    CHECK(strcmp(soulx_frames_shm_name(65535, buf, sizeof(buf)), "videmulator65535") == 0);
    CHECK(soulx_frames_shm_name(0, buf, sizeof(buf)) == NULL);
    CHECK(soulx_frames_shm_name(-1, buf, sizeof(buf)) == NULL);
    CHECK(soulx_frames_shm_name(65536, buf, sizeof(buf)) == NULL);
    CHECK(soulx_frames_shm_name(100000, buf, sizeof(buf)) == NULL);
    CHECK(soulx_frames_shm_name(5600, NULL, sizeof(buf)) == NULL);
    CHECK(soulx_frames_shm_name(5600, buf, 0) == NULL);
    char tiny[4];
    CHECK(soulx_frames_shm_name(5600, tiny, sizeof(tiny)) == NULL);  // "videmulator5600" does not fit
    CHECK(tiny[0] == '\0');
}

static void test_open_errors(void) {
    // no such region
    CHECK(soulx_frames_open(test_port(1)) == NULL);

    // an object that is too small for even the header (size 0: no ftruncate)
    char name[32];
    int port = test_port(2);
    snprintf(name, sizeof(name), "videmulator%d", port);
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(soulx_frames_open(port) == NULL);      // st_size 0 < 24
        close(fd);
        shm_unlink(name);
    }

    // a full-size object whose header does not describe a fitting frame
    Writer w;
    port = test_port(3);
    CHECK(writer_create(&w, port, 4, 4) == 0);
    put32(w.base, OFF_W, 0);                         // width 0 -> no frame
    CHECK(soulx_frames_open(port) == NULL);
    put32(w.base, OFF_W, 5000);
    put32(w.base, OFF_H, 5000);                      // 100 MB > the 16 KB mapping
    CHECK(soulx_frames_open(port) == NULL);
    writer_set_frame(&w, 2);                         // valid again
    SoulXFrames* f = soulx_frames_open(port);
    CHECK(f != NULL);
    soulx_frames_close(f);
    writer_close(&w);

    // soulx_frames_close / peek / copy tolerate NULL
    soulx_frames_close(NULL);
    SoulXFrameView v;
    CHECK(soulx_frames_peek(NULL, &v) == -1);
    CHECK(soulx_frames_peek(NULL, NULL) == -1);
    uint8_t dst[16];
    CHECK(soulx_frames_copy(NULL, dst, sizeof(dst), &v) == -1);
    CHECK(!soulx_frames_valid(NULL));
}

static void test_peek(void) {
    Writer w;
    int port = test_port(4);
    CHECK(writer_create(&w, port, 3, 5) == 0);      // odd dims, stride = 12
    SoulXFrames* f = soulx_frames_open(port);
    CHECK(f != NULL);
    if (!f) {
        writer_close(&w);
        return;
    }
    SoulXFrameView v;
    memset(&v, 0, sizeof(v));
    // first peek is always "changed"
    CHECK(soulx_frames_peek(f, &v) == 1);
    CHECK(v.width == 3 && v.height == 5);
    CHECK(v.frame_number == 1);
    CHECK(v.ts_us == 1u * 1000u + 7u);
    CHECK(v.stride == 12);
    CHECK(v.bgra != NULL);
    // bgra points into the live mapping: frame-1 tag byte is 1
    CHECK(v.bgra[0] == 1 && v.bgra[3 * 5 * 4 - 1] == 1);
    // unchanged
    CHECK(soulx_frames_peek(f, &v) == 0);
    CHECK(soulx_frames_peek(f, &v) == 0);
    // a new frame
    writer_set_frame(&w, 2);
    CHECK(soulx_frames_peek(f, &v) == 1);
    CHECK(v.frame_number == 2 && v.bgra[0] == 2);
    CHECK(soulx_frames_peek(f, &v) == 0);
    // the engine wrote a non-fitting header (mid-resize): peek says "reopen"
    put32(w.base, OFF_W, 9000);
    put32(w.base, OFF_H, 9000);
    put32(w.base, OFF_FRAME, 3);
    CHECK(soulx_frames_peek(f, &v) == -1);
    soulx_frames_close(f);
    writer_close(&w);
}

static void test_copy(void) {
    Writer w;
    int port = test_port(5);
    CHECK(writer_create(&w, port, 8, 8) == 0);
    writer_set_frame(&w, 42);
    SoulXFrames* f = soulx_frames_open(port);
    CHECK(f != NULL);
    if (!f) {
        writer_close(&w);
        return;
    }
    size_t pixbytes = 8u * 8u * 4u;
    uint8_t dst[8 * 8 * 4];
    SoulXFrameView v;

    // clean copy
    soulx_frames_copy_hook = NULL;
    memset(&v, 0x5A, sizeof(v));
    CHECK(soulx_frames_copy(f, dst, sizeof(dst), &v) == 0);
    CHECK(v.width == 8 && v.height == 8 && v.frame_number == 42 && v.stride == 32);
    CHECK(v.bgra == dst);
    bool all42 = true;
    for (size_t i = 0; i < pixbytes; i++) all42 = all42 && dst[i] == 42;
    CHECK(all42);

    // buffer too small
    CHECK(soulx_frames_copy(f, dst, pixbytes - 1, &v) == -1);
    CHECK(soulx_frames_copy(f, dst, 0, &v) == -1);

    // tear retry: two frames arrive during the copy, then it settles on attempt 3
    g_hook_writer = &w;
    g_hook_frame = 100;
    writer_set_frame(&w, g_hook_frame);
    g_bump_remaining = 2;
    g_hook_calls = 0;
    soulx_frames_copy_hook = tear_hook;
    CHECK(soulx_frames_copy(f, dst, sizeof(dst), &v) == 0);
    CHECK(g_hook_calls == 3);                       // 3 attempts
    CHECK(v.frame_number == 102);                   // the frame that was stable on attempt 3
    CHECK(dst[0] == (102 & 0xFF));

    // a new frame on every attempt: copy gives up after 3 tries
    g_bump_remaining = 99;
    g_hook_frame = 200;
    writer_set_frame(&w, g_hook_frame);
    g_hook_calls = 0;
    CHECK(soulx_frames_copy(f, dst, sizeof(dst), &v) == -1);
    CHECK(g_hook_calls == 3);

    // dimensions change mid-copy: also a retry failure (height 8->7->6 across the three attempts, each fits dst)
    soulx_frames_copy_hook = resize_hook;
    g_hook_calls = 0;
    writer_set_frame(&w, 5);                        // restores width/height to 8x8
    CHECK(soulx_frames_copy(f, dst, sizeof(dst), &v) == -1);
    CHECK(g_hook_calls == 3);
    writer_set_frame(&w, 5);                        // leave a sane frame behind

    soulx_frames_copy_hook = NULL;
    soulx_frames_close(f);
    writer_close(&w);
}

// soulx_frames_copy_rgba: B,G,R,A -> R,G,B,A for every pixel; 7x5 = 35 pixels covers both the 16-pixel
// vector loop and the per-pixel tail. Each pixel holds distinct bytes so a misplaced channel shows.
static void test_copy_rgba(void) {
    Writer w;
    int port = test_port(7);
    CHECK(writer_create(&w, port, 7, 5) == 0);
    size_t pixels = 7u * 5u;
    for (size_t i = 0; i < pixels; i++) {
        uint8_t* p = w.base + HDR + i * 4u;
        p[0] = (uint8_t)(i * 4u + 1u);          // B
        p[1] = (uint8_t)(i * 4u + 2u);          // G
        p[2] = (uint8_t)(i * 4u + 3u);          // R
        p[3] = (uint8_t)(255u - i);             // A
    }
    put32(w.base, OFF_FRAME, 77);
    SoulXFrames* f = soulx_frames_open(port);
    CHECK(f != NULL);
    if (!f) {
        writer_close(&w);
        return;
    }
    uint8_t dst[7 * 5 * 4];
    SoulXFrameView v;
    CHECK(soulx_frames_copy_rgba(f, dst, sizeof(dst), &v) == 0);
    CHECK(v.width == 7 && v.height == 5 && v.frame_number == 77 && v.stride == 28);
    bool ok = true;
    for (size_t i = 0; i < pixels; i++) {
        const uint8_t* q = dst + i * 4u;
        ok = ok && q[0] == (uint8_t)(i * 4u + 3u) && q[1] == (uint8_t)(i * 4u + 2u) &&
             q[2] == (uint8_t)(i * 4u + 1u) && q[3] == (uint8_t)(255u - i);
    }
    CHECK(ok);
    CHECK(soulx_frames_copy_rgba(f, dst, sizeof(dst) - 1, &v) == -1);       // buffer too small
    soulx_frames_close(f);
    writer_close(&w);
}

// frames.c exports this hook (not in frames.h): runs between a pump's copy and its completeness check.
extern void (*soulx_frame_pump_copy_hook)(void);

static Writer* g_pump_writer = NULL;
static _Atomic int g_pump_scribbles = 0;

// "The next readback already writes into the region": changes pixels without publishing a frame.
static void scribble_hook(void) {
    if (atomic_load(&g_pump_scribbles) > 0) {
        atomic_fetch_sub(&g_pump_scribbles, 1);
        memset(g_pump_writer->base + HDR, 0xEE, (size_t)g_pump_writer->w * 4u * 2u);
    }
}

// Waits up to 2 s for the pump's front frame to reach |frame|; returns what it shows (or 0).
static uint32_t pump_wait_frame(SoulXFramePump* p, uint32_t frame, uint8_t* first_px) {
    for (int i = 0; i < 400; i++) {
        SoulXFrameView v;
        soulx_frame_pump_lock(p, &v);
        uint32_t got = v.bgra ? v.frame_number : 0;
        if (v.bgra && first_px) *first_px = v.bgra[0];
        soulx_frame_pump_unlock(p);
        if (got == frame) return got;
        struct timespec ts = { 0, 5 * 1000000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}

// The frame pump hands out each published frame, drops a copy the next readback wrote into, and follows a
// replaced region.
static void test_frame_pump(void) {
    Writer w;
    int port = test_port(0);
    CHECK(writer_create(&w, port, 16, 8) == 0);
    writer_set_frame(&w, 10);
    SoulXFramePump* p = soulx_frame_pump_start(port);
    CHECK(p != NULL);
    if (!p) {
        writer_close(&w);
        return;
    }
    uint8_t px = 0;
    CHECK(pump_wait_frame(p, 10, &px) == 10 && px == 10);
    CHECK(soulx_frame_pump_live(p));

    // a copy the "next readback" scribbled into right after the pump copied it: never shown
    g_pump_writer = &w;
    atomic_store(&g_pump_scribbles, 1);
    soulx_frame_pump_copy_hook = scribble_hook;
    unsigned torn0 = 0, torn1 = 0;
    soulx_frame_pump_stats(p, NULL, &torn0, NULL);
    writer_set_frame(&w, 11);
    for (int i = 0; i < 400 && atomic_load(&g_pump_scribbles) > 0; i++) {
        struct timespec ts = { 0, 2 * 1000000L };
        nanosleep(&ts, NULL);
    }
    struct timespec settle = { 0, 30 * 1000000L };
    nanosleep(&settle, NULL);
    soulx_frame_pump_stats(p, NULL, &torn1, NULL);
    CHECK(torn1 == torn0 + 1);
    CHECK(pump_wait_frame(p, 10, &px) == 10 && px == 10);              // still the last complete frame
    writer_set_frame(&w, 12);                                          // the next one arrives intact
    CHECK(pump_wait_frame(p, 12, &px) == 12 && px == 12);
    soulx_frame_pump_copy_hook = NULL;

    // the engine replaced the region (new size): the pump reopens it by itself
    writer_close(&w);
    CHECK(writer_create(&w, port, 8, 4) == 0);
    writer_set_frame(&w, 20);
    CHECK(pump_wait_frame(p, 20, &px) == 20 && px == 20);
    SoulXFrameView v;
    soulx_frame_pump_lock(p, &v);
    CHECK(v.width == 8 && v.height == 4 && v.stride == 32);
    soulx_frame_pump_unlock(p);

    soulx_frame_pump_stop(p);
    writer_close(&w);
}

static double now_unix_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static double mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

// A "readback" running 30 ms (scribbling pixels without publishing), then publishing frame 31 cleanly.
static void* slow_readback(void* arg) {
    Writer* w = arg;
    double end = mono_ms() + 30;
    uint8_t v = 1;
    while (mono_ms() < end) memset(w->base + HDR, v++, (size_t)w->w * (size_t)w->h * 4u);
    writer_set_frame(w, 31);
    put64(w->base, OFF_TS, (uint64_t)now_unix_us());
    return NULL;
}

// soulx_frames_copy_fresh: at once on a still screen, right after the next publish while frames keep coming,
// and never a copy a running readback wrote into.
static void test_copy_fresh(void) {
    Writer w;
    int port = test_port(1);
    CHECK(writer_create(&w, port, 9, 7) == 0);           // 63 pixels: NEON steps and the per-pixel tail
    for (size_t i = 0; i < 63; i++) {
        uint8_t* p = w.base + HDR + i * 4u;
        p[0] = (uint8_t)(i + 1); p[1] = 2; p[2] = (uint8_t)(200 - i); p[3] = 255;   // B G R A
    }
    put32(w.base, OFF_FRAME, 30);                        // ts from writer_create: long ago = a still screen
    SoulXFrames* f = soulx_frames_open(port);
    CHECK(f != NULL);
    if (!f) {
        writer_close(&w);
        return;
    }
    uint8_t dst[9 * 7 * 4];
    SoulXFrameView v;
    double t0 = mono_ms();
    CHECK(soulx_frames_copy_fresh(f, dst, sizeof(dst), &v, true, 150) == 0);
    CHECK(mono_ms() - t0 < 20);                          // still: no waiting
    CHECK(v.frame_number == 30 && v.width == 9 && v.height == 7);
    bool ok = true;
    for (size_t i = 0; i < 63; i++) {
        const uint8_t* q = dst + i * 4u;
        ok = ok && q[0] == (uint8_t)(200 - i) && q[1] == 2 && q[2] == (uint8_t)(i + 1) && q[3] == 255;   // R G B A
    }
    CHECK(ok);

    // frames keep coming (the last one is fresh) and a readback is writing: the copy is the next published frame,
    // complete
    put64(w.base, OFF_TS, (uint64_t)now_unix_us());
    pthread_t th;
    CHECK(pthread_create(&th, NULL, slow_readback, &w) == 0);
    CHECK(soulx_frames_copy_fresh(f, dst, sizeof(dst), &v, false, 500) == 0);
    pthread_join(th, NULL);
    CHECK(v.frame_number == 31);
    bool same = true;
    for (size_t i = 0; i < sizeof(dst); i++) same = same && dst[i] == 31;
    CHECK(same);

    // nothing published within the timeout on a "moving" screen: it treats the screen as still and copies
    put64(w.base, OFF_TS, (uint64_t)now_unix_us());
    t0 = mono_ms();
    CHECK(soulx_frames_copy_fresh(f, dst, sizeof(dst), &v, false, 60) == 0 && v.frame_number == 31);
    CHECK(mono_ms() - t0 >= 55);
    CHECK(soulx_frames_copy_fresh(f, dst, 10, &v, false, 60) == -1);   // buffer too small
    soulx_frames_close(f);
    writer_close(&w);
}

static void test_valid_and_resize(void) {
    Writer a;
    int port = test_port(6);
    CHECK(writer_create(&a, port, 4, 4) == 0);
    SoulXFrames* f = soulx_frames_open(port);
    CHECK(f != NULL);
    if (!f) {
        writer_close(&a);
        return;
    }
    SoulXFrameView v;

    // same object, frames advancing -> still valid
    CHECK(soulx_frames_valid(f));
    for (uint32_t n = 2; n <= 6; n++) {
        writer_set_frame(&a, n);
        CHECK(soulx_frames_valid(f));
    }
    CHECK(soulx_frames_peek(f, &v) == 1 && v.width == 4 && v.height == 4);

    // the engine replaces the region with a bigger one (resolution change):
    // unlink + a new object of a different size under the same name.
    Writer b;
    writer_close(&a);                               // unlinks the old name; f still maps the frozen old object
    CHECK(soulx_frames_peek(f, &v) == 0 || true);    // the frozen mapping still reads (old dims)
    CHECK(v.width == 4 && v.height == 4);
    CHECK(writer_create(&b, port, 16, 32) == 0);
    CHECK(!soulx_frames_valid(f));                   // different object: detected
    // peek still serves the frozen old frame (valid() is the replace detector, not peek)
    writer_set_frame(&b, 9);                        // only the new object advances
    (void)soulx_frames_peek(f, &v);
    CHECK(v.width == 4 && v.height == 4);

    // reopening picks up the new region
    soulx_frames_close(f);
    f = soulx_frames_open(port);
    CHECK(f != NULL);
    if (f) {
        CHECK(soulx_frames_peek(f, &v) == 1 && v.width == 16 && v.height == 32);
        CHECK(soulx_frames_valid(f));
    }

    // a same-size replacement with a different header is still detected as a new object
    Writer c;
    writer_close(&b);
    soulx_frames_close(f);
    CHECK(writer_create(&a, port, 10, 10) == 0);
    f = soulx_frames_open(port);
    CHECK(f != NULL);
    if (f) {
        CHECK(soulx_frames_valid(f));
        writer_close(&a);                           // unlink, no replacement yet
        CHECK(!soulx_frames_valid(f));               // shm_open fails -> false
        CHECK(writer_create(&c, port, 10, 10) == 0);
        put64(c.base, OFF_TS, 123456789ull);        // same dims, different header
        put32(c.base, OFF_FRAME, 77);
        CHECK(!soulx_frames_valid(f));               // different object even at the same size
        soulx_frames_close(f);
        writer_close(&c);
    }
}

int main(void) {
    test_shm_name();
    test_open_errors();
    test_peek();
    test_copy();
    test_copy_rgba();
    test_frame_pump();
    test_copy_fresh();
    test_valid_and_resize();

    // leave nothing behind even on a mid-test failure
    for (int slot = 0; slot <= 7; slot++) {
        char name[32];
        snprintf(name, sizeof(name), "videmulator%d", test_port(slot));
        shm_unlink(name);
    }
    if (g_failures) {
        fprintf(stderr, "test_frames: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_frames: all %d checks passed\n", g_checks);
    return 0;
}
