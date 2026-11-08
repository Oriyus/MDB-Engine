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

        // Closest hit along the ray (direction need not be normalized). Triggers are ignored,
        // and so is `ignore` (pass the caster to not hit yourself).
        // staticOnly: only static bodies (the level itself, as the navigation bake sees it).
        bool Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
            RayHit& hit, entt::entity ignore = entt::null, bool staticOnly = false) const;
        // Every surface the ray enters, nearest first (one hit per convex body, one per front-facing
        // triangle of a mesh). Triggers are ignored.
        void RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
            std::vector<RayHit>& hits, bool staticOnly = false) const;
        // The world bounds of every static, non-trigger body. False if there are none.
        bool StaticBounds(glm::vec3& min, glm::vec3& max) const;

    private:
        struct Impl;                          // keeps Jolt's heavy headers out of everything else
        std::unique_ptr<Impl> m_Impl;
    };

} // namespace Engine
