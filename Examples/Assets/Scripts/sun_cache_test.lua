-- Checks the cached far sun cascades (ShadowSettings::SunCacheFrom, default 2): with a still camera the far cascades
-- keep their still casters (redrawn only every few frames), a big camera move re-centres and redraws them at once, and
-- something moving inside a far cascade is drawn over a copy of them (the overlay pass) without redrawing them.
-- Run unattended: MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/sun_cache_test.lua
-- Prints "sun_cache_test: PASS ..." / "FAIL ..." lines and a summary.

local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("sun_cache_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail and ("  (" .. detail .. ")") or ""))
end

local cam, mover
local frame, phase = 0, 1
local samples = {}
local function sample() local s = scene.render_stats(); samples[#samples + 1] = s.sun_cascades_drawn; return s end
local function summary() local t, lo, hi = 0, 99, 0; for _, v in ipairs(samples) do t = t + v; lo = math.min(lo, v); hi = math.max(hi, v) end; return t, lo, hi end

function on_update(self, dt)
    frame = frame + 1
    if phase == 1 then
        cam = scene.spawn("Sun cache camera", vec3(0, 6, 20))
        cam:look_at(vec3(0, 0, 0))
        cam:add_camera({ primary = true })
        phase, frame = 2, 0
    elseif phase == 2 then
        if frame > 10 then sample() end                    -- settled
        if frame == 26 then
            local s = scene.render_stats()
            local total, lo, hi = summary()
            check("4 cascades in use", s.sun_cascades == 4, tostring(s.sun_cascades))
            check("a still camera keeps the far cascades most frames", lo == 2 and total < 4 * #samples - 8,
                string.format("%d redraws in %d frames, %d..%d a frame", total, #samples, lo, hi))
            -- A big move: the far cascades no longer cover the view, so they redraw at once
            cam.position = vec3(300, 6, 20)
            cam:look_at(vec3(300, 0, 0))
            phase, frame, samples = 3, 0, {}
        end
    elseif phase == 3 then
        if frame == 1 then
            local s = sample()
            check("a big camera move redraws every cascade", s.sun_cascades_drawn == 4, tostring(s.sun_cascades_drawn))
            -- Something moving about 45 m ahead (in the last cascade): drawn in the overlay, the still casters stay cached
            mover = scene.spawn("Sun cache mover", vec3(300, 1, -25))
            mover:add_mesh({ geometry = "cube" })
            phase, frame, samples = 4, 0, {}
        end
    elseif phase == 4 then
        mover.position = vec3(300 + math.sin(frame * 0.2) * 3, 1, -25)
        if frame > 6 then sample() end
        if frame == 22 then
            local total, lo, hi = summary()
            check("something moving in a far cascade leaves its still casters cached", lo == 2 and total < 4 * #samples - 8,
                string.format("%d redraws in %d frames, %d..%d a frame", total, #samples, lo, hi))
            log(string.format("sun_cache_test: %d passed, %d failed", passed, failed))
            phase = 5
        end
    end
end
