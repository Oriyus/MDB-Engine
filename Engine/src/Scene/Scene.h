#pragma once
#include <entt/entt.hpp>
#include <string>
#include "Scene/Components.h"
#include <optional>
#include <vector>

namespace Engine {

    class Entity;
    class Camera;
    class Shader;
    class ShadowContext;
    class Environment;

    // A copy of one entity's component values, used to rewind after Play
    struct EntitySnapshot {
        std::string                                 Tag;
        TransformComponent                          Transform;
        std::optional<MeshRendererComponent>        MeshRenderer;
        std::optional<PointLightComponent>          PointLight;
        std::optional<DirectionalLightComponent>    DirectionalLight;
        std::optional<SpinComponent>                Spin;
        std::optional<OrbitComponent>               Orbit;
        std::optional<RigidBodyComponent>           RigidBody;
        std::optional<ScriptComponent>              Script;
    };
    using SceneSnapshot = std::vector<EntitySnapshot>;

    class Scene {
    public:
        Entity CreateEntity(const std::string& name = "Entity");
        void DestroyEntity(Entity entity);

        void Clear();
        void OnUpdate(float dt);
        void RenderShadows(ShadowContext& ctx);
        void Render(const Camera& camera, Shader& shader, const ShadowContext* shadows = nullptr, const Environment* env = nullptr);

        float AmbientIntensity = 0.6f;     // strength of the environment light
        entt::registry& Registry() { return m_Registry; }

        void UpdateOrbitPositions();   // place orbiting entities from their current angle, without advancing it

        // Copy one entity's components, and build a new entity from such a copy
        EntitySnapshot CaptureEntity(entt::entity handle) const;
        entt::entity CreateFromSnapshot(const EntitySnapshot& s);
        // A new entity with copies of every component of `source` (it appears at the top of the Hierarchy)
        Entity DuplicateEntity(Entity source);

        // Copy the whole scene. selectedIndex receives the position of `selected` in the snapshot (-1 if none).
        SceneSnapshot Capture(entt::entity selected, int& selectedIndex);
        // Replace the scene with a snapshot. Returns the entity that corresponds to selectedIndex.
        entt::entity Restore(const SceneSnapshot& snapshot, int selectedIndex);

    private:
        entt::registry m_Registry;
    };

} // namespace Engine