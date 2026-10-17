/*
 * SoulX Android Emulator - shared-memory frame reader (macOS window)
 *
 * The engine runs with -no-window -share-vid and publishes every frame in the POSIX shared-memory object
 * "videmulator<console port>": a 24-byte header { u32 width, height, fps, frameNumber; u64 tsUs } followed by
 * width * height * 4 bytes of B,G,R,A pixels. Frames are PANEL-NATIVE: they do not follow the guest's rotation
 * (the window rotates them for display). Measured: 60 frames/s while animating, visible ~0.1 ms after the engine
 * wrote them.
 */

#ifndef SOULX_FRAMES_H
#define SOULX_FRAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SoulXFrames SoulXFrames;

typedef struct {
    int width;                   // panel-native width in pixels
    int height;                  // panel-native height in pixels
    uint32_t frame_number;       // advances by one per engine frame
    uint64_t ts_us;              // engine timestamp of the frame (microseconds, CLOCK_REALTIME)
    const uint8_t* bgra;         // first pixel (B,G,R,A); points into shared memory, valid until the next
                                 // soulx_frames_* call on this reader
    size_t stride;               // bytes per row (width * 4)
} SoulXFrameView;

const char* soulx_frames_shm_name(int console_port, char* buf, size_t cap);   // "videmulator<port>"

SoulXFrames* soulx_frames_open(int console_port);   // read-only mapping; NULL when the region does not exist (yet),
                                                  // is too small for its header, or its size does not match it
int  soulx_frames_peek(SoulXFrames* f, SoulXFrameView* view);
                                                  // current header + pixel pointer: 1 = frame_number changed since
                                                  // the previous successful peek, 0 = unchanged, -1 = region invalid
                                                  // (resized/unlinked: close and reopen)
int  soulx_frames_copy(SoulXFrames* f, uint8_t* dst, size_t dst_size, SoulXFrameView* view);
                                                  // copy of the current frame into dst; frame_number is read before and
                                                  // after (up to 3 attempts), which catches a publish during the copy
                                                  // but NOT a readback still writing (see soulx_frames_copy_fresh);
                                                  // view describes dst; 0 ok, -1 error / buffer too small
int  soulx_frames_copy_rgba(SoulXFrames* f, uint8_t* dst, size_t dst_size, SoulXFrameView* view);
                                                  // the same, converting to R,G,B,A on the way (for SoulXImage)
int  soulx_frames_copy_fresh(SoulXFrames* f, uint8_t* dst, size_t dst_size, SoulXFrameView* view, bool rgba,
                            int timeout_ms);
                                                  // a COMPLETE frame: the engine reads frames back straight into the
                                                  // region and only then bumps frame_number, so a copy taken at any
                                                  // time can mix two frames. While frames keep coming this waits for
                                                  // the next publish and copies right after it; on a still screen it
                                                  // copies at once; either way it checks the copy against the region.
                                                  // rgba: R,G,B,A instead of B,G,R,A. 0 ok, -1 error / timeout
bool soulx_frames_valid(SoulXFrames* f);            // the mapped object is still the engine's current region
void soulx_frames_close(SoulXFrames* f);

// Frame pump: a thread that copies each new frame right after the engine published it and keeps only copies
// no readback wrote into (the engine reads the next frame back straight into the region, so a copy taken at
// any other time can mix two frames). Opens, validates and reopens the region by itself.
typedef struct SoulXFramePump SoulXFramePump;

SoulXFramePump* soulx_frame_pump_start(int console_port);   // NULL when the thread cannot start
void soulx_frame_pump_stop(SoulXFramePump* p);              // joins the thread
bool soulx_frame_pump_live(SoulXFramePump* p);              // the engine's region is mapped
void soulx_frame_pump_lock(SoulXFramePump* p, SoulXFrameView* view);
                                                  // the newest complete frame (B,G,R,A; view->bgra NULL when none
                                                  // yet), unchanged until soulx_frame_pump_unlock: keep it short
void soulx_frame_pump_unlock(SoulXFramePump* p);
void soulx_frame_pump_stats(SoulXFramePump* p, unsigned* copied, unsigned* torn, unsigned* dropped);

#endif // SOULX_FRAMES_H
