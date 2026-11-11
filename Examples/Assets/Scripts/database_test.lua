-- Checks the game database (Data/database.json) and NPCs (NpcComponent) on the tactics level: lookups, the goblins
-- coming to life with their stats, spawning, inventory, equipment modifiers, using items, timed and over-time
-- effects, death with dropped loot (pickups and the npc_died event) and an NPC that keeps its body.
-- Run unattended: MDB_SCENE=tactics_test.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/database_test.lua
-- Prints "database_test: PASS ..." / "FAIL ..." lines and a summary.

local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1; log("database_test: PASS " .. name .. (detail and ("  (" .. detail .. ")") or ""))
    else failed = failed + 1; log("database_test: FAIL " .. name .. (detail and ("  (" .. detail .. ")") or "")) end
end
local function near(a, b, eps) return math.abs(a - b) <= (eps or 0.01) end

local t = 0
local step = 0
local g, boss, sage          -- a spawned goblin, a second one to kill, the Sage
local died = {}              -- npc_died events: { entity, npc, drops }
local boss_carried = 0
local far = vec3(60, 0, 60)  -- away from the hero (no ambush, no picking up)

function on_event(self, name, data)
    if name == "npc_died" then table.insert(died, data) end
end

function on_update(self, dt)
    t = t + dt
    if step == 0 then
        step = 1
        -- ---- the database
        check("item lookup", db.item("sword") and db.item("sword").name == "Iron Sword")
        check("item stats", db.item("sword").stats.damage_max == 8)
        check("npc lookup", db.npc("goblin") and db.npc("goblin").stats.health == 12)
        check("unknown is nil", db.item("nope") == nil)
        check("lists", #db.items() >= 8 and #db.npcs() >= 2 and #db.stats() >= 4, #db.items() .. " items")
        check("death stat", db.death_stat() == "health")

        -- ---- the level's goblins came to life before on_create
        local goblins = scene.npcs("goblin")
        check("three goblins in the level", #goblins == 3, tostring(#goblins))
        local first = goblins[1] and goblins[1].npc
        check("goblin stats from the database", first and first:stat("health") == 12 and first:stat("armour") == 1)
        check("goblin carries its gold", first and first:count("gold") == 8)

        -- ---- spawn one
        g = scene.spawn_npc("goblin", far)
        local n = g.npc
        check("spawned an NPC", n ~= nil and n.id == "goblin" and n.name == "Goblin" and n.alive)
        check("its record", n.data.description ~= nil)

        -- inventory
        check("give", n:give("shield") == 1 and n:count("shield") == 1)
        check("stack", n:give("potion", 3) == 3 and n:count("potion") >= 3)
        check("take too many fails", n:take("potion", 99) == false)

        -- equipment: the shield's +2 armour counts while equipped
        local ok = n:equip("shield")
        check("equip", ok and n:equipped("shield") == "shield")
        check("equipped modifier", n:stat("armour") == 3 and n:base("armour") == 1, tostring(n:stat("armour")))
        n:unequip("shield")
        check("unequip", n:stat("armour") == 1 and n:equipped("shield") == nil)
        local bad, why = n:equip("potion")
        check("can't equip a potion", bad == false and why ~= nil, why)

        -- the ceiling: health can't pass max_health
        n:set("health", 999)
        check("clamped to max_health", n:stat("health") == 12)

        -- using a potion heals and uses it up
        n:damage(5)
        local before = n:count("potion")
        local used = n:use("potion")
        check("use potion", used and n:stat("health") == 12 and n:count("potion") == before - 1,
              string.format("health %.1f", n:stat("health")))

        -- timed modifier
        n:add_effect("armour", 5, 0.5)
        check("timed modifier on", n:stat("armour") == 6)

        -- the rusty dagger's hit: -1 health a second for 4 s
        n:apply("rusty_dagger", "hit")
        check("over-time effect running", #n:effects() == 2, tostring(#n:effects()))
    elseif step == 1 and t > 1.5 then
        step = 2
        local n = g.npc
        check("timed modifier ran out", n:stat("armour") == 1, tostring(n:stat("armour")))
        local hp = n:stat("health")
        check("festering wound hurts over time", hp < 12 and hp > 9.5, string.format("%.2f", hp))

        -- ---- death: a goblin with known loot
        boss = scene.spawn_npc("goblin", far + vec3(5, 0, 0))
        local b = boss.npc
        b:give("gem", 2)
        local carried = #b:inventory()
        local killed = b:damage(100)
        check("lethal damage reports the kill", killed == true and b.dead)
        check("damage on the dead does nothing", b:damage(5) == false)

        -- the Sage keeps its body
        sage = scene.find("Sage")
        check("the Sage is an NPC", sage and sage.npc and sage.npc.id == "sage")
        sage.npc:kill()
        boss_carried = carried
    elseif step == 2 then
        step = 3
        -- the next frame: events delivered, the dead goblin removed
        local mine
        for _, d in ipairs(died) do if d.npc == "goblin" and (d.position - (far + vec3(5, 0, 0))):length() < 0.1 then mine = d end end
        check("npc_died event", mine ~= nil, #died .. " events")
        if mine then
            check("its loot dropped", #mine.drops == boss_carried, #mine.drops .. " of " .. boss_carried)
            local found = scene.pickups(far + vec3(5, 0, 0), 2)
            check("pickups lie round it", #found == boss_carried, tostring(#found))
            local p = found[1] and found[1].pickup
            check("a pickup knows its item", p ~= nil and db.item(p.item) ~= nil, p and (p.count .. " x " .. p.name))
        end
        check("the dead goblin was removed", not boss:valid())
        check("the Sage stays (removeOnDeath off)", sage:valid() and sage.npc.dead)
        check("scene.npcs skips the dead", #scene.npcs("sage") == 0 and #scene.npcs("sage", true) == 1)
        log(string.format("database_test: %d passed, %d failed", passed, failed))
    end
end
