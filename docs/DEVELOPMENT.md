# Developing SoulX

This guide covers building SoulX from source, running the tests, how the code is organised and how to debug it. For
using SoulX see [USAGE.md](USAGE.md).

## Requirements

| | |
|---|---|
| Mac | Apple Silicon (M1 or later), macOS 12 or later |
| Compiler | Xcode Command Line Tools (`xcode-select --install`): Apple clang, C11 and Objective-C |
| Build system | CMake 3.16 or later (`brew install cmake`) |

Nothing else is needed. Lua 5.4 and stb are vendored in `source/third_party/`, and the Android runtime is downloaded
at run time by `soulx setup`.

## Build

```bash
git clone <SoulX repository URL> soulx
cd soulx
./scripts/build_macos.sh --test        # builds build/macos/soulx and runs the unit tests
```

| option | meaning |
|---|---|
| `--test` | run the unit tests (ctest) after building |
| `--debug` | Debug build instead of Release |
| `--clean` | delete `build/macos` before configuring |

The result is a single binary, `build/macos/soulx` (about 600 KB, with Lua built in). To use it from anywhere:

```bash
sudo cmake --install build/macos       # installs /usr/local/bin/soulx
# or: ln -s "$PWD/build/macos/soulx" /usr/local/bin/soulx
```

You can also drive CMake directly:

```bash
cmake -S . -B build/macos -DCMAKE_BUILD_TYPE=Release
cmake --build build/macos -j
cmake --build build/macos --target soulx_tests && (cd build/macos && ctest --output-on-failure)
```

The binary targets macOS 12 by default (`CMAKE_OSX_DEPLOYMENT_TARGET`). The version has a single source,
`SOULX_VERSION_STRING` in `source/core/soulx_android.h`.

**"You have not agreed to the Xcode license agreements".** When Xcode is installed but its license was never accepted,
`cc`, `git` and `python3` refuse to run. Accept it with `sudo xcodebuild -license`, or build with the Command Line
Tools instead:

```bash
DEVELOPER_DIR=/Library/Developer/CommandLineTools ./scripts/build_macos.sh --test
```

## Packaging and releases

```bash
./scripts/package_macos.sh             # build/dist/SoulX.app, SoulX-<version>.dmg, .zip and .sha256
./scripts/package_macos.sh --skip-build --no-dmg
```

`SoulX.app` holds the launcher (`Contents/MacOS/SoulX`), the CLI (`Contents/Helpers/soulx`, which also runs SoulX's
window and the MCP server) and the icon (drawn by `scripts/make_icon.swift`, the same drawing as `SoulXAppIcon()` in
the window). Its version comes from `SOULX_VERSION_STRING`; the build number is the commit count. The app is signed
ad hoc only: it is deliberately not signed with a Developer ID nor notarized, so people allow it once in
System Settings > Privacy & Security.

Releases are published by `.github/workflows/release.yml`:

1. Set `SOULX_VERSION_STRING` in `source/core/soulx_android.h` and add a `## X.Y.Z` section to `CHANGELOG.md`.
2. Commit, then `git tag vX.Y.Z && git push origin vX.Y.Z`.
3. The workflow checks that the tag matches the version, runs every test, packages the app and creates the GitHub
   Release with the .dmg, .zip and checksums. Its text is the CHANGELOG section plus the install steps
   (`scripts/release_notes.sh`).

Running the workflow by hand (Actions > Release > Run workflow) builds the .dmg as an artifact without publishing.
CI also packages the app on every push, so a broken package shows up before a release.

## Tests

`tests/macos/test_*.c` are standalone unit tests, one executable each. They need no emulator, network or runtime:
fakes stand in for the engine, adb and the console.

| test | covers |
|---|---|
| `test_paths_proc` | `SOULX_HOME` paths, process spawning and environments |
| `test_registry` | instance files, profiles, engine arguments, post-boot commands (lean, navigation) |
| `test_runtime` | runtime packages, downloads, ANGLE preparation |
| `test_console` | the emulator console protocol |
| `test_input`, `test_hidkbd` | touch and keyboard injection |
| `test_screen`, `test_frames` | screenshots, the shared-memory frame reader and frame pump |
| `test_image` | PNG I/O and template matching |
| `test_keymap` | keymap files |
| `test_apps` | `.apk` / `.xapk` / `.apks` / `.apkm` handling |
| `test_lua` | the Lua API, including the sandbox for AI code |
| `test_scripts` | per-instance scripts, migration, the language setting |
| `test_mcp` | MCP settings, permissions, the off switch, the audit log, connecting AI apps |

Run one test with `ctest -R registry --output-on-failure` in `build/macos`.

## Project layout

```
CMakeLists.txt              macOS-only build; target `soulx`, test target `soulx_tests`
scripts/build_macos.sh      build script
scripts/package_macos.sh    SoulX.app, .dmg and .zip (make_icon.swift draws the icon, release_notes.sh the release text)
source/
  core/soulx_android.h      public C API (instances, apps, input, screen) and the version
  native/main_macos.c       the `soulx` CLI: argument parsing, help texts, every command
  platform/macos/           the macOS implementation
    soulx_mac.h             internal contract between the modules
    paths.c                 ~/.soulx-android layout, migration from ~/.soul-android
    proc.c                  posix_spawn with argument arrays and isolated environments
    runtime.c               `soulx setup`: package downloads, SHA-1 checks, licenses, ANGLE preparation
    avd.c, registry.c       device definitions and instance settings (`instances/<name>.conf`)
    engine.c                starting/stopping the engine, GPU modes, post-boot commands
    adb.c, console.c        private adb server, emulator console client
    input.c, hidkbd.c       multi-touch over the console, the virtual USB keyboard (uhid)
    screen.c                screenshots
    apps.c                  installing apps (split APKs, XAPK with OBB)
    device_api.c            the public API on top of the modules
    ocr_vision.m            on-device OCR with Apple's Vision framework
    scripts.c               each instance's scripts (~/Documents/SoulX Scripts/<instance>/)
    mcp.c                   the MCP server (`soulx mcp`), its settings and audit log
    window/                 SoulX's window (AppKit + Metal)
      app.m                 app delegate and menus
      window_controller.m   window, toolbar, status bar
      screen_view.m         Metal view, mouse/keyboard to Android
      frames.c              shared-memory frame reader, NEON BGRA→RGBA, frame pump
      keymap.c, keymap_editor.m   game keymaps
      param_panel.m         script parameter panel
  script/                   Lua 5.4 automation: soulx_lua.c (API), image.c (matching), record.c (recorder)
  third_party/              Lua 5.4, stb_image, stb_image_write
tests/macos/                unit tests
examples/scripts/           example Lua scripts
launcher/                  SwiftUI launcher (Swift package; see launcher/README.md)
docs/                       USAGE, SCRIPTING and this file
```

## Architecture

```
soulx (CLI) ───────┬─ SoulX core (C11) ─┬── emulator engine, no window (QEMU + gfxstream, Hypervisor.framework, Metal)
SoulX window ──────┘                   ├── adb (private server on 127.0.0.1:5137) ── apps, hardware keyboard (uhid)
(AppKit + Metal)                      ├── emulator console (TCP 127.0.0.1:<port>, token auth) ── touch, rotation
   ▲                                  └── Lua 5.4 + image matching + Vision OCR (automation)
   └── frames: shared memory "videmulator<port>", written by the engine
```

**Engine.** SoulX runs Google's Android Emulator 37.2.12 headless (`-no-window -share-vid`) with
`-no-snapshot-load -no-snapshot-save`. It never passes plain `-no-snapshot`, which gave a black screen with the ANGLE
path. Every engine and adb process gets an environment that points into `~/.soulx-android` (`HOME`, `ANDROID_HOME`,
`ANDROID_AVD_HOME`, `ANDROID_EMULATOR_HOME`, `TMPDIR`, the adb port). Instances use console ports 5600–5680, above the
range other adb servers scan. Processes are started with `posix_spawn` and argument arrays, never through a shell.

**GPU modes** map to engine arguments in `engine.c`:
- `host` is `-gpu host`: the GLES translator on Apple OpenGL (on Metal), and Vulkan on MoltenVK.
- `host-novulkan` is `-gpu host` without Vulkan.
- `software` is `-gpu swiftshader_indirect`.
- `metal` is `-gpu swangle -feature -Vulkan` with `SOULX_DEFAULT_PLATFORM=metal`.

The engine overwrites ANGLE's own `ANGLE_DEFAULT_PLATFORM` variable, so `runtime.c` patches the variable name in the
engine's `libGLESv2.dylib` to `SOULX_DEFAULT_PLATFORM`. The new name has the same length, the library is re-signed
ad hoc, and the original is kept as `.google`. The engine has the disable-library-validation entitlement, so it loads
the patched library.

**Display.** The engine publishes every frame in a shared-memory region. Its writer (`VideoFrameSharer::frameAvailable`)
reads pixels straight into the region, then bumps the frame number, then writes the timestamp. A reader can therefore
see a half-written frame. `frames.c` handles this in two ways:
- The window uses a *frame pump*: copy right after a new frame is published, then verify the copy by comparing rows.
- Scripts use `soulx_frames_copy_fresh`, which waits for a quiet moment.

Both convert B,G,R,A to R,G,B,A with NEON. The window uploads each verified frame to a Metal texture and turns it
upright for the current rotation. Display-size polling runs on its own queue, so it never delays input.

**Input.** Taps and swipes are multi-touch events over the emulator console (well under 1 ms each), written into the
guest's virtual touchscreen. The keyboard is a virtual USB keyboard (Android's `hid` tool over adb), so games get real
key-down and key-up events.

**Post-boot commands.** After each boot `engine.c` runs shell commands that are applied once per device and recorded
with a marker property:
- `soulx_nav`: three-button navigation.
- `soulx_lean`: `pm disable-user` for the lean package list, and `max_cached_processes 16`.
- `disable_window_blurs`: only for `metal`.

The same file stops the screen from sleeping when `stay_awake=1`.

**Why no Quick Boot.** Resuming a snapshot broke guest GPU sync objects: thousands of `glClientWaitSync` errors appeared,
and the engine leaked memory without bound. A fresh boot stays flat.

## Debugging

- `-v` or `SOULX_VERBOSE=1` prints debug output.
- `~/.soulx-android/logs/<name>.log` is the engine log. `<name>.window.log` is the window's log.
- `SOULX_ENGINE_FEATURES=A,-B` passes `-feature A -feature -B` to the engine, for trying engine features without
  rebuilding.
- `SOULX_HOME=/tmp/soulx-test` gives you a separate data folder, so experiments do not touch your instances.
- `soulx console …` and `soulx adb …` reach the running engine directly. `adb shell dumpsys gfxinfo <package>` shows
  frame timings.
- **Never attach lldb to a running engine.** With Hypervisor.framework, the engine dies ("unhandled exit"). Use
  `sample`, `vmmap` and `footprint` instead, and remember that `sample` slows the engine while it runs.

## Conventions

- C11 for the core and CLI, Objective-C only in `window/` and `ocr_vision.m`.
- Public symbols use the `soulx_` prefix, types `SoulX…`, macros `SOULX_…`.
- User-visible text follows the [USAGE.md](USAGE.md) wording. When you change a command, update its help text in
  `main_macos.c` and USAGE.md together.
- Add or update a unit test for every behaviour change, and keep `./scripts/build_macos.sh --test` green.
