-- name: 參數面板示範
-- Live parameter panel demo.
--
-- Run this from the SoulX window (Scripts ▸ Run script, or the 腳本 ▸ 執行腳本 toolbar menu). A small "腳本參數"
-- panel appears next to the window; drag the sliders, flip the switch, pick from the menu, press the button, and
-- watch the values update here in the log in real time — no need to stop and restart the script.
--
-- Run the same script headless (soulx run param_panel_demo.lua) and there is no panel: every control simply
-- returns its default, so a script written against the panel still works from the command line.

local speed  = param_slider("Speed", 1, 10, 5)                 -- min, max, default
local tiles  = param_number("Tiles", 1, 30, 12)               -- a precise number field
local auto   = param_toggle("Auto", true)                     -- on/off
local target = param_choice("Target", { "火", "水", "木", "光", "暗" }, "火")   -- a dropdown
local once   = param_button("Do it once")                     -- :pressed() is true once per click

log("Parameter panel demo — adjust the controls in the 腳本參數 panel. Press Stop to end.")

local n = 0
while true do
  n = n + 1
  if once:pressed() then
    log("  ▶ button pressed")
  end
  if auto:on() then
    log(string.format("#%d  speed=%d  tiles=%d  target=%s", n, speed:value(), tiles:value(), target:value()))
  else
    log(string.format("#%d  (auto is off)", n))
  end
  sleep(1000)
end
