#include "Scene/Scene.h"
#include "Core/Profiler.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Renderer/Camera.h"
#include "Renderer/Shader.h"
#include "Renderer/Shadowmap.h"
#include "Renderer/Material.h"
#include "Renderer/MaterialAsset.h"
#include "Renderer/ShaderGraph.h"
#include "Renderer/Environment.h"
#include "Renderer/Model.h"
#include "Renderer/Mesh.h"
#include "Renderer/DrawList.h"
#include "Renderer/RenderStats.h"
#include "Scene/Terrain.h"
#include "Animation/AnimGraph.h"
#include "Core/Log.h"
#include <glad/glad.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <tuple>
#include <random>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_access.hpp>
#include <vector>

namespace Engine {

    Scene::Scene() {
        // Keep FindByID's map honest however IDs come and go (create, destroy, clear, replace)
        m_Registry.on_construct<IDComponent>().connect<&Scene::OnIDChanged>(this);
        m_Registry.on_update<IDComponent>().connect<&Scene::OnIDChanged>(this);
        m_Registry.on_destroy<IDComponent>().connect<&Scene::OnIDChanged>(this);
        // A merged static object going away (destroyed, or its Mesh Renderer removed) rebuilds the batches
        m_Registry.on_destroy<MeshRendererComponent>().connect<&Scene::OnMeshRendererGone>(this);
    }

    Entity Scene::CreateEntity(const std::string& name) {
        Entity entity(m_Registry.create(), this);
        entity.Add<TransformComponent>();
        entity.Add<TagComponent>().Tag = name;
        entity.Add<IDComponent>().ID = NewID();
        return entity;
    }

    void Scene::DestroyEntity(Entity entity) {
        if (!entity) return;
        for (auto child : GetChildren(entity.GetHandle())) DestroyEntity(Entity(child, this));
        m_Registry.destroy(entity.GetHandle());
    }

    // ---------------------------------------------------------------- hierarchy
    uint64_t Scene::NewID() {
        static std::mt19937_64 rng{ std::random_device{}() };
        uint64_t id = 0;
        while (id == 0) id = rng();            // 0 means "no ID"
        return id;
    }

    uint64_t Scene::GetID(entt::entity e) const {
        const auto* c = m_Registry.try_get<IDComponent>(e);
        return c ? c->ID : 0;
    }

    entt::entity Scene::FindByID(uint64_t id) {
        if (id == 0) return entt::null;
        if (m_IDMapDirty) {
            m_IDMap.clear();
            for (auto [e, c] : m_Registry.view<IDComponent>().each()) m_IDMap[c.ID] = e;
            m_IDMapDirty = false;
        }
        auto it = m_IDMap.find(id);
        return it != m_IDMap.end() ? it->second : entt::null;
    }

    entt::entity Scene::GetParent(entt::entity e) {
        const auto* p = m_Registry.try_get<ParentComponent>(e);
        if (!p) return entt::null;
        const entt::entity parent = FindByID(p->Parent);
        return parent == e ? entt::null : parent;
    }

    std::vector<entt::entity> Scene::GetChildren(entt::entity e) {
        std::vector<entt::entity> out;
        const uint64_t id = GetID(e);
        if (id == 0) return out;
        for (auto [c, p] : m_Registry.view<ParentComponent>().each())
            if (p.Parent == id && c != e) out.push_back(c);
        return out;
    }

    bool Scene::IsDescendantOf(entt::entity e, entt::entity ancestor) {
        int guard = 0;
        for (entt::entity p = GetParent(e); p != entt::null && guard < 256; p = GetParent(p), guard++)
            if (p == ancestor) return true;
        return false;
    }

    bool Scene::SetParent(entt::entity child, entt::entity parent, bool keepWorld) {
        if (!m_Registry.valid(child)) return false;
        if (parent != entt::null && (parent == child || !m_Registry.valid(parent) || IsDescendantOf(parent, child)))
            return false;                      // would make a loop
        const glm::mat4 world = GetWorldMatrix(child);
        if (parent == entt::null) m_Registry.remove<ParentComponent>(child);
        else m_Registry.emplace_or_replace<ParentComponent>(child, ParentComponent{ GetID(parent) });
        if (keepWorld) SetWorldMatrix(child, world);
        return true;
    }

    glm::mat4 Scene::GetWorldMatrix(entt::entity e) {
        glm::mat4 m = m_Registry.get<TransformComponent>(e).GetMatrix();
        int guard = 0;                         // a corrupt file could hold a loop: don't hang on it
        for (entt::entity p = GetParent(e); p != entt::null && guard < 256; p = GetParent(p), guard++)
            m = m_Registry.get<TransformComponent>(p).GetMatrix() * m;
        return m;
    }

    glm::mat4 Scene::GetParentWorldMatrix(entt::entity e) {
        const entt::entity p = GetParent(e);
        return p == entt::null ? glm::mat4(1.0f) : GetWorldMatrix(p);
    }

    void Scene::Decompose(const glm::mat4& m, glm::vec3& position, glm::quat& rotation, glm::vec3& scale) {
        position = glm::vec3(m[3]);
        scale = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
        if (glm::determinant(glm::mat3(m)) < 0.0f) scale.x = -scale.x;   // a mirror: put it in x
        glm::vec3 s = scale;
        for (int i = 0; i < 3; i++)                                      // avoid dividing by a zero scale
            if (std::abs(s[i]) < 1e-8f) s[i] = 1e-8f;
        rotation = glm::normalize(glm::quat_cast(glm::mat3(glm::vec3(m[0]) / s.x, glm::vec3(m[1]) / s.y, glm::vec3(m[2]) / s.z)));
    }

    // TransformComponent builds its rotation with glm::quat(vec3(x, y, z)), which is R = Rz(z) * Ry(y) * Rx(x):
    //   R = [ cy*cz   cz*sy*sx - sz*cx   cz*sy*cx + sz*sx ]
    //       [ cy*sz   sz*sy*sx + cz*cx   sz*sy*cx - cz*sx ]
    //       [ -sy     cy*sx              cy*cx            ]
    // glm::eulerAngles reads x and z from entries that all shrink with cos(y), so next to gimbal lock (y near
    // +-90 degrees) float rounding makes them wrong by degrees, differently every frame (a camera aimed with
    // look_at shook). Here z comes first (0 at the lock), and x and y are solved for that z from entries that
    // don't shrink, so the angles always rebuild the same rotation (Day, "Euler angles from a rotation matrix").
    glm::vec3 Scene::EulerFromQuat(const glm::quat& q) {
        const glm::mat3 m = glm::mat3_cast(glm::normalize(q));   // m[column][row]
        auto R = [&](int row, int col) { return m[col][row]; };
        const float cy = std::sqrt(R(0, 0) * R(0, 0) + R(1, 0) * R(1, 0));
        const float z = cy > 1e-4f ? std::atan2(R(1, 0), R(0, 0)) : 0.0f;
        const float sz = std::sin(z), cz = std::cos(z);
        const float x = std::atan2(sz * R(0, 2) - cz * R(1, 2), cz * R(1, 1) - sz * R(0, 1));
        const float y = std::atan2(-R(2, 0), cz * R(0, 0) + sz * R(1, 0));
        return glm::vec3(x, y, z);
    }

    void Scene::SetWorldMatrix(entt::entity e, const glm::mat4& world) {
        const glm::mat4 local = glm::inverse(GetParentWorldMatrix(e)) * world;
        glm::vec3 pos, scale;
        glm::quat rot;
        Decompose(local, pos, rot, scale);
        auto& t = m_Registry.get<TransformComponent>(e);
        t.Position = pos;
        t.Rotation = EulerFromQuat(rot);
        t.Scale = scale;
    }

    void Scene::SetWorldPosition(entt::entity e, const glm::vec3& position) {
        m_Registry.get<TransformComponent>(e).Position =
            glm::vec3(glm::inverse(GetParentWorldMatrix(e)) * glm::vec4(position, 1.0f));
    }

    void Scene::SetWorldRotation(entt::entity e, const glm::quat& rotation) {
        glm::vec3 pp, ps;
        glm::quat parentRot;
        Decompose(GetParentWorldMatrix(e), pp, parentRot, ps);
        m_Registry.get<TransformComponent>(e).Rotation = EulerFromQuat(glm::inverse(parentRot) * rotation);
    }

    void Scene::Clear() {
        m_Registry.clear();
        ClearStaticBatches();        // merged meshes belong to the entities that are gone
        Decals.clear();
        Fog = FogOfWarState{};
        FoliagePushes.clear();
    }

    // ---------------------------------------------------------------- update systems
    void Scene::UpdateOrbitPositions() {
        m_Registry.view<TransformComponent, OrbitComponent>().each(
            [](TransformComponent& t, const OrbitComponent& o) {
                t.Position = o.Center + glm::vec3(std::cos(o.Angle) * o.Radius,
                    o.Height,
                    std::sin(o.Angle) * o.Radius);
            });
    }

    void Scene::OnUpdate(float dt) {
        m_Registry.view<TransformComponent, SpinComponent>().each(
            [dt](TransformComponent& t, const SpinComponent& spin) {
                t.Rotation += spin.Speed * dt;
            });

        m_Registry.view<OrbitComponent>().each(
            [dt](OrbitComponent& o) { o.Angle += o.Speed * dt; });
        UpdateOrbitPositions();
        UpdateAnimations(dt);
        UpdateParticles(dt);
        m_Registry.view<MeshRendererComponent>().each([dt](MeshRendererComponent& mr) {   // hit flashes fade out
            if (mr.FlashTime > 0.0f) mr.FlashTime = std::max(0.0f, mr.FlashTime - dt);
        });
    }

    // ---------------------------------------------------------------- skeletal animation
    // ---------------------------------------------------------------- inverse kinematics
    namespace {
        // The rotation part of a node matrix (columns normalised, so node scale doesn't leak in)
        glm::quat RotationOf(const glm::mat4& m) {
            const glm::mat3 r(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2])));
            return glm::normalize(glm::quat_cast(r));
        }

        // The shortest rotation taking direction a to direction b
        glm::quat RotationBetween(glm::vec3 a, glm::vec3 b) {
            a = glm::normalize(a); b = glm::normalize(b);
            const float d = glm::dot(a, b);
            if (d > 0.99999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            if (d < -0.99999f) {
                glm::vec3 axis = glm::cross(glm::vec3(1, 0, 0), a);
                if (glm::length(axis) < 1e-4f) axis = glm::cross(glm::vec3(0, 1, 0), a);
                return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
            }
            return glm::angleAxis(std::acos(d), glm::normalize(glm::cross(a, b)));
        }

        // Turn node i by a model-space rotation q about its own origin: G' = q * G, so its local
        // rotation becomes P^-1 * q * P * L (P = the parent's model-space rotation)
        void RotateNode(const Model& model, const std::vector<glm::mat4>& globals, std::vector<Model::NodePose>& pose,
            int i, const glm::quat& q) {
            const int p = model.NodeParent(i);
            const glm::quat P = p >= 0 ? RotationOf(globals[p]) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            pose[i].R = glm::normalize(glm::inverse(P) * q * P * pose[i].R);
        }

        float SafeAcos(float x) { return std::acos(std::clamp(x, -1.0f, 1.0f)); }
    }

    static void SolveLimb(const Model& model, AnimatorComponent& an, std::vector<Model::NodePose>& pose,
        int end, const glm::vec3& target, const glm::vec3& pole, bool hasPole, float w);

    // Solve the Animator's IK goals on `pose` (all in model space). targets[i] is goal i's target in
    // model space; modelForward is the entity's +Z in model space. an.Globals is kept up to date.
    static void SolveIK(const Model& model, AnimatorComponent& an, std::vector<Model::NodePose>& pose,
        const std::vector<glm::vec3>& targets, const glm::vec3& modelForward, const glm::mat3& entityToModel,
        const std::vector<glm::mat4>& restGlobals) {
        const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
        for (size_t g = 0; g < an.IK.size(); g++) {
            const AnimIK& ik = an.IK[g];
            const float w = std::clamp(ik.Weight, 0.0f, 1.0f);
            const int end = model.FindNode(ik.Bone);
            if (w <= 0.0f || end < 0) continue;
            const glm::vec3 target = targets[g];

            if (ik.Type == IKType::Look) {
                // The head's own "forward": at rest it looks where the entity faces
                const glm::vec3 localForward = glm::inverse(RotationOf(restGlobals[end])) * modelForward;
                std::vector<int> chain;                          // head first, then its parents
                for (int n = end; n >= 0 && (int)chain.size() < std::max(ik.Chain, 1); n = model.NodeParent(n)) chain.push_back(n);
                const float maxAngle = glm::radians(std::clamp(ik.MaxAngle, 0.0f, 180.0f));

                for (int k = (int)chain.size() - 1; k >= 0; k--) {    // top of the chain first
                    const glm::vec3 headPos = glm::vec3(an.Globals[end][3]);
                    glm::vec3 want = target - headPos;
                    if (glm::length(want) < 1e-5f) break;
                    want = glm::normalize(want);
                    // Never further than MaxAngle from the way the body faces
                    const float off = SafeAcos(glm::dot(modelForward, want));
                    if (off > maxAngle) {
                        const glm::vec3 axis = glm::cross(modelForward, want);
                        want = glm::length(axis) > 1e-5f ? glm::angleAxis(maxAngle, glm::normalize(axis)) * modelForward : modelForward;
                    }
                    const glm::vec3 now = RotationOf(an.Globals[end]) * localForward;
                    // Each bone takes its share of what is left: 1/k+1 ... the head takes the rest
                    const glm::quat turn = glm::slerp(identity, RotationBetween(now, want), w / float(k + 1));
                    RotateNode(model, an.Globals, pose, chain[k], turn);
                    model.PoseToGlobals(pose, an.Globals);
                }
                continue;
            }

            SolveLimb(model, an, pose, end, target, entityToModel * ik.Pole, glm::length(ik.Pole) > 1e-5f, w);
        }
    }

    // Limb: two-bone IK (a = root of the limb, b = the middle joint, c = the end), in three exact steps
    // with the positions refreshed between them: bend b to the right length, swing a to point at the
    // target, then twist around that line so b bends towards the pole (model space). an.Globals is kept up to date.
    static void SolveLimb(const Model& model, AnimatorComponent& an, std::vector<Model::NodePose>& pose,
        int end, const glm::vec3& target, const glm::vec3& pole, bool hasPole, float w) {
        {
            const int b = model.NodeParent(end);
            const int a = b >= 0 ? model.NodeParent(b) : -1;
            if (a < 0) return;
            auto pos = [&](int n) { return glm::vec3(an.Globals[n][3]); };
            const float lab = glm::length(pos(b) - pos(a)), lcb = glm::length(pos(end) - pos(b));
            if (lab < 1e-6f || lcb < 1e-6f) return;
            const glm::quat oldA = pose[a].R, oldB = pose[b].R;

            // 1) Bend: the angle at b that makes |c - a| the distance to the target (clamped to reach)
            const float reach = std::clamp(glm::length(target - pos(a)), std::abs(lab - lcb) + 1e-4f, (lab + lcb) * 0.9999f);
            const glm::vec3 u = glm::normalize(pos(a) - pos(b)), v = glm::normalize(pos(end) - pos(b));
            const float bendNow = SafeAcos(glm::dot(u, v));
            const float bendWant = SafeAcos((lab * lab + lcb * lcb - reach * reach) / (2.0f * lab * lcb));
            glm::vec3 normal = glm::cross(u, v);
            if (glm::length(normal) < 1e-6f) {                   // a straight limb: bend towards the pole, or any way
                normal = glm::cross(u, hasPole ? pole : glm::vec3(0, 1, 0));
                if (glm::length(normal) < 1e-6f) normal = glm::cross(u, glm::vec3(1, 0, 0));
            }
            RotateNode(model, an.Globals, pose, b, glm::angleAxis(bendWant - bendNow, glm::normalize(normal)));
            model.PoseToGlobals(pose, an.Globals);

            // 2) Swing: turn a so the end lies on the line to the target
            RotateNode(model, an.Globals, pose, a, RotationBetween(pos(end) - pos(a), target - pos(a)));
            model.PoseToGlobals(pose, an.Globals);

            // 3) Pole: twist the limb around that line so the middle joint points at the pole
            if (hasPole && glm::length(target - pos(a)) > 1e-5f) {
                const glm::vec3 axis = glm::normalize(target - pos(a));
                glm::vec3 have = pos(b) - pos(a), want = pole;
                have -= glm::dot(have, axis) * axis;
                want -= glm::dot(want, axis) * axis;
                if (glm::length(have) > 1e-6f && glm::length(want) > 1e-6f) {
                    have = glm::normalize(have); want = glm::normalize(want);
                    const float angle = std::atan2(glm::dot(glm::cross(have, want), axis), glm::dot(have, want));
                    RotateNode(model, an.Globals, pose, a, glm::angleAxis(angle, axis));
                }
            }

            // Weight: blend from the animated pose
            if (w < 1.0f) {
                pose[a].R = glm::normalize(glm::slerp(oldA, pose[a].R, w));
                pose[b].R = glm::normalize(glm::slerp(oldB, pose[b].R, w));
            }
            model.PoseToGlobals(pose, an.Globals);
        }
    }

    // Foot placement (FootPlacement in Components.h) on the final pose, before the IK goals. modelToWorld = the
    // entity's world matrix * LocalOffset. Rays go along world down from RayUp above the entity's origin.
    static void PlaceFeet(const Model& model, AnimatorComponent& an, std::vector<Model::NodePose>& pose,
        const glm::mat4& modelToWorld, const glm::mat4& entityWorld, entt::entity self, float dt,
        const std::function<bool(const glm::vec3&, const glm::vec3&, float, entt::entity, glm::vec3&, glm::vec3&)>& ray,
        int grounded) {
        FootPlacement& fp = an.Feet;
        // How far a smoothed value goes towards its target this frame (dt 0: all the way)
        const float follow = (fp.Smoothing <= 0.0f || dt <= 0.0f) ? 1.0f : 1.0f - std::exp(-fp.Smoothing * dt);
        // In the air (a character controller): fade out over about 0.15 s, back in on landing
        const float fadeTarget = grounded == 0 ? 0.0f : 1.0f;
        fp.Fade = dt <= 0.0f ? fadeTarget
            : (fadeTarget > fp.Fade ? std::min(fadeTarget, fp.Fade + dt / 0.15f) : std::max(fadeTarget, fp.Fade - dt / 0.15f));
        const float w = std::clamp(fp.Weight, 0.0f, 1.0f) * fp.Fade;

        const glm::mat4 worldToModel = glm::inverse(modelToWorld);
        const glm::mat3 dirToModel(worldToModel);
        const glm::mat3 normalToModel = glm::transpose(glm::mat3(modelToWorld));
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        const float originY = entityWorld[3].y;
        const float rayUp = std::max(fp.RayUp, 0.0f), rayDown = std::max(fp.RayDown, 0.0f);
        const float maxTilt = glm::radians(std::clamp(fp.MaxSlope, 0.0f, 89.0f));

        // 1) Probe the ground under each foot (world): the smoothed height from the origin, and the slope
        struct Leg { int End = -1; glm::vec3 Foot{ 0.0f }; glm::quat Rot{ 1.0f, 0.0f, 0.0f, 0.0f }; };
        static thread_local std::vector<Leg> legs;
        legs.assign(fp.Feet.size(), Leg{});
        float lowest = 0.0f;
        bool any = false;
        for (size_t i = 0; i < fp.Feet.size(); i++) {
            FootIKLeg& leg = fp.Feet[i];
            const int end = model.FindNode(leg.Bone);
            if (end < 0 || end >= (int)an.Globals.size() || model.NodeParent(end) < 0 || model.NodeParent(model.NodeParent(end)) < 0) continue;
            legs[i].End = end;
            legs[i].Foot = glm::vec3(modelToWorld * an.Globals[end][3]);
            legs[i].Rot = RotationOf(an.Globals[end]);

            float lift = 0.0f;
            glm::vec3 normal = up;
            glm::vec3 point, n;
            const glm::vec3 from(legs[i].Foot.x, originY + rayUp, legs[i].Foot.z);
            const bool hit = ray && ray(from, -up, rayUp + rayDown, self, point, n);
            if (hit) {
                lift = std::clamp(point.y - originY, -rayDown, rayUp);
                normal = glm::length(n) > 1e-5f ? glm::normalize(n) : up;
                if (normal.y < 0.0f) normal = up;
            }
            leg.Ground = hit ? 1.0f : 0.0f;
            leg.Lift += (lift - leg.Lift) * follow;
            leg.Normal = glm::normalize(glm::mix(leg.Normal, normal, follow));
            lowest = any ? std::min(lowest, leg.Lift) : leg.Lift;
            any = true;
        }
        // The pelvis goes down by the lowest foot's drop, so that leg can still reach
        const float drop = any ? std::clamp(lowest, -std::max(fp.MaxPelvisDrop, 0.0f), 0.0f) : 0.0f;
        fp.PelvisDrop += (drop - fp.PelvisDrop) * follow;
        if (w <= 0.0f || !any) return;

        // 2) Lower the pelvis (its translation, in its parent's space)
        // "" = the nearest common ancestor of the bones the legs hang from (a quadruped: the hip, not the spine)
        int pelvis = fp.Pelvis.empty() ? -1 : model.FindNode(fp.Pelvis);
        if (fp.Pelvis.empty()) {
            for (const Leg& l : legs) {
                if (l.End < 0) continue;
                const int hangsFrom = model.NodeParent(model.NodeParent(model.NodeParent(l.End)));
                if (pelvis < 0) { pelvis = hangsFrom; continue; }
                auto isAncestorOrSelf = [&](int anc, int n) { for (; n >= 0; n = model.NodeParent(n)) if (n == anc) return true; return false; };
                while (pelvis >= 0 && !isAncestorOrSelf(pelvis, hangsFrom)) pelvis = model.NodeParent(pelvis);
                if (pelvis < 0) break;
            }
        }
        if (pelvis >= 0 && pelvis < (int)pose.size() && std::abs(fp.PelvisDrop) > 1e-6f) {
            const glm::vec3 now = glm::vec3(modelToWorld * an.Globals[pelvis][3]);
            const glm::vec3 want = glm::vec3(worldToModel * glm::vec4(now + up * (fp.PelvisDrop * w), 1.0f));
            const int parent = model.NodeParent(pelvis);
            const glm::mat4 parentGlobal = parent >= 0 ? an.Globals[parent] : glm::mat4(1.0f);
            pose[pelvis].T = glm::vec3(glm::inverse(parentGlobal) * glm::vec4(want, 1.0f));
            model.PoseToGlobals(pose, an.Globals);
        }

        // 3) Each foot: two-bone IK to the animated position moved by the ground's height (so a raised foot
        //    stays raised by as much), then turn it with the slope from its animated rotation
        const glm::vec3 upModel = glm::normalize(dirToModel * up);
        for (size_t i = 0; i < fp.Feet.size(); i++) {
            const Leg& l = legs[i];
            if (l.End < 0) continue;
            const FootIKLeg& leg = fp.Feet[i];
            const glm::vec3 targetWorld = l.Foot + up * (leg.Lift * w);
            SolveLimb(model, an, pose, l.End, glm::vec3(worldToModel * glm::vec4(targetWorld, 1.0f)), glm::vec3(0.0f), false, 1.0f);

            glm::quat tilt(1.0f, 0.0f, 0.0f, 0.0f);
            const float amount = std::clamp(fp.AlignSlope, 0.0f, 1.0f) * w;
            if (amount > 0.0f) {
                glm::vec3 nModel = glm::normalize(normalToModel * leg.Normal);
                const float angle = SafeAcos(glm::dot(upModel, nModel));
                tilt = RotationBetween(upModel, nModel);
                if (angle > maxTilt && angle > 1e-5f) tilt = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), tilt, maxTilt / angle);
                tilt = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), tilt, amount);
            }
            // The foot's model-space rotation becomes tilt * (its animated one), whatever the leg did to it
            const glm::quat current = RotationOf(an.Globals[l.End]);
            RotateNode(model, an.Globals, pose, l.End, glm::normalize(tilt * l.Rot * glm::inverse(current)));
            model.PoseToGlobals(pose, an.Globals);
        }
    }

    void Scene::UpdateAnimations(float dt) {
        static thread_local std::vector<Model::NodePose> pose, from, layer;   // reused: no allocations per frame
        static thread_local std::vector<uint8_t> mask;
        for (auto [e, an, mr] : m_Registry.view<AnimatorComponent, MeshRendererComponent>().each()) {
            an.Fired.clear();
            const Model* model = dynamic_cast<const Model*>(mr.Geometry.get());
            if (!model || !model->IsSkinned()) { an.Palette.clear(); continue; }

            // The base pose: the animation graph's layers, or Clip (cross-fading from the clip before it).
            // clip / prevTime / curTime / step / loop: the clip that leads it, for root motion.
            int clip = -1;
            float prevTime = 0.0f, curTime = 0.0f, step = 0.0f;
            bool loop = an.Loop;
            if (an.GraphData) {
                AnimGraphBase gb;
                EvaluateAnimGraph(an, *model, dt, pose, gb);
                clip = gb.Clip; prevTime = gb.PrevTime; curTime = gb.Time; step = gb.Step; loop = gb.Loop;
            }
            else {
                // A new clip: fade out of the old one
                if (an.Clip != an.ActiveClip) {
                    an.FadeFrom = an.ActiveClip;
                    an.FadeFromTime = an.Time;
                    an.Fade = (an.FadeFrom.empty() || an.BlendTime <= 0.0f) ? 1.0f : 0.0f;
                    an.ActiveClip = an.Clip;
                    an.Time = 0.0f;
                }
                clip = model->FindAnimation(an.ActiveClip);
                const int fromClip = an.Fade < 1.0f ? model->FindAnimation(an.FadeFrom) : -1;

                const bool advancing = an.Playing && dt != 0.0f;
                step = advancing ? dt * an.Speed : 0.0f;
                if (advancing) {
                    if (an.Fade < 1.0f) an.Fade = std::min(1.0f, an.Fade + std::abs(dt) / std::max(an.BlendTime, 1e-4f));
                }
                else if (dt == 0.0f) {
                    an.Fade = 1.0f;                              // no time passing (editor): no fade, just the clip
                }
                prevTime = an.Time;
                an.Time = AdvanceClip(an, *model, clip, an.ActiveClip, an.Time, step, an.Loop);
                curTime = an.Time;

                if (fromClip >= 0) {                             // the clip fading out keeps running, silently
                    const float d = model->AnimationDuration(fromClip);
                    float t = an.FadeFromTime + step;
                    if (d > 0.0f) { t = an.Loop ? std::fmod(t, d) : std::clamp(t, 0.0f, d); if (t < 0.0f) t += d; }
                    an.FadeFromTime = t;
                }
                model->SamplePose(clip, an.Time, pose);
                if (fromClip >= 0 && an.Fade < 1.0f) {
                    model->SamplePose(fromClip, an.FadeFromTime, from);
                    for (size_t i = 0; i < pose.size(); i++) {
                        pose[i].T = glm::mix(from[i].T, pose[i].T, an.Fade);
                        pose[i].R = glm::slerp(from[i].R, pose[i].R, an.Fade);
                        pose[i].S = glm::mix(from[i].S, pose[i].S, an.Fade);
                    }
                }
            }

            // Root motion: where the root bone goes sideways in the leading clip, the entity goes instead.
            // M takes the bone's translation (its parent's space) into the entity's own space.
            int motionRoot = -1;
            glm::mat3 toEntity(1.0f);
            if (an.RootMotion && clip >= 0) {
                motionRoot = an.RootBone.empty() ? model->RootMotionNode(clip) : model->FindNode(an.RootBone);
                if (motionRoot >= 0) toEntity = glm::mat3(mr.LocalOffset * model->ParentRestGlobal(motionRoot));
            }
            if (motionRoot >= 0 && step != 0.0f) {
                const float d = model->AnimationDuration(clip);
                glm::vec3 delta = model->SampleNodeTranslation(clip, motionRoot, curTime) -
                                  model->SampleNodeTranslation(clip, motionRoot, prevTime);
                if (loop && d > 0.0f) {                          // whole laps carry the clip's full travel
                    const float laps = std::floor((prevTime + step) / d);
                    if (laps != 0.0f)
                        delta += laps * (model->SampleNodeTranslation(clip, motionRoot, d) -
                                         model->SampleNodeTranslation(clip, motionRoot, 0.0f));
                }
                glm::vec3 moveLocal = toEntity * delta;
                moveLocal.y = 0.0f;                              // sideways only: the bob stays in the pose
                const glm::mat4 world = GetWorldMatrix(e);
                SetWorldPosition(e, glm::vec3(world[3]) + glm::mat3(world) * moveLocal);
            }

            // Root motion: keep the root bone where the clip started, sideways (the entity carries it)
            if (motionRoot >= 0 && motionRoot < (int)pose.size()) {
                glm::vec3 offset = toEntity * (model->SampleNodeTranslation(clip, motionRoot, curTime) -
                                               model->SampleNodeTranslation(clip, motionRoot, 0.0f));
                offset.y = 0.0f;
                pose[motionRoot].T -= glm::inverse(toEntity) * offset;
            }

            // Layers, in order: each replaces the pose of its masked nodes by its own clip, by Weight,
            // or (Additive) adds its motion relative to its first frame
            for (auto& L : an.Layers) {
                if (L.TargetWeight >= 0.0f && dt != 0.0f) {      // fading towards a target weight
                    const float delta = L.FadeRate * std::abs(dt);
                    L.Weight = L.Weight < L.TargetWeight ? std::min(L.TargetWeight, L.Weight + delta)
                                                         : std::max(L.TargetWeight, L.Weight - delta);
                    if (L.Weight == L.TargetWeight) L.TargetWeight = -1.0f;
                }
                const int lclip = model->FindAnimation(L.Clip);
                if (lclip < 0) continue;
                L.Time = AdvanceClip(an, *model, lclip, L.Clip, L.Time, an.Playing && dt != 0.0f ? dt * L.Speed : 0.0f, L.Loop);
                const float w = std::clamp(L.Weight, 0.0f, 1.0f);
                if (w <= 0.0f) continue;

                int root = -1;
                if (!L.Mask.empty()) {
                    root = model->FindNode(L.Mask);
                    if (root < 0) continue;                      // an unknown bone masks nothing
                }
                model->SubtreeMask(root, mask);
                model->SamplePose(lclip, L.Time, layer);
                if (L.Additive) {
                    // The change from the clip's first frame, scaled by the weight, on top of the pose
                    model->SamplePose(lclip, 0.0f, from);
                    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
                    for (size_t i = 0; i < pose.size(); i++) {
                        if (!mask[i]) continue;
                        pose[i].T += (layer[i].T - from[i].T) * w;
                        pose[i].R = glm::normalize(pose[i].R * glm::slerp(identity, glm::inverse(from[i].R) * layer[i].R, w));
                        pose[i].S *= glm::mix(glm::vec3(1.0f), layer[i].S / glm::max(from[i].S, glm::vec3(1e-6f)), w);
                    }
                    continue;
                }
                for (size_t i = 0; i < pose.size(); i++) {
                    if (!mask[i]) continue;
                    pose[i].T = glm::mix(pose[i].T, layer[i].T, w);
                    pose[i].R = glm::slerp(pose[i].R, layer[i].R, w);
                    pose[i].S = glm::mix(pose[i].S, layer[i].S, w);
                }
            }
            model->PoseToGlobals(pose, an.Globals);

            // Foot placement on the ground under each foot, then the IK goals on top
            if (an.Feet.Enabled && !an.Feet.Feet.empty()) {
                const glm::mat4 entityWorld = GetWorldMatrix(e);
                PlaceFeet(*model, an, pose, entityWorld * mr.LocalOffset, entityWorld, e, dt, GroundRay,
                    Grounded ? Grounded(e) : -1);
            }

            // Inverse kinematics on the final pose, in model space (the entity's world * LocalOffset undone)
            if (!an.IK.empty()) {
                static thread_local std::vector<Model::NodePose> restPose;
                static thread_local std::vector<glm::mat4> restGlobals;
                static thread_local std::vector<glm::vec3> targets;
                model->SamplePose(-1, 0.0f, restPose);
                model->PoseToGlobals(restPose, restGlobals);
                const glm::mat4 modelToWorld = GetWorldMatrix(e) * mr.LocalOffset;
                const glm::mat4 worldToModel = glm::inverse(modelToWorld);
                const glm::mat3 entityToModel = glm::inverse(glm::mat3(mr.LocalOffset));
                targets.clear();
                for (const AnimIK& ik : an.IK) {
                    glm::vec3 world = ik.Target;                 // a world point, or an offset from the target entity
                    if (ik.TargetID != 0) {
                        const entt::entity t = FindByID(ik.TargetID);
                        world = (t != entt::null ? glm::vec3(GetWorldMatrix(t)[3]) : glm::vec3(modelToWorld[3])) + ik.Target;
                    }
                    targets.push_back(glm::vec3(worldToModel * glm::vec4(world, 1.0f)));
                }
                const glm::vec3 modelForward = glm::normalize(entityToModel * glm::vec3(0.0f, 0.0f, 1.0f));
                SolveIK(*model, an, pose, targets, modelForward, entityToModel, restGlobals);
            }
            model->GlobalsToPalette(an.Globals, an.Palette);
        }
    }

    glm::vec3 Scene::BoneForward(entt::entity e, const std::string& bone) {
        const auto* an = m_Registry.try_get<AnimatorComponent>(e);
        const auto* mr = m_Registry.try_get<MeshRendererComponent>(e);
        const Model* model = mr ? dynamic_cast<const Model*>(mr->Geometry.get()) : nullptr;
        if (!an || !model) return glm::vec3(std::nanf(""));
        const int n = model->FindNode(bone);
        if (n < 0 || n >= (int)an->Globals.size()) return glm::vec3(std::nanf(""));
        std::vector<Model::NodePose> restPose;
        std::vector<glm::mat4> restGlobals;
        model->SamplePose(-1, 0.0f, restPose);
        model->PoseToGlobals(restPose, restGlobals);
        const glm::vec3 modelForward = glm::normalize(glm::inverse(glm::mat3(mr->LocalOffset)) * glm::vec3(0, 0, 1));
        const glm::vec3 now = RotationOf(an->Globals[n]) * (glm::inverse(RotationOf(restGlobals[n])) * modelForward);
        return glm::normalize(glm::mat3(GetWorldMatrix(e) * mr->LocalOffset) * now);
    }

    glm::vec3 Scene::BonePosition(entt::entity e, const std::string& bone) {
        const auto* an = m_Registry.try_get<AnimatorComponent>(e);
        const auto* mr = m_Registry.try_get<MeshRendererComponent>(e);
        const Model* model = mr ? dynamic_cast<const Model*>(mr->Geometry.get()) : nullptr;
        if (!an || !model) return glm::vec3(std::nanf(""));
        const int n = model->FindNode(bone);
        if (n < 0 || n >= (int)an->Globals.size()) return glm::vec3(std::nanf(""));
        return glm::vec3(GetWorldMatrix(e) * mr->LocalOffset * an->Globals[n][3]);
    }

    // The one list of component types that a copy carries (play-mode snapshot, Duplicate, script clone)
    EntitySnapshot Scene::CaptureEntity(entt::entity handle) const {
        EntitySnapshot s;
        s.Tag = m_Registry.get<TagComponent>(handle).Tag;
        s.ID = GetID(handle);
        s.Transform = m_Registry.get<TransformComponent>(handle);
        if (auto* c = m_Registry.try_get<ParentComponent>(handle))           s.Parent = *c;
        if (auto* c = m_Registry.try_get<MeshRendererComponent>(handle))     s.MeshRenderer = *c;
        if (auto* c = m_Registry.try_get<PointLightComponent>(handle))       s.PointLight = *c;
        if (auto* c = m_Registry.try_get<DirectionalLightComponent>(handle)) s.DirectionalLight = *c;
        if (auto* c = m_Registry.try_get<SpinComponent>(handle))             s.Spin = *c;
        if (auto* c = m_Registry.try_get<OrbitComponent>(handle))            s.Orbit = *c;
        if (auto* c = m_Registry.try_get<RigidBodyComponent>(handle))        s.RigidBody = *c;
        if (auto* c = m_Registry.try_get<ColliderComponent>(handle))         s.Collider = *c;
        if (auto* c = m_Registry.try_get<CharacterControllerComponent>(handle)) s.Character = *c;
        if (auto* c = m_Registry.try_get<ScriptComponent>(handle))           s.Script = *c;
        if (auto* c = m_Registry.try_get<CameraComponent>(handle))           s.Camera = *c;
        if (auto* c = m_Registry.try_get<AudioSourceComponent>(handle))      s.AudioSource = *c;
        if (auto* c = m_Registry.try_get<AnimatorComponent>(handle))         s.Animator = *c;
        if (auto* c = m_Registry.try_get<ParticleEmitterComponent>(handle))  s.Particles = *c;
        if (auto* c = m_Registry.try_get<FoliageComponent>(handle))          s.Foliage = *c;
        if (auto* c = m_Registry.try_get<TerrainComponent>(handle))          s.Terrain = *c;
        if (auto* c = m_Registry.try_get<GrassBlockerComponent>(handle))     s.GrassBlocker = *c;
        if (auto* c = m_Registry.try_get<NpcComponent>(handle))              s.Npc = *c;
        if (auto* c = m_Registry.try_get<PickupComponent>(handle))           s.Pickup = *c;
        if (auto* c = m_Registry.try_get<PrefabComponent>(handle))           s.Prefab = *c;
        s.Transient = m_Registry.all_of<TransientComponent>(handle);
        return s;
    }

    entt::entity Scene::CreateFromSnapshot(const EntitySnapshot& s) {
        entt::entity e = m_Registry.create();
        m_Registry.emplace<TagComponent>(e, TagComponent{ s.Tag });
        m_Registry.emplace<IDComponent>(e, IDComponent{ s.ID != 0 ? s.ID : NewID() });
        m_Registry.emplace<TransformComponent>(e, s.Transform);
        if (s.Parent)           m_Registry.emplace<ParentComponent>(e, *s.Parent);
        if (s.MeshRenderer) {
            auto& mr = m_Registry.emplace<MeshRendererComponent>(e, *s.MeshRenderer);
            mr.Merged = false;                 // a copy is drawn on its own (not part of any static batch)
        }
        if (s.PointLight)       m_Registry.emplace<PointLightComponent>(e, *s.PointLight);
        if (s.DirectionalLight) m_Registry.emplace<DirectionalLightComponent>(e, *s.DirectionalLight);
        if (s.Spin)             m_Registry.emplace<SpinComponent>(e, *s.Spin);
        if (s.Orbit)            m_Registry.emplace<OrbitComponent>(e, *s.Orbit);
        if (s.RigidBody)        m_Registry.emplace<RigidBodyComponent>(e, *s.RigidBody);
        if (s.Collider)         m_Registry.emplace<ColliderComponent>(e, *s.Collider);
        if (s.Character)        m_Registry.emplace<CharacterControllerComponent>(e, *s.Character);
        if (s.Script)           m_Registry.emplace<ScriptComponent>(e, *s.Script);
        if (s.Camera)           m_Registry.emplace<CameraComponent>(e, *s.Camera);
        if (s.AudioSource)      m_Registry.emplace<AudioSourceComponent>(e, *s.AudioSource);
        if (s.Animator)         m_Registry.emplace<AnimatorComponent>(e, *s.Animator);
        if (s.Particles)        m_Registry.emplace<ParticleEmitterComponent>(e, *s.Particles);
        if (s.Foliage)          m_Registry.emplace<FoliageComponent>(e, *s.Foliage);
        if (s.Terrain)          m_Registry.emplace<TerrainComponent>(e, *s.Terrain);
        if (s.GrassBlocker)     m_Registry.emplace<GrassBlockerComponent>(e, *s.GrassBlocker);
        if (s.Npc)              m_Registry.emplace<NpcComponent>(e, *s.Npc);
        if (s.Pickup)           m_Registry.emplace<PickupComponent>(e, *s.Pickup);
        if (s.Prefab)           m_Registry.emplace<PrefabComponent>(e, *s.Prefab);
        if (s.Transient)        m_Registry.emplace<TransientComponent>(e);
        return e;
    }

    Entity Scene::DuplicateEntity(Entity source) {
        if (!source) return {};
        // Copy the whole subtree first (copying adds entities, which would disturb a running search),
        // then give every copy a new ID and point the copied children at the copied parents
        std::vector<entt::entity> originals;
        std::function<void(entt::entity)> collect = [&](entt::entity e) {
            originals.push_back(e);
            for (auto c : GetChildren(e)) collect(c);
        };
        collect(source.GetHandle());

        std::vector<EntitySnapshot> copies;
        for (auto e : originals) copies.push_back(CaptureEntity(e));
        std::unordered_map<uint64_t, uint64_t> newIDs;
        for (auto& s : copies) { const uint64_t id = NewID(); newIDs[s.ID] = id; s.ID = id; }
        for (size_t i = 1; i < copies.size(); i++)           // the root keeps its parent: a sibling copy
            if (copies[i].Parent) copies[i].Parent->Parent = newIDs[copies[i].Parent->Parent];

        entt::entity root = entt::null;
        for (const auto& s : copies) {
            entt::entity e = CreateFromSnapshot(s);
            if (root == entt::null) root = e;
        }
        return Entity(root, this);
    }

    SceneSnapshot Scene::Capture(entt::entity selected, int& selectedIndex) {
        SceneSnapshot snap;
        selectedIndex = -1;
        for (auto handle : m_Registry.view<TagComponent>()) {
            if (handle == selected) selectedIndex = (int)snap.size();
            snap.push_back(CaptureEntity(handle));
        }
        return snap;
    }

    entt::entity Scene::Restore(const SceneSnapshot& snap, int selectedIndex) {
        Clear();                       // entities and any merged static meshes
        entt::entity selected = entt::null;

        // The registry iterates newest-first, and Capture stored them in that order.
        // Rebuild back to front so the Hierarchy list order doesn't flip.
        for (size_t n = snap.size(); n-- > 0;) {
            entt::entity e = CreateFromSnapshot(snap[n]);
            if ((int)n == selectedIndex) selected = e;
        }
        return selected;
    }

    // ---------------------------------------------------------------- light gathering
    // The cube faces a light's shadows need (+X, -X, +Y, -Y, +Z, -Z as bits): all six for a point light, for a spot
    // only those its cone can reach (a face sees up to 54.74 degrees off its axis)
    static uint8_t CubeFaceMask(const PointLightData& l) {
        if (!l.IsSpot()) return 0x3F;
        static const glm::vec3 dirs[6] = { { 1, 0, 0 },{ -1, 0, 0 },{ 0, 1, 0 },{ 0, -1, 0 },{ 0, 0, 1 },{ 0, 0, -1 } };
        const float half = std::acos(std::clamp(l.CosOuter, -1.0f, 1.0f));
        const float faceCos = half + 0.9553166f >= glm::pi<float>() ? -2.0f : std::cos(half + 0.9553166f);
        uint8_t mask = 0;
        for (int f = 0; f < 6; f++) if (glm::dot(l.Dir, dirs[f]) >= faceCos) mask |= (uint8_t)(1u << f);
        return mask;
    }

    // The shadow atlas's share-out (GatherPointLights): the AtlasLights most useful candidates (score, index into
    // `all`, best = lowest) get squares sized by how much of the view their reach covers: a spot up to a quarter of the
    // atlas across, a cube face up to an eighth; with room to spare all of them double while they fill at most three
    // quarters of it. A light keeps its squares (and their cached shadows) until its wanted
    // size doubles or drops to a quarter. When the wanted squares would fill more than 90% of the atlas, the least
    // useful lights are halved first. Placing goes most useful first; when no square is free, a less useful light's
    // squares are taken (it is placed again after), and failing that the light tries a smaller size. Sets AtlasEntry.
    static void AssignAtlas(const ShadowContext& ctx, std::vector<std::pair<float, uint32_t>>& candidates,
                            const std::vector<float>& coverage, std::vector<PointLightData>& all) {
        ShadowAtlas& atlas = *ctx.Atlas;
        auto& entries = ctx.AtlasEntries;
        const ShadowSettings& s = ctx.Settings;
        uint32_t size = 1024;
        while (size < (uint32_t)s.AtlasSize && size < 8192) size <<= 1;
        if (atlas.Size() != size) { atlas.Resize(size); entries.clear(); }

        std::sort(candidates.begin(), candidates.end());
        candidates.resize(std::min(candidates.size(), (size_t)(LightClusters::MaxDirect + std::clamp(s.AtlasLights, 0, ShadowSettings::MaxAtlasLights))));

        auto findEntry = [&](uint32_t entity) {
            for (size_t k = 0; k < entries.size(); k++) if (entries[k].Entity == entity) return (int)k;
            return -1;
        };
        auto pow2Floor = [](float v) { uint32_t p = ShadowAtlas::MinTile; while ((float)(p * 2) <= v) p *= 2; return p; };
        struct Request { uint32_t Light; bool Cube; bool Direct; uint8_t Mask; int Faces; uint32_t Tile; float Far; };
        // The largest square: a spot a quarter of the atlas across, a cube face an eighth; half that for lights not shaded directly
        auto maxTileOf = [&](const Request& r) { return std::max(size / (r.Cube ? 8u : 4u) / (r.Direct ? 1u : 2u), ShadowAtlas::MinTile); };
        std::vector<Request> reqs;
        reqs.reserve(candidates.size());
        for (const auto& [score, i] : candidates) {
            const PointLightData& d = all[i];
            Request r;
            r.Light = i;
            r.Cube = !d.UsesSpotMap();
            r.Direct = d.Shadows;
            r.Mask = r.Cube ? CubeFaceMask(d) : (uint8_t)1;
            r.Faces = 0;
            for (int f = 0; f < 6; f++) if (r.Mask & (1u << f)) r.Faces++;
            r.Far = r.Cube ? std::min(d.Range, s.PointFar) : d.Range;
            const uint32_t maxTile = maxTileOf(r);
            r.Tile = std::min(pow2Floor(coverage[i] * 2.0f * (float)maxTile), maxTile);
            if (r.Faces > 0) reqs.push_back(r);
        }
        // Room to spare: double every square (up to its kind's largest) while they fill at most three quarters
        for (;;) {
            uint64_t grown = 0;
            bool any = false;
            for (const Request& r : reqs) {
                const uint32_t t = std::min(r.Tile * 2, maxTileOf(r));
                any = any || t != r.Tile;
                grown += (uint64_t)r.Faces * t * t;
            }
            if (!any || grown > (uint64_t)size * size * 3 / 4) break;
            for (Request& r : reqs) r.Tile = std::min(r.Tile * 2, maxTileOf(r));
        }
        // ... then one light at a time, most useful first, with what room is left
        {
            const uint64_t room = (uint64_t)size * size * 3 / 4;
            uint64_t used = 0;
            for (const Request& r : reqs) used += (uint64_t)r.Faces * r.Tile * r.Tile;
            for (Request& r : reqs) {
                const uint64_t more = (uint64_t)r.Faces * r.Tile * r.Tile * 3;
                if (r.Tile * 2 <= maxTileOf(r) && used + more <= room) { r.Tile *= 2; used += more; }
            }
        }
        // A light keeps the size it has unless the wanted one doubled or dropped to a quarter
        for (Request& r : reqs) {
            const int k = findEntry(all[r.Light].Entity);
            if (k < 0) continue;
            const auto& e = entries[(size_t)k];
            if (e.Tile > 0 && e.Cube == r.Cube && e.FaceMask == r.Mask && r.Tile <= e.Tile && r.Tile * 4 > e.Tile) r.Tile = e.Tile;
        }

        // Fit: halve the least useful while the squares would fill more than 90% of the atlas; drop them if that isn't enough
        const uint64_t budget = (uint64_t)size * size * 9 / 10;
        uint64_t total = 0;
        for (const Request& r : reqs) total += (uint64_t)r.Faces * r.Tile * r.Tile;
        while (total > budget && !reqs.empty()) {
            bool shrunk = false;
            for (size_t k = reqs.size(); k-- > 0 && total > budget;) {
                Request& r = reqs[k];
                if (r.Tile <= ShadowAtlas::MinTile) continue;
                total -= (uint64_t)r.Faces * ((uint64_t)r.Tile * r.Tile - (uint64_t)r.Tile * r.Tile / 4);
                r.Tile /= 2;
                shrunk = true;
            }
            if (!shrunk) { total -= (uint64_t)reqs.back().Faces * reqs.back().Tile * reqs.back().Tile; reqs.pop_back(); }
        }

        // Give up the squares of lights no longer wanted, or whose shape or size changed
        auto freeEntry = [&](ShadowContext::AtlasEntry& e) {
            if (e.Tile == 0) return;
            for (int f = 0; f < 6; f++) if (e.FaceMask & (1u << f)) atlas.Free(e.At[f], e.Tile);
            e.Tile = 0;
            e.Cache.Valid = false;
        };
        for (auto& e : entries) e.Kept = false;
        for (const Request& r : reqs) {
            const int k = findEntry(all[r.Light].Entity);
            if (k < 0) continue;
            auto& e = entries[(size_t)k];
            e.Kept = true;
            if (e.Tile != r.Tile || e.Cube != r.Cube || e.FaceMask != r.Mask) freeEntry(e);
        }
        for (auto& e : entries) if (!e.Kept) freeEntry(e);
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const ShadowContext::AtlasEntry& e) { return !e.Kept; }), entries.end());

        // Place, most useful first
        auto place = [&](ShadowContext::AtlasEntry& e, uint32_t tile, uint8_t mask) {
            for (int f = 0; f < 6; f++) {
                if (!(mask & (1u << f))) continue;
                if (!atlas.Alloc(tile, e.At[f])) {
                    for (int g = 0; g < f; g++) if (mask & (1u << g)) atlas.Free(e.At[g], tile);
                    return false;
                }
            }
            e.Tile = tile;
            e.FaceMask = mask;
            e.Cache.Valid = false;
            return true;
        };
        std::vector<int> entryOf(reqs.size(), -1);
        for (size_t q = 0; q < reqs.size(); q++) {
            const Request& r = reqs[q];
            int k = findEntry(all[r.Light].Entity);
            if (k < 0) {
                entries.emplace_back();
                k = (int)entries.size() - 1;
                entries[(size_t)k].Entity = all[r.Light].Entity;
            }
            entryOf[q] = k;
            auto& e = entries[(size_t)k];
            e.Cube = r.Cube;
            e.Far = r.Far;
            e.Kept = true;
            if (e.Tile == r.Tile && e.FaceMask == r.Mask) continue;           // kept its squares
            uint32_t tile = r.Tile;
            while (!place(entries[(size_t)k], tile, r.Mask)) {
                // Full: take a less useful light's squares (it is placed again when its turn comes), else go smaller
                bool took = false;
                for (size_t w = reqs.size(); w-- > q + 1;) {
                    const int kw = findEntry(all[reqs[w].Light].Entity);
                    if (kw >= 0 && entries[(size_t)kw].Tile > 0) { freeEntry(entries[(size_t)kw]); took = true; break; }
                }
                if (!took) { tile /= 2; if (tile < ShadowAtlas::MinTile) break; }
            }
        }
        uint32_t held = 0;
        for (size_t q = 0; q < reqs.size(); q++)
            if (entries[(size_t)entryOf[q]].Tile > 0) { all[reqs[q].Light].AtlasEntry = entryOf[q]; held++; }
        ctx.AtlasLightsHeld = held;
    }

    // Every point and spot light of the frame into `out` (DrawList::Lights), direct slots first: index i <
    // LightClusters::MaxDirect is a direct light (Shadows set: shaded from uniforms) or a placeholder. Shadow-casting
    // lights are ranked by usefulness: those whose reach the camera can see, nearest (to the edge of their reach) first;
    // a light holding shadows wins ties by a quarter, so cached maps aren't thrown away as the camera moves. The best
    // MaxDirect take the direct slots; they and the next AtlasLights get shadows in the shadow atlas (AssignAtlas). ctx null: no shadows. tiles: the atlas squares for the shaders. RenderShadows and
    // SetLightingUniforms read this same list.
    template <typename VisibleFn>   // VisibleFn(centre, half extent): inside the camera view (Frustum, defined below)
    static void GatherPointLights(Scene& scene, const Camera& camera, const VisibleFn& visible, const ShadowContext* ctx,
                                  std::vector<PointLightData>& out, std::vector<ShadowTileGpu>* tiles) {
        constexpr int kCubes = LightClusters::MaxDirect;
        out.clear();
        std::vector<PointLightData> all;
        std::vector<float> coverage;           // per light: how much of the view its reach covers (0..1; sizes atlas squares)
        std::vector<std::pair<float, uint32_t>> cubeCandidates, spotCandidates;     // (score, index into all)
        std::unordered_set<uint32_t> holders;  // lights holding shadows now (sharp cubes, atlas squares)
        if (ctx) {
            for (uint32_t o : ctx->SlotOwner) if (o != ~0u) holders.insert(o);
            for (const auto& e : ctx->AtlasEntries) if (e.Tile > 0) holders.insert(e.Entity);
        }
        auto view = scene.Registry().view<TransformComponent, PointLightComponent>();
        for (auto entity : view) {
            const auto& l = view.get<PointLightComponent>(entity);
            PointLightData d;
            const glm::mat4 world = scene.GetWorldMatrix(entity);
            d.Pos = glm::vec3(world[3]);
            d.Color = l.Color * l.Intensity;
            d.Range = l.Range > 0.0f ? l.Range : AutoPointLightRange(d.Color);
            d.Radius = std::max(l.Radius, 0.0f);
            d.Contact = l.ContactShadows;
            const bool cookie = l.Cookie && l.Cookie->GetID(), profile = l.Profile && l.Profile->GetID();
            if (l.IsSpot() || cookie || profile) {     // shines along local -Z, like the sun (points: the cookie's aim; area lights below)
                const glm::vec3 fwd = -glm::vec3(world[2]);
                const float len = glm::length(fwd);
                if (len > 1e-6f) d.Dir = fwd / len;
            }
            if (l.IsSpot()) d.SetCone(l.SpotAngle, l.SpotBlend);
            if (l.Shape != LightShape::Sphere) {
                // An area light: its axes from the entity (so it scales with it); a rect shines along its -Z
                const glm::vec3 fwd = -glm::vec3(world[2]);
                if (glm::length(fwd) > 1e-6f) d.Dir = glm::normalize(fwd);
                const glm::vec3 x = glm::vec3(world[0]);
                const float sx = glm::length(x), sy = glm::length(glm::vec3(world[1]));
                d.Shape = (int)l.Shape;
                d.AreaX = (sx > 1e-6f ? x / sx : glm::vec3(1, 0, 0)) * std::max(l.Size.x, 0.0f) * sx * 0.5f;
                d.AreaHalfY = l.Shape == LightShape::Rect ? std::max(l.Size.y, 0.0f) * sy * 0.5f : 0.0f;
                d.Extent = std::sqrt(glm::dot(d.AreaX, d.AreaX) + d.AreaHalfY * d.AreaHalfY);
            }
            if (cookie || profile) {
                // The cookie's top follows the entity's +Y: its turn from the basis the shaders build
                glm::vec3 right, up;
                CookieBasis(d.Dir, right, up);
                const glm::vec3 y = glm::vec3(world[1]);
                d.CookieRoll = std::atan2(-glm::dot(y, right), glm::dot(y, up));
                if (cookie) {
                    d.Cookie = l.Cookie.get();
                    d.CookieGrid = glm::ivec2(std::max(l.CookieColumns, 1), std::max(l.CookieRows, 1));
                    d.CookieFrame = l.CookieFrame;
                    if (l.CookieFps > 0.0f) d.CookieFrame += (int)std::floor(scene.WindTime * l.CookieFps);   // WindTime runs in Edit mode too
                }
                if (profile) d.Profile = l.Profile.get();
            }
            d.Entity = (uint32_t)entity;
            float cov = 0.0f;
            if (ctx && l.CastShadows && d.Range > 0.0f) {
                const glm::vec4 b = PointLightBounds(d);       // a spot light: the sphere round its cone
                const float dist = glm::length(glm::vec3(b) - camera.Position);
                const float edge = std::max(dist - b.w, 0.0f);
                const bool seen = visible(glm::vec3(b), glm::vec3(b.w));
                float score = seen ? edge : 1e6f + edge;
                if (holders.count(d.Entity)) score *= 0.75f;   // a light keeps its shadows while it is about as useful
                cov = seen ? b.w / std::max(dist, b.w) : 0.0f;
                (d.UsesSpotMap() ? spotCandidates : cubeCandidates).emplace_back(score, (uint32_t)all.size());
            }
            all.push_back(d);
            coverage.push_back(cov);
        }

        // The best `slots` candidates get a slot: those that held one keep it, the rest take the free ones
        auto assign = [&](std::vector<std::pair<float, uint32_t>>& candidates, uint32_t* owners, int slots, std::vector<int>& slotOf) {
            const size_t keep = std::min(candidates.size(), (size_t)slots);
            std::partial_sort(candidates.begin(), candidates.begin() + keep, candidates.end());
            candidates.resize(keep);
            std::vector<bool> used((size_t)slots, false);
            for (auto& [score, i] : candidates)
                for (int s = 0; s < slots; s++)
                    if (owners[s] == all[i].Entity) { slotOf[i] = s; used[s] = true; break; }
            for (auto& [score, i] : candidates) {
                if (slotOf[i] >= 0) continue;
                for (int s = 0; s < slots; s++) if (!used[s]) { slotOf[i] = s; used[s] = true; break; }
            }
            for (int s = 0; s < slots; s++) owners[s] = ~0u;
            for (size_t i = 0; i < all.size(); i++) if (slotOf[i] >= 0) owners[slotOf[i]] = all[i].Entity;
        };
        std::vector<int> slotOf(all.size(), -1);
#ifdef __EMSCRIPTEN__
        // The web build has no point / spot light shadows (the atlas needs more textures than WebGL2 gives a shader)
        if (ctx) ctx->AtlasEntries.clear();
        if (false) {
#else
        if (ctx) {
#endif
            // Every shadowed light by rank: the best MaxDirect are shaded directly from uniforms (and get the biggest
            // atlas squares); they and the next AtlasLights get shadows in the atlas
            std::vector<std::pair<float, uint32_t>> ranked = cubeCandidates;
            ranked.insert(ranked.end(), spotCandidates.begin(), spotCandidates.end());
            std::sort(ranked.begin(), ranked.end());
            std::vector<std::pair<float, uint32_t>> direct(ranked.begin(), ranked.begin() + (ptrdiff_t)std::min(ranked.size(), (size_t)kCubes));
            assign(direct, ctx->SlotOwner, kCubes, slotOf);
            for (size_t i = 0; i < all.size(); i++) if (slotOf[i] >= 0) all[i].Shadows = true;
            if (ctx->Atlas) AssignAtlas(*ctx, ranked, coverage, all);
            else ctx->AtlasEntries.clear();
        }

        // Cube slots first (placeholders up to the last slot used), then everything else in registry order
        int lastSlot = -1;
        for (int s : slotOf) lastSlot = std::max(lastSlot, s);
        out.resize((size_t)(lastSlot + 1));
        for (size_t i = 0; i < all.size(); i++)
            if (slotOf[i] >= 0) { out[slotOf[i]] = all[i]; out[slotOf[i]].Shadows = true; }
        for (size_t i = 0; i < all.size(); i++)
            if (slotOf[i] < 0) out.push_back(all[i]);

        // The atlas squares the shaders read (LightClusters::SetShadowTiles): a spot's one, a cube's six (faces it
        // holds no square for stay empty), each with the view-projection GatherShadowViews draws it with
        if (tiles) {
            tiles->clear();
            if (ctx && ctx->Atlas) {
                const float inv = 1.0f / (float)ctx->Atlas->Size();
                for (PointLightData& d : out) {
                    if (d.AtlasEntry < 0) continue;
                    const ShadowContext::AtlasEntry& e = ctx->AtlasEntries[(size_t)d.AtlasEntry];
                    auto rect = [&](int k) { return glm::vec4(glm::vec2(e.At[k]) * inv, (float)e.Tile * inv, e.Far); };
                    if (e.Cube) {
                        d.AtlasCube = (int)tiles->size();
                        for (int f = 0; f < 6; f++) {
                            ShadowTileGpu t;
                            if (e.FaceMask & (1u << f)) { t.ViewProj = AtlasFaceMatrix(d.Pos, f, e.Tile, e.Far); t.Rect = rect(f); }
                            tiles->push_back(t);
                        }
                    } else {
                        d.AtlasSpot = (int)tiles->size();
                        tiles->push_back({ SpotShadowMatrix(d), rect(0) });
                    }
                }
            }
        }
    }

    // How one part of a Mesh Renderer uses alpha: the component's settings combined with the
    // model material's (the stronger mode wins, alphas multiply)
    struct SurfaceAlpha {
        AlphaMode Mode = AlphaMode::Opaque;
        float Alpha = 1.0f;
        float Cutoff = 0.5f;
        const Texture2D* Albedo = nullptr;
        bool DoubleSided = false;          // the component or the model material asks for it
    };

    // Whether a Mesh Renderer is drawn part by part (each submesh with its own material): with model materials or
    // material files. Otherwise it is one part (-1 = the whole renderable) in the component's own values.
    static bool SplitParts(const MeshRendererComponent& mr) { return mr.UseModelMaterials || !mr.Materials.empty(); }
    static uint32_t PartCount(const MeshRendererComponent& mr, const Renderable* geometry) {
        return SplitParts(mr) ? std::max(geometry->SubmeshCount(), 1u) : 1u;
    }
    // The material of part i: its slot's material file, else the model file's (Use model materials), else the built-in
    // Default. Never null. geometry: the level of detail drawn (its parts follow the main one's slots).
    static const Material* PartMaterial(const MeshRendererComponent& mr, const Renderable* geometry, uint32_t i) {
        if (i < mr.Materials.size() && mr.Materials[i].Asset) return &mr.Materials[i].Asset->Surface;
        if (mr.UseModelMaterials)
            if (const Material* m = geometry->GetSubmeshMaterial(i)) return m;
        return &DefaultMaterial();
    }
    // A part's values: its material's, with the script overrides (MeshOverrides) in their place
    struct PartLook { glm::vec3 Color, Emissive; float Metallic, Roughness; };
    static PartLook LookOf(const MeshRendererComponent& mr, const Material& m) {
        const MeshOverrides& o = mr.Overrides;
        return { o.Color.value_or(m.Color), o.Emissive.value_or(m.EmissiveColor),
                 o.Metallic.value_or(m.Metallic), o.Roughness.value_or(m.Roughness) };
    }
    // A material's texture placement and normal strength in the spare padding of InstanceData::Normal (a mat3 in a
    // mat4), coded so that zeros, what every other instance has, mean no change: lit.vert / shadow.vert read them
    static void SetSurfaceExtras(InstanceData& inst, const Material* mat) {
        if (!mat) return;
        inst.Normal[0].w = mat->UVScale.x - 1.0f;
        inst.Normal[1].w = mat->UVScale.y - 1.0f;
        inst.Normal[2].w = 1.0f - mat->NormalStrength;
        inst.Normal[3].x = mat->UVOffset.x;
        inst.Normal[3].y = mat->UVOffset.y;
    }

    // geometry: the level of detail being drawn (null = mr.Geometry)
    static SurfaceAlpha GetSurfaceAlpha(const MeshRendererComponent& mr, uint32_t part, const Renderable* geometry = nullptr) {
        if (!geometry) geometry = mr.Geometry.get();
        const Material* mat = PartMaterial(mr, geometry, part);
        SurfaceAlpha s;
        s.Mode = mr.Alpha;
        s.Alpha = std::clamp(mr.Opacity, 0.0f, 1.0f);
        s.Cutoff = mr.AlphaCutoff;
        s.Albedo = mat->Albedo.get();
        s.DoubleSided = mr.DoubleSided || mat->DoubleSided;
        if ((int)mat->Mode > (int)s.Mode) s.Mode = mat->Mode;
        if (mr.Alpha != AlphaMode::Cutout) s.Cutoff = mat->AlphaCutoff;
        s.Alpha *= mat->Alpha;
        // Alpha-tested: smaller mips keep as many pixels above the cutoff as full size (once per texture),
        // so cut-out leaves don't thin out with distance. After the material is merged in: it decides the mode.
        if (s.Mode == AlphaMode::Cutout && s.Albedo) s.Albedo->PreserveAlphaCoverage(s.Cutoff);
        return s;
    }

    // Specular factor of a part's material as InstanceData::Info.z = 1000 x (1 - specular), so zero, the default of
    // every instance, means full specular. lit.vert turns it back into 0..1.
    static int SpecularCode(const Material* mat) {
        const float s = std::clamp(mat->Specular, 0.0f, 1.0f);
        return (int)std::lround((1.0f - s) * 1000.0f);
    }

    // First directional light, if any. dir = the direction the light travels.
    static bool FindSun(Scene& scene, glm::vec3& dir, glm::vec3& color, bool& shadows) {
        auto& reg = scene.Registry();
        auto view = reg.view<TransformComponent, DirectionalLightComponent>();
        for (auto entity : view) {
            auto [t, d] = view.get<TransformComponent, DirectionalLightComponent>(entity);
            dir = glm::normalize(glm::vec3(scene.GetWorldMatrix(entity) * glm::vec4(0, 0, -1, 0)));
            color = d.Color * d.Intensity;
            // A sun that gives no light casts no shadows: its passes would cost the same for nothing (the village at
            // night drew 4 invisible cascades: about 2 ms of GPU time)
            shadows = d.CastShadows && glm::max(color.r, glm::max(color.g, color.b)) > 0.0f;
            return true;
        }
        return false;
    }

    // ---------------------------------------------------------------- draw list (batching + instancing)
    namespace {
        // The six planes of a view-projection matrix (Gribb/Hartmann); a point p is inside when
        // dot(plane.xyz, p) + plane.w >= 0 for all of them
        struct Frustum {
            glm::vec4 Planes[6];
            explicit Frustum(const glm::mat4& vp) {
                const glm::mat4 m = glm::transpose(vp);           // rows of vp
                Planes[0] = m[3] + m[0]; Planes[1] = m[3] - m[0];
                Planes[2] = m[3] + m[1]; Planes[3] = m[3] - m[1];
                Planes[4] = m[3] + m[2]; Planes[5] = m[3] - m[2];
                for (auto& p : Planes) p /= glm::length(glm::vec3(p));
            }
            // A local box under a transform, tested as its world-space bounding box
            bool Visible(const glm::mat4& world, const glm::vec3& mn, const glm::vec3& mx) const {
                glm::vec3 c, e;
                WorldBox(world, mn, mx, c, e);
                return Visible(c, e);
            }
            // A world-space box (centre, half extents): one object is tested against several views with it
            bool Visible(const glm::vec3& c, const glm::vec3& e) const {
                for (const auto& p : Planes)
                    if (glm::dot(glm::vec3(p), c) + glm::dot(glm::abs(glm::vec3(p)), e) + p.w < 0.0f) return false;
                return true;
            }
            static void WorldBox(const glm::mat4& world, const glm::vec3& mn, const glm::vec3& mx, glm::vec3& c, glm::vec3& e) {
                c = glm::vec3(world * glm::vec4((mn + mx) * 0.5f, 1.0f));
                const glm::vec3 h = (mx - mn) * 0.5f;
                e = glm::abs(glm::vec3(world[0])) * h.x + glm::abs(glm::vec3(world[1])) * h.y + glm::abs(glm::vec3(world[2])) * h.z;
            }
        };

        void SetBones(Shader& shader, const std::vector<glm::mat4>* bones) {
            constexpr size_t kMaxBones = 128;                   // MAX_BONES in lit.vert / shadow.vert
            shader.SetInt("u_Skinned", bones ? 1 : 0);
            if (!bones) return;
            if (bones->size() > kMaxBones) {
                static bool warned = false;
                if (!warned) { ENGINE_WARN("A model has {} bones; only {} are supported", bones->size(), kMaxBones); warned = true; }
            }
            shader.SetMat4Array("u_Bones", bones->data(), (uint32_t)std::min(bones->size(), kMaxBones));
        }

        // Face culling per batch: a double-sided batch turns it off, the next single-sided one back on. Leaves it
        // as it found it, and never turns on culling that was off (X-ray wireframe, grass).
        struct CullSwitch {
            const bool WasOn = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
            bool On = WasOn;
            void Set(bool doubleSided) {
                const bool want = WasOn && !doubleSided;
                if (want == On) return;
                if (want) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
                On = want;
            }
            ~CullSwitch() { if (On != WasOn) { if (WasOn) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE); } }
        };

        // A terrain's layers for lit.frag: the weights on unit 9, the layer textures on 10..13 (the sampler units are
        // set once with the lighting uniforms)
        void BindTerrain(Shader& shader, const TerrainMaterial& m) {
            glBindTextureUnit(9, m.Splat);
            glm::vec4 use(0.0f);
            for (int i = 0; i < 4; i++) {
                if (m.Layers[i]) { m.Layers[i]->Bind(10 + i); use[i] = 1.0f; }
#ifndef __EMSCRIPTEN__
                if (m.Surface[i]) m.Surface[i]->Bind(14 + i);   // (the web build reads storage buffers from 14..21)
#endif
                shader.SetVec4("u_TerrainColor[" + std::to_string(i) + "]", m.Color[i]);
            }
            shader.SetVec4("u_TerrainUseTex", use);
            shader.SetVec4("u_TerrainTiling", m.Tiling);
            shader.SetVec4("u_TerrainVariation", m.Variation);
            shader.SetIVec4("u_TerrainMaps", m.Maps);
            shader.SetVec4("u_TerrainNormalStrength", m.NormalStrength);
            shader.SetVec2("u_TerrainSize", m.Size);
            shader.SetFloat("u_TerrainBlend", m.Blend);
        }

        void DrawBatchGeometry(const DrawBatch& b) {
            if (b.Count == 0) return;                  // every instance culled (occlusion)
            if (b.Part < 0) b.Geometry->DrawInstanced(b.Count);
            else            b.Geometry->DrawSubmeshInstanced((uint32_t)b.Part, b.Count);
        }
    }

    // Bone matrices for a skinned model: the Animator pose, or the rest pose. Null if not skinned.
    const std::vector<glm::mat4>* Scene::GetBonePalette(entt::entity e, const MeshRendererComponent& mr) const {
        const Model* model = dynamic_cast<const Model*>(mr.Geometry.get());
        if (!model || !model->IsSkinned()) return nullptr;
        const auto* an = m_Registry.try_get<AnimatorComponent>(e);
        return (an && !an->Palette.empty()) ? &an->Palette : &model->RestPalette();
    }

    constexpr float kSunBiasTexels = 3.0f;    // sun depth bias at the default SunBias, in shadow-map texels

    // An orthographic sun view around a sphere (center, radius), looking along dir. Casters up to
    // `back` behind the sphere still count. The centre is snapped to whole texels so shadows don't
    // shimmer while the camera moves.
    static ShadowView FitSunView(const glm::vec3& dir, glm::vec3 center, float radius, float back, uint32_t size, int cascade) {
        const glm::vec3 up = std::abs(dir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::mat4 rot = glm::lookAt(glm::vec3(0.0f), dir, up);
        const float texel = 2.0f * radius / (float)size;
        glm::vec3 lc = glm::vec3(rot * glm::vec4(center, 1.0f));
        lc.x = std::floor(lc.x / texel) * texel;
        lc.y = std::floor(lc.y / texel) * texel;
        center = glm::vec3(glm::inverse(rot) * glm::vec4(lc, 1.0f));

        ShadowView v;
        const glm::vec3 eye = center - dir * (radius + back);
        v.ViewProj = glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + back) * glm::lookAt(eye, center, up);
        v.Light = -1;
        v.Face = cascade;
        v.TexelWorld = texel;
        v.DepthRange = 2.0f * radius + back;
        return v;
    }

    // The shadow views of this frame, in the order BuildDrawList fills DrawList::ShadowViews and
    // RenderShadows draws them: the atlas squares of each shadowed light (a spot's one, a cube's faces), then the sun cascades.
    // Also sets list.CascadeSplits / CascadeCount.
    static std::vector<ShadowView> GatherShadowViews(Scene& scene, const ShadowContext& ctx,
        const std::vector<PointLightData>& lights, const Camera& camera, uint32_t sunSize, DrawList& list) {
        const ShadowSettings& s = ctx.Settings;
        std::vector<ShadowView> views;
        if (!s.Enabled) return views;
        // Which lights get their squares re-rendered this frame (the rest keep last time's: the atlas persists).
        // Forced: new squares, moved, turned, re-coned, a different far plane, or something moved near it last frame.
        // Otherwise every PointUpdateInterval frames, staggered so the lights take turns.
        const uint32_t interval = (uint32_t)std::max(s.PointUpdateInterval, 1);
        const uint32_t frame = ctx.Frame++;
        ctx.PointLightsDrawn = 0;
        // The atlas lights: a spot's one square, a cube's squares (faces it needs), cached the same way (new squares,
        // the light moving, turning, changing its cone or far plane, or something moving near it redraws at once).
        // Still cubes not shaded directly redraw at half the rate (there are more of them, and they are further away)
        if (ctx.Atlas)
            for (int i = 0; i < (int)lights.size(); i++) {
                const int k = lights[i].AtlasEntry;
                if (k < 0 || k >= (int)ctx.AtlasEntries.size()) continue;
                ShadowContext::AtlasEntry& e = ctx.AtlasEntries[(size_t)k];
                if (e.Tile == 0) continue;
                auto& c = e.Cache;
                const glm::vec4 cone = lights[i].IsSpot() ? glm::vec4(lights[i].Dir, lights[i].CosOuter) : glm::vec4(0.0f, 0.0f, 0.0f, -2.0f);
                const bool forced = !c.Valid || c.Pos != lights[i].Pos || c.Cone != cone || c.Range != e.Far || c.MovedNear;
                const uint32_t every = (e.Cube && !lights[i].Shadows) ? interval * 2 : interval;
                if (!forced && (frame % every) != ((uint32_t)(k + 1) % every)) continue;
                c.Valid = true;
                c.Pos = lights[i].Pos;
                c.Cone = cone;
                c.Range = e.Far;
                c.MovedNear = false;
                ctx.PointLightsDrawn++;
                for (int f = 0; f < (e.Cube ? 6 : 1); f++) {
                    if (e.Cube && !(e.FaceMask & (1u << f))) continue;
                    ShadowView v;
                    v.ViewProj = e.Cube ? AtlasFaceMatrix(lights[i].Pos, f, e.Tile, e.Far) : SpotShadowMatrix(lights[i]);
                    v.Light = i;
                    v.Face = f;
                    v.AtlasTile = e.Tile;
                    v.AtlasAt = e.At[f];
                    v.Reach = e.Far;
                    views.push_back(v);
                }
            }

        glm::vec3 sunDir, sunColor;
        bool sunShadows = false;
        ctx.SunCascadesDrawn = 0;
        if (!FindSun(scene, sunDir, sunColor, sunShadows) || !sunShadows) {
            for (auto& cc : ctx.SunCache) cc.Valid = false;     // the layers aren't kept up while off
            return views;
        }

        if (!s.SunCascades) {
            for (auto& cc : ctx.SunCache) cc.Valid = false;
            // The old fixed area: a box of SunExtent around the origin, never fading out
            views.push_back(FitSunView(sunDir, glm::vec3(0.0f), s.SunExtent, s.SunDistance, sunSize, 0));
            list.CascadeSplits = glm::vec4(1e9f);
            list.CascadeCount = 1;
            return views;
        }

        // Split the view from the near plane to ShadowDistance: a blend of even and logarithmic slices
        const int count = std::clamp(s.CascadeCount, 1, (int)DirectionalShadowMap::MaxCascades);
        const float n = std::max(camera.GetNear(), 0.05f);
        const float f = std::max(std::min(s.ShadowDistance, camera.GetFar()), n + 1.0f);
        const float lambda = std::clamp(s.CascadeSplit, 0.0f, 1.0f);
        const float tanV = std::tan(glm::radians(camera.GetFov()) * 0.5f);
        const float tanH = tanV * camera.GetAspect();
        const glm::mat4 toWorld = glm::inverse(camera.GetView());

        float sliceNear = n;
        for (int c = 0; c < count; c++) {
            const float t = (float)(c + 1) / (float)count;
            const float sliceFar = lambda * n * std::pow(f / n, t) + (1.0f - lambda) * (n + (f - n) * t);
            list.CascadeSplits[c] = sliceFar;

            // A bounding sphere of the slice. Its size depends only on the lens and the depths, not on
            // where the camera looks, so the cascade keeps its texel size while the camera turns.
            const float zc = 0.5f * (sliceNear + sliceFar);
            const float rNear = glm::length(glm::vec3(sliceNear * tanH, sliceNear * tanV, sliceNear - zc));
            const float rFar = glm::length(glm::vec3(sliceFar * tanH, sliceFar * tanV, sliceFar - zc));
            float radius = std::max(rNear, rFar);
            radius = std::ceil(radius * 16.0f) / 16.0f;
            const glm::vec3 center = glm::vec3(toWorld * glm::vec4(0.0f, 0.0f, -zc, 1.0f));

            if (c < std::clamp(s.SunCacheFrom, 0, 4)) {
                ctx.SunCache[c].Valid = false;
                views.push_back(FitSunView(sunDir, center, radius, s.SunDistance, sunSize, c));
                ctx.SunCascadesDrawn++;
            } else {
                // A cached cascade: fitted 15% bigger and kept while the slice's sphere stays inside it
                auto& cc = ctx.SunCache[c];
                const float R = std::ceil(radius * 1.15f * 16.0f) / 16.0f;
                const bool fits = cc.Valid && cc.Dir == sunDir && cc.Radius == R && cc.Back == s.SunDistance && cc.Size == sunSize &&
                                  glm::length(center - cc.Center) + radius <= R;
                bool draw = true;
                if (fits) {
                    const uint32_t every = (uint32_t)std::max(s.SunCacheInterval, 1);
                    draw = cc.MovedNear || (frame % every) == ((uint32_t)c % every);
                } else {
                    const ShadowView fresh = FitSunView(sunDir, center, R, s.SunDistance, sunSize, c);
                    cc.ViewProj = fresh.ViewProj;
                    cc.TexelWorld = fresh.TexelWorld;
                    cc.DepthRange = fresh.DepthRange;
                    cc.Center = center;
                    cc.Dir = sunDir;
                    cc.Radius = R;
                    cc.Back = s.SunDistance;
                    cc.Size = sunSize;
                    cc.Valid = true;
                }
                if (draw) { cc.MovedNear = false; ctx.SunCascadesDrawn++; }
                // Two views: the still casters into the static layer (only when due), the moving ones over a copy of it
                ShadowView v;
                v.ViewProj = cc.ViewProj;
                v.Light = -1;
                v.Face = c;
                v.TexelWorld = cc.TexelWorld;
                v.DepthRange = cc.DepthRange;
                v.SunPass = 1;
                v.Cached = !draw;
                views.push_back(v);
                v.SunPass = 2;
                v.Cached = false;
                views.push_back(v);
            }
            sliceNear = sliceFar;
        }
        list.CascadeCount = count;
        return views;
    }

    // ---------------------------------------------------------------- static mesh merging
    namespace {
        constexpr float kStaticCell = 8.0f;        // merged meshes are split into cells this big (metres), for culling

        // What a merged mesh shares: textures, alpha settings, the per-object material values and a cell
        struct StaticKey {
            const Texture2D* Albedo = nullptr; const Texture2D* Normal = nullptr;
            const Texture2D* MetalRough = nullptr; const Texture2D* Emissive = nullptr;
            int Mode = 0; float Cutoff = 0.5f;
            glm::vec3 Color{ 1.0f }; float Alpha = 1.0f, Metallic = 0.0f, Roughness = 0.5f;
            int Specular = 0;                // SpecularCode
            glm::vec3 EmissiveColor{ 0.0f };
            int Unlit = 0, CastShadows = 1, DoubleSided = 0;
            float NormalStrength = 1.0f;
            const SGBinding* Graph = nullptr;
            glm::ivec3 Cell{ 0 };
            auto Tie() const {
                return std::tie(Albedo, Normal, MetalRough, Emissive, Mode, Cutoff, Color.x, Color.y, Color.z, Alpha,
                    Metallic, Roughness, EmissiveColor.x, EmissiveColor.y, EmissiveColor.z, Unlit, CastShadows, DoubleSided, Specular, NormalStrength, Graph,
                    Cell.x, Cell.y, Cell.z);
            }
            bool operator<(const StaticKey& o) const { return Tie() < o.Tie(); }
        };
    }

    void Scene::ClearStaticBatches() {
        m_StaticBatches.clear();
        m_StaticActive = false;
        m_StaticDirty = false;
        m_StaticMerged = 0;
        for (auto [e, mr] : m_Registry.view<MeshRendererComponent>().each()) mr.Merged = false;
    }

    void Scene::OnMeshRendererGone(entt::registry& reg, entt::entity e) {
        if (m_StaticActive && reg.get<MeshRendererComponent>(e).Merged) m_StaticDirty = true;
    }

    void Scene::BuildStaticBatches() {
        ClearStaticBatches();
        m_StaticActive = true;

        struct Group { std::vector<Vertex> Vertices; std::vector<uint32_t> Indices; };
        std::map<StaticKey, Group> groups;
        std::vector<Vertex> partV;
        std::vector<uint32_t> partI;

        // Only unique meshes are merged. Copies of one mesh are already a single instanced draw, and each
        // copy is still culled on its own, while a merged chunk goes into every light view it touches.
        // Measured: 2500 static cubes and spheres took 4.3 ms a frame instanced against 5.0 ms merged
        // (8 m cells) or 5.5 ms (16 m), and 6 copies of one model made 59 draws merged against 39.
        std::map<const Renderable*, uint32_t> uses;
        for (auto [e, mr] : m_Registry.view<MeshRendererComponent>().each())
            if (mr.Static && mr.Geometry) uses[mr.Geometry.get()]++;

        struct Part { StaticKey Key; std::vector<Vertex> V; std::vector<uint32_t> I; };
        struct Candidate { MeshRendererComponent* MR; std::vector<Part> Parts; bool Keep = true; };
        std::vector<Candidate> candidates;

        for (auto [e, mr] : m_Registry.view<MeshRendererComponent>().each()) {
            if (!mr.Static || !mr.Geometry || GetBonePalette(e, mr) || mr.Wind != 0.0f) continue;   // swaying parts stay apart
            if (!mr.Lods.empty() || mr.CullDistance > 0.0f) continue;    // levels of detail are picked per frame, by distance
            if (uses[mr.Geometry.get()] > 1) continue;                   // repeated: instancing handles it better
            const uint32_t parts = PartCount(mr, mr.Geometry.get());
            const bool split = SplitParts(mr) && mr.Geometry->SubmeshCount() > 1;   // drawn submesh by submesh

            // All or nothing: an entity with a see-through part (sorted per object) is drawn on its own
            bool mergeable = true;
            for (uint32_t i = 0; i < parts && mergeable; i++) mergeable = GetSurfaceAlpha(mr, i).Mode != AlphaMode::Blend;
            if (!mergeable) continue;

            const glm::mat4 world = GetWorldMatrix(e) * mr.LocalOffset;
            const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
            const bool mirrored = glm::determinant(glm::mat3(world)) < 0.0f;   // flip winding to keep faces outward
            glm::vec3 mn, mx;
            mr.Geometry->GetLocalBounds(mn, mx);
            const glm::vec3 center = glm::vec3(world * glm::vec4((mn + mx) * 0.5f, 1.0f));

            // Collect every part first, so a part without CPU geometry leaves the entity unmerged
            std::vector<Part> collected;
            for (uint32_t i = 0; i < parts && mergeable; i++) {
                const Material* mat = PartMaterial(mr, mr.Geometry.get(), i);
                partV.clear(); partI.clear();
                if (!mr.Geometry->GetPartVertices(split ? (int32_t)i : -1, partV, partI) || partI.empty()) { mergeable = false; break; }
                const SurfaceAlpha s = GetSurfaceAlpha(mr, i);

                Part p;
                p.Key.Albedo = s.Albedo;
                const PartLook look = LookOf(mr, *mat);
                p.Key.Normal = mat->Normal.get();
                p.Key.MetalRough = mat->MetalRough.get();
                p.Key.Emissive = mat->Emissive.get();
                p.Key.Mode = (int)s.Mode;
                p.Key.Cutoff = s.Mode == AlphaMode::Cutout ? s.Cutoff : 0.5f;
                p.Key.Color = look.Color;
                p.Key.Alpha = s.Alpha;
                p.Key.Metallic = look.Metallic;
                p.Key.Roughness = look.Roughness;
                p.Key.Specular = SpecularCode(mat);
                p.Key.EmissiveColor = look.Emissive;
                p.Key.Unlit = mat->Unlit ? 1 : 0;
                p.Key.CastShadows = (mr.CastShadows && mat->CastShadows) ? 1 : 0;
                p.Key.DoubleSided = s.DoubleSided ? 1 : 0;
                p.Key.NormalStrength = mat->NormalStrength;
                p.Key.Graph = mat->Graph.get();
                p.Key.Cell = glm::ivec3(glm::floor(center / kStaticCell));
                const glm::vec2 uvScale = mat ? mat->UVScale : glm::vec2(1.0f), uvOffset = mat ? mat->UVOffset : glm::vec2(0.0f);
                p.V.reserve(partV.size());
                for (const Vertex& v : partV) {
                    Vertex w = v;
                    w.Position = glm::vec3(world * glm::vec4(v.Position, 1.0f));
                    w.Normal = glm::normalize(normalMatrix * v.Normal);
                    w.TexCoord = v.TexCoord * uvScale + uvOffset;   // the material's placement, baked in
                    p.V.push_back(w);
                }
                p.I = partI;
                if (mirrored)
                    for (size_t t = 0; t + 2 < p.I.size(); t += 3) std::swap(p.I[t + 1], p.I[t + 2]);
                collected.push_back(std::move(p));
            }
            if (!mergeable) continue;
            candidates.push_back({ &mr, std::move(collected) });
        }

        // A group needs at least two objects: one alone gains nothing and loses click-picking. Dropping an
        // object can leave another one alone, so repeat until nothing changes.
        for (bool changed = true; changed;) {
            changed = false;
            std::map<StaticKey, uint32_t> members;
            for (const auto& c : candidates)
                if (c.Keep) for (const auto& p : c.Parts) members[p.Key]++;
            for (auto& c : candidates) {
                if (!c.Keep) continue;
                for (const auto& p : c.Parts)
                    if (members[p.Key] < 2) { c.Keep = false; changed = true; break; }
            }
        }

        for (auto& c : candidates) {
            if (!c.Keep) continue;
            for (auto& p : c.Parts) {
                Group& g = groups[p.Key];
                const uint32_t base = (uint32_t)g.Vertices.size();
                g.Vertices.insert(g.Vertices.end(), p.V.begin(), p.V.end());
                for (uint32_t i : p.I) g.Indices.push_back(base + i);
            }
            c.MR->Merged = true;
            m_StaticMerged++;
        }

        for (auto& [key, g] : groups) {
            StaticBatch sb;
            sb.Geometry = std::make_shared<Mesh>(g.Vertices, g.Indices);
            sb.Geometry->GetLocalBounds(sb.Min, sb.Max);
            sb.CastShadows = key.CastShadows != 0 && key.Unlit == 0;
            sb.State.Geometry = sb.Geometry.get();
            sb.State.Part = -1;
            sb.State.Albedo = key.Albedo;
            sb.State.Normal = key.Normal;
            sb.State.MetalRough = key.MetalRough;
            sb.State.Emissive = key.Emissive;
            sb.State.Mode = (AlphaMode)key.Mode;
            sb.State.Cutoff = key.Cutoff;
            sb.State.DoubleSided = key.DoubleSided != 0;
            sb.State.Graph = key.Graph;
            sb.Instance.Color = glm::vec4(key.Color, key.Alpha);
            sb.Instance.Surface = glm::vec4(key.Metallic, key.Roughness, 0.0f, 0.0f);
            sb.Instance.Emissive = glm::vec4(key.EmissiveColor, 0.0f);
            sb.Instance.Info = glm::ivec4(-1, key.Unlit, key.Specular, 0);   // no single entity: click-picking finds nothing
            sb.Instance.Normal[2].w = 1.0f - key.NormalStrength;              // SetSurfaceExtras (the UVs are baked in)
            m_StaticBatches.push_back(std::move(sb));
        }
    }

    bool AssignModelLods(MeshRendererComponent& mr, const Model& model) {
        if (model.LodCount() == 0) return false;
        glm::vec3 mn, mx;
        model.GetLocalBounds(mn, mx);
        const float scale = glm::length(glm::vec3(mr.LocalOffset[0]));         // the placement's (uniform) scale
        const float size = std::max(glm::length(mx - mn) * scale, 0.1f);       // across the box, in entity units
        mr.Lods.clear();
        float last = 0.0f;
        for (uint32_t k = 1; k <= model.LodCount(); k++) {
            MeshLod l;
            l.Geometry = model.Lod(k);
            // Screen share f of a 60 degree view: an object `size` across fills it at size / (2 tan 30 * f)
            const float share = 0.4f * std::pow(0.5f, (float)(k - 1));
            const float hint = model.LodDistance(k) * scale;
            l.Distance = hint > 0.0f ? hint : std::round(size * 0.866f / share * 2.0f) * 0.5f;
            l.Distance = std::max(l.Distance, last + 0.5f);                    // always further than the level before
            last = l.Distance;
            mr.Lods.push_back(std::move(l));
        }
        mr.LodLevel = 0;
        return true;
    }

    // The level of detail for a distance (already scaled to a 60 degree lens and the scene's scale): 0 = Geometry,
    // i = Lods[i - 1], Lods.size() + 1 = beyond CullDistance. Going back to a finer level waits until the object is 5%
    // nearer than the switch distance, so one standing right at it doesn't flip between two levels. cullDistance is the
    // nearer of the part's own CullDistance and the camera's small-object distance (0 = none), in the same scaled units.
    static uint8_t PickLodLevel(const MeshRendererComponent& mr, float distance, float cullDistance) {
        const size_t count = std::min<size_t>(mr.Lods.size(), 254);
        const uint8_t culled = (uint8_t)(count + 1);
        auto from = [&](uint8_t level) {              // the distance a level starts at
            return level == culled ? cullDistance : mr.Lods[level - 1].Distance;
        };
        uint8_t level = 0;
        for (size_t i = 0; i < count; i++) if (distance >= mr.Lods[i].Distance) level = (uint8_t)(i + 1);
        if (cullDistance > 0.0f && distance >= cullDistance) level = culled;
        const uint8_t prev = mr.LodLevel;
        if (prev > level && prev <= culled && (prev != culled || cullDistance > 0.0f) && distance >= from(prev) * 0.95f)
            level = prev;
        return level;
    }

    void Scene::BuildDrawList(DrawList& list, const Camera& camera, const ShadowContext* shadows) {
        const ShadowSettings* shadowSettings = (shadows && shadows->Settings.Enabled) ? &shadows->Settings : nullptr;
        const uint32_t sunSize = (shadows && shadows->Sun) ? shadows->Sun->GetSize() : 2048u;
        list.Clear();
        const Frustum frustum(camera.GetViewProjection());

        std::vector<ShadowTileGpu> atlasTiles;
        GatherPointLights(*this, camera, [&](const glm::vec3& c, const glm::vec3& e) { return frustum.Visible(c, e); },
            shadowSettings ? shadows : nullptr, list.Lights, &atlasTiles);
        list.Clusters.SetShadowTiles(std::move(atlasTiles));
        for (const PointLightData& l : list.Lights) if (l.Shape != 0) { list.AreaLights = true; break; }
        const auto& lights = list.Lights;
        // Shadow views: a frustum each, plus the light's reach as a sphere for a quick first test
        { PROFILE_SCOPE("Shadow views"); list.Views = shadowSettings ? GatherShadowViews(*this, *shadows, lights, camera, sunSize, list) : std::vector<ShadowView>{}; }
        const auto& views = list.Views;
        {   // stats: sun cascades in use and redrawn (the biggest view: not the previews)
            uint32_t sun = 0, drawn = 0;
            for (const ShadowView& v : views) if (v.Light < 0 && v.SunPass != 2) { sun++; if (!v.Cached) drawn++; }
            if (sun >= RenderStats::SunCascades) { RenderStats::SunCascades = sun; RenderStats::SunCascadesDrawn = drawn; }
        }
        std::vector<Frustum> viewFrusta;
        viewFrusta.reserve(views.size());
        for (const auto& v : views) viewFrusta.emplace_back(v.ViewProj);
        list.ShadowViews.resize(views.size());

        static thread_local std::vector<DrawList::Item> opaque, transparent;       // reused each frame
        static thread_local std::vector<std::vector<DrawList::Item>> shadow;
        opaque.clear(); transparent.clear();
        shadow.resize(views.size());
        for (auto& v : shadow) v.clear();
        std::vector<uint8_t> inView(views.size());

        // Something that casts shadows moved (or animates, or changed level) at centre / radius: the shadow maps of the
        // lights it is in reach of are redrawn next frame (cubes: PointFar; spot maps: the spot's range)
        std::vector<std::pair<int, Frustum>> sunCacheFrusta;       // the cached sun cascades' views (moved things redraw them)
        if (shadows)
            for (int c = 0; c < 4; c++)
                if (shadows->SunCache[c].Valid) sunCacheFrusta.emplace_back(c, Frustum(shadows->SunCache[c].ViewProj));
        // The still casters of a cached sun cascade changed there (something started or stopped moving, appeared, or the
        // ground was sculpted): its static layer is drawn again next frame
        auto markSunStill = [&](const glm::vec3& center, float radius) {
            for (auto& [c, fr] : sunCacheFrusta)
                if (!shadows->SunCache[c].MovedNear && fr.Visible(center, glm::vec3(radius))) shadows->SunCache[c].MovedNear = true;
        };
        bool casterMoving = false;                     // the caster classifyShadows is given (set before each call)
        auto markMovedNear = [&](const glm::vec3& center, float radius) {
            for (size_t j = 0; j < lights.size(); j++) {
                const float gap = glm::length(center - lights[j].Pos) - radius;
                const int k = lights[j].AtlasEntry;
                if (k >= 0 && k < (int)shadows->AtlasEntries.size() && gap < shadows->AtlasEntries[(size_t)k].Far)
                    shadows->AtlasEntries[(size_t)k].Cache.MovedNear = true;
            }
        };

        // Which shadow views a caster (a box under a transform) can show up in: fills inView
        auto classifyShadows = [&](const glm::mat4& world, const glm::vec3& mn, const glm::vec3& mx) {
            bool any = false;
            glm::vec3 center, extent;                  // the world box, once for every view
            Frustum::WorldBox(world, mn, mx, center, extent);
            const float radius = glm::length(glm::vec3(world * glm::vec4((mx - mn) * 0.5f, 0.0f)));
            int lastLight = -2;
            bool lightReaches = true;
            for (size_t v = 0; v < views.size(); v++) {
                if (views[v].Light != lastLight) {   // new light: is the caster within its reach at all (a spot: its range)?
                    lastLight = views[v].Light;
                    lightReaches = lastLight < 0 ||
                        glm::length(center - lights[lastLight].Pos) - radius < views[v].Reach;
                }
                // Cached sun cascades: the still pass only when due and only still casters, the overlay only moving ones
                if (views[v].Cached || (views[v].SunPass == 1 && casterMoving) || (views[v].SunPass == 2 && !casterMoving)) { inView[v] = false; continue; }
                inView[v] = lightReaches && viewFrusta[v].Visible(center, extent);
                if (!inView[v]) list.ShadowSkipped++;
                any = any || inView[v];
            }
            return any;
        };

        // Levels of detail: distances are measured as if through a 60 degree lens (a narrower one zooms in: detail
        // stays further out), and divided by the scene's scale instead of multiplying every switch distance
        const float lodDistanceFactor = std::tan(glm::radians(std::clamp(camera.GetFov(), 1.0f, 170.0f)) * 0.5f) /
            std::tan(glm::radians(30.0f)) / std::max(Lod.DistanceScale, 0.01f);
        const float kTan30 = std::tan(glm::radians(30.0f));
        const float smallObjects = std::clamp(camera.CullSmallObjects, 0.0f, 0.5f);

        // Merged static geometry (during Play): one object per batch, in world space
        if (m_StaticActive && m_StaticDirty) BuildStaticBatches();   // a merged object went away
        const glm::mat4 identity(1.0f);
        for (const StaticBatch& sb : m_StaticBatches) {
            if (smallObjects > 0.0f) {          // the camera's small-object culling, for the batch as one object
                const glm::vec3 c = (sb.Min + sb.Max) * 0.5f;
                const float radius = std::max(glm::length(sb.Max - sb.Min) * 0.5f, 0.001f);
                if (glm::length(c - camera.Position) * lodDistanceFactor > radius / (smallObjects * kTan30)) { list.LodCulled++; continue; }
            }
            const bool visible = frustum.Visible(identity, sb.Min, sb.Max);
            if (!visible) list.Culled++;
            casterMoving = false;
            const bool anyShadow = !views.empty() && sb.CastShadows && classifyShadows(identity, sb.Min, sb.Max);
            if (!visible && !anyShadow) continue;

            DrawList::Item item;
            item.Instance = list.AddInstance(sb.Instance);
            item.State = sb.State;
            item.BoxCenter = (sb.Min + sb.Max) * 0.5f;
            item.BoxExtent = (sb.Max - sb.Min) * 0.5f;
            if (anyShadow) {
                DrawList::Item sh = item;
                sh.State.Normal = sh.State.MetalRough = sh.State.Emissive = nullptr;
                if (sh.State.Graph && !sh.State.Graph->WritesAlpha && !sh.State.Graph->HasVertexOffset) sh.State.Graph = nullptr;
                if (sb.State.Mode == AlphaMode::Opaque) { sh.State.Albedo = nullptr; sh.State.Cutoff = 0.5f; }
                for (size_t v = 0; v < views.size(); v++) if (inView[v]) shadow[v].push_back(sh);
            }
            if (visible) opaque.push_back(item);
        }

        PROFILE_SCOPE("Objects");
        static thread_local std::vector<DrawList::Item> selected;   // highlighted parts (editor outline)
        selected.clear();
        const std::vector<uint32_t>* highlight = (list.Highlight && !list.Highlight->empty()) ? list.Highlight : nullptr;
        static const glm::vec3 kLodTint[] = { { 0.35f, 1.0f, 0.35f }, { 1.0f, 1.0f, 0.25f }, { 1.0f, 0.55f, 0.15f },
                                              { 1.0f, 0.2f, 0.2f }, { 0.9f, 0.3f, 1.0f } };
        auto drawables = m_Registry.view<TransformComponent, MeshRendererComponent>();
        for (auto entity : drawables) {
            auto& mr = drawables.get<MeshRendererComponent>(entity);
            const bool highlighted = highlight &&
                std::binary_search(highlight->begin(), highlight->end(), (uint32_t)entt::to_integral(entity));
            // merged: drawn by its static batch (only its outline is drawn from here); hidden: e.visible = false
            if (!mr.Geometry || mr.Hidden || (mr.Merged && !highlighted) || IsEditorHidden(entity)) continue;

            static const glm::mat4 kIdentity(1.0f);
            const glm::mat4 world = mr.LocalOffset == kIdentity ? GetWorldMatrix(entity) : GetWorldMatrix(entity) * mr.LocalOffset;
            const auto* bones = GetBonePalette(entity, mr);
            glm::vec3 mn, mx;
            mr.Geometry->GetLocalBounds(mn, mx);
            if (bones) {                        // the bounds are the rest pose: leave room for animation
                const glm::vec3 c = (mn + mx) * 0.5f, h = (mx - mn) * 0.75f;
                mn = c - h; mx = c + h;
            }

            // Level of detail, from the camera's distance to the middle of the main geometry's box. Every view (camera
            // and shadows) draws the same level, so an object never shadows itself with a different shape.
            uint8_t level = 0;
            const Renderable* geometry = mr.Geometry.get();
            const uint8_t culledLevel = (uint8_t)std::min<size_t>(mr.Lods.size() + 1, 255);
            // Small objects (the camera's CullSmallObjects): a bounding sphere of radius r covers r / (d tan(fov/2)) of
            // the view height, so it drops below share m beyond d tan(fov/2) = r / m, which is r / (m tan 30) in the
            // scaled distance PickLodLevel takes. Highlighted (selected) parts are kept, so their outline shows.
            float cullDistance = mr.CullDistance;
            if (smallObjects > 0.0f && !highlighted) {
                const float radius = glm::length(glm::vec3(world * glm::vec4((mx - mn) * 0.5f, 0.0f)));
                const float sizeCull = std::max(radius, 0.001f) / (smallObjects * kTan30);
                cullDistance = cullDistance > 0.0f ? std::min(cullDistance, sizeCull) : sizeCull;
            }
            if (!mr.Lods.empty() || cullDistance > 0.0f) {
                const glm::vec3 center = glm::vec3(world * glm::vec4((mn + mx) * 0.5f, 1.0f));
                level = PickLodLevel(mr, glm::length(center - camera.Position) * lodDistanceFactor, cullDistance);
                if (level > 0 && level < culledLevel) {
                    uint8_t k = level;                                   // a level that failed to load: the one before
                    while (k > 0 && !mr.Lods[k - 1].Geometry) k--;
                    if (k > 0) {
                        const Renderable* g = mr.Lods[k - 1].Geometry.get();
                        // Skinned: the level is posed with the main model's bones, so it must have the same ones
                        const Model* m = bones ? dynamic_cast<const Model*>(g) : nullptr;
                        const Model* main = bones ? dynamic_cast<const Model*>(geometry) : nullptr;
                        if (!bones || (m && main && m->IsSkinned() && m->BoneCount() == main->BoneCount())) geometry = g;
                    }
                }
            }
            const bool lodChanged = level != mr.LodLevel;
            mr.LodLevel = level;
            const bool lodCulled = level > 0 && level == culledLevel;

            const bool visible = !lodCulled && frustum.Visible(world, mn, mx);
            if (!visible && !lodCulled) list.Culled++;

            // Where it was last frame (motion vectors), recorded for every part, drawn or not, so movement is seen
            // even while it is off screen. A shadow-casting part that moved (or is animated) near a point light makes
            // that light re-render its cube next frame, whatever its update interval (wind sway doesn't count).
            glm::mat4 prevWorld = world;
            bool isNew = true, wasMoving = false;
            const uint32_t moveKey = (uint32_t)entt::to_integral(entity);
            {
                const auto prev = list.PrevWorld.find(moveKey);
                if (prev != list.PrevWorld.end()) { prevWorld = prev->second; isNew = false; }
                list.CurWorld[moveKey] = world;
                wasMoving = list.PrevMoving.count(moveKey) != 0;
            }
            // (A new level of detail changes the shadow's shape too.)
            const bool moving = bones || prevWorld != world || lodChanged;
            casterMoving = moving;                           // cached sun cascades: moving casters go in the overlay pass
            if (shadowSettings && mr.CastShadows) {
                if (moving) list.CurMoving.insert(moveKey);
                if (moving || moving != wasMoving || isNew) {
                    glm::vec3 center, extent;
                    Frustum::WorldBox(world, mn, mx, center, extent);
                    const float radius = glm::length(extent);
                    if (moving) markMovedNear(center, radius);
                    // It started or stopped moving, or is new: the cached cascades' still casters changed
                    if (moving != wasMoving || isNew) markSunStill(center, radius);
                }
            }

            if (lodCulled) { list.LodCulled++; continue; }
            if (geometry != mr.Geometry.get()) list.LodReduced++;

            // Which shadow views this caster can show up in (light markers must not block light)
            const bool anyShadow = !mr.Merged && !views.empty() && mr.CastShadows && classifyShadows(world, mn, mx);
            if (!visible && !anyShadow) continue;

            InstanceData base;
            base.Model = world;
            base.PrevModel = prevWorld;
            base.Normal = glm::mat4(glm::transpose(glm::inverse(glm::mat3(world))));
            // Info.w: the drawn geometry carries wind weights per vertex (vegetation COLOR_0): lit.vert / shadow.vert use them
            base.Info = glm::ivec4((int)entt::to_integral(entity), 0, 0, geometry->HasVertexWind() ? 1 : 0);
            // Wind (lit.vert / shadow.vert): its amount, and the mesh's height range (local) that sets how much each vertex moves
            glm::vec3 windMin(0.0f), windMax(1.0f);
            if (mr.Wind != 0.0f) mr.Geometry->GetLocalBounds(windMin, windMax);
            float distance = -1.0f;

            const glm::vec3 lodTint = TintLodLevels ? kLodTint[std::min<size_t>(level, std::size(kLodTint) - 1)] : glm::vec3(1.0f);

            const uint32_t parts = PartCount(mr, geometry);
            bool boxKnown = false;                           // the world box for occlusion tests (opaque parts only)
            glm::vec3 boxCenter(0.0f), boxExtent(-1.0f);
            const bool split = SplitParts(mr) && geometry->SubmeshCount() > 1;   // drawn submesh by submesh
            for (uint32_t i = 0; i < parts; i++) {
                const Material* mat = PartMaterial(mr, geometry, i);
                const SurfaceAlpha s = GetSurfaceAlpha(mr, i, geometry);

                // The material's values, with the script overrides in their place
                const PartLook look = LookOf(mr, *mat);
                InstanceData inst = base;
                inst.Color = glm::vec4(look.Color * lodTint, s.Alpha);
                inst.Surface = glm::vec4(look.Metallic, look.Roughness, mr.Wind, windMax.y - windMin.y);
                glm::vec3 emissive = look.Emissive;
                if (mr.FlashTime > 0.0f && mr.FlashDuration > 0.0f) emissive += mr.FlashColor * (mr.FlashTime / mr.FlashDuration);
                inst.Emissive = glm::vec4(emissive, windMin.y);
                if (mat->Unlit) inst.Info.y = 1;
                SetSurfaceExtras(inst, mat);                       // UV scale / offset, normal strength
                inst.Info.z = SpecularCode(mat);                   // lit.vert: specular = 1 - Info.z / 1000

                DrawList::Item item;
                item.Instance = list.AddInstance(inst);
                DrawBatch& b = item.State;
                b.Geometry = geometry;
                b.Part = split ? (int32_t)i : -1;
                b.Albedo = s.Albedo;
                b.Normal = mat ? mat->Normal.get() : nullptr;
                b.MetalRough = mat ? mat->MetalRough.get() : nullptr;
                b.Emissive = mat ? mat->Emissive.get() : nullptr;
                b.Mode = s.Mode;
                b.Cutoff = s.Cutoff;
                b.DoubleSided = s.DoubleSided;
                b.Bones = bones;
                b.Graph = mat->Graph.get();

                if (anyShadow && (!mat || (mat->CastShadows && !mat->Unlit))) {
                    // Shadows only need the albedo, and only to alpha-test: opaque casters share batches
                    DrawList::Item sh = item;
                    sh.State.Normal = sh.State.MetalRough = sh.State.Emissive = nullptr;
                    if (sh.State.Graph && !sh.State.Graph->WritesAlpha && !sh.State.Graph->HasVertexOffset) sh.State.Graph = nullptr;   // only a graph's alpha matters here
                    if (s.Mode == AlphaMode::Opaque) { sh.State.Albedo = nullptr; sh.State.Cutoff = 0.5f; }
                    for (size_t v = 0; v < views.size(); v++) if (inView[v]) shadow[v].push_back(sh);
                }
                if (!visible) continue;
                if (highlighted) {
                    DrawList::Item sel = item;
                    sel.State.Normal = sel.State.MetalRough = sel.State.Emissive = nullptr;
                    sel.State.Graph = nullptr;
                    if (s.Mode == AlphaMode::Opaque) { sel.State.Albedo = nullptr; sel.State.Cutoff = 0.5f; }
                    selected.push_back(sel);
                }
                if (mr.Merged) continue;
                if (s.Mode == AlphaMode::Blend) {
                    if (distance < 0.0f)
                        distance = glm::length(glm::vec3(world * glm::vec4((mn + mx) * 0.5f, 1.0f)) - camera.Position);
                    item.Distance = distance;
                    transparent.push_back(item);
                }
                else {
                    if (list.WantBoxes && !boxKnown) { Frustum::WorldBox(world, mn, mx, boxCenter, boxExtent); boxKnown = true; }   // once per object
                    item.BoxCenter = boxCenter;
                    item.BoxExtent = boxExtent;
                    opaque.push_back(item);
                }
            }
        }

        // Terrains: every chunk is a part of its own (one draw each, culled on its own) sharing one instance and the
        // terrain's layered material. Changed chunks are rebuilt here (UpdateGPU), so sculpting shows the same frame.
        for (auto [entity, tc] : m_Registry.view<TerrainComponent>().each()) {
            if (!tc.Data || IsEditorHidden(entity)) continue;
            TerrainData& data = *tc.Data;
            data.UpdateGPU(tc.Size);
            TerrainMaterial& m = data.Material;
            m.Splat = data.SplatTexture();
            for (int i = 0; i < TerrainComponent::MaxLayers; i++) {
                const TerrainLayer& L = tc.Layers[i];
                m.Tiling[i] = std::max(L.Tiling, 0.01f);
                m.Variation[i] = std::clamp(L.Variation, 0.0f, 1.0f);
                if (const MaterialAsset* mat = L.LayerMaterial.Asset.get()) {   // a material file gives the look
                    m.Layers[i] = mat->Surface.Albedo.get();
                    m.Color[i] = glm::vec4(mat->Surface.Color, std::clamp(mat->Desc.Roughness, 0.04f, 1.0f));
                    const bool maps = mat->TerrainMaps && mat->TerrainMaps->Texture;
                    m.Surface[i] = maps ? mat->TerrainMaps->Texture.get() : nullptr;
                    m.Maps[i] = maps ? mat->TerrainMaps->Maps : 0;
                    m.NormalStrength[i] = mat->Desc.NormalStrength;
                    continue;
                }
                m.Layers[i] = L.Texture.get();
                m.Color[i] = glm::vec4(L.Color, std::clamp(L.Roughness, 0.04f, 1.0f));
                const bool maps = L.Surface && L.Surface->Texture;
                m.Surface[i] = maps ? L.Surface->Texture.get() : nullptr;
                m.Maps[i] = maps ? L.Surface->Maps : 0;
                m.NormalStrength[i] = L.NormalStrength;
            }
            m.Size = tc.Size;
            m.Blend = std::clamp(tc.Blend, 0.0f, 1.0f);

            const glm::mat4 world = GetWorldMatrix(entity);
            const uint32_t key = (uint32_t)entt::to_integral(entity) | 0x80000000u;   // apart from a Mesh Renderer's entry
            glm::mat4 prevWorld = world;
            if (const auto prev = list.PrevWorld.find(key); prev != list.PrevWorld.end()) prevWorld = prev->second;
            list.CurWorld[key] = world;
            // Sculpted or moved: the point lights near the changed ground draw their shadows again next frame
            const bool changed = prevWorld != world || data.DrawnRevision != data.Revision();
            data.DrawnRevision = data.Revision();
            const bool highlighted = highlight &&
                std::binary_search(highlight->begin(), highlight->end(), (uint32_t)entt::to_integral(entity));

            InstanceData inst;
            inst.Model = world;
            inst.PrevModel = prevWorld;
            inst.Normal = glm::mat4(glm::transpose(glm::inverse(glm::mat3(world))));
            inst.Color = glm::vec4(1.0f);
            inst.Surface = glm::vec4(0.0f, 0.9f, 0.0f, 1.0f);
            inst.Info = glm::ivec4((int)entt::to_integral(entity), 0, 0, 0);
            uint32_t instance = UINT32_MAX;
            uint32_t tinted[TerrainData::MaxLodLevels];       // Debug > LOD Levels: an instance per level, coloured
            std::fill(std::begin(tinted), std::end(tinted), UINT32_MAX);

            // Levels of detail, chosen from the camera in the terrain's own space (TerrainData::SelectLods). The error
            // is measured in pixels of a 1080-pixel-high view through this lens, x the scene's distance scale.
            {
                const glm::vec3 eye = glm::vec3(glm::inverse(world) * glm::vec4(camera.Position, 1.0f));
                const glm::vec3 axisScale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                    glm::length(glm::vec3(world[2])));
                const float pixelsPerMetre = 540.0f / std::tan(glm::radians(std::clamp(camera.GetFov(), 1.0f, 170.0f)) * 0.5f) *
                    std::max(Lod.DistanceScale, 0.01f);
                RenderStats::TerrainLodReduced += data.SelectLods(eye, axisScale, pixelsPerMetre, Lod.TerrainError);
            }

            for (const TerrainData::Chunk& ch : data.Chunks()) {
                if (!ch.Geometry) continue;
                const bool visible = frustum.Visible(world, ch.Min, ch.Max);
                if (!visible) list.Culled++;
                // Sculpted, moved or a new level of detail: the shadows that see it are drawn again
                if ((changed || ch.LevelChanged) && shadowSettings && tc.CastShadows) {
                    glm::vec3 center, extent;
                    Frustum::WorldBox(world, ch.Min, ch.Max, center, extent);
                    const float radius = glm::length(extent);
                    markMovedNear(center, radius);
                    markSunStill(center, radius);
                }
                casterMoving = false;
                const bool anyShadow = !views.empty() && tc.CastShadows && classifyShadows(world, ch.Min, ch.Max);
                if (!visible && !anyShadow) continue;
                if (instance == UINT32_MAX) instance = list.AddInstance(inst);
                uint32_t use = instance;
                if (TintLodLevels) {
                    uint32_t& t = tinted[std::min<int>(ch.Level, TerrainData::MaxLodLevels - 1)];
                    if (t == UINT32_MAX) {
                        InstanceData c = inst;
                        c.Color = glm::vec4(kLodTint[std::min<size_t>(ch.Level, std::size(kLodTint) - 1)], 1.0f);
                        t = list.AddInstance(c);
                    }
                    use = t;
                }

                DrawList::Item item;
                item.Instance = use;
                item.State.Geometry = ch.Geometry.get();
                item.State.Part = ch.Part();               // the level and stitch variant (TerrainChunkMesh)
                if (anyShadow) {
                    DrawList::Item sh = item;                // plain opaque casters
                    for (size_t v = 0; v < views.size(); v++) if (inView[v]) shadow[v].push_back(sh);
                }
                if (!visible) continue;
                if (highlighted) selected.push_back(item);
                item.State.Terrain = &m;
                if (list.WantBoxes) Frustum::WorldBox(world, ch.Min, ch.Max, item.BoxCenter, item.BoxExtent);
                opaque.push_back(item);
            }
        }

        list.PrevWorld.swap(list.CurWorld);               // this frame's matrices are next frame's "previous"
        list.CurWorld.clear();
        list.PrevMoving.swap(list.CurMoving);
        list.CurMoving.clear();
        { PROFILE_SCOPE("Sort into batches");
        list.OpaqueFirst = (uint32_t)list.Indices.size();
        list.AddBatches(opaque, list.Opaque, DrawList::Order::ByState, list.WantBoxes ? &list.OpaqueBoxes : nullptr);
        list.AddBatches(transparent, list.Transparent, DrawList::Order::BackToFront);
        if (!selected.empty()) list.AddBatches(selected, list.Selected, DrawList::Order::ByState);
        for (size_t v = 0; v < views.size(); v++) list.AddBatches(shadow[v], list.ShadowViews[v], DrawList::Order::ByState);
        }
        {   // Point lights without a shadow slot into the camera's clusters
            PROFILE_SCOPE("Light clusters");
            list.Clusters.Build(list.Lights, camera, GpuLightBinning);
            uint32_t direct = 0;                       // shadow-slot lights: shaded from uniforms, counted as seen
            for (const PointLightData& l : list.Lights) if (l.Shadows) direct++;
            RenderStats::Lights = std::max(RenderStats::Lights, list.Clusters.LightCount() + direct);   // the biggest view (not the previews)
            RenderStats::LightsVisible = std::max(RenderStats::LightsVisible, list.Clusters.VisibleLights() + direct);
            RenderStats::LightsPerCluster = std::max(RenderStats::LightsPerCluster, list.Clusters.MaxPerCluster());
            uint32_t atlas = 0, smallest = 0, largest = 0;
            for (const PointLightData& l : list.Lights) {
                if (l.AtlasEntry < 0 || !shadows) continue;
                const uint32_t t = shadows->AtlasEntries[(size_t)l.AtlasEntry].Tile;
                atlas++;
                smallest = smallest ? std::min(smallest, t) : t;
                largest = std::max(largest, t);
            }
            if (atlas >= RenderStats::ShadowedLights) {              // the biggest view again (every shadow is in the atlas)
                RenderStats::ShadowedLights = atlas;
                RenderStats::AtlasLights = atlas;
                RenderStats::AtlasSmallest = smallest;
                RenderStats::AtlasLargest = largest;
                if (shadows && shadows->Atlas)
                    RenderStats::AtlasUsed = (uint32_t)(shadows->Atlas->UsedArea() * 1000 / ((uint64_t)shadows->Atlas->Size() * shadows->Atlas->Size()));
            }
        }
        { PROFILE_SCOPE("Upload"); list.Upload(); }
        RenderStats::Culled += list.Culled;
        RenderStats::ShadowSkipped += list.ShadowSkipped;
        RenderStats::LodReduced += list.LodReduced;
        RenderStats::LodCulled += list.LodCulled;
        RenderStats::StaticBatches = (uint32_t)m_StaticBatches.size();
        RenderStats::StaticMerged = m_StaticMerged;
    }

    // ---------------------------------------------------------------- shadows
    void Scene::RenderShadows(ShadowContext& ctx, const DrawList& list) {
        const ShadowSettings& s = ctx.Settings;
        if (!s.Enabled) return;
        const auto& lights = list.Lights;
        const auto& views = list.Views;                         // fitted by BuildDrawList
        if (views.size() != list.ShadowViews.size()) return;   // the list was built without shadows
        list.Bind();

        auto drawCasters = [&](Shader& base, const std::vector<DrawBatch>& batches) {
            base.SetInt("u_Texture", 0);
            CullSwitch cull;
            const SGBinding* graph = nullptr;
            Shader* cur = &base;
            for (const DrawBatch& b : batches) {
                // A shader graph that writes alpha cuts its shadow with its own copy of the depth shader
                if (b.Graph != graph) {
                    graph = b.Graph;
                    Shader* next = &base;
                    if (graph && graph->Program && base.GraphVariant >= 0 && (b.Mode != AlphaMode::Opaque || graph->HasVertexOffset))
                        if (Shader* g = graph->Program->Get(base.GraphVariant)) { base.ReplayInto(*g); next = g; }
                    if (next != cur) { next->Bind(); cur = next; }
                    if (graph && cur != &base) graph->Apply(*cur);
                }
                Shader& sh = *cur;
                cull.Set(b.DoubleSided);                // a double-sided plane casts whichever side faces the light
                // Cutout casts its cut shape; see-through (Blend) casts only where alpha >= 0.5
                const bool test = b.Mode != AlphaMode::Opaque;
                sh.SetInt("u_AlphaTest", test ? 1 : 0);
                if (test) {
                    sh.SetFloat("u_AlphaCutoff", b.Mode == AlphaMode::Cutout ? b.Cutoff : 0.5f);
                    sh.SetInt("u_UseTexture", b.Albedo ? 1 : 0);
                    if (b.Albedo) b.Albedo->Bind(0);
                }
                SetBones(sh, b.Bones);
                sh.SetInt("u_InstanceBase", (int)b.First);
                DrawBatchGeometry(b);
            }
        };

        bool inAtlas = false;                    // the atlas's scissor is on
        bool stillDrawn[4] = { false, false, false, false };   // a cached cascade's static layer was drawn this frame
        for (size_t v = 0; v < views.size(); v++) {
            const ShadowView& view = views[v];
            if (view.Cached) continue;                  // a sun cascade kept from an earlier frame
            if (inAtlas && view.AtlasTile == 0) { ctx.Atlas->EndTiles(); inAtlas = false; }
            if (view.AtlasTile > 0) {
                // A square of the shadow atlas (a spot's view, or a cube face): the cubes' distance shader, the
                // light's far plane; the scissor keeps the clear and the drawing inside the square
                if (!ctx.Atlas || !ctx.PointDepth) continue;
                Shader& sh = *ctx.PointDepth;
                sh.Bind();
                SetWindUniforms(sh);
                sh.SetFloat("u_FarPlane", view.Reach);
                sh.SetVec3("u_LightPos", lights[view.Light].Pos);
                ctx.Atlas->BindTile(view.AtlasAt, view.AtlasTile);
                inAtlas = true;
                sh.SetMat4("u_LightVP", view.ViewProj);
                drawCasters(sh, list.ShadowViews[v]);
            }
            else if (view.Light < 0) {
                if (!ctx.Sun || !ctx.SunDepth) continue;
                Shader& sh = *ctx.SunDepth;
                sh.Bind();
                SetWindUniforms(sh);
                const uint32_t face = (uint32_t)view.Face;
                if (view.SunPass == 1) {                         // a cached cascade's still casters, into its static layer
                    ctx.Sun->BindStatic(face);
                    if (face < 4) stillDrawn[face] = true;
                } else if (view.SunPass == 2) {                  // ... then the live layer: that copy plus the moving casters
                    auto& cc = ctx.SunCache[face < 4 ? face : 0];
                    const bool movers = !list.ShadowViews[v].empty();
                    if (!movers && !(face < 4 && stillDrawn[face]) && !cc.LiveDirty) continue;   // the live layer is still right
                    ctx.Sun->RestoreStatic(face);
                    cc.LiveDirty = movers;
                } else ctx.Sun->Bind(face);                      // this cascade's layer
                sh.SetMat4("u_LightVP", view.ViewProj);
                drawCasters(sh, list.ShadowViews[v]);
            }
        }
        if (inAtlas) ctx.Atlas->EndTiles();
    }

    // ---------------------------------------------------------------- render system
    void Scene::Render(const Camera& camera, Shader& shader, const DrawList& list, const ShadowContext* ctx,
        const Environment* env, RenderPass pass) {
        SetLightingUniforms(camera, shader, list, ctx, env);
        DrawListBatches(shader, list, pass);
    }

    void Scene::SetWindUniforms(Shader& shader) const {
        const float a = glm::radians(Wind.Direction);
        shader.SetFloat("u_SceneWindTime", WindTime);
        shader.SetVec2("u_SceneWindDir", glm::vec2(std::cos(a), std::sin(a)));
        shader.SetFloat("u_SceneWindSpeed", Wind.Speed);
        shader.SetFloat("u_SceneWindStrength", Wind.Strength);
    }

    void Scene::SetLightingUniforms(const Camera& camera, Shader& shader, const DrawList& list, const ShadowContext* ctx,
        const Environment* env) {

        glm::vec3 sunDir(0.0f, -1.0f, 0.0f), sunColor(0.0f);
        bool sunShadows = false;
        const bool hasSun = FindSun(*this, sunDir, sunColor, sunShadows);

        const bool shadowsOn = ctx && ctx->Settings.Enabled;

        shader.Bind();
        shader.SetMat4("u_ViewProjection", camera.GetViewProjection());
        shader.SetVec3("u_ViewPos", camera.Position);
        SetWindUniforms(shader);

        // Every sampler gets its own unit, even when unused (two sampler types on one unit is an error)
        shader.SetInt("u_Texture", 0);
        shader.SetInt("u_SunShadowMap", 2);
        shader.SetInt("u_NormalMap", 3);
        shader.SetInt("u_MetalRoughMap", 4);
        shader.SetInt("u_EmissiveMap", 5);
        shader.SetInt("u_TerrainSplat", 9);                 // terrain weights and layers (BindTerrain); 8 is the grass's
        for (int i = 0; i < 4; i++) shader.SetInt("u_TerrainLayer" + std::to_string(i), 10 + i);
        for (int i = 0; i < 4; i++) shader.SetInt("u_TerrainSurface" + std::to_string(i), 14 + i);   // packed maps
        shader.SetFloat("u_Ambient", AmbientIntensity);
        // Environment maps (units 6 and 7). The samplers always point at their own unit.
        shader.SetInt("u_IrradianceMap", 6);
        shader.SetInt("u_PrefilterMap", 7);
        const bool ibl = env && env->IsReady();
        shader.SetInt("u_UseIBL", ibl ? 1 : 0);
        if (ibl) {
            env->BindLighting(6, 7);
            shader.SetFloat("u_PrefilterMaxLod", env->PrefilterMaxLod());
            shader.SetFloat("u_EnvRot", glm::radians(env->RotationDegrees));
        }

        // Point lights, built with the draw list. The direct slots (at most MaxDirect, the first entries of the list:
        // the most useful shadowed lights) are shaded from uniforms; every other light through the clusters (lights.glsl).
        // Shadows of both come from the atlas: u_PointShadow = (spot record, first cube record) as PointLightGpu.Shadow.xy
        int direct = 0;
        while (direct < (int)list.Lights.size() && direct < LightClusters::MaxDirect &&
               (list.Lights[direct].Shadows || list.Lights[direct].Entity == ~0u)) direct++;   // slots and placeholders
        shader.SetInt("u_PointCount", direct);
        for (int i = 0; i < direct; i++) {
            const std::string idx = "[" + std::to_string(i) + "]";
            const PointLightData& l = list.Lights[i];
            shader.SetVec4("u_PointPosRange" + idx, glm::vec4(l.Pos, l.Range));
            shader.SetVec4("u_PointColor" + idx, glm::vec4(l.Color, l.CosInner));
            shader.SetVec4("u_PointSpot" + idx, glm::vec4(l.Dir, l.CosOuter));
            shader.SetIVec4("u_PointShadow" + idx, glm::ivec4(l.AtlasSpot, l.AtlasCube, 0, 0));
            shader.SetVec4("u_PointExtra" + idx, glm::vec4(l.Radius, l.Contact ? 1.0f : 0.0f, (float)l.Shape, l.Extent));
            shader.SetVec4("u_PointArea" + idx, glm::vec4(l.AreaX, l.AreaHalfY));
            shader.SetVec4("u_PointCookie" + idx, list.Clusters.DirectCookie(i));
            shader.SetVec4("u_PointFrames" + idx, list.Clusters.DirectFrames(i));
        }
        list.Clusters.Bind();
        shader.SetInt("u_ClusterLightCount", (int)list.Clusters.LightCount());
        shader.SetVec2("u_ClusterZ", glm::vec2(list.Clusters.ZScale(), list.Clusters.ZBias()));
        shader.SetVec4("u_ClusterViewRow", list.Clusters.ViewRow());
        shader.SetInt("u_ClusterDebug", ClusterDebug ? 1 : 0);
        // The shadow atlas (unit 18; clustered lights carry the records of their squares, binding 9)
        shader.SetInt("u_LightCookies", 20);           // cookies and profiles, bound by list.Clusters.Bind
        shader.SetInt("u_ShadowAtlas", 18);
        shader.SetInt("u_ShadowAtlasRaw", 19);
        // Contact shadows: the pre-pass depth on unit 1 and the (jittered) camera it was drawn with
        shader.SetInt("u_ContactDepth", 1);
        const bool contact = shadowsOn && ctx->ContactDepth != 0 && ctx->Settings.ContactLength > 0.0f;
        shader.SetFloat("u_ContactLength", contact ? ctx->Settings.ContactLength : 0.0f);
        shader.SetInt("u_SunContact", contact && ctx->Settings.SunContact ? 1 : 0);
        shader.SetMat4("u_ContactVP", camera.GetViewProjection());      // also AO's view depth
        if (contact) {
#ifndef __EMSCRIPTEN__
            glBindTextureUnit(1, ctx->ContactDepth);
#endif
            shader.SetVec2("u_ContactNearFar", glm::vec2(camera.GetNear(), camera.GetFar()));
        }
        // Ambient occlusion on the ambient light only (unit 21), when SceneRenderer computed it before this pass
        shader.SetInt("u_AOMap", 21);
        const bool ao = ctx && ctx->AOMap != 0;
        shader.SetInt("u_AOOn", ao ? 1 : 0);
        if (ao) {
#ifndef __EMSCRIPTEN__
            glBindTextureUnit(21, ctx->AOMap);
#endif
            shader.SetFloat("u_AOStrength", ctx->AOStrength);
            shader.SetVec2("u_AOScreen", ctx->ScreenSize);
        }
        shader.SetFloat("u_ShadowFrame", ctx ? (float)(ctx->Frame % 64u) : 0.0f);
        const bool atlasShadows = shadowsOn && ctx->Atlas && ctx->Atlas->Allocated();
        shader.SetInt("u_AtlasOn", atlasShadows ? 1 : 0);
        if (atlasShadows) {
            ctx->Atlas->BindTexture(18, 19);
            shader.SetFloat("u_AtlasBias", ctx->Settings.PointBias);
            shader.SetFloat("u_AtlasTexel", 1.0f / (float)ctx->Atlas->Size());
        }

        // Sun
        shader.SetInt("u_HasSun", hasSun ? 1 : 0);
        if (hasSun) {
            shader.SetVec3("u_SunDir", sunDir);
            shader.SetVec3("u_SunColor", sunColor);

            // The cascades were fitted by BuildDrawList; the shader picks one by view depth
            const bool useSunShadow = shadowsOn && ctx->Sun && sunShadows && list.CascadeCount > 0;
            shader.SetInt("u_SunShadow", useSunShadow ? 1 : 0);
            if (useSunShadow) {
                ctx->Sun->BindTexture(2);
                shader.SetInt("u_CascadeCount", list.CascadeCount);
                for (int c = 0; c < 4; c++) shader.SetFloat("u_CascadeSplits[" + std::to_string(c) + "]", list.CascadeSplits[c]);
                shader.SetVec4("u_ViewRow", glm::row(camera.GetView(), 2));
                shader.SetInt("u_CascadeDebug", ctx->Settings.CascadeDebug ? 1 : 0);
                const float biasScale = ctx->Settings.SunBias / 0.002f;
                for (const ShadowView& v : list.Views) {
                    if (v.Light >= 0) continue;
                    const std::string idx = "[" + std::to_string(v.Face) + "]";
                    shader.SetMat4("u_SunVP" + idx, v.ViewProj);
                    shader.SetFloat("u_SunTexel" + idx, v.TexelWorld);
                    // Depth bias: a few texels of the cascade, in that cascade's depth units
                    shader.SetFloat("u_SunBias" + idx, biasScale * kSunBiasTexels * v.TexelWorld / v.DepthRange);
                }
            }
        }
    }

    void Scene::DrawListBatches(Shader& shader, const DrawList& list, RenderPass pass) {
        // Each batch: its textures and alpha settings, then one instanced call for all its objects
        list.Bind();
        auto drawBatches = [&](const std::vector<DrawBatch>& batches) {
            CullSwitch cull;
            const TerrainMaterial* terrain = nullptr;
            const SGBinding* graph = nullptr;
            Shader* cur = &shader;                   // a shader graph batch swaps in the graph's program (batches are sorted by graph)
            shader.SetInt("u_Terrain", 0);
            for (const DrawBatch& b : batches) {
                if (b.Graph != graph) {
                    graph = b.Graph;
                    Shader* next = &shader;
                    // The depth pre-pass needs the graph only for its alpha; the lit pass always
                    const bool wanted = graph && graph->Program && shader.GraphVariant >= 0 &&
                        (shader.GraphVariant == 0 || graph->HasVertexOffset || (graph->WritesAlpha && b.Mode == AlphaMode::Cutout));
                    if (wanted)
                        if (Shader* g = graph->Program->Get(shader.GraphVariant)) {
                            shader.ReplayInto(*g);           // the lighting, camera and per-pass values the plain shader got
                            next = g;
                        }
                    if (next != cur) { next->Bind(); cur = next; terrain = nullptr; cur->SetInt("u_Terrain", 0); }
                    if (graph && cur != &shader) graph->Apply(*cur);
                }
                Shader& shader = *cur;
                cull.Set(b.DoubleSided);
                shader.SetInt("u_DoubleSided", b.DoubleSided ? 1 : 0);   // lit.frag: back faces lit from their side
                if (b.Albedo)     b.Albedo->Bind(0);
                if (b.Normal)     b.Normal->Bind(3);
                if (b.MetalRough) b.MetalRough->Bind(4);
                if (b.Emissive)   b.Emissive->Bind(5);
                shader.SetInt("u_UseTexture", b.Albedo ? 1 : 0);
                shader.SetInt("u_UseNormalMap", b.Normal ? 1 : 0);
                shader.SetInt("u_UseMetalRough", b.MetalRough ? 1 : 0);
                shader.SetInt("u_UseEmissive", b.Emissive ? 1 : 0);
                shader.SetInt("u_AlphaMode", (int)b.Mode);
                shader.SetFloat("u_AlphaCutoff", b.Cutoff);
                if (b.Terrain != terrain) {                  // lit.frag's terrain branch: weights and layers
                    terrain = b.Terrain;
                    shader.SetInt("u_Terrain", terrain ? 1 : 0);
                    if (terrain) BindTerrain(shader, *terrain);
                }
                SetBones(shader, b.Bones);
                shader.SetInt("u_InstanceBase", (int)b.First);
                DrawBatchGeometry(b);
            }
            if (terrain) cur->SetInt("u_Terrain", 0);
            if (cur != &shader) shader.Bind();      // back to the plain program for whoever draws next
        };

        if (pass == RenderPass::Opaque) {
            // Opaque and cutout parts, in any order (the depth buffer sorts them)
            drawBatches(list.Opaque);
            return;
        }
        if (pass == RenderPass::Selected) {     // the highlighted parts, blend state up to the caller
            drawBatches(list.Selected);
            return;
        }

        // See-through parts: farthest first, blended over what is already drawn. They test depth
        // but don't write it, so they never hide each other or what's behind them.
        if (list.Transparent.empty()) return;
        glEnable(GL_BLEND);
        glBlendFunci(0, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // premultiplied (the entity-ID target is integer: never blended)
        glDepthMask(GL_FALSE);
        drawBatches(list.Transparent);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

} // namespace Engine

namespace Engine {
    const Material& PartMaterialOf(const MeshRendererComponent& mr, uint32_t part) {
        if (!mr.Geometry) return part < mr.Materials.size() && mr.Materials[part].Asset ? mr.Materials[part].Asset->Surface : DefaultMaterial();
        return *PartMaterial(mr, mr.Geometry.get(), part);
    }
}
