-- Third-person follow camera: trails the target at a fixed offset and keeps looking at it.
-- Put it on an entity with a Camera component.

local target_name = "Player"
local offset = vec3(0, 3.5, 6.5)    -- above and behind (+Z is "behind" for this level)
local stiffness = 4.0               -- higher = snappier, lower = floatier
local target

local function aim(self)
    self:look_at(target.position + vec3(0, 0.5, 0))
end

function on_create(self)
    target = scene.find(target_name)
    if target then
        self.position = target.position + offset
        aim(self)
    else
        log("follow.lua: no entity named", target_name)
    end
end

function on_update(self, dt)
    if not (target and target:valid()) then
        target = scene.find(target_name)
        if not target then return end
    end
    local want = target.position + offset
    self.position = self.position + (want - self.position) * math.min(1, dt * stiffness)
    aim(self)
end
