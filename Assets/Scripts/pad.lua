-- Glows while anything is inside its trigger volume.
-- Exit also fires when the thing inside is destroyed, so `other` may no longer exist:
-- tostring(other) is safe, other.name would be an error then.

local inside = 0
local glow = 0

function on_trigger_enter(self, other)
    inside = inside + 1
    log("Pad:", tostring(other), "entered,", inside, "inside")
end

function on_trigger_exit(self, other)
    inside = math.max(0, inside - 1)
    log("Pad:", tostring(other), "left,", inside, "inside")
end

function on_update(self, dt)
    local target = inside > 0 and 3 or 0
    glow = glow + (target - glow) * math.min(1, dt * 6)
    self.mesh.emissive = vec3(0.2, 0.6, 1.0) * glow
end
