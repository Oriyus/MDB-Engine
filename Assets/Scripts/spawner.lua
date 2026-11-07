-- Drops a ball every `every` seconds from around this entity's position.
-- The ball is a prefab (Prefabs/drop_ball.json: mesh, body and ball.lua); each one gets its own
-- colour and a little sideways nudge here. Edit the prefab to change every ball.

local every = 1.2
local timer = 0
local count = 0

function on_update(self, dt)
    timer = timer + dt
    if timer < every then return end
    timer = timer - every
    count = count + 1

    local p = self.position + vec3(math.random() * 1.6 - 0.8, 0, math.random() * 1.6 - 0.8)
    local ball = scene.spawn_prefab("drop_ball", p)
    if not ball then return end                        -- prefab missing: warned once already
    ball.name = "Drop " .. count
    ball.mesh.color = vec3(math.random(), math.random(), math.random())
    ball.body:add_impulse(vec3(math.random() - 0.5, 0, math.random() - 0.5) * 0.3)
end
