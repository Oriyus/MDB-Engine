-- Checks fog of war (fog.*, e.visible) on the tactics yard. tactics.lua turns fog on and reveals around
-- the hero, so the hero stays at its start (south) and the checks look at the north-west.
-- Run unattended: MDB_SCENE=tactics_test.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/fog_test.lua
-- Prints "fog_test: PASS ..." / "FAIL ..." lines and "fog_test: done".

local frame = 0
local function check(name, ok, detail)
    log("fog_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end

local viewer = vec3(-2.5, -1, -5.5)
local open = vec3(-2.5, -1, -2.5)            -- open ground 3 m away
local terrace = vec3(-8.5, 1.5, -9.5)        -- on top of the west terrace: behind its wall
local far = vec3(-2.5, -1, -12.5)            -- outside a 6 m sight range
local saved

local steps = {
    function()
        scene.find("Hero").position = vec3(-2.5, -1, 9.5)      -- its start: 12 m or more from every point below
        check("tactics.lua turned fog on", fog.enabled())
    end,
    function()
        fog.reset()
        check("reset: the north-west is unexplored", not fog.is_explored(open) and not fog.is_explored(viewer))
        fog.reveal(viewer, 6)
        check("open ground in range: visible", fog.is_visible(open))
        check("the viewer's own cell: visible", fog.is_visible(viewer))
        check("on the terrace, behind its wall: not visible", not fog.is_visible(terrace))
        check("beyond the range: not visible", not fog.is_visible(far))
    end,
    function()
        check("last frame's sight is gone (reveal is per frame)", not fog.is_visible(open))
        check("what was seen stays explored", fog.is_explored(open))
        check("what was not seen is not", not fog.is_explored(terrace) and not fog.is_explored(far))
        fog.explore(far, 1.5)
        check("fog.explore marks without sight", fog.is_explored(far) and not fog.is_visible(far))
        saved = fog.save()
        check("fog.save gives a string", type(saved) == "string" and #saved > 10, saved and #saved)
        fog.reset()
        check("reset forgets it", not fog.is_explored(open))
        check("fog.load restores it", fog.load(saved) and fog.is_explored(open) and fog.is_explored(far))
        check("fog.load refuses junk", not fog.load("fog1:3:3:00"))
    end,
    function()
        local enemy = scene.find("Enemy")
        enemy.visible = false
        check("e.visible = false", enemy.visible == false)
        enemy.visible = true
        check("e.visible = true", enemy.visible == true)
        fog.reset()
    end,
    function()
        local p = scene.find("Hero").position
        check("near the hero: explored again (tactics.lua reveals it)", fog.is_explored(p + vec3(-1, 0, 0)) and not fog.is_explored(open))
        log("fog_test: done")
    end,
}

function on_update(self, dt)
    frame = frame + 1
    local i = frame - 4
    if i >= 1 and steps[i] then steps[i]() end
end
