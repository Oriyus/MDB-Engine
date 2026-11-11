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
#include "Scene/Particles.h"
#include "Core/Profiler.h"
#include "Renderer/SceneRenderer.h"
#include "Runtime/ResourceLibrary.h"
#include "Runtime/GameSession.h"
#include "Runtime/GameUI.h"
#include "Navigation/NavGrid.h"
#include "DemoLevels.h"
#include "EditorHistory.h"
#include "AssetBrowser.h"
#include "DialogueEditor.h"
#include "Platform.h"
#include "Projects.h"
#include "GameBuilder.h"
#include "TerrainEditor.h"
#include "AnimGraphEditor.h"
#include "Animation/AnimGraph.h"

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
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <sstream>

class EditorApp : public Engine::Application {
public:
    // The editor opens as a maximized window; F11 switches to full screen (borderless) and back
    static Engine::WindowProps EditorWindow() {
        Engine::WindowProps p;
        p.Title = "MDB Editor";
        p.Width = 1600;
        p.Height = 900;
        p.Maximized = EnvVar("MDB_WINDOWED") != "1";    // test hook: a plain 1600 x 900 window for screen captures
        return p;
    }
    EditorApp() : Application(EditorWindow()) {}

    void OnStart() override {
        using namespace Engine;

        // Shared engine pieces: named resources, the render pipeline, and the play-mode systems
        m_Resources = std::make_unique<ResourceLibrary>(Projects::Current().AssetDir(), Projects::EngineDir());
        m_Renderer = std::make_unique<SceneRenderer>(Projects::EngineDir(), Projects::Current().AssetDir());
        m_Session = std::make_unique<GameSession>(m_Scene, Projects::Current().AssetDir(), Projects::EngineDir(), m_Resources->Registry());
        {   // save slots while play-testing: MDB_SAVE_DIR, else %APPDATA%/MDB Engine/Editor Saves/<project title>
            std::string dir = EnvVar("MDB_SAVE_DIR");
            if (dir.empty()) {
                const std::string app = EnvVar("APPDATA"), title = GameBuilder::SafeName(Projects::Current().Title);
                dir = app.empty() ? std::string("Saves") : app + "/MDB Engine/Editor Saves/" + title;
            }
            m_Session->SetSaveDirectory(dir);
        }

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
            if (std::filesystem::exists(std::filesystem::u8path(Projects::Current().AssetDir() + "Scripts/bob.lua")))
                e.Add<ScriptComponent>().Path = "Scripts/bob.lua";   // hops up and down while playing (the Examples project)

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

        // --- Orbiting point light (the editor draws a lightbulb for it; nothing is rendered)
        {
            const glm::vec3 lightColor(1.0f, 0.95f, 0.85f);
            auto e = m_Scene.CreateEntity("Light");
            e.Add<PointLightComponent>().Color = lightColor;
            auto& orbit = e.Add<OrbitComponent>();
            orbit.Radius = 3.0f;
            orbit.Height = 1.5f;
            orbit.Speed = 1.0f;
        }

        // --- Second point light: cool blue, slower, orbiting the other way
        {
            const glm::vec3 c(0.4f, 0.6f, 1.0f);
            auto e = m_Scene.CreateEntity("Light 2");
            e.Add<PointLightComponent>().Color = c;
            auto& orbit = e.Add<OrbitComponent>();
            orbit.Radius = 4.5f;
            orbit.Height = 2.5f;
            orbit.Speed = -0.6f;
            orbit.Angle = 3.14f;
        }

        // --- Sun: the light travels along local -Z (the editor draws arrows for it; nothing is rendered)
        {
            auto e = m_Scene.CreateEntity("Sun");
            auto& t = e.Get<TransformComponent>();
            t.Position = glm::vec3(0.0f, 6.0f, 0.0f);
            t.Rotation = glm::vec3(-0.9f, 0.5f, 0.0f);
            e.Add<DirectionalLightComponent>();
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
        m_GameFonts = LoadGameFonts();   // before the first ImGui frame builds the font atlas
        m_Session->Scripts().SetUIFonts(m_GameFonts.Bold, m_GameFonts.Regular);   // ui.label measures with them
        m_Camera->Position = glm::vec3(0.0f, 1.0f, 6.0f);

        // Test hooks, for running the app unattended:
        //   MDB_DEMO=script|coinrush builds that example level, MDB_AUTOPLAY=1 presses Play, MDB_QUIT_AFTER=<s> closes
        //   MDB_TEST_SCRIPT=Scripts/x.lua adds an entity "Test" running that script
        //   MDB_SCENE=<file> loads Assets/Scenes/<file> first, MDB_SAVE_SCENE=<file> saves after the rest
        // Without test hooks the project's start scene opens (project.json "scene"); if it doesn't exist yet
        // (a new project), the built-in scene above takes its name, so Ctrl+S creates it.
        const std::string startScene = Projects::Current().Scene;
        if (EnvVar("MDB_SCENE").empty() && EnvVar("MDB_DEMO").empty() && !startScene.empty()) {
            if (std::filesystem::exists(std::filesystem::u8path(ScenePath(startScene)))) LoadScene(startScene);
            else m_SceneFile = startScene;
        }
        if (auto sc = EnvVar("MDB_SCENE"); !sc.empty()) LoadScene(sc);
        if (auto demo = EnvVar("MDB_DEMO"); demo == "1" || demo == "script") BuildLevel(BuildScriptDemo);
        else if (demo == "coinrush") BuildLevel(BuildCoinRush);
        else if (demo == "tactics") BuildLevel(BuildTacticsTest);
        else if (demo == "cellar") BuildLevel(BuildTacticsCellar);
        else if (demo == "terrain") BuildLevel(BuildTerrainDemo);
        if (auto ts = EnvVar("MDB_TEST_SCRIPT"); !ts.empty())
            m_Scene.CreateEntity("Test").Add<ScriptComponent>().Path = ts;
        if (auto sv = EnvVar("MDB_SAVE_SCENE"); !sv.empty()) SaveScene(sv);
        if (auto dl = EnvVar("MDB_OPEN_DIALOGUE"); !dl.empty()) {     // "Dialogues/x.json" or "Dialogues/x.json#7" (selects node 7)
            const size_t hash = dl.find('#');
            m_Dialogue.Open(dl.substr(0, hash));
            if (hash != std::string::npos) m_Dialogue.Select(std::atoi(dl.c_str() + hash + 1));
            if (EnvVar("MDB_SAVE_DIALOGUE") == "1") m_Dialogue.SaveIfNamed();   // re-save it through the editor
            m_ShowDialogue = true;
        }
        // Every edit in the AnimGraph Editor goes live at once: the animators using that file play the new graph
        // (their states are kept by id), and so does anything that loads it from now on (until saved or discarded)
        m_AnimGraph.OnChanged = [this](const std::string& path, std::shared_ptr<const Engine::AnimGraph> graph) {
            m_Resources->SetAnimGraph(path, graph);
            for (auto [e, an] : m_Scene.Registry().view<Engine::AnimatorComponent>().each())
                if (an.Graph == path) an.GraphData = graph;
        };
        if (auto ag = EnvVar("MDB_OPEN_ANIMGRAPH"); !ag.empty()) {   // test hook: "AnimGraphs/x.json"
            m_AnimGraph.Open(ag);
            m_ShowAnimGraph = true;
        }
        if (EnvVar("MDB_TEST_PICK_FOLDER") == "1")                // test hook: the folder picker must show (it once hung)
            LOG_INFO("Picked: '{}'", Platform::PickFolder("Test: pick a folder", Projects::Current().Root));
        if (auto out = EnvVar("MDB_BUILD_GAME"); !out.empty()) {   // File > Build Game into <out> (the editor waits for it on quit)
            m_Builder.Start(Projects::Current(), Projects::EngineDir(), out);
            m_ShowBuildGame = true;
        }
        // Test hooks for the viewport: MDB_VIEW_MODE=wireframe|xray|solid|textured|rendered, MDB_SELECT=<entity name>
        if (auto vm = EnvVar("MDB_VIEW_MODE"); !vm.empty()) {
            if (vm == "wireframe" || vm == "xray") m_ViewMode = ViewMode::Wireframe;
            else if (vm == "solid") m_ViewMode = ViewMode::Solid;
            else if (vm == "textured") m_ViewMode = ViewMode::Textured;
            m_WireXRay = vm == "xray";
        }
        m_PendingSelect = EnvVar("MDB_SELECT");     // selected once the scene has it (loads are queued)
        // MDB_TERRAIN_TOOL=raise|smooth|flatten|noise|paint[:layer 1-4]: that terrain brush is chosen (with MDB_SELECT=Terrain)
        if (auto tt = EnvVar("MDB_TERRAIN_TOOL"); !tt.empty()) {
            using T = TerrainEditor::Tool;
            const std::string tool = tt.substr(0, tt.find(':'));
            m_TerrainEditor.SetTool(tool == "raise" ? T::Raise : tool == "smooth" ? T::Smooth : tool == "flatten" ? T::Flatten
                                  : tool == "noise" ? T::Noise : tool == "paint" ? T::Paint : T::None);
            if (tool == "paint" && tt.size() > 6) m_TerrainEditor.SetPaintLayer(std::atoi(tt.c_str() + 6) - 1);
        }
        if (EnvVar("MDB_MOTION_VECTORS") == "1") m_Renderer->ShowMotionVectors = true;   // Viewport > Debug > Motion Vectors
        if (EnvVar("MDB_AUTOPLAY") == "1") m_PlayCmd = PlayCmd::Play;
        if (auto q = EnvVar("MDB_QUIT_AFTER"); !q.empty()) m_QuitAfter = std::strtof(q.c_str(), nullptr);
        if (EnvVar("MDB_NO_VSYNC") == "1") GetWindow().SetVSync(false);   // test hook: uncapped frame rate, for timing

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
            PROFILE_SCOPE("Undo history check");
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
            if (m_PlayState == PlayState::Edit) { StartPlay(); m_Session->NewGame(m_SceneFile); m_Session->Start(); }
            break;
        case PlayCmd::Stop:   // the session first: Restore invalidates every entity handle
            if (m_PlayState != PlayState::Edit) {
                m_Session->Stop();
                if (m_Session->SceneReplaced()) ApplyRenderSettings(m_PlaySettings);   // a level change brought its own
                StopPlay();
            }
            break;
        case PlayCmd::Pause:
            if (m_PlayState == PlayState::Play)        m_PlayState = PlayState::Paused;
            else if (m_PlayState == PlayState::Paused) m_PlayState = PlayState::Play;
            break;
        case PlayCmd::Step:
            if (m_PlayState == PlayState::Paused) m_StepFrame = true;
            break;
        case PlayCmd::Restart:   // the current level again, as it was entered (the session does it next frame)
            if (m_PlayState != PlayState::Edit) m_Session->RequestRestart();
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
        const bool dialogueKeys = (m_ShowDialogue && m_Dialogue.IsFocused()) || (m_ShowAnimGraph && m_AnimGraph.IsFocused());   // those editors have their own shortcuts
        if (!typing && kesc && !dialogueKeys) {
            if (m_PlayState != PlayState::Edit) m_PlayCmd = PlayCmd::Stop;
            else RequestQuit();
        }

        // Ctrl shortcuts (Edit mode, not while typing or in the middle of a drag)
        const bool ctrl = ImGui::GetIO().KeyCtrl, shift = ImGui::GetIO().KeyShift;
        const bool kz = EdgePressed(Key::Z, m_ZDown), ky = EdgePressed(Key::Y, m_YDown);
        const bool kd = EdgePressed(Key::D, m_DDown), ks = EdgePressed(Key::S, m_SDown);
        const bool kn = EdgePressed(Key::N, m_NDown);
        const bool ka = EdgePressed(Key::A, m_ADown);
        const bool kgGroup = ctrl && Engine::Input::IsKeyDown(Key::G) && !m_GDown;   // Ctrl+G (G's edge is read below)
        SyncSelection();
        if (EdgePressed(Key::F11, m_F11Down)) m_ToggleFullscreen = true;
        if (ctrl && !typing && !interacting && !dialogueKeys && m_PlayState == PlayState::Edit) {
            if (kz && !shift) UndoStep();
            if (ky || (kz && shift)) RedoStep();
            if (kd) DuplicateSelected();
            if (ks && shift) m_OpenSaveAs = true;
            else if (ks) SaveScene(m_SceneFile);
            if (kn && !shift) RequestNewScene();
            if (kgGroup && !m_ViewingGame) GroupSelected();
            if (ka && !m_ViewingGame) SelectAll();
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
        const bool kShade[4] = { EdgePressed(Key::Num1, m_Num1Down), EdgePressed(Key::Num2, m_Num2Down),
                                 EdgePressed(Key::Num3, m_Num3Down), EdgePressed(Key::Num4, m_Num4Down) };
        if (kp && ImGui::GetIO().KeyCtrl)
            m_PlayCmd = (m_PlayState == PlayState::Edit) ? PlayCmd::Play : PlayCmd::Stop;

        // Looking through a game camera, the game owns the keyboard: only Esc, Ctrl+P, F1 and F5 stay
        const bool editorKeys = !typing && !m_ViewingGame && !dialogueKeys;
        if (!editorKeys) knSpawn = f = g = kw = ke = kr = kq = kdel = kt = false;

        if (!typing) {
            if (f5) {
                m_Renderer->ReloadShaders();
                if (m_PlayState != PlayState::Edit) m_ReloadScripts = true;   // applied next frame
            }
            if (knSpawn) SpawnCube();
            if (f && !ImGui::GetIO().KeyAlt) FrameSelected();
            if (kdel) DeleteSelected();
            if (g && !ctrl) DeleteLastSpawned();
            if (f1) m_ShowUI = !m_ShowUI;
            if (!ImGui::GetIO().KeyAlt) {
                if (kq || kw || ke || kr) m_TerrainEditor.SetTool(TerrainEditor::Tool::None);   // back to selecting / the gizmo
                if (kq) m_SelectTool = true;
                if (kw) { m_SelectTool = false; m_GizmoOp = ImGuizmo::TRANSLATE; }
                if (ke) { m_SelectTool = false; m_GizmoOp = ImGuizmo::ROTATE; }
                if (kr) { m_SelectTool = false; m_GizmoOp = ImGuizmo::SCALE; }
            }
            // 1-4: Wireframe / Solid / Textured / Rendered (pressing 1 again in wireframe toggles X-ray)
            if (editorKeys && !ctrl && !ImGui::GetIO().KeyAlt && !shift) {
                for (int i = 0; i < 4; i++) {
                    if (!kShade[i]) continue;
                    if (i == 0 && m_ViewMode == ViewMode::Wireframe) m_WireXRay = !m_WireXRay;
                    m_ViewMode = (ViewMode)i;
                }
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
            // A save loaded, a level change or a restart (asked for last frame): the session replaces
            // the scene, and the new level brings its render settings (put back on Stop)
            {
                Engine::SceneSettings s = GatherSettings();
                if (m_Session->ApplyPending(s)) {
                    m_Renderer->ApplySettings(s);
                    m_Renderer->ResetTemporalHistory();   // a new level: a camera cut
                    ClearSelection();             // the old handles are gone
                    m_Spawned.clear();
                }
            }
            // ui.button / ui.mouse: the pointer over the viewport (last frame's layout), only while
            // looking through the game camera (otherwise clicks belong to the editor)
            {
                const ImVec2 mp = ImGui::GetIO().MousePos;
                const float px = (mp.x - m_ViewportOrigin.x) / std::max(m_ViewportSize.x, 1.0f);
                const float py = (mp.y - m_ViewportOrigin.y) / std::max(m_ViewportSize.y, 1.0f);
                const bool over = m_ViewportHovered && m_ViewingGame;
                m_Session->Scripts().SetPointer(px, py, over, ImGui::IsMouseDown(ImGuiMouseButton_Left),
                    over ? ImGui::GetIO().MouseWheel : 0.0f, ImGui::IsMouseDown(ImGuiMouseButton_Right));
                m_Session->Scripts().SetViewAspect(aspect);   // for scene.screen_ray
            }
            { PROFILE_SCOPE("Session update"); m_Session->Update(dt, m_StepFrame); }   // scene.restart() is handled by the session
            if (m_Session->Scripts().ConsumeQuitRequest()) m_PlayCmd = PlayCmd::Stop;   // scene.quit(): the editor stops Play
        }
        else {
            PROFILE_SCOPE("Edit-mode scene update");
            m_Scene.UpdateOrbitPositions();
            m_Scene.UpdateAnimations(0.0f);   // pose animated models at their current time (scrubbing works)
            if (m_PlayState == PlayState::Edit) m_Scene.UpdateParticles(dt);   // a live preview while editing (not while paused)
        }
        m_StepFrame = false;

        // During Play, look through the scene's game camera (after scripts moved it this frame)
        const bool wasViewingGame = m_ViewingGame;
        m_ViewingGame = m_PlayState != PlayState::Edit && m_PlayThroughGameCamera
            && m_Session->UpdateGameCamera(*m_GameCamera, aspect);
        Camera& view = ViewCamera();
        if (m_ViewingGame != wasViewingGame) m_Renderer->ResetTemporalHistory();   // switched cameras: a cut
        m_Session->SetListener(view);

        // Shading and the selection outline are editor views: the game camera shows the game as it is
        m_Renderer->Mode = m_ViewingGame ? ViewMode::Rendered : m_ViewMode;
        m_Renderer->XRay = m_WireXRay;
        m_Renderer->Highlight.clear();
        if (!m_PendingSelect.empty()) {         // MDB_SELECT: names separated by commas (the first entity of each)
            std::vector<entt::entity> found;
            std::stringstream names(m_PendingSelect);
            for (std::string name; std::getline(names, name, ',');)
                for (auto [e, tag] : m_Scene.Registry().view<Engine::TagComponent>().each())
                    if (tag.Tag == name) { found.push_back(e); break; }
            if (!found.empty()) {
                m_PendingSelect.clear();
                SelectOnly(found.front());
                for (auto e : found) AddToSelection(e);
            }
        }
        SyncSelection();
        if (m_ShowOutline && !m_ViewingGame && !m_Selection.empty()) {
            // every selected entity and everything under it
            const std::unordered_set<entt::entity> selected(m_Selection.begin(), m_Selection.end());
            for (auto e : m_Scene.Registry().view<Engine::MeshRendererComponent>()) {
                int guard = 0;
                for (entt::entity p = e; p != entt::null && guard++ < 256; p = m_Scene.GetParent(p))
                    if (selected.count(p)) { m_Renderer->Highlight.push_back((uint32_t)entt::to_integral(e)); break; }
            }
        }
        { PROFILE_SCOPE("Render"); m_Renderer->Render(m_Scene, view, vw, vh); }

        glViewport(0, 0, (int)GetWindow().GetWidth(), (int)GetWindow().GetHeight());
    }

    // ------------------------------------------------------------------ UI
    void OnImGui() override {
        // Menu bar and toolbar take their strips off the top; the dock area fills the rest
        DrawMainMenuBar();
        DrawToolbar();
        ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        BuildDefaultLayoutOnce(dockspace);

        { PROFILE_SCOPE("Viewport panel"); DrawViewportPanel(); }
        if (m_ShowUI) {
            if (m_ShowHierarchy) { PROFILE_SCOPE("Hierarchy"); DrawHierarchyPanel(); }
            if (m_ShowInspector) { PROFILE_SCOPE("Inspector"); DrawInspectorPanel(); }
            if (m_ShowSceneSettings) { PROFILE_SCOPE("Scene settings"); DrawSceneSettingsPanel(); }
            if (m_ShowConsole) { PROFILE_SCOPE("Console"); DrawConsolePanel(); }
            if (m_ShowAssets) {
                // A new panel: dock it with the Console the first time (saved layouts don't know it yet)
                if (ImGuiWindow* console = ImGui::FindWindowByName("Console"); console && console->DockId)
                    ImGui::SetNextWindowDockID(console->DockId, ImGuiCond_FirstUseEver);
                PROFILE_SCOPE("Assets");
                HandleAssetActions(m_Assets.Draw(&m_ShowAssets, [this](const std::string& p) -> uint32_t {
                    auto t = m_Resources->LoadTexture(p);
                    return t ? t->GetID() : 0;
                }));
            }
        }
        if (m_ShowDialogue) {
            // the first time: a tab beside the Viewport (saved layouts don't know the window yet)
            if (ImGuiWindow* vp = ImGui::FindWindowByName("Viewport"); vp && vp->DockId)
                ImGui::SetNextWindowDockID(vp->DockId, ImGuiCond_FirstUseEver);
            m_Dialogue.Draw(&m_ShowDialogue);
        }
        if (m_ShowAnimGraph) {
            if (ImGuiWindow* vp = ImGui::FindWindowByName("Viewport"); vp && vp->DockId)
                ImGui::SetNextWindowDockID(vp->DockId, ImGuiCond_FirstUseEver);
            LinkAnimGraphEditor();
            m_AnimGraph.Draw(&m_ShowAnimGraph);
        }
        DrawGraphQuitModal();
        DrawDialogueQuitModal();
        DrawUnsavedChangesModal();
        DrawBuildGameWindow();
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
        m_Selection.clear();        // the old handles are gone; the history gives back the active one
        m_Selected = m_History.Undo(m_Scene);
        m_Spawned.clear();          // those handles died with the restore
        m_MaybeChanged = false;
    }

    void RedoStep() {
        if (!m_History.CanRedo()) return;
        LOG_INFO("Redo: {}", m_History.RedoLabel());
        m_Selection.clear();
        m_Selected = m_History.Redo(m_Scene);
        m_Spawned.clear();
        m_MaybeChanged = false;
    }

    // Copies every selected entity (with its children) and selects the copies; the active one's copy stays active
    void DuplicateSelected() {
        const auto roots = SelectionRoots();
        if (roots.empty()) return;
        const entt::entity active = m_Selected;
        std::vector<entt::entity> copies;
        entt::entity activeCopy = entt::null;
        std::string name;
        for (entt::entity e : roots) {
            Engine::Entity copy = m_Scene.DuplicateEntity(Engine::Entity(e, &m_Scene));
            copies.push_back(copy.GetHandle());
            if (e == active) activeCopy = copy.GetHandle();
            name = copy.Get<Engine::TagComponent>().Tag;
        }
        m_Selection = copies;
        if (activeCopy != entt::null) { std::erase(m_Selection, activeCopy); m_Selection.push_back(activeCopy); }
        m_Selected = m_Selection.back();
        m_History.Track(m_Scene, m_Selected, m_Resources->Registry(),
            copies.size() == 1 ? "Duplicate " + name : "Duplicate " + std::to_string(copies.size()) + " objects");
    }

    // "MDB Editor - My Game - level.json *": the project, the scene, and a star for unsaved changes
    void UpdateTitle() {
        std::string title = "MDB Editor - " + Projects::Current().Title + " - " + m_SceneFile + (m_History.IsDirty() ? " *" : "");
        if (title != m_Title) { m_Title = title; SetTitle(title); }
    }

    // Quit (Esc in Edit mode or the window's close button): ask first if there are unsaved changes
    void RequestQuit() {
        if (m_AnimGraph.IsDirty() && !m_GraphDiscarded) { m_AskGraphQuit = true; m_ShowAnimGraph = true; return; }
        if (m_Dialogue.IsDirty() && !m_DialogueDiscarded) { m_AskDialogueQuit = true; m_ShowDialogue = true; return; }
        if (m_History.IsDirty()) m_Confirm = ConfirmAction::Quit;
        else Close();
    }

    // ------------------------------------------------------------------ projects
    // Another project: the editor restarts with --project <folder> (after the usual unsaved-changes questions)
    void OpenProject(const std::string& folder) {
        if (!Engine::Project::IsProject(folder)) {
            LOG_ERROR("{} is not a project (it has no project.json). File > New Project makes one.", folder);
            Projects::RemoveRecent(folder);
            return;
        }
        if (Engine::Project::Normalize(folder) == Projects::Current().Root) return;
        m_SwitchProject = Engine::Project::Normalize(folder);
        RequestQuit();
    }

    // Pick a folder (an empty one, or one to put the project in), make the project there and open it
    void NewProject() {
        const std::string folder = Platform::PickFolder("New project: pick or create an empty folder for it");
        if (folder.empty()) return;
        if (Engine::Project::IsProject(folder)) { LOG_INFO("{} already holds a project; opening it", folder); OpenProject(folder); return; }
        Engine::Project p;
        std::string error;
        if (!Engine::Project::Create(folder, "", p, &error)) { LOG_ERROR("New project: {}", error); return; }
        LOG_INFO("Created the project {} in {}", p.Title, p.Root);
        Projects::AddRecent(p.Root);
        OpenProject(p.Root);
    }

    void OnShutdown() override {
        if (!m_SwitchProject.empty() && !Platform::LaunchSelf({ "--project", m_SwitchProject }))
            LOG_ERROR("Could not restart the editor for {}", m_SwitchProject);
    }

    // File > Build Game: the project's game settings (saved to project.json) and the build with its log
    void DrawBuildGameWindow() {
        if (!m_ShowBuildGame) return;
        Engine::Project& project = Projects::CurrentMutable();
        if (m_BuildOutput.empty()) m_BuildOutput = project.Root + "Build";
        if (m_BuildExclude.empty() && !m_BuildExcludeLoaded) {
            for (const auto& e : project.Exclude) m_BuildExclude += e + "\n";
            m_BuildExcludeLoaded = true;
        }
        ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Build Game", &m_ShowBuildGame, ImGuiWindowFlags_NoDocking)) { ImGui::End(); return; }

        const bool running = m_Builder.Running();
        ImGui::BeginDisabled(running);
        ImGui::TextDisabled("Project: %s", project.Root.c_str());
        bool changed = false;
        char title[128];
        std::snprintf(title, sizeof(title), "%s", project.Title.c_str());
        if (ImGui::InputText("Title", title, sizeof(title))) { project.Title = title; changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The window title, the exe's name and the save folder (%%APPDATA%%/<title>/Saves)");
        if (ImGui::BeginCombo("Start scene", project.Scene.c_str())) {
            for (const auto& f : ListSceneFiles())
                if (ImGui::Selectable(f.c_str(), f == project.Scene)) { project.Scene = f; changed = true; }
            ImGui::EndCombo();
        }
        int size[2] = { (int)project.Width, (int)project.Height };
        if (ImGui::InputInt2("Window size", size)) { project.Width = (uint32_t)std::max(320, size[0]); project.Height = (uint32_t)std::max(240, size[1]); changed = true; }
        changed |= ImGui::Checkbox("Full screen", &project.Fullscreen);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("VSync", &project.VSync);
        ImGui::Text("Leave out (files under Assets, one per line, * matches anything):");
        char excludeBuf[2048];
        std::snprintf(excludeBuf, sizeof(excludeBuf), "%s", m_BuildExclude.c_str());
        if (ImGui::InputTextMultiline("##exclude", excludeBuf, sizeof(excludeBuf), ImVec2(-1, 70))) {
            m_BuildExclude = excludeBuf;
            project.Exclude.clear();
            size_t start = 0;
            while (start <= m_BuildExclude.size()) {
                size_t end = m_BuildExclude.find('\n', start);
                if (end == std::string::npos) end = m_BuildExclude.size();
                std::string line = m_BuildExclude.substr(start, end - start);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (!line.empty()) project.Exclude.push_back(line);
                start = end + 1;
            }
            changed = true;
        }
        if (changed) { std::string error; if (!project.Save(&error)) LOG_ERROR("{}", error); }

        char outBuf[512];
        std::snprintf(outBuf, sizeof(outBuf), "%s", m_BuildOutput.c_str());
        if (ImGui::InputText("Output folder", outBuf, sizeof(outBuf))) m_BuildOutput = outBuf;
        ImGui::SameLine();
        if (ImGui::Button("...")) { const std::string f = Platform::PickFolder("Where to put the built game", m_BuildOutput); if (!f.empty()) m_BuildOutput = f; }
        const std::string target = Engine::Project::Normalize(m_BuildOutput) + GameBuilder::SafeName(project.Title) + "/";
        ImGui::TextDisabled("The game goes to %s", target.c_str());
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::BeginDisabled(running);
        if (ImGui::Button(running ? "Building..." : "Build", ImVec2(140, 0))) {
            if (m_History.IsDirty()) LOG_WARN("The scene has unsaved changes: the game uses the saved file. Save first (Ctrl+S) to include them.");
            m_Builder.Start(project, Projects::EngineDir(), m_BuildOutput);
        }
        ImGui::EndDisabled();
        if (m_Builder.Done() && m_Builder.Succeeded()) {
            ImGui::SameLine();
            if (ImGui::Button("Play it")) Platform::OpenWithSystem(m_Builder.ExePath());
            ImGui::SameLine();
            if (ImGui::Button("Show in Explorer")) Platform::ShowInExplorer(m_Builder.GameFolder());
        }
        if (running) { ImGui::SameLine(); ImGui::TextDisabled("compiling and copying, the editor keeps working"); }

        ImGui::Separator();
        if (m_MonoFont) ImGui::PushFont(m_MonoFont);
        ImGui::BeginChild("##buildlog", ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar);
        const auto lines = m_Builder.Lines();
        for (const auto& line : lines) {
            const bool bad = line.rfind("FAILED", 0) == 0 || line.find("error") != std::string::npos;
            const bool good = line.rfind("Done", 0) == 0;
            if (bad) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
            else if (good) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.9f, 0.5f, 1.0f));
            ImGui::TextUnformatted(line.c_str());
            if (bad || good) ImGui::PopStyleColor();
        }
        if (lines.size() != m_BuildLogLines) { m_BuildLogLines = lines.size(); ImGui::SetScrollHereY(1.0f); }
        ImGui::EndChild();
        if (m_MonoFont) ImGui::PopFont();
        ImGui::End();
    }

    bool OnCloseRequested() override {
        if (m_AnimGraph.IsDirty() && !m_GraphDiscarded && !m_QuitConfirmed) { m_AskGraphQuit = true; m_ShowAnimGraph = true; return false; }
        if (m_Dialogue.IsDirty() && !m_DialogueDiscarded && !m_QuitConfirmed) { m_AskDialogueQuit = true; m_ShowDialogue = true; return false; }
        if (m_QuitConfirmed || !m_History.IsDirty()) return true;
        m_Confirm = ConfirmAction::Quit;
        return false;
    }

    // Quitting with unsaved changes in the AnimGraph Editor: asked first, like the Dialogue Editor's
    void DrawGraphQuitModal() {
        if (m_AskGraphQuit && !ImGui::IsPopupOpen("Unsaved animation graph changes")) ImGui::OpenPopup("Unsaved animation graph changes");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Unsaved animation graph changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        const std::string path = m_AnimGraph.Path();
        ImGui::Text("The animation graph %s has unsaved changes.", path.empty() ? "(not saved yet)" : path.c_str());
        if (path.empty()) ImGui::TextDisabled("Use File > Save As in the AnimGraph Editor to keep it.");
        ImGui::Spacing();
        if (!path.empty() && ImGui::Button("Save", ImVec2(110, 0)) && m_AnimGraph.SaveIfNamed()) {
            m_AskGraphQuit = false;
            ImGui::CloseCurrentPopup();
            RequestQuit();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(110, 0))) {
            m_AskGraphQuit = false;
            m_GraphDiscarded = true;
            ImGui::CloseCurrentPopup();
            RequestQuit();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) { m_AskGraphQuit = false; m_SwitchProject.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // The AnimGraph Editor's live link: the Animator it shows (the selected entity's if it uses the open graph, else
    // the first one that does) and the clips and bones of a model for its pickers (that entity's, else the selection's)
    void LinkAnimGraphEditor() {
        using namespace Engine;
        auto& reg = m_Scene.Registry();
        auto skinned = [&](entt::entity e) -> const Model* {
            const auto* mr = reg.try_get<MeshRendererComponent>(e);
            const Model* m = mr ? dynamic_cast<const Model*>(mr->Geometry.get()) : nullptr;
            return m && m->IsSkinned() ? m : nullptr;
        };
        const std::string& path = m_AnimGraph.Path();
        entt::entity liveEntity = entt::null;
        if (!path.empty()) {
            const bool selectedUses = m_Selected != entt::null && reg.valid(m_Selected) &&
                reg.all_of<AnimatorComponent>(m_Selected) && reg.get<AnimatorComponent>(m_Selected).Graph == path;
            if (selectedUses) liveEntity = m_Selected;
            else for (auto [e, an] : reg.view<AnimatorComponent>().each()) if (an.Graph == path && an.GraphData) { liveEntity = e; break; }
        }
        if (liveEntity != entt::null && skinned(liveEntity))
            m_AnimGraph.SetLive(&reg.get<AnimatorComponent>(liveEntity), reg.get<TagComponent>(liveEntity).Tag);
        else m_AnimGraph.SetLive(nullptr, "");
        const Model* model = liveEntity != entt::null ? skinned(liveEntity) : nullptr;
        if (!model && m_Selected != entt::null && reg.valid(m_Selected)) model = skinned(m_Selected);
        if (model) m_AnimGraph.SetModel(model->AnimationNames(), model->NodeNames());
        else m_AnimGraph.SetModel({}, {});
    }

    static std::vector<std::string> ListAnimGraphFiles() {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& f : std::filesystem::directory_iterator(std::filesystem::u8path(Projects::Current().AssetDir()) / "AnimGraphs", ec))
            if (f.is_regular_file() && f.path().extension() == ".json") out.push_back("AnimGraphs/" + f.path().filename().string());
        std::sort(out.begin(), out.end());
        return out;
    }

    // Quitting with unsaved changes in the Dialogue Editor: asked before the scene's own question
    void DrawDialogueQuitModal() {
        if (m_AskDialogueQuit && !ImGui::IsPopupOpen("Unsaved dialogue changes")) ImGui::OpenPopup("Unsaved dialogue changes");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Unsaved dialogue changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        const std::string path = m_Dialogue.Path();
        ImGui::Text("The dialogue %s has unsaved changes.", path.empty() ? "(not saved yet)" : path.c_str());
        if (path.empty()) ImGui::TextDisabled("Use File > Save As in the Dialogue Editor to keep it.");
        ImGui::Spacing();
        if (!path.empty() && ImGui::Button("Save", ImVec2(110, 0)) && m_Dialogue.SaveIfNamed()) {
            m_AskDialogueQuit = false;
            ImGui::CloseCurrentPopup();
            RequestQuit();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(110, 0))) {
            m_AskDialogueQuit = false;
            m_DialogueDiscarded = true;
            ImGui::CloseCurrentPopup();
            RequestQuit();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) { m_AskDialogueQuit = false; m_SwitchProject.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
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
        ImGui::TextDisabled(m_Confirm == ConfirmAction::Quit ? (m_SwitchProject.empty() ? "Save them before quitting?" : "Save them before opening the other project?")
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
            m_SwitchProject.clear();
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
        m_PlaySettings = GatherSettings();   // put back on Stop if a level change replaced them
        m_PlayState = PlayState::Play;
    }

    void StopPlay() {
        if (m_PlayState == PlayState::Edit) return;
        m_Selection.clear();
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
        const char* items = allowNone ? "Box\0Sphere\0Convex hull\0Mesh\0Capsule\0None (child colliders only)\0Terrain\0"
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
        else if (shape == ColliderShape::Terrain) {
            if (!e.Has<TerrainComponent>()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Needs a Terrain component (a box is used)");
            else ImGui::TextDisabled("The terrain's heights as a height field, scaled with the entity");
            if (dynamic) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "A terrain can't be dynamic: use Static");
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
        for (const auto& f : std::filesystem::directory_iterator(std::filesystem::u8path(Projects::Current().AssetDir()) / "Sounds", ec)) {
            const auto ext = f.path().extension().string();
            if (ext == ".wav" || ext == ".mp3" || ext == ".flac") files.push_back("Sounds/" + f.path().filename().string());
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    // ------------------------------------------------------------ scene files
    static std::string ScenePath(const std::string& file) { return Projects::Current().AssetDir() + "Scenes/" + file; }

    std::vector<std::string> ListSceneFiles() const {
        std::vector<std::string> files;
        std::error_code ec;
        for (const auto& f : std::filesystem::directory_iterator(std::filesystem::u8path(Projects::Current().AssetDir()) / "Scenes", ec))
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

    // The renderer's part and gravity, not the editor camera (used when Play ends after a level change)
    void ApplyRenderSettings(const Engine::SceneSettings& s) {
        m_Renderer->ApplySettings(s);
        m_Gravity = s.Gravity;
        m_Session->Physics().SetGravity(m_Gravity);
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
        // Terrain heights and paint go to their own files (Assets/Terrains), which the scene file names
        // Save As (another file name): the terrains get files of their own, so the two scenes don't share them
        if (!TerrainEditor::SaveTerrains(m_Scene, *m_Resources, Projects::Current().AssetDir(), file, file != m_SceneFile))
            LOG_WARN("A terrain could not be saved (see above)");
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
    LevelAssets MakeLevelAssets() {
        EnsureSphere();
        LevelAssets a{ m_Cube, m_Plane, m_Sphere, m_Checker, m_Resources->LoadModel("Models/Fox.glb"),
            m_Resources->LoadTexture("Textures/fence.png"), m_Resources->LoadTexture("Textures/window.png"),
            m_Resources->Capsule(), m_Resources->Cone() };
        const char* layers[4] = { "grass", "earth", "rock", "sand" };
        for (int i = 0; i < 4; i++) a.TerrainLayers[i] = m_Resources->LoadTexture(std::string("Textures/Terrain/") + layers[i] + ".jpg");
        return a;
    }
    void BuildLevel(void (*build)(Engine::Scene&, const LevelAssets&, Engine::PrefabLibrary&)) {
        m_Selected = entt::null;
        m_Spawned.clear();
        build(m_Scene, MakeLevelAssets(), m_Session->Prefabs());
        if (build == BuildTerrainDemo) {                   // above the hills, looking over the meadow
            m_Camera->Position = glm::vec3(0.0f, 24.0f, 46.0f);
            m_Camera->FocusDistance = 50.0f;
            m_Camera->SetOrientation(-90.0f, -24.0f);
        }
        m_History.Track(m_Scene, m_Selected, m_Resources->Registry(),
            build == BuildCoinRush ? "Build Coin Rush" : build == BuildTacticsTest ? "Build tactics test" : build == BuildTacticsCellar ? "Build tactics cellar" : build == BuildTerrainDemo ? "Build terrain demo" : "Build script demo");   // undoable like any edit
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
            const ImU32 col = IsSelected(e) ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 210, 150);
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

    // Suns (Directional Lights) are only light: the editor shows each as a ring with a few parallel arrows pointing
    // the way the light travels (local -Z), the same size on screen at any distance. Drawn with ImGui over the
    // Viewport, so never rendered (nor in the game). Clicking the gizmo selects the sun (SunGizmoAt).
    struct SunGizmo { entt::entity E; glm::vec3 Pos, Dir, U, V; float Size; };
    std::vector<SunGizmo> GatherSunGizmos() {
        using namespace Engine;
        std::vector<SunGizmo> out;
        const glm::vec3 eye = ViewCamera().Position;
        for (auto [e, light] : m_Scene.Registry().view<DirectionalLightComponent>().each()) {
            glm::vec3 p, sc;
            glm::quat q;
            Scene::Decompose(m_Scene.GetWorldMatrix(e), p, q, sc);
            const glm::vec3 dir = glm::normalize(q * glm::vec3(0, 0, -1));
            const glm::vec3 u = glm::normalize(q * glm::vec3(1, 0, 0)), v = glm::normalize(q * glm::vec3(0, 1, 0));
            out.push_back({ e, p, dir, u, v, std::max(glm::length(p - eye) * 0.085f, 0.05f) });
        }
        return out;
    }
    bool ProjectToViewport(const glm::vec3& w, const ImVec2& origin, const ImVec2& size, ImVec2& out) {
        const glm::vec4 c = ViewCamera().GetViewProjection() * glm::vec4(w, 1.0f);
        if (c.w <= 0.01f) return false;
        out = ImVec2(origin.x + (c.x / c.w * 0.5f + 0.5f) * size.x, origin.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * size.y);
        return true;
    }
    void DrawSunGizmos(const ImVec2& origin, const ImVec2& size) {
        using namespace Engine;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (const SunGizmo& g : GatherSunGizmos()) {
            const bool selected = IsSelected(g.E);
            const glm::vec3 c = glm::clamp(m_Scene.Registry().get<DirectionalLightComponent>(g.E).Color, 0.0f, 1.0f);
            const ImU32 col = selected ? IM_COL32(255, 255, 255, 255)
                                       : IM_COL32((int)(150 + 105 * c.r), (int)(150 + 105 * c.g), (int)(110 + 100 * c.b), 210);
            const float thick = selected ? 2.2f : 1.5f;
            auto line = [&](const glm::vec3& a, const glm::vec3& b) {
                ImVec2 pa, pb;
                if (ProjectToViewport(a, origin, size, pa) && ProjectToViewport(b, origin, size, pb)) dl->AddLine(pa, pb, col, thick);
            };
            const float s = g.Size, ring = 0.45f * s, len = 1.5f * s, head = 0.28f * s;
            // the ring the light comes through
            const int n = 24;
            for (int i = 0; i < n; i++) {
                const float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
                line(g.Pos + (g.U * std::cos(a0) + g.V * std::sin(a0)) * ring, g.Pos + (g.U * std::cos(a1) + g.V * std::sin(a1)) * ring);
            }
            // parallel arrows: one from the centre, four from the ring
            const glm::vec3 starts[5] = { g.Pos, g.Pos + g.U * ring, g.Pos - g.U * ring, g.Pos + g.V * ring, g.Pos - g.V * ring };
            for (int i = 0; i < 5; i++) {
                const glm::vec3 a = starts[i], b = a + g.Dir * (i == 0 ? len * 1.25f : len);
                line(a, b);
                const glm::vec3 back = b - g.Dir * head;
                line(b, back + g.U * head * 0.45f);
                line(b, back - g.U * head * 0.45f);
                line(b, back + g.V * head * 0.45f);
                line(b, back - g.V * head * 0.45f);
            }
            ImVec2 p;
            if (ProjectToViewport(g.Pos, origin, size, p)) dl->AddCircleFilled(p, selected ? 4.5f : 3.5f, col);
        }
    }
    // The light whose gizmo is under a point of the Viewport (a sun's centre, ring or centre arrow; a bulb), or entt::null
    entt::entity LightGizmoAt(const ImVec2& mouse, const ImVec2& origin, const ImVec2& size) {
        entt::entity best = entt::null;
        float bestDist = 10.0f;                                   // pixels
        auto segDist = [](ImVec2 p, ImVec2 a, ImVec2 b) {
            const ImVec2 ab(b.x - a.x, b.y - a.y), ap(p.x - a.x, p.y - a.y);
            const float l2 = ab.x * ab.x + ab.y * ab.y;
            const float t = l2 > 0.0f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
            const float dx = p.x - (a.x + ab.x * t), dy = p.y - (a.y + ab.y * t);
            return std::sqrt(dx * dx + dy * dy);
        };
        if (m_ShowLightGizmos) for (const SunGizmo& g : GatherSunGizmos()) {
            ImVec2 c, tip, r;
            if (!ProjectToViewport(g.Pos, origin, size, c)) continue;
            float d = std::sqrt((mouse.x - c.x) * (mouse.x - c.x) + (mouse.y - c.y) * (mouse.y - c.y));
            if (ProjectToViewport(g.Pos + g.U * 0.45f * g.Size, origin, size, r)) {   // anywhere inside the ring
                const float ringPx = std::sqrt((r.x - c.x) * (r.x - c.x) + (r.y - c.y) * (r.y - c.y));
                if (d < ringPx) d = std::min(d, 4.0f);
            }
            if (ProjectToViewport(g.Pos + g.Dir * 1.9f * g.Size, origin, size, tip)) d = std::min(d, segDist(mouse, c, tip));
            if (d < bestDist) { bestDist = d; best = g.E; }
        }
        // grass blockers: the centre diamond or the outline (not the inside: clicks there pick what is on the ground)
        if (m_ShowGrassBlockers)
            for (auto [e, b] : m_Scene.Registry().view<Engine::GrassBlockerComponent>().each()) {
                ImVec2 c;
                float d = 1e9f;
                if (ProjectToViewport(glm::vec3(m_Scene.GetWorldMatrix(e)[3]), origin, size, c))
                    d = std::sqrt((mouse.x - c.x) * (mouse.x - c.x) + (mouse.y - c.y) * (mouse.y - c.y));
                const auto pts = BlockerWorldOutline(e, b, 0.0f);
                for (size_t i = 0; i < pts.size(); i++) {
                    ImVec2 a, bb;
                    if (ProjectToViewport(pts[i], origin, size, a) && ProjectToViewport(pts[(i + 1) % pts.size()], origin, size, bb))
                        d = std::min(d, segDist(mouse, a, bb) + 2.0f);   // the diamond wins a close call
                }
                if (d < 7.0f && d < bestDist) { bestDist = d; best = e; }
            }
        // point lights: the lightbulb icon
        if (m_ShowLightGizmos) for (auto [e, light] : m_Scene.Registry().view<Engine::PointLightComponent>().each()) {
            ImVec2 c;
            if (!ProjectToViewport(glm::vec3(m_Scene.GetWorldMatrix(e)[3]), origin, size, c)) continue;
            const float r = kBulbRadius;
            const ImVec2 glass(c.x, c.y - r * 0.35f);
            float d = std::sqrt((mouse.x - glass.x) * (mouse.x - glass.x) + (mouse.y - glass.y) * (mouse.y - glass.y)) - r;
            if (mouse.x > c.x - r * 0.6f && mouse.x < c.x + r * 0.6f && mouse.y > glass.y && mouse.y < c.y + r * 1.25f) d = 0.0f;   // the base
            d = std::max(d, 0.0f);
            if (d < 4.0f && d < bestDist) { bestDist = d; best = e; }
        }
        return best;
    }

    // Grass Blockers: their shape as an outline lying on the ground (on a terrain, following it), a fainter inner one
    // where a soft edge reaches full strength, and a small diamond at the centre. Editor only, never rendered.
    static std::vector<glm::vec2> BlockerOutline(Engine::GrassBlockerShape shape, glm::vec2 size) {
        using Engine::GrassBlockerShape;
        std::vector<glm::vec2> out;
        if (shape == GrassBlockerShape::Circle) {
            for (int i = 0; i < 48; i++) {
                const float a = 6.2831853f * i / 48;
                out.push_back(glm::vec2(std::cos(a), std::sin(a)) * size.x * 0.5f);
            }
        }
        else if (shape == GrassBlockerShape::Capsule) {
            const float r = size.y * 0.5f, half = std::max(size.x * 0.5f - r, 0.0f);
            for (int i = 0; i <= 24; i++) {                       // right end, then left end
                const float a = -1.5707963f + 3.1415927f * i / 24;
                out.push_back(glm::vec2(half + std::cos(a) * r, std::sin(a) * r));
            }
            for (int i = 0; i <= 24; i++) {
                const float a = 1.5707963f + 3.1415927f * i / 24;
                out.push_back(glm::vec2(-half + std::cos(a) * r, std::sin(a) * r));
            }
        }
        else {
            const glm::vec2 h = size * 0.5f;
            const glm::vec2 c[4] = { { -h.x, -h.y }, { h.x, -h.y }, { h.x, h.y }, { -h.x, h.y } };
            for (int s = 0; s < 4; s++)                           // each side in steps, to follow a terrain
                for (int i = 0; i < 12; i++) out.push_back(glm::mix(c[s], c[(s + 1) % 4], i / 12.0f));
        }
        return out;
    }
    // A blocker's outline in world space, on the ground under it where there is a terrain
    std::vector<glm::vec3> BlockerWorldOutline(entt::entity e, const Engine::GrassBlockerComponent& b, float inset) {
        std::vector<glm::vec3> out;
        glm::vec2 size = b.Size - glm::vec2(2.0f * inset);
        if (b.Shape == Engine::GrassBlockerShape::Circle ? size.x <= 0.0f : (size.x <= 0.0f || size.y <= 0.0f)) return out;
        const glm::mat4 M = m_Scene.GetWorldMatrix(e);
        for (const glm::vec2& p : BlockerOutline(b.Shape, size)) {
            glm::vec3 w = glm::vec3(M * glm::vec4(p.x, 0.0f, p.y, 1.0f));
            float y;
            if (Engine::TerrainSurfaceAt(m_Scene, w.x, w.z, y)) w.y = y + 0.03f;
            out.push_back(w);
        }
        return out;
    }
    void DrawGrassBlockers(const ImVec2& origin, const ImVec2& size) {
        using namespace Engine;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (auto [e, b] : m_Scene.Registry().view<GrassBlockerComponent>().each()) {
            const bool selected = IsSelected(e);
            const ImU32 col = selected ? IM_COL32(170, 255, 190, 255) : IM_COL32(90, 220, 120, 200);
            auto loop = [&](const std::vector<glm::vec3>& pts, ImU32 c, float thick) {
                for (size_t i = 0; i < pts.size(); i++) {
                    ImVec2 a, bb;
                    if (ProjectToViewport(pts[i], origin, size, a) && ProjectToViewport(pts[(i + 1) % pts.size()], origin, size, bb))
                        dl->AddLine(a, bb, c, thick);
                }
            };
            loop(BlockerWorldOutline(e, b, 0.0f), col, selected ? 2.2f : 1.5f);
            if (b.Softness > 0.0f) loop(BlockerWorldOutline(e, b, b.Softness), (col & 0x00FFFFFFu) | (90u << 24), 1.0f);
            ImVec2 c;
            if (ProjectToViewport(glm::vec3(m_Scene.GetWorldMatrix(e)[3]), origin, size, c)) {
                const float r = selected ? 6.0f : 5.0f;
                dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), col);
            }
        }
    }

    // Point lights are only light too: the editor draws a lightbulb icon in the light's colour at each one (the same
    // size at any distance), never rendered.
    static constexpr float kBulbRadius = 13.0f;     // pixels
    void DrawPointLightGizmos(const ImVec2& origin, const ImVec2& size) {
        using namespace Engine;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (auto [e, light] : m_Scene.Registry().view<PointLightComponent>().each()) {
            ImVec2 c;
            if (!ProjectToViewport(glm::vec3(m_Scene.GetWorldMatrix(e)[3]), origin, size, c)) continue;
            const bool selected = IsSelected(e);
            const float r = kBulbRadius * (selected ? 1.15f : 1.0f);
            const glm::vec3 lc = glm::clamp(light.Color, 0.0f, 1.0f);
            const ImU32 glassCol = IM_COL32((int)(90 + 165 * lc.r), (int)(90 + 165 * lc.g), (int)(90 + 165 * lc.b), 235);
            const ImU32 edge = selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(25, 25, 30, 220);
            const float th = selected ? 2.2f : 1.4f;
            const ImVec2 glass(c.x, c.y - r * 0.35f);
            // a soft glow, the glass, the filament
            dl->AddCircleFilled(glass, r * 1.6f, (glassCol & 0x00FFFFFFu) | (40u << 24));
            dl->AddCircleFilled(glass, r, glassCol);
            dl->AddCircle(glass, r, edge, 0, th);
            const ImU32 wire = IM_COL32(120, 80, 20, 230);
            dl->AddLine(ImVec2(glass.x - r * 0.35f, glass.y + r * 0.55f), ImVec2(glass.x - r * 0.35f, glass.y), wire, 1.2f);
            dl->AddLine(ImVec2(glass.x + r * 0.35f, glass.y + r * 0.55f), ImVec2(glass.x + r * 0.35f, glass.y), wire, 1.2f);
            dl->AddBezierQuadratic(ImVec2(glass.x - r * 0.35f, glass.y), ImVec2(glass.x, glass.y - r * 0.6f), ImVec2(glass.x + r * 0.35f, glass.y), wire, 1.2f);
            // the screw base: a grey collar with threads and a dark tip
            const float bw = r * 0.55f, top = glass.y + r * 0.82f, bottom = top + r * 0.62f;
            dl->AddRectFilled(ImVec2(c.x - bw, top), ImVec2(c.x + bw, bottom), IM_COL32(165, 168, 178, 255), 2.0f);
            dl->AddRect(ImVec2(c.x - bw, top), ImVec2(c.x + bw, bottom), edge, 2.0f, 0, selected ? 1.6f : 1.0f);
            for (int i = 1; i < 3; i++) {
                const float y = top + (bottom - top) * i / 3.0f;
                dl->AddLine(ImVec2(c.x - bw, y), ImVec2(c.x + bw, y), IM_COL32(90, 92, 100, 255), 1.0f);
            }
            dl->AddRectFilled(ImVec2(c.x - bw * 0.45f, bottom), ImVec2(c.x + bw * 0.45f, bottom + r * 0.22f), IM_COL32(40, 40, 46, 255), 1.5f);
        }
    }

    // The baked navigation grid (Play only: it is baked from physics bodies): a square per walkable
    // cell (green on the lowest layer of its column, blue above it, e.g. on a bridge; red = blocked)
    // and a faint line for each orthogonal link, so walls between cells show as gaps.
    void DrawNavGrid(const ImVec2& origin, const ImVec2& size) {
        using namespace Engine;
        const NavGrid& nav = m_Session->Navigation();
        if (!nav.IsBaked()) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const glm::mat4 vp = ViewCamera().GetViewProjection();
        auto project = [&](const glm::vec3& w, ImVec2& out) {
            glm::vec4 c = vp * glm::vec4(w, 1.0f);
            if (c.w <= 0.01f) return false;
            out = ImVec2(origin.x + (c.x / c.w * 0.5f + 0.5f) * size.x,
                origin.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * size.y);
            return true;
        };
        const float h = nav.Settings().CellSize * 0.42f;
        const glm::vec3 lift(0.0f, 0.03f, 0.0f);
        const bool links = nav.CellCount() <= 20000;
        for (int i = 0; i < nav.CellCount(); i++) {
            const glm::vec3 p = nav.CellPosition(i) + lift;
            ImVec2 q[4];
            const glm::vec3 k[4] = { p + glm::vec3(-h, 0, -h), p + glm::vec3(h, 0, -h), p + glm::vec3(h, 0, h), p + glm::vec3(-h, 0, h) };
            bool ok = true;
            for (int c = 0; c < 4 && ok; c++) ok = project(k[c], q[c]);
            if (!ok) continue;
            const ImU32 col = nav.IsBlocked(i) ? IM_COL32(255, 70, 60, 90)
                : nav.CellLayer(i) > 0 ? IM_COL32(80, 160, 255, 70) : IM_COL32(80, 230, 120, 55);
            dl->AddQuadFilled(q[0], q[1], q[2], q[3], col);
            if (!links) continue;
            for (int d : { 0, 2 }) {                  // +X and +Z: every link drawn once
                const int n = nav.Neighbor(i, d);
                ImVec2 a, b;
                if (n >= 0 && project(p, a) && project(nav.CellPosition(n) + lift, b))
                    dl->AddLine(a, b, IM_COL32(220, 255, 230, 60), 1.0f);
            }
        }
    }

    // Collision geometry for the overlay, fetched once per geometry (the copy was made every frame before)
    struct CollisionWire {
        std::weak_ptr<Engine::Renderable> Source;
        std::vector<glm::vec3> Positions;
        std::vector<uint32_t> Indices;
    };
    const CollisionWire& WireFor(const std::shared_ptr<Engine::Renderable>& geometry) {
        CollisionWire& w = m_CollisionWires[geometry.get()];
        if (w.Source.lock() != geometry) {           // new, or the address was reused by another geometry
            w.Source = geometry;
            w.Positions.clear(); w.Indices.clear();
            geometry->GetCollisionGeometry(w.Positions, w.Indices);
        }
        return w;
    }

    // True for the selected entity and everything under it (a body's child colliders)
    bool IsSelectedOrInside(entt::entity e) {
        int guard = 0;
        for (entt::entity p = e; p != entt::null && guard++ < 256; p = m_Scene.GetParent(p))
            if (p == m_Selected) return true;
        return false;
    }

    void DrawColliders(const ImVec2& origin, const ImVec2& size) {
        PROFILE_SCOPE("Collider overlay");
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
        // A box (local corners mn..mx under M) as 12 edges. Its 8 corners are projected once (through vp * M) and
        // a box wholly off screen is skipped; unselected outlines are 1 px (ImGui's fast line path). A forest of
        // 450 bodies took 8.7 ms a frame in a Debug build with each edge projecting its own ends.
        auto box = [&](const glm::mat4& M, const glm::vec3& mn, const glm::vec3& mx, ImU32 col, float thickness) {
            const glm::mat4 clip = vp * M;
            glm::vec4 c[8];
            int left = 0, right = 0, below = 0, above = 0, behind = 0;
            for (int i = 0; i < 8; i++) {
                c[i] = clip * glm::vec4((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z, 1.0f);
                left += c[i].x < -c[i].w; right += c[i].x > c[i].w; below += c[i].y < -c[i].w; above += c[i].y > c[i].w;
                behind += c[i].w <= 0.01f;
            }
            if (left == 8 || right == 8 || below == 8 || above == 8 || behind == 8) return;
            ImVec2 s[8];
            bool ok[8];
            for (int i = 0; i < 8; i++) {
                ok[i] = c[i].w > 0.01f;
                if (ok[i]) s[i] = ImVec2(origin.x + (c[i].x / c[i].w * 0.5f + 0.5f) * size.x, origin.y + (1.0f - (c[i].y / c[i].w * 0.5f + 0.5f)) * size.y);
            }
            static const int edges[12][2] = { { 0,1 },{ 2,3 },{ 4,5 },{ 6,7 },{ 0,2 },{ 1,3 },{ 4,6 },{ 5,7 },{ 0,4 },{ 1,5 },{ 2,6 },{ 3,7 } };
            for (auto& ed : edges)
                if (ok[ed[0]] && ok[ed[1]]) dl->AddLine(s[ed[0]], s[ed[1]], col, thickness);
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

            if (shape == ColliderShape::Terrain) {
                // The height field's border, following the ground (the surface itself is what is drawn)
                const auto* t = m_Scene.Registry().try_get<TerrainComponent>(entity);
                if (!t || !t->Data) return;
                const int n = 48;
                for (int side = 0; side < 4; side++) {
                    glm::vec3 prev(0.0f);
                    for (int i = 0; i <= n; i++) {
                        const float s = (float)i / n - 0.5f;
                        const float x = (side == 0 ? s : side == 1 ? 0.5f : side == 2 ? -s : -0.5f) * t->Size.x;
                        const float z = (side == 0 ? -0.5f : side == 1 ? s : side == 2 ? 0.5f : -s) * t->Size.y;
                        float h = 0.0f;
                        t->Data->HeightAt(t->Size, std::clamp(x, -0.4999f * t->Size.x, 0.4999f * t->Size.x),
                                          std::clamp(z, -0.4999f * t->Size.y, 0.4999f * t->Size.y), h);
                        const glm::vec3 p = glm::vec3(M * glm::vec4(x, h, z, 1.0f));
                        if (i > 0) line(prev, p, col);
                        prev = p;
                    }
                }
                return;
            }

            if (shape == ColliderShape::ConvexHull || shape == ColliderShape::Mesh) {
                const auto* mr = m_Scene.Registry().try_get<MeshRendererComponent>(entity);
                if (!mr || !mr->Geometry) return;
                const glm::mat4 G = M * mr->LocalOffset;
                // Not selected: the geometry's bounding box (a level built from mesh-collider pieces would
                // otherwise draw tens of thousands of lines a frame: 11 FPS on the village demo)
                if (!IsSelectedOrInside(entity)) {
                    glm::vec3 mn, mx;
                    mr->Geometry->GetLocalBounds(mn, mx);
                    box(G, mn, mx, (col & 0x00FFFFFFu) | (90u << 24), 1.0f);
                    return;
                }
                // Selected: the geometry the collider is built from, as a wireframe (cached per geometry).
                // Big meshes show an even sample of their triangles (at most 2000), which still reads as the shape.
                const CollisionWire& wire = WireFor(mr->Geometry);
                const size_t tris = wire.Indices.size() / 3;
                const size_t step = std::max<size_t>(1, tris / 2000);
                const ImU32 faint = (col & 0x00FFFFFFu) | (110u << 24);   // a wireframe this dense must not hide the model
                std::vector<glm::vec3>& pos = m_WireScratch;
                pos.resize(wire.Positions.size());
                for (size_t i = 0; i < pos.size(); i++) pos[i] = glm::vec3(G * glm::vec4(wire.Positions[i], 1.0f));
                for (size_t t = 0; t < tris; t += step) {
                    const size_t i = t * 3;
                    line(pos[wire.Indices[i]], pos[wire.Indices[i + 1]], faint);
                    line(pos[wire.Indices[i + 1]], pos[wire.Indices[i + 2]], faint);
                    line(pos[wire.Indices[i + 2]], pos[wire.Indices[i]], faint);
                }
                return;
            }
            if (shape == ColliderShape::Box) {
                const glm::vec3 h = sz * 0.5f;
                box(M, center - h, center + h, col, IsSelectedOrInside(entity) ? 1.5f : 1.0f);
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
            if (Projects::IsExamples(Projects::Current())) {     // built from code, with the Examples project's scripts
                ImGui::Separator();
                if (ImGui::BeginMenu("Example Levels")) {
                    ImGui::TextDisabled("Replaces the current scene");
                    if (ImGui::MenuItem("Script Demo")) BuildLevel(BuildScriptDemo);
                    if (ImGui::MenuItem("Coin Rush")) BuildLevel(BuildCoinRush);
                    if (ImGui::MenuItem("Tactics Test (navigation)")) BuildLevel(BuildTacticsTest);
                    if (ImGui::MenuItem("Tactics Cellar (second level)")) BuildLevel(BuildTacticsCellar);
                    if (ImGui::MenuItem("Terrain (sculpt and paint)")) BuildLevel(BuildTerrainDemo);
                    ImGui::EndMenu();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("New Project...")) NewProject();
            if (ImGui::MenuItem("Open Project...")) {
                const std::string folder = Platform::PickFolder("Open a project (a folder with project.json)", Projects::Current().Root);
                if (!folder.empty()) OpenProject(folder);
            }
            if (ImGui::BeginMenu("Recent Projects")) {
                for (const auto& r : Projects::Recent()) {
                    Engine::Project p;
                    const bool current = Engine::Project::Normalize(r) == Projects::Current().Root;
                    const std::string label = (Engine::Project::Load(r, p) ? p.Title : r) + "##" + r;
                    if (ImGui::MenuItem(label.c_str(), nullptr, current, !current)) OpenProject(r);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.c_str());
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Show Project in Explorer")) Platform::ShowInExplorer(Projects::Current().Root);
            ImGui::EndDisabled();
            if (playing) ImGui::TextDisabled("Stop Play to change scenes or projects");
            ImGui::Separator();
            if (ImGui::MenuItem("Build Game...")) m_ShowBuildGame = true;
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
            const bool selected = HasSelection();
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, selected && !playing)) DuplicateSelected();
            if (ImGui::MenuItem("Delete", "Del", false, selected)) DeleteSelected();
            if (ImGui::MenuItem("Group", "Ctrl+G", false, selected && !playing)) GroupSelected();
            ImGui::Separator();
            if (ImGui::MenuItem("Select All", "Ctrl+A", false, !playing)) SelectAll();
            if (ImGui::MenuItem("Deselect", nullptr, false, selected)) ClearSelection();
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
            ImGui::MenuItem("Assets", nullptr, &m_ShowAssets);
            ImGui::MenuItem("Dialogue Editor", nullptr, &m_ShowDialogue);
            ImGui::MenuItem("AnimGraph Editor", nullptr, &m_ShowAnimGraph);
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
        if (ImGui::MenuItem("Capsule")) shape("Capsule", m_Resources->Capsule(), 1.0f);
        if (ImGui::MenuItem("Cylinder")) shape("Cylinder", m_Resources->Cylinder(), 1.0f);
        if (ImGui::MenuItem("Cone")) shape("Cone", m_Resources->Cone(), 1.0f);
        if (ImGui::MenuItem("Terrain")) {
            // 64 x 64 m at the origin, solid (a static height field). Inspector > Terrain has the brushes.
            Entity e = m_Scene.CreateEntity("Terrain");
            auto& t = e.Add<TerrainComponent>();
            t.Data = std::make_shared<TerrainData>(129);
            t.DataPath = TerrainEditor::NewDataPath(m_Scene, Projects::Current().AssetDir(), m_SceneFile);
            auto& rb = e.Add<RigidBodyComponent>();
            rb.Type = BodyType::Static;
            rb.Shape = ColliderShape::Terrain;
            rb.Friction = 0.8f;
            rb.Restitution = 0.0f;
            m_Selected = e.GetHandle();
            m_TerrainEditor.SetTool(TerrainEditor::Tool::Raise);
        }
        ImGui::SeparatorText("Lights");
        if (ImGui::MenuItem("Point Light")) {
            Entity e = m_Scene.CreateEntity("Point Light");
            e.Add<PointLightComponent>();                     // shown in the editor as a lightbulb, never rendered
            place(e);
        }
        if (ImGui::MenuItem("Directional Light (Sun)")) {
            Entity e = m_Scene.CreateEntity("Sun");
            auto& t = e.Get<TransformComponent>();
            t.Rotation = glm::vec3(-0.9f, 0.5f, 0.0f);
            e.Add<DirectionalLightComponent>();       // shown in the editor as arrows (DrawSunGizmos), never rendered
            place(e);
        }
        if (ImGui::BeginMenu("Effects")) {
            for (const auto& name : ParticlePresetNames()) {
                if (!ImGui::MenuItem(name.c_str())) continue;
                Entity e = m_Scene.CreateEntity(name);
                auto& pe = e.Add<ParticleEmitterComponent>();
                ApplyParticlePreset(pe, name);
                place(e);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Grass")) {
            Entity e = m_Scene.CreateEntity("Grass");
            e.Add<FoliageComponent>();
            place(e);
        }
        if (ImGui::MenuItem("Grass Blocker")) {                 // an area kept free of grass (a path, a yard)
            Entity e = m_Scene.CreateEntity("Grass Blocker");
            e.Add<GrassBlockerComponent>();
            place(e);
            float y;                                            // on the ground under it, if it is over a terrain
            auto& t = e.Get<TransformComponent>();
            if (TerrainSurfaceAt(m_Scene, t.Position.x, t.Position.z, y)) t.Position.y = y;
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
            if (auto drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::Environment }); !drop.empty()) m_Renderer->RequestEnvironment(std::filesystem::path(drop).filename().string());
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
            ImGui::SliderInt("Update every", &s.PointUpdateInterval, 1, 8, s.PointUpdateInterval == 1 ? "frame" : "%d frames");
            ImGui::SetItemTooltip("How often a still light re-renders its shadows (the lights take turns).\n"
                "A light that moves, or has something moving or animated in reach, updates at once;\n"
                "swaying trees don't count, so their lamp shadows update at this rate.\n"
                "Each shadowed point light costs about 0.5 ms a frame on an RTX 2060 when it updates.");
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
            ImGui::Checkbox("Temporal anti-aliasing (TAA)", &m_Renderer->Post.TAA);
            ImGui::SetItemTooltip("Each frame is drawn shifted by a fraction of a pixel and blended with the frames\n"
                "before it: smooth, steady edges, leaves and fog in motion. Replaces FXAA. Rendered view only.");
            ImGui::BeginDisabled(m_Renderer->Post.TAA);
            ImGui::Checkbox("FXAA (anti-aliasing)", &m_Renderer->Post.FXAA);
            ImGui::EndDisabled();
            if (m_Renderer->Post.TAA) ImGui::SetItemTooltip("Used when TAA is off");

            ImGui::SeparatorText("Color grading");
            auto& grade = m_Renderer->Post;
            ImGui::SliderFloat("Contrast", &grade.Contrast, 0.5f, 2.0f);
            ImGui::SetItemTooltip("Above 1: deeper shadows and brighter lights (around mid grey)");
            ImGui::SliderFloat("Saturation", &grade.Saturation, 0.0f, 2.0f);
            ImGui::SliderFloat("Temperature", &grade.Temperature, -1.0f, 1.0f);
            ImGui::SetItemTooltip("White balance: left cooler (blue), right warmer (orange)");
            ImGui::SliderFloat("Tint", &grade.Tint, -1.0f, 1.0f);
            ImGui::SetItemTooltip("Left green, right magenta");
            ImGui::ColorEdit3("Shadow tint", &grade.ShadowTint.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            ImGui::ColorEdit3("Highlight tint", &grade.HighlightTint.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            ImGui::SetItemTooltip("Cool shadows with warm highlights is the classic film look (white = none)");
            ImGui::SliderFloat("Vignette", &grade.Vignette, 0.0f, 1.0f);
            ImGui::SliderFloat("Film grain", &grade.Grain, 0.0f, 0.1f, "%.3f");
            if (ImGui::Button("Reset grading")) {
                const Engine::PostSettings d;
                grade.Contrast = d.Contrast; grade.Saturation = d.Saturation; grade.Temperature = d.Temperature; grade.Tint = d.Tint;
                grade.ShadowTint = d.ShadowTint; grade.HighlightTint = d.HighlightTint; grade.Vignette = d.Vignette; grade.Grain = d.Grain;
            }

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

        if (ImGui::CollapsingHeader("Wind")) {
            auto& w = m_Scene.Wind;
            ImGui::SliderFloat("Direction##wind", &w.Direction, 0.0f, 360.0f, "%.0f deg");
            ImGui::SetItemTooltip("Around Y: 0 blows towards +X, 90 towards +Z");
            ImGui::DragFloat("Speed##wind", &w.Speed, 0.01f, 0.0f, 5.0f);
            ImGui::DragFloat("Strength##wind", &w.Strength, 0.01f, 0.0f, 5.0f);
            ImGui::TextDisabled("Sways Mesh Renderers with Wind > 0 (trees).\nGrass fields have their own wind.");
        }

        if (ImGui::CollapsingHeader("Volumetric Fog")) {
            auto& v = m_Renderer->VolFog;
            ImGui::PushID("volfog");
            ImGui::Checkbox("Enabled", &v.Enabled);
            ImGui::BeginDisabled(!v.Enabled);
            ImGui::SeparatorText("Shape");
            ImGui::SliderFloat("Density", &v.Density, 0.0f, 0.3f, "%.3f");
            ImGui::SetItemTooltip("How thick the fog is at and below the base height (per metre)");
            ImGui::DragFloat("Base height", &v.BaseHeight, 0.05f, -100.0f, 100.0f, "%.2f m");
            ImGui::SliderFloat("Height falloff", &v.HeightFalloff, 0.1f, 50.0f, "%.1f m");
            ImGui::SetItemTooltip("Above the base height the fog thins to about a third every this many metres");
            ImGui::SliderFloat("Wisps", &v.NoiseAmount, 0.0f, 1.0f);
            ImGui::SetItemTooltip("0 = even fog, 1 = patchy");
            ImGui::SliderFloat("Wisp scale", &v.NoiseScale, 0.01f, 1.0f, "%.3f / m");
            ImGui::DragFloat("Wind speed", &v.WindSpeed, 0.01f, 0.0f, 10.0f, "%.2f m/s");
            ImGui::SliderFloat("Wind direction", &v.WindDirection, 0.0f, 360.0f, "%.0f deg");
            ImGui::SeparatorText("Light");
            ImGui::ColorEdit3("Color", &v.Color.x);
            ImGui::SliderFloat("Sun shafts", &v.SunIntensity, 0.0f, 5.0f);
            ImGui::SliderFloat("Forward scattering", &v.Anisotropy, -0.9f, 0.9f);
            ImGui::SetItemTooltip("Towards 1 the fog glows when you look towards the sun");
            ImGui::SliderFloat("Sky light", &v.AmbientIntensity, 0.0f, 5.0f);
            ImGui::SliderFloat("Lamp glow", &v.PointLightIntensity, 0.0f, 5.0f);
            ImGui::SeparatorText("Quality");
            ImGui::SliderFloat("Distance", &v.MaxDistance, 5.0f, 500.0f, "%.0f m");
            ImGui::SliderInt("Steps", &v.Steps, 8, 96);
            ImGui::SetItemTooltip("Samples along each view ray (marched at half resolution)");
            ImGui::EndDisabled();
            ImGui::PopID();
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
                row("1 / 2 / 3 / 4", "Wireframe / Solid / Textured / Rendered");
                ImGui::SeparatorText("Tools");
                row("Q / W / E / R", "Select / Move / Rotate / Scale");
                row("N", "Spawn a cube");
                row("Del", "Delete the selection");
                row("Ctrl / Shift + click", "Add to or remove from the selection");
                row("Drag on the scene", "Box select (Ctrl / Shift: add)");
                row("Shift + click (Hierarchy)", "Select a range of rows");
                row("Ctrl+G / Ctrl+A", "Group the selection / Select all");
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
            e.Add<DirectionalLightComponent>();       // shown in the editor as arrows, never rendered
        }
        m_Renderer->VolFog = Engine::VolumetricFogSettings{};   // a new scene has no fog (not the last scene's)
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

    // ------------------------------------------------------------------ assets (the Assets panel's actions)
    // A model file on a Mesh Renderer: 2 m across, standing on the entity's origin, its own materials
    bool AssignModel(Engine::MeshRendererComponent& mr, const std::string& path) {
        auto model = m_Resources->LoadModel(path);
        if (!model) { LOG_WARN("Can't load the model {}", path); return false; }
        mr.Geometry = model;
        mr.LocalOffset = model->GetPlacementMatrix(2.0f);
        mr.UseModelMaterials = model->HasTexturedMaterials();
        if (mr.UseModelMaterials) mr.Color = glm::vec3(1.0f);
        return true;
    }

    // Put a model or prefab into the scene at a world position (as a child of `parent` if given)
    void PlaceAsset(const std::string& path, const glm::vec3& position, entt::entity parent = entt::null) {
        using namespace Engine;
        Entity e;
        if (AssetBrowser::KindOf(path) == AssetBrowser::Kind::Prefab) {
            e = m_Session->Prefabs().Instantiate(m_Scene, path);
            if (!e) { LOG_WARN("Can't load the prefab {}", path); return; }
        }
        else {
            e = m_Scene.CreateEntity(std::filesystem::path(path).stem().string());
            auto& mr = e.Add<MeshRendererComponent>();
            if (!AssignModel(mr, path)) { m_Scene.DestroyEntity(e); return; }
            // An animated model gets an Animator playing its first clip
            if (const auto* model = dynamic_cast<const Model*>(mr.Geometry.get()); model && model->IsSkinned()) {
                auto& an = e.Add<AnimatorComponent>();
                if (!model->AnimationNames().empty()) an.Clip = model->AnimationNames().front();
            }
        }
        glm::vec3 at = position;
        // A kit piece snaps to the 0.5 m grid (X/Z) and becomes still level geometry: Static, with a static mesh body
        if (e.Has<MeshRendererComponent>()) {
            if (const auto* model = dynamic_cast<const Model*>(e.Get<MeshRendererComponent>().Geometry.get()); model && model->NativeSize) {
                at.x = std::round(at.x * 2.0f) * 0.5f;
                at.z = std::round(at.z * 2.0f) * 0.5f;
                e.Get<MeshRendererComponent>().Static = true;
                auto& rb = e.Add<RigidBodyComponent>();
                rb.Type = BodyType::Static;
                rb.Shape = ColliderShape::Mesh;
            }
        }
        m_Scene.SetWorldPosition(e.GetHandle(), at);
        if (parent != entt::null && m_Scene.Registry().valid(parent)) m_Scene.SetParent(e.GetHandle(), parent, true);
        m_Selected = e.GetHandle();
        m_MaybeChanged = true;
    }

    // A texture, script or sound dropped on an entity
    void ApplyAssetToEntity(const std::string& path, entt::entity target) {
        using namespace Engine;
        if (target == entt::null || !m_Scene.Registry().valid(target)) return;
        Entity e(target, &m_Scene);
        const std::string& name = e.Get<TagComponent>().Tag;
        switch (AssetBrowser::KindOf(path)) {
        case AssetBrowser::Kind::Texture:
            if (!e.Has<MeshRendererComponent>()) { LOG_WARN("{} has no Mesh Renderer to put {} on", name, path); return; }
            if (auto tex = m_Resources->LoadTexture(path)) {
                auto& mr = e.Get<MeshRendererComponent>();
                mr.Texture = tex;
                mr.UseModelMaterials = false;            // the texture replaces the model's own materials
                LOG_INFO("{}: texture {}", name, path);
            }
            break;
        case AssetBrowser::Kind::Script:
            if (e.Has<ScriptComponent>()) e.Get<ScriptComponent>().Path = path;
            else e.Add<ScriptComponent>().Path = path;
            LOG_INFO("{}: script {}", name, path);
            break;
        case AssetBrowser::Kind::Sound:
            if (e.Has<AudioSourceComponent>()) e.Get<AudioSourceComponent>().Clip = path;
            else e.Add<AudioSourceComponent>().Clip = path;
            LOG_INFO("{}: sound {}", name, path);
            break;
        default: return;
        }
        m_Selected = target;
        m_MaybeChanged = true;
    }

    void HandleAssetActions(const std::vector<AssetBrowser::Action>& actions) {
        const bool playing = m_PlayState != PlayState::Edit;
        for (const auto& a : actions) {
            switch (a.What) {
            case AssetBrowser::Action::OpenScene:
                if (playing) { LOG_WARN("Stop Play before opening another scene"); break; }
                if (a.Path.rfind("Scenes/", 0) == 0) RequestLoad(a.Path.substr(7));
                else LOG_WARN("Scenes open from Assets/Scenes ({} is elsewhere)", a.Path);
                break;
            case AssetBrowser::Action::Place:
                PlaceAsset(a.Path, m_Camera->Position + m_Camera->GetFront() * 4.0f);
                break;
            case AssetBrowser::Action::PlaySound:
                m_Session->Audio().Play(a.Path);
                break;
            case AssetBrowser::Action::SetSky:
                if (a.Path.rfind("Environments/", 0) == 0) m_Renderer->RequestEnvironment(a.Path.substr(13));
                else LOG_WARN("Sky maps load from Assets/Environments ({} is elsewhere)", a.Path);
                break;
            case AssetBrowser::Action::OpenAnimGraph:
                m_AnimGraph.Open(a.Path);
                m_ShowAnimGraph = true;
                break;
            case AssetBrowser::Action::OpenDialogue:
                m_Dialogue.Open(a.Path);
                m_ShowDialogue = true;
                ImGui::SetWindowFocus("###DialogueEditor");
                break;
            case AssetBrowser::Action::OpenExternal:
                if (!Platform::OpenWithSystem(m_Assets.Absolute(a.Path))) LOG_WARN("No program opens {}", a.Path);
                break;
            }
        }
    }

    // Where the mouse points in the scene: the surface under it (from last frame's depth buffer), or
    // a few metres along the ray when it is over the sky. Also the entity under it, if any.
    glm::vec3 PointUnderMouse(const ImVec2& origin, const ImVec2& size, entt::entity* hit = nullptr) {
        return PointAt(ImGui::GetMousePos(), origin, size, hit);
    }
    glm::vec3 PointAt(const ImVec2& mouse, const ImVec2& origin, const ImVec2& size, entt::entity* hit = nullptr) {
        const float u = (mouse.x - origin.x) / std::max(size.x, 1.0f), v = (mouse.y - origin.y) / std::max(size.y, 1.0f);
        const int px = (int)(mouse.x - origin.x);
        const int py = (int)m_Renderer->GetFramebuffer().GetHeight() - 1 - (int)(mouse.y - origin.y);
        if (hit) {
            const int id = m_Renderer->GetFramebuffer().ReadEntityID(px, py);
            const entt::entity e = id < 0 ? entt::null : static_cast<entt::entity>(id);
            *hit = (e != entt::null && m_Scene.Registry().valid(e)) ? e : entt::null;
        }
        const Engine::Camera& cam = ViewCamera();
        const glm::mat4 inv = glm::inverse(cam.GetViewProjection());
        const float depth = m_Renderer->GetFramebuffer().ReadDepth(px, py);
        const glm::vec2 ndc(u * 2.0f - 1.0f, 1.0f - v * 2.0f);
        if (depth < 0.99999f) {
            const glm::vec4 p = inv * glm::vec4(ndc, depth * 2.0f - 1.0f, 1.0f);
            return glm::vec3(p) / p.w;
        }
        const glm::vec4 far = inv * glm::vec4(ndc, 1.0f, 1.0f);
        const glm::vec3 dir = glm::normalize(glm::vec3(far) / far.w - cam.Position);
        return cam.Position + dir * 6.0f;
    }

    // An asset dropped on the viewport image (call right after the image item)
    void AcceptViewportDrop(const ImVec2& origin, const ImVec2& size) {
        using K = AssetBrowser::Kind;
        const std::string path = AssetBrowser::AcceptDrop({ K::Model, K::Prefab, K::Texture, K::Script, K::Sound, K::Environment });
        if (path.empty()) return;
        entt::entity hit = entt::null;
        const glm::vec3 point = PointUnderMouse(origin, size, &hit);
        switch (AssetBrowser::KindOf(path)) {
        case K::Model: case K::Prefab: PlaceAsset(path, point); break;
        case K::Environment:
            if (path.rfind("Environments/", 0) == 0) m_Renderer->RequestEnvironment(path.substr(13));
            break;
        default:
            if (hit == entt::null) LOG_WARN("Drop {} on an object to give it to that object", path);
            else ApplyAssetToEntity(path, hit);
            break;
        }
    }

    // ---- Multiple selection. m_Selection holds every selected entity in the order picked; m_Selected is the
    // active one (the last picked): the Inspector shows it and the gizmo sits on it. Code that sets m_Selected
    // to something not in the selection makes it the only selection (SyncSelection), so single-select paths
    // (spawn, place, undo...) need nothing extra.
    void SyncSelection() {
        auto& reg = m_Scene.Registry();
        std::erase_if(m_Selection, [&](entt::entity e) { return !reg.valid(e); });
        if (m_Selected != entt::null && !reg.valid(m_Selected))
            m_Selected = m_Selection.empty() ? entt::null : m_Selection.back();
        if (m_Selected == entt::null) m_Selection.clear();
        else if (!IsSelected(m_Selected)) m_Selection = { m_Selected };
    }
    bool IsSelected(entt::entity e) const { return std::find(m_Selection.begin(), m_Selection.end(), e) != m_Selection.end(); }
    void SelectOnly(entt::entity e) {
        m_Selected = (e != entt::null && m_Scene.Registry().valid(e)) ? e : entt::null;
        m_Selection.clear();
        if (m_Selected != entt::null) m_Selection.push_back(m_Selected);
    }
    void AddToSelection(entt::entity e) {
        if (e == entt::null || !m_Scene.Registry().valid(e)) return;
        if (!IsSelected(e)) m_Selection.push_back(e);
        m_Selected = e;
    }
    void ToggleSelected(entt::entity e) {
        if (e == entt::null || !m_Scene.Registry().valid(e)) return;
        if (IsSelected(e)) {
            std::erase(m_Selection, e);
            if (m_Selected == e) m_Selected = m_Selection.empty() ? entt::null : m_Selection.back();
        }
        else AddToSelection(e);
    }
    void ClearSelection() { m_Selected = entt::null; m_Selection.clear(); }
    bool HasSelection() { return m_Selected != entt::null && m_Scene.Registry().valid(m_Selected); }

    // The selected entities that have no selected ancestor: moving, deleting, duplicating or re-parenting these
    // handles the whole selection once (a selected child goes along with its selected parent)
    std::vector<entt::entity> SelectionRoots() {
        SyncSelection();
        std::vector<entt::entity> roots;
        if (m_Selection.size() == 1) return m_Selection;
        const std::unordered_set<entt::entity> selected(m_Selection.begin(), m_Selection.end());
        for (entt::entity e : m_Selection) {
            bool under = false;
            int guard = 0;
            for (entt::entity p = m_Scene.GetParent(e); p != entt::null && guard++ < 256; p = m_Scene.GetParent(p))
                if (selected.count(p)) { under = true; break; }
            if (!under) roots.push_back(e);
        }
        return roots;
    }

    void DeleteSelected() {
        for (entt::entity e : SelectionRoots())
            if (m_Scene.Registry().valid(e)) m_Scene.DestroyEntity(Engine::Entity(e, &m_Scene));
        ClearSelection();
    }

    void UnparentSelected() {
        for (entt::entity e : SelectionRoots()) m_Scene.SetParent(e, entt::null, true);
    }

    // Put the selection under a new empty entity "Group" at the middle of their positions. The group goes
    // under the selection's common parent when they all share one, else it is a root. Selects the group.
    void GroupSelected() {
        using namespace Engine;
        const auto roots = SelectionRoots();
        if (roots.empty() || m_PlayState != PlayState::Edit) return;
        entt::entity parent = m_Scene.GetParent(roots.front());
        glm::vec3 center(0.0f);
        for (entt::entity e : roots) {
            center += glm::vec3(m_Scene.GetWorldMatrix(e)[3]);
            if (m_Scene.GetParent(e) != parent) parent = entt::null;
        }
        center /= (float)roots.size();
        Entity group = m_Scene.CreateEntity("Group");
        m_Scene.SetWorldPosition(group.GetHandle(), center);
        if (parent != entt::null) m_Scene.SetParent(group.GetHandle(), parent, true);
        for (entt::entity e : roots) m_Scene.SetParent(e, group.GetHandle(), true);
        m_HierarchyExpanded.insert(m_Scene.GetID(group.GetHandle()));   // show what went into it
        SelectOnly(group.GetHandle());
        m_History.Track(m_Scene, m_Selected, m_Resources->Registry(), "Group " + std::to_string(roots.size()) + " objects");
    }

    void SelectAll() {
        m_Selection.clear();
        for (auto e : m_Scene.Registry().view<Engine::TagComponent>()) m_Selection.push_back(e);
        m_Selected = m_Selection.empty() ? entt::null : m_Selection.back();
    }

    // Stats drawn over the top-right corner of the viewport image
    void DrawStatsOverlay(const ImVec2& origin, const ImVec2& size) {
        using namespace Engine;
        char lines[12][96];
        int n = 0;
        std::snprintf(lines[n++], 96, "%.0f FPS  (%.2f ms)", ImGui::GetIO().Framerate, 1000.0f / std::max(ImGui::GetIO().Framerate, 1.0f));
        std::snprintf(lines[n++], 96, "Draw calls  %u", RenderStats::DrawCalls);
        std::snprintf(lines[n++], 96, "Triangles   %u", RenderStats::Triangles);
        std::snprintf(lines[n++], 96, "Instances   %u", RenderStats::Instances);
        std::snprintf(lines[n++], 96, "Culled      %u", RenderStats::Culled);
        std::snprintf(lines[n++], 96, "Shadow skip %u", RenderStats::ShadowSkipped);
        if (m_Scene.StaticBatchCount() > 0)
            std::snprintf(lines[n++], 96, "Static      %u batches / %u objects", m_Scene.StaticBatchCount(), m_Scene.StaticMergedCount());
        if (RenderStats::Particles > 0) std::snprintf(lines[n++], 96, "Particles   %u", RenderStats::Particles);
        if (RenderStats::Foliage > 0) std::snprintf(lines[n++], 96, "Foliage     %u clumps", RenderStats::Foliage);
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
        // Only the rows in view are drawn (ImGuiListClipper): a forest of 700 entities cost 8 ms a frame in Debug
        // drawing every row. The tree is flattened into rows first, following the open nodes.
        if (!filter.empty()) {
            // Searching: a flat list of the matches
            std::vector<entt::entity> matches;
            for (auto handle : reg.view<TagComponent>())
                if (Lower(reg.get<TagComponent>(handle).Tag).find(filter) != std::string::npos) matches.push_back(handle);
            m_RowOrder = matches;
            ImGuiListClipper clipper;
            clipper.Begin((int)matches.size());
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    const entt::entity handle = matches[i];
                    ImGui::PushID((int)entt::to_integral(handle));
                    ImGui::Selectable(reg.get<TagComponent>(handle).Tag.c_str(), IsSelected(handle));
                    if (ImGui::IsItemClicked()) RowClicked(handle);
                    DrawRowContextMenu(handle);
                    ImGui::PopID();
                }
        }
        else {
            // A tree: roots first, children nested. Drag an entity onto another to make it a child,
            // or onto the empty space below the list to make it a root again.
            std::unordered_map<entt::entity, std::vector<entt::entity>> children;
            for (auto [child, p] : reg.view<ParentComponent>().each()) {
                const entt::entity parent = m_Scene.GetParent(child);
                if (parent != entt::null && parent != child) children[parent].push_back(child);
            }
            // Something newly selected (in the Viewport, say) inside folded groups: unfold its parents so its row shows
            if (m_Selected != m_HierarchyRevealed) {
                m_HierarchyRevealed = m_Selected;
                if (m_Selected != entt::null && reg.valid(m_Selected)) {
                    int guard = 0;
                    for (entt::entity p = m_Scene.GetParent(m_Selected); p != entt::null && guard++ < 64; p = m_Scene.GetParent(p))
                        m_HierarchyExpanded.insert(m_Scene.GetID(p));
                }
            }
            m_HierarchyRows.clear();
            std::function<void(entt::entity, int)> add = [&](entt::entity e, int depth) {
                auto it = children.find(e);
                const bool hasChildren = it != children.end();
                m_HierarchyRows.push_back({ e, depth, hasChildren });
                if (hasChildren && depth < 64 && m_HierarchyExpanded.count(m_Scene.GetID(e)))
                    for (auto c : it->second) add(c, depth + 1);
            };
            for (auto handle : reg.view<TagComponent>())
                if (m_Scene.GetParent(handle) == entt::null) add(handle, 0);
            m_RowOrder.clear();
            for (const auto& r : m_HierarchyRows) m_RowOrder.push_back(r.E);
            ImGuiListClipper clipper;
            clipper.Begin((int)m_HierarchyRows.size());
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) DrawHierarchyRow(m_HierarchyRows[i]);
        }

        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y, 30.0f)));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("MDB_ENTITY"))
                m_Reparent = { *(const entt::entity*)p->Data, entt::null, true };
            ImGui::EndDragDropTarget();
        }
        {   // a model or prefab dropped below the list: a new root entity in front of the camera
            using K = AssetBrowser::Kind;
            const std::string asset = AssetBrowser::AcceptDrop({ K::Model, K::Prefab });
            if (!asset.empty()) m_AssetDrop = { asset, entt::null, true };
        }
        if (ImGui::IsItemClicked() && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) ClearSelection();   // a click on empty space deselects
        // A plain click on a row that was already part of a multiple selection selects only it on release,
        // unless the press started a drag (dragging moves the whole selection)
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (m_PendingRowClick != entt::null && !m_RowDragged) SelectOnly(m_PendingRowClick);
            m_PendingRowClick = entt::null;
            m_RowDragged = false;
        }
        if (ImGui::BeginPopupContextWindow("HierarchyEmpty", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            DrawCreateMenuItems();
            ImGui::EndPopup();
        }
        ImGui::EndChild();
        ImGui::End();

        // Applied after the tree is drawn, so the views above aren't changed while being walked
        if (m_Reparent.Pending && reg.valid(m_Reparent.Child)) {
            // Dragging a selected row moves the whole selection; any other row moves alone
            std::vector<entt::entity> moving = IsSelected(m_Reparent.Child) ? SelectionRoots() : std::vector<entt::entity>{ m_Reparent.Child };
            int refused = 0;
            for (entt::entity e : moving)
                if (e != m_Reparent.Parent && !m_Scene.SetParent(e, m_Reparent.Parent, true)) refused++;
            if (refused) LOG_WARN("Can't parent an entity to itself or to one of its own children");
        }
        if (m_AssetDrop.Pending) {
            m_AssetDrop.Pending = false;
            const auto kind = AssetBrowser::KindOf(m_AssetDrop.Path);
            if (kind == AssetBrowser::Kind::Model || kind == AssetBrowser::Kind::Prefab) {
                // On a row: a child of that entity, at its position. Below the list: in front of the camera.
                const bool onRow = m_AssetDrop.Target != entt::null && reg.valid(m_AssetDrop.Target);
                const glm::vec3 at = onRow ? glm::vec3(m_Scene.GetWorldMatrix(m_AssetDrop.Target)[3])
                                           : m_Camera->Position + m_Camera->GetFront() * 4.0f;
                PlaceAsset(m_AssetDrop.Path, at, onRow ? m_AssetDrop.Target : entt::null);
            }
            else ApplyAssetToEntity(m_AssetDrop.Path, m_AssetDrop.Target);
        }
        if (m_RowAction.Kind != RowAction::None && reg.valid(m_RowAction.Target)) {
            // the menu acts on the whole selection (right-clicking a row outside it selected that row alone)
            if (m_RowAction.Kind == RowAction::Duplicate) DuplicateSelected();
            else if (m_RowAction.Kind == RowAction::Delete) DeleteSelected();
            else if (m_RowAction.Kind == RowAction::Unparent) UnparentSelected();
            else if (m_RowAction.Kind == RowAction::Group) GroupSelected();
        }
    }

    // A left click on a Hierarchy row. Ctrl: add or remove it. Shift: select the rows from the last clicked one
    // to this one (Ctrl+Shift adds them). Plain: select only it (on release when it is part of a selection).
    void RowClicked(entt::entity e) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift && m_RowAnchor != entt::null && m_Scene.Registry().valid(m_RowAnchor)) {
            auto a = std::find(m_RowOrder.begin(), m_RowOrder.end(), m_RowAnchor);
            auto b = std::find(m_RowOrder.begin(), m_RowOrder.end(), e);
            if (a != m_RowOrder.end() && b != m_RowOrder.end()) {
                if (a > b) std::swap(a, b);
                if (!io.KeyCtrl) m_Selection.clear();
                for (auto it = a; it <= b; ++it) if (!IsSelected(*it)) m_Selection.push_back(*it);
                std::erase(m_Selection, e);
                m_Selection.push_back(e);        // the clicked row is the active one
                m_Selected = e;
                return;
            }
        }
        m_RowAnchor = e;
        if (io.KeyCtrl) ToggleSelected(e);
        else if (IsSelected(e) && m_Selection.size() > 1) { m_PendingRowClick = e; m_Selected = e; std::erase(m_Selection, e); m_Selection.push_back(e); }
        else SelectOnly(e);
    }
    std::vector<entt::entity> m_RowOrder;               // the rows of the list as shown (for Shift ranges)
    entt::entity m_RowAnchor = entt::null;              // where a Shift range starts
    entt::entity m_PendingRowClick = entt::null;        // plain click inside a selection, applied on release
    bool m_RowDragged = false;

    // Right-click on a row: actions on the selection (applied after the list is drawn). A row outside the
    // selection becomes the only selected one first.
    void DrawRowContextMenu(entt::entity e) {
        if (!ImGui::BeginPopupContextItem()) return;     // keyed to the row just drawn
        if (!IsSelected(e)) SelectOnly(e);
        const bool playing = m_PlayState != PlayState::Edit;
        const size_t n = m_Selection.size();
        if (n > 1) ImGui::TextDisabled("%zu objects", n);
        bool anyParent = false;
        for (entt::entity s : m_Selection) if (m_Scene.GetParent(s) != entt::null) anyParent = true;
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !playing)) m_RowAction = { RowAction::Duplicate, e };
        if (ImGui::MenuItem("Delete", "Del")) m_RowAction = { RowAction::Delete, e };
        if (ImGui::MenuItem("Group", "Ctrl+G", false, !playing)) m_RowAction = { RowAction::Group, e };
        if (ImGui::MenuItem("Unparent", nullptr, false, anyParent)) m_RowAction = { RowAction::Unparent, e };
        ImGui::Separator();
        if (ImGui::MenuItem("Frame", "F")) FrameSelected();
        ImGui::EndPopup();
    }

    enum class RowAction { None, Duplicate, Delete, Unparent, Group };
    struct PendingRowAction { RowAction Kind; entt::entity Target; };
    PendingRowAction m_RowAction{ RowAction::None, entt::null };

    struct PendingReparent { entt::entity Child, Parent; bool Pending; };
    PendingReparent m_Reparent{ entt::null, entt::null, false };

    // One row of the flattened tree (DrawHierarchyPanel): indented by depth; open state kept in m_HierarchyExpanded
    struct HierarchyRow { entt::entity E; int Depth; bool HasChildren; };
    std::vector<HierarchyRow> m_HierarchyRows;
    std::unordered_set<uint64_t> m_HierarchyExpanded;     // entity IDs whose children are shown (everything starts folded)
    entt::entity m_HierarchyRevealed = entt::null;        // the selection whose parents were last unfolded

    void DrawHierarchyRow(const HierarchyRow& row) {
        using namespace Engine;
        auto& reg = m_Scene.Registry();
        const entt::entity e = row.E;
        const uint64_t id = m_Scene.GetID(e);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth
            | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (!row.HasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
        if (IsSelected(e)) flags |= ImGuiTreeNodeFlags_Selected;

        if (row.Depth > 0) ImGui::Indent(row.Depth * ImGui::GetStyle().IndentSpacing);
        const bool instance = reg.all_of<PrefabComponent>(e);
        if (instance) ImGui::PushStyleColor(ImGuiCol_Text, kPrefabColor);
        const bool wasOpen = row.HasChildren && m_HierarchyExpanded.count(id);
        if (row.HasChildren) ImGui::SetNextItemOpen(wasOpen);
        const bool open = ImGui::TreeNodeEx((void*)(uintptr_t)id, flags, "%s", reg.get<TagComponent>(e).Tag.c_str());
        if (instance) ImGui::PopStyleColor();
        if (row.Depth > 0) ImGui::Unindent(row.Depth * ImGui::GetStyle().IndentSpacing);
        if (row.HasChildren && open != wasOpen) {
            if (open) m_HierarchyExpanded.insert(id);
            else m_HierarchyExpanded.erase(id);
        }
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) RowClicked(e);
        DrawRowContextMenu(e);

        if (ImGui::BeginDragDropSource()) {
            m_RowDragged = true;
            ImGui::SetDragDropPayload("MDB_ENTITY", &e, sizeof(e));
            if (IsSelected(e) && m_Selection.size() > 1) ImGui::Text("%zu objects", m_Selection.size());
            else ImGui::Text("%s", reg.get<TagComponent>(e).Tag.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("MDB_ENTITY"))
                m_Reparent = { *(const entt::entity*)p->Data, e, true };
            ImGui::EndDragDropTarget();
        }
        {   // an asset dropped on this row (applied after the tree)
            using K = AssetBrowser::Kind;
            const std::string asset = AssetBrowser::AcceptDrop({ K::Model, K::Prefab, K::Texture, K::Script, K::Sound });
            if (!asset.empty()) m_AssetDrop = { asset, e, true };
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
                ImGui::MenuItem("Light Gizmos", nullptr, &m_ShowLightGizmos);
                ImGui::MenuItem("Grass Blockers", nullptr, &m_ShowGrassBlockers);
                ImGui::SetItemTooltip("Arrows for suns and lightbulbs for point lights (editor only, never rendered)");
                ImGui::MenuItem("Navigation Grid", nullptr, &m_ShowNavGrid);
                ImGui::MenuItem("Fog of War", nullptr, &m_Renderer->ShowFog);   // off: see through the fog while play-testing
                ImGui::MenuItem("Selection Outline", nullptr, &m_ShowOutline);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Shading")) {
                static const char* kNames[] = { "Wireframe", "Solid", "Textured", "Rendered" };
                static const char* kKeys[] = { "1", "2", "3", "4" };
                for (int i = 0; i < 4; i++)
                    if (ImGui::MenuItem(kNames[i], kKeys[i], m_ViewMode == (ViewMode)i)) m_ViewMode = (ViewMode)i;
                ImGui::Separator();
                ImGui::MenuItem("X-Ray Wireframe", nullptr, &m_WireXRay);
                ImGui::TextDisabled("The game camera always shows Rendered");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Debug")) {
                ImGui::MenuItem("Ambient Occlusion Only", nullptr, &m_Renderer->Post.AODebug);
                ImGui::MenuItem("Shadow Cascades", nullptr, &m_Renderer->Shadows().CascadeDebug);
                ImGui::MenuItem("Motion Vectors", nullptr, &m_Renderer->ShowMotionVectors);
                ImGui::SetItemTooltip("How far each pixel moved since the last frame (red = sideways, green = up/down;\n"
                    "black = still). Needs TAA on and the Rendered view.");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Camera")) {
                if (ImGui::MenuItem("Frame Selected", "F")) FrameSelected();
                ImGui::SetNextItemWidth(160.0f);
                ImGui::SliderFloat("Orbit speed", &m_Camera->OrbitSensitivity, 0.05f, 1.0f);
                ImGui::TextDisabled("Alt + left / middle / right drag: orbit / pan / zoom");
                ImGui::EndMenu();
            }
            // The shading modes as a row of toggles on the right of the bar
            {
                static const char* kShort[] = { "Wire", "Solid", "Textured", "Rendered" };
                static const char* kTips[] = { "Wireframe (1; press again for X-ray)", "Solid: colours, no textures (2)",
                                               "Textured: colours and textures, simple light (3)", "Rendered: full lighting (4)" };
                const ImGuiStyle& st = ImGui::GetStyle();
                float w = 0.0f;
                for (const char* n : kShort) w += ImGui::CalcTextSize(n).x + st.FramePadding.x * 2.0f + st.ItemSpacing.x;
                const float x = ImGui::GetWindowContentRegionMax().x - w;
                if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);
                ImGui::BeginDisabled(m_ViewingGame);
                for (int i = 0; i < 4; i++) {
                    const bool on = m_ViewMode == (ViewMode)i;
                    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    if (ImGui::SmallButton(kShort[i])) {
                        if (i == 0 && on) m_WireXRay = !m_WireXRay;
                        m_ViewMode = (ViewMode)i;
                    }
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", kTips[i]);
                }
                ImGui::EndDisabled();
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
        AcceptViewportDrop(origin, size);              // assets dragged from the Assets panel
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
        if (!m_ViewingGame && m_ShowLightGizmos) { DrawSunGizmos(origin, size); DrawPointLightGizmos(origin, size); }
        if (!m_ViewingGame && m_ShowGrassBlockers) DrawGrassBlockers(origin, size);
        if (!m_ViewingGame && m_ShowNavGrid && m_PlayState != PlayState::Edit) DrawNavGrid(origin, size);
        if (m_PlayState != PlayState::Edit)   // kept while paused: the scripts did not run again
            DrawScriptUI(ImGui::GetWindowDrawList(), m_GameFonts, m_Session->Scripts().UIItems(), origin, size, &m_UITextures);
        if (m_ShowStatsOverlay && !m_ViewingGame) DrawStatsOverlay(origin, size);
        if (m_ViewingGame) {
            ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + 8.0f, origin.y + size.y - 22.0f),
                IM_COL32(255, 255, 255, 110), "Game camera (Play > Play Through Game Camera)");
        }

        // ---- Terrain brushes (a terrain selected and a tool chosen in the Inspector): they own the left button
        bool terrainTool = false;
        if (!m_ViewingGame && m_PlayState == PlayState::Edit && !m_MouseLook && m_Selected != entt::null &&
            m_Scene.Registry().valid(m_Selected) && m_Scene.Registry().all_of<Engine::TerrainComponent>(m_Selected))
            terrainTool = m_TerrainEditor.Viewport(m_Scene, m_Selected, ViewCamera(), origin, size, imageHovered, navigating, io.DeltaTime);

        // ---- Gizmo for the selected entity (not while looking through the game camera)
        bool gizmoBusy = false;
        m_GizmoBusy = false;
        if (!m_ViewingGame && !m_SelectTool && !terrainTool && m_Selected != entt::null && m_Scene.Registry().valid(m_Selected) && !m_MouseLook) {
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
                // The gizmo sits on the active entity; every selected entity gets the same change, so a group
                // moves together, and rotates and scales about the active one's pivot
                const glm::mat4 before = m_Scene.GetWorldMatrix(m_Selected);
                const glm::mat4 delta = model * glm::inverse(before);
                for (entt::entity r : SelectionRoots())
                    m_Scene.SetWorldMatrix(r, r == m_Selected ? model : delta * m_Scene.GetWorldMatrix(r));   // back to local transforms
            }
            gizmoBusy = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
            m_GizmoBusy = ImGuizmo::IsUsing();
        }

        // ---- Click to select, drag on the scene for a selection box (not while using the gizmo)
        // Plain: select only that. Ctrl or Shift: add or remove it (a box adds everything inside it).
        auto& fb = m_Renderer->GetFramebuffer();
        if (!m_ViewingGame && imageHovered && !gizmoBusy && !terrainTool && !navigating && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_MouseLook) {
            m_BoxSelecting = true;
            m_BoxStart = ImGui::GetMousePos();
        }
        if (m_BoxSelecting) {
            const ImVec2 mouse = ImGui::GetMousePos();
            const bool dragged = std::abs(mouse.x - m_BoxStart.x) > 4.0f || std::abs(mouse.y - m_BoxStart.y) > 4.0f;
            const ImVec2 a(std::clamp(std::min(m_BoxStart.x, mouse.x), origin.x, origin.x + size.x), std::clamp(std::min(m_BoxStart.y, mouse.y), origin.y, origin.y + size.y));
            const ImVec2 b(std::clamp(std::max(m_BoxStart.x, mouse.x), origin.x, origin.x + size.x), std::clamp(std::max(m_BoxStart.y, mouse.y), origin.y, origin.y + size.y));
            if (dragged && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(a, b, IM_COL32(255, 140, 25, 30));
                dl->AddRect(a, b, IM_COL32(255, 140, 25, 200));
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || m_GizmoBusy || m_ViewingGame) {
                m_BoxSelecting = false;
                const bool add = io.KeyCtrl || io.KeyShift;
                const int fbH = (int)fb.GetHeight();
                if (!m_GizmoBusy && !m_ViewingGame && dragged) {
                    // every entity with a visible pixel inside the box (entity-ID buffer, origin bottom-left)
                    const int x0 = (int)(a.x - origin.x), x1 = (int)(b.x - origin.x);
                    const int y0 = fbH - (int)(b.y - origin.y), y1 = fbH - (int)(a.y - origin.y);
                    if (!add) ClearSelection();
                    for (int id : fb.ReadEntityIDs(x0, y0, x1 - x0, y1 - y0))
                        AddToSelection(static_cast<entt::entity>(id));
                    // lights and grass blockers aren't rendered: their gizmo's centre counts
                    auto inBox = [&](const glm::vec3& w) {
                        ImVec2 p;
                        return ProjectToViewport(w, origin, size, p) && p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y;
                    };
                    if (m_ShowLightGizmos) {
                        for (const SunGizmo& g : GatherSunGizmos()) if (inBox(g.Pos)) AddToSelection(g.E);
                        for (auto [le, light] : m_Scene.Registry().view<Engine::PointLightComponent>().each())
                            if (inBox(glm::vec3(m_Scene.GetWorldMatrix(le)[3]))) AddToSelection(le);
                    }
                    if (m_ShowGrassBlockers)
                        for (auto [ge, gb] : m_Scene.Registry().view<Engine::GrassBlockerComponent>().each())
                            if (inBox(glm::vec3(m_Scene.GetWorldMatrix(ge)[3]))) AddToSelection(ge);
                }
                else if (!m_GizmoBusy && !m_ViewingGame) {
                    const int px = (int)(m_BoxStart.x - origin.x);
                    const int py = fbH - 1 - (int)(m_BoxStart.y - origin.y);
                    const int id = fb.ReadEntityID(px, py);
                    entt::entity picked = id < 0 ? entt::null : static_cast<entt::entity>(id);
                    // a light's gizmo (drawn over the scene) wins over what is rendered behind it
                    {
                        if (const entt::entity g = LightGizmoAt(m_BoxStart, origin, size); g != entt::null) picked = g;   // each kind checks its own View toggle
                    }
                    if (add) ToggleSelected(picked);       // a click on nothing keeps the selection
                    else SelectOnly(picked);
                }
            }
        }
        ImGui::End();
    }
    bool m_BoxSelecting = false;     // the left button went down on the scene: a click or a selection box
    ImVec2 m_BoxStart{ 0.0f, 0.0f };

    // The standard layout (Unity / Godot style): Hierarchy left, Viewport centre, Console below it,
    // Inspector and Scene Settings as tabs on the right. Built when there is no editor.ini yet, or on
    // Window > Reset Layout.
    void BuildDefaultLayoutOnce(ImGuiID dockspace) {
        if (m_LayoutChecked && !m_ResetLayout) return;
        const bool force = m_ResetLayout;
        m_LayoutChecked = true;
        m_ResetLayout = false;
        if (!force && std::filesystem::exists("editor.ini")) return;
        m_ShowHierarchy = m_ShowInspector = m_ShowSceneSettings = m_ShowConsole = m_ShowAssets = m_ShowUI = true;

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
        ImGui::DockBuilderDockWindow("Assets", bottom);
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderDockWindow("###DialogueEditor", center);   // a tab beside the Viewport: it wants room
        ImGui::DockBuilderDockWindow("###AnimGraphEditor", center);
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

        if (m_Selection.size() > 1) {    // the Inspector edits one entity: the active one (last picked)
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "%zu objects selected", m_Selection.size());
            ImGui::SameLine();
            ImGui::TextDisabled("(showing the active one)");
            ImGui::SetItemTooltip("Move, rotate, scale, delete, duplicate, group and re-parent act on all of them.\n"
                                  "The fields below edit only this one. Ctrl+click it to deselect it.");
        }

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
                    for (const char* builtin : { "cube", "sphere", "plane", "capsule", "cylinder", "cone" })
                        if (ImGui::Selectable(builtin, current == builtin)) {
                            mr.Geometry = registry.FindGeometry(builtin);
                            mr.LocalOffset = glm::mat4(1.0f);
                            mr.UseModelMaterials = false;
                        }
                    ImGui::Separator();
                    std::error_code ec;
                    std::vector<std::string> files;
                    for (const auto& f : std::filesystem::directory_iterator(std::filesystem::u8path(Projects::Current().AssetDir()) / "Models", ec)) {
                        std::string ext = f.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                        if (ext == ".glb" || ext == ".gltf" || ext == ".fbx" || ext == ".obj") files.push_back("Models/" + f.path().filename().string());
                    }
                    std::sort(files.begin(), files.end());
                    for (const auto& file : files) {
                        if (!ImGui::Selectable(file.c_str(), current == file)) continue;
                        if (auto model = m_Resources->LoadModel(file)) {
                            mr.Geometry = model;
                            mr.LocalOffset = model->GetPlacementMatrix(2.0f);   // 2 m across, standing on the origin (kit pieces: as they are)
                            mr.UseModelMaterials = model->HasTexturedMaterials();
                            if (mr.UseModelMaterials) mr.Color = glm::vec3(1.0f);
                        }
                    }
                    ImGui::EndCombo();
                }
                if (auto drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::Model }); !drop.empty()) AssignModel(mr, drop);   // dragged from Assets
            }
            ImGui::ColorEdit3("Color", &mr.Color.x);
            if (!mr.UseModelMaterials) {
                ImGui::SliderFloat("Metallic", &mr.Metallic, 0.0f, 1.0f);
                ImGui::SliderFloat("Roughness", &mr.Roughness, 0.04f, 1.0f);
                ImGui::ColorEdit3("Emissive", &mr.Emissive.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
            }
            ImGui::SliderFloat("Specular", &mr.Specular, 0.0f, 1.0f);
            ImGui::SetItemTooltip("How much a non-metal reflects: 1 = the usual 4%% plus the bright sheen at grazing\n"
                "angles, 0 = none (matte bark, cloth, dry clay). With model materials it multiplies\n"
                "the file's own value (glTF KHR_materials_specular: Blender's Specular IOR Level).");
            if (mr.UseModelMaterials && mr.Geometry) {
                float lowest = 1.0f;
                for (uint32_t i = 0; i < mr.Geometry->SubmeshCount(); i++)
                    if (const auto* m = mr.Geometry->GetSubmeshMaterial(i)) lowest = std::min(lowest, m->Specular);
                if (lowest < 1.0f) ImGui::TextDisabled("The file lowers specular to %.2f on some parts", lowest);
            }
            ImGui::Checkbox("Unlit", &mr.Unlit);
            ImGui::SameLine();
            ImGui::Checkbox("Cast shadows", &mr.CastShadows);
            ImGui::Checkbox("Static", &mr.Static);
            ImGui::SetItemTooltip("Never moves or changes during Play: merged with other static\n"
                "parts of the same material when Play starts (fewer draws)");
            ImGui::SameLine();
            ImGui::Checkbox("Double-sided", &mr.DoubleSided);
            ImGui::SetItemTooltip("Draw the back faces too, lit from their own side, so a single plane\n"
                "(a leaf, a sign, cloth) is visible from behind. Back faces cost extra only on this object.");
            if (mr.Static) {
                // Grass fields with "Avoid static objects" leave out this object's footprint
                ImGui::Checkbox("Blocks grass", &mr.BlocksGrass);
                ImGui::SetItemTooltip("Grass (Avoid static objects) leaves out this object's footprint: the outline of\n"
                    "its vertices near the ground. Off: grass grows right through or under it.");
                if (mr.BlocksGrass) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
                    ImGui::DragFloat("##grassmargin", &mr.GrassMargin, 0.01f, -2.0f, 10.0f, "margin %.2f m");
                    ImGui::SetItemTooltip("Grow the grass-free footprint by this much (negative: shrink it, so grass\n"
                        "reaches in under the edges)");
                }
            }
            if (!mr.DoubleSided && mr.UseModelMaterials && mr.Geometry) {
                uint32_t doubleSided = 0;
                for (uint32_t i = 0; i < mr.Geometry->SubmeshCount(); i++)
                    if (const auto* m = mr.Geometry->GetSubmeshMaterial(i); m && m->DoubleSided) doubleSided++;
                if (doubleSided) ImGui::TextDisabled("%u model material(s) are double-sided (from the file)", doubleSided);
            }
            ImGui::DragFloat("Wind", &mr.Wind, 0.01f, 0.0f, 5.0f);
            ImGui::SetItemTooltip("Sway in the scene's wind (Scene Settings > Wind): 0 = still, 1 = a tree.\n"
                "The top of the mesh moves most, its base stays put.");
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
                    for (const auto& f : std::filesystem::directory_iterator(std::filesystem::u8path(Projects::Current().AssetDir()) / "Textures", ec)) {
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
                if (auto drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::Texture }); !drop.empty()) { if (auto t = m_Resources->LoadTexture(drop)) mr.Texture = t; }
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
            if (rb.Type == BodyType::Dynamic) {
                ImGui::SameLine();
                ImGui::Checkbox("Lock rotation", &rb.LockRotation);
                ImGui::SetItemTooltip("Physics only moves it, never turns it: for characters (a pushed box\n"
                    "would tip over). Scripts can still turn it. Applies on the next Play.");
            }

            if (ImGui::Button("Remove##rb")) e.Remove<RigidBodyComponent>();
            if (rb.Type != BodyType::Static && (e.Has<SpinComponent>() || e.Has<OrbitComponent>()) && rb.Type == BodyType::Dynamic)
                ImGui::TextDisabled("Physics overrides Spin/Orbit on dynamic bodies");
            if (m_PlayState != PlayState::Edit) ImGui::TextDisabled("Changes apply on the next Play");
        }

        if (e.Has<ParticleEmitterComponent>() && ImGui::CollapsingHeader("Particle Emitter", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& pe = e.Get<ParticleEmitterComponent>();
            ImGui::PushID("particles");
            // Presets and live controls
            if (ImGui::BeginCombo("Preset", "Apply a preset...")) {
                for (const auto& name : ParticlePresetNames())
                    if (ImGui::Selectable(name.c_str())) ApplyParticlePreset(pe, name);
                ImGui::EndCombo();
            }
            if (ImGui::Button("Burst")) pe.PendingBurst += pe.Burst > 0 ? pe.Burst : 20;
            ImGui::SetItemTooltip("Emit a burst now (the Burst count, or 20)");
            ImGui::SameLine();
            if (ImGui::Button("Clear")) pe.Particles.clear();
            ImGui::SameLine();
            if (ImGui::Button("Restart")) { pe.Particles.clear(); pe.Started = false; pe.SpawnCarry = 0.0f; }
            ImGui::SameLine();
            ImGui::TextDisabled("%zu / %d alive", pe.Particles.size(), pe.MaxParticles);

            ImGui::SeparatorText("Emission");
            ImGui::Checkbox("Emitting", &pe.Emitting);
            ImGui::SameLine();
            ImGui::Checkbox("World space", &pe.WorldSpace);
            ImGui::SetItemTooltip("On: particles stay where they were emitted. Off: they move with the entity.");
            ImGui::DragFloat("Rate", &pe.Rate, 0.5f, 0.0f, 2000.0f, "%.1f / s");
            ImGui::DragInt("Burst at start", &pe.Burst, 1.0f, 0, 5000);
            ImGui::DragInt("Max particles", &pe.MaxParticles, 5.0f, 1, 20000);
            static const char* shapes[] = { "Point", "Sphere", "Box" };
            int shape = (int)pe.Shape;
            if (ImGui::Combo("Shape", &shape, shapes, IM_ARRAYSIZE(shapes))) pe.Shape = (EmitShape)shape;
            if (pe.Shape == EmitShape::Sphere) ImGui::DragFloat("Radius", &pe.ShapeSize.x, 0.01f, 0.0f, 100.0f);
            else if (pe.Shape == EmitShape::Box) ImGui::DragFloat3("Box size", &pe.ShapeSize.x, 0.01f, 0.0f, 100.0f);

            ImGui::SeparatorText("Motion");
            ImGui::DragFloat2("Lifetime", &pe.Lifetime.x, 0.02f, 0.01f, 60.0f, "%.2f s");
            ImGui::DragFloat2("Speed", &pe.Speed.x, 0.05f, 0.0f, 100.0f, "%.2f m/s");
            ImGui::DragFloat3("Direction", &pe.Direction.x, 0.02f, -1.0f, 1.0f);
            ImGui::SliderFloat("Spread", &pe.Spread, 0.0f, 180.0f, "%.0f deg");
            ImGui::DragFloat("Gravity", &pe.GravityScale, 0.01f, -5.0f, 5.0f);
            ImGui::SetItemTooltip("1 = falls like a dropped object, negative = rises");
            ImGui::DragFloat("Drag", &pe.Drag, 0.01f, 0.0f, 20.0f);
            ImGui::DragFloat2("Spin", &pe.SpinSpeed.x, 1.0f, -1000.0f, 1000.0f, "%.0f deg/s");

            ImGui::SeparatorText("Look");
            ImGui::DragFloat2("Size", &pe.Size.x, 0.005f, 0.0f, 50.0f, "%.3f m");
            ImGui::DragFloat("End size", &pe.EndSize, 0.01f, 0.0f, 20.0f, "x %.2f");
            ImGui::ColorEdit4("Start color", &pe.StartColor.x);
            ImGui::ColorEdit4("End color", &pe.EndColor.x);
            ImGui::DragFloat("Emission", &pe.Emission, 0.05f, 0.0f, 50.0f);
            ImGui::SetItemTooltip("Brightness: above 1 glows with bloom");
            ImGui::Checkbox("Additive", &pe.Additive);
            ImGui::SetItemTooltip("Adds light (fire, sparks, magic). Off: covers what is behind (smoke, dust).");
            {
                const auto registry = m_Resources->Registry();
                std::string current = pe.Texture ? registry.TextureName(pe.Texture) : "(soft dot)";
                if (current.rfind("texture:", 0) == 0) current = current.substr(8);
                if (ImGui::BeginCombo("Texture##particles", current.c_str())) {
                    if (ImGui::Selectable("(soft dot)", !pe.Texture)) pe.Texture = nullptr;
                    std::error_code ec;
                    for (const auto& file : std::filesystem::directory_iterator(std::filesystem::u8path(Projects::Current().AssetDir()) / "Textures", ec)) {
                        const std::string rel = "Textures/" + file.path().filename().string();
                        if (AssetBrowser::KindOf(rel) != AssetBrowser::Kind::Texture) continue;
                        if (ImGui::Selectable(rel.c_str(), current == rel))
                            if (auto t = m_Resources->LoadTexture(rel)) pe.Texture = t;
                    }
                    ImGui::EndCombo();
                }
                if (auto drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::Texture }); !drop.empty()) {
                    if (auto t = m_Resources->LoadTexture(drop)) pe.Texture = t;
                }
            }
            ImGui::Checkbox("Destroy when done", &pe.DestroyWhenDone);
            ImGui::SetItemTooltip("Remove the entity once it stops emitting and its particles are gone (one-shot effects)");
            if (ImGui::Button("Remove##particles")) e.Remove<ParticleEmitterComponent>();
            ImGui::PopID();
        }

        if (e.Has<GrassBlockerComponent>() && ImGui::CollapsingHeader("Grass Blocker", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& b = e.Get<GrassBlockerComponent>();
            ImGui::PushID("grassblocker");
            int shape = (int)b.Shape;
            if (ImGui::Combo("Shape", &shape, "Box\0Circle\0Capsule\0")) b.Shape = (GrassBlockerShape)shape;
            switch (b.Shape) {
            case GrassBlockerShape::Box:
                ImGui::DragFloat2("Size", &b.Size.x, 0.05f, 0.05f, 1000.0f, "%.2f m");
                ImGui::SetItemTooltip("X by Z, in the entity's space (its rotation and scale apply)");
                break;
            case GrassBlockerShape::Circle:
                ImGui::DragFloat("Diameter", &b.Size.x, 0.05f, 0.05f, 1000.0f, "%.2f m");
                break;
            case GrassBlockerShape::Capsule:
                ImGui::DragFloat("Length", &b.Size.x, 0.05f, 0.05f, 1000.0f, "%.2f m");
                ImGui::SetItemTooltip("End to end, along the entity's X: rotate it to aim a path");
                ImGui::DragFloat("Width", &b.Size.y, 0.05f, 0.05f, 1000.0f, "%.2f m");
                break;
            }
            b.Size = glm::max(b.Size, glm::vec2(0.05f));
            ImGui::DragFloat("Soft edge", &b.Softness, 0.01f, 0.0f, 50.0f, "%.2f m");
            ImGui::SetItemTooltip("The grass thins out over this distance inside the edge, instead of stopping at a line");
            ImGui::TextDisabled("Keeps every grass field out of this area. Editor only: never rendered.");
            if (ImGui::Button("Remove##grassblocker")) e.Remove<GrassBlockerComponent>();
            ImGui::PopID();
        }

        if (e.Has<FoliageComponent>() && ImGui::CollapsingHeader("Foliage", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& f = e.Get<FoliageComponent>();
            ImGui::PushID("foliage");
            ImGui::TextDisabled("%.0f clumps", f.Size.x * f.Size.y * f.Density);
            ImGui::SameLine();
            if (ImGui::SmallButton("New seed")) f.Seed = f.Seed * 1664525u + 1013904223u;
            ImGui::SeparatorText("Area");
            ImGui::DragFloat2("Size", &f.Size.x, 0.1f, 0.1f, 1000.0f, "%.1f m");
            ImGui::SetItemTooltip("In the entity's X/Z, centred on it. The clumps stand on its height.");
            ImGui::DragFloat("Density", &f.Density, 0.1f, 0.05f, 60.0f, "%.1f / m2");
            int seed = (int)f.Seed;
            if (ImGui::DragInt("Seed", &seed, 1.0f, 0, INT_MAX)) f.Seed = (uint32_t)std::max(seed, 0);
            ImGui::Checkbox("Avoid static objects", &f.AvoidStatic);
            ImGui::SetItemTooltip("No grass under Static meshes standing on the ground: walls, floors, props.\nMoving them scatters it again.");
            ImGui::Checkbox("Follow terrain", &f.FollowTerrain);
            ImGui::SetItemTooltip("Over a terrain, the clumps stand on its surface instead of the entity's height");
            {
                const char* layers[] = { "Anywhere", "Terrain layer 1", "Terrain layer 2", "Terrain layer 3", "Terrain layer 4" };
                int layer = std::clamp(f.TerrainLayer + 1, 0, 4);
                if (ImGui::Combo("Grows on", &layer, layers, 5)) f.TerrainLayer = layer - 1;
                ImGui::SetItemTooltip("Only where that layer of a terrain is painted (painting it there grows grass)");
            }
            ImGui::DragFloat("Draw distance", &f.DrawDistance, 0.5f, 2.0f, 500.0f, "%.0f m");
            ImGui::SetItemTooltip("The grass thins out over the second half of this distance");
            ImGui::SeparatorText("Look");
            ImGui::DragFloat("Height", &f.Height, 0.01f, 0.02f, 5.0f, "%.2f m");
            ImGui::SliderFloat("Height variation", &f.HeightVariation, 0.0f, 0.95f);
            ImGui::DragFloat("Width", &f.Width, 0.01f, 0.1f, 5.0f, "x %.2f");
            ImGui::ColorEdit3("Root color", &f.BaseColor.x);
            ImGui::ColorEdit3("Tip color", &f.TipColor.x);
            ImGui::ColorEdit3("Dry color", &f.DryColor.x);
            ImGui::SliderFloat("Dry patches", &f.DryAmount, 0.0f, 1.0f);
            ImGui::SliderFloat("Flowers", &f.FlowerAmount, 0.0f, 1.0f);
            ImGui::SliderFloat("Roughness", &f.Roughness, 0.05f, 1.0f);
            ImGui::SeparatorText("Wind and touch");
            ImGui::DragFloat("Wind strength", &f.WindStrength, 0.01f, 0.0f, 3.0f);
            ImGui::DragFloat("Wind speed", &f.WindSpeed, 0.01f, 0.0f, 5.0f);
            ImGui::SliderFloat("Wind direction", &f.WindDirection, 0.0f, 360.0f, "%.0f deg");
            ImGui::SetItemTooltip("Around Y: 0 blows towards +X, 90 towards +Z");
            ImGui::SliderFloat("Interaction", &f.Interaction, 0.0f, 2.0f);
            ImGui::SetItemTooltip("How far moving bodies (dynamic, kinematic) and foliage.push bend the grass");
            ImGui::DragFloat("Recovery", &f.Recovery, 0.01f, 0.05f, 20.0f, "%.2f s");
            ImGui::SetItemTooltip("Seconds for pressed grass to stand up again");
            if (ImGui::Button("Remove##foliage")) e.Remove<FoliageComponent>();
            ImGui::PopID();
        }

        if (e.Has<TerrainComponent>() && ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (m_PlayState != PlayState::Edit) ImGui::TextDisabled("Stop Play to edit the terrain");
            else m_TerrainEditor.DrawInspector(m_Scene, e.GetHandle(), *m_Resources, Projects::Current().AssetDir());
            if (ImGui::Button("Remove##terrain")) { m_TerrainEditor.SetTool(TerrainEditor::Tool::None); e.Remove<TerrainComponent>(); }
        }

        if (e.Has<AnimatorComponent>() && ImGui::CollapsingHeader("Animator", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& an = e.Get<AnimatorComponent>();
            const auto* mr = e.Has<MeshRendererComponent>() ? &e.Get<MeshRendererComponent>() : nullptr;
            const Model* model = mr ? dynamic_cast<const Model*>(mr->Geometry.get()) : nullptr;
            if (!model || !model->IsSkinned()) {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Needs a Mesh Renderer with an animated model");
            }
            else {
                // Animation graph: state machines instead of a single clip (AnimGraph Editor)
                auto setGraph = [&](const std::string& path) {
                    an.Graph = path;
                    an.GraphData = path.empty() ? nullptr : m_Resources->LoadAnimGraph(path);
                    an.GraphLayers.clear();
                };
                if (ImGui::BeginCombo("Graph", an.Graph.empty() ? "(none: play a clip)" : an.Graph.c_str())) {
                    if (ImGui::Selectable("(none: play a clip)", an.Graph.empty())) setGraph("");
                    for (const auto& f : ListAnimGraphFiles())
                        if (ImGui::Selectable(f.c_str(), an.Graph == f)) setGraph(f);
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("An animation graph (Assets/AnimGraphs): states, blends, layers and transitions\n"
                                      "driven by parameters. Drop one from the Assets panel.");
                if (auto drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::AnimGraph }); !drop.empty()) setGraph(drop);
                if (!an.Graph.empty()) {
                    if (ImGui::Button("Edit graph")) { m_AnimGraph.Open(an.Graph); m_ShowAnimGraph = true; }
                    if (!an.GraphData) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Can't load %s", an.Graph.c_str());
                    else {
                        const AnimGraph& g = *an.GraphData;
                        for (size_t l = 0; l < g.Layers.size(); l++)
                            ImGui::TextDisabled("%s: %s", g.Layers[l].Name.c_str(), AnimGraphStateName(an, l).c_str());
                        // the parameters' current values (Edit mode: a preview; Play: the running game's)
                        for (const AnimParam& p : g.Params) {
                            auto it = an.Params.find(p.Name);
                            float& v = it != an.Params.end() ? it->second : (an.Params[p.Name] = p.Default);
                            const std::string label = p.Name + "##gp";
                            switch (p.Type) {
                            case AnimParamType::Float: ImGui::DragFloat(label.c_str(), &v, 0.01f); break;
                            case AnimParamType::Int: { int iv = (int)std::round(v); if (ImGui::DragInt(label.c_str(), &iv)) v = (float)iv; break; }
                            case AnimParamType::Bool: { bool b = v > 0.5f; if (ImGui::Checkbox(label.c_str(), &b)) v = b ? 1.0f : 0.0f; break; }
                            case AnimParamType::Trigger:
                                if (ImGui::Button(("Fire " + label).c_str())) v = 1.0f;
                                break;
                            }
                        }
                    }
                }
                if (an.Graph.empty() && ImGui::BeginCombo("Clip", an.Clip.empty() ? "(rest pose)" : an.Clip.c_str())) {
                    if (ImGui::Selectable("(rest pose)", an.Clip.empty())) an.Clip.clear();
                    for (const auto& name : model->AnimationNames())
                        if (ImGui::Selectable(name.c_str(), an.Clip == name)) an.Clip = name;
                    ImGui::EndCombo();
                }
                ImGui::SliderFloat("Speed##anim", &an.Speed, -2.0f, 3.0f);
                if (an.Graph.empty()) { ImGui::Checkbox("Loop##anim", &an.Loop); ImGui::SameLine(); }
                ImGui::Checkbox("Playing##anim", &an.Playing);
                if (an.Graph.empty()) {
                    ImGui::DragFloat("Blend time", &an.BlendTime, 0.01f, 0.0f, 2.0f, "%.2f s");
                    const float duration = model->AnimationDuration(model->FindAnimation(an.ActiveClip));
                    if (duration > 0.0f) ImGui::SliderFloat("Time##anim", &an.Time, 0.0f, duration, "%.2f s");
                }
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
                const std::filesystem::path dir = std::filesystem::u8path(Projects::Current().AssetDir()) / "Scripts";
                for (const auto& f : std::filesystem::directory_iterator(dir, ec))
                    if (f.path().extension() == ".lua") files.push_back("Scripts/" + f.path().filename().string());
                std::sort(files.begin(), files.end());
                for (const auto& f : files)
                    if (ImGui::Selectable(f.c_str(), f == sc.Path)) sc.Path = f;
                ImGui::EndCombo();
            }
            if (auto drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::Script }); !drop.empty()) sc.Path = drop;   // dragged from Assets
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", sc.Path.c_str());
            if (ImGui::InputText("Path", buf, sizeof(buf))) sc.Path = buf;
            const bool exists = !sc.Path.empty() && std::filesystem::exists(Projects::Current().AssetDir() + sc.Path);
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
            if (auto drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::Sound }); !drop.empty()) a.Clip = drop;   // dragged from Assets
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
            if (!e.Has<ParticleEmitterComponent>() && ImGui::MenuItem("Particle Emitter")) e.Add<ParticleEmitterComponent>();
            if (!e.Has<FoliageComponent>() && ImGui::MenuItem("Foliage")) e.Add<FoliageComponent>();
            if (!e.Has<GrassBlockerComponent>() && ImGui::MenuItem("Grass Blocker")) e.Add<GrassBlockerComponent>();
            if (!e.Has<TerrainComponent>() && ImGui::MenuItem("Terrain")) {
                auto& t = e.Add<TerrainComponent>();
                t.Data = std::make_shared<TerrainData>(129);
                t.DataPath = TerrainEditor::NewDataPath(m_Scene, Projects::Current().AssetDir(), m_SceneFile);
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
        else if (e.Has<TerrainComponent>() && e.Get<TerrainComponent>().Data) {
            const auto& tc = e.Get<TerrainComponent>();
            const glm::vec3 lo(-0.5f * tc.Size.x, tc.Data->MinHeight(), -0.5f * tc.Size.y);
            const glm::vec3 hi(0.5f * tc.Size.x, tc.Data->MaxHeight(), 0.5f * tc.Size.y);
            const glm::mat4 m = m_Scene.GetWorldMatrix(handle);
            mn = glm::vec3(FLT_MAX);
            mx = glm::vec3(-FLT_MAX);
            for (int i = 0; i < 8; i++) {
                const glm::vec3 w = glm::vec3(m * glm::vec4((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z, 1.0f));
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

        SyncSelection();
        if (!m_Selection.empty()) {             // the box around everything selected
            for (entt::entity e : m_Selection) {
                glm::vec3 a, b;
                GetWorldBounds(e, a, b);
                mn = glm::min(mn, a);
                mx = glm::max(mx, b);
            }
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
    Engine::UIFonts m_GameFonts;                       // large fonts for script UI (null: the default font)
    std::shared_ptr<Engine::Mesh> m_Cube;              // shortcuts into m_Resources
    std::shared_ptr<Engine::Mesh> m_Sphere;
    std::shared_ptr<Engine::Texture2D> m_Checker;
    std::shared_ptr<Engine::Mesh>  m_Plane;
    std::shared_ptr<Engine::Model> m_ModelResource;
    std::vector<Engine::Entity> m_Spawned;
    entt::entity m_Selected = entt::null;              // the active entity (Inspector, gizmo pivot)
    std::vector<entt::entity> m_Selection;             // every selected entity, m_Selected last (see SyncSelection)
    std::mt19937 m_Rng{ std::random_device{}() };
    std::string m_SceneFile = "scene.json";      // in Assets/Scenes
    std::string m_PendingSceneLoad;              // queued Load: applied at the start of OnUpdate
    char m_SaveAsName[64] = {};

    enum class PlayState { Edit, Play, Paused };
    enum class PlayCmd { None, Play, Stop, Pause, Step, Restart };

    PlayState m_PlayState = PlayState::Edit;
    PlayCmd   m_PlayCmd = PlayCmd::None;     // set by the UI, applied at the start of the next frame
    Engine::SceneSnapshot m_Snapshot;
    Engine::SceneSettings m_PlaySettings;    // the render settings when Play started
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
    bool  m_ShowColliders = false;        // Viewport > View > Colliders (off by default)
    std::unordered_map<const Engine::Renderable*, CollisionWire> m_CollisionWires;
    std::vector<glm::vec3> m_WireScratch;
    bool  m_ShowNavGrid = true;      // the navigation grid, once a script baked it (Play)
    // Viewport shading (keys 1-4) and the selection outline; looking through the game camera always renders
    using ViewMode = Engine::SceneRenderer::ViewMode;
    ViewMode m_ViewMode = ViewMode::Rendered;
    bool  m_WireXRay = false;        // wireframe: show the edges behind surfaces too
    bool  m_ShowOutline = true;
    std::string m_PendingSelect;     // MDB_SELECT test hook
    bool  m_Num1Down = false, m_Num2Down = false, m_Num3Down = false, m_Num4Down = false;

    // Editor shell (menu bar, toolbar, panels)
    bool  m_ShowHierarchy = true, m_ShowInspector = true, m_ShowSceneSettings = true, m_ShowConsole = true;
    bool  m_ShowStatsOverlay = true, m_ShowFrustums = true, m_ShowControls = false;
    bool  m_ShowLightGizmos = true;       // Viewport > View > Light Gizmos: sun arrows and point-light bulbs
    bool  m_ShowGrassBlockers = true;     // Viewport > View > Grass Blockers: their outlines
    AssetBrowser m_Assets{ Projects::Current().AssetDir() };   // the Assets panel
    bool  m_ShowAssets = true;
    DialogueEditor m_Dialogue{ Projects::Current().AssetDir() };   // the Dialogue Editor window (Window menu, or a dialogue in Assets)
    TerrainEditor m_TerrainEditor;        // the terrain brushes (Inspector > Terrain, used in the Viewport)
    AnimGraphEditor m_AnimGraph{ Projects::Current().AssetDir() };   // the AnimGraph Editor window (Window menu, or a graph in Assets)
    bool  m_ShowAnimGraph = false;
    bool  m_AskGraphQuit = false, m_GraphDiscarded = false;           // quitting with an unsaved graph
    bool  m_ShowDialogue = false;
    bool  m_AskDialogueQuit = false, m_DialogueDiscarded = false;   // quitting with an unsaved dialogue
    struct AssetDrop { std::string Path; entt::entity Target = entt::null; bool Pending = false; };
    AssetDrop m_AssetDrop;                           // an asset dropped on the Hierarchy, applied after the tree
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
    std::string m_SwitchProject;                 // File > Open Project: restart with --project <this> when quitting
    GameBuilder m_Builder;                       // File > Build Game
    bool m_ShowBuildGame = false, m_BuildExcludeLoaded = false;
    std::string m_BuildOutput, m_BuildExclude;
    size_t m_BuildLogLines = 0;

    ImGuizmo::OPERATION m_GizmoOp = ImGuizmo::TRANSLATE;

    ImVec2 m_ViewportSize{ 0.0f, 0.0f };
    ImVec2 m_ViewportOrigin{ 0.0f, 0.0f };            // the viewport image's top left on screen
    bool m_ViewportHovered = false;
    Engine::UITextureCache m_UITextures{ Projects::Current().AssetDir() };   // images for ui.image                  
    bool m_LayoutChecked = false;      
    bool m_GizmoLocal = false;
    bool m_Snap = false;
    bool m_WDown = false, m_EDown = false, m_RDown = false;
    bool m_ADown = false;
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
    return std::make_unique<EditorApp>();
}
