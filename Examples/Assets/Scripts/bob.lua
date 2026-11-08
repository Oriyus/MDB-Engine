-- Hops the entity up and down from where it started.
-- Globals in a script are per entity, so every entity running this file has its own `time` and `base`.

local height = 0.6   -- metres
local speed  = 3.0   -- radians per second

function on_create(self)
    base = self.position
    time = 0
    log(self.name, "starts bobbing at", base)
end

function on_update(self, dt)
    time = time + dt
    -- position is a copy: change it, then assign it back
    local p = self.position
    p.y = base.y + math.abs(math.sin(time * speed)) * height
    self.position = p
end
