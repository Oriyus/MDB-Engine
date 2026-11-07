#pragma once
#include <entt/entt.hpp>
#include <string>
#include "Scene/Components.h"
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Engine {

    class Entity;
    class Camera;
    class Shader;
    struct ShadowContext;
    class Environment;

    // A copy of one entity's component values, used to rewind after Play
    struct EntitySnapshot {
        std::string                                 Tag;
        uint64_t                                    ID = 0;
        TransformComponent                          Transform;
        std::optional<ParentComponent>              Parent;
        std::optional<MeshRendererComponent>        MeshRenderer;
        std::optional<PointLightComponent>          PointLight;
        std::optional<DirectionalLightComponent>    DirectionalLight;
        std::optional<SpinComponent>                Spin;
        std::optional<OrbitComponent>               Orbit;
        std::optional<RigidBodyComponent>           RigidBody;
        std::optional<ColliderComponent>            Collider;
        std::optional<ScriptComponent>              Script;
        std::optional<CameraComponent>              Camera;
        std::optional<AudioSourceComponent>         AudioSource;
        std::optional<AnimatorComponent>            Animator;
        std::optional<PrefabComponent>              Prefab;
    };
    using SceneSnapshot = std::vector<EntitySnapshot>;

    class Scene {
    public:
        Scene();
        Scene(const Scene&) = delete;              // the registry's signals point at this object
        Scene& operator=(const Scene&) = delete;

        Entity CreateEntity(const std::string& name = "Entity");
        void DestroyEntity(Entity entity);         // and all of its children, recursively

        // ---- hierarchy. Transforms are local to the parent; these give and take world placement.
        static uint64_t NewID();
        uint64_t GetID(entt::entity e) const;
        entt::entity FindByID(uint64_t id);        // entt::null if no such entity
        entt::entity GetParent(entt::entity e);    // entt::null for a root (or a dangling link)
        std::vector<entt::entity> GetChildren(entt::entity e);   // direct children
        bool IsDescendantOf(entt::entity e, entt::entity ancestor);
        // Re-parent (parent = entt::null makes it a root). keepWorld leaves it where it is on screen.
        // Refuses (returns false) to make an entity a child of itself or of one of its descendants.
        bool SetParent(entt::entity child, entt::entity parent, bool keepWorld = true);

        glm::mat4 GetWorldMatrix(entt::entity e);
        glm::mat4 GetParentWorldMatrix(entt::entity e);           // identity for a root
        void SetWorldMatrix(entt::entity e, const glm::mat4& world);   // sets the local transform to match
        void SetWorldPosition(entt::entity e, const glm::vec3& position);
        void SetWorldRotation(entt::entity e, const glm::quat& rotation);
        // Split a matrix into position, rotation and scale (no shear)
        static void Decompose(const glm::mat4& m, glm::vec3& position, glm::quat& rotation, glm::vec3& scale);

        void Clear();
        void OnUpdate(float dt);
        void RenderShadows(ShadowContext& ctx);
        // Opaque: opaque and cutout surfaces. Transparent: see-through (Blend) ones, sorted back to front,
        // drawn after the sky.
        enum class RenderPass { Opaque, Transparent };
        void Render(const Camera& camera, Shader& shader, const ShadowContext* shadows = nullptr, const Environment* env = nullptr,
            RenderPass pass = RenderPass::Opaque);

        float AmbientIntensity = 0.6f;     // strength of the environment light
        entt::registry& Registry() { return m_Registry; }

        void UpdateOrbitPositions();   // place orbiting entities from their current angle, without advancing it
        // Advance every Animator by dt and compute its bone matrices (dt 0: just pose it, e.g. in the editor)
        void UpdateAnimations(float dt);

        // Copy one entity's components, and build a new entity from such a copy
        EntitySnapshot CaptureEntity(entt::entity handle) const;
        entt::entity CreateFromSnapshot(const EntitySnapshot& s);
        // A new entity with copies of every component of `source` (it appears at the top of the Hierarchy),
        // under the same parent, with copies of all its children too. Copies get new IDs.
        Entity DuplicateEntity(Entity source);

        // Copy the whole scene. selectedIndex receives the position of `selected` in the snapshot (-1 if none).
        SceneSnapshot Capture(entt::entity selected, int& selectedIndex);
        // Replace the scene with a snapshot. Returns the entity that corresponds to selectedIndex.
        entt::entity Restore(const SceneSnapshot& snapshot, int selectedIndex);

    private:
        void OnIDChanged(entt::registry&, entt::entity) { m_IDMapDirty = true; }
        void SetSkinning(Shader& shader, entt::entity e, const MeshRendererComponent& mr);   // u_Skinned / u_Bones

        entt::registry m_Registry;
        std::unordered_map<uint64_t, entt::entity> m_IDMap;   // rebuilt lazily when IDs change
        bool m_IDMapDirty = true;
    };

} // namespace Engine