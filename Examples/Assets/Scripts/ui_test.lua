-- Checks the laid-out UI (ui.begin_panel, ui.label, ui.slot ...) by driving it with a simulated pointer.
-- Run unattended: MDB_SCENE=tactics_test.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/ui_test.lua
-- Prints "ui_test: PASS ..." / "FAIL ..." lines and "ui_test: done". A simulated pointer set in one frame
-- is seen by the next one, so every step below acts one frame after the step that moved the pointer.

local LONG = "This sentence is long enough that a 392 unit wide label has to wrap it over more than one line, "
    .. "and {#ffcc66}some of it{/} is {b}styled{/b}."

local frame, step = 0, 0
local clicks, tab, dropped, dragging_seen = 0, 1, nil, false
local closed = false
local r = {}                        -- rects from the last draw: name -> { x, y, w, h } (0..1 of the view)
local vw                            -- the view width in units (the height is 720)

local function rect(name) r[name] = { ui.last_rect() } end
local function center(name) return r[name][1] + r[name][3] / 2, r[name][2] + r[name][4] / 2 end
local function units_h(name) return r[name][4] * 720 end
local function check(name, ok, detail)
    log("ui_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end

local function draw()
    if not ui.begin_panel("ui_test", { x = 0.5, y = 0.45, w = 420, title = "UI test", close = true }) then closed = true end
    ui.label(LONG); rect("long")
    ui.label("Short"); rect("short")
    ui.begin_row()
    if ui.button("Press me") then clicks = clicks + 1 end
    rect("button")
    ui.button("Other", { tooltip = "Does nothing" })
    ui.end_row(); rect("row")
    tab = ui.tabs("tabs", { "One", "Two" }, tab); rect("tabs")
    ui.begin_row()
    ui.slot("a", { icon = "Textures/Icons/sword.png", drag = { from = "a" }, tooltip = "A sword" }); rect("slot_a")
    local _, got = ui.slot("b", {}); rect("slot_b")
    if got then dropped = got end
    ui.end_row()
    if ui.dragging() then dragging_seen = true end
    ui.begin_scroll("list", 100)
    for i = 1, 20 do
        ui.label("Line " .. i)
        if i == 1 then rect("line1") end
    end
    ui.end_scroll(); rect("scroll")
    ui.end_panel(); rect("panel")
    vw = 420 / r.panel[3]
end

local steps = {
    -- layout
    function()
        local lineH = 18 * 1.25
        check("long label wraps (more than one line)", units_h("long") > lineH * 1.5, units_h("long"))
        check("short label is one line", math.abs(units_h("short") - lineH) < 0.5, units_h("short"))
        check("label as wide as the panel's content", math.abs(r.long[3] * vw - 392) < 1, r.long[3] * vw)
        check("row below the labels", r.row[2] > r.short[2] + r.short[4] - 1e-4)
        check("auto height panel fits its content", r.panel[2] + r.panel[4] >= r.scroll[2] + r.scroll[4] - 1e-4)
        check("markup has no width of its own", math.abs(ui.text_width("{#ff0000}ab{/}") - ui.text_width("ab")) < 1e-3)
        check("pointer outside: wants_mouse false", not ui.wants_mouse())
        ui.simulate_pointer(center("button"))
    end,
    function()
        check("over a panel: wants_mouse", ui.wants_mouse())
        ui.simulate_pointer(r.button[1] + r.button[3] / 2, r.button[2] + r.button[4] / 2, true)
    end,
    function()
        check("no click while held", clicks == 0, clicks)
        ui.simulate_pointer(center("button"))           -- release
    end,
    function()
        check("button clicks on release", clicks == 1, clicks)
        -- the second tab: the right part of the tabs row
        ui.simulate_pointer(r.tabs[1] + r.tabs[3] * 0.8, r.tabs[2] + r.tabs[4] / 2, true)
    end,
    function() ui.simulate_pointer(r.tabs[1] + r.tabs[3] * 0.8, r.tabs[2] + r.tabs[4] / 2) end,
    function()
        check("tabs switch", tab == 2, tab)
        -- drag the sword from slot a to slot b
        ui.simulate_pointer(center("slot_a"))
    end,
    function() local x, y = center("slot_a"); ui.simulate_pointer(x, y, true) end,
    function() local x, y = center("slot_b"); ui.simulate_pointer(x, y, true) end,      -- moved while held: a drag
    function()
        check("dragging reported", dragging_seen)
        check("over a panel while dragging: wants_mouse", ui.wants_mouse())
        ui.simulate_pointer(center("slot_b"))           -- drop
    end,
    function()
        check("dropped value arrives in slot b", type(dropped) == "table" and dropped.from == "a")
        r.line1_before = r.line1
        ui.simulate_pointer(center("scroll"))
    end,
    function()
        check("drag over after the drop", ui.dragging() == nil)
        local x, y = center("scroll"); ui.simulate_pointer(x, y, false, -1)   -- one notch down
    end,
    function()
        local moved = (r.line1_before[2] - r.line1[2]) * 720
        check("wheel scrolls the list by 48 units", math.abs(moved - 48) < 0.5, moved)
        local x, y = center("scroll"); ui.simulate_pointer(x, y, false, -100)
    end,
    function()
        local moved = (r.line1_before[2] - r.line1[2]) * 720
        local content = 20 * 18 * 1.25 + 19 * 8
        check("scrolling stops at the end", math.abs(moved - (content - 100)) < 0.5, moved)
        -- the close button: 23 units in from the top right corner
        local x = r.panel[1] + r.panel[3] - 23 / vw
        local y = r.panel[2] + 23 / 720
        ui.simulate_pointer(x, y, true)
    end,
    function()
        local x = r.panel[1] + r.panel[3] - 23 / vw
        local y = r.panel[2] + 23 / 720
        ui.simulate_pointer(x, y)
    end,
    function()
        check("close button", closed)
        ui.simulate_pointer(0.02, 0.6)
    end,
    function()
        check("off the UI: wants_mouse false again", not ui.wants_mouse())
        ui.simulate_pointer()
        log("ui_test: done")
    end,
}

function on_update(self, dt)
    frame = frame + 1
    if closed then
        step = step + 1
        if steps[step] then steps[step]() end
        return
    end
    draw()
    if frame < 4 then return end                        -- panel sizes settle (auto height uses last frame's)
    step = step + 1
    if steps[step] then steps[step]() end
end
