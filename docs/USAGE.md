# Using SoulX

This guide covers every command, option and setting. For Lua automation see [SCRIPTING.md](SCRIPTING.md). To build
SoulX from source see [DEVELOPMENT.md](DEVELOPMENT.md).

- [First run](#first-run)
- [Everyday use](#everyday-use)
- [Global options and environment variables](#global-options-and-environment-variables)
- [Command reference](#command-reference)
- [Instances and their settings](#instances-and-their-settings)
- [GPU modes](#gpu-modes)
- [Frame rate cap](#frame-rate-cap)
- [What SoulX changes inside Android](#what-soulx-changes-inside-android)
- [The SoulX window](#the-soulx-window)
- [Game keymaps](#game-keymaps)
- [Automation (Lua)](#automation-lua)
- [AI control (MCP)](#ai-control-mcp)
- [Where SoulX keeps its data](#where-soulx-keeps-its-data)
- [Performance](#performance)
- [Troubleshooting](#troubleshooting)
- [Uninstall](#uninstall)

## First run

```bash
soulx doctor     # checks the Mac: Apple Silicon, Hypervisor.framework, memory, disk
soulx setup      # one-time: downloads SoulX's runtime (asks you to accept Google's Android SDK license)
soulx play ~/Downloads/mygame.apk
```

`soulx setup` downloads three packages from Google's official repository. Each package is checked against a pinned
size and SHA-1, and an interrupted download resumes where it stopped:

| package | size | what it is |
|---|---|---|
| Android Emulator 37.2.12 | 416 MB | the virtualization engine (QEMU with Hypervisor.framework and gfxstream on the Mac's GPU) |
| platform-tools 37.0.1 | 16 MB | adb |
| Android 17 Google Play r06 | 2.26 GB | the system image (arm64, 4 KB memory pages) |

Google licenses these components under the *Android Software Development Kit License Agreement*. SoulX shows you
the text and records your acceptance in `~/.soulx-android/licenses/`. SoulX never redistributes them.

`soulx play` does everything needed to start playing:
1. It creates the `default` instance on first use. If the game asks for landscape, the instance gets a landscape screen.
2. It boots Android (about 15 s; every start is a fresh boot).
3. It installs the game, or skips the install when that version is already there.
4. It launches the game and opens [SoulX's window](#the-soulx-window).

## Everyday use

```bash
soulx list                          # your instances and their state
soulx start                         # start "default" and open its window
soulx window                        # open the window of a running instance (or bring it to the front)
soulx stop                          # shut down (about 1 s)
soulx play com.example.game         # start the instance if needed and launch an installed game
soulx install game.xapk             # .apk / .xapk (with OBB data) / .apks / .apkm / folder of split APKs
soulx apps                          # installed third-party apps
soulx screenshot                    # PNG of the screen
soulx screenrecord gameplay.webm    # record a video; Ctrl+C stops (the engine caps it at 3 min)
soulx status                        # details: serial, pid, memory use, log file
soulx config --cores 6 --ram 6144      # change the settings of an instance (applies at its next start)
soulx stats --watch                     # CPU and memory use of the running instances
soulx volume 30                         # sound volume, right away (mute / unmute)
```

Run `soulx help` for the overview and `soulx help <command>` for the details of one command.

## Global options and environment variables

These options work with every command that acts on an instance:

| option | meaning |
|---|---|
| `-i NAME`, `--instance NAME` | the instance to use (default: `$SOULX_INSTANCE`, else `default`) |
| `-v`, `--verbose` | print debug output |
| `-h`, `--help` | show the command's help |

`adb`, `logcat` and `shell` pass every other word to adb, so `-v` and `-h` go to adb there. Only `-i NAME` is read by
SoulX for those commands.

| variable | meaning |
|---|---|
| `SOULX_HOME` | the folder SoulX keeps everything in (default `~/.soulx-android`) |
| `SOULX_INSTANCE` | the default instance name for `-i` |
| `SOULX_VERBOSE=1` | the same as `-v` on every command |
| `SOULX_LANG=en` | the language of the window and launcher for this run (`en`, `zh-hant`, `zh-hans`), over the saved choice |
| `SOULX_SCRIPTS` | the root folder of the per-instance scripts folders (default `~/Documents/SoulX Scripts`) |
| `SOULX_ACCEPT_ANDROID_SDK_LICENSE=1` | accept the Android SDK license without asking (the same as `soulx setup --accept-licenses`) |
| `SOULX_ENGINE_FEATURES=A,B` | diagnostics: passes `-feature A -feature B` to the engine at start |

## Command reference

Instance names use lowercase letters, digits, `-` and `_` (up to 32 characters). Coordinates are screen pixels: the
same pixels as a `soulx screenshot`.

### Setup and diagnostics

**`soulx setup [--image play|google|play16k] [--accept-licenses] [--keep-downloads] [--force]`**
Downloads and installs SoulX's Android runtime into `$SOULX_HOME`: the engine, adb and an Android 17 system image.
Android Studio is never needed or used.

| option | meaning |
|---|---|
| `--image KEY` | the system image to install: `play` (Android 17, Google Play; the default), `google` (Android 17, Google APIs; `adb root` works) or `play16k` (Android 17 QPR1, Google Play, 16 KB memory pages) |
| `--accept-licenses` | accept the Android SDK licenses without asking |
| `--keep-downloads` | keep the downloaded zip files in `$SOULX_HOME/downloads` |
| `--force` | reinstall packages that are already installed |

**`soulx doctor`** checks this Mac (Apple Silicon, Hypervisor, memory, free disk), SoulX's runtime, the adb server and
your instances. It exits with 1 when something keeps SoulX from running.

**`soulx runtime [--json]`** shows which runtime packages are installed, their versions and disk usage. `--json` also
says whether the default runtime is ready to run (for launchers).

**`soulx language [system|en|zh-hant|zh-hans] [--json]`** shows or sets the language of SoulX's window and of the
launcher. `system` (the default) follows the Mac's language; the choice is saved in `~/.soulx-android/ui_language`.
An open window keeps its language until it is opened again. The `soulx` command itself always prints English.

**`soulx version`** prints the SoulX version and the Android engine it runs.

**`soulx help [command]`** shows the overview, or the details of one command.

### Instances

**`soulx create [name] [options]`** creates an instance (an Android 17 phone). The name defaults to `$SOULX_INSTANCE`
or `default`.

| option | meaning |
|---|---|
| `--profile P` | `lite`, `balanced` (the default) or `performance` (see [profiles](#profiles)) |
| `--landscape` | a landscape screen (wider than tall), for landscape games |
| `--cores N` | virtual CPUs, 1–8 |
| `--ram MB` | memory in MB, 4096–12288 |
| `--res WxH` | screen size in pixels, e.g. `1080x2400` (320–3840 per side) |
| `--dpi N` | screen density, 120–640 |
| `--data GB` | storage for apps and data in GB, 4–64 |
| `--image KEY` | system image: `play` (default), `google` or `play16k` (it must be installed with `soulx setup --image`) |
| `--gpu MODE` | `host` (default), `host-novulkan`, `software` or `metal` (see [GPU modes](#gpu-modes)) |
| `--dns IP[,IP]` | DNS servers to use instead of the Mac's (up to 4) |
| `--no-stay-awake` | let the screen turn off while the instance runs |
| `--no-lean` | keep every preinstalled Google app running (see [Lean Android](#lean-android)) |
| `--fps N` | frame rate cap, 20–60 (default 60; see [Frame rate cap](#frame-rate-cap)) |
| `--no-audio` | no sound device (saves a little CPU; for instances that run in the background) |

Example: `soulx create racing --profile performance --landscape`

**`soulx config [name] [options] [--json]`** shows the settings of an instance, or changes them. A stopped
instance gets the new settings at its next start. A running instance keeps its current settings until `soulx restart`.
The system image is fixed when the instance is created, and storage can only grow (the engine enlarges the disk at
the next start).

| option | meaning |
|---|---|
| `--profile P` | `lite`, `balanced` or `performance`: CPUs, RAM and screen (keeps the orientation and the storage) |
| `--landscape`, `--portrait` | turn the screen |
| `--cores N`, `--ram MB`, `--dpi N` | the same ranges as `create` |
| `--res WxH` | screen size; its shape sets the orientation unless `--landscape` or `--portrait` is given |
| `--data GB` | storage, larger than now, up to 64 |
| `--gpu MODE` | `host`, `host-novulkan`, `software` or `metal` |
| `--dns IP[,IP]`, `--no-dns` | DNS servers, or back to the Mac's |
| `--stay-awake`, `--no-stay-awake` | keep the screen on, or not |
| `--lean`, `--no-lean` | [Lean Android](#lean-android) on or off |
| `--fps N` | frame rate cap, 20–60 (60 = no cap) |
| `--audio`, `--no-audio` | sound device on or off |
| `--json` | print the settings (after any change) as JSON, the same fields as `soulx list --json` |

Examples: `soulx config`, `soulx config racing --cores 6 --ram 6144 --landscape`, `soulx config --gpu host-novulkan`.

**`soulx delete <name> [-y|--yes]`** deletes a stopped instance and everything on it (installed apps and game data).
You must give the name explicitly. `--yes` skips the confirmation.

**`soulx list [--json]`** lists your instances: id, name, state, Android version, CPUs, RAM, screen and adb serial.

**`soulx status [name] [--json]`** shows one instance: state, Android version, hardware, screen, adb serial, engine pid,
uptime, memory use and log file.

**`soulx start [name] [--headless] [--cold] [--no-wait] [--timeout S]`** starts an instance, waits until Android has
booted, then opens the window.

| option | meaning |
|---|---|
| `--headless` | no window (for automation) |
| `--cold` | kept for older scripts; every start is already a full boot |
| `--no-wait` | return as soon as the engine is launched |
| `--timeout S` | stop waiting for the boot after S seconds (10–3600, default 300) |

**`soulx stop [name] [--force]`** shuts an instance down in about a second. Apps and data stay. `--force` kills the
engine immediately.

**`soulx restart [name] [...]`** stops a running instance and starts it again. It takes the same options as `start`. An
instance that runs headless stays headless.

**`soulx window [name]`** opens SoulX's window for a running instance, or brings an open one to the front. `start` and
`play` open it automatically unless you pass `--headless`.

### Apps

**`soulx play [-i NAME] <game.apk|package> [--headless] [--reinstall]`** is the one-step way to play. It creates the
instance on first use, starts it, installs the game and launches it. It accepts `.apk`, `.xapk`, `.apks` and `.apkm`
files, a folder of split APKs, or the package name of an installed app.
- `--headless`: start the instance without a window (if it is not running yet).
- `--reinstall`: install the game even when the same version is already installed.

**`soulx install [-i NAME] <file>... [--reinstall]`** installs `.apk`, `.xapk` (including OBB data), `.apks` and
`.apkm` files, or a folder of split APKs, on a running instance. It skips apps whose installed version is the same.

**`soulx uninstall [-i NAME] <package>`** removes an app and its data.

**`soulx apps [-i NAME] [--all]`** lists the package names of the apps you installed. `--all` includes the system apps.

**`soulx launch [-i NAME] <package>`** starts an installed app.

**`soulx stop-app [-i NAME] <package>`** force-stops an app.

### Input

**`soulx tap [-i NAME] X Y`** taps at X,Y.

**`soulx swipe [-i NAME] X1 Y1 X2 Y2 [MS]`** swipes from X1,Y1 to X2,Y2 in MS milliseconds (default 300).

**`soulx key [-i NAME] KEY`** presses a key. KEY can be any of these:
- a name: `BACK`, `HOME`, `APP_SWITCH`, `MENU`, `ENTER`, `DEL`, `TAB`, `SPACE`, `ESCAPE`, `DPAD_UP`, `DPAD_DOWN`,
  `DPAD_LEFT`, `DPAD_RIGHT`, `DPAD_CENTER`, `VOLUME_UP`, `VOLUME_DOWN`, `POWER`, or `A`–`Z`;
- an Android `KEYCODE_*` name;
- a numeric Android key code (`4` = BACK).

A bare number is a key code, not a digit: use `KEYCODE_0`..`KEYCODE_9` to type digits.

**`soulx text [-i NAME] TEXT...`** types text into the focused field. Several words are joined with spaces.

### Sound and resources

**`soulx volume [-i NAME] [0-100|mute|unmute] [--json]`** shows or sets the media volume of a running instance (the
sound games play). It applies right away, without a restart. `mute` silences the instance and `unmute` brings the
previous level back. To remove the sound device altogether, use `soulx config --no-audio`.

**`soulx stats [name] [--json] [--watch] [--interval MS]`** measures the CPU and memory use of every running instance,
or only of the named one, over one interval.
- CPU is in percent of one core: an instance that uses two full cores shows 200%.
- Memory is the engine's footprint, the number Activity Monitor shows.
- `--json` prints one JSON object per measurement, `--watch` keeps measuring until Ctrl+C, and `--interval`
  sets the length of one measurement (200–60000 ms, default 1000).

A launcher can run `soulx stats --watch --json` once and read one line per second.

### Screen

**`soulx screenshot [-i NAME] [out.png]`** saves a PNG and prints its path. The default file is
`./soulx-<instance>-<date>-<time>.png`.

**`soulx screenrecord [-i NAME] [out.webm]`** records the screen to a WebM video. Ctrl+C stops it, and the engine stops
by itself after 3 minutes. The default file is `./soulx-<instance>-<date>-<time>.webm`. The path must end in `.webm`
and contain no spaces.

### Automation

**`soulx run [-i NAME] [--repeat N | --loop] [--speed X] <script.lua> [args...]`** runs a Lua 5.4 script on a running
instance. The script can be a path, or the name of one of the instance's scripts (`soulx run "Daily run"`). Options go before the script name. Words after it reach the script in the global table `args`. Ctrl+C stops
the script.

| option | meaning |
|---|---|
| `--repeat N` | run the script N times (1–100000); default 1 |
| `--loop` | run it over and over until you stop it |
| `--speed X` | playback speed 0.1–10 (default 1): every `sleep()` and replay pause is divided by X |

**`soulx scripts [-i NAME] [list|folder|import PATH...|remove ENTRY|rename FILE NAME] [--json]`** manages the
instance's scripts (see [Scripts per instance](#scripts-per-instance)). `import` copies .lua files or script
folders in, `remove` moves an entry to the Trash, `rename` sets the name a script shows (its `-- name:` line).

**`soulx record [-i NAME] <out.lua> [--force]`** records your taps, swipes, long presses, keys and the pauses between
them as a Lua script. Ctrl+C stops recording. `--force` overwrites an existing file.

### Low-level access

These commands use SoulX's own adb, a private server on port 5137, so they never interfere with Android Studio's adb.

**`soulx adb [-i NAME] [args...]`** runs adb against the instance. Put `--` first to pass a leading option through
verbatim. Examples: `soulx adb shell getprop ro.build.version.release`, `soulx adb push save.dat /sdcard/Download/`.

**`soulx shell [-i NAME] [command...]`** opens an interactive Android shell, or runs one command.

**`soulx logcat [-i NAME] [args...]`** streams the Android log. Its options go to adb (`soulx logcat -s Unity`).

**`soulx console [-i NAME] <command...>`** sends one command to the emulator console and prints the reply. It exits
with 1 when the console answers KO. Examples: `soulx console geo fix -122.084 37.422` (longitude, then latitude),
`soulx console power capacity 15`.

## Instances and their settings

Each instance is an independent Android 17 phone with its own apps and data. Choose an instance with `-i NAME` or
`SOULX_INSTANCE`.

```bash
soulx create racer --landscape --profile performance   # 6 CPUs, 6 GB, 2400x1080
soulx play -i racer ~/Downloads/racer.apk
soulx create bot --profile lite                         # a light instance for automation
soulx start bot --headless                              # no window; drive it with scripts
```

### Profiles

| profile | CPUs | RAM | screen |
|---|---|---|---|
| lite | 2 | 4096 MB | 720x1600 @ 320 dpi |
| balanced (default) | 4 | 4096 MB | 1080x2400 @ 420 dpi |
| performance | 6 | 6144 MB | 1080x2400 @ 420 dpi |

`--cores`, `--ram`, `--res`, `--dpi` and `--data` override single values. Choose the orientation when you create the
instance. A landscape game on a portrait instance still works, but the window shows it sideways.

### System images

The default `play` image includes Google Play services and the Play Store, which is the best choice for games
(sign-in, in-app purchases).

```bash
soulx setup --image google     # Android 17 Google APIs: Play services, no Play Store, `adb root` works
soulx setup --image play16k    # Android 17 QPR1, 16 KB memory pages
soulx create test16k --image play16k
```

Images newer than Android 17 (API 37.0) use 16 KB memory pages only. Many games have native libraries built for 4 KB
pages, including many Unity, Unreal and Cocos builds. On a 16 KB image those games run in a slower compatibility mode
with a warning dialog, or crash. That is why the default stays on API 37.0, the newest image with 4 KB pages.

### The instance file

Settings are stored in `~/.soulx-android/instances/<name>.conf`, one `key=value` per line. Change them with
`soulx config`, which checks every value. If you edit the file by hand, do it only while the instance is stopped. The
changes take effect at the next start.

| key | values | meaning |
|---|---|---|
| `id` | number | internal id, set by SoulX |
| `name` | name | the instance name |
| `image` | `play`, `google`, `play16k` | system image |
| `cores` | 1–8 | virtual CPUs |
| `ram_mb` | 4096–12288 | guest memory in MB |
| `data_gb` | 4–64 | storage for apps and data |
| `width`, `height` | 320–3840 | screen size in pixels (portrait values; see `landscape`) |
| `dpi` | 120–640 | screen density |
| `landscape` | `0` or `1` | landscape screen |
| `gpu` | `host`, `host-novulkan`, `software`, `metal` | [GPU mode](#gpu-modes) |
| `dns` | empty, or up to 4 IPs separated by commas | DNS servers (empty: the Mac's) |
| `stay_awake` | `0` or `1` | keep the screen on while running |
| `lean` | `0` or `1` | [Lean Android](#lean-android) (default `1`; a file without the key counts as `1`) |
| `fps` | 20–60 | [frame rate cap](#frame-rate-cap) (default `60`) |
| `audio` | `0` or `1` | sound device (default `1`) |
| `created` | timestamp | set by SoulX |
| `last_port` | number | the console port of the last start, set by SoulX |

## GPU modes

| mode | what renders | when to use it |
|---|---|---|
| `host` (default) | the Mac's GPU through gfxstream: Vulkan 1.3 through MoltenVK, and OpenGL ES 3.0 through Apple's OpenGL (which runs on Metal) | everyday use |
| `host-novulkan` | the same as `host`, but without Vulkan, so games use OpenGL ES | a game renders incorrectly or crashes at start on `host` |
| `software` | the CPU (SwiftShader) | last resort: compatible but slow |
| `metal` (experimental) | OpenGL ES through the engine's ANGLE directly on Metal (one translation layer fewer) | testing only |

**`metal` is not for everyday use yet.** ANGLE on Metal corrupts textures in this engine: icons and text get streaks,
and letters go missing. It also draws Android's blur black, so SoulX turns window blurs off for these instances. At
the first `metal` start SoulX prepares the engine's ANGLE library and keeps the original as `libGLESv2.dylib.google`.

To change the mode of an existing instance: `soulx config <name> --gpu MODE`, then `soulx restart <name>`.

## Frame rate cap

`fps` sets the refresh rate of the Android screen (the engine's `hw.lcd.vsync`). Android draws at most that many
frames per second, so games render less often and the engine needs less CPU and GPU. It is a good fit for instances
that farm in the background or run scripts.

```bash
soulx config farm --fps 30 && soulx restart farm
```

Measured on an M2 Pro with the same scrolling animation on a `lite` instance: at 60 fps the engine used 56% of a core
and drew 452 frames; at 30 fps it used 40% and drew 240 frames. Values above 60 are refused, because a guest refresh
rate above the Mac's display rate is undefined in the engine. Android reports the cap to apps as the display's only
refresh rate, so games do not fight it.

## What SoulX changes inside Android

### No Quick Boot

Every start is a fresh boot (about 15 s), and a stop saves nothing. Resuming a Quick Boot snapshot broke the guest's
GPU sync objects: the engine log filled with thousands of `glClientWaitSync` errors. In that state the engine leaked
memory without bound, reaching 30 GB in 11 minutes of a Unity game with Auto play. After a fresh boot, the same session
stayed flat at about 3.5 GB. Installed apps and their data stay across restarts.

### Three-button navigation

SoulX switches every new device to Android's three-button navigation once: back, home and recents at the bottom,
like the toolbar's. With gesture navigation, Android takes every drag that starts at a screen edge for itself. A
mouse starts drags there all the time, so drawers stopped half open. Games hide the system bars, so their layout and
script coordinates do not change. To go back to gestures, use Android's Settings ▸ System ▸ Gestures. SoulX does not
switch it again.

### Lean Android

By default (`lean=1`), SoulX disables the preinstalled Google apps that a game never needs, once per device:
- YouTube, YouTube Music, Photos, Messages, Gemini, the Google app, Gmail, Maps, Drive, Calendar and Clock;
- Digital Wellbeing, speech services, Android System Intelligence and Personal Safety;
- the restore and wallpaper apps, Android Auto and the AI-glasses services.

SoulX also caps cached background apps at 16. Together this frees about 600 MB of a 4 GB guest, and those apps no
longer wake up for background syncs. The Play Store, Play services, Chrome, WebView, the keyboard, the launcher,
phone, contacts and the accessibility apps stay enabled.

To undo it:
- Enable a single app again in Android's Settings ▸ Apps. SoulX leaves it enabled.
- Run `soulx config --no-lean` to bring them all back at the next start.
- Use `soulx create --no-lean` to create an instance without lean mode.

## The SoulX window

SoulX shows Android in its own window. The engine always runs in the background without a window of its own. The
window reads every frame straight from the engine's shared memory, draws it with Metal, and passes your mouse and
keyboard to Android.

- `soulx play` and `soulx start` open the window once Android has booted. `--headless` starts without one.
- **Closing** the window (⌘W, or Quit ⌘Q) stops the instance. **Window ▸ Close Window (Keep Running)** (⌥⌘W) closes
  only the window. Android keeps running, and `soulx window` brings the window back.
- When the instance is stopped from a terminal or the engine exits, the window closes by itself.
- The window works on any display, including external monitors with a different scale.

### Toolbar

| button | shortcut | what it does |
|---|---|---|
| 返回 Back | Esc, right click | Android Back |
| 首頁 Home | | Android Home |
| 多工 Recents | | the recent-apps overview |
| 旋轉 Rotate | | turns the screen between portrait and landscape (and turns auto-rotate off) |
| 錄製腳本 Record script | ⌘R | records what you do as a Lua script. Click again to stop, then choose where to save it (this instance's scripts folder by default). |
| 執行腳本 Run script | ⌘O | picks a `.lua` script, how many times to run it (or until you stop it) and the speed, then runs it. Click again (or ⌘.) to stop it. |
| 截圖 Screenshot | ⌘S | saves the screen, upright, as `~/Pictures/SoulX/<instance>-<date>-<time>.png` |
| 遊戲鍵位 Keymap | ⌘K | edits the [game keymap](#game-keymaps) |

The labels follow the system language: Traditional Chinese, Simplified Chinese or English. The status bar shows the
frame rate, the device serial, the rotation, the script or recording state and the last message.

### Menus

| menu | item | shortcut |
|---|---|---|
| View | Actual Size / Fit to Screen | ⌘0 / ⌘9 |
| View | Enter Full Screen | ⌃⌘F |
| View | Hide Toolbar: folds the toolbar into a thin strip (also the chevron at its top; SoulX remembers the choice) | ⌥⌘T |
| View | Inspect (x, y, colour): hover to read the coordinates and pixel colour, click to copy them | ⇧⌘I |
| Device | Back, Home, Recents, Rotate, Screenshot | Esc, –, –, –, ⌘S |
| Device | Paste: types the Mac clipboard's text into the focused Android field (ASCII for now) | ⌘V |
| Automation | Record, Run Script…, Stop | ⌘R, ⌘O, ⌘. |
| Automation | Record Video: records to `~/Movies/SoulX/<instance>-<date>.webm` (the engine stops after 3 min) | ⇧⌘E |
| Automation | Show Log: the output of the running script (scripts never pop a window open) | ⇧⌘L |
| Keymap | Edit, Show Bindings | ⌘K, – |
| Window | Minimize, Close Window (Keep Running), Close and Stop | ⌘M, ⌥⌘W, ⌘W |

### Mouse and keyboard

- The left mouse button is your finger: click to tap, drag to swipe.
- The right mouse button is Back.
- The scroll wheel scrolls what is under the pointer (SoulX turns it into short swipes).
- Typing goes to Android as a real hardware keyboard, including Enter, Backspace, the arrow keys and shortcuts with
  Shift, Ctrl or Alt.
- Shortcuts with ⌘ belong to SoulX's menus and never reach Android.
- When the window loses focus, SoulX releases every key and finger that is still held.

## Game keymaps

A keymap turns keys into touches, so you can play touch games with the keyboard. Click **遊戲鍵位 Keymap** (⌘K) to edit
the keymap of the game in the foreground:
- Click an empty spot, then press a key: that key now taps there. Holding the key holds the finger down.
- Drag a binding to move it. Right-click it to delete it, change its key or turn it into a joystick.
- **Add joystick** places a WASD joystick. Scroll over its ring to change the radius.
- **Done** saves the keymap. Esc cancels.

While you play, **Keymap ▸ Show Bindings** draws the bindings over the game. Keys without a binding still type
normally.

Keymaps are text files in `~/.soulx-android/keymaps/`. Each game has its own `<package>.keymap`, and `default.keymap`
applies to every game without one. Coordinates are fractions of the upright screen (0 = left or top, 1 = right or
bottom), so a keymap works at any resolution:

```
# ~/.soulx-android/keymaps/com.example.game.keymap
# joystick: up left down right, centre x y, radius (a fraction of the screen's short side)
joystick W A S D 0.18 0.75 0.10
# attack: the finger stays down while J is held
tap J 0.86 0.80
tap SPACE 0.93 0.64
# dash: one swipe upwards in 150 ms for each press of E
swipe E 0.50 0.70 0.50 0.35 150
```

Key names:
- letters and digits: `A`–`Z`, `0`–`9`;
- function and editing keys: `F1`–`F12`, `SPACE`, `ENTER`, `TAB`, `ESC`, `BACKSPACE`;
- arrows: `UP`, `DOWN`, `LEFT`, `RIGHT`;
- modifiers: `LSHIFT`, `RSHIFT`, `LCTRL`, `RCTRL`, `LALT`, `RALT`;
- punctuation: `MINUS`, `EQUAL`, `LBRACKET`, `RBRACKET`, `BACKSLASH`, `SEMICOLON`, `QUOTE`, `COMMA`, `PERIOD`,
  `SLASH`, `GRAVE`.

Up to 8 bindings can hold a finger down at the same time.

## Automation (Lua)

SoulX has a built-in Lua 5.4 engine for automating games. Scripts can tap, swipe, press keys and type text, wait for
images on the screen, read pixel colours, read text with OCR, and record and replay.

```lua
-- farm.lua   (run with: soulx run farm.lua)
launch("com.example.game")
wait("img/start.png", 30)        -- wait until the start button is visible
tap("img/start.png")             -- tap where it was found

for i = 1, 50 do
  swipe(300, 1500, 800, 1500, 200)
  tap(540, 1650)
  sleep(800 + math.random(200))
  if color(1000, 80) == 0xFF2D2D then
    tap(980, 1700)               -- low HP: drink a potion
  end
end
```

```bash
soulx record macro.lua                  # play by hand; Ctrl+C stops recording
soulx run macro.lua                     # replays your taps, swipes, keys and pauses
soulx run --repeat 20 macro.lua         # 20 times in a row
soulx run --loop --speed 2 macro.lua    # over and over, twice as fast, until Ctrl+C
```

### Scripts per instance

Each instance keeps its own scripts in `~/Documents/SoulX Scripts/<instance>/`: .lua files, or folders that hold a
script together with the files it needs (`lib/` ...). SoulX's window lists, records into and runs only that
instance's scripts; the launcher shows them on the instance's page, where you can import, rename, show in Finder
or move them to the Trash (running them is the window's job). Only scripts with a `-- name:` line appear in the
window's list.

Scripts from before this layout (directly in `~/Documents/SoulX Scripts/`) are moved into `default/` once, the
first time SoulX runs. `SOULX_SCRIPTS` sets another root folder.

See [SCRIPTING.md](SCRIPTING.md) for the full API and a tutorial, and [examples/scripts/](../examples/scripts/) for
complete scripts. Scripts written for Soul keep working: the old `soul` table is an alias of `soulx`.

## AI control (MCP)

SoulX can let an AI app (any Model Context Protocol client) look at an
instance's screen and operate it: take screenshots, read text, tap, swipe, drag, press keys, type, start apps, run
your scripts and run Lua it writes.

**It is off by default.** Turn it on in the launcher (**AI control** in the sidebar) or with
`soulx mcp config --enable`. Then connect your AI app once:

In the AI app, add an MCP server (stdio) whose command is the `soulx` executable and whose argument is `mcp`. In
an app's JSON configuration that is:

```json
"soulx": { "command": "/usr/local/bin/soulx", "args": ["mcp"] }
```

`soulx mcp setup` prints this with the right path; the launcher's AI control page has it ready to copy.

### What the AI can use

| tool | what it does |
|---|---|
| `list_instances`, `instance_status` | the instances it may use; state, screen size, CPU and memory |
| `start_instance`, `stop_instance` | start (with or without a window) and stop (permission: start-stop) |
| `screenshot` | an image of the screen (or a region), scaled to `max_size` (default 1024) |
| `read_text`, `find_image`, `pixel_color` | OCR, template matching (templates live in the instance's scripts folder), pixel colours |
| `tap`, `long_press`, `swipe`, `drag_path`, `press_key`, `type_text` | input; coordinates are screen pixels |
| `list_apps`, `launch_app`, `stop_app`, `install_app` | apps (installing needs the install permission) |
| `list_scripts`, `run_script`, `stop_script` | the instance's own scripts: run and wait, or in the background (permission: scripts) |
| `run_lua` | Lua the AI writes, with SoulX's script API (permission: lua; always sandboxed) |

### Security

- **Local only.** The AI app starts `soulx mcp` itself and talks to it over stdin and stdout. SoulX opens no
  network port, so nothing on the network can reach it.
- **Off switch that acts at once.** Every tool call re-reads the settings, so turning AI control off (or removing
  an instance or a permission) applies to the next call of an AI that is already connected.
- **Only the instances you allow.** "All instances" or a list; the others are invisible to the AI.
- **Permissions:** start and stop instances (on by default), install apps (**off** by default), run your scripts
  (on), run Lua it writes (on). Looking and touching are always allowed while AI control is on.
- **Sandboxed Lua.** Code the AI sends runs without `io`, without the file, process and environment functions of
  `os`, without `load`, `dofile`, `require` and the emulator console; files can be read or written only inside the
  instance's scripts folder (absolute paths, `..` and symbolic links out of it are refused). It can operate the
  instance but cannot read or change anything else on your Mac.
- **Your scripts only.** `run_script` runs only scripts in the instance's own scripts folder, never another file.
- **Never available:** creating, deleting or changing instances, and installing the runtime.
- **Audit log.** Every call (tool, instance, arguments, success) goes to `~/.soulx-android/logs/mcp.log`; the
  launcher shows the recent ones, and `soulx mcp log` prints them.
- Every tool has a time limit. When the AI app disconnects, background script runs it started are stopped.

### Commands

**`soulx mcp [serve|config|log|setup]`**

| | |
|---|---|
| `soulx mcp` | the server (your AI app runs this) |
| `soulx mcp config [--enable\|--disable] [--instances "*"\|a,b] [--allow P,...] [--deny P,...] [--json]` | settings; permissions `start-stop`, `install`, `scripts`, `lua` |
| `soulx mcp log [-n N] [--json]` | the last N calls |
| `soulx mcp setup` | how to connect an AI app |

## Where SoulX keeps its data

Everything lives in `~/.soulx-android`. Set `SOULX_HOME` to use a different folder.

| folder | contents |
|---|---|
| `sdk/` | the runtime: `emulator/`, `platform-tools/`, `system-images/…`, with install markers in `sdk/.soulx/` |
| `avd/` | device definitions written by SoulX (`soulx_<name>.ini`, `soulx_<name>.avd/` with the disks) |
| `instances/` | instance settings (`<name>.conf`) |
| `keymaps/` | game keymaps |
| `run/`, `logs/` | the state of running instances; the engine log (`logs/<name>.log`) and window log (`logs/<name>.window.log`) |
| `home/` | the `HOME` given to the engine and adb: adb keys, console token, engine preferences |
| `licenses/`, `downloads/`, `tmp/` | accepted license texts, the download cache, temporary files |

Your own files go to `~/Documents/SoulX Scripts/<instance>/` (that instance's scripts; see [Scripts per instance](#scripts-per-instance)), `~/Pictures/SoulX` (screenshots) and
`~/Movies/SoulX` (videos).

**Moving from Soul.** The first time SoulX runs, it moves Soul's `~/.soul-android` to `~/.soulx-android` and renames
the devices and the folders above. It does this only when `SOULX_HOME` is not set and `~/.soulx-android` does not
exist yet. It refuses while a Soul engine still runs from the old folder; stop it first.

**Isolation.** Every engine and adb process runs with an environment that points into `~/.soulx-android`: `HOME`,
`ANDROID_HOME`, `ANDROID_AVD_HOME`, `ANDROID_EMULATOR_HOME`, `TMPDIR` and a private adb server port. Instances use
console ports 5600–5680, above the 5555–5585 range that other adb servers scan. As a result, Android Studio's adb never
finds SoulX's devices, and no "Allow USB debugging?" dialog pops up over your game.

## Performance

Measured on an M2 Pro (16 GB) with the default instance:

| | |
|---|---|
| start | about 13–15 s, always a fresh boot |
| stop | about 1 s |
| frames | up to 60 fps; a new frame reaches the window about 0.1 ms after the engine writes it |
| touch injection | under 1 ms per event; a tap takes about 40 ms |
| screen capture (scripts) | about 1.3 ms per 1080×2400 frame (the console PNG it replaced took about 130 ms) |
| memory | about 1.2–1.5 GB resident for the engine. The 4 GB guest RAM is mapped, so Activity Monitor shows a 6–8 GB footprint, most of it compressed by macOS. It stays flat while a game runs with Auto play. |

## Troubleshooting

| problem | what to do |
|---|---|
| `soulx doctor` says Hypervisor is unavailable | macOS must run natively on Apple Silicon, not in a VM |
| no internet inside Android (VPN or company DNS) | create the instance with public DNS: `soulx create game --dns 8.8.8.8,8.8.4.4` |
| `INSTALL_FAILED_NO_MATCHING_ABIS` / "needs 32-bit ARM" | Android 17 arm64 images are 64-bit only, and Apple Silicon cannot run 32-bit ARM code. You need an `arm64-v8a` build of the game. |
| "This app isn't 16 KB compatible" dialog | you are on the `play16k` image; use the default `play` image |
| the game says the device is not certified or refuses to run on an emulator | Play Integrity and anti-cheat checks fail on every emulator; there is no fix |
| black or glitchy graphics | use `gpu=host` (the default). If it still happens, try `host-novulkan`, then `software`. |
| the engine's memory keeps growing | look for `glClientWaitSync` errors in `~/.soulx-android/logs/<name>.log` and restart the instance |
| the instance won't boot | read `~/.soulx-android/logs/<name>.log`; run `soulx doctor` |
| no window, or you closed it with ⌥⌘W | `soulx window` opens it again; its log is `~/.soulx-android/logs/<name>.window.log` |
| "slow display mode" in the status bar | the engine's shared screen is not available, so the window shows screenshots at about 8 fps. Run `soulx restart`. |
| a key types instead of tapping | the key is not in the foreground game's keymap. Open the editor (⌘K) while the game runs. |
| the screen stays sideways | create a `--landscape` instance for landscape games |

## Uninstall

```bash
soulx stop                        # for every running instance
rm -rf ~/.soulx-android           # removes the runtime, every instance and all data
sudo rm /usr/local/bin/soulx      # if you installed it
```
