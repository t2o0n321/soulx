# Example automation scripts

SoulX runs Lua 5.4 scripts that play a game for you. These examples cover the usual building
blocks; the complete reference is [docs/SCRIPTING.md](../../docs/SCRIPTING.md).

| Script | What it shows |
|---|---|
| [tap_loop.lua](tap_loop.lua) | Taps, swipes and pixel colours in a loop, with random jitter. No images needed. |
| [image_loop.lua](image_loop.lua) | Image-driven loop: wait for a button, tap it, wait for the result. Timeouts and a round limit. |
| [recorded_macro.lua](recorded_macro.lua) | What `soulx record` writes, ready to replay with `soulx run`. |
| [daily_routine.lua](daily_routine.lua) | A complete routine: restart the game, get past the title screen and pop-ups, play several battles, stop cleanly. |

## Running a script

```sh
soulx start                                   # or: soulx start --headless (no window)
soulx run examples/scripts/tap_loop.lua       # Ctrl+C stops it at any time
soulx run examples/scripts/tap_loop.lua 200   # words after the script name arrive in args[1], args[2], ...
soulx run -i farm2 examples/scripts/image_loop.lua   # another instance
```

The script talks to the instance chosen with `-i NAME` (default: `$SOULX_INSTANCE`, else
`default`). It must be running. While a script runs you can keep watching the emulator window;
avoid touching it, or the script and you will fight over the screen.

`soulx run` prints `script finished in 12.3 s` when the script ends, `script stopped (Ctrl+C)
after 12.3 s` when you stop it (exit code 130), and `script error: file.lua:LINE: message` plus a
stack trace when something goes wrong (exit code 1).

## Capturing templates

`tap("img/start.png")`, `wait(...)`, `find(...)` and friends look for a small picture (a
*template*) on the screen. Templates are PNG files; relative paths are relative to the folder of
the script, so keep them in an `img/` folder next to it.

1. Bring the game to the screen that shows the button and take a screenshot:

   ```sh
   soulx screenshot shot.png
   ```

2. Cut the button out of `shot.png` with any image editor (Preview: drag a rectangle, then
   Tools > Crop, then File > Export as PNG) and save it as `img/start.png`.

   Or let a script do it. Read the rectangle (x, y, width, height) off the screenshot and run a
   two-line script:

   ```lua
   capture():crop(440, 1610, 200, 80):save("img/start.png")
   log("saved")
   ```

   `screenshot("img/start.png", 440, 1610, 200, 80)` does the same in one call.

Rules for good templates:

- **Take them from SoulX** (`soulx screenshot`, `capture()`, `screenshot()`), never from a macOS
  screenshot of the emulator window: the window is scaled (Retina) and matching works pixel for
  pixel, without resizing.
- **Small and distinctive**: the button's label or icon, not a whole panel. 40 to 200 pixels
  wide is typical.
- **Only the parts that never change**: leave out counters, timers, animated glows and the
  background behind rounded corners. Parts you cannot crop away can be made transparent (a PNG
  with alpha): pixels with alpha below 128 are ignored.
- **One template per look**: a pressed or disabled button looks different; save each state you
  want to recognise.

## Tips

- **Regions** make searches faster and safer: `find("img/ok.png", {region = {0, 1200, 1080, 1200}})`
  only looks at the lower half of a 1080x2400 screen, so nothing elsewhere can match.
- **Thresholds**: the default `0.88` suits most buttons. Print the score to calibrate:
  `log(find("img/ok.png"))` shows `x y score`. Raise the threshold (0.95) when similar buttons
  are confused, lower it (0.8) for buttons with soft animations.
- **Colours**: matching compares brightness structure first, then checks the colours
  (`color_tolerance`, default 40). Lower it to tell a red button from a grey one with the same
  shape; `color_tolerance = 0` turns the check off.
- **Jitter**: add a few random pixels to taps and random milliseconds to pauses
  (`sleep(800 + math.random(200))`), so that no two rounds are identical.
- **Safety first**: give every `wait()` a timeout and check its result, cap loops with a maximum
  number of rounds, and stop (or `screenshot("stuck.png")` and stop) when something unexpected
  shows up. A script that taps blindly on the wrong screen can spend your in-game currency.
- **Headless**: `soulx start --headless` runs the emulator without a window; scripts work the same.
- **Log progress** with `log(...)`: messages carry the time since the script started, e.g.
  `[12.3s] round 4 done`.

## Recording instead of writing

```sh
soulx record my_macro.lua     # play by hand; Ctrl+C stops the recording
soulx run my_macro.lua        # replays your taps, swipes, keys and the pauses between them
```

A recording is a plain Lua script (see [recorded_macro.lua](recorded_macro.lua)). To repeat it,
wrap its lines in a loop:

```lua
for round = 1, 20 do
    -- recorded lines here
end
```

and consider replacing fixed `sleep(...)` calls before important taps with `wait("img/...", 10)`,
so that the macro waits for the game instead of hoping it is fast enough.
