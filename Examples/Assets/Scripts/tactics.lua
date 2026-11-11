-- Tactics test: one layered nav grid, two ways of moving on it.
--
-- Exploring: click the ground and the hero walks there along a straightened (any-angle) path. The grid
-- never shows. Combat (C, the button, or walking within `ambush_range` of an enemy): every unit steps to
-- the nearest free cell, and turns go hero first, then each enemy. On the hero's turn the cells it can
-- reach with its movement points are shown, the path under the mouse is previewed with its cost, and a
-- click walks it cell by cell. Diagonal steps cost `diagonal`. Space or "End turn" passes. Enemies walk
-- towards the hero on their turn (as far as their points go) and stop next to it.
-- The hero attacks a goblin next to it by clicking it (once a turn): the goblins are NPCs of the game database
-- (Data/database.json: health, armour, loot). A dead one drops its loot, which the hero picks up by walking over it
-- (inventory.lua).
--
-- Units have no rigid body: this script moves them. Occupied cells are blocked on the grid during
-- combat, so paths go around units.

local move_points = 6
local diagonal = 1.5
local walk_speed, run_speed = 1.7, 3.8
local ambush_range = 4.5

local units = {}             -- { e, name, enemy, cell, path, index, moving, speed, clip, face }
local hero
local mode = "explore"
local sight_range = 9        -- fog of war: how far the hero sees (metres)
local turn = 1               -- the unit whose turn it is (combat)
local points = 0             -- movement left this turn
local reach                  -- cell -> { cell, cost, previous } for the hero's turn (nil = recompute)
local acted, wait = false, 0 -- enemy turn pacing
local armed = true           -- an ambush can start combat (re-armed once the hero is well away)
local message, message_time = nil, 0
local destination            -- explore: where the hero is going (for the marker)
local cs = 1
local owner                  -- this script's entity

local function say(text)
    message, message_time = text, 2.5
    log(text)
end

-- ---- ground markers: decals projected onto the floor, steps and ramps (decal.draw: this frame only)
local function tile(pos, color, alpha, size)            -- a grid cell: a faint square with a brighter border
    decal.draw(pos, { size = (size or 0.92) * cs, shape = "square", color = color, alpha = alpha,
                      thickness = 0.14, fill = 0.45, softness = 0.08, height = 1.2 })
end
local function ring(pos, color, alpha, size)
    decal.draw(pos, { size = size, shape = "ring", color = color, alpha = alpha, thickness = 0.22, height = 1.2 })
end
local function dot(pos, color, alpha, size)
    decal.draw(pos, { size = size, color = color, alpha = alpha, softness = 0.35, height = 1.2 })
end

-- ---- floating text over the world (scene.world_to_screen), rising and fading
local floaters = {}
local function float_text(text, pos, color)
    table.insert(floaters, { text = text, pos = pos, color = color or vec3(1, 1, 1), time = 0 })
end
local function draw_floaters(dt)
    for i = #floaters, 1, -1 do
        local f = floaters[i]
        f.time = f.time + dt
        if f.time > 1.2 then table.remove(floaters, i)
        else
            local x, y, visible = scene.world_to_screen(f.pos + vec3(0, f.time * 0.8, 0))
            if visible then ui.text(f.text, x, y, { size = 26, align = "center", color = f.color, alpha = math.min(1, 1.6 - f.time) }) end
        end
    end
end

-- ---- walking
local function play(u, clip)
    local a = u.e.animator
    if a and u.clip ~= clip then
        a:play(clip, 0.25)
        u.clip = clip
    end
end

local function walk(u, path_points, speed)
    u.path, u.index, u.speed = path_points, 2, speed
    u.moving = #path_points >= 2
    play(u, u.moving and (speed > 2.5 and "Run" or "Walk") or "Survey")
end

local function step_unit(u, dt)
    if not u.moving then return end
    local e = u.e
    local p = e.position
    local left = u.speed * dt
    while left > 0 and u.index <= #u.path do
        local target = u.path[u.index]
        local flat = vec3(target.x - p.x, 0, target.z - p.z)
        local dist = flat:length()
        if dist > 1e-3 then u.face = math.atan(flat.x, flat.z) end   -- the model faces +Z
        if dist <= left then
            p = vec3(target.x, p.y, target.z)
            left = left - dist
            u.index = u.index + 1
        else
            p = p + flat / dist * left
            left = 0
        end
    end
    -- Feet on the grid surface below (eased, so steps and ramps look smooth). The query starts a little
    -- above the feet, so it picks the floor the unit is on (ground or bridge), not the other layer.
    local ground = nav.height(vec3(p.x, p.y + 0.2, p.z)) or u.path[math.min(u.index, #u.path)].y
    p.y = p.y + (ground - p.y) * math.min(1, dt * 14)
    if u.face then
        local r = e.rotation.y
        local d = (u.face - r + math.pi) % (2 * math.pi) - math.pi
        e.rotation = vec3(0, r + d * math.min(1, dt * 12), 0)
    end
    if u.index > #u.path then
        u.moving = false
        p.y = ground
        play(u, "Survey")
    end
    e.position = p
end

-- ---- combat
local function adjacent(a, b)
    if not (a.cell and b.cell) then return false end
    local pa, pb = nav.cell_position(a.cell), nav.cell_position(b.cell)
    return math.abs(pa.x - pb.x) <= cs * 1.01 and math.abs(pa.z - pb.z) <= cs * 1.01 and math.abs(pa.y - pb.y) < 0.5
end

local function refresh_reach()
    local u = units[turn]
    reach = {}
    if not u.cell then return end
    for _, r in ipairs(nav.reachable(u.cell, points, { diagonal = diagonal })) do reach[r.cell] = r end
end

-- The cells from the unit's cell to `cell`, following the reach tree back
local function chain(cell)
    local list = {}
    while cell do
        table.insert(list, 1, cell)
        cell = reach[cell].previous
    end
    return list
end

local function begin_turn()
    local u = units[turn]
    points = move_points
    u.attacked = false
    acted, wait = false, 0.5
    reach = nil
    if u.enemy then say(u.name .. "'s turn") else say("Your turn") end
end

local function end_turn()
    turn = turn % #units + 1
    begin_turn()
end

local function enter_combat(reason)
    mode = "combat"
    destination = nil
    nav.clear_blocked()
    for _, u in ipairs(units) do
        u.moving = false
        u.cell = nav.nearest(u.e.position, 3, true)     -- a free cell: the units already placed block theirs
        if u.cell then
            nav.set_blocked(u.cell, true)
            walk(u, { u.e.position, nav.cell_position(u.cell) }, walk_speed)
        end
    end
    turn = 1
    begin_turn()
    say(reason or "Combat!")
end

local function leave_combat()
    mode = "explore"
    armed = false
    reach = nil
    nav.clear_blocked()
    for _, u in ipairs(units) do u.cell = nil end
    say("Combat over: exploring")
end

local function toggle_combat()
    if mode == "explore" then enter_combat("Combat!") else leave_combat() end
end

-- ---- per mode
local function update_explore(dt, mx, my, click)
    -- an ambush: an enemy close by (re-armed once the hero is well away from all of them)
    local nearest = 1e9
    for _, u in ipairs(units) do
        if u.enemy then nearest = math.min(nearest, (u.e.position - hero.e.position):length()) end
    end
    if nearest > ambush_range + 2.5 then armed = true end
    if armed and nearest < ambush_range then
        enter_combat("Ambush! Combat starts")
        return
    end

    if click then
        local hit = scene.pick(mx, my)
        local path = hit and nav.find_path(hero.e.position, hit.point)
        if path then
            local length = 0
            for i = 2, #path.points do length = length + (path.points[i] - path.points[i - 1]):length() end
            walk(hero, path.points, length > 5 and run_speed or walk_speed)
            destination = path.points[#path.points]
            if not path.complete then say("Can't get there: going as close as possible") end
        end
    end
    if destination and hero.moving then ring(destination, vec3(1, 0.85, 0.3), 0.9, 0.7)
    else destination = nil end
end

-- The enemy under the mouse (around its body on screen), if any
local function enemy_under(mx, my)
    for _, e in ipairs(units) do
        if e.enemy and e.seen ~= false then
            local x, y, visible = scene.world_to_screen(e.e.position + vec3(0, 0.9, 0))
            if visible and math.abs(x - mx) < 0.03 and math.abs(y - my) < 0.08 then return e end
        end
    end
end

-- The hero hits a goblin: damage from the equipped weapon (its stats in the game database, Data/database.json)
-- less the goblin's armour, then the weapon's "hit" effects (the rusty dagger's festering wound). A goblin
-- at 0 health dies: its loot falls to the ground (npc_died, below) and walking over it picks it up.
local function attack(u, target)
    local weapon = game.equip and game.equip.weapon and game.equip.weapon.id
    local w = weapon and db.item(weapon)
    local stats = w and w.stats or {}
    local lo, hi = math.floor(stats.damage_min or 1), math.floor(stats.damage_max or 2)
    local npc = target.e.npc
    local armour = npc and npc:stat("armour") or 0
    local damage = math.max(1, math.random(lo, math.max(lo, hi)) - math.floor(armour))
    u.attacked = true
    target.e:flash(vec3(1, 0.25, 0.15), 0.4)
    float_text("-" .. damage, target.e.position + vec3(0, 1.8, 0), vec3(1, 0.45, 0.35))
    audio.play("Sounds/coin.wav")
    if not npc then return end
    if weapon then npc:apply(weapon, "hit") end
    if npc:damage(damage) then say(target.name .. " is defeated!")
    else say(string.format("You hit %s for %d", target.name, damage)) end
end

local function update_hero_turn(dt, mx, my, click)
    local u = units[turn]
    if u.moving then return end
    -- an enemy under the mouse: attack it if it is next to the hero (once a turn)
    local foe = enemy_under(mx, my)
    if foe and foe.cell then
        local near = adjacent(u, foe)
        ring(nav.cell_position(foe.cell), vec3(1, 0.2, 0.15), 0.95, 1.2)
        ui.text(u.attacked and "Already attacked this turn" or near and "Attack" or "Too far: step next to it first",
            mx, my - 0.045, { size = 18, align = "center", color = near and not u.attacked and vec3(1, 0.6, 0.4) or vec3(0.8, 0.8, 0.85) })
        if click and near and not u.attacked then attack(u, foe) end
        return
    end
    if not reach then refresh_reach() end

    for cell, r in pairs(reach) do
        if cell ~= u.cell then tile(r.position, vec3(0.2, 0.5, 1.0), 0.6) end
    end

    local hit = scene.pick(mx, my)
    local cell = hit and nav.cell_at(hit.point)
    local r = cell and reach[cell]
    if r and cell ~= u.cell then
        local cells = chain(cell)
        for i = 2, #cells do dot(nav.cell_position(cells[i]), vec3(1, 0.55, 0.1), 0.9, 0.45) end
        -- what standing there means against the enemies: who could see you, and the cover from each
        local seen, half, full = 0, 0, 0
        local here = nav.cell_position(cell)
        for _, e in ipairs(units) do
            if e.enemy and e.cell then
                local from = nav.cell_position(e.cell)
                if nav.line_of_sight(from, here) then seen = seen + 1 end
                local c = nav.cover(here, from)
                if c == "full" then full = full + 1 elseif c == "half" then half = half + 1 end
            end
        end
        local info = string.format("cost %.1f   seen by %d", r.cost, seen)
        if full > 0 then info = info .. string.format("   full cover x%d", full) end
        if half > 0 then info = info .. string.format("   half cover x%d", half) end
        ui.text(info, mx, my - 0.045, { size = 18, align = "center", color = vec3(1, 0.9, 0.5) })
        if click then
            local path_points = {}
            for i, c in ipairs(cells) do path_points[i] = nav.cell_position(c) end
            path_points[1] = u.e.position
            nav.set_blocked(u.cell, false)
            u.cell = cell
            nav.set_blocked(cell, true)
            points = points - r.cost
            reach = nil
            walk(u, path_points, walk_speed)
        end
    end
end

local function update_enemy_turn(dt)
    local u = units[turn]
    if u.moving then return end
    wait = wait - dt
    if wait > 0 then return end
    if not acted then
        acted, wait = true, 0.6
        if not u.cell then return end
        local goal = hero.cell and nav.cell_position(hero.cell) or hero.e.position
        -- the hero's cell is blocked, so `closest` ends the path next to it (or as near as it can get)
        local path = nav.find_path(nav.cell_position(u.cell), goal,
            { grid = true, diagonal = diagonal, closest = true, max_cost = move_points })
        if path and #path.cells > 1 then
            nav.set_blocked(u.cell, false)
            u.cell = path.cells[#path.cells]
            nav.set_blocked(u.cell, true)
            walk(u, path.points, walk_speed)
        else
            say(u.name .. " waits")
        end
    else
        if adjacent(u, hero) then
            say(u.name .. " attacks! (no damage in this test)")
            hero.e:flash(vec3(1, 0.25, 0.15), 0.4)             -- the hit flash (e:flash)
            float_text("Hit!", hero.e.position + vec3(0, 1.6, 0), vec3(1, 0.4, 0.3))
        end
        end_turn()
    end
end

-- ---- UI (laid out: ui.begin_panel ... ui.end_panel; see ScriptEngine.h)
local function clock(t) return string.format("%d:%02d", math.floor(t / 60), math.floor(t % 60)) end

-- Top left: what you can do now, gold, the level and the quest
local function draw_hud()
    local combat = mode == "combat"
    ui.begin_panel("hud", { x = 0.012, y = 0.02, anchor = "topleft", w = 420, title = "Tactics Test", alpha = 0.78 })
    if not combat then
        ui.label("Exploring: click the ground to walk. Walk near an enemy, or press {b}C{/b}, to fight.")
        ui.label("Q / E turn the camera.  I inventory.  F6 / F9 quicksave / quickload.", { muted = true, size = 16 })
    else
        local u = units[turn]
        ui.label("Combat: " .. (u.enemy and (u.name .. " is moving...") or "{#9fd0ff}your turn{/}"))
        ui.bar(math.max(points, 0) / move_points, { h = 20, color = vec3(0.25, 0.5, 0.95),
            text = string.format("Movement %.1f / %d", math.max(points, 0), move_points) })
        ui.label(string.format("Click a blue cell to move (diagonals cost %.1f), a goblin next to you to attack. Space ends the turn.", diagonal),
            { muted = true, size = 16 })
    end
    ui.begin_row(8)
    ui.image("Textures/coin_icon.png", { w = 22 })
    ui.label(string.format("{#ffcc66}%d{/}", game.gold or 0), { bold = true })
    ui.space(10)
    ui.label(level.current() .. "   " .. clock(level.play_time()), { muted = true, size = 16 })
    ui.end_row()
    if game.quest_cellar then
        ui.label("{#ffcc66}Quest:{/} explore the cellar (the glowing pad in the south-west corner).", { size = 16 })
    end
    ui.end_panel()
    if message and message_time > 0 then
        ui.text(message, 0.5, 0.08, { size = 28, align = "center", alpha = math.min(1, message_time) })
    end
end

-- ---- save slots: F6 / F9 quicksave and quickload, and a panel with three slots ("Saves" button)
local menu_open = false
local slot_info = {}               -- slot -> { name, time, level, play_time } (read when the panel opens)

local function refresh_slots()
    slot_info = {}
    for _, s in ipairs(saves.list()) do slot_info[s.slot] = s end
end

local function save_to(slot)
    if mode == "combat" then say("You can't save during combat") return end
    local name = string.format("%s, %d gold", level.current(), game.gold or 0)
    local ok, err = saves.save(slot, name)
    say(ok and ("Saved: " .. slot) or ("Save failed: " .. tostring(err)))
    refresh_slots()
end

local function load_from(slot)
    if saves.load(slot) then say("Loading " .. slot .. "...") else say("No save in " .. slot) end
end

local function draw_save_menu()
    if not menu_open then return end
    if not ui.begin_panel("saves", { x = 0.985, y = 0.03, anchor = "topright", w = 470, title = "Save Slots", close = true,
                                     image = "Textures/panel_frame.png", slice = 20, padding = 26 }) then
        menu_open = false
    end
    for i = 1, 3 do
        local slot = "slot" .. i
        local s = slot_info[slot]
        ui.begin_row(10)
        ui.slot("icon" .. i, { icon = s and "Textures/Icons/scroll.png" or nil, size = 48, accept = false })
        ui.begin_column(220)
        ui.label(s and s.name or "{#a0a8b8}(empty){/}", { bold = s ~= nil, size = 17 })
        if s then ui.label(s.time .. "   played " .. clock(s.play_time), { muted = true, size = 14 }) end
        ui.end_column()
        if ui.button("Save", { id = "save" .. i, w = 70, disabled = mode == "combat",
                               tooltip = mode == "combat" and "Not during combat" or nil }) then save_to(slot) end
        if ui.button("Load", { id = "load" .. i, w = 70, disabled = s == nil, color = vec3(0.25, 0.45, 0.85) }) then load_from(slot) end
        ui.end_row()
        if i < 3 then ui.separator() end
    end
    ui.end_panel()
end

-- Bottom right: the buttons (auto width: the panel fits them)
local function draw_toolbar(hero_turn)
    ui.begin_panel("toolbar", { x = 0.988, y = 0.975, anchor = "bottomright", w = 0, padding = 10, alpha = 0.7 })
    ui.begin_row(8)
    if shared.can_talk and mode == "explore" and not shared.talking then
        if ui.button("Talk (T)", { color = vec3(0.2, 0.55, 0.45) }) then shared.talk_to = shared.can_talk end
    end
    if ui.button("Inventory (I)", { selected = shared.inventory_open }) then shared.toggle_inventory = true end
    if ui.button("Saves", { selected = menu_open }) then
        menu_open = not menu_open
        if menu_open then refresh_slots() end
    end
    if hero_turn and ui.button("End turn", { color = vec3(0.25, 0.45, 0.85) }) then end_turn() end
    if ui.button(mode == "explore" and "Start combat" or "Leave combat") then toggle_combat() end
    ui.end_row()
    ui.end_panel()
end

-- ---- callbacks

-- Events from other scripts and dialogues (events.emit). The Sage's dialogue sends "quest_started".
function on_event(self, name, data)
    if name == "quest_started" then say("New quest: explore the cellar") end
    -- a goblin died (its loot is on the ground already): out of the fight, and the fight ends with the last one
    if name == "npc_died" then
        for i, u in ipairs(units) do
            if u.e == data.entity then
                if u.cell then nav.set_blocked(u.cell, false) end
                table.remove(units, i)
                if i < turn then turn = turn - 1 end
                if turn > #units then turn = 1 end
                break
            end
        end
        local left = 0
        for _, u in ipairs(units) do if u.enemy then left = left + 1 end end
        if left == 0 and mode == "combat" then leave_combat(); say("Victory! Pick up the loot") end
    end
end

function on_create(self)
    owner = self
    local count = nav.bake{ cell_size = 1, agent_radius = 0.3, agent_height = 1.8, max_step = 0.4 }
    cs = nav.cell_size()
    log("tactics.lua: nav grid has " .. count .. " cells")

    local h = scene.find("Hero")
    if not h then
        log("tactics.lua: no entity named Hero")
        return
    end
    -- Arrived from another level (level.change): stand at this level's "Entry: <entry>". After a load
    -- the hero stands where it was saved instead.
    local entry = level.entry()
    if level.arrived() and entry then
        local spot = scene.find("Entry: " .. entry)
        if spot then
            h.position = spot.position
            h.rotation = spot.rotation
        else
            log("tactics.lua: no entity named 'Entry: " .. entry .. "'")
        end
    end
    hero = { e = h, name = "Hero", enemy = false }
    table.insert(units, hero)
    for i, e in ipairs(scene.find_all("Enemy")) do
        table.insert(units, { e = e, name = "Enemy " .. i, enemy = true })
    end
    play(hero, "Survey")
    shared.focus = h.position
    -- fog of war: black where nobody has been, dimmed where the hero has been but can't see now
    fog.enable{ explored_alpha = 0.6 }
end

-- The explored map is saved with the level (level changes and save slots)
function on_save(self) return { fog = fog.save() } end
function on_load(self, data) if data.fog then fog.load(data.fog) end end

function on_update(self, dt)
    if not hero then return end
    message_time = message_time - dt

    -- the panels first: a click on them must not also walk (ui.wants_mouse below)
    local mx, my, inside = ui.mouse()
    local hero_turn = mode == "combat" and not units[turn].enemy and not units[turn].moving
    draw_hud()
    draw_toolbar(hero_turn)
    draw_save_menu()
    if input.key_pressed(Key.C) then toggle_combat() end
    if hero_turn and input.key_pressed(Key.Space) then end_turn() end
    if input.key_pressed(Key.F6) then save_to("quick") end
    if input.key_pressed(Key.F9) then load_from("quick") end
    if shared.notice then say(shared.notice); shared.notice = nil end   -- from other scripts (chest.lua)
    shared.mode = mode                                                   -- exit.lua: no level change in combat

    local click = inside and not ui.wants_mouse() and input.mouse_pressed(Mouse.Left)
    if mode == "explore" then
        update_explore(dt, mx, my, click)
    elseif units[turn].enemy then
        update_enemy_turn(dt)
    else
        update_hero_turn(dt, mx, my, click)
    end

    for _, u in ipairs(units) do step_unit(u, dt) end

    -- fog of war: the hero sees what is in sight range and not behind walls; enemies in the fog are hidden
    fog.reveal(hero.e.position, sight_range)
    for _, u in ipairs(units) do
        if u.enemy then u.seen = fog.is_visible(u.e.position); u.e.visible = u.seen end
    end

    -- in combat, mark where each unit stands
    if mode == "combat" then
        for i, u in ipairs(units) do
            if u.cell and (not u.enemy or u.seen) then
                local col = u.enemy and vec3(1, 0.25, 0.2) or vec3(0.3, 0.6, 1)
                ring(nav.cell_position(u.cell), col, i == turn and 0.95 or 0.6, 1.1)
            end
            -- a name plate over each unit (scene.world_to_screen)
            local x, y, visible = scene.world_to_screen(u.e.position + vec3(0, 2.0, 0))
            if visible and (not u.enemy or u.seen) then
                ui.text(u.name, x, y, { size = 16, align = "center", color = u.enemy and vec3(1, 0.55, 0.5) or vec3(0.6, 0.8, 1) })
                local npc = u.e.npc                  -- its health, from its NPC state (game database)
                if npc then
                    ui.text(string.format("%d / %d", math.ceil(npc:stat("health")), math.ceil(npc:stat("max_health"))),
                        x, y + 0.026, { size = 14, align = "center", color = vec3(0.95, 0.75, 0.7) })
                end
            end
        end
    end
    draw_floaters(dt)

    local active = mode == "combat" and units[turn] or hero
    shared.focus = active.e.position
end
