-- Roll the ball with the arrow keys, Space jumps while standing on something.
-- Needs a dynamic sphere body. Movement uses forces, so mass and friction matter.
-- With a "Main Camera" in the scene, Up rolls away from the camera; otherwise along -Z.
-- Other scripts can call shared.respawn() to put the ball back at its start.

local push = 14.0      -- newtons
local jump = 7.0       -- impulse (N*s): mass 2 -> 3.5 m/s upwards
local spawn

local function grounded(self)
    -- A short ray down from the centre: radius is half the scaled diameter
    local radius = 0.5 * self.scale.x
    return scene.raycast(self.position, vec3(0, -1, 0), radius + 0.08, self) ~= nil
end

-- Forward and right on the ground plane, as seen from the camera
local function move_axes()
    local cam = scene.find("Main Camera")
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

function on_create(self)
    spawn = self.position
    shared.score = 0
    shared.respawn = function()
        self.position = spawn               -- setting position teleports a dynamic body
        self.body.velocity = vec3(0, 0, 0)
        self.body.angular_velocity = vec3(0, 0, 0)
    end
    log("Player ready: arrows roll, Space jumps")
end

function on_update(self, dt)
    if shared.state and shared.state ~= "playing" then return end   -- game over: no control

    local forward, right = move_axes()
    local dir = vec3(0, 0, 0)
    if input.key_down(Key.Up)    then dir = dir + forward end
    if input.key_down(Key.Down)  then dir = dir - forward end
    if input.key_down(Key.Right) then dir = dir + right end
    if input.key_down(Key.Left)  then dir = dir - right end
    if dir:length() > 0 then
        self.body:add_force(dir:normalize() * push)
    end

    if input.key_pressed(Key.Space) and grounded(self) then
        self.body:add_impulse(vec3(0, jump, 0))
    end

    if self.position.y < -10 then shared.respawn() end    -- fell off the world
end

function on_collision_begin(self, other, hit)
    if other.name == "Sweeper" then log("Player got swept!") end
end
