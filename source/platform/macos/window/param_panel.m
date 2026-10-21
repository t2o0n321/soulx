// SoulXParamPanel — a small floating panel that mirrors a running script's live parameters.
//
// A script declares controls with param_slider/param_toggle/param_choice/param_number/param_button (see
// source/script/soulx_lua.c, k_ui). Those write tab-separated declaration lines to <dir>/params; this panel polls
// that file, builds a control per line, and writes the current values back to <dir>/values whenever the user edits
// one. The script reads <dir>/values live. Each side writes a different file (script -> params, panel -> values),
// so there is never a write/write race, and every read is best-effort: a missing or malformed file is ignored, so
// a broken script cannot crash the window and a broken panel cannot crash the script.
//
// The <dir> comes from the window via SOULX_UI_DIR when it spawns `soulx run`; the window creates and removes it.

#import "window_internal.h"

enum { PANEL_LABEL_W = 116, PANEL_CONTROL_W = 150, PANEL_MAX_H = 460 };

@interface SoulXParamRow : NSObject
@property (nonatomic, copy) NSString* name;
@property (nonatomic, copy) NSString* kind;
@property (nonatomic, strong) NSControl* control;   // slider / switch / popup / text field / button
@property (nonatomic, strong) NSTextField* readout;  // slider only: shows the number
@property (nonatomic) double step;                   // slider / number
@property (nonatomic) double lo;                     // number: declared min (hi>lo enables clamping)
@property (nonatomic) double hi;                     // number: declared max
@property (nonatomic) NSInteger presses;             // button only
@end
@implementation SoulXParamRow
@end

@interface SoulXParamPanel () <NSWindowDelegate>
@end

@implementation SoulXParamPanel {
    NSString* _dir;
    NSString* _title;
    __weak NSWindow* _host;
    NSPanel* _panel;
    NSStackView* _stack;
    NSTimer* _poll;
    NSString* _lastDecls;                            // raw params text the controls were last built from
    NSMutableArray<SoulXParamRow*>* _rows;
    BOOL _stopping;                                  // YES during programmatic teardown (don't fire onClose)
}

- (instancetype)initWithDir:(NSString*)dir title:(NSString*)title near:(NSWindow*)host {
    if ((self = [super init])) {
        _dir = [dir copy];
        _title = [title copy];
        _host = host;
        _rows = [NSMutableArray array];
    }
    return self;
}

- (void)start {
    if (_poll) return;
    _poll = [NSTimer scheduledTimerWithTimeInterval:0.3 repeats:YES block:^(NSTimer* t) {
        (void)t;
        [self poll];
    }];
    [self poll];
}

- (void)stop {
    _stopping = YES;                                // this close is programmatic: don't treat it as "user closed"
    [_poll invalidate];
    _poll = nil;
    _panel.delegate = nil;
    [_panel close];
    _panel = nil;
    [_rows removeAllObjects];
    _lastDecls = nil;
}

// The user clicked the panel's close button: a GUI script's window closing means "stop the script".
- (void)windowWillClose:(NSNotification*)note {
    (void)note;
    if (_stopping) return;
    void (^cb)(void) = self.onClose;
    if (cb) cb();
}

// --- declarations -> controls ------------------------------------------------

- (void)poll {
    NSString* text = [NSString stringWithContentsOfFile:[_dir stringByAppendingPathComponent:@"params"]
                                               encoding:NSUTF8StringEncoding error:NULL];
    if (text.length == 0) return;                   // nothing declared yet
    if ([text isEqualToString:_lastDecls]) return;  // unchanged
    _lastDecls = [text copy];
    [self rebuild:text];
}

- (void)ensurePanel {
    if (_panel) return;
    NSRect frame = NSMakeRect(0, 0, PANEL_LABEL_W + PANEL_CONTROL_W + 60, 80);
    _panel = [[NSPanel alloc] initWithContentRect:frame
                                        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                   NSWindowStyleMaskUtilityWindow)
                                          backing:NSBackingStoreBuffered defer:NO];
    _panel.title = _title;
    _panel.delegate = self;                             // so closing the panel stops the script
    _panel.hidesOnDeactivate = NO;
    _panel.becomesKeyOnlyIfNeeded = YES;            // editing a text field can focus it, but it won't steal focus
    _panel.releasedWhenClosed = NO;
    // NSUtilityWindow panels default to floating (see _logPanel below for the same fix): say so explicitly, or
    // it stays pinned above every window — including other apps' — instead of ordering normally.
    _panel.floatingPanel = NO;

    _stack = [[NSStackView alloc] init];
    _stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    _stack.alignment = NSLayoutAttributeLeading;
    _stack.spacing = 10;
    _stack.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);
    _stack.translatesAutoresizingMaskIntoConstraints = NO;
    [_panel.contentView addSubview:_stack];
    [NSLayoutConstraint activateConstraints:@[
        [_stack.leadingAnchor constraintEqualToAnchor:_panel.contentView.leadingAnchor],
        [_stack.trailingAnchor constraintEqualToAnchor:_panel.contentView.trailingAnchor],
        [_stack.topAnchor constraintEqualToAnchor:_panel.contentView.topAnchor],
        [_stack.bottomAnchor constraintEqualToAnchor:_panel.contentView.bottomAnchor],
    ]];
}

- (void)rebuild:(NSString*)text {
    [self ensurePanel];
    for (NSView* v in _stack.arrangedSubviews.copy) [_stack removeArrangedSubview:v], [v removeFromSuperview];
    [_rows removeAllObjects];

    for (NSString* line in [text componentsSeparatedByString:@"\n"]) {
        if (line.length == 0) continue;
        NSArray<NSString*>* f = [line componentsSeparatedByString:@"\t"];
        if (f.count < 3) continue;
        NSString* kind = f[0], *name = f[1], *label = f[2];
        SoulXParamRow* row = [[SoulXParamRow alloc] init];
        row.name = name;
        row.kind = kind;
        NSView* control = nil;

        if ([kind isEqualToString:@"slider"] && f.count >= 7) {
            double def = f[3].doubleValue, lo = f[4].doubleValue, hi = f[5].doubleValue;
            row.step = f[6].doubleValue > 0 ? f[6].doubleValue : 1;
            NSSlider* s = [NSSlider sliderWithValue:def minValue:lo maxValue:hi target:self
                                             action:@selector(sliderChanged:)];
            s.continuous = YES;
            row.control = s;
            row.readout = [self readoutLabel:[self formatNumber:def step:row.step]];
            control = [self rowWith:label control:s trailing:row.readout];
        } else if ([kind isEqualToString:@"number"] && f.count >= 4) {
            double def = f[3].doubleValue;
            if (f.count >= 6) { row.lo = f[4].doubleValue; row.hi = f[5].doubleValue; }
            row.step = (f.count >= 7 && f[6].doubleValue > 0) ? f[6].doubleValue : 0;
            NSTextField* tf = [NSTextField textFieldWithString:[self formatNumber:def step:row.step]];
            tf.target = self;
            tf.action = @selector(fieldChanged:);
            row.control = tf;
            control = [self rowWith:label control:tf trailing:nil];
        } else if ([kind isEqualToString:@"toggle"] && f.count >= 4) {
            NSSwitch* sw = [[NSSwitch alloc] init];
            sw.state = f[3].integerValue ? NSControlStateValueOn : NSControlStateValueOff;
            sw.target = self;
            sw.action = @selector(toggleChanged:);
            row.control = sw;
            control = [self rowWith:label control:sw trailing:nil];
        } else if ([kind isEqualToString:@"choice"] && f.count >= 4) {
            NSPopUpButton* pop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
            for (NSUInteger i = 4; i < f.count; i++) [pop addItemWithTitle:f[i]];
            [pop selectItemWithTitle:f[3]];
            if (!pop.selectedItem && pop.numberOfItems) [pop selectItemAtIndex:0];
            pop.target = self;
            pop.action = @selector(popupChanged:);
            row.control = pop;
            control = [self rowWith:label control:pop trailing:nil];
        } else if ([kind isEqualToString:@"button"]) {
            NSButton* b = [NSButton buttonWithTitle:label target:self action:@selector(buttonPressed:)];
            b.bezelStyle = NSBezelStyleRounded;
            row.control = b;
            control = [self rowWith:label control:b trailing:nil usingControlAsLabel:YES];
        } else {
            continue;
        }
        if (row.control) row.control.tag = (NSInteger)_rows.count;
        [_rows addObject:row];
        [_stack addArrangedSubview:control];
    }

    if (_rows.count == 0) { [_panel close]; _panel = nil; _lastDecls = nil; return; }
    [self sizeAndShow];
    [self writeValues];                             // publish the initial (default) state
}

// label | control [ | trailing ]
- (NSView*)rowWith:(NSString*)label control:(NSView*)control trailing:(NSView*)trailing {
    return [self rowWith:label control:control trailing:trailing usingControlAsLabel:NO];
}

- (NSView*)rowWith:(NSString*)label control:(NSView*)control trailing:(NSView*)trailing
    usingControlAsLabel:(BOOL)controlIsLabel {
    NSStackView* h = [[NSStackView alloc] init];
    h.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    h.alignment = NSLayoutAttributeCenterY;
    h.spacing = 10;
    if (!controlIsLabel) {
        NSTextField* l = [NSTextField labelWithString:label];
        l.alignment = NSTextAlignmentRight;
        l.textColor = NSColor.secondaryLabelColor;
        [l.widthAnchor constraintEqualToConstant:PANEL_LABEL_W].active = YES;
        l.lineBreakMode = NSLineBreakByTruncatingTail;
        [h addArrangedSubview:l];
    }
    [control.widthAnchor constraintGreaterThanOrEqualToConstant:controlIsLabel ? 120 : PANEL_CONTROL_W].active = YES;
    [h addArrangedSubview:control];
    if (trailing) [h addArrangedSubview:trailing];
    return h;
}

- (NSTextField*)readoutLabel:(NSString*)text {
    NSTextField* r = [NSTextField labelWithString:text];
    r.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
    r.textColor = NSColor.secondaryLabelColor;
    r.alignment = NSTextAlignmentRight;
    [r.widthAnchor constraintGreaterThanOrEqualToConstant:36].active = YES;
    return r;
}

- (void)sizeAndShow {
    [_panel.contentView layoutSubtreeIfNeeded];
    NSSize fit = _stack.fittingSize;
    // grow to fit the controls, up to nearly the screen height (so a param-heavy script does not get clipped)
    NSScreen* screen = _panel.screen ? _panel.screen : NSScreen.mainScreen;
    CGFloat maxH = screen ? fmax((CGFloat)PANEL_MAX_H, screen.visibleFrame.size.height - 120) : (CGFloat)PANEL_MAX_H;
    CGFloat h = fmin(fit.height, maxH);
    NSRect content = NSMakeRect(0, 0, fmax(fit.width, PANEL_LABEL_W + PANEL_CONTROL_W + 60), h);
    [_panel setContentSize:content.size];
    if (!_panel.isVisible) {
        // dock to the right of the host window
        NSWindow* host = _host;
        if (host) {
            NSRect hf = host.frame, pf = _panel.frame;
            NSPoint origin = NSMakePoint(NSMaxX(hf) + 12, NSMaxY(hf) - pf.size.height);
            [_panel setFrameOrigin:origin];
        } else {
            [_panel center];
        }
        [_panel orderFront:nil];
    }
}

// --- edits -> values ---------------------------------------------------------

- (SoulXParamRow*)rowForSender:(id)sender {
    NSInteger i = [sender tag];
    return (i >= 0 && i < (NSInteger)_rows.count) ? _rows[(NSUInteger)i] : nil;
}

- (void)sliderChanged:(id)sender {
    SoulXParamRow* row = [self rowForSender:sender];
    if (row) row.readout.stringValue = [self formatNumber:[sender doubleValue] step:row.step];
    [self writeValues];
}
- (void)fieldChanged:(id)sender { (void)sender; [self writeValues]; }
- (void)toggleChanged:(id)sender { (void)sender; [self writeValues]; }
- (void)popupChanged:(id)sender { (void)sender; [self writeValues]; }
- (void)buttonPressed:(id)sender {
    SoulXParamRow* row = [self rowForSender:sender];
    if (row) { row.presses++; [self writeValues]; }
}

- (NSString*)formatNumber:(double)v step:(double)step {
    double r = v;
    if (step > 0) r = round(v / step) * step;
    if (fabs(r - round(r)) < 1e-9) return [NSString stringWithFormat:@"%lld", (long long)llround(r)];
    return [NSString stringWithFormat:@"%g", r];
}

- (void)writeValues {
    NSMutableString* out = [NSMutableString string];
    for (SoulXParamRow* row in _rows) {
        NSString* v = nil;
        if ([row.kind isEqualToString:@"slider"]) {
            v = [self formatNumber:[(NSSlider*)row.control doubleValue] step:row.step];
        } else if ([row.kind isEqualToString:@"number"]) {
            double nv = [(NSTextField*)row.control doubleValue];
            if (row.hi > row.lo) nv = fmax(row.lo, fmin(row.hi, nv));   // keep within the declared range
            v = [self formatNumber:nv step:row.step];
            ((NSTextField*)row.control).stringValue = v;                // reflect any clamp back to the field
        } else if ([row.kind isEqualToString:@"toggle"]) {
            v = [(NSSwitch*)row.control state] == NSControlStateValueOn ? @"1" : @"0";
        } else if ([row.kind isEqualToString:@"choice"]) {
            NSString* title = [(NSPopUpButton*)row.control titleOfSelectedItem];
            v = title ? title : @"";
        } else if ([row.kind isEqualToString:@"button"]) {
            v = [NSString stringWithFormat:@"%ld", (long)row.presses];
        }
        if (v) {
            // newlines/tabs would corrupt the line format; a control value never legitimately holds them
            v = [[v componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet] componentsJoinedByString:@" "];
            [out appendFormat:@"%@\t%@\n", row.name, v];
        }
    }
    NSData* data = [out dataUsingEncoding:NSUTF8StringEncoding];
    [data writeToFile:[_dir stringByAppendingPathComponent:@"values"] atomically:YES];  // temp + rename = atomic
}

@end
