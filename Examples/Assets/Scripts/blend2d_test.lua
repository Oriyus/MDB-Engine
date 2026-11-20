-- Checks 2D blend spaces (Animation/AnimGraph.h, AnimBlend2DWeights) on AnimGraphs/fox_2d.json: Survey at (0, 0),
-- Walk at (1, 0), Run at (0, 1), weighed Directional (state "Move") and Cartesian (state "Grid").
-- Run unattended (any scene): MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/blend2d_test.lua
-- Spawns its own fox. Prints "blend2d_test: PASS ..." / "FAIL ..." lines and a summary.

local passed, failed = 0, 0
local fox, an
local stage, stage_t = 0, 0
local mark = {}

local function check(name, ok, detail)
    if ok then passed = passed + 1; log("blend2d_test: PASS " .. name .. (detail and ("  (" .. detail .. ")") or ""))
    else failed = failed + 1; log("blend2d_test: FAIL " .. name .. (detail and ("  (" .. detail .. ")") or "")) end
end

local function next_stage() stage = stage + 1; stage_t = 0 end

local function weights_at(x, y)
    an:set("x", x); an:set("y", y)
    local w = an:blend_weights()
    return w.Survey or 0, w.Walk or 0, w.Run or 0
end
local function fmt(s, w, r) return string.format("survey %.3f, walk %.3f, run %.3f", s, w, r) end
local function near(a, b, tol) return math.abs(a - b) <= (tol or 0.01) end

-- weights on a grid of values from (lo, lo) to (2, 2): they sum to 1, none negative, and change smoothly (the
-- directional sweep stays in the covered quadrant: the test graph has no points behind or to the left, see Issues)
local function sweep(lo)
    local ok_sum, jump = true, 0
    local prev
    for i = 0, 40 do
        local x = lo + i * (2 - lo) / 40
        for j = 0, 40 do
            local y = lo + j * (2 - lo) / 40
            local s, w, r = weights_at(x, y)
            if math.abs(s + w + r - 1) > 1e-3 or s < 0 or w < 0 or r < 0 then ok_sum = false end
            if prev and j > 0 then jump = math.max(jump, math.abs(s - prev[1]), math.abs(w - prev[2]), math.abs(r - prev[3])) end
            prev = { s, w, r }
        end
        prev = nil
    end
    return ok_sum, jump
end

function on_create(self)
    fox = scene.spawn("Blend Fox", vec3(-2, 0, -2))
    fox:add_mesh{ geometry = "model:Models/Fox.glb", fit = 1.6 }
    fox:add_animator{ graph = "AnimGraphs/fox_2d.json" }
    an = fox.animator
end

function on_update(self, dt)
    stage_t = stage_t + dt
    if stage == 0 and stage_t > 0.2 then
        check("graph loaded, 2D state entered", an:state() == "Move", an:state())

        local s, w, r = weights_at(0, 0)
        check("directional: origin plays the middle clip alone", near(s, 1) and near(w, 0) and near(r, 0), fmt(s, w, r))
        s, w, r = weights_at(1, 0)
        check("directional: on a point plays it alone", near(w, 1) and near(s, 0) and near(r, 0), fmt(s, w, r))
        s, w, r = weights_at(0.5, 0)
        check("directional: half way out = half idle, half walk", near(s, 0.5) and near(w, 0.5) and near(r, 0), fmt(s, w, r))
        s, w, r = weights_at(0.7071, 0.7071)
        check("directional: between two directions keeps the speed (no idle)", near(s, 0) and near(w, 0.5) and near(r, 0.5), fmt(s, w, r))
        s, w, r = weights_at(0, 3)
        check("directional: beyond the outermost point it plays alone", near(r, 1), fmt(s, w, r))
        local ok, jump = sweep(0)
        check("directional: weights sum to 1 everywhere", ok)
        check("directional: weights change smoothly", jump < 0.3, string.format("largest step %.3f", jump))

        -- the phase runs at the weighted cycle length (Walk and Run kept in step)
        weights_at(0.7071, 0.7071)
        mark.expected = 1 / (0.5 * an:duration("Walk") + 0.5 * an:duration("Run"))
        mark.p0 = an:phase()
        next_stage()
    elseif stage == 1 and stage_t >= 0.3 then
        local d = an:phase() - mark.p0
        if d < 0 then d = d + 1 end
        local rate = d / stage_t
        check("phase follows the blended cycle", math.abs(rate - mark.expected) / mark.expected < 0.12,
            string.format("%.3f cycles/s, expected %.3f", rate, mark.expected))
        an:trigger("cartesian")
        next_stage()
    elseif stage == 2 and stage_t > 0.05 then
        check("cartesian state entered", an:state() == "Grid", an:state())
        local s, w, r = weights_at(0, 0)
        check("cartesian: on a point plays it alone", near(s, 1), fmt(s, w, r))
        s, w, r = weights_at(0.7071, 0.7071)
        -- gradient bands in plain X / Y: idle keeps 0.293 / 1.293 there (worked by hand)
        check("cartesian: idle shares the diagonal", near(s, 0.2265, 0.01) and near(w, r, 1e-4), fmt(s, w, r))
        local ok, jump = sweep(-2)
        check("cartesian: weights sum to 1 everywhere", ok)
        check("cartesian: weights change smoothly", jump < 0.3, string.format("largest step %.3f", jump))
        an.graph = ""
        check("no graph: no weights", next(an:blend_weights()) == nil)
        log(string.format("blend2d_test: %d passed, %d failed", passed, failed))
        next_stage()
    end
end
