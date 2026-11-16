-- Checks material files (Renderer/MaterialAsset.h, Assets/Materials/*.mat): reading them and their parents, mesh parts
-- using them from scripts (add_mesh{ material }, e.mesh.material / materials), material.set, errors, and a save game
-- round trip of the slots (component <-> JSON). Run unattended:
--   MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/material_test.lua MDB_SAVE_DIR=<folder>
-- Prints "material_test: PASS ..." / "FAIL ..." lines and a summary.

local phase = 1
local frame = 0
local passed, failed = 0, 0

local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log("material_test: " .. (ok and "PASS " or "FAIL ") .. name .. (detail ~= nil and ("  (" .. tostring(detail) .. ")") or ""))
end

local function near(a, b) return math.abs(a - b) < 1e-4 end

function on_save(self) return { phase = phase, passed = passed, failed = failed } end
function on_load(self, data) phase, passed, failed = data.phase, data.passed, data.failed; frame = 0 end

function on_update(self, dt)
    frame = frame + 1
    if phase == 1 and frame == 2 then
        check("exists", material.exists("Materials/gold.mat") and not material.exists("Materials/nope.mat"))
        local gold = material.get("Materials/gold.mat")
        check("values", gold.metallic == 1 and near(gold.roughness, 0.28) and near(gold.color.x, 1.0) and gold.shader == "standard",
            gold.metallic .. " " .. gold.roughness)
        check("defaults for keys the file leaves out", gold.specular == 1 and gold.alphaMode == "opaque" and gold.castShadows == true)

        -- A variant: the parent's values, then its own
        local mossy = material.get("Materials/Terrain/rock_mossy.mat")
        check("variant keeps the parent's maps", mossy.albedoMap == "Textures/Terrain/rock.jpg" and mossy.normalMap == "Textures/Terrain/rock_normal.png",
            mossy.albedoMap)
        check("variant's own values win", near(mossy.color.y, 0.86) and mossy.uvScale[1] == 2 and mossy.parent == "Materials/Terrain/rock.mat")

        -- Mesh parts
        local a = scene.spawn("Material ball", vec3(0, 1, 0))
        a:add_mesh{ geometry = "sphere", material = "Materials/gold.mat" }
        check("add_mesh material", a.mesh.material == "Materials/gold.mat" and #a.mesh.materials == 1)
        a.mesh.material = "Materials/glow.mat"
        check("set material", a.mesh.material == "Materials/glow.mat")
        local b = scene.spawn("Material cube", vec3(2, 1, 0))
        b:add_mesh{ geometry = "cube", materials = { "Materials/glass.mat" } }
        check("add_mesh materials", b.mesh.materials[1] == "Materials/glass.mat")
        b.mesh.materials = {}
        check("clear materials", b.mesh.material == "" and #b.mesh.materials == 0)
        b.mesh.material = "Materials/Terrain/rock_mossy.mat"
        check("missing file is an error", not pcall(function() a.mesh.material = "Materials/nope.mat" end) and a.mesh.material == "Materials/glow.mat")
        check("add_mesh with a missing file is an error", not pcall(function() scene.spawn("bad"):add_mesh{ material = "Materials/nope.mat" } end))

        -- material.set: everything using the file follows (until Play stops)
        material.set("Materials/gold.mat", "roughness", 0.9)
        material.set("Materials/gold.mat", "color", vec3(0.5, 0.25, 0.125))
        local g2 = material.get("Materials/gold.mat")
        check("material.set", near(g2.roughness, 0.9) and near(g2.color.y, 0.25), g2.roughness)
        check("material.set: unknown key", not pcall(function() material.set("Materials/gold.mat", "shine", 1) end))
        check("material.set: wrong type", not pcall(function() material.set("Materials/gold.mat", "roughness", "high") end))
        local mossy2 = material.get("Materials/Terrain/rock_mossy.mat")
        material.set("Materials/Terrain/rock.mat", "normalStrength", 0.5)
        check("a parent's change reaches its variant", near(material.get("Materials/Terrain/rock_mossy.mat").normalStrength, 0.5),
            mossy2.normalStrength)
    elseif phase == 1 and frame == 4 then
        local s = scene.render_stats()
        check("drawn", s.draw_calls > 0, s.draw_calls)
        phase = 2
        saves.save("material_test")                         -- the slots go through EntityJson
        saves.load("material_test")
    elseif phase == 2 and frame == 3 then
        local a, b = scene.find("Material ball"), scene.find("Material cube")
        check("saved slots load back", a and a.mesh.material == "Materials/glow.mat" and b and b.mesh.material == "Materials/Terrain/rock_mossy.mat",
            a and a.mesh.material)
        saves.delete("material_test")
        log(string.format("material_test: %d passed, %d failed", passed, failed))
        phase = 3
    end
end
