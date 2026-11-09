-- Checks that the village kit (Models/Village) works with the nav grid, on the demo village.
-- Run unattended: MDB_SCENE=village.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/village_nav_test.lua
-- Prints "village_nav_test: PASS ..." / "FAIL ..." lines and a summary to the log.

local frame = 0
local passed, failed = 0, 0

local function check(name, ok, detail)
    if ok then passed = passed + 1; log("village_nav_test: PASS " .. name)
    else failed = failed + 1; log("village_nav_test: FAIL " .. name .. (detail and ("  (" .. detail .. ")") or "")) end
end

local function walkable(p) return nav.cell_at(p) ~= nil end

local function reaches(from, to)
    local p = nav.find_path(from, to, { closest = false })
    return p ~= nil, p and (#p.points .. " points") or "no path"
end

local function run()
    check("baked", nav.baked() or nav.bake{ cell_size = 1 } > 0)

    -- House A (x -9..-3, z -7..-3): floor tiles inside, front door at x -6.6..-5.4 on z = -3
    check("floor inside house A", walkable(vec3(-7.5, 0.06, -5.5)))
    check("in front of the door", walkable(vec3(-5.5, 0, -2.5)))
    check("just inside the door", walkable(vec3(-5.5, 0.06, -3.5)))
    check("through the doorway", nav.line_clear(vec3(-5.5, 0, -2.5), vec3(-5.5, 0.06, -3.5)))
    check("not through a window wall", not nav.line_clear(vec3(-8.5, 0, -2.5), vec3(-8.5, 0.06, -3.5)))
    check("not through a plain wall", not nav.line_clear(vec3(-2.5, 0, -6.5), vec3(-3.5, 0.06, -6.5)))
    check("cells beside walls stay walkable", walkable(vec3(-2.5, 0, -6.5)) and walkable(vec3(-3.5, 0.06, -6.5)))
    check("square to the back of house A", reaches(vec3(0.5, 0, 0.5), vec3(-8.5, 0.06, -6.5)))

    -- House C (x -11..-7, z 3..7), door on x = -7 at z 5.4..6.6
    check("through house C's door", nav.line_clear(vec3(-6.5, 0, 5.5), vec3(-7.5, 0.06, 5.5)))
    check("into house C", reaches(vec3(-4.5, 0, 5.5), vec3(-10.5, 0.06, 3.5)))

    -- House B (x 5..9, z -5..-1): the broken wall and floor still let you in
    check("into ruined house B", reaches(vec3(3.5, 0, 0.5), vec3(6.5, 0.06, -3.5)))

    -- the low ruin (z = -10) blocks the way straight across but is low enough for half cover
    check("ruin blocks the line", not nav.line_clear(vec3(9.5, 0, -10.5), vec3(9.5, 0, -9.5)))
    local cover = nav.cover(vec3(9.5, 0, -9.5), vec3(9.5, 0, -14))
    check("ruin gives half cover", cover == "half", tostring(cover))

    -- the fence (z = 10) blocks, the gate gap (x 9..11) does not
    check("fence blocks", not nav.line_clear(vec3(5.5, 0, 9.5), vec3(5.5, 0, 10.5)))
    check("gate is open", nav.line_clear(vec3(10.5, 0, 9.5), vec3(10.5, 0, 10.5)))

    log(string.format("village_nav_test: %d passed, %d failed", passed, failed))
end

function on_update(self, dt)
    frame = frame + 1
    if frame == 3 then run() end
end
