#!/usr/bin/env bash
#
# build_angle_emulator.sh — build SoulX's "angle" engine.
#
# This produces a PATCHED Android emulator whose gfxstream host renderer can read
# back a Vulkan-rendered color buffer into the -share-vid shared-memory region.
# That lets `-feature GuestAngle` (which gives the guest GLES 3.1 via ANGLE→Vulkan
# — required by the Google sign-in WebView on macOS, where Apple's host GLES is
# capped at 3.0) work together with SoulX's native Metal window. Without the patch
# GuestAngle aborts with "ensureReadbackWorker: GL/EGL emulation not enabled".
#
# REPRODUCIBILITY
#   Tracked in git : this script + patches/gfxstream-*.patch (+ the pinned
#                    manifest once generated). Any macOS arm64 host or CI runner
#                    re-creates the build from these.
#   NOT tracked    : the multi-GB AOSP source trees under external/emu-build/ —
#                    this script fetches them from upstream.
#
# PREREQS  macOS (arm64), Xcode + command line tools, git, python3, curl,
#          ~80 GB free disk, network access.
#
# USAGE    scripts/build_angle_emulator.sh            # full: fetch + patch + build
#          scripts/build_angle_emulator.sh --pin      # after a sync: write the
#                                                      # pinned manifest for CI
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/external/emu-build"
PATCHES_DIR="$REPO_ROOT/patches"
PINNED_MANIFEST="$REPO_ROOT/scripts/angle-emulator.pinned.xml"
LOCAL_MANIFEST="$REPO_ROOT/scripts/angle-emulator.local_manifest.xml"
OUT_DIR="$REPO_ROOT/build/angle-emulator"
ENTITLEMENTS="$REPO_ROOT/scripts/emu-entitlements.plist"
SOULX_REV="soulx1"   # SoulX patch revision; bump when the patches change for the same emulator base

# Branch of the Android emulator (QEMU) manifest. emu-main-dev is emulator 37.3.6
# (newer than the 37.2.12 SoulX ships for the legacy engine): it boots the Android 37
# image, forces the guest to GLES 3.1 under GuestAngle (what the Play sign-in WebView
# needs), and still has the Vulkan readback primitive (ColorBufferVk::readToBytes) our
# patch wires into -share-vid. (emu-35-6 was tried first but sits below the Android 37
# image's minimum of emulator 36.5.11 and only exposes GLES 3.0.) It is a dev branch,
# so run `--pin` after a sync to record an exact manifest for reproducible / CI builds.
MANIFEST_URL="https://android.googlesource.com/platform/manifest"
MANIFEST_BRANCH="emu-main-dev"

log() { printf '\n==> %s\n' "$*"; }

ensure_tools() {
  mkdir -p "$HOME/.bin"
  export PATH="$HOME/.bin:$BUILD_DIR/.depot_tools:$PATH"
  if [ ! -x "$HOME/.bin/repo" ]; then
    log "installing repo"
    curl -fsSL https://storage.googleapis.com/git-repo-downloads/repo -o "$HOME/.bin/repo"
    chmod +x "$HOME/.bin/repo"
  fi
  if [ ! -d "$BUILD_DIR/.depot_tools" ]; then
    log "cloning depot_tools"
    git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git \
      "$BUILD_DIR/.depot_tools"
  fi
}

fetch_source() {
  mkdir -p "$BUILD_DIR"; cd "$BUILD_DIR"
  log "repo init ($MANIFEST_BRANCH)"
  repo init -u "$MANIFEST_URL" -b "$MANIFEST_BRANCH" --depth=1
  if [ -f "$PINNED_MANIFEST" ]; then
    log "using pinned manifest for a deterministic sync"
    cp "$PINNED_MANIFEST" .repo/manifests/soulx-pinned.xml
    repo init -m soulx-pinned.xml
    # The pinned manifest is captured AFTER the trim below has already removed those projects, so it does not
    # list them at all. Applying the trim manifest on top then fails ("remove-project element specifies
    # non-existent project"), so only trim when there is no pin — the pin is already the trimmed set.
  elif [ -f "$LOCAL_MANIFEST" ]; then
    # Skip projects this build never uses (~47 GB, mostly system-images). See the file's own comment.
    mkdir -p .repo/local_manifests
    cp "$LOCAL_MANIFEST" .repo/local_manifests/soulx-trim.xml
  fi
  log "repo sync (this is the long one)"
  repo sync -c -j4 --no-tags --fail-fast
  trim_unused
}

# Qt and clang are single git repos whose unused platforms/versions are subdirectories, so the manifest
# cannot exclude them (unlike the projects in angle-emulator.local_manifest.xml). Drop them after sync:
# this darwin_aarch64 build uses only Qt's darwin-aarch64 and the clang version(s) pinned in toolchains.json.
# Each item was verified unreferenced in the generated build graph. Saves ~22 GB and is safe to re-run.
trim_unused() {
  local qt="$BUILD_DIR/prebuilts/android-emulator-build/qt"
  if [ -d "$qt" ]; then
    log "trimming unused Qt platforms (keeping darwin-aarch64)"
    find "$qt" -mindepth 1 -maxdepth 1 -type d \
      ! -name 'darwin-aarch64' ! -name 'darwin-aarch64-nowebengine' -exec rm -rf {} +
  fi
  local clangdir="$BUILD_DIR/prebuilts/clang/host/darwin-x86"
  local toolchains="$BUILD_DIR/external/qemu/android/build/toolchains.json"
  if [ -d "$clangdir" ] && [ -f "$toolchains" ]; then
    local keep; keep=$(grep -oE 'clang-r[0-9a-z]+' "$toolchains" | sort -u)
    log "trimming unused clang versions (keeping: $(echo $keep))"
    local d base
    for d in "$clangdir"/clang-r*; do
      [ -d "$d" ] || continue
      base=$(basename "$d")
      printf '%s\n' "$keep" | grep -qx "$base" || rm -rf "$d"
    done
  fi
}

apply_patches() {
  shopt -s nullglob
  # Patches are named by the subtree they apply to:
  #   gfxstream-*.patch -> hardware/google/gfxstream  (Vulkan readback for -share-vid)
  #   qemu-*.patch      -> external/qemu              (macOS 26+ SDK detection, etc.)
  local spec tree prefix p
  for spec in "hardware/google/gfxstream:gfxstream" "external/qemu:qemu"; do
    tree="$BUILD_DIR/${spec%%:*}"; prefix="${spec##*:}"
    cd "$tree"
    for p in "$PATCHES_DIR"/${prefix}-*.patch; do
      if git apply --reverse --check "$p" >/dev/null 2>&1; then
        echo "already applied: $(basename "$p")"
      else
        git apply --check "$p"
        git apply "$p"
        echo "applied: $(basename "$p")"
      fi
    done
  done
}

# The emulator's Mach-O binaries need a valid signature carrying the Hypervisor / JIT entitlements, or qemu
# cannot use Hypervisor.framework (fails with HV_DENIED). The AOSP build's own install-time signing is
# incomplete on a from-source build, so re-sign every Mach-O ad-hoc with scripts/emu-entitlements.plist.
# Ad-hoc (no Developer ID, not notarized) is fine: SoulX clears the download quarantine on install.
sign_distribution() {
  local dist="$1" f n=0
  [ -f "$ENTITLEMENTS" ] || { log "ERROR: missing $ENTITLEMENTS"; exit 1; }
  log "re-signing emulator Mach-O (ad-hoc + entitlements)"
  while IFS= read -r f; do
    file "$f" 2>/dev/null | grep -q "Mach-O" || continue
    if codesign --force -s - --entitlements "$ENTITLEMENTS" "$f" >/dev/null 2>&1; then n=$((n+1)); fi
  done < <(find "$dist" -type f \( -name '*.dylib' -o -name '*.so' -o -perm -u+x \) | sort)
  log "re-signed $n Mach-O files"
  codesign -v "$dist/qemu/darwin-aarch64/qemu-system-aarch64" >/dev/null 2>&1 \
    || { log "ERROR: qemu signature invalid after re-sign"; exit 1; }
}

# Pack the signed distribution into a versioned zip whose top level is emulator/ (the installer's zip_root),
# plus a manifest with sha1/size to paste into runtime.c and attach to the GitHub Release. ditto keeps
# symlinks and the embedded code signatures.
package_distribution() {
  local dist="$1" rev zipname zip sha size
  rev="$(grep -m1 '^Pkg.Revision' "$BUILD_DIR/external/qemu/source.properties" | cut -d= -f2 | tr -d ' \r')"
  ANGLE_VERSION="${rev}-${SOULX_REV}"
  zipname="emulator-angle-${ANGLE_VERSION}-darwin_aarch64.zip"
  zip="$OUT_DIR/$zipname"
  mkdir -p "$OUT_DIR"; rm -f "$zip"
  log "packaging -> $zipname"
  ditto -c -k --keepParent "$dist" "$zip"
  sha="$(shasum -a 1 "$zip" | cut -d' ' -f1)"
  size="$(stat -f%z "$zip")"
  printf 'version=%s\nfile=%s\nsize=%s\nsha1=%s\ntag=engine-angle-%s\n' \
    "$ANGLE_VERSION" "$zipname" "$size" "$sha" "$ANGLE_VERSION" > "$OUT_DIR/emulator-angle.manifest"
  log "done: $zip"
  log "  version=$ANGLE_VERSION size=$size sha1=$sha"
  log "  release tag: engine-angle-$ANGLE_VERSION  (fill url/size/sha1 into runtime.c k_angle_package)"
}

build() {
  cd "$BUILD_DIR/external/qemu"
  # Cap build parallelism by RAM. The emulator + QtWebEngine + protoc compiles are memory-hungry; a full
  # -j<ncpu> pushes a smaller Mac into swap until the OOM killer takes a process ("Killed: 9", e.g. protoc).
  # rebuild.sh builds via `cmake --build`, which honours CMAKE_BUILD_PARALLEL_LEVEL. ~1 job per 4 GB, >=2,
  # never more than the CPU count.
  local ram_gb ncpu jobs
  ram_gb=$(( $(sysctl -n hw.memsize) / 1073741824 ))
  ncpu=$(sysctl -n hw.ncpu)
  jobs=$(( ram_gb / 4 )); [ "$jobs" -lt 2 ] && jobs=2; [ "$jobs" -gt "$ncpu" ] && jobs=$ncpu
  export CMAKE_BUILD_PARALLEL_LEVEL="$jobs"
  log "building emulator (darwin_aarch64) with -j$jobs (${ram_gb} GB RAM) — hours on a cold cache"
  # rebuild.sh runs the emulator's own unit tests at the end and exits non-zero if any fail; several need a
  # GPU/display a headless CI host lacks, so a non-zero exit does NOT mean the build failed. Verify the
  # artifact instead of trusting the exit code.
  android/rebuild.sh --target=darwin_aarch64 \
    || log "rebuild.sh exited non-zero (often just its unit tests) — verifying the build artifact"
  local dist="$BUILD_DIR/external/qemu/objs/distribution/emulator"
  if [ ! -x "$dist/emulator" ] || [ ! -f "$dist/lib64/libgfxstream_backend.dylib" ]; then
    log "ERROR: build did not produce $dist/emulator + the patched gfxstream — real build failure"
    exit 1
  fi
  sign_distribution "$dist"
  package_distribution "$dist"
}

write_pinned_manifest() {
  cd "$BUILD_DIR"
  # Pin the host-platform (platform-darwin) group too: the build needs the darwin host prebuilts — python,
  # clang, cmake, ninja, bazel — which are marked "notdefault,platform-darwin". `repo sync` adds the host
  # platform group automatically, but `repo manifest` only honours manifest.groups, so without this the pin
  # omits them and a CI sync from the pin has no python/clang/... ("rebuild.sh: .../python3: No such file").
  repo init -g default,platform-darwin >/dev/null
  repo manifest -r -o "$PINNED_MANIFEST"
  log "pinned manifest written to $PINNED_MANIFEST (commit it)"
}

if [ "${1:-}" = "--pin" ]; then
  write_pinned_manifest; exit 0
fi

ensure_tools
fetch_source
apply_patches
build
