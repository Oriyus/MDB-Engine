// The standalone game: plays one scene full screen (or in a window) with no editor.
//
// The game is a project (a folder with project.json and Assets/, see Core/Project.h). Settings come from
// project.json, and the command line can override them:
//   Game.exe [--project <folder>] [--scene <file in Assets/Scenes>] [--fullscreen | --windowed] [--width N]
//            [--height N] [--no-vsync] [--quit-after <seconds>] [--save-dir <folder>]
// Save slots go to %APPDATA%/<title>/Saves unless --save-dir says otherwise.
// Where things are: a packaged game (File > Build Game in the editor, or the package_game target) has
// project.json, Assets and EngineAssets next to the exe. Otherwise (running from the build) the project is
// --project or the source tree's Examples, and the engine files are the source tree's EngineAssets.
//
// Keys: Esc pauses (a menu with Resume / Restart / Quit buttons; Esc, R and Q work too),
// F11 or Alt+Enter toggles full screen.
// Everything else belongs to the game's scripts.

#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Input.h"
#include "Core/Log.h"
#include "Core/Project.h"
#include "Renderer/Camera.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Physics/PhysicsWorld.h"
#include "Scripting/ScriptEngine.h"
#include "Runtime/ResourceLibrary.h"
#include "Runtime/GameSession.h"
#include "Runtime/GameUI.h"

#include <glad/glad.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

    struct GameConfig {
        std::string AssetDir;                 // the project's Assets: absolute, with a trailing slash
        std::string EngineDir;                // the engine's files (shaders, Scripts/Lib): the same
        std::string Scene = "main.json";      // in Assets/Scenes
        Engine::WindowProps Window;
        float QuitAfter = 0.0f;               // test hook: close after this many seconds (0 = never)
        std::string SaveDir;                  // save slots: %APPDATA%/<title>/Saves unless --save-dir is given
    };

    GameConfig LoadConfig() {
        const auto& args = Engine::Application::GetCommandLineArgs();
        GameConfig c;
        c.Window.Title = "MDB Game";

        // The project: --project, else project.json next to the executable (a packaged game), else the
        // source tree's Examples. The engine files: EngineAssets next to the executable, else the source tree's.
        std::error_code ec;
        const fs::path exeDir = args.empty() ? fs::current_path(ec) : fs::absolute(args[0], ec).parent_path();
        std::string projectDir;
        for (size_t i = 1; i + 1 < args.size(); i++) if (args[i] == "--project") projectDir = args[i + 1];
        if (projectDir.empty()) projectDir = Engine::Project::IsProject(Engine::Project::PathString(exeDir)) ? Engine::Project::PathString(exeDir) : std::string(EXAMPLES_DIR);
        Engine::Project project;
        std::string error;
        if (!Engine::Project::Load(projectDir, project, &error)) {
            LOG_WARN("{} - using defaults", error);
            project.Root = Engine::Project::Normalize(projectDir);
        }
        c.AssetDir = project.AssetDir();
        c.EngineDir = fs::is_directory(exeDir / "EngineAssets" / "Shaders", ec) ? Engine::Project::Normalize(Engine::Project::PathString(exeDir / "EngineAssets"))
                                                                                   : std::string(ENGINE_ASSET_DIR);
        c.Window.Title = project.Title;
        c.Scene = project.Scene;
        c.Window.Width = project.Width;
        c.Window.Height = project.Height;
        c.Window.Fullscreen = project.Fullscreen;
        c.Window.VSync = project.VSync;
        LOG_INFO("Project: {} ({})  Engine files: {}", project.Title, project.Root, c.EngineDir);

        for (size_t i = 1; i < args.size(); i++) {
            const std::string& a = args[i];
            const bool hasValue = i + 1 < args.size();
            if (a == "--project" && hasValue) i++;       // read above
            else if (a == "--scene" && hasValue) c.Scene = args[++i];
            else if (a == "--fullscreen") c.Window.Fullscreen = true;
            else if (a == "--windowed") c.Window.Fullscreen = false;
            else if (a == "--no-vsync") c.Window.VSync = false;      // uncapped frame rate (benchmarks)
            else if (a == "--width" && hasValue) c.Window.Width = (uint32_t)std::stoul(args[++i]);
            else if (a == "--height" && hasValue) c.Window.Height = (uint32_t)std::stoul(args[++i]);
            else if (a == "--quit-after" && hasValue) c.QuitAfter = std::stof(args[++i]);
            else if (a == "--save-dir" && hasValue) c.SaveDir = args[++i];
            else LOG_WARN("Unknown argument '{}'", a);
        }
        if (c.SaveDir.empty()) {
            // %APPDATA%/<title>/Saves (the title without characters Windows refuses in names)
            std::string title;
            for (char ch : c.Window.Title) if (std::string("<>:\"/\\|?*").find(ch) == std::string::npos) title += ch;
            if (title.empty()) title = "MDB Game";
            char* appData = nullptr;
            size_t len = 0;
            if (_dupenv_s(&appData, &len, "APPDATA") == 0 && appData) c.SaveDir = (fs::path(appData) / title / "Saves").generic_string();
            else c.SaveDir = (exeDir / "Saves").generic_string();
            std::free(appData);
        }
        return c;
    }

} // namespace

class GameApp : public Engine::Application {
public:
    explicit GameApp(GameConfig cfg) : Application(cfg.Window), m_Cfg(std::move(cfg)) {
        m_ShowFpsInTitle = false;
    }

    void OnStart() override {
        using namespace Engine;
        ImGui::GetIO().IniFilename = nullptr;    // no imgui.ini next to a shipped game
        glClearColor(0.02f, 0.02f, 0.03f, 1.0f);   // only visible if the scene can't be shown
        m_Fonts = LoadGameFonts();

        m_Resources = std::make_unique<ResourceLibrary>(m_Cfg.AssetDir, m_Cfg.EngineDir);
        m_Renderer = std::make_unique<SceneRenderer>(m_Cfg.EngineDir, m_Cfg.AssetDir, GetWindow().GetWidth(), GetWindow().GetHeight());
        m_Session = std::make_unique<GameSession>(m_Scene, m_Cfg.AssetDir, m_Cfg.EngineDir, m_Resources->Registry());
        m_Session->Scripts().SetUIFonts(m_Fonts.Bold, m_Fonts.Regular);   // ui.label measures with them

        SceneSettings settings;
        m_Renderer->GatherSettings(settings);    // defaults for anything the file leaves out
        const std::string path = m_Cfg.AssetDir + "Scenes/" + m_Cfg.Scene;
        if (!SceneSerializer(m_Scene, m_Resources->Registry(), &m_Session->Prefabs()).Load(path, &settings)) {
            m_Error = "Could not load Scenes/" + m_Cfg.Scene;
            return;
        }
        m_Renderer->ApplySettings(settings);
        m_Session->Physics().SetGravity(settings.Gravity);

        // Used only if the scene has no Camera entity: the view the level was saved with
        m_FallbackCamera.Position = settings.CameraPosition;
        m_FallbackCamera.FocusDistance = settings.CameraFocusDistance;
        m_FallbackCamera.SetOrientation(settings.CameraYaw, settings.CameraPitch);

        m_Session->SetSaveDirectory(m_Cfg.SaveDir);
        m_Session->NewGame(m_Cfg.Scene);       // a restart goes back to this level as it loaded
        m_Session->Start();
        m_Running = true;
        LOG_INFO("Playing {} (saves in {})", m_Cfg.Scene, m_Cfg.SaveDir);
    }

    void OnUpdate(float dt) override {
        using namespace Engine;
        dt = std::min(dt, 0.1f);     // a long hitch (window drag, environment bake) shouldn't fling the game
        m_Time += dt;
        if (m_Cfg.QuitAfter > 0.0f && m_Time >= m_Cfg.QuitAfter) Close();

        // ---- the few keys the game shell keeps for itself
        const bool alt = Input::IsKeyDown(342) || Input::IsKeyDown(346);   // left / right Alt
        const bool esc = Edge(Key::Escape, m_EscDown);
        if (esc && !m_Running) { Close(); return; }   // nothing to pause: the scene did not load
        if (esc) m_Paused = !m_Paused;
        if (Edge(Key::F11, m_F11Down) || (alt && Edge(257, m_EnterDown)))  // F11, Alt+Enter
            GetWindow().SetFullscreen(!GetWindow().IsFullscreen());
        const bool r = Edge(Key::R, m_RDown), q = Edge(Key::Q, m_QDown);
        if (m_Paused && q) Close();
        if (!m_Running) return;
        if (m_Paused && r) { m_RestartPending = true; m_Paused = false; }
        if (m_RestartPending) { m_RestartPending = false; m_Session->RequestRestart(); }

        // A save loaded, a level change or a restart (asked for last frame) replaces the scene now,
        // and brings that level's settings
        SceneSettings settings;
        m_Renderer->GatherSettings(settings);
        if (m_Session->ApplyPending(settings)) {
            m_Renderer->ApplySettings(settings);
            m_FallbackCamera.Position = settings.CameraPosition;
            m_FallbackCamera.FocusDistance = settings.CameraFocusDistance;
            m_FallbackCamera.SetOrientation(settings.CameraYaw, settings.CameraPitch);
        }

        m_Session->SetPaused(m_Paused);
        if (!m_Paused) {
            // ui.button / ui.mouse: the pointer over the window (ImGui puts it far outside when it's gone)
            const ImGuiIO& io = ImGui::GetIO();
            const ImVec2 d = io.DisplaySize;
            const float px = io.MousePos.x / std::max(d.x, 1.0f), py = io.MousePos.y / std::max(d.y, 1.0f);
            const bool inside = px >= 0.0f && px <= 1.0f && py >= 0.0f && py <= 1.0f;
            m_Session->Scripts().SetPointer(px, py, inside, ImGui::IsMouseDown(ImGuiMouseButton_Left),
                inside ? io.MouseWheel : 0.0f, ImGui::IsMouseDown(ImGuiMouseButton_Right));
            m_Session->Scripts().SetViewAspect(d.x / std::max(d.y, 1.0f));   // for scene.screen_ray

            m_Session->Update(dt);                                            // scene.restart() is handled by the session
            if (m_Session->Scripts().ConsumeQuitRequest()) Close();            // scene.quit()
        }

        const uint32_t w = std::max(GetWindow().GetWidth(), 1u), h = std::max(GetWindow().GetHeight(), 1u);
        const float aspect = (float)w / (float)h;
        Camera* view = &m_GameCamera;
        if (!m_Session->UpdateGameCamera(m_GameCamera, aspect)) {
            m_FallbackCamera.SetAspect(aspect);
            m_FallbackCamera.Refresh();
            view = &m_FallbackCamera;
        }
        m_Session->SetListener(*view);
        m_Renderer->Render(m_Scene, *view, w, h);
        glViewport(0, 0, (int)w, (int)h);
    }

    void OnImGui() override {
        using namespace Engine;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const ImVec2 size = ImGui::GetIO().DisplaySize;

        if (!m_Error.empty()) {
            std::vector<ScriptUIItem> lines(2);
            lines[0].Text = m_Error;
            lines[0].X = 0.5f; lines[0].Y = 0.42f; lines[0].Size = 36.0f; lines[0].Align = 1;
            lines[0].Color = glm::vec3(1.0f, 0.45f, 0.35f);
            lines[1].Text = "Check project.json or the --scene argument.  Esc quits.";
            lines[1].X = 0.5f; lines[1].Y = 0.52f; lines[1].Size = 24.0f; lines[1].Align = 1;
            lines[1].Color = glm::vec3(0.8f);
            DrawScriptText(dl, m_Fonts.Bold, lines, ImVec2(0, 0), size);
            return;
        }
        if (!m_Running) return;

        // The rendered frame fills the window (GL textures are bottom-up, so flip V)
        dl->AddImage((ImTextureID)(intptr_t)m_Renderer->GetOutput(), ImVec2(0, 0), size, ImVec2(0, 1), ImVec2(1, 0));
        DrawScriptUI(dl, m_Fonts, m_Session->Scripts().UIItems(), ImVec2(0, 0), size, &m_UITextures);

        if (m_Paused) DrawPauseMenu(dl, size);
    }

    // Built from the same UI items scripts use; the clicks are handled right here
    void DrawPauseMenu(ImDrawList* dl, const ImVec2& size) {
        using namespace Engine;
        dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 150));
        std::vector<ScriptUIItem> menu;
        ScriptUIItem title;
        title.Text = "Paused";
        title.X = 0.5f; title.Y = 0.26f; title.Size = 72.0f; title.Align = 1;
        menu.push_back(title);

        const char* labels[] = { "Resume", "Restart", "Quit" };
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        int clicked = -1;
        for (int i = 0; i < 3; i++) {
            ScriptUIItem b;
            b.Kind = ScriptUIItem::Type::Button;
            b.Text = labels[i];
            b.X = 0.4f; b.Y = 0.42f + 0.1f * i; b.W = 0.2f; b.H = 0.075f;
            b.Color = glm::vec3(0.16f, 0.2f, 0.3f);
            b.Alpha = 0.95f;
            b.Rounding = 10.0f;
            b.Size = 30.0f;
            b.Hovered = UIButtonHovered(b, ImVec2(0, 0), size, mouse);
            b.Pressed = b.Hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if (b.Hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = i;
            menu.push_back(b);
        }
        ScriptUIItem keys;
        keys.Text = "Esc  resume     R  restart     Q  quit     F11  full screen";
        keys.X = 0.5f; keys.Y = 0.75f; keys.Size = 22.0f; keys.Align = 1;
        keys.Color = glm::vec3(0.75f);
        menu.push_back(keys);
        DrawScriptUI(dl, m_Fonts, menu, ImVec2(0, 0), size, nullptr);

        // Acted on next frame by OnUpdate (or right away for quitting)
        if (clicked == 0) m_Paused = false;
        if (clicked == 1) { m_RestartPending = true; m_Paused = false; }
        if (clicked == 2) Close();
    }

private:
    static bool Edge(int key, bool& wasDown) {
        const bool down = Engine::Input::IsKeyDown(key);
        const bool pressed = down && !wasDown;
        wasDown = down;
        return pressed;
    }

    GameConfig m_Cfg;
    // Declaration order: the scene and resources must outlive the session that uses them
    Engine::Scene m_Scene;
    std::unique_ptr<Engine::ResourceLibrary> m_Resources;
    std::unique_ptr<Engine::SceneRenderer> m_Renderer;
    std::unique_ptr<Engine::GameSession> m_Session;
    Engine::Camera m_GameCamera, m_FallbackCamera;
    Engine::UIFonts m_Fonts;                 // script UI and the pause menu
    Engine::UITextureCache m_UITextures{ m_Cfg.AssetDir };   // images for ui.image (declared after m_Cfg)
    std::string m_Error;

    bool m_Running = false, m_Paused = false, m_RestartPending = false;
    bool m_EscDown = false, m_F11Down = false, m_EnterDown = false, m_RDown = false, m_QDown = false;
    float m_Time = 0.0f;
};

std::unique_ptr<Engine::Application> Engine::CreateApplication() {
    return std::make_unique<GameApp>(LoadConfig());
}
