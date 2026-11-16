-- A walking player: the "move" action walks (relative to the camera's view; WASD, arrows or the left stick, which
-- also walks slowly when pushed part way), "sprint" runs (Shift, LT or a stick click), "jump" jumps (Space, A),
-- "crouch" (C, if the action exists) crouches. The actions are in Assets/Data/input.json.
-- Put it on an entity with a Character Controller (Inspector > Add Component): the controller walks up steps and
-- slopes, keeps to the ground going down them, rides moving platforms and pushes boxes; this script only says
-- which way to go. A child named "Model" is the visible box: it turns to face the way you walk.

local walk_speed  = 4.0     -- m/s
local run_speed   = 7.0     -- m/s, holding Shift
local crouch_speed = 2.0    -- m/s, crouching
local accel       = 12.0    -- how quickly it reaches the wanted speed on the ground, and stops (per second)
local air_accel   = 3.0     -- the same in the air (a little steering, mostly momentum)
local jump_speed  = 5.0     -- m/s upwards: about 1.3 m high
local turn_speed  = 12.0    -- how quickly the model turns to face the way it walks
local crouch_height = 1.1   -- m (standing: the controller's height when the level starts)

local spawn, model, model_scale, model_pos
local yaw = 0.0
local flat = vec3(0, 0, 0)  -- the sideways velocity we ask for, eased
local stand_height
local crouching = false

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

local function has_action(name)
    for _, a in ipairs(input.actions()) do if a == name then return true end end
    return false
end

function on_create(self)
    spawn = self.position
    for _, c in ipairs(self:children()) do
        if c.name == "Model" then model = c end
    end
    if model then model_scale, model_pos = model.scale, model.position end
    if not self.character then log("village_player.lua: the player needs a Character Controller"); return end
    stand_height = self.character.height
end

-- Crouch and stand: the capsule gets shorter with its feet in place (standing up needs room above)
local function update_crouch(self, ch)
    if not has_action("crouch") then return end
    local want = input.action("crouch")
    if want ~= crouching and ch:set_size(want and crouch_height or stand_height) then
        crouching = want
        if model then                    -- squash the box to match, feet on the ground
            local k = ch.height / stand_height
            model.scale = vec3(model_scale.x, model_scale.y * k, model_scale.z)
            model.position = model_pos - vec3(0, (stand_height - ch.height) * 0.5, 0)
        end
    end
end

function on_update(self, dt)
    local ch = self.character
    if not ch then return end
    update_crouch(self, ch)

    local forward, right = move_axes()
    local mx, my = input.vector("move")                  -- length 0..1: a stick part way walks slower
    local dir = forward * my + right * mx
    local speed = crouching and crouch_speed or (input.action("sprint") and run_speed or walk_speed)
    local want = dir * speed

    -- Ease towards the wanted velocity (quickly on the ground, slowly in the air); gravity is the controller's
    local a = ch.grounded and accel or air_accel
    flat = flat + (want - flat) * (1 - math.exp(-a * dt))
    ch:move(flat)
    if input.pressed("jump") and not crouching then ch:jump(jump_speed) end

    -- The model faces the way it walks (the controller itself never turns)
    if model and want:length() > 0.1 then
        local target = math.atan(want.x, want.z)          -- +Z is the model's front
        yaw = yaw + angle_delta(yaw, target) * math.min(1, turn_speed * dt)
        model.rotation = vec3(0, yaw, 0)
    end

    if self.position.y < -10 then                         -- fell off the world: back to the start
        self.position = spawn
        ch.velocity = vec3(0, 0, 0)
        flat = vec3(0, 0, 0)
    end
end
