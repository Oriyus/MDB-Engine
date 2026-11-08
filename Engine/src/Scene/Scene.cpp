#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Renderer/Camera.h"
#include "Renderer/Shader.h"
#include "Renderer/Shadowmap.h"
#include "Renderer/Material.h"
#include "Renderer/Environment.h"
#include "Renderer/Model.h"
#include "Renderer/Mesh.h"
#include "Renderer/DrawList.h"
#include "Renderer/RenderStats.h"
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

    void Scene::SetWorldMatrix(entt::entity e, const glm::mat4& world) {
        const glm::mat4 local = glm::inverse(GetParentWorldMatrix(e)) * world;
        glm::vec3 pos, scale;
        glm::quat rot;
        Decompose(local, pos, rot, scale);
        auto& t = m_Registry.get<TransformComponent>(e);
        t.Position = pos;
        t.Rotation = glm::eulerAngles(rot);
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
        m_Registry.get<TransformComponent>(e).Rotation = glm::eulerAngles(glm::normalize(glm::inverse(parentRot) * rotation));
    }

    void Scene::Clear() {
        m_Registry.clear();
        ClearStaticBatches();        // merged meshes belong to the entities that are gone
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
    }

    // ---------------------------------------------------------------- skeletal animation
    // Advance a clip's time by `step` seconds and report the events it crossed into an.Fired.
    // Looping clips wrap (either direction); others hold the end and fire "end" once when they reach it.
    static float AdvanceClip(AnimatorComponent& an, const Model& model, int clip, const std::string& clipName,
        float time, float step, bool loop) {
        const float d = model.AnimationDuration(clip);
        if (d <= 0.0f) return 0.0f;
        if (step == 0.0f) return loop ? time : std::clamp(time, 0.0f, d);

        // Fire every event of this clip with a time in the half-open range (a, b]
        auto fire = [&](float a, float b) {
            for (const auto& ev : an.Events)
                if (ev.Clip == clipName && ev.Time > a && ev.Time <= b) an.Fired.emplace_back(ev.Name, clipName);
        };
        const float raw = time + step;
        if (!loop) {
            const float t = std::clamp(raw, 0.0f, d);
            if (step > 0.0f) {
                fire(time, t);
                if (time < d && t >= d) an.Fired.emplace_back("end", clipName);
            }
            else {
                fire(t - 1e-6f, time - 1e-6f);   // backwards: crossing from above
                if (time > 0.0f && t <= 0.0f) an.Fired.emplace_back("end", clipName);
            }
            return t;
        }
        // Looping: walk the range one lap at a time (a big step can cross several laps)
        float t = raw;
        if (step > 0.0f) {
            float from = time;
            int guard = 0;
            while (t > d && guard++ < 8) { fire(from, d); from = -1e-6f; t -= d; }   // -1e-6: an event at 0 fires on wrap
            fire(from, t);
        }
        else {
            float to = time;
            int guard = 0;
            while (t < 0.0f && guard++ < 8) { fire(-1e-6f, to - 1e-6f); to = d + 1e-6f; t += d; }
            fire(t - 1e-6f, to - 1e-6f);
        }
        t = std::fmod(t, d);
        return t < 0.0f ? t + d : t;
    }

    void Scene::UpdateAnimations(float dt) {
        static thread_local std::vector<Model::NodePose> pose, from, layer;   // reused: no allocations per frame
        static thread_local std::vector<uint8_t> mask;
        for (auto [e, an, mr] : m_Registry.view<AnimatorComponent, MeshRendererComponent>().each()) {
            an.Fired.clear();
            const Model* model = dynamic_cast<const Model*>(mr.Geometry.get());
            if (!model || !model->IsSkinned()) { an.Palette.clear(); continue; }

            // A new clip: fade out of the old one
            if (an.Clip != an.ActiveClip) {
                an.FadeFrom = an.ActiveClip;
                an.FadeFromTime = an.Time;
                an.Fade = (an.FadeFrom.empty() || an.BlendTime <= 0.0f) ? 1.0f : 0.0f;
                an.ActiveClip = an.Clip;
                an.Time = 0.0f;
            }
            const int clip = model->FindAnimation(an.ActiveClip);
            const int fromClip = an.Fade < 1.0f ? model->FindAnimation(an.FadeFrom) : -1;

            const bool advancing = an.Playing && dt != 0.0f;
            const float step = advancing ? dt * an.Speed : 0.0f;
            if (advancing) {
                if (an.Fade < 1.0f) an.Fade = std::min(1.0f, an.Fade + std::abs(dt) / std::max(an.BlendTime, 1e-4f));
            }
            else if (dt == 0.0f) {
                an.Fade = 1.0f;                                  // no time passing (editor): no fade, just the clip
            }
            const float prevTime = an.Time;
            an.Time = AdvanceClip(an, *model, clip, an.ActiveClip, an.Time, step, an.Loop);

            // Root motion: where the root bone goes sideways in this clip, the entity goes instead.
            // M takes the bone's translation (its parent's space) into the entity's own space.
            int motionRoot = -1;
            glm::mat3 toEntity(1.0f);
            if (an.RootMotion && clip >= 0) {
                motionRoot = an.RootBone.empty() ? model->RootMotionNode(clip) : model->FindNode(an.RootBone);
                if (motionRoot >= 0) toEntity = glm::mat3(mr.LocalOffset * model->ParentRestGlobal(motionRoot));
            }
            if (motionRoot >= 0 && step != 0.0f) {
                const float d = model->AnimationDuration(clip);
                glm::vec3 delta = model->SampleNodeTranslation(clip, motionRoot, an.Time) -
                                  model->SampleNodeTranslation(clip, motionRoot, prevTime);
                if (an.Loop && d > 0.0f) {                       // whole laps carry the clip's full travel
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

            if (fromClip >= 0) {                                 // the clip fading out keeps running, silently
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

            // Root motion: keep the root bone where the clip started, sideways (the entity carries it)
            if (motionRoot >= 0 && motionRoot < (int)pose.size()) {
                glm::vec3 offset = toEntity * (model->SampleNodeTranslation(clip, motionRoot, an.Time) -
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
            model->PoseToPalette(pose, an.Palette);
        }
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
        if (auto* c = m_Registry.try_get<ScriptComponent>(handle))           s.Script = *c;
        if (auto* c = m_Registry.try_get<CameraComponent>(handle))           s.Camera = *c;
        if (auto* c = m_Registry.try_get<AudioSourceComponent>(handle))      s.AudioSource = *c;
        if (auto* c = m_Registry.try_get<AnimatorComponent>(handle))         s.Animator = *c;
        if (auto* c = m_Registry.try_get<PrefabComponent>(handle))           s.Prefab = *c;
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
        if (s.Script)           m_Registry.emplace<ScriptComponent>(e, *s.Script);
        if (s.Camera)           m_Registry.emplace<CameraComponent>(e, *s.Camera);
        if (s.AudioSource)      m_Registry.emplace<AudioSourceComponent>(e, *s.AudioSource);
        if (s.Animator)         m_Registry.emplace<AnimatorComponent>(e, *s.Animator);
        if (s.Prefab)           m_Registry.emplace<PrefabComponent>(e, *s.Prefab);
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
    struct PointLightData {
        glm::vec3 Pos;
        glm::vec3 Color;     // already multiplied by intensity
        bool Shadows;
    };

    // Up to MaxLights point lights. The index in this list = the light's shadow layer,
    // so RenderShadows and Render must use this same function.
    static std::vector<PointLightData> GatherPointLights(Scene& scene) {
        auto& reg = scene.Registry();
        std::vector<PointLightData> out;
        auto view = reg.view<TransformComponent, PointLightComponent>();
        for (auto entity : view) {
            if (out.size() >= PointShadowArray::MaxLights) break;
            auto [t, l] = view.get<TransformComponent, PointLightComponent>(entity);
            out.push_back({ glm::vec3(scene.GetWorldMatrix(entity)[3]), l.Color * l.Intensity, l.CastShadows });
        }
        return out;
    }

    // How one part of a Mesh Renderer uses alpha: the component's settings combined with the
    // model material's (the stronger mode wins, alphas multiply)
    struct SurfaceAlpha {
        AlphaMode Mode = AlphaMode::Opaque;
        float Alpha = 1.0f;
        float Cutoff = 0.5f;
        const Texture2D* Albedo = nullptr;
    };

    static SurfaceAlpha GetSurfaceAlpha(const MeshRendererComponent& mr, uint32_t part) {
        const Material* mat = mr.UseModelMaterials ? mr.Geometry->GetSubmeshMaterial(part) : nullptr;
        SurfaceAlpha s;
        s.Mode = mr.Alpha;
        s.Alpha = std::clamp(mr.Opacity, 0.0f, 1.0f);
        s.Cutoff = mr.AlphaCutoff;
        s.Albedo = mat ? mat->Albedo.get() : mr.Texture.get();
        if (mat) {
            if ((int)mat->Mode > (int)s.Mode) s.Mode = mat->Mode;
            if (mr.Alpha != AlphaMode::Cutout) s.Cutoff = mat->AlphaCutoff;
            s.Alpha *= mat->Alpha;
        }
        return s;
    }

    // First directional light, if any. dir = the direction the light travels.
    static bool FindSun(Scene& scene, glm::vec3& dir, glm::vec3& color, bool& shadows) {
        auto& reg = scene.Registry();
        auto view = reg.view<TransformComponent, DirectionalLightComponent>();
        for (auto entity : view) {
            auto [t, d] = view.get<TransformComponent, DirectionalLightComponent>(entity);
            dir = glm::normalize(glm::vec3(scene.GetWorldMatrix(entity) * glm::vec4(0, 0, -1, 0)));
            color = d.Color * d.Intensity;
            shadows = d.CastShadows;
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
                const glm::vec3 c = glm::vec3(world * glm::vec4((mn + mx) * 0.5f, 1.0f));
                const glm::vec3 h = (mx - mn) * 0.5f;
                const glm::vec3 e = glm::abs(glm::vec3(world[0])) * h.x + glm::abs(glm::vec3(world[1])) * h.y +
                    glm::abs(glm::vec3(world[2])) * h.z;
                for (const auto& p : Planes)
                    if (glm::dot(glm::vec3(p), c) + glm::dot(glm::abs(glm::vec3(p)), e) + p.w < 0.0f) return false;
                return true;
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

        void DrawBatchGeometry(const DrawBatch& b) {
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
    // RenderShadows draws them: the 6 cube faces of each shadowed point light, then the sun cascades.
    // Also sets list.CascadeSplits / CascadeCount.
    static std::vector<ShadowView> GatherShadowViews(Scene& scene, const ShadowSettings& s,
        const std::vector<PointLightData>& lights, const Camera& camera, uint32_t sunSize, DrawList& list) {
        std::vector<ShadowView> views;
        if (!s.Enabled) return views;
        static const glm::vec3 dirs[6] = {
            { 1, 0, 0 },{ -1, 0, 0 },{ 0, 1, 0 },{ 0, -1, 0 },{ 0, 0, 1 },{ 0, 0, -1 } };
        static const glm::vec3 ups[6] = {
            { 0, -1, 0 },{ 0, -1, 0 },{ 0, 0, 1 },{ 0, 0, -1 },{ 0, -1, 0 },{ 0, -1, 0 } };
        const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, s.PointFar);
        for (int i = 0; i < (int)lights.size(); i++) {
            if (!lights[i].Shadows) continue;
            for (int f = 0; f < 6; f++) {
                ShadowView v;
                v.ViewProj = proj * glm::lookAt(lights[i].Pos, lights[i].Pos + dirs[f], ups[f]);
                v.Light = i;
                v.Face = f;
                views.push_back(v);
            }
        }

        glm::vec3 sunDir, sunColor;
        bool sunShadows = false;
        if (!FindSun(scene, sunDir, sunColor, sunShadows) || !sunShadows) return views;

        if (!s.SunCascades) {
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

            views.push_back(FitSunView(sunDir, center, radius, s.SunDistance, sunSize, c));
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
            glm::vec3 EmissiveColor{ 0.0f };
            int Unlit = 0, CastShadows = 1;
            glm::ivec3 Cell{ 0 };
            auto Tie() const {
                return std::tie(Albedo, Normal, MetalRough, Emissive, Mode, Cutoff, Color.x, Color.y, Color.z, Alpha,
                    Metallic, Roughness, EmissiveColor.x, EmissiveColor.y, EmissiveColor.z, Unlit, CastShadows,
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
            if (!mr.Static || !mr.Geometry || GetBonePalette(e, mr)) continue;
            if (uses[mr.Geometry.get()] > 1) continue;                   // repeated: instancing handles it better
            const uint32_t parts = mr.UseModelMaterials ? mr.Geometry->SubmeshCount() : 1;

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
                const Material* mat = mr.UseModelMaterials ? mr.Geometry->GetSubmeshMaterial(i) : nullptr;
                partV.clear(); partI.clear();
                if (!mr.Geometry->GetPartVertices(mat ? (int32_t)i : -1, partV, partI) || partI.empty()) { mergeable = false; break; }
                const SurfaceAlpha s = GetSurfaceAlpha(mr, i);

                Part p;
                p.Key.Albedo = s.Albedo;
                p.Key.Normal = mat ? mat->Normal.get() : nullptr;
                p.Key.MetalRough = mat ? mat->MetalRough.get() : nullptr;
                p.Key.Emissive = mat ? mat->Emissive.get() : nullptr;
                p.Key.Mode = (int)s.Mode;
                p.Key.Cutoff = s.Mode == AlphaMode::Cutout ? s.Cutoff : 0.5f;
                p.Key.Color = mat ? mat->Color * mr.Color : mr.Color;
                p.Key.Alpha = s.Alpha;
                p.Key.Metallic = mat ? mat->Metallic : mr.Metallic;
                p.Key.Roughness = mat ? mat->Roughness : mr.Roughness;
                p.Key.EmissiveColor = mat ? mat->EmissiveColor : mr.Emissive;
                p.Key.Unlit = mr.Unlit ? 1 : 0;
                p.Key.CastShadows = mr.CastShadows ? 1 : 0;
                p.Key.Cell = glm::ivec3(glm::floor(center / kStaticCell));
                p.V.reserve(partV.size());
                for (const Vertex& v : partV) {
                    Vertex w = v;
                    w.Position = glm::vec3(world * glm::vec4(v.Position, 1.0f));
                    w.Normal = glm::normalize(normalMatrix * v.Normal);
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
            sb.Instance.Color = glm::vec4(key.Color, key.Alpha);
            sb.Instance.Surface = glm::vec4(key.Metallic, key.Roughness, 0.0f, 0.0f);
            sb.Instance.Emissive = glm::vec4(key.EmissiveColor, 0.0f);
            sb.Instance.Info = glm::ivec4(-1, key.Unlit, 0, 0);       // no single entity: click-picking finds nothing
            m_StaticBatches.push_back(std::move(sb));
        }
    }

    void Scene::BuildDrawList(DrawList& list, const Camera& camera, const ShadowContext* shadows) {
        const ShadowSettings* shadowSettings = (shadows && shadows->Settings.Enabled) ? &shadows->Settings : nullptr;
        const uint32_t sunSize = (shadows && shadows->Sun) ? shadows->Sun->GetSize() : 2048u;
        list.Clear();
        const Frustum frustum(camera.GetViewProjection());

        // Shadow views: a frustum each, plus the light's reach as a sphere for a quick first test
        const auto lights = GatherPointLights(*this);
        list.Views = shadowSettings ? GatherShadowViews(*this, *shadowSettings, lights, camera, sunSize, list) : std::vector<ShadowView>{};
        const auto& views = list.Views;
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

        // Which shadow views a caster (a box under a transform) can show up in: fills inView
        auto classifyShadows = [&](const glm::mat4& world, const glm::vec3& mn, const glm::vec3& mx) {
            bool any = false;
            const glm::vec3 center = glm::vec3(world * glm::vec4((mn + mx) * 0.5f, 1.0f));
            const float radius = glm::length(glm::vec3(world * glm::vec4((mx - mn) * 0.5f, 0.0f)));
            int lastLight = -2;
            bool lightReaches = true;
            for (size_t v = 0; v < views.size(); v++) {
                if (views[v].Light != lastLight) {   // new light: is the caster within its range at all?
                    lastLight = views[v].Light;
                    lightReaches = lastLight < 0 ||
                        glm::length(center - lights[lastLight].Pos) - radius < shadowSettings->PointFar;
                }
                inView[v] = lightReaches && viewFrusta[v].Visible(world, mn, mx);
                if (!inView[v]) list.ShadowSkipped++;
                any = any || inView[v];
            }
            return any;
        };

        // Merged static geometry (during Play): one object per batch, in world space
        if (m_StaticActive && m_StaticDirty) BuildStaticBatches();   // a merged object went away
        const glm::mat4 identity(1.0f);
        for (const StaticBatch& sb : m_StaticBatches) {
            const bool visible = frustum.Visible(identity, sb.Min, sb.Max);
            if (!visible) list.Culled++;
            const bool anyShadow = !views.empty() && sb.CastShadows && classifyShadows(identity, sb.Min, sb.Max);
            if (!visible && !anyShadow) continue;

            DrawList::Item item;
            item.Instance = list.AddInstance(sb.Instance);
            item.State = sb.State;
            if (anyShadow) {
                DrawList::Item sh = item;
                sh.State.Normal = sh.State.MetalRough = sh.State.Emissive = nullptr;
                if (sb.State.Mode == AlphaMode::Opaque) { sh.State.Albedo = nullptr; sh.State.Cutoff = 0.5f; }
                for (size_t v = 0; v < views.size(); v++) if (inView[v]) shadow[v].push_back(sh);
            }
            if (visible) opaque.push_back(item);
        }

        auto drawables = m_Registry.view<TransformComponent, MeshRendererComponent>();
        for (auto entity : drawables) {
            const auto& mr = drawables.get<MeshRendererComponent>(entity);
            if (!mr.Geometry || mr.Merged) continue;    // merged: drawn by its static batch

            const glm::mat4 world = GetWorldMatrix(entity) * mr.LocalOffset;
            const auto* bones = GetBonePalette(entity, mr);
            glm::vec3 mn, mx;
            mr.Geometry->GetLocalBounds(mn, mx);
            if (bones) {                        // the bounds are the rest pose: leave room for animation
                const glm::vec3 c = (mn + mx) * 0.5f, h = (mx - mn) * 0.75f;
                mn = c - h; mx = c + h;
            }
            const bool visible = frustum.Visible(world, mn, mx);
            if (!visible) list.Culled++;

            // Which shadow views this caster can show up in (light markers must not block light)
            const bool anyShadow = !views.empty() && mr.CastShadows && !mr.Unlit && classifyShadows(world, mn, mx);
            if (!visible && !anyShadow) continue;

            InstanceData base;
            base.Model = world;
            base.Normal = glm::mat4(glm::transpose(glm::inverse(glm::mat3(world))));
            base.Info = glm::ivec4((int)entt::to_integral(entity), mr.Unlit ? 1 : 0, 0, 0);
            float distance = -1.0f;

            const uint32_t parts = mr.UseModelMaterials ? mr.Geometry->SubmeshCount() : 1;
            for (uint32_t i = 0; i < parts; i++) {
                const Material* mat = mr.UseModelMaterials ? mr.Geometry->GetSubmeshMaterial(i) : nullptr;
                const SurfaceAlpha s = GetSurfaceAlpha(mr, i);

                // With a model material the component Color tints it; metallic/roughness/emissive come from the file
                InstanceData inst = base;
                inst.Color = glm::vec4(mat ? mat->Color * mr.Color : mr.Color, s.Alpha);
                inst.Surface = glm::vec4(mat ? mat->Metallic : mr.Metallic, mat ? mat->Roughness : mr.Roughness, 0.0f, 0.0f);
                inst.Emissive = glm::vec4(mat ? mat->EmissiveColor : mr.Emissive, 0.0f);

                DrawList::Item item;
                item.Instance = list.AddInstance(inst);
                DrawBatch& b = item.State;
                b.Geometry = mr.Geometry.get();
                b.Part = mat ? (int32_t)i : -1;
                b.Albedo = s.Albedo;
                b.Normal = mat ? mat->Normal.get() : nullptr;
                b.MetalRough = mat ? mat->MetalRough.get() : nullptr;
                b.Emissive = mat ? mat->Emissive.get() : nullptr;
                b.Mode = s.Mode;
                b.Cutoff = s.Cutoff;
                b.Bones = bones;

                if (anyShadow) {
                    // Shadows only need the albedo, and only to alpha-test: opaque casters share batches
                    DrawList::Item sh = item;
                    sh.State.Normal = sh.State.MetalRough = sh.State.Emissive = nullptr;
                    if (s.Mode == AlphaMode::Opaque) { sh.State.Albedo = nullptr; sh.State.Cutoff = 0.5f; }
                    for (size_t v = 0; v < views.size(); v++) if (inView[v]) shadow[v].push_back(sh);
                }
                if (!visible) continue;
                if (s.Mode == AlphaMode::Blend) {
                    if (distance < 0.0f)
                        distance = glm::length(glm::vec3(world * glm::vec4((mn + mx) * 0.5f, 1.0f)) - camera.Position);
                    item.Distance = distance;
                    transparent.push_back(item);
                }
                else opaque.push_back(item);
            }
        }
        list.AddBatches(opaque, list.Opaque, DrawList::Order::ByState);
        list.AddBatches(transparent, list.Transparent, DrawList::Order::BackToFront);
        for (size_t v = 0; v < views.size(); v++) list.AddBatches(shadow[v], list.ShadowViews[v], DrawList::Order::ByState);
        list.Upload();
        RenderStats::Culled += list.Culled;
        RenderStats::ShadowSkipped += list.ShadowSkipped;
        RenderStats::StaticBatches = (uint32_t)m_StaticBatches.size();
        RenderStats::StaticMerged = m_StaticMerged;
    }

    // ---------------------------------------------------------------- shadows
    void Scene::RenderShadows(ShadowContext& ctx, const DrawList& list) {
        const ShadowSettings& s = ctx.Settings;
        if (!s.Enabled) return;
        const auto lights = GatherPointLights(*this);
        const auto& views = list.Views;                         // fitted by BuildDrawList
        if (views.size() != list.ShadowViews.size()) return;   // the list was built without shadows
        list.Bind();

        auto drawCasters = [&](Shader& sh, const std::vector<DrawBatch>& batches) {
            sh.SetInt("u_Texture", 0);
            for (const DrawBatch& b : batches) {
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

        for (size_t v = 0; v < views.size(); v++) {
            const ShadowView& view = views[v];
            if (view.Light >= 0) {
                // A point light cube face. Every face is bound and cleared, even with no casters in it.
                if (!ctx.Points || !ctx.PointDepth) continue;
                Shader& sh = *ctx.PointDepth;
                sh.Bind();
                sh.SetFloat("u_FarPlane", s.PointFar);
                sh.SetVec3("u_LightPos", lights[view.Light].Pos);
                ctx.Points->BindFace((uint32_t)view.Light, (uint32_t)view.Face);
                sh.SetMat4("u_LightVP", view.ViewProj);
                drawCasters(sh, list.ShadowViews[v]);
            }
            else {
                if (!ctx.Sun || !ctx.SunDepth) continue;
                Shader& sh = *ctx.SunDepth;
                sh.Bind();
                ctx.Sun->Bind((uint32_t)view.Face);              // this cascade's layer
                sh.SetMat4("u_LightVP", view.ViewProj);
                drawCasters(sh, list.ShadowViews[v]);
            }
        }
    }

    // ---------------------------------------------------------------- render system
    void Scene::Render(const Camera& camera, Shader& shader, const DrawList& list, const ShadowContext* ctx,
        const Environment* env, RenderPass pass) {
        const auto lights = GatherPointLights(*this);

        glm::vec3 sunDir(0.0f, -1.0f, 0.0f), sunColor(0.0f);
        bool sunShadows = false;
        const bool hasSun = FindSun(*this, sunDir, sunColor, sunShadows);

        const bool shadowsOn = ctx && ctx->Settings.Enabled;

        shader.Bind();
        shader.SetMat4("u_ViewProjection", camera.GetViewProjection());
        shader.SetVec3("u_ViewPos", camera.Position);

        // Every sampler gets its own unit, even when unused (two sampler types on one unit is an error)
        shader.SetInt("u_Texture", 0);
        shader.SetInt("u_PointShadowMaps", 1);
        shader.SetInt("u_SunShadowMap", 2);
        shader.SetInt("u_NormalMap", 3);
        shader.SetInt("u_MetalRoughMap", 4);
        shader.SetInt("u_EmissiveMap", 5);
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

        // Point lights
        const bool pointShadows = shadowsOn && ctx->Points;
        shader.SetInt("u_PointCount", (int)lights.size());
        for (size_t i = 0; i < lights.size(); i++) {
            const std::string idx = "[" + std::to_string(i) + "]";
            shader.SetVec3("u_PointPos" + idx, lights[i].Pos);
            shader.SetVec3("u_PointColor" + idx, lights[i].Color);
            shader.SetInt("u_PointShadow" + idx, (pointShadows && lights[i].Shadows) ? 1 : 0);
        }
        if (pointShadows) {
            ctx->Points->BindTexture(1);
            shader.SetFloat("u_PointFar", ctx->Settings.PointFar);
            shader.SetFloat("u_PointBias", ctx->Settings.PointBias);
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

        // Each batch: its textures and alpha settings, then one instanced call for all its objects
        list.Bind();
        auto drawBatches = [&](const std::vector<DrawBatch>& batches) {
            for (const DrawBatch& b : batches) {
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
                SetBones(shader, b.Bones);
                shader.SetInt("u_InstanceBase", (int)b.First);
                DrawBatchGeometry(b);
            }
        };

        if (pass == RenderPass::Opaque) {
            // Opaque and cutout parts, in any order (the depth buffer sorts them)
            drawBatches(list.Opaque);
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
