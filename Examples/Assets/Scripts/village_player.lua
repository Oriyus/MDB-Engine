-- A walking player: WASD moves (relative to the camera's view), Shift runs, Space jumps.
-- Put it on an entity with a dynamic Rigid Body that has Lock rotation on (so collisions never tip it over) and
-- friction 0 (so walls don't grab it while you walk along them). A child named "Model" is the visible box: it
-- turns to face the way you walk. Movement sets the body's velocity (sideways only; gravity keeps the vertical),
-- so walls, trees and houses with colliders stop it like any other physics object.

local walk_speed  = 4.0     -- m/s
local run_speed   = 7.0     -- m/s, holding Shift
local accel       = 12.0    -- how quickly it reaches the wanted speed, and stops (per second)
local jump_speed  = 5.0     -- m/s upwards: about 1.3 m high
local turn_speed  = 12.0    -- how quickly the model turns to face the way it walks
local half_height = 0.9     -- from the body's centre to its feet (capsule 1.8 m tall)

local spawn, model
local yaw = 0.0

-- Standing on something: a short ray down from just above the feet
local function grounded(self)
    local feet = self.position - vec3(0, half_height - 0.05, 0)
    return scene.raycast(feet, vec3(0, -1, 0), 0.15, self) ~= nil
end

-- Forward and right on the ground, as seen from the game camera (or the world axes without one)
local function move_axes()
    local cam = scene.find("Main Camera") or scene.find("MainCamera")
    if cam then
        local f = cam.forward
        f.y = 0
        if f:length() > 0.01 then
            f = f:normalize()
            return f, vec3(-f.z, 0, f.x)
        end
    end
    return vec3(0, 0, -1), vec3(1, 0, 0)
end

-- The shortest way round from angle a to b (radians)
local function angle_delta(a, b)
    local d = (b - a) % (2 * math.pi)
    if d > math.pi then d = d - 2 * math.pi end
    return d
end

function on_create(self)
    spawn = self.position
    for _, c in ipairs(self:children()) do
        if c.name == "Model" then model = c end
    end
    if not self.body then log("village_player.lua: the player needs a dynamic Rigid Body") end
end

function on_update(self, dt)
    if not self.body then return end
    local forward, right = move_axes()
    local dir = vec3(0, 0, 0)
    if input.key_down(Key.W) then dir = dir + forward end
    if input.key_down(Key.S) then dir = dir - forward end
    if input.key_down(Key.D) then dir = dir + right end
    if input.key_down(Key.A) then dir = dir - right end

    local speed = (input.key_down(Key.LeftShift) or input.key_down(Key.RightShift)) and run_speed or walk_speed
    local want = dir:length() > 0 and dir:normalize() * speed or vec3(0, 0, 0)

    -- Ease the sideways velocity towards the wanted one; keep the vertical one (gravity, jumps, falling)
    local v = self.body.velocity
    local flat = vec3(v.x, 0, v.z)
    flat = flat + (want - flat) * (1 - math.exp(-accel * dt))
    local vy = v.y
    if input.key_pressed(Key.Space) and grounded(self) then vy = jump_speed end
    self.body.velocity = vec3(flat.x, vy, flat.z)

    -- The model faces the way it walks (the body itself never turns: Lock rotation)
    if model and want:length() > 0.1 then
        local target = math.atan(want.x, want.z)          -- +Z is the model's front
        yaw = yaw + angle_delta(yaw, target) * math.min(1, turn_speed * dt)
        model.rotation = vec3(0, yaw, 0)
    end

    if self.position.y < -10 then                         -- fell off the world: back to the start
        self.position = spawn
        self.body.velocity = vec3(0, 0, 0)
    end
end
