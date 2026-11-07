-- A dropped ball: flashes on every impact and removes itself after a while.

local life = 6.0
local age = 0
local bounces = 0

function on_update(self, dt)
    age = age + dt
    if age > life or self.position.y < -10 then
        self:destroy()
        return
    end
    self.mesh.emissive = self.mesh.emissive * math.max(0, 1 - dt * 5)   -- fade the flash
end

function on_collision_begin(self, other, hit)
    bounces = bounces + 1
    self.mesh.emissive = vec3(2.0, 1.2, 0.4)
    if bounces == 1 then
        log(self.name, "landed on", other.name, "- normal", hit.normal)
    end
end

function on_destroy(self)
    log(self.name, "despawned after", bounces, "bounces")
end
