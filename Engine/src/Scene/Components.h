#pragma once
#include "Renderer/Renderable.h"
#include "Renderer/Texture.h"
#include "Renderer/Material.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <vector>

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
    // Box: Size = full extents. Sphere: Size.x = diameter. Capsule: Size.x = diameter, Size.y = total
    // height (caps included), standing on the local Y axis. Sizes are multiplied by the entity's scale.
    // ConvexHull and Mesh use the entity's MeshRenderer geometry (with its LocalOffset), scaled by the
    // entity: Size and Center are ignored for them. Mesh is exact but static/kinematic only.
    // None: the body has no shape of its own, only its children's ColliderComponents.
    // (The numbers are stored in scene files: only add at the end.)
    enum class ColliderShape { Box, Sphere, ConvexHull, Mesh, Capsule, None };

    // A rigid body's shape is its own (Shape/Size/Center) plus the ColliderComponent of every
    // descendant that has no RigidBody itself: together they make one compound collider.
    struct RigidBodyComponent {
        BodyType Type = BodyType::Dynamic;
        ColliderShape Shape = ColliderShape::Box;
        glm::vec3 Size{ 1.0f };          // see ColliderShape
        glm::vec3 Center{ 0.0f };        // collider offset in the entity's local space
        float Mass = 1.0f;               // dynamic only
        float Friction = 0.5f;
        float Restitution = 0.2f;        // bounciness
        glm::vec3 InitialVelocity{ 0.0f };   // used once when the body is created (e.g. thrown balls)
        bool IsTrigger = false;          // sensor: reports overlaps (on_trigger_enter/exit) but doesn't collide
    };

    // An extra shape for the rigid body of the nearest ancestor that has one (a compound collider):
    // a table is a body with a top and four legs as child colliders. It has no body of its own and
    // sits where its entity is. Shape None is not used here.
    struct ColliderComponent {
        ColliderShape Shape = ColliderShape::Box;
        glm::vec3 Size{ 1.0f };
        glm::vec3 Center{ 0.0f };
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
        // Transparency. With model materials the stronger of this and the material's mode is used
        // (Opaque < Cutout < Blend), and Opacity multiplies the material's alpha.
        AlphaMode Alpha = AlphaMode::Opaque;
        float Opacity = 1.0f;                     // 0..1, multiplies the texture's alpha
        float AlphaCutoff = 0.5f;                 // Cutout: drop pixels below this alpha
        bool  CastShadows = true;                 // see-through parts cast where alpha >= 0.5
        // Static: never moves or changes while playing. Static parts with the same material are merged
        // into a few big meshes when Play starts (Scene::BuildStaticBatches), saving per-object work.
        bool  Static = false;
        bool  Merged = false;                     // runtime: drawn by a static batch, not on its own (not saved)
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

    // A named moment in a clip. It fires when playback crosses Time (also across a loop), for the base
    // clip and for layers. Scripts get on_anim_event(self, name, clip); a name "sound:Sounds/x.wav"
    // plays that sound at the entity instead.
    struct AnimEvent {
        std::string Clip;
        float Time = 0.0f;                // seconds into the clip
        std::string Name;
    };

    // Another clip played over the base clip on part of the skeleton: Mask is a bone (node) name and
    // everything below it ("" = the whole skeleton). Weight 0..1 blends it in over the base pose
    // (override), or scales the motion it adds (Additive).
    struct AnimLayer {
        std::string Clip;
        std::string Mask;
        float Weight = 1.0f;
        float Speed = 1.0f;
        bool  Loop = true;
        // Additive: add the clip's motion relative to its first frame on top of the pose, instead of
        // replacing it (a head turn or breathing over any walk)
        bool  Additive = false;

        // Runtime state (not saved)
        float Time = 0.0f;
        float TargetWeight = -1.0f;       // >= 0: Weight is fading towards it
        float FadeRate = 0.0f;            // weight per second
    };

    // Inverse kinematics on the final pose (after layers). The target is another entity (TargetID, its
    // world position + Offset) or, with TargetID 0, the world point Target.
    //   Look: turn Bone (a head) to face the target, spread over Chain bones (the head and its parents),
    //         limited to MaxAngle degrees from the way the entity faces. "Facing" is the entity's +Z.
    //   Limb: two-bone IK. Bone is the end (paw, foot, hand); its parent and grandparent bend so it
    //         reaches the target. Pole (entity space, optional) is the direction the middle joint bends.
    enum class IKType : int { Look = 0, Limb = 1 };     // stored in scene files: append only
    struct AnimIK {
        IKType Type = IKType::Look;
        std::string Bone;
        uint64_t TargetID = 0;            // an entity's IDComponent; 0 = use Target
        glm::vec3 Target{ 0.0f };         // world point (TargetID 0), or offset from the target entity
        float Weight = 1.0f;
        float MaxAngle = 70.0f;           // Look: degrees
        int   Chain = 2;                  // Look: how many bones turn (1 = just Bone)
        glm::vec3 Pole{ 0.0f };           // Limb: bend direction in entity space (0 = keep the current bend)
    };

    // Plays a skeletal animation on this entity's Mesh Renderer model (one with bones). Changing
    // Clip cross-fades from the old clip over BlendTime seconds. Layers go on top, in order.
    struct AnimatorComponent {
        std::string Clip;                 // animation name ("" = the rest pose)
        float Speed = 1.0f;
        bool  Loop = true;
        bool  Playing = true;
        float BlendTime = 0.25f;          // seconds
        std::vector<AnimLayer> Layers;
        std::vector<AnimEvent> Events;
        // Root motion: the root bone's horizontal movement in the base clip moves the entity instead of
        // the mesh (the bone stays put sideways; vertical bob stays in the animation). RootBone "" = the
        // topmost bone the clip moves. Works on the transform: use no body or a kinematic one.
        bool RootMotion = false;
        std::string RootBone;
        std::vector<AnimIK> IK;           // solved after layers, in order

        // Runtime state (not saved)
        float Time = 0.0f;                // seconds into ActiveClip
        std::string ActiveClip;           // what Time belongs to; a different Clip starts a fade
        std::string FadeFrom;             // the clip being faded out
        float FadeFromTime = 0.0f;
        float Fade = 1.0f;                // 0 = all FadeFrom, 1 = all ActiveClip
        std::vector<glm::mat4> Palette;   // bone matrices for the renderer (empty: not posed yet)
        // Events that fired this frame as (name, clip), plus "end" when a non-looping clip finishes.
        // Filled by Scene::UpdateAnimations, delivered by ScriptEngine::DispatchAnimationEvents.
        std::vector<std::pair<std::string, std::string>> Fired;
        std::vector<glm::mat4> Globals;   // each node in model space after IK (bone_position; empty: not posed)
    };

    // A Lua script driving this entity while playing (see Scripting/ScriptEngine.h).
    // Path is relative to the asset directory, e.g. "Scripts/bob.lua".
    struct ScriptComponent {
        std::string Path;
    };

} // namespace Engine