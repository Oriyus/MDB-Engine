-- Checks the grass (FoliageComponent) on the demo village and walks a push through it.
-- Run unattended: MDB_SCENE=village.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/foliage_test.lua
-- Prints "foliage_test: PASS ..." / "FAIL ..." lines and a summary; a ball rolls and a push walks a circle
-- in front of the camera (see the trails in a screenshot).

local frame, t = 0, 0
local passed, failed = 0, 0
local ball

local function check(name, ok, detail)
    if ok then passed = passed + 1; log("foliage_test: PASS " .. name)
    else failed = failed + 1; log("foliage_test: FAIL " .. name .. (detail and ("  (" .. detail .. ")") or "")) end
end

function on_update(self, dt)
    frame = frame + 1
    t = t + dt
    -- something without a body walking a circle (a script character)
    local p = vec3(6 + math.cos(t * 0.9) * 3, 0, 10.5 + math.sin(t * 0.9) * 2.5)
    foliage.push(p, 0.45)
    if frame == 5 then
        local s = scene.render_stats()
        check("grass drawn", s.foliage > 1000, tostring(s.foliage) .. " clumps")
        check("drawn instanced", s.draw_calls < 400, tostring(s.draw_calls) .. " draw calls")
        -- a body rolling through the grass presses it by itself
        ball = scene.spawn("Rolling ball", vec3(-2, 0.3, 13))
        ball:add_mesh{ geometry = "sphere", color = vec3(0.8, 0.3, 0.2) }
        ball.scale = vec3(0.6, 0.6, 0.6)
        ball:add_body{ type = "dynamic", shape = "sphere", size = vec3(1, 1, 1), mass = 2 }
    end
    if frame == 8 and ball and ball.body then ball.body.velocity = vec3(3.5, 0, -0.6) end
    if frame == 200 then
        local s = scene.render_stats()
        check("still drawn after pushes", s.foliage > 1000, tostring(s.foliage))
        log(string.format("foliage_test: %d passed, %d failed", passed, failed))
    end
end
