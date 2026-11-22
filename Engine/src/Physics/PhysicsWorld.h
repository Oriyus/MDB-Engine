#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <vector>

namespace Engine {

    class Scene;

    // One begin or end of contact between two entities, gathered during Step().
    // Each touching pair produces one Begin when it starts touching and one End when it separates,
    // however many fixed steps or contact points that spans. Point and Normal are only set on Begin;
    // Normal points from A towards B.
    struct CollisionEvent {
        entt::entity A = entt::null, B = entt::null;
        bool Begin = true;          // false = the pair separated
        bool Trigger = false;       // at least one of the two is a trigger (sensor)
        glm::vec3 Point{ 0.0f };
        glm::vec3 Normal{ 0.0f };
    };

    struct RayHit {
        entt::entity Entity = entt::null;
        glm::vec3 Point{ 0.0f };
        glm::vec3 Normal{ 0.0f };
        float Distance = 0.0f;
    };

    // What a character controller (CharacterControllerComponent) stands on, after the last fixed step
    struct CharacterState {
        bool Grounded = false;            // on walkable ground (not steeper than MaxSlope)
        bool OnSteepGround = false;       // touching ground too steep to stand on (it slides down)
        glm::vec3 Velocity{ 0.0f };       // m/s, world: what it moved with in the last step
        glm::vec3 GroundNormal{ 0.0f, 1.0f, 0.0f };
        glm::vec3 GroundVelocity{ 0.0f }; // the ground's own velocity there (a moving platform)
        glm::vec3 GroundPoint{ 0.0f };
        entt::entity GroundEntity = entt::null;
    };

    class PhysicsWorld {
    public:
        PhysicsWorld();
        ~PhysicsWorld();
        PhysicsWorld(const PhysicsWorld&) = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;

        // Advance the simulation. Creates bodies for new entities, drops bodies of removed ones,
        // runs fixed 60 Hz steps, and writes the (interpolated) result back into the transforms.
        // snapToLatest = true shows the newest state directly (used for single-stepping).
        // Transforms changed from outside (scripts, the gizmo) teleport the body there.
        void Step(Scene& scene, float dt, bool snapToLatest = false);

        // Create bodies for entities that don't have one yet (and drop stale ones) without stepping.
        // Step() does this too; call it after Play starts so scripts can reach bodies in on_create.
        void Sync(Scene& scene);

        // Contacts that started or ended during the last Step(), in order
        const std::vector<CollisionEvent>& Events() const;

        void Reset();                         // destroy every body (call on Play and Stop)
        void SetGravity(float down);          // positive = downward, m/s^2
        uint32_t BodyCount() const;

        // ---- Per-entity access (for scripts). An entity without a body yet gets one on the spot
        // if it has a RigidBodyComponent; otherwise these do nothing and return false / zero.
        bool HasBody(entt::entity e) const;
        void Invalidate(entt::entity e);      // drop the body; it is rebuilt from the component on next use
        glm::vec3 GetLinearVelocity(entt::entity e);
        void SetLinearVelocity(entt::entity e, const glm::vec3& v);
        glm::vec3 GetAngularVelocity(entt::entity e);
        void SetAngularVelocity(entt::entity e, const glm::vec3& w);
        void AddForce(entt::entity e, const glm::vec3& force);       // newtons, for this frame: call every frame for a steady push
        void AddImpulse(entt::entity e, const glm::vec3& impulse);   // N*s, applied at once

        // ---- Character controllers (CharacterControllerComponent). Each fixed step: while grounded, the vertical
        // velocity becomes the ground's (so it rides platforms and doesn't bounce down slopes); in the air it keeps its
        // own; gravity x GravityScale is added; the sideways velocity is the wanted one from SetCharacterMove, plus
        // the ground's while standing. A character gets created on the spot like bodies (Find).
        bool HasCharacter(entt::entity e) const;
        // The wanted sideways velocity (m/s, world; y is ignored). Kept until changed: set it every frame.
        void SetCharacterMove(entt::entity e, const glm::vec3& velocity);
        glm::vec3 GetCharacterMove(entt::entity e);
        // Leave the ground upwards at this speed in the next fixed step. Does nothing (returns false) unless
        // standing on walkable ground now.
        bool CharacterJump(entt::entity e, float speed);
        // Replace the velocity (a launch, a knock-up): the vertical part is kept until it lands; the sideways part
        // is the wanted move from the next step on.
        void SetCharacterVelocity(entt::entity e, const glm::vec3& velocity);
        CharacterState GetCharacterState(entt::entity e);
        // Change the capsule (crouch / stand up). False, and nothing changes, if the new capsule would be inside
        // something (no room to stand up). On success the component is updated too.
        bool SetCharacterShape(entt::entity e, float height, float radius, const glm::vec3& center);
        uint32_t CharacterCount() const;

        // Closest hit along the ray (direction need not be normalized). Triggers are ignored,
        // and so is `ignore` (pass the caster to not hit yourself).
        // staticOnly: only static bodies (the level itself, as the navigation bake sees it).
        bool Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
            RayHit& hit, entt::entity ignore = entt::null, bool staticOnly = false) const;
        // Every surface the ray enters, nearest first (one hit per convex body, one per front-facing
        // triangle of a mesh). Triggers are ignored.
        void RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
            std::vector<RayHit>& hits, bool staticOnly = false) const;
        // Sweep a sphere of `radius` along the ray: the first thing it touches (triggers ignored, `ignore` skipped).
        // Distance is how far the centre got; Point is the contact, Normal points back towards the sphere. A sphere
        // that starts touching something hits at distance 0. For cameras (keeping them out of walls) and wide probes.
        bool SphereCast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, float radius,
            RayHit& hit, entt::entity ignore = entt::null, bool staticOnly = false) const;
        // Every body a shape placed in the world touches right now (weapon hits, explosions, "who is near"): one hit
        // per entity, nearest first. Point is the deepest contact on that body (on its surface); Distance is from the
        // query's centre (sphere) or its segment (capsule) to that point. `ignore` is skipped (a body or
        // a character's inner capsule); includeStatic = false leaves the level out; triggers only with includeTriggers.
        void OverlapSphere(const glm::vec3& center, float radius, std::vector<RayHit>& hits,
            entt::entity ignore = entt::null, bool includeStatic = true, bool includeTriggers = false) const;
        // A capsule from a to b (the centres of its two end spheres) of `radius`: a sword's blade, a sweep along a swing.
        // a == b is a sphere.
        void OverlapCapsule(const glm::vec3& a, const glm::vec3& b, float radius, std::vector<RayHit>& hits,
            entt::entity ignore = entt::null, bool includeStatic = true, bool includeTriggers = false) const;

        // The world bounds of every static, non-trigger body. False if there are none.
        bool StaticBounds(glm::vec3& min, glm::vec3& max) const;

    private:
        struct Impl;                          // keeps Jolt's heavy headers out of everything else
        std::unique_ptr<Impl> m_Impl;
    };

} // namespace Engine
