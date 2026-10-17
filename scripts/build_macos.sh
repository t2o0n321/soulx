#!/usr/bin/env bash
#
# SoulX Android Emulator - build the macOS (Apple Silicon) port
#
# Builds build/macos/soulx with CMake. Works from any directory: paths are
# relative to this script, nothing is hard-coded.
#
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/build_macos.sh [--test] [--debug] [--clean]

  --test    run the unit tests (ctest) after building
  --debug   Debug build instead of Release
  --clean   delete build/macos before configuring
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

run_tests=0
build_type=Release
clean=0
for arg in "$@"; do
    case "$arg" in
        --test)    run_tests=1 ;;
        --debug)   build_type=Debug ;;
        --clean)   clean=1 ;;
        -h|--help) usage; exit 0 ;;
        *)         echo "error: unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

orig_pwd="$PWD"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
build_dir="build/macos"
cd "$repo_root"

# --- Host checks -------------------------------------------------------------
[[ "$(uname -s)" == "Darwin" ]] || die "this script builds the macOS port"
if [[ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" != "1" ]]; then
    die "SoulX for macOS needs an Apple Silicon (arm64) Mac; this Mac is $(uname -m)"
fi
if [[ "$(uname -m)" != "arm64" ]]; then
    echo "note: this shell runs under Rosetta ($(uname -m)); building a native arm64 binary anyway" >&2
fi
command -v cmake >/dev/null 2>&1 || die "cmake not found: install it with 'brew install cmake' or from https://cmake.org/download/"
command -v cc >/dev/null 2>&1 || die "no C compiler found: install the Xcode Command Line Tools with 'xcode-select --install'"

jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# --- Configure and build -----------------------------------------------------
if [[ "$clean" == 1 ]]; then
    echo "=== Removing $build_dir ==="
    rm -rf -- "${repo_root:?}/$build_dir"
fi

echo "=== Configuring SoulX Android for macOS ($build_type, arm64) ==="
cmake -S "$repo_root" -B "$repo_root/$build_dir" \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DCMAKE_OSX_ARCHITECTURES=arm64

echo "=== Building with $jobs jobs ==="
cmake --build "$repo_root/$build_dir" -j"$jobs"

if [[ "$run_tests" == 1 ]]; then
    echo "=== Running unit tests ==="
    (cd "$repo_root/$build_dir" && ctest --output-on-failure)
fi

# --- Summary -----------------------------------------------------------------
if [[ "$orig_pwd" == "$repo_root" ]]; then
    soulx_cmd="$build_dir/soulx"
else
    soulx_cmd="$repo_root/$build_dir/soulx"
fi

echo
echo "=== Build complete ==="
echo "Binary: $repo_root/$build_dir/soulx"
echo
echo "Next steps:"
echo "  $soulx_cmd setup            # one-time: install SoulX's own Android 17 runtime"
echo "  $soulx_cmd play game.apk    # create an instance, install the game and play it"
