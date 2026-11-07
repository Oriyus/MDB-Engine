#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Renderer/Camera.h"
#include "Renderer/Shader.h"
#include "Renderer/Shadowmap.h"
#include "Renderer/Material.h"
#include "Renderer/Environment.h"

#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

namespace Engine {

    Entity Scene::CreateEntity(const std::string& name) {
        Entity entity(m_Registry.create(), this);
        entity.Add<TransformComponent>();
        entity.Add<TagComponent>().Tag = name;
        return entity;
    }

    void Scene::DestroyEntity(Entity entity) {
        if (entity) m_Registry.destroy(entity.GetHandle());
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
    }

    SceneSnapshot Scene::Capture(entt::entity selected, int& selectedIndex) {
        SceneSnapshot snap;
        selectedIndex = -1;

        for (auto handle : m_Registry.view<TagComponent>()) {
            EntitySnapshot s;
            s.Tag = m_Registry.get<TagComponent>(handle).Tag;
            s.Transform = m_Registry.get<TransformComponent>(handle);
            if (auto* c = m_Registry.try_get<MeshRendererComponent>(handle))     s.MeshRenderer = *c;
            if (auto* c = m_Registry.try_get<PointLightComponent>(handle))       s.PointLight = *c;
            if (auto* c = m_Registry.try_get<DirectionalLightComponent>(handle)) s.DirectionalLight = *c;
            if (auto* c = m_Registry.try_get<SpinComponent>(handle))             s.Spin = *c;
            if (auto* c = m_Registry.try_get<OrbitComponent>(handle))            s.Orbit = *c;
            if (auto* c = m_Registry.try_get<RigidBodyComponent>(handle))        s.RigidBody = *c;

            if (handle == selected) selectedIndex = (int)snap.size();
            snap.push_back(std::move(s));
        }
        return snap;
    }

    entt::entity Scene::Restore(const SceneSnapshot& snap, int selectedIndex) {
        m_Registry.clear();
        entt::entity selected = entt::null;

        // The registry iterates newest-first, and Capture stored them in that order.
        // Rebuild back to front so the Hierarchy list order doesn't flip.
        for (size_t n = snap.size(); n-- > 0;) {
            const EntitySnapshot& s = snap[n];
            entt::entity e = m_Registry.create();
            m_Registry.emplace<TagComponent>(e, TagComponent{ s.Tag });
            m_Registry.emplace<TransformComponent>(e, s.Transform);
            if (s.MeshRenderer)     m_Registry.emplace<MeshRendererComponent>(e, *s.MeshRenderer);
            if (s.PointLight)       m_Registry.emplace<PointLightComponent>(e, *s.PointLight);
            if (s.DirectionalLight) m_Registry.emplace<DirectionalLightComponent>(e, *s.DirectionalLight);
            if (s.Spin)             m_Registry.emplace<SpinComponent>(e, *s.Spin);
            if (s.Orbit)            m_Registry.emplace<OrbitComponent>(e, *s.Orbit);
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
    static std::vector<PointLightData> GatherPointLights(entt::registry& reg) {
        std::vector<PointLightData> out;
        auto view = reg.view<TransformComponent, PointLightComponent>();
        for (auto entity : view) {
            if (out.size() >= PointShadowArray::MaxLights) break;
            auto [t, l] = view.get<TransformComponent, PointLightComponent>(entity);
            out.push_back({ t.Position, l.Color * l.Intensity, l.CastShadows });
        }
        return out;
    }

    // First directional light, if any. dir = the direction the light travels.
    static bool FindSun(entt::registry& reg, glm::vec3& dir, glm::vec3& color, bool& shadows) {
        auto view = reg.view<TransformComponent, DirectionalLightComponent>();
        for (auto entity : view) {
            auto [t, d] = view.get<TransformComponent, DirectionalLightComponent>(entity);
            dir = glm::normalize(glm::vec3(glm::mat4_cast(glm::quat(t.Rotation)) * glm::vec4(0, 0, -1, 0)));
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
                sh.SetMat4("u_Model", t.GetMatrix() * mr.LocalOffset);
                mr.Geometry->Draw();
            }
            };

        // ---- point lights: 6 cube faces each
        auto lights = GatherPointLights(m_Registry);
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
        if (ctx.Sun && ctx.SunDepth && FindSun(m_Registry, sunDir, sunColor, sunShadows) && sunShadows) {
            Shader& sh = *ctx.SunDepth;
            sh.Bind();
            ctx.Sun->Bind();
            sh.SetMat4("u_LightVP", SunViewProjection(sunDir, s));
            drawScene(sh);
        }
    }

    // ---------------------------------------------------------------- render system
    void Scene::Render(const Camera& camera, Shader& shader, const ShadowContext* ctx, const Environment* env) {
        const auto lights = GatherPointLights(m_Registry);

        glm::vec3 sunDir(0.0f, -1.0f, 0.0f), sunColor(0.0f);
        bool sunShadows = false;
        const bool hasSun = FindSun(m_Registry, sunDir, sunColor, sunShadows);

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
            shader.SetMat4("u_Model", t.GetMatrix() * mr.LocalOffset);

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