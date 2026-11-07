-- An animated character that wanders in a circle around where it starts: it walks, breaks into
-- a run, then stops to look around, cross-fading between its clips.
-- Needs an Animator on a model with "Walk", "Run" and "Survey" clips (like Models/Fox.glb).
-- The model faces +Z; its yaw is set from the direction it moves.

local radius = 2.5
local plan = {                     -- { clip, seconds, metres per second }
    { "Walk", 6.0, 1.1 },
    { "Run", 3.0, 3.6 },
    { "Survey", 3.4, 0.0 },
}
local center, angle = nil, 0
local step, left = 1, 0

local function begin(self, i)
    step = i
    left = plan[i][2]
    self.animator:play(plan[i][1], 0.35)
end

function on_create(self)
    center = self.position
    begin(self, 1)
end

function on_update(self, dt)
    left = left - dt
    if left <= 0 then begin(self, step % #plan + 1) end

    local speed = plan[step][3]
    angle = angle + speed / radius * dt
    self.position = center + vec3(math.cos(angle), 0, math.sin(angle)) * radius
    -- moving along the circle: the direction is its tangent
    local dir = vec3(-math.sin(angle), 0, math.cos(angle))
    self.rotation = vec3(0, math.atan(dir.x, dir.z), 0)
end
