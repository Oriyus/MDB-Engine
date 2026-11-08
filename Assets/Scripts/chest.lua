-- A chest that opens when the hero comes close: the lid swings up and its gold goes into game.gold
-- (the campaign table: saved, and kept from level to level). It stays open: the lid's turn is part of
-- the level's saved state (a transform), and `opened` comes back through on_save / on_load.
-- Needs a child named "Lid Hinge" (turning it about X opens the lid). Named "Chest: <item id>", it also
-- gives that item (through game.pending_loot, which inventory.lua empties into the bag).

local gold = 25
local open_angle = -1.9
local opened = false
local hinge, hero
local angle = 0

function on_create(self)
    for _, c in ipairs(self:children()) do
        if c.name == "Lid Hinge" then hinge = c end
    end
    if hinge then angle = hinge.rotation.x end      -- a restored level brings the lid as it was
end

function on_save(self)
    return { opened = opened }
end

function on_load(self, data)
    opened = data.opened == true
end

function on_update(self, dt)
    if not (hero and hero:valid()) then hero = scene.find("Hero") end
    if hero and not opened and (hero.position - self.position):length() < 1.6 then   -- any neighbouring cell
        opened = true
        game.gold = (game.gold or 0) + gold
        shared.notice = "Found " .. gold .. " gold"     -- tactics.lua shows it
        local item = self.name:match("^Chest:%s*(%S+)")
        if item then
            game.pending_loot = game.pending_loot or {}
            table.insert(game.pending_loot, item)
        end
        audio.play("Sounds/coin.wav")
    end
    if hinge then
        local goal = opened and open_angle or 0
        angle = angle + (goal - angle) * math.min(1, dt * 4)
        hinge.rotation = vec3(angle, 0, 0)
    end
end
