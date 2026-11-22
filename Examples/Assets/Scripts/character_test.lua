-- Checks the character controller (CharacterControllerComponent, PhysicsWorld): falling and landing, walking, steps,
-- walls, slopes, jumping, a moving platform, pushing a box, raycasts and triggers seeing it, crouching under a low
-- ceiling, and a save game round trip. The test entity is the character; it builds its own course far from the level.
-- Run unattended:
--   MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/character_test.lua MDB_SAVE_DIR=<folder>
-- Prints "character_test: PASS ..." / "FAIL ..." lines and a summary.

local O = vec3(500, 0, 0)          -- the course's origin
local passed, failed = 0, 0
local step, elapsed = 1, 0.0
local started = false
local platform, crate, trigger_hits = nil, nil, 0
local mark = {}                    -- values remembered between steps
local loaded = false

local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("character_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end
local function f2(v) return string.format("%.2f", v) end
local function rel(self) return self.position - O end

local function box(name, pos, size, opts)
    local e = scene.spawn(name, O + pos)
    e:add_mesh{ geometry = "cube", color = vec3(0.5, 0.5, 0.55) }
    e.scale = size
    local o = opts or {}
    e:add_body{ type = o.type or "static", shape = "box", size = vec3(1, 1, 1), mass = o.mass, trigger = o.trigger }
    if o.rot then e.rotation = o.rot end
    return e
end

local function build()
    box("CT ground", vec3(0, -0.5, 30), vec3(60, 1, 90))
    -- lane A (z = 0): a 0.3 m step, then a 1 m wall
    box("CT step", vec3(5, 0.15, 0), vec3(2, 0.3, 4))
    box("CT wall", vec3(9.5, 0.5, 0), vec3(1, 1, 4))
    -- lane B (z = 10): a moving platform (kinematic, moved below)
    platform = box("CT platform", vec3(0, 0.5, 10), vec3(3, 0.2, 3), { type = "kinematic" })
    -- lane C (z = 20): a crate to push
    crate = box("CT crate", vec3(2, 0.5, 20), vec3(1, 1, 1), { type = "dynamic", mass = 5 })
    -- lane D (z = 30): a trigger to walk through
    box("CT trigger", vec3(3, 1, 30), vec3(1, 2, 4), { type = "kinematic", trigger = true })
    -- lane E (z = 40): a low ceiling (underside at 1.3 m)
    box("CT ceiling", vec3(3, 1.55, 40), vec3(2, 0.5, 4))
    -- lanes F and G (z = 50, 60): a walkable 25 degree ramp and a 60 degree one
    box("CT ramp 25", vec3(4, 0, 50), vec3(6, 0.2, 4), { rot = vec3(0, 0, math.rad(25)) })
    box("CT ramp 60", vec3(4, 0, 60), vec3(6, 0.2, 4), { rot = vec3(0, 0, math.rad(60)) })
end

local function go(self, x, z)          -- teleport to a lane, standing still
    self.position = O + vec3(x, 0.05, z)
    self.character.velocity = vec3(0, 0, 0)
    self.character:move(vec3(0, 0, 0))
end

-- { seconds, run at the start, check at the end }
local steps = {
    { 1.2, function(self) end, function(self)
        local ch = self.character
        check("falls and lands", ch.grounded and math.abs(rel(self).y) < 0.05, f2(rel(self).y))
        check("ground is the ground entity", ch.ground_entity ~= nil and ch.ground_entity.name == "CT ground",
            ch.ground_entity and ch.ground_entity.name)
        check("ground normal up", ch.ground_normal.y > 0.99)
        check("a raycast hits the character", (function()
            local hit = scene.raycast(self.position + vec3(0, 4, 0), vec3(0, -1, 0), 6)
            return hit ~= nil and hit.entity == self end)())
        check("a raycast can ignore it", (function()
            local hit = scene.raycast(self.position + vec3(0, 4, 0), vec3(0, -1, 0), 6, self)
            return hit ~= nil and hit.entity.name == "CT ground" end)())
        mark.still = { changes = 0 }                -- standing: watch the position frame by frame
    end },
    { 1.0, function(self) end, function(self)
        -- exactly the same every frame: drawing between two equal physics states used to round off by a float step
        -- now and then, which made still things count as moving (and their cached sun shadows flicker)
        check("standing still, the position doesn't change at all", mark.still.changes == 0, mark.still.changes)
        mark.still = nil
        mark.x = rel(self).x
        self.character:move(vec3(3, 0, 0))
    end },
    { 1.0, function(self) end, function(self)
        local dx = rel(self).x - mark.x
        check("walks at the wanted speed", math.abs(dx - 3) < 0.25, f2(dx))
        check("move is kept", math.abs(self.character.wanted.x - 3) < 1e-4)
    end },
    { 1.0, function(self) end, function(self)
        check("walks up a 0.3 m step", math.abs(rel(self).y - 0.3) < 0.05 and self.character.grounded, f2(rel(self).y))
    end },
    { 2.0, function(self) end, function(self)
        local x = rel(self).x
        check("a 1 m wall stops it", x > 8.4 and x < 8.7, f2(x))
        self.character:move(vec3(0, 0, 0))
        mark.y = rel(self).y
        check("jump from the ground", self.character:jump(5))
    end },
    { 0.1, function(self) end, function(self)
        check("in the air after a jump", not self.character.grounded)
        check("no jump in the air", not self.character:jump(5))
    end },
    { 0.4, function(self) end, function(self)
        check("jumps about 1.2 m", rel(self).y - mark.y > 1.0, f2(rel(self).y - mark.y))
    end },
    { 1.5, function(self) end, function(self)
        check("lands again", self.character.grounded)
        go(self, 0, 10)
    end },
    { 1.0, function(self) mark.moving = false end, function(self)
        check("stands on the platform", self.character.ground_entity == platform and math.abs(rel(self).y - 0.6) < 0.05,
            f2(rel(self).y))
        mark.px, mark.cx = platform.position.x, self.position.x
        mark.moving = true
    end },
    { 1.5, function(self) end, function(self)
        mark.moving = false
        local carried = self.position.x - mark.cx
        local moved = platform.position.x - mark.px
        check("rides the moving platform", moved > 2 and math.abs(carried - moved) < 0.15, f2(carried) .. " / " .. f2(moved))
        check("ground velocity is the platform's", math.abs(self.character.ground_velocity.x - 1.5) < 0.3,
            f2(self.character.ground_velocity.x))
        go(self, 0, 20)
        mark.crate = crate.position.x
    end },
    { 0.6, function(self) end, function(self) self.character:move(vec3(2, 0, 0)) end },
    { 2.5, function(self) end, function(self)
        check("pushes a crate", crate.position.x - mark.crate > 2, f2(crate.position.x - mark.crate))
        go(self, 0, 30)
    end },
    { 0.6, function(self) self.character:move(vec3(3, 0, 0)) end, function(self) end },
    { 1.6, function(self) end, function(self)
        check("walks through a trigger", trigger_hits > 0, trigger_hits)
        go(self, 0, 40)
    end },
    { 0.6, function(self) end, function(self)
        check("crouches", self.character:set_size(1.0) and math.abs(self.character.height - 1.0) < 1e-4)
        check("crouching keeps the feet", math.abs(self.character.center.y - 0.5) < 1e-3, f2(self.character.center.y))
        self.character:move(vec3(3, 0, 0))
    end },
    { 1.0, function(self) end, function(self)
        self.character:move(vec3(0, 0, 0))
        check("walks under a low ceiling crouched", math.abs(rel(self).x - 3) < 0.4, f2(rel(self).x))
        check("can't stand up under it", not self.character:set_size(1.8) and math.abs(self.character.height - 1.0) < 1e-4)
        self.character:move(vec3(3, 0, 0))
    end },
    { 1.0, function(self) end, function(self)
        check("stands up in the open", self.character:set_size(1.8))
        go(self, 0, 50)
    end },
    { 0.5, function(self) self.character:move(vec3(3, 0, 0)) end, function(self) end },
    { 2.0, function(self) end, function(self)
        check("climbs a 25 degree ramp", rel(self).y > 0.7, f2(rel(self).y))
        go(self, 0, 60)
    end },
    { 0.5, function(self) self.character:move(vec3(3, 0, 0)) end, function(self) end },
    { 2.0, function(self) end, function(self)
        check("can't climb a 60 degree slope", rel(self).y < 0.6, f2(rel(self).y))
        self.character:move(vec3(0, 0, 0))
        self.character.max_slope = 40
        saves.save("character_test")
        saves.load("character_test")
    end },
}

function on_save(self) return { step = step, passed = passed, failed = failed } end
function on_load(self, data) step, passed, failed = data.step, data.passed, data.failed; loaded = true end

function on_create(self)
    if not self.character then self:add_character{ center = vec3(0, 0.9, 0) } end
end

function on_trigger_enter(self, other)
    if other.name == "CT trigger" then trigger_hits = trigger_hits + 1 end
end

function on_update(self, dt)
    if loaded then                     -- after the save game came back
        loaded = false
        local ch = self.character
        check("the controller is saved", ch ~= nil and math.abs(ch.radius - 0.4) < 1e-4 and math.abs(ch.max_slope - 40) < 1e-4,
            ch and ch.max_slope)
        saves.delete("character_test")
        log(string.format("character_test: %d passed, %d failed", passed, failed))
        step = #steps + 2
        return
    end
    if step > #steps then return end
    if step == 1 and not started and not mark.built then  -- the course (a save game brings it back by itself)
        build()
        self.position = O + vec3(0, 1, 0)
        mark.built = true
    end
    if mark.still then
        local p = self.position
        local k = string.format("%.9g %.9g %.9g", p.x, p.y, p.z)
        if mark.still.last and mark.still.last ~= k then mark.still.changes = mark.still.changes + 1 end
        mark.still.last = k
    end
    if mark.moving and platform then platform.position = platform.position + vec3(1.5 * dt, 0, 0) end
    if not started then steps[step][2](self); started = true end
    elapsed = elapsed + dt
    if elapsed >= steps[step][1] then
        steps[step][3](self)
        step, elapsed, started = step + 1, 0.0, false
    end
end
