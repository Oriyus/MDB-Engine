#include "Core/Application.h"
#include "Core/Input.h"
#include "Core/Log.h"
#include "Core/EntryPoint.h"
#include "Renderer/Shader.h"
#include "Renderer/Texture.h"
#include "Renderer/Camera.h"
#include "Renderer/Mesh.h"
#include "Renderer/Model.h"
#include "Renderer/RenderStats.h"
#include "Renderer/Framebuffer.h"
#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Scene/SceneSerializer.h"
#include "Renderer/Shadowmap.h"
#include "Renderer/PostProcess.h"
#include "Renderer/Material.h"

#include <cfloat>
#include <glad/glad.h>
#include <imgui.h>              // must come BEFORE ImGuizmo.h
#include <imgui_internal.h>     // DockBuilder (default layout)
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <vector>

class SandboxApp : public Engine::Application {
public:
    SandboxApp() : Application("Sandbox") {}

    void OnStart() override {
        using namespace Engine;

        m_Shader = std::make_unique<Shader>(
            std::string(ASSET_DIR) + "Shaders/lit.vert",
            std::string(ASSET_DIR) + "Shaders/lit.frag");

        m_ShadowShader = std::make_unique<Shader>(
            std::string(ASSET_DIR) + "Shaders/shadow.vert",
            std::string(ASSET_DIR) + "Shaders/shadow.frag");
        
        m_SunShader = std::make_unique<Shader>(
            std::string(ASSET_DIR) + "Shaders/shadow.vert",
            std::string(ASSET_DIR) + "Shaders/sun_shadow.frag");

        m_PostShader = std::make_unique<Shader>(
            std::string(ASSET_DIR) + "Shaders/tonemap.vert",
            std::string(ASSET_DIR) + "Shaders/tonemap.frag");

        m_PointShadows = std::make_unique<PointShadowArray>(1024);
        m_SunShadowMap = std::make_unique<DirectionalShadowMap>(2048);

        m_Shadows.Points = m_PointShadows.get();
        m_Shadows.Sun = m_SunShadowMap.get();
        m_Shadows.PointDepth = m_ShadowShader.get();
        m_Shadows.SunDepth = m_SunShader.get();

        m_Cube = Mesh::CreateCube();
        m_Checker = CreateCheckerTexture();

        // --- Floor
        {
            auto e = m_Scene.CreateEntity("Floor");
            e.Get<TransformComponent>().Position = glm::vec3(0.0f, -1.0f, 0.0f);
            auto& mr = e.Add<MeshRendererComponent>();
            m_Plane = Mesh::CreatePlane(12.0f, 6.0f);
            mr.Geometry = m_Plane;
            mr.Texture = m_Checker;
            mr.Color = glm::vec3(0.8f);
        }

        // --- Spinning cube
        {
            auto e = m_Scene.CreateEntity("Cube");
            e.Get<TransformComponent>().Position = glm::vec3(-2.5f, -0.5f, 0.0f);
            auto& mr = e.Add<MeshRendererComponent>();
            mr.Geometry = m_Cube;
            mr.Texture = m_Checker;
            e.Add<SpinComponent>().Speed = glm::vec3(0.0f, 0.6f, 0.0f);
        }

        // --- Loaded model
        {
            m_ModelResource = std::make_shared<Model>(std::string(ASSET_DIR) + "Models/BoomBox.glb");
            auto model = m_ModelResource;
            if (model->IsValid()) {
                auto e = m_Scene.CreateEntity("Model");
                auto& mr = e.Add<MeshRendererComponent>();
                mr.Geometry = model;
                mr.LocalOffset = model->GetNormalizeMatrix(2.0f);
                mr.UseModelMaterials = model->HasTexturedMaterials();
                mr.Color = mr.UseModelMaterials ? glm::vec3(1.0f) : glm::vec3(0.85f, 0.6f, 0.35f);
                e.Add<SpinComponent>().Speed = glm::vec3(0.0f, 0.3f, 0.0f);
            }
        }

        // --- Orbiting point light, drawn as a small unlit cube
        {
            const glm::vec3 lightColor(1.0f, 0.95f, 0.85f);
            auto e = m_Scene.CreateEntity("Light");
            e.Get<TransformComponent>().Scale = glm::vec3(0.15f);
            e.Add<PointLightComponent>().Color = lightColor;
            auto& orbit = e.Add<OrbitComponent>();
            orbit.Radius = 3.0f;
            orbit.Height = 1.5f;
            orbit.Speed = 1.0f;
            auto& mr = e.Add<MeshRendererComponent>();
            mr.Geometry = m_Cube;
            mr.Color = lightColor;
            mr.Unlit = true;
        }

        // --- Second point light: cool blue, slower, orbiting the other way
        {
            const glm::vec3 c(0.4f, 0.6f, 1.0f);
            auto e = m_Scene.CreateEntity("Light 2");
            e.Get<TransformComponent>().Scale = glm::vec3(0.15f);
            e.Add<PointLightComponent>().Color = c;
            auto& orbit = e.Add<OrbitComponent>();
            orbit.Radius = 4.5f;
            orbit.Height = 2.5f;
            orbit.Speed = -0.6f;
            orbit.Angle = 3.14f;
            auto& mr = e.Add<MeshRendererComponent>();
            mr.Geometry = m_Cube;
            mr.Color = c;
            mr.Unlit = true;
        }

        // --- Sun: the marker's long axis shows which way the light travels (local -Z)
        {
            auto e = m_Scene.CreateEntity("Sun");
            auto& t = e.Get<TransformComponent>();
            t.Position = glm::vec3(0.0f, 6.0f, 0.0f);
            t.Rotation = glm::vec3(-0.9f, 0.5f, 0.0f);
            t.Scale = glm::vec3(0.12f, 0.12f, 0.6f);
            e.Add<DirectionalLightComponent>();
            auto& mr = e.Add<MeshRendererComponent>();
            mr.Geometry = m_Cube;
            mr.Color = glm::vec3(1.0f, 0.9f, 0.4f);
            mr.Unlit = true;
        }

        m_Framebuffer = std::make_unique<Framebuffer>(1280, 720);
        m_Post = std::make_unique<PostProcess>(1280, 720);
        m_Camera = std::make_unique<Camera>(60.0f, AspectRatio());
        m_Camera->Position = glm::vec3(0.0f, 1.0f, 6.0f);

        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
        LOG_INFO("Alt+LMB/MMB/RMB orbit/pan/zoom | W/E/R tools | N spawn | F frame | G delete last | F1 toggle UI | F5 reload shaders | ESC quits");
    }

    void OnUpdate(float dt) override {
        using namespace Engine;

        const bool typing = ImGui::GetIO().WantTextInput;

        if (!typing && Input::IsKeyDown(Key::Escape)) Close();

        bool f5 = EdgePressed(Key::F5, m_F5Down);
        bool f = EdgePressed(Key::F, m_FDown);
        bool g = EdgePressed(Key::G, m_GDown);
        bool f1 = EdgePressed(Key::F1, m_F1Down);
        bool kw = EdgePressed(Key::W, m_WDown);
        bool ke = EdgePressed(Key::E, m_EDown);
        bool kr = EdgePressed(Key::R, m_RDown);
        bool kq = EdgePressed(Key::Q, m_QDown);
        bool kn = EdgePressed(Key::N, m_NDown);
        bool kdel = EdgePressed(Key::Delete, m_DelDown);

        if (!typing) {
            if (f5) { m_Shader->Reload(); m_ShadowShader->Reload(); m_SunShader->Reload(); m_PostShader->Reload(); }
            if (kn) SpawnCube();
            if (f && !ImGui::GetIO().KeyAlt) FrameSelected();
            if (kdel && m_Selected != entt::null && m_Scene.Registry().valid(m_Selected)) {
                m_Scene.DestroyEntity(Engine::Entity(m_Selected, &m_Scene));
                m_Selected = entt::null;
            }
            if (g)  DeleteLastSpawned();
            if (f1) m_ShowUI = !m_ShowUI;
            if (!ImGui::GetIO().KeyAlt) {
                if (kq) m_SelectTool = true;
                if (kw) { m_SelectTool = false; m_GizmoOp = ImGuizmo::TRANSLATE; }
                if (ke) { m_SelectTool = false; m_GizmoOp = ImGuizmo::ROTATE; }
                if (kr) { m_SelectTool = false; m_GizmoOp = ImGuizmo::SCALE; }
            }
        }

        // Make the framebuffer match the Viewport panel (panel size comes from last frame's UI)
        if (m_ViewportSize.x >= 1.0f && m_ViewportSize.y >= 1.0f)
            m_Framebuffer->Resize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);

        m_Camera->SetAspect((float)m_Framebuffer->GetWidth() / (float)m_Framebuffer->GetHeight());
        // Maya-style navigation (gathered by the Viewport panel during the last UI pass)
        switch (m_NavMode) {
            case NavMode::Orbit: m_Camera->Orbit(m_NavDx, m_NavDy); break;
            case NavMode::Pan:   m_Camera->Pan(m_NavDx, m_NavDy, (float)m_Framebuffer->GetHeight()); break;
            case NavMode::Dolly: m_Camera->Dolly((m_NavDx - m_NavDy) * 0.01f); break;
            default: break;
        }
        if (m_NavWheel != 0.0f) m_Camera->Dolly(m_NavWheel * 0.15f);
        m_NavDx = m_NavDy = m_NavWheel = 0.0f;
        m_Camera->Refresh();

        m_Scene.OnUpdate(dt);

        // 1) Depth passes (all point lights + sun)
        m_Scene.RenderShadows(m_Shadows);

        // 2) Scene into the HDR framebuffer
        m_Framebuffer->Bind();
        m_Framebuffer->Clear(0.005f, 0.005f, 0.01f, 1.0f);
        m_Scene.Render(*m_Camera, *m_Shader, &m_Shadows);
        Framebuffer::Unbind();

        // 3) Exposure + tone map + gamma into the 8-bit texture the Viewport displays
        m_Post->Resize(m_Framebuffer->GetWidth(), m_Framebuffer->GetHeight());
        m_Post->Apply(m_Framebuffer->GetColorAttachment(), *m_PostShader, m_PostSettings);

        glViewport(0, 0, (int)GetWindow().GetWidth(), (int)GetWindow().GetHeight());
    }

    // ------------------------------------------------------------------ UI
    void OnImGui() override {
        ImGuiID dockspace = ImGui::DockSpaceOverViewport();   // full-window dock area
        BuildDefaultLayoutOnce(dockspace);

        DrawViewportPanel();
        if (m_ShowUI) {
            DrawToolbarPanel();
            DrawStatsPanel();
            DrawHierarchyPanel();
            DrawInspectorPanel();
        }
    }

    Engine::ResourceRegistry MakeResources() {
        using namespace Engine;
        ResourceRegistry r;
        r.GeometryName = [this](const std::shared_ptr<Renderable>& g) -> std::string {
            if (g == m_Cube) return "cube";
            if (g == m_Plane) return "plane";
            if (g == m_ModelResource) return "model";
            return "";
            };
        r.FindGeometry = [this](const std::string& n) -> std::shared_ptr<Renderable> {
            if (n == "cube") return m_Cube;
            if (n == "plane") return m_Plane;
            if (n == "model") return m_ModelResource;
            return nullptr;
            };
        r.TextureName = [this](const std::shared_ptr<Texture2D>& t) -> std::string {
            return t && t == m_Checker ? "checker" : "";
            };
        r.FindTexture = [this](const std::string& n) -> std::shared_ptr<Texture2D> {
            return n == "checker" ? m_Checker : nullptr;
            };
        return r;
    }

private:
    void DrawStatsPanel() {
        using namespace Engine;
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("Stats");

        ImGui::Text("%.0f FPS  (%.2f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
        ImGui::Text("Draw calls: %u", RenderStats::DrawCalls);
        ImGui::Text("Triangles:  %u", RenderStats::Triangles);
        ImGui::Text("Entities:   %zu", m_Scene.Registry().view<TagComponent>().size());
        ImGui::TextDisabled("%s", (const char*)glGetString(GL_RENDERER));

        bool vsync = GetWindow().IsVSync();
        if (ImGui::Checkbox("VSync", &vsync)) GetWindow().SetVSync(vsync);

        ImGui::Separator();
        ImGui::Text("Camera  (%.1f, %.1f, %.1f)", m_Camera->Position.x, m_Camera->Position.y, m_Camera->Position.z);
        ImGui::SliderFloat("Orbit sensitivity", &m_Camera->OrbitSensitivity, 0.05f, 1.0f);

        ImGui::Separator();
        ImGui::TextDisabled("Alt+mouse nav | Q/W/E/R tools | F frame | N spawn | Del delete");
        ImGui::End();
    }

    void DrawToolbarPanel() {
        using namespace Engine;
        ImGui::Begin("Tools");

        ImGui::TextUnformatted("Gizmo");
        if (ImGui::RadioButton("Select (Q)", m_SelectTool)) m_SelectTool = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("Move (W)", !m_SelectTool && m_GizmoOp == ImGuizmo::TRANSLATE)) { m_SelectTool = false; m_GizmoOp = ImGuizmo::TRANSLATE; }
        if (ImGui::RadioButton("Rotate (E)", !m_SelectTool && m_GizmoOp == ImGuizmo::ROTATE)) { m_SelectTool = false; m_GizmoOp = ImGuizmo::ROTATE; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Scale (R)", !m_SelectTool && m_GizmoOp == ImGuizmo::SCALE)) { m_SelectTool = false; m_GizmoOp = ImGuizmo::SCALE; }

        ImGui::Checkbox("Local space", &m_GizmoLocal);
        ImGui::Checkbox("Snap", &m_Snap);

        ImGui::Separator();
        ImGui::TextUnformatted("Shadows");
        auto& s = m_Shadows.Settings;
        ImGui::Checkbox("Enabled", &s.Enabled);
        ImGui::SliderFloat("Point bias", &s.PointBias, 0.005f, 0.3f);
        ImGui::SliderFloat("Point range", &s.PointFar, 5.0f, 100.0f);
        ImGui::SliderFloat("Sun bias", &s.SunBias, 0.0002f, 0.01f, "%.4f");
        ImGui::SliderFloat("Sun area", &s.SunExtent, 4.0f, 40.0f);

        ImGui::Separator();
        ImGui::TextUnformatted("Display");
        ImGui::SliderFloat("Exposure", &m_PostSettings.Exposure, 0.1f, 4.0f);
        const char* modes[] = { "None (clamp)", "Reinhard", "ACES" };
        ImGui::Combo("Tone map", &m_PostSettings.ToneMap, modes, 3);
        ImGui::Checkbox("Gamma correction", &m_PostSettings.Gamma);

        ImGui::Separator();
        ImGui::TextUnformatted("Scene");
        if (ImGui::Button("Save")) {
            SceneSerializer(m_Scene, MakeResources()).Save(m_ScenePath);
        }
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            m_Selected = entt::null;
            m_Spawned.clear();
            SceneSerializer(m_Scene, MakeResources()).Load(m_ScenePath);
        }
        ImGui::TextDisabled("%s", m_ScenePath.c_str());
        ImGui::End();
    }

    void DrawHierarchyPanel() {
        using namespace Engine;
        ImGui::SetNextWindowPos(ImVec2(10, 230), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300, 280), ImGuiCond_FirstUseEver);
        ImGui::Begin("Hierarchy");

        if (ImGui::Button("+ Cube")) {
            auto e = m_Scene.CreateEntity("Cube");
            auto& mr = e.Add<MeshRendererComponent>();
            mr.Geometry = m_Cube;
            e.Get<TransformComponent>().Position = m_Camera->Position + m_Camera->GetFront() * 3.0f;
            m_Selected = e.GetHandle();
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Light")) {
            auto e = m_Scene.CreateEntity("Point Light");
            auto& t = e.Get<TransformComponent>();
            t.Position = m_Camera->Position + m_Camera->GetFront() * 3.0f;
            t.Scale = glm::vec3(0.15f);
            e.Add<PointLightComponent>();
            auto& mr = e.Add<MeshRendererComponent>();
            mr.Geometry = m_Cube;
            mr.Unlit = true;
            m_Selected = e.GetHandle();
        }
        ImGui::Separator();

        auto& reg = m_Scene.Registry();
        for (auto handle : reg.view<TagComponent>()) {
            ImGui::PushID((int)entt::to_integral(handle));
            if (ImGui::Selectable(reg.get<TagComponent>(handle).Tag.c_str(), handle == m_Selected))
                m_Selected = handle;
            ImGui::PopID();
        }
        ImGui::End();
    }

    void DrawViewportPanel() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("Viewport");
        ImGui::PopStyleVar();

        ImVec2 avail = ImGui::GetContentRegionAvail();
        m_ViewportSize = avail;

        ImGui::Image((ImTextureID)(intptr_t)m_Post->GetOutput(),
            avail, ImVec2(0, 1), ImVec2(1, 0));

        const bool imageHovered = ImGui::IsItemHovered();
        const ImVec2 origin = ImGui::GetItemRectMin();
        const ImVec2 size = ImGui::GetItemRectSize();

        // ---- Maya-style navigation: Alt + LMB tumble / MMB track / RMB dolly, wheel dollies
        ImGuiIO& io = ImGui::GetIO();
        const bool alt = io.KeyAlt;

        if (m_NavMode == NavMode::None) {
            if (alt && imageHovered && !m_MouseLook) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))   m_NavMode = NavMode::Orbit;
                else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) m_NavMode = NavMode::Pan;
                else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))  m_NavMode = NavMode::Dolly;
            }
        }
        else {
            // Once started, the drag continues until the button is released (Alt can be let go)
            int button = m_NavMode == NavMode::Orbit ? ImGuiMouseButton_Left
                : m_NavMode == NavMode::Pan ? ImGuiMouseButton_Middle
                : ImGuiMouseButton_Right;
            if (ImGui::IsMouseDown(button)) {
                m_NavDx += io.MouseDelta.x;
                m_NavDy += io.MouseDelta.y;
            }
            else {
                m_NavMode = NavMode::None;
            }
        }
        if (imageHovered && !m_MouseLook) m_NavWheel += io.MouseWheel;

        const bool navigating = alt || m_NavMode != NavMode::None;

        // ---- Gizmo for the selected entity
        bool gizmoBusy = false;
        if (!m_SelectTool && m_Selected != entt::null && m_Scene.Registry().valid(m_Selected) && !m_MouseLook) {
            Engine::Entity e(m_Selected, &m_Scene);
            auto& t = e.Get<Engine::TransformComponent>();

            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist();
            ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);

            glm::mat4 view = m_Camera->GetView();
            glm::mat4 proj = m_Camera->GetProjection();
            glm::mat4 model = t.GetMatrix();

            float snap = m_GizmoOp == ImGuizmo::ROTATE ? 15.0f : (m_GizmoOp == ImGuizmo::SCALE ? 0.1f : 0.5f);
            float snapValues[3] = { snap, snap, snap };

            ImGuizmo::Enable(!navigating);
            ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), m_GizmoOp,
                m_GizmoLocal ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
                glm::value_ptr(model), nullptr, m_Snap ? snapValues : nullptr);

            if (ImGuizmo::IsUsing()) {
                glm::vec3 pos, rotDeg, scale;
                ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(model),
                    &pos.x, &rotDeg.x, &scale.x);
                t.Position = pos;
                t.Rotation = glm::radians(rotDeg);
                t.Scale = scale;
            }
            gizmoBusy = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
        }

        // ---- Click to select (not while interacting with the gizmo)
        if (imageHovered && !gizmoBusy && !navigating && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_MouseLook) {
            ImVec2 mouse = ImGui::GetMousePos();
            int px = (int)(mouse.x - origin.x);
            int py = (int)m_Framebuffer->GetHeight() - 1 - (int)(mouse.y - origin.y);

            int id = m_Framebuffer->ReadEntityID(px, py);
            entt::entity picked = id < 0 ? entt::null : static_cast<entt::entity>(id);
            m_Selected = (picked != entt::null && m_Scene.Registry().valid(picked)) ? picked : entt::null;
        }
        ImGui::End();
    }

    void BuildDefaultLayoutOnce(ImGuiID dockspace) {
        if (m_LayoutChecked) return;
        m_LayoutChecked = true;
        if (std::filesystem::exists("imgui.ini")) return;

        ImGui::DockBuilderRemoveNode(dockspace);
        ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);

        ImGuiID center = dockspace;
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
        ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.60f, nullptr, &left);

        ImGui::DockBuilderDockWindow("Stats", left);
        ImGui::DockBuilderDockWindow("Hierarchy", leftBottom);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderFinish(dockspace);
    }

    void DrawInspectorPanel() {
        using namespace Engine;
        ImGui::SetNextWindowPos(ImVec2(940, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 520), ImGuiCond_FirstUseEver);
        ImGui::Begin("Inspector");

        if (m_Selected == entt::null || !m_Scene.Registry().valid(m_Selected)) {
            m_Selected = entt::null;
            ImGui::TextDisabled("Nothing selected");
            ImGui::End();
            return;
        }

        Entity e(m_Selected, &m_Scene);
        bool deleteRequested = false;

        // Name
        {
            auto& tag = e.Get<TagComponent>();
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", tag.Tag.c_str());
            if (ImGui::InputText("Name", buf, sizeof(buf))) tag.Tag = buf;
        }

        // Transform
        if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& t = e.Get<TransformComponent>();
            ImGui::DragFloat3("Position", &t.Position.x, 0.05f);

            glm::vec3 deg = glm::degrees(t.Rotation);
            if (ImGui::DragFloat3("Rotation", &deg.x, 0.5f)) t.Rotation = glm::radians(deg);

            ImGui::DragFloat3("Scale", &t.Scale.x, 0.02f, 0.01f, 100.0f);
            if (e.Has<SpinComponent>())  ImGui::TextDisabled("Rotation is driven by Spin");
            if (e.Has<OrbitComponent>()) ImGui::TextDisabled("Position is driven by Orbit");
        }

        if (e.Has<MeshRendererComponent>() &&
            ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& mr = e.Get<MeshRendererComponent>();
            ImGui::ColorEdit3("Color", &mr.Color.x);
            ImGui::SliderFloat("Shininess", &mr.Shininess, 2.0f, 256.0f);
            ImGui::SliderFloat("Specular", &mr.SpecularStrength, 0.0f, 2.0f);
            ImGui::Checkbox("Unlit", &mr.Unlit);
            if (mr.Geometry && mr.Geometry->GetSubmeshMaterial(0)) {
                ImGui::Checkbox("Use model materials", &mr.UseModelMaterials);
                if (mr.UseModelMaterials) {
                    ImGui::TextDisabled("Color tints the model's own materials");
                    if (ImGui::TreeNode("Materials")) {
                        for (uint32_t i = 0; i < mr.Geometry->SubmeshCount(); i++) {
                            const Material* mat = mr.Geometry->GetSubmeshMaterial(i);
                            ImGui::BulletText("%s%s", mat->Name.empty() ? "(unnamed)" : mat->Name.c_str(),
                                mat->Albedo ? "  [textured]" : "");
                        }
                        ImGui::TreePop();
                    }
                }
            }
            bool hasTex = mr.Texture != nullptr;
            if (ImGui::Checkbox("Checker texture", &hasTex)) mr.Texture = hasTex ? m_Checker : nullptr;
        }

        if (e.Has<PointLightComponent>() &&
            ImGui::CollapsingHeader("Point Light", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& l = e.Get<PointLightComponent>();
            ImGui::Checkbox("Casts shadows", &l.CastShadows);
            ImGui::ColorEdit3("Light color", &l.Color.x);
            ImGui::SliderFloat("Intensity", &l.Intensity, 0.0f, 5.0f);
        }

        if (e.Has<DirectionalLightComponent>() &&
            ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& d = e.Get<DirectionalLightComponent>();
            ImGui::ColorEdit3("Sun color", &d.Color.x);
            ImGui::SliderFloat("Sun intensity", &d.Intensity, 0.0f, 3.0f);
            ImGui::Checkbox("Casts shadows", &d.CastShadows);
            ImGui::TextDisabled("Aim with Rotate (E). Light travels along local -Z.");
        }

        if (e.Has<SpinComponent>() &&
            ImGui::CollapsingHeader("Spin", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::DragFloat3("Speed (rad/s)", &e.Get<SpinComponent>().Speed.x, 0.02f);
            if (ImGui::Button("Remove Spin")) e.Remove<SpinComponent>();
        }

        if (e.Has<OrbitComponent>() &&
            ImGui::CollapsingHeader("Orbit", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& o = e.Get<OrbitComponent>();
            ImGui::DragFloat3("Center", &o.Center.x, 0.05f);
            ImGui::SliderFloat("Radius", &o.Radius, 0.0f, 10.0f);
            ImGui::SliderFloat("Height", &o.Height, -5.0f, 10.0f);
            ImGui::SliderFloat("Orbit speed", &o.Speed, -5.0f, 5.0f);
            if (ImGui::Button("Remove Orbit")) e.Remove<OrbitComponent>();
        }

        ImGui::Separator();
        if (ImGui::Button("Add Component")) ImGui::OpenPopup("AddComponent");
        if (ImGui::BeginPopup("AddComponent")) {
            if (!e.Has<SpinComponent>() && ImGui::MenuItem("Spin"))        e.Add<SpinComponent>().Speed = glm::vec3(0.0f, 1.0f, 0.0f);
            if (!e.Has<OrbitComponent>() && ImGui::MenuItem("Orbit"))       e.Add<OrbitComponent>();
            if (!e.Has<PointLightComponent>() && ImGui::MenuItem("Point Light")) e.Add<PointLightComponent>();
            if (!e.Has<MeshRendererComponent>() && ImGui::MenuItem("Mesh Renderer (cube)"))
                e.Add<MeshRendererComponent>().Geometry = m_Cube;
            if (!e.Has<DirectionalLightComponent>() && ImGui::MenuItem("Directional Light")) e.Add<DirectionalLightComponent>();
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Delete Entity")) deleteRequested = true;

        ImGui::End();

        // Delete after the UI is built so we never destroy something mid-draw
        if (deleteRequested) {
            m_Scene.DestroyEntity(e);
            m_Selected = entt::null;
        }
    }

    // ------------------------------------------------------------ helpers
    // True only on the frame the key goes from up to down
    static bool EdgePressed(int key, bool& wasDown) {
        bool down = Engine::Input::IsKeyDown(key);
        bool pressed = down && !wasDown;
        wasDown = down;
        return pressed;
    }

    // World-space box of an entity (its mesh box transformed by its matrices)
    void GetWorldBounds(entt::entity handle, glm::vec3& mn, glm::vec3& mx) {
        using namespace Engine;
        Entity e(handle, &m_Scene);
        auto& t = e.Get<TransformComponent>();

        if (e.Has<MeshRendererComponent>() && e.Get<MeshRendererComponent>().Geometry) {
            auto& mr = e.Get<MeshRendererComponent>();
            glm::vec3 lo, hi;
            mr.Geometry->GetLocalBounds(lo, hi);
            glm::mat4 m = t.GetMatrix() * mr.LocalOffset;

            mn = glm::vec3(FLT_MAX);
            mx = glm::vec3(-FLT_MAX);
            for (int i = 0; i < 8; i++) {
                glm::vec3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
                glm::vec3 w = glm::vec3(m * glm::vec4(corner, 1.0f));
                mn = glm::min(mn, w);
                mx = glm::max(mx, w);
            }
        }
        else {
            mn = t.Position - glm::vec3(0.5f);   // lights, empties
            mx = t.Position + glm::vec3(0.5f);
        }
    }

    void FrameSelected() {
        using namespace Engine;
        glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
        auto& reg = m_Scene.Registry();

        if (m_Selected != entt::null && reg.valid(m_Selected)) {
            GetWorldBounds(m_Selected, mn, mx);
        }
        else {
            bool any = false;
            for (auto h : reg.view<TagComponent>()) {
                glm::vec3 a, b;
                GetWorldBounds(h, a, b);
                mn = glm::min(mn, a);
                mx = glm::max(mx, b);
                any = true;
            }
            if (!any) return;
        }

        glm::vec3 center = (mn + mx) * 0.5f;
        float radius = std::max(glm::length(mx - mn) * 0.5f, 0.1f);
        m_Camera->Focus(center, radius);
    }

    float Rand(float lo, float hi) {
        return std::uniform_real_distribution<float>(lo, hi)(m_Rng);
    }

    void SpawnCube() {
        using namespace Engine;
        auto e = m_Scene.CreateEntity("Spawned Cube");
        auto& t = e.Get<TransformComponent>();
        t.Position = m_Camera->Position + m_Camera->GetFront() * 3.0f;
        t.Scale = glm::vec3(0.5f);

        auto& mr = e.Add<MeshRendererComponent>();
        mr.Geometry = m_Cube;
        mr.Color = glm::vec3(Rand(0.3f, 1.0f), Rand(0.3f, 1.0f), Rand(0.3f, 1.0f));

        e.Add<SpinComponent>().Speed = glm::vec3(Rand(-2.0f, 2.0f), Rand(0.5f, 2.0f), Rand(-2.0f, 2.0f));

        m_Spawned.push_back(e);
        m_Selected = e.GetHandle();
    }

    void DeleteLastSpawned() {
        // Skip any that were already deleted from the inspector
        while (!m_Spawned.empty() && !m_Spawned.back().IsValid()) m_Spawned.pop_back();
        if (m_Spawned.empty()) return;
        if (m_Spawned.back().GetHandle() == m_Selected) m_Selected = entt::null;
        m_Scene.DestroyEntity(m_Spawned.back());
        m_Spawned.pop_back();
    }

    std::shared_ptr<Engine::Texture2D> CreateCheckerTexture() {
        const uint32_t size = 256, cell = 32;
        std::vector<uint8_t> pixels(size * size * 4);
        for (uint32_t y = 0; y < size; y++) {
            for (uint32_t x = 0; x < size; x++) {
                bool light = ((x / cell) + (y / cell)) % 2 == 0;
                uint8_t c = light ? 230 : 60;
                uint8_t* p = &pixels[(y * size + x) * 4];
                p[0] = c; p[1] = c; p[2] = light ? 255 : c; p[3] = 255;
            }
        }
        return std::make_shared<Engine::Texture2D>(size, size, pixels.data());
    }

    float AspectRatio() {
        auto& w = GetWindow();
        return w.GetHeight() ? (float)w.GetWidth() / (float)w.GetHeight() : 1.0f;
    }

    Engine::Scene m_Scene;
    std::unique_ptr<Engine::Shader> m_Shader;
    std::unique_ptr<Engine::Camera> m_Camera;
    std::unique_ptr<Engine::Framebuffer> m_Framebuffer;
    std::shared_ptr<Engine::Mesh> m_Cube;
    std::shared_ptr<Engine::Texture2D> m_Checker;
    std::shared_ptr<Engine::Mesh>  m_Plane;
    std::shared_ptr<Engine::Model> m_ModelResource;
    std::vector<Engine::Entity> m_Spawned;
    entt::entity m_Selected = entt::null;
    std::mt19937 m_Rng{ std::random_device{}() };
    std::string m_ScenePath = std::string(ASSET_DIR) + "Scenes/scene.json";
    std::unique_ptr<Engine::PointShadowArray> m_PointShadows;
    std::unique_ptr<Engine::DirectionalShadowMap> m_SunShadowMap;
    std::unique_ptr<Engine::Shader> m_SunShader;
    Engine::ShadowContext m_Shadows;
    std::unique_ptr<Engine::Shader> m_ShadowShader;
    std::unique_ptr<Engine::PostProcess> m_Post;
    std::unique_ptr<Engine::Shader> m_PostShader;
    Engine::PostSettings m_PostSettings;

    ImGuizmo::OPERATION m_GizmoOp = ImGuizmo::TRANSLATE;

    ImVec2 m_ViewportSize{ 0.0f, 0.0f };                  
    bool m_LayoutChecked = false;      
    bool m_GizmoLocal = false;
    bool m_Snap = false;
    bool m_WDown = false, m_EDown = false, m_RDown = false;
    bool m_SelectTool = false;
    bool m_QDown = false, m_NDown = false, m_DelDown = false;

    bool m_F5Down = false, m_FDown = false, m_GDown = false, m_F1Down = false, m_TabDown = false;
    bool m_MouseLook = false, m_FirstMouse = true, m_ShowUI = true;
    float m_LastX = 0, m_LastY = 0;

    enum class NavMode { None, Orbit, Pan, Dolly };
    NavMode m_NavMode = NavMode::None;
    float m_NavDx = 0.0f, m_NavDy = 0.0f, m_NavWheel = 0.0f;
};

std::unique_ptr<Engine::Application> Engine::CreateApplication() {
    return std::make_unique<SandboxApp>();
}