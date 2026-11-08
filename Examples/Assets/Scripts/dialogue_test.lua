-- Checks the dialogue runner (Scripts/Lib/dialogue.lua) on the Sage's dialogue (Dialogues/sage.json).
-- Run unattended: MDB_SCENE=tactics_test.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/dialogue_test.lua
-- Prints "dialogue_test: PASS ..." / "FAIL ..." lines and "dialogue_test: done".

local frame = 0
local heard = {}                     -- event name -> how many times
local last = {}                      -- event name -> its data

local function check(name, ok, detail)
    log("dialogue_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end

function on_event(self, name, data)
    heard[name] = (heard[name] or 0) + 1
    last[name] = data
end

local function option(talk, text)    -- the index of the option whose text starts with `text`
    for i, o in ipairs(talk.options) do
        if o.text:sub(1, #text) == text then return i, o end
    end
end

local sage, talk

local steps = {
    function()
        sage = scene.find("Sage")
        check("the Sage is here", sage ~= nil)
        check("inventory helpers registered", dialogue.helpers.give ~= nil and dialogue.helpers.has_item ~= nil)
        game.met_sage = nil
        talk = dialogue.open("sage", { speaker = sage })
        check("dialogue.open", talk ~= nil and talk.active)
        check("first meeting: branch -> action -> greeting", talk.text:find("Well met") ~= nil, talk.text)
        check("the action ran", game.met_sage == true)
        check("speaker defaults to the entity's name", talk.speaker == "Sage", talk.speaker)
        check("a line without choices continues", #talk.options == 0)
        talk:continue()
        check("the hub has five options", #talk.options == 5, #talk.options)
    end,
    function()
        check("dialogue_started event", heard.dialogue_started == 1)
        game.gold = 10
        talk:choose((option(talk, "Do you sell")))
        check("{=expr} in text", talk.text:find("You have {#ffcc66}10{/}", 1, true) ~= nil, talk.text)
        local i, buy = option(talk, "Buy a potion")
        check("a false condition shows greyed out", buy ~= nil and not buy.enabled)
        check("with its reason", buy and buy.why == "You need 25 gold. Chests have some.")
        talk:choose(i)
        check("a disabled choice can't be picked", talk.text:find("You have") ~= nil and game.gold == 10)
        game.gold = 30
        talk:choose((option(talk, "Not now")))
        talk:choose((option(talk, "Do you sell")))
        local potions = 0
        for _, e in ipairs(game.bag) do if e and e.id == "potion" then potions = e.count end end
        local j, buy2 = option(talk, "Buy a potion")
        check("enough gold: enabled", buy2 ~= nil and buy2.enabled)
        talk:choose(j)
        local after = 0
        for _, e in ipairs(game.bag) do if e and e.id == "potion" then after = e.count end end
        check("the choice's action paid", game.gold == 5, game.gold)
        check("and gave a potion (inventory helper)", after == potions + 1, potions .. " -> " .. after)
        check("then the thank-you line", talk.text:find("Here you are") ~= nil, talk.text)
        talk:continue()
        check("which continues back to the shop", talk.text:find("You have {#ffcc66}5{/}", 1, true) ~= nil, talk.text)
        talk:choose((option(talk, "Not now")))
        talk:choose((option(talk, "What's down")))
        check("the line's action ran", game.quest_cellar == "started")
        talk:choose(1)                                      -- back to the hub
        talk:choose((option(talk, "Can you show")))
        check("an action node passes on to its line", talk.text:find("Watch closely") ~= nil, talk.text)
    end,
    function()
        check("quest_started event delivered", heard.quest_started == 1)
        check("its data: dialogue and node", last.quest_started and last.quest_started.dialogue == "sage" and last.quest_started.node == 6)
        check("magic event with the speaker", heard.magic == 1 and last.magic.speaker == sage)
        check("npc.lua answered it with sparkles", #scene.find_all("Magic burst") == 1, #scene.find_all("Magic burst"))
        talk:continue()
        talk:choose((option(talk, "Farewell")))
        check("farewell line", talk.text:find("Safe travels") ~= nil)
        talk:continue()
        check("next = 0 ends it", not talk.active)
        check("update() says it is over", talk:update() == false)
    end,
    function()
        check("dialogue_ended event", heard.dialogue_ended == 1)
        talk = dialogue.open("sage", { speaker = sage })
        check("second meeting: the other branch", talk.text:find("Back again") ~= nil, talk.text)
        talk:close()
        check("a missing file gives nil", dialogue.open("no_such_dialogue") == nil)
        -- a broken condition is logged and counts as false; the conversation goes on
        local broken = dialogue.open("sage", { speaker = sage })
        broken.nodes[1].cond = "game.met_sage and and"
        broken.compiled = {}
        broken:go(1)
        check("a broken condition doesn't stop it", broken.active and broken.text:find("Well met") ~= nil)
        broken:close()
        log("dialogue_test: done")
    end,
}

function on_update(self, dt)
    frame = frame + 1
    if talk and talk.active then talk:update() end         -- the default box, drawn while the test runs
    local i = frame - 4
    if i >= 1 and steps[i] then steps[i]() end
end
