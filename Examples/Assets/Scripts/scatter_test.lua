-- Checks the terrain Scatter brush's Fill on the terrain demo: instances in their top-level group, standing on
-- the ground, apart by the spacing, only on gentle slopes, static with a trunk body.
-- Run unattended (the editor fills the terrain, then presses Play):
--   MDB_SCENE=terrain.json MDB_SELECT=Terrain MDB_TERRAIN_TOOL=scatter:Models/Village/pine.gltf,Models/Village/oak.gltf
--   MDB_SCATTER_FILL=1 MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/scatter_test.lua
-- Prints "scatter_test: PASS ..." / "FAIL ..." lines and a summary.

local frame = 0
local passed, failed = 0, 0

local function check(name, ok, detail)
    if ok then passed = passed + 1; log("scatter_test: PASS " .. name .. (detail and ("  (" .. detail .. ")") or ""))
    else failed = failed + 1; log("scatter_test: FAIL " .. name .. (detail and ("  (" .. detail .. ")") or "")) end
end

function on_update(self, dt)
    frame = frame + 1
    if frame ~= 3 then return end
    local group = scene.find("Scatter")
    check("group exists", group ~= nil)
    if not group then return end
    check("group is top level", group.parent == nil)
    local items = group:children()
    check("instances placed", #items > 50, tostring(#items))

    local worstGround, steep, notStatic, noBody, names = 0, 0, 0, 0, {}
    local pts = {}
    local cosMax = math.cos(math.rad(35 + 3))     -- the rule is 35 deg; normals are per triangle here
    for _, e in ipairs(items) do
        local p = e.world_position
        pts[#pts + 1] = p
        local h = terrain.height(p.x, p.z)
        if h then worstGround = math.max(worstGround, math.abs((h - 0.05) - p.y)) else worstGround = 99 end
        local n = terrain.normal(p.x, p.z)
        if n and n.y < cosMax then steep = steep + 1 end
        if not (e.mesh and e.mesh.static) then notStatic = notStatic + 1 end
        if not (e.body and e.body.type == "static") then noBody = noBody + 1 end
        names[e.name] = true
    end
    check("standing on the ground (sink 0.05)", worstGround < 0.02, string.format("worst %.4f m", worstGround))
    check("no steep spots", steep == 0, tostring(steep))
    check("static meshes", notStatic == 0, tostring(notStatic))
    check("trunk bodies", noBody == 0, tostring(noBody))
    check("both items used", names["pine"] and names["oak"])

    local closest = 1e9
    for i = 1, #pts do
        for j = i + 1, #pts do
            local dx, dz = pts[i].x - pts[j].x, pts[i].z - pts[j].z
            closest = math.min(closest, math.sqrt(dx * dx + dz * dz))
        end
    end
    check("spacing kept (3 m)", closest >= 2.99, string.format("closest %.2f m", closest))

    -- A trunk body blocks a ray across the base of a tree
    local p = pts[1]
    local hit = scene.raycast(vec3(p.x - 3, p.y + 1, p.z), vec3(1, 0, 0), 6)
    check("trunk is solid", hit ~= nil and hit.entity == items[1], hit and hit.entity.name or "no hit")

    log(string.format("scatter_test: %d passed, %d failed", passed, failed))
end
