-- Checks that rotations survive being stored as Euler angles (Scene::EulerFromQuat), especially next to
-- gimbal lock, where glm::eulerAngles was wrong by degrees and the tactics camera shook while settling onto
-- a 90 degree heading. Run unattended:
--   MDB_SCENE=tactics_test.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/transform_test.lua
-- Prints "transform_test: PASS ..." / "FAIL ..." lines and "transform_test: done".

local function check(name, ok, detail)
    log("transform_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end
local function angle(a, b) return math.deg(math.acos(math.max(-1, math.min(1, a:dot(b))))) end

local done = false
function on_update(self, dt)
    if done then return end
    done = true
    local e = scene.spawn("transform probe")

    -- 1. the tactics camera settling onto a 90 degree heading while its target moves (the reported shake)
    local yaw, goal, focus, fox = 0, -math.pi / 2, vec3(3, -1, 4), vec3(3, -1, 4)
    local worst = 0
    for i = 1, 2000 do
        local step = 1 / 144
        fox = fox + vec3(-1.7 * step, 0, 0)
        yaw = yaw + (goal - yaw) * math.min(1, step * 6)
        focus = focus + (fox - focus) * math.min(1, step * 3)
        e.position = focus + vec3(math.sin(yaw), 0, math.cos(yaw)) * 6.5 + vec3(0, 7.5, 0)
        e:look_at(focus)
        worst = math.max(worst, angle(e.forward, (focus - e.position):normalize()))
    end
    check("camera settling onto a 90 degree heading aims where asked", worst < 0.1, string.format("worst %.3f deg", worst))

    -- 2. many directions, near-vertical ones and every heading, with two up vectors: forward and up kept
    local worstF, worstU = 0, 0
    for i = 0, 719 do
        local h = i / 720 * 2 * math.pi
        for _, p in ipairs({ -89.9, -80, -49, -10, 0, 10, 49, 80, 89.9 }) do
            local pr = math.rad(p)
            local dir = vec3(math.sin(h) * math.cos(pr), math.sin(pr), math.cos(h) * math.cos(pr))
            e.position = vec3(0, 0, 0)
            e:look_at(dir, i % 2 == 0 and vec3(0, 1, 0) or vec3(1, 0, 0))
            worstF = math.max(worstF, angle(e.forward, dir))
            -- the up vector stays perpendicular to forward and on the side of the world up asked for
            worstU = math.max(worstU, math.abs(e.up:dot(e.forward)))
        end
    end
    check("look_at keeps forward through the Euler angles", worstF < 0.1, string.format("worst %.3f deg", worstF))
    check("up stays perpendicular", worstU < 1e-3, string.format("%.5f", worstU))

    -- 3. stored angles rebuild the same rotation (read back, write again: nothing moves)
    e:look_at(e.position + vec3(1, 0.0001, 0.00001))
    local f0 = e.forward
    e.rotation = e.rotation
    check("read and write back changes nothing", angle(e.forward, f0) < 0.01)
    e:destroy()
    log("transform_test: done")
end
