#pragma once
#include <cstdint>
#include <memory>

namespace Engine {

    class Scene;

    class PhysicsWorld {
    public:
        PhysicsWorld();
        ~PhysicsWorld();
        PhysicsWorld(const PhysicsWorld&) = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;

        // Advance the simulation. Creates bodies for new entities, drops bodies of removed ones,
        // runs fixed 60 Hz steps, and writes the (interpolated) result back into the transforms.
        // snapToLatest = true shows the newest state directly (used for single-stepping).
        void Step(Scene& scene, float dt, bool snapToLatest = false);

        void Reset();                         // destroy every body (call on Play and Stop)
        void SetGravity(float down);          // positive = downward, m/s^2
        uint32_t BodyCount() const;

    private:
        struct Impl;                          // keeps Jolt's heavy headers out of everything else
        std::unique_ptr<Impl> m_Impl;
    };

} // namespace Engine