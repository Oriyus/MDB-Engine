-- wander.lua's fox, driven by an animation graph instead of clip by clip: the script only moves the fox and sets
-- the graph's parameters, and AnimGraphs/fox.json decides what plays (Idle / Walk-Run blend / a one-shot Look,
-- plus an additive head layer). Try it: give the Script Demo's Fox the Graph "AnimGraphs/fox.json" (Inspector >
-- Animator) and this script, press Play, and open the graph (Edit graph) to watch its states live.

local radius = 2.5
local plan = {                     -- { metres per second, seconds }: walk, run, stop (look around), walk slowly
    { 1.1, 6.0 }, { 3.6, 3.0 }, { 0.0, 3.0 }, { 0.6, 4.0 },
}
local center, angle = nil, 0
local step, left = 1, 0
local speed = 0                    -- eased towards the plan's speed, so the blend glides from walk to run

function on_create(self)
    center = self.position
    left = plan[1][2]
end

function on_anim_event(self, name, state)
    if name == "enter" then log(self.name .. " now in " .. state) end
end

function on_update(self, dt)
    left = left - dt
    if left <= 0 then
        step = step % #plan + 1
        left = plan[step][2]
        if plan[step][1] == 0 then self.animator:trigger("look_around") end   -- the one-shot Look state
    end
    local want = plan[step][1]
    speed = speed + math.max(-dt * 3, math.min(dt * 3, want - speed))
    self.animator:set("speed", speed)
    -- the head layer: look about while walking slowly
    local head = self.animator:get("head")
    local goal = (want > 0 and want < 1) and 1 or 0
    self.animator:set("head", head + math.max(-dt * 2, math.min(dt * 2, goal - head)))

    if speed > 0.01 then
        angle = angle + speed * dt / radius
        self.position = center + vec3(math.cos(angle), 0, math.sin(angle)) * radius
        local dir = vec3(-math.sin(angle), 0, math.cos(angle))   -- along the circle (the model faces +Z)
        self.rotation = vec3(0, math.atan(dir.x, dir.z), 0)
    end
end
