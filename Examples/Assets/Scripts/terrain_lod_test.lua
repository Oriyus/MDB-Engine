-- Checks terrain levels of detail (TerrainData::SelectLods, Scene::Lod.TerrainError): coarser chunks far away, none at
-- error 0, more at a larger error or a smaller distance scale, fewer triangles drawn, heights untouched, the setting.
-- Run unattended:
--   MDB_SCENE=terrain.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/terrain_lod_test.lua
-- Prints "terrain_lod_test: PASS ..." / "FAIL ..." lines and a summary.

local passed, failed = 0, 0
local frame = 0
local step = 0
local done = false
local results = {}
local probeHeight

local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("terrain_lod_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end

-- Each step: set things up, wait a few frames (stats are last frame's), then read the stats
local steps = {
    { name = "default", error = 1, scale = 1 },
    { name = "full", error = 0, scale = 1 },
    { name = "loose", error = 8, scale = 1 },
    { name = "near", error = 1, scale = 0.25 },
    { name = "back from coarser", error = 1, scale = 1 },
    { name = "full again", error = 0, scale = 1 },
    { name = "back from full", error = 1, scale = 1 },
}

function on_create(self)
    for _, c in ipairs(scene.find_all("Main Camera")) do if c.camera then c.camera.primary = false end end
    local cam = scene.spawn("LOD camera", vec3(0, 30, 75))
    cam:add_camera{ fov = 60, primary = true }
    cam:look_at(vec3(0, 0, -20))
    probeHeight = terrain.height(10, -30)
end

function on_update(self, dt)
    if done then return end
    frame = frame + 1
    if step == 0 or frame >= 6 then
        if step > 0 then
            local s = scene.render_stats()
            results[steps[step].name] = { reduced = s.terrain_lod_reduced, triangles = s.triangles }
            log(string.format("terrain_lod_test: %s: %d chunks coarser, %d triangles", steps[step].name, s.terrain_lod_reduced, s.triangles))
        end
        step = step + 1
        frame = 0
        local st = steps[step]
        if st then
            scene.terrain_lod_error(st.error)
            scene.lod_scale(st.scale)
            return
        end
        local d, f, l, n = results["default"], results["full"], results["loose"], results["near"]
        local c, b = results["back from coarser"], results["back from full"]
        check("far chunks are coarser by default", d.reduced > 0, d.reduced)
        check("error 0 draws every chunk in full", f.reduced == 0, f.reduced)
        check("coarser chunks draw fewer triangles", d.triangles < f.triangles, d.triangles .. " < " .. f.triangles)
        check("a larger error coarsens more", l.reduced >= d.reduced and l.triangles < d.triangles, l.reduced .. ", " .. l.triangles)
        check("a smaller distance scale coarsens more", n.triangles < d.triangles, n.triangles .. " < " .. d.triangles)
        check("coming back from coarser keeps them within the margin (hysteresis)", c.reduced >= d.reduced and c.triangles <= d.triangles,
            c.reduced .. " chunks, " .. c.triangles)
        check("coming back from full detail gives the same levels again", b.triangles == d.triangles and b.reduced == d.reduced,
            b.triangles .. " / " .. d.triangles)
        check("heights don't change with the level drawn", terrain.height(10, -30) == probeHeight)
        check("terrain_lod_error reads back", scene.terrain_lod_error(2.5) == 2.5)
        check("a negative error is 0", scene.terrain_lod_error(-3) == 0)
        scene.terrain_lod_error(1)
        log(string.format("terrain_lod_test: %d passed, %d failed", passed, failed))
        done = true
    end
end
