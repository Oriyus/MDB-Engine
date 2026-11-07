// The standalone game: plays one scene full screen (or in a window) with no editor.
//
// Settings come from Assets/game.json, and the command line can override them:
//   Game.exe [--scene <file in Assets/Scenes>] [--fullscreen | --windowed] [--width N] [--height N]
//            [--quit-after <seconds>]
// The Assets folder is looked for next to the executable first (a packaged build, see the
// package_game target), then at the source tree's Assets (ASSET_DIR, for running from the build).
//
// Keys: Esc pauses (then Esc resumes, R restarts, Q quits), F11 or Alt+Enter toggles full screen.
// Everything else belongs to the game's scripts.

#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Input.h"
#include "Core/Log.h"
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
        std::string AssetDir;                 // absolute, with a trailing slash
        std::string Scene = "coin_rush.json"; // in Assets/Scenes
        Engine::WindowProps Window;
        float QuitAfter = 0.0f;               // test hook: close after this many seconds (0 = never)
    };

    GameConfig LoadConfig() {
        const auto& args = Engine::Application::GetCommandLineArgs();
        GameConfig c;
        c.Window.Title = "MDB Game";

        // Assets next to the executable (a packaged game), else the source tree's
        std::error_code ec;
        const fs::path exeDir = args.empty() ? fs::current_path(ec) : fs::absolute(args[0], ec).parent_path();
        if (fs::exists(exeDir / "Assets" / "Shaders", ec)) c.AssetDir = (exeDir / "Assets").generic_string() + "/";
        else c.AssetDir = ASSET_DIR;
        LOG_INFO("Assets: {}", c.AssetDir);

        // game.json: { "title", "scene", "width", "height", "fullscreen", "vsync" }, all optional
        if (std::ifstream in(c.AssetDir + "game.json"); in) {
            try {
                nlohmann::json j;
                in >> j;
                c.Window.Title = j.value("title", c.Window.Title);
                c.Scene = j.value("scene", c.Scene);
                c.Window.Width = j.value("width", c.Window.Width);
                c.Window.Height = j.value("height", c.Window.Height);
                c.Window.Fullscreen = j.value("fullscreen", c.Window.Fullscreen);
                c.Window.VSync = j.value("vsync", c.Window.VSync);
            }
            catch (const std::exception& ex) {
                LOG_WARN("game.json is malformed ({}), using defaults", ex.what());
            }
        }

        for (size_t i = 1; i < args.size(); i++) {
            const std::string& a = args[i];
            const bool hasValue = i + 1 < args.size();
            if (a == "--scene" && hasValue) c.Scene = args[++i];
            else if (a == "--fullscreen") c.Window.Fullscreen = true;
            else if (a == "--windowed") c.Window.Fullscreen = false;
            else if (a == "--width" && hasValue) c.Window.Width = (uint32_t)std::stoul(args[++i]);
            else if (a == "--height" && hasValue) c.Window.Height = (uint32_t)std::stoul(args[++i]);
            else if (a == "--quit-after" && hasValue) c.QuitAfter = std::stof(args[++i]);
            else LOG_WARN("Unknown argument '{}'", a);
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
        m_Font = LoadGameFont();

        m_Resources = std::make_unique<ResourceLibrary>(m_Cfg.AssetDir);
        m_Renderer = std::make_unique<SceneRenderer>(m_Cfg.AssetDir, GetWindow().GetWidth(), GetWindow().GetHeight());
        m_Session = std::make_unique<GameSession>(m_Scene, m_Cfg.AssetDir, m_Resources->Registry());

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

        int unused = -1;
        m_Snapshot = m_Scene.Capture(entt::null, unused);   // what a restart goes back to
        m_Session->Start();
        m_Running = true;
        LOG_INFO("Playing {}", m_Cfg.Scene);
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

        if (m_RestartPending) {
            m_RestartPending = false;
            m_Session->Stop();
            m_Scene.Restore(m_Snapshot, -1);
            m_Session->Start();
        }

        m_Session->SetPaused(m_Paused);
        if (!m_Paused) {
            m_Session->Update(dt);
            if (m_Session->ConsumeRestartRequest()) m_RestartPending = true;   // scene.restart(): next frame
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
            std::vector<ScriptText> lines(2);
            lines[0].Text = m_Error;
            lines[0].X = 0.5f; lines[0].Y = 0.42f; lines[0].Size = 36.0f; lines[0].Align = 1;
            lines[0].Color = glm::vec3(1.0f, 0.45f, 0.35f);
            lines[1].Text = "Check game.json or the --scene argument.  Esc quits.";
            lines[1].X = 0.5f; lines[1].Y = 0.52f; lines[1].Size = 24.0f; lines[1].Align = 1;
            lines[1].Color = glm::vec3(0.8f);
            DrawScriptText(dl, m_Font, lines, ImVec2(0, 0), size);
            return;
        }
        if (!m_Running) return;

        // The rendered frame fills the window (GL textures are bottom-up, so flip V)
        dl->AddImage((ImTextureID)(intptr_t)m_Renderer->GetOutput(), ImVec2(0, 0), size, ImVec2(0, 1), ImVec2(1, 0));
        DrawScriptText(dl, m_Font, m_Session->Scripts().Texts(), ImVec2(0, 0), size);

        if (m_Paused) {
            dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 150));
            std::vector<ScriptText> menu(2);
            menu[0].Text = "Paused";
            menu[0].X = 0.5f; menu[0].Y = 0.36f; menu[0].Size = 72.0f; menu[0].Align = 1;
            menu[1].Text = "Esc  resume     R  restart     Q  quit     F11  full screen";
            menu[1].X = 0.5f; menu[1].Y = 0.52f; menu[1].Size = 28.0f; menu[1].Align = 1;
            menu[1].Color = glm::vec3(0.85f);
            DrawScriptText(dl, m_Font, menu, ImVec2(0, 0), size);
        }
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
    Engine::SceneSnapshot m_Snapshot;
    Engine::Camera m_GameCamera, m_FallbackCamera;
    ImFont* m_Font = nullptr;
    std::string m_Error;

    bool m_Running = false, m_Paused = false, m_RestartPending = false;
    bool m_EscDown = false, m_F11Down = false, m_EnterDown = false, m_RDown = false, m_QDown = false;
    float m_Time = 0.0f;
};

std::unique_ptr<Engine::Application> Engine::CreateApplication() {
    return std::make_unique<GameApp>(LoadConfig());
}
