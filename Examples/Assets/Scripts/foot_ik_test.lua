-- Checks foot placement (AnimatorComponent::Feet, Scene.cpp PlaceFeet): feet follow the ground up a step and down
-- a drop (with the pelvis lowered), turn with a slope, keep the animation where the ground is flat, smooth over
-- time, obey weight and the ray range, fade out while a character controller is in the air, and keep working on a
-- walking clip. Foxes stand side by side on their own course far from the level; a reference fox on flat ground
-- with foot placement off gives the animated positions to compare with.
-- Run unattended: MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/foot_ik_test.lua
-- Prints "foot_ik_test: PASS ..." / "FAIL ..." lines and a summary.

local O = vec3(700, 0, 0)          -- the course's origin
local passed, failed = 0, 0
local stage, stage_t, frames = 0, 0, 0
local FEET = { "b_LeftFoot01_017", "b_RightFoot01_021", "b_LeftHand_011", "b_RightHand_08" }   -- hind, hind, front, front
local HIND, FRONT = { 1, 2 }, { 3, 4 }
local foxes = {}                   -- name -> entity
local paw = {}                     -- the reference fox's paws: local offsets from its origin
local mark = {}

local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("foot_ik_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end
local function f3(v) return string.format("%.3f", v) end
local function next_stage() stage = stage + 1; stage_t = 0; frames = 0 end

local function box(name, pos, size, rot)
    local e = scene.spawn(name, O + pos)
    e:add_mesh{ geometry = "cube", color = vec3(0.45, 0.45, 0.5) }
    e.scale = size
    if rot then e.rotation = rot end
    e:add_body{ type = "static", shape = "box", size = vec3(1, 1, 1) }
    return e
end

local function fox(name, x, opts)
    opts = opts or {}
    local e = scene.spawn(name, O + vec3(x, opts.y or 0, 0))
    e:add_mesh{ geometry = "model:Models/Fox.glb", fit = 1.6 }
    e:add_animator{ clip = opts.clip or "" }
    if opts.feet ~= false then
        e.animator:set_feet{ bones = FEET, smoothing = opts.smoothing or 0 }
    end
    foxes[name] = e
    return e
end

-- A paw's height relative to the same paw of the reference fox (same clip, same time, flat ground)
local function rel_y(name, i, ref)
    local a = foxes[name].animator:bone_position(FEET[i])
    local b = foxes[ref or "Ref"].animator:bone_position(FEET[i])
    return a.y - b.y
end
local function rel_xz(name, i, ref)
    local a = foxes[name].animator:bone_position(FEET[i]) - foxes[name].position
    local b = foxes[ref or "Ref"].animator:bone_position(FEET[i]) - foxes[ref or "Ref"].position
    return math.sqrt((a.x - b.x) ^ 2 + (a.z - b.z) ^ 2)
end
local function all(list, fn) for _, i in ipairs(list) do if not fn(i) then return false end end return true end
local function worst(list, fn) local m = 0 for _, i in ipairs(list) do m = math.max(m, math.abs(fn(i))) end return m end

function on_create(self)
    box("FT ground", vec3(10, -0.5, 0), vec3(40, 1, 10))
    fox("Ref", 0, { feet = false })
    fox("Flat", 3)
    fox("FlatRef2", 30, { feet = false })      -- far apart: nothing under it but the ground
end

function on_update(self, dt)
    stage_t = stage_t + dt
    frames = frames + 1
    if stage == 0 and frames >= 3 then
        -- Measure the rest pose so the steps can be built under the right paws
        local p = foxes.Ref.position
        for i = 1, 4 do paw[i] = foxes.Ref.animator:bone_position(FEET[i]) - p end
        log(string.format("foot_ik_test: paws hind z %.2f / %.2f, front z %.2f / %.2f, y %.3f %.3f",
            paw[1].z, paw[2].z, paw[3].z, paw[4].z, paw[1].y, paw[3].y))
        check("front paws ahead of hind paws", paw[3].z > paw[1].z + 0.2 and paw[4].z > paw[2].z + 0.2)
        check("flat ground: feet stay as animated", worst({ 1, 2, 3, 4 }, function(i) return rel_y("Flat", i) end) < 0.002,
            f3(worst({ 1, 2, 3, 4 }, function(i) return rel_y("Flat", i) end)) .. " m")
        local f = foxes.Flat.animator:feet()
        check("flat ground: rays hit, no lift, no pelvis drop",
            f.ground[1] and f.ground[4] and math.abs(f.lift[1]) < 0.002 and math.abs(f.pelvis_drop) < 0.002,
            "lift " .. f3(f.lift[1]) .. ", drop " .. f3(f.pelvis_drop))
        check("feet() reports the settings", #f.bones == 4 and f.bones[3] == FEET[3] and f.enabled and f.ray_up == 0.5)

        local mid = (math.max(paw[1].z, paw[2].z) + math.min(paw[3].z, paw[4].z)) * 0.5   -- between hind and front
        local front_len = 1.0
        -- Lane "Up": a 0.1 m step under the front paws
        fox("Up", 6)
        box("FT up", vec3(6, 0.05, mid + front_len * 0.5), vec3(1.2, 0.1, front_len))
        -- Lane "Down": standing on a 0.15 m block with the front paws out over the ground (0.15 m lower)
        fox("Down", 9, { y = 0.15 })
        box("FT down hind", vec3(9, 0.075, mid - 1.0), vec3(1.2, 0.15, 2.0))
        -- Lane "Slope": the front paws on a 15 degree ramp rising forwards (its top surface at y 0 under the paws)
        mark.slope = math.rad(15)
        fox("Slope", 12)
        local fz = (paw[3].z + paw[4].z) * 0.5
        box("FT slope", vec3(12, -0.5 / math.cos(mark.slope), fz), vec3(1.2, 1, 2), vec3(-mark.slope, 0, 0))
        -- Lane "Ledge": on a 0.8 m block with the front paws out over the ground, further down than ray_down (0.5)
        fox("Ledge", 15, { y = 0.8 })
        box("FT ledge", vec3(15, 0.4, mid - 1.0), vec3(1.2, 0.8, 2.0))
        -- Lane "Smooth": the same step as "Up" with smoothing 10 /s
        fox("Smooth", 18, { smoothing = 10 })
        box("FT smooth", vec3(18, 0.05, mid + front_len * 0.5), vec3(1.2, 0.1, front_len))
        -- Lane "Walk": walking over the step, against a walking reference on flat ground
        fox("WalkRef", 24, { feet = false, clip = "Walk" })
        fox("Walk", 21, { clip = "Walk" })
        box("FT walk", vec3(21, 0.05, mid + 1.0), vec3(1.2, 0.1, 2.0))
        -- Lane "Char": a fox that is a character controller (origin at the feet), to jump
        local c = fox("Char", 27)
        c:add_character{ radius = 0.3, height = 0.9, center = vec3(0, 0.45, 0) }
        next_stage()
    elseif stage == 1 and frames == 1 then
        -- Smoothing: one frame after the step appeared the smoothed foot is on its way, not there
        local f = foxes.Smooth.animator:feet()
        mark.smooth_first = f.lift[3]
        next_stage()
    elseif stage == 2 and stage_t > 0.8 then
        local up = function(i) return rel_y("Up", i) end
        check("step up: front paws rise by the step", all(FRONT, function(i) return math.abs(up(i) - 0.1) < 0.006 end),
            f3(up(3)) .. " / " .. f3(up(4)) .. " m")
        check("step up: hind paws stay", worst(HIND, up) < 0.004, f3(worst(HIND, up)) .. " m")
        check("step up: paws stay over their animated spot", worst(FRONT, function(i) return rel_xz("Up", i) end) < 0.03,
            f3(worst(FRONT, function(i) return rel_xz("Up", i) end)) .. " m")
        check("step up: no pelvis drop", math.abs(foxes.Up.animator:feet().pelvis_drop) < 0.002)

        local down = function(i) return rel_y("Down", i) - 0.15 end
        local fd = foxes.Down.animator:feet()
        check("drop: pelvis lowered by the drop", math.abs(fd.pelvis_drop + 0.15) < 0.004, f3(fd.pelvis_drop) .. " m")
        check("drop: front paws go down to the ground", all(FRONT, function(i) return math.abs(down(i) + 0.15) < 0.006 end),
            f3(down(3)) .. " / " .. f3(down(4)) .. " m")
        check("drop: hind paws still reach their ground", worst(HIND, down) < 0.006, f3(worst(HIND, down)) .. " m")
        local hip = foxes.Down.animator:bone_position("b_Hip_01").y - foxes.Ref.animator:bone_position("b_Hip_01").y - 0.15
        check("drop: hip lower", math.abs(hip + 0.15) < 0.004, f3(hip) .. " m")

        local fs = foxes.Slope.animator:feet()
        local tilt = math.acos(math.min(1, fs.normal[3].y))
        check("slope: ground normal under the front paw", math.abs(tilt - mark.slope) < math.rad(1),
            string.format("%.1f deg", math.deg(tilt)))
        local a = foxes.Slope.animator:bone_forward(FEET[3])
        local b = foxes.Ref.animator:bone_forward(FEET[3])
        local turned = math.deg(math.acos(math.max(-1, math.min(1, a:dot(b)))))
        check("slope: front paw turned with the slope", math.abs(turned - 15) < 2, string.format("%.1f deg", turned))
        local bh = foxes.Slope.animator:bone_forward(FEET[1])
        local turned_hind = math.deg(math.acos(math.max(-1, math.min(1, bh:dot(foxes.Ref.animator:bone_forward(FEET[1]))))))
        check("slope: hind paw on flat ground not turned", turned_hind < 0.5, string.format("%.2f deg", turned_hind))

        local fh = foxes.Ledge.animator:feet()
        check("out of ray range: ignored", not fh.ground[3] and worst(FRONT, function(i) return rel_y("Ledge", i) - 0.8 end) < 0.002,
            "lift " .. f3(fh.lift[3]))

        local fsm = foxes.Smooth.animator:feet()
        check("smoothing: on its way after one frame, there later",
            mark.smooth_first > 1e-5 and mark.smooth_first < 0.09 and math.abs(fsm.lift[3] - 0.1) < 0.003,
            "first " .. f3(mark.smooth_first) .. ", now " .. f3(fsm.lift[3]))

        local walk = function(i) return rel_y("Walk", i, "WalkRef") end
        check("walking: front paws lifted by the step", all(FRONT, function(i) return math.abs(walk(i) - 0.1) < 0.012 end),
            f3(walk(3)) .. " / " .. f3(walk(4)) .. " m")
        check("walking: hind paws as animated", worst(HIND, walk) < 0.006, f3(worst(HIND, walk)) .. " m")

        check("character on the ground: full weight", math.abs(foxes.Char.animator:feet().fade - 1) < 1e-3)
        foxes.Up.animator:set_feet{ weight = 0 }
        foxes.Char.character:jump(4)
        next_stage()
    elseif stage == 3 and stage_t > 0.25 then
        check("weight 0: animated pose again", worst(FRONT, function(i) return rel_y("Up", i) end) < 0.002)
        local c = foxes.Char
        check("character in the air: faded out", not c.character.grounded and c.animator:feet().fade < 0.01,
            "fade " .. f3(c.animator:feet().fade) .. ", y " .. f3(c.position.y - O.y))
        foxes.Up.animator:set_feet{ weight = 1, enabled = false }
        next_stage()
    elseif stage == 4 and foxes.Char.character.grounded and stage_t > 0.3 then
        check("disabled: animated pose", worst(FRONT, function(i) return rel_y("Up", i) end) < 0.002)
        mark.landed = stage_t
        next_stage()
    elseif stage == 5 and stage_t > 0.3 then
        check("character landed: fades back in", foxes.Char.animator:feet().fade > 0.99, f3(foxes.Char.animator:feet().fade))
        check("bad bones are an error", not pcall(function() foxes.Up.animator:set_feet{ bones = { 1, 2 } } end))
        log(string.format("foot_ik_test: %d passed, %d failed", passed, failed))
        next_stage()
    elseif stage == 6 and stage_t > 0.1 then
        scene.quit()
    end
    if stage < 6 and stage_t > 6 then
        check("stage " .. stage .. " finished in time", false)
        log(string.format("foot_ik_test: %d passed, %d failed", passed, failed))
        stage = 6
    end
end
