-- A pickup: put it on an entity with a trigger body. The Player collects it on contact.
-- `shared` is one table for all scripts, so every coin counts into the same score.

local collected = false

function on_create(self)
    shared.coins = (shared.coins or 0) + 1
end

function on_trigger_enter(self, other)
    if collected or other.name ~= "Player" then return end
    collected = true
    shared.coins = shared.coins - 1
    shared.score = (shared.score or 0) + 1
    log("Coin! score", shared.score, "- left", shared.coins)
    audio.play_at("Sounds/coin.wav", self.position)
    if shared.coins == 0 then
        log("All coins collected!")
        if shared.say then shared.say("All coins! Now reach the goal", 3) end
        audio.play("Sounds/unlock.wav")
    end
    self:destroy()
end
