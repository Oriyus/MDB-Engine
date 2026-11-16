-- Checks shader graph materials (Renderer/ShaderGraph.h, Assets/Shaders/*.shadergraph): the example graphs compile and
-- draw, materials read their graph and params, material.set_param, errors, and a save game round trip of a graph slot.
-- Run unattended (the log must also have no "failed to compile" line):
--   MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/shader_graph_test.lua MDB_SAVE_DIR=<folder>
-- Prints "shader_graph_test: PASS ..." / "FAIL ..." lines and a summary.

local phase = 1
local frame = 0
local passed, failed = 0, 0
local MATS = { "Materials/lava.mat", "Materials/rim_glow.mat", "Materials/dissolve.mat", "Materials/triplanar_rock.mat", "Materials/wobble.mat", "Materials/water.mat" }

local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("shader_graph_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end
local function near(a, b) return math.abs(a - b) < 1e-4 end

function on_save(self) return { phase = phase, passed = passed, failed = failed } end
function on_load(self, data) phase, passed, failed = data.phase, data.passed, data.failed; frame = 0 end

function on_update(self, dt)
    frame = frame + 1
    if phase == 1 and frame == 2 then
        for _, m in ipairs(MATS) do
            local v = material.get(m)
            check(m .. " uses a graph and compiles", v.shader:sub(-12) == ".shadergraph" and v.error == "", v.error)
        end
        local d = material.get("Materials/dissolve.mat")
        check("render state stays the material's", d.alphaMode == "cutout" and d.doubleSided == true)

        -- Draw them (the programs compile on first use: a failure shows in the log and draws magenta)
        local base = vec3(0, 60, 400)
        for i, m in ipairs(MATS) do
            scene.spawn("Graph ball " .. i, base + vec3(i * 1.5, 0, 0)):add_mesh{ geometry = "sphere", material = m }
        end
        check("a graph material on a part", scene.find("Graph ball 1").mesh.material == "Materials/lava.mat")

        -- Params
        material.set_param("Materials/lava.mat", "Glow", 2.5)
        material.set_param("Materials/lava.mat", "Hot", vec3(0.2, 0.9, 0.3))
        local p = material.get("Materials/lava.mat").params
        check("set_param: a number", p and near(p.Glow, 2.5), p and p.Glow)
        check("set_param: a colour", p and p.Hot and near(p.Hot[2], 0.9))
        check("set_param on a standard material is an error", not pcall(material.set_param, "Materials/gold.mat", "x", 1))
        check("set_param: a table of tables is an error", not pcall(material.set_param, "Materials/lava.mat", "Glow", { a = { 1 } }))
        material.set_param("Materials/triplanar_rock.mat", "Rock", "Textures/Terrain/sand.jpg")
        check("set_param: a texture", material.get("Materials/triplanar_rock.mat").params.Rock == "Textures/Terrain/sand.jpg")
        -- Per-object overrides still tint a graph material (the object's colour multiplies the graph's)
        local b = scene.find("Graph ball 2")
        b.mesh.color = vec3(1, 0, 0)
        check("an override on a graph part", near(b.mesh.color.x, 1) and near(b.mesh.color.y, 0))
    elseif phase == 1 and frame == 5 then
        local s = scene.render_stats()
        check("drawn", s.draw_calls > 0, s.draw_calls)
        phase = 2
        saves.save("shader_graph_test")
        saves.load("shader_graph_test")
    elseif phase == 2 and frame == 3 then
        local b = scene.find("Graph ball 3")
        check("a saved graph slot loads back", b and b.mesh.material == "Materials/dissolve.mat", b and b.mesh.material)
        local o = scene.find("Graph ball 2")
        check("the override is saved too", o and near(o.mesh.color.x, 1) and near(o.mesh.color.y, 0))
        saves.delete("shader_graph_test")
        log(string.format("shader_graph_test: %d passed, %d failed", passed, failed))
        phase = 3
    end
end
