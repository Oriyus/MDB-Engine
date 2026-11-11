-- Someone to talk to. Walk next to it and press T (or the toolbar's Talk button, which sets shared.talk_to
-- to this entity's name). It plays the dialogue of its NPC entry in the game database (Window > Database),
-- else Dialogues/<its name in lower case>.json: an entity named "Sage" plays Dialogues/sage.json. Make and
-- edit those in the editor's Dialogue Editor (Window > Dialogue Editor, or
-- double-click the file in the Assets panel). Walking away or a fight ends the conversation.
--
-- Events from the dialogue reach every script's on_event. This one answers "magic" (sent by the Sage's
-- dialogue) with a burst of sparkles above its head, when the event came from its own conversation.

local talk_range = 2.2
local talk, hero

-- Its dialogue: the one its database entry names (an NPC of Data/database.json), else its name
local function dialogue_name(self)
    local data = self.npc and self.npc.data
    local path = data and data.dialogue
    local name = path and path:match("Dialogues/(.+)%.json$")
    return name or self.name:lower()
end

function on_event(self, name, data)
    if name == "magic" and data and data.speaker == self then
        scene.spawn_particles(self.position + vec3(0, 2.1, 0), { preset = "Magic", count = 60 })
        audio.play_at("Sounds/unlock.wav", self.position)
    end
end

function on_update(self, dt)
    if not (hero and hero:valid()) then hero = scene.find("Hero") end
    if not hero then return end
    local d = hero.position - self.position
    local near = vec3(d.x, 0, d.z):length() < talk_range

    -- let the HUD offer a Talk button (several talkers: the nearest one that says so wins)
    if near then shared.can_talk = self.name elseif shared.can_talk == self.name then shared.can_talk = nil end
    if near and not talk and shared.mode ~= "combat" and (input.key_pressed(Key.T) or shared.talk_to == self.name) then
        talk = dialogue.open(dialogue_name(self), { speaker = self })
    end
    if shared.talk_to == self.name then shared.talk_to = nil end
    if talk and (not near or shared.mode == "combat") then talk:close() end

    if talk then
        self.rotation = vec3(0, math.atan(d.x, d.z), 0)      -- face the hero
        if not talk:update() then talk = nil end
    end
    if talk then shared.talking = self.name elseif shared.talking == self.name then shared.talking = nil end
end
