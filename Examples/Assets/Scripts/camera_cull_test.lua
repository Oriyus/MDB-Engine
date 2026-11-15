-- Checks the camera's culling (CameraComponent::SmallObjects / GrassDistance, Scene::BuildDrawList, FoliageRenderer):
-- grass distance through the game camera, small objects culled by their size on screen (with the 5% hysteresis, the
-- lens and the scene's distance scale), and a save game round trip of both fields (component <-> JSON). Run unattended:
--   MDB_SCENE=village.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/camera_cull_test.lua MDB_SAVE_DIR=<folder>
-- Prints "camera_cull_test: PASS ..." / "FAIL ..." lines and a summary.

local phase = 1
local frame = 0
local passed, failed = 0, 0
local main, cam
local grass0 = 0
local probes = {}
-- Far from the village: a camera at EYE looking along -Z, probes ahead of it (a little apart, so none hides another)
local EYE = vec3(0, 60, 400)

local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("camera_cull_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end

local function place(e, distance, x) e.position = EYE + vec3(x or 0, 0, -distance) end

local function probe(name, distance, x, size)
    local e = scene.spawn(name, EYE)
    place(e, distance, x)
    e:add_mesh{ geometry = "sphere" }            -- a unit sphere: its box half-diagonal is 0.866 m
    if size then e.scale = vec3(size, size, size) end
    return e
end

-- A unit sphere hides beyond 0.866 / (share * tan(fov / 2)): 75 m at 2% through a 60 degree lens
local SHARE = 0.02

function on_save(self) return { phase = phase, passed = passed, failed = failed } end
function on_load(self, data) phase, passed, failed = data.phase, data.passed, data.failed; frame = 0 end

function on_update(self, dt)
    frame = frame + 1
    if phase == 1 then
        -- Grass, through the village's own game camera
        if frame == 2 then
            main = scene.find("Main Camera")
            check("main camera", main ~= nil and main.camera ~= nil)
            check("defaults: off and x1", main.camera.small_objects == 0 and main.camera.grass_distance == 1,
                main.camera.small_objects .. ", " .. main.camera.grass_distance)
        elseif frame == 5 then
            grass0 = scene.render_stats().foliage
            check("grass drawn", grass0 > 1000, grass0)
            main.camera.grass_distance = 0.3
        elseif frame == 8 then
            local n = scene.render_stats().foliage
            check("shorter grass distance draws fewer clumps", n < grass0 * 0.7, n .. " of " .. grass0)
            main.camera.grass_distance = 0
        elseif frame == 11 then
            local n = scene.render_stats().foliage
            check("grass distance 0: no grass", n == 0, n)
            main.camera.grass_distance = 1
        elseif frame == 14 then
            local n = scene.render_stats().foliage
            check("grass back at x1", n > grass0 * 0.9, n .. " of " .. grass0)
            -- Small objects, through a camera of our own
            main.camera.primary = false
            cam = scene.spawn("Cull camera", EYE)
            cam:add_camera{ fov = 60, primary = true, small_objects = SHARE }
            probes.near = probe("Cull near", 50, -6)
            probes.far = probe("Cull far", 100, 0)
            probes.big = probe("Cull big", 100, 20, 10)       -- 10x the size: hides only beyond 750 m
        elseif frame == 17 then
            check("near small object drawn", probes.near.mesh.lod == 0, probes.near.mesh.lod)
            check("far small object culled", probes.far.mesh.lod == 1, probes.far.mesh.lod)
            check("big object at the same distance drawn", probes.big.mesh.lod == 0, probes.big.mesh.lod)
            local s = scene.render_stats()
            check("stats count it", s.lod_culled >= 1, s.lod_culled)
            place(probes.far, 73)                               -- inside the 5% band (71.25 .. 75): stays culled
        elseif frame == 19 then
            check("hysteresis: stays culled just inside", probes.far.mesh.lod == 1, probes.far.mesh.lod)
            place(probes.far, 70)
        elseif frame == 21 then
            check("shown again 5% nearer", probes.far.mesh.lod == 0, probes.far.mesh.lod)
            place(probes.far, 100)
            cam.camera.fov = 30                                 -- a narrow lens: objects look bigger, kept further
        elseif frame == 24 then
            check("narrow lens keeps it", probes.far.mesh.lod == 0, probes.far.mesh.lod)
            cam.camera.fov = 60
            scene.lod_scale(0.5)                                -- lower quality: everything hides at half the distance
        elseif frame == 27 then
            check("lod scale 0.5 culls the near one too", probes.near.mesh.lod == 1, probes.near.mesh.lod)
            scene.lod_scale(1)
            cam.camera.small_objects = 0
        elseif frame == 30 then
            check("off: everything drawn", probes.near.mesh.lod == 0 and probes.far.mesh.lod == 0,
                probes.near.mesh.lod .. " " .. probes.far.mesh.lod)
            check("bad values clamp to 0", (function() cam.camera.grass_distance = -2; return cam.camera.grass_distance == 0 end)())
            cam.camera.grass_distance = 0.5
            cam.camera.small_objects = SHARE
            phase = 2
            saves.save("camera_cull_test")                      -- the camera goes through EntityJson
            saves.load("camera_cull_test")
        end
    elseif phase == 2 and frame == 3 then
        local c = scene.find("Cull camera")
        check("saved camera culling loads back", c ~= nil and c.camera ~= nil and math.abs(c.camera.small_objects - SHARE) < 1e-6
            and c.camera.grass_distance == 0.5, c and c.camera and (c.camera.small_objects .. ", " .. c.camera.grass_distance) or "missing")
        saves.delete("camera_cull_test")
        log(string.format("camera_cull_test: %d passed, %d failed", passed, failed))
        phase = 3
    end
end
