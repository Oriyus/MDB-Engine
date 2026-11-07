#pragma once
#include "Renderer/Renderable.h"
#include "Renderer/Texture.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>

namespace Engine {

    // Every entity gets Tag, Transform and ID automatically (see Scene::CreateEntity)
    struct TagComponent {
        std::string Tag;
    };

    // A stable identity: kept by snapshots (Play/Stop, undo) and scene files, unlike entt handles.
    // Parent links refer to it.
    struct IDComponent {
        uint64_t ID = 0;
    };

    // This entity is a child of the entity with that ID: its Transform is relative to the parent's.
    // Moving, rotating or scaling the parent carries its children along.
    struct ParentComponent {
        uint64_t Parent = 0;
    };

    // Position / Rotation / Scale are LOCAL: relative to the parent if there is one, else the world.
    // Scene::GetWorldMatrix gives the world placement.
    struct TransformComponent {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Rotation{ 0.0f };   // Euler angles in radians
        glm::vec3 Scale{ 1.0f };

        glm::mat4 GetMatrix() const {
            return glm::translate(glm::mat4(1.0f), Position)
                * glm::mat4_cast(glm::quat(Rotation))
                * glm::scale(glm::mat4(1.0f), Scale);
        }
    };

    enum class BodyType { Static, Dynamic, Kinematic };
    // ConvexHull and Mesh use the entity's MeshRenderer geometry (with its LocalOffset), scaled by the
    // entity: Size and Center are ignored for them. Mesh is exact but static/kinematic only.
    enum class ColliderShape { Box, Sphere, ConvexHull, Mesh };

    struct RigidBodyComponent {
        BodyType Type = BodyType::Dynamic;
        ColliderShape Shape = ColliderShape::Box;
        glm::vec3 Size{ 1.0f };          // box: full extents. sphere: diameter in x. Multiplied by the transform scale.
        glm::vec3 Center{ 0.0f };        // collider offset in the entity's local space
        float Mass = 1.0f;               // dynamic only
        float Friction = 0.5f;
        float Restitution = 0.2f;        // bounciness
        glm::vec3 InitialVelocity{ 0.0f };   // used once when the body is created (e.g. thrown balls)
        bool IsTrigger = false;          // sensor: reports overlaps (on_trigger_enter/exit) but doesn't collide
    };

    // Geometry + material in one component for now. We'll split material out later.
    struct MeshRendererComponent {
        std::shared_ptr<Renderable> Geometry;
        std::shared_ptr<Texture2D>  Texture;      // optional
        glm::vec3 Color{ 1.0f };                  // tints the texture, or is the flat color
        glm::mat4 LocalOffset{ 1.0f };            // applied before the entity transform (used by models)
        float Metallic = 0.0f;
        float Roughness = 0.5f;
        glm::vec3 Emissive{ 0.0f };               // linear, can exceed 1
        bool  Unlit = false;
        bool  UseModelMaterials = false;   // use the model file's materials; Color becomes a tint
    };

    struct PointLightComponent {
        glm::vec3 Color{ 1.0f };
        float Intensity = 1.0f;
        bool  CastShadows = true;
    };

    // Direction comes from the entity's rotation: the light travels along local -Z.
    // Position doesn't matter for the light itself (the entity just holds a marker).
    struct DirectionalLightComponent {
        glm::vec3 Color{ 1.0f, 0.95f, 0.9f };
        float Intensity = 0.4f;
        bool  CastShadows = true;
    };

    // Behaviour components: each one is paired with a system in Scene::OnUpdate
    struct SpinComponent {
        glm::vec3 Speed{ 0.0f };      // radians per second around each axis
    };

    struct OrbitComponent {
        glm::vec3 Center{ 0.0f };
        float Radius = 3.0f;
        float Height = 1.5f;
        float Speed = 1.0f;          // radians per second
        float Angle = 0.0f;
    };

    // A game camera. Like the sun, it looks along the entity's local -Z (scale is ignored).
    // During Play the viewport shows the primary camera (or the first one if none is primary).
    struct CameraComponent {
        float FOV = 60.0f;            // vertical, degrees
        float Near = 0.1f;
        float Far = 500.0f;
        bool  Primary = true;
    };

    // A sound attached to an entity, played while the game runs (see Audio/AudioEngine.h).
    // Clip is relative to the asset directory, e.g. "Sounds/lava_loop.wav".
    struct AudioSourceComponent {
        std::string Clip;
        float Volume = 1.0f;
        float Pitch = 1.0f;
        bool  Loop = true;
        bool  PlayOnStart = true;     // start when Play starts (or when the entity appears)
        bool  Spatial = true;         // positioned at the entity, quieter with distance
        float MinDistance = 1.0f;     // full volume inside this radius
        float MaxDistance = 25.0f;    // stops getting quieter beyond this
    };

    // Marks an entity as part of a prefab instance (see Scene/Prefab.h). Path is relative to the
    // asset directory, e.g. "Prefabs/coin.json". Node says which part of the prefab: 0 is the root,
    // other values are the keys of the prefab's child nodes.
    struct PrefabComponent {
        std::string Path;
        uint64_t Node = 0;
    };

    // A Lua script driving this entity while playing (see Scripting/ScriptEngine.h).
    // Path is relative to the asset directory, e.g. "Scripts/bob.lua".
    struct ScriptComponent {
        std::string Path;
    };

} // namespace Engine