-- Third-person follow camera: trails the target at a fixed offset and keeps looking at it, and never goes into or
-- behind walls: a sphere swept from the target out to where the camera wants to be (scene.spherecast) stops at the
-- first static thing in the way, and the camera comes in to there at once, then eases back out once it's clear.
-- Put it on an entity with a Camera component.

local target_name = "Player"
local offset = vec3(0, 3.5, 6.5)    -- above and behind (+Z is "behind" for this level)
local stiffness = 4.0               -- higher = snappier, lower = floatier
local look_height = 0.5             -- the point it looks at (and casts from), above the target's origin
local radius = 0.3                  -- how far the camera keeps from walls (covers the near plane's corners)
local min_distance = 0.05           -- never closer than this to the look point (a wall right behind the target wins)
local recover = 3.0                 -- m/s: how quickly it moves back out once nothing is in the way
local collide = true                -- false: plain follow, through walls
local target
local follow                        -- where it would be without walls (smoothed)
local distance                      -- how far out it is allowed, eased (nil = all the way)

local function pivot() return target.position + vec3(0, look_height, 0) end

-- Where the camera may be: `want` pulled in along the line from the look point so a sphere there touches nothing
local function clamp(want, dt)
    local p = pivot()
    local d = want - p
    local full = d:length()
    if not collide or full < 1e-4 then distance = nil; return want end
    local dir = d / full
    local hit = scene.spherecast(p, dir, full, radius, target, true)
    local allowed = hit and math.max(min_distance, hit.distance - 0.02) or full
    if distance == nil or allowed < distance then distance = allowed          -- blocked: in at once
    else distance = math.min(allowed, distance + recover * dt) end           -- clear: back out gently
    if distance >= full - 1e-3 then distance = nil; return want end
    return p + dir * distance
end

local function aim(self) self:look_at(pivot()) end

function on_create(self)
    target = scene.find(target_name)
    if target then
        follow = target.position + offset
        self.position = clamp(follow, 0)
        aim(self)
    else
        log("follow.lua: no entity named", target_name)
    end
end

function on_update(self, dt)
    if not (target and target:valid()) then
        target = scene.find(target_name)
        if not target then return end
        follow = target.position + offset
    end
    local want = target.position + offset
    follow = follow + (want - follow) * math.min(1, dt * stiffness)
    self.position = clamp(follow, dt)
    aim(self)
end
