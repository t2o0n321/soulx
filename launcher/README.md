# SoulX launcher

A small SwiftUI app for managing SoulX instances without the terminal: create, configure, start (with or without a
window), stop and delete instances, choose each instance's emulator engine (standard, or Angle for Google Play
sign-in) and networking (NAT, Host-only or Bridged, installing the network helper when one is first needed), see
their CPU and memory use live, set the volume, install, open and uninstall apps (shown by name; APKs can be dropped
on the page), manage each instance's scripts (import, rename, show in Finder, move to the Trash; running them stays
in SoulX's window), root an instance with Magisk and open an Android shell or a root shell, record its WiFi traffic
to a `.pcap`, export and import instances as `.soulxvm` files, turn AI control (MCP) on or off, connect AI apps with
one click, set its permissions and see what the AI did, switch the language, install the Android runtime on first
use, and update itself from GitHub releases.

It drives SoulX only through the `soulx` CLI — reading its `--json` output (`list`, `config`, `runtime`,
`stats --watch`, …) and running its commands (create, start/stop, export/import, root, `net`, network capture,
shell, volume, language, …) — so it never depends on SoulX's file formats.

Its only network access is the update check: `GET https://api.github.com/repos/t2o0n321/soulx/releases/latest`
when it opens and every 6 hours, and the release's `SoulX-v<version>.zip` and `.sha256` when the user updates. A
build run from source has no version and never checks.

## Run it from source

```bash
./scripts/build_macos.sh          # the soulx CLI (the launcher finds build/macos/soulx)
cd launcher
swift run SoulXLauncher
```

It looks for `soulx` in this order: `$SOULX_CLI`, inside its app bundle, the repository's `build/macos/soulx`,
`/usr/local/bin/soulx`, `/opt/homebrew/bin/soulx`.

## Tests

```bash
cd launcher && swift test
```

With only the Command Line Tools installed (no Xcode), pass the Testing framework's location:

```bash
F=/Library/Developer/CommandLineTools/Library/Developer/Frameworks
L=/Library/Developer/CommandLineTools/Library/Developer/usr/lib
swift test -Xswiftc -F -Xswiftc $F -Xlinker -F -Xlinker $F -Xlinker -rpath -Xlinker $F -Xlinker -rpath -Xlinker $L
```

## Layout

- `Sources/SoulXCore`: CLI access (`SoulXCLI`), JSON models, settings and the arguments built from them, and updates
  (`Updates.swift`: the version check and the installer that downloads, checks and swaps SoulX.app). No UI.
- `Sources/SoulXLauncher`: the SwiftUI app (instance list, instance page, create sheet, runtime setup).
  Text follows the system language: Traditional Chinese, Simplified Chinese or English.
- `Tests/SoulXCoreTests`: Swift Testing tests, including a fake `soulx` for the CLI runner.
