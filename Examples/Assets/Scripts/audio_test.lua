-- Checks the audio mixer buses: the volume / mute API, clamping, bad names, sources and one-shots on their bus,
-- and moving a playing source to another bus.
-- Run unattended: MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/audio_test.lua
-- (needs an audio device for the voice counts). The editor puts back its own mix on Stop.
-- Prints "audio_test: PASS ..." / "FAIL ..." lines and a summary.

local frame = 0
local passed, failed = 0, 0
local src

local function check(name, ok, detail)
    if ok then passed = passed + 1; log("audio_test: PASS " .. name .. (detail and ("  (" .. detail .. ")") or ""))
    else failed = failed + 1; log("audio_test: FAIL " .. name .. (detail and ("  (" .. detail .. ")") or "")) end
end

local function near(a, b) return math.abs(a - b) < 1e-4 end

function on_update(self, dt)
    frame = frame + 1
    if frame == 2 then
        local buses = audio.buses()
        check("five buses", #buses == 5 and buses[1] == "sfx" and buses[2] == "music", table.concat(buses, ","))

        audio.set_volume("music", 0.4)
        check("bus volume", near(audio.volume("music"), 0.4), tostring(audio.volume("music")))
        audio.set_volume("sfx", 2.5)
        check("volume clamps to 1", near(audio.volume("sfx"), 1.0), tostring(audio.volume("sfx")))
        audio.set_volume("voice", -1)
        check("volume clamps to 0", near(audio.volume("voice"), 0.0), tostring(audio.volume("voice")))
        audio.set_volume("master", 0.8)
        check("master volume", near(audio.volume("master"), 0.8))
        audio.set_muted("ambience", true)
        check("mute", audio.muted("ambience") == true and audio.muted("music") == false)
        audio.set_muted("ambience", false)
        check("unmute", audio.muted("ambience") == false)
        audio.set_muted("master", true)
        check("master mute", audio.muted("master") == true)
        audio.set_muted("master", false)

        local ok, err = pcall(audio.set_volume, "drums", 0.5)
        check("unknown bus is an error", not ok and tostring(err):find("unknown audio bus") ~= nil, tostring(err))
        ok = pcall(audio.play, "Sounds/coin.wav", { bus = "nope" })
        check("unknown bus in play is an error", not ok)

        src = scene.spawn("Audio test source", vec3(0, 1, 0))
        src:add_audio("Sounds/lava_loop.wav", { bus = "ambience", spatial = false, volume = 0.05 })
        check("add_audio bus", src.audio.bus == "ambience", src.audio.bus)
        audio.play("Sounds/coin.wav", { bus = "ui", volume = 0.05 })
        audio.play("Sounds/coin.wav", { volume = 0.05 })
    end
    if frame == 4 then
        check("source on its bus", audio.voices("ambience") == 1, tostring(audio.voices("ambience")))
        check("one-shot on the ui bus", audio.voices("ui") >= 1, tostring(audio.voices("ui")))
        check("default bus is sfx", audio.voices("sfx") >= 1, tostring(audio.voices("sfx")))
        check("source playing", src.audio.playing)
        src.audio.bus = "music"
    end
    if frame == 6 then
        check("moved to another bus", audio.voices("music") == 1 and audio.voices("ambience") == 0,
              string.format("music %d, ambience %d", audio.voices("music"), audio.voices("ambience")))
        check("still playing after the move", src.audio.playing)
        check("bus reads back", src.audio.bus == "music")
        log(string.format("audio_test: %d passed, %d failed", passed, failed))
    end
end
