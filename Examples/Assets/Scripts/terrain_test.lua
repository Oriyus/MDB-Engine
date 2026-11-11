-- Checks the heightmap terrain (TerrainComponent) on the terrain demo: the script height lookup, the physics
-- height field agreeing with it, bodies resting on it, the navigation bake, and grass standing on it.
-- Run unattended: MDB_SCENE=terrain.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/terrain_test.lua
--   (or MDB_DEMO=terrain instead of MDB_SCENE to build it fresh)
-- Prints "terrain_test: PASS ..." / "FAIL ..." lines and a summary.

local frame = 0
local passed, failed = 0, 0

local function check(name, ok, detail)
    if ok then passed = passed + 1; log("terrain_test: PASS " .. name .. (detail and ("  (" .. detail .. ")") or ""))
    else failed = failed + 1; log("terrain_test: FAIL " .. name .. (detail and ("  (" .. detail .. ")") or "")) end
end

function on_update(self, dt)
    frame = frame + 1
    if frame == 3 then
        local h = terrain.height(0, 0)
        check("height in the meadow", h ~= nil and h > 3 and h < 20, tostring(h))
        check("nothing off the terrain", terrain.height(500, 500) == nil)
        check("vec3 form", terrain.height(vec3(0, 99, 0)) == h)
        local n = terrain.normal(0, 0)
        check("meadow is level", n ~= nil and n.y > 0.99, n and string.format("%.3f %.3f %.3f", n.x, n.y, n.z) or "nil")

        -- The physics height field is the drawn surface: rays down land where terrain.height says
        local worst, misses = 0, 0
        local ground = scene.find("Terrain")
        for i = 0, 24 do
            local x, z = (i % 5) * 23 - 46 + 0.37, math.floor(i / 5) * 23 - 46 + 0.61
            local hit = scene.raycast(vec3(x, 200, z), vec3(0, -1, 0), 400)
            local th = terrain.height(x, z)
            if not hit or not th or hit.entity ~= ground then misses = misses + 1
            else worst = math.max(worst, math.abs(hit.point.y - th)) end
        end
        check("rays hit the terrain", misses == 0, tostring(misses) .. " of 25 missed")
        check("physics matches the drawn surface", worst < 0.02, string.format("worst %.4f m", worst))

        -- Navigation: the bake sees the terrain (a static height field) around the meadow
        local cells = nav.bake{ min = vec3(-12, -5, -12), max = vec3(12, 40, 12) }
        check("nav cells on the terrain", cells and cells > 300, tostring(cells))
        check("nav cell under the meadow", nav.cell_at(vec3(0, h + 0.5, 0)) ~= nil)
    end
    if frame == 10 then
        local s = scene.render_stats()
        check("grass grows on the grass layer", s.foliage > 2000, tostring(s.foliage) .. " clumps")
    end
    if frame == 300 then
        -- The balls rolled down without falling through
        local balls, ok, low = scene.find_all("Ball"), 0, 1e9
        for _, b in ipairs(balls) do
            local p = b.world_position
            local th = terrain.height(p.x, p.z)
            if th and p.y > th + 0.3 and p.y < th + 0.8 then ok = ok + 1 end
            if th then low = math.min(low, p.y - th) end
        end
        check("balls rest on the ground", #balls > 0 and ok == #balls, string.format("%d of %d (lowest %.2f m above)", ok, #balls, low))
        log(string.format("terrain_test: %d passed, %d failed", passed, failed))
    end
end
