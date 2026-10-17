/*
 * SoulX Android Emulator - the Android screen of SoulX's window (MTKView): frames, rotation, input
 *
 * Frames: the engine publishes panel-native BGRA frames in shared memory, reading each one back straight into the
 * region. A frame pump thread (frames.c) copies every new frame right after it was published and keeps only
 * complete ones; on a display refresh the newest of them is memcpy'd into the next of three shared MTLBuffers
 * that back BGRA8Unorm linear textures (measured for 1080x2400 on an M2 Pro: 0.17 ms, against 0.55 ms for
 * replaceRegion into a tiled texture). The main thread never waits for the GPU: with every buffer still in
 * flight it simply tries again at the next refresh. One quad is drawn into the aspect-fit rect (black letterbox). Frames do not follow the guest
 * rotation R, so the quad's texture coordinates are rotated: logical (u, v) shows panel (s, t) =
 *     R=0: (u, v)    R=1: (1-v, u)    R=2: (1-u, 1-v)    R=3: (v, 1-u)
 * which is input.c's logical -> natural mapping, normalised. Nothing is drawn while neither the frame nor the
 * view changed. Without shared memory the view can show console screenshots instead (slow mode, ~8 fps).
 *
 * Input: every SoulXInput / SoulXHidKbd / keymap-runtime call runs on the serial queue com.soulx.input, never on
 * the main thread:
 *   - left mouse = finger 0 (moves coalesced: the latest position, at most one per 8 ms); right mouse = BACK
 *   - mouse wheel = one swipe on finger 9 per >= 60 ms (about 120 logical px per notch, 50 ms); trackpad
 *     gestures drag finger 9 directly; momentum events are ignored (Android flings by itself)
 *   - keys: a keymap binding turns into touches on fingers 1..8; any other key goes to the guest as a real key
 *     down/up through the hidkbd virtual keyboard, or, while that is unavailable, through soulx_input_text /
 *     soulx_input_key. Esc = BACK unless the keymap binds it. Command shortcuts belong to the menus.
 * The guest rotation is re-read on the input queue every second (not while a finger is down: a gesture keeps
 * its mapping) and published to the renderer atomically.
 */

#import "window_internal.h"

#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "image.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define SCREEN_RING                 3           // frame buffers / textures in flight
#define SCREEN_MAX_DIM              16384
#define SCREEN_STATS_MAX            8192        // render-time samples kept
#define SCREEN_SLOW_PERIOD_MS       125.0       // slow mode: one console screenshot per 125 ms (~8 fps)
#define INPUT_MOVE_MS               8.0         // at most one coalesced move per 8 ms
#define INPUT_ROTATION_MS           1000        // guest rotation refresh (adb on the display-poll queue)
#define INPUT_SIZES_MS              30000.0     // ... and display size (`wm size`) refresh
#define INPUT_REOPEN_MS             2000.0      // retry soulx_input_open
#define INPUT_FRAME_MS              16          // swipe frame interval
#define WHEEL_SWIPE_GAP_MS          60.0        // at most one wheel swipe per 60 ms
#define WHEEL_SWIPE_MS              50
#define WHEEL_PX_PER_NOTCH          120.0
#define WHEEL_SLOP_PX               12.0        // trackpad: movement before finger 9 goes down (no taps)
#define HIDKBD_RETRY_MS             10000.0     // after a failed soulx_hidkbd_open
#define HID_MAX_KEYS                6
#define KEYMAP_POLL_MS              2000
#define KEYMAP_ACTIONS_MAX          16
#define SIM_TAP_HOLD_MS             80
#define SIM_KEY_HOLD_MS             80

// macOS virtual key codes (Carbon's kVK_* values; Carbon itself is not needed)
enum {
    VK_RETURN = 0x24, VK_TAB = 0x30, VK_DELETE = 0x33, VK_ESCAPE = 0x35, VK_SHIFT = 0x38, VK_OPTION = 0x3A,
    VK_CONTROL = 0x3B, VK_RSHIFT = 0x3C, VK_ROPTION = 0x3D, VK_RCONTROL = 0x3E, VK_KEYPAD_ENTER = 0x4C,
    VK_HOME = 0x73, VK_PAGE_UP = 0x74, VK_FORWARD_DELETE = 0x75, VK_END = 0x77, VK_PAGE_DOWN = 0x79,
    VK_LEFT = 0x7B, VK_RIGHT = 0x7C, VK_DOWN = 0x7D, VK_UP = 0x7E,
};

// Device-dependent modifier bits of NSEvent.modifierFlags (IOKit's NX_DEVICE*KEYMASK): left / right sides
#define DEV_LCTRL   0x00000001u
#define DEV_LSHIFT  0x00000002u
#define DEV_RSHIFT  0x00000004u
#define DEV_LALT    0x00000020u
#define DEV_RALT    0x00000040u
#define DEV_RCTRL   0x00002000u

// Keys outside keymap.h's key set: HID usage (page 0x07) and Android key name for the fallback path
typedef struct {
    unsigned short vk;
    int usage;
    const char* android;
} ExtraKey;

static const ExtraKey k_extra_keys[] = {
    { VK_FORWARD_DELETE, 0x4C, "FORWARD_DEL" },
    { VK_HOME,           0x4A, "MOVE_HOME" },
    { VK_END,            0x4D, "MOVE_END" },
    { VK_PAGE_UP,        0x4B, "PAGE_UP" },
    { VK_PAGE_DOWN,      0x4E, "PAGE_DOWN" },
    { VK_KEYPAD_ENTER,   0x58, "ENTER" },
};

// Fallback (no hidkbd): non-printing keys -> Android key names for soulx_input_key
static const struct {
    unsigned short vk;
    const char* android;
} k_fallback_keys[] = {
    { VK_RETURN, "ENTER" }, { VK_TAB, "TAB" }, { VK_DELETE, "DEL" },
    { VK_LEFT, "DPAD_LEFT" }, { VK_RIGHT, "DPAD_RIGHT" }, { VK_UP, "DPAD_UP" }, { VK_DOWN, "DPAD_DOWN" },
};

// Metal: one textured quad; the fragment ignores alpha (the engine's alpha byte is not meaningful)
static const char k_shader_source[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct SoulXVertex { float2 pos; float2 uv; };\n"
    "struct SoulXVOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex SoulXVOut soulx_vertex(uint vid [[vertex_id]], constant SoulXVertex* v [[buffer(0)]]) {\n"
    "    SoulXVOut o;\n"
    "    o.pos = float4(v[vid].pos, 0.0, 1.0);\n"
    "    o.uv = v[vid].uv;\n"
    "    return o;\n"
    "}\n"
    "fragment float4 soulx_fragment(SoulXVOut in [[stage_in]], texture2d<float> tex [[texture(0)]],\n"
    "                              sampler smp [[sampler(0)]]) {\n"
    "    return float4(tex.sample(smp, in.uv).rgb, 1.0);\n"
    "}\n";

typedef struct {
    float pos[2];
    float uv[2];
} QuadVertex;

// Latest-position mailbox between the main thread (writer) and the input queue (sender)
typedef struct {
    os_unfair_lock lock;
    bool pending;                // x, y not sent yet
    bool scheduled;              // a send block is queued
    int  x, y;
    double last_ms;              // when the previous move was sent
} MoveSlot;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static dispatch_time_t after_ms(double ms) {
    return dispatch_time(DISPATCH_TIME_NOW, ms > 0 ? (int64_t)(ms * (double)NSEC_PER_MSEC) : 0);
}

// Texture coordinates of logical (u, v) for guest rotation r (see the header comment).
static void rotate_uv(int r, float u, float v, float out[2]) {
    switch (r) {
    case 1:  out[0] = 1.0f - v; out[1] = u;        break;
    case 2:  out[0] = 1.0f - u; out[1] = 1.0f - v; break;
    case 3:  out[0] = v;        out[1] = 1.0f - u; break;
    default: out[0] = u;        out[1] = v;        break;
    }
}

// Aspect-fit rect of a lw x lh image in a w x h area (y down when the caller's y is down).
static CGRect fit_rect(double w, double h, double lw, double lh) {
    if (w <= 0 || h <= 0 || lw <= 0 || lh <= 0) return CGRectZero;
    double scale = fmin(w / lw, h / lh);
    double fw = lw * scale, fh = lh * scale;
    return CGRectMake((w - fw) / 2.0, (h - fh) / 2.0, fw, fh);
}

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// BGRA (or RGBA with swap = false) pixels -> tightly packed opaque RGBA.
static void to_opaque_rgba(const uint8_t* src, size_t src_stride, int w, int h, bool swap, uint8_t* dst) {
    for (int y = 0; y < h; y++) {
        const uint8_t* s = src + (size_t)y * src_stride;
        uint8_t* d = dst + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; x++, s += 4, d += 4) {
            d[0] = swap ? s[2] : s[0];
            d[1] = s[1];
            d[2] = swap ? s[0] : s[2];
            d[3] = 255;
        }
    }
}

// A keymap file exists in $SOULX_HOME/keymaps (otherwise the foreground app is not polled).
static bool keymaps_present(void) {
    char dir[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "keymaps") != 0) return false;
    DIR* d = opendir(dir);
    if (!d) return false;
    bool found = false;
    struct dirent* e;
    while (!found && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        found = n > 7 && e->d_name[0] != '.' && strcmp(e->d_name + n - 7, ".keymap") == 0;
    }
    closedir(d);
    return found;
}

static long long file_mtime_ns(const char* rel_name) {
    char path[PATH_MAX];
    struct stat sb;
    if (soulx_path(path, sizeof(path), "keymaps/%s.keymap", rel_name) != 0 || stat(path, &sb) != 0) return 0;
    return (long long)sb.st_mtimespec.tv_sec * 1000000000LL + sb.st_mtimespec.tv_nsec + (long long)sb.st_size;
}

// ---------------------------------------------------------------------------
// SoulXScreenView
// ---------------------------------------------------------------------------

@implementation SoulXScreenView {
    // --- set once ---
    SoulXInstanceConf _conf;
    int _port;
    char _serial[64];
    dispatch_queue_t _inputQueue;
    dispatch_queue_t _hidQueue;
    dispatch_queue_t _keymapQueue;
    dispatch_queue_t _slowQueue;
    SoulXKeymapOverlay* _overlay;

    // --- main thread: frames and Metal ---
    SoulXFramePump* _pump;        // complete frames from the engine's shared memory (its own thread)
    id<MTLCommandQueue> _commands;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLSamplerState> _sampler;
    dispatch_semaphore_t _inflight;
    id<MTLBuffer> _buffers[SCREEN_RING];
    id<MTLTexture> _textures[SCREEN_RING];
    int _ring;                   // texture holding the newest frame (-1 = none)
    int _texW, _texH;            // size of the textures
    MTLPixelFormat _texFormat;
    NSUInteger _rowBytes;
    bool _texUpright;            // slow-mode image: already upright
    bool _forceUpload;           // the last frame could not be uploaded yet
    bool _needsRedraw;
    bool _haveFrameNumber;
    uint32_t _lastFrameNumber;
    int _drawRotation;
    _Atomic int _publishedRotation;
    BOOL _hasFrame;
    BOOL _shutdown;

    // --- main thread: slow mode ---
    BOOL _slowMode;
    SoulXImage _slowImage;        // newest console screenshot (upright RGBA)
    bool _slowImageNew;

    // --- main thread: test hooks and statistics ---
    NSString* _dumpPath;
    void (^_dumpDone)(BOOL ok);
    double _statsStart;
    unsigned _statsPresented, _statsUploads;
    unsigned _statsPumpDropped, _statsPumpTorn;     // the pump's counters at resetStats
    double _renderMs[SCREEN_STATS_MAX];
    unsigned _renderCount;
    unsigned _fpsCount;
    NSTimer* _fpsTimer;

    // --- main thread: pointer ---
    BOOL _mouseDown;
    BOOL _wheelDragging;         // trackpad gesture holds finger 9
    BOOL _wheelGesture;          // a trackpad gesture is in progress
    double _wheelStartX, _wheelStartY, _wheelDX, _wheelDY;   // logical px
    double _wheelAccum;          // mouse wheel: logical px not sent yet
    NSPoint _wheelPoint;         // logical point of the pointer
    bool _wheelFlushQueued;
    double _wheelLastMs;
    MoveSlot _mouseSlot;
    MoveSlot _wheelSlot;

    // --- input queue only ---
    SoulXInput* _in;
    double _inRetryAt;
    BOOL _rotating;              // a rotate command is in flight (input queue): debounce the button
    _Atomic int _pendingMusicVol; // newest volume the slider wants (-1 = none); coalesces fast drags
    unsigned _fingers;           // bit per finger we hold down
    unsigned _swipeGen[SOULX_MAX_FINGERS];
    SoulXHidKbd* _kbd;
    bool _kbdOpening;
    double _kbdRetryAt;
    NSMutableArray* _kbdWaiters;
    unsigned _sideMods;          // DEV_* bits currently down (not bound by the keymap)
    int _hidUsages[HID_MAX_KEYS];
    int _hidCount;
    SoulXKeymapRuntime* _km;
    dispatch_source_t _rotationTimer;   // fires on the display-poll queue

    // --- display-poll queue only ---
    dispatch_queue_t _pollQueue;        // adb calls for rotation / size: never on the input queue, where they
                                        // would hold back touches queued behind them
    double _sizesDueAt;

    // --- keymap queue only ---
    dispatch_source_t _keymapTimer;
    char _kmPackage[256];
    long long _kmSignature;
    bool _kmLoaded;
}

@synthesize overlay = _overlay;

- (instancetype)initWithConf:(const SoulXInstanceConf*)conf state:(const SoulXRunState*)st {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        soulx_log(SOULX_LOG_ERROR, "no Metal device is available");
        return nil;
    }
    self = [super initWithFrame:NSMakeRect(0, 0, 360, 800) device:device];
    if (!self) return nil;

    _conf = *conf;
    _port = st->port;
    _pump = soulx_frame_pump_start(_port);
    soulx_serial(st->port, _serial, sizeof(_serial));
    _ring = -1;
    _drawRotation = 0;
    atomic_init(&_publishedRotation, 0);
    atomic_init(&_pendingMusicVol, -1);
    _mouseSlot.lock = OS_UNFAIR_LOCK_INIT;
    _wheelSlot.lock = OS_UNFAIR_LOCK_INIT;
    _inputEnabled = YES;
    _kbdWaiters = [NSMutableArray array];

    dispatch_queue_attr_t interactive =
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    dispatch_queue_attr_t utility = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0);
    _inputQueue = dispatch_queue_create("com.soulx.input", interactive);
    _hidQueue = dispatch_queue_create("com.soulx.hidkbd", utility);
    _keymapQueue = dispatch_queue_create("com.soulx.keymap", utility);
    _slowQueue = dispatch_queue_create("com.soulx.slow-display", utility);
    _pollQueue = dispatch_queue_create("com.soulx.display-poll", utility);

    self.delegate = self;
    self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    self.framebufferOnly = NO;                  // --dump-frame reads the drawable back
    self.preferredFramesPerSecond = 60;
    self.clearColor = MTLClearColorMake(0, 0, 0, 1);
    self.enableSetNeedsDisplay = NO;
    self.paused = NO;
    if (![self setUpMetal:device]) return nil;

    _overlay = [[SoulXKeymapOverlay alloc] initWithScreen:self];
    _overlay.frame = self.bounds;
    _overlay.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [self addSubview:_overlay];

    [self resetStats];
    __weak SoulXScreenView* weakSelf = self;
    _fpsTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer* t) {
        (void)t;
        SoulXScreenView* s = weakSelf;
        if (!s) return;
        double fps = s->_fpsCount;
        s->_fpsCount = 0;
        [s.owner screenView:s fps:fps];
    }];
    [self startInput];
    [self startKeymapWatch];
    return self;
}

- (void)dealloc {
    soulx_frame_pump_stop(_pump);
    soulx_image_free(&_slowImage);
}

- (BOOL)setUpMetal:(id<MTLDevice>)device {
    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:@(k_shader_source) options:nil error:&err];
    if (!lib) {
        soulx_log(SOULX_LOG_ERROR, "cannot compile the screen shader: %s", err.localizedDescription.UTF8String);
        return NO;
    }
    MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [lib newFunctionWithName:@"soulx_vertex"];
    pd.fragmentFunction = [lib newFunctionWithName:@"soulx_fragment"];
    pd.colorAttachments[0].pixelFormat = self.colorPixelFormat;
    _pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!_pipeline) {
        soulx_log(SOULX_LOG_ERROR, "cannot create the screen pipeline: %s", err.localizedDescription.UTF8String);
        return NO;
    }
    MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    _sampler = [device newSamplerStateWithDescriptor:sd];
    _commands = [device newCommandQueue];
    _inflight = dispatch_semaphore_create(SCREEN_RING);
    return _sampler != nil && _commands != nil;
}

- (BOOL)isOpaque {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

- (int)rotation {
    return _drawRotation;
}

- (BOOL)hasFrame {
    return _hasFrame;
}

- (BOOL)slowMode {
    return _slowMode;
}

- (BOOL)keymapEditing {
    return _overlay.editing;
}

- (BOOL)showBindings {
    return _overlay.showBindings;
}

- (void)setShowBindings:(BOOL)show {
    _overlay.showBindings = show;
}

- (NSSize)logicalSize {
    if (_ring >= 0 && _texUpright) return NSMakeSize(_texW, _texH);
    int w = _ring >= 0 ? _texW : _conf.width;
    int h = _ring >= 0 ? _texH : _conf.height;
    return (_drawRotation & 1) ? NSMakeSize(h, w) : NSMakeSize(w, h);
}

- (NSRect)imageRect {
    NSSize ls = self.logicalSize;
    NSRect b = self.bounds;
    CGRect r = fit_rect(b.size.width, b.size.height, ls.width, ls.height);
    return NSMakeRect(b.origin.x + r.origin.x, b.origin.y + r.origin.y, r.size.width, r.size.height);
}

// View point -> logical pixel. Outside the image: NO unless clamp.
- (BOOL)logicalFromPoint:(NSPoint)p clamp:(BOOL)clamp x:(int*)x y:(int*)y {
    NSSize ls = self.logicalSize;
    NSRect r = [self imageRect];
    if (r.size.width <= 0 || r.size.height <= 0 || ls.width < 1 || ls.height < 1) return NO;
    double u = (p.x - r.origin.x) / r.size.width;
    double v = 1.0 - (p.y - r.origin.y) / r.size.height;      // the view's y axis points up
    if (!clamp && (u < 0 || u >= 1 || v < 0 || v >= 1)) return NO;
    int lw = (int)ls.width, lh = (int)ls.height;
    *x = clampi((int)floor(clampd(u, 0, 1) * lw), 0, lw - 1);
    *y = clampi((int)floor(clampd(v, 0, 1) * lh), 0, lh - 1);
    return YES;
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    _needsRedraw = true;
    [_overlay relayout];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    _needsRedraw = true;
    [_overlay relayout];
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
    (void)view;
    (void)size;
    _needsRedraw = true;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

- (BOOL)ensureTexturesWidth:(int)w height:(int)h format:(MTLPixelFormat)fmt {
    if (_ring >= 0 && w == _texW && h == _texH && fmt == _texFormat) return YES;
    id<MTLDevice> dev = self.device;
    NSUInteger align = [dev minimumLinearTextureAlignmentForPixelFormat:fmt];
    if (align == 0) align = 16;
    NSUInteger row = ((NSUInteger)w * 4 + align - 1) / align * align;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                                 width:(NSUInteger)w
                                                                                height:(NSUInteger)h
                                                                             mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead;
    for (int i = 0; i < SCREEN_RING; i++) {
        // in-flight command buffers keep the old buffers alive until they complete
        _buffers[i] = [dev newBufferWithLength:row * (NSUInteger)h options:MTLResourceStorageModeShared];
        _textures[i] = _buffers[i] ? [_buffers[i] newTextureWithDescriptor:td offset:0 bytesPerRow:row] : nil;
        if (!_textures[i]) {
            soulx_log(SOULX_LOG_ERROR, "cannot allocate a %dx%d screen texture", w, h);
            for (int k = 0; k < SCREEN_RING; k++) {
                _buffers[k] = nil;
                _textures[k] = nil;
            }
            _ring = -1;
            return NO;
        }
    }
    _texW = w;
    _texH = h;
    _texFormat = fmt;
    _rowBytes = row;
    _ring = SCREEN_RING - 1;
    return YES;
}

// Copies pixels into the next ring slot. The caller holds an in-flight slot (semaphore).
- (BOOL)uploadPixels:(const uint8_t*)src stride:(size_t)stride width:(int)w height:(int)h
              format:(MTLPixelFormat)fmt upright:(bool)upright {
    if (w <= 0 || h <= 0 || w > SCREEN_MAX_DIM || h > SCREEN_MAX_DIM || stride < (size_t)w * 4) return NO;
    bool geometry = _ring < 0 || w != _texW || h != _texH || upright != _texUpright;
    if (![self ensureTexturesWidth:w height:h format:fmt]) return NO;
    int next = (_ring + 1) % SCREEN_RING;
    uint8_t* dst = _buffers[next].contents;
    if (stride == _rowBytes) {
        memcpy(dst, src, stride * (size_t)h);
    } else {
        for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * _rowBytes, src + (size_t)y * stride, (size_t)w * 4);
    }
    _ring = next;
    _texUpright = upright;
    if (geometry) {
        __weak SoulXScreenView* weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            SoulXScreenView* s = weakSelf;
            if (!s) return;
            [s->_overlay relayout];
            [s.owner screenViewGeometryDidChange:s];
        });
    }
    return YES;
}

// Uploads a new shared-memory frame or slow-mode image. 1 = uploaded, 0 = nothing new, -1 = no free buffer.
- (int)uploadIfNew {
    if (soulx_frame_pump_live(_pump)) {
        SoulXFrameView fv;
        soulx_frame_pump_lock(_pump, &fv);               // the pump cannot swap it while we copy
        bool fresh = fv.bgra && (_forceUpload || _ring < 0 || _texUpright || !_haveFrameNumber ||
                                 fv.frame_number != _lastFrameNumber);
        if (!fresh) {
            soulx_frame_pump_unlock(_pump);
            return 0;
        }
        if (dispatch_semaphore_wait(_inflight, DISPATCH_TIME_NOW)) {    // every buffer in flight: next refresh
            soulx_frame_pump_unlock(_pump);
            _forceUpload = true;
            return -1;
        }
        BOOL ok = [self uploadPixels:fv.bgra stride:fv.stride width:fv.width height:fv.height
                              format:MTLPixelFormatBGRA8Unorm upright:false];
        soulx_frame_pump_unlock(_pump);
        if (!ok) {
            dispatch_semaphore_signal(_inflight);
            return 0;
        }
        _lastFrameNumber = fv.frame_number;
        _haveFrameNumber = true;
        _forceUpload = false;
        if (_slowMode) [self stopSlowMode];
        return 1;
    }
    if (_slowMode && _slowImageNew && _slowImage.rgba) {
        if (dispatch_semaphore_wait(_inflight, DISPATCH_TIME_NOW)) {
            return -1;
        }
        _slowImageNew = false;
        if (![self uploadPixels:_slowImage.rgba stride:(size_t)_slowImage.w * 4 width:_slowImage.w
                         height:_slowImage.h format:MTLPixelFormatRGBA8Unorm upright:true]) {
            dispatch_semaphore_signal(_inflight);
            return 0;
        }
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Rendering (main thread, display refresh)
// ---------------------------------------------------------------------------

- (void)drawInMTKView:(MTKView*)view {
    (void)view;
    double t0 = soulx_now_ms();

    int r = atomic_load(&_publishedRotation);
    if (r != _drawRotation) {
        _drawRotation = r;
        _needsRedraw = true;
        [_overlay relayout];
        [self.owner screenViewGeometryDidChange:self];
    }

    int up = [self uploadIfNew];
    if (up < 0) return;                         // GPU busy: try again at the next refresh
    bool uploaded = up > 0;
    if (!uploaded && !_needsRedraw && !_dumpPath) return;
    if (!uploaded) {
        if (dispatch_semaphore_wait(_inflight, DISPATCH_TIME_NOW)) return;   // _needsRedraw stays: next refresh
    }

    MTLRenderPassDescriptor* rp = self.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = self.currentDrawable;
    if (!rp || !drawable) {
        dispatch_semaphore_signal(_inflight);
        return;
    }
    _needsRedraw = false;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

    id<MTLCommandBuffer> cb = [_commands commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (_ring >= 0) {
        CGSize ds = self.drawableSize;
        NSSize ls = self.logicalSize;
        CGRect fr = fit_rect(ds.width, ds.height, ls.width, ls.height);
        double x0 = round(fr.origin.x), y0 = round(fr.origin.y);
        double x1 = round(fr.origin.x + fr.size.width), y1 = round(fr.origin.y + fr.size.height);
        // y0 / y1 measured from the top of the drawable
        float l = (float)(2.0 * x0 / ds.width - 1.0), rgt = (float)(2.0 * x1 / ds.width - 1.0);
        float top = (float)(1.0 - 2.0 * y0 / ds.height), bot = (float)(1.0 - 2.0 * y1 / ds.height);
        int rot = _texUpright ? 0 : _drawRotation;
        QuadVertex q[4] = {
            { { l, top }, { 0, 0 } }, { { rgt, top }, { 0, 0 } }, { { l, bot }, { 0, 0 } }, { { rgt, bot }, { 0, 0 } },
        };
        rotate_uv(rot, 0, 0, q[0].uv);
        rotate_uv(rot, 1, 0, q[1].uv);
        rotate_uv(rot, 0, 1, q[2].uv);
        rotate_uv(rot, 1, 1, q[3].uv);
        [enc setRenderPipelineState:_pipeline];
        [enc setVertexBytes:q length:sizeof(q) atIndex:0];
        [enc setFragmentTexture:_textures[_ring] atIndex:0];
        [enc setFragmentSamplerState:_sampler atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [enc endEncoding];

    if (_dumpPath) [self encodeDump:cb texture:drawable.texture];

    dispatch_semaphore_t inflight = _inflight;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        dispatch_semaphore_signal(inflight);
    }];
    [cb presentDrawable:drawable];
    [cb commit];

    double dt = soulx_now_ms() - t0;
    _statsPresented++;
    _fpsCount++;
    if (uploaded) _statsUploads++;
    _renderMs[_renderCount % SCREEN_STATS_MAX] = dt;
    _renderCount++;
    if (uploaded && !_hasFrame) {
        _hasFrame = YES;
        [self.owner screenViewDidShowFirstFrame:self];
    }
}

// --dump-frame: copies the drawable (exactly what is shown) into a shared buffer and writes a PNG.
- (void)encodeDump:(id<MTLCommandBuffer>)cb texture:(id<MTLTexture>)tex {
    NSString* path = _dumpPath;
    void (^done)(BOOL) = _dumpDone;
    _dumpPath = nil;
    _dumpDone = nil;
    NSUInteger w = tex.width, h = tex.height;
    id<MTLBuffer> buf = [self.device newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
    if (!buf) {
        dispatch_async(dispatch_get_main_queue(), ^{ done(NO); });
        return;
    }
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0
      destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
    [blit endEncoding];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        BOOL gpu_ok = finished.status == MTLCommandBufferStatusCompleted;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            BOOL ok = NO;
            SoulXImage img = { (int)w, (int)h, gpu_ok ? malloc(w * h * 4) : NULL };
            if (img.rgba) {
                to_opaque_rgba(buf.contents, w * 4, (int)w, (int)h, true, img.rgba);
                ok = soulx_image_save_png(path.fileSystemRepresentation, &img) == 0;
                if (!ok) soulx_log(SOULX_LOG_ERROR, "cannot write %s: %s", path.UTF8String, soulx_image_error());
            }
            free(img.rgba);
            dispatch_async(dispatch_get_main_queue(), ^{ done(ok); });
        });
    }];
}

- (void)dumpFrameToFile:(NSString*)path completion:(void (^)(BOOL ok))done {
    if (_dumpPath) {
        done(NO);
        return;
    }
    _dumpPath = [path copy];
    _dumpDone = [done copy];
    _needsRedraw = true;
}

- (void)resetStats {
    _statsStart = soulx_now_ms();
    _statsPresented = 0;
    _statsUploads = 0;
    soulx_frame_pump_stats(_pump, NULL, &_statsPumpTorn, &_statsPumpDropped);
    _renderCount = 0;
}

- (NSString*)statsLine {
    double secs = (soulx_now_ms() - _statsStart) / 1000.0;
    unsigned n = _renderCount < SCREEN_STATS_MAX ? _renderCount : SCREEN_STATS_MAX;
    double p50 = 0, p95 = 0;
    if (n > 0) {
        double* s = malloc(sizeof(double) * n);
        if (s) {
            memcpy(s, _renderMs, sizeof(double) * n);
            qsort(s, n, sizeof(double), cmp_double);
            p50 = s[n / 2];
            p95 = s[(size_t)((n - 1) * 0.95)];
            free(s);
        }
    }
    unsigned torn = 0, dropped = 0;
    soulx_frame_pump_stats(_pump, NULL, &torn, &dropped);
    return [NSString stringWithFormat:@"fps=%.1f uploads=%u dropped=%u torn=%u render_ms_p50=%.3f render_ms_p95=%.3f",
                                      secs > 0 ? _statsPresented / secs : 0.0, _statsUploads,
                                      dropped - _statsPumpDropped, torn - _statsPumpTorn, p50, p95];
}

// ---------------------------------------------------------------------------
// Slow mode fallback: console screenshots, already upright
// ---------------------------------------------------------------------------

- (void)startSlowMode {
    if (_slowMode || _shutdown) return;
    _slowMode = YES;
    soulx_log(SOULX_LOG_WARN, "no shared-memory frames from the engine of '%s'; using slow display mode", _conf.name);
    NSString* name = @(_conf.name);             // blocks cannot capture a C array
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_slowQueue, ^{
        SoulXInput* cap = soulx_input_open(name.UTF8String);   // its own console connection: never blocks input
        [weakSelf slowLoop:cap];
    });
}

- (void)stopSlowMode {
    _slowMode = NO;                             // the loop sees it and closes its connection
    soulx_image_free(&_slowImage);
    _slowImageNew = false;
}

// slow queue
- (void)slowLoop:(SoulXInput*)cap {
    __block BOOL running = YES;
    dispatch_sync(dispatch_get_main_queue(), ^{ running = self->_slowMode && !self->_shutdown; });
    if (!running) {
        soulx_input_close(cap);
        return;
    }
    double t0 = soulx_now_ms();
    SoulXImage img = { 0, 0, NULL };
    if (!cap) cap = soulx_input_open(_conf.name);
    if (cap && soulx_input_capture(cap, &img) == 0) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!self->_slowMode) {
                SoulXImage drop = img;
                soulx_image_free(&drop);
                return;
            }
            soulx_image_free(&self->_slowImage);
            self->_slowImage = img;
            self->_slowImageNew = true;
        });
    }
    double wait = SCREEN_SLOW_PERIOD_MS - (soulx_now_ms() - t0);
    __weak SoulXScreenView* weakSelf = self;
    dispatch_after(after_ms(wait), _slowQueue, ^{
        SoulXScreenView* s = weakSelf;
        if (s) [s slowLoop:cap];
        else soulx_input_close(cap);
    });
}

// ---------------------------------------------------------------------------
// Screenshot source
// ---------------------------------------------------------------------------

- (void)captureUpright:(void (^)(SoulXImage* img))done {
    if (soulx_frame_pump_live(_pump)) {
        SoulXFrameView fv;
        soulx_frame_pump_lock(_pump, &fv);
        uint8_t* bgra = NULL;
        SoulXFrameView copy = fv;
        if (fv.bgra && fv.width > 0 && fv.height > 0 && fv.width <= SCREEN_MAX_DIM && fv.height <= SCREEN_MAX_DIM) {
            size_t size = (size_t)fv.height * fv.stride;
            if ((bgra = malloc(size)) != NULL) memcpy(bgra, fv.bgra, size);
        }
        soulx_frame_pump_unlock(_pump);
        if (bgra) {
            int rot = _drawRotation;
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                SoulXImage fb = { copy.width, copy.height, malloc((size_t)copy.width * (size_t)copy.height * 4) };
                SoulXImage* out = NULL;
                if (fb.rgba) {
                    to_opaque_rgba(bgra, copy.stride, copy.width, copy.height, true, fb.rgba);
                    out = calloc(1, sizeof(SoulXImage));
                    if (out && soulx_image_rotate_to_logical(&fb, rot, out) != 0) {
                        free(out);
                        out = NULL;
                    }
                }
                free(fb.rgba);
                free(bgra);
                dispatch_async(dispatch_get_main_queue(), ^{ done(out); });
            });
            return;
        }
    }
    if (_slowMode && _slowImage.rgba) {
        SoulXImage* out = calloc(1, sizeof(SoulXImage));
        size_t size = (size_t)_slowImage.w * (size_t)_slowImage.h * 4;
        if (out && (out->rgba = malloc(size)) != NULL) {
            memcpy(out->rgba, _slowImage.rgba, size);
            out->w = _slowImage.w;
            out->h = _slowImage.h;
            done(out);
            return;
        }
        free(out);
    }
    done(NULL);
}

// ---------------------------------------------------------------------------
// Input queue: connection, rotation, touches
// ---------------------------------------------------------------------------

- (void)startInput {
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_inputQueue, ^{
        [weakSelf inputOpen];
    });
    _rotationTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _pollQueue);
    dispatch_source_set_timer(_rotationTimer, after_ms(INPUT_ROTATION_MS), INPUT_ROTATION_MS * NSEC_PER_MSEC,
                              50 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(_rotationTimer, ^{
        [weakSelf displayPollTick];
    });
    dispatch_resume(_rotationTimer);
}

// input queue
- (BOOL)inputOpen {
    if (_in) return YES;
    if (_shutdown || soulx_now_ms() < _inRetryAt) return NO;
    _in = soulx_input_open(_conf.name);
    if (!_in) {
        _inRetryAt = soulx_now_ms() + INPUT_REOPEN_MS;
        return NO;
    }
    soulx_input_set_display_pushed(_in, true);           // the display-poll queue keeps rotation / size current
    [self publishRotation];
    return YES;
}

// input queue
- (void)publishRotation {
    int r = soulx_input_rotation(_in);
    if (r >= 0 && r <= 3) atomic_store(&_publishedRotation, r);
}

// display-poll queue, every second: one adb call here, so the input queue never waits for it (touches
// queued behind a 30-40 ms adb round trip stalled drags); the display size only every INPUT_SIZES_MS
- (void)displayPollTick {
    if (_shutdown) return;
    double now = soulx_now_ms();
    bool sizes = now >= _sizesDueAt;
    if (sizes) _sizesDueAt = now + INPUT_SIZES_MS;
    char serial[32];
    soulx_serial(_port, serial, sizeof(serial));
    SoulXDisplayInfo d;
    (void)soulx_input_query_display(serial, sizes, &d);
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_inputQueue, ^{
        [weakSelf applyDisplay:d];
    });
}

// input queue
- (void)applyDisplay:(SoulXDisplayInfo)d {
    if (_shutdown) return;
    if (![self inputOpen]) return;
    if (_fingers == 0) soulx_input_apply_display(_in, &d);   // a finger that is down keeps its mapping
    [self publishRotation];
    if (_km) {                                            // keep the joystick radius circular after a rotation
        int lw = 0, lh = 0;
        [self inputLogicalW:&lw h:&lh];
        soulx_keymap_runtime_set_screen(_km, lw, lh);
    }
}

// input queue: one touch, keeping track of the fingers we hold down
- (void)touch:(int)finger action:(int)action x:(int)x y:(int)y {
    if (!_in || finger < 0 || finger >= SOULX_MAX_FINGERS) return;
    unsigned bit = 1u << finger;
    if (action == SOULX_TOUCH_MOVE && !(_fingers & bit)) return;
    if (action == SOULX_TOUCH_UP && !(_fingers & bit)) return;
    int rc = soulx_input_touch(_in, finger, action, x, y);
    if (action == SOULX_TOUCH_DOWN) {
        if (rc == 0) _fingers |= bit;
        else (void)soulx_input_touch(_in, finger, SOULX_TOUCH_UP, 0, 0);   // may be down when the reply was lost
    } else if (action == SOULX_TOUCH_UP) {
        _fingers &= ~bit;
    }
}

// input queue: logical size of the guest screen as input.c maps it
- (void)inputLogicalW:(int*)w h:(int*)h {
    *w = 0;
    *h = 0;
    if (_in) (void)soulx_input_screen_size(_in, w, h);
}

// input queue: swipe on a given finger, one move per frame on dispatch timers (other input interleaves)
- (void)swipeFinger:(int)f fromX:(int)x1 y:(int)y1 toX:(int)x2 y:(int)y2 ms:(int)ms {
    if (!_in || f < 0 || f >= SOULX_MAX_FINGERS) return;
    unsigned gen = ++_swipeGen[f];
    if (_fingers & (1u << f)) [self touch:f action:SOULX_TOUCH_UP x:0 y:0];
    [self touch:f action:SOULX_TOUCH_DOWN x:x1 y:y1];
    if (!(_fingers & (1u << f))) return;
    int steps = ms / INPUT_FRAME_MS;
    if (steps < 2) steps = 2;
    dispatch_time_t t0 = dispatch_time(DISPATCH_TIME_NOW, 0);
    __weak SoulXScreenView* weakSelf = self;
    for (int i = 1; i <= steps; i++) {
        int x = x1 + (int)((long long)(x2 - x1) * i / steps);
        int y = y1 + (int)((long long)(y2 - y1) * i / steps);
        bool last = i == steps;
        dispatch_after(dispatch_time(t0, (int64_t)ms * i / steps * (int64_t)NSEC_PER_MSEC), _inputQueue, ^{
            SoulXScreenView* s = weakSelf;
            if (!s || s->_swipeGen[f] != gen) return;
            [s touch:f action:SOULX_TOUCH_MOVE x:x y:y];
            if (last) [s touch:f action:SOULX_TOUCH_UP x:0 y:0];
        });
    }
}

// input queue
- (void)cancelSwipe:(int)f {
    if (f < 0 || f >= SOULX_MAX_FINGERS) return;
    _swipeGen[f]++;
}

// main thread: queue the latest position of a finger; at most one move per INPUT_MOVE_MS
- (void)queueMove:(MoveSlot*)slot finger:(int)finger x:(int)x y:(int)y {
    os_unfair_lock_lock(&slot->lock);
    slot->x = x;
    slot->y = y;
    slot->pending = true;
    bool schedule = !slot->scheduled;
    if (schedule) slot->scheduled = true;
    double last = slot->last_ms;
    os_unfair_lock_unlock(&slot->lock);
    if (!schedule) return;
    __weak SoulXScreenView* weakSelf = self;
    dispatch_after(after_ms(last + INPUT_MOVE_MS - soulx_now_ms()), _inputQueue, ^{
        [weakSelf flushMove:slot finger:finger];
    });
}

// input queue
- (void)flushMove:(MoveSlot*)slot finger:(int)finger {
    os_unfair_lock_lock(&slot->lock);
    bool pending = slot->pending;
    int x = slot->x, y = slot->y;
    slot->pending = false;
    slot->scheduled = false;
    if (pending) slot->last_ms = soulx_now_ms();
    os_unfair_lock_unlock(&slot->lock);
    if (pending) [self touch:finger action:SOULX_TOUCH_MOVE x:x y:y];
}

// main thread: forget a queued move (the finger is lifted)
- (void)dropMove:(MoveSlot*)slot {
    os_unfair_lock_lock(&slot->lock);
    slot->pending = false;
    os_unfair_lock_unlock(&slot->lock);
}

// ---------------------------------------------------------------------------
// Mouse and wheel (main thread)
// ---------------------------------------------------------------------------

- (BOOL)acceptsInput {
    return _inputEnabled && !_shutdown && !_overlay.editing;
}

// Inspect mode: read the logical (x, y) and pixel colour under the pointer, click to copy (for scripting).
- (void)setInspecting:(BOOL)on {
    _inspecting = on;
    self.window.acceptsMouseMovedEvents = on;
    if (on) [self.window makeFirstResponder:self];
    else [self.owner screenView:self message:@""];
}

// RGB (0xRRGGBB) of the current frame under logical pixel (lx, ly); 0xFFFFFFFF when no frame.
- (uint32_t)colorAtLogicalX:(int)lx y:(int)ly {
    SoulXFrameView fv;
    soulx_frame_pump_lock(_pump, &fv);
    uint32_t rgb = [self colorIn:&fv logicalX:lx y:ly];
    soulx_frame_pump_unlock(_pump);
    return rgb;
}

// colorAtLogicalX:y: on a frame the caller keeps still
- (uint32_t)colorIn:(const SoulXFrameView*)fv logicalX:(int)lx y:(int)ly {
    if (!fv->bgra) return 0xFFFFFFFFu;
    NSSize ls = self.logicalSize;
    if (ls.width < 1 || ls.height < 1 || fv->width < 1 || fv->height < 1) return 0xFFFFFFFFu;
    float u = ((float)lx + 0.5f) / (float)ls.width;
    float v = ((float)ly + 0.5f) / (float)ls.height;
    float st[2];
    rotate_uv(_texUpright ? 0 : _drawRotation, u, v, st);          // same mapping the shader samples with
    int nx = (int)(st[0] * (float)fv->width);
    int ny = (int)(st[1] * (float)fv->height);
    if (nx < 0) nx = 0; else if (nx >= fv->width) nx = fv->width - 1;
    if (ny < 0) ny = 0; else if (ny >= fv->height) ny = fv->height - 1;
    const uint8_t* px = fv->bgra + (size_t)ny * fv->stride + (size_t)nx * 4;
    return ((uint32_t)px[2] << 16) | ((uint32_t)px[1] << 8) | (uint32_t)px[0];
}

- (void)mouseMoved:(NSEvent*)event {
    if (!_inspecting) return;
    int x, y;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (![self logicalFromPoint:p clamp:NO x:&x y:&y]) { [self.owner screenView:self message:@""]; return; }
    uint32_t rgb = [self colorAtLogicalX:x y:y];
    NSString* msg = rgb == 0xFFFFFFFFu ? [NSString stringWithFormat:@"x=%d  y=%d", x, y]
                                       : [NSString stringWithFormat:@"x=%d  y=%d   #%06X", x, y, rgb & 0xFFFFFF];
    [self.owner screenView:self message:msg];
}

- (void)mouseDown:(NSEvent*)event {
    if (_inspecting) {                                             // copy the coordinate (and colour), don't tap
        int ix, iy;
        NSPoint ip = [self convertPoint:event.locationInWindow fromView:nil];
        if ([self logicalFromPoint:ip clamp:NO x:&ix y:&iy]) {
            uint32_t rgb = [self colorAtLogicalX:ix y:iy];
            NSString* copy = rgb == 0xFFFFFFFFu ? [NSString stringWithFormat:@"%d, %d", ix, iy]
                                                : [NSString stringWithFormat:@"%d, %d  -- #%06X", ix, iy, rgb & 0xFFFFFF];
            [[NSPasteboard generalPasteboard] clearContents];
            [[NSPasteboard generalPasteboard] setString:copy forType:NSPasteboardTypeString];
            [self.owner screenView:self message:[NSString stringWithFormat:SoulXText(@"Copied: %@"), copy]];
        }
        return;
    }
    if (![self acceptsInput]) return;
    [self.window makeFirstResponder:self];
    int x, y;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (![self logicalFromPoint:p clamp:NO x:&x y:&y]) return;      // letterbox
    _mouseDown = YES;
    [self dropMove:&_mouseSlot];
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_inputQueue, ^{
        SoulXScreenView* s = weakSelf;
        if (!s || ![s inputOpen]) return;
        [s touch:0 action:SOULX_TOUCH_DOWN x:x y:y];
    });
}

- (void)mouseDragged:(NSEvent*)event {
    if (!_mouseDown) return;
    int x, y;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (![self logicalFromPoint:p clamp:YES x:&x y:&y]) return;
    [self queueMove:&_mouseSlot finger:0 x:x y:y];
}

- (void)mouseUp:(NSEvent*)event {
    (void)event;
    if (!_mouseDown) return;
    _mouseDown = NO;
    __weak SoulXScreenView* weakSelf = self;
    MoveSlot* slot = &_mouseSlot;
    dispatch_async(_inputQueue, ^{
        SoulXScreenView* s = weakSelf;
        if (!s) return;
        [s flushMove:slot finger:0];            // the release position is the last one dragged to
        [s touch:0 action:SOULX_TOUCH_UP x:0 y:0];
    });
}

- (void)rightMouseDown:(NSEvent*)event {
    (void)event;
    if (![self acceptsInput]) return;
    [self androidKey:"BACK"];
}

- (void)scrollWheel:(NSEvent*)event {
    if (![self acceptsInput]) return;
    if (event.momentumPhase != NSEventPhaseNone) return;           // Android flings by itself
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    int x, y;
    NSRect r = [self imageRect];
    NSSize ls = self.logicalSize;
    if (r.size.height <= 0) return;
    if (event.phase != NSEventPhaseNone) {
        [self trackpadScroll:event at:p rect:r logical:ls];
        return;
    }
    if (![self logicalFromPoint:p clamp:NO x:&x y:&y]) return;
    double dy = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY * ls.height / r.size.height
                                                : event.scrollingDeltaY * WHEEL_PX_PER_NOTCH;
    if (dy == 0) return;
    _wheelAccum += dy;
    _wheelPoint = NSMakePoint(x, y);
    if (_wheelFlushQueued) return;
    _wheelFlushQueued = true;
    __weak SoulXScreenView* weakSelf = self;
    dispatch_after(after_ms(_wheelLastMs + WHEEL_SWIPE_GAP_MS - soulx_now_ms()), dispatch_get_main_queue(), ^{
        [weakSelf flushWheel];
    });
}

// main thread: one swipe for the accumulated wheel distance, centred on the pointer (same direction as the
// content moves on a trackpad: positive deltaY = content moves down = finger moves down)
- (void)flushWheel {
    _wheelFlushQueued = false;
    double d = _wheelAccum;
    _wheelAccum = 0;
    if (d == 0 || ![self acceptsInput]) return;
    _wheelLastMs = soulx_now_ms();
    NSSize ls = self.logicalSize;
    double lh = ls.height;
    d = clampd(d, -(lh - 2), lh - 2);
    double y1 = clampd(_wheelPoint.y - d / 2, 0, lh - 1), y2 = clampd(y1 + d, 0, lh - 1);
    if (y2 - y1 != d) y1 = clampd(y2 - d, 0, lh - 1);            // keep the length near an edge
    int x = (int)_wheelPoint.x, iy1 = (int)lround(y1), iy2 = (int)lround(y2);
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_inputQueue, ^{
        SoulXScreenView* s = weakSelf;
        if (!s || ![s inputOpen]) return;
        [s swipeFinger:SOULX_WINDOW_SCROLL_FINGER fromX:x y:iy1 toX:x y:iy2 ms:WHEEL_SWIPE_MS];
    });
}

// main thread: a trackpad scroll gesture drags finger 9 (it only goes down after WHEEL_SLOP_PX, so a resting
// two-finger touch never taps)
- (void)trackpadScroll:(NSEvent*)event at:(NSPoint)p rect:(NSRect)r logical:(NSSize)ls {
    __weak SoulXScreenView* weakSelf = self;
    const int f = SOULX_WINDOW_SCROLL_FINGER;
    if (event.phase & NSEventPhaseBegan) {
        int x, y;
        _wheelGesture = [self logicalFromPoint:p clamp:NO x:&x y:&y];
        _wheelStartX = x;
        _wheelStartY = y;
        _wheelDX = 0;
        _wheelDY = 0;
        _wheelDragging = NO;
    }
    if (!_wheelGesture) return;
    if (event.phase & (NSEventPhaseBegan | NSEventPhaseChanged)) {
        double k = ls.height / r.size.height;
        _wheelDX += event.scrollingDeltaX * k;
        _wheelDY += event.scrollingDeltaY * k;
        int x = clampi((int)lround(_wheelStartX + _wheelDX), 0, (int)ls.width - 1);
        int y = clampi((int)lround(_wheelStartY + _wheelDY), 0, (int)ls.height - 1);
        if (!_wheelDragging) {
            if (hypot(_wheelDX, _wheelDY) < WHEEL_SLOP_PX) return;
            _wheelDragging = YES;
            int sx = (int)_wheelStartX, sy = (int)_wheelStartY;
            [self dropMove:&_wheelSlot];
            dispatch_async(_inputQueue, ^{
                SoulXScreenView* s = weakSelf;
                if (!s || ![s inputOpen]) return;
                [s cancelSwipe:f];
                [s touch:f action:SOULX_TOUCH_DOWN x:sx y:sy];
            });
        }
        [self queueMove:&_wheelSlot finger:f x:x y:y];
    }
    if (event.phase & (NSEventPhaseEnded | NSEventPhaseCancelled)) {
        _wheelGesture = NO;
        if (!_wheelDragging) return;
        _wheelDragging = NO;
        MoveSlot* slot = &_wheelSlot;
        dispatch_async(_inputQueue, ^{
            SoulXScreenView* s = weakSelf;
            if (!s) return;
            [s flushMove:slot finger:f];
            [s touch:f action:SOULX_TOUCH_UP x:0 y:0];
        });
    }
}

// ---------------------------------------------------------------------------
// Keyboard (main thread -> input queue)
// ---------------------------------------------------------------------------

- (BOOL)performKeyEquivalent:(NSEvent*)event {
    if (event.type == NSEventTypeKeyDown && event.keyCode == VK_ESCAPE &&
        !(event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption))) {
        if (_overlay.editing) return [_overlay handleEscape];
        if (self.window.firstResponder == self && [self acceptsInput]) {
            [self keyDown:event];                // Esc = BACK, or a keymap binding
            return YES;
        }
    }
    return [super performKeyEquivalent:event];
}

- (void)keyDown:(NSEvent*)event {
    if (![self acceptsInput]) return;
    if (event.modifierFlags & NSEventModifierFlagCommand) return;      // shortcuts are the menus' business
    [self forwardKey:event down:YES];
}

- (void)keyUp:(NSEvent*)event {
    if (!_inputEnabled || _shutdown) return;
    [self forwardKey:event down:NO];
}

- (void)forwardKey:(NSEvent*)event down:(BOOL)down {
    unsigned short vk = event.keyCode;
    int key = soulx_key_from_mac_keycode(vk);
    BOOL repeat = down && event.isARepeat;
    NSString* chars = down ? event.characters : nil;
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_inputQueue, ^{
        SoulXScreenView* s = weakSelf;
        if (!s || ![s inputOpen]) return;
        [s inputKey:key vk:vk down:down repeat:repeat chars:chars];
    });
}

- (void)flagsChanged:(NSEvent*)event {
    if (!_inputEnabled || _shutdown || _overlay.editing) return;
    unsigned short vk = event.keyCode;
    unsigned raw = (unsigned)(event.modifierFlags & 0xFFFFu);
    unsigned bit;
    switch (vk) {
    case VK_SHIFT:    bit = DEV_LSHIFT; break;
    case VK_RSHIFT:   bit = DEV_RSHIFT; break;
    case VK_CONTROL:  bit = DEV_LCTRL;  break;
    case VK_RCONTROL: bit = DEV_RCTRL;  break;
    case VK_OPTION:   bit = DEV_LALT;   break;
    case VK_ROPTION:  bit = DEV_RALT;   break;
    default: return;                    // Command, Caps Lock, Fn stay on the Mac
    }
    BOOL down = (raw & bit) != 0;
    int key = soulx_key_from_mac_keycode(vk);
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_inputQueue, ^{
        SoulXScreenView* s = weakSelf;
        if (!s || ![s inputOpen]) return;
        [s inputModifier:key bit:bit down:down];
    });
}

// input queue: keymap binding -> touches; otherwise the guest keyboard
- (void)inputKey:(int)key vk:(unsigned short)vk down:(BOOL)down repeat:(BOOL)repeat chars:(NSString*)chars {
    if (key >= 0 && _km && soulx_keymap_binds(_km, key)) {
        if (repeat) return;
        SoulXKeymapAction acts[KEYMAP_ACTIONS_MAX];
        int n = soulx_keymap_key_event(_km, key, down, acts, KEYMAP_ACTIONS_MAX);
        [self performKeymapActions:acts count:n];
        return;
    }
    if (vk == VK_ESCAPE) {
        if (down && !repeat) (void)soulx_input_key(_in, "BACK");
        return;
    }

    int usage = key >= 0 ? soulx_key_hid_usage(key) : -1;
    const char* fallback_name = NULL;
    for (size_t i = 0; i < sizeof(k_extra_keys) / sizeof(k_extra_keys[0]); i++) {
        if (k_extra_keys[i].vk != vk) continue;
        if (usage <= 0) usage = k_extra_keys[i].usage;
        fallback_name = k_extra_keys[i].android;
    }
    if (usage > 0 && usage >= 0xE0 && usage <= 0xE7) return;           // modifiers arrive via flagsChanged

    if (usage > 0 && [self keyboardReady]) {
        if (repeat) return;                     // the guest repeats a held key itself
        if (down) {
            bool held = false;
            for (int i = 0; i < _hidCount; i++) held = held || _hidUsages[i] == usage;
            if (!held && _hidCount < HID_MAX_KEYS) _hidUsages[_hidCount++] = usage;
        } else {
            int k = 0;
            for (int i = 0; i < _hidCount; i++) {
                if (_hidUsages[i] != usage) _hidUsages[k++] = _hidUsages[i];
            }
            _hidCount = k;
        }
        if ([self sendHidState]) return;
    }
    if (!down) return;

    // fallback: printable ASCII as text, a few special keys as Android key codes
    const char* text = chars.UTF8String;
    bool printable = text && text[0];
    for (const char* c = text; printable && *c; c++) printable = *c >= 0x20 && *c <= 0x7e;
    if (printable) {
        (void)soulx_input_text(_in, text);
        return;
    }
    for (size_t i = 0; !fallback_name && i < sizeof(k_fallback_keys) / sizeof(k_fallback_keys[0]); i++) {
        if (k_fallback_keys[i].vk == vk) fallback_name = k_fallback_keys[i].android;
    }
    const char* name = key >= 0 ? soulx_key_name(key) : NULL;
    if (!fallback_name && name && name[0] == 'F' && name[1] >= '1' && name[1] <= '9') fallback_name = name;
    if (fallback_name) (void)soulx_input_key(_in, fallback_name);
}

// input queue
- (void)inputModifier:(int)key bit:(unsigned)bit down:(BOOL)down {
    if (key >= 0 && _km && soulx_keymap_binds(_km, key)) {
        SoulXKeymapAction acts[KEYMAP_ACTIONS_MAX];
        int n = soulx_keymap_key_event(_km, key, down, acts, KEYMAP_ACTIONS_MAX);
        [self performKeymapActions:acts count:n];
        return;
    }
    if (down) _sideMods |= bit;
    else _sideMods &= ~bit;
    if ([self keyboardReady]) (void)[self sendHidState];
}

// input queue: the full keyboard state to the virtual keyboard; NO (and the keyboard closed) on failure
- (BOOL)sendHidState {
    if (!_kbd) return NO;
    int mods = 0;
    if (_sideMods & (DEV_LSHIFT | DEV_RSHIFT)) mods |= SOULX_HID_MOD_LSHIFT;
    if (_sideMods & (DEV_LCTRL | DEV_RCTRL)) mods |= SOULX_HID_MOD_LCTRL;
    if (_sideMods & (DEV_LALT | DEV_RALT)) mods |= SOULX_HID_MOD_LALT;
    if (soulx_hidkbd_set(_kbd, mods, _hidUsages, _hidCount) == 0) return YES;
    soulx_log(SOULX_LOG_WARN, "the virtual keyboard of '%s' stopped working; reconnecting", _conf.name);
    soulx_hidkbd_close(_kbd);
    _kbd = NULL;
    _hidCount = 0;
    _kbdRetryAt = soulx_now_ms() + 2000.0;
    return NO;
}

// input queue: YES when the virtual keyboard is open; otherwise starts opening it (in the background)
- (BOOL)keyboardReady {
    if (_kbd) return YES;
    [self openKeyboard];
    return NO;
}

// input queue
- (void)openKeyboard {
    if (_kbd || _kbdOpening || _shutdown || !_in || soulx_now_ms() < _kbdRetryAt) return;
    _kbdOpening = true;
    NSString* serial = @(soulx_input_serial(_in));
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_hidQueue, ^{
        SoulXHidKbd* k = soulx_hidkbd_open(serial.UTF8String);
        SoulXScreenView* s = weakSelf;
        if (!s) {
            soulx_hidkbd_close(k);
            return;
        }
        dispatch_async(s->_inputQueue, ^{
            [s keyboardOpened:k];
        });
    });
}

// input queue
- (void)keyboardOpened:(SoulXHidKbd*)k {
    _kbdOpening = false;
    if (_shutdown) {
        soulx_hidkbd_close(k);
        k = NULL;
    }
    _kbd = k;
    _hidCount = 0;
    if (!k) {
        _kbdRetryAt = soulx_now_ms() + HIDKBD_RETRY_MS;
        soulx_log(SOULX_LOG_WARN, "the virtual keyboard of '%s' is not available; keys are typed with adb", _conf.name);
    } else {
        soulx_log(SOULX_LOG_DEBUG, "virtual keyboard ready on '%s'", _conf.name);
    }
    NSArray* waiters = [_kbdWaiters copy];
    [_kbdWaiters removeAllObjects];
    BOOL ok = k != NULL;
    for (void (^w)(BOOL) in waiters) {
        dispatch_async(dispatch_get_main_queue(), ^{ w(ok); });
    }
}

// input queue: keymap actions -> touches (fractions of the logical screen)
- (void)performKeymapActions:(const SoulXKeymapAction*)acts count:(int)n {
    if (n <= 0) return;
    int lw, lh;
    [self inputLogicalW:&lw h:&lh];
    if (lw <= 0 || lh <= 0) return;
    for (int i = 0; i < n; i++) {
        const SoulXKeymapAction* a = &acts[i];
        int x = clampi((int)lround(a->x * lw), 0, lw - 1);
        int y = clampi((int)lround(a->y * lh), 0, lh - 1);
        if (a->kind == SOULX_KM_ACT_SWIPE) {
            int x2 = clampi((int)lround(a->x2 * lw), 0, lw - 1);
            int y2 = clampi((int)lround(a->y2 * lh), 0, lh - 1);
            [self swipeFinger:a->finger fromX:x y:y toX:x2 y:y2 ms:a->ms];
        } else {
            if (a->action == SOULX_TOUCH_DOWN) [self cancelSwipe:a->finger];
            [self touch:a->finger action:a->action x:x y:y];
        }
    }
}

// input queue: every key up, every finger up
- (void)inputReleaseAll {
    if (_km) {
        SoulXKeymapAction acts[KEYMAP_ACTIONS_MAX];
        int n = soulx_keymap_release_all(_km, acts, KEYMAP_ACTIONS_MAX);
        [self performKeymapActions:acts count:n];
    }
    for (int f = 0; f < SOULX_MAX_FINGERS; f++) {
        _swipeGen[f]++;
        if (_fingers & (1u << f)) [self touch:f action:SOULX_TOUCH_UP x:0 y:0];
    }
    _fingers = 0;
    _sideMods = 0;
    if (_hidCount > 0 && _kbd) {
        _hidCount = 0;
        (void)[self sendHidState];
    }
    _hidCount = 0;
}

// ---------------------------------------------------------------------------
// Public input actions
// ---------------------------------------------------------------------------

- (void)androidKey:(const char*)name {
    NSString* key = @(name);
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_inputQueue, ^{
        SoulXScreenView* s = weakSelf;
        if (!s || ![s inputOpen]) return;
        (void)soulx_input_key(s->_in, key.UTF8String);
    });
}

// Input queue: force-read the rotation and publish it so the screen turns the instant the guest does.
// Keep polling until the guest reaches `target`; after ~2.4 s with no change, the app controls its own
// orientation (common for games) — say so instead of leaving the button feeling broken.
- (void)pollRotationTo:(int)target attempt:(int)n {
    if (_shutdown || !_in) { _rotating = NO; return; }
    __weak SoulXScreenView* weakSelf = self;
    dispatch_queue_t inputQueue = _inputQueue;
    int port = _port;
    dispatch_async(_pollQueue, ^{                          // the adb call off the input queue
        char serial[32];
        soulx_serial(port, serial, sizeof(serial));
        SoulXDisplayInfo d;
        (void)soulx_input_query_display(serial, false, &d);
        dispatch_async(inputQueue, ^{
            [weakSelf finishRotationPoll:d target:target attempt:n];
        });
    });
}

// input queue
- (void)finishRotationPoll:(SoulXDisplayInfo)d target:(int)target attempt:(int)n {
    if (_shutdown || !_in) { _rotating = NO; return; }
    if (_fingers == 0) soulx_input_apply_display(_in, &d);
    int r = soulx_input_rotation(_in);
    [self publishRotation];
    if (r == target) { _rotating = NO; return; }
    if (n >= 16) {
        _rotating = NO;
        __weak SoulXScreenView* weak = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            SoulXScreenView* t = weak;
            if (t) [t.owner screenView:t message:SoulXText(@"This app stays in one orientation.")];
        });
        return;
    }
    __weak SoulXScreenView* weak = self;
    dispatch_after(after_ms(150), _inputQueue, ^{ [weak pollRotationTo:target attempt:n + 1]; });
}

- (void)toggleRotation {
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_inputQueue, ^{
        SoulXScreenView* s = weakSelf;
        if (!s || ![s inputOpen] || s->_rotating) return;      // ignore clicks while a rotation is still settling
        s->_rotating = YES;
        int shown = atomic_load(&s->_publishedRotation);       // toggle from what the user currently sees
        int target = (shown == 1 || shown == 3) ? 0 : 1;
        char cmd[160];
        snprintf(cmd, sizeof(cmd), "settings put system accelerometer_rotation 0; settings put system user_rotation %d",
                 target);
        char* out = NULL;
        int rc = soulx_adb_shell(soulx_input_serial(s->_in), cmd, &out, 8000);
        free(out);
        if (rc != 0) {
            soulx_log(SOULX_LOG_ERROR, "could not rotate '%s' (adb exit status %d)", s->_conf.name, rc);
            s->_rotating = NO;
            return;
        }
        [s pollRotationTo:target attempt:0];                   // pollRotationTo force-reads the result each time
    });
}

// Music-stream (STREAM_MUSIC = 3) output volume, set absolutely and instantly via `cmd audio set-volume`
// (AudioManager.setStreamVolume) — no key-event stepping, so the slider has no lag.
- (void)setMusicVolume:(int)index {
    if (index < 0) index = 0;
    if (index > 15) index = 15;
    atomic_store(&_pendingMusicVol, index);             // coalesce: a fast drag collapses to the latest value
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_inputQueue, ^{
        SoulXScreenView* s = weakSelf;
        if (!s || ![s inputOpen]) return;
        int v = atomic_exchange(&s->_pendingMusicVol, -1);
        if (v < 0) return;                              // an earlier block already sent the newest value
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "cmd audio set-volume 3 %d", v);
        char* out = NULL;
        soulx_adb_shell(soulx_input_serial(s->_in), cmd, &out, 8000);
        free(out);
    });
}

// Types a string into the guest (⌘V paste). Uses the console text path (chunked, ASCII) on the input queue.
- (void)typeText:(NSString*)s {
    const char* utf8 = s.UTF8String;
    if (!utf8 || !utf8[0]) return;
    char* copy = strdup(utf8);
    if (!copy) return;
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_inputQueue, ^{
        SoulXScreenView* sv = weakSelf;
        if (sv && [sv inputOpen]) soulx_input_text(sv->_in, copy);
        free(copy);
    });
}

// Reads the current music volume (0..15, or -1 on failure) via `cmd audio get-stream-volume 3`.
- (void)readMusicVolumeThen:(void (^)(int v))done {
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_inputQueue, ^{
        SoulXScreenView* s = weakSelf;
        int vol = -1;
        if (s && [s inputOpen]) {
            char* out = NULL;
            if (soulx_adb_shell(soulx_input_serial(s->_in), "cmd audio get-stream-volume 3", &out, 8000) == 0 && out) {
                char* p = strstr(out, "-> ");                // "...getStreamVolume(3) -> 8"
                if (p) vol = atoi(p + 3);
            }
            free(out);
        }
        dispatch_async(dispatch_get_main_queue(), ^{ done(vol); });
    });
}

- (void)releaseAllInput {
    _mouseDown = NO;
    _wheelDragging = NO;
    _wheelGesture = NO;
    _wheelAccum = 0;
    [self dropMove:&_mouseSlot];
    [self dropMove:&_wheelSlot];
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_inputQueue, ^{
        [weakSelf inputReleaseAll];
    });
}

- (void)shutdownInput:(dispatch_block_t)done {
    [self releaseAllInput];
    _inputEnabled = NO;
    _shutdown = YES;
    _slowMode = NO;
    [_fpsTimer invalidate];
    _fpsTimer = nil;
    if (_keymapTimer) {
        dispatch_source_cancel(_keymapTimer);
        _keymapTimer = nil;
    }
    dispatch_source_t rt = _rotationTimer;
    _rotationTimer = nil;
    SoulXScreenView* s = self;
    dispatch_async(_inputQueue, ^{
        if (rt) dispatch_source_cancel(rt);
        if (s->_km) {
            soulx_keymap_runtime_free(s->_km);
            s->_km = NULL;
        }
        if (s->_kbd) {
            soulx_hidkbd_close(s->_kbd);
            s->_kbd = NULL;
        }
        if (s->_in) {
            soulx_input_close(s->_in);
            s->_in = NULL;
        }
        dispatch_async(dispatch_get_main_queue(), done);
    });
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

- (BOOL)simulateTapAtX:(int)x y:(int)y {
    NSSize ls = self.logicalSize;
    NSWindow* win = self.window;
    if (!win || x < 0 || y < 0 || x >= ls.width || y >= ls.height) return NO;
    NSRect r = [self imageRect];
    NSPoint vp = NSMakePoint(r.origin.x + (x + 0.5) / ls.width * r.size.width,
                             r.origin.y + (1.0 - (y + 0.5) / ls.height) * r.size.height);
    NSPoint wp = [self convertPoint:vp toView:nil];
    NSInteger number = win.windowNumber;
    NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:wp modifierFlags:0
                                      timestamp:[NSProcessInfo processInfo].systemUptime windowNumber:number
                                        context:nil eventNumber:0 clickCount:1 pressure:1.0f];
    [NSApp postEvent:down atStart:NO];
    dispatch_after(after_ms(SIM_TAP_HOLD_MS), dispatch_get_main_queue(), ^{
        NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:wp modifierFlags:0
                                        timestamp:[NSProcessInfo processInfo].systemUptime windowNumber:number
                                          context:nil eventNumber:0 clickCount:1 pressure:0.0f];
        [NSApp postEvent:up atStart:NO];
    });
    return YES;
}

// Characters a key produces without modifiers (for synthetic key events).
static NSString* key_characters(const char* name, unsigned short vk) {
    if (name && name[0] && !name[1]) {
        char c = name[0];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return [NSString stringWithFormat:@"%c", c];
    }
    unichar u = 0;
    switch (vk) {
    case VK_RETURN:         u = '\r'; break;
    case VK_TAB:            u = '\t'; break;
    case VK_DELETE:         u = 0x7f; break;
    case VK_ESCAPE:         u = 0x1b; break;
    case VK_LEFT:           u = NSLeftArrowFunctionKey; break;
    case VK_RIGHT:          u = NSRightArrowFunctionKey; break;
    case VK_UP:             u = NSUpArrowFunctionKey; break;
    case VK_DOWN:           u = NSDownArrowFunctionKey; break;
    default:
        if (name && strcmp(name, "SPACE") == 0) u = ' ';
        break;
    }
    return u ? [NSString stringWithCharacters:&u length:1] : @"";
}

- (void)simulateKeyNamed:(NSString*)name completion:(void (^)(BOOL ok))done {
    int key = soulx_key_from_name(name.UTF8String);
    int vk = -1;
    for (int c = 0; key >= 0 && c < 128 && vk < 0; c++) {
        if (soulx_key_from_mac_keycode((unsigned short)c) == key) vk = c;
    }
    if (vk < 0) {
        soulx_log(SOULX_LOG_ERROR, "--simulate-key: unknown key '%s'", name.UTF8String);
        done(NO);
        return;
    }
    void (^post)(BOOL) = ^(BOOL kbd_ok) {
        if (!kbd_ok) soulx_log(SOULX_LOG_WARN, "--simulate-key: no virtual keyboard; the key goes through adb");
        NSWindow* win = self.window;
        NSString* chars = key_characters(soulx_key_name(key), (unsigned short)vk);
        NSEvent* (^make)(NSEventType) = ^NSEvent*(NSEventType type) {
            return [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:0
                                   timestamp:[NSProcessInfo processInfo].systemUptime windowNumber:win.windowNumber
                                     context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:NO
                                     keyCode:(unsigned short)vk];
        };
        // A key event goes to the key window; deliver it to ours directly when another app is active
        void (^deliver)(NSEvent*) = ^(NSEvent* e) {
            if (win.isKeyWindow) [NSApp postEvent:e atStart:NO];
            else [win sendEvent:e];
        };
        [win makeFirstResponder:self];
        deliver(make(NSEventTypeKeyDown));
        dispatch_after(after_ms(SIM_KEY_HOLD_MS), dispatch_get_main_queue(), ^{
            deliver(make(NSEventTypeKeyUp));
            done(YES);
        });
    };
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_inputQueue, ^{
        SoulXScreenView* s = weakSelf;
        if (!s || ![s inputOpen]) {
            dispatch_async(dispatch_get_main_queue(), ^{ post(NO); });
            return;
        }
        if (s->_kbd) {
            dispatch_async(dispatch_get_main_queue(), ^{ post(YES); });
            return;
        }
        s->_kbdRetryAt = 0;
        [s->_kbdWaiters addObject:[post copy]];
        [s openKeyboard];
        if (!s->_kbdOpening && !s->_kbd) [s keyboardOpened:NULL];   // could not even start: run the waiters
    });
}

// ---------------------------------------------------------------------------
// Keymap: the active keymap follows the foreground app
// ---------------------------------------------------------------------------

- (void)startKeymapWatch {
    __weak SoulXScreenView* weakSelf = self;
    _keymapTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _keymapQueue);
    dispatch_source_set_timer(_keymapTimer, after_ms(500), KEYMAP_POLL_MS * NSEC_PER_MSEC, 100 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(_keymapTimer, ^{
        [weakSelf keymapTick:NO];
    });
    dispatch_resume(_keymapTimer);
}

// keymap queue
- (void)keymapTick:(BOOL)force {
    bool any = keymaps_present();
    if (!any && !_kmLoaded && !force) return;
    char pkg[256] = "";
    if (any && soulx_foreground_package(_serial, pkg, sizeof(pkg)) != 0) pkg[0] = '\0';
    long long sig = any ? file_mtime_ns("default") ^ (pkg[0] ? file_mtime_ns(pkg) * 31 : 0) : 0;
    if (!force && _kmLoaded == any && sig == _kmSignature && strcmp(pkg, _kmPackage) == 0) return;
    _kmSignature = sig;
    _kmLoaded = any;
    snprintf(_kmPackage, sizeof(_kmPackage), "%s", pkg);

    SoulXKeymap* km = calloc(1, sizeof(SoulXKeymap));
    if (!km) return;
    int rc = any ? soulx_keymap_load(pkg, km) : 2;
    if (rc < 0) soulx_log(SOULX_LOG_WARN, "keymap for '%s' could not be read", pkg[0] ? pkg : "default");
    bool active = (rc == 0 || rc == 1) && km->count > 0;
    NSString* status = @"";
    if (active && rc == 0) {
        status = [NSString stringWithFormat:SoulXText(@"Keymap: %@ (%d keys)"), @(pkg), km->count];
    } else if (active) {
        status = SoulXText(@"Keymap: default");
    }
    SoulXKeymapRuntime* rt = active ? soulx_keymap_runtime_new(km) : NULL;
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_inputQueue, ^{
        SoulXScreenView* s = weakSelf;
        if (!s || s->_shutdown) {
            soulx_keymap_runtime_free(rt);
            return;
        }
        if (s->_km) {
            SoulXKeymapAction acts[KEYMAP_ACTIONS_MAX];
            int n = soulx_keymap_release_all(s->_km, acts, KEYMAP_ACTIONS_MAX);
            [s performKeymapActions:acts count:n];
            soulx_keymap_runtime_free(s->_km);
        }
        s->_km = rt;
        if (rt) {                               // joystick radius is a fraction of the shorter side
            int lw = 0, lh = 0;
            [s inputLogicalW:&lw h:&lh];
            soulx_keymap_runtime_set_screen(rt, lw, lh);
        }
    });
    dispatch_async(dispatch_get_main_queue(), ^{
        SoulXScreenView* s = weakSelf;
        if (s) {
            [s->_overlay setActiveKeymap:active ? km : NULL];
            [s.owner screenView:s keymapStatus:status];
        }
        free(km);
    });
}

- (void)setKeymapEditing:(BOOL)editing {
    if (editing == _overlay.editing) return;
    if (!editing) {
        [_overlay endEditing:NO];
        return;
    }
    [self releaseAllInput];
    __weak SoulXScreenView* weakSelf = self;
    NSString* serial = @(_serial);
    dispatch_async(_keymapQueue, ^{
        char pkg[256] = "";
        if (soulx_foreground_package(serial.UTF8String, pkg, sizeof(pkg)) != 0) pkg[0] = '\0';
        NSString* package = @(pkg);
        SoulXKeymap* km = calloc(1, sizeof(SoulXKeymap));
        if (!km) return;
        int rc = soulx_keymap_load(pkg, km);
        if (rc < 0 || rc == 2) memset(km, 0, sizeof(*km));
        snprintf(km->package, sizeof(km->package), "%s", pkg);    // edits are saved for this app
        dispatch_async(dispatch_get_main_queue(), ^{
            SoulXScreenView* s = weakSelf;
            if (s && !s->_shutdown) {
                [s->_overlay beginEditing:km package:package];
                [s.owner screenViewEditingDidChange:s];
            }
            free(km);
        });
    });
}

- (void)keymapEditorDidSave {
    __weak SoulXScreenView* weakSelf = self;
    dispatch_async(_keymapQueue, ^{
        [weakSelf keymapTick:YES];
    });
}

- (void)keymapEditorDidEnd {
    [self.window makeFirstResponder:self];
    [self.owner screenViewEditingDidChange:self];
}

@end
