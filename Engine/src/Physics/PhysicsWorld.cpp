#include <Jolt/Jolt.h>                    // must be first
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

#include "Physics/PhysicsWorld.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Core/Log.h"

#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <thread>
#include <unordered_map>

namespace Engine {

    namespace {

        constexpr float kFixedStep = 1.0f / 60.0f;
        constexpr int   kMaxSteps = 5;             // after a long hitch, drop time instead of spiralling

        // Two object layers: things that never move, and things that do
        namespace Layers {
            constexpr JPH::ObjectLayer Static = 0;
            constexpr JPH::ObjectLayer Moving = 1;
            constexpr JPH::uint Count = 2;
        }
        namespace BPLayers {
            constexpr JPH::BroadPhaseLayer Static(0);
            constexpr JPH::BroadPhaseLayer Moving(1);
            constexpr JPH::uint Count = 2;
        }

        class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
        public:
            bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
                if (a == Layers::Static) return b == Layers::Moving;   // static things never collide with each other
                return true;
            }
        };

        class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
        public:
            BPLayerInterfaceImpl() {
                m_Map[Layers::Static] = BPLayers::Static;
                m_Map[Layers::Moving] = BPLayers::Moving;
            }
            JPH::uint GetNumBroadPhaseLayers() const override { return BPLayers::Count; }
            JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return m_Map[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
            const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override {
                return l == BPLayers::Static ? "STATIC" : "MOVING";
            }
#endif
        private:
            JPH::BroadPhaseLayer m_Map[Layers::Count];
        };

        class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
        public:
            bool ShouldCollide(JPH::ObjectLayer l1, JPH::BroadPhaseLayer l2) const override {
                if (l1 == Layers::Static) return l2 == BPLayers::Moving;
                return true;
            }
        };

        inline JPH::Vec3 ToJolt(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
        inline JPH::Quat ToJolt(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
        inline glm::vec3 ToGlm(JPH::Vec3 v) { return glm::vec3(v.GetX(), v.GetY(), v.GetZ()); }
        inline glm::quat ToGlm(JPH::Quat q) { return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }

    } // namespace

    struct Binding {
        JPH::BodyID Id;
        BodyType Type = BodyType::Dynamic;
        glm::vec3 PrevPos{ 0.0f }, CurPos{ 0.0f };      // the last two physics states, for interpolation
        glm::quat PrevRot{ 1, 0, 0, 0 }, CurRot{ 1, 0, 0, 0 };
    };

    struct PhysicsWorld::Impl {
        Impl()
            : Temp(10 * 1024 * 1024),
            Jobs(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                std::max(1, (int)std::thread::hardware_concurrency() - 1)) {}

        // Declaration order = construction order. System is last, so it is destroyed first.
        JPH::TempAllocatorImpl Temp;
        JPH::JobSystemThreadPool Jobs;
        BPLayerInterfaceImpl BPInterface;
        ObjectVsBroadPhaseLayerFilterImpl ObjVsBP;
        ObjectLayerPairFilterImpl ObjPair;
        JPH::PhysicsSystem System;

        std::unordered_map<entt::entity, Binding> Bindings;
        float Accumulator = 0.0f;

        void RemoveAll() {
            auto& bi = System.GetBodyInterface();
            for (auto& [e, b] : Bindings) {
                bi.RemoveBody(b.Id);
                bi.DestroyBody(b.Id);
            }
            Bindings.clear();
            Accumulator = 0.0f;
        }

        void CreateBody(entt::entity e, const TransformComponent& t, const RigidBodyComponent& rb) {
            const glm::vec3 scale = glm::abs(t.Scale);

            JPH::ShapeRefC shape;
            if (rb.Shape == ColliderShape::Sphere) {
                float r = std::max(0.01f, 0.5f * rb.Size.x * std::max({ scale.x, scale.y, scale.z }));
                shape = new JPH::SphereShape(r);
            }
            else {
                glm::vec3 half = glm::max(0.5f * rb.Size * scale, glm::vec3(0.01f));
                float convexRadius = std::min(0.05f, std::min({ half.x, half.y, half.z }));
                shape = new JPH::BoxShape(ToJolt(half), convexRadius);
            }

            const glm::vec3 offset = rb.Center * t.Scale;
            if (glm::dot(offset, offset) > 1e-10f)
                shape = new JPH::RotatedTranslatedShape(ToJolt(offset), JPH::Quat::sIdentity(), shape.GetPtr());

            const glm::quat q(t.Rotation);
            JPH::EMotionType motion = rb.Type == BodyType::Static ? JPH::EMotionType::Static
                : rb.Type == BodyType::Kinematic ? JPH::EMotionType::Kinematic
                : JPH::EMotionType::Dynamic;
            JPH::ObjectLayer layer = rb.Type == BodyType::Static ? Layers::Static : Layers::Moving;

            JPH::BodyCreationSettings bs(shape, ToJolt(t.Position), ToJolt(q), motion, layer);
            bs.mFriction = rb.Friction;
            bs.mRestitution = rb.Restitution;
            if (motion == JPH::EMotionType::Dynamic) {
                bs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                bs.mMassPropertiesOverride.mMass = std::max(rb.Mass, 0.001f);
            }

            auto& bi = System.GetBodyInterface();
            JPH::BodyID id = bi.CreateAndAddBody(bs, motion == JPH::EMotionType::Static
                ? JPH::EActivation::DontActivate
                : JPH::EActivation::Activate);
            if (id.IsInvalid()) { ENGINE_ERROR("Physics: out of bodies"); return; }

            if (motion == JPH::EMotionType::Dynamic &&
                glm::dot(rb.InitialVelocity, rb.InitialVelocity) > 0.0f)
                bi.SetLinearVelocity(id, ToJolt(rb.InitialVelocity));

            Binding b;
            b.Id = id;
            b.Type = rb.Type;
            b.PrevPos = b.CurPos = t.Position;
            b.PrevRot = b.CurRot = q;
            Bindings[e] = b;
        }

        // Keep the physics world in step with the scene: new entities get bodies, removed ones lose them
        void Sync(entt::registry& reg) {
            auto& bi = System.GetBodyInterface();
            for (auto it = Bindings.begin(); it != Bindings.end();) {
                if (!reg.valid(it->first) || !reg.all_of<RigidBodyComponent>(it->first)) {
                    bi.RemoveBody(it->second.Id);
                    bi.DestroyBody(it->second.Id);
                    it = Bindings.erase(it);
                }
                else {
                    ++it;
                }
            }
            reg.view<TransformComponent, RigidBodyComponent>().each(
                [&](entt::entity e, TransformComponent& t, RigidBodyComponent& rb) {
                    if (!Bindings.count(e)) CreateBody(e, t, rb);
                });
        }
    };

    PhysicsWorld::PhysicsWorld() {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();

        m_Impl = std::make_unique<Impl>();
        m_Impl->System.Init(4096, 0, 4096, 4096, m_Impl->BPInterface, m_Impl->ObjVsBP, m_Impl->ObjPair);
        m_Impl->System.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
        ENGINE_INFO("Physics ready (Jolt)");
    }

    PhysicsWorld::~PhysicsWorld() {
        m_Impl.reset();
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    void PhysicsWorld::Reset() { m_Impl->RemoveAll(); }

    void PhysicsWorld::SetGravity(float down) { m_Impl->System.SetGravity(JPH::Vec3(0.0f, -down, 0.0f)); }

    uint32_t PhysicsWorld::BodyCount() const { return (uint32_t)m_Impl->Bindings.size(); }

    void PhysicsWorld::Step(Scene& scene, float dt, bool snapToLatest) {
        Impl& I = *m_Impl;
        auto& reg = scene.Registry();
        auto& bi = I.System.GetBodyInterface();

        I.Sync(reg);

        I.Accumulator += std::min(dt, 0.25f);
        int steps = (int)(I.Accumulator / kFixedStep);
        if (steps > kMaxSteps) { steps = kMaxSteps; I.Accumulator = 0.0f; }
        else                   I.Accumulator -= steps * kFixedStep;

        if (steps > 0) {
            // Kinematic bodies follow their transform: give them the velocity that reaches it
            // exactly when this batch of steps ends, so they push dynamic bodies properly
            const float span = steps * kFixedStep;
            for (auto& [e, b] : I.Bindings) {
                if (b.Type != BodyType::Kinematic) continue;
                const auto& t = reg.get<TransformComponent>(e);
                bi.MoveKinematic(b.Id, ToJolt(t.Position), ToJolt(glm::quat(t.Rotation)), span);
            }

            for (int i = 0; i < steps; i++) {
                for (auto& [e, b] : I.Bindings)
                    if (b.Type == BodyType::Dynamic) { b.PrevPos = b.CurPos; b.PrevRot = b.CurRot; }

                I.System.Update(kFixedStep, 1, &I.Temp, &I.Jobs);

                for (auto& [e, b] : I.Bindings) {
                    if (b.Type != BodyType::Dynamic) continue;
                    b.CurPos = ToGlm(JPH::Vec3(bi.GetPosition(b.Id)));
                    b.CurRot = ToGlm(bi.GetRotation(b.Id));
                }
            }
        }

        // Draw between the last two physics states, so motion is smooth at any frame rate
        const float alpha = snapToLatest ? 1.0f : I.Accumulator / kFixedStep;
        for (auto& [e, b] : I.Bindings) {
            if (b.Type != BodyType::Dynamic) continue;
            auto& t = reg.get<TransformComponent>(e);
            t.Position = glm::mix(b.PrevPos, b.CurPos, alpha);
            t.Rotation = glm::eulerAngles(glm::slerp(b.PrevRot, b.CurRot, alpha));
        }
    }

} // namespace Engine