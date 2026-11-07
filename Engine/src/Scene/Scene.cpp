#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Renderer/Camera.h"
#include "Renderer/Shader.h"
#include "Renderer/Shadowmap.h"
#include "Renderer/Material.h"
#include "Renderer/Environment.h"
#include "Renderer/Model.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

namespace Engine {

    Scene::Scene() {
        // Keep FindByID's map honest however IDs come and go (create, destroy, clear, replace)
        m_Registry.on_construct<IDComponent>().connect<&Scene::OnIDChanged>(this);
        m_Registry.on_update<IDComponent>().connect<&Scene::OnIDChanged>(this);
        m_Registry.on_destroy<IDComponent>().connect<&Scene::OnIDChanged>(this);
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

    void Scene::Clear() { m_Registry.clear(); }

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
    void Scene::UpdateAnimations(float dt) {
        static thread_local std::vector<Model::NodePose> pose, from;   // reused: no allocations per frame
        for (auto [e, an, mr] : m_Registry.view<AnimatorComponent, MeshRendererComponent>().each()) {
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

            if (an.Playing && dt != 0.0f) {
                an.Time += dt * an.Speed;
                an.FadeFromTime += dt * an.Speed;
                if (an.Fade < 1.0f) an.Fade = std::min(1.0f, an.Fade + std::abs(dt) / std::max(an.BlendTime, 1e-4f));
            }
            else if (dt == 0.0f) {
                an.Fade = 1.0f;                                  // no time passing (editor): no fade, just the clip
            }
            // Loop (either direction) or hold the last frame
            auto wrap = [&](float t, int c) {
                const float d = model->AnimationDuration(c);
                if (d <= 0.0f) return 0.0f;
                if (an.Loop) { t = std::fmod(t, d); return t < 0.0f ? t + d : t; }
                return std::clamp(t, 0.0f, d);
            };
            an.Time = wrap(an.Time, clip);
            an.FadeFromTime = wrap(an.FadeFromTime, fromClip);

            model->SamplePose(clip, an.Time, pose);
            if (fromClip >= 0 && an.Fade < 1.0f) {
                model->SamplePose(fromClip, an.FadeFromTime, from);
                for (size_t i = 0; i < pose.size(); i++) {
                    pose[i].T = glm::mix(from[i].T, pose[i].T, an.Fade);
                    pose[i].R = glm::slerp(from[i].R, pose[i].R, an.Fade);
                    pose[i].S = glm::mix(from[i].S, pose[i].S, an.Fade);
                }
            }
            model->PoseToPalette(pose, an.Palette);
        }
    }

    // Bone matrices for a skinned model: the Animator's pose, or the rest pose if nothing animates it
    void Scene::SetSkinning(Shader& shader, entt::entity e, const MeshRendererComponent& mr) {
        constexpr size_t kMaxBones = 128;                       // MAX_BONES in lit.vert / shadow.vert
        const Model* model = dynamic_cast<const Model*>(mr.Geometry.get());
        if (!model || !model->IsSkinned()) { shader.SetInt("u_Skinned", 0); return; }
        const auto* an = m_Registry.try_get<AnimatorComponent>(e);
        const auto& palette = (an && !an->Palette.empty()) ? an->Palette : model->RestPalette();
        if (palette.size() > kMaxBones) {
            static bool warned = false;
            if (!warned) { ENGINE_WARN("A model has {} bones; only {} are supported", palette.size(), kMaxBones); warned = true; }
        }
        shader.SetInt("u_Skinned", 1);
        shader.SetMat4Array("u_Bones", palette.data(), (uint32_t)std::min(palette.size(), kMaxBones));
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
        if (s.MeshRenderer)     m_Registry.emplace<MeshRendererComponent>(e, *s.MeshRenderer);
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
        m_Registry.clear();
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

    // Orthographic camera looking along the sun direction at the origin
    static glm::mat4 SunViewProjection(const glm::vec3& dir, const ShadowSettings& s) {
        glm::vec3 up = std::abs(dir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        glm::mat4 view = glm::lookAt(-dir * s.SunDistance, glm::vec3(0.0f), up);
        glm::mat4 proj = glm::ortho(-s.SunExtent, s.SunExtent, -s.SunExtent, s.SunExtent, 0.1f, s.SunDistance * 2.0f);
        return proj * view;
    }

    // ---------------------------------------------------------------- shadow pass
    void Scene::RenderShadows(ShadowContext& ctx) {
        const ShadowSettings& s = ctx.Settings;
        if (!s.Enabled) return;

        auto drawables = m_Registry.view<TransformComponent, MeshRendererComponent>();
        auto drawScene = [&](Shader& sh) {
            for (auto entity : drawables) {
                auto [t, mr] = drawables.get<TransformComponent, MeshRendererComponent>(entity);
                if (!mr.Geometry || mr.Unlit) continue;   // light markers must not block light
                SetSkinning(sh, entity, mr);
                sh.SetMat4("u_Model", GetWorldMatrix(entity) * mr.LocalOffset);
                mr.Geometry->Draw();
            }
            };

        // ---- point lights: 6 cube faces each
        auto lights = GatherPointLights(*this);
        if (ctx.Points && ctx.PointDepth && !lights.empty()) {
            static const glm::vec3 dirs[6] = {
                { 1, 0, 0 },{ -1, 0, 0 },{ 0, 1, 0 },{ 0, -1, 0 },{ 0, 0, 1 },{ 0, 0, -1 } };
            static const glm::vec3 ups[6] = {
                { 0, -1, 0 },{ 0, -1, 0 },{ 0, 0, 1 },{ 0, 0, -1 },{ 0, -1, 0 },{ 0, -1, 0 } };
            const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, s.PointFar);

            Shader& sh = *ctx.PointDepth;
            sh.Bind();
            sh.SetFloat("u_FarPlane", s.PointFar);

            for (uint32_t i = 0; i < lights.size(); i++) {
                if (!lights[i].Shadows) continue;
                sh.SetVec3("u_LightPos", lights[i].Pos);
                for (uint32_t face = 0; face < 6; face++) {
                    ctx.Points->BindFace(i, face);
                    sh.SetMat4("u_LightVP", proj * glm::lookAt(lights[i].Pos, lights[i].Pos + dirs[face], ups[face]));
                    drawScene(sh);
                }
            }
        }

        // ---- sun: one orthographic depth pass
        glm::vec3 sunDir, sunColor;
        bool sunShadows = false;
        if (ctx.Sun && ctx.SunDepth && FindSun(*this, sunDir, sunColor, sunShadows) && sunShadows) {
            Shader& sh = *ctx.SunDepth;
            sh.Bind();
            ctx.Sun->Bind();
            sh.SetMat4("u_LightVP", SunViewProjection(sunDir, s));
            drawScene(sh);
        }
    }

    // ---------------------------------------------------------------- render system
    void Scene::Render(const Camera& camera, Shader& shader, const ShadowContext* ctx, const Environment* env) {
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

            const bool useSunShadow = shadowsOn && ctx->Sun && sunShadows;
            shader.SetInt("u_SunShadow", useSunShadow ? 1 : 0);
            if (useSunShadow) {
                ctx->Sun->BindTexture(2);
                shader.SetMat4("u_SunVP", SunViewProjection(sunDir, ctx->Settings));
                shader.SetFloat("u_SunBias", ctx->Settings.SunBias);
                shader.SetFloat("u_SunTexel", 2.0f * ctx->Settings.SunExtent / (float)ctx->Sun->GetSize());
            }
        }

        auto drawables = m_Registry.view<TransformComponent, MeshRendererComponent>();
        for (auto entity : drawables) {
            auto [t, mr] = drawables.get<TransformComponent, MeshRendererComponent>(entity);
            if (!mr.Geometry) continue;

            shader.SetInt("u_Unlit", mr.Unlit ? 1 : 0);
            shader.SetInt("u_EntityID", (int)entt::to_integral(entity));
            SetSkinning(shader, entity, mr);
            shader.SetMat4("u_Model", GetWorldMatrix(entity) * mr.LocalOffset);

            const uint32_t parts = mr.UseModelMaterials ? mr.Geometry->SubmeshCount() : 1;
            for (uint32_t i = 0; i < parts; i++) {
                const Material* mat = mr.UseModelMaterials ? mr.Geometry->GetSubmeshMaterial(i) : nullptr;

                // With a model material the component Color tints it; metallic/roughness/emissive come from the file
                const glm::vec3 color = mat ? mat->Color * mr.Color : mr.Color;
                const Texture2D* albedo = mat ? mat->Albedo.get() : mr.Texture.get();
                const Texture2D* normal = mat ? mat->Normal.get() : nullptr;
                const Texture2D* metalRough = mat ? mat->MetalRough.get() : nullptr;
                const Texture2D* emissiveTex = mat ? mat->Emissive.get() : nullptr;

                if (albedo)       albedo->Bind(0);
                if (normal)       normal->Bind(3);
                if (metalRough)   metalRough->Bind(4);
                if (emissiveTex)  emissiveTex->Bind(5);

                shader.SetVec3("u_Color", color);
                shader.SetInt("u_UseTexture", albedo ? 1 : 0);
                shader.SetInt("u_UseNormalMap", normal ? 1 : 0);
                shader.SetInt("u_UseMetalRough", metalRough ? 1 : 0);
                shader.SetInt("u_UseEmissive", emissiveTex ? 1 : 0);
                shader.SetFloat("u_Metallic", mat ? mat->Metallic : mr.Metallic);
                shader.SetFloat("u_Roughness", mat ? mat->Roughness : mr.Roughness);
                shader.SetVec3("u_Emissive", mat ? mat->EmissiveColor : mr.Emissive);

                if (mat) mr.Geometry->DrawSubmesh(i);
                else     mr.Geometry->Draw();
            }
        }
    }

} // namespace Engine