-- Checks the nav grid against the Tactics test level (File > Example Levels > Tactics Test).
-- Run unattended: MDB_DEMO=tactics MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/nav_test.lua
-- Prints "nav_test: PASS ..." / "FAIL ..." lines and a summary to the log.

local frame = 0
local passed, failed = 0, 0

local function check(name, ok, detail)
    if ok then passed = passed + 1; log("nav_test: PASS " .. name)
    else failed = failed + 1; log("nav_test: FAIL " .. name .. (detail and ("  (" .. detail .. ")") or "")) end
end

local function near(a, b, eps) return math.abs(a - b) <= (eps or 0.02) end

local function path_length(p)
    local l = 0
    for i = 2, #p.points do l = l + (p.points[i] - p.points[i - 1]):length() end
    return l
end

local function run()
    check("baked", nav.baked() or nav.bake{ cell_size = 1 } > 0)

    -- two layers in one column: the ground under the bridge and the deck
    local g = nav.cell_at(vec3(0.5, -1, -9.5))
    local b = nav.cell_at(vec3(0.5, 1.5, -9.5))
    check("ground under the bridge", g ~= nil and near(nav.cell_position(g).y, -1))
    check("bridge deck", b ~= nil and near(nav.cell_position(b).y, 1.5))
    check("two different cells", g ~= nil and b ~= nil and g ~= b)

    -- a crate takes its cell; the top of a crate is not reachable
    check("crate cell not walkable", nav.cell_at(vec3(2.5, -1, 8.5)) == nil)
    local top = nav.find_path(vec3(1.5, -1, 9.5), vec3(2.5, 0, 8.5), { closest = false })
    check("crate top unreachable", top == nil)

    -- the thin fence at x = 0 (z 0..8): neighbours on both sides, no link through
    local a1, a2 = vec3(-0.5, -1, 4.5), vec3(0.5, -1, 4.5)
    check("cells beside the fence", nav.cell_at(a1) ~= nil and nav.cell_at(a2) ~= nil)
    check("fence blocks the straight line", not nav.line_clear(a1, a2))
    local around = nav.find_path(a1, a2)
    check("path goes around the fence", around ~= nil and around.complete and path_length(around) > 6,
        around and string.format("%.2f m", path_length(around)))

    -- open ground: a straight line, smoothed down to two points
    local s = nav.find_path(vec3(-10.5, -1, 10.5), vec3(-7.2, -1, 8.3))
    check("open ground is one straight segment", s ~= nil and #s.points == 2, s and (#s.points .. " points"))
    check("free path ends exactly at the target", s ~= nil and near(s.points[#s.points].x, -7.2) and near(s.points[#s.points].z, 8.3))

    -- up the ramp to the west terrace, then over the bridge to the east terrace
    local r = nav.find_path(vec3(-8.5, -1, 4.5), vec3(-8.5, 1.5, -9.5))
    check("ramp reaches the west terrace", r ~= nil and r.complete and near(r.points[#r.points].y, 1.5))
    local e = nav.find_path(vec3(-2.5, -1, 9.5), vec3(8.5, 1.5, -9.5))
    local on_bridge = false
    if e then
        for _, c in ipairs(e.cells) do
            local p = nav.cell_position(c)
            if p.x > -4 and p.x < 4 and p.y > 1.2 then on_bridge = true end
        end
    end
    check("east terrace via ramp and bridge", e ~= nil and e.complete and on_bridge)
    -- under the bridge: a ground path from one side to the other stays on the ground
    local u = nav.find_path(vec3(0.5, -1, -6.5), vec3(0.5, -1, -11.5), { grid = true })
    local stays_low = u ~= nil
    if u then for _, p in ipairs(u.points) do stays_low = stays_low and p.y < 0 end end
    check("under the bridge stays on the ground", stays_low and u.complete)

    -- the dais: only by the steps (its other sides drop 0.9 m)
    local d = nav.find_path(vec3(7.5, -1, 7.5), vec3(7.5, -0.1, 4.5), { grid = true })
    local via_steps = false
    if d then for _, p in ipairs(d.points) do via_steps = via_steps or (p.x > 3 and p.x < 5 and p.y > -0.75 and p.y < -0.3) end end
    check("dais reached by the steps", d ~= nil and d.complete and via_steps)

    -- grid costs: orthogonal 1, diagonal as asked
    local o = nav.find_path(vec3(-10.5, -1, 10.5), vec3(-7.5, -1, 10.5), { grid = true })
    check("3 orthogonal steps cost 3", o ~= nil and near(o.cost, 3) and #o.points == 4)
    local dg = nav.find_path(vec3(-10.5, -1, 10.5), vec3(-8.5, -1, 8.5), { grid = true, diagonal = 1.5 })
    check("2 diagonal steps cost 3 at 1.5", dg ~= nil and near(dg.cost, 3) and #dg.points == 3, dg and tostring(dg.cost))
    local cut = nav.find_path(vec3(-10.5, -1, 10.5), vec3(-4.5, -1, 10.5), { grid = true, max_cost = 4 })
    check("max_cost cuts the path", cut ~= nil and not cut.complete and near(cut.cost, 4))

    -- reachable: budget 1 with diagonal 1 = the cell and its 8 neighbours (open ground)
    local n = nav.reachable(vec3(-8.5, -1, 9.5), 1, { diagonal = 1 })
    check("reachable within 1 step", #n == 9, #n .. " cells")

    -- blocked cells: a path around a blocked cell, and a blocked goal gives the nearest cell
    local mid = nav.cell_at(vec3(-9.5, -1, 10.5))
    nav.set_blocked(mid, true)
    local bp = nav.find_path(vec3(-10.5, -1, 10.5), vec3(-8.5, -1, 10.5), { grid = true, diagonal = 1.5 })
    local avoids = bp ~= nil
    if bp then for _, c in ipairs(bp.cells) do avoids = avoids and c ~= mid end end
    check("path avoids a blocked cell", avoids and bp.complete and bp.cost > 2)
    local to_blocked = nav.find_path(vec3(-11.5, -1, 10.5), vec3(-9.5, -1, 10.5), { grid = true })
    check("blocked goal ends next to it", to_blocked ~= nil and not to_blocked.complete
        and #to_blocked.cells == 2, to_blocked and (#to_blocked.cells .. " cells"))
    nav.clear_blocked()

    local o2, d2 = scene.screen_ray(0.5, 0.5)
    check("screen_ray through the game camera", o2 ~= nil and d2 ~= nil and near(d2:length(), 1, 1e-3))

    log(string.format("nav_test: %d passed, %d failed", passed, failed))
end

function on_update(self, dt)
    frame = frame + 1
    if frame == 3 then run() end     -- after tactics.lua baked the grid in its on_create
end
