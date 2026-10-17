-- tap_loop.lua - the simplest automation: swipe, tap and check a pixel, over and over.
--
-- Open your game first, then:
--     soulx run examples/scripts/tap_loop.lua          (50 rounds)
--     soulx run examples/scripts/tap_loop.lua 200      (200 rounds)
-- Ctrl+C stops it at any time.
--
-- Coordinates are screen pixels, the same as in `soulx screenshot` (x to the right, y down,
-- 0,0 = top-left). The numbers below were picked on a 1080x2400 screen; P() scales them to the
-- current screen so the example runs on any instance. In your own scripts plain numbers are fine:
--     tap(540, 1650)

local rounds = tonumber(args[1]) or 50
local W, H = screen_size()

local function P(x, y)
    return x * W // 1080, y * H // 2400
end

-- A point near (x, y): up to `r` pixels off, so that taps do not always hit the same pixel.
local function near(x, y, r)
    r = r or 8
    return x + math.random(-r, r), y + math.random(-r, r)
end

log(("screen %dx%d, %d rounds"):format(W, H, rounds))

local bx, by = P(540, 1650)            -- the main button
local dx, dy = P(1000, 80)             -- where a red "reward" badge appears
local cx, cy = P(980, 1700)            -- its "collect" button

for i = 1, rounds do
    local x1, y = P(300, 1500)
    local x2 = P(800, 1500)
    swipe(x1, y, x2, y, 200)            -- drag to the right in 200 ms

    tap(near(bx, by))
    sleep(800 + math.random(200))       -- 0.8 to 1.0 s

    -- exact colour check (0xRRGGBB); is_color() allows a tolerance instead:
    --     if is_color(dx, dy, 0xFF2D2D, 24) then ... end
    if color(dx, dy) == 0xFF2D2D then
        log("reward badge seen in round " .. i)
        tap(near(cx, cy, 5))
        sleep(500)
    end

    if i % 10 == 0 then
        log(("round %d of %d"):format(i, rounds))
    end
end

log("done")
