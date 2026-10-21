# Changelog

Versions are vYY.M.PATCH (see "Versioning" in README.md). Changes go under "Unreleased"; the scripts in
scripts/versioning/ turn it into the release's section, which becomes the text of its GitHub Release.

## Unreleased

- **Network capture**: record an instance's WiFi traffic to a `.pcap` file for Wireshark whenever you want, without a
  restart. The launcher's toolbar has a record button (it shows the file in Finder when you stop), and
  `soulx network-capture-start` / `soulx network-capture-stop` do the same from the command line. Every instance records on its own,
  and nothing is recorded or written before you start. Files go to `~/Documents/SoulX Network Captures/<instance>/`.
  Instances that are already running need one restart first.
- WiFi: instances now run the engine's own WiFi instead of netsim, so a recording can be attached to one instance's
  `wlan0` while it runs. Inside Android it is the same `AndroidWifi` network, and downloads were as fast in a quick test.
- **Export and import instances**: save a stopped instance as one `.soulxvm` file (its apps, game progress, settings and
  scripts, plus the game keymaps) and add it again on this Mac or another one: **Export…** in an instance's ⋯ menu,
  **Import existing instance…** under the sidebar's +, or `soulx export` / `soulx import`. The Android runtime is not
  in the file. Every file is checked against checksums before the instance is added; an import never replaces an
  existing instance (a taken name gets another), and nothing is left behind when it fails.
- Deleting an instance also moves its scripts folder and network captures (in `~/Documents`) to the Trash, and takes it
  off AI control's list of allowed instances. They stayed behind before.
- **New look**: the launcher is redesigned with Liquid Glass. An instance's page shows its CPU, memory, storage and
  uptime, its apps (search, list or grid) and scripts side by side with its settings, and adapts to the window's width
  (cards re-flow instead of being cut off). New instance, AI control, first-run setup and SoulX's own window (toolbar
  card, rounded screen, centred title) follow the same design. Settings that cannot change while an instance runs are
  greyed out, and so are AI control's settings while it is off.
- First run: setup shows the three steps from installing to playing, and the rest of the launcher waits until the
  runtime is installed. An install can be cancelled; it resumes where it stopped.
- **Repair the runtime**: when runtime files go missing (a folder deleted by hand), the launcher notices within half a
  minute and offers to download just those again. The wrench in the sidebar's bottom card reinstalls the whole runtime.
  Instances and their data are kept.
- **Show an instance's files in Finder** from its ⋯ menu. `soulx list --json` reports the folder as `files_dir`.

## v26.10.2 (2026-10-18)

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
