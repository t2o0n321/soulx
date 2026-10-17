# SoulX

**An Android 17 emulator for Apple Silicon Macs, built for playing and automating games.**

![Platform](https://img.shields.io/badge/macOS-Apple%20Silicon-000000?logo=apple&logoColor=white)
![Android](https://img.shields.io/badge/Android-17%20(API%2037)-3DDC84?logo=android&logoColor=white)
![License](https://img.shields.io/badge/License-MIT-green)
[![CI](https://github.com/t2o0n321/soulx/actions/workflows/ci.yml/badge.svg)](https://github.com/t2o0n321/soulx/actions/workflows/ci.yml)

SoulX runs Android 17 on M-series Macs through Hypervisor.framework and draws it on the Mac's GPU. It does one job
well: install your game, play it in a native macOS window, and let Lua scripts play it for you.

```bash
soulx setup                        # once: download SoulX's own Android runtime
soulx play ~/Downloads/game.apk    # create a phone, boot it, install the game, launch it
soulx run farm.lua                 # let a Lua script play
```

## Features

- **Self-contained.** `soulx setup` downloads its own emulator engine, adb and Android 17 system image into
  `~/.soulx-android` and checks every download. SoulX does not need or touch Android Studio, its SDK, Java or `~/.android`.
- **Uses the Mac's GPU.** Games render on Apple's GPU through gfxstream: Vulkan through MoltenVK and OpenGL ES through
  Apple's OpenGL on Metal. SoulX never falls back to software rendering unless you ask for it.
- **A native window.** The window is AppKit + Metal. It reads every frame straight from the engine's shared memory
  (about 0.1 ms after the engine writes it) and has a toolbar, screenshots, video recording and a coordinate/colour
  inspector.
- **Low memory.** Every start is a fresh boot, because resuming Quick Boot snapshots made the engine leak tens of GB.
  "Lean" mode disables preinstalled Google apps that games never use. Memory stays flat during long Auto play sessions.
- **Game keymaps.** Keyboard keys become taps, held touches, swipes or a WASD joystick, set in a visual editor.
- **Lua 5.4 automation.** Scripts can tap, swipe, press keys and type text. They can find images on the screen
  (template matching), read pixel colours and read text with on-device OCR (Vision). `soulx record` turns what you do
  into a script.
- **Multiple instances.** Several independent phones can each have their own apps, screen size, CPU and RAM. Each
  can run with or without a window.
- **Built for long sessions.** A frame rate cap (30 fps uses about 30% less engine CPU), per-instance sound
  control, and `soulx stats` for each instance's CPU and memory use.
- **Installs games in any common format:** `.apk`, `.xapk` (with OBB data), `.apks`, `.apkm` and folders of split APKs.

## Requirements

| | |
|---|---|
| Mac | Apple Silicon (M1 or later). Intel Macs are not supported. |
| macOS | 12 or later |
| Memory | 8 GB minimum, 16 GB recommended (each running instance gives Android 4 GB or more) |
| Disk | about 5 GB for the runtime, plus about 6 GB per instance |
| Network | once, for `soulx setup` (about 2.7 GB from dl.google.com) |

SoulX is macOS-only. Windows and Linux are not supported.

## Quick start

```bash
./scripts/build_macos.sh               # builds build/macos/soulx (see docs/DEVELOPMENT.md)
sudo cmake --install build/macos       # optional: installs /usr/local/bin/soulx

soulx doctor                           # checks this Mac
soulx setup                            # one-time download; asks you to accept Google's Android SDK license
soulx play ~/Downloads/game.apk        # play
```

In the window, the mouse is your finger and right-click is Back. The toolbar has Back, Home, Recents, Rotate, Record
script, Run script, Screenshot and Keymap. ⌘W stops the instance, and ⌥⌘W closes only the window.

## Documentation

| | |
|---|---|
| **[docs/USAGE.md](docs/USAGE.md)** | Using SoulX: every command and option, instance settings, GPU modes, the window, keymaps, data folders, performance and troubleshooting |
| **[docs/SCRIPTING.md](docs/SCRIPTING.md)** | The Lua automation API, with a tutorial |
| **[examples/scripts/](examples/scripts/)** | Complete example scripts |
| **[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)** | For developers: building from source, tests, project layout, architecture and debugging |

## Acknowledgements

SoulX is a fork of **[Soul Android Emulator](https://github.com/vikrant-project/soul-android-emulator)**, and we
built SoulX with Soul as our reference. Thank you to the Soul authors for the project and the groundwork it laid.
If you already have Soul data (`~/.soul-android`), SoulX moves it to `~/.soulx-android` the first time it runs.

SoulX is also built on:
- the [Android Emulator](https://developer.android.com/studio/run/emulator) (QEMU + gfxstream) and Android system
  images by Google, which `soulx setup` downloads under the Android SDK License (SoulX never redistributes them);
- [Lua 5.4](https://www.lua.org/) (MIT);
- [stb_image / stb_image_write](https://github.com/nothings/stb) (public domain / MIT).

## License

MIT. See [LICENSE](LICENSE).
