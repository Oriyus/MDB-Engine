#pragma once
#include <entt/entt.hpp>
#include <string>
#include "Scene/Components.h"
#include "Renderer/DrawList.h"
#include "Scene/Decal.h"
#include "Scene/Fog.h"
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Engine {

    class Entity;
    class Camera;
    class Shader;
    class Mesh;
    struct ShadowContext;
    struct ShadowSettings;
    class Environment;
    class DrawList;
    struct ParticleDraw;

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
        std::optional<ParticleEmitterComponent>     Particles;
        std::optional<FoliageComponent>             Foliage;
        std::optional<PrefabComponent>              Prefab;
        bool                                        Transient = false;
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
        // The Euler angles (radians) that TransformComponent turns back into q (glm::quat(euler): R = Rz * Ry * Rx).
        // Stable next to gimbal lock (y near +-90 degrees), where glm::eulerAngles loses several degrees: the result
        // always rebuilds the same rotation, and roll is 0 right at the lock.
        static glm::vec3 EulerFromQuat(const glm::quat& q);

        void Clear();
        void OnUpdate(float dt);
        // Collect everything to draw this frame (camera-culled, batched for instancing) and upload it.
        // shadows: also fit the shadow views (sun cascades follow the camera) and sort the casters into them (null: none). The render calls below draw from it.
        // Static mesh merging: merge every Static Mesh Renderer part (same material and 16 m cell) into a few
        // world-space meshes, drawn instead of the entities. GameSession::Start builds them; Clear and Restore
        // (Stop) drop them. A merged entity being destroyed rebuilds them. Moving or recolouring one does not.
        void BuildStaticBatches();
        void ClearStaticBatches();
        uint32_t StaticBatchCount() const { return (uint32_t)m_StaticBatches.size(); }
        uint32_t StaticMergedCount() const { return m_StaticMerged; }   // entities drawn by the batches

        void BuildDrawList(DrawList& list, const Camera& camera, const ShadowContext* shadows);
        void RenderShadows(ShadowContext& ctx, const DrawList& list);
        // Opaque: opaque and cutout surfaces. Transparent: see-through (Blend) ones, sorted back to front,
        // drawn after the sky.
        enum class RenderPass { Opaque, Transparent };
        void Render(const Camera& camera, Shader& shader, const DrawList& list, const ShadowContext* shadows = nullptr,
            const Environment* env = nullptr, RenderPass pass = RenderPass::Opaque);

        float AmbientIntensity = 0.6f;     // strength of the environment light
        std::vector<Decal> Decals;         // this frame's decals (scripts add them with decal.draw; drawn by SceneRenderer)
        FogOfWarState Fog;                 // fog of war (fog.* in Lua, Navigation/FogOfWar; drawn by SceneRenderer)
        // Grass pushes from scripts (foliage.push), used and cleared by the foliage renderer on the next frame
        struct FoliagePush { glm::vec3 Position; float Radius = 0.5f; float Strength = 1.0f; };
        std::vector<FoliagePush> FoliagePushes;
        // The lighting uniforms of lit.frag (camera, lights, sun cascades, environment) on any shader that uses it
        void SetLightingUniforms(const Camera& camera, Shader& shader, const DrawList& list, const ShadowContext* shadows,
            const Environment* env);
    private:
        void DrawListBatches(Shader& shader, const DrawList& list, RenderPass pass);
    public:
        entt::registry& Registry() { return m_Registry; }

        void UpdateOrbitPositions();   // place orbiting entities from their current angle, without advancing it
        // Advance every Animator by dt and compute its bone matrices (dt 0: just pose it, e.g. in the editor)
        void UpdateAnimations(float dt);
        // Where a bone of an animated model is now, in world space (after IK). NaN if unknown.
        glm::vec3 BonePosition(entt::entity e, const std::string& bone);

        // Particle emitters (Scene/Particles.cpp): advance them by dt (spawn, move, expire), and build this
        // frame's draw lists for ParticleRenderer. The editor also runs UpdateParticles in Edit mode (a preview).
        void UpdateParticles(float dt);
        void GatherParticles(const Camera& camera, std::vector<ParticleDraw>& draws);
        uint32_t ParticleCount() const;
        void ResetParticles();             // every emitter starts over (Play does this: the editor preview ran them)
        // The world direction a bone faces: the direction that, at rest, points where the entity faces (+Z)
        glm::vec3 BoneForward(entt::entity e, const std::string& bone);

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
        void OnMeshRendererGone(entt::registry& reg, entt::entity e);
        const std::vector<glm::mat4>* GetBonePalette(entt::entity e, const MeshRendererComponent& mr) const;   // null: not skinned

        entt::registry m_Registry;
        std::unordered_map<uint64_t, entt::entity> m_IDMap;   // rebuilt lazily when IDs change
        bool m_IDMapDirty = true;

        // Static mesh merging: built on Play (BuildStaticBatches), dropped on Clear / ClearStaticBatches
        struct StaticBatch {
            std::shared_ptr<Mesh> Geometry;  // world-space vertices of every merged part
            DrawBatch State;
            InstanceData Instance;           // identity transform + the shared material values
            glm::vec3 Min{ 0.0f }, Max{ 0.0f };
            bool CastShadows = true;
        };
        std::vector<StaticBatch> m_StaticBatches;
        bool m_StaticActive = false, m_StaticDirty = false;
        uint32_t m_StaticMerged = 0;
    };

} // namespace Engine