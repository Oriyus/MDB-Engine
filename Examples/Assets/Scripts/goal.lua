-- The finish: a trigger that ends the level once every coin is collected (see game.lua).
-- Grey while locked, pulsing green once open.

local done = false

function on_update(self, dt)
    local open = (shared.coins or 1) == 0
    if open then
        local pulse = 1 + 0.5 * math.sin(scene.time() * 4)
        self.mesh.emissive = vec3(0.2, 2.5, 0.6) * pulse
    else
        self.mesh.emissive = vec3(0.3, 0.3, 0.3)
    end
end

function on_trigger_enter(self, other)
    if done or other.name ~= "Player" or shared.state ~= "playing" then return end
    local left = shared.coins or 0
    if left > 0 then
        if shared.say then shared.say(left == 1 and "1 coin left!" or (left .. " coins left!"), 2) end
        return
    end
    done = true
    shared.state = "won"
    log("Level complete with", math.floor(shared.time_left), "seconds left")
end
