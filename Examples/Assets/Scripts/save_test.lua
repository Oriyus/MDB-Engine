-- Save games and level changes, end to end, on the two tactics levels. Run unattended:
--   MDB_SCENE=tactics_test.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/save_test.lua MDB_SAVE_DIR=<folder>
-- Prints "save_test: PASS ..." / "FAIL ..." lines and "save_test: done" at the end.
-- The script keeps its progress (phase) through on_save / on_load, and travels between the levels
-- with level.change{ carry = { self } }, so each load or level change continues where it left off.

local phase = 1
local frame = 0
local note = "start"
local marker_uid
local loaded_ref
local loading_at_create = false
local step = 0

local function check(name, ok, detail)
    log("save_test: " .. (ok and "PASS" or "FAIL") .. " [" .. phase .. "] " .. name
        .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end
local function count(name) return #scene.find_all(name) end
local function near(a, b) return a and b and (a - b):length() < 0.05 end
local function hero() return scene.find("Hero") end
local function at_entry(name)
    local e = scene.find("Entry: " .. name)
    return e ~= nil and near(hero().position, e.position)
end

function on_create(self)
    loading_at_create = scene.is_loading()
end

function on_save(self)
    return { phase = phase, note = note, marker = marker_uid, ref = scene.find("Hero"), v = vec3(1, 2, 3),
             list = { 1, 2.5, "x", true }, map = { [7] = "seven", ["#tag"] = "hash" } }
end

local restored
function on_load(self, data)
    restored = data
    phase, note, marker_uid, loaded_ref = data.phase, data.note, data.marker, data.ref
end

function on_update(self, dt)
    frame = frame + 1
    if frame < 5 then return end      -- let the level's own scripts settle first
    step = step + 1

    if phase == 1 then
        if step == 1 then
            for _, s in ipairs({ "test_a", "test_b", "restart_marker" }) do saves.delete(s) end
            check("fresh game: no gold yet", game.gold == nil)   -- (inventory.lua already filled game.bag)
            check("not loading on a new game", not loading_at_create and not level.arrived())
            game.gold = 42
            shared.flag = "yard"
            note = "before"
            hero().position = vec3(-5.5, -1, 9.5)
            scene.find("Crate"):destroy()                  -- 5 -> 4 (destroys happen at the end of the frame)
            local m = scene.spawn("Marker", vec3(1, -1, 1))
            m:add_mesh{ geometry = "sphere", color = vec3(1, 0, 1) }
            marker_uid = m.uid
            local t = scene.spawn("Temp marker")            -- transient: never saved
            t.transient = true
        elseif step == 2 then
            phase = 3                                      -- what the save will hold
            local ok, err = saves.save("test_a", "Test A")
            check("save test_a", ok, err)
            check("saves.exists", saves.exists("test_a"))
            -- now change everything, then load: all of it must come back
            game.gold = 0
            shared.flag = "changed"
            note = "after"
            hero().position = vec3(0.5, -1, 0.5)
            scene.find_uid(marker_uid):destroy()
            scene.find("Crate"):destroy()
            check("saves.load accepted", saves.load("test_a"))
            check("load of a missing slot refused", not saves.load("no_such_slot"))
        end

    elseif phase == 3 then                                 -- after loading test_a
        check("is_loading during on_create", loading_at_create)
        check("level", level.current() == "tactics_test.json", level.current())
        check("not arrived (a load)", not level.arrived())
        check("game.gold", game.gold == 42, game.gold)
        check("shared restored", shared.flag == "yard", shared.flag)
        check("script variable (on_load)", note == "before", note)
        check("vec3 round trip", restored and restored.v and near(restored.v, vec3(1, 2, 3)))
        check("array round trip", restored and restored.list and #restored.list == 4 and restored.list[2] == 2.5
            and restored.list[3] == "x" and restored.list[4] == true)
        check("integer and '#' keys", restored and restored.map and restored.map[7] == "seven" and restored.map["#tag"] == "hash")
        check("entity reference", loaded_ref ~= nil and loaded_ref:valid() and loaded_ref.name == "Hero")
        check("hero position", near(hero().position, vec3(-5.5, -1, 9.5)), hero().position)
        check("destroyed crate stays destroyed, the other one is back", count("Crate") == 4, count("Crate"))
        local m = marker_uid and scene.find_uid(marker_uid)
        check("spawned entity restored with its uid and mesh", m ~= nil and m.name == "Marker" and m.mesh ~= nil)
        check("transient entity not saved", count("Temp marker") == 0)
        local listed = false
        for _, s in ipairs(saves.list()) do if s.slot == "test_a" and s.name == "Test A" then listed = true end end
        check("saves.list", listed)
        game.gold = 50
        scene.find("Crate"):destroy()                      -- 4 -> 3
        phase = 4
        level.change("tactics_cellar.json", "From Yard", { carry = { self } })

    elseif phase == 4 then                                 -- first time in the cellar, carried along
        check("level", level.current() == "tactics_cellar.json", level.current())
        check("entry and arrived", level.entry() == "From Yard" and level.arrived())
        check("not loading (first visit)", not loading_at_create)
        check("carried script data", note == "before", note)
        check("game kept across the change", game.gold == 50, game.gold)
        check("shared is per level", shared.flag == nil, shared.flag)
        check("hero placed at the entry", at_entry("From Yard"))
        check("cellar crates", count("Crate") == 2, count("Crate"))
        check("carried entity exists once", count("Test") == 1, count("Test"))
        shared.cellar = "visited"
        scene.find("Crate"):destroy()                      -- cellar: 2 -> 1
        phase = 5
        level.change("tactics_test.json", "From Cellar", { carry = { self } })

    elseif phase == 5 then                                 -- back in the yard
        check("level", level.current() == "tactics_test.json")
        check("loading (visited before)", loading_at_create)
        check("yard as it was left: 3 crates", count("Crate") == 3, count("Crate"))
        check("yard shared", shared.flag == "yard", shared.flag)
        check("marker still there", marker_uid ~= nil and scene.find_uid(marker_uid) ~= nil)
        check("hero at the entry", at_entry("From Cellar"))
        check("carried entity exists once", count("Test") == 1, count("Test"))
        phase = 7
        local ok, err = saves.save("test_b", "Test B")     -- holds the cellar as it was left, too
        check("save test_b", ok, err)
        level.change("tactics_cellar.json", "From Yard", { carry = { self } })

    elseif phase == 7 and level.current() == "tactics_cellar.json" then
        check("cellar remembered: 1 crate", count("Crate") == 1, count("Crate"))
        check("cellar shared remembered", shared.cellar == "visited", shared.cellar)
        check("load test_b", saves.load("test_b"))          -- back to the yard, phase 7 (from the save)
        phase = 0                                          -- wait for the load

    elseif phase == 7 then                                 -- the yard, loaded from test_b
        check("loaded test_b: yard", level.current() == "tactics_test.json" and count("Crate") == 3)
        phase = 9
        level.change("tactics_cellar.json", "From Yard", { carry = { self } })

    elseif phase == 9 then                                 -- the cellar state came from inside the save
        if step == 1 then
            if saves.exists("restart_marker") then
                check("restart: the crate destroyed after entering is back", count("Crate") == 1, count("Crate"))
                saves.delete("restart_marker")
                log("save_test: done")
                phase = 0
                return
            end
            check("visited level stored in the save: 1 crate", count("Crate") == 1, count("Crate"))
            check("its shared too", shared.cellar == "visited", shared.cellar)
            scene.find("Crate"):destroy()                  -- 1 -> 0, then restart
        elseif step == 2 then
            check("crate destroyed before the restart", count("Crate") == 0, count("Crate"))
            saves.save("restart_marker")                   -- a file survives the restart; Lua state doesn't
            scene.restart()
        end
    end
end
