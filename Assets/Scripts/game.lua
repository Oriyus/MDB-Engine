-- Coin Rush rules and HUD: collect every coin before the clock runs out, then reach the goal.
-- Game state lives in `shared` so the other scripts can read it:
--   shared.state      "playing" | "won" | "lost"
--   shared.time_left  seconds
--   shared.say(text, seconds)   show a message in the middle of the screen

local time_limit = 120
local total                      -- coins in the level (counted on the first frame)
local message, message_time = nil, 0

local white = vec3(1, 1, 1)
local gold = vec3(1, 0.82, 0.3)
local red = vec3(1, 0.35, 0.3)

function on_create(self)
    shared.state = "playing"
    shared.time_left = time_limit
    shared.say = function(text, seconds)
        message = text
        message_time = seconds or 2
    end
end

function on_update(self, dt)
    -- every coin registered itself in its on_create, which all ran before the first frame
    total = total or (shared.coins or 0)
    local left = shared.coins or 0

    if shared.state == "playing" then
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

    -- ---- HUD
    ui.text(string.format("Coins  %d / %d", total - left, total), 0.03, 0.03, { size = 36, color = gold })
    local t = math.ceil(shared.time_left)
    ui.text(string.format("%d:%02d", t // 60, t % 60), 0.97, 0.03,
        { size = 36, align = "right", color = shared.time_left < 15 and red or white })

    if shared.state == "won" then
        ui.text("You win!", 0.5, 0.32, { size = 84, align = "center", color = gold })
        ui.text(string.format("%d seconds to spare  -  press R to play again", math.floor(shared.time_left)),
            0.5, 0.47, { size = 30, align = "center" })
    elseif shared.state == "lost" then
        ui.text("Time's up!", 0.5, 0.32, { size = 84, align = "center", color = red })
        ui.text("Press R to try again", 0.5, 0.47, { size = 30, align = "center" })
    elseif message_time > 0 and message then
        ui.text(message, 0.5, 0.3, { size = 44, align = "center" })
    end

    if scene.time() < 8 and shared.state == "playing" then
        local grey = vec3(0.85, 0.85, 0.85)
        ui.text("Arrows roll  ·  Space jumps", 0.5, 0.80, { size = 26, align = "center", color = grey })
        ui.text("Collect every coin, then reach the goal on the platform", 0.5, 0.85,
            { size = 26, align = "center", color = grey })
    end
end
