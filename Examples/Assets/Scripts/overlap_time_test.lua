-- Checks scene.overlap_sphere / scene.overlap_capsule and scene.time_scale / scene.hitstop / scene.real_dt.
-- Builds its own little course far from the level's content.
-- Run unattended:
--   MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/overlap_time_test.lua
-- Prints "overlap_time_test: PASS ..." / "FAIL ..." lines and a summary.

local O = vec3(500, 0, 500)
local passed, failed = 0, 0
local step, real_elapsed, started = 1, 0.0, false
local ground, ball, crate, sensor, hero
local t0, r0                                -- scene.time() / scene.real_time() when a timed step began
local min_dt_in_stop, max_dt_in_stop

local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("overlap_time_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end
local function f3(v) return string.format("%.3f", v) end
local function names(hits)
    local t = {}
    for _, h in ipairs(hits) do t[#t + 1] = h.entity.name end
    return table.concat(t, ",")
end
local function has(hits, e)
    for _, h in ipairs(hits) do if h.entity.uid == e.uid then return h end end
    return nil
end

local function body(name, pos, shape, size, kind, trigger)
    local e = scene.spawn(name, O + pos)
    e:add_body{ type = kind, shape = shape, size = size, trigger = trigger or false }
    return e
end

local steps = {
    -- 1: build the course
    { 0.2, function()
        ground = body("OT ground", vec3(0, -0.5, 0), "box", vec3(20, 1, 20), "static")
        ball   = body("OT ball", vec3(0, 1, 0), "sphere", 1.0, "kinematic")             -- 0.5 m radius at y = 1
        crate  = body("OT crate", vec3(4, 1, 0), "box", vec3(1, 1, 1), "kinematic")
        sensor = body("OT sensor", vec3(-4, 1, 0), "box", vec3(1, 1, 1), "static", true)
        hero = scene.spawn("OT hero", O + vec3(0, 0, 4))
        hero:add_character{ radius = 0.4, height = 1.8, center = vec3(0, 0.9, 0) }
    end, function()
        local h = scene.overlap_sphere(O + vec3(0, 1, 0), 0.1, { static = false })
        check("a sphere inside the ball finds it", has(h, ball) ~= nil, names(h))
        check("one hit per entity", #h == 1, names(h))
        local d = has(h, ball)                  -- the deepest contact is on the ball's surface, 0.5 from its centre
        check("inside: distance to the surface contact", d and math.abs(d.distance - 0.5) < 0.05, d and f3(d.distance))
        local e = has(scene.overlap_sphere(O + vec3(0, 1.6, 0), 0.2), ball)
        check("touching from outside: distance to the contact", e and math.abs(e.distance - 0.1) < 0.05, e and f3(e.distance))

        check("nothing out in the open", #scene.overlap_sphere(O + vec3(0, 5, -6), 0.5) == 0)
        local g = scene.overlap_sphere(O + vec3(8, 0.2, 8), 0.5)
        check("statics are found by default", has(g, ground) ~= nil, names(g))
        check("static = false leaves the level out", #scene.overlap_sphere(O + vec3(8, 0.2, 8), 0.5, { static = false }) == 0)

        check("triggers are skipped by default", has(scene.overlap_sphere(O + vec3(-4, 1, 0), 0.3), sensor) == nil)
        check("triggers = true finds them", has(scene.overlap_sphere(O + vec3(-4, 1, 0), 0.3, { triggers = true }), sensor) ~= nil)

        check("ignore skips an entity", has(scene.overlap_sphere(O + vec3(0, 1, 0), 0.1, { ignore = ball }), ball) == nil)

        local c = scene.overlap_sphere(O + vec3(0, 1, 4), 0.2, { static = false })
        check("a character controller is found", has(c, hero) ~= nil, names(c))
        check("ignore skips a character", has(scene.overlap_sphere(O + vec3(0, 1, 4), 0.2, { ignore = hero }), hero) == nil)

        -- a sword sweep from beside the ball through to the crate: both, nearest (to the segment) first
        local s = scene.overlap_capsule(O + vec3(-0.2, 1.8, 0), O + vec3(4.0, 1.8, 0), 0.4, { static = false })
        check("a capsule finds both", has(s, ball) ~= nil and has(s, crate) ~= nil, names(s))
        local sorted = true
        for i = 2, #s do if s[i].distance < s[i - 1].distance then sorted = false end end
        check("nearest first", sorted, names(s))
        check("a capsule above everything misses", #scene.overlap_capsule(O + vec3(-2, 4, 0), O + vec3(6, 4, 0), 0.3) == 0)
        check("a == b works as a sphere", has(scene.overlap_capsule(O + vec3(0, 1, 0), O + vec3(0, 1, 0), 0.1), ball) ~= nil)
        local n = has(scene.overlap_sphere(O + vec3(0, 1.6, 0), 0.2), ball)
        check("normal points from the centre out to the contact", n and n.normal.y < -0.9, n and tostring(n.normal))
    end },

    -- 2: half speed
    { 0.5, function()
        check("time_scale is 1 at the start", scene.time_scale() == 1)
        check("time_scale clamps to 10", scene.time_scale(25) == 10)
        scene.time_scale(0.5)
        t0, r0 = scene.time(), scene.real_time()
    end, function()
        local game, real = scene.time() - t0, scene.real_time() - r0
        check("time_scale 0.5 runs game time at half speed", math.abs(game / real - 0.5) < 0.05, f3(game) .. " / " .. f3(real))
        scene.time_scale(1)
    end },

    -- 3: a hit-stop freezes for its real seconds, then ends by itself
    { 0.2, function()
        min_dt_in_stop, max_dt_in_stop = 1, 0
        scene.hitstop(0.15, 0)
        scene.hitstop(0.05, 0.5)             -- shorter and faster: the first one wins
        t0, r0 = scene.time(), scene.real_time()
    end, function()
        local game = scene.time() - t0
        -- 0.15 s frozen, then about 0.05 s at full speed
        check("hitstop froze the game", game < 0.09, f3(game))
        check("hitstop's dt was 0 meanwhile", max_dt_in_stop < 1e-6, f3(max_dt_in_stop))
        check("real_dt kept running", scene.real_dt() > 0)
        t0, r0 = scene.time(), scene.real_time()
    end, function(dt)
        -- from the frame after the call (the frame that calls it has already run at full speed)
        if scene.real_time() > r0 and scene.real_time() - r0 < 0.12 then max_dt_in_stop = math.max(max_dt_in_stop, dt) end
    end },

    -- 4: and afterwards the game runs normally again
    { 0.3, function() t0, r0 = scene.time(), scene.real_time() end, function()
        local game, real = scene.time() - t0, scene.real_time() - r0
        check("after the hitstop the game is back to full speed", math.abs(game / real - 1) < 0.05, f3(game) .. " / " .. f3(real))
        input.simulate_mouse_delta(5, -3)
    end },

    -- 5: mouse look: a simulated delta reaches scripts; without it (and without a lock) there is none
    { 0.1, function() end, function()
        local dx, dy = input.mouse_delta()
        check("simulated mouse delta", dx == 5 and dy == -3, tostring(dx) .. ", " .. tostring(dy))
        input.simulate_mouse_delta()
    end },
    { 0.1, function() end, function()
        local dx, dy = input.mouse_delta()
        check("no delta while the cursor is free", dx == 0 and dy == 0, tostring(dx) .. ", " .. tostring(dy))
        check("not locked unless asked", input.mouse_locked() == false)
        log(string.format("overlap_time_test: %d passed, %d failed", passed, failed))
    end },
}

function on_update(self, dt)
    if step > #steps then return end
    local s = steps[step]
    if not started then s[2](); started = true end
    if s[4] then s[4](dt) end
    real_elapsed = real_elapsed + scene.real_dt()      -- steps wait in real time: the test changes game time
    if real_elapsed >= s[1] then
        s[3]()
        step, real_elapsed, started = step + 1, 0.0, false
    end
end
