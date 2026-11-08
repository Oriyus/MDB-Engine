-- Tactics test: one layered nav grid, two ways of moving on it.
--
-- Exploring: click the ground and the hero walks there along a straightened (any-angle) path. The grid
-- never shows. Combat (C, the button, or walking within `ambush_range` of an enemy): every unit steps to
-- the nearest free cell, and turns go hero first, then each enemy. On the hero's turn the cells it can
-- reach with its movement points are shown, the path under the mouse is previewed with its cost, and a
-- click walks it cell by cell. Diagonal steps cost `diagonal`. Space or "End turn" passes. Enemies walk
-- towards the hero on their turn (as far as their points go) and stop next to it.
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
local turn = 1               -- the unit whose turn it is (combat)
local points = 0             -- movement left this turn
local reach                  -- cell -> { cell, cost, previous } for the hero's turn (nil = recompute)
local acted, wait = false, 0 -- enemy turn pacing
local armed = true           -- an ambush can start combat (re-armed once the hero is well away)
local message, message_time = nil, 0
local destination            -- explore: where the hero is going (for the marker)
local cs = 1
local owner                  -- this script's entity (the tiles are its children)

local function say(text)
    message, message_time = text, 2.5
    log(text)
end

-- ---- overlay tiles: thin see-through boxes, pooled (spawned once, hidden below the level when unused).
-- Lit, with some emissive so they read in shade too (unlit meshes glow 4x: they would turn white).
local tiles, used = {}, 0
local function tile(pos, color, alpha, size, lift)
    used = used + 1
    local t = tiles[used]
    if not t then
        t = scene.spawn("Nav tile")
        t:add_mesh{ geometry = "cube", color = color, opacity = alpha, cast_shadows = false,
                    roughness = 1, metallic = 0 }   -- rough: no sky reflection washing the colour out
        t.parent = owner                   -- grouped under this entity in the Hierarchy
        t.transient = true                 -- never saved
        tiles[used] = t
    end
    local s = (size or 0.9) * cs
    t.position = pos + vec3(0, lift or 0.03, 0)
    t.scale = vec3(s, 0.02, s)
    local m = t.mesh
    m.color = color
    m.emissive = color * 0.35
    m.opacity = alpha
end
local function hide_unused_tiles()
    for i = used + 1, #tiles do
        if tiles[i].position.y > -50 then tiles[i].position = vec3(0, -100, 0) end
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
    if destination and hero.moving then tile(destination, vec3(1, 0.85, 0.3), 0.7, 0.35, 0.04)
    else destination = nil end
end

local function update_hero_turn(dt, mx, my, click)
    local u = units[turn]
    if u.moving then return end
    if not reach then refresh_reach() end

    for cell, r in pairs(reach) do
        if cell ~= u.cell then tile(r.position, vec3(0.1, 0.4, 1.0), 0.45) end
    end

    local hit = scene.pick(mx, my)
    local cell = hit and nav.cell_at(hit.point)
    local r = cell and reach[cell]
    if r and cell ~= u.cell then
        local cells = chain(cell)
        for i = 2, #cells do tile(nav.cell_position(cells[i]), vec3(1, 0.5, 0.05), 0.8, 0.6, 0.045) end
        ui.text(string.format("cost %.1f", r.cost), mx, my - 0.04, { size = 18, align = "center", color = vec3(1, 0.9, 0.5) })
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
        if adjacent(u, hero) then say(u.name .. " attacks! (no damage in this test)") end
        end_turn()
    end
end

-- ---- UI
local function panel_text(text, line, opts)
    ui.text(text, 0.025, 0.028 + line * 0.036, opts or { size = 17 })
end

local function draw_ui()
    local combat = mode == "combat"
    local dim = { size = 17, color = vec3(0.7, 0.75, 0.8) }
    ui.rect(0.012, 0.015, 0.38, combat and 0.2 or 0.165, { color = vec3(0.02, 0.03, 0.05), alpha = 0.55, rounding = 6 })
    panel_text("TACTICS TEST", 0, { size = 22, color = vec3(1, 0.85, 0.4) })
    if not combat then
        panel_text("Exploring: click the ground to walk (free movement).", 1)
        panel_text("C or the button: combat. Walk near an enemy: ambush.", 2)
        panel_text("Q / E: turn the camera.  F6 quicksave, F9 quickload.", 3, dim)
    else
        local u = units[turn]
        panel_text("Combat: " .. (u.enemy and (u.name .. " is moving...") or "your turn"), 1)
        panel_text(string.format("Movement %.1f / %d   (diagonal steps cost %.1f)", math.max(points, 0), move_points, diagonal), 2)
        panel_text("Click a blue cell to move. Space: end turn.", 3)
        panel_text("C: leave combat. Q / E: turn the camera.", 4, dim)
    end
    local t = level.play_time()
    panel_text(string.format("Gold %d     %s     %d:%02d", game.gold or 0, level.current(), math.floor(t / 60), math.floor(t % 60)),
        combat and 5 or 4, { size = 17, color = vec3(1, 0.85, 0.4) })
    if message and message_time > 0 then
        ui.text(message, 0.5, 0.08, { size = 28, align = "center", alpha = math.min(1, message_time) })
    end
end

-- ---- save slots: F6 / F9 quicksave and quickload, and a panel with three slots ("Saves" button)
local menu_open = false
local slot_info = {}               -- slot -> { name, time, level, play_time } (read when the panel opens)
local menu_x, menu_y, menu_w, menu_h = 0.6, 0.12, 0.38, 0.36

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

-- Draws the panel; true if the mouse is over it (so clicks there don't walk)
local function draw_save_menu(mx, my)
    if not menu_open then return false end
    ui.rect(menu_x, menu_y, menu_w, menu_h, { color = vec3(0.02, 0.03, 0.05), alpha = 0.8, rounding = 8 })
    ui.text("SAVE SLOTS", menu_x + 0.015, menu_y + 0.015, { size = 22, color = vec3(1, 0.85, 0.4) })
    for i = 1, 3 do
        local slot = "slot" .. i
        local y = menu_y + 0.07 + (i - 1) * 0.095
        local s = slot_info[slot]
        ui.text(s and s.name or "(empty)", menu_x + 0.015, y, { size = 18 })
        if s then
            ui.text(string.format("%s   played %d:%02d", s.time, math.floor(s.play_time / 60), math.floor(s.play_time % 60)),
                menu_x + 0.015, y + 0.032, { size = 15, color = vec3(0.7, 0.75, 0.8) })
        end
        if ui.button("Save", menu_x + 0.22, y, 0.07, 0.055, { rounding = 5 }) then save_to(slot) end
        if s and ui.button("Load", menu_x + 0.295, y, 0.07, 0.055, { rounding = 5, color = vec3(0.25, 0.45, 0.85) }) then load_from(slot) end
    end
    return mx >= menu_x and mx <= menu_x + menu_w and my >= menu_y and my <= menu_y + menu_h
end

-- ---- callbacks
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
end

function on_update(self, dt)
    if not hero then return end
    used = 0
    message_time = message_time - dt

    -- buttons first, so their click doesn't also count as a click on the ground
    local mx, my, inside = ui.mouse()
    local ui_click = false
    if ui.button(mode == "explore" and "Start combat" or "Leave combat", 0.83, 0.91, 0.15, 0.065, { rounding = 6 }) then
        toggle_combat()
        ui_click = true
    end
    local hero_turn = mode == "combat" and not units[turn].enemy and not units[turn].moving
    if hero_turn and ui.button("End turn", 0.66, 0.91, 0.15, 0.065, { rounding = 6, color = vec3(0.25, 0.45, 0.85) }) then
        end_turn()
        ui_click = true
    end
    if ui.button(menu_open and "Close" or "Saves", 0.50, 0.91, 0.14, 0.065, { rounding = 6 }) then
        menu_open = not menu_open
        if menu_open then refresh_slots() end
        ui_click = true
    end
    local over_menu = draw_save_menu(mx, my)
    if input.key_pressed(Key.C) then toggle_combat() end
    if hero_turn and input.key_pressed(Key.Space) then end_turn() end
    if input.key_pressed(Key.F6) then save_to("quick") end
    if input.key_pressed(Key.F9) then load_from("quick") end
    if shared.notice then say(shared.notice); shared.notice = nil end   -- from other scripts (chest.lua)
    shared.mode = mode                                                   -- exit.lua: no level change in combat

    local click = inside and not ui_click and not over_menu and my < 0.89 and input.mouse_pressed(Mouse.Left)
    if mode == "explore" then
        update_explore(dt, mx, my, click)
    elseif units[turn].enemy then
        update_enemy_turn(dt)
    else
        update_hero_turn(dt, mx, my, click)
    end

    for _, u in ipairs(units) do step_unit(u, dt) end

    -- in combat, mark where each unit stands
    if mode == "combat" then
        for i, u in ipairs(units) do
            if u.cell then
                local col = u.enemy and vec3(1, 0.25, 0.2) or vec3(0.3, 0.6, 1)
                tile(nav.cell_position(u.cell), col, i == turn and 0.75 or 0.4, 0.95, 0.035)
            end
        end
    end
    hide_unused_tiles()

    local active = mode == "combat" and units[turn] or hero
    shared.focus = active.e.position
    draw_ui()
end
