-- An animated character that wanders in a circle around where it starts: it walks, breaks into
-- a run, then stops to look around, cross-fading between its clips.
-- Needs an Animator on a model with "Walk", "Run" and "Survey" clips (like Models/Fox.glb).
-- If the Animator has a layer 1 (in the demo: Survey added on the neck), the character turns its head
-- to look around in the middle of each walk. If it has IK 1 (in the demo: look at the player ball),
-- it watches that target whenever it comes within `watch_range`. Footstep sounds are Animator
-- events, not script. The model faces +Z; its yaw is set from the direction it moves.

local radius = 2.5
local watch_range = 4.0
local plan = {                     -- { clip, seconds, metres per second }
    { "Walk", 6.0, 1.1 },
    { "Run", 3.0, 3.6 },
    { "Survey", 3.4, 0.0 },
}
local center, angle = nil, 0
local step, left = 1, 0
local looking = false
local watch = 0                    -- IK weight, eased towards 0 or 1

local function look(self, on)
    if looking == on or self.animator:layer_count() < 1 then return end
    looking = on
    self.animator:layer_weight(1, on and 1 or 0, 0.6)     -- fade the head layer in or out
end

local function begin(self, i)
    step = i
    left = plan[i][2]
    self.animator:play(plan[i][1], 0.35)
    look(self, false)
end

-- Is IK 1's target within watch_range?
local function target_near(self)
    if self.animator:ik_count() < 1 then return false end
    local k = self.animator:ik(1)
    return k.target ~= nil and (k.target - self.position):length() < watch_range
end

function on_create(self)
    center = self.position
    begin(self, 1)
end

function on_update(self, dt)
    left = left - dt
    if left <= 0 then begin(self, step % #plan + 1) end

    -- Watch the target when it is near (IK), otherwise look around in the middle of a walk (layer)
    local near = target_near(self)
    if self.animator:ik_count() > 0 then
        local goal = near and 1 or 0
        watch = watch + math.max(-dt * 3, math.min(dt * 3, goal - watch))
        self.animator:ik_weight(1, watch)
    end
    if plan[step][1] == "Walk" and not near then
        local elapsed = plan[step][2] - left
        look(self, elapsed > 1.5 and elapsed < 4.5)
    else
        look(self, false)
    end

    local speed = plan[step][3]
    angle = angle + speed / radius * dt
    self.position = center + vec3(math.cos(angle), 0, math.sin(angle)) * radius
    -- moving along the circle: the direction is its tangent
    local dir = vec3(-math.sin(angle), 0, math.cos(angle))
    self.rotation = vec3(0, math.atan(dir.x, dir.z), 0)
end
