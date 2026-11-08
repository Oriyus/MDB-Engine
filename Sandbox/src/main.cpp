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
    // The editor opens full screen (borderless, at the monitor's resolution); F11 switches to a window
    static Engine::WindowProps EditorWindow() {
        Engine::WindowProps p;
        p.Title = "Sandbox";
        p.Width = 1600;
        p.Height = 900;
        p.Fullscreen = EnvVar("MDB_WINDOWED") != "1";   // test hook: screen captures need a normal window
        return p;
    }
    SandboxApp() : Application(EditorWindow()) {}

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
                rb.Shape = ColliderShape::ConvexHull;   // wraps the model's real shape
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
        // Editor look: the layout file, a proper UI font (the first font is the default), a mono font for
        // the Console, and the style. All before the first ImGui frame builds the font atlas.
        {
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = "editor.ini";
            if (std::filesystem::exists("C:/Windows/Fonts/segoeui.ttf"))
                io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 17.0f);
            else io.Fonts->AddFontDefault();
            if (std::filesystem::exists("C:/Windows/Fonts/consola.ttf"))
                m_MonoFont = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/consola.ttf", 15.0f);
            ApplyEditorStyle();
        }
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
        LOG_INFO("Editor ready. Help > Controls lists the shortcuts (Alt+mouse moves the camera, W/E/R the gizmo, Ctrl+P plays).");
    }

    void OnUpdate(float dt) override {
        using namespace Engine;

        if (m_PendingNewScene) { m_PendingNewScene = false; NewScene(); }
        if (m_ToggleFullscreen) { m_ToggleFullscreen = false; GetWindow().SetFullscreen(!GetWindow().IsFullscreen()); }
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
        const bool kn = EdgePressed(Key::N, m_NDown);
        if (EdgePressed(Key::F11, m_F11Down)) m_ToggleFullscreen = true;
        if (ctrl && !typing && !interacting && m_PlayState == PlayState::Edit) {
            if (kz && !shift) UndoStep();
            if (ky || (kz && shift)) RedoStep();
            if (kd) DuplicateSelected();
            if (ks && shift) m_OpenSaveAs = true;
            else if (ks) SaveScene(m_SceneFile);
            if (kn && !shift) RequestNewScene();
        }

        bool f5 = EdgePressed(Key::F5, m_F5Down);
        bool f = EdgePressed(Key::F, m_FDown);
        bool g = EdgePressed(Key::G, m_GDown);
        bool f1 = EdgePressed(Key::F1, m_F1Down);
        bool kw = EdgePressed(Key::W, m_WDown);
        bool ke = EdgePressed(Key::E, m_EDown);
        bool kr = EdgePressed(Key::R, m_RDown);
        bool kq = EdgePressed(Key::Q, m_QDown);
        bool knSpawn = kn && !ctrl;                      // plain N spawns a cube (Ctrl+N is New Scene)
        bool kdel = EdgePressed(Key::Delete, m_DelDown);
        bool kt = EdgePressed(Key::T, m_TDown);
        if (kp && ImGui::GetIO().KeyCtrl)
            m_PlayCmd = (m_PlayState == PlayState::Edit) ? PlayCmd::Play : PlayCmd::Stop;

        // Looking through a game camera, the game owns the keyboard: only Esc, Ctrl+P, F1 and F5 stay
        const bool editorKeys = !typing && !m_ViewingGame;
        if (!editorKeys) knSpawn = f = g = kw = ke = kr = kq = kdel = kt = false;

        if (!typing) {
            if (f5) {
                m_Renderer->ReloadShaders();
                if (m_PlayState != PlayState::Edit) m_ReloadScripts = true;   // applied next frame
            }
            if (knSpawn) SpawnCube();
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
            // ui.button / ui.mouse: the pointer over the viewport (last frame's layout), only while
            // looking through the game camera (otherwise clicks belong to the editor)
            {
                const ImVec2 mp = ImGui::GetIO().MousePos;
                const float px = (mp.x - m_ViewportOrigin.x) / std::max(m_ViewportSize.x, 1.0f);
                const float py = (mp.y - m_ViewportOrigin.y) / std::max(m_ViewportSize.y, 1.0f);
                m_Session->Scripts().SetPointer(px, py, m_ViewportHovered && m_ViewingGame, ImGui::IsMouseDown(ImGuiMouseButton_Left));
            }
            m_Session->Update(dt, m_StepFrame);
            if (m_Session->ConsumeRestartRequest()) m_PlayCmd = PlayCmd::Restart;   // next frame
            if (m_Session->Scripts().ConsumeQuitRequest()) m_PlayCmd = PlayCmd::Stop;   // scene.quit(): the editor stops Play
        }
        else {
            m_Scene.UpdateOrbitPositions();
            m_Scene.UpdateAnimations(0.0f);   // pose animated models at their current time (scrubbing works)
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
        // Menu bar and toolbar take their strips off the top; the dock area fills the rest
        DrawMainMenuBar();
        DrawToolbar();
        ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        BuildDefaultLayoutOnce(dockspace);

        DrawViewportPanel();
        if (m_ShowUI) {
            if (m_ShowHierarchy) DrawHierarchyPanel();
            if (m_ShowInspector) DrawInspectorPanel();
            if (m_ShowSceneSettings) DrawSceneSettingsPanel();
            if (m_ShowConsole) DrawConsolePanel();
        }
        DrawUnsavedChangesModal();
        DrawDialogs();

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
        ImGui::TextDisabled(m_Confirm == ConfirmAction::Quit ? "Save them before quitting?"
            : m_Confirm == ConfirmAction::New ? "Save them before starting a new scene?" : "Save them before loading %s?",
            m_ConfirmFile.c_str());
        ImGui::Spacing();
        auto proceed = [this] {
            if (m_Confirm == ConfirmAction::Quit) { m_QuitConfirmed = true; Close(); }
            else if (m_Confirm == ConfirmAction::Load) m_PendingSceneLoad = m_ConfirmFile;
            else if (m_Confirm == ConfirmAction::New) m_PendingNewScene = true;
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

    // Size a collider to the mesh's bounds
    static void FitCollider(const Engine::MeshRendererComponent& mr, Engine::ColliderShape shape, glm::vec3& size, glm::vec3& center) {
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
        size = mx - mn;
        center = (mn + mx) * 0.5f;
        if (shape == Engine::ColliderShape::Sphere) size = glm::vec3(std::max({ size.x, size.y, size.z }));
        if (shape == Engine::ColliderShape::Capsule) size = glm::vec3(std::max(size.x, size.z), size.y, std::max(size.x, size.z));
    }
    static void FitCollider(const Engine::MeshRendererComponent& mr, Engine::RigidBodyComponent& rb) {
        FitCollider(mr, rb.Shape, rb.Size, rb.Center);
    }

    // Shape, size and centre fields, shared by the Rigid Body and Collider sections
    void ShapeFields(Engine::Entity e, Engine::ColliderShape& shape, glm::vec3& size, glm::vec3& center,
        bool allowNone, bool dynamic) {
        using namespace Engine;
        int s = (int)shape;
        // Same order as the enum (the numbers are stored in scene files)
        const char* items = allowNone ? "Box\0Sphere\0Convex hull\0Mesh\0Capsule\0None (child colliders only)\0"
                                      : "Box\0Sphere\0Convex hull\0Mesh\0Capsule\0";
        if (ImGui::Combo("Shape", &s, items)) shape = (ColliderShape)s;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Capsule: a pill along local Y, for characters.\n"
                "Convex hull: the mesh's outer shape, any body type.\n"
                "Mesh: its exact triangles, for static and kinematic bodies (level geometry).\n"
                "Child entities with a Collider component add more shapes to this body.");

        if (shape == ColliderShape::ConvexHull || shape == ColliderShape::Mesh) {
            const bool hasMesh = e.Has<MeshRendererComponent>() && e.Get<MeshRendererComponent>().Geometry;
            if (!hasMesh) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Needs a Mesh Renderer (a box is used)");
            else ImGui::TextDisabled("Built from the Mesh Renderer (%u triangles), scaled with the entity",
                e.Get<MeshRendererComponent>().Geometry->TriangleCount());
            if (shape == ColliderShape::Mesh && dynamic)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Dynamic bodies can't use a mesh: a convex hull is used");
        }
        else if (shape == ColliderShape::None) {
            ImGui::TextDisabled("Only the Collider components of child entities");
        }
        else {
            if (shape == ColliderShape::Sphere) ImGui::DragFloat("Diameter", &size.x, 0.02f, 0.01f, 100.0f);
            else if (shape == ColliderShape::Capsule) {
                ImGui::DragFloat("Diameter", &size.x, 0.02f, 0.01f, 100.0f);
                ImGui::DragFloat("Height", &size.y, 0.02f, 0.01f, 100.0f);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Total height, caps included");
            }
            else ImGui::DragFloat3("Size", &size.x, 0.02f, 0.01f, 100.0f);
            ImGui::DragFloat3("Center", &center.x, 0.02f);
            if (ImGui::Button("Fit to mesh") && e.Has<MeshRendererComponent>())
                FitCollider(e.Get<MeshRendererComponent>(), shape, size, center);
        }
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
    LevelAssets MakeLevelAssets() { EnsureSphere(); return { m_Cube, m_Plane, m_Sphere, m_Checker, m_Resources->LoadModel("Models/Fox.glb"),
        m_Resources->LoadTexture("Textures/fence.png"), m_Resources->LoadTexture("Textures/window.png") }; }
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
        // A circle of radius r around c in the plane of u, v (from angle a0 to a1)
        auto arc = [&](const glm::vec3& c, const glm::vec3& u, const glm::vec3& v, float r, float a0, float a1, ImU32 col) {
            const int n = 24;
            glm::vec3 prev = c + (u * std::cos(a0) + v * std::sin(a0)) * r;
            for (int k = 1; k <= n; k++) {
                const float a = a0 + (a1 - a0) * k / n;
                const glm::vec3 p = c + (u * std::cos(a) + v * std::sin(a)) * r;
                line(prev, p, col);
                prev = p;
            }
            };

        // One collider of any shape, placed by its entity's world matrix (the same rules as PhysicsWorld)
        auto drawShape = [&](entt::entity entity, ColliderShape shape, const glm::vec3& sz, const glm::vec3& center, ImU32 col) {
            const glm::mat4 M = m_Scene.GetWorldMatrix(entity);   // parents included
            if (shape == ColliderShape::None) return;

            if (shape == ColliderShape::ConvexHull || shape == ColliderShape::Mesh) {
                // The geometry the collider is built from, as a wireframe. Big meshes show an even
                // sample of their triangles (at most 2000), which still reads as the shape.
                const auto* mr = m_Scene.Registry().try_get<MeshRendererComponent>(entity);
                if (!mr || !mr->Geometry) return;
                const glm::mat4 G = M * mr->LocalOffset;
                std::vector<glm::vec3> pos;
                std::vector<uint32_t> idx;
                mr->Geometry->GetCollisionGeometry(pos, idx);
                const size_t tris = idx.size() / 3;
                const size_t step = std::max<size_t>(1, tris / 2000);
                const ImU32 faint = (col & 0x00FFFFFFu) | (110u << 24);   // a wireframe this dense must not hide the model
                for (auto& p : pos) p = glm::vec3(G * glm::vec4(p, 1.0f));
                for (size_t t = 0; t < tris; t += step) {
                    const size_t i = t * 3;
                    line(pos[idx[i]], pos[idx[i + 1]], faint);
                    line(pos[idx[i + 1]], pos[idx[i + 2]], faint);
                    line(pos[idx[i + 2]], pos[idx[i]], faint);
                }
                return;
            }
            if (shape == ColliderShape::Box) {
                const glm::vec3 h = sz * 0.5f;
                glm::vec3 c[8];
                for (int i = 0; i < 8; i++)
                    c[i] = glm::vec3(M * glm::vec4(center + glm::vec3((i & 1) ? h.x : -h.x,
                        (i & 2) ? h.y : -h.y,
                        (i & 4) ? h.z : -h.z), 1.0f));
                static const int edges[12][2] = { { 0,1 },{ 2,3 },{ 4,5 },{ 6,7 },{ 0,2 },{ 1,3 },{ 4,6 },{ 5,7 },{ 0,4 },{ 1,5 },{ 2,6 },{ 3,7 } };
                for (auto& ed : edges) line(c[ed[0]], c[ed[1]], col);
                return;
            }

            glm::vec3 wp, ws;
            glm::quat wq;
            Scene::Decompose(M, wp, wq, ws);
            const glm::vec3 s = glm::abs(ws);
            const glm::vec3 centre = glm::vec3(M * glm::vec4(center, 1.0f));
            const glm::mat3 R = glm::mat3_cast(wq);
            const float pi = 3.14159265f;
            if (shape == ColliderShape::Capsule) {
                // Two cap centres on local Y, rings around them, side lines and half circles
                const float r = 0.5f * sz.x * std::max(s.x, s.z);
                const float half = std::max(0.0f, 0.5f * sz.y * s.y - r);
                const glm::vec3 x = R[0], y = R[1], z = R[2];
                const glm::vec3 top = centre + y * half, bottom = centre - y * half;
                arc(top, x, z, r, 0.0f, 2.0f * pi, col);
                arc(bottom, x, z, r, 0.0f, 2.0f * pi, col);
                for (const glm::vec3& side : { x, -x, z, -z }) line(top + side * r, bottom + side * r, col);
                arc(top, x, y, r, 0.0f, pi, col);
                arc(top, z, y, r, 0.0f, pi, col);
                arc(bottom, x, y, r, pi, 2.0f * pi, col);
                arc(bottom, z, y, r, pi, 2.0f * pi, col);
                return;
            }
            // Sphere: three great circles
            const float r = 0.5f * sz.x * std::max({ s.x, s.y, s.z });
            for (int axis = 0; axis < 3; axis++)
                arc(centre, R[(axis + 1) % 3], R[(axis + 2) % 3], r, 0.0f, 2.0f * pi, col);
            };

        auto& reg = m_Scene.Registry();
        for (auto [entity, rb] : reg.view<RigidBodyComponent>().each()) {
            const ImU32 col = rb.IsTrigger ? IM_COL32(230, 90, 255, 220)
                : rb.Type == BodyType::Static ? IM_COL32(80, 200, 255, 200)
                : rb.Type == BodyType::Dynamic ? IM_COL32(90, 230, 110, 220)
                : IM_COL32(255, 200, 60, 220);
            drawShape(entity, rb.Shape, rb.Size, rb.Center, col);
        }
        // Child colliders: in their body's colour (white if no body above them: they do nothing)
        for (auto [entity, c] : reg.view<ColliderComponent>().each()) {
            if (reg.all_of<RigidBodyComponent>(entity)) continue;   // not a part: an entity's own body wins
            ImU32 col = IM_COL32(255, 255, 255, 120);
            int guard = 0;
            for (entt::entity p = m_Scene.GetParent(entity); p != entt::null && guard++ < 256; p = m_Scene.GetParent(p)) {
                if (const auto* rb = reg.try_get<RigidBodyComponent>(p)) {
                    col = rb->IsTrigger ? IM_COL32(230, 90, 255, 220) : rb->Type == BodyType::Static ? IM_COL32(80, 200, 255, 200)
                        : rb->Type == BodyType::Dynamic ? IM_COL32(90, 230, 110, 220) : IM_COL32(255, 200, 60, 220);
                    break;
                }
            }
            drawShape(entity, c.Shape, c.Size, c.Center, col);
        }
    }

private:
    // ================================================================== editor shell
    // Menu bar, toolbar, Scene Settings, Console and the create menu, laid out like Unity / Godot.

    // A darker, flatter look with a blue accent (closer to Unity / Godot than ImGui's default)
    static void ApplyEditorStyle() {
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowPadding = ImVec2(8, 8);
        s.FramePadding = ImVec2(6, 4);
        s.ItemSpacing = ImVec2(8, 5);
        s.ItemInnerSpacing = ImVec2(6, 4);
        s.IndentSpacing = 16.0f;
        s.ScrollbarSize = 13.0f;
        s.GrabMinSize = 10.0f;
        s.WindowRounding = 3.0f;
        s.ChildRounding = 3.0f;
        s.FrameRounding = 3.0f;
        s.PopupRounding = 3.0f;
        s.ScrollbarRounding = 6.0f;
        s.GrabRounding = 3.0f;
        s.TabRounding = 3.0f;
        s.WindowBorderSize = 1.0f;
        s.FrameBorderSize = 0.0f;
        s.PopupBorderSize = 1.0f;
        s.TabBorderSize = 0.0f;
        s.WindowMenuButtonPosition = ImGuiDir_None;      // no dock menu triangle on every panel
        s.SeparatorTextBorderSize = 1.0f;

        ImVec4* c = s.Colors;
        const ImVec4 bg(0.125f, 0.13f, 0.145f, 1.0f), panel(0.155f, 0.16f, 0.178f, 1.0f);
        const ImVec4 frame(0.21f, 0.215f, 0.24f, 1.0f), frameHover(0.27f, 0.28f, 0.31f, 1.0f);
        const ImVec4 accent(0.26f, 0.53f, 0.90f, 1.0f), accentDim(0.22f, 0.40f, 0.65f, 1.0f);
        c[ImGuiCol_Text] = ImVec4(0.90f, 0.91f, 0.93f, 1.0f);
        c[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.54f, 0.58f, 1.0f);
        c[ImGuiCol_WindowBg] = panel;
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = ImVec4(0.14f, 0.145f, 0.16f, 0.98f);
        c[ImGuiCol_Border] = ImVec4(0.08f, 0.08f, 0.09f, 1.0f);
        c[ImGuiCol_FrameBg] = frame;
        c[ImGuiCol_FrameBgHovered] = frameHover;
        c[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.31f, 0.35f, 1.0f);
        c[ImGuiCol_TitleBg] = bg;
        c[ImGuiCol_TitleBgActive] = bg;
        c[ImGuiCol_TitleBgCollapsed] = bg;
        c[ImGuiCol_MenuBarBg] = bg;
        c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.31f, 0.34f, 1.0f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.38f, 0.39f, 0.43f, 1.0f);
        c[ImGuiCol_ScrollbarGrabActive] = accentDim;
        c[ImGuiCol_CheckMark] = accent;
        c[ImGuiCol_SliderGrab] = accentDim;
        c[ImGuiCol_SliderGrabActive] = accent;
        c[ImGuiCol_Button] = frame;
        c[ImGuiCol_ButtonHovered] = frameHover;
        c[ImGuiCol_ButtonActive] = accentDim;
        c[ImGuiCol_Header] = ImVec4(0.22f, 0.36f, 0.56f, 0.85f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.42f, 0.64f, 0.80f);
        c[ImGuiCol_HeaderActive] = accent;
        c[ImGuiCol_Separator] = ImVec4(0.24f, 0.25f, 0.28f, 1.0f);
        c[ImGuiCol_SeparatorHovered] = accentDim;
        c[ImGuiCol_SeparatorActive] = accent;
        c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ResizeGripHovered] = accentDim;
        c[ImGuiCol_ResizeGripActive] = accent;
        c[ImGuiCol_Tab] = bg;
        c[ImGuiCol_TabHovered] = frameHover;
        c[ImGuiCol_TabSelected] = panel;
        c[ImGuiCol_TabSelectedOverline] = accent;
        c[ImGuiCol_TabDimmed] = bg;
        c[ImGuiCol_TabDimmedSelected] = panel;
        c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_DockingPreview] = ImVec4(0.26f, 0.53f, 0.90f, 0.5f);
        c[ImGuiCol_DockingEmptyBg] = bg;
        c[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.53f, 0.90f, 0.35f);
        c[ImGuiCol_NavHighlight] = accent;
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);
    }

    // ------------------------------------------------------------------ main menu bar
    void DrawMainMenuBar() {
        using namespace Engine;
        const bool playing = m_PlayState != PlayState::Edit;
        if (!ImGui::BeginMainMenuBar()) return;

        if (ImGui::BeginMenu("File")) {
            ImGui::BeginDisabled(playing);
            if (ImGui::MenuItem("New Scene", "Ctrl+N")) RequestNewScene();
            if (ImGui::BeginMenu("Open Scene")) {
                for (const auto& f : ListSceneFiles())       // rescanned while open: new files show up
                    if (ImGui::MenuItem(f.c_str(), nullptr, f == m_SceneFile)) RequestLoad(f);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S")) SaveScene(m_SceneFile);
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) m_OpenSaveAs = true;
            ImGui::Separator();
            if (ImGui::BeginMenu("Example Levels")) {
                ImGui::TextDisabled("Replaces the current scene");
                if (ImGui::MenuItem("Script Demo")) BuildLevel(BuildScriptDemo);
                if (ImGui::MenuItem("Coin Rush")) BuildLevel(BuildCoinRush);
                ImGui::EndMenu();
            }
            ImGui::EndDisabled();
            if (playing) ImGui::TextDisabled("Stop Play to change scenes");
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Esc")) RequestQuit();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            const std::string undo = m_History.CanUndo() ? "Undo " + m_History.UndoLabel() : "Undo";
            const std::string redo = m_History.CanRedo() ? "Redo " + m_History.RedoLabel() : "Redo";
            if (ImGui::MenuItem(undo.c_str(), "Ctrl+Z", false, !playing && m_History.CanUndo())) UndoStep();
            if (ImGui::MenuItem(redo.c_str(), "Ctrl+Y", false, !playing && m_History.CanRedo())) RedoStep();
            ImGui::Separator();
            const bool selected = m_Selected != entt::null && m_Scene.Registry().valid(m_Selected);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, selected && !playing)) DuplicateSelected();
            if (ImGui::MenuItem("Delete", "Del", false, selected)) DeleteSelected();
            if (ImGui::MenuItem("Deselect", nullptr, false, selected)) m_Selected = entt::null;
            ImGui::Separator();
            if (ImGui::MenuItem("Frame Selected", "F")) FrameSelected();
            ImGui::Separator();
            if (ImGui::MenuItem("Reload Shaders", "F5")) m_Renderer->ReloadShaders();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Create")) {
            DrawCreateMenuItems();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Play")) {
            if (!playing) { if (ImGui::MenuItem("Play", "Ctrl+P")) m_PlayCmd = PlayCmd::Play; }
            else if (ImGui::MenuItem("Stop", "Ctrl+P")) m_PlayCmd = PlayCmd::Stop;
            if (ImGui::MenuItem(m_PlayState == PlayState::Paused ? "Resume" : "Pause", nullptr, false, playing)) m_PlayCmd = PlayCmd::Pause;
            if (ImGui::MenuItem("Step One Frame", nullptr, false, m_PlayState == PlayState::Paused)) m_PlayCmd = PlayCmd::Step;
            ImGui::Separator();
            ImGui::MenuItem("Play Through Game Camera", nullptr, &m_PlayThroughGameCamera);
            ImGui::SetItemTooltip("During Play, show the scene's primary Camera entity.\n"
                "Untick to fly the editor camera (and use editor shortcuts) while the game runs.");
            if (ImGui::MenuItem("Reload Scripts", "F5", false, playing)) m_ReloadScripts = true;
            ImGui::Separator();
            ImGui::TextDisabled("T throws a ball while playing");
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Window")) {
            ImGui::MenuItem("Hierarchy", nullptr, &m_ShowHierarchy);
            ImGui::MenuItem("Inspector", nullptr, &m_ShowInspector);
            ImGui::MenuItem("Scene Settings", nullptr, &m_ShowSceneSettings);
            ImGui::MenuItem("Console", nullptr, &m_ShowConsole);
            ImGui::Separator();
            if (ImGui::MenuItem("Hide Panels", "F1", !m_ShowUI)) m_ShowUI = !m_ShowUI;
            if (ImGui::MenuItem("Full Screen", "F11", GetWindow().IsFullscreen())) m_ToggleFullscreen = true;
            bool vsync = GetWindow().IsVSync();
            if (ImGui::MenuItem("VSync", nullptr, &vsync)) GetWindow().SetVSync(vsync);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Layout")) m_ResetLayout = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Controls...")) m_ShowControls = true;
            if (ImGui::MenuItem("About MDB Engine...")) m_OpenAbout = true;
            ImGui::EndMenu();
        }

        // Right side: the scene file (with * for unsaved changes) and the frame rate
        char right[160];
        std::snprintf(right, sizeof(right), "%s%s    %.0f FPS", m_SceneFile.c_str(), m_History.IsDirty() ? " *" : "",
            ImGui::GetIO().Framerate);
        const float w = ImGui::CalcTextSize(right).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - w);
        ImGui::TextDisabled("%s", right);
        ImGui::EndMainMenuBar();
    }

    // The entries of Create (the menu bar, the Hierarchy "+" button and its right-click menu)
    void DrawCreateMenuItems() {
        using namespace Engine;
        const glm::vec3 spot = m_Camera->Position + m_Camera->GetFront() * 3.0f;   // in front of the camera
        auto place = [&](Entity e) {
            e.Get<TransformComponent>().Position = spot;
            m_Selected = e.GetHandle();
        };
        auto shape = [&](const char* name, std::shared_ptr<Renderable> geometry, float scale) {
            Entity e = m_Scene.CreateEntity(name);
            auto& mr = e.Add<MeshRendererComponent>();
            mr.Geometry = geometry;
            e.Get<TransformComponent>().Scale = glm::vec3(scale);
            place(e);
        };

        if (ImGui::MenuItem("Empty Entity")) place(m_Scene.CreateEntity("Entity"));
        ImGui::SeparatorText("3D Objects");
        if (ImGui::MenuItem("Cube")) shape("Cube", m_Cube, 1.0f);
        if (ImGui::MenuItem("Sphere")) { EnsureSphere(); shape("Sphere", m_Sphere, 1.0f); }
        if (ImGui::MenuItem("Plane")) shape("Plane", m_Plane, 0.25f);
        ImGui::SeparatorText("Lights");
        if (ImGui::MenuItem("Point Light")) {
            Entity e = m_Scene.CreateEntity("Point Light");
            e.Get<TransformComponent>().Scale = glm::vec3(0.15f);
            e.Add<PointLightComponent>();
            auto& mr = e.Add<MeshRendererComponent>();       // a small glowing marker
            mr.Geometry = m_Cube;
            mr.Unlit = true;
            place(e);
        }
        if (ImGui::MenuItem("Directional Light (Sun)")) {
            Entity e = m_Scene.CreateEntity("Sun");
            auto& t = e.Get<TransformComponent>();
            t.Rotation = glm::vec3(-0.9f, 0.5f, 0.0f);
            t.Scale = glm::vec3(0.12f, 0.12f, 0.6f);
            e.Add<DirectionalLightComponent>();
            auto& mr = e.Add<MeshRendererComponent>();
            mr.Geometry = m_Cube;
            mr.Color = glm::vec3(1.0f, 0.9f, 0.4f);
            mr.Unlit = true;
            place(e);
        }
        ImGui::SeparatorText("Gameplay");
        if (ImGui::MenuItem("Camera")) {
            Entity e = m_Scene.CreateEntity("Camera");
            e.Add<CameraComponent>();
            place(e);
        }
        if (ImGui::MenuItem("Physics Box")) SpawnBody(false, spot, glm::vec3(0.0f), true);
        if (ImGui::MenuItem("Physics Ball")) SpawnBody(true, spot, glm::vec3(0.0f), true);
        if (ImGui::MenuItem("Material Grid")) SpawnMaterialGrid();
        ImGui::Separator();
        if (ImGui::BeginMenu("Prefab")) {
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
            ImGui::EndMenu();
        }
    }

    // ------------------------------------------------------------------ toolbar
    // A button that stays highlighted while its tool / option is on
    bool ToggleButton(const char* label, bool on, const char* tooltip, float width = 0.0f) {
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.40f, 0.65f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.46f, 0.74f, 1.0f));
        }
        const bool clicked = ImGui::Button(label, ImVec2(width, 0.0f));
        if (on) ImGui::PopStyleColor(2);
        if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
        return clicked;
    }

    // A square button with a drawn icon: 0 play, 1 stop, 2 pause, 3 step
    bool IconButton(const char* id, int icon, bool on, bool enabled, const char* tooltip) {
        const float h = ImGui::GetFrameHeight();
        const ImVec2 size(h * 1.5f, h);
        ImGui::BeginDisabled(!enabled);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.40f, 0.65f, 1.0f));
        const bool clicked = ImGui::Button(id, size);
        if (on) ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (tooltip) ImGui::SetItemTooltip("%s", tooltip);

        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
        const float r = h * 0.26f;
        const ImU32 col = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        switch (icon) {
        case 0: dl->AddTriangleFilled(ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x - r * 0.8f, c.y + r), ImVec2(c.x + r, c.y), col); break;
        case 1: dl->AddRectFilled(ImVec2(c.x - r * 0.85f, c.y - r * 0.85f), ImVec2(c.x + r * 0.85f, c.y + r * 0.85f), col, 1.5f); break;
        case 2:
            dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x - r * 0.25f, c.y + r), col);
            dl->AddRectFilled(ImVec2(c.x + r * 0.25f, c.y - r), ImVec2(c.x + r * 0.8f, c.y + r), col);
            break;
        case 3:
            dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x - r, c.y + r), ImVec2(c.x + r * 0.45f, c.y), col);
            dl->AddRectFilled(ImVec2(c.x + r * 0.55f, c.y - r), ImVec2(c.x + r, c.y + r), col);
            break;
        }
        return clicked;
    }

    // The strip under the menu bar: transform tools (left), Play / Pause / Step (centre), status (right)
    void DrawToolbar() {
        const bool playing = m_PlayState != PlayState::Edit;
        const float height = ImGui::GetFrameHeight() + 10.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.125f, 0.13f, 0.145f, 1.0f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoScrollWithMouse;
        if (ImGui::BeginViewportSideBar("##Toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, height, flags)) {
            // Transform tools
            if (ToggleButton("Select", m_SelectTool, "Select (Q): click objects, no gizmo")) m_SelectTool = true;
            ImGui::SameLine(0, 2);
            if (ToggleButton("Move", !m_SelectTool && m_GizmoOp == ImGuizmo::TRANSLATE, "Move (W)")) { m_SelectTool = false; m_GizmoOp = ImGuizmo::TRANSLATE; }
            ImGui::SameLine(0, 2);
            if (ToggleButton("Rotate", !m_SelectTool && m_GizmoOp == ImGuizmo::ROTATE, "Rotate (E)")) { m_SelectTool = false; m_GizmoOp = ImGuizmo::ROTATE; }
            ImGui::SameLine(0, 2);
            if (ToggleButton("Scale", !m_SelectTool && m_GizmoOp == ImGuizmo::SCALE, "Scale (R): always in local space")) { m_SelectTool = false; m_GizmoOp = ImGuizmo::SCALE; }
            ImGui::SameLine(0, 12);
            if (ToggleButton(m_GizmoLocal ? "Local" : "World", false, "Gizmo space: click to switch between Local and World", 52.0f))
                m_GizmoLocal = !m_GizmoLocal;
            ImGui::SameLine(0, 2);
            if (ToggleButton("Snap", m_Snap, "Snap moves to 0.5 m, rotations to 15 deg, scales to 0.1")) m_Snap = !m_Snap;

            // Play controls, centred in the window
            const float h = ImGui::GetFrameHeight();
            const float groupWidth = h * 1.5f * 3.0f + 4.0f;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0f, (ImGui::GetWindowWidth() - groupWidth) * 0.5f));
            if (IconButton(playing ? "##stop" : "##play", playing ? 1 : 0, playing, true, playing ? "Stop (Ctrl+P)" : "Play (Ctrl+P)"))
                m_PlayCmd = playing ? PlayCmd::Stop : PlayCmd::Play;
            ImGui::SameLine(0, 2);
            if (IconButton("##pause", 2, m_PlayState == PlayState::Paused, playing, m_PlayState == PlayState::Paused ? "Resume" : "Pause"))
                m_PlayCmd = PlayCmd::Pause;
            ImGui::SameLine(0, 2);
            if (IconButton("##step", 3, false, m_PlayState == PlayState::Paused, "Step one frame (while paused)"))
                m_PlayCmd = PlayCmd::Step;

            // Status and the game-camera switch, on the right
            const char* status = m_PlayState == PlayState::Edit ? "Editing"
                : m_PlayState == PlayState::Play ? "Playing: changes are discarded on Stop" : "Paused: changes are discarded on Stop";
            const ImVec4 statusCol = m_PlayState == PlayState::Edit ? ImVec4(0.55f, 0.57f, 0.6f, 1.0f)
                : m_PlayState == PlayState::Play ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f) : ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
            const float camWidth = ImGui::CalcTextSize("Game camera").x + h + 8.0f;
            const float rightWidth = ImGui::CalcTextSize(status).x + camWidth + 24.0f;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0f, ImGui::GetWindowWidth() - rightWidth));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(statusCol, "%s", status);
            ImGui::SameLine(0, 16);
            ImGui::Checkbox("Game camera", &m_PlayThroughGameCamera);
            ImGui::SetItemTooltip("During Play, look through the scene's primary Camera entity");
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    // ------------------------------------------------------------------ Scene Settings panel
    // What the old Tools panel held: environment, lighting and shadows, post-processing, physics, audio.
    // All of it is saved in the scene file.
    void DrawSceneSettingsPanel() {
        using namespace Engine;
        if (!ImGui::Begin("Scene Settings", &m_ShowSceneSettings)) { ImGui::End(); return; }
        ImGui::PushItemWidth(-140.0f);

        if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
            // The list is rescanned while open; the bake happens at the start of the next frame
            const std::string current = m_Renderer->CurrentEnvironment();
            if (ImGui::BeginCombo("Sky map", current.empty() ? "Procedural sky" : current.c_str())) {
                if (ImGui::Selectable("Procedural sky", current.empty())) m_Renderer->RequestEnvironment("");
                for (const auto& f : m_Renderer->ListEnvironments())
                    if (ImGui::Selectable(f.c_str(), current == f)) m_Renderer->RequestEnvironment(f);
                ImGui::EndCombo();
            }
            ImGui::Checkbox("Show sky", &m_Renderer->GetEnvironment().Show);
            ImGui::SliderFloat("Sky blur", &m_Renderer->GetEnvironment().Blur, 0.0f, 6.0f);
            ImGui::SliderFloat("Sky rotation", &m_Renderer->GetEnvironment().RotationDegrees, -180.0f, 180.0f, "%.0f deg");
            ImGui::SliderFloat("Ambient intensity", &m_Scene.AmbientIntensity, 0.0f, 4.0f);
        }

        if (ImGui::CollapsingHeader("Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& s = m_Renderer->Shadows();
            ImGui::Checkbox("Enabled##shadows", &s.Enabled);
            ImGui::BeginDisabled(!s.Enabled);
            ImGui::SeparatorText("Point lights");
            ImGui::SliderFloat("Range", &s.PointFar, 5.0f, 100.0f, "%.0f m");
            ImGui::SliderFloat("Bias##point", &s.PointBias, 0.005f, 0.3f, "%.3f");
            ImGui::SeparatorText("Sun");
            ImGui::SliderFloat("Bias##sun", &s.SunBias, 0.0002f, 0.01f, "%.4f");
            ImGui::Checkbox("Cascades (follow the camera)", &s.SunCascades);
            if (s.SunCascades) {
                ImGui::SliderInt("Cascade count", &s.CascadeCount, 1, 4);
                ImGui::SliderFloat("Shadow distance", &s.ShadowDistance, 10.0f, 300.0f, "%.0f m");
                ImGui::SliderFloat("Cascade split", &s.CascadeSplit, 0.0f, 1.0f, "%.2f");
                ImGui::SetItemTooltip("0 = even slices, 1 = logarithmic (sharper near the camera)");
            }
            else ImGui::SliderFloat("Area around origin", &s.SunExtent, 4.0f, 40.0f, "%.0f m");
            ImGui::EndDisabled();
        }

        if (ImGui::CollapsingHeader("Post Processing")) {
            ImGui::SeparatorText("Tone mapping");
            ImGui::SliderFloat("Exposure", &m_Renderer->Post.Exposure, 0.1f, 4.0f);
            const char* modes[] = { "None (clamp)", "Reinhard", "ACES" };
            ImGui::Combo("Tone map", &m_Renderer->Post.ToneMap, modes, 3);
            ImGui::Checkbox("Gamma correction", &m_Renderer->Post.Gamma);
            ImGui::Checkbox("FXAA (anti-aliasing)", &m_Renderer->Post.FXAA);

            ImGui::SeparatorText("Ambient occlusion");
            ImGui::Checkbox("Enabled##ao", &m_Renderer->Post.AOEnabled);
            ImGui::BeginDisabled(!m_Renderer->Post.AOEnabled);
            ImGui::SliderFloat("Strength##ao", &m_Renderer->Post.AOStrength, 0.0f, 1.0f);
            ImGui::SliderFloat("Radius##ao", &m_Renderer->GetSSAO().Radius, 0.1f, 2.0f);
            ImGui::SliderFloat("Bias##ao", &m_Renderer->GetSSAO().Bias, 0.0f, 0.1f, "%.3f");
            ImGui::SliderInt("Samples##ao", &m_Renderer->GetSSAO().Samples, 4, 32);
            ImGui::EndDisabled();

            ImGui::SeparatorText("Bloom");
            ImGui::Checkbox("Enabled##bloom", &m_Renderer->Post.BloomEnabled);
            ImGui::BeginDisabled(!m_Renderer->Post.BloomEnabled);
            ImGui::SliderFloat("Intensity##bloom", &m_Renderer->Post.BloomIntensity, 0.0f, 1.0f);
            ImGui::SliderFloat("Threshold##bloom", &m_Renderer->GetBloom().Threshold, 0.0f, 5.0f);
            ImGui::SliderFloat("Knee##bloom", &m_Renderer->GetBloom().Knee, 0.0f, 1.0f);
            ImGui::SliderFloat("Radius##bloom", &m_Renderer->GetBloom().Radius, 0.5f, 2.0f);
            ImGui::SliderFloat("Max brightness##bloom", &m_Renderer->GetBloom().MaxBrightness, 1.0f, 30.0f);
            ImGui::EndDisabled();
        }

        if (ImGui::CollapsingHeader("Physics")) {
            if (ImGui::SliderFloat("Gravity", &m_Gravity, 0.0f, 30.0f, "%.2f m/s2")) m_Session->Physics().SetGravity(m_Gravity);
            ImGui::TextDisabled("Bodies: %u (they exist during Play)", m_Session->Physics().BodyCount());
        }

        if (ImGui::CollapsingHeader("Audio")) {
            if (!m_Session->Audio().IsReady()) ImGui::TextDisabled("No audio device: sound is off");
            else {
                float master = m_Session->Audio().GetMasterVolume();
                if (ImGui::SliderFloat("Master volume", &master, 0.0f, 1.0f)) m_Session->Audio().SetMasterVolume(master);
                ImGui::TextDisabled("Voices playing: %u", m_Session->Audio().VoiceCount());
            }
        }

        if (ImGui::CollapsingHeader("Scripts")) {
            if (m_Session->Scripts().IsRunning()) {
                ImGui::Text("Running: %u", m_Session->Scripts().InstanceCount());
                ImGui::SameLine();
                if (ImGui::SmallButton("Reload (F5)")) m_ReloadScripts = true;
            }
            else ImGui::TextDisabled("Scripts load when Play starts");
            ImGui::TextDisabled("Errors appear in the Console");
        }
        ImGui::PopItemWidth();
        ImGui::End();
    }

    // ------------------------------------------------------------------ Console panel
    // Every log line (engine, game, scripts), with level filters, a search box and auto-scroll
    void DrawConsolePanel() {
        if (!ImGui::Begin("Console", &m_ShowConsole)) { ImGui::End(); return; }
        const uint64_t count = Engine::Log::HistoryCount();
        if (count != m_ConsoleSeen) { m_ConsoleLines = Engine::Log::History(); m_ConsoleSeen = count; }

        if (ImGui::Button("Clear")) { Engine::Log::ClearHistory(); m_ConsoleLines.clear(); }
        ImGui::SameLine();
        int infos = 0, warns = 0, errors = 0;
        for (const auto& l : m_ConsoleLines) (l.Level >= 4 ? errors : l.Level == 3 ? warns : infos)++;
        char label[48];
        std::snprintf(label, sizeof(label), "Info %d", infos);
        ImGui::Checkbox(label, &m_ConsoleInfo);
        ImGui::SameLine();
        std::snprintf(label, sizeof(label), "Warnings %d", warns);
        ImGui::Checkbox(label, &m_ConsoleWarn);
        ImGui::SameLine();
        std::snprintf(label, sizeof(label), "Errors %d", errors);
        ImGui::Checkbox(label, &m_ConsoleError);
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &m_ConsoleAutoScroll);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##consolefilter", "Search", m_ConsoleFilter, sizeof(m_ConsoleFilter));
        ImGui::Separator();

        ImGui::BeginChild("##lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        if (m_MonoFont) ImGui::PushFont(m_MonoFont);
        const std::string filter = Lower(m_ConsoleFilter);
        for (const auto& l : m_ConsoleLines) {
            if (l.Level >= 4 ? !m_ConsoleError : l.Level == 3 ? !m_ConsoleWarn : !m_ConsoleInfo) continue;
            if (!filter.empty() && Lower(l.Text).find(filter) == std::string::npos) continue;
            const ImVec4 col = l.Level >= 4 ? ImVec4(1.0f, 0.42f, 0.38f, 1.0f) : l.Level == 3 ? ImVec4(1.0f, 0.8f, 0.35f, 1.0f)
                : l.Logger == "GAME" ? ImVec4(0.85f, 0.87f, 0.9f, 1.0f) : ImVec4(0.6f, 0.63f, 0.68f, 1.0f);
            ImGui::TextDisabled("%s", l.Time.c_str());
            ImGui::SameLine();
            ImGui::TextColored(col, "%s", l.Text.c_str());
        }
        if (m_ConsoleAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
        if (m_MonoFont) ImGui::PopFont();
        ImGui::EndChild();
        ImGui::End();
    }

    static std::string Lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    // ------------------------------------------------------------------ dialogs
    void DrawDialogs() {
        // Save As
        if (m_OpenSaveAs) { ImGui::OpenPopup("Save Scene As"); m_OpenSaveAs = false; }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Save Scene As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("File name in Assets/Scenes:");
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(280.0f);
            const bool enter = ImGui::InputTextWithHint("##saveasname", "new_level", m_SaveAsName, sizeof(m_SaveAsName),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::BeginDisabled(m_SaveAsName[0] == '\0');
            if (ImGui::Button("Save", ImVec2(120, 0)) || (enter && m_SaveAsName[0] != '\0')) {
                std::string name = m_SaveAsName;
                if (std::filesystem::path(name).extension() != ".json") name += ".json";
                if (SaveScene(name)) { m_SceneFile = name; m_SaveAsName[0] = '\0'; }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // About
        if (m_OpenAbout) { ImGui::OpenPopup("About MDB Engine"); m_OpenAbout = false; }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("About MDB Engine", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("MDB Engine editor");
            ImGui::TextDisabled("C++20, OpenGL 4.5, EnTT, Jolt Physics, Lua 5.4, miniaudio, Dear ImGui");
            ImGui::TextDisabled("%s", (const char*)glGetString(GL_RENDERER));
            if (ImGui::Button("Close", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // Controls (a normal window, so it can stay open while working)
        if (m_ShowControls) {
            ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
            if (ImGui::Begin("Controls", &m_ShowControls, ImGuiWindowFlags_NoDocking)) {
                auto row = [](const char* keys, const char* what) {
                    ImGui::TextUnformatted(keys);
                    ImGui::SameLine(170.0f);
                    ImGui::TextDisabled("%s", what);
                };
                ImGui::SeparatorText("Viewport camera");
                row("Alt + Left drag", "Orbit");
                row("Alt + Middle drag", "Pan");
                row("Alt + Right drag / wheel", "Zoom");
                row("F", "Frame the selection");
                ImGui::SeparatorText("Tools");
                row("Q / W / E / R", "Select / Move / Rotate / Scale");
                row("N", "Spawn a cube");
                row("Del", "Delete the selection");
                ImGui::SeparatorText("Edit");
                row("Ctrl+Z / Ctrl+Y", "Undo / Redo");
                row("Ctrl+D", "Duplicate");
                row("Ctrl+N / Ctrl+S", "New scene / Save");
                row("Ctrl+Shift+S", "Save as");
                ImGui::SeparatorText("Play");
                row("Ctrl+P", "Play / Stop");
                row("T", "Throw a ball (while playing)");
                row("F5", "Reload shaders (and scripts while playing)");
                ImGui::SeparatorText("Window");
                row("F1", "Hide the panels");
                row("F11", "Full screen on / off");
                row("Esc", "Stop Play, or quit");
            }
            ImGui::End();
        }
    }

    // ------------------------------------------------------------------ new scene
    void RequestNewScene() {
        if (m_History.IsDirty()) m_Confirm = ConfirmAction::New;
        else m_PendingNewScene = true;
    }

    // An empty level: a sun and a floor. Settings (sky, post...) are kept as they are.
    void NewScene() {
        using namespace Engine;
        m_Scene.Clear();
        m_Selected = entt::null;
        m_Spawned.clear();
        {
            Entity e = m_Scene.CreateEntity("Sun");
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
        {
            Entity e = m_Scene.CreateEntity("Floor");
            e.Get<TransformComponent>().Position = glm::vec3(0.0f, -1.0f, 0.0f);
            auto& mr = e.Add<MeshRendererComponent>();
            mr.Geometry = m_Plane;
            mr.Texture = m_Checker;
            mr.Color = glm::vec3(0.8f);
            mr.Roughness = 0.85f;
            mr.Static = true;
            auto& rb = e.Add<RigidBodyComponent>();
            rb.Type = BodyType::Static;
            rb.Size = glm::vec3(12.0f, 1.0f, 12.0f);
            rb.Center = glm::vec3(0.0f, -0.5f, 0.0f);
        }
        m_SceneFile = "untitled.json";
        m_History.Reset(m_Scene, m_Selected, m_Resources->Registry());
        LOG_INFO("New scene (File > Save As to name it)");
    }

    void DeleteSelected() {
        if (m_Selected == entt::null || !m_Scene.Registry().valid(m_Selected)) return;
        m_Scene.DestroyEntity(Engine::Entity(m_Selected, &m_Scene));
        m_Selected = entt::null;
    }

    // Stats drawn over the top-right corner of the viewport image
    void DrawStatsOverlay(const ImVec2& origin, const ImVec2& size) {
        using namespace Engine;
        char lines[9][96];
        int n = 0;
        std::snprintf(lines[n++], 96, "%.0f FPS  (%.2f ms)", ImGui::GetIO().Framerate, 1000.0f / std::max(ImGui::GetIO().Framerate, 1.0f));
        std::snprintf(lines[n++], 96, "Draw calls  %u", RenderStats::DrawCalls);
        std::snprintf(lines[n++], 96, "Triangles   %u", RenderStats::Triangles);
        std::snprintf(lines[n++], 96, "Instances   %u", RenderStats::Instances);
        std::snprintf(lines[n++], 96, "Culled      %u", RenderStats::Culled);
        std::snprintf(lines[n++], 96, "Shadow skip %u", RenderStats::ShadowSkipped);
        if (m_Scene.StaticBatchCount() > 0)
            std::snprintf(lines[n++], 96, "Static      %u batches / %u objects", m_Scene.StaticBatchCount(), m_Scene.StaticMergedCount());
        std::snprintf(lines[n++], 96, "Entities    %zu", m_Scene.Registry().view<TagComponent>().size());
        std::snprintf(lines[n++], 96, "Camera      %.1f, %.1f, %.1f", m_Camera->Position.x, m_Camera->Position.y, m_Camera->Position.z);

        if (m_MonoFont) ImGui::PushFont(m_MonoFont);
        float w = 0.0f;
        for (int i = 0; i < n; i++) w = std::max(w, ImGui::CalcTextSize(lines[i]).x);
        const float lh = ImGui::GetTextLineHeight();
        const ImVec2 pad(8.0f, 6.0f);
        const ImVec2 mx(origin.x + size.x - 8.0f, origin.y + 8.0f + pad.y * 2 + lh * n);
        const ImVec2 mn(mx.x - w - pad.x * 2, origin.y + 8.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(mn, mx, IM_COL32(15, 16, 20, 175), 4.0f);
        for (int i = 0; i < n; i++)
            dl->AddText(ImVec2(mn.x + pad.x, mn.y + pad.y + lh * i), i == 0 ? IM_COL32(255, 255, 255, 235) : IM_COL32(205, 210, 220, 215), lines[i]);
        if (m_MonoFont) ImGui::PopFont();
    }

    void DrawHierarchyPanel() {
        using namespace Engine;
        ImGui::SetNextWindowPos(ImVec2(10, 230), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300, 280), ImGuiCond_FirstUseEver);
        ImGui::Begin("Hierarchy", &m_ShowHierarchy);

        // "+" opens the Create menu; the box filters the list by name
        if (ImGui::Button("+", ImVec2(ImGui::GetFrameHeight() * 1.4f, 0.0f))) ImGui::OpenPopup("HierarchyCreate");
        ImGui::SetItemTooltip("Create (also in the Create menu, or right-click the list)");
        if (ImGui::BeginPopup("HierarchyCreate")) { DrawCreateMenuItems(); ImGui::EndPopup(); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##hierarchyfilter", "Search", m_HierarchyFilter, sizeof(m_HierarchyFilter));
        ImGui::Separator();

        auto& reg = m_Scene.Registry();
        m_Reparent = { entt::null, entt::null, false };
        m_RowAction = { RowAction::None, entt::null };
        const std::string filter = Lower(m_HierarchyFilter);
        ImGui::BeginChild("##entities");
        if (!filter.empty()) {
            // Searching: a flat list of the matches
            for (auto handle : reg.view<TagComponent>()) {
                const std::string& name = reg.get<TagComponent>(handle).Tag;
                if (Lower(name).find(filter) == std::string::npos) continue;
                ImGui::PushID((int)entt::to_integral(handle));
                if (ImGui::Selectable(name.c_str(), handle == m_Selected)) m_Selected = handle;
                DrawRowContextMenu(handle);
                ImGui::PopID();
            }
        }
        else {
            // A tree: roots first, children nested. Drag an entity onto another to make it a child,
            // or onto the empty space below the list to make it a root again.
            std::vector<entt::entity> roots;
            for (auto handle : reg.view<TagComponent>())
                if (m_Scene.GetParent(handle) == entt::null) roots.push_back(handle);
            for (auto r : roots) DrawHierarchyNode(r, 0);
        }

        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y, 30.0f)));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("MDB_ENTITY"))
                m_Reparent = { *(const entt::entity*)p->Data, entt::null, true };
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemClicked()) m_Selected = entt::null;           // a click on empty space deselects
        if (ImGui::BeginPopupContextWindow("HierarchyEmpty", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            DrawCreateMenuItems();
            ImGui::EndPopup();
        }
        ImGui::EndChild();
        ImGui::End();

        // Applied after the tree is drawn, so the views above aren't changed while being walked
        if (m_Reparent.Pending && reg.valid(m_Reparent.Child)) {
            if (!m_Scene.SetParent(m_Reparent.Child, m_Reparent.Parent, true))
                LOG_WARN("Can't parent an entity to itself or to one of its own children");
        }
        if (m_RowAction.Kind != RowAction::None && reg.valid(m_RowAction.Target)) {
            m_Selected = m_RowAction.Target;
            if (m_RowAction.Kind == RowAction::Duplicate) DuplicateSelected();
            else if (m_RowAction.Kind == RowAction::Delete) DeleteSelected();
            else if (m_RowAction.Kind == RowAction::Unparent) m_Scene.SetParent(m_RowAction.Target, entt::null, true);
        }
    }

    // Right-click on a row: actions on that entity (applied after the list is drawn)
    void DrawRowContextMenu(entt::entity e) {
        if (!ImGui::BeginPopupContextItem()) return;     // keyed to the row just drawn
        m_Selected = e;
        const bool playing = m_PlayState != PlayState::Edit;
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !playing)) m_RowAction = { RowAction::Duplicate, e };
        if (ImGui::MenuItem("Delete", "Del")) m_RowAction = { RowAction::Delete, e };
        if (ImGui::MenuItem("Unparent", nullptr, false, m_Scene.GetParent(e) != entt::null)) m_RowAction = { RowAction::Unparent, e };
        ImGui::Separator();
        if (ImGui::MenuItem("Frame", "F")) FrameSelected();
        ImGui::EndPopup();
    }

    enum class RowAction { None, Duplicate, Delete, Unparent };
    struct PendingRowAction { RowAction Kind; entt::entity Target; };
    PendingRowAction m_RowAction{ RowAction::None, entt::null };

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
        DrawRowContextMenu(e);

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
            const bool isPart = e.Get<PrefabComponent>().Node != 0;
            ImGui::TextColored(kPrefabColor, isPart ? "Part of prefab  %s" : "Prefab  %s",
                std::filesystem::path(path).filename().string().c_str());
            ImGui::SameLine();
            if (!loadable)     ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "(file missing or broken)");
            else if (modified) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "(%d override%s)", (int)overrides.size(), overrides.size() == 1 ? "" : "s");

            // Apply works on the whole instance (its root and every part), whichever part is selected.
            // Always available: adding or deleting children changes the instance without overriding anything.
            ImGui::BeginDisabled(playing || !loadable);
            if (ImGui::Button("Apply to prefab")) {
                const int n = prefabs.Apply(m_Scene, e);
                m_PrefabStatus = n < 0 ? "Could not write the prefab" : "Applied: " + std::to_string(n) + " other instance(s) updated";
                m_PrefabStatusUntil = m_RunTime + 4.0f;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Write this whole instance (root, parts and their children) to %s.\n"
                    "Other instances follow, keeping their own overrides.", path.c_str());
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!modified);
            if (ImGui::Button("Revert all")) prefabs.Revert(e);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Drop every override of this entity: take its prefab node again");
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
        ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_MenuBar);
        ImGui::PopStyleVar();

        // The viewport's own menu: what is drawn over the scene, debug views, the camera
        if (ImGui::BeginMenuBar()) {
            ImGui::Dummy(ImVec2(2.0f, 0.0f));
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Stats", nullptr, &m_ShowStatsOverlay);
                ImGui::MenuItem("Colliders", nullptr, &m_ShowColliders);
                ImGui::MenuItem("Camera Frustums", nullptr, &m_ShowFrustums);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Debug")) {
                ImGui::MenuItem("Ambient Occlusion Only", nullptr, &m_Renderer->Post.AODebug);
                ImGui::MenuItem("Shadow Cascades", nullptr, &m_Renderer->Shadows().CascadeDebug);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Camera")) {
                if (ImGui::MenuItem("Frame Selected", "F")) FrameSelected();
                ImGui::SetNextItemWidth(160.0f);
                ImGui::SliderFloat("Orbit speed", &m_Camera->OrbitSensitivity, 0.05f, 1.0f);
                ImGui::TextDisabled("Alt + left / middle / right drag: orbit / pan / zoom");
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        ImVec2 avail = ImGui::GetContentRegionAvail();
        m_ViewportSize = avail;

        ImGui::Image((ImTextureID)(intptr_t)m_Renderer->GetOutput(),
            avail, ImVec2(0, 1), ImVec2(1, 0));

        const bool imageHovered = ImGui::IsItemHovered();
        m_ViewportHovered = imageHovered;
        m_ViewportOrigin = ImGui::GetItemRectMin();
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
        if (!m_ViewingGame && m_ShowFrustums) DrawCameraFrustums(origin, size);
        if (m_PlayState != PlayState::Edit)   // kept while paused: the scripts did not run again
            DrawScriptUI(ImGui::GetWindowDrawList(), m_GameFont, m_Session->Scripts().UIItems(), origin, size, &m_UITextures);
        if (m_ShowStatsOverlay && !m_ViewingGame) DrawStatsOverlay(origin, size);
        if (m_ViewingGame) {
            ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + 8.0f, origin.y + size.y - 22.0f),
                IM_COL32(255, 255, 255, 110), "Game camera (Play > Play Through Game Camera)");
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

    // The standard layout (Unity / Godot style): Hierarchy left, Viewport centre, Console below it,
    // Inspector and Scene Settings as tabs on the right. Built when there is no editor.ini yet, or on
    // Window > Reset Layout.
    void BuildDefaultLayoutOnce(ImGuiID dockspace) {
        if (m_LayoutChecked && !m_ResetLayout) return;
        const bool force = m_ResetLayout;
        m_LayoutChecked = true;
        m_ResetLayout = false;
        if (!force && std::filesystem::exists("editor.ini")) return;
        m_ShowHierarchy = m_ShowInspector = m_ShowSceneSettings = m_ShowConsole = m_ShowUI = true;

        ImGui::DockBuilderRemoveNode(dockspace);
        ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);

        ImGuiID center = dockspace;
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
        ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.24f, nullptr, &center);

        ImGui::DockBuilderDockWindow("Hierarchy", left);
        ImGui::DockBuilderDockWindow("Scene Settings", right);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Console", bottom);
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderFinish(dockspace);
        m_FocusInspector = true;           // the Inspector tab in front of Scene Settings
    }

    void DrawInspectorPanel() {
        using namespace Engine;
        ImGui::SetNextWindowPos(ImVec2(940, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 520), ImGuiCond_FirstUseEver);
        if (m_FocusInspector) { ImGui::SetNextWindowFocus(); m_FocusInspector = false; }
        ImGui::Begin("Inspector", &m_ShowInspector);

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
            // Geometry: a built-in shape or any model file under Assets/Models (listed when opened)
            {
                const auto registry = m_Resources->Registry();
                std::string current = registry.GeometryName(mr.Geometry);
                if (current.rfind("model:", 0) == 0) current = current.substr(6);
                if (ImGui::BeginCombo("Geometry", current.empty() ? "(custom)" : current.c_str())) {
                    for (const char* builtin : { "cube", "sphere", "plane" })
                        if (ImGui::Selectable(builtin, current == builtin)) {
                            mr.Geometry = registry.FindGeometry(builtin);
                            mr.LocalOffset = glm::mat4(1.0f);
                            mr.UseModelMaterials = false;
                        }
                    ImGui::Separator();
                    std::error_code ec;
                    std::vector<std::string> files;
                    for (const auto& f : std::filesystem::directory_iterator(std::filesystem::path(ASSET_DIR) / "Models", ec)) {
                        std::string ext = f.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                        if (ext == ".glb" || ext == ".gltf" || ext == ".fbx" || ext == ".obj") files.push_back("Models/" + f.path().filename().string());
                    }
                    std::sort(files.begin(), files.end());
                    for (const auto& file : files) {
                        if (!ImGui::Selectable(file.c_str(), current == file)) continue;
                        if (auto model = m_Resources->LoadModel(file)) {
                            mr.Geometry = model;
                            mr.LocalOffset = model->GetGroundedMatrix(2.0f);   // 2 m across, standing on the origin
                            mr.UseModelMaterials = model->HasTexturedMaterials();
                            if (mr.UseModelMaterials) mr.Color = glm::vec3(1.0f);
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::ColorEdit3("Color", &mr.Color.x);
            if (!mr.UseModelMaterials) {
                ImGui::SliderFloat("Metallic", &mr.Metallic, 0.0f, 1.0f);
                ImGui::SliderFloat("Roughness", &mr.Roughness, 0.04f, 1.0f);
                ImGui::ColorEdit3("Emissive", &mr.Emissive.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
            }
            ImGui::Checkbox("Unlit", &mr.Unlit);
            ImGui::SameLine();
            ImGui::Checkbox("Cast shadows", &mr.CastShadows);
            ImGui::Checkbox("Static", &mr.Static);
            ImGui::SetItemTooltip("Never moves or changes during Play: merged with other static\n"
                "parts of the same material when Play starts (fewer draws)");
            {
                // Transparency
                static const char* modes[] = { "Opaque", "Cutout (alpha test)", "Blend (see-through)" };
                int mode = (int)mr.Alpha;
                if (ImGui::Combo("Alpha", &mode, modes, IM_ARRAYSIZE(modes))) mr.Alpha = (AlphaMode)mode;
                if (ImGui::SliderFloat("Opacity", &mr.Opacity, 0.0f, 1.0f) && mr.Opacity < 1.0f && mr.Alpha == AlphaMode::Opaque)
                    mr.Alpha = AlphaMode::Blend;                  // dragging opacity down means see-through
                if (mr.Alpha == AlphaMode::Cutout) ImGui::SliderFloat("Alpha cutoff", &mr.AlphaCutoff, 0.0f, 1.0f);
                if (mr.Alpha == AlphaMode::Opaque && mr.Opacity < 1.0f)
                    ImGui::TextDisabled("Opacity is used by Cutout and Blend only");
            }
            if (mr.Geometry && mr.Geometry->GetSubmeshMaterial(0)) {
                ImGui::Checkbox("Use model materials", &mr.UseModelMaterials);
                if (mr.UseModelMaterials) {
                    ImGui::TextDisabled("Color tints the model's own materials");
                    if (ImGui::TreeNode("Materials")) {
                        for (uint32_t i = 0; i < mr.Geometry->SubmeshCount(); i++) {
                            const Material* mat = mr.Geometry->GetSubmeshMaterial(i);
                            ImGui::BulletText("%s  M:%.2f R:%.2f%s%s%s", mat->Name.empty() ? "(unnamed)" : mat->Name.c_str(),
                                mat->Metallic, mat->Roughness,
                                mat->Albedo ? " [tex]" : "", mat->Normal ? " [normal]" : "",
                                mat->Mode == AlphaMode::Blend ? " [blend]" : mat->Mode == AlphaMode::Cutout ? " [cutout]" : "");
                        }
                        ImGui::TreePop();
                    }
                }
            }
            if (!mr.UseModelMaterials) {
                // Texture: none, the built-in checker, or an image from Assets/Textures (its alpha is used
                // by Cutout and Blend)
                const auto registry = m_Resources->Registry();
                std::string current = mr.Texture ? registry.TextureName(mr.Texture) : "(none)";
                if (current.rfind("texture:", 0) == 0) current = current.substr(8);
                if (ImGui::BeginCombo("Texture", current.empty() ? "(custom)" : current.c_str())) {
                    if (ImGui::Selectable("(none)", !mr.Texture)) mr.Texture = nullptr;
                    if (ImGui::Selectable("checker", current == "checker")) mr.Texture = m_Checker;
                    ImGui::Separator();
                    std::error_code ec;
                    std::vector<std::string> files;
                    for (const auto& f : std::filesystem::directory_iterator(std::filesystem::path(ASSET_DIR) / "Textures", ec)) {
                        std::string ext = f.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp")
                            files.push_back("Textures/" + f.path().filename().string());
                    }
                    std::sort(files.begin(), files.end());
                    for (const auto& file : files)
                        if (ImGui::Selectable(file.c_str(), current == file))
                            if (auto tex = m_Resources->LoadTexture(file)) mr.Texture = tex;
                    ImGui::EndCombo();
                }
            }
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
            ShapeFields(e, rb.Shape, rb.Size, rb.Center, true, rb.Type == BodyType::Dynamic && !rb.IsTrigger);

            if (rb.Type == BodyType::Dynamic) ImGui::DragFloat("Mass", &rb.Mass, 0.05f, 0.01f, 1000.0f);
            ImGui::SliderFloat("Friction", &rb.Friction, 0.0f, 1.0f);
            ImGui::SliderFloat("Bounciness", &rb.Restitution, 0.0f, 1.0f);
            ImGui::Checkbox("Trigger", &rb.IsTrigger);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reports overlaps to scripts (on_trigger_enter / exit) instead of colliding.\n"
                    "Triggers follow their transform like kinematic bodies and detect dynamic bodies.");

            if (ImGui::Button("Remove##rb")) e.Remove<RigidBodyComponent>();
            if (rb.Type != BodyType::Static && (e.Has<SpinComponent>() || e.Has<OrbitComponent>()) && rb.Type == BodyType::Dynamic)
                ImGui::TextDisabled("Physics overrides Spin/Orbit on dynamic bodies");
            if (m_PlayState != PlayState::Edit) ImGui::TextDisabled("Changes apply on the next Play");
        }

        if (e.Has<AnimatorComponent>() && ImGui::CollapsingHeader("Animator", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& an = e.Get<AnimatorComponent>();
            const auto* mr = e.Has<MeshRendererComponent>() ? &e.Get<MeshRendererComponent>() : nullptr;
            const Model* model = mr ? dynamic_cast<const Model*>(mr->Geometry.get()) : nullptr;
            if (!model || !model->IsSkinned()) {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Needs a Mesh Renderer with an animated model");
            }
            else {
                if (ImGui::BeginCombo("Clip", an.Clip.empty() ? "(rest pose)" : an.Clip.c_str())) {
                    if (ImGui::Selectable("(rest pose)", an.Clip.empty())) an.Clip.clear();
                    for (const auto& name : model->AnimationNames())
                        if (ImGui::Selectable(name.c_str(), an.Clip == name)) an.Clip = name;
                    ImGui::EndCombo();
                }
                ImGui::SliderFloat("Speed##anim", &an.Speed, -2.0f, 3.0f);
                ImGui::Checkbox("Loop##anim", &an.Loop);
                ImGui::SameLine();
                ImGui::Checkbox("Playing##anim", &an.Playing);
                ImGui::DragFloat("Blend time", &an.BlendTime, 0.01f, 0.0f, 2.0f, "%.2f s");
                const float duration = model->AnimationDuration(model->FindAnimation(an.ActiveClip));
                if (duration > 0.0f) ImGui::SliderFloat("Time##anim", &an.Time, 0.0f, duration, "%.2f s");
                ImGui::TextDisabled("%u bones, %d animations. Plays during Play; drag Time to preview.",
                    model->BoneCount(), (int)model->AnimationNames().size());

                auto clipCombo = [&](const char* label, std::string& clip) {
                    if (ImGui::BeginCombo(label, clip.empty() ? "(none)" : clip.c_str())) {
                        for (const auto& name : model->AnimationNames())
                            if (ImGui::Selectable(name.c_str(), clip == name)) clip = name;
                        ImGui::EndCombo();
                    }
                };

                // Root motion: the root bone's sideways travel moves the entity
                ImGui::Checkbox("Root motion", &an.RootMotion);
                ImGui::SetItemTooltip("The root bone's sideways travel in the clip moves the entity\n"
                    "(use no Rigid Body, or a kinematic one)");
                if (an.RootMotion) {
                    if (ImGui::BeginCombo("Root bone", an.RootBone.empty() ? "(automatic)" : an.RootBone.c_str())) {
                        if (ImGui::Selectable("(automatic)", an.RootBone.empty())) an.RootBone.clear();
                        for (const auto& b : model->NodeNames())
                            if (ImGui::Selectable(b.c_str(), an.RootBone == b)) an.RootBone = b;
                        ImGui::EndCombo();
                    }
                }

                // Layers: another clip over the base one, on a bone and everything below it
                if (ImGui::TreeNode("Layers", "Layers (%d)", (int)an.Layers.size())) {
                    const auto bones = model->NodeNames();
                    int removeLayer = -1;
                    for (int i = 0; i < (int)an.Layers.size(); i++) {
                        auto& L = an.Layers[i];
                        ImGui::PushID(i);
                        ImGui::SeparatorText(("Layer " + std::to_string(i + 1)).c_str());
                        clipCombo("Clip##layer", L.Clip);
                        if (ImGui::BeginCombo("Mask", L.Mask.empty() ? "(whole skeleton)" : L.Mask.c_str())) {
                            if (ImGui::Selectable("(whole skeleton)", L.Mask.empty())) L.Mask.clear();
                            for (const auto& b : bones)
                                if (ImGui::Selectable(b.c_str(), L.Mask == b)) L.Mask = b;
                            ImGui::EndCombo();
                        }
                        ImGui::SetItemTooltip("The layer moves this bone and everything below it");
                        ImGui::SliderFloat("Weight", &L.Weight, 0.0f, 1.0f);
                        ImGui::SliderFloat("Speed##layer", &L.Speed, -2.0f, 3.0f);
                        ImGui::Checkbox("Loop##layer", &L.Loop);
                        ImGui::SameLine();
                        ImGui::Checkbox("Additive", &L.Additive);
                        ImGui::SetItemTooltip("Add the clip's motion (relative to its first frame) instead of replacing the pose");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove layer")) removeLayer = i;
                        ImGui::PopID();
                    }
                    if (removeLayer >= 0) an.Layers.erase(an.Layers.begin() + removeLayer);
                    if (an.Layers.size() < 8 && ImGui::Button("Add layer")) {
                        AnimLayer L;
                        L.Clip = an.Clip;
                        an.Layers.push_back(L);
                    }
                    ImGui::TreePop();
                }

                // Events: on_anim_event(self, name, clip) in scripts; "sound:Sounds/x.wav" plays a sound
                if (ImGui::TreeNode("Events", "Events (%d)", (int)an.Events.size())) {
                    int removeEvent = -1;
                    for (int i = 0; i < (int)an.Events.size(); i++) {
                        auto& ev = an.Events[i];
                        ImGui::PushID(1000 + i);
                        ImGui::SeparatorText(("Event " + std::to_string(i + 1)).c_str());
                        char buf[128];
                        std::snprintf(buf, sizeof(buf), "%s", ev.Name.c_str());
                        if (ImGui::InputText("Name##event", buf, sizeof(buf))) ev.Name = buf;
                        clipCombo("Clip##event", ev.Clip);
                        const float d = model->AnimationDuration(model->FindAnimation(ev.Clip));
                        ImGui::SliderFloat("Time##event", &ev.Time, 0.0f, std::max(d, 0.01f), "%.2f s");
                        if (ImGui::SmallButton("Remove event")) removeEvent = i;
                        ImGui::PopID();
                    }
                    if (removeEvent >= 0) an.Events.erase(an.Events.begin() + removeEvent);
                    if (ImGui::Button("Add event at current time"))
                        an.Events.push_back({ an.ActiveClip.empty() ? an.Clip : an.ActiveClip, an.Time, "event" });
                    ImGui::TextDisabled("Scripts: on_anim_event(self, name, clip).\nName \"sound:Sounds/x.wav\" plays that sound.");
                    ImGui::TreePop();
                }

                // IK: look at / reach for a target, on the final pose (also previews in Edit mode)
                if (ImGui::TreeNode("IK", "IK (%d)", (int)an.IK.size())) {
                    const auto bones = model->NodeNames();
                    int removeIK = -1;
                    for (int i = 0; i < (int)an.IK.size(); i++) {
                        auto& ik = an.IK[i];
                        ImGui::PushID(2000 + i);
                        ImGui::SeparatorText(("IK " + std::to_string(i + 1)).c_str());
                        int type = (int)ik.Type;
                        static const char* types[] = { "Look at (head)", "Limb (two bones)" };
                        if (ImGui::Combo("Type", &type, types, IM_ARRAYSIZE(types))) ik.Type = (IKType)type;
                        if (ImGui::BeginCombo("Bone##ik", ik.Bone.empty() ? "(choose)" : ik.Bone.c_str())) {
                            for (const auto& b : bones)
                                if (ImGui::Selectable(b.c_str(), ik.Bone == b)) ik.Bone = b;
                            ImGui::EndCombo();
                        }
                        ImGui::SetItemTooltip(ik.Type == IKType::Look ? "The head: it and Chain-1 parents turn"
                                                                      : "The end of the limb (paw, foot, hand)");
                        // Target: another entity, or a world point
                        const entt::entity targetEntity = ik.TargetID ? m_Scene.FindByID(ik.TargetID) : entt::null;
                        const std::string targetName = ik.TargetID == 0 ? "(world point)"
                            : targetEntity != entt::null ? m_Scene.Registry().get<TagComponent>(targetEntity).Tag : "(missing)";
                        if (ImGui::BeginCombo("Target", targetName.c_str())) {
                            if (ImGui::Selectable("(world point)", ik.TargetID == 0)) ik.TargetID = 0;
                            for (auto [te, tag, id] : m_Scene.Registry().view<TagComponent, IDComponent>().each()) {
                                if (te == e.GetHandle()) continue;
                                ImGui::PushID((int)entt::to_integral(te));
                                if (ImGui::Selectable(tag.Tag.c_str(), ik.TargetID == id.ID)) ik.TargetID = id.ID;
                                ImGui::PopID();
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::DragFloat3(ik.TargetID ? "Offset" : "Point", &ik.Target.x, 0.02f);
                        ImGui::SliderFloat("Weight##ik", &ik.Weight, 0.0f, 1.0f);
                        if (ik.Type == IKType::Look) {
                            ImGui::SliderFloat("Max angle", &ik.MaxAngle, 0.0f, 180.0f, "%.0f deg");
                            ImGui::SliderInt("Chain", &ik.Chain, 1, 4);
                            ImGui::SetItemTooltip("How many bones share the turn: 1 = the head only, 2 = head and neck...");
                        }
                        else {
                            ImGui::DragFloat3("Pole", &ik.Pole.x, 0.02f);
                            ImGui::SetItemTooltip("Which way the middle joint bends, in the entity's space (0 = keep the animation's bend)");
                        }
                        if (ImGui::SmallButton("Remove IK")) removeIK = i;
                        ImGui::PopID();
                    }
                    if (removeIK >= 0) an.IK.erase(an.IK.begin() + removeIK);
                    if (an.IK.size() < 8 && ImGui::Button("Add IK")) an.IK.push_back({});
                    ImGui::TextDisabled("Look faces the target within Max angle of the entity's +Z.\nScripts: set_ik, ik_weight, bone_position.");
                    ImGui::TreePop();
                }
            }
            if (ImGui::Button("Remove##animator")) e.Remove<AnimatorComponent>();
        }

        if (e.Has<ColliderComponent>() && ImGui::CollapsingHeader("Collider", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& c = e.Get<ColliderComponent>();
            // The body this shape joins: the nearest ancestor with a Rigid Body
            entt::entity body = entt::null;
            int guard = 0;
            for (entt::entity p = m_Scene.GetParent(e.GetHandle()); p != entt::null && guard++ < 256; p = m_Scene.GetParent(p))
                if (m_Scene.Registry().all_of<RigidBodyComponent>(p)) { body = p; break; }
            if (e.Has<RigidBodyComponent>())
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "This entity has its own Rigid Body: this collider is not used");
            else if (body == entt::null)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "No Rigid Body above it: this collider does nothing");
            else
                ImGui::TextDisabled("Part of %s's body", m_Scene.Registry().get<TagComponent>(body).Tag.c_str());
            const bool dynamicBody = body != entt::null && m_Scene.Registry().get<RigidBodyComponent>(body).Type == BodyType::Dynamic;
            ShapeFields(e, c.Shape, c.Size, c.Center, false, dynamicBody);
            if (ImGui::Button("Remove##collider")) e.Remove<ColliderComponent>();
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
            if (!e.Has<AnimatorComponent>() && ImGui::MenuItem("Animator")) {
                auto& an = e.Add<AnimatorComponent>();
                const auto* mr = e.Has<MeshRendererComponent>() ? &e.Get<MeshRendererComponent>() : nullptr;
                const Model* model = mr ? dynamic_cast<const Model*>(mr->Geometry.get()) : nullptr;
                if (model && !model->AnimationNames().empty()) an.Clip = model->AnimationNames().front();
            }
            if (!e.Has<ColliderComponent>() && ImGui::MenuItem("Collider (part of the parent's body)")) {
                auto& c = e.Add<ColliderComponent>();
                if (e.Has<MeshRendererComponent>()) FitCollider(e.Get<MeshRendererComponent>(), c.Shape, c.Size, c.Center);
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

    // Editor shell (menu bar, toolbar, panels)
    bool  m_ShowHierarchy = true, m_ShowInspector = true, m_ShowSceneSettings = true, m_ShowConsole = true;
    bool  m_ShowStatsOverlay = true, m_ShowFrustums = true, m_ShowControls = false;
    bool  m_OpenSaveAs = false, m_OpenAbout = false, m_ResetLayout = false, m_FocusInspector = false;
    bool  m_PendingNewScene = false, m_ToggleFullscreen = false, m_F11Down = false;
    char  m_HierarchyFilter[64] = {};
    ImFont* m_MonoFont = nullptr;                    // Console and the stats overlay
    std::vector<Engine::LogEntry> m_ConsoleLines;    // a copy, refreshed when new lines arrive
    uint64_t m_ConsoleSeen = ~0ull;
    bool  m_ConsoleInfo = true, m_ConsoleWarn = true, m_ConsoleError = true, m_ConsoleAutoScroll = true;
    char  m_ConsoleFilter[64] = {};
    bool  m_TDown = false;

    EditorHistory m_History;              // undo / redo (Edit mode only)
    bool m_MaybeChanged = false;          // an interaction happened: compare the scene once it is over
    bool m_GizmoBusy = false;             // the gizmo is being dragged
    bool m_ZDown = false, m_YDown = false, m_DDown = false, m_SDown = false;
    std::string m_Title;
    enum class ConfirmAction { None, Quit, Load, New };
    ConfirmAction m_Confirm = ConfirmAction::None;   // the "unsaved changes" dialog is open for this
    std::string m_ConfirmFile;            // the scene to load after the dialog
    bool m_QuitConfirmed = false;

    ImGuizmo::OPERATION m_GizmoOp = ImGuizmo::TRANSLATE;

    ImVec2 m_ViewportSize{ 0.0f, 0.0f };
    ImVec2 m_ViewportOrigin{ 0.0f, 0.0f };            // the viewport image's top left on screen
    bool m_ViewportHovered = false;
    Engine::UITextureCache m_UITextures{ std::string(ASSET_DIR) };   // images for ui.image                  
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
