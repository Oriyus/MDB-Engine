-- The dialogue runner: plays dialogue trees made in the editor's Dialogue Editor (Assets/Dialogues/*.json).
-- Loaded for every script (Scripts/Lib runs in the shared globals).
--
--   local talk = dialogue.open("sage", { speaker = self })      -- Dialogues/sage.json
--   function on_update(self, dt)
--       if talk and not talk:update() then talk = nil end      -- draws the default box, takes clicks and keys
--   end
--
-- Or draw it yourself: talk.speaker, talk.text, talk.options ({ text, enabled, why }), talk:choose(i),
-- talk:continue() (a line without choices), talk:close(), talk.active, talk.node (the current line).
-- dialogue.open options: speaker (the entity talking: `speaker` in code, its name as the default speaker),
-- start (a node id), on_end(talk), width, frame (an image, or false for a plain panel).
--
-- Node types (the file format is described in Editor/src/DialogueEditor.h):
--   line    speaker + text, then its choices, or `next` when it has none (Continue / End)
--   branch  `cond` true -> next, false -> else
--   action  runs `action`, sends `event`, goes to next
-- A node's or choice's next = 0 (or missing) ends the conversation.
-- cond is a Lua expression, action Lua statements. They see the globals (game, shared, scene, audio,
-- events, level...), `speaker`, and every function in dialogue.helpers (inventory.lua adds give, take,
-- has_item). event is a name sent with events.emit(name, { dialogue, node, speaker }) to every script's
-- on_event. Text takes ui.label markup, and {=expr} is replaced by the expression's value.
-- The runner itself sends "dialogue_started" and "dialogue_ended" ({ dialogue, speaker }).

dialogue = dialogue or {}
dialogue.helpers = dialogue.helpers or {}

local Talk = {}
Talk.__index = Talk

local function report(talk, where, err)
    log("dialogue " .. talk.name .. " (" .. where .. "): " .. tostring(err))
end

-- cond / action / {=expr} code, compiled once per conversation
local function compile(talk, code, expression, where)
    local key = (expression and "e:" or "s:") .. code
    local fn = talk.compiled[key]
    if fn == nil then
        local err
        fn, err = load(expression and ("return " .. code) or code, "=" .. talk.name .. " " .. where, "t", talk.env)
        if not fn then report(talk, where, err); fn = false end
        talk.compiled[key] = fn
    end
    return fn
end

local function test(talk, code, where)          -- an empty condition is true
    if code == nil or code == "" then return true end
    local fn = compile(talk, code, true, where)
    if not fn then return false end
    local ok, value = pcall(fn)
    if not ok then report(talk, where, value) return false end
    return value and true or false
end

local function run(talk, code, where)
    if code == nil or code == "" then return end
    local fn = compile(talk, code, false, where)
    if not fn then return end
    local ok, err = pcall(fn)
    if not ok then report(talk, where, err) end
end

local function fire(talk, name, node)
    if name == nil or name == "" then return end
    events.emit(name, { dialogue = talk.name, node = node and node.id, speaker = talk.speaker_entity })
end

local function interpolate(talk, text, where)
    return (text:gsub("{=(.-)}", function(code)
        local fn = compile(talk, code, true, where)
        if not fn then return "?" end
        local ok, value = pcall(fn)
        if not ok then report(talk, where, value) return "?" end
        return tostring(value)
    end))
end

-- Follow branches and actions from `id` to the next line (or the end)
function Talk:go(id)
    for _ = 1, 100 do
        local node = id and self.nodes[id]
        if not node then self:close() return end
        local where = "node " .. node.id
        if node.type == "branch" then
            if test(self, node.cond, where) then id = node.next else id = node["else"] end
        elseif node.type == "action" then
            run(self, node.action, where)
            fire(self, node.event, node)
            id = node.next
        else
            run(self, node.action, where)
            fire(self, node.event, node)
            self.node = node
            self.speaker = (node.speaker and node.speaker ~= "") and node.speaker or self.default_speaker
            self.text = interpolate(self, node.text or "", where)
            self.options = {}
            for i, c in ipairs(node.choices or {}) do
                local ok = test(self, c.cond, where .. " choice " .. i)
                if ok or c.show == "disabled" then
                    table.insert(self.options, { text = interpolate(self, c.text or "", where), enabled = ok,
                                                 why = c.why, choice = c })
                end
            end
            return
        end
    end
    report(self, "go", "more than 100 branch / action nodes in a row (a loop?)")
    self:close()
end

function Talk:choose(i)
    local opt = self.active and self.options[i]
    if not opt or not opt.enabled then return end
    local c = opt.choice
    run(self, c.action, "node " .. self.node.id .. " choice " .. i)
    fire(self, c.event, self.node)
    self:go(c.next)
end

function Talk:continue()
    if self.active and #self.options == 0 then self:go(self.node.next) end
end

function Talk:close()
    if not self.active then return end
    self.active = false
    if dialogue.current == self then dialogue.current = nil end
    events.emit("dialogue_ended", { dialogue = self.name, speaker = self.speaker_entity })
    if self.on_end then self.on_end(self) end
end

-- The default dialogue box (bottom centre). Number keys pick choices, Space / Enter continue.
-- Returns false once the conversation is over.
function Talk:update()
    if not self.active then return false end
    local o = self.opts
    local frame = o.frame == nil and "Textures/panel_frame.png" or (o.frame or "")
    ui.begin_panel("dialogue:" .. self.name, { x = 0.5, y = 0.97, anchor = "bottom", w = o.width or 660,
        title = self.speaker, image = frame, slice = frame ~= "" and 20 or nil, padding = frame ~= "" and 26 or nil })
    local long = #self.text > 320
    if long then ui.begin_scroll("text", 150) end
    ui.label(self.text, { size = 19 })
    if long then ui.end_scroll() end
    ui.space(4)
    local pick
    if #self.options == 0 then
        local more = self.node.next ~= nil and self.node.next ~= 0
        if ui.button(more and "Continue" or "End", { fill = true, id = "continue" })
            or input.key_pressed(Key.Space) or input.key_pressed(Key.Enter) then pick = 0 end
    else
        for i, opt in ipairs(self.options) do
            local why = (not opt.enabled and opt.why ~= nil and opt.why ~= "") and opt.why or nil
            if ui.button(i .. ".  " .. opt.text, { fill = true, disabled = not opt.enabled, id = "choice" .. i, tooltip = why }) then
                pick = i
            end
            if opt.enabled and i <= 9 and input.key_pressed(Key["Num" .. i]) then pick = i end
        end
    end
    ui.end_panel()
    if pick == 0 then self:continue() elseif pick then self:choose(pick) end
    return self.active
end

-- Start a conversation. name: "sage" (Dialogues/sage.json) or a path under Assets. Nil if it can't be read.
-- The file is read each time, so a dialogue saved in the editor during Play is used by the next conversation.
function dialogue.open(name, opts)
    opts = opts or {}
    local path = name:find("/") and name or ("Dialogues/" .. name .. (name:find("%.json$") and "" or ".json"))
    local doc, err = data.load(path)
    if not doc then log("dialogue.open: " .. tostring(err)) return nil end
    local talk = setmetatable({
        name = name, nodes = {}, compiled = {}, active = true, opts = opts,
        speaker_entity = opts.speaker, default_speaker = opts.speaker and opts.speaker.name or "",
        on_end = opts.on_end, options = {}, text = "", speaker = "",
    }, Talk)
    for _, n in ipairs(doc.nodes or {}) do talk.nodes[n.id] = n end
    talk.env = setmetatable({ speaker = opts.speaker, dialogue = dialogue }, {
        __index = function(_, k)
            local h = dialogue.helpers[k]
            if h ~= nil then return h end
            return _G[k]
        end,
        __newindex = _G,
    })
    dialogue.current = talk
    events.emit("dialogue_started", { dialogue = name, speaker = opts.speaker })
    talk:go(opts.start or doc.start)
    return talk
end
