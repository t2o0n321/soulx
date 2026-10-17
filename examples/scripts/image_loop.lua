-- image_loop.lua - an image-driven loop: wait for the start button, tap it, wait until the round
-- is over, close the result screen, repeat. Every wait has a timeout and the number of rounds is
-- capped, so the script never taps blindly on a screen it does not know.
--
-- Templates (PNG files next to this script; see README.md, "Capturing templates"):
--     img/start.png   the button that starts a round
--     img/ok.png      the button that closes the result screen
--
-- Run:  soulx run examples/scripts/image_loop.lua [max_rounds]      (default 20)

local MAX_ROUNDS = tonumber(args[1]) or 20
local START = "img/start.png"
local OK = "img/ok.png"

-- Both buttons live in the lower half of the screen: searching only there is faster, and nothing
-- in the upper half can be mistaken for them.
local W, H = screen_size()
local LOWER = { region = { 0, H // 2, W, H - H // 2 } }

-- Taps a little off the exact centre, like a finger would.
local function tap_near(x, y)
    tap(x + math.random(-6, 6), y + math.random(-6, 6))
end

local done = 0
for round = 1, MAX_ROUNDS do
    -- 1. the start button must show up within 30 s
    local x, y = wait(START, 30, LOWER)
    if not x then
        log("no start button for 30 s; stopping")
        screenshot("image_loop-stuck.png")              -- a picture of what was on screen
        break
    end
    tap_near(x, y)
    log(("round %d: started"):format(round))

    -- 2. the round ends when the OK button appears (at most 3 minutes)
    x, y = wait(OK, 180, LOWER)
    if not x then
        log(("round %d did not end within 3 minutes; stopping"):format(round))
        screenshot("image_loop-stuck.png")
        break
    end
    tap_near(x, y)

    -- 3. the result screen must close before we look for the start button again
    if not wait_gone(OK, 10, LOWER) then
        local ox, oy = find(OK, LOWER)                  -- still there: the tap was missed
        if ox then
            tap_near(ox, oy)
        end
    end
    done = done + 1
    sleep(500 + math.random(500))
end

log(("%d of %d rounds played"):format(done, MAX_ROUNDS))
