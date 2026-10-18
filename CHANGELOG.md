# Changelog

Versions are vYY.M.PATCH (see "Versioning" in README.md). Changes go under "Unreleased"; the scripts in
scripts/versioning/ turn it into the release's section, which becomes the text of its GitHub Release.

## Unreleased

- Scripts: `load_image` opens relative paths from the script's folder, like templates, `screenshot`, `img:save` and
  `install` do. It used the working folder, so scripts started from SoulX.app could not load their own images (the
  command line worked when run from the script's folder). Under AI control it is now limited to the scripts folder
  like the other file functions.

## v26.10.1 (2026-10-18)

- **Updates**: SoulX.app looks for a new release on GitHub when it opens (and every 6 hours) and offers it, with what
  changed, in its sidebar. **Update now** downloads it, checks its SHA-256, identifier, version and signature,
  replaces the app and reopens it; nothing changes if a check fails. **Update automatically the next time SoulX
  opens** installs new versions at start. Versions before this one cannot update themselves: install it from the
  .dmg once.
- Scripts: `read_text` (OCR) reads Traditional and Simplified Chinese as documented; it recognised English only.

## v26.10.0 (2026-10-17)

First release of SoulX as a Mac app.

- **SoulX.app**: open it, create an instance and play. The launcher creates, configures, starts (with or without a
  window), stops and deletes instances, shows their CPU and memory use, sets the volume, manages each instance's
  scripts and installs the Android 17 runtime on first use. English, Traditional and Simplified Chinese.
- **SoulX's window**: Android on the Mac's GPU, toolbar, game keymap, script recording and running, screenshots.
- **Lua automation**: taps, swipes, OCR, image matching and colours; scripts kept per instance.
- **AI control (MCP)**, off by default: let an AI app look at the screen and play, connected with one click from the
  launcher; per-instance and per-action permissions, sandboxed Lua and an audit log. Local only: no network port.
- Settings per instance: performance profile, orientation, resolution, frame rate cap, audio.
