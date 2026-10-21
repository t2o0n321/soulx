/*
 * SoulX Android Emulator - SoulX's own window (macOS): interfaces shared by the window files
 *
 *   app.m                soulx_window_run(): options, NSApplication, menus, icon, pid file, lifecycle
 *   window_controller.m  the window: toolbar, status bar, overlays, log panel, record / run, screenshot
 *   screen_view.m        MTKView: frames -> Metal, rotation, mouse / wheel / keyboard -> the input queue
 *   keymap_editor.m      keymap overlay layers and the keymap edit mode
 *
 * Threads:
 *   - main thread: AppKit, Metal encoding, the frames reader (SoulXFrames is used on the main thread only)
 *   - "com.soulx.input" (serial): every SoulXInput, SoulXHidKbd and keymap-runtime call, in event order
 *   - background queues: boot polling, foreground-app polling, hidkbd open, screenshots, soulx_mac_stop
 * The main thread never waits for adb or the emulator console.
 *
 * Coordinates: the main thread turns a pointer position into fractions (0..1) of the displayed Android screen;
 * the input queue multiplies them by the logical size input.c maps touches with (soulx_input_screen_size), so a
 * touch lands where the user clicked even when the guest's display size differs from the shared frames.
 */

#ifndef SOULX_WINDOW_INTERNAL_H
#define SOULX_WINDOW_INTERNAL_H

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "soulx_mac.h"
#include "frames.h"
#include "keymap.h"

// input.c exports used by the window (declared there for record.c and the tests, not in soulx_mac.h)
int  soulx_image_rotate_to_logical(const SoulXImage* fb, int rotation, SoulXImage* out);

// keymap.c export, not in keymap.h yet: the logical screen size in pixels, so that a joystick radius (a fraction
// of the shorter side) moves the finger on a circle instead of an ellipse
void soulx_keymap_runtime_set_screen(SoulXKeymapRuntime* rt, int width, int height);

#define SOULX_WINDOW_MOUSE_FINGER    0       // finger 0 = mouse, 1..8 = keymap (keymap.h), 9 = wheel
#define SOULX_WINDOW_SCROLL_FINGER   9
#define SOULX_WINDOW_DUMP_AFTER_MS   2000    // --dump-frame default delay

// Test hooks (strings point into argv)
typedef struct {
    const char* dump_frame;      // --dump-frame <out.png>: write what is on screen, then exit
    int  after_ms;               // --after-ms N: delay of --dump-frame, counted from the first frame (-1 = default)
    bool tap;                    // --simulate-tap X Y (logical pixels), after the first frame
    int  tap_x;
    int  tap_y;
    const char* key;             // --simulate-key NAME (keymap.h key name), after the first frame
    int  stats_after_ms;         // --stats-after-ms N: print render statistics N ms after the first frame, exit
    int  exit_after_ms;          // --exit-after-ms N: close the window (instance keeps running) after N ms
    // test-only (lets end-to-end tests exercise the toolbar code paths without a host-screen grab)
    bool test_screenshot;        // --test-screenshot: save a screenshot (the toolbar action), print its path, exit
    bool test_rotate;            // --test-rotate: toggle the guest rotation (the toolbar action), then exit
} SoulXWindowOptions;

// Localised UI text: Traditional Chinese (zh-Hant/TW/HK/MO), Simplified Chinese (zh-Hans/CN/SG) or the
// English key itself (app.m).
NSString* SoulXText(NSString* english) NS_FORMAT_ARGUMENT(1);
NSImage*  SoulXAppIcon(void);                                   // drawn in code (app.m)

@class SoulXScreenView;
@class SoulXKeymapOverlay;

// ---------------------------------------------------------------------------
// screen_view.m
// ---------------------------------------------------------------------------

@protocol SoulXScreenViewOwner <NSObject>
- (void)screenViewDidShowFirstFrame:(SoulXScreenView*)view;
- (void)screenViewGeometryDidChange:(SoulXScreenView*)view;    // logical size / rotation changed
- (void)screenView:(SoulXScreenView*)view fps:(double)fps;      // once per second
- (void)screenView:(SoulXScreenView*)view message:(NSString*)text;
- (void)screenView:(SoulXScreenView*)view keymapStatus:(NSString*)text;
- (void)screenViewEditingDidChange:(SoulXScreenView*)view;
@end

@interface SoulXScreenView : MTKView <MTKViewDelegate>
- (instancetype)initWithConf:(const SoulXInstanceConf*)conf state:(const SoulXRunState*)st;
@property (nonatomic, weak) id<SoulXScreenViewOwner> owner;
@property (nonatomic, readonly) NSSize logicalSize;   // pixels of the upright Android screen (as displayed)
@property (nonatomic, readonly) int rotation;         // guest rotation 0..3 used for display
@property (nonatomic, readonly) BOOL hasFrame;
@property (nonatomic, readonly) BOOL slowMode;
@property (nonatomic) BOOL inputEnabled;               // NO while the window is closing
@property (nonatomic, readonly) SoulXKeymapOverlay* overlay;
@property (nonatomic, readonly) BOOL keymapEditing;
@property (nonatomic) BOOL showBindings;
@property (nonatomic) BOOL inspecting;                 // read x,y + colour under the pointer; click copies

- (NSRect)imageRect;                                   // aspect-fit rect of the Android screen, view points
- (void)startSlowMode;                                 // Fallback: console screenshots, ~8 fps
- (void)androidKey:(const char*)name;                  // "BACK", "HOME", "APP_SWITCH" (input queue)
- (void)toggleRotation;                                // guest rotation 0 <-> 1
- (void)releaseAllInput;                               // fingers up, keys up (focus loss)
- (void)shutdownInput:(dispatch_block_t)done;          // releases everything and closes input + hidkbd;
                                                       // done runs on the main thread
- (void)captureUpright:(void (^)(SoulXImage* img))done;
                                                       // current frame, upright RGBA (malloc'd, the receiver
                                                       // frees it with soulx_image_free + free); NULL when none
- (void)dumpFrameToFile:(NSString*)path completion:(void (^)(BOOL ok))done;
- (void)resetStats;
- (NSString*)statsLine;
- (BOOL)simulateTapAtX:(int)x y:(int)y;
- (void)simulateKeyNamed:(NSString*)name completion:(void (^)(BOOL ok))done;
- (void)setMusicVolume:(int)index;                     // absolute STREAM_MUSIC volume 0..15 (instant)
- (void)readMusicVolumeThen:(void (^)(int v))done;     // current music volume 0..15 (-1 on failure)
- (void)typeText:(NSString*)s;                         // type a string into the guest (⌘V paste)
- (void)setKeymapEditing:(BOOL)editing;
- (void)keymapEditorDidSave;                           // the editor wrote a keymap file: reload
- (void)keymapEditorDidEnd;                            // the editor closed (saved or cancelled)
@end

// ---------------------------------------------------------------------------
// keymap_editor.m
// ---------------------------------------------------------------------------

@interface SoulXKeymapOverlay : NSView
- (instancetype)initWithScreen:(SoulXScreenView*)screen;
@property (nonatomic, readonly) BOOL editing;
@property (nonatomic) BOOL showBindings;               // play mode: draw the active keymap
- (void)setActiveKeymap:(const SoulXKeymap*)km;         // NULL = none
- (void)beginEditing:(const SoulXKeymap*)km package:(NSString*)package;
- (void)endEditing:(BOOL)save;
- (void)relayout;                                      // image rect or logical size changed
- (BOOL)handleEscape;                                  // YES when the editor used Esc
@end

// ---------------------------------------------------------------------------
// window_controller.m
// ---------------------------------------------------------------------------

@interface SoulXWindowController : NSWindowController <NSWindowDelegate, SoulXScreenViewOwner, NSMenuItemValidation>
- (instancetype)initWithConf:(const SoulXInstanceConf*)conf state:(const SoulXRunState*)st
                     options:(const SoulXWindowOptions*)opts;
@property (nonatomic, copy) void (^onFinish)(int code);   // once, when the window process should exit
- (void)engineDidExit;                                   // the engine went away by itself
- (void)bringToFront;                                    // another 'soulx window <name>' asked for the window
- (void)requestRun:(NSString*)path;                       // the launcher asked to run one of this instance's scripts
- (void)requestStop;                                      // the launcher's Stop button (same as soulxStopScript:)
- (void)closeAndStop:(id)sender;                         // close button, Quit, Command-W: graceful stop
- (void)closeKeepRunning:(id)sender;                     // Option-Command-W, signals, --exit-after-ms
// menu actions
- (void)soulxBack:(id)sender;
- (void)soulxHome:(id)sender;
- (void)soulxRecents:(id)sender;
- (void)soulxRotate:(id)sender;
- (void)soulxScreenshot:(id)sender;
- (void)soulxRecord:(id)sender;
- (void)soulxRunScript:(id)sender;
- (void)soulxStopScript:(id)sender;
- (void)soulxImportScript:(id)sender;    // copy a script (or its folder) into ~/Documents/SoulX Scripts
- (void)soulxRecordVideo:(id)sender;     // start/stop screen video recording (WebM to ~/Movies/SoulX)
- (void)soulxPaste:(id)sender;           // ⌘V: type the Mac clipboard into the guest
- (void)soulxEditKeymap:(id)sender;
- (void)soulxShowBindings:(id)sender;
- (void)soulxInspect:(id)sender;         // toggle the coordinate/colour inspector
- (void)soulxActualSize:(id)sender;
- (void)soulxFit:(id)sender;
- (void)soulxToggleToolbar:(id)sender;   // hide/show the right-hand toolbar
- (void)soulxShowLog:(id)sender;         // open the script-output log panel
@end

// ---------------------------------------------------------------------------
// param_panel.m — live parameter panel for a running script (SOULX_UI_DIR protocol)
// ---------------------------------------------------------------------------

@interface SoulXParamPanel : NSObject
- (instancetype)initWithDir:(NSString*)dir title:(NSString*)title near:(NSWindow*)host;
- (void)start;                                          // begin polling <dir>/params and showing controls
- (void)stop;                                           // close the panel and stop polling (programmatic)
@property (nonatomic, copy) void (^onClose)(void);      // the user closed the panel (closing its window = stop)
@end

#endif // SOULX_WINDOW_INTERNAL_H
