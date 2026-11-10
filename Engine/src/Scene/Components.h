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
    // Runtime-only: never written to save games (a script's overlay markers, one-shot effects). Set by
    // scripts (e.transient) and scene.spawn_particles. Not stored in scene files either.
    struct TransientComponent {
        bool Value = true;
    };

    struct ParentComponent {
        uint64_t Parent = 0;
    };

    // Position / Rotation / Scale are LOCAL: relative to the parent if there is one, else the world.
    // Scene::GetWorldMatrix gives the world placement.
    struct TransformComponent {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Rotation{ 0.0f };   // Euler angles in radians
        glm::vec3 Scale{ 1.0f };

        // translate * rotate * scale, built directly: the rotation's columns times the scale, then the position.
        // The same numbers as multiplying the three matrices (those products only add zeros), without the two
        // 4x4 multiplies (it runs for every object every frame: 7 us each in a Debug build).
        glm::mat4 GetMatrix() const {
            const glm::mat3 r = glm::mat3_cast(glm::quat(Rotation));
            return glm::mat4(glm::vec4(r[0] * Scale.x, 0.0f), glm::vec4(r[1] * Scale.y, 0.0f),
                             glm::vec4(r[2] * Scale.z, 0.0f), glm::vec4(Position, 1.0f));
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
        // Double-sided: back faces are drawn too (not culled) and lit from their own side, so a single plane is
        // visible from behind (leaves, cloth, a sign). With model materials, a part is double-sided when this or
        // its material says so. Back faces cost extra only on these parts.
        bool  DoubleSided = false;
        // Static: never moves or changes while playing. Static parts with the same material are merged
        // into a few big meshes when Play starts (Scene::BuildStaticBatches), saving per-object work.
        bool  Static = false;
        // Sway in the scene's wind (Scene::Wind; lit.vert / shadow.vert): 0 = still, 1 = a tree. The higher a
        // vertex is in the mesh the more it moves, so the base stays put. A swaying part is never merged.
        float Wind = 0.0f;
        bool  Merged = false;                     // runtime: drawn by a static batch, not on its own (not saved)
        // Runtime hit flash (e:flash in Lua, not saved): FlashColor glows on top of the emissive colour, fading
        // out over FlashDuration seconds. Merged static parts don't flash.
        glm::vec3 FlashColor{ 0.0f };
        float FlashTime = 0.0f, FlashDuration = 0.0f;
        bool  Hidden = false;                     // runtime (e.visible = false in Lua, not saved): not drawn, casts no shadow
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

    // One live particle (simulated on the CPU by Scene::UpdateParticles)
    struct Particle {
        glm::vec3 Position{ 0.0f };       // world space (or the emitter's space when not WorldSpace)
        glm::vec3 Velocity{ 0.0f };
        float Age = 0.0f, Life = 1.0f;    // seconds
        float Size = 0.1f;                // size at birth (metres)
        float Rotation = 0.0f, Spin = 0.0f;   // radians, radians per second
    };

    // Emits camera-facing particles: fire, smoke, sparks, dust. Simulated in Edit mode too (a live
    // preview) and drawn after the see-through pass. See ApplyParticlePreset for ready-made looks.
    enum class EmitShape : int { Point = 0, Sphere = 1, Box = 2 };   // stored in scene files: append only
    struct ParticleEmitterComponent {
        bool  Emitting = true;            // spawn Rate particles per second
        float Rate = 20.0f;
        int   Burst = 0;                  // particles spawned at once when the emitter starts
        int   MaxParticles = 500;
        glm::vec2 Lifetime{ 1.0f, 2.0f }; // seconds, random between
        glm::vec2 Speed{ 1.0f, 2.0f };    // metres per second, random between
        glm::vec3 Direction{ 0.0f, 1.0f, 0.0f };   // in the entity's space
        float Spread = 25.0f;             // cone half-angle, degrees (180 = every way)
        EmitShape Shape = EmitShape::Point;
        glm::vec3 ShapeSize{ 0.5f };      // sphere: radius in x; box: full size
        float GravityScale = 0.0f;        // 1 = falls like a dropped object
        float Drag = 0.0f;                // velocity lost per second (0..)
        glm::vec2 Size{ 0.15f, 0.25f };   // at birth, metres, random between
        float EndSize = 1.0f;             // size at death, as a multiple of the birth size
        glm::vec4 StartColor{ 1.0f, 1.0f, 1.0f, 1.0f };   // sRGB-style + alpha, like the colour pickers
        glm::vec4 EndColor{ 1.0f, 1.0f, 1.0f, 0.0f };
        float Emission = 1.0f;            // brightness multiplier (> 1 glows with bloom)
        glm::vec2 SpinSpeed{ 0.0f, 0.0f };   // degrees per second, random between (and a random start angle if non-zero)
        std::shared_ptr<Texture2D> Texture;  // optional; none = a soft round dot
        bool  Additive = false;           // add light (fire, sparks, magic) instead of covering (smoke, dust)
        bool  WorldSpace = true;          // particles stay where they were emitted (else they move with the entity)
        bool  DestroyWhenDone = false;    // remove the entity once it stops emitting and its particles are gone

        // Runtime state (not saved)
        std::vector<Particle> Particles;
        float SpawnCarry = 0.0f;          // fractional particles owed by Rate
        bool  Started = false;            // the start Burst has been emitted
        int   PendingBurst = 0;           // queued by scripts / the Inspector
    };

    // Grass and flowers over an area: GPU-instanced clumps (Renderer/FoliageRenderer, Shaders/foliage.vert with
    // lit.frag). The clumps are generated from these settings and Seed every time they change, never stored.
    // They sway in the wind and bend away from moving bodies (dynamic and kinematic) and script pushes
    // (foliage.push), then stand up again over Recovery seconds.
    struct FoliageComponent {
        glm::vec2 Size{ 20.0f, 20.0f };   // the area in the entity's X/Z, centred on it; clumps stand on its Y
        float Density = 6.0f;             // clumps per square metre
        uint32_t Seed = 1;
        float Height = 0.45f;             // metres
        float HeightVariation = 0.4f;     // each clump is Height x (1 +- this)
        float Width = 1.0f;               // blade width and clump spread
        glm::vec3 BaseColor{ 0.13f, 0.19f, 0.06f };   // sRGB-style, like the colour pickers: blade roots
        glm::vec3 TipColor{ 0.50f, 0.60f, 0.22f };    // blade tips
        glm::vec3 DryColor{ 0.66f, 0.58f, 0.32f };    // tips where the grass is dry (in patches)
        float DryAmount = 0.3f;           // 0..1: how much of the area is dry
        float FlowerAmount = 0.04f;       // 0..1: share of clumps that are flowers
        float Roughness = 0.75f;
        float WindStrength = 1.0f;        // 0 = still
        float WindSpeed = 1.0f;
        float WindDirection = 30.0f;      // degrees around Y; 0 blows towards +X
        float Interaction = 1.0f;         // how far bodies and pushes bend the blades (0 = not at all)
        float Recovery = 1.2f;            // seconds for pressed grass to stand up again
        float DrawDistance = 45.0f;       // metres; clumps thin out over the last half of it
        bool  AvoidStatic = true;         // no clumps under Static meshes standing on the ground (walls, floors, props)
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