-- Checks the player's bindings: labels and rebind flags from input.json, input.set_bindings / reset_bindings /
-- default_bindings, that a rebound action follows its new key and not the old one, slot-wise overrides on a
-- vector, and the errors for bad bindings. (The Game's Settings > Controls menu uses the same overrides.)
-- Run unattended: MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/input_rebind_test.lua
-- Prints "input_rebind_test: PASS ..." / "FAIL ..." lines and a summary.

local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("input_rebind_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail and ("  (" .. detail .. ")") or ""))
end
local function has(list, v) for _, x in ipairs(list) do if x == v then return true end end return false end
local function same(list, want)
    if #list ~= #want then return false end
    for i = 1, #want do if list[i] ~= want[i] then return false end end
    return true
end

-- Each step runs on its own frame; what a step simulates is seen by the next one
local steps = {
    function()
        check("label from the name", input.label("jump") == "Jump", input.label("jump"))
        check("label from input.json", input.label("interact") == "Talk / use", input.label("interact"))
        check("ui actions not rebindable", not input.rebindable("ui_up") and input.rebindable("jump"))
        input.set_bindings("jump", { "key:F", "pad:Y" })
        check("set_bindings replaces", same(input.bindings("jump"), { "key:F", "pad:Y" }), table.concat(input.bindings("jump"), ","))
        check("defaults unchanged", same(input.default_bindings("jump"), { "key:Space", "pad:A" }),
            table.concat(input.default_bindings("jump"), ","))
        input.simulate_key(Key.Space, true)
    end,
    function()
        check("old key no longer feeds it", not input.action("jump"))
        input.simulate_key(Key.Space, false)
        input.simulate_key(Key.F, true)
    end,
    function()
        check("new key presses it", input.pressed("jump"))
        input.simulate_key(Key.F, false)
        input.set_bindings("move", { up = { "key:I", "pad:LeftUp" } })
        local m = input.bindings("move")
        check("vector: only the given slot", same(m.up, { "key:I", "pad:LeftUp" }) and has(m.down, "key:S") and has(m.down, "key:Down"),
            table.concat(m.up, ",") .. " / " .. table.concat(m.down, ","))
        input.simulate_key(Key.I, true)
    end,
    function()
        local x, y = input.vector("move")
        check("rebound vector slot works", y == 1 and x == 0, string.format("%.2f, %.2f", x, y))
        input.simulate_key(Key.I, false)
        input.simulate_key(Key.W, true)
    end,
    function()
        local _, y = input.vector("move")
        check("old vector key gone", y == 0, tostring(y))
        input.simulate_key(Key.W, false)
        local ok, err = pcall(input.set_bindings, "jump", { "key:Nope" })
        check("bad binding is an error", not ok and tostring(err):find("not a binding") ~= nil, tostring(err))
        ok, err = pcall(input.set_bindings, "move", { sideways = { "key:K" } })
        check("bad slot is an error", not ok and tostring(err):find("no slot 'sideways'") ~= nil, tostring(err))
        ok = pcall(input.set_bindings, "jmup", { "key:K" })
        check("unknown action is an error", not ok)
        check("errors changed nothing", same(input.bindings("jump"), { "key:F", "pad:Y" }))
        input.set_bindings("turn", { positive = { "key:L" } })
        input.reset_bindings("jump")
        check("reset one action", same(input.bindings("jump"), { "key:Space", "pad:A" })
            and same(input.bindings("turn").positive, { "key:L" }))
        input.reset_bindings()
        check("reset all", has(input.bindings("move").up, "key:W") and same(input.bindings("turn").positive, { "key:E", "pad:RB" }))
        input.set_bindings("sprint", {})
        check("empty list unbinds", #input.bindings("sprint") == 0)
        input.simulate_key(Key.LeftShift, true)
    end,
    function()
        check("unbound action stays up", not input.action("sprint"))
        input.simulate_key(Key.LeftShift, false)
        input.reset_bindings()
        log(string.format("input_rebind_test: %d passed, %d failed", passed, failed))
    end,
}

local frame = 0
function on_update(self, dt)
    frame = frame + 1
    local step = steps[frame - 1]      -- start on frame 2, after every on_create
    if step then step() end
end
