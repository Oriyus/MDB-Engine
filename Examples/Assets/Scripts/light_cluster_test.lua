-- Checks clustered point and spot lights: the automatic range, the range setting, that every light is counted, that
-- lights out of view aren't in any cluster, that short ranges keep each cluster's list short (long ones fill them),
-- that the GPU binning agrees with the CPU one, that a spot light's cone is what gets binned, cookies (spots and points,
-- flipbooks) and IES profiles.
-- Run unattended: MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/light_cluster_test.lua
-- Prints "light_cluster_test: PASS ..." / "FAIL ..." lines and a summary.

local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("light_cluster_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail and ("  (" .. detail .. ")") or ""))
end
local function stats() return scene.render_stats() end
local function describe(s) return string.format("%d lights, %d seen, %d max per cluster", s.lights, s.lights_visible, s.lights_per_cluster) end

local lamps = {}
local far, spot, spotLight, cam
local base = 0
local gpu, before
local wait = 0
local step = 1

-- Each step returns true when it is done; a step after a change waits a few frames (stats are of the last frame, and
-- the GPU binning's are read back three frames after it ran)
local steps = {
    function()
        check("binning on the GPU by default", scene.light_binning() == "gpu", scene.light_binning())
        base = stats().lights
        local probe = scene.spawn("Probe", vec3(0, 1, 0))
        local l = probe:add_light({ color = vec3(1, 1, 1), intensity = 1 })
        check("automatic range", math.abs(l.reach - 70.35) < 0.1 and l.range == 0, string.format("%.2f", l.reach))
        l.range = 6
        check("range set", l.range == 6 and l.reach == 6)
        l.intensity = 4
        l.range = 0
        check("automatic range grows with brightness", l.reach > 100 - 1e-3, string.format("%.2f", l.reach))
        check("a point light has no cone", l.spot_angle == 0)
        l.spot_angle = 400
        check("cone angle clamped", l.spot_angle == 179, tostring(l.spot_angle))
        l.spot_angle = 40
        l.cookie = "Textures/window.png"
        check("a cookie is set", l.cookie == "Textures/window.png", l.cookie)
        local ok = pcall(function() l.cookie = "Textures/no_such_image.png" end)
        check("a missing cookie image is an error", not ok and l.cookie == "Textures/window.png")
        l.cookie = ""
        check("a cookie is cleared", l.cookie == "")
        -- Point lights take cookies too (a panorama), animated ones as a grid of frames, and IES profiles
        l.spot_angle = 0
        l.cookie = "Textures/lantern_cookie.png"
        check("a point light takes a cookie", l.cookie == "Textures/lantern_cookie.png", l.cookie)
        check("a still cookie by default", l.cookie_columns == 1 and l.cookie_rows == 1 and l.cookie_fps == 0 and l.cookie_frame == 0)
        l.cookie_columns, l.cookie_rows, l.cookie_fps, l.cookie_frame = 100, 2, 12, 3
        check("flipbook settings (columns clamped)", l.cookie_columns == 64 and l.cookie_rows == 2 and l.cookie_fps == 12 and l.cookie_frame == 3,
            string.format("%d x %d, %g fps, frame %d", l.cookie_columns, l.cookie_rows, l.cookie_fps, l.cookie_frame))
        check("no profile by default", l.profile == "")
        l.profile = "Lights/downlight.ies"
        check("an IES profile is set", l.profile == "Lights/downlight.ies", l.profile)
        ok = pcall(function() l.profile = "Lights/no_such_profile.ies" end)
        check("a missing profile is an error", not ok and l.profile == "Lights/downlight.ies")
        local probe2 = scene.spawn("Probe 2", vec3(0, 3, 0))
        local made = probe2:add_light({ cookie = "Textures/fan_cookie.png", cookie_columns = 4, cookie_rows = 2,
            cookie_fps = 10, profile = "Lights/downlight.ies" })
        check("add_light takes cookie, flipbook and profile", made.cookie == "Textures/fan_cookie.png" and made.cookie_columns == 4 and
            made.cookie_rows == 2 and made.cookie_fps == 10 and made.profile == "Lights/downlight.ies")
        l.profile = ""
        check("a profile is cleared", l.profile == "")
        -- A light's size (softer shadows, wider highlights) and its contact shadows
        check("a point by default, contact shadows on", l.radius == 0 and l.contact_shadows == true)
        l.radius = 0.2
        l.contact_shadows = false
        check("size and contact shadows set", math.abs(l.radius - 0.2) < 1e-6 and l.contact_shadows == false)
        l.radius = 50
        check("size clamped", l.radius == 5, tostring(l.radius))
        -- Area lights: a tube or a rect
        check("a sphere by default", l.shape == "sphere")
        l.shape = "tube"
        l.width = 2
        check("a tube", l.shape == "tube" and l.width == 2, l.shape)
        local okShape = pcall(function() l.shape = "triangle" end)
        check("an unknown shape is an error", not okShape and l.shape == "tube")
        local panel = probe2:add_light({ shape = "rect", width = 1.5, height = 0.5 })
        check("add_light takes shape, width and height", panel.shape == "rect" and panel.width == 1.5 and panel.height == 0.5)
        l.shape = "sphere"
        local sized = probe2:add_light({ radius = 0.3, contact_shadows = false })
        check("add_light takes radius and contact_shadows", math.abs(sized.radius - 0.3) < 1e-6 and sized.contact_shadows == false)
        probe:destroy()
        probe2:destroy()
        -- A field of 144 small lamps over the floor, and one far out of sight
        for x = 0, 11 do
            for z = 0, 11 do
                local e = scene.spawn("Lamp", vec3(-8.25 + x * 1.5, 0.4, -8.25 + z * 1.5))
                lamps[#lamps + 1] = e:add_light({ color = vec3(1, 0.6, 0.3), intensity = 1, range = 2.5, shadows = (x + z) % 7 == 0 })
            end
        end
        far = scene.spawn("Far lamp", vec3(0, 2000, 0)):add_light({ range = 5 })
        return true
    end,
    function()
        local s = stats()
        check("every light counted", s.lights == base + 145, describe(s))
        check("the far light is in no cluster", s.lights_visible <= base + 144 and s.lights_visible >= base + 72, describe(s))
        check("short ranges keep clusters short", s.lights_per_cluster > 0 and s.lights_per_cluster <= 40, describe(s))
        gpu = s
        scene.light_binning("cpu")
        return true
    end,
    function()
        local s = stats()
        check("CPU binning selected", scene.light_binning() == "cpu")
        -- (close, not equal: the GPU tests each tile's side planes exactly, the CPU a rectangle round each slice of the
        -- sphere, which also takes in a few lamps just off the edge of the screen)
        check("GPU and CPU see the same lights", math.abs(s.lights_visible - gpu.lights_visible) <= math.max(3, s.lights_visible // 20),
            "gpu " .. describe(gpu) .. " / cpu " .. describe(s))
        check("GPU and CPU fill clusters alike", math.abs(s.lights_per_cluster - gpu.lights_per_cluster) <= math.max(3, s.lights_per_cluster // 3),
            "gpu " .. gpu.lights_per_cluster .. " / cpu " .. s.lights_per_cluster)
        scene.light_binning("gpu")
        for _, l in ipairs(lamps) do l.range = 200 end
        return true
    end,
    function()
        local s = stats()
        -- (lamps holding one of the 4 shadow slots are shaded directly, not through the clusters)
        check("long ranges fill every cluster", s.lights_per_cluster >= 140, describe(s))
        for _, l in ipairs(lamps) do l.range = 2.5 end
        return true
    end,
    function()
        local s = stats()
        check("ranges change back", s.lights_per_cluster <= 40, describe(s))
        -- A camera of its own for the spot checks (the view the clusters are built for)
        cam = scene.spawn("Spot test camera", vec3(0, 3, 12))
        cam:look_at(vec3(0, 0.5, 0))
        cam:add_camera({ primary = true })
        return true
    end,
    function()
        -- A spot light 1 m behind the camera, aimed backwards: its range sphere holds the camera, its cone doesn't
        before = stats()
        local origin, dir = cam.world_position, cam.forward
        spot = scene.spawn("Spot", origin - dir * 1.0)
        spot:look_at(origin - dir * 10)
        spotLight = spot:add_light({ color = vec3(1, 1, 1), intensity = 3, range = 30, spot_angle = 30, shadows = false })
        check("spot settings", spotLight.spot_angle == 30 and math.abs(spotLight.spot_blend - 0.15) < 1e-6)
        return true
    end,
    function()
        local s = stats()
        check("a spot aimed away is in no cluster", s.lights == before.lights + 1 and s.lights_visible == before.lights_visible, describe(s))
        spot:look_at(cam.world_position + cam.forward * 10)
        return true
    end,
    function()
        local s = stats()
        check("aimed into the view it is", s.lights_visible == before.lights_visible + 1, describe(s))
        spotLight.spot_angle = 0                 -- a point light again: the sphere holds the camera
        spot:look_at(cam.world_position - cam.forward * 10)          -- aimed away again
        return true
    end,
    function()
        local s = stats()
        check("as a point light it is seen whichever way it faces", s.lights_visible == before.lights_visible + 1, describe(s))
        log(string.format("light_cluster_test: %d passed, %d failed", passed, failed))
        return true
    end,
}

function on_update(self, dt)
    if wait > 0 then wait = wait - 1; return end
    local f = steps[step]
    if f and f() then step = step + 1; wait = 8 end
end
