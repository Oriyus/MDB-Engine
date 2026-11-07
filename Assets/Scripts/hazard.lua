-- Lava: touching it sends the player back to the start (player.lua's shared.respawn).

function on_trigger_enter(self, other)
    if other.name ~= "Player" or not shared.respawn then return end
    if shared.state and shared.state ~= "playing" then return end
    audio.play_at("Sounds/lava_hit.wav", other.position)
    shared.respawn()
    if shared.say then shared.say("Ouch! Lava.", 1.5) end
    log("Player hit", self.name)
end

function on_update(self, dt)
    -- a slow, uneven glow
    local t = scene.time()
    local k = 0.8 + 0.2 * math.sin(t * 2.3 + self.position.x) + 0.1 * math.sin(t * 7.1)
    self.mesh.emissive = vec3(1.4, 0.12, 0.01) * k
end
