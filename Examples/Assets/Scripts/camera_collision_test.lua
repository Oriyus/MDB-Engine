-- Checks scene.spherecast and the follow camera's collision (Scripts/follow.lua): the camera comes in at once when a
-- wall gets between it and the player, never sees through it, eases back out when it's gone, and ignores dynamic
-- bodies. Moves the village's Player to a course of its own.
-- Run unattended:
--   MDB_SCENE=village.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/camera_collision_test.lua
-- Prints "camera_collision_test: PASS ..." / "FAIL ..." lines and a summary.

local O = vec3(300, 0, 300)
local passed, failed = 0, 0
local step, elapsed, started = 1, 0.0, false
local player, cam, wall, crate
local FULL = vec3(0, 3.0, 6.5):length()     -- follow.lua's offset from its look point (0.5 above the origin)

local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("camera_collision_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end
local function f2(v) return string.format("%.2f", v) end

local function box(name, pos, size, kind)
    local e = scene.spawn(name, O + pos)
    e:add_mesh{ geometry = "cube", color = vec3(0.6, 0.6, 0.65) }
    e.scale = size
    e:add_body{ type = kind or "static", shape = "box", size = vec3(1, 1, 1), mass = 20 }
    return e
end

local function pivot() return player.position + vec3(0, 0.5, 0) end
local function cam_distance() return (cam.position - pivot()):length() end
local function sees_player()          -- nothing static between the camera and the look point
    local d = pivot() - cam.position
    return scene.raycast(cam.position, d, d:length(), player) == nil
end

local steps = {
    { 0.2, function()
        box("CC ground", vec3(0, -0.5, 0), vec3(40, 1, 40))
        player.position = O + vec3(0, 0.95, 0)
    end, function()
        local h = scene.spherecast(O + vec3(0, 5, -5), vec3(0, -1, 0), 10, 0.5)
        check("spherecast hits the ground", h ~= nil and h.entity.name == "CC ground" and math.abs(h.distance - 4.5) < 0.02,
            h and f2(h.distance))
        check("spherecast normal", h ~= nil and h.normal.y > 0.99, h and tostring(h.normal))
        check("spherecast contact on the surface", h ~= nil and math.abs(h.point.y) < 0.02, h and f2(h.point.y))
        local r = scene.raycast(O + vec3(0.45, 5, -5), vec3(0, -1, 0), 10)
        check("a ray beside it agrees", r ~= nil and math.abs(r.distance - 5) < 0.02)
        local inside = scene.spherecast(O + vec3(0, 0.1, -5), vec3(0, 1, 0), 3, 0.5)
        check("starting inside hits at 0", inside ~= nil and inside.distance < 1e-3, inside and f2(inside.distance))
        check("misses above it all", scene.spherecast(O + vec3(0, 5, -5), vec3(0, 1, 0), 10, 0.5) == nil)
    end },
    { 3.0, function() end, function()
        check("follows at full distance", math.abs(cam_distance() - FULL) < 0.25, f2(cam_distance()))
        check("sees the player", sees_player())
        wall = box("CC wall", vec3(0, 2, 3), vec3(12, 6, 0.4))
    end },
    { 0.15, function() end, function()
        local d = cam_distance()
        check("a wall pulls the camera in at once", d < 3.0 and d > 2.0, f2(d))
        check("it doesn't see through the wall", sees_player())
        check("it stays on the player's side", cam.position.z < wall.position.z - 0.2 - 0.25, f2(cam.position.z - O.z))
        wall:destroy()
    end },
    { 0.4, function() end, function()
        local d = cam_distance()
        check("eases back out", d > 3.0 and d < FULL - 0.3, f2(d))
    end },
    { 3.0, function() end, function()
        check("back at full distance", math.abs(cam_distance() - FULL) < 0.25, f2(cam_distance()))
        crate = box("CC crate", vec3(0, 1.6, 3), vec3(6, 3, 0.5), "dynamic")
    end },
    { 1.0, function() end, function()
        check("a dynamic body doesn't push it in", math.abs(cam_distance() - FULL) < 0.25, f2(cam_distance()))
        check("the crate is in the way", scene.raycast(cam.position, pivot() - cam.position, cam_distance(), player) ~= nil)
        log(string.format("camera_collision_test: %d passed, %d failed", passed, failed))
    end },
}

function on_update(self, dt)
    if step > #steps then return end
    player = player or scene.find("Player")
    cam = cam or scene.find("Main Camera")
    if not (player and cam) then check("village Player and Main Camera", false); step = #steps + 1; return end
    if not started then steps[step][2](); started = true end
    elapsed = elapsed + dt
    if elapsed >= steps[step][1] then
        steps[step][3]()
        step, elapsed, started = step + 1, 0.0, false
    end
end
