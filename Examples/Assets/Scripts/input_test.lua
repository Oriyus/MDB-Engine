-- Checks the input actions of Assets/Data/input.json and the gamepad API with simulated keys and a simulated
-- gamepad: presses and releases, vectors from keys and sticks, dead zones, trigger hysteresis, axes, the last
-- device, and the errors for unknown names.
-- Run unattended: MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/input_test.lua
-- Prints "input_test: PASS ..." / "FAIL ..." lines and a summary.

local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("input_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail and ("  (" .. detail .. ")") or ""))
end
local function near(a, b, eps) return math.abs(a - b) < (eps or 1e-3) end
local function has(list, v) for _, x in ipairs(list) do if x == v then return true end end return false end

-- Each step runs on its own frame. What a step simulates is seen by the next step (input is sampled at the
-- start of the frame).
local steps = {
    function()
        local names = input.actions()
        check("actions listed", has(names, "move") and has(names, "jump") and has(names, "turn"), table.concat(names, ","))
        check("action types", input.action_type("move") == "vector" and input.action_type("jump") == "button"
            and input.action_type("turn") == "axis")
        local b = input.bindings("jump")
        check("button bindings", has(b, "key:Space") and has(b, "pad:A"), table.concat(b, ","))
        local m = input.bindings("move")
        check("vector groups expand", has(m.up, "key:W") and has(m.up, "key:Up") and has(m.up, "pad:LeftUp")
            and has(m.right, "pad:DPadRight"), table.concat(m.up, ","))
        local ok, err = pcall(input.action, "jmup")
        check("unknown action is an error", not ok and tostring(err):find("unknown input action 'jmup'") ~= nil, tostring(err))
        ok = pcall(input.pad_down, "Triangle")
        check("unknown pad button is an error", not ok)
        check("idle", not input.action("jump") and input.value("move") == 0)
        input.simulate_key(Key.Space, true)
    end,
    function()
        check("key press", input.pressed("jump") and input.action("jump") and input.value("jump") == 1)
        check("device keyboard", input.device() == "keyboard", input.device())
    end,
    function()
        check("held is not pressed again", input.action("jump") and not input.pressed("jump"))
        input.simulate_key(Key.Space, false)
    end,
    function()
        check("release", input.released("jump") and not input.action("jump"))
        input.simulate_key(Key.W, true)
        input.simulate_key(Key.D, true)
    end,
    function()
        local x, y = input.vector("move")
        check("keys diagonal is length 1", near(x, 0.7071) and near(y, 0.7071) and near(input.value("move"), 1),
            string.format("%.3f, %.3f", x, y))
        input.simulate_key(Key.W, false)
        input.simulate_key(Key.D, false)
        input.simulate_key(Key.Q, true)
    end,
    function()
        check("axis negative", near(input.value("turn"), -1), tostring(input.value("turn")))
        input.simulate_key(Key.E, true)
    end,
    function()
        check("axis both cancel", near(input.value("turn"), 0) and not input.action("turn"))
        input.simulate_key(Key.Q, false)
        input.simulate_key(Key.E, false)
        input.simulate_gamepad({ LeftX = 0.15, LeftY = 0.1 })
    end,
    function()
        local connected, name = input.gamepad()
        check("simulated pad connected", connected and name == "Simulated gamepad", tostring(name))
        local x, y = input.vector("move")
        check("stick inside the dead zone", x == 0 and y == 0 and input.pad_axis("LeftX") == 0, string.format("%.3f, %.3f", x, y))
        input.simulate_gamepad({ LeftX = 0.6 })
    end,
    function()
        local x, y = input.vector("move")
        check("stick rescaled past the dead zone", near(x, 0.5) and near(y, 0), string.format("%.3f, %.3f", x, y))
        check("half tilt is down", input.action("move"))
        input.simulate_gamepad({ LeftY = 1.0 })
    end,
    function()
        local x, y = input.vector("move")
        check("stick up is +y", near(x, 0) and near(y, 1), string.format("%.3f, %.3f", x, y))
        input.simulate_gamepad({ A = true })
    end,
    function()
        check("pad button feeds the action", input.pressed("jump") and input.pad_pressed("A") and input.pad_down("A"))
        check("device gamepad", input.device() == "gamepad", input.device())
        input.simulate_gamepad({ LT = 0.3 })
    end,
    function()
        check("pad release", input.pad_released("A") and input.released("jump"))
        check("trigger below half", near(input.value("sprint"), 0.2 / 0.9) and not input.action("sprint"),
            tostring(input.value("sprint")))
        input.simulate_gamepad({ LT = 0.9 })
    end,
    function()
        check("trigger pressed", input.pressed("sprint"))
        input.simulate_gamepad({ LT = 0.45 })
    end,
    function()
        check("trigger hysteresis keeps it down", input.action("sprint") and not input.released("sprint"),
            tostring(input.value("sprint")))
        input.simulate_gamepad({ LT = 0.3 })
    end,
    function()
        check("trigger released below the lower mark", input.released("sprint"), tostring(input.value("sprint")))
        input.simulate_gamepad({ connected = false, A = true })
    end,
    function()
        check("unplugged pad gives nothing", not input.gamepad() and not input.action("jump"))
        input.simulate_gamepad(nil)
        input.simulate_key(Key.Enter, true)
    end,
    function()
        check("back to the keyboard", input.device() == "keyboard" and input.pressed("confirm"))
        input.simulate_key(Key.Enter, false)
        log(string.format("input_test: %d passed, %d failed", passed, failed))
    end,
}

local frame = 0
function on_update(self, dt)
    frame = frame + 1
    local step = steps[frame - 1]      -- start on frame 2, after every on_create
    if step then step() end
end
