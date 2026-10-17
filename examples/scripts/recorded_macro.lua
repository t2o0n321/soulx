-- EXAMPLE: below the dashed line is a recording exactly as `soulx record` writes it.
-- Record your own:   soulx record my_macro.lua    (play by hand, then press Ctrl+C)
-- Replay it:         soulx run my_macro.lua
-- A recording is plain Lua: edit it freely, e.g. wrap it in `for round = 1, 10 do ... end`
-- or replace a fixed sleep() before an important tap with wait("img/button.png", 10).
-- ------------------------------------------------------------------------------------------

-- SoulX macro recorded with `soulx record` from instance 'default' on 2026-10-12 15:42:07
-- Screen: 1080x2400 logical pixels (rotation 0 degrees; panel 1080x2400)
-- Replay: soulx run recorded_macro.lua   (on this instance: soulx run -i default recorded_macro.lua)
-- Coordinates are logical screen pixels, the same as in screenshots.

tap(540, 1650)
sleep(1240)
swipe(300, 1500, 800, 1500, 210)
sleep(870)
tap(980, 1700)
sleep(2010)
long_press(200, 400, 900)
sleep(650)
key("BACK")
sleep(1500)
tap(540, 600)
sleep(400)
text("player1")
sleep(300)
key("ENTER")

-- end of recording: 8 actions in 8.1 s
