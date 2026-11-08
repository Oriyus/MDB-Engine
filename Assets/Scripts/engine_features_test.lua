-- Checks world_to_screen, decals, line of sight / cover and the new primitives on the tactics yard.
-- Run unattended: MDB_SCENE=tactics_test.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/engine_features_test.lua
-- Prints "engine_features_test: PASS ..." / "FAIL ..." lines and "engine_features_test: done".

local frame = 0
local function check(name, ok, detail)
    log("engine_features_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end

local steps = {
    function()
        -- ---- world_to_screen is the inverse of screen_ray
        local worst = 0
        for _, p in ipairs({ { 0.5, 0.5 }, { 0.1, 0.2 }, { 0.9, 0.85 }, { 0.33, 0.71 } }) do
            local o, d = scene.screen_ray(p[1], p[2])
            local x, y, visible = scene.world_to_screen(o + d * 12)
            worst = math.max(worst, math.abs(x - p[1]), math.abs(y - p[2]))
            if not visible then worst = 1 end
        end
        check("world_to_screen inverts screen_ray", worst < 1e-3, string.format("%.6f", worst))
        local o, d = scene.screen_ray(0.5, 0.5)
        local bx, by, bvis = scene.world_to_screen(o - d * 5)
        check("behind the camera: not visible, no x / y", bx == nil and by == nil and bvis == false)
        local ox, oy, ovis, depth = scene.world_to_screen(o + d * 7)
        check("depth along the view", math.abs(depth - 7) < 1e-3, depth)

        -- ---- decals
        decal.draw(vec3(0.5, -1, 9.5), { size = 1, shape = "square", color = vec3(0, 0.5, 1) })
        decal.draw(vec3(1.5, -1, 9.5), { size = vec3(2, 1, 1), shape = "ring", rotation = 0.5 })
        decal.draw(vec3(2.5, -1, 9.5), { size = 1, texture = "Textures/coin_icon.png" })
        check("decal.count this frame", decal.count() == 3, decal.count())
        check("bad shape is an error", not pcall(decal.draw, vec3(0), { shape = "hexagon" }))
        check("missing texture is an error", not pcall(decal.draw, vec3(0), { texture = "Textures/nope.png" }))
    end,
    function()
        check("decals are per frame", decal.count() == 0, decal.count())
        decal.draw(vec3(0.5, -1, 9.5), { size = 1 })
    end,
    function()
        local s = scene.render_stats()
        check("the renderer drew them", s.decals == 1, s.decals)

        -- ---- line of sight
        check("open ground: visible", nav.line_of_sight(vec3(-8.5, -1, 9.5), vec3(-5.5, -1, 9.5)))
        local seen, hit = nav.line_of_sight(vec3(-2.5, -1, -5.5), vec3(-8.5, 1.5, -9.5))
        check("through the west terrace: blocked", not seen and hit ~= nil, hit)
        check("over the 1.2 m fence (eye 1.5): visible", nav.line_of_sight(vec3(-1.5, -1, 4.5), vec3(1.5, -1, 4.5)))
        check("over a crate: visible", nav.line_of_sight(vec3(0.5, -1, 8.5), vec3(4.5, -1, 8.5)))
        check("low eyes behind a crate: blocked",
            not nav.line_of_sight(vec3(0.5, -1, 8.5), vec3(4.5, -1, 8.5), { eye = 0.3, target = 0.3 }))
        local vis = nav.visible_cells(vec3(-8.5, -1, 9.5), 3)
        local within = true
        for _, c in ipairs(vis) do
            local dx, dz = c.position.x + 8.5, c.position.z - 9.5
            within = within and dx * dx + dz * dz <= 9.001
        end
        check("visible_cells: some, all within range", #vis > 10 and within, #vis)

        -- ---- cover
        local c1, n1 = nav.cover(vec3(1.5, -1, 8.5), vec3(7.5, -1, 8.5))
        check("crate between: half cover", c1 == "half" and n1 == 1, c1)
        check("crate behind you: no cover", nav.cover(vec3(1.5, -1, 8.5), vec3(-4.5, -1, 8.5)) == "none")
        check("against the terrace wall: full cover", nav.cover(vec3(-3.5, -1, -6.5), vec3(-10, -1, -6.5)) == "full",
            nav.cover(vec3(-3.5, -1, -6.5), vec3(-10, -1, -6.5)))
        check("behind the fence: half cover", nav.cover(vec3(0.5, -1, 4.5), vec3(-5, -1, 4.5)) == "half")
        check("open ground: no cover", nav.cover(vec3(-8.5, -1, 9.5), vec3(-2.5, -1, 9.5)) == "none")

        -- ---- primitives and flash
        local ok = true
        for _, g in ipairs({ "capsule", "cylinder", "cone" }) do
            local e = scene.spawn("prim " .. g, vec3(0, 5, 0))
            e:add_mesh{ geometry = g }
            ok = ok and e.mesh.geometry == g
            e:destroy()
        end
        check("capsule, cylinder and cone meshes", ok)
        local enemy = scene.find("Enemy")
        check("enemies are figures (child parts)", enemy ~= nil and #enemy:children() >= 4, enemy and #enemy:children())
        check("flash on a figure", pcall(function() enemy:flash(vec3(1, 0, 0), 0.3) end))
        log("engine_features_test: done")
    end,
}

function on_update(self, dt)
    frame = frame + 1
    local i = frame - 4
    if i >= 1 and steps[i] then steps[i]() end
end
