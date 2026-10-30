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

The build produces two binaries: `build/macos/soulx` (the CLI, about 600 KB, with Lua built in) and
`build/macos/soulx_netd` (a small privileged vmnet bridge helper for Host-only / Bridged networking; its own CMake
target, installed to `libexec`). To use the CLI from anywhere:

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

The binary targets macOS 12 by default (`CMAKE_OSX_DEPLOYMENT_TARGET`). The version has a single source, the
`VERSION` file (YY.M.PATCH; CMake passes it to the code as `SOULX_VERSION_STRING`).

**"You have not agreed to the Xcode license agreements".** When Xcode is installed but its license was never accepted,
`cc`, `git` and `python3` refuse to run. Accept it with `sudo xcodebuild -license`, or build with the Command Line
Tools instead:

```bash
DEVELOPER_DIR=/Library/Developer/CommandLineTools ./scripts/build_macos.sh --test
```

## Packaging and releases

```bash
./scripts/package_macos.sh             # build/dist/SoulX.app, SoulX-v<version>.dmg, .zip and .sha256
./scripts/package_macos.sh --skip-build --no-dmg
```

`SoulX.app` holds the launcher (`Contents/MacOS/SoulX`), the CLI (`Contents/Helpers/soulx`, which also runs SoulX's
window and the MCP server), the vmnet bridge helper (`Contents/Helpers/soulx_netd`, for Host-only / Bridged
networking) and the icon (drawn by `scripts/make_icon.swift`, the same drawing as `SoulXAppIcon()` in
the window). Its version comes from `VERSION`; the build number is the commit count. The app is signed
ad hoc only: it is deliberately not signed with a Developer ID nor notarized, so people allow it once in
System Settings > Privacy & Security.

Releases are published by `.github/workflows/release.yml`:

1. Describe the changes under `## Unreleased` in `CHANGELOG.md` and commit.
2. Run `scripts/versioning/bump-version-calendar.sh` for the first release of a month (`YY.M.0`) or
   `scripts/versioning/bump-version-patch.sh` for another one in the same month (`YY.M.PATCH+1`). It writes
   `VERSION`, turns `## Unreleased` into `## v<version> (<date>)`, commits `Release v<version>` and tags it
   (`--dry-run` shows what it would do). Then `git push origin main v<version>`.
3. The workflow checks that the tag matches the version, runs every test, packages the app and creates the GitHub
   Release with the .dmg, .zip and checksums. Its text is the CHANGELOG section plus the install steps
   (`scripts/release_notes.sh`).

Running the workflow by hand (Actions > Release > Run workflow) builds the .dmg as an artifact without publishing.
`ci.yml` builds and tests on every push and PR — a release/debug matrix for the CLI plus a separate SwiftUI launcher
job — so a broken build or package shows up before a release.

The **angle engine** is released separately by `.github/workflows/engine-angle.yml`: pushing an
`engine-angle-<emuver>-soulx<N>` tag builds, signs and publishes the patched emulator with
`scripts/build_angle_emulator.sh` (AOSP `emu-main-dev`, the pinned + local manifests in `scripts/`, the patches in
`patches/`, the entitlements in `scripts/emu-entitlements.plist`). A rebuild's ad-hoc signature changes the zip's
SHA-1, so after it runs, pin the SHA-1 and size it prints into `k_angle_package` in `runtime.c` (bump `SOULX_REV` in
the build script when the patches change) and cut the matching app release.

## Tests

`tests/macos/test_*.c` are standalone unit tests, one executable each. They need no emulator, network or runtime:
fakes stand in for the engine, adb and the console.

| test | covers |
|---|---|
| `test_paths_proc` | `SOULX_HOME` paths, process spawning and environments |
| `test_registry` | instance files, profiles, engine arguments, post-boot commands (lean, navigation), network capture (against a fake QMP socket) |
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
CMakeLists.txt              macOS-only build; targets `soulx` and `soulx_netd`, test target `soulx_tests`
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
    engine.c                starting/stopping the engine (legacy/angle), GPU modes, post-boot commands
    magisk.c                one-click Magisk root (`soulx root`): patches an instance's ramdisk
    netd/soulx_netd.c       privileged vmnet bridge daemon (Host-only / Bridged); its own CMake target → libexec
    netd_client.c           asks the soulx_netd daemon to open a bridge (SoulX itself stays unprivileged)
    vm_archive.c            export/import an instance as a `.soulxvm` file (Apple Archive)
    log.c                   macOS logging (DEBUG dropped unless -v / SOULX_VERBOSE=1)
    network_capture.c               network capture: the QMP client behind `network-capture-start` / `network-capture-stop`
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
      launch.c              starting and finding SoulX's window process
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

Each instance picks an **engine**. The default **legacy** engine is Google's 37.2.12 above. The **angle** engine
(`engine=angle`) is a self-built patched emulator — package `emulator-angle` 37.3.6-soulx1, built from AOSP
`emu-main-dev` by `scripts/build_angle_emulator.sh` with the patches in `patches/` — started with `-feature GuestAngle`
and `-systemui-renderer skiavk`. GuestAngle gives the guest OpenGL ES 3.1 (Apple's host GLES caps at 3.0), which the
Google Play / Google-account sign-in page needs; skiavk composes the guest UI straight on Vulkan so app UI renders
correctly. `runtime.c` downloads it on demand (`soulx setup --engine angle`) and pins the host Vulkan driver to
MoltenVK (`ANDROID_EMU_VK_SELECT_ICD=moltenvk`). See `k_angle_package` in `runtime.c` and the angle branch in `engine.c`.

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

**WiFi and network capture.** The engine runs with `-feature -WiFiPacketStream`, so `wlan0` uses the emulator's own
WiFi (`virtio-wifi`, a QEMU `user` network backend) instead of netsim. `network-capture-start` talks QMP to `run/<name>.qmp`
(the engine's `-qemu -qmp unix:...,server,nowait`) and adds a `filter-dump` object on the `virtio-wifi` backend with
`object-add`; `network-capture-stop` removes it with `object-del`. QEMU writes the pcap itself, so nothing exists before or
after. netsim cannot do this: its netsimd is shared by every running instance and only records everything from its
start (`--pcap`); its frontend service (`PatchCapture`) is switched off in the emulator's build, even with
`-feature NetsimCliUi` or `--dev`. The engine's `-tcpdump` and the console's `network capture` hook only `mynet`
(`eth0`, cellular), which Android leaves idle while WiFi is up. A netsimd exits by itself within seconds of its last
device going away, so SoulX does not stop it.

**Networking modes.** Each instance picks NAT (the default), Host-only or Bridged. NAT keeps the emulator's own
user-mode networking and needs no helper. Host-only and Bridged route the instance through `soulx_netd` vmnet bridges:
`engine.c` passes `-wifi-socket` (and, for Host-only, a second `-net-socket` for the radio) pointing at loopback ports
the helper opens. `soulx net install` sets up the privileged `soulx_netd` LaunchDaemon once (an admin prompt), and
`netd_client.c` asks it to open each bridge, so SoulX itself stays unprivileged.

**Why no Quick Boot.** Resuming a snapshot broke guest GPU sync objects: thousands of `glClientWaitSync` errors appeared,
and the engine leaked memory without bound. A fresh boot stays flat.

## Debugging

- `-v` or `SOULX_VERBOSE=1` prints debug output.
- `~/.soulx-android/logs/<name>.log` is the engine log. `<name>.window.log` is the window's log.
- `SOULX_ENGINE_FEATURES=A,-B` passes `-feature A -feature -B` to the engine, for trying engine features without
  rebuilding.
- `SOULX_HOME=/tmp/soulx-test` gives you a separate data folder, so experiments do not touch your instances.
- `SOULX_ENGINE_FEATURES=WiFiPacketStream` runs an instance on netsim WiFi again (capture refuses to start). `info network`
  through QMP (`human-monitor-command`) lists the backends; the built-in WiFi shows a `virtio-wifi-device` card.
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
