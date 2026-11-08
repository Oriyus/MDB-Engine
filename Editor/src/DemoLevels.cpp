#include "DemoLevels.h"

#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Scene/Prefab.h"
#include "Scene/Particles.h"
#include "Renderer/Model.h"
#include "Core/Log.h"

#include <glm/gtc/matrix_transform.hpp>
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


    // Level geometry that never moves or changes is marked Static, so Play merges it: a Mesh Renderer
    // whose entity and ancestors have no script, spin, orbit, animator or moving (non-static) body
    void MarkStatic(Scene& scene) {
        auto& reg = scene.Registry();
        auto keepsStill = [&](entt::entity e) {
            if (reg.any_of<ScriptComponent, SpinComponent, OrbitComponent, AnimatorComponent>(e)) return false;
            if (const auto* rb = reg.try_get<RigidBodyComponent>(e))
                if (rb->Type != BodyType::Static || rb->IsTrigger) return false;
            return true;
        };
        for (auto [e, mr] : reg.view<MeshRendererComponent>().each()) {
            bool still = true;
            for (entt::entity x = e; still && x != entt::null; x = scene.GetParent(x)) still = keepsStill(x);
            mr.Static = still;
        }
    }

    // ---- tactics levels (tactics.lua). Units have no rigid body: the script moves them along nav paths.
    glm::mat4 BlockOffset(float w, float h) {          // a w x h x w block standing on the entity's origin
        return glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, h * 0.5f, 0.0f)), glm::vec3(w, h, w));
    }

    void AddHero(Scene& scene, const LevelAssets& a, const glm::vec3& pos, float yaw) {
        Entity e = scene.CreateEntity("Hero");
        e.Get<TransformComponent>().Position = pos;
        e.Get<TransformComponent>().Rotation = glm::vec3(0.0f, yaw, 0.0f);
        auto& mr = e.Add<MeshRendererComponent>();
        if (a.Fox && a.Fox->IsSkinned()) {
            mr.Geometry = a.Fox;
            mr.LocalOffset = a.Fox->GetGroundedMatrix(1.4f);
            mr.UseModelMaterials = a.Fox->HasTexturedMaterials();
            mr.Color = glm::vec3(1.0f);
            auto& an = e.Add<AnimatorComponent>();
            an.Clip = "Survey";
            for (float t : { 0.10f, 0.45f }) an.Events.push_back({ "Walk", t, "sound:Sounds/step.wav" });
            for (float t : { 0.20f, 0.75f }) an.Events.push_back({ "Run", t, "sound:Sounds/step.wav" });
        }
        else {                                         // no fox model: a blue block
            mr.Geometry = a.Cube;
            mr.Color = glm::vec3(0.3f, 0.55f, 1.0f);
            mr.LocalOffset = BlockOffset(0.6f, 1.4f);
        }
    }

    void AddEnemy(Scene& scene, const LevelAssets& a, const glm::vec3& pos) {
        Entity e = scene.CreateEntity("Enemy");
        e.Get<TransformComponent>().Position = pos;
        auto& mr = e.Add<MeshRendererComponent>();     // a red block
        mr.Geometry = a.Cube;
        mr.Color = glm::vec3(0.85f, 0.18f, 0.12f);
        mr.Roughness = 0.5f;
        mr.LocalOffset = BlockOffset(0.7f, 1.6f);
    }

    // The camera (tactics_camera.lua) and the entity running tactics.lua
    void AddTacticsDirector(Scene& scene, const glm::vec3& cameraPos) {
        Entity cam = scene.CreateEntity("Main Camera");
        cam.Get<TransformComponent>().Position = cameraPos;
        cam.Get<TransformComponent>().Rotation = glm::vec3(-0.85f, 0.0f, 0.0f);
        cam.Add<CameraComponent>().FOV = 50.0f;
        cam.Add<ScriptComponent>().Path = "Scripts/tactics_camera.lua";
        scene.CreateEntity("Tactics").Add<ScriptComponent>().Path = "Scripts/tactics.lua";
        scene.CreateEntity("Inventory").Add<ScriptComponent>().Path = "Scripts/inventory.lua";   // the bag (in `game`)
    }

    // A glowing pad that takes the hero to another level (exit.lua reads the target from the name:
    // "Exit: <scene file> > <entry>"). No body: it doesn't block the nav grid.
    void AddExit(Scene& scene, const LevelAssets& a, const std::string& sceneFile, const std::string& entry, const glm::vec3& pos) {
        Entity e = scene.CreateEntity("Exit: " + sceneFile + " > " + entry);
        auto& t = e.Get<TransformComponent>();
        t.Position = pos + glm::vec3(0.0f, 0.02f, 0.0f);
        t.Scale = glm::vec3(1.0f, 0.04f, 1.0f);
        auto& mr = e.Add<MeshRendererComponent>();
        mr.Geometry = a.Cube;
        mr.Color = glm::vec3(0.2f, 0.7f, 0.8f);
        mr.Emissive = glm::vec3(0.1f, 0.6f, 0.8f);
        e.Add<ScriptComponent>().Path = "Scripts/exit.lua";
    }

    // Where the party appears when it arrives by that entry (tactics.lua: "Entry: <name>")
    void AddEntry(Scene& scene, const std::string& name, const glm::vec3& pos, float yaw) {
        Entity e = scene.CreateEntity("Entry: " + name);
        e.Get<TransformComponent>().Position = pos;
        e.Get<TransformComponent>().Rotation = glm::vec3(0.0f, yaw, 0.0f);
    }

    // A chest with a hinged lid (chest.lua opens it when the hero comes close: gold into game.gold)
    void AddChest(Scene& scene, const LevelAssets& a, const glm::vec3& pos, float yaw, const std::string& loot = "") {
        const glm::vec3 wood(0.5f, 0.3f, 0.15f), trim(0.85f, 0.65f, 0.25f);
        Entity chest = scene.CreateEntity(loot.empty() ? std::string("Chest") : "Chest: " + loot);   // chest.lua gives that item
        chest.Get<TransformComponent>().Position = pos;
        chest.Get<TransformComponent>().Rotation = glm::vec3(0.0f, yaw, 0.0f);
        auto& mr = chest.Add<MeshRendererComponent>();
        mr.Geometry = a.Cube;
        mr.Color = wood;
        mr.Roughness = 0.8f;
        mr.LocalOffset = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.275f, 0.0f)), glm::vec3(0.9f, 0.55f, 0.6f));
        auto& rb = chest.Add<RigidBodyComponent>();    // static: its cell is not walkable
        rb.Type = BodyType::Static;
        rb.Size = glm::vec3(0.9f, 0.55f, 0.6f);
        rb.Center = glm::vec3(0.0f, 0.275f, 0.0f);
        chest.Add<ScriptComponent>().Path = "Scripts/chest.lua";

        Entity hinge = scene.CreateEntity("Lid Hinge");    // along the back top edge: turning it opens the lid
        hinge.Get<TransformComponent>().Position = glm::vec3(0.0f, 0.55f, -0.3f);
        hinge.Add<ParentComponent>(ParentComponent{ chest.Get<IDComponent>().ID });
        Entity lid = scene.CreateEntity("Lid");
        auto& lt = lid.Get<TransformComponent>();
        lt.Position = glm::vec3(0.0f, 0.06f, 0.3f);
        lt.Scale = glm::vec3(0.94f, 0.12f, 0.62f);
        auto& lm = lid.Add<MeshRendererComponent>();
        lm.Geometry = a.Cube;
        lm.Color = trim;
        lm.Metallic = 0.6f;
        lm.Roughness = 0.4f;
        lid.Add<ParentComponent>(ParentComponent{ hinge.Get<IDComponent>().ID });
    }

    // Someone to talk to (npc.lua plays Dialogues/sage.json: walk close, press T). A static body: its cell isn't walkable.
    void AddSage(Scene& scene, const LevelAssets& a, const glm::vec3& pos) {
        Entity e = scene.CreateEntity("Sage");
        e.Get<TransformComponent>().Position = pos;
        auto& mr = e.Add<MeshRendererComponent>();
        mr.Geometry = a.Cube;
        mr.Color = glm::vec3(0.25f, 0.7f, 0.65f);
        mr.Roughness = 0.5f;
        mr.LocalOffset = BlockOffset(0.6f, 1.7f);
        auto& rb = e.Add<RigidBodyComponent>();
        rb.Type = BodyType::Static;
        rb.Size = glm::vec3(0.6f, 1.7f, 0.6f);
        rb.Center = glm::vec3(0.0f, 0.85f, 0.0f);
        e.Add<ScriptComponent>().Path = "Scripts/npc.lua";       // plays Dialogues/sage.json
        Entity hat = scene.CreateEntity("Hat");            // so you can tell which way it faces
        auto& ht = hat.Get<TransformComponent>();
        ht.Position = glm::vec3(0.0f, 1.82f, 0.08f);
        ht.Scale = glm::vec3(0.5f, 0.24f, 0.5f);
        auto& hm = hat.Add<MeshRendererComponent>();
        hm.Geometry = a.Cube;
        hm.Color = glm::vec3(0.35f, 0.25f, 0.6f);
        hat.Add<ParentComponent>(ParentComponent{ e.Get<IDComponent>().ID });
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
    Entity player = AddPlayer(scene, a, glm::vec3(-1.0f, -0.6f, 1.0f));
    if (a.Fox && a.Fox->IsSkinned()) {
        // An animated fox walking a circle (wander.lua cross-fades its Walk / Run / Survey clips)
        Entity e = scene.CreateEntity("Fox");
        e.Get<TransformComponent>().Position = glm::vec3(-3.0f, -1.0f, -3.0f);
        auto& mr = e.Add<MeshRendererComponent>();
        mr.Geometry = a.Fox;
        mr.LocalOffset = a.Fox->GetGroundedMatrix(1.6f);
        mr.UseModelMaterials = a.Fox->HasTexturedMaterials();
        mr.Color = glm::vec3(1.0f);
        auto& an = e.Add<AnimatorComponent>();
        an.Clip = "Walk";
        // Footsteps: a sound event on each paw strike (two per Walk cycle, two per Run stride)
        for (float t : { 0.10f, 0.45f }) an.Events.push_back({ "Walk", t, "sound:Sounds/step.wav" });
        for (float t : { 0.20f, 0.75f }) an.Events.push_back({ "Run", t, "sound:Sounds/step.wav" });
        // An additive layer: Survey's head movement on the neck and head, added to whatever the body
        // is doing (wander.lua fades it in to look around while walking)
        AnimLayer look;
        look.Clip = "Survey";
        look.Mask = "b_Neck_04";
        look.Weight = 0.0f;
        look.Additive = true;
        // Look-at IK on the player ball: wander.lua fades it in when the ball comes close
        AnimIK watch;
        watch.Type = IKType::Look;
        watch.Bone = "b_Head_05";
        watch.TargetID = player.Get<IDComponent>().ID;
        watch.Weight = 0.0f;
        watch.MaxAngle = 75.0f;
        watch.Chain = 2;
        an.IK.push_back(watch);
        an.Layers.push_back(look);
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
    MarkStatic(scene);
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
        // Embers drifting up off the lava (a box emitter the size of the pad)
        auto& embers = e.Add<ParticleEmitterComponent>();
        ApplyParticlePreset(embers, "Embers");
        embers.ShapeSize = glm::vec3(0.9f, 0.0f, 0.9f);
        embers.Rate = 22.0f;
        embers.Size = glm::vec2(0.07f, 0.13f);
        embers.Speed = glm::vec2(0.6f, 1.4f);
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
    MarkStatic(scene);
    LOG_INFO("Coin Rush built. Press Play (Ctrl+P).");
}

void BuildTacticsTest(Scene& scene, const LevelAssets& a, PrefabLibrary&) {
    scene.Clear();

    // ---- 24 x 24 arena at y = -1, walled in. Everything is aligned to whole metres, so with
    // 1 m cells every cell centre sits in the middle of a tile (x.5, z.5).
    AddFloor(scene, a, 2.0f);
    AddSun(scene, 1.25f);
    const glm::vec3 wall(0.35f, 0.4f, 0.55f), stone(0.85f, 0.72f, 0.52f), wood(0.55f, 0.36f, 0.2f);
    AddBox(scene, a, "Wall N", glm::vec3(0.0f, -0.4f, -12.25f), glm::vec3(25.0f, 1.2f, 0.5f), wall);
    AddBox(scene, a, "Wall S", glm::vec3(0.0f, -0.4f, 12.25f), glm::vec3(25.0f, 1.2f, 0.5f), wall);
    AddBox(scene, a, "Wall W", glm::vec3(-12.25f, -0.4f, 0.0f), glm::vec3(0.5f, 1.2f, 24.0f), wall);
    AddBox(scene, a, "Wall E", glm::vec3(12.25f, -0.4f, 0.0f), glm::vec3(0.5f, 1.2f, 24.0f), wall);

    // ---- two high terraces (top y = 1.5) along the north wall, joined by a bridge. The ground under
    // the bridge stays walkable (2.2 m of head room): those columns have two layers.
    AddBox(scene, a, "Terrace W", glm::vec3(-8.0f, 0.25f, -9.0f), glm::vec3(8.0f, 2.5f, 6.0f), stone);
    AddBox(scene, a, "Terrace E", glm::vec3(8.0f, 0.25f, -9.0f), glm::vec3(8.0f, 2.5f, 6.0f), stone);
    AddBox(scene, a, "Bridge", glm::vec3(0.0f, 1.35f, -9.0f), glm::vec3(8.0f, 0.3f, 2.0f), wood);
    AddBox(scene, a, "Rail N", glm::vec3(0.0f, 1.9f, -10.05f), glm::vec3(8.0f, 0.8f, 0.1f), wood);
    AddBox(scene, a, "Rail S", glm::vec3(0.0f, 1.9f, -7.95f), glm::vec3(8.0f, 0.8f, 0.1f), wood);

    // ---- a ramp up to the west terrace: 2.5 m over 8 m (about 0.31 m per cell, under the 0.4 m step)
    {
        const float run = 8.0f, rise = 2.5f, thick = 0.3f;
        const float angle = std::atan(rise / run);
        const float length = std::sqrt(run * run + rise * rise) + 0.1f;
        const float yMid = -1.0f + rise * 0.5f - 0.5f * thick / std::cos(angle);
        AddBox(scene, a, "Ramp", glm::vec3(-8.0f, yMid, -2.0f), glm::vec3(4.0f, thick, length), stone,
            BodyType::Static, glm::vec3(angle, 0.0f, 0.0f));
    }

    // ---- a dais in the east, reached only by two 0.3 m steps on its west side
    AddBox(scene, a, "Step 1", glm::vec3(3.5f, -0.85f, 3.5f), glm::vec3(1.0f, 0.3f, 5.0f), stone);
    AddBox(scene, a, "Step 2", glm::vec3(4.5f, -0.7f, 3.5f), glm::vec3(1.0f, 0.6f, 5.0f), stone);
    AddBox(scene, a, "Dais", glm::vec3(7.5f, -0.55f, 3.5f), glm::vec3(5.0f, 0.9f, 5.0f), stone);

    // ---- a thin fence on a cell boundary (x = 0): the cells on both sides are walkable, the link
    // between them is not. And a few crates, one cell each.
    AddBox(scene, a, "Fence", glm::vec3(0.0f, -0.4f, 4.0f), glm::vec3(0.1f, 1.2f, 8.0f), wood);
    for (const glm::vec3& p : { glm::vec3(2.5f, -0.5f, 8.5f), glm::vec3(-6.5f, -0.5f, 6.5f),
             glm::vec3(6.5f, -0.5f, -2.5f), glm::vec3(-2.5f, -0.5f, -2.5f), glm::vec3(-3.5f, -0.5f, -2.5f) })
        AddBox(scene, a, "Crate", p, glm::vec3(1.0f), wood * 1.3f);

    {
        Entity e = scene.CreateEntity("Bridge Light");
        e.Get<TransformComponent>().Position = glm::vec3(0.0f, 4.0f, -6.0f);
        auto& l = e.Add<PointLightComponent>();
        l.Color = glm::vec3(1.0f, 0.8f, 0.55f);
        l.Intensity = 2.0f;
    }

    // ---- the way down to the cellar (a second level), where you arrive from it, and a chest
    AddExit(scene, a, "tactics_cellar.json", "From Yard", glm::vec3(-10.5f, -1.0f, 10.5f));
    AddEntry(scene, "From Cellar", glm::vec3(-8.5f, -1.0f, 10.5f), 1.5708f);
    AddChest(scene, a, glm::vec3(10.5f, -1.0f, 10.5f), 3.14159f, "gem");
    AddSage(scene, a, glm::vec3(-4.5f, -1.0f, 7.5f));

    // ---- the units. They have no rigid body: tactics.lua moves them along nav paths.
    AddHero(scene, a, glm::vec3(-2.5f, -1.0f, 9.5f), 3.14159265f);
    for (const glm::vec3& p : { glm::vec3(7.5f, -0.1f, 2.5f), glm::vec3(8.5f, -0.1f, 4.5f), glm::vec3(9.5f, 1.5f, -9.5f) })
        AddEnemy(scene, a, p);
    AddTacticsDirector(scene, glm::vec3(-2.5f, 8.0f, 17.0f));
    MarkStatic(scene);
    LOG_INFO("Tactics test built. Press Play (Ctrl+P), then click the ground to move; C starts combat.");
}

void BuildTacticsCellar(Scene& scene, const LevelAssets& a, PrefabLibrary&) {
    scene.Clear();

    // ---- a 12 x 12 room at y = -1 with four pillars, dimly lit
    AddFloor(scene, a, 1.0f);
    AddSun(scene, 0.35f);
    const glm::vec3 wall(0.3f, 0.28f, 0.32f), stone(0.55f, 0.5f, 0.45f);
    AddBox(scene, a, "Wall N", glm::vec3(0.0f, 0.0f, -6.25f), glm::vec3(13.0f, 2.0f, 0.5f), wall);
    AddBox(scene, a, "Wall S", glm::vec3(0.0f, 0.0f, 6.25f), glm::vec3(13.0f, 2.0f, 0.5f), wall);
    AddBox(scene, a, "Wall W", glm::vec3(-6.25f, 0.0f, 0.0f), glm::vec3(0.5f, 2.0f, 12.0f), wall);
    AddBox(scene, a, "Wall E", glm::vec3(6.25f, 0.0f, 0.0f), glm::vec3(0.5f, 2.0f, 12.0f), wall);
    for (const glm::vec3& p : { glm::vec3(-2.5f, 0.25f, -2.5f), glm::vec3(2.5f, 0.25f, -2.5f),
             glm::vec3(-2.5f, 0.25f, 2.5f), glm::vec3(2.5f, 0.25f, 2.5f) })
        AddBox(scene, a, "Pillar", p, glm::vec3(1.0f, 2.5f, 1.0f), stone);
    for (const glm::vec3& p : { glm::vec3(0.5f, -0.5f, -4.5f), glm::vec3(4.5f, -0.5f, 0.5f) })
        AddBox(scene, a, "Crate", p, glm::vec3(1.0f), glm::vec3(0.55f, 0.36f, 0.2f) * 1.3f);
    for (const glm::vec3& p : { glm::vec3(0.0f, 2.2f, 0.0f), glm::vec3(-4.0f, 1.6f, 4.0f) }) {
        Entity e = scene.CreateEntity("Lamp");
        e.Get<TransformComponent>().Position = p;
        auto& l = e.Add<PointLightComponent>();
        l.Color = glm::vec3(1.0f, 0.65f, 0.35f);
        l.Intensity = 2.5f;
    }

    AddExit(scene, a, "tactics_test.json", "From Cellar", glm::vec3(-4.5f, -1.0f, 4.5f));
    AddEntry(scene, "From Yard", glm::vec3(-4.5f, -1.0f, 2.5f), 3.14159f);
    AddChest(scene, a, glm::vec3(4.5f, -1.0f, -4.5f), 0.0f, "helmet");

    AddHero(scene, a, glm::vec3(-4.5f, -1.0f, 2.5f), 3.14159265f);
    AddEnemy(scene, a, glm::vec3(3.5f, -1.0f, -1.5f));
    AddTacticsDirector(scene, glm::vec3(-4.5f, 7.0f, 9.0f));
    MarkStatic(scene);
    LOG_INFO("Tactics cellar built (the yard's stairs lead here: tactics_test.json).");
}
