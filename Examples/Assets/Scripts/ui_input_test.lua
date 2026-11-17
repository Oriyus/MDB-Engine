-- Checks text fields (ui.input, Runtime/UILayout UIContext::Input): focus by click / Tab / focus = true, typing,
-- caret keys, selection, Ctrl+A / Ctrl+Backspace / Ctrl+X / Ctrl+V, filters and max length, Enter and Escape,
-- keep_focus with the script clearing the text, losing focus (a click elsewhere, the field not drawn), disabled
-- fields, and game keys being masked while typing (and until released after).
-- Run unattended: MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/ui_input_test.lua
-- (uses the clipboard). Prints "ui_input_test: PASS ..." / "FAIL ..." lines and a summary.

local passed, failed = 0, 0
local name, age, secret, chat, locked = "", "", "", "", "fixed"
local res = {}                     -- per field: { submitted, focused } of this frame
local rect = {}                    -- per field: x, y, w, h (0..1)
local show_form = true
local want_focus = false
local step = 1
local wait = 0

local function check(label, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("ui_input_test: " .. (ok and "PASS " or "FAIL ") .. label .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end

local function field(id, value, opts)
    local text, submitted, focused = ui.input(id, value, opts)
    res[id] = { submitted = submitted, focused = focused }
    rect[id] = { ui.last_rect() }
    return text
end

local function draw()
    if not show_form then return end
    ui.begin_panel("form", { x = 0.5, y = 0.5, w = 420, title = "Form" })
    name = field("name", name, { hint = "Your name", max = 12 })
    age = field("age", age, { filter = "integer", max = 3, hint = "Age" })
    secret = field("secret", secret, { password = true, focus = want_focus })
    chat = field("chat", chat, { keep_focus = true })
    locked = field("locked", locked, { disabled = true })
    ui.end_panel()
end

-- Pointer: press (this frame) / release (the next) at a point inside a field, x from its left edge (0..1 of it)
local function press(id, fx) local r = rect[id]; ui.simulate_pointer(r[1] + r[3] * (fx or 0.5), r[2] + r[4] * 0.5, true) end
local function release() local p = rect.name; ui.simulate_pointer(0.5, 0.95, false) end
local function key(k) ui.simulate_key(k) end

-- Each step runs after this frame's draw: it checks what the previous step queued, then queues more
local steps = {
    function()
        check("nothing focused at first", not ui.typing() and not res.name.focused)
        press("name")
    end,
    function() release(); check("a click focuses the field", res.name.focused and ui.typing()) ; ui.simulate_type("Hello") end,
    function() check("typing", name == "Hello", name); key("backspace"); key("backspace") end,
    function() check("backspace", name == "Hel", name); key("home"); ui.simulate_type("X"); key("end"); ui.simulate_type("!") end,
    function() check("home / end", name == "XHel!", name); key("ctrl+a"); ui.simulate_type("Bob") end,
    function() check("ctrl+a replaces", name == "Bob", name); key("shift+left"); key("shift+left"); key("backspace") end,
    function() check("shift+arrows select", name == "B", name); ui.simulate_type("abcdefghijklmnopqrst") end,
    function() check("max length", name == "Babcdefghijk" and #name == 12, name); key("ctrl+a"); ui.simulate_type("one two") end,
    function() key("ctrl+backspace") end,
    function() check("ctrl+backspace deletes a word", name == "one ", "'" .. name .. "'"); key("escape") end,
    function()
        check("escape restores the text from when it was focused", name == "", "'" .. name .. "'")
        check("escape lets go", not res.name.focused and not ui.typing())
        press("name")
    end,
    function() release(); ui.simulate_type("Ann"); key("enter") end,
    function()
        check("enter submits", res.name.submitted and name == "Ann", name)
        check("enter lets go", not res.name.focused)
        press("name")
    end,
    function() release(); key("tab") end,
    function() end,     -- the Tab target takes the focus when it is drawn
    function()
        check("tab moves to the next field", res.age.focused and not res.name.focused)
        ui.simulate_type("12a3x4")
    end,
    function() check("integer filter and max", age == "123", age); key("shift+tab") end,
    function() end,
    function()
        check("shift+tab goes back (text selected)", res.name.focused)
        ui.simulate_type("Zed")
    end,
    function() check("focus by tab selects all", name == "Zed", name); press("name", 0.0) end,
    function() release(); ui.simulate_type("Q") end,
    function() check("a click places the caret", name == "QZed", name); key("ctrl+a"); key("ctrl+x") end,
    function() check("ctrl+x cuts", name == "", "'" .. name .. "'"); press("chat") end,
    function() release(); key("ctrl+v") end,
    function() check("ctrl+v pastes", chat == "QZed", chat); key("ctrl+a"); ui.simulate_type("hi"); key("enter") end,
    function()
        check("keep_focus: enter submits and keeps typing", res.chat.submitted and res.chat.focused and chat == "hi", chat)
        chat = ""                                      -- the script clears the line after sending it
    end,
    function() ui.simulate_type("yo") end,
    function() check("the script's change is taken over", chat == "yo" and res.chat.focused, chat) end,
    -- Game keys are masked while typing, and stay up until released after
    function()
        input.simulate_key(Key.Space, true)
    end,
    function()
        check("game keys read as up while typing", not input.action("jump"))
        ui.simulate_pointer(0.05, 0.05, true)          -- a press outside the panel
    end,
    function() ui.simulate_pointer(0.05, 0.05, false) end,   -- (the field lets go at the end of the press's frame)
    function()
        check("a click elsewhere lets go", not res.chat.focused and not ui.typing())
        check("a key held across letting go stays up", not input.action("jump"))
        input.simulate_key(Key.Space, false)
    end,
    function() end,
    function() input.simulate_key(Key.Space, true) end,
    function() check("and works again once pressed anew", input.action("jump")); input.simulate_key(Key.Space, false); press("locked") end,
    function() release(); check("a disabled field takes no focus", not res.locked.focused and locked == "fixed") end,
    function() want_focus = true end,
    function()
        check("focus = true takes the focus", res.secret.focused)
        ui.simulate_type("pa55")
    end,
    function()
        check("password field keeps the real text", secret == "pa55", secret)
        key("enter")
    end,
    function()
        check("focus = true held on doesn't take it again", not res.secret.focused, "after enter")
        want_focus = false
        press("name")
    end,
    function() release(); check("focused again by a click", res.name.focused); show_form = false end,   -- not drawn next frame
    function() show_form = true end,
    function()
        check("a field not drawn lets go", not res.name.focused and not ui.typing())
        check("bad filter is an error", not pcall(function() ui.input("x", "", { filter = "hex" }) end))
        check("bad key name is an error", not pcall(function() ui.simulate_key("ctrl+nope") end))
        ui.simulate_pointer()
        show_form = false
        show_prompt = true                             -- UI/name_prompt.json from here on
    end,
    -- A layout file (ui.show): its field binds the script's hero_name, takes the focus by its own expression
    -- ("not named"), filters to a name, and on Enter runs on_submit, sends its event and reports its name
    function()
        check("layout field takes the focus (focus expression)", ui.typing())
        ui.simulate_type("Bob the 3rd!")
    end,
    function() check("bind writes the script's variable (name filter)", hero_name == "Bobthe3rd", hero_name); key("enter") end,
    function()
        check("on_submit ran", named == true)
        check("ui.show reports the submitted field", prompt_clicked == "hero_name", tostring(prompt_clicked))
    end,
    function()
        check("its event was sent", event_seen == "hero_named", tostring(event_seen))
        check("the prompt is gone (visible = not named)", not ui.typing())
        log(string.format("ui_input_test: %d passed, %d failed", passed, failed))
    end,
    function() scene.quit() end,
}

-- The layout's expressions read and write these (script globals, so the bind can assign them)
hero_name, named = "", false
show_prompt, prompt_clicked, event_seen = false, nil, nil

function on_event(self, name, data)
    if name == "hero_named" then event_seen = name end
end

function on_update(self, dt)
    draw()
    if show_prompt then
        local clicked = ui.show("UI/name_prompt.json")
        if clicked then prompt_clicked = clicked end
    end
    wait = wait + 1
    if wait < 3 then return end                        -- let the panel settle (auto height from last frame)
    if steps[step] then
        local ok, err = pcall(steps[step])
        if not ok then check("step " .. step .. " ran", false, err) end
        step = step + 1
    end
end
