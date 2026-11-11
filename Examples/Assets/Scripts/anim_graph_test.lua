-- Checks animation graphs (Animation/AnimGraph.h) on AnimGraphs/fox.json: states, a 1D blend, conditions, triggers,
-- Any State, exit time, cross-fades, play_state and an additive layer weighted by a parameter.
-- Run unattended (any scene): MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/anim_graph_test.lua
-- Spawns its own fox. Prints "anim_graph_test: PASS ..." / "FAIL ..." lines and a summary.

local passed, failed = 0, 0
local fox, an
local t, stage, stage_t = 0, 0, 0
local mark = {}

local function check(name, ok, detail)
    if ok then passed = passed + 1; log("anim_graph_test: PASS " .. name .. (detail and ("  (" .. detail .. ")") or ""))
    else failed = failed + 1; log("anim_graph_test: FAIL " .. name .. (detail and ("  (" .. detail .. ")") or "")) end
end

local function next_stage() stage = stage + 1; stage_t = 0 end

function on_create(self)
    fox = scene.spawn("Graph Fox", vec3(2, 0, -2))
    fox:add_mesh{ geometry = "model:Models/Fox.glb", fit = 1.6 }
    fox:add_animator{ graph = "AnimGraphs/fox.json" }
    an = fox.animator
end

function on_update(self, dt)
    t = t + dt
    stage_t = stage_t + dt
    if stage == 0 and stage_t > 0.2 then
        check("graph loaded", an.graph == "AnimGraphs/fox.json")
        check("starts in the entry state", an:state() == "Idle", an:state())
        check("second layer runs", an:state(2) == "Look about", an:state(2))
        check("parameter default", an:get("speed") == 0)
        check("unknown parameter is an error", not pcall(function() an:set("nope", 1) end))
        an:set("speed", 2.0)
        next_stage()
    elseif stage == 1 and stage_t > 0.05 then
        check("condition: speed > 0.2 -> Locomotion", an:state() == "Locomotion", an:state())
        check("cross-fading", an:in_transition())
        next_stage()
    elseif stage == 2 and stage_t > 0.5 then
        check("cross-fade done", not an:in_transition())
        -- the blend runs slower towards the run end (a run cycle is longer than a walk's): compare phase rates
        mark.p0 = an:phase()
        next_stage()
    elseif stage == 3 and stage_t > 0.1 then
        local walk_rate = (an:phase() - mark.p0) / stage_t
        mark.walk_rate = walk_rate
        an:set("speed", 3.6)
        mark.p1 = an:phase()
        next_stage()
    elseif stage == 4 and stage_t > 0.1 then
        local run_rate = (an:phase() - mark.p1) / stage_t
        check("blend changes the cycle", math.abs(run_rate - mark.walk_rate) > 0.01 and run_rate > 0 and mark.walk_rate > 0,
            string.format("walk %.2f, run %.2f cycles/s", mark.walk_rate, run_rate))
        an:set("speed", 0)
        next_stage()
    elseif stage == 5 and stage_t > 0.5 then
        check("condition: speed < 0.15 -> Idle", an:state() == "Idle", an:state())
        an:trigger("look_around")
        next_stage()
    elseif stage == 6 and stage_t > 0.05 then
        check("trigger from Any State -> Look", an:state() == "Look", an:state())
        check("trigger used up", an:get("look_around") == 0)
        next_stage()
    elseif stage == 7 and an:state() ~= "Look" then
        -- Look is a one-shot (Survey at 1.5x): exit time 1 takes it back to Idle when it ends
        local expected = an:duration("Survey") / 1.5
        check("exit time: back to Idle at the end", an:state() == "Idle" and math.abs(stage_t - expected) < 0.25,
            string.format("%s after %.2f s, clip %.2f s", an:state(), stage_t, expected))
        an:play_state("Locomotion", 1, 0)
        next_stage()
    elseif stage == 8 and stage_t > 0.05 then
        -- play_state forces it, but speed is 0, so the graph's own transition takes it back to Idle
        check("transitions still apply after play_state", an:state() == "Idle" or an:in_transition(), an:state())
        check("play_state of a missing state is an error", not pcall(function() an:play_state("Nope") end))
        -- the additive head layer: weight 0 changes nothing, 1 moves the neck
        an.playing = false
        an:set("head", 0)
        next_stage()
    elseif stage == 9 and stage_t > 0.1 then
        mark.neck0 = an:bone_position("b_Head_05") or an:bone_position("b_Neck_04")
        an:set("head", 1)
        next_stage()
    elseif stage == 10 and stage_t > 0.1 then
        local neck1 = an:bone_position("b_Head_05") or an:bone_position("b_Neck_04")
        local moved = (mark.neck0 and neck1) and (neck1 - mark.neck0):length() or -1
        check("layer weight parameter fades the head layer", moved > 0.001, string.format("head moved %.3f m", moved))
        an.graph = ""
        check("back to plain clips", an:state() == "" and an.graph == "")
        log(string.format("anim_graph_test: %d passed, %d failed", passed, failed))
        next_stage()
    end
end
