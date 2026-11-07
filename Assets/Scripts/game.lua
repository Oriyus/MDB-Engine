-- Coin Rush rules and UI: a title screen, then collect every coin before the clock runs out and
-- reach the goal. Game state lives in `shared` so the other scripts can read it:
--   shared.state      "menu" | "playing" | "won" | "lost"
--   shared.time_left  seconds
--   shared.say(text, seconds)   show a message in the middle of the screen

local time_limit = 120
local total                      -- coins in the level (counted on the first frame)
local last_state                 -- to play the win / lose sound once
local message, message_time = nil, 0
local started_at = 0             -- scene.time() when Play was pressed

local white = vec3(1, 1, 1)
local grey = vec3(0.8, 0.8, 0.8)
local gold = vec3(1, 0.82, 0.3)
local red = vec3(1, 0.35, 0.3)
local dark = vec3(0.02, 0.03, 0.06)

function on_create(self)
    shared.state = "menu"
    shared.time_left = time_limit
    shared.say = function(text, seconds)
        message = text
        message_time = seconds or 2
    end
end

local function start()
    shared.state = "playing"
    started_at = scene.time()
    audio.play("Sounds/unlock.wav", { volume = 0.6 })
end

-- Two buttons side by side under the end-of-game title
local function end_buttons()
    if ui.button("Play again", 0.33, 0.56, 0.16, 0.075) then scene.restart() end
    if ui.button("Quit", 0.51, 0.56, 0.16, 0.075, { color = vec3(0.3, 0.12, 0.12) }) then scene.quit() end
    ui.text("or press R", 0.5, 0.66, { size = 22, align = "center", color = grey })
end

function on_update(self, dt)
    -- every coin registered itself in its on_create, which all ran before the first frame
    total = total or (shared.coins or 0)
    local left = shared.coins or 0

    -- ---- rules
    if shared.state == "menu" then
        if input.key_pressed(Key.Enter) or input.key_pressed(Key.Space) then start() end
    elseif shared.state == "playing" then
        shared.time_left = shared.time_left - dt
        if shared.time_left <= 0 then
            shared.time_left = 0
            shared.state = "lost"
            log("Time's up with", left, "coins left")
        end
    elseif input.key_pressed(Key.R) then
        scene.restart()
    end
    message_time = message_time - dt
    if shared.state ~= last_state then
        if shared.state == "won" then audio.play("Sounds/win.wav") end
        if shared.state == "lost" then audio.play("Sounds/lose.wav") end
        last_state = shared.state
    end

    -- ---- title screen
    if shared.state == "menu" then
        ui.rect(0, 0, 1, 1, { color = dark, alpha = 0.55 })
        ui.image("Textures/coin_icon.png", 0.455, 0.12, 0.09)
        ui.text("Coin Rush", 0.5, 0.3, { size = 96, align = "center", color = gold })
        ui.text(string.format("Collect all %d coins in %d minutes, then reach the goal", total, time_limit // 60),
            0.5, 0.45, { size = 26, align = "center", color = grey })
        if ui.button("Play", 0.41, 0.54, 0.18, 0.085, { size = 36, color = vec3(0.15, 0.45, 0.25) }) then start() end
        if ui.button("Quit", 0.44, 0.65, 0.12, 0.06, { size = 24, color = vec3(0.3, 0.12, 0.12) }) then scene.quit() end
        ui.text("Arrows roll  ·  Space jumps  ·  Enter starts", 0.5, 0.88, { size = 22, align = "center", color = grey })
        return
    end

    -- ---- HUD: coins top left, time top right with a bar under it
    ui.rect(0.015, 0.02, 0.21, 0.085, { color = dark, alpha = 0.45, rounding = 12 })
    ui.image("Textures/coin_icon.png", 0.025, 0.029, 0.04)
    ui.text(string.format("%d / %d", total - left, total), 0.075, 0.032, { size = 38, color = gold })

    local t = math.ceil(shared.time_left)
    local low = shared.time_left < 15
    ui.rect(0.8, 0.02, 0.185, 0.085, { color = dark, alpha = 0.45, rounding = 12 })
    ui.text(string.format("%d:%02d", t // 60, t % 60), 0.975, 0.024, { size = 34, align = "right", color = low and red or white })
    ui.bar(0.812, 0.083, 0.16, 0.012, shared.time_left / time_limit,
        { color = low and red or vec3(0.35, 0.8, 1.0), rounding = 4 })

    -- ---- end screens and messages
    if shared.state == "won" then
        ui.rect(0, 0, 1, 1, { color = dark, alpha = 0.35 })
        ui.text("You win!", 0.5, 0.3, { size = 84, align = "center", color = gold })
        ui.text(string.format("%d seconds to spare", math.floor(shared.time_left)), 0.5, 0.45, { size = 30, align = "center" })
        end_buttons()
    elseif shared.state == "lost" then
        ui.rect(0, 0, 1, 1, { color = dark, alpha = 0.35 })
        ui.text("Time's up!", 0.5, 0.3, { size = 84, align = "center", color = red })
        ui.text(string.format("%d coin%s left", left, left == 1 and "" or "s"), 0.5, 0.45, { size = 30, align = "center" })
        end_buttons()
    elseif message_time > 0 and message then
        ui.text(message, 0.5, 0.3, { size = 44, align = "center", alpha = math.min(1, message_time * 2) })
    end

    if shared.state == "playing" and scene.time() - started_at < 8 then
        ui.text("Arrows roll  ·  Space jumps", 0.5, 0.80, { size = 26, align = "center", color = grey })
        ui.text("Collect every coin, then reach the goal on the platform", 0.5, 0.85, { size = 26, align = "center", color = grey })
    end
end
