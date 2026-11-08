-- Tactical camera: looks down at shared.focus (set by tactics.lua: the unit whose turn it is, or
-- the hero) from above and behind, easing after it. Q / E turn the view by 45 degrees.
-- Put it on an entity with a Camera component.

local distance, height = 6.5, 7.5
local yaw, yaw_goal = 0, 0           -- radians around the focus; 0 = looking towards -Z
local focus

local function place(self)
    local back = vec3(math.sin(yaw), 0, math.cos(yaw)) * distance
    self.position = focus + back + vec3(0, height, 0)
    self:look_at(focus)
end

function on_create(self)
    focus = shared.focus or vec3(0, 0, 0)
    place(self)
end

function on_update(self, dt)
    if input.key_pressed(Key.Q) then yaw_goal = yaw_goal - math.pi / 4 end
    if input.key_pressed(Key.E) then yaw_goal = yaw_goal + math.pi / 4 end
    yaw = yaw + (yaw_goal - yaw) * math.min(1, dt * 6)
    local want = shared.focus or focus
    focus = focus + (want - focus) * math.min(1, dt * 3)
    place(self)
end
