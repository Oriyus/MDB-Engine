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
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>

#include "Physics/PhysicsWorld.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Core/Log.h"

#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <mutex>
#include <thread>
#include <cfloat>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

        // Jolt calls these from its worker threads while every body is locked, so they only
        // record what happened; PhysicsWorld turns the records into events on the main thread.
        class ContactRecorder final : public JPH::ContactListener {
        public:
            struct Record {
                JPH::BodyID A, B;
                bool Added = true;
                glm::vec3 Point{ 0.0f }, Normal{ 0.0f };
            };

            void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
                const JPH::ContactManifold& m, JPH::ContactSettings&) override {
                Record r;
                r.A = b1.GetID();
                r.B = b2.GetID();
                r.Point = ToGlm(JPH::Vec3(m.GetWorldSpaceContactPointOn1(0)));
                r.Normal = ToGlm(m.mWorldSpaceNormal);   // moves body 2 out of body 1: points 1 -> 2
                std::lock_guard lock(m_Mutex);
                m_Records.push_back(r);
            }

            void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
                Record r;
                r.A = pair.GetBody1ID();
                r.B = pair.GetBody2ID();
                r.Added = false;
                std::lock_guard lock(m_Mutex);
                m_Records.push_back(r);
            }

            // Main thread, between PhysicsSystem::Update calls
            std::vector<Record> Take() {
                std::lock_guard lock(m_Mutex);
                std::vector<Record> out;
                out.swap(m_Records);
                return out;
            }

        private:
            std::mutex m_Mutex;
            std::vector<Record> m_Records;
        };

        class RayFilter final : public JPH::BodyFilter {
        public:
            explicit RayFilter(JPH::BodyID ignore, bool staticOnly = false) : m_Ignore(ignore), m_StaticOnly(staticOnly) {}
            bool ShouldCollide(const JPH::BodyID& id) const override { return id != m_Ignore; }
            bool ShouldCollideLocked(const JPH::Body& body) const override {
                return !body.IsSensor() && (!m_StaticOnly || body.IsStatic());
            }
        private:
            JPH::BodyID m_Ignore;
            bool m_StaticOnly = false;
        };

        inline uint64_t PairKey(JPH::BodyID a, JPH::BodyID b) {
            uint32_t x = a.GetIndexAndSequenceNumber(), y = b.GetIndexAndSequenceNumber();
            if (x > y) std::swap(x, y);
            return (uint64_t(x) << 32) | y;
        }

    } // namespace

    struct Binding {
        JPH::BodyID Id;
        BodyType Type = BodyType::Dynamic;    // as simulated (triggers run as kinematic)
        bool Trigger = false;
        bool FollowsTransform = true;         // teleport the body when the transform changes from outside
        glm::vec3 PrevPos{ 0.0f }, CurPos{ 0.0f };      // the last two physics states, for interpolation
        glm::quat PrevRot{ 1, 0, 0, 0 }, CurRot{ 1, 0, 0, 0 };
        glm::vec3 WrittenPos{ 0.0f }, WrittenRot{ 0.0f };  // the transform as physics last left it
        glm::vec3 Force{ 0.0f };              // AddForce this frame, applied in each of its fixed steps
        glm::vec3 Impulse{ 0.0f };            // AddImpulse, applied once at the next fixed step
    };

    // A pair of bodies that touch, with how many sub-shape contacts they currently share
    struct Touch {
        entt::entity A = entt::null, B = entt::null;
        int Count = 0;
        bool Trigger = false;
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
        ContactRecorder Contacts;
        JPH::PhysicsSystem System;

        Scene* Sc = nullptr;                  // the scene from the last Step/Sync, for on-demand bodies
        entt::registry* Reg = nullptr;        // its registry
        std::unordered_map<entt::entity, Binding> Bindings;
        std::unordered_map<uint32_t, entt::entity> BodyToEntity;   // BodyID index+sequence -> entity
        std::unordered_map<uint64_t, Touch> Touching;
        std::vector<CollisionEvent> Queued;    // gathered since the last Step (contacts, removed bodies)
        std::vector<CollisionEvent> Events;    // what the last Step published
        float Accumulator = 0.0f;

        void RemoveAll() {
            auto& bi = System.GetBodyInterface();
            for (auto& [e, b] : Bindings) {
                bi.RemoveBody(b.Id);
                bi.DestroyBody(b.Id);
            }
            Bindings.clear();
            BodyToEntity.clear();
            Touching.clear();
            Queued.clear();
            Events.clear();
            Contacts.Take();
            ShapeCache.clear();          // geometry may have changed (or been freed) since the last Play
            Warned.clear();
            Accumulator = 0.0f;
        }

        void RemoveBinding(std::unordered_map<entt::entity, Binding>::iterator it) {
            const uint32_t id = it->second.Id.GetIndexAndSequenceNumber();
            // Close every contact this body had, so scripts see the matching end / exit
            for (auto t = Touching.begin(); t != Touching.end();) {
                if (uint32_t(t->first >> 32) == id || uint32_t(t->first) == id) {
                    Queued.push_back({ t->second.A, t->second.B, false, t->second.Trigger });
                    t = Touching.erase(t);
                }
                else {
                    ++t;
                }
            }
            auto& bi = System.GetBodyInterface();
            bi.RemoveBody(it->second.Id);
            bi.DestroyBody(it->second.Id);
            BodyToEntity.erase(id);
            Bindings.erase(it);
        }

        // ---- convex hull and mesh shapes, built from the entity's MeshRenderer geometry
        std::unordered_map<std::string, JPH::ShapeRefC> ShapeCache;   // same geometry + scale -> one shape
        std::unordered_set<entt::entity> Warned;

        void WarnOnce(entt::entity e, const char* what) {
            if (!Warned.insert(e).second) return;
            const auto* tag = Reg ? Reg->try_get<TagComponent>(e) : nullptr;
            ENGINE_WARN("Physics: '{}': {}", tag ? tag->Tag : std::string("?"), what);
        }

        JPH::ShapeRefC GeometryShape(entt::entity e, bool exactMesh, const glm::vec3& worldScale) {
            const auto* mr = Reg->try_get<MeshRendererComponent>(e);
            if (!mr || !mr->Geometry) { WarnOnce(e, "a convex hull or mesh collider needs a Mesh Renderer: using a box"); return nullptr; }

            // The shape lives in body space: the geometry through its LocalOffset, then the entity's scale
            std::string key((const char*)&mr->Geometry, sizeof(void*));
            key.append((const char*)&mr->LocalOffset, sizeof(glm::mat4));
            key.append((const char*)&worldScale, sizeof(glm::vec3));
            key.push_back(exactMesh ? 'm' : 'h');
            if (auto it = ShapeCache.find(key); it != ShapeCache.end()) return it->second;

            std::vector<glm::vec3> pos;
            std::vector<uint32_t> idx;
            mr->Geometry->GetCollisionGeometry(pos, idx);
            if (pos.empty()) { WarnOnce(e, "its geometry has no triangles for a collider: using a box"); return nullptr; }
            glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
            for (auto& p : pos) {
                p = glm::vec3(mr->LocalOffset * glm::vec4(p, 1.0f)) * worldScale;
                mn = glm::min(mn, p);
                mx = glm::max(mx, p);
            }

            JPH::ShapeSettings::ShapeResult result;
            if (exactMesh) {
                JPH::VertexList verts;
                verts.reserve(pos.size());
                for (const auto& p : pos) verts.push_back(JPH::Float3(p.x, p.y, p.z));
                JPH::IndexedTriangleList tris;
                tris.reserve(idx.size() / 3);
                for (size_t i = 0; i + 2 < idx.size(); i += 3) tris.push_back(JPH::IndexedTriangle(idx[i], idx[i + 1], idx[i + 2], 0));
                result = JPH::MeshShapeSettings(std::move(verts), std::move(tris)).Create();
            }
            else {
                // Merge points closer than 1/1000 of the size, and keep at most a few thousand:
                // the hull of a big model is just as good from a sample
                const glm::vec3 extent = mx - mn;
                const float cell = std::max({ extent.x, extent.y, extent.z }) / 1000.0f + 1e-6f;
                std::unordered_set<uint64_t> seen;
                JPH::Array<JPH::Vec3> points;
                for (const auto& p : pos) {
                    const glm::ivec3 c = glm::ivec3(glm::floor((p - mn) / cell));
                    const uint64_t k = (uint64_t(c.x & 0x1FFFFF) << 42) | (uint64_t(c.y & 0x1FFFFF) << 21) | uint64_t(c.z & 0x1FFFFF);
                    if (seen.insert(k).second) points.push_back(ToJolt(p));
                }
                constexpr size_t kMaxPoints = 4000;
                if (points.size() > kMaxPoints) {
                    JPH::Array<JPH::Vec3> sample;
                    const double stride = double(points.size()) / kMaxPoints;
                    for (size_t i = 0; i < kMaxPoints; i++) sample.push_back(points[(size_t)(i * stride)]);
                    points = std::move(sample);
                }
                const float radius = std::min(0.05f, 0.1f * std::min({ extent.x, extent.y, extent.z }));
                result = JPH::ConvexHullShapeSettings(points, std::max(radius, 0.0f)).Create();
            }
            if (result.HasError()) {
                ENGINE_WARN("Physics: can't build a {} for '{}' ({}): using a box", exactMesh ? "mesh collider" : "convex hull",
                    Reg->get<TagComponent>(e).Tag, result.GetError().c_str());
                return nullptr;
            }
            return ShapeCache[key] = result.Get();
        }

        // One collider shape in its entity's space: the scale baked in, Center applied. Never null
        // (anything that can't be built falls back to a box, with a warning).
        JPH::ShapeRefC BuildShape(entt::entity e, ColliderShape kind, const glm::vec3& size, const glm::vec3& center,
            const glm::vec3& worldScale, bool dynamic) {
            const glm::vec3 scale = glm::abs(worldScale);
            if (kind == ColliderShape::ConvexHull || kind == ColliderShape::Mesh) {
                // Jolt can't simulate a triangle mesh as a dynamic body: those get a convex hull
                bool exact = kind == ColliderShape::Mesh;
                if (exact && dynamic) {
                    WarnOnce(e, "a Mesh collider can't be dynamic: using a convex hull");
                    exact = false;
                }
                if (JPH::ShapeRefC s = GeometryShape(e, exact, worldScale)) return s;
            }

            JPH::ShapeRefC shape;
            if (kind == ColliderShape::Sphere) {
                const float r = std::max(0.01f, 0.5f * size.x * std::max({ scale.x, scale.y, scale.z }));
                shape = new JPH::SphereShape(r);
            }
            else if (kind == ColliderShape::Capsule) {
                // Along local Y: the radius from x/z, the height (caps included) from y
                const float r = std::max(0.01f, 0.5f * size.x * std::max(scale.x, scale.z));
                const float halfCylinder = std::max(0.0f, 0.5f * size.y * scale.y - r);
                if (halfCylinder > 1e-4f) shape = new JPH::CapsuleShape(halfCylinder, r);
                else                      shape = new JPH::SphereShape(r);   // too short: just the caps
            }
            else {
                const glm::vec3 half = glm::max(0.5f * size * scale, glm::vec3(0.01f));
                const float convexRadius = std::min(0.05f, std::min({ half.x, half.y, half.z }));
                shape = new JPH::BoxShape(ToJolt(half), convexRadius);
            }
            const glm::vec3 offset = center * worldScale;
            if (glm::dot(offset, offset) > 1e-10f)
                shape = new JPH::RotatedTranslatedShape(ToJolt(offset), JPH::Quat::sIdentity(), shape.GetPtr());
            return shape;
        }

        // Colliders that belong to body `e`: descendants with a ColliderComponent, not looking inside
        // descendants that have a RigidBody of their own (those are separate bodies)
        void CollectColliders(entt::entity e, std::vector<entt::entity>& out) {
            for (auto c : Sc->GetChildren(e)) {
                if (Reg->all_of<RigidBodyComponent>(c)) continue;
                if (Reg->all_of<ColliderComponent>(c)) out.push_back(c);
                CollectColliders(c, out);
            }
        }

        // Where the entity is in the world (transforms are local to their parent)
        void WorldPose(entt::entity e, glm::vec3& pos, glm::quat& rot, glm::vec3& scale) const {
            Scene::Decompose(Sc->GetWorldMatrix(e), pos, rot, scale);
        }

        Binding* CreateBody(entt::entity e, const TransformComponent& t, const RigidBodyComponent& rb, bool followsTransform) {
            glm::vec3 worldPos, worldScale;
            glm::quat q;
            WorldPose(e, worldPos, q, worldScale);

            // Triggers are kinematic sensors: a static sensor loses its contacts when the body
            // inside falls asleep (a false "exit"), an active kinematic one never does
            const BodyType type = rb.IsTrigger ? BodyType::Kinematic : rb.Type;

            // The body's own shape plus its descendants' colliders, as one compound if there are several
            const bool dynamic = type == BodyType::Dynamic;
            JPH::ShapeRefC own;
            if (rb.Shape != ColliderShape::None) own = BuildShape(e, rb.Shape, rb.Size, rb.Center, worldScale, dynamic);

            std::vector<entt::entity> parts;
            CollectColliders(e, parts);
            JPH::ShapeRefC shape;
            if (parts.empty()) {
                shape = own;
            }
            else {
                // Each part goes where its entity is, relative to the body (in the body's unscaled
                // space; the part's own world scale is baked into its shape)
                JPH::StaticCompoundShapeSettings compound;
                if (own) compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), own.GetPtr());
                const glm::quat inv = glm::inverse(q);
                for (auto p : parts) {
                    glm::vec3 pp, ps;
                    glm::quat pq;
                    WorldPose(p, pp, pq, ps);
                    const auto& c = Reg->get<ColliderComponent>(p);
                    if (c.Shape == ColliderShape::None) continue;
                    JPH::ShapeRefC part = BuildShape(p, c.Shape, c.Size, c.Center, ps, dynamic);
                    compound.AddShape(ToJolt(inv * (pp - worldPos)), ToJolt(glm::normalize(inv * pq)), part.GetPtr());
                }
                auto result = compound.Create();
                if (result.HasError()) WarnOnce(e, "its child colliders can't be combined: using its own shape only");
                else shape = result.Get();
                if (!shape) shape = own;
            }
            if (!shape) {
                WarnOnce(e, "a rigid body with shape None needs child colliders: using a small box");
                shape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.05f));
            }

            JPH::EMotionType motion = type == BodyType::Static ? JPH::EMotionType::Static
                : type == BodyType::Kinematic ? JPH::EMotionType::Kinematic
                : JPH::EMotionType::Dynamic;
            JPH::ObjectLayer layer = type == BodyType::Static ? Layers::Static : Layers::Moving;

            JPH::BodyCreationSettings bs(shape, ToJolt(worldPos), ToJolt(q), motion, layer);
            bs.mFriction = rb.Friction;
            bs.mRestitution = rb.Restitution;
            bs.mIsSensor = rb.IsTrigger;
            if (motion == JPH::EMotionType::Dynamic) {
                bs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                bs.mMassPropertiesOverride.mMass = std::max(rb.Mass, 0.001f);
            }

            auto& bi = System.GetBodyInterface();
            JPH::BodyID id = bi.CreateAndAddBody(bs, motion == JPH::EMotionType::Static
                ? JPH::EActivation::DontActivate
                : JPH::EActivation::Activate);
            if (id.IsInvalid()) { ENGINE_ERROR("Physics: out of bodies"); return nullptr; }

            if (motion == JPH::EMotionType::Dynamic &&
                glm::dot(rb.InitialVelocity, rb.InitialVelocity) > 0.0f)
                bi.SetLinearVelocity(id, ToJolt(rb.InitialVelocity));

            Binding b;
            b.Id = id;
            b.Type = type;
            b.Trigger = rb.IsTrigger;
            b.FollowsTransform = followsTransform;
            b.PrevPos = b.CurPos = worldPos;              // physics states are world space...
            b.WrittenPos = t.Position;                    // ...the transform as last seen is local
            b.PrevRot = b.CurRot = q;
            b.WrittenRot = t.Rotation;
            BodyToEntity[id.GetIndexAndSequenceNumber()] = e;
            return &(Bindings[e] = b);
        }

        // The binding for an entity, creating its body now if it has a RigidBody but no body yet
        Binding* Find(entt::entity e) {
            if (auto it = Bindings.find(e); it != Bindings.end()) return &it->second;
            if (!Reg || !Reg->valid(e)) return nullptr;
            auto* rb = Reg->try_get<RigidBodyComponent>(e);
            auto* t = Reg->try_get<TransformComponent>(e);
            if (!rb || !t) return nullptr;
            return CreateBody(e, *t, *rb, FollowsTransform(e));
        }

        // Spin and Orbit write the transform every frame. On a dynamic body physics wins (as the
        // Inspector says), so those transform writes must not count as teleports.
        bool FollowsTransform(entt::entity e) const {
            const auto* rb = Reg->try_get<RigidBodyComponent>(e);
            const bool dynamic = rb && rb->Type == BodyType::Dynamic && !rb->IsTrigger;
            return !(dynamic && Reg->any_of<SpinComponent, OrbitComponent>(e));
        }

        // Keep the physics world in step with the scene: new entities get bodies, removed ones lose them
        void Sync(Scene& scene) {
            Sc = &scene;
            entt::registry& reg = scene.Registry();
            Reg = &reg;
            for (auto it = Bindings.begin(); it != Bindings.end();) {
                auto next = std::next(it);
                if (!reg.valid(it->first) || !reg.all_of<RigidBodyComponent>(it->first)) RemoveBinding(it);
                it = next;
            }
            reg.view<TransformComponent, RigidBodyComponent>().each(
                [&](entt::entity e, TransformComponent& t, RigidBodyComponent& rb) {
                    if (!Bindings.count(e)) CreateBody(e, t, rb, FollowsTransform(e));
                });
        }

        // Turn what the listener recorded during one System.Update into begin / end events
        void GatherContacts() {
            for (const auto& r : Contacts.Take()) {
                const uint64_t key = PairKey(r.A, r.B);
                if (r.Added) {
                    auto ea = BodyToEntity.find(r.A.GetIndexAndSequenceNumber());
                    auto eb = BodyToEntity.find(r.B.GetIndexAndSequenceNumber());
                    if (ea == BodyToEntity.end() || eb == BodyToEntity.end()) continue;
                    Touch& t = Touching[key];
                    if (t.Count++ == 0) {
                        t.A = ea->second;
                        t.B = eb->second;
                        t.Trigger = Bindings[t.A].Trigger || Bindings[t.B].Trigger;
                        Queued.push_back({ t.A, t.B, true, t.Trigger, r.Point, r.Normal });
                    }
                }
                else {
                    auto it = Touching.find(key);
                    if (it == Touching.end()) continue;       // already closed (a body was removed)
                    if (--it->second.Count <= 0) {
                        Queued.push_back({ it->second.A, it->second.B, false, it->second.Trigger });
                        Touching.erase(it);
                    }
                }
            }
        }
    };

    PhysicsWorld::PhysicsWorld() {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();

        m_Impl = std::make_unique<Impl>();
        m_Impl->System.Init(4096, 0, 4096, 4096, m_Impl->BPInterface, m_Impl->ObjVsBP, m_Impl->ObjPair);
        m_Impl->System.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
        m_Impl->System.SetContactListener(&m_Impl->Contacts);
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

    const std::vector<CollisionEvent>& PhysicsWorld::Events() const { return m_Impl->Events; }

    void PhysicsWorld::Sync(Scene& scene) { m_Impl->Sync(scene); }

    void PhysicsWorld::Step(Scene& scene, float dt, bool snapToLatest) {
        Impl& I = *m_Impl;
        auto& reg = scene.Registry();
        auto& bi = I.System.GetBodyInterface();

        I.Sync(scene);

        I.Accumulator += std::min(dt, 0.25f);
        int steps = (int)(I.Accumulator / kFixedStep);
        if (steps > kMaxSteps) { steps = kMaxSteps; I.Accumulator = 0.0f; }
        else                   I.Accumulator -= steps * kFixedStep;

        for (auto& [e, b] : I.Bindings) {
            // Someone moved a static or dynamic body's transform (a script, the gizmo, a moving
            // parent for a static one): teleport it. Physics itself writes dynamic transforms, so
            // for those only a change to the local transform since that write counts.
            if (b.Type == BodyType::Kinematic || !b.FollowsTransform) continue;
            const auto& t = reg.get<TransformComponent>(e);
            glm::vec3 pos, scale;
            glm::quat q;
            bool moved = t.Position != b.WrittenPos || t.Rotation != b.WrittenRot;
            // A static body without a parent moves only with its own transform (checked just above); one with a
            // parent can be carried by it, so its world pose is checked every frame (a forest of roots isn't)
            if (moved || (b.Type == BodyType::Static && reg.all_of<ParentComponent>(e))) {
                I.WorldPose(e, pos, q, scale);
                if (b.Type == BodyType::Static) moved = moved || pos != b.CurPos || q != b.CurRot;
            }
            if (moved) {
                bi.SetPositionAndRotation(b.Id, ToJolt(pos), ToJolt(q),
                    b.Type == BodyType::Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
                b.PrevPos = b.CurPos = pos;
                b.PrevRot = b.CurRot = q;
                b.WrittenPos = t.Position;
                b.WrittenRot = t.Rotation;
            }
        }

        if (steps > 0) {
            // Kinematic bodies follow their transform: give them the velocity that reaches it
            // exactly when this batch of steps ends, so they push dynamic bodies properly
            const float span = steps * kFixedStep;
            for (auto& [e, b] : I.Bindings) {
                if (b.Type == BodyType::Kinematic) {       // follows its world placement (parents included)
                    glm::vec3 pos, scale;
                    glm::quat q;
                    I.WorldPose(e, pos, q, scale);
                    bi.MoveKinematic(b.Id, ToJolt(pos), ToJolt(q), span);
                }
                else if (b.Type == BodyType::Dynamic && glm::dot(b.Impulse, b.Impulse) > 0.0f) {
                    bi.AddImpulse(b.Id, ToJolt(b.Impulse));
                }
                b.Impulse = glm::vec3(0.0f);
            }

            for (int i = 0; i < steps; i++) {
                for (auto& [e, b] : I.Bindings) {
                    if (b.Type != BodyType::Dynamic) continue;
                    b.PrevPos = b.CurPos;
                    b.PrevRot = b.CurRot;
                    // This frame's force acts during every fixed step the frame runs, like gravity.
                    // (Jolt clears forces after each step.) A frame that runs no step applies none:
                    // its time is simulated next frame, under next frame's force.
                    if (glm::dot(b.Force, b.Force) > 0.0f) bi.AddForce(b.Id, ToJolt(b.Force));
                }

                I.System.Update(kFixedStep, 1, &I.Temp, &I.Jobs);
                I.GatherContacts();

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
            const glm::vec3 pos = glm::mix(b.PrevPos, b.CurPos, alpha);
            const glm::quat rot = glm::slerp(b.PrevRot, b.CurRot, alpha);
            if (reg.all_of<ParentComponent>(e)) {          // physics is world space; the transform is local
                scene.SetWorldPosition(e, pos);
                scene.SetWorldRotation(e, rot);
            }
            else {
                t.Position = pos;
                t.Rotation = Scene::EulerFromQuat(rot);
            }
            b.WrittenPos = t.Position;
            b.WrittenRot = t.Rotation;
        }
        for (auto& [e, b] : I.Bindings) b.Force = glm::vec3(0.0f);   // add_force lasts one frame

        // Publish everything since the last Step (including bodies removed between steps)
        I.Events.swap(I.Queued);
        I.Queued.clear();
    }

    // ---------------------------------------------------------------- per-entity access
    bool PhysicsWorld::HasBody(entt::entity e) const { return m_Impl->Bindings.count(e) != 0; }

    void PhysicsWorld::Invalidate(entt::entity e) {
        if (auto it = m_Impl->Bindings.find(e); it != m_Impl->Bindings.end()) m_Impl->RemoveBinding(it);
    }

    glm::vec3 PhysicsWorld::GetLinearVelocity(entt::entity e) {
        Binding* b = m_Impl->Find(e);
        return b ? ToGlm(m_Impl->System.GetBodyInterface().GetLinearVelocity(b->Id)) : glm::vec3(0.0f);
    }

    void PhysicsWorld::SetLinearVelocity(entt::entity e, const glm::vec3& v) {
        Binding* b = m_Impl->Find(e);
        if (!b || b->Type != BodyType::Dynamic) return;   // kinematic velocity comes from its transform
        auto& bi = m_Impl->System.GetBodyInterface();
        bi.SetLinearVelocity(b->Id, ToJolt(v));
        bi.ActivateBody(b->Id);
    }

    glm::vec3 PhysicsWorld::GetAngularVelocity(entt::entity e) {
        Binding* b = m_Impl->Find(e);
        return b ? ToGlm(m_Impl->System.GetBodyInterface().GetAngularVelocity(b->Id)) : glm::vec3(0.0f);
    }

    void PhysicsWorld::SetAngularVelocity(entt::entity e, const glm::vec3& w) {
        Binding* b = m_Impl->Find(e);
        if (!b || b->Type != BodyType::Dynamic) return;
        auto& bi = m_Impl->System.GetBodyInterface();
        bi.SetAngularVelocity(b->Id, ToJolt(w));
        bi.ActivateBody(b->Id);
    }

    void PhysicsWorld::AddForce(entt::entity e, const glm::vec3& force) {
        if (Binding* b = m_Impl->Find(e); b && b->Type == BodyType::Dynamic) b->Force += force;
    }

    void PhysicsWorld::AddImpulse(entt::entity e, const glm::vec3& impulse) {
        if (Binding* b = m_Impl->Find(e); b && b->Type == BodyType::Dynamic) b->Impulse += impulse;
    }

    bool PhysicsWorld::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
        RayHit& hit, entt::entity ignore, bool staticOnly) const {
        const float len = glm::length(direction);
        if (len < 1e-6f || maxDistance <= 0.0f) return false;
        const glm::vec3 dir = direction / len * maxDistance;

        JPH::BodyID ignoreId;
        if (auto it = m_Impl->Bindings.find(ignore); it != m_Impl->Bindings.end()) ignoreId = it->second.Id;

        JPH::RRayCast ray{ JPH::RVec3(origin.x, origin.y, origin.z), ToJolt(dir) };
        JPH::RayCastResult result;
        RayFilter filter(ignoreId, staticOnly);
        if (!m_Impl->System.GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, filter)) return false;

        auto ent = m_Impl->BodyToEntity.find(result.mBodyID.GetIndexAndSequenceNumber());
        if (ent == m_Impl->BodyToEntity.end()) return false;

        const JPH::RVec3 p = ray.GetPointOnRay(result.mFraction);
        hit.Entity = ent->second;
        hit.Point = ToGlm(JPH::Vec3(p));
        hit.Distance = result.mFraction * maxDistance;
        hit.Normal = -dir / maxDistance;
        JPH::BodyLockRead lock(m_Impl->System.GetBodyLockInterface(), result.mBodyID);
        if (lock.Succeeded())
            hit.Normal = ToGlm(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, p));
        return true;
    }

    void PhysicsWorld::RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
        std::vector<RayHit>& hits, bool staticOnly) const {
        hits.clear();
        const float len = glm::length(direction);
        if (len < 1e-6f || maxDistance <= 0.0f) return;
        const glm::vec3 dir = direction / len * maxDistance;

        JPH::RRayCast ray{ JPH::RVec3(origin.x, origin.y, origin.z), ToJolt(dir) };
        JPH::RayCastSettings settings;                 // back faces ignored, convex shapes solid
        JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
        RayFilter filter(JPH::BodyID(), staticOnly);
        m_Impl->System.GetNarrowPhaseQuery().CastRay(ray, settings, collector, {}, {}, filter);
        collector.Sort();

        for (const JPH::RayCastResult& r : collector.mHits) {
            auto ent = m_Impl->BodyToEntity.find(r.mBodyID.GetIndexAndSequenceNumber());
            if (ent == m_Impl->BodyToEntity.end()) continue;
            const JPH::RVec3 p = ray.GetPointOnRay(r.mFraction);
            RayHit h;
            h.Entity = ent->second;
            h.Point = ToGlm(JPH::Vec3(p));
            h.Distance = r.mFraction * maxDistance;
            h.Normal = -dir / maxDistance;
            JPH::BodyLockRead lock(m_Impl->System.GetBodyLockInterface(), r.mBodyID);
            if (lock.Succeeded())
                h.Normal = ToGlm(lock.GetBody().GetWorldSpaceSurfaceNormal(r.mSubShapeID2, p));
            hits.push_back(h);
        }
    }

    bool PhysicsWorld::StaticBounds(glm::vec3& mn, glm::vec3& mx) const {
        bool any = false;
        for (const auto& [e, b] : m_Impl->Bindings) {
            if (b.Type != BodyType::Static || b.Trigger) continue;
            JPH::BodyLockRead lock(m_Impl->System.GetBodyLockInterface(), b.Id);
            if (!lock.Succeeded()) continue;
            const JPH::AABox box = lock.GetBody().GetWorldSpaceBounds();
            const glm::vec3 lo = ToGlm(box.mMin), hi = ToGlm(box.mMax);
            mn = any ? glm::min(mn, lo) : lo;
            mx = any ? glm::max(mx, hi) : hi;
            any = true;
        }
        return any;
    }

} // namespace Engine
