# patches/

Source patches applied on top of upstream when building SoulX's **"angle" engine**
(a patched Android emulator). The multi-GB source trees they apply to are fetched by
`scripts/build_angle_emulator.sh` and are NOT tracked.

Patches are named by the subtree they apply to, and `scripts/build_angle_emulator.sh`
applies each one in the matching tree:

| prefix        | subtree                     |
|---------------|-----------------------------|
| `gfxstream-*` | `hardware/google/gfxstream` |
| `qemu-*`      | `external/qemu`             |

**Base: AOSP `emu-main-dev` (emulator 37.3.6).** The patches were generated against:

| project                            | commit                                     |
|------------------------------------|--------------------------------------------|
| `platform/hardware/google/gfxstream` | `49ee51f297118cd2e98c5f4867df556e8966562a` |
| `platform/external/qemu`             | `cf0f40a0aad1aa3fca68f2f89372525c8d28ca68` |

`emu-main-dev` moves; a later sync may shift line numbers (git apply tolerates offsets).
`emu-35-6/` keeps the patches for the abandoned emu-35-6-release base (it is too old for the
Android 37 images: they require emulator >= 36.5.11, and its GuestAngle mode only offers
GLES 3.0). The build script only applies `patches/<prefix>-*.patch`, never that folder.

## gfxstream-vk-readback-onpost.patch

**Applies to:** `hardware/google/gfxstream`, path `host/frame_buffer.cpp`.

**Why:** On macOS, Apple's host OpenGL ES tops out at GLES 3.0, but Android API 37 /
the Google sign-in Chromium WebView needs GLES 3.1. The emulator's own fix is
`-feature GuestAngle`: the guest renders GLES through ANGLE→Vulkan, and for API 35+ the
emulator then reports GLES 3.1. Under GuestAngle the host has no GL emulation, so there is
no GL `ReadbackWorker` — but SoulX feeds its native Metal window through `-share-vid`, whose
readback is GL-only. Upstream's onPost then posts a buffer that is never filled (the window
freezes), and the readback thread dereferences the missing worker (`m_readbackWorker->init()` /
`->getPixels()` on nullptr) as soon as `-share-vid` registers its callback.

**What it changes** (Vulkan-only host = `!m_emulationGl && m_emulationVk`; the GL paths are
untouched):
1. `postImpl()` onPost: reads the posted ColorBuffer with `ColorBuffer::readToBytes`
   (→ `ColorBufferVk::readToBytes`, upstream since ~2025-03) into `onPost::img`, swapping R and B
   when the consumer wants the other order: `-share-vid` asks for B,G,R,A (`readBgra`), which is
   what SoulX's frame reader expects (`source/platform/macos/window/frames.h`).
2. `sendReadbackWorkerCmd()`: on a Vulkan-only host, GetPixels copies that frame (guarded by its
   own lock, never `m_lock`, which the post thread holds while it waits for GetPixels); Init /
   AddRecordDisplay are no-ops; DelRecordDisplay forgets the frame before `onPost::img` is freed.
3. `ensureReadbackWorker()` / `flushReadPipeline()`: no GL worker to create or flush, so no
   per-frame "GL/EGL emulation not enabled" error and no null dereference.

The null dereference in the readback thread is the likely cause of the single-core 100% CPU seen
with the emu-35-6 patch under `-share-vid` (that patch only stopped the abort in
`ensureReadbackWorker`); confirm with `sample <qemu pid>` if it persists.

**Verify after building:** colours (R/B order) and orientation of SoulX's window match the
legacy engine; `adb shell getprop ro.opengles.version` reports `196609` (GLES 3.1).

**Regenerate** (from the SoulX repo root, after editing the build tree):
```
git -C external/emu-build/hardware/google/gfxstream diff host/frame_buffer.cpp \
  > patches/gfxstream-vk-readback-onpost.patch
```

## qemu-macos-sdk-detection.patch

**Applies to:** `external/qemu`, path `android/scripts/unix/gen-android-sdk-toolchain.sh`.

**Why:** `get_osx_sysroot()` (shared by the darwin and darwin-aarch64 builds) only accepts the
SDKs in a hardcoded list (`13.3 15.2 15.5 26.0 26.2 26.4`). Xcode 26.6 ships the 26.5 SDK, so
configure stops with `Required sdk (13.3) not available..`.

**What it changes:**
1. Parses `xcodebuild -showsdks` with POSIX `grep -oE 'macosx[0-9]+\.[0-9]+'`, one entry per
   SDK, sorted by version.
2. When no SDK from the list is installed, uses the newest installed SDK (and logs it), so newer
   macOS releases build without editing the list.

**Regenerate** (from the SoulX repo root, after editing the build tree):
```
git -C external/emu-build/external/qemu diff android/scripts/unix/gen-android-sdk-toolchain.sh \
  > patches/qemu-macos-sdk-detection.patch
```
