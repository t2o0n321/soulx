/*
 * SoulX Android Emulator - game key mapping (keyboard -> touches) for SoulX's window
 *
 * Keymap files live in $SOULX_HOME/keymaps/<package>.keymap (default.keymap applies when no per-game file exists).
 * One binding per line, '#' starts a comment, coordinates are fractions (0..1) of the LOGICAL screen (the upright
 * screen the player sees), so a keymap survives resolution changes:
 *
 *   tap      <key> <x> <y>                                   hold the key = hold the finger
 *   joystick <up> <left> <down> <right> <cx> <cy> <radius>   radius = fraction of the screen's shorter side
 *   swipe    <key> <x1> <y1> <x2> <y2> <ms>                  one swipe per key press
 *
 * Key names (case-insensitive): A-Z 0-9 F1-F12 SPACE ENTER TAB ESC BACKSPACE UP DOWN LEFT RIGHT
 * LSHIFT RSHIFT LCTRL RCTRL LALT RALT MINUS EQUAL LBRACKET RBRACKET BACKSLASH SEMICOLON QUOTE COMMA PERIOD
 * SLASH GRAVE
 *
 * Fingers 1..8 are reserved for the keymap (finger 0 = mouse, finger 9 = scroll-wheel emulation).
 */

#ifndef SOULX_KEYMAP_H
#define SOULX_KEYMAP_H

#include <stdbool.h>
#include <stddef.h>

#define SOULX_KEYMAP_MAX           64
#define SOULX_KEYMAP_FIRST_FINGER  1
#define SOULX_KEYMAP_LAST_FINGER   8

typedef enum {
    SOULX_KM_TAP = 0,
    SOULX_KM_JOYSTICK,
    SOULX_KM_SWIPE
} SoulXKeymapKind;

typedef struct {
    SoulXKeymapKind kind;
    int keys[4];                 // tap/swipe: keys[0]; joystick: up, left, down, right (SoulXKey codes)
    double x, y;                 // tap point / joystick centre / swipe start (0..1)
    double x2, y2;               // swipe end (0..1)
    double radius;               // joystick radius (0..0.5 of the shorter side)
    int ms;                      // swipe duration in ms (1..5000)
} SoulXKeyBinding;

typedef struct {
    char package[256];           // "" for default.keymap
    SoulXKeyBinding b[SOULX_KEYMAP_MAX];
    int count;
} SoulXKeymap;

// SoulX key codes (independent of macOS virtual key codes)
int  soulx_key_from_name(const char* name);                 // -1 when unknown
const char* soulx_key_name(int key);                        // canonical upper-case name, NULL when invalid
int  soulx_key_from_mac_keycode(unsigned short keycode);    // kVK_* (Carbon) -> SoulXKey, -1 when not mappable
int  soulx_key_hid_usage(int key);                          // SoulXKey -> HID usage (page 0x07), -1 when none

// Files
int  soulx_keymap_parse(const char* text, SoulXKeymap* km, char* err, size_t errcap);   // 0 ok; err names the line
int  soulx_keymap_format(const SoulXKeymap* km, char* buf, size_t cap);                // 0 ok
int  soulx_keymap_load(const char* package, SoulXKeymap* km);   // 0 = per-game file, 1 = default.keymap, 2 = none, -1 error
int  soulx_keymap_save(const SoulXKeymap* km);                  // atomic write of keymaps/<package or default>.keymap

// Runtime: key events -> touch actions
typedef enum {
    SOULX_KM_ACT_TOUCH = 0,       // finger DOWN / MOVE / UP at (x, y)
    SOULX_KM_ACT_SWIPE            // one swipe (x, y) -> (x2, y2) in ms on the given finger
} SoulXKeymapActionKind;

typedef struct {
    SoulXKeymapActionKind kind;
    int finger;                  // SOULX_KEYMAP_FIRST_FINGER..SOULX_KEYMAP_LAST_FINGER
    int action;                  // SOULX_TOUCH_DOWN / SOULX_TOUCH_MOVE / SOULX_TOUCH_UP (touch only)
    double x, y;                 // fractions of the logical screen
    double x2, y2;               // swipe end
    int ms;                      // swipe duration
} SoulXKeymapAction;

typedef struct SoulXKeymapRuntime SoulXKeymapRuntime;

SoulXKeymapRuntime* soulx_keymap_runtime_new(const SoulXKeymap* km);   // copies km
void soulx_keymap_runtime_free(SoulXKeymapRuntime* rt);
bool soulx_keymap_binds(const SoulXKeymapRuntime* rt, int key);      // the key is used by some binding
int  soulx_keymap_key_event(SoulXKeymapRuntime* rt, int key, bool down, SoulXKeymapAction* out, int max);
                                                   // actions to perform now (0 when the key is not bound or nothing
                                                   // changes, e.g. key repeat); joystick: DOWN at the centre then MOVE
                                                   // to centre + radius * direction, MOVE when the direction changes,
                                                   // UP when all its keys are released
int  soulx_keymap_release_all(SoulXKeymapRuntime* rt, SoulXKeymapAction* out, int max);   // window lost focus

#endif // SOULX_KEYMAP_H
