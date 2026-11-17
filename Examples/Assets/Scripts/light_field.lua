-- Many small point lights at once (clustered lighting): a field of glowing lamps that drift up and down over the
-- floor, with a short range each so a pixel only weighs the few near it. Put it on any entity; the field is centred
-- on that entity. Settings: count per side, spacing, range, height. Used for timing (MDB_PROFILE=1) and as an example.
-- Try it: MDB_SCENE=demo.json MDB_AUTOPLAY=1 MDB_TEST_SCRIPT=Scripts/light_field.lua (MDB_LIGHT_CLUSTERS=1: the debug view)

local side = 16          -- lamps per side (side x side)
local spacing = 1.8
local range = 3.0
local height = 0.5

local lamps = {}
local time = 0

local palette = { vec3(1.0, 0.55, 0.2), vec3(0.3, 0.6, 1.0), vec3(0.4, 1.0, 0.45), vec3(1.0, 0.3, 0.7) }

function on_create(self)
    local centre = self.world_position
    local half = (side - 1) * spacing * 0.5
    for x = 0, side - 1 do
        for z = 0, side - 1 do
            local c = palette[(x + z) % #palette + 1]
            local e = scene.spawn("Lamp", centre + vec3(x * spacing - half, height, z * spacing - half))
            e.scale = vec3(0.12, 0.12, 0.12)
            e:add_mesh({ geometry = "sphere", color = c, emissive = c * 2 })
            e.mesh.cast_shadows = false
            e:add_light({ color = c, intensity = 0.35, range = range, shadows = false })
            lamps[#lamps + 1] = { entity = e, base = e.position, phase = (x * 0.7 + z * 1.3) }
        end
    end
    log(string.format("light_field: %d lamps, range %.1f m", #lamps, range))
end

function on_update(self, dt)
    time = time + dt
    for _, l in ipairs(lamps) do
        l.entity.position = l.base + vec3(0, 0.35 * math.sin(time * 1.5 + l.phase), 0)
    end
end
