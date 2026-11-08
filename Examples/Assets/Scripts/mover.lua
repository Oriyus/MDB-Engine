-- Arrow keys move the entity on the ground plane, Space lifts it.
-- (W/E/R/Q are the editor's gizmo keys, so this uses the arrows.)

local speed = 3.0

function on_create(self)
    log("mover attached to", self.name)
end

function on_update(self, dt)
    local dir = vec3(0, 0, 0)
    if input.key_down(Key.Left)  then dir.x = dir.x - 1 end
    if input.key_down(Key.Right) then dir.x = dir.x + 1 end
    if input.key_down(Key.Up)    then dir.z = dir.z - 1 end
    if input.key_down(Key.Down)  then dir.z = dir.z + 1 end
    if input.key_down(Key.Space) then dir.y = dir.y + 1 end

    if dir:length() > 0 then
        self.position = self.position + dir:normalize() * (speed * dt)
    end
end

function on_destroy(self)
    log("mover on", self.name, "stopped at", self.position)
end
