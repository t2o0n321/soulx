/*
 * SoulX Android Emulator - shared-memory frame reader for SoulX's window (macOS)
 *
 * Maps the engine's -share-vid region "videmulator<console port>" read-only (layout in frames.h) and hands out
 * the current frame without copying (peek) or as a tear-checked copy (copy).
 *
 * Measured on macOS 26 (Apple Silicon): fstat() on a POSIX shared-memory descriptor reports st_dev = st_ino = 0
 * and the size rounded up to the 16 KB page, and an object can be ftruncate()d only once. A resolution change
 * therefore means a NEW object under the same name, and the object identity cannot come from fstat().
 * soulx_frames_valid() compares the size and the 24-byte header seen through a fresh mapping of the name with the
 * header of our mapping (frame counter + microsecond timestamp: two different objects practically never agree).
 *
 * Threads: peek updates the change detection of the reader; copy and valid only read, so they may run on
 * another thread while one thread peeks. close must not race with anything. The frame pump (below) owns its own
 * reader on its own thread.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "frames.h"

#define FRAMES_HEADER_SIZE      24u     // u32 width, height, fps, frameNumber; u64 tsUs
#define FRAMES_MAX_DIM          16384u  // sanity bound per side (1 GB of pixels at most)
#define FRAMES_NAME_MAX         32      // PSHMNAMLEN is 31 on macOS
#define FRAMES_COPY_ATTEMPTS    3
#define FRAMES_VALID_ROUNDS     3       // header comparisons before an object counts as replaced

#define OFF_WIDTH               0
#define OFF_HEIGHT              4
#define OFF_FPS                 8
#define OFF_FRAME               12
#define OFF_TS                  16

struct SoulXFrames {
    char name[FRAMES_NAME_MAX];
    const uint8_t* base;        // whole object, PROT_READ, MAP_SHARED
    size_t size;                // st_size at open (page-rounded)
    uint32_t last_frame;        // frame number of the previous successful peek
    bool have_last;
};

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t frame;
    uint64_t ts_us;
} FrameHeader;

// Exported for tests/macos/test_frames.c (not part of frames.h): called after the pixel copy of every
// soulx_frames_copy() attempt, before the frame number is read again (lets a test move the writer on).
extern void (*soulx_frames_copy_hook)(int attempt);
void (*soulx_frames_copy_hook)(int attempt) = NULL;

// ---------------------------------------------------------------------------
// Header access (the engine writes the region concurrently; the fields are naturally aligned)
// ---------------------------------------------------------------------------

static uint32_t load32(const uint8_t* base, size_t off) {
    return __atomic_load_n((const uint32_t*)(const void*)(base + off), __ATOMIC_RELAXED);
}

static uint64_t load64(const uint8_t* base, size_t off) {
    return __atomic_load_n((const uint64_t*)(const void*)(base + off), __ATOMIC_RELAXED);
}

// The frame number first (acquire): what the engine wrote before publishing it is visible afterwards.
static void read_header(const uint8_t* base, FrameHeader* h) {
    h->frame = __atomic_load_n((const uint32_t*)(const void*)(base + OFF_FRAME), __ATOMIC_ACQUIRE);
    h->width = load32(base, OFF_WIDTH);
    h->height = load32(base, OFF_HEIGHT);
    h->fps = load32(base, OFF_FPS);
    h->ts_us = load64(base, OFF_TS);
}

static bool same_header(const FrameHeader* a, const FrameHeader* b) {
    return a->width == b->width && a->height == b->height && a->fps == b->fps && a->frame == b->frame &&
           a->ts_us == b->ts_us;
}

// Header size + pixels of the frame the header describes; false for empty or absurd dimensions.
static bool frame_bytes(const FrameHeader* h, size_t* total) {
    if (h->width == 0 || h->height == 0 || h->width > FRAMES_MAX_DIM || h->height > FRAMES_MAX_DIM) return false;
    *total = FRAMES_HEADER_SIZE + (size_t)h->width * (size_t)h->height * 4u;
    return true;
}

static bool header_fits(const FrameHeader* h, size_t mapped) {
    size_t total;
    return frame_bytes(h, &total) && total <= mapped;
}

static void fill_view(SoulXFrameView* view, const FrameHeader* h, const uint8_t* pixels) {
    view->width = (int)h->width;
    view->height = (int)h->height;
    view->frame_number = h->frame;
    view->ts_us = h->ts_us;
    view->bgra = pixels;
    view->stride = (size_t)h->width * 4u;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

const char* soulx_frames_shm_name(int console_port, char* buf, size_t cap) {
    if (!buf || cap == 0) return NULL;
    buf[0] = '\0';
    if (console_port <= 0 || console_port > 65535) return NULL;
    int n = snprintf(buf, cap, "videmulator%d", console_port);
    if (n < 0 || (size_t)n >= cap) {
        buf[0] = '\0';
        return NULL;
    }
    return buf;
}

// errno on NULL: ENOENT = no region (yet), EINVAL = invalid port or the header does not describe a frame that
// fits the region (not written yet, resized), EACCES = the region belongs to another user, ENOMEM.
SoulXFrames* soulx_frames_open(int console_port) {
    char name[FRAMES_NAME_MAX];
    if (!soulx_frames_shm_name(console_port, name, sizeof(name))) {
        errno = EINVAL;
        return NULL;
    }
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return NULL;
    }
    if (st.st_uid != geteuid()) {               // only ever show frames written by our own engine
        close(fd);
        errno = EACCES;
        return NULL;
    }
    if (st.st_size < (off_t)FRAMES_HEADER_SIZE) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    void* map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    int e = errno;
    close(fd);                                  // the mapping keeps the object alive
    if (map == MAP_FAILED) {
        errno = e;
        return NULL;
    }

    FrameHeader h;
    read_header(map, &h);
    if (!header_fits(&h, size)) {
        munmap(map, size);
        errno = EINVAL;
        return NULL;
    }
    SoulXFrames* f = calloc(1, sizeof(*f));
    if (!f) {
        munmap(map, size);
        errno = ENOMEM;
        return NULL;
    }
    memcpy(f->name, name, sizeof(name));
    f->base = map;
    f->size = size;
    return f;
}

int soulx_frames_peek(SoulXFrames* f, SoulXFrameView* view) {
    if (!view) return -1;
    memset(view, 0, sizeof(*view));
    if (!f) return -1;
    FrameHeader h;
    read_header(f->base, &h);
    if (!header_fits(&h, f->size)) return -1;
    fill_view(view, &h, f->base + FRAMES_HEADER_SIZE);
    int changed = (!f->have_last || h.frame != f->last_frame) ? 1 : 0;
    f->last_frame = h.frame;
    f->have_last = true;
    return changed;
}

// B,G,R,A -> R,G,B,A while copying, 16 pixels per NEON step (one pass over memory, bandwidth bound).
static void copy_bgra_to_rgba(uint8_t* dst, const uint8_t* src, size_t bytes) {
    size_t i = 0;
#if defined(__ARM_NEON)
    for (; i + 64 <= bytes; i += 64) {
        uint8x16x4_t px = vld4q_u8(src + i);    // de-interleaves: val[0] = B, val[1] = G, val[2] = R, val[3] = A
        uint8x16_t b = px.val[0];
        px.val[0] = px.val[2];
        px.val[2] = b;
        vst4q_u8(dst + i, px);
    }
#endif
    for (; i + 4 <= bytes; i += 4) {
        dst[i] = src[i + 2];
        dst[i + 1] = src[i + 1];
        dst[i + 2] = src[i];
        dst[i + 3] = src[i + 3];
    }
}

static int copy_frame(SoulXFrames* f, uint8_t* dst, size_t dst_size, SoulXFrameView* view, bool rgba) {
    if (view) memset(view, 0, sizeof(*view));
    if (!f || !dst) return -1;
    for (int attempt = 1; attempt <= FRAMES_COPY_ATTEMPTS; attempt++) {
        FrameHeader before;
        read_header(f->base, &before);
        size_t total;
        if (!frame_bytes(&before, &total) || total > f->size) return -1;
        size_t pixels = total - FRAMES_HEADER_SIZE;
        if (pixels > dst_size) return -1;
        if (rgba) copy_bgra_to_rgba(dst, f->base + FRAMES_HEADER_SIZE, pixels);
        else memcpy(dst, f->base + FRAMES_HEADER_SIZE, pixels);
        if (soulx_frames_copy_hook) soulx_frames_copy_hook(attempt);
        atomic_thread_fence(memory_order_acquire);   // the pixel loads complete before the counter is re-read
        FrameHeader after;
        read_header(f->base, &after);
        if (after.frame == before.frame && after.width == before.width && after.height == before.height) {
            if (view) fill_view(view, &before, dst);
            return 0;
        }
    }
    return -1;                                  // the engine published a new frame during every attempt
}

int soulx_frames_copy(SoulXFrames* f, uint8_t* dst, size_t dst_size, SoulXFrameView* view) {
    return copy_frame(f, dst, dst_size, view, false);
}

int soulx_frames_copy_rgba(SoulXFrames* f, uint8_t* dst, size_t dst_size, SoulXFrameView* view) {
    return copy_frame(f, dst, dst_size, view, true);
}

bool soulx_frames_valid(SoulXFrames* f) {
    if (!f) return false;
    FrameHeader ours;
    read_header(f->base, &ours);
    if (!header_fits(&ours, f->size)) return false;

    int fd = shm_open(f->name, O_RDONLY, 0);
    if (fd < 0) return false;                   // unlinked: the engine exited or replaced it
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && st.st_uid == geteuid() && st.st_size >= (off_t)FRAMES_HEADER_SIZE &&
              (size_t)st.st_size == f->size;
    void* cur = MAP_FAILED;
    if (ok) cur = mmap(NULL, FRAMES_HEADER_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (!ok || cur == MAP_FAILED) return false;

    // Same object = the same header through both mappings. Read ours before and after theirs, so a frame
    // published in between does not count as a difference; retry a few times for the same reason.
    bool same = false;
    for (int round = 0; round < FRAMES_VALID_ROUNDS && !same; round++) {
        FrameHeader a, theirs, b;
        read_header(f->base, &a);
        read_header(cur, &theirs);
        read_header(f->base, &b);
        same = same_header(&theirs, &a) || same_header(&theirs, &b);
    }
    munmap(cur, FRAMES_HEADER_SIZE);
    return same;
}

void soulx_frames_close(SoulXFrames* f) {
    if (!f) return;
    munmap((void*)f->base, f->size);
    free(f);
}

// ---------------------------------------------------------------------------
// Frame pump: complete frames for the window
// ---------------------------------------------------------------------------
//
// The engine (VideoFrameSharer::frameAvailable, read with IDA) reads the framebuffer back STRAIGHT INTO the
// shared pixels and only then stores the new frame number and timestamp. A reader that copies while that
// readback runs gets rows of two frames (the streaks users saw), and the frame number cannot reveal it: it only
// changes after the readback. So the pump copies right after a frame number change, before the next readback
// can start, and then checks that the pixels it copied are still what the region holds (every
// PUMP_CHECK_STEP-th row). A difference means the next readback is already writing: that copy is dropped and
// the next frame is taken instead. Complete frames go to a front buffer the window copies from under a lock.

#define PUMP_POLL_MS            1.0     // frame number polling while frames arrive
#define PUMP_IDLE_POLL_MS       4.0     // ... after PUMP_IDLE_AFTER_MS without a new frame
#define PUMP_IDLE_AFTER_MS      1000.0
#define PUMP_REOPEN_MS          1000.0  // open / validity check of the region
#define PUMP_CHECK_STEP         4       // compared rows after a copy: every 4th

void (*soulx_frame_pump_copy_hook)(void) = NULL;     // tests: runs between a pump's copy and its check

struct SoulXFramePump {
    int port;
    pthread_t thread;
    _Atomic bool stop;
    _Atomic bool live;              // the region is mapped
    pthread_mutex_t lock;           // front buffer + its view
    uint8_t* front;
    size_t front_cap;
    SoulXFrameView front_view;       // bgra = front (NULL = no frame yet)
    uint8_t* back;                  // pump thread only
    size_t back_cap;
    SoulXFrames* frames;             // pump thread only
    _Atomic unsigned copied, torn, dropped;
};

static double pump_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void pump_sleep_ms(double ms) {
    struct timespec ts = { (time_t)(ms / 1000.0), (long)((ms - (double)(time_t)(ms / 1000.0) * 1000.0) * 1e6) };
    nanosleep(&ts, NULL);
}

static bool pump_reserve(uint8_t** buf, size_t* cap, size_t need) {
    if (*cap >= need) return true;
    uint8_t* b = realloc(*buf, need);
    if (!b) return false;
    *buf = b;
    *cap = need;
    return true;
}

// The copy still equals the region on every PUMP_CHECK_STEP-th row: no readback wrote into it meanwhile.
static bool pump_copy_intact(const uint8_t* copy, const uint8_t* live, size_t stride, int height) {
    for (int y = 0; y < height; y += PUMP_CHECK_STEP) {
        if (memcmp(copy + (size_t)y * stride, live + (size_t)y * stride, stride) != 0) return false;
    }
    return memcmp(copy + (size_t)(height - 1) * stride, live + (size_t)(height - 1) * stride, stride) == 0;
}

static void* pump_main(void* arg) {
    SoulXFramePump* p = arg;
    double check_at = 0, last_new = pump_now_ms();
    uint32_t last = 0;
    bool have_last = false;
    while (!atomic_load(&p->stop)) {
        double now = pump_now_ms();
        if (now >= check_at) {
            check_at = now + PUMP_REOPEN_MS;
            if (p->frames && !soulx_frames_valid(p->frames)) {
                soulx_frames_close(p->frames);
                p->frames = NULL;
            }
            if (!p->frames) {
                p->frames = soulx_frames_open(p->port);
                have_last = false;
            }
            atomic_store(&p->live, p->frames != NULL);
        }
        if (!p->frames) {
            pump_sleep_ms(50);
            continue;
        }
        FrameHeader h;
        read_header(p->frames->base, &h);
        size_t total;
        if (!frame_bytes(&h, &total) || total > p->frames->size) {
            check_at = 0;                       // resized or replaced: validate and reopen at once
            pump_sleep_ms(PUMP_POLL_MS);
            continue;
        }
        if (have_last && h.frame == last) {
            pump_sleep_ms(now - last_new > PUMP_IDLE_AFTER_MS ? PUMP_IDLE_POLL_MS : PUMP_POLL_MS);
            continue;
        }
        last_new = now;
        size_t bytes = total - FRAMES_HEADER_SIZE, stride = (size_t)h.width * 4u;
        const uint8_t* live = p->frames->base + FRAMES_HEADER_SIZE;
        if (!pump_reserve(&p->back, &p->back_cap, bytes)) {
            pump_sleep_ms(PUMP_POLL_MS);
            continue;
        }
        memcpy(p->back, live, bytes);
        if (soulx_frame_pump_copy_hook) soulx_frame_pump_copy_hook();
        atomic_thread_fence(memory_order_acquire);
        FrameHeader after;
        read_header(p->frames->base, &after);
        bool complete = after.frame == h.frame && after.width == h.width && after.height == h.height &&
                        pump_copy_intact(p->back, live, stride, (int)h.height);
        if (have_last && h.frame > last + 1 && h.frame - last < 100000) atomic_fetch_add(&p->dropped, h.frame - last - 1);
        last = h.frame;
        have_last = true;
        if (!complete) {                        // the next readback is writing: take the next frame instead
            atomic_fetch_add(&p->torn, 1);
            continue;
        }
        pthread_mutex_lock(&p->lock);
        uint8_t* b = p->front;
        size_t c = p->front_cap;
        p->front = p->back;
        p->front_cap = p->back_cap;
        p->back = b;
        p->back_cap = c;
        fill_view(&p->front_view, &h, p->front);
        pthread_mutex_unlock(&p->lock);
        atomic_fetch_add(&p->copied, 1);
    }
    if (p->frames) soulx_frames_close(p->frames);
    p->frames = NULL;
    atomic_store(&p->live, false);
    return NULL;
}

SoulXFramePump* soulx_frame_pump_start(int console_port) {
    SoulXFramePump* p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->port = console_port;
    pthread_mutex_init(&p->lock, NULL);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    int rc = pthread_create(&p->thread, &attr, pump_main, p);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_destroy(&p->lock);
        free(p);
        return NULL;
    }
    return p;
}

void soulx_frame_pump_stop(SoulXFramePump* p) {
    if (!p) return;
    atomic_store(&p->stop, true);
    pthread_join(p->thread, NULL);
    pthread_mutex_destroy(&p->lock);
    free(p->front);
    free(p->back);
    free(p);
}

bool soulx_frame_pump_live(SoulXFramePump* p) {
    return p && atomic_load(&p->live);
}

void soulx_frame_pump_lock(SoulXFramePump* p, SoulXFrameView* view) {
    if (view) memset(view, 0, sizeof(*view));
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    if (view) *view = p->front_view;
}

void soulx_frame_pump_unlock(SoulXFramePump* p) {
    if (p) pthread_mutex_unlock(&p->lock);
}

void soulx_frame_pump_stats(SoulXFramePump* p, unsigned* copied, unsigned* torn, unsigned* dropped) {
    if (copied) *copied = p ? atomic_load(&p->copied) : 0;
    if (torn) *torn = p ? atomic_load(&p->torn) : 0;
    if (dropped) *dropped = p ? atomic_load(&p->dropped) : 0;
}

// ---------------------------------------------------------------------------
// One complete frame for one-off readers (screenshots, scripts)
// ---------------------------------------------------------------------------

#define FRESH_IDLE_MS           40.0    // no frame published for this long: no readback is running
#define FRESH_POLL_US           250

static double fresh_unix_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// B,G,R,A -> R,G,B,A in place, 16 pixels per NEON step.
static void swizzle_in_place(uint8_t* px, size_t bytes) {
    size_t i = 0;
#if defined(__ARM_NEON)
    for (; i + 64 <= bytes; i += 64) {
        uint8x16x4_t v = vld4q_u8(px + i);
        uint8x16_t b = v.val[0];
        v.val[0] = v.val[2];
        v.val[2] = b;
        vst4q_u8(px + i, v);
    }
#endif
    for (; i + 4 <= bytes; i += 4) {
        uint8_t b = px[i];
        px[i] = px[i + 2];
        px[i + 2] = b;
    }
}

int soulx_frames_copy_fresh(SoulXFrames* f, uint8_t* dst, size_t dst_size, SoulXFrameView* view, bool rgba,
                           int timeout_ms) {
    if (view) memset(view, 0, sizeof(*view));
    if (!f || !dst) return -1;
    double deadline = pump_now_ms() + (timeout_ms > 0 ? timeout_ms : 0);
    FrameHeader h;
    read_header(f->base, &h);
    // the engine stamps each frame with its wall-clock time: a recent one means frames (and readbacks) keep coming
    bool still = fresh_unix_ms() - (double)h.ts_us / 1000.0 > FRESH_IDLE_MS;
    for (;;) {
        if (!still) {                           // wait for the next publish, then copy before the next readback
            FrameHeader now;
            do {
                read_header(f->base, &now);
                if (now.frame != h.frame) break;
                if (pump_now_ms() >= deadline) {
                    still = true;               // nothing published meanwhile: the screen came to rest
                    break;
                }
                usleep(FRESH_POLL_US);
            } while (true);
            h = now;
        }
        size_t total;
        if (!frame_bytes(&h, &total) || total > f->size) return -1;
        size_t bytes = total - FRAMES_HEADER_SIZE, stride = (size_t)h.width * 4u;
        if (bytes > dst_size) return -1;
        const uint8_t* live = f->base + FRAMES_HEADER_SIZE;
        memcpy(dst, live, bytes);
        atomic_thread_fence(memory_order_acquire);
        FrameHeader after;
        read_header(f->base, &after);
        if (after.frame == h.frame && after.width == h.width && after.height == h.height &&
            pump_copy_intact(dst, live, stride, (int)h.height)) {
            if (rgba) swizzle_in_place(dst, bytes);
            if (view) fill_view(view, &h, dst);
            return 0;
        }
        if (pump_now_ms() >= deadline) return -1;
        still = false;                          // a readback ran into the copy: take the frame it publishes
        h = after;
    }
}
