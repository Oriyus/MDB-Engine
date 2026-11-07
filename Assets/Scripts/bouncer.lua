-- Bounce pad: launches any dynamic body that lands on it straight up.

local power = 9.0      -- m/s upwards: about 4 m high
local flash = 0

function on_trigger_enter(self, other)
    local body = other.body
    if body and body.type == "dynamic" then
        local v = body.velocity
        v.y = power
        body.velocity = v
        flash = 1
    end
end

function on_update(self, dt)
    flash = math.max(0, flash - dt * 3)
    self.mesh.emissive = vec3(0.1, 0.8, 0.3) * (0.6 + flash * 4)
end
