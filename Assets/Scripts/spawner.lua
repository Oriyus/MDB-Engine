-- Drops a ball every `every` seconds from around this entity's position.
-- Shows building an entity from scratch: spawn, then add a mesh, a body and a script.

local every = 1.2
local timer = 0
local count = 0

function on_update(self, dt)
    timer = timer + dt
    if timer < every then return end
    timer = timer - every
    count = count + 1

    local p = self.position + vec3(math.random() * 1.6 - 0.8, 0, math.random() * 1.6 - 0.8)
    local ball = scene.spawn("Drop " .. count, p)
    ball.scale = vec3(0.4, 0.4, 0.4)      -- set before the body: the collider is sized from it
    ball:add_mesh{ geometry = "sphere", color = vec3(math.random(), math.random(), math.random()), roughness = 0.4 }
    ball:add_body{ shape = "sphere", bounciness = 0.35 }
    ball.body:add_impulse(vec3(math.random() - 0.5, 0, math.random() - 0.5) * 0.3)
    ball:add_script("Scripts/ball.lua")
end
