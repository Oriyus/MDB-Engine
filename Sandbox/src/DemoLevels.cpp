#include "DemoLevels.h"

#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Scene/Prefab.h"
#include "Renderer/Model.h"
#include "Core/Log.h"

#include <cmath>
#include <functional>
#include <string>

using namespace Engine;

namespace {

    // ---- small builders. A "box" is the unit cube scaled to `size`, with a matching collider.
    Entity AddBox(Scene& scene, const LevelAssets& a, const std::string& name, const glm::vec3& pos,
        const glm::vec3& size, const glm::vec3& color, BodyType type = BodyType::Static,
        const glm::vec3& rotation = glm::vec3(0.0f)) {
        Entity e = scene.CreateEntity(name);
        auto& t = e.Get<TransformComponent>();
        t.Position = pos;
        t.Rotation = rotation;
        t.Scale = size;
        auto& mr = e.Add<MeshRendererComponent>();
        mr.Geometry = a.Cube;
        mr.Color = color;
        mr.Roughness = 0.7f;
        e.Add<RigidBodyComponent>().Type = type;
        return e;
    }

    // A thin slab lying on a surface, with a trigger volume `height` metres tall above it
    Entity AddPad(Scene& scene, const LevelAssets& a, const std::string& name, const glm::vec3& pos,
        float sx, float sz, float height, const glm::vec3& color, const glm::vec3& emissive, const char* script) {
        const float thick = 0.06f;
        Entity e = scene.CreateEntity(name);
        auto& t = e.Get<TransformComponent>();
        t.Position = pos;
        t.Scale = glm::vec3(sx, thick, sz);
        auto& mr = e.Add<MeshRendererComponent>();
        mr.Geometry = a.Cube;
        mr.Color = color;
        mr.Emissive = emissive;
        auto& rb = e.Add<RigidBodyComponent>();
        rb.IsTrigger = true;
        rb.Size = glm::vec3(1.0f, height / thick, 1.0f);      // collider sizes are multiplied by the scale
        rb.Center = glm::vec3(0.0f, 0.5f * height / thick, 0.0f);
        e.Add<ScriptComponent>().Path = script;
        return e;
    }

    Entity AddCoin(Scene& scene, const LevelAssets& a, const glm::vec3& pos) {
        Entity e = scene.CreateEntity("Coin");
        auto& t = e.Get<TransformComponent>();
        t.Position = pos;
        t.Scale = glm::vec3(0.3f, 0.3f, 0.08f);
        auto& mr = e.Add<MeshRendererComponent>();
        mr.Geometry = a.Cube;
        mr.Color = glm::vec3(1.0f, 0.8f, 0.2f);
        mr.Metallic = 1.0f;
        mr.Roughness = 0.3f;
        mr.Emissive = glm::vec3(0.6f, 0.4f, 0.05f);
        e.Add<SpinComponent>().Speed = glm::vec3(0.0f, 2.0f, 0.0f);
        auto& rb = e.Add<RigidBodyComponent>();
        rb.IsTrigger = true;
        rb.Size = glm::vec3(2.0f, 2.0f, 6.0f);   // ~0.6 m around the thin coin
        e.Add<ScriptComponent>().Path = "Scripts/coin.lua";
        return e;
    }

    Entity AddPlayer(Scene& scene, const LevelAssets& a, const glm::vec3& pos) {
        Entity e = scene.CreateEntity("Player");
        auto& t = e.Get<TransformComponent>();
        t.Position = pos;
        t.Scale = glm::vec3(0.8f);
        auto& mr = e.Add<MeshRendererComponent>();
        mr.Geometry = a.Sphere;
        mr.Texture = a.Checker;
        mr.Color = glm::vec3(0.3f, 0.55f, 1.0f);
        mr.Roughness = 0.35f;
        auto& rb = e.Add<RigidBodyComponent>();
        rb.Shape = ColliderShape::Sphere;
        rb.Mass = 2.0f;
        rb.Friction = 0.8f;
        e.Add<ScriptComponent>().Path = "Scripts/player.lua";
        return e;
    }

    Entity AddFloor(Scene& scene, const LevelAssets& a, float scale) {
        Entity e = scene.CreateEntity("Floor");
        auto& t = e.Get<TransformComponent>();
        t.Position = glm::vec3(0.0f, -1.0f, 0.0f);
        t.Scale = glm::vec3(scale, 1.0f, scale);
        auto& mr = e.Add<MeshRendererComponent>();
        mr.Geometry = a.Plane;
        mr.Texture = a.Checker;
        mr.Color = glm::vec3(0.8f);
        mr.Roughness = 0.85f;
        auto& rb = e.Add<RigidBodyComponent>();
        rb.Type = BodyType::Static;
        rb.Size = glm::vec3(12.0f, 1.0f, 12.0f);
        rb.Center = glm::vec3(0.0f, -0.5f, 0.0f);
        return e;
    }

    Entity AddSun(Scene& scene, float intensity) {
        Entity e = scene.CreateEntity("Sun");
        auto& t = e.Get<TransformComponent>();
        t.Position = glm::vec3(0.0f, 6.0f, 0.0f);
        t.Rotation = glm::vec3(-0.9f, 0.5f, 0.0f);
        t.Scale = glm::vec3(0.12f, 0.12f, 0.6f);
        e.Add<DirectionalLightComponent>().Intensity = intensity;
        return e;
    }

    // An instance of a prefab. The first time (no file yet) `make` builds the entity, which is saved
    // as the prefab and removed, so the prefab file can be edited afterwards and stays as edited.
    Entity FromPrefab(Scene& scene, PrefabLibrary& prefabs, const std::string& path,
        const std::function<Entity()>& make, const glm::vec3& pos) {
        if (!prefabs.Exists(path)) {
            Entity proto = make();
            if (!prefabs.Save(proto, path)) {             // can't write the file: keep a plain entity
                proto.Remove<PrefabComponent>();
                proto.Get<TransformComponent>().Position = pos;
                return proto;
            }
            scene.DestroyEntity(proto);
        }
        Entity e = prefabs.Instantiate(scene, path);
        if (e) e.Get<TransformComponent>().Position = pos;
        return e;
    }

    Entity AddCoinPrefab(Scene& scene, const LevelAssets& a, PrefabLibrary& prefabs, const glm::vec3& pos) {
        return FromPrefab(scene, prefabs, "Prefabs/coin.json", [&] { return AddCoin(scene, a, glm::vec3(0.0f)); }, pos);
    }

    // What spawner.lua drops: a small bouncy ball running ball.lua (the spawner picks its colour)
    void EnsureDropBallPrefab(Scene& scene, const LevelAssets& a, PrefabLibrary& prefabs) {
        const std::string path = "Prefabs/drop_ball.json";
        if (prefabs.Exists(path)) return;
        Entity e = scene.CreateEntity("Drop");
        e.Get<TransformComponent>().Scale = glm::vec3(0.4f);
        auto& mr = e.Add<MeshRendererComponent>();
        mr.Geometry = a.Sphere;
        mr.Roughness = 0.4f;
        auto& rb = e.Add<RigidBodyComponent>();
        rb.Shape = ColliderShape::Sphere;
        rb.Restitution = 0.35f;
        e.Add<ScriptComponent>().Path = "Scripts/ball.lua";
        prefabs.Save(e, path);
        scene.DestroyEntity(e);
    }

} // namespace

void BuildScriptDemo(Scene& scene, const LevelAssets& a, PrefabLibrary& prefabs) {
    scene.Clear();
    AddFloor(scene, a, 1.0f);
    AddSun(scene, 1.2f);

    AddPad(scene, a, "Pad", glm::vec3(2.5f, -0.97f, -1.5f), 2.0f, 2.0f, 1.0f,
        glm::vec3(0.25f, 0.3f, 0.4f), glm::vec3(0.0f), "Scripts/pad.lua");
    {
        Entity e = scene.CreateEntity("Spawner");
        e.Get<TransformComponent>().Position = glm::vec3(2.5f, 4.0f, -1.5f);
        e.Add<ScriptComponent>().Path = "Scripts/spawner.lua";
    }
    EnsureDropBallPrefab(scene, a, prefabs);
    const int coins = 6;
    for (int i = 0; i < coins; i++) {
        const float ang = i * 6.2831853f / coins;
        AddCoinPrefab(scene, a, prefabs, glm::vec3(std::cos(ang) * 4.0f - 1.0f, -0.5f, std::sin(ang) * 3.0f + 1.0f));
    }
    AddPlayer(scene, a, glm::vec3(-1.0f, -0.6f, 1.0f));
    if (a.Fox && a.Fox->IsSkinned()) {
        // An animated fox walking a circle (wander.lua cross-fades its Walk / Run / Survey clips)
        Entity e = scene.CreateEntity("Fox");
        e.Get<TransformComponent>().Position = glm::vec3(-3.0f, -1.0f, -3.0f);
        auto& mr = e.Add<MeshRendererComponent>();
        mr.Geometry = a.Fox;
        mr.LocalOffset = a.Fox->GetGroundedMatrix(1.6f);
        mr.UseModelMaterials = a.Fox->HasTexturedMaterials();
        mr.Color = glm::vec3(1.0f);
        e.Add<AnimatorComponent>().Clip = "Walk";
        e.Add<ScriptComponent>().Path = "Scripts/wander.lua";
    }
    // See-through pieces: a cutout fence along the back edge and a stained-glass panel (both solid)
    if (a.Fence) {
        for (int i = 0; i < 5; i++) {
            Entity e = AddBox(scene, a, "Fence", glm::vec3(-4.4f + i * 2.2f, -0.35f, -5.6f), glm::vec3(2.2f, 1.3f, 0.04f), glm::vec3(1.0f));
            auto& mr = e.Get<MeshRendererComponent>();
            mr.Texture = a.Fence;
            mr.Alpha = AlphaMode::Cutout;
        }
    }
    if (a.Window) {
        Entity e = AddBox(scene, a, "Stained glass", glm::vec3(1.6f, 0.0f, -4.3f), glm::vec3(2.0f, 2.0f, 0.05f),
            glm::vec3(1.0f), BodyType::Static, glm::vec3(0.0f, glm::radians(-15.0f), 0.0f));
        auto& mr = e.Get<MeshRendererComponent>();
        mr.Texture = a.Window;
        mr.Alpha = AlphaMode::Blend;
        mr.Roughness = 0.1f;
    }
    LOG_INFO("Script demo built. Press Play (Ctrl+P).");
}

void BuildCoinRush(Scene& scene, const LevelAssets& a, PrefabLibrary& prefabs) {
    scene.Clear();

    // ---- arena: 24 x 24 floor at y = -1, walled in
    AddFloor(scene, a, 2.0f);
    AddSun(scene, 1.3f);
    const glm::vec3 wall(0.35f, 0.4f, 0.55f);
    AddBox(scene, a, "Wall N", glm::vec3(0.0f, -0.4f, -12.25f), glm::vec3(25.0f, 1.2f, 0.5f), wall);
    AddBox(scene, a, "Wall S", glm::vec3(0.0f, -0.4f, 12.25f), glm::vec3(25.0f, 1.2f, 0.5f), wall);
    AddBox(scene, a, "Wall W", glm::vec3(-12.25f, -0.4f, 0.0f), glm::vec3(0.5f, 1.2f, 24.0f), wall);
    AddBox(scene, a, "Wall E", glm::vec3(12.25f, -0.4f, 0.0f), glm::vec3(0.5f, 1.2f, 24.0f), wall);

    // ---- raised platform (top at y = 0.5, z from -8 to -1) and the ramp up to it
    const glm::vec3 stone(0.85f, 0.72f, 0.52f);
    AddBox(scene, a, "Platform", glm::vec3(0.0f, -0.25f, -4.5f), glm::vec3(9.0f, 1.5f, 7.0f), stone);
    {
        // Rises 1.5 m over 5 m: low end on the floor at z = 4, high end at the platform edge (z = -1)
        const float run = 5.0f, rise = 1.5f, thick = 0.3f;
        const float angle = std::atan(rise / run);
        const float length = std::sqrt(run * run + rise * rise) + 0.1f;
        const float yMid = -1.0f + rise * 0.5f - 0.5f * thick / std::cos(angle);   // top surface on the line
        AddBox(scene, a, "Ramp", glm::vec3(0.0f, yMid, 1.5f), glm::vec3(3.5f, thick, length), stone,
            BodyType::Static, glm::vec3(angle, 0.0f, 0.0f));
    }

    // ---- the sweeper: a kinematic bar spinning across the platform
    {
        Entity e = AddBox(scene, a, "Sweeper", glm::vec3(0.0f, 0.75f, -4.5f), glm::vec3(4.4f, 0.35f, 0.35f),
            glm::vec3(0.9f, 0.3f, 0.2f), BodyType::Kinematic);
        e.Get<MeshRendererComponent>().Emissive = glm::vec3(0.4f, 0.05f, 0.02f);
        e.Add<SpinComponent>().Speed = glm::vec3(0.0f, 1.4f, 0.0f);
    }

    // ---- hazards and helpers
    const glm::vec3 lava(0.35f, 0.03f, 0.01f), lavaGlow(1.4f, 0.12f, 0.01f);
    for (float x : { -4.5f, 4.5f }) {
        Entity e = AddPad(scene, a, "Lava", glm::vec3(x, -0.97f, 2.0f), 3.0f, 6.0f, 0.4f, lava, lavaGlow, "Scripts/hazard.lua");
        auto& snd = e.Add<AudioSourceComponent>();     // a low bubbling you hear as you get close
        snd.Clip = "Sounds/lava_loop.wav";
        snd.Volume = 0.6f;
        snd.MinDistance = 2.0f;
        snd.MaxDistance = 14.0f;
    }
    AddPad(scene, a, "Bouncer", glm::vec3(7.0f, -0.97f, -4.5f), 1.6f, 1.6f, 0.5f,
        glm::vec3(0.2f, 0.6f, 0.3f), glm::vec3(0.1f, 0.8f, 0.3f), "Scripts/bouncer.lua");
    AddPad(scene, a, "Goal", glm::vec3(0.0f, 0.53f, -7.5f), 2.0f, 1.0f, 1.0f,
        glm::vec3(0.9f), glm::vec3(0.3f), "Scripts/goal.lua");

    // ---- coins: start area, ramp, platform, above the bouncer, far corners
    for (const glm::vec3& p : {
             glm::vec3(0.0f, -0.5f, 7.0f), glm::vec3(-8.0f, -0.5f, 6.0f), glm::vec3(9.0f, -0.5f, 8.0f),
             glm::vec3(0.0f, -0.05f, 2.5f), glm::vec3(-3.0f, 1.0f, -2.5f), glm::vec3(3.0f, 1.0f, -6.5f),
             glm::vec3(7.0f, 1.8f, -4.5f), glm::vec3(-9.0f, -0.5f, -9.0f) })
        AddCoinPrefab(scene, a, prefabs, p);

    {
        Entity e = scene.CreateEntity("Platform Light");
        e.Get<TransformComponent>().Position = glm::vec3(0.0f, 3.0f, -4.5f);
        auto& l = e.Add<PointLightComponent>();
        l.Color = glm::vec3(1.0f, 0.75f, 0.45f);
        l.Intensity = 2.0f;
    }

    AddPlayer(scene, a, glm::vec3(0.0f, -0.6f, 9.0f));
    {
        Entity e = scene.CreateEntity("Main Camera");
        auto& t = e.Get<TransformComponent>();
        t.Position = glm::vec3(0.0f, 2.9f, 15.5f);
        t.Rotation = glm::vec3(-0.45f, 0.0f, 0.0f);
        e.Add<CameraComponent>();
        e.Add<ScriptComponent>().Path = "Scripts/follow.lua";
    }
    {
        Entity e = scene.CreateEntity("Game");
        e.Add<ScriptComponent>().Path = "Scripts/game.lua";
    }
    LOG_INFO("Coin Rush built. Press Play (Ctrl+P).");
}
