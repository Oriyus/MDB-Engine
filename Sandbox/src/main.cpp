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
#include "Renderer/Environment.h"
#include "Renderer/SSAO.h"
#include "Renderer/Bloom.h"
#include "Physics/PhysicsWorld.h"
#include "Scripting/ScriptEngine.h"
#include "Audio/AudioEngine.h"
#include "Scene/Prefab.h"
#include "Renderer/SceneRenderer.h"
#include "Runtime/ResourceLibrary.h"
#include "Runtime/GameSession.h"
#include "Runtime/GameUI.h"
#include "DemoLevels.h"
#include "EditorHistory.h"

#include <cfloat>
#include <glad/glad.h>
#include <imgui.h>              // must come BEFORE ImGuizmo.h
#include <imgui_internal.h>     // DockBuilder (default layout)
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <vector>
#include <algorithm>

class SandboxApp : public Engine::Application {
public:
    SandboxApp() : Application("Sandbox") {}

    void OnStart() override {
        using namespace Engine;

        // Shared engine pieces: named resources, the render pipeline, and the play-mode systems
        m_Resources = std::make_unique<ResourceLibrary>(std::string(ASSET_DIR));
        m_Renderer = std::make_unique<SceneRenderer>(std::string(ASSET_DIR));
        m_Session = std::make_unique<GameSession>(m_Scene, std::string(ASSET_DIR), m_Resources->Registry());

        m_Cube = m_Resources->Cube();
        m_Plane = m_Resources->Plane();
        m_Checker = m_Resources->Checker();
        EnsureSphere();

        // --- Floor
        {
            auto e = m_Scene.CreateEntity("Floor");
            e.Get<TransformComponent>().Position = glm::vec3(0.0f, -1.0f, 0.0f);
            auto& mr = e.Add<MeshRendererComponent>();
            mr.Geometry = m_Plane;
            mr.Texture = m_Checker;
            mr.Color = glm::vec3(0.8f);
            mr.Roughness = 0.85f;

            auto& rb = e.Add<RigidBodyComponent>();
            rb.Type = BodyType::Static;
            rb.Size = glm::vec3(12.0f, 1.0f, 12.0f);
            rb.Center = glm::vec3(0.0f, -0.5f, 0.0f);
        }

        // --- Spinning cube
        {
            auto e = m_Scene.CreateEntity("Cube");
            e.Get<TransformComponent>().Position = glm::vec3(-2.5f, -0.5f, 0.0f);
            auto& mr = e.Add<MeshRendererComponent>();
            mr.Geometry = m_Cube;
            mr.Texture = m_Checker;
            e.Add<SpinComponent>().Speed = glm::vec3(0.0f, 0.6f, 0.0f);
            e.Add<ScriptComponent>().Path = "Scripts/bob.lua";   // hops up and down while playing

            auto& rb = e.Add<RigidBodyComponent>();
            rb.Type = BodyType::Kinematic;
        }

        // --- Loaded model
        {
            m_ModelResource = m_Resources->SampleModel();
            auto model = m_ModelResource;
            if (model->IsValid()) {
                auto e = m_Scene.CreateEntity("Model");
                auto& mr = e.Add<MeshRendererComponent>();
                mr.Geometry = model;
                mr.LocalOffset = model->GetNormalizeMatrix(2.0f);
                mr.UseModelMaterials = model->HasTexturedMaterials();
                mr.Color = mr.UseModelMaterials ? glm::vec3(1.0f) : glm::vec3(0.85f, 0.6f, 0.35f);
                e.Add<SpinComponent>().Speed = glm::vec3(0.0f, 0.3f, 0.0f);

                auto& rb = e.Add<RigidBodyComponent>();
                rb.Type = BodyType::Kinematic;
                FitCollider(mr, rb);
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

        m_Camera = std::make_unique<Camera>(60.0f, AspectRatio());
        m_GameCamera = std::make_unique<Camera>(60.0f, AspectRatio());
        m_GameFont = LoadGameFont();   // before the first ImGui frame builds the font atlas
        m_Camera->Position = glm::vec3(0.0f, 1.0f, 6.0f);

        // Test hooks, for running the app unattended:
        //   MDB_DEMO=script|coinrush builds that example level, MDB_AUTOPLAY=1 presses Play, MDB_QUIT_AFTER=<s> closes
        //   MDB_TEST_SCRIPT=Scripts/x.lua adds an entity "Test" running that script
        //   MDB_SCENE=<file> loads Assets/Scenes/<file> first, MDB_SAVE_SCENE=<file> saves after the rest
        if (auto sc = EnvVar("MDB_SCENE"); !sc.empty()) LoadScene(sc);
        if (auto demo = EnvVar("MDB_DEMO"); demo == "1" || demo == "script") BuildLevel(BuildScriptDemo);
        else if (demo == "coinrush") BuildLevel(BuildCoinRush);
        if (auto ts = EnvVar("MDB_TEST_SCRIPT"); !ts.empty())
            m_Scene.CreateEntity("Test").Add<ScriptComponent>().Path = ts;
        if (auto sv = EnvVar("MDB_SAVE_SCENE"); !sv.empty()) SaveScene(sv);
        if (EnvVar("MDB_AUTOPLAY") == "1") m_PlayCmd = PlayCmd::Play;
        if (auto q = EnvVar("MDB_QUIT_AFTER"); !q.empty()) m_QuitAfter = std::strtof(q.c_str(), nullptr);

        m_History.Reset(m_Scene, m_Selected, m_Resources->Registry());   // the starting point for undo

        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
        LOG_INFO("Alt+LMB/MMB/RMB orbit/pan/zoom | W/E/R tools | N spawn | F frame | Ctrl+Z/Y undo/redo | Ctrl+D duplicate | Ctrl+S save | F5 reload shaders | ESC quits");
    }

    void OnUpdate(float dt) override {
        using namespace Engine;

        if (!m_PendingSceneLoad.empty()) {   // a requested environment bakes in this frame's Render
            LoadScene(m_PendingSceneLoad);
            m_PendingSceneLoad.clear();
        }

        // Undo history: once an interaction is over (nothing held or being edited), record the
        // scene if it changed. A whole drag or typed name becomes one step this way.
        const bool interacting = ImGui::IsAnyItemActive() || m_GizmoBusy
            || ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right)
            || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        if (m_PlayState == PlayState::Edit && m_MaybeChanged && !interacting) {
            m_History.Track(m_Scene, m_Selected, m_Resources->Registry());
            m_MaybeChanged = false;
        }
        UpdateTitle();

        dt = std::min(dt, 0.1f);   // a long hitch (like baking an environment) shouldn't fling the simulation

        // Test hook: MDB_QUIT_AFTER=<seconds> closes the app on its own
        m_RunTime += dt;
        if (m_QuitAfter > 0.0f && m_RunTime >= m_QuitAfter) Close();

        switch (m_PlayCmd) {
        case PlayCmd::Play:
            if (m_PlayState == PlayState::Edit) { StartPlay(); m_Session->Start(); }
            break;
        case PlayCmd::Stop:   // the session first: Restore invalidates every entity handle
            if (m_PlayState != PlayState::Edit) { m_Session->Stop(); StopPlay(); }
            break;
        case PlayCmd::Pause:
            if (m_PlayState == PlayState::Play)        m_PlayState = PlayState::Paused;
            else if (m_PlayState == PlayState::Paused) m_PlayState = PlayState::Play;
            break;
        case PlayCmd::Step:
            if (m_PlayState == PlayState::Paused) m_StepFrame = true;
            break;
        case PlayCmd::Restart:   // scene.restart() from a script: Stop, then Play again
            if (m_PlayState != PlayState::Edit) {
                m_Session->Stop(); StopPlay();
                StartPlay(); m_Session->Start();
            }
            break;
        default: break;
        }
        m_PlayCmd = PlayCmd::None;
        m_Session->Audio().SetPaused(m_PlayState == PlayState::Paused);

        if (m_ReloadScripts) {
            m_ReloadScripts = false;
            if (m_PlayState != PlayState::Edit) m_Session->Scripts().Reload(m_Scene);
        }

        const bool typing = ImGui::GetIO().WantTextInput;

        bool kesc = EdgePressed(Key::Escape, m_EscDown);
        bool kp = EdgePressed(Key::P, m_PDown);
        if (!typing && kesc) {
            if (m_PlayState != PlayState::Edit) m_PlayCmd = PlayCmd::Stop;
            else RequestQuit();
        }

        // Ctrl shortcuts (Edit mode, not while typing or in the middle of a drag)
        const bool ctrl = ImGui::GetIO().KeyCtrl, shift = ImGui::GetIO().KeyShift;
        const bool kz = EdgePressed(Key::Z, m_ZDown), ky = EdgePressed(Key::Y, m_YDown);
        const bool kd = EdgePressed(Key::D, m_DDown), ks = EdgePressed(Key::S, m_SDown);
        if (ctrl && !typing && !interacting && m_PlayState == PlayState::Edit) {
            if (kz && !shift) UndoStep();
            if (ky || (kz && shift)) RedoStep();
            if (kd) DuplicateSelected();
            if (ks) SaveScene(m_SceneFile);
        }

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
        bool kt = EdgePressed(Key::T, m_TDown);
        if (kp && ImGui::GetIO().KeyCtrl)
            m_PlayCmd = (m_PlayState == PlayState::Edit) ? PlayCmd::Play : PlayCmd::Stop;

        // Looking through a game camera, the game owns the keyboard: only Esc, Ctrl+P, F1 and F5 stay
        const bool editorKeys = !typing && !m_ViewingGame;
        if (!editorKeys) kn = f = g = kw = ke = kr = kq = kdel = kt = false;

        if (!typing) {
            if (f5) {
                m_Renderer->ReloadShaders();
                if (m_PlayState != PlayState::Edit) m_ReloadScripts = true;   // applied next frame
            }
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
            if (kt && m_PlayState == PlayState::Play)
                SpawnBody(true, m_Camera->Position + m_Camera->GetFront() * 1.5f, m_Camera->GetFront() * 14.0f, false);
        }

        // Render at the Viewport panel's size (from last frame's UI)
        const uint32_t vw = (uint32_t)std::max(m_ViewportSize.x, 1.0f), vh = (uint32_t)std::max(m_ViewportSize.y, 1.0f);
        const float aspect = (float)vw / (float)vh;

        m_Camera->SetAspect(aspect);
        // Maya-style navigation (gathered by the Viewport panel during the last UI pass)
        switch (m_NavMode) {
            case NavMode::Orbit: m_Camera->Orbit(m_NavDx, m_NavDy); break;
            case NavMode::Pan:   m_Camera->Pan(m_NavDx, m_NavDy, (float)vh); break;
            case NavMode::Dolly: m_Camera->Dolly((m_NavDx - m_NavDy) * 0.01f); break;
            default: break;
        }
        if (m_NavWheel != 0.0f) m_Camera->Dolly(m_NavWheel * 0.15f);
        m_NavDx = m_NavDy = m_NavWheel = 0.0f;
        m_Camera->Refresh();

        const bool simulate = m_PlayState == PlayState::Play || m_StepFrame;
        if (simulate) {
            m_Session->Update(dt, m_StepFrame);
            if (m_Session->ConsumeRestartRequest()) m_PlayCmd = PlayCmd::Restart;   // next frame
        }
        else {
            m_Scene.UpdateOrbitPositions();
        }
        m_StepFrame = false;

        // During Play, look through the scene's game camera (after scripts moved it this frame)
        m_ViewingGame = m_PlayState != PlayState::Edit && m_PlayThroughGameCamera
            && m_Session->UpdateGameCamera(*m_GameCamera, aspect);
        Camera& view = ViewCamera();
        m_Session->SetListener(view);

        m_Renderer->Render(m_Scene, view, vw, vh);

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
        DrawUnsavedChangesModal();

        // Anything that may have changed the scene: check it once the interaction is over
        if (ImGui::IsAnyItemActive() || m_GizmoBusy) m_MaybeChanged = true;
        for (int b = 0; b < 3; b++) if (ImGui::IsMouseReleased(b)) m_MaybeChanged = true;
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
            if (ImGui::IsKeyPressed((ImGuiKey)k, false)) { m_MaybeChanged = true; break; }
    }

    // ------------------------------------------------------------------ undo / redo, unsaved changes
    void UndoStep() {
        if (!m_History.CanUndo()) return;
        LOG_INFO("Undo: {}", m_History.UndoLabel());
        m_Selected = m_History.Undo(m_Scene);
        m_Spawned.clear();          // those handles died with the restore
        m_MaybeChanged = false;
    }

    void RedoStep() {
        if (!m_History.CanRedo()) return;
        LOG_INFO("Redo: {}", m_History.RedoLabel());
        m_Selected = m_History.Redo(m_Scene);
        m_Spawned.clear();
        m_MaybeChanged = false;
    }

    void DuplicateSelected() {
        if (m_Selected == entt::null || !m_Scene.Registry().valid(m_Selected)) return;
        Engine::Entity copy = m_Scene.DuplicateEntity(Engine::Entity(m_Selected, &m_Scene));
        m_Selected = copy.GetHandle();
        m_History.Track(m_Scene, m_Selected, m_Resources->Registry(), "Duplicate " + copy.Get<Engine::TagComponent>().Tag);
    }

    // "Sandbox - coin_rush.json *": the star means unsaved changes
    void UpdateTitle() {
        std::string title = "Sandbox - " + m_SceneFile + (m_History.IsDirty() ? " *" : "");
        if (title != m_Title) { m_Title = title; SetTitle(title); }
    }

    // Quit (Esc in Edit mode or the window's close button): ask first if there are unsaved changes
    void RequestQuit() {
        if (m_History.IsDirty()) m_Confirm = ConfirmAction::Quit;
        else Close();
    }

    bool OnCloseRequested() override {
        if (m_QuitConfirmed || !m_History.IsDirty()) return true;
        m_Confirm = ConfirmAction::Quit;
        return false;
    }

    void RequestLoad(const std::string& file) {
        if (m_History.IsDirty()) { m_Confirm = ConfirmAction::Load; m_ConfirmFile = file; }
        else m_PendingSceneLoad = file;
    }

    void DrawUnsavedChangesModal() {
        if (m_Confirm != ConfirmAction::None && !ImGui::IsPopupOpen("Unsaved changes")) ImGui::OpenPopup("Unsaved changes");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

        ImGui::Text("%s has unsaved changes.", m_SceneFile.c_str());
        ImGui::TextDisabled(m_Confirm == ConfirmAction::Quit ? "Save them before quitting?" : "Save them before loading %s?",
            m_ConfirmFile.c_str());
        ImGui::Spacing();
        auto proceed = [this] {
            if (m_Confirm == ConfirmAction::Quit) { m_QuitConfirmed = true; Close(); }
            else if (m_Confirm == ConfirmAction::Load) m_PendingSceneLoad = m_ConfirmFile;
            m_Confirm = ConfirmAction::None;
            ImGui::CloseCurrentPopup();
        };
        if (ImGui::Button("Save", ImVec2(110, 0))) { if (SaveScene(m_SceneFile)) proceed(); }
        ImGui::SameLine();
        if (ImGui::Button("Don't save", ImVec2(110, 0))) proceed();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_Confirm = ConfirmAction::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void SpawnMaterialGrid() {
        using namespace Engine;
        EnsureSphere();

        // Rows: metallic 0 -> 1 (top to bottom). Columns: roughness 0.05 -> 1 (left to right).
        const int n = 5;
        for (int row = 0; row < n; row++) {
            for (int col = 0; col < n; col++) {
                auto e = m_Scene.CreateEntity("Sphere");
                e.Get<TransformComponent>().Position = glm::vec3((col - 2) * 1.3f, 2.0f + (2 - row) * 1.3f, -3.5f);
                auto& mr = e.Add<MeshRendererComponent>();
                mr.Geometry = m_Sphere;
                mr.Color = glm::vec3(0.9f, 0.55f, 0.25f);    // copper-ish
                mr.Metallic = row / float(n - 1);
                mr.Roughness = 0.05f + 0.95f * col / float(n - 1);
            }
        }
    }

    void StartPlay() {
        if (m_PlayState != PlayState::Edit) return;
        m_Snapshot = m_Scene.Capture(m_Selected, m_SnapshotSelected);
        m_PlayState = PlayState::Play;
    }

    void StopPlay() {
        if (m_PlayState == PlayState::Edit) return;
        m_Selected = m_Scene.Restore(m_Snapshot, m_SnapshotSelected);
        m_Snapshot.clear();
        m_Spawned.clear();            // those handles died with the old registry
        m_PlayState = PlayState::Edit;
    }

    // Size the collider to the mesh's bounds
    static void FitCollider(const Engine::MeshRendererComponent& mr, Engine::RigidBodyComponent& rb) {
        if (!mr.Geometry) return;
        glm::vec3 lo, hi;
        mr.Geometry->GetLocalBounds(lo, hi);
        glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
        for (int i = 0; i < 8; i++) {
            glm::vec3 c((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
            glm::vec3 w = glm::vec3(mr.LocalOffset * glm::vec4(c, 1.0f));
            mn = glm::min(mn, w);
            mx = glm::max(mx, w);
        }
        rb.Size = mx - mn;
        rb.Center = (mn + mx) * 0.5f;
        if (rb.Shape == Engine::ColliderShape::Sphere) rb.Size = glm::vec3(std::max({ rb.Size.x, rb.Size.y, rb.Size.z }));
    }

    void EnsureSphere() { if (!m_Sphere) m_Sphere = m_Resources->Sphere(); }

    Engine::Entity SpawnBody(bool sphere, const glm::vec3& pos, const glm::vec3& velocity, bool select) {
        using namespace Engine;
        if (sphere) EnsureSphere();
        auto e = m_Scene.CreateEntity(sphere ? "Ball" : "Box");
        auto& t = e.Get<TransformComponent>();
        t.Position = pos;
        t.Scale = glm::vec3(0.5f);

        auto& mr = e.Add<MeshRendererComponent>();
        mr.Geometry = sphere ? std::shared_ptr<Renderable>(m_Sphere) : std::shared_ptr<Renderable>(m_Cube);
        mr.Color = glm::vec3(Rand(0.3f, 1.0f), Rand(0.3f, 1.0f), Rand(0.3f, 1.0f));
        mr.Roughness = 0.4f;

        auto& rb = e.Add<RigidBodyComponent>();
        rb.Shape = sphere ? ColliderShape::Sphere : ColliderShape::Box;
        rb.Restitution = sphere ? 0.5f : 0.2f;
        rb.InitialVelocity = velocity;

        if (select) m_Selected = e.GetHandle();
        return e;
    }

    // Sound files under Assets/Sounds, as clip paths ("Sounds/x.wav")
    static std::vector<std::string> ListSoundFiles() {
        std::vector<std::string> files;
        std::error_code ec;
        for (const auto& f : std::filesystem::directory_iterator(std::filesystem::path(ASSET_DIR) / "Sounds", ec)) {
            const auto ext = f.path().extension().string();
            if (ext == ".wav" || ext == ".mp3" || ext == ".flac") files.push_back("Sounds/" + f.path().filename().string());
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    // ------------------------------------------------------------ scene files
    static std::string ScenePath(const std::string& file) { return std::string(ASSET_DIR) + "Scenes/" + file; }

    std::vector<std::string> ListSceneFiles() const {
        std::vector<std::string> files;
        std::error_code ec;
        for (const auto& f : std::filesystem::directory_iterator(std::filesystem::path(ASSET_DIR) / "Scenes", ec))
            if (f.path().extension() == ".json") files.push_back(f.path().filename().string());
        std::sort(files.begin(), files.end());
        return files;
    }

    Engine::SceneSettings GatherSettings() const {
        Engine::SceneSettings s;
        m_Renderer->GatherSettings(s);           // environment (incl. a pending one), sky, post, AO, bloom, shadows
        s.Gravity = m_Gravity;
        s.CameraPosition = m_Camera->Position;
        s.CameraYaw = m_Camera->GetYaw();
        s.CameraPitch = m_Camera->GetPitch();
        s.CameraFocusDistance = m_Camera->FocusDistance;
        return s;
    }

    void ApplySettings(const Engine::SceneSettings& s) {
        m_Renderer->ApplySettings(s);            // a new environment bakes at the start of the next Render
        m_Gravity = s.Gravity;
        m_Session->Physics().SetGravity(m_Gravity);
        m_Camera->Position = s.CameraPosition;
        m_Camera->FocusDistance = s.CameraFocusDistance;
        m_Camera->SetOrientation(s.CameraYaw, s.CameraPitch);
    }

    bool SaveScene(const std::string& file) {
        if (m_PlayState != PlayState::Edit) { LOG_WARN("Stop Play before saving the scene"); return false; }
        const Engine::SceneSettings s = GatherSettings();
        if (!Engine::SceneSerializer(m_Scene, m_Resources->Registry(), &m_Session->Prefabs()).Save(ScenePath(file), &s))
            return false;
        m_History.Track(m_Scene, m_Selected, m_Resources->Registry());   // an edit not recorded yet is part of what was saved
        m_History.MarkSaved();
        m_SceneFile = file;
        return true;
    }

    bool LoadScene(const std::string& file) {
        if (m_PlayState != PlayState::Edit) { LOG_WARN("Stop Play before loading a scene"); return false; }
        Engine::SceneSettings s = GatherSettings();   // older files without a section keep these
        if (!Engine::SceneSerializer(m_Scene, m_Resources->Registry(), &m_Session->Prefabs()).Load(ScenePath(file), &s)) return false;
        m_Selected = entt::null;
        m_Spawned.clear();
        ApplySettings(s);
        m_SceneFile = file;
        m_History.Reset(m_Scene, m_Selected, m_Resources->Registry());   // a fresh history, counts as saved
        m_MaybeChanged = false;
        return true;
    }

    static std::string EnvVar(const char* name) {
        char* value = nullptr;
        size_t len = 0;
        std::string out;
        if (_dupenv_s(&value, &len, name) == 0 && value) out = value;
        std::free(value);
        return out;
    }

    // ------------------------------------------------------------ example levels (DemoLevels.cpp)
    LevelAssets MakeLevelAssets() { EnsureSphere(); return { m_Cube, m_Plane, m_Sphere, m_Checker }; }
    void BuildLevel(void (*build)(Engine::Scene&, const LevelAssets&, Engine::PrefabLibrary&)) {
        m_Selected = entt::null;
        m_Spawned.clear();
        build(m_Scene, MakeLevelAssets(), m_Session->Prefabs());
        m_History.Track(m_Scene, m_Selected, m_Resources->Registry(),
            build == BuildCoinRush ? "Build Coin Rush" : "Build script demo");   // undoable like any edit
    }

    // ------------------------------------------------------------ game camera
    Engine::Camera& ViewCamera() { return m_ViewingGame ? *m_GameCamera : *m_Camera; }

    // Small frustum outlines for camera entities, so they can be placed in the editor
    void DrawCameraFrustums(const ImVec2& origin, const ImVec2& size) {
        using namespace Engine;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const glm::mat4 vp = ViewCamera().GetViewProjection();
        auto project = [&](const glm::vec3& w, ImVec2& out) {
            glm::vec4 c = vp * glm::vec4(w, 1.0f);
            if (c.w <= 0.01f) return false;
            out = ImVec2(origin.x + (c.x / c.w * 0.5f + 0.5f) * size.x,
                origin.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * size.y);
            return true;
        };
        auto line = [&](const glm::vec3& a, const glm::vec3& b, ImU32 col) {
            ImVec2 pa, pb;
            if (project(a, pa) && project(b, pb)) dl->AddLine(pa, pb, col, 1.5f);
        };

        const float aspect = size.y > 0.0f ? size.x / size.y : 16.0f / 9.0f;
        for (auto [e, t, c] : m_Scene.Registry().view<TransformComponent, CameraComponent>().each()) {
            const ImU32 col = e == m_Selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 210, 150);
            glm::vec3 p, sc;                       // world placement: cameras can sit under a parent
            glm::quat q;
            Scene::Decompose(m_Scene.GetWorldMatrix(e), p, q, sc);
            const glm::vec3 f = q * glm::vec3(0, 0, -1), r = q * glm::vec3(1, 0, 0), u = q * glm::vec3(0, 1, 0);
            const float d = 0.8f, h = std::tan(glm::radians(c.FOV) * 0.5f) * d, w = h * aspect;
            const glm::vec3 k[4] = { p + f * d - r * w - u * h, p + f * d + r * w - u * h,
                                     p + f * d + r * w + u * h, p + f * d - r * w + u * h };
            for (int i = 0; i < 4; i++) { line(p, k[i], col); line(k[i], k[(i + 1) % 4], col); }
            line(k[3] + u * 0.1f, p + f * d + u * (h + 0.3f), col);      // a little "up" roof
            line(k[2] + u * 0.1f, p + f * d + u * (h + 0.3f), col);
            line(k[3] + u * 0.1f, k[2] + u * 0.1f, col);
        }
    }

    void DrawColliders(const ImVec2& origin, const ImVec2& size) {
        using namespace Engine;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const glm::mat4 vp = ViewCamera().GetViewProjection();

        auto project = [&](const glm::vec3& w, ImVec2& out) {
            glm::vec4 c = vp * glm::vec4(w, 1.0f);
            if (c.w <= 0.01f) return false;                    // behind the camera
            out = ImVec2(origin.x + (c.x / c.w * 0.5f + 0.5f) * size.x,
                origin.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * size.y);
            return true;
            };
        auto line = [&](const glm::vec3& a, const glm::vec3& b, ImU32 col) {
            ImVec2 pa, pb;
            if (project(a, pa) && project(b, pb)) dl->AddLine(pa, pb, col, 1.5f);
            };

        auto view = m_Scene.Registry().view<TransformComponent, RigidBodyComponent>();
        for (auto entity : view) {
            auto [t, rb] = view.get<TransformComponent, RigidBodyComponent>(entity);
            const ImU32 col = rb.IsTrigger ? IM_COL32(230, 90, 255, 220)
                : rb.Type == BodyType::Static ? IM_COL32(80, 200, 255, 200)
                : rb.Type == BodyType::Dynamic ? IM_COL32(90, 230, 110, 220)
                : IM_COL32(255, 200, 60, 220);
            const glm::mat4 M = m_Scene.GetWorldMatrix(entity);   // parents included

            if (rb.Shape == ColliderShape::Box) {
                const glm::vec3 h = rb.Size * 0.5f;
                glm::vec3 c[8];
                for (int i = 0; i < 8; i++)
                    c[i] = glm::vec3(M * glm::vec4(rb.Center + glm::vec3((i & 1) ? h.x : -h.x,
                        (i & 2) ? h.y : -h.y,
                        (i & 4) ? h.z : -h.z), 1.0f));
                static const int edges[12][2] = { { 0,1 },{ 2,3 },{ 4,5 },{ 6,7 },{ 0,2 },{ 1,3 },{ 4,6 },{ 5,7 },{ 0,4 },{ 1,5 },{ 2,6 },{ 3,7 } };
                for (auto& ed : edges) line(c[ed[0]], c[ed[1]], col);
            }
            else {
                glm::vec3 wp, ws;
                glm::quat wq;
                Scene::Decompose(M, wp, wq, ws);
                const glm::vec3 s = glm::abs(ws);
                const float r = 0.5f * rb.Size.x * std::max({ s.x, s.y, s.z });
                const glm::vec3 centre = glm::vec3(M * glm::vec4(rb.Center, 1.0f));
                const glm::mat3 R = glm::mat3_cast(wq);
                for (int axis = 0; axis < 3; axis++) {              // three great circles
                    const glm::vec3 u = R[(axis + 1) % 3], v = R[(axis + 2) % 3];
                    glm::vec3 prev = centre + u * r;
                    for (int k = 1; k <= 32; k++) {
                        float a = k * 6.2831853f / 32.0f;
                        glm::vec3 p = centre + (u * std::cos(a) + v * std::sin(a)) * r;
                        line(prev, p, col);
                        prev = p;
                    }
                }
            }
        }
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

        const bool playing = m_PlayState != PlayState::Edit;

        if (!playing) { if (ImGui::Button("Play")) m_PlayCmd = PlayCmd::Play; }
        else { if (ImGui::Button("Stop")) m_PlayCmd = PlayCmd::Stop; }
        ImGui::SameLine();
        ImGui::BeginDisabled(!playing);
        if (ImGui::Button(m_PlayState == PlayState::Paused ? "Resume" : "Pause")) m_PlayCmd = PlayCmd::Pause;
        ImGui::SameLine();
        ImGui::BeginDisabled(m_PlayState != PlayState::Paused);
        if (ImGui::Button("Step")) m_PlayCmd = PlayCmd::Step;
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        if (m_PlayState == PlayState::Edit)        ImGui::TextDisabled("Edit mode  (Ctrl+P to play)");
        else if (m_PlayState == PlayState::Play)   ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f), "PLAYING - changes are discarded on Stop");
        else                                       ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "PAUSED - changes are discarded on Stop");

        // Undo / redo: the buttons name the step they would take back or redo
        ImGui::BeginDisabled(playing || !m_History.CanUndo());
        if (ImGui::Button("Undo")) UndoStep();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(m_History.CanUndo() ? "Ctrl+Z: undo \"%s\"" : "Ctrl+Z: nothing to undo", m_History.UndoLabel().c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(playing || !m_History.CanRedo());
        if (ImGui::Button("Redo")) RedoStep();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(m_History.CanRedo() ? "Ctrl+Y: redo \"%s\"" : "Ctrl+Y: nothing to redo", m_History.RedoLabel().c_str());
        ImGui::SameLine();
        if (m_History.CanUndo()) ImGui::TextDisabled("%s", m_History.UndoLabel().c_str());
        ImGui::Separator();

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
        auto& s = m_Renderer->Shadows();
        ImGui::Checkbox("Enabled", &s.Enabled);
        ImGui::SliderFloat("Point bias", &s.PointBias, 0.005f, 0.3f);
        ImGui::SliderFloat("Point range", &s.PointFar, 5.0f, 100.0f);
        ImGui::SliderFloat("Sun bias", &s.SunBias, 0.0002f, 0.01f, "%.4f");
        ImGui::SliderFloat("Sun area", &s.SunExtent, 4.0f, 40.0f);
        ImGui::SliderFloat("Ambient (sky)", &m_Scene.AmbientIntensity, 0.0f, 4.0f);

        ImGui::Separator();
        ImGui::TextUnformatted("Environment");
        {
            // The list is rescanned while open; the bake happens at the start of the next frame
            const std::string current = m_Renderer->CurrentEnvironment();
            if (ImGui::BeginCombo("Map", current.empty() ? "Procedural sky" : current.c_str())) {
                if (ImGui::Selectable("Procedural sky", current.empty())) m_Renderer->RequestEnvironment("");
                for (const auto& f : m_Renderer->ListEnvironments())
                    if (ImGui::Selectable(f.c_str(), current == f)) m_Renderer->RequestEnvironment(f);
                ImGui::EndCombo();
            }
        }
        ImGui::Checkbox("Show sky", &m_Renderer->GetEnvironment().Show);
        ImGui::SliderFloat("Sky blur", &m_Renderer->GetEnvironment().Blur, 0.0f, 6.0f);
        ImGui::SliderFloat("Rotation", &m_Renderer->GetEnvironment().RotationDegrees, -180.0f, 180.0f);
        ImGui::SliderFloat("Intensity", &m_Scene.AmbientIntensity, 0.0f, 4.0f);

        ImGui::Separator();
        ImGui::TextUnformatted("Display");
        ImGui::SliderFloat("Exposure", &m_Renderer->Post.Exposure, 0.1f, 4.0f);
        const char* modes[] = { "None (clamp)", "Reinhard", "ACES" };
        ImGui::Combo("Tone map", &m_Renderer->Post.ToneMap, modes, 3);
        ImGui::Checkbox("Gamma correction", &m_Renderer->Post.Gamma);
        ImGui::Checkbox("FXAA", &m_Renderer->Post.FXAA);

        ImGui::Separator();
        ImGui::TextUnformatted("Ambient occlusion");
        ImGui::Checkbox("AO enabled", &m_Renderer->Post.AOEnabled);
        ImGui::SliderFloat("AO strength", &m_Renderer->Post.AOStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("AO radius", &m_Renderer->GetSSAO().Radius, 0.1f, 2.0f);
        ImGui::SliderFloat("AO bias", &m_Renderer->GetSSAO().Bias, 0.0f, 0.1f, "%.3f");
        ImGui::SliderInt("AO samples", &m_Renderer->GetSSAO().Samples, 4, 32);
        ImGui::Checkbox("Show AO only", &m_Renderer->Post.AODebug);

        ImGui::Separator();
        ImGui::TextUnformatted("Bloom");
        ImGui::Checkbox("Bloom enabled", &m_Renderer->Post.BloomEnabled);
        ImGui::SliderFloat("Bloom intensity", &m_Renderer->Post.BloomIntensity, 0.0f, 1.0f);
        ImGui::SliderFloat("Bloom threshold", &m_Renderer->GetBloom().Threshold, 0.0f, 5.0f);
        ImGui::SliderFloat("Bloom knee", &m_Renderer->GetBloom().Knee, 0.0f, 1.0f);
        ImGui::SliderFloat("Bloom radius", &m_Renderer->GetBloom().Radius, 0.5f, 2.0f);
        ImGui::SliderFloat("Bloom max brightness", &m_Renderer->GetBloom().MaxBrightness, 1.0f, 30.0f);

        ImGui::Separator();
        ImGui::TextUnformatted("Physics");
        if (ImGui::SliderFloat("Gravity", &m_Gravity, 0.0f, 30.0f)) m_Session->Physics().SetGravity(m_Gravity);
        ImGui::Checkbox("Show colliders", &m_ShowColliders);
        ImGui::Text("Bodies: %u", m_Session->Physics().BodyCount());
        if (ImGui::Button("Spawn box"))  SpawnBody(false, m_Camera->Position + m_Camera->GetFront() * 3.0f, glm::vec3(0.0f), true);
        ImGui::SameLine();
        if (ImGui::Button("Spawn ball")) SpawnBody(true, m_Camera->Position + m_Camera->GetFront() * 3.0f, glm::vec3(0.0f), true);
        ImGui::TextDisabled("T throws a ball while playing");

        ImGui::Separator();
        ImGui::TextUnformatted("Audio");
        if (!m_Session->Audio().IsReady()) {
            ImGui::TextDisabled("No audio device: sound is off");
        }
        else {
            float master = m_Session->Audio().GetMasterVolume();
            if (ImGui::SliderFloat("Master volume", &master, 0.0f, 1.0f)) m_Session->Audio().SetMasterVolume(master);
            ImGui::Text("Voices: %u", m_Session->Audio().VoiceCount());
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Scripting");
        ImGui::Checkbox("Play through game camera", &m_PlayThroughGameCamera);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("During Play, show the scene's primary Camera entity.\n"
                "Untick to fly the editor camera (and use editor shortcuts) while the game runs.");
        if (m_Session->Scripts().IsRunning()) {
            ImGui::Text("Scripts running: %u", m_Session->Scripts().InstanceCount());
            ImGui::SameLine();
            if (ImGui::Button("Reload (F5)")) m_ReloadScripts = true;
        }
        else {
            ImGui::TextDisabled("Scripts load when Play starts");
        }
        for (const auto& err : m_Session->Scripts().RecentErrors()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", err.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Scene");
        ImGui::BeginDisabled(playing);
        if (ImGui::BeginCombo("File", m_SceneFile.c_str())) {
            // Rescanned while the list is open, so new files show up without a restart
            for (const auto& f : ListSceneFiles())
                if (ImGui::Selectable(f.c_str(), f == m_SceneFile)) m_SceneFile = f;
            ImGui::EndCombo();
        }
        if (ImGui::Button("Save")) SaveScene(m_SceneFile);
        ImGui::SameLine();
        if (ImGui::Button("Load")) RequestLoad(m_SceneFile);   // asks about unsaved changes; loads next frame
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        ImGui::InputTextWithHint("##saveas", "new_level", m_SaveAsName, sizeof(m_SaveAsName));
        ImGui::SameLine();
        ImGui::BeginDisabled(m_SaveAsName[0] == '\0');
        if (ImGui::Button("Save as")) {
            std::string name = m_SaveAsName;
            if (std::filesystem::path(name).extension() != ".json") name += ".json";
            if (SaveScene(name)) { m_SceneFile = name; m_SaveAsName[0] = '\0'; }
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Assets/Scenes. Saves entities, environment, post, shadows, gravity, camera");
        ImGui::TextUnformatted("Example levels (replace the scene)");
        if (ImGui::Button("Script demo")) BuildLevel(BuildScriptDemo);
        ImGui::SameLine();
        if (ImGui::Button("Coin Rush")) BuildLevel(BuildCoinRush);
        ImGui::EndDisabled();
        ImGui::TextDisabled("Arrows roll the ball, Space jumps, R retries");
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
        ImGui::SameLine();
        if (ImGui::Button("+ Material grid")) SpawnMaterialGrid();
        ImGui::SameLine();
        if (ImGui::Button("+ Prefab")) ImGui::OpenPopup("PlacePrefab");
        if (ImGui::BeginPopup("PlacePrefab")) {
            const auto prefabs = m_Session->Prefabs().List();
            if (prefabs.empty()) ImGui::TextDisabled("No prefabs yet: Inspector > Save as prefab");
            for (const auto& p : prefabs) {
                if (!ImGui::MenuItem(std::filesystem::path(p).stem().string().c_str())) continue;
                Entity e = m_Session->Prefabs().Instantiate(m_Scene, p);
                if (e) {
                    e.Get<TransformComponent>().Position = m_Camera->Position + m_Camera->GetFront() * 4.0f;
                    m_Selected = e.GetHandle();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::Separator();

        // A tree: roots first, children nested. Drag an entity onto another to make it a child,
        // or onto the empty space below the list to make it a root again.
        auto& reg = m_Scene.Registry();
        m_Reparent = { entt::null, entt::null, false };
        std::vector<entt::entity> roots;
        for (auto handle : reg.view<TagComponent>())
            if (m_Scene.GetParent(handle) == entt::null) roots.push_back(handle);
        for (auto r : roots) DrawHierarchyNode(r, 0);

        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y, 30.0f)));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("MDB_ENTITY"))
                m_Reparent = { *(const entt::entity*)p->Data, entt::null, true };
            ImGui::EndDragDropTarget();
        }
        ImGui::End();

        // Applied after the tree is drawn, so the views above aren't changed while being walked
        if (m_Reparent.Pending && reg.valid(m_Reparent.Child)) {
            if (!m_Scene.SetParent(m_Reparent.Child, m_Reparent.Parent, true))
                LOG_WARN("Can't parent an entity to itself or to one of its own children");
        }
    }

    struct PendingReparent { entt::entity Child, Parent; bool Pending; };
    PendingReparent m_Reparent{ entt::null, entt::null, false };

    void DrawHierarchyNode(entt::entity e, int depth) {
        using namespace Engine;
        auto& reg = m_Scene.Registry();
        const auto children = m_Scene.GetChildren(e);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth
            | ImGuiTreeNodeFlags_DefaultOpen;
        if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (e == m_Selected) flags |= ImGuiTreeNodeFlags_Selected;

        const bool instance = reg.all_of<PrefabComponent>(e);
        if (instance) ImGui::PushStyleColor(ImGuiCol_Text, kPrefabColor);
        const bool open = ImGui::TreeNodeEx((void*)(uintptr_t)m_Scene.GetID(e), flags, "%s", reg.get<TagComponent>(e).Tag.c_str());
        if (instance) ImGui::PopStyleColor();
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) m_Selected = e;

        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("MDB_ENTITY", &e, sizeof(e));
            ImGui::Text("%s", reg.get<TagComponent>(e).Tag.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("MDB_ENTITY"))
                m_Reparent = { *(const entt::entity*)p->Data, e, true };
            ImGui::EndDragDropTarget();
        }

        if (open && !children.empty()) {
            if (depth < 64) for (auto c : children) DrawHierarchyNode(c, depth + 1);
            ImGui::TreePop();
        }
    }

    static constexpr ImVec4 kPrefabColor{ 0.45f, 0.75f, 1.0f, 1.0f };   // prefab instances in the UI

    // Inspector block: the prefab link (Apply / Revert / Unpack) or "Save as prefab"
    void DrawPrefabSection(Engine::Entity e) {
        using namespace Engine;
        const bool playing = m_PlayState != PlayState::Edit;

        if (e.Has<PrefabComponent>()) {
            const std::string path = e.Get<PrefabComponent>().Path;
            auto& prefabs = m_Session->Prefabs();
            const bool loadable = prefabs.CanLoad(path);
            const auto overrides = loadable ? prefabs.ListOverrides(e) : std::vector<std::string>{};
            const bool modified = !overrides.empty();
            ImGui::TextColored(kPrefabColor, "Prefab  %s", std::filesystem::path(path).filename().string().c_str());
            ImGui::SameLine();
            if (!loadable)     ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "(file missing or broken)");
            else if (modified) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "(%d override%s)", (int)overrides.size(), overrides.size() == 1 ? "" : "s");

            ImGui::BeginDisabled(playing || !modified);
            if (ImGui::Button("Apply all")) {
                const int n = prefabs.Apply(m_Scene, e);
                m_PrefabStatus = n < 0 ? "Could not write the prefab" : "Applied: " + std::to_string(n) + " other instance(s) updated";
                m_PrefabStatusUntil = m_RunTime + 4.0f;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Write this entity's components to %s.\nOther instances follow, keeping their own overrides.", path.c_str());
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!modified);
            if (ImGui::Button("Revert all")) prefabs.Revert(e);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Drop every override: take the prefab's components again");
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Unpack")) e.Remove<PrefabComponent>();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep the components, drop the link to the prefab");

            // Each override, readable, with its own revert
            if (modified && ImGui::TreeNodeEx("Overrides", ImGuiTreeNodeFlags_DefaultOpen)) {
                std::string revert;
                for (const auto& item : overrides) {
                    ImGui::PushID(item.c_str());
                    if (ImGui::SmallButton("Revert")) revert = item;
                    ImGui::SameLine();
                    if (item[0] == '+')      ImGui::Text("added %s", item.c_str() + 1);
                    else if (item[0] == '-') ImGui::Text("removed %s", item.c_str() + 1);
                    else                     ImGui::TextUnformatted(item.c_str());
                    ImGui::PopID();
                }
                ImGui::TreePop();
                if (!revert.empty()) prefabs.RevertOverride(e, revert);   // after the list: it rebuilds the components
            }
        }
        else {
            ImGui::BeginDisabled(playing);
            if (ImGui::Button("Save as prefab...")) {
                // Suggest a file name from the entity name: lower case, spaces to underscores
                std::string name;
                for (char c : e.Get<TagComponent>().Tag) {
                    if (std::isalnum((unsigned char)c) || c == '-' || c == '_') name += (char)std::tolower((unsigned char)c);
                    else if (c == ' ') name += '_';
                }
                std::snprintf(m_PrefabName, sizeof(m_PrefabName), "%s", name.empty() ? "prefab" : name.c_str());
                ImGui::OpenPopup("SavePrefab");
            }
            ImGui::EndDisabled();
            if (ImGui::BeginPopup("SavePrefab")) {
                ImGui::TextUnformatted("Assets/Prefabs/");
                ImGui::SameLine(0, 0);
                ImGui::SetNextItemWidth(160.0f);
                ImGui::InputText("##prefabname", m_PrefabName, sizeof(m_PrefabName));
                ImGui::SameLine(0, 0);
                ImGui::TextUnformatted(".json");
                const std::string path = PrefabLibrary::Resolve(m_PrefabName);
                const bool exists = m_Session->Prefabs().Exists(path);
                ImGui::BeginDisabled(m_PrefabName[0] == '\0');
                if (ImGui::Button(exists ? "Overwrite" : "Save")) {
                    if (m_Session->Prefabs().Save(e, path)) {
                        m_PrefabStatus = "Saved " + path;
                        m_PrefabStatusUntil = m_RunTime + 4.0f;
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                if (exists) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "A prefab with this name exists");
                ImGui::EndPopup();
            }
        }
        if (m_RunTime < m_PrefabStatusUntil) ImGui::TextDisabled("%s", m_PrefabStatus.c_str());
    }

    void DrawViewportPanel() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("Viewport");
        ImGui::PopStyleVar();

        ImVec2 avail = ImGui::GetContentRegionAvail();
        m_ViewportSize = avail;

        ImGui::Image((ImTextureID)(intptr_t)m_Renderer->GetOutput(),
            avail, ImVec2(0, 1), ImVec2(1, 0));

        const bool imageHovered = ImGui::IsItemHovered();
        const ImVec2 origin = ImGui::GetItemRectMin();
        const ImVec2 size = ImGui::GetItemRectSize();
        if (m_PlayState != PlayState::Edit) {
            const ImU32 col = m_PlayState == PlayState::Play ? IM_COL32(255, 140, 0, 255) : IM_COL32(255, 215, 0, 255);
            ImGui::GetWindowDrawList()->AddRect(ImVec2(origin.x + 1.5f, origin.y + 1.5f),
                ImVec2(origin.x + size.x - 1.5f, origin.y + size.y - 1.5f),
                col, 0.0f, 0, 3.0f);
        }

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

        if (m_ShowColliders && !m_ViewingGame) DrawColliders(origin, size);
        if (!m_ViewingGame) DrawCameraFrustums(origin, size);
        if (m_PlayState != PlayState::Edit)   // kept while paused: the scripts did not run again
            DrawScriptText(ImGui::GetWindowDrawList(), m_GameFont, m_Session->Scripts().Texts(), origin, size);
        if (m_ViewingGame) {
            ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + 8.0f, origin.y + size.y - 22.0f),
                IM_COL32(255, 255, 255, 110), "Game camera (Tools > Play through game camera)");
        }

        // ---- Gizmo for the selected entity (not while looking through the game camera)
        bool gizmoBusy = false;
        m_GizmoBusy = false;
        if (!m_ViewingGame && !m_SelectTool && m_Selected != entt::null && m_Scene.Registry().valid(m_Selected) && !m_MouseLook) {
            Engine::Entity e(m_Selected, &m_Scene);
            auto& t = e.Get<Engine::TransformComponent>();

            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist();
            ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);

            glm::mat4 view = ViewCamera().GetView();
            glm::mat4 proj = ViewCamera().GetProjection();
            glm::mat4 model = m_Scene.GetWorldMatrix(m_Selected);   // the gizmo works in world space

            float snap = m_GizmoOp == ImGuizmo::ROTATE ? 15.0f : (m_GizmoOp == ImGuizmo::SCALE ? 0.1f : 0.5f);
            float snapValues[3] = { snap, snap, snap };

            ImGuizmo::Enable(!navigating);
            ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), m_GizmoOp,
                m_GizmoLocal ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
                glm::value_ptr(model), nullptr, m_Snap ? snapValues : nullptr);

            if (ImGuizmo::IsUsing()) {
                m_Scene.SetWorldMatrix(m_Selected, model);   // back to a transform local to the parent
            }
            gizmoBusy = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
            m_GizmoBusy = ImGuizmo::IsUsing();
        }

        // ---- Click to select (not while interacting with the gizmo)
        if (!m_ViewingGame && imageHovered && !gizmoBusy && !navigating && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_MouseLook) {
            ImVec2 mouse = ImGui::GetMousePos();
            int px = (int)(mouse.x - origin.x);
            int py = (int)m_Renderer->GetFramebuffer().GetHeight() - 1 - (int)(mouse.y - origin.y);

            int id = m_Renderer->GetFramebuffer().ReadEntityID(px, py);
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
        DrawPrefabSection(e);

        // Transform
        if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            const entt::entity parent = m_Scene.GetParent(e.GetHandle());
            if (parent != entt::null) {
                ImGui::TextDisabled("Relative to %s", m_Scene.Registry().get<TagComponent>(parent).Tag.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Unparent")) m_Scene.SetParent(e.GetHandle(), entt::null, true);
            }
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
            if (!mr.UseModelMaterials) {
                ImGui::SliderFloat("Metallic", &mr.Metallic, 0.0f, 1.0f);
                ImGui::SliderFloat("Roughness", &mr.Roughness, 0.04f, 1.0f);
                ImGui::ColorEdit3("Emissive", &mr.Emissive.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
            }
            ImGui::Checkbox("Unlit", &mr.Unlit);
            if (mr.Geometry && mr.Geometry->GetSubmeshMaterial(0)) {
                ImGui::Checkbox("Use model materials", &mr.UseModelMaterials);
                if (mr.UseModelMaterials) {
                    ImGui::TextDisabled("Color tints the model's own materials");
                    if (ImGui::TreeNode("Materials")) {
                        for (uint32_t i = 0; i < mr.Geometry->SubmeshCount(); i++) {
                            const Material* mat = mr.Geometry->GetSubmeshMaterial(i);
                            ImGui::BulletText("%s  M:%.2f R:%.2f%s%s", mat->Name.empty() ? "(unnamed)" : mat->Name.c_str(),
                                mat->Metallic, mat->Roughness,
                                mat->Albedo ? " [tex]" : "", mat->Normal ? " [normal]" : "");
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

        if (e.Has<RigidBodyComponent>() && ImGui::CollapsingHeader("Rigid Body", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& rb = e.Get<RigidBodyComponent>();
            int type = (int)rb.Type;
            if (ImGui::Combo("Type", &type, "Static\0Dynamic\0Kinematic\0")) rb.Type = (BodyType)type;
            int shape = (int)rb.Shape;
            if (ImGui::Combo("Shape", &shape, "Box\0Sphere\0")) rb.Shape = (ColliderShape)shape;

            if (rb.Shape == ColliderShape::Sphere) ImGui::DragFloat("Diameter", &rb.Size.x, 0.02f, 0.01f, 100.0f);
            else                                   ImGui::DragFloat3("Size", &rb.Size.x, 0.02f, 0.01f, 100.0f);
            ImGui::DragFloat3("Center", &rb.Center.x, 0.02f);

            if (rb.Type == BodyType::Dynamic) ImGui::DragFloat("Mass", &rb.Mass, 0.05f, 0.01f, 1000.0f);
            ImGui::SliderFloat("Friction", &rb.Friction, 0.0f, 1.0f);
            ImGui::SliderFloat("Bounciness", &rb.Restitution, 0.0f, 1.0f);
            ImGui::Checkbox("Trigger", &rb.IsTrigger);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reports overlaps to scripts (on_trigger_enter / exit) instead of colliding.\n"
                    "Triggers follow their transform like kinematic bodies and detect dynamic bodies.");

            if (ImGui::Button("Fit to mesh") && e.Has<MeshRendererComponent>())
                FitCollider(e.Get<MeshRendererComponent>(), rb);
            ImGui::SameLine();
            if (ImGui::Button("Remove##rb")) e.Remove<RigidBodyComponent>();
            if (rb.Type != BodyType::Static && (e.Has<SpinComponent>() || e.Has<OrbitComponent>()) && rb.Type == BodyType::Dynamic)
                ImGui::TextDisabled("Physics overrides Spin/Orbit on dynamic bodies");
            if (m_PlayState != PlayState::Edit) ImGui::TextDisabled("Changes apply on the next Play");
        }

        if (e.Has<ScriptComponent>() && ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& sc = e.Get<ScriptComponent>();
            if (ImGui::BeginCombo("Script", sc.Path.empty() ? "(none)" : sc.Path.c_str())) {
                // Rescanned while the list is open, so new files show up without a restart
                std::vector<std::string> files;
                std::error_code ec;
                const std::filesystem::path dir = std::filesystem::path(ASSET_DIR) / "Scripts";
                for (const auto& f : std::filesystem::directory_iterator(dir, ec))
                    if (f.path().extension() == ".lua") files.push_back("Scripts/" + f.path().filename().string());
                std::sort(files.begin(), files.end());
                for (const auto& f : files)
                    if (ImGui::Selectable(f.c_str(), f == sc.Path)) sc.Path = f;
                ImGui::EndCombo();
            }
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", sc.Path.c_str());
            if (ImGui::InputText("Path", buf, sizeof(buf))) sc.Path = buf;
            const bool exists = !sc.Path.empty() && std::filesystem::exists(std::string(ASSET_DIR) + sc.Path);
            if (!exists) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "File not found in Assets/");
            ImGui::TextDisabled("Relative to Assets/. Loads on Play; F5 reloads while playing.");
            if (ImGui::Button("Remove##script")) e.Remove<ScriptComponent>();
        }

        if (e.Has<CameraComponent>() && ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& c = e.Get<CameraComponent>();
            ImGui::SliderFloat("Field of view", &c.FOV, 10.0f, 120.0f, "%.0f deg");
            ImGui::DragFloat("Near", &c.Near, 0.01f, 0.01f, 10.0f);
            ImGui::DragFloat("Far", &c.Far, 1.0f, 1.0f, 5000.0f);
            ImGui::Checkbox("Primary", &c.Primary);
            ImGui::TextDisabled("Looks along local -Z. Play shows the primary camera.");
            if (ImGui::Button("Remove##camera")) e.Remove<CameraComponent>();
        }

        if (e.Has<AudioSourceComponent>() && ImGui::CollapsingHeader("Audio Source", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& a = e.Get<AudioSourceComponent>();
            if (ImGui::BeginCombo("Clip", a.Clip.empty() ? "(none)" : a.Clip.c_str())) {
                for (const auto& f : ListSoundFiles())
                    if (ImGui::Selectable(f.c_str(), f == a.Clip)) a.Clip = f;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Preview")) m_Session->Audio().Play(a.Clip, a.Volume, a.Pitch);
            ImGui::SliderFloat("Volume##audio", &a.Volume, 0.0f, 2.0f);
            ImGui::SliderFloat("Pitch##audio", &a.Pitch, 0.25f, 3.0f);
            ImGui::Checkbox("Loop", &a.Loop);
            ImGui::SameLine();
            ImGui::Checkbox("Play on start", &a.PlayOnStart);
            ImGui::SameLine();
            ImGui::Checkbox("3D", &a.Spatial);
            if (a.Spatial) {
                ImGui::DragFloat("Full volume within", &a.MinDistance, 0.1f, 0.01f, 100.0f, "%.1f m");
                ImGui::DragFloat("Fades out to", &a.MaxDistance, 0.5f, 0.1f, 500.0f, "%.1f m");
            }
            ImGui::TextDisabled("Plays during Play. Assets/Sounds: .wav .mp3 .flac");
            if (ImGui::Button("Remove##audio")) e.Remove<AudioSourceComponent>();
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
            if (!e.Has<RigidBodyComponent>() && ImGui::MenuItem("Rigid Body")) {
                auto& rb = e.Add<RigidBodyComponent>();
                if (e.Has<MeshRendererComponent>()) FitCollider(e.Get<MeshRendererComponent>(), rb);
            }
            if (!e.Has<ScriptComponent>() && ImGui::MenuItem("Script")) e.Add<ScriptComponent>().Path = "Scripts/mover.lua";
            if (!e.Has<CameraComponent>() && ImGui::MenuItem("Camera")) e.Add<CameraComponent>();
            if (!e.Has<AudioSourceComponent>() && ImGui::MenuItem("Audio Source")) {
                auto files = ListSoundFiles();
                e.Add<AudioSourceComponent>().Clip = files.empty() ? "" : files.front();
            }
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
            glm::mat4 m = m_Scene.GetWorldMatrix(handle) * mr.LocalOffset;

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
            const glm::vec3 p(m_Scene.GetWorldMatrix(handle)[3]);   // lights, empties
            mn = p - glm::vec3(0.5f);
            mx = p + glm::vec3(0.5f);
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

    float AspectRatio() {
        auto& w = GetWindow();
        return w.GetHeight() ? (float)w.GetWidth() / (float)w.GetHeight() : 1.0f;
    }

    // Declaration order matters: the scene and resources must outlive the session that uses them
    Engine::Scene m_Scene;
    std::unique_ptr<Engine::ResourceLibrary> m_Resources;   // named meshes/textures for files and scripts
    std::unique_ptr<Engine::SceneRenderer> m_Renderer;      // the whole frame pipeline
    std::unique_ptr<Engine::GameSession> m_Session;         // physics, scripts, audio, prefabs (Play mode)
    std::unique_ptr<Engine::Camera> m_Camera;          // the editor camera (Maya-style navigation)
    std::unique_ptr<Engine::Camera> m_GameCamera;      // follows the scene's primary CameraComponent
    bool m_PlayThroughGameCamera = true;
    bool m_ViewingGame = false;                        // this frame renders through m_GameCamera
    ImFont* m_GameFont = nullptr;                      // large font for ui.text (null: default font)
    std::shared_ptr<Engine::Mesh> m_Cube;              // shortcuts into m_Resources
    std::shared_ptr<Engine::Mesh> m_Sphere;
    std::shared_ptr<Engine::Texture2D> m_Checker;
    std::shared_ptr<Engine::Mesh>  m_Plane;
    std::shared_ptr<Engine::Model> m_ModelResource;
    std::vector<Engine::Entity> m_Spawned;
    entt::entity m_Selected = entt::null;
    std::mt19937 m_Rng{ std::random_device{}() };
    std::string m_SceneFile = "scene.json";      // in Assets/Scenes
    std::string m_PendingSceneLoad;              // queued Load: applied at the start of OnUpdate
    char m_SaveAsName[64] = {};

    enum class PlayState { Edit, Play, Paused };
    enum class PlayCmd { None, Play, Stop, Pause, Step, Restart };

    PlayState m_PlayState = PlayState::Edit;
    PlayCmd   m_PlayCmd = PlayCmd::None;     // set by the UI, applied at the start of the next frame
    Engine::SceneSnapshot m_Snapshot;
    int  m_SnapshotSelected = -1;
    bool m_StepFrame = false;
    bool m_PDown = false, m_EscDown = false;

    char m_PrefabName[64] = {};
    std::string m_PrefabStatus;
    float m_PrefabStatusUntil = 0.0f;
    bool  m_ReloadScripts = false;   // queued: applied at the start of the next OnUpdate
    float m_RunTime = 0.0f;
    float m_QuitAfter = 0.0f;        // MDB_QUIT_AFTER test hook (0 = never)
    float m_Gravity = 9.81f;
    bool  m_ShowColliders = true;
    bool  m_TDown = false;

    EditorHistory m_History;              // undo / redo (Edit mode only)
    bool m_MaybeChanged = false;          // an interaction happened: compare the scene once it is over
    bool m_GizmoBusy = false;             // the gizmo is being dragged
    bool m_ZDown = false, m_YDown = false, m_DDown = false, m_SDown = false;
    std::string m_Title;
    enum class ConfirmAction { None, Quit, Load };
    ConfirmAction m_Confirm = ConfirmAction::None;   // the "unsaved changes" dialog is open for this
    std::string m_ConfirmFile;            // the scene to load after the dialog
    bool m_QuitConfirmed = false;

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
