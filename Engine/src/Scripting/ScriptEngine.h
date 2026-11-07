#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

    class Scene;
    class PhysicsWorld;
    class AudioEngine;
    class PrefabLibrary;
    struct ResourceRegistry;

    // One ui.text call: drawn by the app over the viewport for the frame it was made in
    struct ScriptText {
        std::string Text;
        float X = 0.0f, Y = 0.0f;           // 0..1 across the viewport, from the top left
        float Size = 24.0f;                 // pixels at a 720 px tall viewport (scales with it)
        glm::vec3 Color{ 1.0f };
        int Align = 0;                      // 0 left, 1 center, 2 right (of X)
    };

    // Runs the Lua scripts of every entity with a ScriptComponent while the scene plays.
    // Each entity gets its own copy of the script's globals, so two entities running the same
    // file don't share state. A script that errors is logged and switched off; the rest keep running.
    // Each Start() builds a fresh Lua state and re-reads the files (so does Reload()).
    //
    // ---- Callbacks (all optional)
    //   on_create(self)                         once, when Play starts or the entity appears
    //   on_update(self, dt)                     every simulated frame
    //   on_destroy(self)                        on Stop / Reload, or when the entity is destroyed by a script
    //   on_collision_begin(self, other, hit)    started touching; hit.point, hit.normal (points from other to self)
    //   on_collision_end(self, other)           stopped touching (other may already be destroyed: other:valid())
    //   on_trigger_enter(self, other)           one of the two is a trigger (sensor) body
    //   on_trigger_exit(self, other)
    //
    // ---- Entity (self, other, results of scene.find / spawn / clone)
    //   e.name, e.position, e.rotation (radians), e.scale     read/write; vec3 values are copies
    //   e.forward, e.right, e.up                              world-space directions (read-only)
    //   e.id, e:valid(), e:destroy(), e:clone()
    //   e:has(kind), e:remove(kind)     kind = "mesh" "body" "light" "sun" "script" "spin" "orbit" "camera" "audio"
    //   e.mesh   -> nil or { color, emissive, metallic, roughness, unlit, geometry ("cube" "sphere" ...) }
    //   e.light  -> nil or { color, intensity, shadows }
    //   e.body   -> nil or { velocity, angular_velocity, type, trigger, mass,
    //                         add_force(v) (newtons, lasts this frame: call each frame), add_impulse(v) }
    //   e:add_mesh{ geometry="cube", texture="checker", color=vec3, metallic, roughness, emissive, unlit }
    //   e:add_body{ type="dynamic"|"static"|"kinematic", shape="box"|"sphere", size=number|vec3,
    //               center=vec3, mass, friction, bounciness, trigger=bool, velocity=vec3 }
    //   e:add_light{ color=vec3, intensity, shadows }
    //   e:add_script("Scripts/x.lua")        starts on the next frame
    //   e.camera -> nil or { fov, primary }       e:add_camera{ fov=60, near, far, primary=true }
    //   e:look_at(target [, up])               turn local -Z (forward) towards a point
    //   e.audio -> nil or { clip, volume, pitch, loop, playing, play(), stop() }
    //   e:add_audio(clip [, { volume, pitch, loop=true, spatial=true, play=true, min_distance, max_distance }])
    //
    // ---- Globals
    //   vec3(x, y, z): + - * / unary-, v:length(), v:normalize(), v:dot(w), v:cross(w)
    //   scene.find(name), scene.find_all(name), scene.spawn(name [, position]), scene.time()
    //   scene.spawn_prefab(name [, position])  an instance of Prefabs/<name>.json (nil if missing)
    //   scene.restart()                      Stop + Play on the next frame (retry a level)
    //   audio.play(clip [, { volume, pitch }])             2D one-shot, e.g. "Sounds/coin.wav"
    //   audio.play_at(clip, position [, { volume, pitch }]) 3D one-shot, quieter with distance
    //   ui.text(text, x, y [, { size=24, color=vec3, align="left"|"center"|"right" }])   x, y in 0..1, this frame only
    //   scene.raycast(origin, dir, max_dist [, ignore_entity]) -> nil or { entity, point, normal, distance }
    //   input.key_down(k), input.key_pressed(k), input.key_released(k)    Key.A..Key.Z, Key.Num0..9, Key.F1..12,
    //   input.mouse_down(b), input.mouse_pressed(b), input.mouse_position()  Key.Space, Key.Left ... Mouse.Left ...
    //   shared                                  a table all scripts see (score, game state...)
    //   log(...), print(...)
    class ScriptEngine {
    public:
        ScriptEngine(std::string assetDir, PhysicsWorld* physics, AudioEngine* audio);
        ~ScriptEngine();
        ScriptEngine(const ScriptEngine&) = delete;
        ScriptEngine& operator=(const ScriptEngine&) = delete;

        // Name <-> resource mapping used by add_mesh and mesh.geometry
        void SetResources(const ResourceRegistry& resources);
        void SetPrefabs(PrefabLibrary* prefabs);   // for scene.spawn_prefab

        void Start(Scene& scene);              // call when Play starts (after PhysicsWorld::Sync)
        void Update(Scene& scene, float dt);   // call once per simulated frame, before the physics step
        void DispatchCollisions(Scene& scene); // call after the physics step: delivers its contact events
        void Stop(Scene& scene);               // call before the scene is restored on Stop
        void Reload(Scene& scene);             // Stop + Start, keeping the scene as it is (hot reload)

        bool IsRunning() const;
        uint32_t InstanceCount() const;        // scripts currently running
        const std::vector<std::string>& RecentErrors() const;   // newest last, cleared on Start
        const std::vector<ScriptText>& Texts() const;            // this frame's ui.text calls
        bool ConsumeRestartRequest();                           // true once after scene.restart()

    private:
        struct Impl;                           // keeps sol2 out of everything else
        std::unique_ptr<Impl> m_Impl;
    };

} // namespace Engine
