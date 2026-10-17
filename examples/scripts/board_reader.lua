-- name: 通用網格讀取器
-- board_reader.lua — a GENERIC tile-grid reader. Nothing in it is specific to any game.
--
-- It samples an R×C grid of cells anywhere on screen and prints the dominant colour of each cell. That is all a
-- match-3 (Candy Crush…), a chess/go/sudoku board, minesweeper, a card hand, or any other grid needs for
-- its "read the board" step. You aim it at the grid with the live 腳本參數 panel — the engine never learns the game.
--
-- Use it:
--   1. Run from the SoulX window (Scripts ▸ 執行腳本). A 腳本參數 panel appears.
--   2. Put the game's board on screen. Adjust X0/Y0 (centre of the top-left cell), DX/DY (spacing between cells),
--      and Cols/Rows until the printed grid matches what you see. (Inspect, ⇧⌘I, shows exact x/y/#colour under the
--      pointer — handy for the first X0/Y0.)
--   3. Turn "Auto" on to watch it live, or press "Read once".
--   4. "Show hex" also prints each cell's raw 0xRRGGBB — copy those to build a precise palette for a real solver.
--
-- Then a game-specific script is tiny: call read_board(x0, y0, dx, dy, cols, rows, PALETTE) with the numbers you
-- found here and the colours you want named. Engine generic; only the numbers + palette are per-game.

-- A generic "name any colour" table — basic colours, NOT a game's colours. Short codes keep the grid readable.
local NAMED = {
  { "R", 0xE0352B }, { "O", 0xE07C20 }, { "Y", 0xE8C84A }, { "G", 0x4EB24A },
  { "C", 0x3AC6C6 }, { "B", 0x3A6EE0 }, { "U", 0x9A4FD0 }, { "P", 0xE86AA6 },
  { "W", 0xF0F0F0 }, { ".", 0x808080 }, { "K", 0x202020 },
}
local function name_of(rgb)
  local best, bd = "?", nil
  for _, e in ipairs(NAMED) do
    local d = color_dist(rgb, e[2])          -- helper-library colour distance
    if not bd or d < bd then best, bd = e[1], d end
  end
  return best
end

local w, h = screen_size()

-- Grid geometry and options, all live-adjustable. Defaults are a rough guess you then calibrate.
local X0   = param_slider("X0 (first cell x)", 0, w, math.floor(w * 0.10))
local Y0   = param_slider("Y0 (first cell y)", 0, h, math.floor(h * 0.55))
local DX   = param_slider("DX (col spacing)", 8, math.floor(w / 2), math.floor(w / 6))
local DY   = param_slider("DY (row spacing)", 8, math.floor(h / 2), math.floor(w / 6))
local COLS = param_number("Cols", 1, 16, 6)
local ROWS = param_number("Rows", 1, 16, 5)
local INT  = param_slider("Interval ms", 100, 2000, 500)
local AUTO = param_toggle("Auto", true)
local HEX  = param_toggle("Show hex", false)
local ONCE = param_button("Read once")

log("board_reader (generic). Codes: R/O/Y/G/C/B/U/P = red/orange/yellow/green/cyan/blue/purple/pink; W white, . grey, K black.")

-- Reads a grid from one capture, clamping every sample inside the screen so dragging a slider off the edge can
-- never raise an error (a calibration tool must never crash while you are aiming it).
local function read_grid_clamped(x0, y0, dx, dy, cols, rows)
  local img = capture()
  local iw, ih = img:size()
  local b = {}
  for r = 1, rows do
    b[r] = {}
    for c = 1, cols do
      local x = math.min(iw - 1, math.max(0, math.floor(x0 + (c - 1) * dx + 0.5)))
      local y = math.min(ih - 1, math.max(0, math.floor(y0 + (r - 1) * dy + 0.5)))
      b[r][c] = img:color(x, y)
    end
  end
  return b
end

while true do
  if ONCE:pressed() or AUTO:on() then
    local cols, rows = math.floor(COLS:value()), math.floor(ROWS:value())
    local board = read_grid_clamped(X0:value(), Y0:value(), DX:value(), DY:value(), cols, rows)
    for r = 1, rows do
      local names = {}
      for c = 1, cols do names[c] = name_of(board[r][c]) end
      log(table.concat(names, " "))
    end
    if HEX:on() then
      for r = 1, rows do
        local hexes = {}
        for c = 1, cols do hexes[c] = string.format("%06X", board[r][c]) end
        log("  " .. table.concat(hexes, " "))
      end
    end
    log("—")
  end
  sleep(math.max(50, math.floor(INT:value())))
end
