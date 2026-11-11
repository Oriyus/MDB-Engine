-- The party's inventory: a bag of 16 slots and three equipment slots, kept in `game` (so it is saved,
-- and carried from level to level). I (or tactics.lua's Inventory button) opens it.
-- Drag items between slots; an equipment slot only takes items for that slot. Right-click a consumable to use it.
-- Other scripts give items by adding ids to game.pending_loot (chest.lua). Walking over an item lying on the
-- ground (a pickup: what a dead goblin dropped) picks it up; gold goes into game.gold.
--
-- What the items are (name, icon, slot, stacking, description) comes from the game database,
-- Assets/Data/database.json: edit them in the editor's Window > Database.

local BAG = 16
local EQUIP = { { key = "head", label = "Head" }, { key = "weapon", label = "Weapon" }, { key = "shield", label = "Shield" } }
local PICKUP_RANGE = 1.3

local open = false
local hero

local function item(id) return db.item(id) end

-- Put n of an item in the bag: on its stack if it stacks, else in the first empty slot. False if full.
local function give(id, n)
    n = n or 1
    local it = item(id)
    if not it then log("inventory.lua: unknown item " .. tostring(id)) return false end
    if id == "gold" then game.gold = (game.gold or 0) + n return true end   -- coins go into the purse
    if it.stackable then
        for i = 1, BAG do
            local e = game.bag[i]
            if e and e.id == id then e.count = e.count + n return true end
        end
    end
    for i = 1, BAG do
        if not game.bag[i] then
            game.bag[i] = { id = id, count = it.stackable and n or 1 }
            if not it.stackable and n > 1 then return give(id, n - 1) end
            return true
        end
    end
    shared.notice = "Your bag is full"
    return false
end

local function setup()
    if game.bag then return end
    game.bag = {}
    for i = 1, BAG do game.bag[i] = false end            -- false, not nil: keeps the bag an array when saved
    game.equip = { head = false, weapon = false, shield = false }
    give("sword"); give("shield"); give("potion", 3); give("scroll")
end

-- Where a slot's contents live: { where = "bag", key = 3 } or { where = "equip", key = "head" }
local function get(at) return at.where == "bag" and game.bag[at.key] or game.equip[at.key] end
local function set(at, entry)
    if at.where == "bag" then game.bag[at.key] = entry or false else game.equip[at.key] = entry or false end
end
local function fits(entry, at)        -- can `entry` go into `at`?
    if not entry or at.where == "bag" then return true end
    local it = item(entry.id)
    return it ~= nil and it.slot == at.key
end

-- Move what is at `from` to `to`; whatever was at `to` goes back to `from` (if it fits there)
local function move(from, to)
    if from.where == to.where and from.key == to.key then return end
    local a, b = get(from), get(to)
    if not fits(a, to) then shared.notice = "That doesn't go there" return end
    if not fits(b, from) then shared.notice = "No room for the " .. item(b.id).name return end
    if a and b and a.id == b.id and item(a.id).stackable then    -- same stackable item: merge
        b.count = b.count + a.count
        set(from, nil)
        return
    end
    set(to, a)
    set(from, b)
end

local function tooltip(entry)
    local it = item(entry.id)
    if not it then return entry.id end
    local text = "{b}" .. it.name .. "{/b}\n" .. (it.description or "")
    if it.value and it.value > 0 then text = text .. string.format("\n{#a0a8b8}Worth %d gold{/}", it.value) end
    return text
end

local function use(at)
    local e = get(at)
    if not e then return end
    local it = item(e.id)
    if not (it and it.consumable) then return end
    e.count = e.count - 1
    if e.count <= 0 then set(at, nil) end
    shared.notice = e.id == "potion" and "You drink the potion. You feel better." or ("You use the " .. it.name)
    audio.play("Sounds/unlock.wav")
end

-- One slot of the inventory. A drag carries where it came from.
local function item_slot(id, at)
    local e = get(at)
    local held = ui.dragging()
    local accept = true
    if held then accept = fits(get(held), at) end
    local it = e and item(e.id)
    local clicked, dropped, right = ui.slot(id, {
        icon = it and it.icon, count = e and e.count or 0, tooltip = e and tooltip(e) or nil,
        drag = e and at or nil, accept = accept, highlight = at.where == "equip" and e ~= false and e ~= nil })
    if dropped then move(dropped, at) end
    if right then use(at) end
end

local function draw()
    if not ui.begin_panel("inventory", { x = 0.5, y = 0.47, w = 560, title = "Inventory", close = true,
                                         image = "Textures/panel_frame.png", slice = 20, padding = 26 }) then
        open = false
    end
    ui.begin_row(26)
        ui.begin_column(150)
            ui.label("Equipped", { bold = true, muted = true })
            for _, s in ipairs(EQUIP) do
                ui.begin_row(10)
                item_slot("equip_" .. s.key, { where = "equip", key = s.key })
                ui.label(s.label, { muted = true })
                ui.end_row()
            end
        ui.end_column()
        ui.begin_column(4 * 64 + 3 * 8)
            ui.label("Bag", { bold = true, muted = true })
            for row = 0, 3 do
                ui.begin_row(8)
                for col = 1, 4 do
                    local i = row * 4 + col
                    item_slot("bag" .. i, { where = "bag", key = i })
                end
                ui.end_row()
            end
        ui.end_column()
    ui.end_row()
    ui.separator()
    ui.label(string.format("{#ffcc66}%d gold{/}     Drag items between slots. Right-click a potion to drink it.", game.gold or 0),
        { size = 15, muted = true })
    ui.end_panel()
end

-- How many of an item the party carries (bag and equipment)
local function count(id)
    local n = 0
    for i = 1, BAG do local e = game.bag[i]; if e and e.id == id then n = n + e.count end end
    for _, e in pairs(game.equip) do if e and e.id == id then n = n + e.count end end
    return n
end

-- Remove n of an item (bag first, then equipment). False (and nothing removed) if there aren't that many.
local function take(id, n)
    n = n or 1
    if count(id) < n then return false end
    local function from(get, set)
        local e = get()
        if not e or e.id ~= id or n <= 0 then return end
        local k = math.min(n, e.count)
        e.count = e.count - k
        n = n - k
        if e.count <= 0 then set(false) end
    end
    for i = 1, BAG do from(function() return game.bag[i] end, function(v) game.bag[i] = v end) end
    for key in pairs(game.equip) do from(function() return game.equip[key] end, function(v) game.equip[key] = v end) end
    return true
end

-- Items lying near the hero go into the bag (the pickup entity is removed)
local function collect()
    if not (hero and hero:valid()) then hero = scene.find("Hero") end
    if not hero then return end
    for _, p in ipairs(scene.pickups(hero.position, PICKUP_RANGE)) do
        local loot = p.pickup
        if give(loot.item, loot.count) then
            shared.notice = loot.count > 1 and string.format("Picked up %d %s", loot.count, loot.name) or ("Picked up: " .. loot.name)
            audio.play("Sounds/coin.wav")
            p:destroy()
        end
    end
end

function on_create(self)
    setup()
    -- Dialogue conditions and actions can use these (Lib/dialogue.lua): give("potion"), has_item("gem"), take("gem")
    dialogue.helpers.give = function(id, n) setup(); local ok = give(id, n); if ok then shared.notice = "You got: " .. item(id).name end; return ok end
    dialogue.helpers.take = function(id, n) setup(); return take(id, n) end
    dialogue.helpers.has_item = function(id, n) setup(); return count(id) >= (n or 1) end
end

function on_update(self, dt)
    setup()                                     -- a game loaded from an older save may not have one yet
    -- items given by other scripts
    if game.pending_loot then
        for _, id in ipairs(game.pending_loot) do
            if give(id) then shared.notice = "You got: " .. item(id).name end
        end
        game.pending_loot = nil
    end
    collect()
    if input.key_pressed(Key.I) or shared.toggle_inventory then
        open = not open
        shared.toggle_inventory = nil
    end
    shared.inventory_open = open
    if open then draw() end
end
