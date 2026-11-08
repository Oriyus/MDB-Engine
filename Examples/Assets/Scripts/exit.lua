-- A pad that takes the hero to another level. The entity's name says where:
--   "Exit: <scene file> > <entry>"     e.g. "Exit: tactics_cellar.json > From Yard"
-- The level being left is kept as it is (coming back finds it the same), and the new level puts the
-- hero at its "Entry: <entry>" entity (tactics.lua). The pad works once the hero has stepped away from
-- it, so arriving next to it doesn't bounce you straight back, and not during combat.

local target, entry
local armed = false
local hero

function on_create(self)
    target, entry = self.name:match("^Exit:%s*(.-)%s*>%s*(.-)%s*$")
    if not target then log("exit.lua: name the entity 'Exit: <scene file> > <entry>'") end
end

function on_update(self, dt)
    if not target then return end
    if not (hero and hero:valid()) then hero = scene.find("Hero") end
    if not hero then return end
    local d = hero.position - self.position
    local dist = vec3(d.x, 0, d.z):length()
    if dist > 1.6 then armed = true end
    if armed and dist < 0.6 and shared.mode ~= "combat" then
        armed = false
        log("exit.lua: to " .. target .. " (" .. entry .. ")")
        level.change(target, entry)
    end
end
