-- daily_routine.lua - a complete daily routine for a typical mobile game:
--   1. restart the game, 2. get past the title screen and the daily pop-ups,
--   3. play a number of battles (start -> auto -> victory -> collect), 4. leave cleanly.
-- Every step has a timeout; when something unexpected happens the script saves a screenshot,
-- stops the game and ends with exit code 1, so it never keeps tapping on a screen it does not know.
--
-- Run (start the instance first; `soulx start --headless` is fine):
--     soulx run examples/scripts/daily_routine.lua com.example.game [battles]
--
-- Templates (PNG files in img/ next to this script; see README.md, "Capturing templates"):
--     title.png     something that is always on the title screen (logo, "TAP TO START")
--     close.png     the X that closes the daily pop-ups (news, login bonus, offers)
--     home.png      something that is always on the home screen (e.g. the menu bar)
--     battle.png    the button that starts a battle
--     auto.png      the "auto battle" switch while it is OFF (delete that step if your game has none)
--     victory.png   a part of the victory screen
--     collect.png   the button that collects the reward

local PACKAGE = args[1] or "com.example.game"
local BATTLES = tonumber(args[2]) or 10

local IMG = {
    title = "img/title.png",
    close = "img/close.png",
    home = "img/home.png",
    battle = "img/battle.png",
    auto = "img/auto.png",
    victory = "img/victory.png",
    collect = "img/collect.png",
}

local W, H = screen_size()

-- Helpers -----------------------------------------------------------------------------------

local function tap_near(x, y)
    tap(x + math.random(-6, 6), y + math.random(-6, 6))
end

-- Waits up to `timeout` seconds for an image and taps it; false when it never showed up.
local function tap_when_visible(img, timeout, opts)
    local x, y = wait(img, timeout, opts)
    if not x then
        return false
    end
    tap_near(x, y)
    return true
end

-- Something went wrong: keep a picture of the screen, stop the game, end with exit code 1.
local function give_up(why)
    log("giving up: " .. why)
    log("screenshot: " .. screenshot(os.date("daily-failed-%Y%m%d-%H%M%S.png")))
    stop_app(PACKAGE)
    os.exit(1)
end

-- Closes pop-ups until none is left (at most 6, in case one keeps coming back).
local function close_popups()
    for _ = 1, 6 do
        if not wait(IMG.close, 3) then
            return
        end
        tap(IMG.close)
        sleep(800)
    end
end

-- 1. A fresh start ---------------------------------------------------------------------------

log(("daily routine for %s: %d battles on a %dx%d screen"):format(PACKAGE, BATTLES, W, H))
-- Restart the game for a clean start. stop_app() raises for a package that is not installed, so
-- stop it only when it is actually running; a wrong package name then falls through to launch(),
-- which reports "cannot start <pkg> (is it installed?)".
if current_app() == PACKAGE then
    stop_app(PACKAGE)
    sleep(1000)
end
launch(PACKAGE)

-- 2. Title screen and pop-ups -----------------------------------------------------------------

if not wait(IMG.title, 120) then
    give_up("the title screen did not appear within 2 minutes")
end
sleep(1500)                                         -- let the title animation settle
tap_near(W // 2, H * 3 // 4)                        -- "tap anywhere to start"

if not wait(IMG.home, 90) then
    close_popups()                                  -- pop-ups may cover the home screen
    if not wait(IMG.home, 30) then
        give_up("the home screen did not appear")
    end
end
close_popups()
log("on the home screen")

-- 3. Battles ----------------------------------------------------------------------------------

local won = 0
for i = 1, BATTLES do
    if not tap_when_visible(IMG.battle, 20) then
        give_up("no battle button on the home screen")
    end
    sleep(2000)

    -- switch auto battle on when it is off (the switch stays on for the following battles)
    tap(IMG.auto, { threshold = 0.92 })

    -- a battle takes at most 5 minutes; log a line every minute so you can see it is alive
    local seen = false
    for minute = 1, 5 do
        if wait(IMG.victory, 60) then
            seen = true
            break
        end
        log(("battle %d: still fighting after %d min"):format(i, minute))
    end
    if not seen then
        give_up(("battle %d did not end within 5 minutes"):format(i))
    end
    sleep(1000)

    if not tap_when_visible(IMG.collect, 20) then
        give_up("no collect button after the battle")
    end
    won = won + 1
    log(("battle %d of %d won"):format(i, BATTLES))

    close_popups()                                  -- level-up and reward pop-ups
    if not wait(IMG.home, 30) then
        give_up("did not get back to the home screen")
    end
    sleep(1000 + math.random(2000))                 -- a short, irregular break
end

-- 4. Leave cleanly ----------------------------------------------------------------------------

log(("%d battles won in %.0f s"):format(won, millis() / 1000))
key("HOME")
sleep(1000)
stop_app(PACKAGE)
