# SoulX launcher

A small SwiftUI app for managing SoulX instances without the terminal: create, configure, start (with or without a
window), stop and delete instances, see their CPU and memory use live, set the volume, manage each instance's
scripts (import, rename, show in Finder, move to the Trash; running them stays in SoulX's window), turn AI control
(MCP) on or off, connect AI apps with one click, set its permissions and see what the AI did, switch the language, and install the Android runtime
on first use.

It talks to SoulX only through the `soulx` CLI's JSON output (`list`, `config`, `stats --watch`, `volume`,
`runtime`, `scripts`, `language`, all with `--json`), so it never depends on SoulX's file formats.

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

- `Sources/SoulXCore`: CLI access (`SoulXCLI`), JSON models, settings and the arguments built from them. No UI.
- `Sources/SoulXLauncher`: the SwiftUI app (instance list, instance page, create sheet, runtime setup).
  Text follows the system language: Traditional Chinese, Simplified Chinese or English.
- `Tests/SoulXCoreTests`: Swift Testing tests, including a fake `soulx` for the CLI runner.
