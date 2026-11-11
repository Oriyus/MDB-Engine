-- Checks ui.show: layouts made in the UI Editor (Assets/UI/*.json), here UI/test_layout.json. Text expressions,
-- repeats (count and list), visible conditions, vars over the script's globals, a broken expression that must not
-- stop the rest, and a click on the middle of three repeated buttons (its name and index, on_click, its event).
-- Run unattended: MDB_SCENE=tactics_test.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/ui_layout_test.lua
-- Prints "ui_layout_test: PASS ..." / "FAIL ..." lines and a summary.

local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1; log("ui_layout_test: PASS " .. name .. (detail and ("  (" .. detail .. ")") or ""))
    else failed = failed + 1; log("ui_layout_test: FAIL " .. name .. (detail and ("  (" .. detail .. ")") or "")) end
end

local PATH = "UI/test_layout.json"
local frame = 0
local done = false

-- what the layout's expressions call (globals of this script: that is where they look)
ticks, notes, boomed, seen_count, var_seen, afters, items, bars = 0, {}, 0, 0, nil, 0, {}, 0
myvar, show_me, hide_me, list, clicks, picked = 1, true, false, { "a", "b" }, 0, 0
function tick() ticks = ticks + 1; return ticks end
function note(i) table.insert(notes, i); return i end
function boom() boomed = boomed + 1; return "boom" end
function seen() seen_count = seen_count + 1; return "seen" end
function check_var(v) var_seen = v; return v end
function after() afters = afters + 1; return "after" end
function note_item(it, i) table.insert(items, tostring(i) .. "=" .. tostring(it)); return it end
function bar_value() bars = bars + 1; return 0.5 end

local results = {}        -- frame -> clicked, index
local event_data

function on_event(self, name, data)
    if name == "picked_event" then event_data = data end
end

function on_update(self, dt)
    if done then return end
    frame = frame + 1
    -- the pointer rests on the centre (the middle button), then presses and lets go
    ui.simulate_pointer(0.5, 0.5, frame == 4)   -- (the engine reads it next frame: pressed in 5, clicked in 6)
    notes, items = {}, {}
    local clicked, index = ui.show(PATH, { vars = { myvar = 7 } })
    results[frame] = { clicked = clicked, index = index }

    if frame == 2 then
        check("nothing clicked yet", clicked == nil, tostring(clicked))
        check("text expressions run", ticks == 2, tostring(ticks))
        check("a counted repeat", #notes == 3 and notes[1] == 1 and notes[3] == 3, table.concat(notes, ","))
        check("a list repeat gives item and index", #items == 2 and items[1] == "1=a" and items[2] == "2=b", table.concat(items, " "))
        check("false, and nil (an unset name), hide", boomed == 0, tostring(boomed))
        check("a visible condition", seen_count == 2, tostring(seen_count))
        check("vars come before the script's globals", var_seen == 7, tostring(var_seen))
        check("a broken expression doesn't stop the rest", afters == 2, tostring(afters))
        check("a bar's value expression", bars == 2, tostring(bars))
        check("index / item don't leak into the script", rawget(_ENV, "index") == nil and rawget(_ENV, "item") == nil)
        check("a missing layout gives nil", ui.show("UI/no_such_layout.json") == nil)
        local ok, err = pcall(ui.show, "UI/tactics_hud.json", { vars = { combat = true, points = 2, move_points = 6, diagonal = 1.5, place = "test" } })
        check("the tactics HUD layout draws", ok, tostring(err))
    elseif frame == 8 then
        local hit
        for f, r in pairs(results) do if r.clicked then hit = { f = f, clicked = r.clicked, index = r.index } end end
        check("a click on the middle button", hit and hit.clicked == "pick" and hit.index == 2,
              hit and (tostring(hit.clicked) .. " #" .. tostring(hit.index) .. " in frame " .. hit.f) or "no click")
        check("on_click ran in the script", picked == 2 and clicks == 1, "picked " .. tostring(picked) .. ", clicks " .. tostring(clicks))
        check("its event", event_data ~= nil and event_data.widget == "pick" and event_data.index == 2 and event_data.ui == PATH,
              event_data and (tostring(event_data.widget) .. " " .. tostring(event_data.index)) or "none")
        ui.simulate_pointer()
        done = true
        log(string.format("ui_layout_test: %d passed, %d failed", passed, failed))
    end
end
