-- Checks the shadow atlas: many shadowed lights at once (the 4 shaded directly plus up to Scene Settings > Shadows >
-- Atlas lights, default 24, get squares of the atlas), squares sized by how much of the view a light covers (near
-- ones bigger), the atlas never over-full, lights giving their squares back when they stop casting shadows, and
-- spot lights in the atlas too. Builds its own field: a grid of posts with a shadowed lamp beside each.
-- Run unattended: MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/shadow_atlas_test.lua
-- Prints "shadow_atlas_test: PASS ..." / "FAIL ..." lines and a summary.

local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("shadow_atlas_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail and ("  (" .. detail .. ")") or ""))
end
local function describe(s)
    return string.format("%d shadowed, %d in the atlas, squares %d-%d px, %.0f%% used", s.shadowed_lights, s.atlas_lights,
        s.atlas_smallest, s.atlas_largest, s.atlas_used * 100)
end

local lamps, spots = {}, {}
local base
local wait, step = 0, 1

local steps = {
    function()
        -- Away from the demo's own things: a camera looking down on a 7 x 7 field 300 m off
        local cam = scene.spawn("Atlas test camera", vec3(300, 9, 311))
        cam:look_at(vec3(300, 0, 300))
        cam:add_camera({ primary = true })
        local floor = scene.spawn("Atlas floor", vec3(300, 0, 300))
        floor:add_mesh({ geometry = "plane" })
        floor.scale = vec3(3, 1, 3)
        for x = 0, 6 do
            for z = 0, 6 do
                local p = vec3(291 + x * 3, 0, 291 + z * 3)
                local post = scene.spawn("Post", p + vec3(0, 1, 0))
                post:add_mesh({ geometry = "cube" })
                post.scale = vec3(0.3, 2, 0.3)
                local lamp = scene.spawn("Lamp", p + vec3(0.9, 1.2, 0.9))
                lamps[#lamps + 1] = lamp:add_light({ color = vec3(1, 0.8, 0.6), intensity = 0.3, range = 4, shadows = true })
            end
        end
        return true
    end,
    function()
        local s = scene.render_stats()
        check("4 direct + 24 more, all in the atlas (the default)", s.shadowed_lights == 28 and s.atlas_lights == 28, describe(s))
        check("the atlas is never over-full", s.atlas_used > 0 and s.atlas_used <= 0.9, describe(s))
        check("squares come in sizes", s.atlas_smallest >= 64 and s.atlas_largest >= s.atlas_smallest and s.atlas_largest <= 1024, describe(s))
        base = s
        -- Most lamps stop casting shadows: their squares go back, the rest may grow
        for i = 9, #lamps do lamps[i].shadows = false end
        return true
    end,
    function()
        local s = scene.render_stats()
        check("lights give their squares back", s.atlas_lights == 8, describe(s))
        check("fewer lights: squares no smaller", s.atlas_smallest >= base.atlas_smallest, describe(s))
        -- Spot lights take one square each
        for i = 1, 6 do
            local e = scene.spawn("Atlas spot", vec3(292 + i * 2.5, 6, 300))
            e:look_at(vec3(292 + i * 2.5, 0, 300.5))
            spots[#spots + 1] = e:add_light({ color = vec3(0.6, 0.8, 1), intensity = 3, range = 9, spot_angle = 50, shadows = true })
        end
        return true
    end,
    function()
        local s = scene.render_stats()
        check("spot lights go to the atlas", s.atlas_lights == 14, describe(s))
        for _, l in ipairs(lamps) do l.shadows = true end
        return true
    end,
    function()
        local s = scene.render_stats()
        check("all back: still 28 in the atlas, never over-full", s.atlas_lights == 28 and s.atlas_used <= 0.9, describe(s))
        log(string.format("shadow_atlas_test: %d passed, %d failed", passed, failed))
        return true
    end,
}

function on_update(self, dt)
    if wait > 0 then wait = wait - 1; return end
    local f = steps[step]
    if f and f() then step = step + 1; wait = 10 end
end
