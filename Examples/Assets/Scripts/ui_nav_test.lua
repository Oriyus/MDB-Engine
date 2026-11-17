-- Checks gamepad / keyboard navigation of laid-out UI (Runtime/UILayout UIContext, the ui_* input actions): the
-- first press only shows the focus (on the default widget), arrows move it to the nearest widget that way,
-- sliders take Left / Right, Enter presses buttons and starts typing in a text field (the pad's B cancels it),
-- a draggable slot is carried to another, the pad's X right-clicks, a disabled button can be focused but not
-- pressed, a scroll region follows the focus, Tab / Shift+Tab go in draw order, a panel that appears takes the
-- focus, the focus falls back when its widget goes, the mouse hides the ring, Back closes a closable panel and
-- otherwise reaches ui.back(), and a layout file's slider (UI/options.json) follows its bind.
-- Run unattended: MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/ui_nav_test.lua
-- Prints "ui_nav_test: PASS ..." / "FAIL ..." lines and a summary.

local passed, failed = 0, 0
local focus = {}                 -- widget -> had the navigation focus this frame
local clicks, results = {}, {}   -- this frame
local vol, name = 0.5, ""
local menu_open, popup = true, false
local back_seen = false
local scroll_rect, item_rects = nil, {}
local step, wait = 1, 0

local function check(label, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("ui_nav_test: " .. (ok and "PASS " or "FAIL ") .. label .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end
local function focused()
    for k, v in pairs(focus) do if v then return k end end
    return nil
end
local function mark(id) focus[id] = ui.nav_focused() end
local function button(id, label, opts)
    local c = ui.button(label or id, opts)
    mark(id)
    if c then clicks[id] = true end
end

local function draw()
    focus, clicks, results = {}, {}, {}
    if menu_open then
        if not ui.begin_panel("menu", { x = 0.5, y = 0.5, w = 420, title = "Menu", close = true }) then menu_open = false end
        ui.begin_row()
        button("A"); button("B", nil, { default_focus = true }); button("C")
        ui.end_row()
        button("Wide", nil, { fill = true })
        local changed
        vol, changed = ui.slider("vol", vol, { step = 0.1, text = string.format("%d%%", math.floor(vol * 100 + 0.5)) })
        mark("vol")
        local submitted, f
        name, submitted, f = ui.input("name", name, { hint = "Name" })
        mark("name")
        results.submitted = submitted
        ui.begin_row()
        local c1, d1, r1 = ui.slot("s1", { icon = "Textures/coin_icon.png", drag = "sword", size = 180 })
        mark("s1")
        local c2, d2, r2 = ui.slot("s2", { size = 180 })
        mark("s2")
        ui.end_row()
        results.dropped, results.right = d2, r2
        results.dragging = ui.dragging()
        button("Off", nil, { disabled = true, fill = true })
        ui.begin_scroll("list", 80)
        for i = 1, 6 do
            button("item" .. i, "Item " .. i, { fill = true })
            item_rects[i] = { ui.last_rect() }
        end
        ui.end_scroll()
        scroll_rect = { ui.last_rect() }
        ui.end_panel()
    end
    if popup then
        ui.begin_panel("popup", { x = 0.5, y = 0.2, w = 300, title = "Sure?" })
        ui.begin_row()
        button("Yes"); button("No", nil, { default_focus = true })
        ui.end_row()
        ui.end_panel()
        if clicks.No or clicks.Yes then popup = false end
    end
    if show_options then
        local c = ui.show("UI/options.json")
        if c then options_clicked = c end
        if not options_open then show_options = false end
    end
end

-- A key or pad button held over one frame: down in this step, up in the next (the press is seen in between)
local held = nil
local function tap(k) input.simulate_key(k, true); held = k end
local function pad(buttons) input.simulate_gamepad(buttons) end
local function inside(r, outer)
    return r[2] >= outer[2] - 0.001 and r[2] + r[4] <= outer[2] + outer[4] + 0.001
end

-- Each step runs after this frame's draw: it checks what the previous step's input did, then gives more
local steps = {
    function()
        local names = table.concat(input.actions(), ",")
        check("ui_* actions exist by default", names:find("ui_confirm") ~= nil and names:find("ui_back") ~= nil, names)
        check("no ring before any navigation input", not ui.navigating() and focused() == nil)
        tap(Key.Down)
    end,
    function() check("the first press shows the focus on the default", ui.navigating() and focused() == "B", focused()) end,
    function() tap(Key.Right) end,
    function() check("right", focused() == "C", focused()) end,
    function() tap(Key.Right) end,
    function() check("nothing further right: stays", focused() == "C", focused()) end,
    function() tap(Key.Left) end,
    function() end,
    function() tap(Key.Left) end,
    function() check("left twice", focused() == "A", focused()) end,
    function() tap(Key.Down) end,
    function() check("down to the wide button", focused() == "Wide", focused()) end,
    function() tap(Key.Enter) end,
    function() check("enter presses it", clicks.Wide == true) end,
    function() check("pressed once", clicks.Wide == nil) end,
    function() tap(Key.Down) end,
    function() check("down to the slider", focused() == "vol", focused()) end,
    function() tap(Key.Right) end,
    function() check("right on a slider raises it a step", math.abs(vol - 0.6) < 1e-4 and focused() == "vol", vol) end,
    function() tap(Key.Left) end,
    function() end,
    function() tap(Key.Left) end,
    function() check("left lowers it", math.abs(vol - 0.4) < 1e-4, vol) end,
    function() tap(Key.Down) end,
    function() check("down to the text field", focused() == "name" and not ui.typing(), focused()) end,
    function() tap(Key.Enter) end,
    function()
        check("enter starts typing (and isn't a submit)", ui.typing() and not results.submitted)
        ui.simulate_type("Zed")
    end,
    function() ui.simulate_key("enter") end,
    function() check("typed and submitted", name == "Zed" and results.submitted and not ui.typing(), name) end,
    function() check("the field keeps the focus after typing", focused() == "name", focused()); pad({ A = true }) end,
    function() pad({}) end,
    function() check("the pad's A starts typing too", ui.typing()); ui.simulate_type("Q") end,
    function() check("typed (focus by navigation selects the text)", name == "Q", name); pad({ B = true }) end,
    function() pad({}) end,
    function() check("the pad's B cancels the typing", name == "Zed" and not ui.typing(), name); tap(Key.Down) end,
    function() check("down to the nearer slot below", focused() == "s2", focused()) end,
    function() tap(Key.Left) end,
    function() check("left to the other slot", focused() == "s1", focused()) end,
    function() tap(Key.Enter) end,
    function() check("enter picks a draggable slot's item up", results.dragging == "sword", tostring(results.dragging)) end,
    function() tap(Key.Right) end,
    function() check("carried along", focused() == "s2" and results.dragging == "sword") end,
    function() tap(Key.Enter) end,
    function() check("enter puts it down: dropped", results.dropped == "sword", tostring(results.dropped)) end,
    function() pad({ X = true }) end,
    function() check("the pad's X right-clicks a slot", results.right == true); pad({}) end,
    function() check("right-clicked once", results.right ~= true); tap(Key.Down) end,
    function() check("a disabled button can take the focus", focused() == "Off", focused()) end,
    function() tap(Key.Enter) end,
    function() check("but isn't pressed", clicks.Off == nil); tap(Key.Down) end,
    function() check("down into the scroll region", focused() == "item1", focused()) end,
}
for i = 2, 6 do
    steps[#steps + 1] = function() tap(Key.Down) end
    steps[#steps + 1] = function() end
end
local rest = {
    function()
        check("down through the list", focused() == "item6", focused())
        check("the scroll region followed the focus", scroll_rect and item_rects[6] and inside(item_rects[6], scroll_rect)
            and not inside(item_rects[1], scroll_rect))
        ui.simulate_key("tab")
    end,
    function() check("tab wraps to the first widget", focused() == "A", focused()); ui.simulate_key("shift+tab") end,
    function() check("shift+tab goes back", focused() == "item6", focused()); popup = true end,
    function() end,     -- (the panel appears; the next frame gives it the focus)
    function() check("a panel that appears takes the focus (its default)", focused() == "No", focused()); tap(Key.Enter) end,
    function() check("pressed there", not popup) end,
    function() end,
    function() check("its widget gone, the focus goes back to a default", focused() == "B", focused()) end,
    -- the mouse takes over
    function() ui.simulate_pointer(0.03, 0.97, false) end,
    function() check("moving the pointer hides the ring", not ui.navigating() and focused() == nil) end,
    function() tap(Key.Up) end,
    function() check("a press shows it again where it was", ui.navigating() and focused() == "B", focused()) end,
    -- gamepad
    function() pad({ DPadDown = true }) end,
    function() pad({}) end,
    function() check("the pad's D-pad moves too", focused() == "Wide", focused()) end,
    function() pad({ LeftX = 0.9 }) end,
    function() pad({}) end,
    function() check("so does the stick (nothing to the right of the wide button)", focused() == "Wide", focused()) end,
    function() pad({ LeftY = -0.9 }) end,
    function() pad({}); input.simulate_gamepad() end,
    function() check("stick down", focused() == "vol", focused()) end,
    -- Back
    function() tap(Key.Escape) end,
    function() check("escape closes the closable panel", not menu_open and not back_seen) end,
    function() tap(Key.Escape) end,
    function() check("with none, ui.back() gets it", back_seen); back_seen = false; show_options = true end,
    -- a layout file's slider (bind music_volume, default_focus)
    function() end,
    function() tap(Key.Right) end,
    function() check("layout slider takes the focus and follows its bind", math.abs(music_volume - 0.7) < 1e-4, music_volume) end,
    function() tap(Key.Down) end,
    function() end,
    function() tap(Key.Enter) end,
    function() check("layout button pressed by enter", subtitles == false, tostring(subtitles)) end,
    function() tap(Key.Escape) end,
    function()
        check("escape closes the layout panel (on_close)", not options_open and options_clicked == "options", tostring(options_clicked))
        ui.simulate_pointer()
        log(string.format("ui_nav_test: %d passed, %d failed", passed, failed))
    end,
    function() scene.quit() end,
}
for _, f in ipairs(rest) do steps[#steps + 1] = f end

-- The layout's expressions read and write these (script globals)
music_volume, subtitles, options_open = 0.6, true, true
show_options, options_clicked = false, nil

function on_update(self, dt)
    draw()
    if ui.back() then back_seen = true end
    if held then input.simulate_key(held, false); held = nil end   -- a tap is one frame long
    wait = wait + 1
    if wait == 1 then ui.simulate_pointer(0.03, 0.95, false) end   -- park the pointer (no real mouse moves)
    if wait < 3 then return end                        -- let the panel settle (auto height from last frame)
    if steps[step] then
        local ok, err = pcall(steps[step])
        if not ok then check("step " .. step .. " ran", false, err) end
        step = step + 1
    end
end
