-- Checks Mesh Renderer levels of detail (MeshRendererComponent::Lods, CullDistance; Scene::BuildDrawList): the level
-- picked by distance, the 5% hysteresis, the lens and the scene's distance scale, stats, and a save game round trip
-- (component <-> JSON). Run unattended:
--   MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/lod_test.lua MDB_SAVE_DIR=<folder>
-- Prints "lod_test: PASS ..." / "FAIL ..." lines and a summary.

local phase = 1
local frame = 0
local passed, failed = 0, 0
local cam
local probes = {}
-- Far from the demo's own objects: a camera at EYE looking along -Z, probes straight ahead of it
local EYE = vec3(0, 60, 400)

local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("lod_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end

local LODS = { { geometry = "cone", distance = 40 }, { geometry = "cube", distance = 20 } }   -- unsorted on purpose

local function probe(name, distance)
    local e = scene.spawn(name, EYE + vec3(0, 0, -distance))
    e:add_mesh{ geometry = "sphere", lods = LODS, cull_distance = 80 }
    return e
end

local function place(e, distance) e.position = EYE + vec3(0, 0, -distance) end

local function setup_camera()
    for _, c in ipairs(scene.find_all("Main Camera")) do if c.camera then c.camera.primary = false end end
    cam = scene.spawn("LOD camera", EYE)
    cam:add_camera{ fov = 60, primary = true }
end

function on_save(self) return { phase = phase, passed = passed, failed = failed } end
function on_load(self, data) phase, passed, failed = data.phase, data.passed, data.failed; frame = 0 end

function on_update(self, dt)
    frame = frame + 1
    if phase == 1 then
        if frame == 2 then
            setup_camera()
            probes.near = probe("LOD near", 10)
            probes.mid = probe("LOD mid", 30)
            probes.far = probe("LOD far", 60)
            probes.gone = probe("LOD gone", 100)
            local l = probes.near.mesh.lods
            check("lods sorted nearest first", #l == 2 and l[1].geometry == "cube" and l[2].geometry == "cone" and l[1].distance == 20,
                #l == 2 and (l[1].geometry .. " " .. l[1].distance .. ", " .. l[2].geometry) or #l)
            check("cull distance", probes.near.mesh.cull_distance == 80)
        elseif frame == 4 then
            check("near: main geometry", probes.near.mesh.lod == 0, probes.near.mesh.lod)
            check("mid: level 1", probes.mid.mesh.lod == 1, probes.mid.mesh.lod)
            check("far: level 2", probes.far.mesh.lod == 2, probes.far.mesh.lod)
            check("beyond cull distance", probes.gone.mesh.lod == 3, probes.gone.mesh.lod)
            local s = scene.render_stats()
            check("stats count them", s.lod_reduced >= 2 and s.lod_culled >= 1, s.lod_reduced .. " lower, " .. s.lod_culled .. " culled")
            -- Hysteresis: back to the finer level only 5% nearer than the switch distance
            place(probes.mid, 19.5)
        elseif frame == 6 then
            check("hysteresis keeps level 1 at 19.5 m", probes.mid.mesh.lod == 1, probes.mid.mesh.lod)
            place(probes.mid, 18.5)
        elseif frame == 8 then
            check("level 0 again at 18.5 m", probes.mid.mesh.lod == 0, probes.mid.mesh.lod)
            place(probes.mid, 30)
            scene.lod_scale(2)                               -- every distance x 2: 30 m acts like 15 m
        elseif frame == 10 then
            check("lod_scale 2 keeps detail further", probes.mid.mesh.lod == 0 and probes.far.mesh.lod == 1,
                probes.mid.mesh.lod .. " " .. probes.far.mesh.lod)
            scene.lod_scale(1)
            cam.camera.fov = 30                              -- zoomed in: 60 m looks like 28 m
        elseif frame == 12 then
            check("narrow lens keeps detail further", probes.far.mesh.lod == 1, probes.far.mesh.lod)
            check("scale back to 1", scene.lod_scale() == 1)
            cam.camera.fov = 60
            probes.mid.mesh.lods = { { geometry = "cylinder", distance = 25 } }
            probes.mid.mesh.cull_distance = 0
        elseif frame == 14 then
            check("lods replaced from a script", probes.mid.mesh.lod == 1 and #probes.mid.mesh.lods == 1, probes.mid.mesh.lod)
            check("bad geometry is an error", not pcall(function() probes.near.mesh.lods = { { geometry = "nope", distance = 5 } } end))
            phase = 2
            saves.save("lod_test")                          -- the probes go through EntityJson
            saves.load("lod_test")
        end
    elseif phase == 2 and frame == 3 then
        local mid, far = scene.find("LOD mid"), scene.find("LOD far")
        local l = mid and mid.mesh and mid.mesh.lods or {}
        check("saved lods load back", #l == 1 and l[1].geometry == "cylinder" and l[1].distance == 25 and mid.mesh.cull_distance == 0,
            #l == 1 and (l[1].geometry .. " " .. l[1].distance) or #l)
        local f = far and far.mesh and far.mesh.lods or {}
        check("saved cull distance loads back", #f == 2 and far.mesh.cull_distance == 80, #f)
        saves.delete("lod_test")
        log(string.format("lod_test: %d passed, %d failed", passed, failed))
        phase = 3
    end
end
