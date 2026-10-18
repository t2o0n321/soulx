# SoulX scripting guide (Lua 5.4)

SoulX has a built-in script engine that plays games for you: it taps, swipes, presses keys,
looks at the screen and reacts to what it sees. Scripts are plain [Lua 5.4](https://www.lua.org/manual/5.4/)
files with a small set of extra functions:

```lua
launch("com.example.game")
wait("img/start.png", 30)          -- wait up to 30 s for the start button
tap("img/start.png")               -- and tap it
for i = 1, 50 do
    swipe(300, 1500, 800, 1500, 200)
    tap(540, 1650)
    sleep(800 + math.random(200))
    if color(1000, 80) == 0xFF2D2D then tap(980, 1700) end
end
```

```sh
soulx run farm.lua
```

Nothing else needs to be installed: the Lua interpreter is part of `soulx`.

**Contents**

1. [Your first script in 5 minutes](#1-your-first-script-in-5-minutes)
2. [How scripts run](#2-how-scripts-run)
3. [Coordinates and orientation](#3-coordinates-and-orientation)
4. [API reference](#4-api-reference)
5. [Image matching guide](#5-image-matching-guide)
6. [Record and replay](#6-record-and-replay)
7. [Running headless and for a long time](#7-running-headless-and-for-a-long-time)
8. [Stopping a script (Ctrl+C)](#8-stopping-a-script-ctrlc)
9. [Errors and troubleshooting](#9-errors-and-troubleshooting)
10. [Limitations](#10-limitations)

Complete example scripts are in [examples/scripts/](../examples/scripts/).

---

## 1. Your first script in 5 minutes

**1. Start an instance and open the game.**

```sh
soulx play mygame.apk          # creates and starts the instance, installs the game, launches it
# or, step by step:
soulx start
soulx launch com.example.game
```

**2. Take a screenshot** to find out where things are:

```sh
soulx screenshot shot.png
```

Script coordinates are pixels of this screenshot: `x` from the left edge, `y` from the top
edge. To read a position, open the PNG in an image editor that shows the cursor position. In
Preview, drag a selection from the top-left corner of the image to the point: the selection
size shown while dragging is the point's `x × y`.

**3. Write `first.lua`:**

```lua
local w, h = screen_size()
log("the screen is", w, "x", h)

tap(w // 2, h // 2)                  -- tap the middle of the screen
sleep(1000)                          -- wait one second
screenshot("after.png")              -- saved next to the script
log("done")
```

**4. Run it:**

```sh
soulx run first.lua
```

```
soulx: running first.lua on default (Ctrl+C stops it)
[0.0s] the screen is 1080 x 2400
[1.3s] done
script finished in 1.3 s
```

**5. React to the screen.** Cut a button out of `shot.png` and save it as `img/start.png`
next to the script (or let SoulX do it: `screenshot("img/start.png", 440, 1610, 200, 80)` saves
that rectangle of the screen). Then:

```lua
if wait("img/start.png", 20) then    -- wait up to 20 s until the button is visible
    tap("img/start.png")
else
    log("no start button")
end
```

**6. Repeat it.** Put the steps in a loop, add a little randomness, and stop the script with
Ctrl+C whenever you like:

```lua
for round = 1, 100 do
    if tap("img/start.png") then
        log("round", round)
    end
    sleep(1500 + math.random(500))
end
```

That is all there is to it. The rest of this guide is reference.

---

## 2. How scripts run

```sh
soulx run [-i NAME] [--repeat N | --loop] [--speed X] script.lua [args...]
```

- **Repeat**: `--repeat N` runs the whole script `N` times in a row (1–100000); `--loop` runs it
  over and over until you stop it (Ctrl+C). Each pass starts with a fresh Lua state, so globals do
  not carry over — persist state with a file or the `args` you pass in if you need it. An error, a
  Ctrl+C, or `os.exit()` ends the whole run immediately. Inside the script, `iteration()` returns
  the current pass and the total (see below). In SoulX's window, the **執行腳本 / Run script** button
  asks for the number of times (or “repeat until I stop it”) and the speed before it runs.
- **Speed**: `--speed X` (0.1–10, default 1) divides every `sleep()` and every recorded replay
  pause by `X`, so `--speed 2` replays twice as fast and `--speed 0.5` at half speed. It changes
  only the pauses your script waits — it does not shorten a `wait()` image timeout, and it cannot
  make the game itself run faster. A script can read or change it at runtime with `speed()`.
- **Instance**: the script controls the instance given with `-i NAME` (default: the
  `SOULX_INSTANCE` environment variable, else `default`). It must be running (`soulx start`).
  `use("other")` switches to another running instance from inside the script.
- **Script folder**: relative paths given to SoulX's functions (templates, `load_image`, `screenshot`,
  `img:save`, `install`) are relative to the folder that contains the script, wherever you run
  `soulx` from. SoulX never changes the working directory, so Lua's own `io.open`, `dofile` and
  `loadfile` still resolve relative paths against the current directory. `require("helpers")`
  finds `helpers.lua` (or `helpers/init.lua`) next to the script.
- **Arguments**: the words after the script name are in the global table `args`
  (`args[1]`, `args[2]`, ...; `args[0]` is the script path; `#args` is their number). They are
  also the script's `...` and, as in the standard `lua` interpreter, the table `arg`:

  ```lua
  -- soulx run farm.lua 30 fast
  local rounds = tonumber(args[1]) or 10     -- 30
  local mode = args[2] or "normal"           -- "fast"
  ```

- **Output**: `print(...)` writes to standard output. `log(...)` writes to standard error with
  the time since the start: `[12.3s] message`. SoulX's own messages (start, end, errors) go to
  standard error too, so `soulx run farm.lua > results.txt` collects only what the script prints.
- **End of the script**: SoulX prints one line and exits with a code:

  | Outcome | Message (standard error) | Exit code |
  |---|---|---|
  | The script ran to its end (or called `os.exit()` / `os.exit(0)`) | `script finished in 12.3 s` (or `finished 3 passes in …` with `--repeat`) | 0 |
  | An error | `script error: farm.lua:12: <message>` and a stack traceback | 1 |
  | Ctrl+C | `script stopped (Ctrl+C) after 12.3 s` | 130 |
  | `os.exit(n)` with n ≠ 0 | `script exited with code n after 12.3 s` | n |
  | Instance missing or not running, script not readable | `soulx: error: ...` | 1 |

- **One script per instance**: two scripts on the same instance fight over the touchscreen.
  Run one script per instance; scripts on different instances run independently.
- **Lua**: the complete Lua 5.4 standard library is available (`string`, `table`, `math`,
  `os.time`, `os.date`, `io.open`, coroutines, ...), with three changes:
  - `os.exit([code])` ends the script (not the `soulx` process) with that exit code
    (`true` = 0, `false` = 1, else 0–255);
  - `os.execute` and `io.popen` are disabled, and `package.loadlib`, `package.cpath` and the C
    module searchers are removed: scripts never run programs or load native code on your Mac
    (`os.execute()` without arguments returns `false`). Use `shell("...")` to run a command inside
    Android. `require` still loads `.lua` modules next to the script. (Lua's `io` and `os.remove`/
    `os.rename` can still read and write your files: run only scripts you trust.);
  - `pcall`, `xpcall` and `coroutine.resume` cannot catch the Ctrl+C stop request or `os.exit()`.

  `math.random` is seeded differently on every run.
- **`soulx` table**: every function below is also available as `soulx.<name>`
  (`soulx.tap(10, 20)`), which helps when a local variable hides a global one
  (`local key = ...` hides `key()` in its scope). `soulx.version` is SoulX's version.

---

## 3. Coordinates and orientation

- Coordinates are **logical screen pixels**, exactly the pixels of `soulx screenshot`,
  `screenshot()` and `capture()`: `(0, 0)` is the top-left corner, `x` grows to the right and
  `y` downwards. A default instance is 1080 × 2400 in portrait.
- `screen_size()` returns the current width and height.
- Coordinates follow the **current orientation**. When a game turns the screen to landscape,
  screenshots become landscape (2400 × 1080) and so do coordinates; SoulX converts them for the
  touchscreen itself. If your game rotates, read `screen_size()` instead of hard-coding the size.
- **Landscape games**: give them a landscape instance, `soulx create racing --landscape`
  (`soulx play` does this by itself when the APK asks for landscape). Do not use the rotate
  button of the emulator window: it rotates the emulator's view without asking Android.
- Numbers are rounded to the nearest pixel (`tap(w / 3, h / 2)` is fine).
- A point outside the screen is an error, with the screen size in the message:
  `tap: point (1200, 80) is outside the screen (1080x2400)`.
- Coordinates and templates belong to a resolution. A script written on a 1080 × 2400 instance
  needs other coordinates on a 720 × 1600 one (`--profile lite`). Either keep your instances at
  one resolution or scale (see `P()` in [tap_loop.lua](../examples/scripts/tap_loop.lua)).

---

## 4. API reference

Conventions: `x, y, w, h` are screen pixels; `ms` is milliseconds; `image` is a template file
name (PNG or JPEG, relative to the script folder or absolute) or an [Image](#image-objects);
`opts` is an optional [options table](#matching-options). Arguments are checked: a wrong type, a
value out of range or an unknown option raises an error naming the function and the argument.

| Group | Functions |
|---|---|
| [Apps](#apps) | `launch`, `stop_app`, `install`, `current_app` |
| [Touch](#touch) | `tap`, `long_press`, `swipe`, `touch_down`, `touch_move`, `touch_up` |
| [Keys and text](#keys-and-text) | `key`, `text` |
| [Time and output](#time-and-output) | `sleep`, `speed`, `iteration`, `millis`, `log`, `print` |
| [Screen](#screen) | `screen_size`, `color`, `is_color`, `capture`, `find`, `find_all`, `exists`, `wait`, `wait_gone`, `screenshot` |
| [Image objects](#image-objects) | `img:size`, `img:color`, `img:find`, `img:find_all`, `img:crop`, `img:save` |
| [Instances and low level](#instances-and-low-level) | `use`, `console`, `shell` |

### Apps

#### `launch(package)`
Starts an installed app (its launcher activity) and waits until it is shown, typically 1–2 s.
Raises an error when the app cannot be started (not installed, no launcher activity).

```lua
launch("com.example.game")
```

#### `stop_app(package)`
Force-stops an app (`am force-stop`). Stopping an app that is not running is not an error.

#### `install(path [, reinstall])` → package name
Installs an `.apk`, `.xapk`, `.apks` or `.apkm` file, or a folder of split APKs; OBB files of an
XAPK are copied too. `path` is relative to the script folder. When the same version is
already installed nothing is done, unless `reinstall` is `true`. Returns the package name.
Raises an error when the installation fails (the reason, as reported by Android, is printed
just before it).

```lua
local pkg = install("game.xapk")
launch(pkg)
```

#### `current_app()` → package name or `nil`
The package of the app in the foreground, e.g. to restart the game when it crashed:

```lua
if current_app() ~= "com.example.game" then launch("com.example.game") end
```

### Touch

#### `tap(x, y)` → `true`
Taps the point (finger down for about 40 ms).

#### `tap(image [, opts])` → `true` or `false`
Looks for `image` on the screen and taps the centre of the best match. Returns `true` when it
was found and tapped, `false` when it is not on the screen (that is not an error). A template
file that does not exist is an error.

```lua
if not tap("img/close.png") then log("no pop-up to close") end
tap("img/ok.png", {region = {0, 1200, 1080, 1200}})     -- only look in the lower half
```

#### `long_press(x, y [, ms])`
Touches the point for `ms` milliseconds (default 800, at most 60000).

#### `swipe(x1, y1, x2, y2 [, ms])`
Drags a finger from `(x1, y1)` to `(x2, y2)` in `ms` milliseconds (default 300, 1–60000), with a
movement every ~16 ms. Short durations flick, long ones drag.

```lua
swipe(900, 1200, 100, 1200, 150)    -- quick swipe to the left
swipe(540, 1800, 540, 600, 1500)    -- slow drag upwards
```

#### `touch_down(x, y [, finger])`, `touch_move(x, y [, finger])`, `touch_up([finger])`
Low-level touches for gestures that `tap`/`swipe` cannot do: holding, dragging along a path and
multi-touch. `finger` is 0–9 (default 0); each finger is independent. `touch_down` on a finger
that is already down and `touch_move` on a finger that is not down are errors; `touch_up`
returns `false` when that finger was not down. Fingers still down when the script ends (or
stops, or fails) are lifted automatically.

```lua
-- pinch to zoom out: two fingers move towards each other
touch_down(300, 1000, 0)
touch_down(780, 1400, 1)
for i = 1, 10 do
    touch_move(300 + i * 20, 1000 + i * 15, 0)
    touch_move(780 - i * 20, 1400 - i * 15, 1)
    sleep(16)
end
touch_up(0)
touch_up(1)

-- hold a joystick to the right for 2 seconds
touch_down(250, 1900)
touch_move(350, 1900)
sleep(2000)
touch_up()
```

### Keys and text

#### `key(name_or_code)`
Presses and releases a key. Names are case-insensitive Android key names, with or without the
`KEYCODE_` prefix:

| Kind | Names |
|---|---|
| Navigation | `BACK`, `HOME`, `APP_SWITCH` (or `RECENTS`), `MENU`, `SEARCH`, `NOTIFICATION`, `SETTINGS` |
| Editing | `ENTER` (or `RETURN`), `DEL` (or `BACKSPACE`), `FORWARD_DEL`, `TAB`, `SPACE`, `ESCAPE` (or `ESC`), `MOVE_HOME`, `MOVE_END`, `PAGE_UP`, `PAGE_DOWN`, `INSERT`, `COPY`, `PASTE`, `CUT` |
| D-pad | `DPAD_UP`, `DPAD_DOWN`, `DPAD_LEFT`, `DPAD_RIGHT`, `DPAD_CENTER` (or `UP`, `DOWN`, `LEFT`, `RIGHT`, `CENTER`) |
| Hardware | `POWER`, `VOLUME_UP`, `VOLUME_DOWN`, `VOLUME_MUTE`, `CAMERA`, `SLEEP`, `WAKEUP` |
| Letters, digits, F-keys | `A` … `Z`, `KEYCODE_0` … `KEYCODE_9`, `F1` … `F12`, `NUMPAD_0` … `NUMPAD_9` |
| Game pads | `BUTTON_A`, `BUTTON_B`, `BUTTON_X`, `BUTTON_Y`, `BUTTON_L1`, `BUTTON_R1`, `BUTTON_L2`, `BUTTON_R2`, `BUTTON_START`, `BUTTON_SELECT`, `BUTTON_THUMBL`, `BUTTON_THUMBR`, `BUTTON_MODE` |
| Media | `MEDIA_PLAY_PAUSE`, `MEDIA_PLAY`, `MEDIA_PAUSE`, `MEDIA_STOP`, `MEDIA_NEXT`, `MEDIA_PREVIOUS` |

A number is an Android key code (1–999), e.g. `key(4)` = `key("BACK")`. Each key takes about
60 ms. An unknown name is an error.

#### `text(str)`
Types text into the focused field, like a keyboard. ASCII only (letters, digits, punctuation,
space), up to 1000 characters; `"\n"` presses Enter and `"\t"` Tab. Long strings are typed in short
bursts (the emulator's keyboard drops a command longer than about 15 key presses), so a whole
sentence arrives in full but takes a few milliseconds per character.

```lua
tap(540, 900)               -- focus the name field
text("player1\n")
```

> The very first time you focus a text field on a brand-new instance, Gboard may show a "Try out
> your stylus" sheet that swallows the first `text()`. Dismiss it once (tap **Cancel** or
> `key("BACK")`) and it does not come back. Tap the field and check it is focused before typing.

### Time and output

#### `sleep(ms)`
Pauses the script (0 to 86400000 ms). Ctrl+C interrupts it immediately. The actual pause is
divided by the playback speed (`--speed` / `speed()`), so at `--speed 2` a `sleep(1000)` waits
500 ms.

#### `speed([x])` → number
Returns the current playback speed. With an argument (0.1–10) it sets the speed for the rest of
this pass and returns the new value, so a script can slow down a tricky section and speed back up:

```lua
speed(0.5)                      -- careful: half speed through the menu
tap("img/settings.png"); sleep(400)
speed(2)                        -- then fast through the grind
```

The speed resets to the `--speed` value (default 1) at the start of each `--repeat` / `--loop` pass.

#### `iteration()` → pass, total
Returns the current repeat pass (1-based) and the total requested with `--repeat` (0 while
`--loop` runs until stopped). Useful to do something only on the first pass:

```lua
local pass, total = iteration()
if pass == 1 then launch("com.example.game"); wait("img/home.png", 60) end
print(("pass %d/%s"):format(pass, total == 0 and "∞" or total))
```

#### `millis()` → integer
Milliseconds since the script started. Useful for time limits:

```lua
local stop_at = millis() + 30 * 60 * 1000       -- play for 30 minutes
while millis() < stop_at do
    ...
end
```

#### `log(...)`
Writes its arguments (converted like `tostring`, separated by spaces) to standard error with
the time since the start: `log("round", 4, "won")` → `[12.3s] round 4 won`. When SoulX is used
as a library, the line also goes to the registered log callback.

#### `print(...)`
Standard Lua `print`: tab-separated values on standard output.

### Screen

All reading functions work on a capture of the screen in [logical orientation](#3-coordinates-and-orientation).

**Screen cache.** `color`, `is_color`, `find`, `find_all`, `exists` and `tap(image)` reuse the
last capture while it is younger than 100 ms, so several checks in a row cost one capture:

```lua
if is_color(100, 200, 0xFFFFFF) and is_color(300, 200, 0xFFFFFF) then ... end   -- one capture
```

Every action (tap, swipe, key, text, launch, `shell`, `console`, ...) drops the cached capture,
so a check after an action always sees a new screen. `capture()` and `screenshot()` always
capture. `wait()` and `wait_gone()` look once (reusing a capture younger than 100 ms) and then
capture a new screen every 200 ms or so.

#### `screen_size()` → `w, h`
The current screen size in logical pixels (follows the orientation).

#### `color(x, y)` → `0xRRGGBB`
The colour of a pixel as a number; compare it with a hexadecimal literal:

```lua
if color(1000, 80) == 0xFF2D2D then tap(980, 1700) end
log(string.format("%06X", color(10, 10)))          -- prints e.g. 1E90FF
```

#### `is_color(x, y, rgb [, tolerance])` → boolean
`true` when each of red, green and blue differs from `rgb` by at most `tolerance` (0–255,
default 16). Safer than `==` because colours vary slightly (gradients, compression, shading).

#### `capture()` → Image
Captures the screen now and returns it as an [Image](#image-objects) (it also becomes the
cached screen). Keep it when you want to examine one moment of the screen several times, or
to save parts of it as templates.

#### `load_image(path)` → Image
Loads a PNG or JPEG file as an [Image](#image-objects) — a reference picture, a template, or a saved screenshot to
test recognition on. `path` is relative to the script folder (or absolute). Use `img:color` / `img:find` / `img:crop`
on it exactly like a `capture()`.

#### `find(image [, opts])` → `x, y, score` or `nil`
Looks for `image` on the screen. Returns the centre of the best match and its score (0–1,
1 = identical), or `nil` when there is no match above the threshold.

```lua
local x, y, score = find("img/coin.png")
if x then
    log("coin at", x, y, "score", score)
    tap(x, y)
end
```

#### `find_all(image [, opts])` → array
Every match, best first, as `{x = ..., y = ..., score = ...}` tables (at most 256;
overlapping matches of the same object are reported once). An empty table when there is none.

```lua
for _, m in ipairs(find_all("img/coin.png", {threshold = 0.9})) do
    tap(m.x, m.y)
    sleep(300)
end
```

#### `exists(image [, opts])` → boolean
`true` when `find` would find it.

#### `read_text([region])` → string or `nil`
On-device OCR (the macOS **Vision** framework — local, no network) of the whole screen, or of a
`{x, y, w, h}` region in screen pixels. Returns the recognised lines joined by `"\n"` (empty string
when nothing is read), or `nil` on error. Reading a small region is much faster than the whole
screen (≈150 ms vs ≈800 ms), so pass a tight region when you can. Vision reads arbitrary fonts
(including Chinese); for a game's own stylised glyphs, template matching (`find`/`classify_cell`) is
often faster and surer, but needs template images — OCR needs none. The companion `read_number` (in
the helper library) pulls the first number out of the text.

#### `wait(image [, timeout_s [, opts]])` → `x, y` or `nil`
Waits until `image` is visible and returns the centre of the match, or `nil` after
`timeout_s` seconds (default 10, at most 86400; `0` looks once). The screen is checked about
every 200 ms. **Always check the result**: a script that taps on with the wrong screen in front
of it does random things.

`wait` returns on the *first* frame that matches, which may be mid-animation (a button still
sliding in), so the coordinates can be a little off. When you then tap the returned point, let the
animation settle first — `sleep(300)` and `find` again, or just `tap("img/button.png")` (which
captures a fresh screen):

```lua
if wait("img/next.png", 10) then
    sleep(300)                      -- let the screen settle
    tap("img/next.png")             -- tap where it is now, not where it was sliding in
end
```

```lua
local x, y = wait("img/victory.png", 120)
if not x then
    screenshot("stuck.png")
    error("no victory screen after 2 minutes")
end
```

#### `wait_gone(image [, timeout_s [, opts]])` → boolean
Waits until `image` is no longer visible: `true` when it disappeared (or was not there),
`false` after `timeout_s` seconds (default 10).

#### `screenshot(path [, x, y, w, h])` → absolute path
Captures the screen now and saves it as PNG; with `x, y, w, h` only that rectangle (which
must lie inside the screen). Missing folders are created. Returns the absolute path of the file.

```lua
screenshot("shots/round-" .. round .. ".png")
screenshot("img/button.png", 440, 1610, 200, 80)    -- make a template
```

### Image objects

`capture()` and `img:crop()` return Image objects (in RGBA). Their pixels are freed
automatically when the Image is no longer used; `local img <close> = capture()` frees them as
soon as the variable goes out of scope.

| Method | Result |
|---|---|
| `img:size()` | `w, h` |
| `img:color(x, y)` | `0xRRGGBB` of a pixel of the image (error outside it) |
| `img:find(image [, opts])` | like `find`, searched in this image: `x, y, score` or `nil` |
| `img:find_all(image [, opts])` | like `find_all`, in this image |
| `img:crop(x, y, w, h)` | a new Image with that rectangle (must lie inside the image) |
| `img:save(path)` | saves as PNG (relative to the script folder; folders created); returns the absolute path |
| `tostring(img)` | `"Image(1080x2400)"` |

An Image can be used wherever a template is expected:

```lua
local before = capture():crop(100, 300, 120, 60)   -- remember what this area looks like
tap(540, 1650)
sleep(500)
if find(before, {region = {100, 300, 120, 60}}) then
    log("nothing changed")
end
```

### Matching options

`find`, `find_all`, `exists`, `wait`, `wait_gone`, `tap(image)`, `img:find` and `img:find_all`
take an optional table:

| Option | Default | Meaning |
|---|---|---|
| `threshold` | `0.88` | Minimum score, above 0 and at most 1. Higher = stricter. |
| `region` | whole screen | `{x, y, w, h}` (or `{x = , y = , w = , h = }`): search only there. A match must lie entirely inside; the part outside the screen is ignored. |
| `color_tolerance` | `40` | 1–255: reject a match whose average red, green or blue differs from the template's by more than this. `0` turns the colour check off. |
| `scale` | `1.0` | 0.1–10: resize the template by this factor before matching. Use it when the template was captured at a different resolution than the screen; `find_scaled` tries several for you. |

Unknown keys are errors (so that a typo like `treshold` does not go unnoticed). Keep option
tables in variables when you reuse them:

```lua
local BOTTOM = {region = {0, 1600, 1080, 800}, threshold = 0.9}
wait("img/next.png", 15, BOTTOM)
tap("img/next.png", BOTTOM)
```

**Masked templates.** Template pixels that are **transparent** (alpha < 128) are *ignored* when matching —
so you can erase the parts of a template that change (a glossy highlight, a background behind an orb, a
number that varies) in any image editor and keep only the stable shape. A template needs at least some
opaque pixels. This, with `color_tolerance`, is how you match an icon reliably against a busy or shifting
background. For a template captured at an unknown size, `find_scaled(image, {0.8, 0.9, 1.0, 1.1, 1.2}, opts)`
returns `x, y, score, scale` of the best fit (see the helper library).

### Instances and low level

#### `use(name)` → instance name
Sends everything that follows to another running instance (name or numeric id). Touches still
held on the previous instance are lifted. Useful for scripts that drive several accounts:

```lua
for _, name in ipairs({"farm1", "farm2", "farm3"}) do
    use(name)
    tap("img/collect.png")
end
```

#### `console(cmd)` → reply
Sends one command to the [emulator console](https://developer.android.com/studio/run/emulator-console)
and returns its reply text. A command the console rejects (`KO: ...`) raises an error. Examples:
`console("geo fix 2.2945 48.8583")` (longitude first), `console("power capacity 15")`,
`console("network speed edge")`. Single line, at most 4000 characters.

#### `shell(cmd [, timeout_s])` → output, exit status
Runs a command **inside Android** (`adb shell`) and returns its standard output (trailing
newlines removed) and its exit status. Default timeout 60 s (at most 3600); running out of time,
or adb failing (device offline), raises an error. The command runs in Android's shell: pipes and
quotes work there.

```lua
local sdk = shell("getprop ro.build.version.sdk")                 -- "37"
local out, status = shell("pm path com.example.game")
if status ~= 0 then log("not installed") end
shell("settings put system screen_brightness 50")
```

### Helper library

These are built in (pure Lua over the functions above — always available, no `require`). They are globals, so a
script may override any of them.

**Flow**
- `find_any(images [, opts])` → `key, x, y` or `nil` — the first image in the table that is on screen (`images` is a
  table `{name = "a.png", ...}` or an array; `key` is which one matched).
- `wait_any(images [, timeout_s [, opts]])` → `key, x, y` or `nil` — wait for the first of several images.
- `tap_until(image, done [, timeout_s [, opts]])` — tap `image` until `done` holds: `done` a function → until it
  returns truthy; an image → until it appears; `nil` → until `image` is gone. Returns `true`/`false` (timeout).
- `retry(fn [, times [, pause_ms]])` → runs `fn(i)` until it returns truthy (up to `times`, default 3); returns it.
- `assert_screen(image [, opts [, msg]])` — error unless `image` is on screen.

**Vision (colours and grids — for match‑3 / board games)**
- `color_dist(a, b)` → squared, green‑weighted distance between two `0xRRGGBB` colours.
- `hue(rgb)` → `0`–`360` (degrees) or `nil` for grey. `classify_hue(rgb, palette [, max_deg])` → the `palette` key
  whose **hue** is nearest (`palette = {name = degrees, …}`; `nil` past `max_deg`). Hue ignores brightness and
  saturation, so a tile that is **enhanced, frozen, framed or badged** still matches its base colour — far more
  robust for decorated/variant tiles than nearest‑RGB. (Grey tiles have no hue; fall back to `classify` for those.)
- `classify(rgb, palette [, max_dist])` → the `palette` key whose colour is nearest `rgb` (`palette = {name = 0xRRGGBB,
  …}`); `nil` if beyond `max_dist`.
- `read_grid(x0, y0, dx, dy, cols, rows [, sample])` → `board[r][c] = 0xRRGGBB`, one capture, cell centres at
  `(x0 + (c-1)·dx, y0 + (r-1)·dy)` (1‑based, row‑major). With `sample` > 0 it averages a ±`sample`‑pixel box per
  cell instead of reading the centre pixel, **weighted by saturation** so the colourful body wins and a white glow,
  frame or symbol is ignored (and clamps samples to the screen, so edge cells never error). Pair it with
  `classify_hue` for decorated tiles. A good `sample` is about a quarter of the cell spacing.
- `read_board(x0, y0, dx, dy, cols, rows, palette [, max_dist [, sample]])` → `board[r][c] = key` — `read_grid` then
  `classify`. Pass `sample` for averaged (robust) reading; if you do, calibrate the palette from averaged colours.
- `classify_cell(region, templates [, opts])` → `key, score` — the best‑matching template in a region (robust to orb
  variants); `region = {x, y, w, h}`, `templates = {name = "orb.png", …}`.
- `read_number([region])` → number or `nil` — the first number in `read_text(region)` (commas stripped). Pass a tight
  `{x, y, w, h}` region around the number for speed.

**Gestures**
- `drag_path(points [, opts])` — drag finger 0 through `points = {{x, y}, …}`; `opts.step_ms` between moves
  (default 16), `opts.hold_ms` before the first move (default 60). For "swap one tile along a path" games.

Example — read a 6×5 board by colour and drag a tile:
```lua
local ORB = { water=0x3AA0E0, fire=0xD83A2A, wood=0x4EB24A, light=0xE8C84A, dark=0x9A4FD0, heart=0xE86AA6 }
local board = read_board(96, 1020, 144, 144, 6, 5, ORB)   -- grid geometry from Inspect (⇧⌘I)
for r = 1, 5 do log(table.concat(board[r], " ")) end
drag_path({ {96,1020}, {240,1020}, {240,1164} }, { step_ms = 20 })
```

### Live parameter panel

A script can declare controls that appear in a small **腳本參數** panel beside the SoulX window, and read their
values **while it runs** — so you tune speed, targets or toggles without stopping and editing the script. Run the
same script headless (`soulx run`) and there is no panel: every control just returns its default, so the script
still works from the command line. (See `examples/scripts/param_panel_demo.lua`.)

- `param_slider(label, min, max [, default [, step]])` → object; `obj:value()` → the current number.
- `param_number(label, min, max [, default [, step]])` → object; `obj:value()` → the current number (typed field).
- `param_toggle(label [, default])` → object; `obj:on()` (or `:value()`) → `true`/`false`.
- `param_choice(label, options [, default])` → object; `obj:value()` → the chosen option string (`options` is an
  array of strings).
- `param_button(label)` → object; `obj:pressed()` → `true` exactly once per click (poll it in your loop).

```lua
local speed  = param_slider("Speed", 1, 10, 5)
local auto   = param_toggle("Auto", true)
local target = param_choice("Target", { "火", "水", "木" }, "火")
local once   = param_button("Step once")
while true do
  if once:pressed() then do_one_step() end
  if auto:on() then
    run_at_speed(speed:value(), target:value())   -- picks up slider/menu changes live
  end
  sleep(300)
end
```

Values refresh about every 100 ms, so reading `obj:value()` in a tight loop is cheap. Labels are the control's key:
keep them unique within a script. The panel talks to the script through small files in a private directory
(`$SOULX_UI_DIR`); a malformed or missing file is ignored, so neither side can crash the other.

---

## 5. Image matching guide

**How it works.** SoulX compares the template with every position of the screen (or the
region) using normalised cross-correlation on brightness: the score says how well the *shape*
matches, independently of overall brightness and contrast (1.0 = identical). It searches a
reduced copy first and refines the best candidates at full resolution. A second check then
compares the colours (`color_tolerance`), because a red and a green button of the same shape
have the same brightness structure. Matching is pixel for pixel: it does not resize or rotate
the template.

**Making templates.**

- Take them from SoulX (`soulx screenshot`, `screenshot(path, x, y, w, h)`,
  `capture():crop(...):save(...)`), at the resolution of the instance that runs the script.
  A macOS screenshot of the emulator window is scaled and will not match.
- Small and distinctive: the label or icon of a button rather than the whole panel; 40–200
  pixels wide is typical. Bigger templates are slower and more fragile.
- Static parts only: leave out numbers, timers, animated glows, and the background around
  rounded corners. Parts you cannot crop away can be made **transparent**: template pixels with
  alpha below 128 are ignored, so a PNG with a transparent background matches any background.
- A template that is all one colour has no shape; SoulX then compares brightness directly. Prefer
  `is_color()` for plain colour checks.
- A template bigger than the screen or the region can never be found; SoulX warns about it once.

**Thresholds and scores.** The default `0.88` works for most buttons. Calibrate with the
score: `log(find("img/ok.png", {threshold = 0.5}))` prints `x y score` of the best candidate.
A good template on its screen scores 0.95–1.0. Raise the threshold when look-alike elements are
confused; lower it (0.8) for elements with soft animations or semi-transparent overlays. Below
0.7 false matches become likely.

**Colour tolerance.** The default 40 accepts normal rendering differences and rejects other
colours. Lower it (15–20) to tell apart buttons that differ only in colour (enabled vs
disabled); set it to 0 to match by shape only (e.g. an icon that changes colour).

**Regions.** A region makes the search faster and prevents matches in the wrong place: if the
"OK" button is always in the lower third, search only there. Regions are clipped to the screen.

**Performance.** A capture takes about 0.1–0.5 s (about 0.12 s on the normal path, which
reads the emulator's framebuffer; when that is not available SoulX falls back to
`adb screencap`, about 0.5 s). Searching a full 1080 × 2400 screen takes 10–50 ms for typical
templates. So:

- several checks on the same screen are almost free (cache, or one `capture()` and `img:find`);
- `wait()` reacts within roughly 0.2–0.5 s;
- `find_all` costs about the same as `find`;
- pixel checks (`color`, `is_color`) cost only the capture.

---

## 6. Record and replay

Instead of writing coordinates by hand, record them:

```sh
soulx record macro.lua        # play by hand on the instance; Ctrl+C stops the recording
soulx run macro.lua           # replays it
```

The recorder watches the touchscreen and the keyboard and writes Lua:

| What you do | What is written |
|---|---|
| short touch (< 300 ms, moved < 20 px) | `tap(x, y)` |
| long touch (≥ 300 ms, moved < 20 px) | `long_press(x, y, ms)` |
| touch that moves | `swipe(x1, y1, x2, y2, ms)` (start to end) |
| two or more fingers | `touch_down` / `touch_move` / `touch_up` lines |
| a key | `key("NAME")` |
| typing | `text("...")` |
| a pause (≥ 30 ms) | `sleep(ms)` (rounded to 10 ms) |

Coordinates are written in logical pixels for the orientation at that moment. The time before
your first action is not recorded. Each action is also shown while you record. Use
`soulx record macro.lua --force` to overwrite an existing file. An example of a recording is
[recorded_macro.lua](../examples/scripts/recorded_macro.lua).

A recording is a normal script, so you can edit it: wrap it in a loop, delete mistakes, replace
a fixed `sleep()` before an important tap with `wait("img/button.png", 10)` so the replay waits
for the game rather than hoping it is as fast as when you recorded it.

---

## 7. Running headless and for a long time

- `soulx start --headless` runs an instance without a window. Scripts work exactly the same;
  `soulx screenshot` shows you what is on the screen.
- Instances keep the screen on while they run (unless created with `--no-stay-awake`).
- To keep a script running after you close the terminal:

  ```sh
  nohup soulx run farm.lua > farm.log 2>&1 &
  ```

  `tail -f farm.log` shows its progress. `kill -INT <pid>`, `kill <pid>` (SIGTERM) and closing the
  terminal (SIGHUP) all stop it cleanly, like Ctrl+C: the script ends, held touches are lifted and
  SoulX prints the stop line. (`soulx` installs its handlers even when the shell started the job with
  SIGINT ignored, so `kill -INT` works for background jobs too.)
- Several instances, one script each:

  ```sh
  soulx run -i farm1 farm.lua &
  soulx run -i farm2 farm.lua &
  ```

- Long-running scripts should cap their loops (a maximum number of rounds or a time limit with
  `millis()`), check every `wait()`, and stop (or restart the game) when they get lost. The
  [daily_routine.lua](../examples/scripts/daily_routine.lua) example shows the pattern.

---

## 8. Stopping a script (Ctrl+C)

Press **Ctrl+C**: the script stops at once (also in the middle of `sleep`, `wait`, a busy loop or
while a `shell()` command is still running), held touches are lifted, and SoulX prints `script
stopped (Ctrl+C) after 12.3 s` (exit code 130). A script cannot catch this with `pcall`. Pressing
Ctrl+C a second time ends `soulx` immediately, without cleaning up. `kill`/`kill -INT`/`kill -HUP`
of a background `soulx run` do the same clean stop (see [section 7](#7-running-headless-and-for-a-long-time)).

A script can end itself early with `os.exit()` (finished, code 0), `os.exit(1)` (failed) or
`error("message")` (failed with a message and a stack trace).

---

## 9. Errors and troubleshooting

An error stops the script and shows where it happened, followed by the chain of calls:

```
script error: farm.lua:12: tap: point (1200, 80) is outside the screen (1080x2400)
stack traceback:
	[C]: in function 'tap'
	farm.lua:12: in local 'collect'
	farm.lua:30: in main chunk
	[C]: in ?
```

Common messages:

| Message | Meaning |
|---|---|
| `instance 'x' is not running — start it with: soulx start x` | Start the instance first. |
| `cannot open image 'img/a.png' (/full/path/img/a.png): No such file or directory` | The template is looked for next to the script; the full path shows where. |
| `point (x, y) is outside the screen (WxH)` | Coordinates from another resolution or orientation. |
| `bad argument #2 to 'find' (unknown option 'treshold' ...)` | A typo in an options table. |
| `bad argument #1 to 'tap' (number expected, got nil)` | A variable that was never set, e.g. the result of a failed `find`. |
| `launch: cannot start com.example.game (is it installed?)` | Wrong package name, or not installed (`soulx apps` lists them). |
| `stop_app: com.example.game is not installed on 'default' (see: soulx apps)` | `stop_app` needs an installed package (force-stopping one that is merely not running is fine). |
| `key: unknown key 'X' (use a name like BACK, HOME, ENTER, A, F1, ...)` | A key name or code `key()` does not recognise (see the key table). |
| `console("..."): KO: ...` | The emulator console rejected the command. |
| `shell: '...' did not finish within 60 s` | Give `shell()` a longer timeout, or run the command in the background (`... &`). |
| `... did not accept the input (is it still running?)` | The emulator stopped or is not responding. |

Tips:

- `soulx -v run farm.lua` shows debug output, including how many screen captures the script made.
- Print scores to tune thresholds: `log(find("img/x.png", {threshold = 0.5}))`.
- Save evidence when something unexpected happens: `screenshot("stuck.png")`.
- Run single statements quickly by putting them in a tiny script, or use the CLI: `soulx tap X Y`,
  `soulx swipe X1 Y1 X2 Y2 MS`, `soulx key BACK`, `soulx text hello`, `soulx screenshot`.

---

## 10. Limitations

- `text()` types ASCII only (no accents, emoji or other scripts), in bursts of about 15 key presses.
- `key()` goes through `adb shell input keyevent` and takes about 30–60 ms per key (the first key in
  a script can cost around half a second); taps and swipes are injected directly and are much faster.
- A screen capture takes about 0.1–0.5 s, so scripts react within a few hundred milliseconds:
  fine for menus and turn-based play, not for frame-perfect timing.
- Image matching is pixel for pixel: templates must come from the same resolution, and it does
  not read text (no OCR).
- At most 10 fingers at once (`touch_down(..., 0..9)`).
- Coordinates and templates belong to one resolution and orientation (see
  [section 3](#3-coordinates-and-orientation)).
- Scripts cannot run programs on the Mac or load native code (`os.execute`, `io.popen`,
  `package.loadlib` and C modules are disabled); `shell()` runs commands inside Android. Lua's file
  functions (`io`, `os.remove`, …) do work, so run only scripts you trust.
- Run one script per instance at a time.
