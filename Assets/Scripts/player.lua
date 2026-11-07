-- Roll the ball with the arrow keys, Space jumps while standing on something.
-- Needs a dynamic sphere body. Movement uses forces, so mass and friction matter.

local push = 14.0      -- newtons
local jump = 7.0       -- impulse (N*s): mass 2 -> 3.5 m/s upwards
local spawn

local function grounded(self)
    -- A short ray down from the centre: radius is half the scaled diameter
    local radius = 0.5 * self.scale.x
    return scene.raycast(self.position, vec3(0, -1, 0), radius + 0.08, self) ~= nil
end

function on_create(self)
    spawn = self.position
    shared.score = 0
    log("Player ready: arrows roll, Space jumps")
end

function on_update(self, dt)
    local dir = vec3(0, 0, 0)
    if input.key_down(Key.Left)  then dir.x = dir.x - 1 end
    if input.key_down(Key.Right) then dir.x = dir.x + 1 end
    if input.key_down(Key.Up)    then dir.z = dir.z - 1 end
    if input.key_down(Key.Down)  then dir.z = dir.z + 1 end
    if dir:length() > 0 then
        self.body:add_force(dir:normalize() * push)
    end

    if input.key_pressed(Key.Space) and grounded(self) then
        self.body:add_impulse(vec3(0, jump, 0))
    end

    -- Fell off the world: put it back (setting position teleports a dynamic body)
    if self.position.y < -10 then
        self.position = spawn
        self.body.velocity = vec3(0, 0, 0)
        self.body.angular_velocity = vec3(0, 0, 0)
    end
end

function on_collision_begin(self, other, hit)
    if other.name ~= "Floor" then
        log("Player bumped into", other.name)
    end
end
