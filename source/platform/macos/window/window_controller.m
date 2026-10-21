/*
 * SoulX Android Emulator - the window of 'soulx window'
 *
 *   ┌──────────────────────────────┬──────────┐
 *   │ screen view (aspect-fit)     │ toolbar  │   toolbar: Back Home Recents Rotate Record Run Screenshot Keymap
 *   ├──────────────────────────────┴──────────┤
 *   │ status: fps · serial · rotation · script · keymap · message [Show in Finder]
 *   └─────────────────────────────────────────┘
 *
 * Record and Run are child processes of this same executable ('soulx record' / 'soulx run'), never code running in
 * the window process: a script crash cannot take the window down and Ctrl+C state stays separate. They run in
 * their own process group with default signal handling; stdout and stderr stream into the log panel.
 * Closing the window stops the instance gracefully (soulx_mac_stop on a background queue) while a notice counts
 * the seconds; "Close Window (Keep Running)" leaves the instance alone.
 */

#import "window_internal.h"
#include "soulx_android.h"          // soulx_start_recording / soulx_stop_recording (screen video)

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "image.h"

extern char** environ;

#define TOOLBAR_WIDTH           192.0       // the column right of the screen; the toolbar card sits inside it
#define RAIL_INSET              8.0         // gap between the card and the column's edges
#define RAIL_PAD                12.0        // the card's own inner padding
#define RAIL_INNER              (TOOLBAR_WIDTH - 2 * RAIL_INSET - 2 * RAIL_PAD)
#define COLLAPSED_WIDTH         8.0         // collapsed = toolbar fully hidden; the title-bar button brings it back
#define STATUS_HEIGHT           24.0
#define SCREEN_INSET            8.0         // the Android screen sits inset in the window, with rounded corners
#define SCREEN_RADIUS           12.0
#define VERT_CHROME             (STATUS_HEIGHT + 2 * SCREEN_INSET)   // window height that is not the screen
#define INITIAL_HEIGHT_SHARE    0.85        // of the screen's visible height
#define MIN_SCREEN_SIDE         200.0
#define LOG_MAX_CHARS           (1024 * 1024)
#define LOG_KEEP_CHARS          (768 * 1024)
#define RECORD_STOP_WAIT_S      10.0        // SIGINT -> SIGKILL for the recorder (it writes the file on SIGINT)
#define RUN_STOP_WAIT_S         3.0         // SIGINT -> SIGKILL for a script
#define CLOSE_JOB_WAIT_S        3.0         // while the window closes
#define SLOW_MODE_AFTER_BOOT_S  60.0        // no shared memory this long after the boot: slow display mode
#define SLOW_MODE_BOOTED_S      5.0         // ... when the guest had booted before the window opened
#define HOOK_FRAME_TIMEOUT_S    120.0       // test hooks give up when no frame arrives
#define HOOK_DUMP_TIMEOUT_S     15.0

typedef enum {
    JOB_NONE = 0,
    JOB_RECORD,
    JOB_RUN,
    JOB_INSTALL          // drag-and-drop: `soulx install` an APK, or `soulx adb push` a file
} JobKind;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Absolute, symlink-free path of this executable (record / run children are this same program).
static bool self_executable(char* buf, size_t cap) {
    char raw[PATH_MAX];
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0) return false;
    char real[PATH_MAX];
    if (!realpath(raw, real)) return false;
    int n = snprintf(buf, cap, "%s", real);
    return n > 0 && (size_t)n < cap;
}

// argv[0] absolute; stdin = /dev/null; stdout and stderr on one pipe (*out_fd = read end); own process group;
// default signal dispositions and an empty mask (the window ignores SIGINT / SIGPIPE itself).
// extra_env, when non-NULL, is one extra "KEY=VALUE" entry added to the child's environment (e.g. SOULX_UI_DIR).
static pid_t spawn_piped(const char* const* argv, const char* extra_env, int* out_fd) {
    *out_fd = -1;
    int p[2];
    if (pipe(p) != 0) return -1;
    (void)fcntl(p[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(p[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    int rc = posix_spawn_file_actions_init(&fa);
    if (rc != 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    rc = posix_spawnattr_init(&attr);
    if (rc != 0) {
        posix_spawn_file_actions_destroy(&fa);
        close(p[0]);
        close(p[1]);
        return -1;
    }
    sigset_t defaults, mask;
    sigemptyset(&defaults);
    sigemptyset(&mask);
    const int sigs[] = { SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGALRM, SIGTERM, SIGCHLD, SIGTSTP, SIGTTIN, SIGTTOU,
                         SIGUSR1, SIGUSR2, SIGWINCH };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaddset(&defaults, sigs[i]);
    short flags = (short)(POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT |
                          POSIX_SPAWN_SETPGROUP);
    if (rc == 0) rc = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, p[1], STDOUT_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, p[1], STDERR_FILENO);
    if (rc == 0) rc = posix_spawnattr_setsigdefault(&attr, &defaults);
    if (rc == 0) rc = posix_spawnattr_setsigmask(&attr, &mask);
    if (rc == 0) rc = posix_spawnattr_setpgroup(&attr, 0);
    if (rc == 0) rc = posix_spawnattr_setflags(&attr, flags);
    pid_t pid = -1;
    char** envp = environ;
    char** built = NULL;
    if (rc == 0 && extra_env) {
        size_t c = 0;
        while (environ[c]) c++;
        built = calloc(c + 2, sizeof(char*));
        if (built) {
            for (size_t i = 0; i < c; i++) built[i] = environ[i];
            built[c] = (char*)extra_env;
            envp = built;
        }
    }
    if (rc == 0) rc = posix_spawn(&pid, argv[0], &fa, &attr, (char* const*)argv, envp);
    free(built);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(p[1]);
    if (rc != 0) {
        close(p[0]);
        soulx_log(SOULX_LOG_ERROR, "cannot start %s: %s", argv[0], strerror(rc));
        return -1;
    }
    *out_fd = p[0];
    return pid;
}

static int decode_status(int wstatus) {
    if (WIFEXITED(wstatus)) return WEXITSTATUS(wstatus);
    if (WIFSIGNALED(wstatus)) return 128 + WTERMSIG(wstatus);
    return -1;
}

static NSString* timestamp_now(void) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmv);
    return @(buf);
}

// "2026-10-13T09:47:12Z" -> epoch seconds (0 when unparsable)
static time_t parse_utc(const char* iso) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    const char* end = iso ? strptime(iso, "%Y-%m-%dT%H:%M:%S", &tmv) : NULL;
    return end ? timegm(&tmv) : 0;
}

static NSString* elapsed_text(double seconds) {
    long s = seconds > 0 ? (long)seconds : 0;
    return [NSString stringWithFormat:@"%ld:%02ld", s / 60, s % 60];
}

// Decodes UTF-8 output that may end in the middle of a character; *keep = trailing bytes to prepend next time.
static NSString* decode_chunk(NSMutableData* pending) {
    const uint8_t* b = pending.bytes;
    size_t n = pending.length, cut = n;
    for (size_t back = 1; back <= 3 && back <= n; back++) {
        uint8_t c = b[n - back];
        if ((c & 0xC0) == 0x80) continue;                       // continuation byte
        size_t need = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (need > back) cut = n - back;                        // incomplete sequence at the end
        break;
    }
    NSString* s = [[NSString alloc] initWithBytes:b length:cut encoding:NSUTF8StringEncoding];
    if (!s) s = [[NSString alloc] initWithBytes:b length:cut encoding:NSISOLatin1StringEncoding];
    [pending replaceBytesInRange:NSMakeRange(0, cut) withBytes:NULL length:0];
    return s ? s : @"";
}

// ---------------------------------------------------------------------------
// Content view: screen | toolbar over the status bar
// ---------------------------------------------------------------------------

@interface SoulXContentView : NSView
@property (nonatomic, strong) NSView* screen;
@property (nonatomic, strong) NSView* toolbar;
@property (nonatomic, strong) NSView* status;
@property (nonatomic, strong) NSView* notice;
@property (nonatomic, strong) NSView* flash;
@property (nonatomic) BOOL toolbarCollapsed;
@property (nonatomic) CGFloat fixedScreenWidth;   // >0 during the collapse animation: lock the game width, toolbar = rest
@property (nonatomic, copy) void (^onDropFiles)(NSArray<NSURL*>*);   // files dropped on the window
@property (nonatomic, copy) void (^onDropActive)(BOOL active);       // a file drag entered/left the window
@end

@implementation SoulXContentView

- (void)place {
    NSRect b = self.bounds;
    CGFloat screenW = self.fixedScreenWidth > 0
        ? self.fixedScreenWidth
        : b.size.width - SCREEN_INSET - (self.toolbarCollapsed ? COLLAPSED_WIDTH : TOOLBAR_WIDTH);
    screenW = fmax(fmin(screenW, b.size.width - SCREEN_INSET - 1), 1);
    NSRect screen = NSMakeRect(SCREEN_INSET, STATUS_HEIGHT + SCREEN_INSET, screenW, fmax(b.size.height - VERT_CHROME, 1));
    self.screen.frame = screen;
    self.flash.frame = screen;
    NSRect column = NSMakeRect(NSMaxX(screen), STATUS_HEIGHT, fmax(b.size.width - NSMaxX(screen), 0),
                               fmax(b.size.height - STATUS_HEIGHT, 0));
    self.toolbar.frame = column.size.width > 2 * RAIL_INSET ? NSInsetRect(column, RAIL_INSET, RAIL_INSET) : NSZeroRect;
    self.status.frame = NSMakeRect(0, 0, b.size.width, STATUS_HEIGHT);
    NSSize ns = self.notice.fittingSize;
    CGFloat nw = fmin(fmax(ns.width, 240), fmax(screen.size.width - 24, 120));
    self.notice.frame = NSMakeRect(round(NSMidX(screen) - nw / 2), round(NSMidY(screen) - 32), nw, 64);
}

- (void)resizeSubviewsWithOldSize:(NSSize)old {
    (void)old;
    [self place];
}

- (void)layout {
    [super layout];
    [self place];
}

// Drag-and-drop: APK files to install, any other file to push into the guest.
- (NSArray<NSURL*>*)droppedURLs:(id<NSDraggingInfo>)sender {
    NSArray<NSURL*>* urls = [sender.draggingPasteboard readObjectsForClasses:@[ NSURL.class ]
                             options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
    return urls ? urls : @[];
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    if ([self droppedURLs:sender].count == 0) return NSDragOperationNone;
    if (self.onDropActive) self.onDropActive(YES);
    return NSDragOperationCopy;
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
    (void)sender;
    if (self.onDropActive) self.onDropActive(NO);
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray<NSURL*>* urls = [self droppedURLs:sender];
    if (self.onDropActive) self.onDropActive(NO);
    if (urls.count == 0 || !self.onDropFiles) return NO;
    self.onDropFiles(urls);
    return YES;
}

@end

// ---------------------------------------------------------------------------
// Toolbar row: an aligned icon column + label, with a hover/selection highlight.
// Stays an NSButton so .title / .image / .contentTintColor / .enabled / target-action keep working.
// ---------------------------------------------------------------------------

#define ROW_HEIGHT        32.0
#define ROW_ICON_X        10.0
#define ROW_ICON_SIZE     17.0
#define ROW_TEXT_X        36.0

@interface SoulXToolRow : NSButton
@property (nonatomic) BOOL hasFlyout;                    // draw a trailing chevron; opens a menu on hover
@property (nonatomic, copy) void (^onHover)(SoulXToolRow*);
@end

@implementation SoulXToolRow {
    BOOL _hover;
    NSTrackingArea* _track;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_track) [self removeTrackingArea:_track];
    _track = [[NSTrackingArea alloc] initWithRect:self.bounds
              options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                owner:self userInfo:nil];
    [self addTrackingArea:_track];
}

- (void)mouseEntered:(NSEvent*)e { (void)e; _hover = YES; self.needsDisplay = YES; if (self.onHover) self.onHover(self); }
- (void)mouseExited:(NSEvent*)e  { (void)e; _hover = NO;  self.needsDisplay = YES; }

// Redraw when the properties the controller sets change.
- (void)setTitle:(NSString*)t            { [super setTitle:t]; self.needsDisplay = YES; }
- (void)setImage:(NSImage*)i             { [super setImage:i]; self.needsDisplay = YES; }
- (void)setContentTintColor:(NSColor*)c  { [super setContentTintColor:c]; self.needsDisplay = YES; }
- (void)setEnabled:(BOOL)e               { [super setEnabled:e]; self.needsDisplay = YES; }

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = self.bounds;
    BOOL on = self.isEnabled;

    NSColor* bg = nil;
    if (self.isHighlighted) bg = [NSColor.labelColor colorWithAlphaComponent:0.16];
    else if (_hover && on)  bg = [NSColor.labelColor colorWithAlphaComponent:0.08];
    if (bg) {
        [bg set];
        [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(b, 0, 1) xRadius:6 yRadius:6] fill];
    }

    NSColor* accent = self.contentTintColor;
    NSColor* fg = accent ? accent : (on ? NSColor.labelColor : NSColor.tertiaryLabelColor);

    if (self.image) {
        NSImageSymbolConfiguration* cfg =
            [[NSImageSymbolConfiguration configurationWithPointSize:15 weight:NSFontWeightRegular]
                configurationByApplyingConfiguration:[NSImageSymbolConfiguration configurationWithPaletteColors:@[ fg ]]];
        NSImage* icon = [self.image imageWithSymbolConfiguration:cfg];
        if (!icon) icon = self.image;
        NSRect ir = NSMakeRect(ROW_ICON_X, round((b.size.height - ROW_ICON_SIZE) / 2), ROW_ICON_SIZE, ROW_ICON_SIZE);
        [icon drawInRect:ir fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0
          respectFlipped:YES hints:nil];
    }

    NSMutableParagraphStyle* ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    NSDictionary* attrs = @{ NSFontAttributeName: [NSFont systemFontOfSize:13],
                             NSForegroundColorAttributeName: fg,
                             NSParagraphStyleAttributeName: ps };
    NSString* t = self.title ? self.title : @"";
    CGFloat th = [t sizeWithAttributes:attrs].height;
    CGFloat rightPad = self.hasFlyout ? 18 : 6;
    NSRect tr = NSMakeRect(ROW_TEXT_X, round((b.size.height - th) / 2), b.size.width - ROW_TEXT_X - rightPad, th);
    [t drawInRect:tr withAttributes:attrs];

    if (self.hasFlyout) {                                   // trailing chevron: "opens a menu"
        NSImage* chev = [NSImage imageWithSystemSymbolName:@"chevron.right" accessibilityDescription:nil];
        NSImageSymbolConfiguration* cc =
            [[NSImageSymbolConfiguration configurationWithPointSize:10 weight:NSFontWeightSemibold]
                configurationByApplyingConfiguration:
                    [NSImageSymbolConfiguration configurationWithPaletteColors:@[ NSColor.tertiaryLabelColor ]]];
        NSImage* ci = [chev imageWithSymbolConfiguration:cc];
        if (ci) {
            NSSize cs = ci.size;
            NSRect cr = NSMakeRect(b.size.width - cs.width - 7, round((b.size.height - cs.height) / 2), cs.width, cs.height);
            [ci drawInRect:cr fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0
              respectFlipped:YES hints:nil];
        }
    }
}

@end

// ---------------------------------------------------------------------------
// Navigation tile: a square-ish button, icon over its label, on a faint rounded plate (Back / Home / ...).
// ---------------------------------------------------------------------------

#define TILE_HEIGHT       50.0

@interface SoulXNavTile : NSButton
@end

@implementation SoulXNavTile {
    BOOL _hover;
    NSTrackingArea* _track;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_track) [self removeTrackingArea:_track];
    _track = [[NSTrackingArea alloc] initWithRect:self.bounds
              options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                owner:self userInfo:nil];
    [self addTrackingArea:_track];
}

- (void)mouseEntered:(NSEvent*)e { (void)e; _hover = YES; self.needsDisplay = YES; }
- (void)mouseExited:(NSEvent*)e  { (void)e; _hover = NO;  self.needsDisplay = YES; }
- (void)setTitle:(NSString*)t    { [super setTitle:t]; self.needsDisplay = YES; }
- (void)setEnabled:(BOOL)e       { [super setEnabled:e]; self.needsDisplay = YES; }

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = NSInsetRect(self.bounds, 0.5, 0.5);
    BOOL on = self.isEnabled;
    CGFloat fill = self.isHighlighted ? 0.16 : ((_hover && on) ? 0.11 : 0.06);
    NSBezierPath* plate = [NSBezierPath bezierPathWithRoundedRect:b xRadius:11 yRadius:11];
    [[NSColor.labelColor colorWithAlphaComponent:fill] setFill];
    [plate fill];
    [[NSColor.labelColor colorWithAlphaComponent:0.09] setStroke];
    plate.lineWidth = 1;
    [plate stroke];

    NSColor* fg = on ? NSColor.labelColor : NSColor.tertiaryLabelColor;
    NSDictionary* attrs = @{ NSFontAttributeName: [NSFont systemFontOfSize:11 weight:NSFontWeightMedium],
                             NSForegroundColorAttributeName: fg };
    NSString* t = self.title ? self.title : @"";
    NSSize ts = [t sizeWithAttributes:attrs];
    CGFloat iconH = 17, gap = 4;
    CGFloat top = round((self.bounds.size.height - iconH - gap - ts.height) / 2);
    if (self.image) {
        NSImageSymbolConfiguration* cfg =
            [[NSImageSymbolConfiguration configurationWithPointSize:15 weight:NSFontWeightRegular]
                configurationByApplyingConfiguration:[NSImageSymbolConfiguration configurationWithPaletteColors:@[ fg ]]];
        NSImage* icon = [self.image imageWithSymbolConfiguration:cfg];
        if (!icon) icon = self.image;
        NSSize is = icon.size;
        CGFloat scale = fmin(1.0, iconH / fmax(is.height, 1));
        NSSize ds = NSMakeSize(is.width * scale, is.height * scale);
        NSRect ir = NSMakeRect(round((self.bounds.size.width - ds.width) / 2), top + round((iconH - ds.height) / 2),
                               ds.width, ds.height);
        [icon drawInRect:ir fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0
          respectFlipped:YES hints:nil];
    }
    [t drawAtPoint:NSMakePoint(round((self.bounds.size.width - ts.width) / 2), top + iconH + gap) withAttributes:attrs];
}

- (BOOL)isFlipped { return YES; }

@end

// ---------------------------------------------------------------------------
// SoulXWindowController
// ---------------------------------------------------------------------------

@interface SoulXWindowController ()
- (void)askRunOptionsFor:(NSString*)path;   // the Run-script options sheet (times + speed)
- (void)applyToolbarCollapsed:(BOOL)collapsed;
- (CGFloat)toolbarWidth;                    // current width of the toolbar column (collapsed or not)
- (void)setVideoButton;                     // refresh the 錄影 toolbar button for the recording state
- (void)handleDroppedFiles:(NSArray<NSURL*>*)urls;   // install an APK / push a file dropped on the window
- (NSPopover*)flyoutPopoverWith:(NSArray<NSView*>*)buttons;
- (SoulXToolRow*)categoryButton:(NSString*)title symbol:(NSString*)symbol ident:(NSString*)ident popover:(NSPopover*)pop;
- (void)showFlyoutPopover:(NSPopover*)pop from:(SoulXToolRow*)btn;
- (void)closeFlyout;
- (void)checkFlyoutHover;
- (void)showScriptPicker;                   // the click-list of named scripts (replaces the file browser)
- (void)startRun:(NSString*)path askOptions:(BOOL)ask;   // run a script (requires a declared name)
- (void)soulxBrowseScript:(id)sender;        // "Browse…" fallback: pick any .lua file
- (void)scriptPicked:(id)sender;            // a row in the script picker was clicked
- (NSString*)scriptName:(NSString*)path;    // the script's declared `-- name:` (nil when none)
- (void)ensureName:(NSString*)path fallback:(NSString*)fallback;   // give a recording a name if it has none
@end

@implementation SoulXWindowController {
    SoulXInstanceConf _conf;
    SoulXRunState _st;
    SoulXWindowOptions _opts;
    char _serial[64];
    NSString* _android;          // "Android 17"
    SoulXContentView* _content;
    SoulXScreenView* _screen;
    NSStackView* _tools;
    NSButton* _collapseButton;           // sidebar toggle in the window title bar
    NSMutableDictionary<NSString*, NSButton*>* _buttons;
    NSSlider* _volumeSlider;             // output volume control in the sidebar
    NSTextField* _volumeValue;           // its level as a percentage, beside the "Volume" heading
    NSView* _volumeCard;                 // the plate around the volume controls
    double _volumeTouchedMs;             // when the user last moved the slider (polling waits a moment after)
    int _volumeTicks;
    NSMutableDictionary<NSString*, NSPopover*>* _flyoutPopovers;   // category ident -> its flyout
    NSMutableDictionary<NSString*, SoulXToolRow*>* _categoryButtons;  // category ident -> its toolbar row
    NSPopover* _flyout;                  // the open category flyout, if any
    SoulXToolRow* _flyoutOwner;           // the category row it belongs to
    NSTimer* _flyoutTimer;               // polls the mouse to close the flyout when it leaves
    NSTextField* _fpsLabel;
    NSTextField* _serialLabel;
    NSTextField* _rotationLabel;
    NSTextField* _specLabel;             // resolution and orientation, right end of the status bar
    NSTextField* _jobLabel;
    NSTextField* _keymapLabel;
    NSTextField* _messageLabel;
    NSButton* _finderButton;
    NSString* _finderPath;
    NSView* _notice;
    NSTextField* _noticeLabel;
    NSProgressIndicator* _noticeSpinner;
    NSTimer* _tick;
    double _openedMs;
    time_t _engineStarted;

    // boot
    BOOL _booted;
    BOOL _bootedBeforeWindow;
    BOOL _bootKnown;
    double _bootedMs;

    // closing
    BOOL _closing;
    BOOL _stopping;
    double _stopStartMs;
    BOOL _finished;

    // record / run child
    JobKind _job;
    pid_t _jobPid;
    NSString* _jobPath;          // recording: temporary file; run: the script
    double _jobStartMs;
    dispatch_source_t _jobExit;
    dispatch_source_t _jobRead;
    NSTimer* _jobKill;
    BOOL _jobStopRequested;
    NSMutableArray<dispatch_block_t>* _jobWaiters;
    NSString* _uiDir;            // run job: SOULX_UI_DIR rendezvous for the live parameter panel
    SoulXParamPanel* _paramPanel;

    // screen video recording (engine-side; independent of record/run)
    BOOL _recordingVideo;
    NSString* _videoPath;
    unsigned _recCounter;

    // log panel
    NSPanel* _logPanel;
    NSTextView* _logView;

    // test hooks
    int _hooksPending;
    BOOL _hooksFailed;
    BOOL _hooksStarted;
    int _lastAspect;             // 1 = landscape, 0 = portrait, -1 = unknown
}

- (instancetype)initWithConf:(const SoulXInstanceConf*)conf state:(const SoulXRunState*)st
                     options:(const SoulXWindowOptions*)opts {
    NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 900)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                          NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    self = [super initWithWindow:win];
    if (!self) return nil;
    _conf = *conf;
    _st = *st;
    _opts = *opts;
    soulx_serial(st->port, _serial, sizeof(_serial));
    const SoulXPackage* img = soulx_runtime_find(conf->image);
    _android = img && img->android_name ? @(img->android_name) : @"Android";
    _buttons = [NSMutableDictionary dictionary];
    _jobWaiters = [NSMutableArray array];
    _openedMs = soulx_now_ms();
    _engineStarted = parse_utc(st->started);
    _lastAspect = -1;

    _screen = [[SoulXScreenView alloc] initWithConf:conf state:st];
    if (!_screen) return nil;
    _screen.owner = self;
    _screen.wantsLayer = YES;
    _screen.layer.cornerRadius = SCREEN_RADIUS;
    _screen.layer.cornerCurve = kCACornerCurveContinuous;
    _screen.layer.masksToBounds = YES;
    _screen.layer.borderWidth = 1;                       // a hairline edge, so the screen reads as a glass pane
    _screen.layer.borderColor = [NSColor colorWithWhite:1 alpha:0.08].CGColor;

    win.title = [NSString stringWithFormat:@"%s · %@", conf->name, _android];
    win.delegate = self;
    win.releasedWhenClosed = NO;
    win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    win.backgroundColor = NSColor.windowBackgroundColor;     // shows around the inset, rounded screen
    win.contentMinSize = NSMakeSize(SCREEN_INSET + TOOLBAR_WIDTH + MIN_SCREEN_SIDE, VERT_CHROME + MIN_SCREEN_SIDE);
    win.tabbingMode = NSWindowTabbingModeDisallowed;

    _content = [[SoulXContentView alloc] initWithFrame:NSMakeRect(0, 0, 480, 900)];
    _content.screen = _screen;
    _content.toolbar = [self buildToolbar];
    _content.status = [self buildStatusBar];
    _content.notice = [self buildNotice];
    NSView* flash = [[NSView alloc] initWithFrame:NSZeroRect];
    flash.wantsLayer = YES;
    flash.layer.backgroundColor = NSColor.whiteColor.CGColor;
    flash.layer.cornerRadius = SCREEN_RADIUS;            // the screenshot flash follows the rounded screen
    flash.alphaValue = 0;
    _content.flash = flash;
    for (NSView* v in @[ _screen, _content.toolbar, _content.status, flash, _content.notice ]) [_content addSubview:v];
    win.contentView = _content;
    [self installTitlebarToggle];
    [self installCenteredTitle];
    [self applyToolbarCollapsed:[[NSUserDefaults standardUserDefaults] boolForKey:@"SoulXToolbarCollapsed"]];
    __weak SoulXWindowController* weakVol = self;
    [_screen readMusicVolumeThen:^(int v) {            // show the real output level on the slider
        SoulXWindowController* s = weakVol;
        if (s && v >= 0) { s->_volumeSlider.integerValue = v; [s showVolumeValue]; }
    }];

    [_content registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    __weak SoulXWindowController* weakDrop = self;
    _content.onDropFiles = ^(NSArray<NSURL*>* urls) { [weakDrop handleDroppedFiles:urls]; };
    _content.onDropActive = ^(BOOL active) {
        SoulXWindowController* s = weakDrop;
        if (s && active && s->_job == JOB_NONE && !s->_closing)
            [s message:SoulXText(@"Drop an APK to install, or a file to push to the guest") finderPath:nil];
    };
    [self updateStatus];
    [self updateNotice];
    [self fitAnimated:NO center:YES];
    [win makeFirstResponder:_screen];

    __weak SoulXWindowController* weakSelf = self;
    _tick = [NSTimer timerWithTimeInterval:1.0 repeats:YES block:^(NSTimer* t) {
        (void)t;
        [weakSelf tick];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];
    [self pollBoot];
    [self armHooks];
    return self;
}

// ---------------------------------------------------------------------------
// Building the views
// ---------------------------------------------------------------------------

- (NSButton*)toolButton:(NSString*)ident title:(NSString*)title symbol:(NSString*)symbol tip:(NSString*)tip
                 action:(SEL)action {
    SoulXToolRow* b = [[SoulXToolRow alloc] initWithFrame:NSZeroRect];
    b.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:title];
    b.title = title;
    b.target = self;
    b.action = action;
    b.bordered = NO;
    b.focusRingType = NSFocusRingTypeNone;
    b.toolTip = tip;
    b.identifier = ident;
    b.translatesAutoresizingMaskIntoConstraints = NO;
    [b.heightAnchor constraintEqualToConstant:ROW_HEIGHT].active = YES;
    [b.widthAnchor constraintEqualToConstant:TOOLBAR_WIDTH - 12].active = YES;
    _buttons[ident] = b;
    return b;
}

- (NSButton*)navTile:(NSString*)ident title:(NSString*)title symbol:(NSString*)symbol tip:(NSString*)tip
               action:(SEL)action {
    SoulXNavTile* b = [[SoulXNavTile alloc] initWithFrame:NSZeroRect];
    b.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:title];
    b.title = title;
    b.target = self;
    b.action = action;
    b.bordered = NO;
    b.focusRingType = NSFocusRingTypeNone;
    b.toolTip = tip;
    b.identifier = ident;
    b.translatesAutoresizingMaskIntoConstraints = NO;
    [b.heightAnchor constraintEqualToConstant:TILE_HEIGHT].active = YES;
    _buttons[ident] = b;
    return b;
}

// A small grey section heading in the toolbar ("Navigation", "Tools", "Volume").
- (NSTextField*)toolbarHeading:(NSString*)text {
    NSTextField* l = [NSTextField labelWithString:text];
    l.font = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    l.textColor = NSColor.secondaryLabelColor;
    return l;
}

// The volume slider's level as a percentage (the slider holds Android's 0...15 steps).
- (void)showVolumeValue {
    double max = _volumeSlider.maxValue > 0 ? _volumeSlider.maxValue : 15;
    _volumeValue.stringValue = [NSString stringWithFormat:@"%d%%", (int)lround(_volumeSlider.doubleValue * 100.0 / max)];
}

- (NSView*)buildToolbar {
    // A rounded card floating in the column right of the screen: Liquid Glass on macOS 26 (as the launcher),
    // else a faint plate (NSBox: its colours follow light / dark).
    NSRect cardFrame = NSMakeRect(0, 0, TOOLBAR_WIDTH - 2 * RAIL_INSET, 400);
    NSView* card;
    NSView* bar;
    if (@available(macOS 26.0, *)) {
        NSGlassEffectView* glass = [[NSGlassEffectView alloc] initWithFrame:cardFrame];
        glass.cornerRadius = 16;
        bar = [[NSView alloc] initWithFrame:cardFrame];
        glass.contentView = bar;
        card = glass;
    } else {
        NSBox* box = [[NSBox alloc] initWithFrame:cardFrame];
        box.boxType = NSBoxCustom;
        box.titlePosition = NSNoTitle;
        box.cornerRadius = 16;
        box.borderWidth = 1;
        box.fillColor = [NSColor.labelColor colorWithAlphaComponent:0.055];
        box.borderColor = [NSColor.labelColor colorWithAlphaComponent:0.09];
        box.contentViewMargins = NSZeroSize;
        bar = box.contentView;
        card = box;
    }

    // Navigation: four tiles, two by two.
    NSArray<NSButton*>* nav = @[
        [self navTile:@"back" title:SoulXText(@"Back") symbol:@"chevron.backward"
                  tip:SoulXText(@"Back (Esc or right-click)") action:@selector(soulxBack:)],
        [self navTile:@"home" title:SoulXText(@"Home") symbol:@"house"
                  tip:SoulXText(@"Home") action:@selector(soulxHome:)],
        [self navTile:@"recents" title:SoulXText(@"Recents") symbol:@"square.on.square"
                  tip:SoulXText(@"Recent apps") action:@selector(soulxRecents:)],
        [self navTile:@"rotate" title:SoulXText(@"Rotate") symbol:@"rotate.right"
                  tip:SoulXText(@"Rotate the screen") action:@selector(soulxRotate:)],
    ];
    NSGridView* navGrid = [NSGridView gridViewWithViews:@[ @[ nav[0], nav[1] ], @[ nav[2], nav[3] ] ]];
    navGrid.rowSpacing = 8;
    navGrid.columnSpacing = 8;
    navGrid.translatesAutoresizingMaskIntoConstraints = NO;
    CGFloat tileW = (RAIL_INNER - 8) / 2;
    for (NSButton* t in nav) [t.widthAnchor constraintEqualToConstant:tileW].active = YES;
    // Grouped tools: each category opens a small flyout to the RIGHT on hover (closes when the mouse leaves).
    // Sub-buttons are real SoulXToolRows in _buttons, so setJobButtons / setVideoButton still style them.
    NSPopover* appsPop = [self flyoutPopoverWith:@[
        [self toolButton:@"install" title:SoulXText(@"Install APK") symbol:@"square.and.arrow.down"
                     tip:SoulXText(@"Install an app, or drag one onto the window") action:@selector(soulxInstallApp:)],
        [self toolButton:@"keymap" title:SoulXText(@"Keymap") symbol:@"keyboard"
                     tip:SoulXText(@"Edit the game keymap") action:@selector(soulxEditKeymap:)],
    ]];
    NSPopover* scriptsPop = [self flyoutPopoverWith:@[
        [self toolButton:@"import" title:SoulXText(@"Import script") symbol:@"square.and.arrow.down"
                     tip:SoulXText(@"Import a script, or a whole script folder, into your scripts")
                  action:@selector(soulxImportScript:)],
        [self toolButton:@"record" title:SoulXText(@"Record script") symbol:@"record.circle"
                     tip:SoulXText(@"Record your taps as a Lua script") action:@selector(soulxRecord:)],
        [self toolButton:@"run" title:SoulXText(@"Run script") symbol:@"play.circle"
                     tip:SoulXText(@"Run a Lua script (the Stop button appears here while one runs)")
                  action:@selector(soulxRunScript:)],
        [self toolButton:@"showlog" title:SoulXText(@"Show Log") symbol:@"doc.plaintext"
                     tip:SoulXText(@"Show the script output") action:@selector(soulxShowLog:)],
        [self toolButton:@"inspect" title:SoulXText(@"Inspect") symbol:@"scope"
                     tip:SoulXText(@"Read x, y and colour; click to copy (⇧⌘I)") action:@selector(soulxInspect:)],
    ]];
    NSPopover* capturePop = [self flyoutPopoverWith:@[
        [self toolButton:@"shot" title:SoulXText(@"Screenshot") symbol:@"camera"
                     tip:SoulXText(@"Save a screenshot") action:@selector(soulxScreenshot:)],
        [self toolButton:@"video" title:SoulXText(@"Record video") symbol:@"video"
                     tip:SoulXText(@"Record the screen to a video") action:@selector(soulxRecordVideo:)],
    ]];

    SoulXToolRow* lastCat = [self categoryButton:SoulXText(@"Capture") symbol:@"camera.viewfinder" ident:@"cat.capture" popover:capturePop];
    NSArray* cats = @[
        [self categoryButton:SoulXText(@"Apps") symbol:@"square.grid.2x2" ident:@"cat.apps" popover:appsPop],
        [self categoryButton:SoulXText(@"Scripts") symbol:@"play.square" ident:@"cat.scripts" popover:scriptsPop],
        lastCat,
    ];

    NSBox* divider = [[NSBox alloc] initWithFrame:NSZeroRect];
    divider.boxType = NSBoxSeparator;
    divider.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField* navHead = [self toolbarHeading:SoulXText(@"Navigation")];
    NSTextField* toolsHead = [self toolbarHeading:SoulXText(@"Tools")];
    NSMutableArray<NSView*>* views = [@[ navHead, navGrid, divider, toolsHead ] mutableCopy];
    [views addObjectsFromArray:cats];

    _tools = [NSStackView stackViewWithViews:views];
    _tools.orientation = NSUserInterfaceLayoutOrientationVertical;
    _tools.alignment = NSLayoutAttributeLeading;
    _tools.spacing = 2;
    _tools.translatesAutoresizingMaskIntoConstraints = NO;
    [_tools setCustomSpacing:8 afterView:navHead];
    [_tools setCustomSpacing:14 afterView:navGrid];       // breathing room around the group divider
    [_tools setCustomSpacing:12 afterView:divider];
    [_tools setCustomSpacing:6 afterView:toolsHead];
    [bar addSubview:_tools];
    [divider.widthAnchor constraintEqualToConstant:RAIL_INNER].active = YES;
    [NSLayoutConstraint activateConstraints:@[
        [_tools.topAnchor constraintEqualToAnchor:bar.topAnchor constant:RAIL_PAD],
        [_tools.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:RAIL_PAD],
    ]];

    // Volume: a small card pinned to the bottom of the toolbar — heading and level, then speaker + slider.
    NSImageView* spk = [NSImageView imageViewWithImage:
        [NSImage imageWithSystemSymbolName:@"speaker.wave.2.fill" accessibilityDescription:SoulXText(@"Volume")]];
    spk.contentTintColor = NSColor.secondaryLabelColor;
    _volumeSlider = [NSSlider sliderWithValue:10 minValue:0 maxValue:15 target:self action:@selector(volumeChanged:)];
    _volumeSlider.controlSize = NSControlSizeSmall;
    _volumeSlider.trackFillColor = NSColor.controlAccentColor;   // the level as a blue bar
    _volumeSlider.continuous = YES;                     // set live while dragging (each set is instant)
    _volumeSlider.toolTip = SoulXText(@"Output volume");
    _volumeValue = [NSTextField labelWithString:@""];
    _volumeValue.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
    _volumeValue.textColor = NSColor.secondaryLabelColor;
    NSStackView* volHead = [NSStackView stackViewWithViews:@[ [self toolbarHeading:SoulXText(@"Volume")], _volumeValue ]];
    volHead.distribution = NSStackViewDistributionEqualCentering;
    NSStackView* volRow = [NSStackView stackViewWithViews:@[ spk, _volumeSlider ]];
    volRow.spacing = 6;
    NSStackView* volCard = [NSStackView stackViewWithViews:@[ volHead, volRow ]];
    volCard.orientation = NSUserInterfaceLayoutOrientationVertical;
    volCard.alignment = NSLayoutAttributeLeading;
    volCard.spacing = 8;
    volCard.edgeInsets = NSEdgeInsetsMake(10, 10, 10, 10);
    volCard.wantsLayer = YES;
    volCard.layer.cornerRadius = 11;
    volCard.layer.borderWidth = 1;
    volCard.translatesAutoresizingMaskIntoConstraints = NO;
    [self paintVolumeCard:volCard];
    [bar addSubview:volCard];
    CGFloat inner = RAIL_INNER - 20;
    [NSLayoutConstraint activateConstraints:@[
        [volCard.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:RAIL_PAD],
        [volCard.widthAnchor constraintEqualToConstant:RAIL_INNER],
        [volCard.bottomAnchor constraintEqualToAnchor:bar.bottomAnchor constant:-RAIL_PAD],
        [volHead.widthAnchor constraintEqualToConstant:inner],
        [_volumeSlider.widthAnchor constraintEqualToConstant:inner - 26],
    ]];
    _volumeCard = volCard;
    [self showVolumeValue];
    return card;
}

// The card's plate follows light / dark: layer colours do not, so set them from the current appearance.
- (void)paintVolumeCard:(NSView*)card {
    [card.effectiveAppearance performAsCurrentDrawingAppearance:^{
        card.layer.backgroundColor = [NSColor.labelColor colorWithAlphaComponent:0.05].CGColor;
        card.layer.borderColor = [NSColor.labelColor colorWithAlphaComponent:0.08].CGColor;
    }];
}

- (NSPopover*)flyoutPopoverWith:(NSArray<NSView*>*)buttons {
    NSStackView* stack = [NSStackView stackViewWithViews:buttons];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 2;
    stack.edgeInsets = NSEdgeInsetsMake(6, 6, 6, 6);
    NSViewController* vc = [[NSViewController alloc] init];
    vc.view = stack;
    NSPopover* pop = [[NSPopover alloc] init];
    pop.contentViewController = vc;
    pop.behavior = NSPopoverBehaviorTransient;
    pop.animates = NO;
    [stack layoutSubtreeIfNeeded];
    CGFloat h = stack.fittingSize.height;               // rows may vary in height (separators, empty-state text)
    if (h < ROW_HEIGHT) h = (CGFloat)buttons.count * (ROW_HEIGHT + 2) + 12;
    pop.contentSize = NSMakeSize(TOOLBAR_WIDTH, h);
    return pop;
}

- (SoulXToolRow*)categoryButton:(NSString*)title symbol:(NSString*)symbol ident:(NSString*)ident popover:(NSPopover*)pop {
    SoulXToolRow* b = [[SoulXToolRow alloc] initWithFrame:NSZeroRect];
    b.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:title];
    b.title = title;
    b.bordered = NO;
    b.focusRingType = NSFocusRingTypeNone;
    b.hasFlyout = YES;
    b.identifier = ident;
    b.target = self;
    b.action = @selector(categoryClicked:);
    b.translatesAutoresizingMaskIntoConstraints = NO;
    [b.heightAnchor constraintEqualToConstant:ROW_HEIGHT].active = YES;
    [b.widthAnchor constraintEqualToConstant:RAIL_INNER].active = YES;
    if (!_flyoutPopovers) _flyoutPopovers = [NSMutableDictionary dictionary];
    _flyoutPopovers[ident] = pop;
    if (!_categoryButtons) _categoryButtons = [NSMutableDictionary dictionary];
    _categoryButtons[ident] = b;
    __weak SoulXWindowController* weakSelf = self;
    b.onHover = ^(SoulXToolRow* btn) { [weakSelf showFlyoutPopover:pop from:btn]; };
    return b;
}

- (void)categoryClicked:(id)sender {
    if (![sender isKindOfClass:[SoulXToolRow class]]) return;
    SoulXToolRow* b = sender;
    [self showFlyoutPopover:_flyoutPopovers[b.identifier] from:b];
}

// Shows a category flyout to the RIGHT of its row, then polls the mouse to close it when it leaves.
- (void)showFlyoutPopover:(NSPopover*)pop from:(SoulXToolRow*)btn {
    if (!pop || _closing || !btn.window) return;
    if (_flyout == pop && _flyout.shown) return;
    [self closeFlyout];
    _flyout = pop;
    _flyoutOwner = btn;
    [pop showRelativeToRect:btn.bounds ofView:btn preferredEdge:NSRectEdgeMaxX];
    __weak SoulXWindowController* weakSelf = self;
    _flyoutTimer = [NSTimer scheduledTimerWithTimeInterval:0.12 repeats:YES block:^(NSTimer* t) {
        (void)t;
        [weakSelf checkFlyoutHover];
    }];
}

- (void)closeFlyout {
    [_flyoutTimer invalidate];
    _flyoutTimer = nil;
    [_flyout close];
    _flyout = nil;
    _flyoutOwner = nil;
}

- (void)checkFlyoutHover {
    if (!_flyout || !_flyoutOwner || !_flyout.shown) { [self closeFlyout]; return; }
    NSPoint m = [NSEvent mouseLocation];                    // screen coords
    NSRect br = [_flyoutOwner.window convertRectToScreen:[_flyoutOwner convertRect:_flyoutOwner.bounds toView:nil]];
    br = NSInsetRect(br, -12, -6);                          // bridge the gap between row and popover
    NSWindow* pw = _flyout.contentViewController.view.window;
    NSRect pr = pw ? NSInsetRect(pw.frame, -8, -8) : NSZeroRect;
    if (!NSPointInRect(m, br) && !NSPointInRect(m, pr)) [self closeFlyout];
}

// The collapse toggle lives in the title bar (the standard sidebar-toggle spot), not in the sidebar itself.
- (void)installTitlebarToggle {
    NSImage* icon = [NSImage imageWithSystemSymbolName:@"sidebar.right" accessibilityDescription:SoulXText(@"Hide toolbar")];
    NSButton* b = icon ? [NSButton buttonWithImage:icon target:self action:@selector(soulxToggleToolbar:)]
                       : [NSButton buttonWithTitle:@"▐" target:self action:@selector(soulxToggleToolbar:)];
    b.bordered = NO;
    b.focusRingType = NSFocusRingTypeNone;
    b.imageScaling = NSImageScaleProportionallyDown;
    b.toolTip = SoulXText(@"Hide toolbar");
    _collapseButton = b;

    // The accessory gets a fixed-size container, never the button itself: when the window moves to another
    // display the title bar lays out again, the button's frame changed with it, and AppKit threw from
    // -[NSTitlebarAccessoryViewController _auxiliaryViewFrameChanged:] (an uncaught exception: SoulX crashed).
    NSView* box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 30, 22)];
    b.translatesAutoresizingMaskIntoConstraints = NO;
    [box addSubview:b];
    [NSLayoutConstraint activateConstraints:@[
        [b.centerXAnchor constraintEqualToAnchor:box.centerXAnchor],
        [b.centerYAnchor constraintEqualToAnchor:box.centerYAnchor],
        [b.widthAnchor constraintEqualToConstant:30],
        [b.heightAnchor constraintEqualToConstant:22],
    ]];

    NSTitlebarAccessoryViewController* vc = [[NSTitlebarAccessoryViewController alloc] init];
    vc.view = box;
    vc.layoutAttribute = NSLayoutAttributeRight;
    [self.window addTitlebarAccessoryViewController:vc];
}

// The title, centred over the whole window: the instance name in bold, the Android version after it in grey.
- (void)installCenteredTitle {
    NSWindow* win = self.window;
    NSView* titlebar = [win standardWindowButton:NSWindowCloseButton].superview;
    if (!titlebar) return;
    win.titleVisibility = NSWindowTitleHidden;            // the real title stays for the Window menu / Mission Control
    NSMutableAttributedString* t = [[NSMutableAttributedString alloc]
        initWithString:@(_conf.name)
            attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold],
                          NSForegroundColorAttributeName: NSColor.labelColor }];
    [t appendAttributedString:[[NSAttributedString alloc]
        initWithString:[@"  " stringByAppendingString:_android]
            attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:13],
                          NSForegroundColorAttributeName: NSColor.secondaryLabelColor }]];
    NSTextField* label = [NSTextField labelWithAttributedString:t];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [titlebar addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.centerXAnchor constraintEqualToAnchor:titlebar.centerXAnchor],
        [label.centerYAnchor constraintEqualToAnchor:[win standardWindowButton:NSWindowCloseButton].centerYAnchor],
    ]];
}

// Collapse or expand the right-hand toolbar: set the state and the chevron, but do not lay out (callers do).
- (void)applyToolbarCollapsed:(BOOL)collapsed {
    _content.toolbarCollapsed = collapsed;
    _tools.hidden = collapsed;
    _volumeCard.hidden = collapsed;
    NSImage* img = [NSImage imageWithSystemSymbolName:(collapsed ? @"sidebar.left" : @"sidebar.right")
                                accessibilityDescription:nil];
    if (img) _collapseButton.image = img;
    _collapseButton.toolTip = collapsed ? SoulXText(@"Show toolbar") : SoulXText(@"Hide toolbar");
}

- (void)soulxToggleToolbar:(id)sender {
    (void)sender;
    BOOL collapsed = !_content.toolbarCollapsed;
    [[NSUserDefaults standardUserDefaults] setBool:collapsed forKey:@"SoulXToolbarCollapsed"];
    NSWindow* win = self.window;
    if (win.styleMask & NSWindowStyleMaskFullScreen) {       // can't resize in full screen: just relayout
        [self applyToolbarCollapsed:collapsed];
        [_content setNeedsLayout:YES];
        return;
    }
    // Resize the window by the toolbar delta so the game keeps its size (no black gap), and animate it.
    CGFloat h = _screen.frame.size.height;
    _content.fixedScreenWidth = _screen.frame.size.width;    // hold the game width steady through the animation
    [self applyToolbarCollapsed:collapsed];                  // toolbarWidth now returns the new value
    NSSize cs = [self contentSizeForScreenHeight:h];
    NSRect target = [win frameRectForContentRect:NSMakeRect(0, 0, cs.width, cs.height)];
    NSRect old = win.frame;
    target.origin.x = old.origin.x;                          // keep the top-left corner fixed
    target.origin.y = NSMaxY(old) - target.size.height;
    NSScreen* scr = win.screen ? win.screen : NSScreen.mainScreen;
    NSRect vf = scr ? scr.visibleFrame : old;
    if (NSMaxX(target) > NSMaxX(vf)) target.origin.x = NSMaxX(vf) - target.size.width;
    if (target.origin.x < vf.origin.x) target.origin.x = vf.origin.x;

    __weak SoulXWindowController* weak = self;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
        ctx.duration = 0.2;
        [win.animator setFrame:target display:YES];
    } completionHandler:^{
        SoulXWindowController* s = weak;
        if (!s) return;
        s->_content.fixedScreenWidth = 0;                    // back to normal (toolbar fixed, screen fills the rest)
        [s->_content setNeedsLayout:YES];
        [s->_content layoutSubtreeIfNeeded];
    }];
}

- (void)soulxShowLog:(id)sender {
    (void)sender;
    [self showLogPanel];
}

- (NSTextField*)statusLabel {
    NSTextField* l = [NSTextField labelWithString:@""];
    l.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
    l.textColor = NSColor.secondaryLabelColor;
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    return l;
}

- (NSView*)buildStatusBar {
    NSView* bar = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 400, STATUS_HEIGHT)];   // on the window's own background
    _fpsLabel = [self statusLabel];
    _serialLabel = [self statusLabel];
    _rotationLabel = [self statusLabel];
    _jobLabel = [self statusLabel];
    _keymapLabel = [self statusLabel];
    _messageLabel = [self statusLabel];
    _messageLabel.textColor = NSColor.labelColor;
    [_messageLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                            forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_keymapLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow + 1
                                           forOrientation:NSLayoutConstraintOrientationHorizontal];
    _finderButton = [NSButton buttonWithTitle:SoulXText(@"Show in Finder") target:self action:@selector(showInFinder:)];
    _finderButton.bordered = NO;
    _finderButton.font = [NSFont systemFontOfSize:11];
    _finderButton.contentTintColor = NSColor.linkColor;
    _finderButton.attributedTitle = [[NSAttributedString alloc] initWithString:SoulXText(@"Show in Finder")
        attributes:@{ NSForegroundColorAttributeName: NSColor.linkColor, NSFontAttributeName: _finderButton.font }];
    _finderButton.hidden = YES;
    NSStackView* row = [NSStackView stackViewWithViews:@[ _fpsLabel, _serialLabel, _rotationLabel, _jobLabel,
                                                          _keymapLabel, _messageLabel, _finderButton ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing = 12;
    row.edgeInsets = NSEdgeInsetsMake(0, 10, 0, 10);
    row.detachesHiddenViews = YES;
    row.translatesAutoresizingMaskIntoConstraints = NO;
    [bar addSubview:row];
    _specLabel = [self statusLabel];                    // "1080p · Portrait", at the right end
    _specLabel.translatesAutoresizingMaskIntoConstraints = NO;
    [bar addSubview:_specLabel];
    [NSLayoutConstraint activateConstraints:@[
        [row.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:SCREEN_INSET - 10 + 8],
        [row.trailingAnchor constraintLessThanOrEqualToAnchor:_specLabel.leadingAnchor constant:-8],
        [row.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor],
        [_specLabel.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor constant:-(SCREEN_INSET + 8)],
        [_specLabel.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor],
    ]];
    return bar;
}

- (NSView*)buildNotice {
    NSView* box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 64)];
    box.wantsLayer = YES;
    box.layer.backgroundColor = [NSColor colorWithWhite:0.08 alpha:0.86].CGColor;
    box.layer.cornerRadius = 12;
    _noticeLabel = [NSTextField labelWithString:@""];
    _noticeLabel.font = [NSFont systemFontOfSize:15 weight:NSFontWeightMedium];
    _noticeLabel.textColor = NSColor.whiteColor;
    _noticeLabel.alignment = NSTextAlignmentCenter;
    _noticeSpinner = [[NSProgressIndicator alloc] init];
    _noticeSpinner.style = NSProgressIndicatorStyleSpinning;
    _noticeSpinner.controlSize = NSControlSizeSmall;
    _noticeSpinner.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    NSStackView* row = [NSStackView stackViewWithViews:@[ _noticeSpinner, _noticeLabel ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing = 10;
    row.translatesAutoresizingMaskIntoConstraints = NO;
    [box addSubview:row];
    [NSLayoutConstraint activateConstraints:@[
        [row.centerXAnchor constraintEqualToAnchor:box.centerXAnchor],
        [row.centerYAnchor constraintEqualToAnchor:box.centerYAnchor],
        [row.leadingAnchor constraintGreaterThanOrEqualToAnchor:box.leadingAnchor constant:16],
    ]];
    _notice = box;
    return box;
}

// ---------------------------------------------------------------------------
// Window size: the screen area keeps the logical aspect ratio
// ---------------------------------------------------------------------------

- (CGFloat)toolbarWidth {
    return SCREEN_INSET + (_content.toolbarCollapsed ? COLLAPSED_WIDTH : TOOLBAR_WIDTH);   // all width that is not the screen
}

- (NSSize)contentSizeForScreenHeight:(CGFloat)h {
    NSSize ls = _screen.logicalSize;
    if (ls.width < 1 || ls.height < 1) ls = NSMakeSize(9, 16);
    return NSMakeSize(round(h * ls.width / ls.height) + [self toolbarWidth], round(h) + VERT_CHROME);
}

// Resizes the window so that its screen area is h points high (shrunk to fit the visible frame); keeps the
// top-left corner, or centres the window.
- (void)setScreenHeight:(CGFloat)h animate:(BOOL)animate center:(BOOL)center {
    NSWindow* win = self.window;
    NSScreen* scr = win.screen ? win.screen : NSScreen.mainScreen;
    NSRect vf = scr ? scr.visibleFrame : NSMakeRect(0, 0, 1440, 900);
    if (h < MIN_SCREEN_SIDE) h = MIN_SCREEN_SIDE;
    NSSize cs = [self contentSizeForScreenHeight:h];
    NSRect fr = [win frameRectForContentRect:NSMakeRect(0, 0, cs.width, cs.height)];
    double fit = fmin(1.0, fmin(vf.size.width / fr.size.width, vf.size.height / fr.size.height));
    if (fit < 1.0) {
        CGFloat chrome = fr.size.height - cs.height + VERT_CHROME;
        CGFloat avail_h = vf.size.height - chrome;
        CGFloat avail_w = vf.size.width - (fr.size.width - cs.width) - [self toolbarWidth];
        NSSize ls = _screen.logicalSize;
        double by_w = ls.width > 0 ? avail_w * ls.height / ls.width : avail_h;
        h = fmax(MIN_SCREEN_SIDE, fmin(avail_h, by_w));
        cs = [self contentSizeForScreenHeight:h];
        fr = [win frameRectForContentRect:NSMakeRect(0, 0, cs.width, cs.height)];
    }
    if (center) {
        fr.origin.x = round(NSMidX(vf) - fr.size.width / 2);
        fr.origin.y = round(NSMidY(vf) - fr.size.height / 2);
    } else {
        NSRect old = win.frame;
        fr.origin.x = old.origin.x;
        fr.origin.y = NSMaxY(old) - fr.size.height;
        if (NSMaxX(fr) > NSMaxX(vf)) fr.origin.x = NSMaxX(vf) - fr.size.width;
        if (fr.origin.x < vf.origin.x) fr.origin.x = vf.origin.x;
        if (fr.origin.y < vf.origin.y) fr.origin.y = vf.origin.y;
        if (NSMaxY(fr) > NSMaxY(vf)) fr.origin.y = NSMaxY(vf) - fr.size.height;
    }
    [win setFrame:fr display:YES animate:animate];
}

- (void)fitAnimated:(BOOL)animate center:(BOOL)center {
    NSWindow* win = self.window;
    NSScreen* scr = win.screen ? win.screen : NSScreen.mainScreen;
    NSRect vf = scr ? scr.visibleFrame : NSMakeRect(0, 0, 1440, 900);
    NSRect probe = [win frameRectForContentRect:NSMakeRect(0, 0, 100, 100)];
    CGFloat chrome = probe.size.height - 100 + VERT_CHROME;
    [self setScreenHeight:vf.size.height * INITIAL_HEIGHT_SHARE - chrome animate:animate center:center];
}

- (NSSize)windowWillResize:(NSWindow*)win toSize:(NSSize)frameSize {
    if (win.styleMask & NSWindowStyleMaskFullScreen) return frameSize;
    NSSize ls = _screen.logicalSize;
    if (ls.width < 1 || ls.height < 1) return frameSize;
    CGFloat tw = [self toolbarWidth];
    NSRect want = [win contentRectForFrameRect:NSMakeRect(0, 0, frameSize.width, frameSize.height)];
    NSRect cur = [win contentRectForFrameRect:win.frame];
    CGFloat sw = want.size.width - tw, sh = want.size.height - VERT_CHROME;
    if (fabs(want.size.width - cur.size.width) > fabs(want.size.height - cur.size.height)) {
        sh = sw * ls.height / ls.width;
    } else {
        sw = sh * ls.width / ls.height;
    }
    if (sw < MIN_SCREEN_SIDE || sh < MIN_SCREEN_SIDE) {
        double k = fmax(MIN_SCREEN_SIDE / fmax(sw, 1), MIN_SCREEN_SIDE / fmax(sh, 1));
        sw *= k;
        sh *= k;
    }
    NSRect fr = [win frameRectForContentRect:NSMakeRect(0, 0, round(sw) + tw, round(sh) + VERT_CHROME)];
    return fr.size;
}

// ---------------------------------------------------------------------------
// Status bar and notices
// ---------------------------------------------------------------------------

- (void)updateStatus {
    static const char* const degrees[4] = { "0°", "90°", "180°", "270°" };
    int r = _screen.rotation;
    _serialLabel.stringValue = @(_serial);
    // rotation: a small rotate glyph before the angle
    NSMutableAttributedString* rot = [[NSMutableAttributedString alloc] init];
    NSImage* glyph = [NSImage imageWithSystemSymbolName:@"rotate.right" accessibilityDescription:nil];
    if (glyph) {
        NSTextAttachment* att = [[NSTextAttachment alloc] init];
        att.image = [glyph imageWithSymbolConfiguration:
                     [NSImageSymbolConfiguration configurationWithPointSize:10 weight:NSFontWeightRegular]];
        [rot appendAttributedString:[NSAttributedString attributedStringWithAttachment:att]];
        [rot appendAttributedString:[[NSAttributedString alloc] initWithString:@" "]];
    }
    [rot appendAttributedString:[[NSAttributedString alloc] initWithString:@(degrees[r & 3])]];
    [rot addAttributes:@{ NSFontAttributeName: _rotationLabel.font, NSForegroundColorAttributeName: NSColor.secondaryLabelColor }
                 range:NSMakeRange(0, rot.length)];
    _rotationLabel.attributedStringValue = rot;
    int shortSide = _conf.width < _conf.height ? _conf.width : _conf.height;
    BOOL wide = _conf.landscape != ((r & 1) != 0);
    _specLabel.stringValue = [NSString stringWithFormat:@"%dp · %@", shortSide,
                              wide ? SoulXText(@"Landscape") : SoulXText(@"Portrait")];
    if (_job == JOB_RECORD) {
        _jobLabel.stringValue = [NSString stringWithFormat:SoulXText(@"● Recording %@"),
                                                           elapsed_text((soulx_now_ms() - _jobStartMs) / 1000.0)];
        _jobLabel.textColor = NSColor.systemRedColor;
    } else if (_job == JOB_RUN) {
        _jobLabel.stringValue = [NSString stringWithFormat:SoulXText(@"▶ Script %@"),
                                                           elapsed_text((soulx_now_ms() - _jobStartMs) / 1000.0)];
        _jobLabel.textColor = NSColor.systemGreenColor;
    } else {
        _jobLabel.stringValue = _screen.slowMode ? SoulXText(@"Slow display mode") : @"";
        _jobLabel.textColor = NSColor.systemOrangeColor;
    }
    _jobLabel.hidden = _jobLabel.stringValue.length == 0;
    _keymapLabel.hidden = _keymapLabel.stringValue.length == 0;
}

- (void)message:(NSString*)text finderPath:(NSString*)path {
    _messageLabel.stringValue = text ? text : @"";
    _messageLabel.toolTip = text;
    _finderPath = [path copy];
    _finderButton.hidden = path == nil;
}

- (void)showInFinder:(id)sender {
    (void)sender;
    if (_finderPath) [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:_finderPath] ]];
}

- (void)updateNotice {
    NSString* text = nil;
    if (_stopping) {
        text = [NSString stringWithFormat:SoulXText(@"Shutting down… %lds"),
                                          (long)((soulx_now_ms() - _stopStartMs) / 1000.0)];
    } else if (_closing) {
        text = nil;
    } else if (!_booted) {
        double secs = _engineStarted > 0 ? difftime(time(NULL), _engineStarted) : (soulx_now_ms() - _openedMs) / 1000.0;
        text = [NSString stringWithFormat:SoulXText(@"Booting %@… %lds"), _android, (long)fmax(0, secs)];
    } else if (!_screen.hasFrame) {
        text = SoulXText(@"Connecting to the screen…");
    }
    _notice.hidden = text == nil;
    if (text) {
        _noticeLabel.stringValue = text;
        [_noticeSpinner startAnimation:nil];
        [_content place];
    } else {
        [_noticeSpinner stopAnimation:nil];
    }
}

- (void)tick {
    if (_finished) return;
    [self updateStatus];
    [self updateNotice];
    // the volume can change elsewhere (the launcher, a game, Android's own buttons): follow it every 3 s, but
    // never while the user is dragging the slider or right after
    if (++_volumeTicks % 3 == 0 && !(NSEvent.pressedMouseButtons & 1) && soulx_now_ms() - _volumeTouchedMs > 2000) {
        __weak SoulXWindowController* weakVol = self;
        [_screen readMusicVolumeThen:^(int v) {
            SoulXWindowController* s = weakVol;
            if (s && v >= 0 && soulx_now_ms() - s->_volumeTouchedMs > 2000) { s->_volumeSlider.integerValue = v; [s showVolumeValue]; }
        }];
    }
    if (_job == JOB_RECORD) {
        _buttons[@"record"].title = elapsed_text((soulx_now_ms() - _jobStartMs) / 1000.0);
    }
    // Fallback: no shared-memory frames long after the boot -> console screenshots
    if (_booted && !_screen.hasFrame && !_screen.slowMode && !_closing) {
        double wait = _bootedBeforeWindow ? SLOW_MODE_BOOTED_S : SLOW_MODE_AFTER_BOOT_S;
        if ((soulx_now_ms() - _bootedMs) / 1000.0 >= wait) {
            [_screen startSlowMode];
            [self message:SoulXText(@"Slow display mode: the engine shares no frames.") finderPath:nil];
        }
    }
}

// Polls sys.boot_completed on a background queue until the guest has booted.
- (void)pollBoot {
    NSString* serial = @(_serial);              // blocks cannot capture a C array
    __weak SoulXWindowController* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        char value[16] = "";
        bool booted = soulx_adb_getprop(serial.UTF8String, "sys.boot_completed", value, sizeof(value)) == 0 &&
                      strcmp(value, "1") == 0;
        dispatch_async(dispatch_get_main_queue(), ^{
            SoulXWindowController* s = weakSelf;
            if (!s || s->_closing) return;
            if (booted) {
                s->_booted = YES;
                s->_bootedBeforeWindow = !s->_bootKnown;
                s->_bootedMs = soulx_now_ms();
                [s updateNotice];
                return;
            }
            s->_bootKnown = YES;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                [weakSelf pollBoot];
            });
        });
    });
}

// ---------------------------------------------------------------------------
// SoulXScreenViewOwner
// ---------------------------------------------------------------------------

- (void)screenViewDidShowFirstFrame:(SoulXScreenView*)view {
    (void)view;
    [self updateNotice];
    [self startHooks];
}

- (void)screenViewGeometryDidChange:(SoulXScreenView*)view {
    [self updateStatus];
    NSSize ls = view.logicalSize;
    int aspect = ls.width > ls.height ? 1 : 0;
    if (_lastAspect >= 0 && aspect != _lastAspect && !(self.window.styleMask & NSWindowStyleMaskFullScreen)) {
        // orientation flipped: same screen height, new width
        NSRect cr = [self.window contentRectForFrameRect:self.window.frame];
        [self setScreenHeight:cr.size.height - VERT_CHROME animate:YES center:NO];
    }
    _lastAspect = aspect;
}

- (void)screenView:(SoulXScreenView*)view fps:(double)fps {
    (void)view;
    // Android sends a frame only when the screen changes: a still screen is 0 fps and looked frozen. Show that the
    // connection is live, and the frame rate only while the picture moves.
    NSFont* font = _fpsLabel.font;
    NSMutableAttributedString* s = [[NSMutableAttributedString alloc] initWithString:@"● "
        attributes:@{ NSForegroundColorAttributeName: NSColor.systemGreenColor, NSFontAttributeName: font }];
    NSString* rest = fps >= 1 ? [NSString stringWithFormat:@"%@ · %.0f fps", SoulXText(@"Live"), fps] : SoulXText(@"Live");
    NSColor* color = _fpsLabel.textColor ? _fpsLabel.textColor : NSColor.secondaryLabelColor;
    [s appendAttributedString:[[NSAttributedString alloc] initWithString:rest
        attributes:@{ NSForegroundColorAttributeName: color, NSFontAttributeName: font }]];
    _fpsLabel.attributedStringValue = s;
    _fpsLabel.toolTip = SoulXText(@"Connected. Android only draws when something changes, so a still screen shows no frame rate.");
}

- (void)screenView:(SoulXScreenView*)view message:(NSString*)text {
    (void)view;
    [self message:text finderPath:nil];
}

- (void)screenView:(SoulXScreenView*)view keymapStatus:(NSString*)text {
    (void)view;
    _keymapLabel.stringValue = text ? text : @"";
    _keymapLabel.hidden = _keymapLabel.stringValue.length == 0;
}

- (void)screenViewEditingDidChange:(SoulXScreenView*)view {
    NSButton* b = _buttons[@"keymap"];
    b.contentTintColor = view.keymapEditing ? NSColor.controlAccentColor : nil;
    b.title = view.keymapEditing ? SoulXText(@"Editing…") : SoulXText(@"Keymap");
}

// ---------------------------------------------------------------------------
// Device actions
// ---------------------------------------------------------------------------

- (BOOL)deviceUsable {
    return !_closing && !_screen.keymapEditing;
}

- (void)soulxBack:(id)sender {
    (void)sender;
    if (_screen.keymapEditing) {
        [_screen.overlay handleEscape];
        return;
    }
    if ([self deviceUsable]) [_screen androidKey:"BACK"];
}

- (void)soulxHome:(id)sender {
    (void)sender;
    if ([self deviceUsable]) [_screen androidKey:"HOME"];
}

- (void)soulxRecents:(id)sender {
    (void)sender;
    if ([self deviceUsable]) [_screen androidKey:"APP_SWITCH"];
}

- (void)soulxRotate:(id)sender {
    (void)sender;
    if ([self deviceUsable]) [_screen toggleRotation];
}

- (void)soulxEditKeymap:(id)sender {
    (void)sender;
    if (_closing) return;
    [_screen setKeymapEditing:!_screen.keymapEditing];
}

- (void)soulxShowBindings:(id)sender {
    (void)sender;
    _screen.showBindings = !_screen.showBindings;
}

- (void)soulxInspect:(id)sender {
    (void)sender;
    if (_closing) return;
    [_screen setInspecting:!_screen.inspecting];
    _buttons[@"inspect"].contentTintColor = _screen.inspecting ? NSColor.controlAccentColor : nil;   // lit while on
    [self message:_screen.inspecting ? SoulXText(@"Inspect: hover to read x, y and colour; click to copy")
                                     : SoulXText(@"Inspect off") finderPath:nil];
}

- (void)soulxActualSize:(id)sender {
    (void)sender;
    if (self.window.styleMask & NSWindowStyleMaskFullScreen) return;
    CGFloat scale = self.window.backingScaleFactor > 0 ? self.window.backingScaleFactor : 1.0;
    [self setScreenHeight:_screen.logicalSize.height / scale animate:YES center:NO];
}

- (void)soulxFit:(id)sender {
    (void)sender;
    if (self.window.styleMask & NSWindowStyleMaskFullScreen) return;
    [self fitAnimated:YES center:NO];
}

// ---------------------------------------------------------------------------
// Screenshot
// ---------------------------------------------------------------------------

- (void)soulxScreenshot:(id)sender {
    (void)sender;
    if (_closing) return;
    __weak SoulXWindowController* weakSelf = self;
    [self captureScreenshotThen:^(NSString* path) {
        SoulXWindowController* s = weakSelf;
        if (!s) return;
        if (!path) {
            [s message:SoulXText(@"The screenshot could not be saved.") finderPath:nil];
            return;
        }
        [s flash];
        [s message:[NSString stringWithFormat:SoulXText(@"Saved %@"), path.stringByAbbreviatingWithTildeInPath]
         finderPath:path];
    }];
}

// Captures the upright frame and saves it to ~/Pictures/SoulX; done(path) on the main thread, path = nil on
// failure (no frame yet, or the file could not be written). Shared by the toolbar action and --test-screenshot.
- (void)captureScreenshotThen:(void (^)(NSString* path))done {
    NSString* name = @(_conf.name);
    [_screen captureUpright:^(SoulXImage* img) {
        if (!img) {
            done(nil);
            return;
        }
        NSString* stamp = timestamp_now();
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSString* dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Pictures/SoulX"];
            NSError* err = nil;
            NSString* path = nil;
            if ([[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                           attributes:nil error:&err]) {
                for (int n = 1; n < 100 && !path; n++) {
                    NSString* file = n == 1 ? [NSString stringWithFormat:@"%@-%@.png", name, stamp]
                                            : [NSString stringWithFormat:@"%@-%@-%d.png", name, stamp, n];
                    NSString* p = [dir stringByAppendingPathComponent:file];
                    if (![[NSFileManager defaultManager] fileExistsAtPath:p]) path = p;
                }
            }
            BOOL ok = path && soulx_image_save_png(path.fileSystemRepresentation, img) == 0;
            if (!ok) {
                soulx_log(SOULX_LOG_ERROR, "cannot save the screenshot%s%s: %s", path ? " " : "",
                         path ? path.UTF8String : "", path ? soulx_image_error() : err.localizedDescription.UTF8String);
            }
            soulx_image_free(img);
            free(img);
            dispatch_async(dispatch_get_main_queue(), ^{
                done(ok ? path : nil);
            });
        });
    }];
}

- (void)flash {
    NSView* f = _content.flash;
    f.alphaValue = 0.7;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
        ctx.duration = 0.35;
        f.animator.alphaValue = 0;
    } completionHandler:nil];
}

// ---------------------------------------------------------------------------
// Record / Run
// ---------------------------------------------------------------------------

- (void)appendLog:(NSString*)text {
    if (text.length == 0) return;
    if (!_logPanel) [self buildLogPanel];
    NSTextStorage* ts = _logView.textStorage;
    NSClipView* clip = _logView.enclosingScrollView.contentView;
    BOOL atBottom = NSMaxY(clip.documentVisibleRect) >= NSMaxY(_logView.bounds) - 24;
    NSDictionary* attrs = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
                             NSForegroundColorAttributeName: NSColor.textColor };
    [ts appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:attrs]];
    if (ts.length > LOG_MAX_CHARS) {
        NSUInteger cut = ts.length - LOG_KEEP_CHARS;
        NSRange nl = [ts.string rangeOfString:@"\n" options:0 range:NSMakeRange(cut, ts.length - cut)];
        if (nl.location != NSNotFound) cut = nl.location + 1;
        [ts deleteCharactersInRange:NSMakeRange(0, cut)];
    }
    if (atBottom) [_logView scrollRangeToVisible:NSMakeRange(ts.length, 0)];
}

- (void)buildLogPanel {
    NSRect wf = self.window.frame;
    NSRect frame = NSMakeRect(NSMaxX(wf) + 8, NSMaxY(wf) - 420, 640, 400);
    _logPanel = [[NSPanel alloc] initWithContentRect:frame
                                           styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                     NSWindowStyleMaskResizable | NSWindowStyleMaskUtilityWindow
                                             backing:NSBackingStoreBuffered
                                               defer:YES];
    _logPanel.title = [NSString stringWithFormat:SoulXText(@"Script output · %s"), _conf.name];
    _logPanel.hidesOnDeactivate = NO;
    _logPanel.releasedWhenClosed = NO;
    _logPanel.floatingPanel = NO;
    NSScrollView* sv = [[NSScrollView alloc] initWithFrame:_logPanel.contentView.bounds];
    sv.hasVerticalScroller = YES;
    sv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    NSSize cs = sv.contentSize;
    _logView = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, cs.width, cs.height)];
    _logView.editable = NO;
    _logView.richText = NO;
    _logView.verticallyResizable = YES;
    _logView.horizontallyResizable = NO;
    _logView.autoresizingMask = NSViewWidthSizable;
    _logView.textContainer.widthTracksTextView = YES;
    _logView.minSize = NSMakeSize(0, cs.height);
    _logView.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _logView.textContainerInset = NSMakeSize(4, 4);
    sv.documentView = _logView;
    _logPanel.contentView = sv;
}

- (void)showLogPanel {
    if (!_logPanel) [self buildLogPanel];
    [_logPanel orderFront:nil];
}

- (void)setJobButtons {
    NSButton* rec = _buttons[@"record"];
    NSButton* run = _buttons[@"run"];
    if (_job == JOB_RECORD) {
        rec.image = [NSImage imageWithSystemSymbolName:@"stop.circle.fill" accessibilityDescription:nil];
        rec.contentTintColor = NSColor.systemRedColor;
        rec.title = elapsed_text((soulx_now_ms() - _jobStartMs) / 1000.0);
        rec.toolTip = SoulXText(@"Stop recording (⌘R)");
    } else {
        rec.image = [NSImage imageWithSystemSymbolName:@"record.circle" accessibilityDescription:nil];
        rec.contentTintColor = nil;
        rec.title = SoulXText(@"Record script");
        rec.toolTip = SoulXText(@"Record your taps as a Lua script (⌘R)");
    }
    if (_job == JOB_RUN) {
        run.image = [NSImage imageWithSystemSymbolName:@"stop.circle" accessibilityDescription:nil];
        run.contentTintColor = NSColor.systemOrangeColor;
        run.title = SoulXText(@"Stop");
        run.toolTip = SoulXText(@"Stop the script");
    } else {
        run.image = [NSImage imageWithSystemSymbolName:@"play.circle" accessibilityDescription:nil];
        run.contentTintColor = nil;
        run.title = SoulXText(@"Run script");
        run.toolTip = SoulXText(@"Run a Lua script (⌘O)");
    }
    rec.enabled = !_closing && (_job == JOB_NONE || _job == JOB_RECORD);   // also disabled while installing
    run.enabled = !_closing && (_job == JOB_NONE || _job == JOB_RUN);
    [self updateStatus];
}

// Starts 'soulx <args...>' as the record / run child. NO when it could not be started.
- (BOOL)startJob:(JobKind)kind args:(NSArray<NSString*>*)args path:(NSString*)path {
    char exe[PATH_MAX];
    if (!self_executable(exe, sizeof(exe))) {
        [self message:SoulXText(@"Cannot find the soulx program.") finderPath:nil];
        return NO;
    }
    const char* argv[16];
    size_t n = 0;
    argv[n++] = exe;
    for (NSString* a in args) {
        if (n + 1 >= sizeof(argv) / sizeof(argv[0])) return NO;
        argv[n++] = a.fileSystemRepresentation;
    }
    argv[n] = NULL;

    // Live parameter panel: a script run gets a private SOULX_UI_DIR to publish its controls in.
    char uienv[PATH_MAX + 16];
    const char* extra_env = NULL;
    _uiDir = nil;
    if (kind == JOB_RUN) {
        char dir[PATH_MAX];
        static unsigned seq = 0;
        if (soulx_path(dir, sizeof(dir), "tmp/ui-%ld-%u", (long)getpid(), ++seq) == 0 && soulx_mkdirs(dir) == 0) {
            _uiDir = @(dir);
            snprintf(uienv, sizeof(uienv), "SOULX_UI_DIR=%s", dir);
            extra_env = uienv;
        }
    }

    int fd = -1;
    pid_t pid = spawn_piped(argv, extra_env, &fd);
    if (pid < 0) {
        if (_uiDir) { soulx_rmtree(_uiDir.fileSystemRepresentation); _uiDir = nil; }
        [self message:SoulXText(@"The script could not be started.") finderPath:nil];
        return NO;
    }
    _job = kind;
    _jobPid = pid;
    _jobPath = [path copy];
    _jobStartMs = soulx_now_ms();
    _jobStopRequested = NO;
    if (kind == JOB_RUN) {
        // run/<name>.script: lets the launcher show which script is running (soulx_running_script), so its
        // own Run button can show "Stop" and offer it without owning the process itself.
        char dir[PATH_MAX], spath[PATH_MAX];
        if (soulx_path(dir, sizeof(dir), "run") == 0 && soulx_mkdirs(dir) == 0 &&
            soulx_path(spath, sizeof(spath), "run/%s.script", _conf.name) == 0) {
            char body[PATH_MAX + 32];
            int bn = snprintf(body, sizeof(body), "pid=%d\npath=%s\n", (int)pid, path.fileSystemRepresentation);
            if (bn > 0 && (size_t)bn < sizeof(body)) soulx_write_file_atomic(spath, body, (size_t)bn);
        }
    }
    if (kind == JOB_RUN && _uiDir) {
        _paramPanel = [[SoulXParamPanel alloc] initWithDir:_uiDir title:SoulXText(@"Script parameters")
                                                     near:self.window];
        __weak SoulXWindowController* weakSelf = self;
        _paramPanel.onClose = ^{                         // closing the panel stops the script (no need to hit Stop)
            SoulXWindowController* s = weakSelf;
            if (s && s->_job == JOB_RUN) [s stopJob];
        };
        [_paramPanel start];
    }

    // output -> log panel
    dispatch_queue_t q = dispatch_queue_create("com.soulx.job-output", DISPATCH_QUEUE_SERIAL);
    dispatch_source_t rd = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, q);
    NSMutableData* pending = [NSMutableData data];
    __weak SoulXWindowController* weakSelf = self;
    dispatch_source_set_event_handler(rd, ^{
        char buf[65536];
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r <= 0) {
            if (r < 0 && (errno == EINTR || errno == EAGAIN)) return;
            dispatch_source_cancel(rd);
            return;
        }
        [pending appendBytes:buf length:(NSUInteger)r];
        NSString* text = decode_chunk(pending);
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf appendLog:text];
        });
    });
    dispatch_source_set_cancel_handler(rd, ^{
        close(fd);
    });
    _jobRead = rd;
    dispatch_resume(rd);

    // exit
    dispatch_source_t ex = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)pid, DISPATCH_PROC_EXIT,
                                                  dispatch_get_main_queue());
    dispatch_source_set_event_handler(ex, ^{
        [weakSelf jobExited:pid];
    });
    _jobExit = ex;
    dispatch_resume(ex);
    // the child may have exited before the source was armed
    if (waitpid(pid, NULL, WNOHANG | WNOWAIT) == pid) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf jobExited:pid];
        });
    }
    [self setJobButtons];
    return YES;
}

- (void)stopJob {
    if (_job == JOB_NONE || _jobStopRequested) return;
    _jobStopRequested = YES;
    kill(_jobPid, SIGINT);
    double wait = _closing ? CLOSE_JOB_WAIT_S : (_job == JOB_RECORD ? RECORD_STOP_WAIT_S : RUN_STOP_WAIT_S);
    pid_t pid = _jobPid;
    __weak SoulXWindowController* weakSelf = self;
    _jobKill = [NSTimer scheduledTimerWithTimeInterval:wait repeats:NO block:^(NSTimer* t) {
        (void)t;
        SoulXWindowController* s = weakSelf;
        if (s && s->_job != JOB_NONE && s->_jobPid == pid) {
            soulx_log(SOULX_LOG_WARN, "the script process %d did not stop; killing it", (int)pid);
            kill(-pid, SIGKILL);                // its whole process group (adb children too)
            kill(pid, SIGKILL);
        }
    }];
}

- (void)jobExited:(pid_t)pid {
    if (_job == JOB_NONE || pid != _jobPid) return;
    int wstatus = 0;
    pid_t w;
    do {
        w = waitpid(pid, &wstatus, 0);
    } while (w < 0 && errno == EINTR);
    int status = w == pid ? decode_status(wstatus) : -1;
    JobKind kind = _job;
    NSString* path = _jobPath;
    BOOL stopped = _jobStopRequested;
    if (kind == JOB_RUN) {
        char spath[PATH_MAX];
        if (soulx_path(spath, sizeof(spath), "run/%s.script", _conf.name) == 0) unlink(spath);
    }
    if (_jobExit) dispatch_source_cancel(_jobExit);
    _jobExit = nil;
    _jobRead = nil;                             // its handler drains the pipe to EOF, then closes it
    [_jobKill invalidate];
    _jobKill = nil;
    _job = JOB_NONE;
    _jobPid = 0;
    _jobPath = nil;
    if (_paramPanel) { [_paramPanel stop]; _paramPanel = nil; }
    if (_uiDir) { soulx_rmtree(_uiDir.fileSystemRepresentation); _uiDir = nil; }
    [self setJobButtons];

    if (kind == JOB_RUN) {
        NSString* text = stopped ? SoulXText(@"Script stopped.")
                       : status == 0 ? SoulXText(@"Script finished.")
                       : [NSString stringWithFormat:SoulXText(@"Script failed (exit status %d)."), status];
        [self message:text finderPath:nil];
        [self appendLog:[NSString stringWithFormat:@"\n— %@\n", text]];
    } else if (kind == JOB_INSTALL) {
        NSString* text = stopped ? SoulXText(@"Cancelled.")
                       : status == 0 ? SoulXText(@"Done.")
                       : [NSString stringWithFormat:SoulXText(@"Failed (exit status %d)."), status];
        [self message:text finderPath:nil];
        [self appendLog:[NSString stringWithFormat:@"\n— %@\n", text]];
    } else {
        [self recordingEnded:path status:status];
    }
    NSArray* waiters = [_jobWaiters copy];
    [_jobWaiters removeAllObjects];
    for (dispatch_block_t w2 in waiters) w2();
}

// This instance's own scripts: ~/Documents/SoulX Scripts/<instance>/ (each instance keeps its scripts apart).
- (NSString*)scriptsFolder {
    char dir[PATH_MAX];
    if (soulx_scripts_dir(_conf.name, dir, sizeof(dir), true) == 0) return @(dir);
    NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    if (!docs) docs = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents"];
    return [[docs stringByAppendingPathComponent:@"SoulX Scripts"] stringByAppendingPathComponent:@(_conf.name)];
}

- (void)recordingEnded:(NSString*)tmp status:(int)status {
    NSFileManager* fm = [NSFileManager defaultManager];
    NSDictionary* attrs = tmp ? [fm attributesOfItemAtPath:tmp error:nil] : nil;
    if (!attrs || attrs.fileSize == 0) {
        if (tmp) [fm removeItemAtPath:tmp error:nil];
        [self message:[NSString stringWithFormat:SoulXText(@"Recording failed (exit status %d)."), status] finderPath:nil];
        [self showLogPanel];
        return;
    }
    NSString* folder = [self scriptsFolder];
    [fm createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
    NSString* name = [NSString stringWithFormat:@"recording-%@.lua", timestamp_now()];
    if (_closing) {                             // no dialog while closing: keep it in the default folder
        NSString* dst = [folder stringByAppendingPathComponent:name];
        NSError* err = nil;
        if ([fm moveItemAtPath:tmp toPath:dst error:&err]) {
            [self ensureName:dst fallback:name.lastPathComponent.stringByDeletingPathExtension];
            soulx_log(SOULX_LOG_INFO, "recording saved as %s", dst.UTF8String);
        } else {
            soulx_log(SOULX_LOG_ERROR, "cannot save the recording: %s (kept at %s)", err.localizedDescription.UTF8String,
                     tmp.UTF8String);
        }
        return;
    }
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.title = SoulXText(@"Save Recording");
    panel.directoryURL = [NSURL fileURLWithPath:folder isDirectory:YES];
    panel.nameFieldStringValue = name;
    UTType* lua = [UTType typeWithFilenameExtension:@"lua"];
    if (lua) panel.allowedContentTypes = @[ lua ];
    __weak SoulXWindowController* weakSelf = self;
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
        SoulXWindowController* s = weakSelf;
        if (result != NSModalResponseOK || !panel.URL) {
            [fm removeItemAtPath:tmp error:nil];
            [s message:SoulXText(@"Recording discarded.") finderPath:nil];
            return;
        }
        NSError* err = nil;
        NSURL* dst = panel.URL;
        [fm removeItemAtURL:dst error:nil];     // the panel already asked about replacing it
        if (![fm moveItemAtURL:[NSURL fileURLWithPath:tmp] toURL:dst error:&err]) {
            [s message:[NSString stringWithFormat:SoulXText(@"Cannot save the recording: %@"), err.localizedDescription]
            finderPath:nil];
            return;
        }
        [s ensureName:dst.path fallback:dst.path.lastPathComponent.stringByDeletingPathExtension];   // keep it runnable
        [s message:[NSString stringWithFormat:SoulXText(@"Recording saved: %@"), dst.path.stringByAbbreviatingWithTildeInPath]
        finderPath:dst.path];
    }];
}

- (void)soulxRecord:(id)sender {
    (void)sender;
    if (_closing) return;
    if (_job == JOB_RECORD) {
        [self stopJob];
        return;
    }
    if (_job != JOB_NONE) return;
    char tmp[PATH_MAX], dir[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "tmp") != 0 || soulx_mkdirs(dir) != 0 ||
        soulx_path(tmp, sizeof(tmp), "tmp/rec-%d-%u.lua", (int)getpid(), ++_recCounter) != 0) {
        [self message:SoulXText(@"Cannot create a temporary file.") finderPath:nil];
        return;
    }
    if ([self startJob:JOB_RECORD args:@[ @"record", @"-i", @(_conf.name), @"--force", @(tmp) ] path:@(tmp)]) {
        [self appendLog:[NSString stringWithFormat:@"\n▶ soulx record (%@)\n", timestamp_now()]];
        [self message:SoulXText(@"Recording… click Record again to stop.") finderPath:nil];
    }
}

// The speeds offered in the Run sheet (index matches the pop-up order).
static const double k_run_speeds[] = { 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 5.0 };

- (void)soulxRunScript:(id)sender {
    (void)sender;
    if (_closing) return;
    if (_job == JOB_RUN) {              // the button reads "Stop" while a script runs
        [self stopJob];
        return;
    }
    if (_job != JOB_NONE) return;
    [self showScriptPicker];            // a click-list of scripts, not a file browser
}

// A script must declare a name near the top:  -- name: My Script  (also accepts --@name / -- name = ...).
// Returns the declared name, or nil when there is none. We read only the first lines, without running the script.
- (NSString*)scriptName:(NSString*)path {
    NSString* src = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL];
    if (!src) src = [NSString stringWithContentsOfFile:path encoding:NSISOLatin1StringEncoding error:NULL];
    if (!src) return nil;
    NSCharacterSet* ws = NSCharacterSet.whitespaceCharacterSet;
    NSArray<NSString*>* lines = [src componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet];
    NSUInteger limit = MIN(lines.count, (NSUInteger)40);
    for (NSUInteger i = 0; i < limit; i++) {
        NSString* line = [lines[i] stringByTrimmingCharactersInSet:ws];
        if (![line hasPrefix:@"--"]) continue;
        NSString* rest = [[line substringFromIndex:2] stringByTrimmingCharactersInSet:ws];
        if ([rest hasPrefix:@"@"]) rest = [[rest substringFromIndex:1] stringByTrimmingCharactersInSet:ws];
        if (rest.length <= 4 || ![rest.lowercaseString hasPrefix:@"name"]) continue;
        unichar sep = [rest characterAtIndex:4];
        if (sep != ':' && sep != '=' && sep != ' ' && sep != '\t') continue;
        NSString* after = [[rest substringFromIndex:5] stringByTrimmingCharactersInSet:ws];
        while ([after hasPrefix:@":"] || [after hasPrefix:@"="]) {
            after = [[after substringFromIndex:1] stringByTrimmingCharactersInSet:ws];
        }
        if (after.length) return after;
    }
    return nil;
}

// Prepend a name directive when a script has none (used for recordings so they stay runnable).
- (void)ensureName:(NSString*)path fallback:(NSString*)fallback {
    if ([self scriptName:path]) return;
    NSString* src = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL];
    if (!src) return;
    NSString* named = [NSString stringWithFormat:@"-- name: %@\n%@", fallback.length ? fallback : @"Recording", src];
    [named writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:NULL];
}

// Named scripts in this instance's scripts folder (top level + one level of sub-folders, skipping 'lib').
- (NSArray<NSDictionary*>*)installedNamedScripts {
    NSMutableArray<NSDictionary*>* out = [NSMutableArray array];
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* root = [self scriptsFolder];
    NSArray* top = [[fm contentsOfDirectoryAtPath:root error:NULL]
                    sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
    for (NSString* entry in top) {
        NSString* full = [root stringByAppendingPathComponent:entry];
        BOOL dir = NO;
        [fm fileExistsAtPath:full isDirectory:&dir];
        if (!dir) {
            if (![entry.pathExtension.lowercaseString isEqualToString:@"lua"]) continue;
            NSString* nm = [self scriptName:full];
            if (nm) [out addObject:@{ @"name": nm, @"path": full }];
        } else if (![entry.lowercaseString isEqualToString:@"lib"]) {
            NSArray* sub = [[fm contentsOfDirectoryAtPath:full error:NULL]
                            sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
            for (NSString* sn in sub) {
                if (![sn.pathExtension.lowercaseString isEqualToString:@"lua"]) continue;
                NSString* sp = [full stringByAppendingPathComponent:sn];
                NSString* nm = [self scriptName:sp];
                if (nm) [out addObject:@{ @"name": nm, @"path": sp }];
            }
        }
    }
    return out;
}

- (SoulXToolRow*)pickerRow:(NSString*)title path:(NSString*)path symbol:(NSString*)symbol action:(SEL)action {
    SoulXToolRow* b = [[SoulXToolRow alloc] initWithFrame:NSZeroRect];
    b.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:title];
    b.title = title;
    b.toolTip = title;                  // the row truncates long names; hover shows the full one
    b.bordered = NO;
    b.focusRingType = NSFocusRingTypeNone;
    b.target = self;
    b.action = action;
    if (path) b.identifier = path;
    b.translatesAutoresizingMaskIntoConstraints = NO;
    [b.heightAnchor constraintEqualToConstant:ROW_HEIGHT].active = YES;
    [b.widthAnchor constraintEqualToConstant:TOOLBAR_WIDTH - 12].active = YES;
    return b;
}

// A thin divider row for a flyout (separates the script list from the Import/Browse actions).
- (NSView*)flyoutSeparator {
    NSBox* line = [[NSBox alloc] initWithFrame:NSZeroRect];
    line.boxType = NSBoxSeparator;
    line.translatesAutoresizingMaskIntoConstraints = NO;
    [line.heightAnchor constraintEqualToConstant:1].active = YES;
    [line.widthAnchor constraintEqualToConstant:TOOLBAR_WIDTH - 20].active = YES;
    return line;
}

- (void)showScriptPicker {
    SoulXToolRow* anchor = _categoryButtons[@"cat.scripts"];
    if (!anchor) return;
    NSMutableArray<NSView*>* rows = [NSMutableArray array];
    NSArray<NSDictionary*>* scripts = [self installedNamedScripts];
    if (scripts.count == 0) {
        // a centred, padded empty state (not a cramped label jammed against the actions)
        NSTextField* hint = [NSTextField wrappingLabelWithString:SoulXText(@"No scripts yet.\nImport one to get started.")];
        hint.alignment = NSTextAlignmentCenter;
        hint.textColor = NSColor.secondaryLabelColor;
        hint.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        hint.translatesAutoresizingMaskIntoConstraints = NO;
        NSView* pad = [[NSView alloc] initWithFrame:NSZeroRect];
        pad.translatesAutoresizingMaskIntoConstraints = NO;
        [pad addSubview:hint];
        [NSLayoutConstraint activateConstraints:@[
            [pad.widthAnchor constraintEqualToConstant:TOOLBAR_WIDTH - 12],
            [hint.leadingAnchor constraintEqualToAnchor:pad.leadingAnchor constant:8],
            [hint.trailingAnchor constraintEqualToAnchor:pad.trailingAnchor constant:-8],
            [hint.topAnchor constraintEqualToAnchor:pad.topAnchor constant:12],
            [hint.bottomAnchor constraintEqualToAnchor:pad.bottomAnchor constant:-12],
        ]];
        [rows addObject:pad];
    } else {
        for (NSDictionary* s in scripts) {
            [rows addObject:[self pickerRow:s[@"name"] path:s[@"path"] symbol:@"play.circle"
                                     action:@selector(scriptPicked:)]];
        }
    }
    [rows addObject:[self flyoutSeparator]];                 // scripts above, actions below
    [rows addObject:[self pickerRow:SoulXText(@"Import script…") path:nil symbol:@"square.and.arrow.down"
                             action:@selector(soulxImportScript:)]];
    [rows addObject:[self pickerRow:SoulXText(@"Browse…") path:nil symbol:@"folder"
                             action:@selector(soulxBrowseScript:)]];
    [self showFlyoutPopover:[self flyoutPopoverWith:rows] from:anchor];
}

- (void)scriptPicked:(id)sender {
    if (![sender isKindOfClass:[SoulXToolRow class]]) return;
    NSString* path = [(SoulXToolRow*)sender identifier];
    [self closeFlyout];
    if (path.length) [self startRun:path askOptions:NO];
}

- (void)soulxBrowseScript:(id)sender {
    (void)sender;
    [self closeFlyout];
    if (_closing || _job != JOB_NONE) return;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = SoulXText(@"Run Script");
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    UTType* lua = [UTType typeWithFilenameExtension:@"lua"];
    if (lua) panel.allowedContentTypes = @[ lua ];
    NSString* folder = [self scriptsFolder];
    if ([[NSFileManager defaultManager] fileExistsAtPath:folder]) panel.directoryURL = [NSURL fileURLWithPath:folder];
    __weak SoulXWindowController* weakSelf = self;
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
        SoulXWindowController* s = weakSelf;
        if (!s || result != NSModalResponseOK || !panel.URL) return;
        [s startRun:panel.URL.path askOptions:YES];
    }];
}

- (void)startRun:(NSString*)path askOptions:(BOOL)ask {
    if (_closing || _job != JOB_NONE || !path) return;
    if (![self scriptName:path]) {
        [self message:SoulXText(@"This script has no name — add a line like  -- name: My Script  at the top.")
          finderPath:path];
        return;
    }
    if (ask) {
        [self askRunOptionsFor:path];
    } else {
        [self startJob:JOB_RUN args:@[ @"run", @"-i", @(_conf.name), path ] path:path];
    }
}

// Second sheet: how many times, and how fast. Remembers the last choice in NSUserDefaults.
- (void)askRunOptionsFor:(NSString*)path {
    NSUserDefaults* def = [NSUserDefaults standardUserDefaults];
    NSInteger savedTimes = [def objectForKey:@"SoulXRunRepeat"] ? [def integerForKey:@"SoulXRunRepeat"] : 1;
    BOOL savedLoop = [def boolForKey:@"SoulXRunLoop"];
    double savedSpeed = [def objectForKey:@"SoulXRunSpeed"] ? [def doubleForKey:@"SoulXRunSpeed"] : 1.0;
    if (savedTimes < 1) savedTimes = 1;

    NSView* box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 96)];

    NSTextField* timesLabel = [NSTextField labelWithString:SoulXText(@"Run how many times:")];
    timesLabel.frame = NSMakeRect(0, 68, 180, 20);
    [box addSubview:timesLabel];
    NSComboBox* times = [[NSComboBox alloc] initWithFrame:NSMakeRect(186, 64, 110, 26)];
    times.completes = NO;
    times.hasVerticalScroller = NO;
    times.numberOfVisibleItems = 6;
    for (NSString* n in @[ @"1", @"3", @"5", @"10", @"20", @"50" ]) [times addItemWithObjectValue:n];
    times.integerValue = savedTimes;
    [box addSubview:times];

    NSButton* loop = [NSButton checkboxWithTitle:SoulXText(@"Repeat until I stop it (ignores the count)")
                                          target:nil action:nil];
    loop.frame = NSMakeRect(0, 38, 300, 20);
    loop.state = savedLoop ? NSControlStateValueOn : NSControlStateValueOff;
    [box addSubview:loop];

    NSTextField* speedLabel = [NSTextField labelWithString:SoulXText(@"Speed:")];
    speedLabel.frame = NSMakeRect(0, 6, 60, 20);
    [box addSubview:speedLabel];
    NSPopUpButton* speed = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(60, 2, 150, 26)];
    NSInteger speedSel = 3;                                 // default 1.0 (Normal)
    for (size_t i = 0; i < sizeof(k_run_speeds) / sizeof(k_run_speeds[0]); i++) {
        double v = k_run_speeds[i];
        NSString* title = v == 1.0 ? SoulXText(@"Normal (1×)") : [NSString stringWithFormat:@"%g×", v];
        [speed addItemWithTitle:title];
        if (fabs(v - savedSpeed) < 1e-9) speedSel = (NSInteger)i;
    }
    [speed selectItemAtIndex:speedSel];
    [box addSubview:speed];

    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = [NSString stringWithFormat:SoulXText(@"Run %@"), path.lastPathComponent];
    alert.informativeText = SoulXText(@"Choose how many times to run the script and the playback speed.");
    [alert addButtonWithTitle:SoulXText(@"Run")];
    [alert addButtonWithTitle:SoulXText(@"Cancel")];
    alert.accessoryView = box;

    __weak SoulXWindowController* weakSelf = self;
    [alert beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse resp) {
        SoulXWindowController* s = weakSelf;
        if (!s || resp != NSAlertFirstButtonReturn || s->_closing || s->_job != JOB_NONE) return;

        BOOL isLoop = (loop.state == NSControlStateValueOn);
        NSInteger n = times.integerValue;
        if (n < 1) n = 1;
        if (n > 100000) n = 100000;
        double sp = k_run_speeds[speed.indexOfSelectedItem < 0 ? 3 : speed.indexOfSelectedItem];

        [def setInteger:n forKey:@"SoulXRunRepeat"];
        [def setBool:isLoop forKey:@"SoulXRunLoop"];
        [def setDouble:sp forKey:@"SoulXRunSpeed"];

        NSMutableArray<NSString*>* args = [@[ @"run", @"-i", @(s->_conf.name) ] mutableCopy];
        if (isLoop) {
            [args addObject:@"--loop"];
        } else if (n != 1) {
            [args addObject:@"--repeat"];
            [args addObject:[@(n) stringValue]];
        }
        if (fabs(sp - 1.0) > 1e-9) {
            [args addObject:@"--speed"];
            [args addObject:[NSString stringWithFormat:@"%g", sp]];
        }
        [args addObject:path];

        if ([s startJob:JOB_RUN args:args path:path]) {
            NSString* how = isLoop ? SoulXText(@"looping") : [NSString stringWithFormat:SoulXText(@"%ld×"), (long)n];
            // Do not pop the log window open; it collects output quietly (Automation ▸ Show Log opens it).
            [s appendLog:[NSString stringWithFormat:@"\n▶ soulx %@ (%@)\n",
                          [args componentsJoinedByString:@" "], timestamp_now()]];
            [s message:[NSString stringWithFormat:SoulXText(@"Running %@ (%@, %g×)…"),
                        path.lastPathComponent, how, sp] finderPath:nil];
        }
    }];
}

- (void)soulxStopScript:(id)sender {
    (void)sender;
    [self stopJob];
}

// Import a script (or a whole script folder, so a multi-file script keeps its lib/ next to it) into this instance's
// scripts folder, where Run script looks. Importing the folder keeps require("lib.x") working.
- (void)soulxImportScript:(id)sender {
    (void)sender;
    if (_closing) return;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = SoulXText(@"Import Script");
    panel.message = SoulXText(@"Choose a .lua script, or a folder containing a script and the files it needs.");
    panel.prompt = SoulXText(@"Import");
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = YES;                   // multi-file script: import its whole folder
    panel.allowsMultipleSelection = YES;
    UTType* lua = [UTType typeWithFilenameExtension:@"lua"];
    panel.allowedContentTypes = lua ? @[ lua, UTTypeFolder ] : @[ UTTypeFolder ];
    __weak SoulXWindowController* weakSelf = self;
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
        SoulXWindowController* s = weakSelf;
        if (!s || result != NSModalResponseOK) return;
        NSFileManager* fm = [NSFileManager defaultManager];
        NSString* folder = [s scriptsFolder];
        if (![fm createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:NULL]) {
            [s message:SoulXText(@"Could not create the scripts folder.") finderPath:nil];
            return;
        }
        int copied = 0;
        NSString* last = nil;
        for (NSURL* url in panel.URLs) {
            NSString* dest = [folder stringByAppendingPathComponent:url.lastPathComponent];
            if ([dest isEqualToString:url.path]) continue;           // already in the scripts folder
            if ([fm fileExistsAtPath:dest]) [fm removeItemAtPath:dest error:NULL];   // replace on re-import
            NSError* e = nil;
            if ([fm copyItemAtURL:url toURL:[NSURL fileURLWithPath:dest] error:&e]) {
                copied++;
                last = dest;
            } else {
                soulx_log(SOULX_LOG_ERROR, "import failed for %s: %s",
                         url.lastPathComponent.UTF8String, e.localizedDescription.UTF8String);
            }
        }
        if (copied == 0) {
            [s message:SoulXText(@"Nothing was imported.") finderPath:nil];
            return;
        }
        NSString* msg = copied == 1 ? SoulXText(@"Imported. Use Run script to run it.")
                                    : [NSString stringWithFormat:SoulXText(@"Imported %d items."), copied];
        [s message:msg finderPath:(copied == 1 ? last : folder)];
    }];
}

// Screen video recording: the engine writes the WebM itself (console screenrecord), so this is just a
// start/stop pair on a background queue — it runs independently of record/run and the Lua scripts.
- (void)soulxRecordVideo:(id)sender {
    (void)sender;
    if (_closing) return;
    int instanceId = _conf.id;
    if (_recordingVideo) {
        NSString* path = _videoPath;
        _recordingVideo = NO;
        _videoPath = nil;
        [self setVideoButton];
        [self message:SoulXText(@"Saving video…") finderPath:nil];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            soulx_stop_recording(instanceId);
            dispatch_async(dispatch_get_main_queue(), ^{
                [self message:[NSString stringWithFormat:SoulXText(@"Saved %@"),
                               path.stringByAbbreviatingWithTildeInPath] finderPath:path];
            });
        });
        return;
    }
    NSString* name = @(_conf.name);
    NSString* stamp = timestamp_now();
    NSString* dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Movies/SoulX"];
    if (![[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                    attributes:nil error:nil]) {
        [self message:SoulXText(@"Could not create ~/Movies/SoulX.") finderPath:nil];
        return;
    }
    NSString* path = nil;
    for (int n = 1; n < 100 && !path; n++) {
        NSString* file = n == 1 ? [NSString stringWithFormat:@"%@-%@.webm", name, stamp]
                                : [NSString stringWithFormat:@"%@-%@-%d.webm", name, stamp, n];
        NSString* p = [dir stringByAppendingPathComponent:file];
        if (![[NSFileManager defaultManager] fileExistsAtPath:p]) path = p;
    }
    if (!path) {
        [self message:SoulXText(@"Could not choose a file name.") finderPath:nil];
        return;
    }
    __weak SoulXWindowController* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        int rc = soulx_start_recording(instanceId, path.fileSystemRepresentation);
        dispatch_async(dispatch_get_main_queue(), ^{
            SoulXWindowController* s = weakSelf;
            if (!s || s->_closing) {
                if (rc == SOULX_OK) soulx_stop_recording(instanceId);     // window went away mid-start
                return;
            }
            if (rc == SOULX_OK) {
                s->_recordingVideo = YES;
                s->_videoPath = path;
                [s setVideoButton];
                [s message:SoulXText(@"Recording video… (Automation ▸ Stop Video to finish)") finderPath:nil];
            } else {
                [s message:SoulXText(@"Could not start the recording.") finderPath:nil];
            }
        });
    });
}

// Reflect the recording state on the toolbar's 錄影 button (red stop while recording).
- (void)setVideoButton {
    NSButton* v = _buttons[@"video"];
    if (!v) return;
    if (_recordingVideo) {
        v.image = [NSImage imageWithSystemSymbolName:@"stop.circle.fill" accessibilityDescription:nil];
        v.contentTintColor = NSColor.systemRedColor;
        v.title = SoulXText(@"Stop video");
        v.toolTip = SoulXText(@"Stop recording the video");
    } else {
        v.image = [NSImage imageWithSystemSymbolName:@"video" accessibilityDescription:nil];
        v.contentTintColor = nil;
        v.title = SoulXText(@"Record video");
        v.toolTip = SoulXText(@"Record the screen to a video (⇧⌘E)");
    }
}

// The sidebar output-volume slider. Volume moves only via VOLUME_UP/DOWN key events on this image, so we
// track the level and send the delta as key events.
- (void)volumeChanged:(id)sender {
    (void)sender;
    _volumeTouchedMs = soulx_now_ms();
    _volumeSlider.integerValue = lround(_volumeSlider.doubleValue);   // Android's whole steps
    [self showVolumeValue];
    [_screen setMusicVolume:(int)_volumeSlider.integerValue];   // absolute + instant; 0 = mute
}

// ⌘V: type the Mac's clipboard text into the guest (ASCII for now).
- (void)soulxPaste:(id)sender {
    (void)sender;
    if (_closing) return;
    NSString* s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    if (s.length == 0) {
        [self message:SoulXText(@"The clipboard has no text.") finderPath:nil];
        return;
    }
    if (![s canBeConvertedToEncoding:NSASCIIStringEncoding]) {
        [self message:SoulXText(@"Only ASCII text can be pasted for now.") finderPath:nil];
        return;
    }
    [_screen typeText:s];
    [self message:SoulXText(@"Pasted.") finderPath:nil];
}

// Install an APK / folder of split APKs, or push any other file into the guest's Downloads. Shared by the
// toolbar Install button and drag-and-drop.
- (void)installFromPath:(NSString*)path {
    if (_closing || path.length == 0) return;
    if (_job != JOB_NONE) {
        [self message:SoulXText(@"Busy — wait for the current task to finish.") finderPath:nil];
        return;
    }
    BOOL dir = NO;
    [[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir];
    NSString* ext = path.pathExtension.lowercaseString;
    BOOL isApk = dir || [@[ @"apk", @"xapk", @"apks", @"apkm" ] containsObject:ext];
    NSArray<NSString*>* args = isApk
        ? @[ @"install", @"-i", @(_conf.name), path ]
        : @[ @"adb", @"-i", @(_conf.name), @"push", path, @"/sdcard/Download/" ];
    if ([self startJob:JOB_INSTALL args:args path:path]) {
        [self showLogPanel];
        [self appendLog:[NSString stringWithFormat:@"\n⤓ soulx %@ (%@)\n",
                         [args componentsJoinedByString:@" "], timestamp_now()]];
        [self message:[NSString stringWithFormat:(isApk ? SoulXText(@"Installing %@…") : SoulXText(@"Pushing %@…")),
                       path.lastPathComponent] finderPath:nil];
    }
}

- (void)handleDroppedFiles:(NSArray<NSURL*>*)urls {
    if (urls.count == 0) return;
    [self installFromPath:urls.firstObject.path];     // one at a time
}

- (void)soulxInstallApp:(id)sender {
    (void)sender;
    if (_closing) return;
    if (_job != JOB_NONE) {
        [self message:SoulXText(@"Busy — wait for the current task to finish.") finderPath:nil];
        return;
    }
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = SoulXText(@"Install app");
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = YES;                 // a folder of split APKs
    panel.allowsMultipleSelection = NO;
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (NSString* e in @[ @"apk", @"xapk", @"apks", @"apkm" ]) {
        UTType* t = [UTType typeWithFilenameExtension:e];
        if (t) [types addObject:t];
    }
    if (types.count) {
        UTType* folder = [UTType typeWithIdentifier:@"public.folder"];
        if (folder) [types addObject:folder];
        panel.allowedContentTypes = types;
    }
    __weak SoulXWindowController* weakSelf = self;
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
        SoulXWindowController* s = weakSelf;
        if (!s || result != NSModalResponseOK || !panel.URL || s->_closing || s->_job != JOB_NONE) return;
        [s installFromPath:panel.URL.path];
    }];
}

// Runs next once no record / run child is left (stopping it first).
- (void)stopJobsThen:(dispatch_block_t)next {
    if (_job == JOB_NONE) {
        next();
        return;
    }
    [_jobWaiters addObject:[next copy]];
    [self stopJob];
}

// ---------------------------------------------------------------------------
// Closing
// ---------------------------------------------------------------------------

- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    [self closeAndStop:nil];
    return NO;                                  // the window goes away once the instance has stopped
}

- (void)beginClosing {
    _closing = YES;
    [self closeFlyout];
    if (_recordingVideo) {                                 // finish the video before the instance stops
        int instanceId = _conf.id;
        _recordingVideo = NO;
        _videoPath = nil;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ soulx_stop_recording(instanceId); });
    }
    [self setJobButtons];
    for (NSButton* b in _buttons.allValues) b.enabled = NO;
    if (_screen.keymapEditing) [_screen.overlay endEditing:NO];
}

- (void)closeAndStop:(id)sender {
    (void)sender;
    if (_closing) return;
    [self beginClosing];
    _stopping = YES;
    _stopStartMs = soulx_now_ms();
    [self updateNotice];
    NSString* name = @(_conf.name);
    __weak SoulXWindowController* weakSelf = self;
    [self stopJobsThen:^{
        SoulXWindowController* s = weakSelf;
        if (!s) return;
        [s->_screen shutdownInput:^{
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                int rc = soulx_mac_stop(name.UTF8String, false);
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (rc != 0) soulx_log(SOULX_LOG_ERROR, "'%s' could not be stopped cleanly", name.UTF8String);
                    [weakSelf finish:rc == 0 ? 0 : 1];
                });
            });
        }];
    }];
}

- (void)closeKeepRunning:(id)sender {
    (void)sender;
    [self closeKeepRunningWithCode:0];
}

- (void)closeKeepRunningWithCode:(int)code {
    if (_closing) return;
    [self beginClosing];
    [self updateNotice];
    __weak SoulXWindowController* weakSelf = self;
    [self stopJobsThen:^{
        SoulXWindowController* s = weakSelf;
        if (!s) return;
        [s->_screen shutdownInput:^{
            [weakSelf finish:code];
        }];
    }];
}

- (void)engineDidExit {
    if (_stopping || _closing) return;          // our own stop: soulx_mac_stop returns right after
    soulx_log(SOULX_LOG_INFO, "the engine of '%s' exited; closing the window", _conf.name);
    [self beginClosing];
    __weak SoulXWindowController* weakSelf = self;
    // the window must be gone within a second, whatever the input queue is doing
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 900 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        [weakSelf finish:0];
    });
    [self stopJobsThen:^{
        SoulXWindowController* s = weakSelf;
        if (!s) return;
        [s->_screen shutdownInput:^{
            [weakSelf finish:0];
        }];
    }];
}

- (void)finish:(int)code {
    if (_finished) return;
    _finished = YES;
    [_tick invalidate];
    _tick = nil;
    [_logPanel orderOut:nil];
    [self.window orderOut:nil];
    if (self.onFinish) self.onFinish(code);
}

- (void)windowDidResignKey:(NSNotification*)note {
    (void)note;
    [_screen releaseAllInput];
}

// A second 'soulx window <name>' asked for this window (app.m).
- (void)bringToFront {
    if (_finished) return;
    [self showWindow:nil];
    [self.window makeKeyAndOrderFront:nil];
    if (@available(macOS 14.0, *)) {
        [NSApp activate];
    } else {
        [NSApp activateIgnoringOtherApps:YES];
    }
}

// The launcher's Run button asked this window to run one of the instance's scripts (app.m, kRunNotification):
// same as picking it from the script flyout, so it gets the live parameter panel too. Silently does nothing when
// a job is already running (the window is still brought forward so the user can see what's going on) or the
// script has gone missing; startRun: already reports a missing "-- name:" line.
- (void)requestRun:(NSString*)path {
    if (_finished) return;
    [self bringToFront];
    if (_closing || _job != JOB_NONE || !path.length) return;
    [self startRun:path askOptions:NO];
}

// Same as soulxStopScript: (Cmd-.), for the launcher's Stop button.
- (void)requestStop {
    if (_finished) return;
    [self stopJob];
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    SEL a = item.action;
    if (a == @selector(soulxShowBindings:)) {
        item.state = _screen.showBindings ? NSControlStateValueOn : NSControlStateValueOff;
        return !_closing;
    }
    if (a == @selector(soulxInspect:)) {
        item.state = _screen.inspecting ? NSControlStateValueOn : NSControlStateValueOff;
        return !_closing;
    }
    if (a == @selector(soulxEditKeymap:)) {
        item.state = _screen.keymapEditing ? NSControlStateValueOn : NSControlStateValueOff;
        return !_closing;
    }
    if (a == @selector(soulxRecord:)) {
        item.title = _job == JOB_RECORD ? SoulXText(@"Stop Recording") : SoulXText(@"Record");
        return !_closing && _job != JOB_RUN;
    }
    if (a == @selector(soulxRunScript:)) return !_closing && _job == JOB_NONE;
    if (a == @selector(soulxStopScript:)) return !_closing && _job != JOB_NONE;
    if (a == @selector(soulxInstallApp:)) return !_closing && _job == JOB_NONE;
    if (a == @selector(soulxPaste:)) return !_closing && [self deviceUsable];
    if (a == @selector(soulxRecordVideo:)) {
        item.title = _recordingVideo ? SoulXText(@"Stop Video") : SoulXText(@"Record Video");
        return !_closing && [self deviceUsable];
    }
    if (a == @selector(soulxBack:) || a == @selector(soulxHome:) || a == @selector(soulxRecents:) ||
        a == @selector(soulxRotate:)) {
        return [self deviceUsable] || (a == @selector(soulxBack:) && _screen.keymapEditing);
    }
    if (a == @selector(soulxActualSize:) || a == @selector(soulxFit:)) {
        return !(self.window.styleMask & NSWindowStyleMaskFullScreen);
    }
    if (a == @selector(soulxToggleToolbar:)) {
        item.title = _content.toolbarCollapsed ? SoulXText(@"Show Toolbar") : SoulXText(@"Hide Toolbar");
        return !_closing;
    }
    if (a == @selector(soulxShowLog:)) return !_closing;
    if (a == @selector(closeAndStop:) || a == @selector(closeKeepRunning:) || a == @selector(soulxScreenshot:)) {
        return !_closing;
    }
    return YES;
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

- (void)hookDone:(BOOL)ok {
    if (!ok) _hooksFailed = YES;
    if (--_hooksPending > 0) return;
    [self closeKeepRunningWithCode:_hooksFailed ? 1 : 0];
}

// Timers that run from the start: --exit-after-ms and the no-frame watchdog.
- (void)armHooks {
    __weak SoulXWindowController* weakSelf = self;
    if (_opts.exit_after_ms > 0) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)_opts.exit_after_ms * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            [weakSelf closeKeepRunning:nil];
        });
    }
    if (_opts.dump_frame) _hooksPending++;
    if (_opts.stats_after_ms > 0) _hooksPending++;
    if (_opts.test_screenshot) _hooksPending++;
    if (_opts.test_rotate) _hooksPending++;
    if (_hooksPending > 0) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(HOOK_FRAME_TIMEOUT_S * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            SoulXWindowController* s = weakSelf;
            if (s && !s->_hooksStarted && !s->_closing) {
                soulx_log(SOULX_LOG_ERROR, "no frame arrived within %.0f s", HOOK_FRAME_TIMEOUT_S);
                s->_hooksPending = 1;
                [s hookDone:NO];
            }
        });
    }
}

// After the first frame: --dump-frame, --stats-after-ms, --simulate-tap, --simulate-key.
- (void)startHooks {
    if (_hooksStarted) return;
    _hooksStarted = YES;
    __weak SoulXWindowController* weakSelf = self;
    if (_opts.stats_after_ms > 0) {
        [_screen resetStats];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)_opts.stats_after_ms * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            SoulXWindowController* s = weakSelf;
            if (!s || s->_finished) return;
            printf("%s\n", [s->_screen statsLine].UTF8String);
            fflush(stdout);
            [s hookDone:YES];
        });
    }
    if (_opts.dump_frame) {
        NSString* path = @(_opts.dump_frame);
        int after = _opts.after_ms >= 0 ? _opts.after_ms : SOULX_WINDOW_DUMP_AFTER_MS;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)after * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            SoulXWindowController* s = weakSelf;
            if (!s || s->_finished) return;
            __block BOOL answered = NO;
            [s->_screen dumpFrameToFile:path completion:^(BOOL ok) {
                if (answered) return;
                answered = YES;
                if (ok) printf("%s\n", path.fileSystemRepresentation);
                fflush(stdout);
                [weakSelf hookDone:ok];
            }];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(HOOK_DUMP_TIMEOUT_S * NSEC_PER_SEC)),
                           dispatch_get_main_queue(), ^{
                if (answered) return;
                answered = YES;
                soulx_log(SOULX_LOG_ERROR, "--dump-frame: the window did not draw (is it hidden?)");
                [weakSelf hookDone:NO];
            });
        });
    }
    if (_opts.tap) {
        int x = _opts.tap_x, y = _opts.tap_y;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            SoulXWindowController* s = weakSelf;
            if (s && ![s->_screen simulateTapAtX:x y:y]) {
                soulx_log(SOULX_LOG_ERROR, "--simulate-tap %d %d is outside the screen", x, y);
            }
        });
    }
    if (_opts.key) {
        NSString* key = @(_opts.key);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            SoulXWindowController* s = weakSelf;
            [s->_screen simulateKeyNamed:key completion:^(BOOL ok) {
                if (!ok) soulx_log(SOULX_LOG_ERROR, "--simulate-key %s failed", key.UTF8String);
            }];
        });
    }
    // test-only: exercise the toolbar code paths
    if (_opts.test_rotate) {
        [self soulxRotate:nil];                  // toggles the guest rotation like the toolbar button
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            SoulXWindowController* s = weakSelf;
            if (!s || s->_finished) return;
            printf("rotated to %d degrees\n", s->_screen.rotation * 90);
            fflush(stdout);
            [s hookDone:YES];
        });
    }
    if (_opts.test_screenshot) {
        [self captureScreenshotThen:^(NSString* path) {
            SoulXWindowController* s = weakSelf;
            if (!s || s->_finished) return;
            if (path) {
                printf("%s\n", path.fileSystemRepresentation);
                fflush(stdout);
            } else {
                soulx_log(SOULX_LOG_ERROR, "--test-screenshot: could not save a screenshot");
            }
            [s hookDone:path != nil];
        }];
    }
}

@end
