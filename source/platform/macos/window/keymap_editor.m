/*
 * SoulX Android Emulator - keymap overlay and keymap edit mode of SoulX's window
 *
 * The overlay is a transparent subview of the screen view, drawn with CALayers above the Metal layer:
 *   tap      a circle with the key name
 *   joystick a ring with the up / left / down / right key names
 *   swipe    an arrow from the start point (with the key name) to the end point
 * Play mode shows the active keymap only when "Show bindings" is on and never takes events (hitTest = nil).
 * Edit mode (toolbar keymap button or Command-K) takes the mouse and the keyboard:
 *   - click an empty spot, then press a key: a new tap binding
 *   - drag a binding to move it (a swipe's end point separately); scroll on a joystick to change its radius
 *   - right-click a binding: Delete, Change key, Make joystick (WASD) / Make tap, Use arrow keys
 *   - "Add joystick"; "All games" saves default.keymap instead of the app's own file
 *   - Done saves with soulx_keymap_save, Esc or Cancel discards
 * Positions are fractions of the logical (upright) screen, so they follow rotation and window size.
 */

#import "window_internal.h"

#import <QuartzCore/QuartzCore.h>

#include <math.h>
#include <string.h>

#define TAP_RADIUS_PT       22.0
#define HIT_SLOP_PT         8.0
#define JOY_DEFAULT_RADIUS  0.12
#define JOY_MIN_RADIUS      0.03
#define JOY_MAX_RADIUS      0.5
#define BAR_HEIGHT_PT       34.0
#define NEW_BINDING         SOULX_KEYMAP_MAX     // _awaitIndex: create a new tap binding

static double clamp01(double v) {
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

static NSString* key_label(int key) {
    const char* n = soulx_key_name(key);
    return n ? @(n) : @"?";
}

static CATextLayer* text_layer(NSString* text, CGFloat size, CGFloat scale) {
    CATextLayer* t = [CATextLayer layer];
    t.string = text;
    t.font = (__bridge CFTypeRef)[NSFont boldSystemFontOfSize:size];
    t.fontSize = size;
    t.alignmentMode = kCAAlignmentCenter;
    t.foregroundColor = NSColor.whiteColor.CGColor;
    t.contentsScale = scale;
    t.shadowOpacity = 0.8f;
    t.shadowRadius = 1.5;
    t.shadowOffset = CGSizeZero;
    return t;
}

static void center_text(CATextLayer* t, CGPoint c, CGFloat size) {
    CGFloat w = 120, h = size * 1.35;
    t.frame = CGRectMake(c.x - w / 2, c.y - h / 2, w, h);
}

@implementation SoulXKeymapOverlay {
    __weak SoulXScreenView* _screen;
    SoulXKeymap _active;          // play mode
    bool _hasActive;
    SoulXKeymap _edit;            // edit mode working copy
    NSString* _package;
    BOOL _editing;
    BOOL _showBindings;
    int _dragIndex;              // -1 = none
    int _dragPart;               // 0 = binding / swipe start, 1 = swipe end
    double _dragDX, _dragDY;     // binding position minus pointer (fractions)
    int _awaitIndex;             // -1 = none, NEW_BINDING = new tap at _awaitX/_awaitY, else change its key
    double _awaitX, _awaitY;
    CALayer* _layers;            // binding layers
    NSView* _bar;
    NSTextField* _barTitle;
    NSButton* _allGames;
    NSTextField* _hint;
}

@synthesize editing = _editing;

- (instancetype)initWithScreen:(SoulXScreenView*)screen {
    self = [super initWithFrame:NSMakeRect(0, 0, 100, 100)];
    if (!self) return nil;
    _screen = screen;
    _dragIndex = -1;
    _awaitIndex = -1;
    self.wantsLayer = YES;
    self.layer.backgroundColor = NSColor.clearColor.CGColor;
    _layers = [CALayer layer];
    _layers.frame = self.bounds;
    [self.layer addSublayer:_layers];
    [self buildBar];
    return self;
}

- (BOOL)isFlipped {
    return NO;
}

- (BOOL)acceptsFirstResponder {
    return _editing;
}

- (NSView*)hitTest:(NSPoint)point {
    return _editing ? [super hitTest:point] : nil;   // play mode: the screen view gets every event
}

- (BOOL)showBindings {
    return _showBindings;
}

- (void)setShowBindings:(BOOL)show {
    _showBindings = show;
    [self relayout];
}

// ---------------------------------------------------------------------------
// Edit bar
// ---------------------------------------------------------------------------

- (NSButton*)barButton:(NSString*)title action:(SEL)action {
    NSButton* b = [NSButton buttonWithTitle:title target:self action:action];
    b.bezelStyle = NSBezelStyleRounded;
    b.controlSize = NSControlSizeSmall;
    b.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    return b;
}

- (void)buildBar {
    _bar = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 100, BAR_HEIGHT_PT)];
    _bar.wantsLayer = YES;
    _bar.layer.backgroundColor = [NSColor colorWithWhite:0 alpha:0.72].CGColor;
    _bar.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;

    _barTitle = [NSTextField labelWithString:@""];
    _barTitle.textColor = NSColor.whiteColor;
    _barTitle.font = [NSFont boldSystemFontOfSize:12];
    _barTitle.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [_barTitle setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSButton* add = [self barButton:SoulXText(@"Add joystick") action:@selector(addJoystick:)];
    _allGames = [NSButton checkboxWithTitle:SoulXText(@"All games") target:self action:@selector(toggleAllGames:)];
    _allGames.controlSize = NSControlSizeSmall;
    _allGames.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    _allGames.attributedTitle = [[NSAttributedString alloc] initWithString:SoulXText(@"All games")
        attributes:@{ NSForegroundColorAttributeName: NSColor.whiteColor, NSFontAttributeName: _allGames.font }];
    NSButton* cancel = [self barButton:SoulXText(@"Cancel") action:@selector(cancelEditing:)];
    NSButton* done = [self barButton:SoulXText(@"Done") action:@selector(saveEditing:)];
    done.keyEquivalent = @"\r";

    NSStackView* row = [NSStackView stackViewWithViews:@[ _barTitle, add, _allGames, cancel, done ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing = 8;
    row.edgeInsets = NSEdgeInsetsMake(4, 10, 4, 10);
    row.translatesAutoresizingMaskIntoConstraints = NO;
    [_bar addSubview:row];
    [NSLayoutConstraint activateConstraints:@[
        [row.leadingAnchor constraintEqualToAnchor:_bar.leadingAnchor],
        [row.trailingAnchor constraintLessThanOrEqualToAnchor:_bar.trailingAnchor],
        [row.centerYAnchor constraintEqualToAnchor:_bar.centerYAnchor],
    ]];

    _hint = [NSTextField wrappingLabelWithString:SoulXText(@"Click an empty spot, then press a key. Drag to move, "
                                                         @"right-click for more.")];
    _hint.textColor = [NSColor colorWithWhite:1 alpha:0.85];
    _hint.font = [NSFont systemFontOfSize:11];
    _hint.alignment = NSTextAlignmentCenter;
    _hint.drawsBackground = NO;

    _bar.hidden = YES;
    _hint.hidden = YES;
    [self addSubview:_bar];
    [self addSubview:_hint];
}

- (void)layoutEditChrome {
    NSRect b = self.bounds;
    _bar.frame = NSMakeRect(0, NSMaxY(b) - BAR_HEIGHT_PT, b.size.width, BAR_HEIGHT_PT);
    _hint.frame = NSMakeRect(10, 6, fmax(b.size.width - 20, 10), 32);
    NSString* who = _allGames.state == NSControlStateValueOn || _package.length == 0
                        ? SoulXText(@"all games") : _package;
    _barTitle.stringValue = [NSString stringWithFormat:SoulXText(@"Keymap: %@"), who];
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

- (NSRect)image {
    SoulXScreenView* s = _screen;
    return s ? [s imageRect] : NSZeroRect;
}

- (CGPoint)pointForX:(double)fx y:(double)fy {
    NSRect r = [self image];
    return CGPointMake(r.origin.x + fx * r.size.width, r.origin.y + (1.0 - fy) * r.size.height);
}

- (void)fractionForPoint:(NSPoint)p x:(double*)fx y:(double*)fy {
    NSRect r = [self image];
    *fx = r.size.width > 0 ? (p.x - r.origin.x) / r.size.width : 0;
    *fy = r.size.height > 0 ? 1.0 - (p.y - r.origin.y) / r.size.height : 0;
}

- (CGFloat)joystickRadiusPt:(const SoulXKeyBinding*)b {
    NSRect r = [self image];
    return (CGFloat)(b->radius * fmin(r.size.width, r.size.height));
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self relayout];
}

- (void)relayout {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _layers.frame = self.bounds;
    _layers.sublayers = nil;
    const SoulXKeymap* km = _editing ? &_edit : (_showBindings && _hasActive ? &_active : NULL);
    if (_editing) {
        CALayer* tint = [CALayer layer];
        tint.frame = NSRectToCGRect([self image]);
        tint.backgroundColor = [NSColor colorWithWhite:0 alpha:0.25].CGColor;
        [_layers addSublayer:tint];
    }
    CGFloat scale = self.window ? self.window.backingScaleFactor : 2.0;
    for (int i = 0; km && i < km->count; i++) [self addLayersFor:&km->b[i] highlighted:i == _dragIndex scale:scale];
    if (_editing && _awaitIndex >= 0) [self addPromptLayerScale:scale];
    [CATransaction commit];
    if (_editing) [self layoutEditChrome];
}

- (void)addLayersFor:(const SoulXKeyBinding*)b highlighted:(BOOL)hl scale:(CGFloat)scale {
    CGColorRef stroke = [NSColor colorWithWhite:1 alpha:0.9].CGColor;
    CGColorRef fill = (hl ? [NSColor colorWithRed:0.25 green:0.6 blue:1 alpha:0.55]
                          : [NSColor colorWithWhite:1 alpha:_editing ? 0.3 : 0.18]).CGColor;
    CGPoint c = [self pointForX:b->x y:b->y];
    if (b->kind == SOULX_KM_JOYSTICK) {
        CGFloat rad = [self joystickRadiusPt:b];
        CAShapeLayer* ring = [CAShapeLayer layer];
        ring.path = CGPathCreateWithEllipseInRect(CGRectMake(c.x - rad, c.y - rad, 2 * rad, 2 * rad), NULL);
        ring.fillColor = fill;
        ring.strokeColor = stroke;
        ring.lineWidth = 2;
        CGPathRelease(ring.path);
        [_layers addSublayer:ring];
        CAShapeLayer* dot = [CAShapeLayer layer];
        dot.path = CGPathCreateWithEllipseInRect(CGRectMake(c.x - 4, c.y - 4, 8, 8), NULL);
        dot.fillColor = stroke;
        CGPathRelease(dot.path);
        [_layers addSublayer:dot];
        // up, left, down, right inside the ring
        const CGFloat in = rad > 30 ? rad - 14 : rad * 0.55;
        const CGPoint at[4] = { { c.x, c.y + in }, { c.x - in, c.y }, { c.x, c.y - in }, { c.x + in, c.y } };
        for (int k = 0; k < 4; k++) {
            CATextLayer* t = text_layer(key_label(b->keys[k]), 12, scale);
            center_text(t, at[k], 12);
            [_layers addSublayer:t];
        }
        return;
    }
    if (b->kind == SOULX_KM_SWIPE) {
        CGPoint e = [self pointForX:b->x2 y:b->y2];
        CGMutablePathRef p = CGPathCreateMutable();
        CGPathMoveToPoint(p, NULL, c.x, c.y);
        CGPathAddLineToPoint(p, NULL, e.x, e.y);
        double ang = atan2(e.y - c.y, e.x - c.x);
        for (int s = -1; s <= 1; s += 2) {
            CGPathMoveToPoint(p, NULL, e.x, e.y);
            CGPathAddLineToPoint(p, NULL, e.x - 14 * cos(ang + s * 0.45), e.y - 14 * sin(ang + s * 0.45));
        }
        CAShapeLayer* arrow = [CAShapeLayer layer];
        arrow.path = p;
        arrow.strokeColor = stroke;
        arrow.fillColor = NULL;
        arrow.lineWidth = 3;
        arrow.lineCap = kCALineCapRound;
        CGPathRelease(p);
        [_layers addSublayer:arrow];
        CAShapeLayer* end = [CAShapeLayer layer];
        end.path = CGPathCreateWithEllipseInRect(CGRectMake(e.x - 6, e.y - 6, 12, 12), NULL);
        end.fillColor = fill;
        end.strokeColor = stroke;
        CGPathRelease(end.path);
        [_layers addSublayer:end];
    }
    CAShapeLayer* circle = [CAShapeLayer layer];
    circle.path = CGPathCreateWithEllipseInRect(
        CGRectMake(c.x - TAP_RADIUS_PT, c.y - TAP_RADIUS_PT, 2 * TAP_RADIUS_PT, 2 * TAP_RADIUS_PT), NULL);
    circle.fillColor = fill;
    circle.strokeColor = stroke;
    circle.lineWidth = 2;
    CGPathRelease(circle.path);
    [_layers addSublayer:circle];
    CATextLayer* t = text_layer(key_label(b->keys[0]), 13, scale);
    center_text(t, c, 13);
    [_layers addSublayer:t];
}

- (void)addPromptLayerScale:(CGFloat)scale {
    double fx = _awaitX, fy = _awaitY;
    if (_awaitIndex != NEW_BINDING && _awaitIndex < _edit.count) {
        fx = _edit.b[_awaitIndex].x;
        fy = _edit.b[_awaitIndex].y;
    }
    CGPoint c = [self pointForX:fx y:fy];
    CAShapeLayer* mark = [CAShapeLayer layer];
    mark.path = CGPathCreateWithEllipseInRect(
        CGRectMake(c.x - TAP_RADIUS_PT, c.y - TAP_RADIUS_PT, 2 * TAP_RADIUS_PT, 2 * TAP_RADIUS_PT), NULL);
    mark.fillColor = [NSColor colorWithRed:1 green:0.75 blue:0.2 alpha:0.45].CGColor;
    mark.strokeColor = NSColor.whiteColor.CGColor;
    mark.lineWidth = 2;
    mark.lineDashPattern = @[ @4, @3 ];
    CGPathRelease(mark.path);
    [_layers addSublayer:mark];
    CATextLayer* t = text_layer(SoulXText(@"Press a key…"), 13, scale);
    t.backgroundColor = [NSColor colorWithWhite:0 alpha:0.7].CGColor;
    t.cornerRadius = 5;
    CGFloat ty = c.y + TAP_RADIUS_PT + 14;
    NSRect r = [self image];
    if (ty + 12 > NSMaxY(r)) ty = c.y - TAP_RADIUS_PT - 14;
    t.frame = CGRectMake(c.x - 70, ty - 10, 140, 20);
    [_layers addSublayer:t];
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

- (void)setActiveKeymap:(const SoulXKeymap*)km {
    _hasActive = km != NULL && km->count > 0;
    if (_hasActive) _active = *km;
    else memset(&_active, 0, sizeof(_active));
    if (!_editing) [self relayout];
}

- (void)beginEditing:(const SoulXKeymap*)km package:(NSString*)package {
    _edit = *km;
    _package = [package copy];
    _editing = YES;
    _dragIndex = -1;
    _awaitIndex = -1;
    _allGames.state = package.length == 0 ? NSControlStateValueOn : NSControlStateValueOff;
    _allGames.enabled = package.length > 0;
    _bar.hidden = NO;
    _hint.hidden = NO;
    [self relayout];
    [self.window makeFirstResponder:self];
}

- (void)endEditing:(BOOL)save {
    if (!_editing) return;
    if (save) {
        SoulXKeymap km = _edit;
        const char* pkg = _allGames.state == NSControlStateValueOn ? "" : _package.UTF8String;
        snprintf(km.package, sizeof(km.package), "%s", pkg ? pkg : "");
        if (soulx_keymap_save(&km) != 0) {
            NSAlert* a = [[NSAlert alloc] init];
            a.messageText = SoulXText(@"The keymap could not be saved.");
            a.informativeText = SoulXText(@"See the window log for details.");
            [a beginSheetModalForWindow:self.window completionHandler:nil];
            return;
        }
        [_screen keymapEditorDidSave];
    }
    _editing = NO;
    _dragIndex = -1;
    _awaitIndex = -1;
    _bar.hidden = YES;
    _hint.hidden = YES;
    [self relayout];
    [_screen keymapEditorDidEnd];
}

- (BOOL)keyUsed:(int)key except:(int)index {
    for (int i = 0; i < _edit.count; i++) {
        if (i == index) continue;
        const SoulXKeyBinding* b = &_edit.b[i];
        int n = b->kind == SOULX_KM_JOYSTICK ? 4 : 1;
        for (int k = 0; k < n; k++) {
            if (b->keys[k] == key) return YES;
        }
    }
    return NO;
}

- (void)say:(NSString*)text {
    SoulXScreenView* s = _screen;
    if (s) [s.owner screenView:s message:text];
}

- (int)hitBindingAt:(NSPoint)p part:(int*)part {
    *part = 0;
    for (int i = _edit.count - 1; i >= 0; i--) {
        const SoulXKeyBinding* b = &_edit.b[i];
        CGPoint c = [self pointForX:b->x y:b->y];
        if (b->kind == SOULX_KM_SWIPE) {
            CGPoint e = [self pointForX:b->x2 y:b->y2];
            if (hypot(p.x - e.x, p.y - e.y) <= 6 + HIT_SLOP_PT) {
                *part = 1;
                return i;
            }
        }
        double reach = b->kind == SOULX_KM_JOYSTICK ? [self joystickRadiusPt:b] + HIT_SLOP_PT : TAP_RADIUS_PT + HIT_SLOP_PT;
        if (hypot(p.x - c.x, p.y - c.y) <= reach) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Edit-mode events
// ---------------------------------------------------------------------------

- (void)mouseDown:(NSEvent*)event {
    if (!_editing) return;
    [self.window makeFirstResponder:self];
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (!NSPointInRect(p, [self image])) return;
    int part;
    int i = [self hitBindingAt:p part:&part];
    double fx, fy;
    [self fractionForPoint:p x:&fx y:&fy];
    _awaitIndex = -1;
    if (i >= 0) {
        _dragIndex = i;
        _dragPart = part;
        const SoulXKeyBinding* b = &_edit.b[i];
        _dragDX = (part ? b->x2 : b->x) - fx;
        _dragDY = (part ? b->y2 : b->y) - fy;
    } else {
        _awaitIndex = NEW_BINDING;
        _awaitX = clamp01(fx);
        _awaitY = clamp01(fy);
    }
    [self relayout];
}

- (void)mouseDragged:(NSEvent*)event {
    if (!_editing || _dragIndex < 0 || _dragIndex >= _edit.count) return;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    double fx, fy;
    [self fractionForPoint:p x:&fx y:&fy];
    SoulXKeyBinding* b = &_edit.b[_dragIndex];
    double nx = clamp01(fx + _dragDX), ny = clamp01(fy + _dragDY);
    if (_dragPart == 1) {
        b->x2 = nx;
        b->y2 = ny;
    } else {
        if (b->kind == SOULX_KM_SWIPE) {                     // the whole arrow moves
            b->x2 = clamp01(b->x2 + nx - b->x);
            b->y2 = clamp01(b->y2 + ny - b->y);
        }
        b->x = nx;
        b->y = ny;
    }
    [self relayout];
}

- (void)mouseUp:(NSEvent*)event {
    (void)event;
    if (_dragIndex >= 0) {
        _dragIndex = -1;
        [self relayout];
    }
}

- (void)scrollWheel:(NSEvent*)event {
    if (!_editing) return;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    int part;
    int i = [self hitBindingAt:p part:&part];
    if (i < 0 || _edit.b[i].kind != SOULX_KM_JOYSTICK) return;
    double step = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY * 0.001 : event.scrollingDeltaY * 0.01;
    SoulXKeyBinding* b = &_edit.b[i];
    b->radius = fmin(JOY_MAX_RADIUS, fmax(JOY_MIN_RADIUS, b->radius + step));
    [self relayout];
}

- (NSMenu*)menuForEvent:(NSEvent*)event {
    if (!_editing) return nil;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    int part;
    int i = [self hitBindingAt:p part:&part];
    NSMenu* m = [[NSMenu alloc] initWithTitle:@""];
    if (i < 0) {
        double fx, fy;
        [self fractionForPoint:p x:&fx y:&fy];
        if (fx < 0 || fx > 1 || fy < 0 || fy > 1) return nil;
        NSMenuItem* joy = [m addItemWithTitle:SoulXText(@"Add joystick here") action:@selector(addJoystickHere:)
                                keyEquivalent:@""];
        joy.target = self;
        joy.representedObject = [NSValue valueWithPoint:NSMakePoint(fx, fy)];
        return m;
    }
    const SoulXKeyBinding* b = &_edit.b[i];
    NSMenuItem* del = [m addItemWithTitle:SoulXText(@"Delete") action:@selector(deleteBinding:) keyEquivalent:@""];
    del.target = self;
    del.tag = i;
    if (b->kind != SOULX_KM_JOYSTICK) {
        NSMenuItem* change = [m addItemWithTitle:SoulXText(@"Change key") action:@selector(changeKey:) keyEquivalent:@""];
        change.target = self;
        change.tag = i;
    }
    if (b->kind == SOULX_KM_TAP) {
        NSMenuItem* joy = [m addItemWithTitle:SoulXText(@"Make joystick (WASD)") action:@selector(makeJoystick:)
                                keyEquivalent:@""];
        joy.target = self;
        joy.tag = i;
    } else if (b->kind == SOULX_KM_JOYSTICK) {
        NSMenuItem* arrows = [m addItemWithTitle:SoulXText(@"Use arrow keys") action:@selector(useArrows:)
                                   keyEquivalent:@""];
        arrows.target = self;
        arrows.tag = i;
        NSMenuItem* wasd = [m addItemWithTitle:SoulXText(@"Use WASD") action:@selector(useWasd:) keyEquivalent:@""];
        wasd.target = self;
        wasd.tag = i;
        NSMenuItem* tap = [m addItemWithTitle:SoulXText(@"Make tap") action:@selector(makeTap:) keyEquivalent:@""];
        tap.target = self;
        tap.tag = i;
    }
    return m;
}

- (void)rightMouseDown:(NSEvent*)event {
    if (!_editing) return;
    NSMenu* m = [self menuForEvent:event];
    if (m) [NSMenu popUpContextMenu:m withEvent:event forView:self];
}

- (BOOL)handleEscape {
    if (!_editing) return NO;
    if (_awaitIndex >= 0) {
        _awaitIndex = -1;
        [self relayout];
    } else {
        [self endEditing:NO];
    }
    return YES;
}

- (void)keyDown:(NSEvent*)event {
    if (!_editing) return;
    if (event.modifierFlags & NSEventModifierFlagCommand) {
        [super keyDown:event];
        return;
    }
    if (event.isARepeat) return;
    if (_awaitIndex < 0) {
        if (event.keyCode == 0x35) [self handleEscape];        // kVK_Escape
        else if (event.keyCode == 0x33 || event.keyCode == 0x75) [self say:SoulXText(@"Right-click a binding to delete it.")];
        return;
    }
    if (event.keyCode == 0x35) {                                 // Esc cancels "Press a key…"
        [self handleEscape];
        return;
    }
    [self assignKey:soulx_key_from_mac_keycode(event.keyCode)];
}

- (void)flagsChanged:(NSEvent*)event {
    if (!_editing || _awaitIndex < 0) return;
    int key = soulx_key_from_mac_keycode(event.keyCode);
    NSEventModifierFlags f = event.modifierFlags;
    BOOL down = (f & (NSEventModifierFlagShift | NSEventModifierFlagControl | NSEventModifierFlagOption)) != 0;
    if (key >= 0 && down) [self assignKey:key];
}

- (void)assignKey:(int)key {
    if (key < 0) {
        NSBeep();
        [self say:SoulXText(@"This key cannot be used in a keymap.")];
        return;
    }
    int except = _awaitIndex == NEW_BINDING ? -1 : _awaitIndex;
    if ([self keyUsed:key except:except]) {
        NSBeep();
        [self say:[NSString stringWithFormat:SoulXText(@"Key %@ is already used."), key_label(key)]];
        return;
    }
    if (_awaitIndex == NEW_BINDING) {
        if (_edit.count >= SOULX_KEYMAP_MAX) {
            NSBeep();
            [self say:SoulXText(@"The keymap is full.")];
        } else {
            SoulXKeyBinding* b = &_edit.b[_edit.count++];
            memset(b, 0, sizeof(*b));
            b->kind = SOULX_KM_TAP;
            b->keys[0] = key;
            b->keys[1] = b->keys[2] = b->keys[3] = -1;
            b->x = _awaitX;
            b->y = _awaitY;
        }
    } else if (_awaitIndex < _edit.count) {
        _edit.b[_awaitIndex].keys[0] = key;
    }
    _awaitIndex = -1;
    [self relayout];
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

- (void)deleteBinding:(NSMenuItem*)item {
    int i = (int)item.tag;
    if (i < 0 || i >= _edit.count) return;
    memmove(&_edit.b[i], &_edit.b[i + 1], sizeof(SoulXKeyBinding) * (size_t)(_edit.count - i - 1));
    _edit.count--;
    _awaitIndex = -1;
    [self relayout];
}

- (void)changeKey:(NSMenuItem*)item {
    int i = (int)item.tag;
    if (i < 0 || i >= _edit.count) return;
    _awaitIndex = i;
    [self.window makeFirstResponder:self];
    [self relayout];
}

// Sets a joystick's four keys if none is used by another binding.
- (BOOL)setJoystick:(int)i keys:(const char* const*)names {
    int keys[4];
    for (int k = 0; k < 4; k++) {
        keys[k] = soulx_key_from_name(names[k]);
        if (keys[k] < 0 || [self keyUsed:keys[k] except:i]) {
            NSBeep();
            [self say:[NSString stringWithFormat:SoulXText(@"Key %@ is already used."), @(names[k])]];
            return NO;
        }
    }
    memcpy(_edit.b[i].keys, keys, sizeof(keys));
    return YES;
}

static const char* const k_wasd[4] = { "W", "A", "S", "D" };
static const char* const k_arrows[4] = { "UP", "LEFT", "DOWN", "RIGHT" };

- (void)makeJoystick:(NSMenuItem*)item {
    int i = (int)item.tag;
    if (i < 0 || i >= _edit.count) return;
    SoulXKeyBinding saved = _edit.b[i];
    _edit.b[i].kind = SOULX_KM_JOYSTICK;
    _edit.b[i].radius = JOY_DEFAULT_RADIUS;
    if (![self setJoystick:i keys:k_wasd] && ![self setJoystick:i keys:k_arrows]) _edit.b[i] = saved;
    [self relayout];
}

- (void)makeTap:(NSMenuItem*)item {
    int i = (int)item.tag;
    if (i < 0 || i >= _edit.count) return;
    _edit.b[i].kind = SOULX_KM_TAP;
    _edit.b[i].keys[1] = _edit.b[i].keys[2] = _edit.b[i].keys[3] = -1;
    [self relayout];
}

- (void)useArrows:(NSMenuItem*)item {
    int i = (int)item.tag;
    if (i >= 0 && i < _edit.count) [self setJoystick:i keys:k_arrows];
    [self relayout];
}

- (void)useWasd:(NSMenuItem*)item {
    int i = (int)item.tag;
    if (i >= 0 && i < _edit.count) [self setJoystick:i keys:k_wasd];
    [self relayout];
}

- (void)addJoystickAtX:(double)fx y:(double)fy {
    if (_edit.count >= SOULX_KEYMAP_MAX) {
        NSBeep();
        [self say:SoulXText(@"The keymap is full.")];
        return;
    }
    int i = _edit.count++;
    SoulXKeyBinding* b = &_edit.b[i];
    memset(b, 0, sizeof(*b));
    b->kind = SOULX_KM_JOYSTICK;
    b->x = clamp01(fx);
    b->y = clamp01(fy);
    b->radius = JOY_DEFAULT_RADIUS;
    if (![self setJoystick:i keys:k_wasd] && ![self setJoystick:i keys:k_arrows]) _edit.count--;
    [self relayout];
}

- (void)addJoystick:(id)sender {
    (void)sender;
    [self addJoystickAtX:0.2 y:0.75];
}

- (void)addJoystickHere:(NSMenuItem*)item {
    NSPoint f = [item.representedObject pointValue];
    [self addJoystickAtX:f.x y:f.y];
}

- (void)toggleAllGames:(id)sender {
    (void)sender;
    [self layoutEditChrome];
}

- (void)cancelEditing:(id)sender {
    (void)sender;
    [self endEditing:NO];
}

- (void)saveEditing:(id)sender {
    (void)sender;
    [self endEditing:YES];
}

@end
